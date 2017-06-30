/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/** @file fil/fil0fil.cc
The tablespace memory cache */

#include "my_config.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>

#include "btr0btr.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "dict0boot.h"
#include "dict0dict.h"
#include "fsp0file.h"
#include "fsp0fsp.h"
#include "fsp0space.h"
#include "fsp0sysspace.h"
#include "ha_prototypes.h"
#include "hash0hash.h"
#include "log0recv.h"
#include "mach0data.h"
#include "mem0mem.h"
#include "mtr0log.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "os0file.h"
#include "page0zip.h"
#include "row0mysql.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "clone0api.h"

#ifndef UNIV_HOTBACKUP
# include "buf0lru.h"
# include "ibuf0ibuf.h"
# include "os0event.h"
# include "sync0sync.h"
#else /* !UNIV_HOTBACKUP */
# include "log0log.h"
# include "srv0srv.h"
#endif /* !UNIV_HOTBACKUP */

#include <fstream>
#include <list>
#include <array>
#include <unordered_map>

using Dirs = std::vector<std::string>;
using Duplicates = std::set<space_id_t>;

#ifdef UNIV_PFS_IO
mysql_pfs_key_t  innodb_tablespace_open_file_key;
#endif /* UNIV_PFS_IO */

fil_space_t*	fil_space_t::s_sys_space;

/** Tries to close a file in the LRU list. The caller must hold the fil_sys
mutex. This function will release the fil sys mutex temporarily.
@return true if success, false if should retry later; since i/o's
generally complete in < 100 ms, and as InnoDB writes at most 128 pages
from the buffer pool in a batch, and then immediately flushes the
files, there is a good chance that the next time we find a suitable
file from the LRU list.
@param[in] print_info   if true, prints information why it
			cannot close a file */
static
bool
fil_try_to_close_file_in_LRU(bool print_info);

/*
		IMPLEMENTATION OF THE TABLESPACE MEMORY CACHE
		=============================================

The tablespace cache is responsible for providing fast read/write access to
tablespaces and logs of the database. File creation and deletion is done
in other modules which know more of the logic of the operation, however.

Only the system  tablespace consists of a list  of files. The size of these
files does not have to be divisible by the database block size, because
we may just leave the last incomplete block unused. When a new file is
appended to the tablespace, the maximum size of the file is also specified.
At the moment, we think that it is best to extend the file to its maximum
size already at the creation of the file, because then we can avoid dynamically
extending the file when more space is needed for the tablespace.

Non system tablespaces contain only a single file.

A block's position in the tablespace is specified with a 32-bit unsigned
integer. The files in the list  are thought to be catenated, and the block
corresponding to an address n is the nth block in the catenated file (where
the first block is named the 0th block, and the incomplete block fragments
at the end of files are not taken into account). A tablespace can be extended
by appending a new file at the end of the list.

Our tablespace concept is similar to the one of Oracle.

To have fast access to a tablespace or a log file, we put the data structures
to a hash table. Each tablespace and log file is given an unique 32-bit
identifier, its tablespace ID.

Some operating systems do not support many open files at the same time,
Therefore, we put the open files in an LRU-list. If we need to open another
file, we may close the file at the end of the LRU-list. When an I/O-operation
is pending on a file, the file cannot be closed. We take the file nodes with
pending I/O-operations out of the LRU-list and keep a count of pending
operations. When an operation completes, we decrement the count and return
the file to the LRU-list if the count drops to zero. */

/** This tablespace name is used internally during recovery to open a
general tablespace before the data dictionary are recovered and available. */
const char general_space_name[] = "innodb_general";

/** Reference to the server data directory. Usually it is the
current working directory ".", but in the MySQL Embedded Server Library
it is an absolute path. */
const char*	fil_path_to_mysql_datadir;
Folder		folder_mysql_datadir;

/** Common InnoDB file extentions */
const char* dot_ext[] = { "", ".ibd", ".cfg", ".cfp" };

/** The number of fsyncs done to the log */
ulint	fil_n_log_flushes			= 0;

/** Number of pending redo log flushes */
ulint	fil_n_pending_log_flushes		= 0;

/** Number of pending tablespace flushes */
ulint	fil_n_pending_tablespace_flushes	= 0;

/** Number of files currently open */
ulint	fil_n_file_opened			= 0;

enum fil_load_status {
	/** The tablespace file(s) were found and valid. */
	FIL_LOAD_OK,

	/** The name no longer matches space_id */
	FIL_LOAD_ID_CHANGED,

	/** The file(s) were not found */
	FIL_LOAD_NOT_FOUND,

	/** The file(s) were not valid */
	FIL_LOAD_INVALID,

	/** The tablespace file ID in the first page doesn't match
	expected value. */
	FIL_LOAD_MISMATCH
};

/** The null file address */
fil_addr_t	fil_addr_null = {FIL_NULL, 0};

/** Hash a NUL terminated 'string' */
struct Char_Ptr_Hash {

	/** Hashing function
	@param[in]	ptr		NUL terminated string to hash
	@return the hash */
	size_t operator()(const char* ptr) const
	{
		return(ut_fold_string(ptr));
	}
};

/** Compare two 'strings' */
struct Char_Ptr_Compare {

	/** Compare two NUL terminated strings
	@param[in]	lhs		Left hand side
	@param[in]	rhs		Right hand side
	@return true if the contents match */
	bool operator()(const char* lhs, const char* rhs) const
	{
		return(strcmp(lhs, rhs) == 0);
	}
};

class Tablespace_files {
public:
	using Names = std::vector<std::string, ut_allocator<std::string>>;
	using Paths = std::unordered_map<space_id_t, Names>;

	/** Default constructor */
	Tablespace_files() : m_paths() { }

	/** Add a space ID to filename mapping.
	@param[in]	space_id	Tablespace ID
	@param[in]	name		File name.
	@return number of files that map to the space ID */
	size_t add(space_id_t space_id, const std::string& name)
	{
		auto&	names = m_paths[space_id];

		names.push_back(name);

		return(names.size());
	}

	/** Get the file names that map to a space ID
	@param[in]	space_Id	Tablespace ID
	@return the filenames that map to space id */
	Names* find(space_id_t space_id)
	{
		auto	it = m_paths.find(space_id);

		if (it != m_paths.end()) {
			return(&it->second);
		}

		return(nullptr);
	}

	/** Remove the entry for the space ID.
	@param[in]	space_id	Tablespace ID mapping to remove */
	void erase(space_id_t space_id)
	{
		auto	n_erased = m_paths.erase(space_id);

		ut_a(n_erased == 1);
	}

private:
	/** Mapping from tablespace ID to filenames */
	Paths			m_paths;
};

/** Tablespace ID to filename mapping that was recovered by scanning the
directories if --innobase-scan-directories was set. */
static Tablespace_files*	tablespace_files;

/** The tablespace memory cache; also the totality of logs (the log
data space) is stored here; below we talk about tablespaces, but also
the ib_logfiles form a 'space' and it is handled here */
struct Fil_system {

	using File_list = UT_LIST_BASE_NODE_T(fil_node_t);
	using Space_list = UT_LIST_BASE_NODE_T(fil_space_t);
	using Spaces = std::unordered_map<space_id_t, fil_space_t*>;

	using Names = std::unordered_map<
		const char*, fil_space_t*, Char_Ptr_Hash, Char_Ptr_Compare>;


#ifndef UNIV_HOTBACKUP
	/** The mutex protecting the cache */
	ib_mutex_t		mutex;
#endif /* !UNIV_HOTBACKUP */

	/** Tablespace instances hashed on the space id */
	Spaces			spaces;

	/** Tablespace instances hashed on the space name */
	Names			names;

	/** Track the mapping from tablespace ID to file name on disk. */
	Tablespace_files	open;

	/** Base node for the LRU list of the most recently used open
	files with no pending i/o's; if we start an i/o on the file,
	we first remove it from this list, and return it to the start
	of the list when the i/o ends; log files and the system tablespace
	are not put to this list: they are opened after the startup, and
	kept open until shutdown */
	File_list		LRU;

	/** Base node for the list of those tablespaces whose files contain
	unflushed writes; those spaces have at least one file where
	modification_counter > flush_counter */
	Space_list		unflushed_spaces;

	/** Number of files currently open */
	size_t			n_open;

	/** n_open is not allowed to exceed this */
	size_t			max_n_open;

	/** When we write to a file we increment this by one */
	int64_t			modification_counter;

	/** Maximum space id in the existing tables, or assigned during
	the time mysqld has been up; at an InnoDB startup we scan the
	data dictionary and set here the maximum of the space id's of
	the tables there */
	space_id_t		max_assigned_id;

	/** true if fil_space_create() has issued a warning about
	potential space_id reuse */
	bool			space_id_reuse_warned;
};

/** The tablespace memory cache. This variable is nullptr before the module is
initialized. */
static Fil_system*	fil_system = nullptr;

#ifdef UNIV_HOTBACKUP
static ulint	srv_data_read;
static ulint	srv_data_written;
#endif /* UNIV_HOTBACKUP */

/** Determine if user has explicitly disabled fsync(). */
#ifndef _WIN32
# define fil_buffering_disabled(s)	\
	((s)->purpose == FIL_TYPE_TABLESPACE	\
	 && srv_unix_file_flush_method	\
	 == SRV_UNIX_O_DIRECT_NO_FSYNC)
#else /* _WIN32 */
# define fil_buffering_disabled(s)	(0)
#endif /* __WIN32 */

#ifdef UNIV_DEBUG
/** Try fil_validate() every this many times */
# define FIL_VALIDATE_SKIP	17

/** Checks the consistency of the tablespace cache some of the time.
@return true if ok or the check was skipped */
static
bool
fil_validate_skip()
{
	/** The fil_validate() call skip counter. Use a signed type
	because of the race condition below. */
	static int fil_validate_count = FIL_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly fil_validate() check
	in debug builds. */
	--fil_validate_count;

	if (fil_validate_count > 0) {
		return(true);
	}

	fil_validate_count = FIL_VALIDATE_SKIP;
	return(fil_validate());
}
#endif /* UNIV_DEBUG */

/** Determines if a file belongs to the least-recently-used list.
@param[in]	space		Tablespace to check
@return true if the file belongs to fil_system->LRU mutex. */
static
bool
fil_space_belongs_in_lru(
	const fil_space_t*	space)
{
	switch (space->purpose) {
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_LOG:
		return(false);
	case FIL_TYPE_TABLESPACE:
		return(fsp_is_ibd_tablespace(space->id));
	case FIL_TYPE_IMPORT:
		return(true);
	}

	ut_ad(0);
	return(false);
}

/** NOTE: you must call fil_mutex_enter_and_prepare_for_io() first!

Prepares a file for i/o. Opens the file if it is closed. Updates the
pending i/o's field in the file and the system appropriately. Takes the file
off the LRU list if it is in the LRU list. The caller must hold the fil_sys
mutex.
@param[in]	file		Tablespace file
@param[in]	system		Tablespace memory cache
@param[in]	space		Tablespace instance
@param[in]	extend		true if file is being extended
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_prepare_for_io(
	fil_node_t*	file,
	Fil_system*	system,
	fil_space_t*	space,
	bool		extend);

/** Updates the data structures when an i/o operation finishes. Updates the
pending i/o's field in the file appropriately.
@param[in,out] file		Tablespace file
@param[in,out] system		tablespace instance
@param[in] type			IO context */
static
void
fil_node_complete_io(
	fil_node_t*		file,
	Fil_system*		system,
	const IORequest&	type);

/** Check if the name is an undo tablespace name.
@param[in]	name	Tablespace name
@param[in]	len	Tablespace name length in bytes
@return true if it is an undo tablespace name */
static
bool
fil_is_undo_tablespace_name(const char* name, size_t len)
{
	if (len >= 8) {

		const char*	end_ptr = name + len;
		size_t	u = (end_ptr[-4] == '_' ? 1 : 0);

		return(end_ptr[-8-u] == OS_PATH_SEPARATOR
		       && end_ptr[-7-u] == 'u'
		       && end_ptr[-6-u] == 'n'
		       && end_ptr[-5-u] == 'd'
		       && end_ptr[-4-u] == 'o'
		       && isdigit(end_ptr[-3])
		       && isdigit(end_ptr[-2])
		       && isdigit(end_ptr[-1]));
	}

	return(false);
}

/** Get the absolute path foe a file name
@param[in]	dir		Directory
@param[in]	filename	Filename without directory prefix
@return the absolute path of dir + filename */
static
std::string
fil_get_abs_path(const char* dir, const std::string& filename)
{
	char    abspath[FN_REFLEN + 2];

	memset(abspath, 0x0, sizeof(abspath));

	my_realpath(abspath, dir, MYF(0));

	size_t  len = strlen(abspath);

	ut_a(len < sizeof(abspath) - 10 - filename.length());

	if (abspath[len - 1] != OS_PATH_SEPARATOR) {
		abspath[len] = OS_PATH_SEPARATOR;
		++len;
	}

	strncat(abspath, filename.c_str(), filename.length());

	return(std::string(abspath));
}
/** Reads data from a space to a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to read; this must not cross a
file boundary; in aio this must be a block size multiple
@param[in,out]	buf		buffer where to store data read; in aio this
must be appropriately aligned
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
static
dberr_t
fil_read(
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf)
{
	return(fil_io(IORequestRead, true, page_id, page_size,
		      byte_offset, len, buf, nullptr));
}

/** Writes data to a space from a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to write; this must not cross
a file boundary; in aio this must be a block size multiple
@param[in]	buf		buffer from which to write; in aio this must
be appropriately aligned
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
	I/O on a tablespace which does not exist */
static
dberr_t
fil_write(
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf)
{
	ut_ad(!srv_read_only_mode);

	return(fil_io(IORequestWrite, true, page_id, page_size,
		      byte_offset, len, buf, nullptr));
}

/** Returns the table space by a given id, nullptr if not found.
@param[in]	id		Tablespace ID
@return the instance of the tablespace meta data */
static
fil_space_t*
fil_space_get_by_id(space_id_t id)
{
	ut_ad(mutex_own(&fil_system->mutex));

	auto	it = fil_system->spaces.find(id);

	if (it == fil_system->spaces.end()) {
		return(nullptr);
	}

	ut_ad(it->second->magic_n == FIL_SPACE_MAGIC_N);

	return(it->second);
}

/** Returns the table space by a given name, nullptr if not found.
@param[in]	name		Tablespace name to search for.
@return nullptr if not found */
static
fil_space_t*
fil_space_get_by_name(const char* name)
{
	ut_ad(mutex_own(&fil_system->mutex));

	auto	it = fil_system->names.find(name);

	if (it == fil_system->names.end()) {
		return(nullptr);
	}

	ut_ad(it->second->magic_n == FIL_SPACE_MAGIC_N);

	return(it->second);
}

#ifndef UNIV_HOTBACKUP
/** Look up a tablespace.
The caller should hold an InnoDB table lock or a MDL that prevents
the tablespace from being dropped during the operation,
or the caller should be in single-threaded crash recovery mode
(no user connections that could drop tablespaces).
If this is not the case, fil_space_acquire() and fil_space_release()
should be used instead.
@param[in]	id	tablespace ID
@return tablespace, or nullptr if not found */
fil_space_t*
fil_space_get(space_id_t id)
{
	mutex_enter(&fil_system->mutex);
	fil_space_t*	space = fil_space_get_by_id(id);
	mutex_exit(&fil_system->mutex);

	return(space);
}

/** Returns the latch of a file space.
@param[in]	id	space id
@param[out]	flags	tablespace flags
@return latch protecting storage allocation */
rw_lock_t*
fil_space_get_latch(
	space_id_t	id,
	ulint*		flags)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != nullptr);

	if (flags != nullptr) {
		*flags = space->flags;
	}

	mutex_exit(&fil_system->mutex);

	return(&space->latch);
}

#ifdef UNIV_DEBUG
/** Gets the type of a file space.
@param[in]	id	tablespace identifier
@return file type */
fil_type_t
fil_space_get_type(space_id_t id)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	mutex_exit(&fil_system->mutex);

	return(space->purpose);
}
#endif /* UNIV_DEBUG */

/** Note that a tablespace has been imported.
It is initially marked as FIL_TYPE_IMPORT so that no logging is
done during the import process when the space ID is stamped to each page.
Now we change it to FIL_SPACE_TABLESPACE to start redo and undo logging.
NOTE: temporary tablespaces are never imported.
@param[in]	id	tablespace identifier */
void
fil_space_set_imported(space_id_t id)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);

	ut_ad(space->purpose == FIL_TYPE_IMPORT);
	space->purpose = FIL_TYPE_TABLESPACE;

	mutex_exit(&fil_system->mutex);
}
#endif /* !UNIV_HOTBACKUP */

/** Checks if all the file nodes in a space are flushed. The caller must hold
the fil_system mutex.
@param[in]	space		Tablespace to check
@return true if all are flushed */
static
bool
fil_space_is_flushed(const fil_space_t* space)
{
	ut_ad(mutex_own(&fil_system->mutex));

	for (const auto& file : space->files) {

		if (file.modification_counter > file.flush_counter) {

			ut_ad(!fil_buffering_disabled(space));
			return(false);
		}
	}

	return(true);
}

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)

#include <sys/ioctl.h>

/** FusionIO atomic write control info */
#define DFS_IOCTL_ATOMIC_WRITE_SET	_IOW(0x95, 2, uint)

/** Try and enable FusionIO atomic writes.
@param[in] file		OS file handle
@return true if successful */
bool
fil_fusionio_enable_atomic_write(pfs_os_file_t file)
{
	if (srv_unix_file_flush_method == SRV_UNIX_O_DIRECT) {

		uint	atomic = 1;

		ut_a(file.m_file != -1);

		if (ioctl(file.m_file, DFS_IOCTL_ATOMIC_WRITE_SET, &atomic)
		    != -1) {

			return(true);
		}
	}

	return(false);
}
#endif /* !NO_FALLOCATE && UNIV_LINUX */

/** Attach a file to a tablespace
@param[in]	name		file name of a file that is not open
@param[in]	size		file size in entire database blocks
@param[in,out]	space		tablespace from fil_space_create()
@param[in]	is_raw		whether this is a raw device or partition
@param[in]	punch_hole	true if supported for this file 
@param[in]	atomic_write	true if the file has atomic write enabled
@param[in]	max_pages	maximum number of pages in file
@return pointer to the file name
@retval nullptr if error */
static
fil_node_t*
fil_node_create_low(
	const char*	name,
	page_no_t	size,
	fil_space_t*	space,
	bool		is_raw,
	bool		punch_hole,
	bool		atomic_write,
	page_no_t	max_pages = PAGE_NO_MAX)
{
	ut_ad(name != nullptr);
	ut_ad(fil_system != nullptr);

	if (space == nullptr) {
		return(nullptr);
	}

	fil_node_t	file;

	memset(&file, 0x0, sizeof(file));

	file.name = mem_strdup(name);

	ut_a(!is_raw || srv_start_raw_disk_in_use);

	file.sync_event = os_event_create("fsync_event");

	file.is_raw_disk = is_raw;

	file.size = size;

	file.magic_n = FIL_NODE_MAGIC_N;

	file.init_size = size;

	file.max_size = max_pages;

	mutex_enter(&fil_system->mutex);

	space->size += size;

	file.space = space;

	os_file_stat_t	stat_info;

#ifdef UNIV_DEBUG
	dberr_t err =
#endif /* UNIV_DEBUG */

	os_file_get_status(
		file.name, &stat_info, false,
		fsp_is_system_temporary(space->id) ? true : srv_read_only_mode);

	ut_ad(err == DB_SUCCESS);

	file.block_size = stat_info.block_size;

	/* In this debugging mode, we can overcome the limitation of some
	OSes like Windows that support Punch Hole but have a hole size
	effectively too large.  By setting the block size to be half the
	page size, we can bypass one of the checks that would normally
	turn Page Compression off.  This execution mode allows compression
	to be tested even when full punch hole support is not available. */
	DBUG_EXECUTE_IF("ignore_punch_hole",
		file.block_size = ut_min(
			static_cast<ulint>(stat_info.block_size),
			UNIV_PAGE_SIZE / 2);
	);

	if (!IORequest::is_punch_hole_supported()
	    || !punch_hole
	    || file.block_size >= srv_page_size) {

		fil_no_punch_hole(&file);
	} else {
		file.punch_hole = punch_hole;
	}

	file.atomic_write = atomic_write;

	space->files.push_back(file);

	ut_a(space->id == TRX_SYS_SPACE
	     || space->id == dict_sys_t::log_space_first_id
	     || space->files.size() == 1);

	mutex_exit(&fil_system->mutex);

	return(&space->files.front());
}

/** Attach a file to a tablespace. File must be closed.
@param[in]	name		file name (file must be closed)
@param[in]	size		file size in database blocks, rounded
				downwards to an integer
@param[in,out]	space		space where to append
@param[in]	is_raw		true if a raw device or a raw disk partition
@param[in]	atomic_write	true if the file has atomic write enabled
@param[in]	max_pages	maximum number of pages in file
@return pointer to the file name
@retval nullptr if error */
char*
fil_node_create(
	const char*	name,
	page_no_t	size,
	fil_space_t*	space,
	bool		is_raw,
	bool		atomic_write,
	page_no_t	max_pages)
{
	fil_node_t*	file;

	file = fil_node_create_low(
		name, size, space, is_raw, IORequest::is_punch_hole_supported(),
		atomic_write, max_pages);

	return(file == nullptr ? nullptr : file->name);
}

/** Open a file of a tablespace.
The caller must own the fil_system mutex.
@param[in,out]	file		Tablespace file
@param[in]	extend		true if the file is being extended
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_open_file(fil_node_t* file, bool extend)
{
	os_offset_t	size_bytes;
	bool		success;
	byte*		buf2;
	byte*		page;
	space_id_t	space_id;
	ulint		flags;
	ulint		min_size;
	bool		read_only_mode;
	fil_space_t*	space = file->space;

	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(file->n_pending == 0);
	ut_a(!file->is_open);

	while (file->in_use > 0) {

		/* We increment the reference count when extending
		the file. */
		if (file->in_use == 1 && extend) {
			break;
		}

		mutex_exit(&fil_system->mutex);

		os_thread_sleep(100000);

		mutex_enter(&fil_system->mutex);
	}

	if (file->is_open) {

		return(true);
	}

	read_only_mode = !fsp_is_system_temporary(space->id)
		&& srv_read_only_mode;

	if (file->size == 0
	    || (space->size_in_header == 0
		&& space->purpose == FIL_TYPE_TABLESPACE
		&& file == &space->files.front()
		&& undo::is_active(space->id)
		&& srv_startup_is_before_trx_rollback_phase)) {
		/* We do not know the size of the file yet. First we
		open the file in the normal mode, no async I/O here,
		for simplicity. Then do some checks, and close the
		file again.  NOTE that we could not use the simple
		file read function os_file_read() in Windows to read
		from a file opened for async I/O! */

retry:
		ut_a(!file->is_open);

		file->handle = os_file_create_simple_no_error_handling(
			innodb_data_file_key, file->name, OS_FILE_OPEN,
			OS_FILE_READ_ONLY, read_only_mode, &success);

		if (!success) {
			/* The following call prints an error message */
			ulint err = os_file_get_last_error(true);

			if (err == EMFILE + 100) {

				/* Note: This call will release the
				file system mutex temporarily. */

				if (fil_try_to_close_file_in_LRU(true)) {
					goto retry;
				}
			}

			ib::warn()
				<< "Cannot open '" << file->name << "'."
				" Have you deleted .ibd files under a"
				" running mysqld server?";

			return(false);
		}

		size_bytes = os_file_get_size(file->handle);
		ut_a(size_bytes != (os_offset_t) -1);

#ifdef UNIV_HOTBACKUP
		if (space->id == 0) {
			file->size = (ulint) (size_bytes / UNIV_PAGE_SIZE);
			os_file_close(file->handle);
			goto add_size;
		}
#endif /* UNIV_HOTBACKUP */
		ut_a(space->purpose != FIL_TYPE_LOG);

		/* Read the first page of the tablespace */

		buf2 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));

		/* Align the memory for file i/o if we might have O_DIRECT
		set */
		page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));
		ut_ad(page == page_align(page));

		IORequest	request(IORequest::READ);

		success = os_file_read(
			request,
			file->handle, page, 0, UNIV_PAGE_SIZE);

		space_id = fsp_header_get_space_id(page);
		flags = fsp_header_get_flags(page);

		/* Close the file now that we have read the space id from it */

		os_file_close(file->handle);

		const page_size_t	page_size(flags);

		min_size = FIL_IBD_FILE_INITIAL_SIZE * page_size.physical();

		if (size_bytes < min_size) {

			ib::error() << "The size of tablespace file "
				<< file->name << " is only " << size_bytes
				<< ", should be at least " << min_size << "!";

			ut_error;
		}

		if (space_id != space->id) {
			ib::fatal() << "Tablespace id is " << space->id
				<< " in the data dictionary but in file "
				<< file->name << " it is " << space_id << "!";
		}

		const page_size_t	space_page_size(space->flags);

		if (!page_size.equals_to(space_page_size)) {
			ib::fatal() << "Tablespace file " << file->name
				<< " has page size " << page_size
				<< " (flags=" << ib::hex(flags) << ") but the"
				" data dictionary expects page size "
				<< space_page_size << " (flags="
				<< ib::hex(space->flags) << ")!";
		}

		/* TODO: Remove this adjustment and enable the below assert
		after dict_tf_to_fsp_flags() removal. */
		space->flags |= flags & FSP_FLAGS_MASK_SDI;
		/* ut_ad(space->flags == flags); */

		if (space->flags != flags) {

			ib::fatal()
				<< "Table flags are "
				<< ib::hex(space->flags) << " in the data"
				" dictionary but the flags in file "
				<< file->name << " are " << ib::hex(flags)
				<< "!";
		}

		{
			page_no_t	size		= fsp_header_get_field(
				page, FSP_SIZE);
			page_no_t	free_limit	= fsp_header_get_field(
				page, FSP_FREE_LIMIT);
			ulint		free_len	= flst_get_len(
				FSP_HEADER_OFFSET + FSP_FREE + page);
			ut_ad(space->free_limit == 0
			      || space->free_limit == free_limit);
			ut_ad(space->free_len == 0
			      || space->free_len == free_len);
			space->size_in_header = size;
			space->free_limit = free_limit;
			space->free_len = free_len;
		}

		ut_free(buf2);

		/* For encrypted tablespace, we need to check the
		encrytion key and iv(initial vector) is readed. */
		if (FSP_FLAGS_GET_ENCRYPTION(flags)
		    && !recv_recovery_is_on()) {
			if (space->encryption_type != Encryption::AES) {
				ib::error()
					<< "Can't read encryption"
					<< " key from file "
					<< file->name << "!";
				return(false);
			}
		}

		if (file->size == 0) {
			ulint	extent_size;

			extent_size = page_size.physical() * FSP_EXTENT_SIZE;
			/* Truncate the size to a multiple of extent size. */
			if (size_bytes >= extent_size) {
				size_bytes = ut_2pow_round(size_bytes,
							   extent_size);
			}

			file->size = static_cast<page_no_t>(
				(size_bytes / page_size.physical()));

#ifdef UNIV_HOTBACKUP
add_size:
#endif /* UNIV_HOTBACKUP */
			space->size += file->size;
		}
	}

	/* Open the file for reading and writing, in Windows normally in the
	unbuffered async I/O mode, though global variables may make
	os_file_create() to fall back to the normal file I/O mode. */

	if (space->purpose == FIL_TYPE_LOG) {
		file->handle = os_file_create(
			innodb_log_file_key, file->name, OS_FILE_OPEN,
			OS_FILE_AIO, OS_LOG_FILE, read_only_mode, &success);
	} else if (file->is_raw_disk) {
		file->handle = os_file_create(
			innodb_data_file_key, file->name, OS_FILE_OPEN_RAW,
			OS_FILE_AIO, OS_DATA_FILE, read_only_mode, &success);
	} else {
		file->handle = os_file_create(
			innodb_data_file_key, file->name, OS_FILE_OPEN,
			OS_FILE_AIO, OS_DATA_FILE, read_only_mode, &success);
	}

	ut_a(success);

	/* The file file is ready for IO. */

	if (fil_space_belongs_in_lru(space)) {

		/* Put the file to the LRU list */
		UT_LIST_ADD_FIRST(fil_system->LRU, file);
	}

	file->is_open = true;

	++fil_n_file_opened;
	++fil_system->n_open;

	return(true);
}

/** Close a tablespace file.
@param[in]	lru_close	true if called from LRU close
@param[in,out]	file		Tablespace file to close */
static
void
fil_node_close_file(fil_node_t* file, bool lru_close)
{
	ut_ad(mutex_own(&(fil_system->mutex)));

	ut_a(file->is_open);
	ut_a(file->in_use == 0);
	ut_a(file->n_pending == 0);
	ut_a(file->n_pending_flushes == 0);

#ifndef UNIV_HOTBACKUP
	ut_a(file->modification_counter == file->flush_counter
	     || file->space->purpose == FIL_TYPE_TEMPORARY
	     || srv_fast_shutdown == 2);
#endif /* !UNIV_HOTBACKUP */

	bool	ret = os_file_close(file->handle);
	ut_a(ret);

	file->is_open = false;

	ut_a(fil_system->n_open > 0);

	--fil_n_file_opened;
	--fil_system->n_open;

	if (fil_space_belongs_in_lru(file->space)) {

		ut_a(UT_LIST_GET_LEN(fil_system->LRU) > 0);

		/* The file is in the LRU list, remove it */
		UT_LIST_REMOVE(fil_system->LRU, file);
	}
}

/** Tries to close a file in the LRU list. The caller must hold the fil_sys
mutex. 
@param[in] print_info   if true, prints information why it cannot close a file
@return true if success, false if should retry later */
static
bool
fil_try_to_close_file_in_LRU(bool print_info)

{
	ut_ad(mutex_own(&fil_system->mutex));

	if (print_info) {
		ib::info() << "fil_sys open file LRU len "
			<< UT_LIST_GET_LEN(fil_system->LRU);
	}

	for (auto file = UT_LIST_GET_LAST(fil_system->LRU);
	     file != nullptr;
	     file = UT_LIST_GET_PREV(LRU, file)) {

		if (file->modification_counter == file->flush_counter
		    && file->n_pending_flushes == 0
		    && file->in_use == 0) {

			/* Will release the fil_system->mutex. */
			fil_node_close_file(file, true);

			return(true);
		}

		if (!print_info) {
			continue;
		}

		if (file->n_pending_flushes > 0) {

			ib::info() << "Cannot close file " << file->name
				<< ", because n_pending_flushes "
				<< file->n_pending_flushes;
		}

		if (file->modification_counter != file->flush_counter) {
			ib::warn() << "Cannot close file " << file->name
				<< ", because modification count "
				<< file->modification_counter <<
				" != flush count " << file->flush_counter;
		}

		if (file->in_use > 0) {
			ib::info() << "Cannot close file " << file->name
				<< ", because it is in use";
		}
	}

	return(false);
}

/* Reserves the fil_system mutex and tries to make sure we can open at least one
file while holding it. This should be called before calling
fil_node_prepare_for_io(), because that function may need to open a file.
@param[in]	space_id	Tablespace ID */
static
void
fil_mutex_enter_and_prepare_for_io(space_id_t space_id)
{
	bool		success;
	ulint		count = 0;
	ulint		count2 = 0;
	bool		print_info = false;

	for (;;) {
		mutex_enter(&fil_system->mutex);

		if (space_id == 0 || dict_sys_t::is_reserved(space_id)) {
			/* We keep log files and system tablespace files always
			open; this is important in preventing deadlocks in this
			module, as a page read completion often performs
			another read from the insert buffer. The insert buffer
			is in tablespace 0, and we cannot end up waiting in
			this function. */
			return;
		}

		fil_space_t*	space = fil_space_get_by_id(space_id);

		if (space != nullptr && space->stop_ios) {
			/* We are going to do a rename file and want to stop
			new i/o's for a while. */

			if (count2 > 20000) {
				ib::warn() << "Tablespace " << space->name
					<< " has i/o ops stopped for a long"
					" time " << count2;
			}

			mutex_exit(&fil_system->mutex);

#ifndef UNIV_HOTBACKUP

			/* Wake the i/o-handler threads to make sure pending
			i/o's are performed */
			os_aio_simulated_wake_handler_threads();

			/* The sleep here is just to give IO helper threads a
			bit of time to do some work. It is not required that
			all IO related to the tablespace being renamed must
			be flushed here as we do fil_flush() in
			fil_rename_tablespace() as well. */
			os_thread_sleep(20000);

#endif /* UNIV_HOTBACKUP */

			/* Flush tablespaces so that we can close modified
			files in the LRU list */
			fil_flush_file_spaces(to_int(FIL_TYPE_TABLESPACE));

			os_thread_sleep(20000);

			++count2;

			continue;
		}

		if (fil_system->n_open < fil_system->max_n_open) {

			return;
		}

		/* If the file is already open, no need to do anything; if the
		space does not exist, we handle the situation in the function
		which called this function. */

		if (space == nullptr || space->files.front().is_open) {

			return;
		}

		if (count > 1) {
			print_info = true;
		}

		/* Too many files are open, try to close some */
		do {
			/* Note: This function will release the
			fil_system->mutex when it closes the file. */

			success = fil_try_to_close_file_in_LRU(print_info);

		} while (success
			 && fil_system->n_open >= fil_system->max_n_open);

		if (fil_system->n_open < fil_system->max_n_open) {
			/* Ok */
			return;
		}

		if (count >= 2) {
			ib::warn() << "Too many (" << fil_system->n_open
				<< ") files stay open while the maximum"
				" allowed value would be "
				<< fil_system->max_n_open << ". You may need"
				" to raise the value of innodb_open_files in"
				" my.cnf.";

			return;
		}

		mutex_exit(&fil_system->mutex);

#ifndef UNIV_HOTBACKUP
		/* Wake the i/o-handler threads to make sure pending i/o's are
		performed */
		os_aio_simulated_wake_handler_threads();

		os_thread_sleep(20000);
#endif /* !UNIV_HOTBACKUP */
		/* Flush tablespaces so that we can close modified files in
		the LRU list. */

		fil_flush_file_spaces(to_int(FIL_TYPE_TABLESPACE));

		++count;
	}
}

/** Prepare to free a file object from a tablespace memory cache.
@param[in,out]	file	Tablespace file
@param[in]	space	tablespace */
static
void
fil_node_close_to_free(
	fil_node_t*	file,
	fil_space_t*	space)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(file->magic_n == FIL_NODE_MAGIC_N);
	ut_a(file->n_pending == 0);
	ut_a(file->in_use == 0);

	if (file->is_open) {
		/* We fool the assertion in fil_node_close_file() to think
		there are no unflushed modifications in the file */

		file->modification_counter = file->flush_counter;
		os_event_set(file->sync_event);

		if (fil_buffering_disabled(space)) {

			ut_ad(!space->is_in_unflushed_spaces);
			ut_ad(fil_space_is_flushed(space));

		} else if (space->is_in_unflushed_spaces
			   && fil_space_is_flushed(space)) {

			space->is_in_unflushed_spaces = false;

			UT_LIST_REMOVE(fil_system->unflushed_spaces, space);
		}

		fil_node_close_file(file, false);
	}
}

/** Detach a space object from the tablespace memory cache.
Closes the tablespace files but does not delete them.
There must not be any pending I/O's or flushes on the files.
@param[in,out]	space		tablespace */
static
void
fil_space_detach(fil_space_t* space)
{
	ut_ad(mutex_own(&fil_system->mutex));

	fil_system->names.erase(space->name);

	if (space->is_in_unflushed_spaces) {

		ut_ad(!fil_buffering_disabled(space));
		space->is_in_unflushed_spaces = false;

		UT_LIST_REMOVE(fil_system->unflushed_spaces, space);
	}

	ut_a(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_a(space->n_pending_flushes == 0);

	for (auto& file : space->files) {

		fil_node_close_to_free(&file, space);
	}
}

/** Free a tablespace object on which fil_space_detach() was invoked.
There must not be any pending i/o's or flushes on the files.
@param[in,out]	space		tablespace */
static
void
fil_space_free_low(fil_space_t* space)
{
	ut_ad(srv_fast_shutdown == 2 || space->max_lsn == 0);

	for (auto& file : space->files) {

		ut_d(space->size -= file.size);

		os_event_destroy(file.sync_event);

		ut_free(file.name);
	}

	call_destructor(&space->files);

	ut_ad(space->size == 0);

	rw_lock_free(&space->latch);

	ut_free(space->name);
	ut_free(space);
}

/** Frees a space object from the tablespace memory cache.
Closes a tablespaces' files but does not delete them.
There must not be any pending i/o's or flushes on the files.
@param[in]	id		tablespace identifier
@param[in]	x_latched	whether the caller holds X-mode space->latch
@return true if success */
static
bool
fil_space_free(
	space_id_t	id,
	bool		x_latched)
{
	ut_ad(id != TRX_SYS_SPACE);

	mutex_enter(&fil_system->mutex);
	fil_space_t*	space = fil_space_get_by_id(id);

	if (space != nullptr) {

		fil_space_detach(space);

		fil_system->spaces.erase(id);
	}

	mutex_exit(&fil_system->mutex);

	if (space != nullptr) {
		if (x_latched) {
			rw_lock_x_unlock(&space->latch);
		}

		fil_space_free_low(space);
	}

	return(space != nullptr);
}

/** Create a space memory object and put it to the fil_system hash table.
The tablespace name is independent from the tablespace file-name.
Error messages are issued to the server log.
@param[in]	name	Tablespace name
@param[in]	id	Tablespace identifier
@param[in]	flags	Tablespace flags
@param[in]	purpose	Tablespace purpose
@return pointer to created tablespace, to be filled in with fil_node_create()
@retval nullptr on failure (such as when the same tablespace exists) */
fil_space_t*
fil_space_create(
	const char*	name,
	space_id_t	id,
	ulint		flags,
	fil_type_t	purpose)
{
	ut_ad(fsp_flags_is_valid(flags));
	ut_ad(srv_page_size == UNIV_PAGE_SIZE_ORIG || flags != 0);

	DBUG_EXECUTE_IF("fil_space_create_failure", return(nullptr););

	/* Must set back to active before returning from function. */
	clone_mark_abort(true);

	mutex_enter(&fil_system->mutex);

	/* Look for a matching tablespace. */
	fil_space_t*	space = fil_space_get_by_name(name);

	ut_a(space == nullptr);

	space = fil_space_get_by_id(id);

	ut_a(space == nullptr);

	space = static_cast<fil_space_t*>(ut_zalloc_nokey(sizeof(*space)));

	space->id = id;

	space->name = mem_strdup(name);

	new(&space->files) fil_space_t::Files();

	if (fil_type_is_data(purpose)
	    && !recv_recovery_on
	    && id > fil_system->max_assigned_id
	    && !dict_sys_t::is_reserved(id)) {

		if (!fil_system->space_id_reuse_warned) {

			fil_system->space_id_reuse_warned = true;

			ib::warn()
				<< "Allocated tablespace ID " << id
				<< " for " << name << ", old maximum was "
				<< fil_system->max_assigned_id;
		}

		fil_system->max_assigned_id = id;
	}

	space->purpose = purpose;

	space->flags = flags;

	space->magic_n = FIL_SPACE_MAGIC_N;

	space->encryption_type = Encryption::NONE;

	rw_lock_create(fil_space_latch_key, &space->latch, SYNC_FSP);

	if (space->purpose == FIL_TYPE_TEMPORARY) {
		ut_d(space->latch.set_temp_fsp());
	}

	{
		auto	it = fil_system->spaces.insert(
			Fil_system::Spaces::value_type(id, space));

		ut_a(it.second);
	}

	{
		auto	it = fil_system->names.insert(
			Fil_system::Names::value_type(space->name, space));

		ut_a(it.second);
	}

	if (!dict_sys_t::is_reserved(id) && id > fil_system->max_assigned_id) {

		fil_system->max_assigned_id = id;
	}

	mutex_exit(&fil_system->mutex);

	clone_mark_active();

	return(space);
}

/** Assigns a new space id for a new single-table tablespace. This works
simply by incrementing the global counter. If 4 billion id's is not enough,
we may need to recycle id's.
@param[out]	space_id		Set this to the new tablespace ID
@return true if assigned, false if not */
bool
fil_assign_new_space_id(space_id_t* space_id)
{
	mutex_enter(&fil_system->mutex);

	space_id_t	id = *space_id;

	if (id < fil_system->max_assigned_id) {
		id = fil_system->max_assigned_id;
	}

	++id;

	space_id_t	reserved_space_id = dict_sys_t::reserved_space_id;

	if (id > (reserved_space_id / 2) && (id % 1000000UL == 0)) {

		ib::warn()
			<< "You are running out of new single-table"
			" tablespace id's. Current counter is " << id
			<< " and it must not exceed " << reserved_space_id
			<< "! To reset the counter to zero you have to dump"
			" all your tables and recreate the whole InnoDB"
			" installation.";
	}

	bool	success = !dict_sys_t::is_reserved(id);

	if (success) {

		*space_id = fil_system->max_assigned_id = id;

	} else {

		ib::warn()
			<< "You have run out of single-table tablespace"
			" id's! Current counter is " << id
			<< ". To reset the counter to zero"
			" you have to dump all your tables and"
			" recreate the whole InnoDB installation.";

		*space_id = SPACE_UNKNOWN;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/** Returns a pointer to the fil_space_t that is in the memory cache
associated with a space id. The caller must lock fil_system->mutex.
@param[in]	space_id	Get the tablespace instance or this ID
@return file_space_t pointer, nullptr if space not found */
static
fil_space_t*
fil_space_get_space(space_id_t id)
{
	fil_space_t*	space = fil_space_get_by_id(id);

	if (space == nullptr || space->size != 0) {
		return(space);
	}

	switch (space->purpose) {
	case FIL_TYPE_LOG:
		break;
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_TABLESPACE:
	case FIL_TYPE_IMPORT:
		ut_a(id != 0);

		mutex_exit(&fil_system->mutex);

		/* It is possible that the space gets evicted at this point
		before the fil_mutex_enter_and_prepare_for_io() acquires
		the fil_system->mutex. Check for this after completing the
		call to fil_mutex_enter_and_prepare_for_io(). */
		fil_mutex_enter_and_prepare_for_io(id);

		/* We are still holding the fil_system->mutex. Check if
		the space is still in memory cache. */

		space = fil_space_get_by_id(id);

		if (space == nullptr) {

			return(nullptr);
		}

		ut_a(1 == space->files.size());

		{
			fil_node_t*	file;

			file = &space->files.front();

			/* It must be a single-table tablespace and
			we have not opened the file yet; the following
			calls will open it and update the size fields */

			if (!fil_node_prepare_for_io(
				file, fil_system, space, false)) {

				/* The single-table tablespace can't be opened,
				because the ibd file is missing. */
				
				return(nullptr);
			}

			fil_node_complete_io(file, fil_system, IORequestRead);
		}
	}

	return(space);
}

/** Returns the path from the first fil_node_t found with this space ID.
The caller is responsible for freeing the memory allocated here for the
value returned.
@param[in]	id	Tablespace ID
@return own: A copy of fil_node_t::path, nullptr if space ID is zero
or not found. */
char*
fil_space_get_first_path(space_id_t id)
{
	ut_a(id > 0);

	fil_mutex_enter_and_prepare_for_io(id);

	fil_space_t*	space = fil_space_get_space(id);

	if (space == nullptr) {
		mutex_exit(&fil_system->mutex);

		return(nullptr);
	}

	ut_ad(mutex_own(&fil_system->mutex));

	char*	path = mem_strdup(space->files.front().name);

	mutex_exit(&fil_system->mutex);

	return(path);
}

/** Returns the size of the space in pages. The tablespace must be cached
in the memory cache.
@param[in]	id		Tablespace ID
@return space size, 0 if space not found */
page_no_t
fil_space_get_size(
	space_id_t	id)
{
	fil_space_t*	space;
	page_no_t	size;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_space(id);

	size = space ? space->size : 0;

	mutex_exit(&fil_system->mutex);

	return(size);
}

/** Returns the flags of the space. The tablespace must be cached
in the memory cache.
@param[in]	space_id	Tablespace ID for which to get the flags
@return flags, ULINT_UNDEFINED if space not found */
ulint
fil_space_get_flags(space_id_t space_id)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_space(space_id);

	if (space == nullptr) {

		mutex_exit(&fil_system->mutex);

		return(ULINT_UNDEFINED);
	}

	ulint	flags = space->flags;

	mutex_exit(&fil_system->mutex);

	return(flags);
}

/** Open each file of a tablespace if not already open.
@param[in]	space_id	tablespace identifier
@retval	true	if all file nodes were opened
@retval	false	on failure */
bool
fil_space_open(space_id_t space_id)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	for (auto& file : space->files) {

		if (!file.is_open && !fil_node_open_file(&file, false)) {

			mutex_exit(&fil_system->mutex);

			return(false);
		}
	}

	mutex_exit(&fil_system->mutex);

	return(true);
}

/** Close each file of a tablespace if open.
@param[in]	space_id	tablespace identifier */
void
fil_space_close(space_id_t space_id)
{
	if (fil_system == nullptr) {
		return;
	}

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space == nullptr) {
		mutex_exit(&fil_system->mutex);
		return;
	}

	for (auto& file : space->files) {

		if (file.is_open) {
			fil_node_close_file(&file, false);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/** Returns the page size of the space and whether it is compressed or not.
The tablespace must be cached in the memory cache.
@param[in]	id	space id
@param[out]	found	true if tablespace was found
@return page size */
const page_size_t
fil_space_get_page_size(
	space_id_t	id,
	bool*		found)
{
	const ulint	flags = fil_space_get_flags(id);

	if (flags == ULINT_UNDEFINED) {
		*found = false;
		return(univ_page_size);
	}

	*found = true;

	return(page_size_t(flags));
}

/** Initializes the tablespace memory cache.
@param[in]	max_n_open	Maximum number of open files */
void
fil_init(ulint max_n_open)
{
	ut_a(fil_system == nullptr);

	ut_a(max_n_open > 0);

	fil_system = static_cast<Fil_system*>(
		ut_zalloc_nokey(sizeof(*fil_system)));

	mutex_create(LATCH_ID_FIL_SYSTEM, &fil_system->mutex);

	new(&fil_system->names) Fil_system::Names();
	new(&fil_system->spaces) Fil_system::Spaces();

	UT_LIST_INIT(fil_system->LRU, &fil_node_t::LRU);

	UT_LIST_INIT(fil_system->unflushed_spaces,
		     &fil_space_t::unflushed_spaces);

	fil_system->max_n_open = max_n_open;
}

/** Opens all log files and system tablespace data files. They stay open
until the database server shutdown. This should be called at a server
startup after the space objects for the log and the system tablespace
have been created. The purpose of this operation is to make sure we
never run out of file descriptors if we need to read from the insert
buffer or to write to the log. */
void
fil_open_log_and_system_tablespace_files()
{
	mutex_enter(&fil_system->mutex);

	for (auto& elem : fil_system->spaces) {

		auto	space = elem.second;

		if (fil_space_belongs_in_lru(space)) {

			continue;
		}

		if (space->id == TRX_SYS_SPACE) {

			ut_a(fil_space_t::s_sys_space == nullptr
			     || fil_space_t::s_sys_space == space);

			fil_space_t::s_sys_space = space;
		}

		for (auto& file : space->files) {

			if (!file.is_open) {

				if (!fil_node_open_file(&file, false)) {
					/* This func is called during server's
					startup. If some file of log or system
					tablespace is missing, the server
					can't start successfully. So we should
					assert for it. */
					ut_a(0);
				}
			}

			if (fil_system->max_n_open < 10 + fil_system->n_open) {

				ib::warn()
					<< "You must raise the value of"
					" innodb_open_files in my.cnf!"
					" Remember that InnoDB keeps all"
					" log files and all system"
					" tablespace files open"
					" for the whole time mysqld is"
					" running, and needs to open also"
					" some .ibd files if the"
					" file-per-table storage model is used."
					" Current open files "
					<< fil_system->n_open
					<< ", max allowed open files "
					<< fil_system->max_n_open
					<< ".";
			}
		}
	}

	mutex_exit(&fil_system->mutex);
}

/** Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */
void
fil_close_all_files()
{
	mutex_enter(&fil_system->mutex);

	auto	end = fil_system->spaces.end();

	for (auto it = fil_system->spaces.begin();
	     it != end;
	     it = fil_system->spaces.erase(it)) {

		auto	space = it->second;

		ut_a(space->id == TRX_SYS_SPACE
		     || space->id == dict_sys_t::log_space_first_id
		     || space->files.size() == 1);

		for (auto& file: space->files) {

			if (file.is_open) {
				fil_node_close_file(&file, false);
			}
		}

		fil_space_detach(space);

		fil_space_free_low(space);
	}

	mutex_exit(&fil_system->mutex);
}

/** Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files.
@param[in]	free_all	If set then free all instances */
void
fil_close_log_files(bool free_all)
{
	mutex_enter(&fil_system->mutex);

	auto	end = fil_system->spaces.end();

	for (auto it = fil_system->spaces.begin(); it != end; /* No op */) {

		auto	space = it->second;

		if (space->purpose != FIL_TYPE_LOG) {
			++it;
			continue;
		}

		ut_ad(space->max_lsn == 0);

		for (auto& file : space->files) {

			if (file.is_open) {
				fil_node_close_file(&file, false);
			}
		}

		if (free_all) {

			fil_space_detach(space);
			fil_space_free_low(space);

			it = fil_system->spaces.erase(it);

		} else {
			++it;
		}
	}

	mutex_exit(&fil_system->mutex);
}

/** Iterate through all persistent tablespace files (FIL_TYPE_TABLESPACE)
returning the nodes via callback function cbk.
@param[in]	include_log	include log files
@param[in]	context		callback function context
@param[in]	callback	callback function
@return any error returned by the callback function. */
dberr_t
fil_iterate_tablespace_files(
	bool		include_log,
	void*		context,
	fil_node_cbk_t*	callback)
{
	mutex_enter(&fil_system->mutex);

	for (auto& elem : fil_system->spaces) {

		auto	space = elem.second;

		if (space->purpose != FIL_TYPE_TABLESPACE
		    && (!include_log || space->purpose != FIL_TYPE_LOG)) {

			continue;

		}

		for (auto& file: space->files) {

			dberr_t	err = callback(&file, context);

			if (err != DB_SUCCESS) {

				mutex_exit(&fil_system->mutex);

				return(err);;
			}
		}
	}

	mutex_exit(&fil_system->mutex);

	return(DB_SUCCESS);
}

/** Sets the max tablespace id counter if the given number is bigger than the
previous value.
@param[in]	max_id		Maximum known tablespace ID */
void
fil_set_max_space_id_if_bigger(space_id_t max_id)
{
	if (dict_sys_t::is_reserved(max_id)) {
		ib::fatal() << "Max tablespace id is too high, " << max_id;
	}

	mutex_enter(&fil_system->mutex);

	if (fil_system->max_assigned_id < max_id) {

		fil_system->max_assigned_id = max_id;
	}

	mutex_exit(&fil_system->mutex);
}

/** Write the flushed LSN to the page header of the first page in the
system tablespace.
@param[in]	lsn	flushed LSN
@return DB_SUCCESS or error number */
dberr_t
fil_write_flushed_lsn(
	lsn_t	lsn)
{
	byte*	buf1;
	byte*	buf;
	dberr_t	err;

	buf1 = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	buf = static_cast<byte*>(ut_align(buf1, UNIV_PAGE_SIZE));

	const page_id_t	page_id(TRX_SYS_SPACE, 0);

	err = fil_read(page_id, univ_page_size, 0, univ_page_size.physical(),
		       buf);

	if (err == DB_SUCCESS) {
		mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN, lsn);

		err = fil_write(page_id, univ_page_size, 0,
				univ_page_size.physical(), buf);

		fil_flush_file_spaces(to_int(FIL_TYPE_TABLESPACE));
	}

	ut_free(buf1);

	return(err);
}

#ifndef UNIV_HOTBACKUP
/** Acquire a tablespace when it could be dropped concurrently.
Used by background threads that do not necessarily hold proper locks
for concurrency control.
@param[in]	id	tablespace ID
@param[in]	silent	whether to silently ignore missing tablespaces
@return the tablespace, or nullptr if missing or being deleted */
inline
fil_space_t*
fil_space_acquire_low(
	space_id_t	id,
	bool		silent)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == nullptr) {
		if (!silent) {
			ib::warn()
				<< "Trying to access missing tablespace " << id;
		}
	} else if (space->stop_new_ops) {
		space = nullptr;
	} else {
		++space->n_pending_ops;
	}

	mutex_exit(&fil_system->mutex);

	return(space);
}

/** Acquire a tablespace when it could be dropped concurrently.
Used by background threads that do not necessarily hold proper locks
for concurrency control.
@param[in]	id	tablespace ID
@return the tablespace, or nullptr if missing or being deleted */
fil_space_t*
fil_space_acquire(space_id_t id)
{
	return(fil_space_acquire_low(id, false));
}

/** Acquire a tablespace that may not exist.
Used by background threads that do not necessarily hold proper locks
for concurrency control.
@param[in]	id	tablespace ID
@return the tablespace, or nullptr if missing or being deleted */
fil_space_t*
fil_space_acquire_silent(space_id_t id)
{
	return(fil_space_acquire_low(id, true));
}

/** Release a tablespace acquired with fil_space_acquire().
@param[in,out]	space	tablespace to release  */
void
fil_space_release(fil_space_t* space)
{
	mutex_enter(&fil_system->mutex);

	ut_ad(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_ad(space->n_pending_ops > 0);

	--space->n_pending_ops;

	mutex_exit(&fil_system->mutex);
}

#endif /* !UNIV_HOTBACKUP */

/** File operations for tablespace */
enum fil_operation_t {
	FIL_OPERATION_DELETE,	/*!< delete a single-table tablespace */
	FIL_OPERATION_CLOSE	/*!< close a single-table tablespace */
};

/** Check for pending operations.
@param[in]	space	tablespace
@param[in]	count	number of attempts so far
@return 0 if no operations else count + 1. */
static
ulint
fil_check_pending_ops(
	fil_space_t*	space,
	ulint		count)
{
	ut_ad(mutex_own(&fil_system->mutex));

	const ulint	n_pending_ops = space ? space->n_pending_ops : 0;

	if (n_pending_ops) {

		if (count > 5000) {
			ib::warn()
				<< "Trying to close/delete"
				" tablespace '" << space->name
				<< "' but there are " << n_pending_ops
				<< " pending operations on it.";
		}

		return(count + 1);
	}

	return(0);
}

/** Check for pending IO.
@param[in]	operation	File operation
@param[in,out]	space		Tablespace to check
@param[out]	file		File in space list
@param[in]	count		number of attempts so far
@return 0 if no pending else count + 1. */
static
ulint
fil_check_pending_io(
	fil_operation_t	operation,
	fil_space_t*	space,
	fil_node_t**	file,
	ulint		count)
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(space->n_pending_ops == 0);

	switch (operation) {
	case FIL_OPERATION_DELETE:
	case FIL_OPERATION_CLOSE:
		break;
	}

	ut_a(space->id == TRX_SYS_SPACE
	     || space->id == dict_sys_t::log_space_first_id
	     || space->files.size() == 1);

	*file = &space->files.front();

	if (space->n_pending_flushes > 0 || (*file)->n_pending > 0) {

		ut_a((*file)->in_use == 0);

		if (count > 1000) {
			ib::warn() << "Trying to delete/close"
				" tablespace '" << space->name
				<< "' but there are "
				<< space->n_pending_flushes
				<< " flushes and " << (*file)->n_pending
				<< " pending i/o's on it.";
		}

		return(count + 1);
	}

	return(0);
}

/** Check pending operations on a tablespace.
@param[in]	id		space id
@param[in]	operation	File operation
@param[out]	space		tablespace instance in memory
@param[out]	path		tablespace path
@return DB_SUCCESS or error failure. */
static
dberr_t
fil_check_pending_operations(
	space_id_t	id,
	fil_operation_t	operation,
	fil_space_t**	space,
	char**		path)
{
	ulint		count = 0;

	ut_ad(!fsp_is_system_or_temp_tablespace(id));
	ut_ad(space);

	*space = 0;

	mutex_enter(&fil_system->mutex);

	fil_space_t* sp = fil_space_get_by_id(id);

	if (sp) {
		sp->stop_new_ops = true;
	}
	mutex_exit(&fil_system->mutex);

	/* Check for pending operations. */

	do {
		mutex_enter(&fil_system->mutex);

		sp = fil_space_get_by_id(id);

		count = fil_check_pending_ops(sp, count);

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

	} while (count > 0);

	/* Check for pending IO. */

	*path = 0;

	do {
		mutex_enter(&fil_system->mutex);

		sp = fil_space_get_by_id(id);

		if (sp == nullptr) {
			mutex_exit(&fil_system->mutex);
			return(DB_TABLESPACE_NOT_FOUND);
		}

		fil_node_t*	file;

		count = fil_check_pending_io(operation, sp, &file, count);

		if (count == 0) {
			*path = mem_strdup(file->name);
		}

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

	} while (count > 0);

	ut_ad(sp);

	*space = sp;

	return(DB_SUCCESS);
}

#ifndef UNIV_HOTBACKUP

/* Convert the paths into absolute paths and compare them.
@param[in]	lhs		Filename to compare
@param[in]	rhs		Filename to compare
@return true if they are the same */
bool
fil_paths_equal(const char* lhs, const char* rhs)
{
	std::string	abs_path1(lhs);

	/* Convert to an absolute path */
	if (*lhs == '.') {
		const char*	ptr = lhs;

		while (*ptr == '.' || *ptr == OS_PATH_SEPARATOR) {
			++ptr;
		}

		abs_path1 = fil_get_abs_path(".", ptr);
	}

	/* Remove adjacent duplicate characters, comparison should not be
	affected if the strings are identical. It will remove '//' */

	abs_path1.erase(
		std::unique(
			abs_path1.begin(), abs_path1.end()), abs_path1.end());

	std::string	abs_path2(rhs);

	if (*abs_path2.begin() == '.') {
		const char*	ptr = rhs;

		while (*ptr == '.' || *ptr == OS_PATH_SEPARATOR) {
			++ptr;
		}

		abs_path2 = fil_get_abs_path(".", ptr);
	}

	/* Remove adjacent duplicate characters, comparison should not be
	affected if the strings are identical. It will remove '//' */

	abs_path2.erase(
		std::unique(
			abs_path2.begin(), abs_path2.end()), abs_path2.end());

	return(abs_path1.compare(abs_path2) == 0);
}

/** Fetch the file name opened for a space_id during recovery
from the file map.
@param[in]	space_id	undo tablespace id
@return file name that was opened */
std::string
fil_system_open_fetch(space_id_t space_id)
{
	const auto	names = tablespace_files->find(space_id);

	if (names != nullptr) {
		ut_a(!names->empty());

		return(names->front());
	}

	/* Must be creating a new database instance. */
	return("");
}

/** Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@param[in,out]	trx		Transaction covering the close
@param[in]	id		Tablespace ID
@return DB_SUCCESS or error */
dberr_t
fil_close_tablespace(
	trx_t*		trx,
	space_id_t	id)
{
	dberr_t		err;
	char*		path = nullptr;
	fil_space_t*	space = nullptr;

	ut_ad(!fsp_is_undo_tablespace(id));
	ut_ad(!fsp_is_system_or_temp_tablespace(id));

	err = fil_check_pending_operations(
		id, FIL_OPERATION_CLOSE, &space, &path);

	if (err != DB_SUCCESS) {
		return(err);
	}

	ut_a(path != nullptr);

	rw_lock_x_lock(&space->latch);

	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since we have set space->stop_new_ops = true, readahead
	or ibuf merge can no longer read more pages of this tablespace to the
	buffer pool. Thus we can clean the tablespace out of the buffer pool
	completely and permanently. The flag stop_new_ops also prevents
	fil_flush() from being applied to this tablespace. */

	buf_LRU_flush_or_remove_pages(id, BUF_REMOVE_FLUSH_WRITE, trx);

	/* If the free is successful, the X lock will be released before
	the space memory data structure is freed. */

	if (!fil_space_free(id, true)) {
		rw_lock_x_unlock(&space->latch);
		err = DB_TABLESPACE_NOT_FOUND;
	} else {
		err = DB_SUCCESS;
	}

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */

	char*	cfg_name = fil_make_filepath(path, nullptr, CFG, false);

	if (cfg_name != nullptr) {

		os_file_delete_if_exists(
			innodb_data_file_key, cfg_name, nullptr);

		ut_free(cfg_name);
	}

	char*	cfp_name = fil_make_filepath(path, nullptr, CFP, false);

	if (cfp_name != nullptr) {

		os_file_delete_if_exists(
			innodb_data_file_key, cfp_name, nullptr);

		ut_free(cfp_name);
	}

	ut_free(path);

	return(err);
}

/** Write a log record about an operation on a tablespace file.
@param[in]	type		MLOG_FILE_OPEN or MLOG_FILE_DELETE
				or MLOG_FILE_CREATE2 or MLOG_FILE_RENAME2
@param[in]	space_id	tablespace identifier
@param[in]	path		file path
@param[in]	new_path	if type is MLOG_FILE_RENAME2, the new name
@param[in]	flags		if type is MLOG_FILE_CREATE2, the space flags
@param[in,out]	mtr		mini-transaction */
static
void
fil_op_write_log(
	mlog_id_t	type,
	space_id_t	space_id,
	const char*	path,
	const char*	new_path,
	ulint		flags,
	mtr_t*		mtr)
{
	ut_ad(space_id != TRX_SYS_SPACE);

	byte*		log_ptr;

	log_ptr = mlog_open(mtr, 11 + 4 + 2 + 1);

	if (log_ptr == nullptr) {
		/* Logging in mtr is switched off during crash recovery:
		in that case mlog_open returns nullptr */
		return;
	}

	log_ptr = mlog_write_initial_log_record_low(
		type, space_id, 0, log_ptr, mtr);
 
	if (type == MLOG_FILE_CREATE) {
		mach_write_to_4(log_ptr, flags);
		log_ptr += 4;
 	}
 
	/* Let us store the strings as null-terminated for easier readability
	and handling */

	ulint	len = strlen(path) + 1;

	mach_write_to_2(log_ptr, len);
	log_ptr += 2;

	mlog_close(mtr, log_ptr);

	mlog_catenate_string(
		mtr, reinterpret_cast<const byte*>(path), len);

	switch (type) {
	case MLOG_FILE_RENAME:

		ut_ad(strchr(new_path, OS_PATH_SEPARATOR) != nullptr);

		len = strlen(new_path) + 1;

		log_ptr = mlog_open(mtr, 2 + len);

		mach_write_to_2(log_ptr, len);

		log_ptr += 2;

		mlog_close(mtr, log_ptr);

		mlog_catenate_string(
			mtr, reinterpret_cast<const byte*>(new_path), len);
		break;
	case MLOG_FILE_DELETE:
	case MLOG_FILE_CREATE:
		break;
	default:
		ut_ad(0);
	}
}
 
/** Write redo log for renaming a file.
@param[in]	space_id	tablespace id
@param[in]	old_name	tablespace file name
@param[in]	new_name	tablespace file name after renaming
@param[in,out]	mtr		mini-transaction */
static
void
fil_name_write_rename(
	space_id_t	space_id,
	const char*	old_name,
	const char*	new_name,
	mtr_t*		mtr)
{
	ut_ad(!fsp_is_system_or_temp_tablespace(space_id));
	ut_ad(!fsp_is_undo_tablespace(space_id));

	/* Note: A checkpoint can take place here. */

	fil_op_write_log(
		MLOG_FILE_RENAME, space_id, old_name, new_name, 0, mtr);

	/* Note: A checkpoint can take place here too before we
	have physically renamed the file. */
}

#endif /* !UNIV_HOTBACKUP */

/** Deletes an IBD tablespace, either general or single-table.
The tablespace must be cached in the memory cache. This will delete the
datafile, fil_space_t & fil_node_t entries from the file_system_t cache.
@param[in]	id		Tablespace id
@param[in]	buf_remove	Specify the action to take on the pages
				for this table in the buffer pool.
@return DB_SUCCESS or error */
dberr_t
fil_delete_tablespace(
	space_id_t	id,
	buf_remove_t	buf_remove)
{
	char*		path = nullptr;
	fil_space_t*	space = nullptr;

	ut_ad(!fsp_is_system_or_temp_tablespace(id));
	ut_ad(!fsp_is_undo_tablespace(id));

	dberr_t err = fil_check_pending_operations(
		id, FIL_OPERATION_DELETE, &space, &path);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Cannot delete tablespace " << id
			<< " because it is not found in the tablespace"
			" memory cache.";

		return(err);
	}

	ut_a(path != nullptr);
	ut_a(space != nullptr);

#ifndef UNIV_HOTBACKUP
	/* IMPORTANT: Because we have set space::stop_new_ops there
	can't be any new ibuf merges, reads or flushes. We are here
	because file::n_pending was zero above. However, it is still
	possible to have pending read and write requests:

	A read request can happen because the reader thread has
	gone through the ::stop_new_ops check in buf_page_init_for_read()
	before the flag was set and has not yet incremented ::n_pending
	when we checked it above.

	A write request can be issued any time because we don't check
	the ::stop_new_ops flag when queueing a block for write.

	We deal with pending write requests in the following function
	where we'd minimally evict all dirty pages belonging to this
	space from the flush_list. Note that if a block is IO-fixed
	we'll wait for IO to complete.

	To deal with potential read requests, we will check the
	::stop_new_ops flag in fil_io(). */

	buf_LRU_flush_or_remove_pages(id, buf_remove, 0);

#endif /* !UNIV_HOTBACKUP */

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */
	{
#ifdef UNIV_HOTBACKUP
		/* When replaying the operation in MySQL Enterprise
		Backup, we do not try to write any log record. */
#else /* UNIV_HOTBACKUP */
		/* Before deleting the file, write a log record about
		it, so that InnoDB crash recovery will expect the file
		to be gone. */
		mtr_t		mtr;

		mtr_start(&mtr);

		fil_op_write_log(MLOG_FILE_DELETE, id, path, nullptr, 0, &mtr);

		mtr_commit(&mtr);

		/* Even if we got killed shortly after deleting the
		tablespace file, the record must have already been
		written to the redo log. */
		log_write_up_to(mtr.commit_lsn(), true);

#endif /* UNIV_HOTBACKUP */

		char*	cfg_name = fil_make_filepath(
			path, nullptr, CFG, false);

		if (cfg_name != nullptr) {

			os_file_delete_if_exists(
				innodb_data_file_key, cfg_name, nullptr);

			ut_free(cfg_name);
		}

		char*	cfp_name = fil_make_filepath(
			path, nullptr, CFP, false);

		if (cfp_name != nullptr) {

			os_file_delete_if_exists(
				innodb_data_file_key, cfp_name, nullptr);

			ut_free(cfp_name);
		}
	}

	/* Must set back to active before returning from function. */
	clone_mark_abort(true);
	mutex_enter(&fil_system->mutex);

	/* Double check the sanity of pending ops after reacquiring
	the fil_system::mutex. */
	if (const fil_space_t* s = fil_space_get_by_id(id)) {

		ut_a(s == space);
		ut_a(space->n_pending_ops == 0);
		ut_a(space->files.size() == 1);
		ut_a(space->files.front().n_pending == 0);

		fil_space_detach(space);

		fil_system->spaces.erase(id);

		mutex_exit(&fil_system->mutex);

		fil_space_free_low(space);

		if (!os_file_delete(innodb_data_file_key, path)
		    && !os_file_delete_if_exists(
			    innodb_data_file_key, path, nullptr)) {

			/* Note: This is because we have removed the
			tablespace instance from the cache. */

			err = DB_IO_ERROR;
		}
	} else {
		mutex_exit(&fil_system->mutex);
		err = DB_TABLESPACE_NOT_FOUND;
	}

	ut_free(path);

	clone_mark_active();

	return(err);
}

/** Prepare for truncating a single-table tablespace.
1) Check pending operations on a tablespace;
2) Remove all insert buffer entries for the tablespace;
@param[in]	id		Tablespace ID
@return DB_SUCCESS or error */
static
dberr_t
fil_prepare_for_truncate(space_id_t id)
{
	char*		path = nullptr;
	fil_space_t*	space = nullptr;

	ut_ad(!fsp_is_system_or_temp_tablespace(id));
	ut_ad(fsp_is_undo_tablespace(id));

	dberr_t err = fil_check_pending_operations(
		id, FIL_OPERATION_CLOSE, &space, &path);

	ut_free(path);

	return(err);
}

/** Truncate the tablespace to needed size.
@param[in]	space_id	id of tablespace to truncate
@param[in]	size_in_pages	truncate size.
@return true if truncate was successful. */
bool
fil_truncate_tablespace(
	space_id_t	space_id,
	page_no_t	size_in_pages)
{
	/* Step-1: Prepare tablespace for truncate. This involves
	stopping all the new operations + IO on that tablespace
	and ensuring that related pages are flushed to disk. */
	if (fil_prepare_for_truncate(space_id) != DB_SUCCESS) {
		return(false);
	}

	/* Step-2: Invalidate buffer pool pages belonging to the tablespace
	to re-create. Remove all insert buffer entries for the tablespace */
	buf_LRU_flush_or_remove_pages(space_id, BUF_REMOVE_ALL_NO_WRITE, 0);

	/* Step-3: Truncate the tablespace and accordingly update
	the fil_space_t handler that is used to access this tablespace. */
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	ut_a(space->files.size() == 1);

	fil_node_t&	file = space->files.front();

	ut_ad(file.is_open);

	space->size = file.size = size_in_pages;

	bool success = os_file_truncate(file.name, file.handle, 0);

	if (success) {

		os_offset_t	size = size_in_pages * UNIV_PAGE_SIZE;

		success = os_file_set_size(
			file.name, file.handle, size,
			srv_read_only_mode, true);

		if (success) {
			space->stop_new_ops = false;
		}
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

#ifdef UNIV_DEBUG
/** Increase redo skipped count for a tablespace.
@param[in]	id	space id */
void
fil_space_inc_redo_skipped_count(space_id_t id)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != nullptr);

	space->redo_skipped_count++;

	mutex_exit(&fil_system->mutex);
}

/** Decrease redo skipped count for a tablespace.
@param[in]	id	space id */
void
fil_space_dec_redo_skipped_count(space_id_t id)
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != nullptr);
	ut_a(space->redo_skipped_count > 0);

	space->redo_skipped_count--;

	mutex_exit(&fil_system->mutex);
}

/**
Check whether a single-table tablespace is redo skipped.
@param[in]	id	space id
@return true if redo skipped */
bool
fil_space_is_redo_skipped(space_id_t id)
{
	fil_space_t*	space;
	bool		is_redo_skipped;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != nullptr);

	is_redo_skipped = space->redo_skipped_count > 0;

	mutex_exit(&fil_system->mutex);

	return(is_redo_skipped);
}
#endif

#ifndef UNIV_HOTBACKUP
/** Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. Free all the pages in use by the tablespace.
@param[in]	id		Tablespace ID
@return DB_SUCCESS or error */
dberr_t
fil_discard_tablespace(space_id_t id)
{
	dberr_t	err;

	switch (err = fil_delete_tablespace(id, BUF_REMOVE_ALL_NO_WRITE)) {
	case DB_SUCCESS:
		break;

	case DB_IO_ERROR:
		ib::warn() << "While deleting tablespace " << id
			<< " in DISCARD TABLESPACE. File rename/delete"
			" failed: " << ut_strerr(err);
		break;

	case DB_TABLESPACE_NOT_FOUND:
		ib::warn() << "Cannot delete tablespace " << id
			<< " in DISCARD TABLESPACE: " << ut_strerr(err);
		break;

	default:
		ut_error;
	}

	/* Remove all insert buffer entries for the tablespace */

	ibuf_delete_for_discarded_space(id);

	return(err);
}
#endif /* !UNIV_HOTBACKUP */

/** Allocate and build a file name from a path, a table or tablespace name
and a suffix.
@param[in]	path	nullptr or the direcory path or the full path and filename
@param[in]	name	nullptr if path is full, or Table/Tablespace name
@param[in]	ext	the file extension to use
@param[in]	trim	whether last name on the path should be trimmed
@return own: file name; must be freed by ut_free() */
char*
fil_make_filepath(
	const char*	path,
	const char*	name,
	ib_extention	ext,
	bool		trim)
{
	/* The path may contain the basename of the file, if so we do not
	need the name.  If the path is nullptr, we can use the default path,
	but there needs to be a name. */
	ut_ad(path != nullptr || name != nullptr);

	/* If we are going to strip a name off the path, there better be a
	path and a new name to put back on. */
	ut_ad(!trim || (path != nullptr && name != nullptr));

	if (path == nullptr) {
		path = fil_path_to_mysql_datadir;
	}

	ulint	len		= 0;	/* current length */
	ulint	path_len	= strlen(path);
	ulint	name_len	= (name ? strlen(name) : 0);
	const char* suffix	= dot_ext[ext];
	ulint	suffix_len	= strlen(suffix);
	ulint	full_len	= path_len + 1 + name_len + suffix_len + 1;

	char*	full_name = static_cast<char*>(ut_malloc_nokey(full_len));
	if (full_name == nullptr) {
		return(nullptr);
	}

	/* If the name is a relative path, do not prepend "./". */
	if (path[0] == '.'
	    && (path[1] == '\0' || path[1] == OS_PATH_SEPARATOR)
	    && name != nullptr && name[0] == '.') {
		path = nullptr;
		path_len = 0;
	}

	if (path != nullptr) {
		memcpy(full_name, path, path_len);
		len = path_len;
		full_name[len] = '\0';
		os_normalize_path(full_name);
	}

	if (trim) {
		/* Find the offset of the last DIR separator and set it to
		null in order to strip off the old basename from this path. */
		char* last_dir_sep = strrchr(full_name, OS_PATH_SEPARATOR);
		if (last_dir_sep) {
			last_dir_sep[0] = '\0';
			len = strlen(full_name);
		}
	}

	if (name != nullptr) {
		if (len && full_name[len - 1] != OS_PATH_SEPARATOR) {
			/* Add a DIR separator */
			full_name[len] = OS_PATH_SEPARATOR;
			full_name[++len] = '\0';
		}

		char*	ptr = &full_name[len];
		memcpy(ptr, name, name_len);
		len += name_len;
		full_name[len] = '\0';
		os_normalize_path(ptr);
	}

	/* Make sure that the specified suffix is at the end of the filepath
	string provided. This assumes that the suffix starts with '.'.
	If the first char of the suffix is found in the filepath at the same
	length as the suffix from the end, then we will assume that there is
	a previous suffix that needs to be replaced. */
	if (suffix != nullptr) {

		/* Need room for the trailing null byte. */
		ut_ad(len < full_len);

		if ((len > suffix_len)
		   && (full_name[len - suffix_len] == suffix[0])) {
			/* Another suffix exists, make it the one requested. */
			memcpy(&full_name[len - suffix_len], suffix, suffix_len);

		} else {
			/* No previous suffix, add it. */
			ut_ad(len + suffix_len < full_len);
			memcpy(&full_name[len], suffix, suffix_len);
			full_name[len + suffix_len] = '\0';
		}
	}

	return(full_name);
}

/** Test if a tablespace file can be renamed to a new filepath by checking
if that the old filepath exists and the new filepath does not exist.
@param[in]	space_id	tablespace id
@param[in]	old_path	old filepath
@param[in]	new_path	new filepath
@param[in]	is_discarded	whether the tablespace is discarded
@return innodb error code */
dberr_t
fil_rename_tablespace_check(
	space_id_t	space_id,
	const char*	old_path,
	const char*	new_path,
	bool		is_discarded)
{
	bool	exists = false;
	os_file_type_t	ftype;

	if (!is_discarded
	    && os_file_status(old_path, &exists, &ftype)
	    && !exists) {

		ib::error()
			<< "Cannot rename '" << old_path
			<< "' to '" << new_path
			<< "' for space ID " << space_id
			<< " because the source file"
			<< " does not exist.";
		return(DB_TABLESPACE_NOT_FOUND);
	}

	exists = false;

	if (!os_file_status(new_path, &exists, &ftype) || exists) {

		ib::error()
			<< "Cannot rename '" << old_path
			<< "' to '" << new_path
			<< "' for space ID " << space_id
			<< " because the target file exists."
			" Remove the target file and try again.";
		return(DB_TABLESPACE_EXISTS);
	}

	return(DB_SUCCESS);
}

/** Rename a single-table tablespace.
The tablespace must exist in the memory cache.
@param[in]	id		Tablespace identifier
@param[in]	old_path	Old file name
@param[in]	new_name	New table name in the
				Databasename/tablename format
@param[in]	new_path_in	New file name, or nullptr if it is located
				in the normal data directory
@return true if success */
bool
fil_rename_tablespace(
	space_id_t	id,
	const char*	old_path,
	const char*	new_name,
	const char*	new_path_in)
{
	fil_space_t*	space;
	ulint		count = 0;
	bool		sleep = false;
	bool		flush = false;

	ut_a(id != TRX_SYS_SPACE);
	ut_ad(strchr(new_name, '/') != nullptr);

retry:
	count++;

	if (!(count % 1000)) {
		ib::warn()
			<< "Cannot rename file " << old_path
			<< " (space id " << id << "), retried " << count
			<< " times. There are either pending IOs or flushes or"
			<< " the file is being extended.";
	}

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	DBUG_EXECUTE_IF("fil_rename_tablespace_failure_1", space = nullptr; );

	if (space == nullptr) {

		ib::error()
			<< "Cannot find space id " << id
			<< " in the tablespace memory cache, though the file '"
			<< old_path
			<< "' in a rename operation should have that id.";

		mutex_exit(&fil_system->mutex);
		return(false);

	} else if (count > 25000) {

		space->stop_ios = false;

		mutex_exit(&fil_system->mutex);

		return(false);

	} else if (space != fil_space_get_by_name(space->name)) {

		ib::error()
			<< "Cannot find " << space->name
			<< " in tablespace memory cache";

		space->stop_ios = false;

		mutex_exit(&fil_system->mutex);

		return(false);

	} else {
		
		auto	new_space = fil_space_get_by_name(new_name);

		if (new_space != nullptr) {

			if (new_space == space) {

				mutex_exit(&fil_system->mutex);

				return(true);

			} else if (new_space->id != space->id) {

				ib::error()
					<< new_name
					<< " is already in the tablespace"
					<< " memory cache";

				space->stop_ios = false;

				mutex_exit(&fil_system->mutex);

				return(false);
			}
		}
	}

	/* We temporarily close the .ibd file because we do not trust that
	operating systems can rename an open file. For the closing we have to
	wait until there are no pending I/O's or flushes on the file. */

	space->stop_ios = true;

	ut_a(space->files.size() == 1);

	fil_node_t&	file= space->files.front();

	if (file.n_pending > 0
	    || file.n_pending_flushes > 0
	    || file.in_use > 0) {

		/* There are pending I/O's or flushes or the file is
		currently being extended, sleep for a while and retry */

		sleep = true;

	} else if (file.modification_counter > file.flush_counter) {

		/* Flush the space */

		sleep = flush = true;

	} else if (file.is_open) {

		/* Close the file */

		fil_node_close_file(&file, false);
	}

	mutex_exit(&fil_system->mutex);

	if (sleep) {

		os_thread_sleep(20000);

		if (flush) {
			fil_flush(id);
		}

		sleep = flush = false;
		goto retry;
	}

	ut_ad(space->stop_ios);

	char*	new_file_name = new_path_in == nullptr
		? fil_make_filepath(nullptr, new_name, IBD, false)
		: mem_strdup(new_path_in);

	char*	old_file_name = file.name;
	char*	new_space_name = mem_strdup(new_name);
	char*	old_space_name = space->name;

	ut_ad(strchr(old_file_name, OS_PATH_SEPARATOR) != nullptr);
	ut_ad(strchr(new_file_name, OS_PATH_SEPARATOR) != nullptr);

	mutex_enter(&fil_system->mutex);

	ut_ad(space->name == old_space_name);

	/* We already checked these. */
	ut_ad(space == fil_space_get_by_name(old_space_name));
	ut_ad(!fil_space_get_by_name(new_space_name));
	ut_ad(file.name == old_file_name);

	bool	success;

	DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2", goto skip_rename; );

	success = os_file_rename(
		innodb_data_file_key, old_file_name, new_file_name);

	DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
			skip_rename: success = false; );

	ut_ad(file.name == old_file_name);

	if (success) {
		file.name = new_file_name;
	}

	ut_ad(space->name == old_space_name);

	if (success) {
		fil_system->names.erase(space->name);

		space->name = new_space_name;

		auto	it = fil_system->names.insert(
			Fil_system::Names::value_type(space->name, space));

		ut_a(it.second);
	} else {
		/* Because nothing was renamed, we must free the new
		names, not the old ones. */
		old_file_name = new_file_name;
		old_space_name = new_space_name;
	}

	ut_ad(space->stop_ios);
	space->stop_ios = false;
	mutex_exit(&fil_system->mutex);

	ut_free(old_file_name);
	ut_free(old_space_name);

	return(success);
}

/** Create a tablespace file.
@param[in]	space_id	Tablespace ID
@param[in]	name		Tablespace name in dbname/tablename format.
For general tablespaces, the 'dbname/' part may be missing.
@param[in]	path		Path and filename of the datafile to create.
@param[in]	flags		Tablespace flags
@param[in]	size		Initial size of the tablespace file in pages,
must be >= FIL_IBD_FILE_INITIAL_SIZE
@return DB_SUCCESS or error code */
dberr_t
fil_ibd_create(
	space_id_t	space_id,
	const char*	name,
	const char*	path,
	ulint		flags,
	page_no_t	size)
{
	pfs_os_file_t	file;
	dberr_t		err;
	byte*		buf2;
	byte*		page;
	bool		success;
	bool		has_shared_space = FSP_FLAGS_GET_SHARED(flags);
	fil_space_t*	space = nullptr;

	ut_ad(!fsp_is_system_or_temp_tablespace(space_id));
	ut_ad(!srv_read_only_mode);
	ut_a(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_a(fsp_flags_is_valid(flags));

	/* Create the subdirectories in the path, if they are
	not there already. */
	if (!has_shared_space) {
		err = os_file_create_subdirs_if_needed(path);
		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	file = os_file_create(
		innodb_data_file_key, path,
		OS_FILE_CREATE | OS_FILE_ON_ERROR_NO_EXIT,
		OS_FILE_NORMAL,
		OS_DATA_FILE,
		srv_read_only_mode,
		&success);

	if (!success) {
		/* The following call will print an error message */
		ulint	error = os_file_get_last_error(true);

		ib::error() << "Cannot create file '" << path << "'";

		if (error == OS_FILE_ALREADY_EXISTS) {
			ib::error() << "The file '" << path << "'"
				" already exists though the"
				" corresponding table did not exist"
				" in the InnoDB data dictionary."
				" Have you moved InnoDB .ibd files"
				" around without using the SQL commands"
				" DISCARD TABLESPACE and IMPORT TABLESPACE,"
				" or did mysqld crash in the middle of"
				" CREATE TABLE?"
				" You can resolve the problem by removing"
				" the file '" << path
				<< "' under the 'datadir' of MySQL.";

			return(DB_TABLESPACE_EXISTS);
		}

		if (error == OS_FILE_DISK_FULL) {
			return(DB_OUT_OF_FILE_SPACE);
		}

		return(DB_ERROR);
	}

	bool	atomic_write;

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
	if (fil_fusionio_enable_atomic_write(file)) {

		int     ret = posix_fallocate(
				file.m_file, 0, size * UNIV_PAGE_SIZE);

		if (ret != 0) {

			ib::error() <<
				"posix_fallocate(): Failed to preallocate"
				" data for file " << path
				<< ", desired size "
				<< size * UNIV_PAGE_SIZE
				<< " Operating system error number " << ret
				<< ". Check"
				" that the disk is not full or a disk quota"
				" exceeded. Make sure the file system supports"
				" this function. Some operating system error"
				" numbers are described at " REFMAN
				" operating-system-error-codes.html";

			success = false;
		} else {
			success = true;
		}

		atomic_write = true;
	} else {
		atomic_write = false;

		success = os_file_set_size(
			path, file, size * UNIV_PAGE_SIZE,
			srv_read_only_mode, true);
	}
#else
	atomic_write = false;

	success = os_file_set_size(
		path, file, size * UNIV_PAGE_SIZE,
		srv_read_only_mode, true);

#endif /* !NO_FALLOCATE && UNIV_LINUX */

	if (!success) {
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		return(DB_OUT_OF_FILE_SPACE);
	}

	/* Note: We are actually punching a hole, previous contents will
	be lost after this call, if it succeeds. In this case the file
	should be full of NULs. */

	bool	punch_hole = os_is_sparse_file_supported(path, file);

	if (punch_hole) {

		dberr_t	punch_err;

		punch_err = os_file_punch_hole(
			file.m_file, 0, size * UNIV_PAGE_SIZE);

		if (punch_err != DB_SUCCESS) {
			punch_hole = false;
		}
	}

	/* We have to write the space id to the file immediately and flush the
	file to disk. This is because in crash recovery we must be aware what
	tablespaces exist and what are their space id's, so that we can apply
	the log records to the right file. It may take quite a while until
	buffer pool flush algorithms write anything to the file and flush it to
	disk. If we would not write here anything, the file would be filled
	with zeros from the call of os_file_set_size(), until a buffer pool
	flush would write to it. */

	buf2 = static_cast<byte*>(ut_malloc_nokey(3 * UNIV_PAGE_SIZE));
	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	memset(page, '\0', UNIV_PAGE_SIZE);

	/* Add the UNIV_PAGE_SIZE to the table flags and write them to the
	tablespace header. */
	flags = fsp_flags_set_page_size(flags, univ_page_size);
	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	const page_size_t	page_size(flags);
	IORequest		request(IORequest::WRITE);

	if (!page_size.is_compressed()) {

		buf_flush_init_for_writing(
			nullptr, page, nullptr, 0,
			fsp_is_checksum_disabled(space_id));

		err = os_file_write(
		request, path, file, page, 0, page_size.physical());

		ut_ad(err != DB_IO_NO_PUNCH_HOLE);

	} else {
		page_zip_des_t	page_zip;

		page_zip_set_size(&page_zip, page_size.physical());
		page_zip.data = page + UNIV_PAGE_SIZE;
#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
			page_zip.m_end = page_zip.m_nonempty =
			page_zip.n_blobs = 0;

		buf_flush_init_for_writing(
			nullptr, page, &page_zip, 0,
			fsp_is_checksum_disabled(space_id));

		err = os_file_write(
			request, path, file, page_zip.data, 0,
			page_size.physical());

		ut_a(err != DB_IO_NO_PUNCH_HOLE);

		punch_hole = false;
	}

	ut_free(buf2);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Could not write the first page to"
			<< " tablespace '" << path << "'";

		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);

		return(DB_ERROR);
	}

	success = os_file_flush(file);

	if (!success) {
		ib::error() << "File flush of tablespace '"
			<< path << "' failed";
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		return(DB_ERROR);
	}

	space = fil_space_create(name, space_id, flags, FIL_TYPE_TABLESPACE);

	DEBUG_SYNC_C("fil_ibd_created_space");

	err = fil_node_create_low(
		path, size, space, false, punch_hole, atomic_write)
		? DB_SUCCESS
		: DB_ERROR;

#ifndef UNIV_HOTBACKUP
	if (err == DB_SUCCESS) {
		const auto&	file = space->files.front();

		mtr_t	mtr;

		mtr_start(&mtr);

		fil_op_write_log(
			MLOG_FILE_CREATE, space_id, file.name,
			nullptr, space->flags, &mtr);

		mtr_commit(&mtr);

		DBUG_EXECUTE_IF(
			"fil_ibd_create_log",
			log_make_checkpoint_at(LSN_MAX, true););
	}
#endif /* !UNIV_HOTBACKUP */

	/* For encryption tablespace, initial encryption information. */
	if (space != nullptr && FSP_FLAGS_GET_ENCRYPTION(space->flags)) {

		err = fil_set_encryption(
			space->id, Encryption::AES, nullptr, nullptr);

		ut_ad(err == DB_SUCCESS);
	}

	os_file_close(file);
	if (err != DB_SUCCESS) {
		os_file_delete(innodb_data_file_key, path);
	}

	return(err);
}

#ifndef UNIV_HOTBACKUP
/** Open a single-table tablespace and optionally check the space id is
right in it. If not successful, print an error message to the error log. This
function is used to open a tablespace when we start up mysqld, and also in
IMPORT TABLESPACE.
NOTE that we assume this operation is used either at the database startup
or under the protection of the dictionary mutex, so that two users cannot
race here.

The fil_node_t::handle will not be left open.

@param[in]	validate	whether we should validate the tablespace
				(read the first page of the file and
				check that the space id in it matches id)
@param[in]	purpose		FIL_TYPE_TABLESPACE or FIL_TYPE_TEMPORARY
@param[in]	id		tablespace ID
@param[in]	flags		tablespace flags
@param[in]	space_name	tablespace name of the datafile
If file-per-table, it is the table name in the databasename/tablename format
@param[in]	path_in		expected filepath, usually read from dictionary
@return DB_SUCCESS or error code */
dberr_t
fil_ibd_open(
	bool		validate,
	fil_type_t	purpose,
	space_id_t	id,
	ulint		flags,
	const char*	space_name,
	const char*	path_in)
{
	Datafile	df;
	bool		is_encrypted = FSP_FLAGS_GET_ENCRYPTION(flags);
	bool		for_import = (purpose == FIL_TYPE_IMPORT);

	ut_ad(fil_type_is_data(purpose));

	if (!fsp_flags_is_valid(flags)) {
		return(DB_CORRUPTION);
	}

	df.init(space_name, flags);
	df.make_filepath(nullptr, space_name, IBD);

	if (path_in) {
		df.set_filepath(path_in);
	}

	/* Attempt to open the tablespace at the dictionary filepath. */
	if (df.open_read_only(true) == DB_SUCCESS) {
		ut_ad(df.is_open());
	} else {
		ut_ad(!df.is_open());
		return(DB_CORRUPTION);
	}

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
	const bool	atomic_write = !srv_use_doublewrite_buf
		&& fil_fusionio_enable_atomic_write(df.handle());
#else
	const bool	atomic_write = false;
#endif /* !NO_FALLOCATE && UNIV_LINUX */

	if ((validate || is_encrypted)
	    && df.validate_to_dd(id, flags, for_import) != DB_SUCCESS) {
		if (!is_encrypted) {
			/* The following call prints an error message.
			For encrypted tablespace we skip print, since it should
			be keyring plugin issues. */
			os_file_get_last_error(true);

			ib::error()
				<< "Could not find a valid tablespace file"
			       << " for `" << space_name << "`. "
			       << TROUBLESHOOT_DATADICT_MSG;
		}

		return(DB_CORRUPTION);
	}

	/* If the encrypted tablespace is already opened,
	return success. */
	if (validate && is_encrypted && fil_space_get(id)) {
		return(DB_SUCCESS);
	}

	fil_space_t*	space = fil_space_create(
		space_name, id, flags, purpose);

	/* We do not measure the size of the file, that is why
	we pass the 0 below */

	if (nullptr == fil_node_create_low(
		    df.filepath(), 0, space, false, true, atomic_write)) {

		return(DB_ERROR);
	}

	/* For encryption tablespace, initialize encryption information.*/
	if (is_encrypted && !for_import) {
		dberr_t err;
		byte*	key = df.m_encryption_key;
		byte*	iv = df.m_encryption_iv;
		ut_ad(key && iv);

		err = fil_set_encryption(space->id, Encryption::AES,
					 key, iv);
		if (err != DB_SUCCESS) {
			return(DB_ERROR);
		}
	}

	return(DB_SUCCESS);
}
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_HOTBACKUP
/** Allocates a file name for an old version of a single-table tablespace.
The string must be freed by caller with ut_free()!
@param[in]	name		Original file name
@return own: file name */
static
char*
fil_make_ibbackup_old_name(const char* name)
{
	static const char	suffix[] = "_ibbackup_old_vers_";
	char*			path;
	ulint			len = strlen(name);

	path = static_cast<char*>(ut_malloc_nokey(len + 15 + sizeof(suffix)));

	memcpy(path, name, len);
	memcpy(path + len, suffix, sizeof(suffix) - 1);
	ut_sprintf_timestamp_without_extra_chars(
		path + len + sizeof(suffix) - 1);
	return(path);
}
#endif /* UNIV_HOTBACKUP */

/** Looks for a pre-existing fil_space_t with the given tablespace ID
and, if found, returns the name and filepath in newly allocated buffers
that the caller must free.
@param[in]	space_id	The tablespace ID to search for.
@param[out]	name		Name of the tablespace found.
@param[out]	filepath	The filepath of the first datafile for the
tablespace.
@return true if tablespace is found, false if not. */
bool
fil_space_read_name_and_filepath(
	space_id_t	space_id,
	char**		name,
	char**		filepath)
{
	bool	success = false;

	*name = nullptr;
	*filepath = nullptr;

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space != nullptr) {

		*name = mem_strdup(space->name);

		*filepath = mem_strdup(space->files.front().name);

		success = true;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/** Convert a file name to a tablespace name.
@param[in]	filename	directory/databasename/tablename.ibd
@return database/tablename string, to be freed with ut_free() */
char*
fil_path_to_space_name(
	const char*	filename)
{
	/* Strip the file name prefix and suffix, leaving
	only databasename/tablename. */
	ulint		filename_len	= strlen(filename);
	const char*	end		= filename + filename_len;
#ifdef HAVE_MEMRCHR
	const char*	tablename	= 1 + static_cast<const char*>(
		memrchr(filename, OS_PATH_SEPARATOR,
			filename_len));
	const char*	dbname		= 1 + static_cast<const char*>(
		memrchr(filename, OS_PATH_SEPARATOR,
			tablename - filename - 1));
#else /* HAVE_MEMRCHR */
	const char*	tablename	= filename;
	const char*	dbname		= nullptr;

	while (const char* t = static_cast<const char*>(
		       memchr(tablename, OS_PATH_SEPARATOR,
			      end - tablename))) {
		dbname = tablename;
		tablename = t + 1;
	}
#endif /* HAVE_MEMRCHR */

	ut_ad(dbname != nullptr);
	ut_ad(tablename > dbname);
	ut_ad(tablename < end);
	ut_ad(end - tablename > 4);

	char*	name;

	if (!memcmp(end - 4, DOT_IBD, 4)) {
		name = mem_strdupl(dbname, (end - 4) - dbname);

		ut_ad(name[tablename - dbname - 1] == OS_PATH_SEPARATOR);
#if OS_PATH_SEPARATOR != '/'
		/* space->name uses '/', not OS_PATH_SEPARATOR. */
		name[tablename - dbname - 1] = '/';
#endif
	} else {
		ut_ad(!memcmp(tablename, "undo", 4));
		name = mem_strdupl(filename, filename_len);
	}

	return(name);
}

/** Open an ibd tablespace and add it to the InnoDB data structures.
This is similar to fil_ibd_open() except that it is used while processing
the redo log, so the data dictionary is not available and very little
validation is done. The tablespace name is extracted from the
dbname/tablename.ibd portion of the filename, which assumes that the file
is a file-per-table tablespace.  Any name will do for now.  General
tablespace names will be read from the dictionary after it has been
recovered.  The tablespace flags are read at this time from the first page
of the file in validate_for_recovery().
@param[in]	space_id	tablespace ID
@param[in]	path		path/to/databasename/tablename.ibd
@param[out]	space		the tablespace, or nullptr on error
@return status of the operation */
static
fil_load_status
fil_ibd_open_for_recovery(
	space_id_t		space_id,
	const std::string&	path,
	fil_space_t*&		space)
{
	const char*	filename = path.c_str();

	/* System tablespace open should never come here. It should be
	opened explicitly using the config path. */

	ut_a(space_id != TRX_SYS_SPACE);

	/* If the a space is already in the file system cache with this
	space ID, then there is nothing to do. */
	mutex_enter(&fil_system->mutex);
	space = fil_space_get_by_id(space_id);
	mutex_exit(&fil_system->mutex);

	/* Only TRX_SYS_SPACE may contain multiple nodes. */

	if (space != nullptr) {

		ut_a(space->files.size() == 1);

		const auto&	file = space->files.front();

		if (strcmp(filename, file.name) == 0) {
			return(FIL_LOAD_OK);
		}

		ib::info()
			<< "Ignoring data file '" << filename
			<< "' with space ID " << space->id
			<< ". Another data file called '" << file.name
			<< "' exists with the same space ID";

		space = nullptr;

		return(FIL_LOAD_ID_CHANGED);
	}

	Datafile	file;

	file.set_filepath(filename);

	if (file.open_read_only(false) != DB_SUCCESS) {
		return(FIL_LOAD_NOT_FOUND);
	}

	ut_ad(file.is_open());

	os_offset_t	size;

	/* Read and validate the first page of the tablespace.
	Assign a tablespace name based on the tablespace type. */
	dberr_t	err = file.validate_for_recovery(space_id);

	ut_a(err == DB_SUCCESS);

	if (err == DB_SUCCESS) {

		ut_a(file.space_id() == space_id);

		/* Get and test the file size. */
		size = os_file_get_size(file.handle());

		/* Every .ibd file is created >= 4 pages in size.
		Smaller files cannot be OK. */
		os_offset_t	minimum_size;

		minimum_size = FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE;

		if (size == static_cast<os_offset_t>(-1)) {

			/* The following call prints an error message */
			os_file_get_last_error(true);

			ib::error()
				<< "Could not measure the size of"
				" single-table tablespace file '"
				<< file.filepath() << "'";

		} else if (size < minimum_size) {
#ifndef UNIV_HOTBACKUP
			ib::error()
				<< "The size of tablespace file '"
				<< file.filepath() << "' is only " << size
				<< ", should be at least " << minimum_size
				<< "!";
#else
			/* In MEB, we work around this error. */
			file.set_space_id(SPACE_UNKNOWN);
			file.set_flags(0);
#endif /* !UNIV_HOTBACKUP */
		}
	}

	ut_ad(space == nullptr);

#ifdef UNIV_HOTBACKUP
	if (file.space_id() == SPACE_UNKNOWN || file.space_id() == 0) {
		char*	new_path;

		ib::info()
			<< "Renaming tablespace file '" << file.filepath()
			<< "' with space ID " << file.space_id() << " to "
			<< file.name() << "_ibbackup_old_vers_<timestamp>"
			" because its size " << size() << " is too small"
			" (< 4 pages 16 kB each), or the space id in the"
			" file header is not sensible. This can happen in"
			" an mysqlbackup run, and is not dangerous.";
		file.close();

		new_path = fil_make_ibbackup_old_name(file.filepath());

		bool	success = os_file_rename(
			innodb_data_file_key, file.filepath(), new_path);

		ut_a(success);

		ut_free(new_path);

		return(FIL_LOAD_ID_CHANGED);
	}

	/* A backup may contain the same space several times, if the space got
	renamed at a sensitive time. Since it is enough to have one version of
	the space, we rename the file if a space with the same space id
	already exists in the tablespace memory cache. We rather rename the
	file than delete it, because if there is a bug, we do not want to
	destroy valuable data. */

	mutex_enter(&fil_system->mutex);
	space = fil_space_get_by_id(space_id);
	mutex_exit(&fil_system->mutex);

	if (space != nullptr) {

		ib::info()
			<< "Renaming data file '" << file.filepath()
			<< "' with space ID " << space_id << " to "
			<< file.name()
			<< "_ibbackup_old_vers_<timestamp> because space "
			<< space->name << " with the same id was scanned"
			" earlier. This can happen if you have renamed tables"
			" during an mysqlbackup run.";

		file.close();

		char*	new_path = fil_make_ibbackup_old_name(file.filepath());

		bool	success = os_file_rename(
			innodb_data_file_key, file.filepath(), new_path);

		ut_a(success);

		ut_free(new_path);
		return(FIL_LOAD_OK);
	}
#endif /* UNIV_HOTBACKUP */

	space = fil_space_create(
		file.name(), space_id, file.flags(), FIL_TYPE_TABLESPACE);

	if (space == nullptr) {
		return(FIL_LOAD_INVALID);
	}

	ut_ad(space->id == file.space_id());
	ut_ad(space->id == space_id);

	/* We do not use the size information we have about the file, because
	the rounding formula for extents and pages is somewhat complex; we
	let fil_node_open() do that task. */

	if (!fil_node_create_low(
		file.filepath(), 0, space, false, true, false)) {

		ut_error;
	}

	/* For encryption tablespace, initial encryption information. */
	if (FSP_FLAGS_GET_ENCRYPTION(space->flags)
	    && file.m_encryption_key != nullptr) {

		dberr_t err = fil_set_encryption(
			space->id, Encryption::AES,
			file.m_encryption_key, file.m_encryption_iv);

		if (err != DB_SUCCESS) {
			ib::error() << "Can't set encryption information for"
				" tablespace " << space->name << "!";
		}
	}


	return(FIL_LOAD_OK);
}

/** Report that a tablespace for a table was not found.
@param[in]	name		Table name
@param[in]	space_id	Table's space ID */
static
void
fil_report_missing_tablespace(const char* name, space_id_t space_id)
{
	ib::error()
		<< "Table " << name
		<< " in the InnoDB data dictionary has tablespace id "
		<< space_id << ","
		" but tablespace with that id or name does not exist. Have"
		" you deleted or moved .ibd files?";
}

/** Returns true if a matching tablespace exists in the InnoDB tablespace
memory cache. Note that if we have not done a crash recovery at the database
startup, there may be many tablespaces which are not yet in the memory cache.
@param[in]	id			Tablespace ID
@param[in]	name			Tablespace name used in
					fil_space_create().
@param[in]	print_err_if_not_exist	Print detailed error information to the
					error log if a matching tablespace is
					not found from memory.
@param[in]	adjust_space		Whether to adjust space id on mismatch
@param[in]	heap			Heap memory
@param[in]	table_id		table id
@return true if a matching tablespace exists in the memory cache */
bool
fil_space_for_table_exists_in_mem(
	space_id_t	id,
	const char*	name,
	bool		print_err_if_not_exist,
	bool		adjust_space,
	mem_heap_t*	heap,
	table_id_t	table_id)
{
	fil_space_t*	fnamespace = nullptr;
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	/* Look if there is a space with the same id */

	space = fil_space_get_by_id(id);

	if (space != nullptr
	    && FSP_FLAGS_GET_SHARED(space->flags)
	    && adjust_space
	    && srv_sys_tablespaces_open
	    && 0 == strncmp(space->name, general_space_name,
			    strlen(general_space_name))) {

		fil_system->names.erase(space->name);
		ut_free(space->name);

		space->name = mem_strdup(name);

		auto	it = fil_system->names.insert(
			Fil_system::Names::value_type(space->name, space));

		ut_a(it.second);

		mutex_exit(&fil_system->mutex);

		return(true);
	}

	if (space != nullptr) {
		if (FSP_FLAGS_GET_SHARED(space->flags)
		    && !srv_sys_tablespaces_open) {

			/* No need to check the name */
			mutex_exit(&fil_system->mutex);
			return(true);
		}

		/* If this space has the expected name, use it. */
		fnamespace = fil_space_get_by_name(name);
		if (space == fnamespace) {
			/* Found */

			mutex_exit(&fil_system->mutex);

			return(true);
		}
	}

	/* Info from "fnamespace" comes from the ibd file itself, it can
	be different from data obtained from System tables since file
	operations are not transactional. If adjust_space is set, and the
	mismatching space are between a user table and its temp table, we
	shall adjust the ibd file name according to system table info */
	if (adjust_space
	    && space != nullptr
	    && row_is_mysql_tmp_table_name(space->name)
	    && !row_is_mysql_tmp_table_name(name)) {

		mutex_exit(&fil_system->mutex);

		DBUG_EXECUTE_IF("ib_crash_before_adjust_fil_space",
				DBUG_SUICIDE(););

		if (fnamespace) {
			const char*	tmp_name;

			tmp_name = dict_mem_create_temporary_tablename(
				heap, name, table_id);

			clone_mark_abort(true);

			fil_rename_tablespace(
				fnamespace->id,
				fnamespace->files.front().name,
				tmp_name, nullptr);

			clone_mark_active();
		}

		DBUG_EXECUTE_IF("ib_crash_after_adjust_one_fil_space",
				DBUG_SUICIDE(););

		clone_mark_abort(true);

		fil_rename_tablespace(
			id, space->files.front().name,
			name, nullptr);

		clone_mark_active();

		DBUG_EXECUTE_IF("ib_crash_after_adjust_fil_space",
				DBUG_SUICIDE(););

		mutex_enter(&fil_system->mutex);
		fnamespace = fil_space_get_by_name(name);
		ut_ad(space == fnamespace);
		mutex_exit(&fil_system->mutex);

		return(true);
	}

	if (!print_err_if_not_exist) {

		mutex_exit(&fil_system->mutex);

		return(false);
	}

	if (space == nullptr) {
		if (fnamespace == nullptr) {
			if (print_err_if_not_exist) {
				fil_report_missing_tablespace(name, id);
			}
		} else {
			ib::error() << "Table " << name << " in InnoDB data"
				" dictionary has tablespace id " << id
				<< ", but a tablespace with that id does not"
				" exist. There is a tablespace of name "
				<< fnamespace->name << " and id "
				<< fnamespace->id << ", though. Have you"
				" deleted or moved .ibd files?";
		}
error_exit:
		ib::warn() << TROUBLESHOOT_DATADICT_MSG;

		mutex_exit(&fil_system->mutex);

		return(false);
	}

	if (0 != strcmp(space->name, name)) {

		ib::error() << "Table " << name << " in InnoDB data dictionary"
			" has tablespace id " << id << ", but the tablespace"
			" with that id has name " << space->name << "."
			" Have you deleted or moved .ibd files?";

		if (fnamespace != nullptr) {
			ib::error() << "There is a tablespace with the right"
				" name: " << fnamespace->name << ", but its id"
				" is " << fnamespace->id << ".";
		}

		goto error_exit;
	}

	mutex_exit(&fil_system->mutex);

	return(false);
}

/** Return the space ID based on the tablespace name.
The tablespace must be found in the tablespace memory cache.
This call is made from external to this module, so the mutex is not owned.
@param[in]	tablespace	Tablespace name
@return space ID if tablespace found, SPACE_UNKNOWN if space not. */
space_id_t
fil_space_get_id_by_name(
	const char*	tablespace)
{
	mutex_enter(&fil_system->mutex);

	/* Search for a space with the same name. */
	fil_space_t*	space = fil_space_get_by_name(tablespace);
	space_id_t	id = (space == nullptr) ? SPACE_UNKNOWN : space->id;

	mutex_exit(&fil_system->mutex);

	return(id);
}

/** Fill the pages with NULs
@param[in] file		Tablespace file
@param[in] page_size	physical page size
@param[in] start	Offset from the start of the file in bytes
@param[in] len		Length in bytes
@param[in] read_only_mode
			if true, then read only mode checks are enforced.
@return DB_SUCCESS or error code */
static
dberr_t
fil_write_zeros(
	const fil_node_t*	file,
	ulint			page_size,
	os_offset_t		start,
	ulint			len,
	bool			read_only_mode)
{
	ut_a(len > 0);

	/* Extend at most 1M at a time */
	ulint	n_bytes = ut_min(static_cast<ulint>(1024 * 1024), len);
	byte*	ptr = reinterpret_cast<byte*>(ut_zalloc_nokey(n_bytes
							      + page_size));
	byte*	buf = reinterpret_cast<byte*>(ut_align(ptr, page_size));

	os_offset_t		offset = start;
	dberr_t			err = DB_SUCCESS;
	const os_offset_t	end = start + len;
	IORequest		request(IORequest::WRITE);

	while (offset < end) {

#ifdef UNIV_HOTBACKUP
		err = = os_file_write(
			request, file->name, file->handle, buf, offset,
			n_bytes);
#else
		err = os_aio_func(
			request, OS_AIO_SYNC, file->name,
			file->handle, buf, offset, n_bytes, read_only_mode,
			nullptr, nullptr);
#endif /* UNIV_HOTBACKUP */

		if (err != DB_SUCCESS) {
			break;
		}

		offset += n_bytes;

		n_bytes = ut_min(n_bytes, static_cast<ulint>(end - offset));

		DBUG_EXECUTE_IF("ib_crash_during_tablespace_extension",
				DBUG_SUICIDE(););
	}

	ut_free(ptr);

	return(err);
}

/** Try to extend a tablespace if it is smaller than the specified size.
@param[in,out]	space	tablespace
@param[in]	size	desired size in pages
@return whether the tablespace is at least as big as requested */
bool
fil_space_extend(
	fil_space_t*	space,
	page_no_t	size)
{
	/* In read-only mode we allow write to shared temporary tablespace
	as intrinsic table created by Optimizer reside in this tablespace. */
	ut_ad(!srv_read_only_mode || fsp_is_system_temporary(space->id));

	DBUG_EXECUTE_IF("fil_space_print_xdes_pages",
			space->print_xdes_pages("xdes_pages.log"););
retry:
	bool		success = true;

	fil_mutex_enter_and_prepare_for_io(space->id);

	if (space->size >= size) {
		/* Space already big enough */
		mutex_exit(&fil_system->mutex);
		return(true);
	}

	page_size_t	pageSize(space->flags);
	const ulint	page_size = pageSize.physical();
	fil_node_t*	file = &space->files.back();

	if (file->in_use == 0) {
		/* Mark this file as undergoing extension. This flag
		is used by other threads to wait for the extension
		opereation to finish or wait for open to complete. */
		++file->in_use;
	} else {
		/* Another thread is currently using the file. Wait
		for it to finish.  It'd have been better to use an event
		driven mechanism but the entire module is peppered with
		polling code. */

		mutex_exit(&fil_system->mutex);
		os_thread_sleep(100000);
		goto retry;
	}

	if (!fil_node_prepare_for_io(file, fil_system, space, true)) {
		/* The tablespace data file, such as .ibd file, is missing */
		ut_a(file->in_use > 0);
		--file->in_use;
		mutex_exit(&fil_system->mutex);

		return(false);
	}

	/* At this point it is safe to release fil_system mutex. No
	other thread can rename, delete or close the file because
	we have set the file->in_use flag. */
	mutex_exit(&fil_system->mutex);

	page_no_t	pages_added;

	/* Note: This code is going to be executed independent of FusionIO HW
	if the OS supports posix_fallocate() */

	ut_ad(size > space->size);

	os_offset_t	node_start = os_file_get_size(file->handle);
	ut_a(node_start != (os_offset_t) -1);

	/* File first page number */
	page_no_t	node_first_page = space->size - file->size;

	/* Number of physical pages in the file */
	page_no_t	n_node_physical_pages
		= static_cast<page_no_t>(node_start / page_size);

	/* Number of pages to extend in the file */
	page_no_t	n_node_extend;

	n_node_extend = size - (node_first_page + file->size);

	/* If we already have enough physical pages to satisfy the
	extend request on the file then ignore it */
	if (file->size + n_node_extend > n_node_physical_pages) {

		DBUG_EXECUTE_IF("ib_crash_during_tablespace_extension",
				DBUG_SUICIDE(););

		os_offset_t     len;
		dberr_t		err = DB_SUCCESS;

		len = ((file->size + n_node_extend) * page_size) - node_start;
		ut_ad(len > 0);

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
		/* This is required by FusionIO HW/Firmware */

		int     ret = posix_fallocate(
			file->handle.m_file, node_start, len);

		/* We already pass the valid offset and len in, if EINVAL
		is returned, it could only mean that the file system doesn't
		support fallocate(), currently one known case is
		ext3 FS with O_DIRECT. We ignore EINVAL here so that the
		error message won't flood. */
		if (ret != 0 && ret != EINVAL) {
			ib::error()
				<< "posix_fallocate(): Failed to preallocate"
				" data for file "
				<< file->name << ", desired size "
				<< len << " bytes."
				" Operating system error number "
				<< ret << ". Check"
				" that the disk is not full or a disk quota"
				" exceeded. Make sure the file system supports"
				" this function. Some operating system error"
				" numbers are described at " REFMAN
				" operating-system-error-codes.html";

			err = DB_IO_ERROR;
		}
#endif /* NO_FALLOCATE || !UNIV_LINUX */

		if (!file->atomic_write || err == DB_IO_ERROR) {

			bool	read_only_mode;

			read_only_mode = (space->purpose != FIL_TYPE_TEMPORARY
					  ? false : srv_read_only_mode);

			err = fil_write_zeros(
				file, page_size, node_start,
				static_cast<ulint>(len), read_only_mode);

			if (err != DB_SUCCESS) {

				ib::warn()
					<< "Error while writing " << len
					<< " zeroes to " << file->name
					<< " starting at offset " << node_start;
			}
		}

		/* Check how many pages actually added */
		os_offset_t	end = os_file_get_size(file->handle);
		ut_a(end != static_cast<os_offset_t>(-1) && end >= node_start);

		os_has_said_disk_full = !(success = (end == node_start + len));

		pages_added = static_cast<page_no_t>(end / page_size);

		ut_a(pages_added >= file->size);
		pages_added -= file->size;

	} else {
		success = true;
		pages_added = n_node_extend;
		os_has_said_disk_full = FALSE;
	}

	mutex_enter(&fil_system->mutex);

	file->size += pages_added;
	space->size += pages_added;

	ut_a(file->in_use > 0);
	--file->in_use;

	fil_node_complete_io(file, fil_system, IORequestWrite);

#ifndef UNIV_HOTBACKUP
	/* Keep the last data file size info up to date, rounded to
	full megabytes */
	page_no_t	pages_per_mb = static_cast<page_no_t>(
		(1024 * 1024) / page_size);

	page_no_t	size_in_pages = 
		((file->size / pages_per_mb) * pages_per_mb);

	if (space->id == TRX_SYS_SPACE) {
		srv_sys_space.set_last_file_size(size_in_pages);
	} else if (fsp_is_system_temporary(space->id)) {
		srv_tmp_space.set_last_file_size(size_in_pages);
	}
#endif /* !UNIV_HOTBACKUP */

	mutex_exit(&fil_system->mutex);

	fil_flush(space->id);

	return(success);
}

#ifdef UNIV_HOTBACKUP
/** Extends all tablespaces to the size stored in the space header. During the
mysqlbackup --apply-log phase we extended the spaces on-demand so that log
records could be applied, but that may have left spaces still too small
compared to the size stored in the space header. */
void
fil_extend_tablespaces_to_stored_len()
{
	byte*		buf;
	ulint		actual_size;
	ulint		size_in_header;
	dberr_t		error;
	bool		success;

	buf = ut_malloc_nokey(UNIV_PAGE_SIZE);

	mutex_enter(&fil_system->mutex);

	for (auto& elem : fil_system->spaces) {

		auto	space = elem.second;

		ut_a(space->purpose == FIL_TYPE_TABLESPACE);

		/* No need to protect with a mutex, because this is
		a single-threaded operation */

		mutex_exit(&fil_system->mutex);

		error = fil_read(
			page_id_t(space->id, 0),
			page_size_t(space->flags),
			0, univ_page_size.physical(), buf);

		ut_a(error == DB_SUCCESS);

		size_in_header = fsp_header_get_field(buf, FSP_SIZE);

		success = fil_space_extend(space, size_in_header);

		if (!success) {
			ib::error() << "Could not extend the tablespace of "
				<< space->name  << " to the size stored in"
				" header, " << size_in_header << " pages;"
				" size after extension " << actual_size
				<< " pages. Check that you have free disk"
				" space and retry!";
			ut_a(success);
		}

		mutex_enter(&fil_system->mutex);
	}

	mutex_exit(&fil_system->mutex);

	ut_free(buf);
}
#endif

/*========== RESERVE FREE EXTENTS (for a B-tree split, for example) ===*/

/** Tries to reserve free extents in a file space.
@param[in]	id		Tablespace ID
@param[in]	n_free_no	Number of free extents now
@param[in]	n_to_reserve	How many one wants to reserve
@return true if succeed */
bool
fil_space_reserve_free_extents(
	space_id_t	id,
	ulint		n_free_now,
	ulint		n_to_reserve)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);

	bool		success;

	if (space->n_reserved_extents + n_to_reserve > n_free_now) {
		success = false;
	} else {
		space->n_reserved_extents += n_to_reserve;
		success = true;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/** Releases free extents in a file space.
@param[in]	id		Tablespace ID
@param[in]	n_reserved	How many were reserved */
void
fil_space_release_free_extents(
	space_id_t	id,
	ulint		n_reserved)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);

	ut_a(space->n_reserved_extents >= n_reserved);

	space->n_reserved_extents -= n_reserved;

	mutex_exit(&fil_system->mutex);
}

/** Gets the number of reserved extents. If the database is silent, this number
should be zero.
@param[in]	id		Tablespace ID
@return the number of reserved extents */
ulint
fil_space_get_n_reserved_extents(space_id_t id)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);

	ulint	n = space->n_reserved_extents;

	mutex_exit(&fil_system->mutex);

	return(n);
}

/*============================ FILE I/O ================================*/

/** NOTE: you must call fil_mutex_enter_and_prepare_for_io() first!

Prepares a file for i/o. Opens the file if it is closed. Updates the
pending i/o's field in the file and the system appropriately. Takes the file 
off the LRU list if it is in the LRU list. The caller must hold the fil_sys
mutex.
@param[in]	file		Tablespace file
@param[in]	system		Tablespace memory cache
@param[in]	space		Tablespace instance
@param[in]	extend		true if file is being extended
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_prepare_for_io(
	fil_node_t*	file,
	Fil_system*	system,
	fil_space_t*	space,
	bool		extend)
{
	ut_ad(mutex_own(&(system->mutex)));

	if (system->n_open > system->max_n_open + 5) {

		static ulint	prev_time;
		auto		curr_time = ut_time();

		/* Spam the log after every minute. Ignore any race here. */

		if ((curr_time - prev_time) > 60) {

			ib::warn()
				<< "Open files " << system->n_open
				<< " exceeds the limit "
				<< system->max_n_open;

			prev_time = curr_time;
		}
	}

	if (!file->is_open) {

		/* File is closed: open it */
		ut_a(file->n_pending == 0);

		if (!fil_node_open_file(file, extend)) {
			return(false);
		}
	}

	if (file->n_pending == 0 && fil_space_belongs_in_lru(space)) {
		/* The file is in the LRU list, remove it */

		ut_a(UT_LIST_GET_LEN(system->LRU) > 0);

		UT_LIST_REMOVE(system->LRU, file);
	}

	++file->n_pending;

	return(true);
}

/** Updates the data structures when an i/o operation finishes. Updates the
pending i/o's field in the file appropriately.
@param[in]	file		Tablespace file 
@param[in]	system		Tablespace memory cache
@param[in]	type		Marks the file as modified if type == WRITE */
static
void
fil_node_complete_io(
	fil_node_t*		file,
	Fil_system*		system,
	const IORequest&	type)
{
	ut_ad(mutex_own(&system->mutex));
	ut_a(file->n_pending > 0);

	--file->n_pending;

	ut_ad(type.validate());

	if (type.is_write()) {

		ut_ad(!srv_read_only_mode
		      || fsp_is_system_temporary(file->space->id));

		++system->modification_counter;

		file->modification_counter = system->modification_counter;

		if (fil_buffering_disabled(file->space)) {

			/* We don't need to keep track of unflushed
			changes as user has explicitly disabled
			buffering. */
			ut_ad(!file->space->is_in_unflushed_spaces);
			file->flush_counter = file->modification_counter;

		} else if (!file->space->is_in_unflushed_spaces) {

			file->space->is_in_unflushed_spaces = true;

			UT_LIST_ADD_FIRST(
				system->unflushed_spaces, file->space);
		}
	}

	if (file->n_pending == 0 && fil_space_belongs_in_lru(file->space)) {

		/* The file must be put back to the LRU list */
		UT_LIST_ADD_FIRST(system->LRU, file);
	}
}

/** Report information about an invalid page access.
@param[in]	block_offset	Block offset
@param[in]	space_id	Tablespace ID
@param[in]	space_name	Tablespace name
@param[in]	byte_offset	Byte offset
@param[in]	len		I/O length
@param[in]	is_read		I/O type */
static
void
fil_report_invalid_page_access(
	page_no_t	block_offset,
	space_id_t	space_id,
	const char*	space_name,
	ulint		byte_offset,
	ulint		len,
	bool		is_read)
{
	ib::error()
		<< "Trying to access page number " << block_offset << " in"
		" space " << space_id << ", space name " << space_name << ","
		" which is outside the tablespace bounds. Byte offset "
		<< byte_offset << ", len " << len << ", i/o type " <<
		(is_read ? "read" : "write")
		<< ". If you get this error at mysqld startup, please check"
		" that your my.cnf matches the ibdata files that you have in"
		" the MySQL server.";

	ib::error() << "Server exits"
#ifdef UNIV_DEBUG
		<< " at " << __FILE__ << "[" << __LINE__ << "]"
#endif
		<< ".";

	_exit(1);
}

/** Set encryption information for IORequest.
@param[in,out]	req_type	IO request
@param[in]	page_id		page id
@param[in]	space		table space */
void
fil_io_set_encryption(
	IORequest&		req_type,
	const page_id_t&	page_id,
	fil_space_t*		space)
{
	/* Don't encrypt page 0 of all tablespaces except redo log
	tablespace, all pages from the system tablespace. */
	if (space->encryption_type == Encryption::NONE
	    || (page_id.page_no() == 0 && !req_type.is_log())) {
		req_type.clear_encrypted();
		return;
	}

	/* For writting redo log, if encryption for redo log is disabled,
	skip set encryption. */
	if (req_type.is_log() && req_type.is_write()
	    && !srv_redo_log_encrypt) {
		req_type.clear_encrypted();
		return;
	}

	/* For writting undo log, if encryption for undo log is disabled,
	skip set encryption. */
	if (fsp_is_undo_tablespace(space->id)
	    && !srv_undo_log_encrypt && req_type.is_write()) {
		req_type.clear_encrypted();
		return;
	}

	/* Make any active clone operation to abort, in case
	log encryption is set after clone operation is started. */
	clone_mark_abort(true);
	clone_mark_active();

	req_type.encryption_key(space->encryption_key,
				space->encryption_klen,
				space->encryption_iv);
	req_type.encryption_algorithm(Encryption::AES);
}

/** Read or write data. This operation could be asynchronous (aio).
@param[in,out]	type		IO context
@param[in]	sync		whether synchronous aio is desired
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to read or write; this must
not cross a file boundary; in aio this must be a block size multiple
@param[in,out]	buf		buffer where to store read data or from where
to write; in aio this must be appropriately aligned
@param[in]	message		message for aio handler if !sync, else ignored
@return error code
@retval DB_SUCCESS on success
@retval DB_TABLESPACE_DELETED if the tablespace does not exist */
dberr_t
fil_io(
	const IORequest&	type,
	bool			sync,
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf,
	void*			message)
{
	os_offset_t		offset;
	IORequest		req_type(type);

	ut_ad(req_type.validate());

	ut_ad(len > 0);
	ut_ad(byte_offset < UNIV_PAGE_SIZE);
	ut_ad(!page_size.is_compressed() || byte_offset == 0);
	ut_ad(UNIV_PAGE_SIZE == (ulong)(1 << UNIV_PAGE_SIZE_SHIFT));
#if (1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX"
#endif
#if (1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN"
#endif
	ut_ad(fil_validate_skip());

#ifndef UNIV_HOTBACKUP

	/* ibuf bitmap pages must be read in the sync AIO mode: */
	ut_ad(recv_no_ibuf_operations
	      || req_type.is_write()
	      || !ibuf_bitmap_page(page_id, page_size)
	      || sync
	      || req_type.is_log());

	ulint	mode;

	if (sync) {

		mode = OS_AIO_SYNC;

	} else if (req_type.is_log()) {

		mode = OS_AIO_LOG;

	} else if (req_type.is_read()
		   && !recv_no_ibuf_operations
		   && ibuf_page(page_id, page_size, nullptr)) {

		mode = OS_AIO_IBUF;

		/* Reduce probability of deadlock bugs in connection with ibuf:
		do not let the ibuf i/o handler sleep */

		req_type.clear_do_not_wake();
	} else {
		mode = OS_AIO_NORMAL;
	}
#else /* !UNIV_HOTBACKUP */
	ut_a(sync);
	mode = OS_AIO_SYNC;
#endif /* !UNIV_HOTBACKUP */

	if (req_type.is_read()) {

		srv_stats.data_read.add(len);

	} else if (req_type.is_write()) {

		ut_ad(!srv_read_only_mode
		      || fsp_is_system_temporary(page_id.space()));

		srv_stats.data_written.add(len);
	}

	/* Reserve the fil_system mutex and make sure that we can open at
	least one file while holding it, if the file is not already open */

	fil_mutex_enter_and_prepare_for_io(page_id.space());

	fil_space_t*	space = fil_space_get_by_id(page_id.space());

	/* If we are deleting a tablespace we don't allow async read
	operations on that. However, we do allow write operations and
	sync read operations. */
	if (space == nullptr
	    || (req_type.is_read()
		&& !sync
		&& space->stop_new_ops)) {

		mutex_exit(&fil_system->mutex);

		if (!req_type.ignore_missing()) {
			if (space == nullptr) {
				ib::error()
					<< "Trying to do I/O on a tablespace"
					<< " which does not exist. I/O type: "
					<< (req_type.is_read()
					    ? "read" : "write")
					<< ", page: " << page_id
					<< ", I/O length: " << len << " bytes";
			} else {
				ib::error()
					<< "Trying to do async read on a"
					<< " tablespace which is being deleted."
					<< " Tablespace name: \"" << space->name
					<< "\", page: " << page_id
					<< ", read length: " << len << " bytes";
			}
		}

		return(DB_TABLESPACE_DELETED);
	}

	ut_ad(mode != OS_AIO_IBUF || fil_type_is_data(space->purpose));

	fil_node_t*	file = nullptr;
	page_no_t	cur_page_no = page_id.page_no();

	if (space->files.size() > 1) {

		ut_a(space->id == TRX_SYS_SPACE
		     || space->id == dict_sys_t::log_space_first_id);

		for (auto& f : space->files) {

			if (f.size > cur_page_no) {
				file = &f;
				break;

			} else {
				cur_page_no -= file->size;
			}
		}

	} else if (space->files.empty()) {

		if (req_type.ignore_missing()) {
			mutex_exit(&fil_system->mutex);
			return(DB_ERROR);
		}

		/* This is a hard error. */
		fil_report_invalid_page_access(
			page_id.page_no(), page_id.space(),
			space->name, byte_offset, len,
			req_type.is_read());

	} else {

		fil_node_t&	f = space->files.front();

		if ((fsp_is_ibd_tablespace(space->id) && f.size == 0)
		    || f.size > cur_page_no) {

			/* We do not know the size of a single-table tablespace
			before we open the file */

			file = &f;

		} else if (space->id != TRX_SYS_SPACE
			   && req_type.is_read()
			   && undo::is_inactive(space->id)) {

			/* Page access request for a page that is
			outside the truncated UNDO tablespace bounds. */

			mutex_exit(&fil_system->mutex);

			return(DB_TABLESPACE_DELETED);

		} else {

			/* This is a hard error. */
			fil_report_invalid_page_access(
				page_id.page_no(), page_id.space(),
				space->name, byte_offset, len,
				req_type.is_read());
		}
	}

	/* Open file if closed */
	if (!fil_node_prepare_for_io(file, fil_system, space, false)) {

		if (fil_type_is_data(space->purpose)
		    && fsp_is_ibd_tablespace(space->id)) {

			mutex_exit(&fil_system->mutex);

			if (!req_type.ignore_missing()) {

				ib::error()
					<< "Trying to do I/O to a tablespace"
					" which exists without .ibd data file."
					" I/O type: "
					<< (req_type.is_read()
					    ? "read" : "write")
					<< ", page: "
					<< page_id_t(page_id.space(),
						     cur_page_no)
					<< ", I/O length: " << len << " bytes";
			}

			return(DB_TABLESPACE_DELETED);
		}

		/* The tablespace is for log. Currently, we just assert here
		to prevent handling errors along the way fil_io returns.
		Also, if the log files are missing, it would be hard to
		promise the server can continue running. */
		ut_a(0);
	}

	/* Check that at least the start offset is within the bounds of a
	single-table tablespace, including rollback tablespaces. */
	if (file->size <= cur_page_no
	    && space->id != TRX_SYS_SPACE
	    && fil_type_is_data(space->purpose)) {

		if (req_type.ignore_missing()) {
			/* If we can tolerate the non-existent pages, we
			should return with DB_ERROR and let caller decide
			what to do. */
			fil_node_complete_io(file, fil_system, req_type);
			mutex_exit(&fil_system->mutex);
			return(DB_ERROR);
		}

		fil_report_invalid_page_access(
			page_id.page_no(), page_id.space(),
			space->name, byte_offset, len, req_type.is_read());
	}

	/* Now we have made the changes in the data structures of fil_system */
	mutex_exit(&fil_system->mutex);

	/* Calculate the low 32 bits and the high 32 bits of the file offset */

	if (!page_size.is_compressed()) {

		offset = ((os_offset_t) cur_page_no
			  << UNIV_PAGE_SIZE_SHIFT) + byte_offset;

		ut_a(file->size - cur_page_no
		     >= ((byte_offset + len + (UNIV_PAGE_SIZE - 1))
			 / UNIV_PAGE_SIZE));
	} else {
		ulint	size_shift;

		switch (page_size.physical()) {
		case 1024: size_shift = 10; break;
		case 2048: size_shift = 11; break;
		case 4096: size_shift = 12; break;
		case 8192: size_shift = 13; break;
		case 16384: size_shift = 14; break;
		case 32768: size_shift = 15; break;
		case 65536: size_shift = 16; break;
		default: ut_error;
		}

		offset = ((os_offset_t) cur_page_no << size_shift)
			+ byte_offset;

		ut_a(file->size - cur_page_no
		     >= (len + (page_size.physical() - 1))
		     / page_size.physical());
	}

	/* Do AIO */

	ut_a(byte_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a((len % OS_FILE_LOG_BLOCK_SIZE) == 0);

	/* Don't compress the log, page 0 of all tablespaces, tables
	compresssed with the old scheme and all pages from the system
	tablespace. */

	if (req_type.is_write()
	    && !req_type.is_log()
	    && !page_size.is_compressed()
	    && page_id.page_no() > 0
	    && IORequest::is_punch_hole_supported()
	    && file->punch_hole) {

		ut_ad(!req_type.is_log());

		req_type.set_punch_hole();

		req_type.compression_algorithm(space->compression_type);

	} else {
		req_type.clear_compressed();
	}

	/* Set encryption information. */
	fil_io_set_encryption(req_type, page_id, space);

	req_type.block_size(file->block_size);

	dberr_t	err;

#ifdef UNIV_HOTBACKUP
	/* In mysqlbackup do normal i/o, not aio */
	if (req_type.is_read()) {

		err = os_file_read(req_type, file->handle, buf, offset, len);

	} else {

		ut_ad(!srv_read_only_mode
		      || fsp_is_system_temporary(page_id.space()));

		err = os_file_write(
			req_type, file->name, file->handle, buf, offset, len);
	}
#else
	/* Queue the aio request */
	err = os_aio(
		req_type,
		mode, file->name, file->handle, buf, offset, len,
		fsp_is_system_temporary(page_id.space())
		? false : srv_read_only_mode,
		file, message);

#endif /* UNIV_HOTBACKUP */

	if (err == DB_IO_NO_PUNCH_HOLE) {

		err = DB_SUCCESS;

		if (file->punch_hole) {

			ib::warn()
				<< "Punch hole failed for '"
				<< file->name << "'";
		}

		fil_no_punch_hole(file);
	}

	/* We an try to recover the page from the double write buffer if
	the decompression fails or the page is corrupt. */

	ut_a(req_type.is_dblwr_recover() || err == DB_SUCCESS);

	if (sync) {
		/* The i/o operation is already completed when we return from
		os_aio: */

		mutex_enter(&fil_system->mutex);

		fil_node_complete_io(file, fil_system, req_type);

		mutex_exit(&fil_system->mutex);

		ut_ad(fil_validate_skip());
	}

	return(err);
}

#ifndef UNIV_HOTBACKUP
/** Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for.
@param[in]	segment		The number of the segment in the AIO array
				to wait for */
void
fil_aio_wait(ulint segment)
{
	fil_node_t*	file;
	IORequest	type;
	void*		message;

	ut_ad(fil_validate_skip());

	dberr_t	err = os_aio_handler(segment, &file, &message, &type);

	ut_a(err == DB_SUCCESS);

	if (file == nullptr) {
		ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
		return;
	}

	srv_set_io_thread_op_info(segment, "complete io for file");

	mutex_enter(&fil_system->mutex);

	fil_node_complete_io(file, fil_system, type);

	mutex_exit(&fil_system->mutex);

	ut_ad(fil_validate_skip());

	/* Do the i/o handling */
	/* IMPORTANT: since i/o handling for reads will read also the insert
	buffer in tablespace 0, you have to be very careful not to introduce
	deadlocks in the i/o system. We keep tablespace 0 data files always
	open, and use a special i/o thread to serve insert buffer requests. */

	switch (file->space->purpose) {
	case FIL_TYPE_TABLESPACE:
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_IMPORT:
		srv_set_io_thread_op_info(segment, "complete io for buf page");

		/* async single page writes from the dblwr buffer don't have
		access to the page */
		if (message != nullptr) {
			buf_page_io_complete(static_cast<buf_page_t*>(message));
		}
		return;
	case FIL_TYPE_LOG:
		srv_set_io_thread_op_info(segment, "complete io for log");
		log_io_complete(static_cast<log_group_t*>(message));
		return;
	}

	ut_ad(0);
}
#endif /* UNIV_HOTBACKUP */

/** Flushes to disk possible writes cached by the OS. If the space does
not exist or is being dropped, does not do anything.
@param[in]	space_id	File space ID (this can be a group of log files
				or a tablespace of the database) */
void
fil_flush(space_id_t space_id)
{
	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space == nullptr
	    || space->purpose == FIL_TYPE_TEMPORARY
	    || space->stop_new_ops) {

		mutex_exit(&fil_system->mutex);

		return;
	}

	if (fil_buffering_disabled(space)) {

		/* No need to flush. User has explicitly disabled
		buffering. */
		ut_ad(!space->is_in_unflushed_spaces);
		ut_ad(fil_space_is_flushed(space));
		ut_ad(space->n_pending_flushes == 0);

#ifdef UNIV_DEBUG
		for (const auto& file : space->files) {
			ut_ad(file.modification_counter == file.flush_counter);
			ut_ad(file.n_pending_flushes == 0);
		}
#endif /* UNIV_DEBUG */

		mutex_exit(&fil_system->mutex);
		return;
	}

	/* Prevent dropping of the space while we are flushing */
	++space->n_pending_flushes;

	for (auto& file : space->files) {

		int64_t	old_mod_counter = file.modification_counter;

		if (old_mod_counter <= file.flush_counter) {
			continue;
		}

		ut_a(file.is_open);

		switch (space->purpose) {
		case FIL_TYPE_TEMPORARY:
			ut_ad(0); // we already checked for this

		case FIL_TYPE_TABLESPACE:
		case FIL_TYPE_IMPORT:
			++fil_n_pending_tablespace_flushes;
			break;

		case FIL_TYPE_LOG:
			++fil_n_log_flushes;
			++fil_n_pending_log_flushes;
			break;
		}
#ifdef _WIN32
		if (file.is_raw_disk) {

			goto skip_flush;
		}
#endif /* _WIN32 */

		bool	skip_flush = false;

		while (file.n_pending_flushes > 0 && !skip_flush) {

			/* We want to avoid calling os_file_flush() on
			the file twice at the same time, because we do
			not know what bugs OS's may contain in file
			I/O */

			int64_t	sig_count = os_event_reset(file.sync_event);

			mutex_exit(&fil_system->mutex);

			os_event_wait_low(file.sync_event, sig_count);

			mutex_enter(&fil_system->mutex);

			if (file.flush_counter >= old_mod_counter) {

				skip_flush = true;
			}
		}

		if (!skip_flush) {

			ut_a(file.is_open);

			++file.n_pending_flushes;

			mutex_exit(&fil_system->mutex);

			os_file_flush(file.handle);

			mutex_enter(&fil_system->mutex);

			os_event_set(file.sync_event);

			--file.n_pending_flushes;
		} 

		if (file.flush_counter < old_mod_counter) {

			file.flush_counter = old_mod_counter;

			if (space->is_in_unflushed_spaces
			    && fil_space_is_flushed(space)) {

				space->is_in_unflushed_spaces = false;

				UT_LIST_REMOVE(
					fil_system->unflushed_spaces,
					space);
			}
		}

		switch (space->purpose) {
		case FIL_TYPE_TEMPORARY:
			ut_ad(0); // we already checked for this

		case FIL_TYPE_TABLESPACE:
		case FIL_TYPE_IMPORT:
			--fil_n_pending_tablespace_flushes;
			continue;

		case FIL_TYPE_LOG:
			--fil_n_pending_log_flushes;
			continue;
		}

		ut_ad(0);
	}

	--space->n_pending_flushes;

	mutex_exit(&fil_system->mutex);
}

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS.
@param[in]	purpose	FIL_TYPE_TABLESPACE or FIL_TYPE_LOG, can be ORred */
void
fil_flush_file_spaces(uint8_t purpose)
{
	ulint		n_space_ids;

	ut_ad((purpose & FIL_TYPE_TABLESPACE) || (purpose & FIL_TYPE_LOG));

	mutex_enter(&fil_system->mutex);

	n_space_ids = UT_LIST_GET_LEN(fil_system->unflushed_spaces);
	if (n_space_ids == 0) {

		mutex_exit(&fil_system->mutex);
		return;
	}

	/* Assemble a list of space ids to flush.  Previously, we
	traversed fil_system->unflushed_spaces and called UT_LIST_GET_NEXT()
	on a space that was just removed from the list by fil_flush().
	Thus, the space could be dropped and the memory overwritten. */
	using Space_Ids = std::vector<space_id_t, ut_allocator<space_id_t>>;

	Space_Ids	space_ids;

	space_ids.reserve(n_space_ids);

	n_space_ids = 0;

	for (auto space = UT_LIST_GET_FIRST(fil_system->unflushed_spaces);
	     space != nullptr;
	     space = UT_LIST_GET_NEXT(unflushed_spaces, space)) {

		if ((to_int(space->purpose) & purpose)
		    && !space->stop_new_ops) {

			space_ids.push_back(space->id);
		}
	}

	mutex_exit(&fil_system->mutex);

	/* Flush the spaces.  It will not hurt to call fil_flush() on
	a non-existing space id. */
	for (auto space_id : space_ids) {

		fil_flush(space_id);
	}
}

/** Validate a tablespace.
@param[in]	space	tablespace to validate
@return		number of open file nodes */
static
ulint
fil_validate_space(const fil_space_t* space)
{
	ut_ad(mutex_own(&fil_system->mutex));

	ulint	size = 0;
	ulint	n_open = 0;

	for (const auto& file : space->files) {

		ut_a(file.is_open || !file.n_pending);

		if (file.is_open) {
			++n_open;
		}

		size += file.size;
	}

	ut_a(space->size == size);

	return(n_open);
}

/** Checks the consistency of the tablespace cache.
@return true if ok */
bool
fil_validate()
{
	ulint	n_open = 0;

	mutex_enter(&fil_system->mutex);

	/* Look for spaces in the hash table */

	for (auto& elem : fil_system->spaces) {

		n_open += fil_validate_space(elem.second);
	}

	ut_a(fil_system->n_open == n_open);

	UT_LIST_CHECK(fil_system->LRU);

	for (auto file  = UT_LIST_GET_FIRST(fil_system->LRU);
	     file != nullptr;
	     file = UT_LIST_GET_NEXT(LRU, file)) {

		ut_a(file->is_open);
		ut_a(file->n_pending == 0);
		ut_a(fil_space_belongs_in_lru(file->space));
	}

	mutex_exit(&fil_system->mutex);

	return(true);
}

/** Returns true if file address is undefined.
@param[in]	addr		Address
@return true if undefined */
bool
fil_addr_is_null(fil_addr_t addr)
{
	return(addr.page == FIL_NULL);
}

/** Get the predecessor of a file page.
@param[in]	page		File page
@return FIL_PAGE_PREV */
page_no_t
fil_page_get_prev(const byte* page)
{
	return(mach_read_from_4(page + FIL_PAGE_PREV));
}

/** Get the successor of a file page.
@param[in]	page		File page
@return FIL_PAGE_NEXT */
page_no_t
fil_page_get_next(const byte* page)
{
	return(mach_read_from_4(page + FIL_PAGE_NEXT));
}

/** Sets the file page type.
@param[in,out]	page		File page
@param[in]	type		Page type */
void
fil_page_set_type(byte* page, ulint type)
{
	mach_write_to_2(page + FIL_PAGE_TYPE, type);
}

/** Reset the page type.
Data files created before MySQL 5.1 may contain garbage in FIL_PAGE_TYPE.
In MySQL 3.23.53, only undo log pages and index pages were tagged.
Any other pages were written with uninitialized bytes in FIL_PAGE_TYPE.
@param[in]	page_id	page number
@param[in,out]	page	page with invalid FIL_PAGE_TYPE
@param[in]	type	expected page type
@param[in,out]	mtr	mini-transaction */
void
fil_page_reset_type(
	const page_id_t&	page_id,
	byte*			page,
	ulint			type,
	mtr_t*			mtr)
{
	ib::info()
		<< "Resetting invalid page " << page_id << " type "
		<< fil_page_get_type(page) << " to " << type << ".";
	mlog_write_ulint(page + FIL_PAGE_TYPE, type, MLOG_2BYTES, mtr);
}

/** Closes the tablespace memory cache. */
void
fil_close()
{
	if (fil_system == nullptr) {
		return;
	}

	call_destructor(&fil_system->names);

	call_destructor(&fil_system->spaces);

	ut_a(UT_LIST_GET_LEN(fil_system->LRU) == 0);
	ut_a(UT_LIST_GET_LEN(fil_system->unflushed_spaces) == 0);

	mutex_free(&fil_system->mutex);

	ut_free(fil_system);
	fil_system = nullptr;
}

/** Initializes a buffer control block when the buf_pool is created.
@param[in]	block		Pointer to the control block
@param[in]	frame		Pointer to buffer frame */
static
void
fil_buf_block_init(
	buf_block_t*	block,
	byte*		frame)
{
	UNIV_MEM_DESC(frame, UNIV_PAGE_SIZE);

	block->frame = frame;

	block->page.io_fix = BUF_IO_NONE;
	/* There are assertions that check for this. */
	block->page.buf_fix_count = 1;
	block->page.state = BUF_BLOCK_READY_FOR_USE;

	page_zip_des_init(&block->page.zip);
}

struct fil_iterator_t {
	/** File handle */
	pfs_os_file_t		file;

	/** File path name */
	const char*		filepath;

	/** From where to start */
	os_offset_t		start;

	/** Where to stop */
	os_offset_t		end;

	/* File size in bytes */
	os_offset_t		file_size;

	/** Page size */
	ulint			page_size;

	/** Number of pages to use for I/O */
	ulint			n_io_buffers;

	/** Buffer to use for IO */
	byte*			io_buffer;

	/** Encryption key */
	byte*			encryption_key;

	/** Encruption iv */
	byte*			encryption_iv;
};

/** TODO: This can be made parallel trivially by chunking up the file
and creating a callback per thread. Main benefit will be to use multiple
CPUs for checksums and compressed tables. We have to do compressed tables
block by block right now. Secondly we need to decompress/compress and copy
too much of data. These are CPU intensive.

Iterate over all the pages in the tablespace.
@param[in]	 iter		Tablespace iterator
@param[in,out]	 block		Block to use for IO
@param[in]	 callback	Callback to inspect and update page contents
@retval DB_SUCCESS or error code */
static
dberr_t
fil_iterate(
	const fil_iterator_t&	iter,
	buf_block_t*		block,
	PageCallback&		callback)
{
	os_offset_t		offset;
	page_no_t		page_no = 0;
	space_id_t		space_id = callback.get_space_id();
	ulint			n_bytes = iter.n_io_buffers * iter.page_size;

	ut_ad(!srv_read_only_mode);

	/* For old style compressed tables we do a lot of useless copying
	for non-index pages. Unfortunately, it is required by
	buf_zip_decompress() */

	ulint	read_type = IORequest::READ;
	ulint	write_type = IORequest::WRITE;

	for (offset = iter.start; offset < iter.end; offset += n_bytes) {

		byte*	io_buffer = iter.io_buffer;

		block->frame = io_buffer;

		if (callback.get_page_size().is_compressed()) {
			page_zip_des_init(&block->page.zip);
			page_zip_set_size(&block->page.zip, iter.page_size);

			block->page.size.copy_from(
				page_size_t(iter.page_size,
					    univ_page_size.logical(),
					    true));

			block->page.zip.data = block->frame + UNIV_PAGE_SIZE;
			ut_d(block->page.zip.m_external = true);
			ut_ad(iter.page_size
			      == callback.get_page_size().physical());

			/* Zip IO is done in the compressed page buffer. */
			io_buffer = block->page.zip.data;
		} else {
			io_buffer = iter.io_buffer;
		}

		/* We have to read the exact number of bytes. Otherwise the
		InnoDB IO functions croak on failed reads. */

		n_bytes = static_cast<ulint>(
			ut_min(static_cast<os_offset_t>(n_bytes),
			       iter.end - offset));

		ut_ad(n_bytes > 0);
		ut_ad(!(n_bytes % iter.page_size));

		dberr_t		err;
		IORequest	read_request(read_type);

		/* For encrypted table, set encryption information. */
		if (iter.encryption_key != nullptr && offset != 0) {
			read_request.encryption_key(iter.encryption_key,
						    ENCRYPTION_KEY_LEN,
						    iter.encryption_iv);
			read_request.encryption_algorithm(Encryption::AES);
		}

		err = os_file_read(
			read_request, iter.file, io_buffer, offset,
			(ulint) n_bytes);

		if (err != DB_SUCCESS) {

			ib::error() << "os_file_read() failed";

			return(err);
		}

		bool		updated = false;
		os_offset_t	page_off = offset;
		ulint		n_pages_read = (ulint) n_bytes / iter.page_size;

		for (ulint i = 0; i < n_pages_read; ++i) {

			buf_block_set_file_page(
				block, page_id_t(space_id, page_no++));

			if ((err = callback(page_off, block)) != DB_SUCCESS) {

				return(err);

			} else if (!updated) {
				updated = buf_block_get_state(block)
					== BUF_BLOCK_FILE_PAGE;
			}

			buf_block_set_state(block, BUF_BLOCK_NOT_USED);
			buf_block_set_state(block, BUF_BLOCK_READY_FOR_USE);

			page_off += iter.page_size;
			block->frame += iter.page_size;
		}

		IORequest	write_request(write_type);

		/* For encrypted table, set encryption information. */
		if (iter.encryption_key != nullptr && offset != 0) {
			write_request.encryption_key(iter.encryption_key,
						     ENCRYPTION_KEY_LEN,
						     iter.encryption_iv);
			write_request.encryption_algorithm(Encryption::AES);
		}

		/* A page was updated in the set, write back to disk.
		Note: We don't have the compression algorithm, we write
		out the imported file as uncompressed. */

		if (updated
		    && (err = os_file_write(
				write_request,
				iter.filepath, iter.file, io_buffer,
				offset, (ulint) n_bytes)) != DB_SUCCESS) {

			/* This is not a hard error */
			if (err == DB_IO_NO_PUNCH_HOLE) {

				err = DB_SUCCESS;
				write_type &= ~IORequest::PUNCH_HOLE;

			} else {
				ib::error() << "os_file_write() failed";

				return(err);
			}
		}
	}

	return(DB_SUCCESS);
}

/** Iterate over all the pages in the tablespace.
@param[in,out]	 table		the table definiton in the server
@param[in]	 n_io_buffers	number of blocks to read and write together
@param[in]	 callback	functor that will do the page updates
@return DB_SUCCESS or error code */
dberr_t
fil_tablespace_iterate(
	dict_table_t*	table,
	ulint		n_io_buffers,
	PageCallback&	callback)
{
	dberr_t		err;
	pfs_os_file_t	file;
	char*		filepath;
	bool		success;

	ut_a(n_io_buffers > 0);
	ut_ad(!srv_read_only_mode);

	DBUG_EXECUTE_IF("ib_import_trigger_corruption_1",
			return(DB_CORRUPTION););

	/* Make sure the data_dir_path is set. */
	dict_get_and_save_data_dir_path(table, false);

	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		ut_a(table->data_dir_path);

		filepath = fil_make_filepath(
			table->data_dir_path, table->name.m_name, IBD, true);
	} else {
		filepath = fil_make_filepath(
			nullptr, table->name.m_name, IBD, false);
	}

	if (filepath == nullptr) {
		return(DB_OUT_OF_MEMORY);
	}

	file = os_file_create_simple_no_error_handling(
		innodb_data_file_key, filepath,
		OS_FILE_OPEN, OS_FILE_READ_WRITE, srv_read_only_mode, &success);

	DBUG_EXECUTE_IF("fil_tablespace_iterate_failure",
	{
		static bool once;

		if (!once || ut_rnd_interval(0, 10) == 5) {
			once = true;
			success = false;
			os_file_close(file);
		}
	});

	if (!success) {
		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib::error() << "Trying to import a tablespace, but could not"
			" open the tablespace file " << filepath;

		ut_free(filepath);

		return(DB_TABLESPACE_NOT_FOUND);

	} else {
		err = DB_SUCCESS;
	}

	callback.set_file(filepath, file);

	os_offset_t	file_size = os_file_get_size(file);
	ut_a(file_size != (os_offset_t) -1);

	/* The block we will use for every physical page */
	buf_block_t*	block;

	block = reinterpret_cast<buf_block_t*>(ut_zalloc_nokey(sizeof(*block)));

	mutex_create(LATCH_ID_BUF_BLOCK_MUTEX, &block->mutex);

	/* Allocate a page to read in the tablespace header, so that we
	can determine the page size and zip size (if it is compressed).
	We allocate an extra page in case it is a compressed table. One
	page is to ensure alignement. */

	void*	page_ptr = ut_malloc_nokey(3 * UNIV_PAGE_SIZE);
	byte*	page = static_cast<byte*>(ut_align(page_ptr, UNIV_PAGE_SIZE));

	fil_buf_block_init(block, page);

	/* Read the first page and determine the page and zip size. */

	IORequest	request(IORequest::READ);

	err = os_file_read(request, file, page, 0, UNIV_PAGE_SIZE);

	if (err != DB_SUCCESS) {

		err = DB_IO_ERROR;

	} else if ((err = callback.init(file_size, block)) == DB_SUCCESS) {
		fil_iterator_t	iter;

		iter.file = file;
		iter.start = 0;
		iter.end = file_size;
		iter.filepath = filepath;
		iter.file_size = file_size;
		iter.n_io_buffers = n_io_buffers;
		iter.page_size = callback.get_page_size().physical();

		/* Set encryption info. */
		iter.encryption_key = table->encryption_key;
		iter.encryption_iv = table->encryption_iv;

		/* Check encryption is matched or not. */
		ulint	space_flags = callback.get_space_flags();
		if (FSP_FLAGS_GET_ENCRYPTION(space_flags)) {
			ut_ad(table->encryption_key != nullptr);

			if (!dict_table_is_encrypted(table)) {
				ib::error() << "Table is not in an encrypted"
					" tablespace, but the data file which"
					" trying to import is an encrypted"
					" tablespace";
				err = DB_IO_NO_ENCRYPT_TABLESPACE;
			}
		}

		if (err == DB_SUCCESS) {

			/* Compressed pages can't be optimised for block IO
			for now.  We do the IMPORT page by page. */

			if (callback.get_page_size().is_compressed()) {
				iter.n_io_buffers = 1;
				ut_a(iter.page_size
				     == callback.get_page_size().physical());
			}

			/** Add an extra page for compressed page scratch
			area. */
			void*	io_buffer = ut_malloc_nokey(
				(2 + iter.n_io_buffers) * UNIV_PAGE_SIZE);

			iter.io_buffer = static_cast<byte*>(
				ut_align(io_buffer, UNIV_PAGE_SIZE));

			err = fil_iterate(iter, block, callback);

			ut_free(io_buffer);
		}
	}

	if (err == DB_SUCCESS) {

		ib::info() << "Sync to disk";

		if (!os_file_flush(file)) {
			ib::info() << "os_file_flush() failed!";
			err = DB_IO_ERROR;
		} else {
			ib::info() << "Sync to disk - done!";
		}
	}

	os_file_close(file);

	ut_free(page_ptr);
	ut_free(filepath);

	mutex_free(&block->mutex);

	ut_free(block);

	return(err);
}

/** Set the tablespace table size.
@param[in]	page	a page belonging to the tablespace */
void
PageCallback::set_page_size(
	const buf_frame_t*	page) UNIV_NOTHROW
{
	m_page_size.copy_from(fsp_header_get_page_size(page));
}

/** Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables.
@param[in] ibd_filepath File path of the IBD tablespace */
void
fil_delete_file(const char* ibd_filepath)
{
	/* Force a delete of any stale .ibd files that are lying around. */

	ib::info() << "Deleting " << ibd_filepath;

	os_file_delete_if_exists(innodb_data_file_key, ibd_filepath, nullptr);

	char*	cfg_filepath = fil_make_filepath(
		ibd_filepath, nullptr, CFG, false);
	if (cfg_filepath != nullptr) {
		os_file_delete_if_exists(
			innodb_data_file_key, cfg_filepath, nullptr);
		ut_free(cfg_filepath);
	}

	char*	cfp_filepath = fil_make_filepath(
		ibd_filepath, nullptr, CFP, false);
	if (cfp_filepath != nullptr) {
		os_file_delete_if_exists(
			innodb_data_file_key, cfp_filepath, nullptr);
		ut_free(cfp_filepath);
	}
}

/** Iterate over all the spaces in the space list and fetch the
tablespace names. It will return a copy of the name that must be
freed by the caller using: delete[].
@param[in,out]	space_name_list	List to append to
@return DB_SUCCESS if all OK. */
dberr_t
fil_get_space_names(space_name_list_t& space_name_list)
{
	dberr_t		err = DB_SUCCESS;

	mutex_enter(&fil_system->mutex);

	for (auto& elem : fil_system->spaces) {

		auto	space = elem.second;

		if (space->purpose == FIL_TYPE_TABLESPACE) {
			ulint	len;
			char*	name;

			len = ::strlen(space->name);
			name = UT_NEW_ARRAY_NOKEY(char, len + 1);

			if (name == 0) {
				/* Caller to free elements allocated so far. */
				err = DB_OUT_OF_MEMORY;
				break;
			}

			memcpy(name, space->name, len);
			name[len] = 0;

			space_name_list.push_back(name);
		}
	}

	mutex_exit(&fil_system->mutex);

	return(err);
}

/** Generate redo log for swapping two .ibd files
@param[in]	old_table	old table
@param[in]	new_table	new table
@param[in]	tmp_name	temporary table name
@param[in,out]	mtr		mini-transaction
@return innodb error code */
dberr_t
fil_mtr_rename_log(
	const dict_table_t*	old_table,
	const dict_table_t*	new_table,
	const char*		tmp_name,
	mtr_t*			mtr)
{
	dberr_t	err;

	bool	old_is_file_per_table =
		dict_table_is_file_per_table(old_table);

	bool	new_is_file_per_table =
		dict_table_is_file_per_table(new_table);

	/* If neither table is file-per-table,
	there will be no renaming of files. */
	if (!old_is_file_per_table && !new_is_file_per_table) {
		return(DB_SUCCESS);
	}

	const char*	old_dir = DICT_TF_HAS_DATA_DIR(old_table->flags)
		? old_table->data_dir_path
		: nullptr;

	char*	old_path = fil_make_filepath(
		old_dir, old_table->name.m_name, IBD, (old_dir != nullptr));

	if (old_path == nullptr) {
		return(DB_OUT_OF_MEMORY);
	}

	if (old_is_file_per_table) {

		char*	tmp_path = fil_make_filepath(
			old_dir, tmp_name, IBD, (old_dir != nullptr));

		if (tmp_path == nullptr) {
			ut_free(old_path);
			return(DB_OUT_OF_MEMORY);
		}

		/* Temp filepath must not exist. */
		err = fil_rename_tablespace_check(
			old_table->space, old_path, tmp_path,
			dict_table_is_discarded(old_table));

		if (err != DB_SUCCESS) {
			ut_free(old_path);
			ut_free(tmp_path);
			return(err);
		}

		fil_name_write_rename(
			old_table->space, old_path, tmp_path, mtr);

		ut_free(tmp_path);
	}

	if (new_is_file_per_table) {

		const char*	new_dir = DICT_TF_HAS_DATA_DIR(new_table->flags)
			? new_table->data_dir_path
			: nullptr;

		char*	new_path = fil_make_filepath(
				new_dir, new_table->name.m_name,
				IBD, (new_dir != nullptr));

		if (new_path == nullptr) {
			ut_free(old_path);
			return(DB_OUT_OF_MEMORY);
		}

		/* Destination filepath must not exist unless this ALTER
		TABLE starts and ends with a file_per-table tablespace. */
		if (!old_is_file_per_table) {

			err = fil_rename_tablespace_check(
				new_table->space, new_path, old_path,
				dict_table_is_discarded(new_table));

			if (err != DB_SUCCESS) {
				ut_free(old_path);
				ut_free(new_path);
				return(err);
			}
		}

		fil_name_write_rename(
			new_table->space, new_path, old_path, mtr);

		ut_free(new_path);
	}

	ut_free(old_path);

	return(DB_SUCCESS);
}

/** Get the space IDs active in the system.
@param[out]	space_ids	All the registered tablespace IDs */
void
fil_space_ids_get(Space_ids* space_ids)
{
	ut_a(space_ids->empty());

	mutex_enter(&fil_system->mutex);

	for (const auto& elem : fil_system->spaces) {
		space_ids->push_back(elem.first);
	}

	mutex_exit(&fil_system->mutex);
}

/** Get the filenames for a tablespace ID and increment pending ops.
@param[in]	space_id	Tablespace ID
@param[out]	files		Files for a tablespace ID */
void
fil_node_fetch(
	space_id_t		space_id,
	fil_space_t::Files*	files)
{
	ut_a(files->empty());

	mutex_enter(&fil_system->mutex);

	auto	space = fil_space_get(space_id);

	/* Skip spaces that are being dropped. */
	if (space == nullptr || space->stop_new_ops) {
		return;
	}

	++space->n_pending_ops;

	for (const auto& file : space->files) {
		files->push_back(file);
	}

	mutex_exit(&fil_system->mutex);
}

/** Releases the tablespace instance by decrementing pending ops.
@param[in]	space_id	Tablespace ID to release. */
void
fil_node_release(space_id_t space_id)
{
	mutex_enter(&fil_system->mutex);

	auto	space = fil_space_get(space_id);

	if (space != nullptr) {
		ut_a(!space->stop_new_ops);

		ut_a(space->n_pending_ops > 0);

		--space->n_pending_ops;
	}

	mutex_exit(&fil_system->mutex);
}

/** Note that the file system where the file resides doesn't support PUNCH HOLE.
Called from AIO handlers when IO returns DB_IO_NO_PUNCH_HOLE
@param[in,out]	file		file to set */
void
fil_no_punch_hole(fil_node_t* file)
{
	file->punch_hole = false;
}

/** Set the compression type for the tablespace of a table
@param[in]	table		The table that should be compressed
@param[in]	algorithm	Text representation of the algorithm
@return DB_SUCCESS or error code */
dberr_t
fil_set_compression(
	dict_table_t*	table,
	const char*	algorithm)
{
	ut_ad(table != nullptr);

	/* We don't support Page Compression for the system tablespace,
	the temporary tablespace, or any general tablespace because
	COMPRESSION is set by TABLE DDL, not TABLESPACE DDL. There is
	no other technical reason.  Also, do not use it for missing
	tables or tables with compressed row_format. */
	if (table->ibd_file_missing
	    || !DICT_TF2_FLAG_IS_SET(table, DICT_TF2_USE_FILE_PER_TABLE)
	    || DICT_TF2_FLAG_IS_SET(table, DICT_TF2_TEMPORARY)
	    || page_size_t(table->flags).is_compressed()) {

		return(DB_IO_NO_PUNCH_HOLE_TABLESPACE);
	}

	dberr_t		err;
	Compression	compression;

	if (algorithm == nullptr || strlen(algorithm) == 0) {

#ifndef UNIV_DEBUG
		compression.m_type = Compression::NONE;
#else
		/* This is a Debug tool for setting compression on all
		compressible tables not otherwise specified. */
		switch (srv_debug_compress) {
		case Compression::LZ4:
		case Compression::ZLIB:
		case Compression::NONE:

			compression.m_type =
				static_cast<Compression::Type>(
					srv_debug_compress);
			break;

		default:
			compression.m_type = Compression::NONE;
		}

#endif /* UNIV_DEBUG */

		err = DB_SUCCESS;

	} else {

		err = Compression::check(algorithm, &compression);
	}

	fil_space_t*	space = fil_space_get(table->space);

	if (space == nullptr) {
		return(DB_NOT_FOUND);
	}

	space->compression_type = compression.m_type;

	if (space->compression_type != Compression::NONE) {

		if (!space->files.front().punch_hole) {

			return(DB_IO_NO_PUNCH_HOLE_FS);
		}
	}

	return(err);
}

/** Get the compression algorithm for a tablespace.
@param[in]	space_id	Space ID to check
@return the compression algorithm */
Compression::Type
fil_get_compression(space_id_t space_id)
{
	fil_space_t*	space = fil_space_get(space_id);

	return(space == nullptr ? Compression::NONE : space->compression_type);
}

/** Set the encryption type for the tablespace
@param[in] space_id		Space ID of tablespace for which to set
@param[in] algorithm		Encryption algorithm
@param[in] key			Encryption key
@param[in] iv			Encryption iv
@return DB_SUCCESS or error code */
dberr_t
fil_set_encryption(
	space_id_t		space_id,
	Encryption::Type	algorithm,
	byte*			key,
	byte*			iv)
{
	ut_ad(space_id != TRX_SYS_SPACE);

	if (fsp_is_system_or_temp_tablespace(space_id)) {
		return(DB_IO_NO_ENCRYPT_TABLESPACE);
	}

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space == nullptr) {
		mutex_exit(&fil_system->mutex);
		return(DB_NOT_FOUND);
	}

	if (key == nullptr) {
		Encryption::random_value(space->encryption_key);
	} else {
		memcpy(space->encryption_key,
		       key, ENCRYPTION_KEY_LEN);
	}

	space->encryption_klen = ENCRYPTION_KEY_LEN;
	if (iv == nullptr) {
		Encryption::random_value(space->encryption_iv);
	} else {
		memcpy(space->encryption_iv,
		       iv, ENCRYPTION_KEY_LEN);
	}

	ut_ad(algorithm != Encryption::NONE);
	space->encryption_type = algorithm;

	mutex_exit(&fil_system->mutex);

	return(DB_SUCCESS);
}

/** Rotate the tablespace keys by new master key.
@return true if the re-encrypt suceeds */
bool
fil_encryption_rotate()
{
	byte	encrypt_info[ENCRYPTION_INFO_SIZE_V2];

	for (auto& elem : fil_system->spaces) {

		auto	space = elem.second;

		/* Skip unencypted tablespaces. Encrypted redo log
		tablespaces is handled in function log_rotate_encryption. */

		if (fsp_is_system_or_temp_tablespace(space->id)
		    || space->purpose == FIL_TYPE_LOG) {

			continue;
		}

		/* Skip the undo tablespace when it's in default key status,
		since it's the first server startup after bootstrap, and the
		server uuid is not ready yet. */

		if (fsp_is_undo_tablespace(space->id)
		    && Encryption::master_key_id
		    == ENCRYPTION_DEFAULT_MASTER_KEY_ID) {

			continue;
		}

		/* Rotate the encrypted tablespaces. */
		if (space->encryption_type != Encryption::NONE) {

			mtr_t	mtr;

			mtr_start(&mtr);

			mtr_x_lock_space(space, &mtr);

			memset(encrypt_info, 0, ENCRYPTION_INFO_SIZE_V2);

			if (!fsp_header_rotate_encryption(
				space, encrypt_info, &mtr)) {

				mtr_commit(&mtr);

				return(false);
			}

			mtr_commit(&mtr);
		}

		DBUG_EXECUTE_IF("ib_crash_during_rotation_for_encryption",
				DBUG_SUICIDE(););
	}

	return(true);
}

/** Build the basic folder name from the path and length provided
@param[in]	path	pathname (may also include the file basename)
@param[in]	len	length of the path, in bytes */
void
Folder::make_path(const char* path, size_t len)
{
	if (is_absolute_path(path)) {
		m_folder = mem_strdupl(path, len);
		m_folder_len = len;
	} else {
		size_t n = 2 + len + strlen(fil_path_to_mysql_datadir);
		m_folder = static_cast<char*>(ut_malloc_nokey(n));
		m_folder_len = 0;

		if (path != fil_path_to_mysql_datadir) {
			/* Put the mysqld datadir into m_folder first. */
			ut_ad(fil_path_to_mysql_datadir[0] != '\0');
			m_folder_len = strlen(fil_path_to_mysql_datadir);
			memcpy(m_folder, fil_path_to_mysql_datadir,
			       m_folder_len);
			if (m_folder[m_folder_len - 1] != OS_PATH_SEPARATOR) {
				m_folder[m_folder_len++] = OS_PATH_SEPARATOR;
			}
		}

		/* Append the path. */
		memcpy(m_folder + m_folder_len, path, len);
		m_folder_len += len;
		m_folder[m_folder_len] = '\0';
	}

	os_normalize_path(m_folder);
}

/** Resolve a relative path in m_folder to an absolute path
in m_abs_path setting m_abs_len. */
void
Folder::make_abs_path()
{
	my_realpath(m_abs_path, m_folder, MYF(0));
	m_abs_len = strlen(m_abs_path);

	ut_ad(m_abs_len + 1 < sizeof(m_abs_path));

	/* Folder::related_to() needs a trailing separator. */
	if (m_abs_path[m_abs_len - 1] != OS_PATH_SEPARATOR) {
		m_abs_path[m_abs_len] = OS_PATH_SEPARATOR;
		m_abs_path[++m_abs_len] = '\0';
	}
}

/** Constructor
@param[in]	path	pathname (may also include the file basename)
@param[in]	len	length of the path, in bytes */
Folder::Folder(const char* path, size_t len)
{
	make_path(path, len);
	make_abs_path();
}

/** Assignment operator
@param[in]	path	folder string provided */
class Folder&
Folder::operator=(const char* path)
{
	ut_free(m_folder);
	make_path(path, strlen(path));
	make_abs_path();

	return(*this);
}

/** Determine if the directory referenced by m_folder exists.
@return whether the directory exists */
bool
Folder::exists()
{
	bool		exists;
	os_file_type_t	type;

#ifdef _WIN32
	/* Temporarily strip the trailing_separator since it will cause
	stat64() to fail on Windows unless the path is the root of some
	drive; like "c:\".  _stat64() will fail if it is "c:". */
	size_t  len = strlen(m_abs_path);

	if (m_abs_path[m_abs_len - 1] == OS_PATH_SEPARATOR
	    && m_abs_path[m_abs_len - 2] != ':') {

		m_abs_path[m_abs_len - 1] = '\0';
	}
#endif /* WIN32 */

	bool ret = os_file_status(m_abs_path, &exists, &type);

#ifdef _WIN32
	/* Put the separator back on. */
	if (m_abs_path[m_abs_len - 1] == '\0') {
		m_abs_path[m_abs_len - 1] = OS_PATH_SEPARATOR;
	}
#endif /* WIN32 */

	return(ret && exists && type == OS_FILE_TYPE_DIR);
}

/** Sets the flags of the tablespace. The tablespace must be locked
in MDL_EXCLUSIVE MODE.
@param[in]	space	tablespace in-memory struct
@param[in]	flags	tablespace flags */
void
fil_space_set_flags(
	fil_space_t*	space,
	ulint		flags)
{
	ut_ad(fsp_flags_is_valid(flags));

	rw_lock_x_lock(&space->latch);
	space->flags = flags;
	rw_lock_x_unlock(&space->latch);
}

/* Unit Tests */
#ifdef UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH
#define MF  fil_make_filepath
#define DISPLAY ib::info() << path
void
test_make_filepath()
{
	char* path;
	const char* long_path =
		"this/is/a/very/long/path/including/a/very/"
		"looooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooong"
		"/folder/name";
	path = MF("/this/is/a/path/with/a/filename", nullptr, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", nullptr, ISL, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", nullptr, CFG, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", nullptr, CFP, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.ibd", nullptr, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.ibd", nullptr, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.dat", nullptr, IBD, false); DISPLAY;
	path = MF(nullptr, "tablespacename", NO_EXT, false); DISPLAY;
	path = MF(nullptr, "tablespacename", IBD, false); DISPLAY;
	path = MF(nullptr, "dbname/tablespacename", NO_EXT, false); DISPLAY;
	path = MF(nullptr, "dbname/tablespacename", IBD, false); DISPLAY;
	path = MF(nullptr, "dbname/tablespacename", ISL, false); DISPLAY;
	path = MF(nullptr, "dbname/tablespacename", CFG, false); DISPLAY;
	path = MF(nullptr, "dbname/tablespacename", CFP, false); DISPLAY;
	path = MF(nullptr, "dbname\\tablespacename", NO_EXT, false); DISPLAY;
	path = MF(nullptr, "dbname\\tablespacename", IBD, false); DISPLAY;
	path = MF("/this/is/a/path", "dbname/tablespacename", IBD, false); DISPLAY;
	path = MF("/this/is/a/path", "dbname/tablespacename", IBD, true); DISPLAY;
	path = MF("./this/is/a/path", "dbname/tablespacename.ibd", IBD, true); DISPLAY;
	path = MF("this\\is\\a\\path", "dbname/tablespacename", IBD, true); DISPLAY;
	path = MF("/this/is/a/path", "dbname\\tablespacename", IBD, true); DISPLAY;
	path = MF(long_path, nullptr, IBD, false); DISPLAY;
	path = MF(long_path, "tablespacename", IBD, false); DISPLAY;
	path = MF(long_path, "tablespacename", IBD, true); DISPLAY;
}
#endif /* UNIV_ENABLE_UNIT_TEST_MAKE_FILEPATH */
/* @} */

/** Release the reserved free extents.
@param[in]	n_reserved	number of reserved extents */
void
fil_space_t::release_free_extents(ulint	n_reserved)
{
	ut_ad(rw_lock_own(&latch, RW_LOCK_X));

	ut_a(n_reserved_extents >= n_reserved);
	n_reserved_extents -= n_reserved;
}

#ifdef UNIV_DEBUG
/** Print the extent descriptor pages of this tablespace into
the given file.
@param[in]	filename	the output file name. */
void
fil_space_t::print_xdes_pages(const char* filename) const
{
	std::ofstream	out(filename);
	print_xdes_pages(out);
}

/** Print the extent descriptor pages of this tablespace into
the given file.
@param[in]	out	the output file name.
@return	the output stream. */
std::ostream&
fil_space_t::print_xdes_pages(std::ostream& out) const
{
	mtr_t			mtr;
	const page_size_t	page_size(flags);

	mtr_start(&mtr);

	for (page_no_t i = 0; i < 100; ++i) {
		page_no_t xdes_page_no = i * UNIV_PAGE_SIZE;

		if (xdes_page_no >= size) {
			break;
		}

		buf_block_t*	xdes_block = buf_page_get(
			page_id_t(id, xdes_page_no), page_size,
			RW_S_LATCH, &mtr);

		page_t*	page = buf_block_get_frame(xdes_block);

		ulint page_type = fil_page_get_type(page);

		switch (page_type) {
		case FIL_PAGE_TYPE_ALLOCATED:
			ut_ad(xdes_page_no >= free_limit);
			goto finish;
		case FIL_PAGE_TYPE_FSP_HDR:
		case FIL_PAGE_TYPE_XDES:
			break;
		default:
			ut_error;
		}

		xdes_page_print(out, page, xdes_page_no, &mtr);
	}
finish:
	mtr_commit(&mtr);
	return(out);
}
#endif /* UNIV_DEBUG */

/** Initialize the table space encryption
@param[in,out]	space		Tablespace instance */
static
void
fil_tablespace_encryption_init(fil_space_t* space)
{
	for (auto& key : *recv_sys->keys) {

		if (key.space_id == space->id) {

			dberr_t	err;

			err = fil_set_encryption(
				space->id, Encryption::AES, key.ptr, key.iv);

			if (err != DB_SUCCESS) {

				ib::error()
					<< "Can't set encryption information"
					<< " for tablespace" << space->name
					<< "!";
			}

			ut_free(key.iv);
			ut_free(key.ptr);

			key.iv = nullptr;
			key.ptr = nullptr;

			key.space_id = std::numeric_limits<space_id_t>::max();
		}
	}
}

/** Open a tablespace that has a redo log record to apply.
@param[in]	space_id		Space id
@return true if the open was successful */
bool
fil_tablespace_open_for_recovery(space_id_t space_id)
{
	ut_ad(recv_recovery_is_on());

	/* There must be a mapping from the space ID to a file. */
	const auto	names = tablespace_files->find(space_id);

	/* Duplicates should have been sorted out before start of recovery. */
	ut_a(names->size() == 1);

	const auto&	path = names->front();

	fil_space_t*	space;

	auto	status = fil_ibd_open_for_recovery(space_id, path, space);

	if (status == FIL_LOAD_OK) {

		/* For encrypted tablespace, set key and iv. */
		if (FSP_FLAGS_GET_ENCRYPTION(space->flags)
		    && recv_sys->keys!= nullptr) {

			fil_tablespace_encryption_init(space);
		}

		if (!recv_sys->dblwr.deferred.empty()) {
			buf_dblwr_recover_pages(space);
		}

		return(true);
	}

	return(false);
}

/** Lookup the tablespace ID.
@param[in]	space_id		Tablespace ID to lookup
@return true if the space ID is known. */
bool
fil_tablespace_lookup_for_recovery(space_id_t space_id)
{
	ut_ad(recv_recovery_is_on());

	/* Single threaded code, no need to acquire mutex. */
	const auto	names = tablespace_files->find(space_id);

	if (names == nullptr) {
		return(false);
	}

	const auto&	end = recv_sys->deleted.end();

	return(recv_sys->deleted.find(space_id) == end);
}

/** This function should be called after recovery has completed.
Check for tablespace files for which we did not see any MLOG_FILE_DELETE
or MLOG_FILE_RENAME record. These could not be recovered
@return true if there were some filenames missing for which we had to
ignore redo log records during the apply phase */
bool
fil_check_missing_tablespaces()
{
	bool		missing = false;
	auto&		dblwr	= recv_sys->dblwr;
	const auto	end = recv_sys->deleted.end();

	/* Called in single threaded mode, no need to acquire the mutex. */

	/* First check if we were able to restore all the doublewrite
	buffer pages. If not then print a warning. */

	for (auto& page : dblwr.deferred) {

		space_id_t	space_id;

		space_id = page_get_space_id(page.m_page);

		/* If the tablespace was in the missing IDs then we
		know that the problem is elsewhere. If a file deleted
		record was not found in the redo log and the tablespace
		doesn't exist in the SYS_TABLESPACES file then it is
		an error or data corruption. */

		if (recv_sys->deleted.find(space_id) == end
		    && recv_sys->missing_ids.find(space_id)
		    != recv_sys->missing_ids.end()
		    && dict_space_is_empty(space_id)) {

			page_no_t	page_no;

			page_no = page_get_page_no(page.m_page);

			ib::warn()
				<< "Doublewrite page " << page.m_no
				<< " for {space: " << space_id << ", page_no:"
				<< page_no << "} could not be restored."
				<< " File name unknown for tablespace ID "
				<< space_id;
		}

		/* Free the memory. */
		page.close();
	}

	dblwr.deferred.clear();

	for (auto space_id : recv_sys->missing_ids) {

		if (recv_sys->deleted.find(space_id) != end) {

			continue;
		}

		const auto	names = tablespace_files->find(space_id);

		if (names == nullptr) {

			ib::error()
				<< "Could not find any file associated with"
				<< " the tablespace ID: " << space_id;

			missing = true;
		} else {
			ut_a(!names->empty());
		}
	}

	return(missing);
}

/** Redo a tablespace create.
@param[in]	ptr		redo log record
@param[in]	end		end of the redo log buffer
@param[in]	page_id		Tablespace Id and first page in file
@param[in]	parsed_bytes	Number of bytes parsed so far
@return pointer to next redo log record
@retval nullptr if this log record was truncated */
byte*
fil_tablespace_redo_create(
	byte*			ptr,
	const byte*		end,
	const page_id_t&	page_id,
	ulint			parsed_bytes)
{
	ut_a(page_id.page_no() == 0);

	/* We never recreate the system tablespace. */
	ut_a(page_id.space() != TRX_SYS_SPACE);

	ut_a(parsed_bytes != ULINT_UNDEFINED);

	/* Where 6 = flags (uint32_t) + len (uint16_t). */
	if (end <= ptr + 6) {
		return(nullptr);
	}

	/* Skip the flags, not used here. */
	ulint	len = mach_read_from_2(ptr + 4);

	ptr += 6;

	/* Do we have the full/valid file name. */
	if (end < ptr + len || len < 5) {

		if (len < 5) {

			char	name[6];

			snprintf(name, sizeof(name), "%.*s", (int) len, ptr);

			ib::error()
				<< "MLOG_FILE_CREATE : Invalid file name."
				<< " Length (" << len << ") must be >= 5"
				<< " and end in '.ibd'. File name in the"
				<< " redo log is '" << name << "'";
		}

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	char*	name	= reinterpret_cast<char*>(ptr);

	os_normalize_path(name);

	ptr += len;

	if (memcmp(ptr - 5, DOT_IBD, 5) != 0) {

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	const auto	names = tablespace_files->find(page_id.space());

	if (names == nullptr) {

		/* No files map to this tablespace ID. It's possible that
		the files were deleted later or ar misisng. */

		return(ptr);
	}

	/* Duplicates should have been sorted out before we get here. */
	ut_a(names->size() == 1);

	/* It's possible that the tablespace file was renamed later. */

	if (names->front().compare(name) == 0) {
		bool	success;

		success = fil_tablespace_open_for_recovery(page_id.space());

		ut_a(success);
	}

	return(ptr);
}

/** Redo a tablespace rename
@param[in]	ptr		redo log record
@param[in]	end		end of the redo log buffer
@param[in]	page_id		Tablespace Id and first page in file
@param[in]	parsed_bytes	Number of bytes parsed so far
@return pointer to next redo log record
@retval nullptr if this log record was truncated */
byte*
fil_tablespace_redo_rename(
	byte*			ptr,
	const byte*		end,
	const page_id_t&	page_id,
	ulint			parsed_bytes)
{
	ut_a(page_id.page_no() == 0);

	/* We never recreate the system tablespace. */
	ut_a(page_id.space() != TRX_SYS_SPACE);

	ut_a(parsed_bytes != ULINT_UNDEFINED);

	/* Where 2 = from name len (uint16_t). */
	if (end <= ptr + 2) {
		return(nullptr);
	}

	/* Read and check the RENAME FROM_NAME. */
	ulint	from_len = mach_read_from_2(ptr);

	ptr += 2;

	/* Do we have the full/valid from and to file names. */
	if (end < ptr + from_len || from_len < 5) {

		if (from_len < 5) {

			char	name[6];

			snprintf(name, sizeof(name), "%.*s",
				 (int) from_len, ptr);

			ib::error()
				<< "MLOG_FILE_RENAME: Invalid from file name."
				<< " Length (" << from_len << ") must be >= 5"
				<< " and end in '.ibd'. File name in the"
				<< " redo log is '" << name << "'";
		}

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	char*	from_name = reinterpret_cast<char*>(ptr);

	os_normalize_path(from_name);

	ptr += from_len;

	if (memcmp(ptr - 5, DOT_IBD, 5) != 0) {

		ib::error()
			<< "MLOG_FILE_RENAME: From file name doesn't end in"
			<< " .ibd. File name in the redo log is '"
			<< from_name << "'";

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	/* Read and check the RENAME TO_NAME. */

	ulint	to_len = mach_read_from_2(ptr);

	ptr += 2;

	if (end < ptr + to_len || to_len < 5) {

		if (to_len < 5) {

			char	name[6];

			snprintf(name, sizeof(name), "%.*s",
				 (int) to_len, ptr);

			ib::error()
				<< "MLOG_FILE_RENAME: Invalid to file name."
				<< " Length (" << to_len << ") must be >= 5"
				<< " and end in '.ibd'. File name in the"
				<< " redo log is '" << name << "'";
		}

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	char*	to_name = reinterpret_cast<char*>(ptr);

	os_normalize_path(to_name);

	/* FIXME: Should we do an abs path compare? */
	if (from_len == to_len && strncmp(to_name, from_name, to_len) == 0) {

		ib::error()
			<< "MLOG_FILE_RENAME: The from and to name are the"
			<< " same: '" << from_name << "', '" << to_name << "'";

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	ptr += to_len;

	if (memcmp(ptr - 5, DOT_IBD, 5) != 0) {

		ib::error()
			<< "MLOG_FILE_RENAME: To file name doesn't end in"
			<< " .ibd. File name in the redo log is '"
			<< to_name << "'";

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	auto	names = tablespace_files->find(page_id.space());

	if (names == nullptr) {

		/* No file on disk maps to this tablespace ID. It's possible
		that the files were deleted later or are misisng. */

		return(ptr);
	}

	/* Duplicates should have been sorted out before start of recovery. */

	ut_a(names->size() == 1);

	auto&	path = names->front();

	if (path.compare(to_name) == 0) {

		/* Rename must have succeeded, open the file. */

		bool	success;

		success = fil_tablespace_open_for_recovery(page_id.space());

		ut_a(success);

	} else if (path.compare(from_name) == 0) {

		/* Replay the rename. */

#ifdef UNIV_HOTBACKUP
		ut_ad(recv_replay_file_ops);
#endif /* UNIV_HOTBACKUP */

		/* In order to replay the rename, the following must hold:
		1. The to name is not already used.
		2. A tablespace exists with the from name.
		3. The space ID for that tablepace matches this log entry.
		This will prevent unintended renames during recovery. */

		space_id_t	space_id = page_id.space();
		fil_space_t*	space = fil_space_get(space_id);

		if (space == nullptr) {
			return(ptr);
		}

		/* If the file is opened then it must match the from name. */

		const auto&	file = space->files.front();

		ut_a(strncmp(file.name, from_name, from_len) == 0);

		/* Create the database directory for the new name, if
		it does not exist yet. */

		const char*	namend = strrchr(to_name, OS_PATH_SEPARATOR);
		ut_a(namend != nullptr);

		char*	dir = static_cast<char*>(
			ut_malloc_nokey(namend - to_name + 1));

		memcpy(dir, to_name, namend - to_name);
		dir[namend - to_name] = '\0';

		bool	success = os_file_create_directory(dir, false);
		ut_a(success);

		ulint	dirlen;

		if (const char* dirend = strrchr(dir, OS_PATH_SEPARATOR)) {
			dirlen = dirend - dir + 1;
		} else {
			dirlen = 0;
		}

		ut_free(dir);

		char*	new_table = mem_strdupl(
			to_name + dirlen, strlen(to_name + dirlen)
			- 4 /* Remove ".ibd" */);

		ut_ad(new_table[namend - to_name - dirlen]
		      == OS_PATH_SEPARATOR);

#if OS_PATH_SEPARATOR != '/'
		new_table[namend - to_name - dirlen] = '/';
#endif /* OS_PATH_SEPARATOR != '/' */

		clone_mark_abort(true);

		if (!fil_rename_tablespace(
				space_id, from_name, new_table, to_name)) {

			ut_error;
		}

		clone_mark_active();

		ut_free(new_table);

		/* Update the name in the space ID to file name mapping. */
		path.assign(to_name);

		success = fil_tablespace_open_for_recovery(page_id.space());

		ut_a(success);

		ib::info() << "RENAME " << from_name << " TO " << to_name;
	}

	return(ptr);
}

/** Redo a tablespace delete.
@param[in]	ptr		redo log record
@param[in]	end		end of the redo log buffer
@param[in]	page_id		Tablespace Id and first page in file
@param[in]	parsed_bytes	Number of bytes parsed so far
@return pointer to next redo log record
@retval nullptr if this log record was truncated */
byte*
fil_tablespace_redo_delete(
	byte*			ptr,
	const byte*		end,
	const page_id_t&	page_id,
	ulint			parsed_bytes)
{
	ut_a(page_id.page_no() == 0);

	/* We never recreate the system tablespace. */
	ut_a(page_id.space() != TRX_SYS_SPACE);

	ut_a(parsed_bytes != ULINT_UNDEFINED);

	/* Where 2 =  len (uint16_t). */
	if (end <= ptr + 2) {
		return(nullptr);
	}

	ulint	len = mach_read_from_2(ptr);

	ptr += 2;

	/* Do we have the full/valid file name. */
	if (end < ptr + len || len < 5) {

		if (len < 5) {

			char	name[6];

			snprintf(name, sizeof(name), "%.*s", (int) len, ptr);

			ib::error()
				<< "MLOG_FILE_DELETE : Invalid file name."
				<< " Length (" << len << ") must be >= 5"
				<< " and end in '.ibd'. File name in the"
				<< " redo log is '" << name << "'";
		}

		return(nullptr);
	}

	char*	name	= reinterpret_cast<char*>(ptr);

	os_normalize_path(name);

	ptr += len;

	if (memcmp(ptr - 5, DOT_IBD, 5) != 0) {

		recv_sys->found_corrupt_log = true;

		return(nullptr);
	}

	const auto	names = tablespace_files->find(page_id.space());

	if (names == nullptr) {

		/* No files map to this tablespace ID. The drop must
		have succeeded. */

		return(ptr);
	}

	/* Duplicates should have been sorted out before we get here. */

	ut_a(names->size() == 1);

	/* If the space ID maps to a file on disk then the name must match. */

	ut_a(names->front().compare(name) == 0);

	fil_space_free(page_id.space(), false);

	tablespace_files->erase(page_id.space());

	recv_sys->deleted.insert(page_id.space());
	recv_sys->missing_ids.erase(page_id.space());

	return(ptr);
}

/** Tokenize a path specification. Convert relative paths to absolute paths.
Check if the paths are valid and filter out invalid or unreadable directories.
Sort and filter out duplicates from dirs.
@param[in]	str		Path specification to tokenize
@param[out]	dirs		Parsed directory paths	
@param[in]	delimiters	Delimiters */
static
void
fil_tokenize_paths(
	const std::string&		str,
	std::vector<std::string>&	dirs,
	const std::string&		delimiters)
{
	std::string::size_type	start = str.find_first_not_of(delimiters);
	std::string::size_type	end = str.find_first_of(delimiters, start);

	/* Scan until 'end' and 'start' don't reach the end of string (npos) */
	while (std::string::npos != start || std::string::npos != end) {

		std::array<char, OS_FILE_MAX_PATH>	dir;

		dir.fill(0);

		const auto	path = str.substr(start, end - start);

		ut_a(path.length() < dir.max_size());

		std::copy(path.begin(), path.end(), dir.data());

		/* Filter out paths that contain '*'. */
		auto	pos = path.find('*');

		/* Filter out invalid path components. */
		if (pos == std::string::npos) {

			os_normalize_path(dir.data());

			std::string	cur_path(dir.data());

			os_file_type_t	type;
			bool		exists;

			if (os_file_status(
				cur_path.c_str(), &exists, &type) && exists) {

				if (type == OS_FILE_TYPE_DIR) {


					dirs.push_back(cur_path);

				} else {
					ib::warn()
						<< "'" << path << "' ignored, "
						<< " not a directory";
				}

			} else {
				ib::warn()
				<< "'" << path << "' ignored"
				<< " os_file_status() failed.";
			}
		} else {
			ib::warn()
				<< "Scan path '" << path << "' ignored"
				<< " contains '*'";
		}

		start = str.find_first_not_of(delimiters, end);

		end = str.find_first_of(delimiters, start);
	}

	/* Remove duplicate paths. Note, this will change the order of
	the directory scan because of the sort. */

	std::sort(dirs.begin(), dirs.end());
	dirs.erase(std::unique(dirs.begin(), dirs.end() ), dirs.end());
}

/** Check for duplicate tablespace IDs.
@param[in]	files		Files to check for duplicate tablespace IDs
@param[in,out]	duplicates	Duplicate space IDs found */
static
void
fil_check_for_duplicate_ids(const Dirs& files, Duplicates* duplicates)
{
	for (const auto& filename : files) {

		std::ifstream	ifs(filename, std::ios::binary);

		if (!ifs) {
			ib::warn() << "Unable to open '" << filename << "'";
			continue;
		}

		ifs.seekg(FIL_PAGE_SPACE_ID, ifs.beg);

		char	buf[sizeof(space_id_t)];

		ifs.read(buf, sizeof(buf));

		if (!ifs.good() || (size_t) ifs.gcount() < sizeof(buf)) {

			ib::warn()
				<< "Unable to read tablespace ID from"
				<< " '" << filename << "'";
		} else {

			space_id_t	space_id;

			space_id = mach_read_from_4(
				reinterpret_cast<byte*>(buf));

			size_t	n_files = tablespace_files->add(
				space_id, filename);

			if (n_files > 1) {

				duplicates->insert(space_id);
			}
		}

		ifs.close();
	}
}

/** Discover tablespaces by reading the header from .ibd files.
@param[in]	directories	Directories to scan
@return DB_SUCCESS if all goes well */
dberr_t
fil_scan_for_tablespaces(const std::string& directories)
{
	ib::info() << "Directories to scan '" << directories << "'";

	Dirs	dirs;
	Dirs	ibd_files;
	Dirs	undo_files;

	fil_tokenize_paths(directories, dirs, ";");

	/* Should be trivial to parallelize the scan and ID check. */
	for (const auto& dir : dirs) {

		ib::info() << "Scanning '" << dir << "'";

		/* Walk the sub-tree of dir. */

		Dir_Walker::walk(dir, [&](const std::string& path)
		{
			/* If it is a file and the suffix
			matches ".ibd" then store it for reading. */

			if (Dir_Walker::is_directory(path)
			    || path.size() <= 4) {

				return;
			}

			if (path.compare(path.length() - 4, 4, ".ibd") == 0) {

				ibd_files.push_back(path);

			} else if (fil_is_undo_tablespace_name(
				   path.c_str(), path.length())) {

				undo_files.push_back(path);
			}
		});
	}

	ib::info()
		<< "Found " << ibd_files.size()
		<< " '.ibd' and "
		<< undo_files.size() << " undo files";

	ut_a(tablespace_files == nullptr);
	tablespace_files = UT_NEW_NOKEY(Tablespace_files());

	Duplicates	duplicates;

	fil_check_for_duplicate_ids(ibd_files, &duplicates);
	fil_check_for_duplicate_ids(undo_files, &duplicates);

	dberr_t	err;

	if (!duplicates.empty()) {

		ib::warn() << "Multiple files found for tablespace ID(s)";

		err = DB_FAIL;
	} else {
		err = DB_SUCCESS;
	}

	/* Print the duplicate names to the error log. */
	for (auto space_id : duplicates) {

		const auto	names = tablespace_files->find(space_id);

		ut_a(names->size() > 1);

		std::ostringstream	oss;

		oss << "Tablespace ID: " << space_id << " = [";

		for (size_t i = 0; i < names->size(); ++i) {

			oss << "'" << (*names)[i] << "'";

			if (i < names->size() - 1) {
				oss << ", ";
			}
		}

		oss << "]" << std::endl;

		ib::warn() << oss.str();
	}

	return(err);
}

/** Callback to check tablespace size with space header size and extend
@param[in]	file	Tablespace file
@param[in]	context	callers context, currently unused
@return	error code */
dberr_t
fil_check_extend_space(
	fil_node_t*	file,
	void* 		context MY_ATTRIBUTE((unused)))
{
	dberr_t	err = DB_SUCCESS;
	bool	open_node = !file->is_open;

	if (recv_sys == nullptr || !recv_sys->is_cloned_db) {

		return(DB_SUCCESS);
	}

	if (open_node && !fil_node_open_file(file, false)) {

		return(DB_CANNOT_OPEN_FILE);
	}

	mutex_exit(&fil_system->mutex);

	fil_space_t*	space = file->space;

	if (space->size < space->size_in_header) {

		ib::info() << "Extending space: " << space->name
			   << " from size " << space->size
			   << " pages to " << space->size_in_header
			   << " pages as stored in space header.";

		if(!fil_space_extend(space, space->size_in_header)) {

			ib::error() << "Failed to extend tablespace."
				    << " Check for free space in disk"
				    << " and try again.";

			err = DB_OUT_OF_FILE_SPACE;
		}
	}

	mutex_enter(&fil_system->mutex);

	/* Close the file if it was opened by current function */
	if (open_node) {

		fil_node_close_file(file, false);
	}

	return(err);
}
