/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2017, MariaDB Corporation.

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

/**************************************************//**
@file trx/trx0undo.cc
Transaction undo log

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "trx0undo.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "mtr0log.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "trx0rseg.h"
#include "trx0trx.h"

/* How should the old versions in the history list be managed?
   ----------------------------------------------------------
If each transaction is given a whole page for its update undo log, file
space consumption can be 10 times higher than necessary. Therefore,
partly filled update undo log pages should be reusable. But then there
is no way individual pages can be ordered so that the ordering agrees
with the serialization numbers of the transactions on the pages. Thus,
the history list must be formed of undo logs, not their header pages as
it was in the old implementation.
	However, on a single header page the transactions are placed in
the order of their serialization numbers. As old versions are purged, we
may free the page when the last transaction on the page has been purged.
	A problem is that the purge has to go through the transactions
in the serialization order. This means that we have to look through all
rollback segments for the one that has the smallest transaction number
in its history list.
	When should we do a purge? A purge is necessary when space is
running out in any of the rollback segments. Then we may have to purge
also old version which might be needed by some consistent read. How do
we trigger the start of a purge? When a transaction writes to an undo log,
it may notice that the space is running out. When a read view is closed,
it may make some history superfluous. The server can have an utility which
periodically checks if it can purge some history.
	In a parallellized purge we have the problem that a query thread
can remove a delete marked clustered index record before another query
thread has processed an earlier version of the record, which cannot then
be done because the row cannot be constructed from the clustered index
record. To avoid this problem, we will store in the update and delete mark
undo record also the columns necessary to construct the secondary index
entries which are modified.
	We can latch the stack of versions of a single clustered index record
by taking a latch on the clustered index page. As long as the latch is held,
no new versions can be added and no versions removed by undo. But, a purge
can still remove old versions from the bottom of the stack. */

/* How to protect rollback segments, undo logs, and history lists with
   -------------------------------------------------------------------
latches?
-------
The contention of the trx_sys_t::mutex should be minimized. When a transaction
does its first insert or modify in an index, an undo log is assigned for it.
Then we must have an x-latch to the rollback segment header.
	When the transaction does more modifys or rolls back, the undo log is
protected with undo_mutex in the transaction.
	When the transaction commits, its insert undo log is either reset and
cached for a fast reuse, or freed. In these cases we must have an x-latch on
the rollback segment page. The update undo log is put to the history list. If
it is not suitable for reuse, its slot in the rollback segment is reset. In
both cases, an x-latch must be acquired on the rollback segment.
	The purge operation steps through the history list without modifying
it until a truncate operation occurs, which can remove undo logs from the end
of the list and release undo log segments. In stepping through the list,
s-latches on the undo log pages are enough, but in a truncate, x-latches must
be obtained on the rollback segment and individual pages. */

/********************************************************************//**
Initializes the fields in an undo log segment page. */
static
void
trx_undo_page_init(
/*===============*/
	page_t* undo_page,	/*!< in: undo log segment page */
	ulint	type,		/*!< in: undo log segment type */
	mtr_t*	mtr);		/*!< in: mtr */

/********************************************************************//**
Creates and initializes an undo log memory object.
@return own: the undo log memory object */
static
trx_undo_t*
trx_undo_mem_create(
/*================*/
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory object */
	ulint		id,	/*!< in: slot index within rseg */
	ulint		type,	/*!< in: type of the log: TRX_UNDO_INSERT or
				TRX_UNDO_UPDATE */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open XA transaction identification*/
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset);/*!< in: undo log header byte offset on page */
/***************************************************************//**
Initializes a cached insert undo log header page for new use. NOTE that this
function has its own log record type MLOG_UNDO_HDR_REUSE. You must NOT change
the operation of this function!
@return undo log header byte offset on page */
static
ulint
trx_undo_insert_header_reuse(
/*=========================*/
	page_t*		undo_page,	/*!< in/out: insert undo log segment
					header page, x-latched */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr);		/*!< in: mtr */
/**********************************************************************//**
If an update undo log can be discarded immediately, this function frees the
space, resetting the page to the proper state for caching. */
static
void
trx_undo_discard_latest_update_undo(
/*================================*/
	page_t*	undo_page,	/*!< in: header page of an undo log of size 1 */
	mtr_t*	mtr);		/*!< in: mtr */

/***********************************************************************//**
Gets the previous record in an undo log from the previous page.
@return undo log record, the page s-latched, NULL if none */
static
trx_undo_rec_t*
trx_undo_get_prev_rec_from_prev_page(
/*=================================*/
	trx_undo_rec_t*	rec,	/*!< in: undo record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset,	/*!< in: undo log header offset on page */
	bool		shared,	/*!< in: true=S-latch, false=X-latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint	space;
	ulint	prev_page_no;
	page_t* prev_page;
	page_t*	undo_page;

	undo_page = page_align(rec);

	prev_page_no = flst_get_prev_addr(undo_page + TRX_UNDO_PAGE_HDR
					  + TRX_UNDO_PAGE_NODE, mtr)
		.page;

	if (prev_page_no == FIL_NULL) {

		return(NULL);
	}

	space = page_get_space_id(undo_page);

	buf_block_t*	block = buf_page_get(
		page_id_t(space, prev_page_no), univ_page_size,
		shared ? RW_S_LATCH : RW_X_LATCH, mtr);

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	prev_page = buf_block_get_frame(block);

	return(trx_undo_page_get_last_rec(prev_page, page_no, offset));
}

/***********************************************************************//**
Gets the previous record in an undo log.
@return undo log record, the page s-latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_prev_rec(
/*==================*/
	trx_undo_rec_t*	rec,	/*!< in: undo record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset,	/*!< in: undo log header offset on page */
	bool		shared,	/*!< in: true=S-latch, false=X-latch */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_undo_rec_t*	prev_rec;

	prev_rec = trx_undo_page_get_prev_rec(rec, page_no, offset);

	if (prev_rec) {

		return(prev_rec);
	}

	/* We have to go to the previous undo log page to look for the
	previous record */

	return(trx_undo_get_prev_rec_from_prev_page(rec, page_no, offset,
						    shared, mtr));
}

/** Gets the next record in an undo log from the next page.
@param[in]	space		undo log header space
@param[in]	undo_page	undo log page
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset on page
@param[in]	mode		latch mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]	mtr		mini-transaction
@return undo log record, the page latched, NULL if none */
static
trx_undo_rec_t*
trx_undo_get_next_rec_from_next_page(
	ulint			space,
	const page_t*		undo_page,
	ulint			page_no,
	ulint			offset,
	ulint			mode,
	mtr_t*			mtr)
{
	const trx_ulogf_t*	log_hdr;
	ulint			next_page_no;
	page_t*			next_page;
	ulint			next;

	if (page_no == page_get_page_no(undo_page)) {

		log_hdr = undo_page + offset;
		next = mach_read_from_2(log_hdr + TRX_UNDO_NEXT_LOG);

		if (next != 0) {

			return(NULL);
		}
	}

	next_page_no = flst_get_next_addr(undo_page + TRX_UNDO_PAGE_HDR
					  + TRX_UNDO_PAGE_NODE, mtr)
		.page;
	if (next_page_no == FIL_NULL) {

		return(NULL);
	}

	const page_id_t	next_page_id(space, next_page_no);

	if (mode == RW_S_LATCH) {
		next_page = trx_undo_page_get_s_latched(
			next_page_id, mtr);
	} else {
		ut_ad(mode == RW_X_LATCH);
		next_page = trx_undo_page_get(next_page_id, mtr);
	}

	return(trx_undo_page_get_first_rec(next_page, page_no, offset));
}

/***********************************************************************//**
Gets the next record in an undo log.
@return undo log record, the page s-latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_next_rec(
/*==================*/
	trx_undo_rec_t*	rec,	/*!< in: undo record */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset,	/*!< in: undo log header offset on page */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint		space;
	trx_undo_rec_t*	next_rec;

	next_rec = trx_undo_page_get_next_rec(rec, page_no, offset);

	if (next_rec) {
		return(next_rec);
	}

	space = page_get_space_id(page_align(rec));

	return(trx_undo_get_next_rec_from_next_page(space,
						    page_align(rec),
						    page_no, offset,
						    RW_S_LATCH, mtr));
}

/** Gets the first record in an undo log.
@param[in]	space		undo log header space
@param[in]	page_no		undo log header page number
@param[in]	offset		undo log header offset on page
@param[in]	mode		latching mode: RW_S_LATCH or RW_X_LATCH
@param[in,out]	mtr		mini-transaction
@return undo log record, the page latched, NULL if none */
trx_undo_rec_t*
trx_undo_get_first_rec(
	ulint			space,
	ulint			page_no,
	ulint			offset,
	ulint			mode,
	mtr_t*			mtr)
{
	page_t*		undo_page;
	trx_undo_rec_t*	rec;

	const page_id_t	page_id(space, page_no);

	if (mode == RW_S_LATCH) {
		undo_page = trx_undo_page_get_s_latched(page_id, mtr);
	} else {
		undo_page = trx_undo_page_get(page_id, mtr);
	}

	rec = trx_undo_page_get_first_rec(undo_page, page_no, offset);

	if (rec) {
		return(rec);
	}

	return(trx_undo_get_next_rec_from_next_page(space,
						    undo_page, page_no, offset,
						    mode, mtr));
}

/*============== UNDO LOG FILE COPY CREATION AND FREEING ==================*/

/**********************************************************************//**
Writes the mtr log entry of an undo log page initialization. */
UNIV_INLINE
void
trx_undo_page_init_log(
/*===================*/
	page_t* undo_page,	/*!< in: undo log page */
	ulint	type,		/*!< in: undo log type */
	mtr_t*	mtr)		/*!< in: mtr */
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_INIT, mtr);

	mlog_catenate_ulint_compressed(mtr, type);
}

/***********************************************************//**
Parses the redo log entry of an undo log page initialization.
@return end of log record or NULL */
byte*
trx_undo_parse_page_init(
/*=====================*/
	const byte*	ptr,	/*!< in: buffer */
	const byte*	end_ptr,/*!< in: buffer end */
	page_t*		page,	/*!< in: page or NULL */
	mtr_t*		mtr)	/*!< in: mtr or NULL */
{
	ulint	type;

	type = mach_parse_compressed(&ptr, end_ptr);

	if (ptr == NULL) {

		return(NULL);
	}

	if (page) {
		trx_undo_page_init(page, type, mtr);
	}

	return(const_cast<byte*>(ptr));
}

/********************************************************************//**
Initializes the fields in an undo log segment page. */
static
void
trx_undo_page_init(
/*===============*/
	page_t* undo_page,	/*!< in: undo log segment page */
	ulint	type,		/*!< in: undo log segment type */
	mtr_t*	mtr)		/*!< in: mtr */
{
	trx_upagef_t*	page_hdr;

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_TYPE, type);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START,
			TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE,
			TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);

	fil_page_set_type(undo_page, FIL_PAGE_UNDO_LOG);

	trx_undo_page_init_log(undo_page, type, mtr);
}

/***************************************************************//**
Creates a new undo log segment in file.
@return DB_SUCCESS if page creation OK possible error codes are:
DB_TOO_MANY_CONCURRENT_TRXS DB_OUT_OF_FILE_SPACE */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
trx_undo_seg_create(
/*================*/
	trx_rseg_t*	rseg MY_ATTRIBUTE((unused)),/*!< in: rollback segment */
	trx_rsegf_t*	rseg_hdr,/*!< in: rollback segment header, page
				x-latched */
	ulint		type,	/*!< in: type of the segment: TRX_UNDO_INSERT or
				TRX_UNDO_UPDATE */
	ulint*		id,	/*!< out: slot index within rseg header */
	page_t**	undo_page,
				/*!< out: segment header page x-latched, NULL
				if there was an error */
	mtr_t*		mtr)	/*!< in: mtr */
{
	ulint		slot_no;
	ulint		space;
	buf_block_t*	block;
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	ulint		n_reserved;
	bool		success;
	dberr_t		err = DB_SUCCESS;

	ut_ad(mtr != NULL);
	ut_ad(id != NULL);
	ut_ad(rseg_hdr != NULL);
	ut_ad(mutex_own(&(rseg->mutex)));

	/*	fputs(type == TRX_UNDO_INSERT
	? "Creating insert undo log segment\n"
	: "Creating update undo log segment\n", stderr); */
	slot_no = trx_rsegf_undo_find_free(rseg_hdr, mtr);

	if (slot_no == ULINT_UNDEFINED) {
		ib::warn() << "Cannot find a free slot for an undo log. Do"
			" you have too many active transactions running"
			" concurrently?";

		return(DB_TOO_MANY_CONCURRENT_TRXS);
	}

	space = page_get_space_id(page_align(rseg_hdr));

	success = fsp_reserve_free_extents(&n_reserved, space, 2, FSP_UNDO,
					   mtr);
	if (!success) {

		return(DB_OUT_OF_FILE_SPACE);
	}

	/* Allocate a new file segment for the undo log */
	block = fseg_create_general(space, 0,
				    TRX_UNDO_SEG_HDR
				    + TRX_UNDO_FSEG_HEADER, TRUE, mtr);

	fil_space_release_free_extents(space, n_reserved);

	if (block == NULL) {
		/* No space left */

		return(DB_OUT_OF_FILE_SPACE);
	}

	buf_block_dbg_add_level(block, SYNC_TRX_UNDO_PAGE);

	*undo_page = buf_block_get_frame(block);

	page_hdr = *undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = *undo_page + TRX_UNDO_SEG_HDR;

	trx_undo_page_init(*undo_page, type, mtr);

	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE,
			 TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(seg_hdr + TRX_UNDO_LAST_LOG, 0, MLOG_2BYTES, mtr);

	flst_init(seg_hdr + TRX_UNDO_PAGE_LIST, mtr);

	flst_add_last(seg_hdr + TRX_UNDO_PAGE_LIST,
		      page_hdr + TRX_UNDO_PAGE_NODE, mtr);

	trx_rsegf_set_nth_undo(rseg_hdr, slot_no,
			       page_get_page_no(*undo_page), mtr);
	*id = slot_no;

	MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);

	return(err);
}

/**********************************************************************//**
Writes the mtr log entry of an undo log header initialization. */
UNIV_INLINE
void
trx_undo_header_create_log(
/*=======================*/
	const page_t*	undo_page,	/*!< in: undo log header page */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr)		/*!< in: mtr */
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_CREATE, mtr);

	mlog_catenate_ull_compressed(mtr, trx_id);
}

/***************************************************************//**
Creates a new undo log header in file. NOTE that this function has its own
log record type MLOG_UNDO_HDR_CREATE. You must NOT change the operation of
this function!
@return header byte offset on page */
static
ulint
trx_undo_header_create(
/*===================*/
	page_t*		undo_page,	/*!< in/out: undo log segment
					header page, x-latched; it is
					assumed that there is
					TRX_UNDO_LOG_XA_HDR_SIZE bytes
					free space on it */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr)		/*!< in: mtr */
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	ulint		prev_log;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);

	log_hdr = undo_page + free;

	new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

	ut_a(free + TRX_UNDO_LOG_XA_HDR_SIZE < UNIV_PAGE_SIZE - 100);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);

	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	prev_log = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);

	if (prev_log != 0) {
		trx_ulogf_t*	prev_log_hdr;

		prev_log_hdr = undo_page + prev_log;

		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, free);
	}

	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, free);

	log_hdr = undo_page + free;

	mach_write_to_2(log_hdr + TRX_UNDO_DEL_MARKS, TRUE);

	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

	mach_write_to_1(log_hdr + TRX_UNDO_XID_EXISTS, FALSE);
	mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

	mach_write_to_2(log_hdr + TRX_UNDO_NEXT_LOG, 0);
	mach_write_to_2(log_hdr + TRX_UNDO_PREV_LOG, prev_log);

	/* Write the log record about the header creation */
	trx_undo_header_create_log(undo_page, trx_id, mtr);

	return(free);
}

/********************************************************************//**
Write X/Open XA Transaction Identification (XID) to undo log header */
static
void
trx_undo_write_xid(
/*===============*/
	trx_ulogf_t*	log_hdr,/*!< in: undo log header */
	const XID*	xid,	/*!< in: X/Open XA Transaction Identification */
	mtr_t*		mtr)	/*!< in: mtr */
{
	mlog_write_ulint(log_hdr + TRX_UNDO_XA_FORMAT,
			 static_cast<ulint>(xid->formatID),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(log_hdr + TRX_UNDO_XA_TRID_LEN,
			 static_cast<ulint>(xid->gtrid_length),
			 MLOG_4BYTES, mtr);

	mlog_write_ulint(log_hdr + TRX_UNDO_XA_BQUAL_LEN,
			 static_cast<ulint>(xid->bqual_length),
			 MLOG_4BYTES, mtr);

	mlog_write_string(log_hdr + TRX_UNDO_XA_XID,
			  reinterpret_cast<const byte*>(xid->data),
			  XIDDATASIZE, mtr);
}

/********************************************************************//**
Read X/Open XA Transaction Identification (XID) from undo log header */
static
void
trx_undo_read_xid(
/*==============*/
	trx_ulogf_t*	log_hdr,/*!< in: undo log header */
	XID*		xid)	/*!< out: X/Open XA Transaction Identification */
{
	xid->formatID=static_cast<long>(mach_read_from_4(
		log_hdr + TRX_UNDO_XA_FORMAT));

	xid->gtrid_length=static_cast<long>(mach_read_from_4(
		log_hdr + TRX_UNDO_XA_TRID_LEN));

	xid->bqual_length=static_cast<long>(mach_read_from_4(
		log_hdr + TRX_UNDO_XA_BQUAL_LEN));

	memcpy(xid->data, log_hdr + TRX_UNDO_XA_XID, XIDDATASIZE);
}

/***************************************************************//**
Adds space for the XA XID after an undo log old-style header. */
static
void
trx_undo_header_add_space_for_xid(
/*==============================*/
	page_t*		undo_page,/*!< in: undo log segment header page */
	trx_ulogf_t*	log_hdr,/*!< in: undo log header */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_upagef_t*	page_hdr;
	ulint		free;
	ulint		new_free;

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	free = mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE);

	/* free is now the end offset of the old style undo log header */

	ut_a(free == (ulint)(log_hdr - undo_page) + TRX_UNDO_LOG_OLD_HDR_SIZE);

	new_free = free + (TRX_UNDO_LOG_XA_HDR_SIZE
			   - TRX_UNDO_LOG_OLD_HDR_SIZE);

	/* Add space for a XID after the header, update the free offset
	fields on the undo log page and in the undo log header */

	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_START, new_free,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(page_hdr + TRX_UNDO_PAGE_FREE, new_free,
			 MLOG_2BYTES, mtr);

	mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, new_free,
			 MLOG_2BYTES, mtr);
}

/**********************************************************************//**
Writes the mtr log entry of an undo log header reuse. */
UNIV_INLINE
void
trx_undo_insert_header_reuse_log(
/*=============================*/
	const page_t*	undo_page,	/*!< in: undo log header page */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr)		/*!< in: mtr */
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_REUSE, mtr);

	mlog_catenate_ull_compressed(mtr, trx_id);
}

/** Parse the redo log entry of an undo log page header create or reuse.
@param[in]	type	MLOG_UNDO_HDR_CREATE or MLOG_UNDO_HDR_REUSE
@param[in]	ptr	redo log record
@param[in]	end_ptr	end of log buffer
@param[in,out]	page	page frame or NULL
@param[in,out]	mtr	mini-transaction or NULL
@return end of log record or NULL */
byte*
trx_undo_parse_page_header(
	mlog_id_t	type,
	const byte*	ptr,
	const byte*	end_ptr,
	page_t*		page,
	mtr_t*		mtr)
{
	trx_id_t	trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

	if (ptr != NULL && page != NULL) {
		switch (type) {
		case MLOG_UNDO_HDR_CREATE:
			trx_undo_header_create(page, trx_id, mtr);
			return(const_cast<byte*>(ptr));
		case MLOG_UNDO_HDR_REUSE:
			trx_undo_insert_header_reuse(page, trx_id, mtr);
			return(const_cast<byte*>(ptr));
		default:
			break;
		}
		ut_ad(0);
	}

	return(const_cast<byte*>(ptr));
}

/***************************************************************//**
Initializes a cached insert undo log header page for new use. NOTE that this
function has its own log record type MLOG_UNDO_HDR_REUSE. You must NOT change
the operation of this function!
@return undo log header byte offset on page */
static
ulint
trx_undo_insert_header_reuse(
/*=========================*/
	page_t*		undo_page,	/*!< in/out: insert undo log segment
					header page, x-latched */
	trx_id_t	trx_id,		/*!< in: transaction id */
	mtr_t*		mtr)		/*!< in: mtr */
{
	trx_upagef_t*	page_hdr;
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	log_hdr;
	ulint		free;
	ulint		new_free;

	ut_ad(mtr && undo_page);

	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;
	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	free = TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE;

	ut_a(free + TRX_UNDO_LOG_XA_HDR_SIZE < UNIV_PAGE_SIZE - 100);

	log_hdr = undo_page + free;

	new_free = free + TRX_UNDO_LOG_OLD_HDR_SIZE;

	/* Insert undo data is not needed after commit: we may free all
	the space on the page */

	ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
			      + TRX_UNDO_PAGE_TYPE)
	     == TRX_UNDO_INSERT);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START, new_free);

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, new_free);

	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE);

	log_hdr = undo_page + free;

	mach_write_to_8(log_hdr + TRX_UNDO_TRX_ID, trx_id);
	mach_write_to_2(log_hdr + TRX_UNDO_LOG_START, new_free);

	mach_write_to_1(log_hdr + TRX_UNDO_XID_EXISTS, FALSE);
	mach_write_to_1(log_hdr + TRX_UNDO_DICT_TRANS, FALSE);

	/* Write the log record MLOG_UNDO_HDR_REUSE */
	trx_undo_insert_header_reuse_log(undo_page, trx_id, mtr);

	return(free);
}

/**********************************************************************//**
Writes the redo log entry of an update undo log header discard. */
UNIV_INLINE
void
trx_undo_discard_latest_log(
/*========================*/
	page_t* undo_page,	/*!< in: undo log header page */
	mtr_t*	mtr)		/*!< in: mtr */
{
	mlog_write_initial_log_record(undo_page, MLOG_UNDO_HDR_DISCARD, mtr);
}

/***********************************************************//**
Parses the redo log entry of an undo log page header discard.
@return end of log record or NULL */
byte*
trx_undo_parse_discard_latest(
/*==========================*/
	byte*	ptr,	/*!< in: buffer */
	byte*	end_ptr MY_ATTRIBUTE((unused)), /*!< in: buffer end */
	page_t*	page,	/*!< in: page or NULL */
	mtr_t*	mtr)	/*!< in: mtr or NULL */
{
	ut_ad(end_ptr);

	if (page) {
		trx_undo_discard_latest_update_undo(page, mtr);
	}

	return(ptr);
}

/**********************************************************************//**
If an update undo log can be discarded immediately, this function frees the
space, resetting the page to the proper state for caching. */
static
void
trx_undo_discard_latest_update_undo(
/*================================*/
	page_t*	undo_page,	/*!< in: header page of an undo log of size 1 */
	mtr_t*	mtr)		/*!< in: mtr */
{
	trx_usegf_t*	seg_hdr;
	trx_upagef_t*	page_hdr;
	trx_ulogf_t*	log_hdr;
	trx_ulogf_t*	prev_log_hdr;
	ulint		free;
	ulint		prev_hdr_offset;

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	free = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	log_hdr = undo_page + free;

	prev_hdr_offset = mach_read_from_2(log_hdr + TRX_UNDO_PREV_LOG);

	if (prev_hdr_offset != 0) {
		prev_log_hdr = undo_page + prev_hdr_offset;

		mach_write_to_2(page_hdr + TRX_UNDO_PAGE_START,
				mach_read_from_2(prev_log_hdr
						 + TRX_UNDO_LOG_START));
		mach_write_to_2(prev_log_hdr + TRX_UNDO_NEXT_LOG, 0);
	}

	mach_write_to_2(page_hdr + TRX_UNDO_PAGE_FREE, free);

	mach_write_to_2(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_CACHED);
	mach_write_to_2(seg_hdr + TRX_UNDO_LAST_LOG, prev_hdr_offset);

	trx_undo_discard_latest_log(undo_page, mtr);
}

/** Allocate an undo log page.
@param[in,out]	trx	transaction
@param[in,out]	undo	undo log
@param[in,out]	mtr	mini-transaction that does not hold any page latch
@return	X-latched block if success
@retval	NULL	on failure */
buf_block_t*
trx_undo_add_page(trx_t* trx, trx_undo_t* undo, mtr_t* mtr)
{
	ut_ad(mutex_own(&trx->undo_mutex));

	trx_rseg_t*	rseg		= undo->rseg;
	buf_block_t*	new_block	= NULL;
	ulint		n_reserved;
	page_t*		header_page;

	/* When we add a page to an undo log, this is analogous to
	a pessimistic insert in a B-tree, and we must reserve the
	counterpart of the tree latch, which is the rseg mutex. */

	mutex_enter(&rseg->mutex);
	if (rseg->curr_size == rseg->max_size) {
		goto func_exit;
	}

	header_page = trx_undo_page_get(
		page_id_t(undo->space, undo->hdr_page_no), mtr);

	if (!fsp_reserve_free_extents(&n_reserved, undo->space, 1,
				      FSP_UNDO, mtr)) {
		goto func_exit;
	}

	new_block = fseg_alloc_free_page_general(
		TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER
		+ header_page,
		undo->top_page_no + 1, FSP_UP, TRUE, mtr, mtr);

	fil_space_release_free_extents(undo->space, n_reserved);

	if (!new_block) {
		goto func_exit;
	}

	ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
	buf_block_dbg_add_level(new_block, SYNC_TRX_UNDO_PAGE);
	undo->last_page_no = new_block->page.id.page_no();

	trx_undo_page_init(new_block->frame, undo->type, mtr);

	flst_add_last(TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST
		      + header_page,
		      TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE
		      + new_block->frame,
		      mtr);
	undo->size++;
	rseg->curr_size++;

func_exit:
	mutex_exit(&rseg->mutex);
	return(new_block);
}

/********************************************************************//**
Frees an undo log page that is not the header page.
@return last page number in remaining log */
static
ulint
trx_undo_free_page(
/*===============*/
	trx_rseg_t* rseg,	/*!< in: rollback segment */
	ibool	in_history,	/*!< in: TRUE if the undo log is in the history
				list */
	ulint	space,		/*!< in: space */
	ulint	hdr_page_no,	/*!< in: header page number */
	ulint	page_no,	/*!< in: page number to free: must not be the
				header page */
	mtr_t*	mtr)		/*!< in: mtr which does not have a latch to any
				undo log page; the caller must have reserved
				the rollback segment mutex */
{
	page_t*		header_page;
	page_t*		undo_page;
	fil_addr_t	last_addr;
	trx_rsegf_t*	rseg_header;
	ulint		hist_size;

	ut_a(hdr_page_no != page_no);
	ut_ad(mutex_own(&(rseg->mutex)));

	undo_page = trx_undo_page_get(page_id_t(space, page_no), mtr);

	header_page = trx_undo_page_get(page_id_t(space, hdr_page_no), mtr);

	flst_remove(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_PAGE_LIST,
		    undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE, mtr);

	fseg_free_page(header_page + TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
		       space, page_no, false, mtr);

	last_addr = flst_get_last(header_page + TRX_UNDO_SEG_HDR
				  + TRX_UNDO_PAGE_LIST, mtr);
	rseg->curr_size--;

	if (in_history) {
		rseg_header = trx_rsegf_get(space, rseg->page_no, mtr);

		hist_size = mtr_read_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE,
					   MLOG_4BYTES, mtr);
		ut_ad(hist_size > 0);
		mlog_write_ulint(rseg_header + TRX_RSEG_HISTORY_SIZE,
				 hist_size - 1, MLOG_4BYTES, mtr);
	}

	return(last_addr.page);
}

/** Free the last undo log page. The caller must hold the rseg mutex.
@param[in,out]	undo	undo log
@param[in,out]	mtr	mini-transaction that does not hold any undo log page
			or that has allocated the undo log page */
void
trx_undo_free_last_page(trx_undo_t* undo, mtr_t* mtr)
{
	ut_ad(undo->hdr_page_no != undo->last_page_no);
	ut_ad(undo->size > 0);

	undo->last_page_no = trx_undo_free_page(
		undo->rseg, FALSE, undo->space,
		undo->hdr_page_no, undo->last_page_no, mtr);

	undo->size--;
}

/** Empties an undo log header page of undo records for that undo log.
Other undo logs may still have records on that page, if it is an update
undo log.
@param[in]	space		space
@param[in]	hdr_page_no	header page number
@param[in]	hdr_offset	header offset
@param[in,out]	mtr		mini-transaction */
static
void
trx_undo_empty_header_page(
	ulint			space,
	ulint			hdr_page_no,
	ulint			hdr_offset,
	mtr_t*			mtr)
{
	page_t*		header_page;
	trx_ulogf_t*	log_hdr;
	ulint		end;

	header_page = trx_undo_page_get(page_id_t(space, hdr_page_no), mtr);

	log_hdr = header_page + hdr_offset;

	end = trx_undo_page_get_end(header_page, hdr_page_no, hdr_offset);

	mlog_write_ulint(log_hdr + TRX_UNDO_LOG_START, end, MLOG_2BYTES, mtr);
}

/** Truncate the tail of an undo log during rollback.
@param[in,out]	undo	undo log
@param[in]	limit	all undo logs after this limit will be discarded
@param[in]	is_temp	whether this is temporary undo log */
void
trx_undo_truncate_end(trx_undo_t* undo, undo_no_t limit, bool is_temp)
{
	ut_ad(mutex_own(&undo->rseg->mutex));
	ut_ad(is_temp == !undo->rseg->is_persistent());

	for (;;) {
		mtr_t		mtr;
		mtr.start();
		if (is_temp) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		}

		trx_undo_rec_t* trunc_here = NULL;
		page_t*		undo_page = trx_undo_page_get(
			page_id_t(undo->space, undo->last_page_no), &mtr);
		trx_undo_rec_t* rec = trx_undo_page_get_last_rec(
			undo_page, undo->hdr_page_no, undo->hdr_offset);
		while (rec) {
			if (trx_undo_rec_get_undo_no(rec) >= limit) {
				/* Truncate at least this record off, maybe
				more */
				trunc_here = rec;
			} else {
				goto function_exit;
			}

			rec = trx_undo_page_get_prev_rec(rec,
							 undo->hdr_page_no,
							 undo->hdr_offset);
		}

		if (undo->last_page_no == undo->hdr_page_no) {
function_exit:
			if (trunc_here) {
				mlog_write_ulint(undo_page + TRX_UNDO_PAGE_HDR
						 + TRX_UNDO_PAGE_FREE,
						 trunc_here - undo_page,
						 MLOG_2BYTES, &mtr);
			}

			mtr.commit();
			return;
		}

		trx_undo_free_last_page(undo, &mtr);
		mtr.commit();
	}
}

/** Truncate the head of an undo log.
NOTE that only whole pages are freed; the header page is not
freed, but emptied, if all the records there are below the limit.
@param[in,out]	rseg		rollback segment
@param[in]	hdr_page_no	header page number
@param[in]	hdr_offset	header offset on the page
@param[in]	limit		first undo number to preserve
(everything below the limit will be truncated) */
void
trx_undo_truncate_start(
	trx_rseg_t*	rseg,
	ulint		hdr_page_no,
	ulint		hdr_offset,
	undo_no_t	limit)
{
	page_t*		undo_page;
	trx_undo_rec_t* rec;
	trx_undo_rec_t* last_rec;
	ulint		page_no;
	mtr_t		mtr;

	ut_ad(mutex_own(&(rseg->mutex)));

	if (!limit) {
		return;
	}
loop:
	mtr_start(&mtr);

	if (!rseg->is_persistent()) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
	}

	rec = trx_undo_get_first_rec(rseg->space, hdr_page_no, hdr_offset,
				     RW_X_LATCH, &mtr);
	if (rec == NULL) {
		/* Already empty */

		mtr_commit(&mtr);

		return;
	}

	undo_page = page_align(rec);

	last_rec = trx_undo_page_get_last_rec(undo_page, hdr_page_no,
					      hdr_offset);
	if (trx_undo_rec_get_undo_no(last_rec) >= limit) {

		mtr_commit(&mtr);

		return;
	}

	page_no = page_get_page_no(undo_page);

	if (page_no == hdr_page_no) {
		trx_undo_empty_header_page(rseg->space,
					   hdr_page_no, hdr_offset,
					   &mtr);
	} else {
		trx_undo_free_page(rseg, TRUE, rseg->space, hdr_page_no,
				   page_no, &mtr);
	}

	mtr_commit(&mtr);

	goto loop;
}

/** Frees an undo log segment which is not in the history list.
@param[in]	undo	undo log
@param[in]	noredo	whether the undo tablespace is redo logged */
static
void
trx_undo_seg_free(
	const trx_undo_t*	undo,
	bool			noredo)
{
	trx_rseg_t*	rseg;
	fseg_header_t*	file_seg;
	trx_rsegf_t*	rseg_header;
	trx_usegf_t*	seg_header;
	ibool		finished;
	mtr_t		mtr;

	rseg = undo->rseg;

	do {

		mtr_start(&mtr);

		if (noredo) {
			mtr.set_log_mode(MTR_LOG_NO_REDO);
		}

		mutex_enter(&(rseg->mutex));

		seg_header = trx_undo_page_get(page_id_t(undo->space,
							 undo->hdr_page_no),
					       &mtr)
			+ TRX_UNDO_SEG_HDR;

		file_seg = seg_header + TRX_UNDO_FSEG_HEADER;

		finished = fseg_free_step(file_seg, false, &mtr);

		if (finished) {
			/* Update the rseg header */
			rseg_header = trx_rsegf_get(
				rseg->space, rseg->page_no, &mtr);
			trx_rsegf_set_nth_undo(rseg_header, undo->id, FIL_NULL,
					       &mtr);

			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_USED);
		}

		mutex_exit(&(rseg->mutex));
		mtr_commit(&mtr);
	} while (!finished);
}

/*========== UNDO LOG MEMORY COPY INITIALIZATION =====================*/

/********************************************************************//**
Creates and initializes an undo log memory object according to the values
in the header in file, when the database is started. The memory object is
inserted in the appropriate list of rseg.
@return own: the undo log memory object */
static
trx_undo_t*
trx_undo_mem_create_at_db_start(
/*============================*/
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory object */
	ulint		id,	/*!< in: slot index within rseg */
	ulint		page_no,/*!< in: undo log segment page number */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_t*		undo_page;
	trx_upagef_t*	page_header;
	trx_usegf_t*	seg_header;
	trx_ulogf_t*	undo_header;
	trx_undo_t*	undo;
	ulint		type;
	ulint		state;
	trx_id_t	trx_id;
	ulint		offset;
	fil_addr_t	last_addr;
	page_t*		last_page;
	trx_undo_rec_t*	rec;
	XID		xid;
	ibool		xid_exists = FALSE;

	ut_a(id < TRX_RSEG_N_SLOTS);

	undo_page = trx_undo_page_get(page_id_t(rseg->space, page_no), mtr);

	page_header = undo_page + TRX_UNDO_PAGE_HDR;

	type = mtr_read_ulint(page_header + TRX_UNDO_PAGE_TYPE, MLOG_2BYTES,
			      mtr);
	seg_header = undo_page + TRX_UNDO_SEG_HDR;

	state = mach_read_from_2(seg_header + TRX_UNDO_STATE);

	offset = mach_read_from_2(seg_header + TRX_UNDO_LAST_LOG);

	undo_header = undo_page + offset;

	trx_id = mach_read_from_8(undo_header + TRX_UNDO_TRX_ID);

	xid_exists = mtr_read_ulint(undo_header + TRX_UNDO_XID_EXISTS,
				    MLOG_1BYTE, mtr);

	/* Read X/Open XA transaction identification if it exists, or
	set it to NULL. */
	xid.null();

	if (xid_exists == TRUE) {
		trx_undo_read_xid(undo_header, &xid);
	}

	mutex_enter(&(rseg->mutex));

	undo = trx_undo_mem_create(rseg, id, type, trx_id, &xid,
				   page_no, offset);
	mutex_exit(&(rseg->mutex));

	undo->dict_operation =	mtr_read_ulint(
		undo_header + TRX_UNDO_DICT_TRANS, MLOG_1BYTE, mtr);

	undo->table_id = mach_read_from_8(undo_header + TRX_UNDO_TABLE_ID);
	undo->state = state;
	undo->size = flst_get_len(seg_header + TRX_UNDO_PAGE_LIST);

	/* If the log segment is being freed, the page list is inconsistent! */
	if (state == TRX_UNDO_TO_FREE) {

		goto add_to_list;
	}

	last_addr = flst_get_last(seg_header + TRX_UNDO_PAGE_LIST, mtr);

	undo->last_page_no = last_addr.page;
	undo->top_page_no = last_addr.page;

	last_page = trx_undo_page_get(
		page_id_t(rseg->space, undo->last_page_no), mtr);

	rec = trx_undo_page_get_last_rec(last_page, page_no, offset);

	if (rec == NULL) {
		undo->empty = TRUE;
	} else {
		undo->empty = FALSE;
		undo->top_offset = rec - last_page;
		undo->top_undo_no = trx_undo_rec_get_undo_no(rec);
	}
add_to_list:
	if (type == TRX_UNDO_INSERT) {
		if (state != TRX_UNDO_CACHED) {

			UT_LIST_ADD_LAST(rseg->insert_undo_list, undo);
		} else {

			UT_LIST_ADD_LAST(rseg->insert_undo_cached, undo);

			MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
		}
	} else {
		ut_ad(type == TRX_UNDO_UPDATE);
		if (state != TRX_UNDO_CACHED) {

			UT_LIST_ADD_LAST(rseg->update_undo_list, undo);
		} else {

			UT_LIST_ADD_LAST(rseg->update_undo_cached, undo);

			MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
		}
	}

	return(undo);
}

/********************************************************************//**
Initializes the undo log lists for a rollback segment memory copy. This
function is only called when the database is started or a new rollback
segment is created.
@return the combined size of undo log segments in pages */
ulint
trx_undo_lists_init(
/*================*/
	trx_rseg_t*	rseg)	/*!< in: rollback segment memory object */
{
	ulint		size	= 0;
	trx_rsegf_t*	rseg_header;
	ulint		i;
	mtr_t		mtr;

	mtr_start(&mtr);

	rseg_header = trx_rsegf_get_new(rseg->space, rseg->page_no, &mtr);

	for (i = 0; i < TRX_RSEG_N_SLOTS; i++) {
		ulint	page_no;

		page_no = trx_rsegf_get_nth_undo(rseg_header, i, &mtr);

		/* In forced recovery: try to avoid operations which look
		at database pages; undo logs are rapidly changing data, and
		the probability that they are in an inconsistent state is
		high */

		if (page_no != FIL_NULL
		    && srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {

			trx_undo_t*	undo;

			undo = trx_undo_mem_create_at_db_start(
				rseg, i, page_no, &mtr);

			size += undo->size;

			mtr_commit(&mtr);

			mtr_start(&mtr);

			rseg_header = trx_rsegf_get(
				rseg->space, rseg->page_no, &mtr);

			/* Found a used slot */
			MONITOR_INC(MONITOR_NUM_UNDO_SLOT_USED);
		}
	}

	mtr_commit(&mtr);

	return(size);
}

/********************************************************************//**
Creates and initializes an undo log memory object.
@return own: the undo log memory object */
static
trx_undo_t*
trx_undo_mem_create(
/*================*/
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory object */
	ulint		id,	/*!< in: slot index within rseg */
	ulint		type,	/*!< in: type of the log: TRX_UNDO_INSERT or
				TRX_UNDO_UPDATE */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open transaction identification */
	ulint		page_no,/*!< in: undo log header page number */
	ulint		offset)	/*!< in: undo log header byte offset on page */
{
	trx_undo_t*	undo;

	ut_ad(mutex_own(&(rseg->mutex)));

	ut_a(id < TRX_RSEG_N_SLOTS);

	undo = static_cast<trx_undo_t*>(ut_malloc_nokey(sizeof(*undo)));

	if (undo == NULL) {

		return(NULL);
	}

	undo->id = id;
	undo->type = type;
	undo->state = TRX_UNDO_ACTIVE;
	undo->del_marks = FALSE;
	undo->trx_id = trx_id;
	undo->xid = *xid;

	undo->dict_operation = FALSE;

	undo->rseg = rseg;

	undo->space = rseg->space;
	undo->hdr_page_no = page_no;
	undo->hdr_offset = offset;
	undo->last_page_no = page_no;
	undo->size = 1;

	undo->empty = TRUE;
	undo->top_page_no = page_no;
	undo->guess_block = NULL;
	undo->withdraw_clock = 0;

	return(undo);
}

/********************************************************************//**
Initializes a cached undo log object for new use. */
static
void
trx_undo_mem_init_for_reuse(
/*========================*/
	trx_undo_t*	undo,	/*!< in: undo log to init */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open XA transaction identification*/
	ulint		offset)	/*!< in: undo log header byte offset on page */
{
	ut_ad(mutex_own(&((undo->rseg)->mutex)));

	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo->state = TRX_UNDO_ACTIVE;
	undo->del_marks = FALSE;
	undo->trx_id = trx_id;
	undo->xid = *xid;

	undo->dict_operation = FALSE;

	undo->hdr_offset = offset;
	undo->empty = TRUE;
}

/********************************************************************//**
Frees an undo log memory copy. */
void
trx_undo_mem_free(
/*==============*/
	trx_undo_t*	undo)	/*!< in: the undo object to be freed */
{
	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	ut_free(undo);
}

/**********************************************************************//**
Creates a new undo log.
@return DB_SUCCESS if successful in creating the new undo lob object,
possible error codes are: DB_TOO_MANY_CONCURRENT_TRXS
DB_OUT_OF_FILE_SPACE DB_OUT_OF_MEMORY */
static MY_ATTRIBUTE((nonnull, warn_unused_result))
dberr_t
trx_undo_create(
/*============*/
	trx_t*		trx,	/*!< in: transaction */
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory copy */
	ulint		type,	/*!< in: type of the log: TRX_UNDO_INSERT or
				TRX_UNDO_UPDATE */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is created */
	const XID*	xid,	/*!< in: X/Open transaction identification*/
	trx_undo_t**	undo,	/*!< out: the new undo log object, undefined
				 * if did not succeed */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_rsegf_t*	rseg_header;
	ulint		page_no;
	ulint		offset;
	ulint		id;
	page_t*		undo_page;
	dberr_t		err;

	ut_ad(mutex_own(&(rseg->mutex)));

	if (rseg->curr_size == rseg->max_size) {

		return(DB_OUT_OF_FILE_SPACE);
	}

	rseg->curr_size++;

	rseg_header = trx_rsegf_get(rseg->space, rseg->page_no, mtr);

	err = trx_undo_seg_create(rseg, rseg_header, type, &id,
				  &undo_page, mtr);

	if (err != DB_SUCCESS) {
		/* Did not succeed */

		rseg->curr_size--;

		return(err);
	}

	page_no = page_get_page_no(undo_page);

	offset = trx_undo_header_create(undo_page, trx_id, mtr);

	trx_undo_header_add_space_for_xid(undo_page, undo_page + offset, mtr);

	*undo = trx_undo_mem_create(rseg, id, type, trx_id, xid,
				   page_no, offset);
	if (*undo == NULL) {

		err = DB_OUT_OF_MEMORY;
	}

	return(err);
}

/*================ UNDO LOG ASSIGNMENT AND CLEANUP =====================*/

/********************************************************************//**
Reuses a cached undo log.
@return the undo log memory object, NULL if none cached */
static
trx_undo_t*
trx_undo_reuse_cached(
/*==================*/
	trx_t*		trx,	/*!< in: transaction */
	trx_rseg_t*	rseg,	/*!< in: rollback segment memory object */
	ulint		type,	/*!< in: type of the log: TRX_UNDO_INSERT or
				TRX_UNDO_UPDATE */
	trx_id_t	trx_id,	/*!< in: id of the trx for which the undo log
				is used */
	const XID*	xid,	/*!< in: X/Open XA transaction identification */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_undo_t*	undo;
	page_t*		undo_page;
	ulint		offset;

	ut_ad(mutex_own(&(rseg->mutex)));

	if (type == TRX_UNDO_INSERT) {

		undo = UT_LIST_GET_FIRST(rseg->insert_undo_cached);
		if (undo == NULL) {

			return(NULL);
		}

		UT_LIST_REMOVE(rseg->insert_undo_cached, undo);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(type == TRX_UNDO_UPDATE);

		undo = UT_LIST_GET_FIRST(rseg->update_undo_cached);
		if (undo == NULL) {

			return(NULL);
		}

		UT_LIST_REMOVE(rseg->update_undo_cached, undo);

		MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
	}

	ut_ad(undo->size == 1);
	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo_page = trx_undo_page_get(
		page_id_t(undo->space, undo->hdr_page_no), mtr);

	if (type == TRX_UNDO_INSERT) {
		offset = trx_undo_insert_header_reuse(undo_page, trx_id, mtr);

		trx_undo_header_add_space_for_xid(
			undo_page, undo_page + offset, mtr);
	} else {
		ut_a(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
				      + TRX_UNDO_PAGE_TYPE)
		     == TRX_UNDO_UPDATE);

		offset = trx_undo_header_create(undo_page, trx_id, mtr);

		trx_undo_header_add_space_for_xid(
			undo_page, undo_page + offset, mtr);
	}

	trx_undo_mem_init_for_reuse(undo, trx_id, xid, offset);

	return(undo);
}

/**********************************************************************//**
Marks an undo log header as a header of a data dictionary operation
transaction. */
static
void
trx_undo_mark_as_dict_operation(
/*============================*/
	trx_t*		trx,	/*!< in: dict op transaction */
	trx_undo_t*	undo,	/*!< in: assigned undo log */
	mtr_t*		mtr)	/*!< in: mtr */
{
	page_t*	hdr_page;

	hdr_page = trx_undo_page_get(
		page_id_t(undo->space, undo->hdr_page_no), mtr);

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		ut_error;
	case TRX_DICT_OP_INDEX:
		/* Do not discard the table on recovery. */
		undo->table_id = 0;
		break;
	case TRX_DICT_OP_TABLE:
		undo->table_id = trx->table_id;
		break;
	}

	mlog_write_ulint(hdr_page + undo->hdr_offset
			 + TRX_UNDO_DICT_TRANS,
			 TRUE, MLOG_1BYTE, mtr);

	mlog_write_ull(hdr_page + undo->hdr_offset + TRX_UNDO_TABLE_ID,
		       undo->table_id, mtr);

	undo->dict_operation = TRUE;
}

/** Assign an undo log for a transaction.
A new undo log is created or a cached undo log reused.
@param[in,out]	trx	transaction
@param[in]	rseg	rollback segment
@param[out]	undo	the undo log
@param[in]	type	TRX_UNDO_INSERT or TRX_UNDO_UPDATE
@retval	DB_SUCCESS	on success
@retval	DB_TOO_MANY_CONCURRENT_TRXS
@retval	DB_OUT_OF_FILE_SPACE
@retval	DB_READ_ONLY
@retval DB_OUT_OF_MEMORY */
dberr_t
trx_undo_assign_undo(
	trx_t*		trx,
	trx_rseg_t*	rseg,
	trx_undo_t**	undo,
	ulint		type)
{
	const bool	is_temp = rseg == trx->rsegs.m_noredo.rseg;
	mtr_t		mtr;
	dberr_t		err = DB_SUCCESS;

	ut_ad(mutex_own(&trx->undo_mutex));
	ut_ad(rseg == trx->rsegs.m_redo.rseg
	      || rseg == trx->rsegs.m_noredo.rseg);
	ut_ad(type == TRX_UNDO_INSERT || type == TRX_UNDO_UPDATE);

	mtr.start(trx);

	if (is_temp) {
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		ut_ad(undo == &trx->rsegs.m_noredo.undo);
	} else {
		ut_ad(undo == (type == TRX_UNDO_INSERT
			       ? &trx->rsegs.m_redo.insert_undo
			       : &trx->rsegs.m_redo.update_undo));
	}

	mutex_enter(&rseg->mutex);

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_too_many_trx",
		err = DB_TOO_MANY_CONCURRENT_TRXS;
		goto func_exit;
	);

	*undo = trx_undo_reuse_cached(trx, rseg, type, trx->id, trx->xid,
				     &mtr);
	if (*undo == NULL) {
		err = trx_undo_create(trx, rseg, type, trx->id, trx->xid,
				      undo, &mtr);
		if (err != DB_SUCCESS) {
			goto func_exit;
		}
	}

	if (is_temp) {
		UT_LIST_ADD_FIRST(rseg->insert_undo_list, *undo);
	} else {
		UT_LIST_ADD_FIRST(type == TRX_UNDO_INSERT
				  ? rseg->insert_undo_list
				  : rseg->update_undo_list, *undo);
		if (trx_get_dict_operation(trx) != TRX_DICT_OP_NONE) {
			trx_undo_mark_as_dict_operation(trx, *undo, &mtr);
		}
	}

func_exit:
	mutex_exit(&rseg->mutex);
	mtr.commit();

	return(err);
}

/******************************************************************//**
Sets the state of the undo log segment at a transaction finish.
@return undo log segment header page, x-latched */
page_t*
trx_undo_set_state_at_finish(
/*=========================*/
	trx_undo_t*	undo,	/*!< in: undo log memory copy */
	mtr_t*		mtr)	/*!< in: mtr */
{
	trx_usegf_t*	seg_hdr;
	trx_upagef_t*	page_hdr;
	page_t*		undo_page;
	ulint		state;

	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo_page = trx_undo_page_get(
		page_id_t(undo->space, undo->hdr_page_no), mtr);

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;
	page_hdr = undo_page + TRX_UNDO_PAGE_HDR;

	if (undo->size == 1
	    && mach_read_from_2(page_hdr + TRX_UNDO_PAGE_FREE)
	       < TRX_UNDO_PAGE_REUSE_LIMIT) {

		state = TRX_UNDO_CACHED;

	} else if (undo->type == TRX_UNDO_INSERT) {

		state = TRX_UNDO_TO_FREE;
	} else {
		state = TRX_UNDO_TO_PURGE;
	}

	undo->state = state;

	mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, state, MLOG_2BYTES, mtr);

	return(undo_page);
}

/** Set the state of the undo log segment at a XA PREPARE or XA ROLLBACK.
@param[in,out]	trx		transaction
@param[in,out]	undo		insert_undo or update_undo log
@param[in]	rollback	false=XA PREPARE, true=XA ROLLBACK
@param[in,out]	mtr		mini-transaction
@return undo log segment header page, x-latched */
page_t*
trx_undo_set_state_at_prepare(
	trx_t*		trx,
	trx_undo_t*	undo,
	bool		rollback,
	mtr_t*		mtr)
{
	trx_usegf_t*	seg_hdr;
	trx_ulogf_t*	undo_header;
	page_t*		undo_page;
	ulint		offset;

	ut_ad(trx && undo && mtr);

	ut_a(undo->id < TRX_RSEG_N_SLOTS);

	undo_page = trx_undo_page_get(
		page_id_t(undo->space, undo->hdr_page_no), mtr);

	seg_hdr = undo_page + TRX_UNDO_SEG_HDR;

	if (rollback) {
		ut_ad(undo->state == TRX_UNDO_PREPARED);
		mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, TRX_UNDO_ACTIVE,
				 MLOG_2BYTES, mtr);
		return(undo_page);
	}

	/*------------------------------*/
	ut_ad(undo->state == TRX_UNDO_ACTIVE);
	undo->state = TRX_UNDO_PREPARED;
	undo->xid   = *trx->xid;
	/*------------------------------*/

	mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, undo->state,
			 MLOG_2BYTES, mtr);

	offset = mach_read_from_2(seg_hdr + TRX_UNDO_LAST_LOG);
	undo_header = undo_page + offset;

	mlog_write_ulint(undo_header + TRX_UNDO_XID_EXISTS,
			 TRUE, MLOG_1BYTE, mtr);

	trx_undo_write_xid(undo_header, &undo->xid, mtr);

	return(undo_page);
}

/**********************************************************************//**
Adds the update undo log header as the first in the history list, and
frees the memory object, or puts it to the list of cached update undo log
segments. */
void
trx_undo_update_cleanup(
/*====================*/
	trx_t*		trx,		/*!< in: trx owning the update
					undo log */
	page_t*		undo_page,	/*!< in: update undo log header page,
					x-latched */
	mtr_t*		mtr)		/*!< in: mtr */
{
	trx_undo_t*	undo	= trx->rsegs.m_redo.update_undo;
	trx_rseg_t*	rseg	= undo->rseg;

	ut_ad(mutex_own(&rseg->mutex));

	trx_purge_add_update_undo_to_history(trx, undo_page, mtr);

	UT_LIST_REMOVE(rseg->update_undo_list, undo);

	trx->rsegs.m_redo.update_undo = NULL;

	if (undo->state == TRX_UNDO_CACHED) {

		UT_LIST_ADD_FIRST(rseg->update_undo_cached, undo);

		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_PURGE);

		trx_undo_mem_free(undo);
	}
}

/** Free an insert or temporary undo log after commit or rollback.
The information is not needed after a commit or rollback, therefore
the data can be discarded.
@param[in,out]	undo	undo log
@param[in]	is_temp	whether this is temporary undo log */
void
trx_undo_commit_cleanup(trx_undo_t* undo, bool is_temp)
{
	trx_rseg_t*	rseg	= undo->rseg;
	ut_ad(is_temp == !rseg->is_persistent());

	mutex_enter(&rseg->mutex);

	UT_LIST_REMOVE(rseg->insert_undo_list, undo);

	if (undo->state == TRX_UNDO_CACHED) {
		UT_LIST_ADD_FIRST(rseg->insert_undo_cached, undo);
		MONITOR_INC(MONITOR_NUM_UNDO_SLOT_CACHED);
	} else {
		ut_ad(undo->state == TRX_UNDO_TO_FREE);

		/* Delete first the undo log segment in the file */
		mutex_exit(&rseg->mutex);
		trx_undo_seg_free(undo, is_temp);
		mutex_enter(&rseg->mutex);

		ut_ad(rseg->curr_size > undo->size);
		rseg->curr_size -= undo->size;

		trx_undo_mem_free(undo);
	}

	mutex_exit(&rseg->mutex);
}

/********************************************************************//**
At shutdown, frees the undo logs of a PREPARED transaction. */
void
trx_undo_free_prepared(
/*===================*/
	trx_t*	trx)	/*!< in/out: PREPARED transaction */
{
	ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);

	if (trx->rsegs.m_redo.update_undo) {
		switch (trx->rsegs.m_redo.update_undo->state) {
		case TRX_UNDO_PREPARED:
			break;
		case TRX_UNDO_ACTIVE:
			/* lock_trx_release_locks() assigns
			trx->is_recovered=false */
			ut_a(!srv_was_started
			     || srv_read_only_mode
			     || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);
			break;
		default:
			ut_error;
		}

		UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->update_undo_list,
			       trx->rsegs.m_redo.update_undo);
		trx_undo_mem_free(trx->rsegs.m_redo.update_undo);

		trx->rsegs.m_redo.update_undo = NULL;
	}

	if (trx->rsegs.m_redo.insert_undo) {
		switch (trx->rsegs.m_redo.insert_undo->state) {
		case TRX_UNDO_PREPARED:
			break;
		case TRX_UNDO_ACTIVE:
			/* lock_trx_release_locks() assigns
			trx->is_recovered=false */
			ut_a(!srv_was_started
			     || srv_read_only_mode
			     || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);
			break;
		default:
			ut_error;
		}

		UT_LIST_REMOVE(trx->rsegs.m_redo.rseg->insert_undo_list,
			       trx->rsegs.m_redo.insert_undo);
		trx_undo_mem_free(trx->rsegs.m_redo.insert_undo);

		trx->rsegs.m_redo.insert_undo = NULL;
	}

	if (trx_undo_t*& undo = trx->rsegs.m_noredo.undo) {
		ut_a(undo->state == TRX_UNDO_PREPARED);

		UT_LIST_REMOVE(trx->rsegs.m_noredo.rseg->insert_undo_list,
			       undo);
		trx_undo_mem_free(undo);
		undo = NULL;
	}
}

/** Truncate UNDO tablespace, reinitialize header and rseg.
@param[in]	undo_trunc	UNDO tablespace handler
@return true if success else false. */
bool
trx_undo_truncate_tablespace(
	undo::Truncate*	undo_trunc)

{
	bool	success = true;
	ulint	space_id = undo_trunc->get_marked_space_id();

	/* Step-1: Truncate tablespace. */
	success = fil_truncate_tablespace(
		space_id, SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);

	if (!success) {
		return(success);
	}

	/* Step-2: Re-initialize tablespace header.
	Avoid REDO logging as we don't want to apply the action if server
	crashes. For fix-up we have UNDO-truncate-ddl-log. */
	mtr_t		mtr;
	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
	fsp_header_init(space_id, SRV_UNDO_TABLESPACE_SIZE_IN_PAGES, &mtr);
	mtr_commit(&mtr);

	/* Step-3: Re-initialize rollback segment header that resides
	in truncated tablespaced. */
	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
	mtr_x_lock(fil_space_get_latch(space_id, NULL), &mtr);

	for (ulint i = 0; i < undo_trunc->rsegs_size(); ++i) {
		trx_rsegf_t*	rseg_header;

		trx_rseg_t*	rseg = undo_trunc->get_ith_rseg(i);

		rseg->page_no = trx_rseg_header_create(
			space_id, ULINT_MAX, rseg->id, &mtr);

		rseg_header = trx_rsegf_get_new(space_id, rseg->page_no, &mtr);

		/* Before re-initialization ensure that we free the existing
		structure. There can't be any active transactions. */
		ut_a(UT_LIST_GET_LEN(rseg->update_undo_list) == 0);
		ut_a(UT_LIST_GET_LEN(rseg->insert_undo_list) == 0);

		trx_undo_t*	next_undo;

		for (trx_undo_t* undo =
			UT_LIST_GET_FIRST(rseg->update_undo_cached);
		     undo != NULL;
		     undo = next_undo) {

			next_undo = UT_LIST_GET_NEXT(undo_list, undo);
			UT_LIST_REMOVE(rseg->update_undo_cached, undo);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
			trx_undo_mem_free(undo);
		}

		for (trx_undo_t* undo =
			UT_LIST_GET_FIRST(rseg->insert_undo_cached);
		     undo != NULL;
		     undo = next_undo) {

			next_undo = UT_LIST_GET_NEXT(undo_list, undo);
			UT_LIST_REMOVE(rseg->insert_undo_cached, undo);
			MONITOR_DEC(MONITOR_NUM_UNDO_SLOT_CACHED);
			trx_undo_mem_free(undo);
		}

		UT_LIST_INIT(rseg->update_undo_list, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->update_undo_cached, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->insert_undo_list, &trx_undo_t::undo_list);
		UT_LIST_INIT(rseg->insert_undo_cached, &trx_undo_t::undo_list);

		rseg->max_size = mtr_read_ulint(
			rseg_header + TRX_RSEG_MAX_SIZE, MLOG_4BYTES, &mtr);

		/* Initialize the undo log lists according to the rseg header */
		rseg->curr_size = mtr_read_ulint(
			rseg_header + TRX_RSEG_HISTORY_SIZE, MLOG_4BYTES, &mtr)
			+ 1;

		ut_ad(rseg->curr_size == 1);

		rseg->trx_ref_count = 0;
		rseg->last_page_no = FIL_NULL;
		rseg->last_offset = 0;
		rseg->last_trx_no = 0;
		rseg->last_del_marks = FALSE;
	}
	mtr_commit(&mtr);

	return(success);
}
