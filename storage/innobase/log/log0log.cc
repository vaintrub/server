/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Google Inc.
Copyright (c) 2014, 2021, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file log/log0log.cc
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "univ.i"
#include <debug_sync.h>
#include <my_service_manager.h>

#include "log0log.h"
#include "log0crypt.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0stats_bg.h"
#include "btr0defragment.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "srv0mon.h"
#include "buf0dump.h"
#include "log0sync.h"
#include "log.h"

/*
General philosophy of InnoDB redo-logs:

Every change to a contents of a data page must be done
through mtr_t, and mtr_t::commit() will write log records
to the InnoDB redo log. */

/** Redo log system */
log_t	log_sys;

/* Margins for free space in the log buffer after a log entry is catenated */
#define LOG_BUF_FLUSH_RATIO	2
#define LOG_BUF_FLUSH_MARGIN	((4 * 4096) /* cf. log_t::append_prepare() */ \
				 + (4U << srv_page_size_shift))

/** Extends the log buffer.
@param[in]	len	requested minimum size in bytes */
void log_buffer_extend(ulong len)
{
	const size_t new_buf_size = ut_calc_align(len, srv_page_size);
	byte* new_buf = static_cast<byte*>
		(ut_malloc_dontdump(new_buf_size, PSI_INSTRUMENT_ME));
	byte* new_flush_buf = static_cast<byte*>
		(ut_malloc_dontdump(new_buf_size, PSI_INSTRUMENT_ME));

	mysql_mutex_lock(&log_sys.mutex);

	if (len <= srv_log_buffer_size) {
		/* Already extended enough by the others */
		mysql_mutex_unlock(&log_sys.mutex);
		ut_free_dodump(new_buf, new_buf_size);
		ut_free_dodump(new_flush_buf, new_buf_size);
		return;
	}

	sql_print_warning("InnoDB: The redo log transaction size %lu"
			  " exceeds innodb_log_buffer_size=%lu",
			  srv_log_buffer_size / 2);

	byte* old_buf = log_sys.buf;
	byte* old_flush_buf = log_sys.flush_buf;
	const ulong old_buf_size = srv_log_buffer_size;
	srv_log_buffer_size = static_cast<ulong>(new_buf_size);
	log_sys.buf = new_buf;
	log_sys.flush_buf = new_flush_buf;
	memcpy_aligned<4096>(new_buf, old_buf, log_sys.buf_free);

	log_sys.max_buf_free = new_buf_size / LOG_BUF_FLUSH_RATIO
		- LOG_BUF_FLUSH_MARGIN;

	mysql_mutex_unlock(&log_sys.mutex);

	ut_free_dodump(old_buf, old_buf_size);
	ut_free_dodump(old_flush_buf, old_buf_size);

	sql_print_information("InnoDB: innodb_log_buffer_size was extended"
			      " to %zu.", new_buf_size);
}

/** Calculate the recommended highest values for lsn - last_checkpoint_lsn
and lsn - buf_pool.get_oldest_modification().
@param[in]	file_size	requested innodb_log_file_size
@retval true on success
@retval false if the smallest log group is too small to
accommodate the number of OS threads in the database server */
bool
log_set_capacity(ulonglong file_size)
{
	/* Margin for the free space in the smallest log, before a new query
	step which modifies the database, is started */
	const size_t LOG_CHECKPOINT_FREE_PER_THREAD = 4U
						      << srv_page_size_shift;
	const size_t LOG_CHECKPOINT_EXTRA_FREE = 8U << srv_page_size_shift;

	lsn_t		margin;
	ulint		free;

	lsn_t smallest_capacity = file_size - log_t::START_OFFSET;
	/* Add extra safety */
	smallest_capacity -= smallest_capacity / 10;

	/* For each OS thread we must reserve so much free space in the
	smallest log group that it can accommodate the log entries produced
	by single query steps: running out of free log space is a serious
	system error which requires rebooting the database. */

	free = LOG_CHECKPOINT_FREE_PER_THREAD * 10
		+ LOG_CHECKPOINT_EXTRA_FREE;
	if (free >= smallest_capacity / 2) {
		ib::error() << "innodb_log_file_size is too small. "
			    << INNODB_PARAMETERS_MSG;
		return false;
	}

	margin = smallest_capacity - free;
	margin = margin - margin / 10;	/* Add still some extra safety */

	mysql_mutex_lock(&log_sys.mutex);

	log_sys.log_capacity = smallest_capacity;

	log_sys.max_modified_age_async = margin - margin / 8;
	log_sys.max_checkpoint_age = margin;

	mysql_mutex_unlock(&log_sys.mutex);

	return(true);
}

/** Initialize the redo log subsystem. */
void log_t::create()
{
  ut_ad(this == &log_sys);
  ut_ad(!is_initialised());
  m_initialised= true;

#if defined(__aarch64__)
  mysql_mutex_init(log_sys_mutex_key, &mutex, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(
    log_flush_order_mutex_key, &flush_order_mutex, MY_MUTEX_INIT_FAST);
#else
  mysql_mutex_init(log_sys_mutex_key, &mutex, nullptr);
  mysql_mutex_init(log_flush_order_mutex_key, &flush_order_mutex, nullptr);
#endif

  /* Start the lsn from one log block from zero: this way every
  log record has a non-zero start lsn, a fact which we will use */

  set_lsn(FIRST_LSN);
  set_flushed_lsn(FIRST_LSN);

  ut_ad(srv_log_buffer_size >= 4U << srv_page_size_shift);

  buf= static_cast<byte*>(ut_malloc_dontdump(srv_log_buffer_size,
                                             PSI_INSTRUMENT_ME));
  TRASH_ALLOC(buf, srv_log_buffer_size);
  flush_buf= static_cast<byte*>(ut_malloc_dontdump(srv_log_buffer_size,
                                                   PSI_INSTRUMENT_ME));
  TRASH_ALLOC(flush_buf, srv_log_buffer_size);

  max_buf_free= srv_log_buffer_size / LOG_BUF_FLUSH_RATIO -
    LOG_BUF_FLUSH_MARGIN;
  set_check_flush_or_checkpoint();

  n_log_ios_old= n_log_ios;
  last_printout_time= time(NULL);

  buf_next_to_write= 0;
  last_checkpoint_lsn= write_lsn= FIRST_LSN;
  n_log_ios= 0;
  n_log_ios_old= 0;
  log_capacity= 0;
  max_modified_age_async= 0;
  max_checkpoint_age= 0;
  next_checkpoint_lsn= 0;
  n_pending_checkpoint_writes= 0;

  buf_free= 0;
  checkpoint_buf= static_cast<byte*>(aligned_malloc(4096, 4096));
  memset_aligned<4096>(checkpoint_buf, 0, 4096);
}

file_os_io::file_os_io(file_os_io &&rhs) : m_fd(rhs.m_fd)
{
  rhs.m_fd= OS_FILE_CLOSED;
}

file_os_io &file_os_io::operator=(file_os_io &&rhs)
{
  std::swap(m_fd, rhs.m_fd);
  return *this;
}

file_os_io::~file_os_io() noexcept
{
  if (is_opened())
    close();
}

dberr_t file_os_io::open(const char *path, bool read_only) noexcept
{
  ut_ad(!is_opened());

  bool success;
  auto tmp_fd= os_file_create(
      innodb_log_file_key, path, OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
      OS_FILE_NORMAL, OS_LOG_FILE, read_only, &success);
  if (!success)
    return DB_ERROR;

  m_durable_writes= srv_file_flush_method == SRV_O_DSYNC;
  m_fd= tmp_fd;
  return success ? DB_SUCCESS : DB_ERROR;
}

dberr_t file_os_io::rename(const char *old_path, const char *new_path) noexcept
{
  return os_file_rename(innodb_log_file_key, old_path, new_path) ? DB_SUCCESS
                                                                 : DB_ERROR;
}

dberr_t file_os_io::close() noexcept
{
  if (!os_file_close(m_fd))
    return DB_ERROR;

  m_fd= OS_FILE_CLOSED;
  return DB_SUCCESS;
}

dberr_t file_os_io::read(os_offset_t offset, span<byte> buf) noexcept
{
  return os_file_read(IORequestRead, m_fd, buf.data(), offset, buf.size());
}

dberr_t file_os_io::write(const char *path, os_offset_t offset,
                          span<const byte> buf) noexcept
{
  return os_file_write(IORequestWrite, path, m_fd, buf.data(), offset,
                       buf.size());
}

dberr_t file_os_io::flush() noexcept
{
  return os_file_flush(m_fd) ? DB_SUCCESS : DB_ERROR;
}

#ifdef HAVE_PMEM

#include <libpmem.h>

/** Memory mapped file */
class mapped_file_t
{
public:
  mapped_file_t()= default;
  mapped_file_t(const mapped_file_t &)= delete;
  mapped_file_t &operator=(const mapped_file_t &)= delete;
  mapped_file_t(mapped_file_t &&)= delete;
  mapped_file_t &operator=(mapped_file_t &&)= delete;
  ~mapped_file_t() noexcept;

  dberr_t map(const char *path, bool read_only= false,
              bool nvme= false) noexcept;
  dberr_t unmap() noexcept;
  byte *data() noexcept { return m_area.data(); }

private:
  span<byte> m_area;
};

mapped_file_t::~mapped_file_t() noexcept
{
  if (!m_area.empty())
    unmap();
}

dberr_t mapped_file_t::map(const char *path, bool read_only,
                           bool nvme) noexcept
{
  auto fd= mysql_file_open(innodb_log_file_key, path,
                           read_only ? O_RDONLY : O_RDWR, MYF(MY_WME));
  if (fd == -1)
    return DB_ERROR;

  const auto file_size= size_t{os_file_get_size(path).m_total_size};

  const int nvme_flag= nvme ? MAP_SYNC : 0;
  void *ptr=
      my_mmap(0, file_size, read_only ? PROT_READ : PROT_READ | PROT_WRITE,
              MAP_SHARED_VALIDATE | nvme_flag, fd, 0);
  mysql_file_close(fd, MYF(MY_WME));

  if (ptr == MAP_FAILED)
    return DB_ERROR;

  m_area= {static_cast<byte *>(ptr), file_size};
  return DB_SUCCESS;
}

dberr_t mapped_file_t::unmap() noexcept
{
  ut_ad(!m_area.empty());

  if (my_munmap(m_area.data(), m_area.size()))
    return DB_ERROR;

  m_area= {};
  return DB_SUCCESS;
}

static bool is_pmem(const char *path) noexcept
{
  mapped_file_t mf;
  return mf.map(path, true, true) == DB_SUCCESS ? true : false;
}

class file_pmem_io final : public file_io
{
public:
  file_pmem_io() noexcept : file_io(true) {}

  dberr_t open(const char *path, bool read_only) noexcept final
  {
    return m_file.map(path, read_only, true);
  }
  dberr_t rename(const char *old_path, const char *new_path) noexcept final
  {
    return os_file_rename(innodb_log_file_key, old_path, new_path) ? DB_SUCCESS
                                                                   : DB_ERROR;
  }
  dberr_t close() noexcept final { return m_file.unmap(); }
  dberr_t read(os_offset_t offset, span<byte> buf) noexcept final
  {
    memcpy(buf.data(), m_file.data() + offset, buf.size());
    return DB_SUCCESS;
  }
  dberr_t write(const char *, os_offset_t offset,
                span<const byte> buf) noexcept final
  {
    pmem_memcpy_persist(m_file.data() + offset, buf.data(), buf.size());
    return DB_SUCCESS;
  }
  dberr_t flush() noexcept final
  {
    ut_ad(0);
    return DB_SUCCESS;
  }

private:
  mapped_file_t m_file;
};
#endif

dberr_t log_file_t::open(bool read_only) noexcept
{
  ut_a(!is_opened());

#ifdef HAVE_PMEM
  auto ptr= is_pmem(m_path.c_str())
                ? std::unique_ptr<file_io>(new file_pmem_io)
                : std::unique_ptr<file_io>(new file_os_io);
#else
  auto ptr= std::unique_ptr<file_io>(new file_os_io);
#endif

  if (dberr_t err= ptr->open(m_path.c_str(), read_only))
    return err;

  m_file= std::move(ptr);
  return DB_SUCCESS;
}

bool log_file_t::is_opened() const noexcept
{
  return static_cast<bool>(m_file);
}

dberr_t log_file_t::rename(std::string new_path) noexcept
{
  if (dberr_t err= m_file->rename(m_path.c_str(), new_path.c_str()))
    return err;

  m_path = std::move(new_path);
  return DB_SUCCESS;
}

dberr_t log_file_t::close() noexcept
{
  ut_a(is_opened());

  if (dberr_t err= m_file->close())
    return err;

  m_file.reset();
  return DB_SUCCESS;
}

dberr_t log_file_t::read(os_offset_t offset, span<byte> buf) noexcept
{
  ut_ad(is_opened());
  return m_file->read(offset, buf);
}

bool log_file_t::writes_are_durable() const noexcept
{
  return m_file->writes_are_durable();
}

dberr_t log_file_t::write(os_offset_t offset, span<const byte> buf) noexcept
{
  ut_ad(is_opened());
  return m_file->write(m_path.c_str(), offset, buf);
}

dberr_t log_file_t::flush() noexcept
{
  ut_ad(is_opened());
  return m_file->flush();
}

void log_t::file::open_file(std::string path)
{
  fd= log_file_t(std::move(path));
  if (const dberr_t err= fd.open(srv_read_only_mode))
    ib::fatal() << "open(" << fd.get_path() << ") returned " << err;
}

/** Update the log block checksum. */
static void log_block_store_checksum(byte* block)
{
  mach_write_to_4(my_assume_aligned<4>(508 + block), my_crc32c(0, block, 508));
}

void log_t::file::write_header_durable(lsn_t lsn)
{
  ut_ad(!recv_no_log_write);
  ut_ad(log_sys.is_latest());

  byte *buf= log_sys.checkpoint_buf;
  memset_aligned<4096>(buf, 0, 4096);

  mach_write_to_4(buf + LOG_HEADER_FORMAT, FORMAT_10_8);
  mach_write_to_8(buf + LOG_HEADER_START_LSN, lsn);
  static constexpr const char LOG_HEADER_CREATOR_CURRENT[]=
    "MariaDB "
    IB_TO_STR(MYSQL_VERSION_MAJOR) "."
    IB_TO_STR(MYSQL_VERSION_MINOR) "."
    IB_TO_STR(MYSQL_VERSION_PATCH);

  strcpy(reinterpret_cast<char*>(buf) + LOG_HEADER_CREATOR,
         LOG_HEADER_CREATOR_CURRENT);
  static_assert(LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR >=
                sizeof LOG_HEADER_CREATOR_CURRENT, "compatibility");
  if (log_sys.is_encrypted())
    log_crypt_write_header(buf + LOG_HEADER_CREATOR_END);
  log_block_store_checksum(buf);

  DBUG_PRINT("ib_log", ("write " LSN_PF, lsn));

  log_sys.log.write(0, {buf, 4096});
  if (!log_sys.log.writes_are_durable())
    log_sys.log.flush();

  memset_aligned<4096>(buf, 0, 4096);
}

void log_t::file::read(os_offset_t offset, span<byte> buf)
{
  if (const dberr_t err= fd.read(offset, buf))
    ib::fatal() << "read(" << fd.get_path() << ") returned "<< err;
}

bool log_t::file::writes_are_durable() const noexcept
{
  return fd.writes_are_durable();
}

void log_t::file::write(os_offset_t offset, span<const byte> buf)
{
  srv_stats.os_log_pending_writes.inc();
  if (const dberr_t err= fd.write(offset, buf))
    ib::fatal() << "write(" << fd.get_path() << ") returned " << err;
  srv_stats.os_log_pending_writes.dec();
  srv_stats.os_log_written.add(buf.size());
  srv_stats.log_writes.inc();
  log_sys.n_log_ios++;
}

void log_t::file::flush()
{
  log_sys.pending_flushes.fetch_add(1, std::memory_order_acquire);
  if (const dberr_t err= fd.flush())
    ib::fatal() << "flush(" << fd.get_path() << ") returned " << err;
  log_sys.pending_flushes.fetch_sub(1, std::memory_order_release);
  log_sys.flushes.fetch_add(1, std::memory_order_release);
}

void log_t::file::close_file()
{
  if (fd.is_opened())
  {
    if (const dberr_t err= fd.close())
      ib::fatal() << "close(" << fd.get_path() << ") returned " << err;
  }
  fd.free();                                    // Free path
}

/** Initialize the redo log. */
void log_t::file::create()
{
  ut_ad(this == &log_sys.log);
  ut_ad(log_sys.is_initialised());

  format= srv_encrypt_log ? log_t::FORMAT_ENC_10_8 : log_t::FORMAT_10_8;
  file_size= srv_log_file_size;
}

/******************************************************//**
Writes a buffer to a log file. */
static void log_write_buf(const byte *buf, size_t len, lsn_t start_lsn)
{
	ut_ad(log_write_lock_own());
	ut_ad(!recv_no_log_write);

loop:
	ut_ad(len);
	auto write_len = len;
	lsn_t next_offset = log_sys.log.calc_lsn_offset(start_lsn);

	if ((next_offset % log_sys.log.file_size) + len
	    > log_sys.log.file_size) {
		/* if the above condition holds, then the below expression
		is < len which is ulint, so the typecast is ok */
		write_len = ulint(log_sys.log.file_size
				  - (next_offset % log_sys.log.file_size));
	}

	DBUG_PRINT("ib_log",
		   ("write " LSN_PF " to " LSN_PF ": len " ULINTPF,
		    start_lsn, next_offset, write_len));

	log_sys.log.write(next_offset, {buf, write_len});

	if (UNIV_UNLIKELY(write_len < len)) {
		start_lsn += write_len;
		len -= write_len;
		buf += write_len;
		goto loop;
	}
}

/** Flush the recently written changes to the log file.
and invoke mysql_mutex_lock(&log_sys.mutex). */
static void log_write_flush_to_disk_low(lsn_t lsn)
{
  if (!log_sys.log.writes_are_durable())
    log_sys.log.flush();
  ut_a(lsn >= log_sys.get_flushed_lsn());
  log_sys.set_flushed_lsn(lsn);
}

/** Invoke commit_checkpoint_notify_ha() to notify that outstanding
log writes have been completed. */
void log_flush_notify(lsn_t flush_lsn);

#if 0
/** CRC-32C of pad messages using between 1 and 15 bytes of NUL bytes
in the payload */
static const unsigned char pad_crc[15][4]= {
  {0xA6,0x59,0xC1,0xDB}, {0xF2,0xAF,0x80,0x73}, {0xED,0x02,0xF1,0x90},
  {0x68,0x4E,0xA3,0xF3}, {0x5D,0x1B,0xEA,0x6A}, {0xE0,0x01,0x86,0xB9},
  {0xD1,0x06,0x86,0xF5}, {0xEB,0x20,0x12,0x33}, {0xBA,0x73,0xB2,0xA3},
  {0x5F,0xA2,0x08,0x03}, {0x70,0x03,0xD6,0x9D}, {0xED,0xB3,0x49,0x78},
  {0xFD,0xD6,0xB9,0x9C}, {0x25,0xF8,0xB1,0x2C}, {0xCD,0xAA,0xE7,0x10}
};

/** Pad the log with some dummy bytes */
ATTRIBUTE_NOINLINE static void log_pad(size_t pad)
{
  byte *const begin= log_sys.buf + log_sys.buf_free, *b= begin;
  const lsn_t lsn{log_sys.get_lsn()};

  if (log_sys.is_encrypted())
  {
    /* The lengths of our pad messages vary between 15 and 29 bytes
    (FILE_CHECKPOINT byte, 1 to 15 NUL bytes, sequence byte,
    4 bytes checksum, 8 NUL bytes nonce). */
    if (pad < 15)
    {
      log_sys.set_lsn(lsn + 15);
      log_sys.buf_free+= 15;
      b[0]= FILE_CHECKPOINT | 1;
      b[1]= 0;
      b[2]= log_sys.get_sequence_bit(lsn + 15);
      memcpy(b + 3, pad_crc[0], 4);
      memset(b + 7, 0, 8);
    }
    else
    {
      const byte seq{log_sys.get_sequence_bit(lsn)};
      /* The caller should never request padding such that the
      file would wrap around to the beginning. That is, the sequence
      bit must be the same for all records. */
      ut_ad(seq == log_sys.get_sequence_bit(lsn + pad));
      log_sys.set_lsn(lsn + pad);
      log_sys.buf_free+= pad;
      /* Pad first with 29-byte messages until the remaining size is
      less than 29+15 bytes, and then write 1 or 2 shorter messages. */
      const byte *const end= begin + pad;
      for (; b + (29 + 15) < end; b+= 29)
      {
        b[0]= FILE_CHECKPOINT | 15;
        memset(b + 1, 0, 15);
        b[16]= seq;
        memcpy(b + 17, pad_crc[14], 4);
        memset(b + 21, 0, 8);
      }
      if (b + 29 < end)
      {
        b[0]= FILE_CHECKPOINT | 1;
        b[1]= 0;
        b[2]= seq;
        memcpy(b + 3, pad_crc[0], 4);
        memset(b + 7, 0, 8);
        b+= 15;
      }
      const size_t last_pad(end - b);
      ut_ad(last_pad >= 15);
      ut_ad(last_pad <= 29);
      b[0]= FILE_CHECKPOINT | byte(last_pad - 14);
      memset(b + 1, 0, last_pad - 14);
      b[last_pad - 13]= seq;
      memcpy(b + last_pad - 12, pad_crc[last_pad - 15], 4);
      memset(b + last_pad - 8, 0, 8);
    }
  }
  else
  {
    /* The lengths of our pad messages vary between 7 and 21 bytes
    (FILE_CHECKPOINT byte, 1 to 15 NUL bytes, sequence byte,
    4 bytes checksum). */
    if (pad < 7)
    {
      log_sys.set_lsn(lsn + 7);
      log_sys.buf_free+= 7;
      b[0]= FILE_CHECKPOINT | 1;
      b[1]= 0;
      b[2]= log_sys.get_sequence_bit(lsn + 7);
      memcpy(b + 3, pad_crc[0], 4);
    }
    else
    {
      const byte seq{log_sys.get_sequence_bit(lsn)};
      /* The caller should never request padding such that the
      file would wrap around to the beginning. That is, the sequence
      bit must be the same for all records. */
      ut_ad(seq == log_sys.get_sequence_bit(lsn + pad));
      log_sys.set_lsn(lsn + pad);
      log_sys.buf_free+= pad;
      /* Pad first with 21-byte messages until the remaining size is
      less than 21+7 bytes, and then write 1 or 2 shorter messages. */
      const byte *const end= begin + pad;
      for (; b + (21 + 7) < end; b+= 21)
      {
        b[0]= FILE_CHECKPOINT | 15;
        memset(b + 1, 0, 15);
        b[16]= seq;
        memcpy(b + 17, pad_crc[14], 4);
      }
      if (b + 21 < end)
      {
        b[0]= FILE_CHECKPOINT | 1;
        b[1]= 0;
        b[2]= seq;
        memcpy(b + 3, pad_crc[0], 4);
        b+= 7;
      }
      const size_t last_pad(end - b);
      ut_ad(last_pad >= 7);
      ut_ad(last_pad <= 21);
      b[0]= FILE_CHECKPOINT | byte(last_pad - 6);
      memset(b + 1, 0, last_pad - 6);
      b[last_pad - 5]= seq;
      memcpy(b + last_pad - 4, pad_crc[last_pad - 7], 4);
    }
  }
}
#endif

/**
Writes log buffer to disk
which is the "write" part of log_write_up_to().

This function does not flush anything.

Note : the caller must have log_sys.mutex locked, and this
mutex is released in the function.

*/
static void log_write()
{
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(!recv_no_log_write);

	if (log_sys.buf_free == log_sys.buf_next_to_write) {
		/* Nothing to write */
		mysql_mutex_unlock(&log_sys.mutex);
		return;
	}

#if 0 // testing
	static size_t pad;
	if (size_t append = pad++ % 200) {
		log_pad(append);
	}
#endif

	ulint		start_offset;
	ulint		end_offset;
	ulint		area_start;
	ulint		area_end;

	start_offset = log_sys.buf_next_to_write;
	end_offset = log_sys.buf_free;

	area_start = start_offset;
	area_end = end_offset;

	const lsn_t lsn = log_sys.get_lsn();
	byte *write_buf = log_sys.buf;

	DBUG_PRINT("ib_log", ("write " LSN_PF " to " LSN_PF,
			      log_sys.write_lsn, lsn));

#if 0 // FIXME
	log_buffer_switch();
#else
	std::swap(log_sys.buf, log_sys.flush_buf);
	log_sys.buf_free = 0;
	log_sys.buf_next_to_write = 0;
#endif

	mysql_mutex_unlock(&log_sys.mutex);
	/* Calculate pad_size if needed. */
	size_t pad_size = 0;
#if 0 // FIXME
	/* Erase the end of the last log block. */
	memset(write_buf + end_offset, 0,
	       -end_offset & (OS_FILE_LOG_BLOCK_SIZE - 1));
	const auto write_ahead_size = srv_log_write_ahead_size;

	if (write_ahead_size > OS_FILE_LOG_BLOCK_SIZE) {
		ulint	end_offset_in_unit;
		lsn_t	end_offset = log_sys.log.calc_lsn_offset(
			ut_uint64_align_up(lsn, OS_FILE_LOG_BLOCK_SIZE));
		end_offset_in_unit = (ulint) (end_offset % write_ahead_size);

		if (end_offset_in_unit > 0
		    && (area_end - area_start) > end_offset_in_unit) {
			/* The first block in the unit was initialized
			after the last writing.
			Needs to be written padded data once. */
			pad_size = std::min<size_t>(
				size_t(write_ahead_size) - end_offset_in_unit,
				srv_log_buffer_size - area_end);
			::memset(write_buf + area_end, 0, pad_size);
		}
	}
#endif

	if (UNIV_UNLIKELY(srv_shutdown_state > SRV_SHUTDOWN_INITIATED)) {
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
					       "InnoDB log write: "
					       LSN_PF, log_sys.write_lsn);
	}

	/* Do the write to the log file */
	log_write_buf(write_buf + area_start, area_end - area_start,
		      log_sys.write_lsn);
	srv_stats.log_padded.add(pad_size);
	log_sys.write_lsn = lsn;
	if (log_sys.log.writes_are_durable()) {
		log_sys.set_flushed_lsn(lsn);
		log_flush_notify(lsn);
	}
	return;
}

static group_commit_lock write_lock;
static group_commit_lock flush_lock;

#ifdef UNIV_DEBUG
bool log_write_lock_own()
{
  return write_lock.is_owner();
}
#endif


/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param lsn      log sequence number that should be included in the file write
@param durable  whether the log should be durably written
@param callback log write completion callback */
void log_write_up_to(lsn_t lsn, bool durable,
                     const completion_callback *callback)
{
  ut_ad(!srv_read_only_mode);
  ut_ad(lsn != LSN_MAX);

  if (recv_no_ibuf_operations)
  {
    /* Recovery is running and no operations on the log files are
    allowed yet (the variable name .._no_ibuf_.. is misleading) */
    ut_a(!callback);
    return;
  }

repeat:
  lsn_t ret_lsn1= 0, ret_lsn2= 0;

  if (durable &&
      flush_lock.acquire(lsn, callback) != group_commit_lock::ACQUIRED)
    return;

  if (write_lock.acquire(lsn, durable ? nullptr : callback) ==
      group_commit_lock::ACQUIRED)
  {
    mysql_mutex_lock(&log_sys.mutex);
    lsn_t write_lsn= log_sys.get_lsn();
    write_lock.set_pending(write_lsn);

    log_write();

    ut_a(log_sys.write_lsn == write_lsn);
    ret_lsn1= write_lock.release(write_lsn);
  }

  if (durable)
  {
    /* Flush the highest written lsn.*/
    auto flush_lsn = write_lock.value();
    flush_lock.set_pending(flush_lsn);
    log_write_flush_to_disk_low(flush_lsn);
    ret_lsn2= flush_lock.release(flush_lsn);

    log_flush_notify(flush_lsn);
    DBUG_EXECUTE_IF("crash_after_log_write_upto", DBUG_SUICIDE(););
  }

  if (ret_lsn1 || ret_lsn2)
  {
    /*
     There is no new group commit lead, some async waiters could stall.
     Rerun log_write_up_to(), to prevent that.
    */
    lsn= std::max(ret_lsn1, ret_lsn2);
    static const completion_callback dummy{[](void *) {},nullptr};
    callback= &dummy;
    goto repeat;
  }
}

/** Write to the log file up to the last log entry.
@param sync  whether to wait for a durable write to complete */
void log_buffer_flush_to_disk(bool sync)
{
  ut_ad(!srv_read_only_mode);
  log_write_up_to(log_sys.get_lsn(std::memory_order_acquire), sync);
}

/** Prepare to invoke log_write_and_flush(), before acquiring log_sys.mutex. */
ATTRIBUTE_COLD void log_write_and_flush_prepare()
{
  mysql_mutex_assert_not_owner(&log_sys.mutex);

  while (flush_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
  while (write_lock.acquire(log_sys.get_lsn() + 1, nullptr) !=
         group_commit_lock::ACQUIRED);
}

/** Durably write the log and release log_sys.mutex */
ATTRIBUTE_COLD void log_write_and_flush()
{
  ut_ad(!srv_read_only_mode);
  auto lsn= log_sys.get_lsn();
  write_lock.set_pending(lsn);
  log_write();
  ut_a(log_sys.write_lsn == lsn);
  write_lock.release(lsn);

  lsn= write_lock.value();
  flush_lock.set_pending(lsn);
  log_write_flush_to_disk_low(lsn);
  flush_lock.release(lsn);
}

/********************************************************************

Tries to establish a big enough margin of free space in the log buffer, such
that a new log entry can be catenated without an immediate need for a flush. */
ATTRIBUTE_COLD static void log_flush_margin()
{
	lsn_t	lsn	= 0;

	mysql_mutex_lock(&log_sys.mutex);

	if (log_sys.buf_free > log_sys.max_buf_free) {
		/* We can write during flush */
		lsn = log_sys.get_lsn();
	}

	mysql_mutex_unlock(&log_sys.mutex);

	if (lsn) {
		log_write_up_to(lsn, false);
	}
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log, such
that a new log entry can be catenated without an immediate need for a
checkpoint. NOTE: this function may only be called if the calling thread
owns no synchronization objects! */
ATTRIBUTE_COLD static void log_checkpoint_margin()
{
  while (log_sys.check_flush_or_checkpoint())
  {
    mysql_mutex_lock(&log_sys.mutex);
    ut_ad(!recv_no_log_write);

    if (!log_sys.check_flush_or_checkpoint())
    {
func_exit:
      mysql_mutex_unlock(&log_sys.mutex);
      return;
    }

    const lsn_t lsn= log_sys.get_lsn();
    const lsn_t checkpoint= log_sys.last_checkpoint_lsn;
    const lsn_t sync_lsn= checkpoint + log_sys.max_checkpoint_age;
    if (lsn <= sync_lsn)
    {
      log_sys.set_check_flush_or_checkpoint(false);
      goto func_exit;
    }

    mysql_mutex_unlock(&log_sys.mutex);

    /* We must wait to prevent the tail of the log overwriting the head. */
    buf_flush_wait_flushed(std::min(sync_lsn, checkpoint + (1U << 20)));
    /* Sleep to avoid a thundering herd */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */
ATTRIBUTE_COLD void log_check_margins()
{
  do
  {
    log_flush_margin();
    log_checkpoint_margin();
    ut_ad(!recv_no_log_write);
  }
  while (log_sys.check_flush_or_checkpoint());
}

extern void buf_resize_shutdown();

/** Make a checkpoint at the latest lsn on shutdown. */
ATTRIBUTE_COLD void logs_empty_and_mark_files_at_shutdown()
{
	lsn_t			lsn;
	ulint			count = 0;

	ib::info() << "Starting shutdown...";

	/* Wait until the master thread and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */
	bool do_srv_shutdown = false;
	if (srv_master_timer) {
		do_srv_shutdown = srv_fast_shutdown < 2;
		srv_master_timer.reset();
	}

	/* Wait for the end of the buffer resize task.*/
	buf_resize_shutdown();
	dict_stats_shutdown();
	btr_defragment_shutdown();

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;

	if (srv_buffer_pool_dump_at_shutdown &&
		!srv_read_only_mode && srv_fast_shutdown < 2) {
		buf_dump_start();
	}
	srv_monitor_timer.reset();

	if (do_srv_shutdown) {
		srv_shutdown(srv_fast_shutdown == 0);
	}


loop:
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);
	ut_ad(fil_system.is_initialised() || !srv_was_started);

#define COUNT_INTERVAL 600U
#define CHECK_INTERVAL 100000U
	std::this_thread::sleep_for(std::chrono::microseconds(CHECK_INTERVAL));

	count++;

	/* Check that there are no longer transactions, except for
	PREPARED ones. We need this wait even for the 'very fast'
	shutdown, because the InnoDB layer may have committed or
	prepared transactions and we don't want to lose them. */

	if (ulint total_trx = srv_was_started && !srv_read_only_mode
	    && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    ? trx_sys.any_active_transactions() : 0) {

		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			service_manager_extend_timeout(
				COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
				"Waiting for %lu active transactions to finish",
				(ulong) total_trx);
			ib::info() << "Waiting for " << total_trx << " active"
				<< " transactions to finish";

			count = 0;
		}

		goto loop;
	}

	/* We need these threads to stop early in shutdown. */
	const char* thread_name;

   if (srv_fast_shutdown != 2 && trx_rollback_is_active) {
		thread_name = "rollback of recovered transactions";
	} else {
		thread_name = NULL;
	}

	if (thread_name) {
		ut_ad(!srv_read_only_mode);
wait_suspend_loop:
		service_manager_extend_timeout(
			COUNT_INTERVAL * CHECK_INTERVAL/1000000 * 2,
			"Waiting for %s to exit", thread_name);
		if (srv_print_verbose_log && count > COUNT_INTERVAL) {
			ib::info() << "Waiting for " << thread_name
				   << " to exit";
			count = 0;
		}
		goto loop;
	}

	/* Check that the background threads are suspended */

	ut_ad(!srv_any_background_activity());
	if (srv_n_fil_crypt_threads_started) {
		fil_crypt_threads_signal(true);
		thread_name = "fil_crypt_thread";
		goto wait_suspend_loop;
	}

	if (buf_page_cleaner_is_active) {
		thread_name = "page cleaner thread";
		pthread_cond_signal(&buf_pool.do_flush_list);
		goto wait_suspend_loop;
	}

	buf_load_dump_end();

	if (!buf_pool.is_initialised()) {
		ut_ad(!srv_was_started);
	} else if (ulint pending_io = buf_pool.io_pending()) {
		if (srv_print_verbose_log && count > 600) {
			ib::info() << "Waiting for " << pending_io << " buffer"
				" page I/Os to complete";
			count = 0;
		}

		goto loop;
	} else {
		buf_flush_buffer_pool();
	}

	if (log_sys.is_initialised()) {
		mysql_mutex_lock(&log_sys.mutex);
		const ulint	n_write	= log_sys.n_pending_checkpoint_writes;
		const ulint	n_flush	= log_sys.pending_flushes;
		mysql_mutex_unlock(&log_sys.mutex);

		if (n_write || n_flush) {
			if (srv_print_verbose_log && count > 600) {
				ib::info() << "Pending checkpoint_writes: "
					<< n_write
					<< ". Pending log flush writes: "
					<< n_flush;
				count = 0;
			}
			goto loop;
		}
	}

	if (srv_fast_shutdown == 2 || !srv_was_started) {
		if (!srv_read_only_mode && srv_was_started) {
			ib::info() << "Executing innodb_fast_shutdown=2."
				" Next startup will execute crash recovery!";

			/* In this fastest shutdown we do not flush the
			buffer pool:

			it is essentially a 'crash' of the InnoDB server.
			Make sure that the log is all flushed to disk, so
			that we can recover all committed transactions in
			a crash recovery. */
			log_buffer_flush_to_disk();
		}

		srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;
		return;
	}

	if (!srv_read_only_mode) {
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
			"ensuring dirty buffer pool are written to log");
		log_make_checkpoint();

                const auto sizeof_cp = log_sys.is_encrypted()
			? SIZE_OF_FILE_CHECKPOINT + 8
			: SIZE_OF_FILE_CHECKPOINT;

		mysql_mutex_lock(&log_sys.mutex);

		lsn = log_sys.get_lsn();

		const bool lsn_changed = lsn != log_sys.last_checkpoint_lsn
			&& lsn != log_sys.last_checkpoint_lsn + sizeof_cp;
		ut_ad(lsn >= log_sys.last_checkpoint_lsn);

		mysql_mutex_unlock(&log_sys.mutex);

		if (lsn_changed) {
			goto loop;
		}

		log_sys.log.flush();
	} else {
		lsn = recv_sys.recovered_lsn;
	}

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
				       "Free innodb buffer pool");
	ut_d(buf_pool.assert_all_freed());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);

	if (UNIV_UNLIKELY(lsn < recv_sys.recovered_lsn)) {
		ib::error() << "Shutdown LSN=" << lsn
			    << " is less than start LSN="
			    << recv_sys.recovered_lsn;
	}

	srv_shutdown_lsn = lsn;

	if (!srv_read_only_mode) {
		dberr_t err = fil_write_flushed_lsn(lsn);

		if (err != DB_SUCCESS) {
			ib::error() << "Writing flushed lsn " << lsn
				<< " failed; error=" << err;
		}
	}

	/* Make some checks that the server really is quiet */
	ut_ad(!srv_any_background_activity());

	ut_a(lsn == log_sys.get_lsn()
	     || srv_force_recovery == SRV_FORCE_NO_LOG_REDO);
}

/******************************************************//**
Prints info of the log. */
void
log_print(
/*======*/
	FILE*	file)	/*!< in: file where to print */
{
	double	time_elapsed;
	time_t	current_time;

	mysql_mutex_lock(&log_sys.mutex);

	const lsn_t lsn= log_sys.get_lsn();
	mysql_mutex_lock(&buf_pool.flush_list_mutex);
	const lsn_t pages_flushed = buf_pool.get_oldest_modification(lsn);
	mysql_mutex_unlock(&buf_pool.flush_list_mutex);

	fprintf(file,
		"Log sequence number " LSN_PF "\n"
		"Log flushed up to   " LSN_PF "\n"
		"Pages flushed up to " LSN_PF "\n"
		"Last checkpoint at  " LSN_PF "\n",
		lsn,
		log_sys.get_flushed_lsn(),
		pages_flushed,
		lsn_t{log_sys.last_checkpoint_lsn});

	current_time = time(NULL);

	time_elapsed = difftime(current_time,
				log_sys.last_printout_time);

	if (time_elapsed <= 0) {
		time_elapsed = 1;
	}

	fprintf(file,
		ULINTPF " pending log flushes, "
		ULINTPF " pending chkp writes\n"
		ULINTPF " log i/o's done, %.2f log i/o's/second\n",
		log_sys.pending_flushes.load(),
		log_sys.n_pending_checkpoint_writes,
		log_sys.n_log_ios,
		static_cast<double>(
			log_sys.n_log_ios - log_sys.n_log_ios_old)
		/ time_elapsed);

	log_sys.n_log_ios_old = log_sys.n_log_ios;
	log_sys.last_printout_time = current_time;

	mysql_mutex_unlock(&log_sys.mutex);
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */
void
log_refresh_stats(void)
/*===================*/
{
	log_sys.n_log_ios_old = log_sys.n_log_ios;
	log_sys.last_printout_time = time(NULL);
}

/** Shut down the redo log subsystem. */
void log_t::close()
{
  ut_ad(this == &log_sys);
  if (!is_initialised()) return;
  m_initialised= false;
  log.close();

  ut_free_dodump(buf, srv_log_buffer_size);
  buf= nullptr;
  ut_free_dodump(flush_buf, srv_log_buffer_size);
  flush_buf= nullptr;

  mysql_mutex_destroy(&mutex);
  mysql_mutex_destroy(&flush_order_mutex);

  recv_sys.close();

  aligned_free(checkpoint_buf);
  checkpoint_buf= nullptr;
}

std::string get_log_file_path(const char *filename)
{
  const size_t size= strlen(srv_log_group_home_dir) + /* path separator */ 1 +
                     strlen(filename) + /* longest suffix */ 3;
  std::string path;
  path.reserve(size);
  path.assign(srv_log_group_home_dir);

  switch (path.back()) {
#ifdef _WIN32
  case '\\':
#endif
  case '/':
    break;
  default:
    path.push_back('/');
  }
  path.append(filename);

  return path;
}

std::vector<std::string> get_existing_log_files_paths() {
  std::vector<std::string> result;

  for (int i= 0; i < 101; i++) {
    auto path= get_log_file_path(LOG_FILE_NAME_PREFIX)
                                 .append(std::to_string(i));
    os_file_stat_t stat;
    dberr_t err= os_file_get_status(path.c_str(), &stat, false, true);
    if (err)
      break;

    if (stat.type != OS_FILE_TYPE_FILE)
      break;

    result.push_back(std::move(path));
  }

  return result;
}
