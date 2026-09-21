/*-------------------------------------------------------------------------
 *
 * pgheap_reader.c
 *    The seven functions of the assignment, in reading order:
 *
 *      pgh_file_open, pgh_file_read_block   relation file -> blocks
 *      pgh_page_verify, pgh_page_item       page -> line pointers
 *      pgh_tuple_header, pgh_tuple_deform   tuple -> column datums
 *      pgh_scan_next                        the three composed: next tuple
 *
 * Built from pgheap.h, pgheap_types.h and pg_ondisk_format.h only, without
 * the PostgreSQL headers.  Everything else the reader needs is provided in
 * pgheap_support.c, pgheap_err.c, pgheap_varlena.c and pgheap_numeric.c.
 *
 *-------------------------------------------------------------------------
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pgheap.h"
#include "pgheap_types.h"
#include "pg_ondisk_format.h"

/* ---------------------------------------------------------------------
 * relation file segments -> blocks
 * ---------------------------------------------------------------------
 *
 * A relation is split into segment files of PGO_RELSEG_SIZE blocks:
 * <relpath>, <relpath>.1, <relpath>.2, ...  A block is PGH_BLCKSZ bytes.
 */

/*
 * pgh_file_open -- open every segment file of one relation
 *
 * Arguments
 *   out      Receives the new pgh_file.  Undefined on entry.  On success
 *            *out points at a heap-allocated struct that the caller later
 *            hands to pgh_file_close (provided in pgheap_support.c).  On
 *            failure do not write *out, and leave nothing allocated or
 *            open.
 *   relpath  NUL-terminated path of segment 0, as PostgreSQL reports it:
 *            <PGDATA>/base/<dboid>/<relfilenode>.  The other segments of
 *            the same relation are this path with ".1", ".2", ... appended.
 *            Read only; do not modify it.  Keep a copy in f->relpath.
 *   e        Error sink.  You never have to use it: on success leave it
 *            alone, and on failure returning -1 is all that is required.
 *            Writing the reason into e->msg is optional (PGH_FAIL does
 *            that and returns -1 in one go).  Whatever is there becomes
 *            the text of the DuckDB error, so a message helps you debug.
 *            The same holds for every e below.
 *
 * What the function must do
 *   A relation is stored as one or more segment files, each holding at
 *   most PGO_RELSEG_SIZE blocks of PGH_BLCKSZ bytes (1 GB).  Segment N
 *   exists only if segments 0 .. N-1 are all full; only the last segment
 *   can be partially filled, from 0 up to PGO_RELSEG_SIZE blocks.  The
 *   function finds every segment that exists, opens each one read-only,
 *   and records how many whole blocks each holds, so that
 *   pgh_file_read_block can later turn a global block number into
 *   (segment, offset) without touching the file system again.
 *
 *   The rules are those of md.c, the PostgreSQL storage manager:
 *     - segment 0 must exist; if it cannot be opened that is an error;
 *     - a segment that does not exist ends the relation; that is not an
 *       error, the relation just has that many segments;
 *     - a segment holding fewer than PGO_RELSEG_SIZE blocks is the last
 *       one; nothing after it is looked at;
 *     - the block count of a segment is its size in bytes (fstat)
 *       divided by PGH_BLCKSZ, a partial trailing block rounded down;
 *     - more than PGH_MAXSEG segments is an error (the arrays in
 *       pgh_file are that size).
 *
 * State on success (the pgh_file fields, see pgheap_types.h)
 *   relpath        copy of the argument
 *   nseg           number of segments opened, at least 1
 *   fds[N]         open read-only descriptor of segment N, for N < nseg
 *   segblocks[N]   whole blocks in segment N; every entry but the last
 *                  is PGO_RELSEG_SIZE
 *   nblocks        sum of segblocks[0 .. nseg); global block numbers run
 *                  0 .. nblocks - 1
 *
 * Return value
 *   0 on success.  -1 on error; then every descriptor opened so far is
 *   closed again and the struct freed (pgh_file_close does both for a
 *   partially filled struct as long as nseg counts exactly the
 *   descriptors that are open).
 *
 * Macros and helpers to use
 *   PGO_RELSEG_SIZE, PGH_BLCKSZ, PGH_MAXSEG, PGH_FAIL,
 *   pgh_file_close
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_file_open(pgh_file **out, const char *relpath, pgh_err *e)
{
    PGH_FAIL(e, "pgh_file_open: not implemented");
}

/*
 * pgh_file_read_block -- read one block of the relation into memory
 *
 * Arguments
 *   f      A pgh_file from pgh_file_open: nseg, fds[], segblocks[] and
 *          nblocks are filled as described above.  Read only; do not
 *          modify it.
 *   blkno  Global block number, 0-based, counted across all segments as
 *          if the relation were one file.  May be out of range.
 *   buf    Caller's buffer of at least PGH_BLCKSZ bytes.  Its contents on
 *          entry do not matter; on success it holds the block; on failure
 *          its contents are unspecified.
 *   e      Error sink.
 *
 * What the function must do
 *   Every segment but the last holds exactly PGO_RELSEG_SIZE blocks, so a
 *   global block number determines the segment it lives in and its block
 *   number inside that segment (division and remainder).  Block n of a
 *   segment occupies bytes [n * PGH_BLCKSZ, (n + 1) * PGH_BLCKSZ) of that
 *   segment file.  Read exactly those PGH_BLCKSZ bytes into buf.
 *
 *   Use pread(2) rather than lseek + read: it reads at an explicit offset
 *   and leaves no file position behind, which is what a block reader that
 *   jumps around wants.
 *
 * Errors
 *   - blkno >= f->nblocks: no such block;
 *   - pread returns anything but PGH_BLCKSZ (0 = end of file, -1 = I/O
 *     error with errno): the file changed under us or is damaged.
 *
 * State on success
 *   buf[0 .. PGH_BLCKSZ)   the bytes of block blkno, exactly as they are
 *                          in the segment file
 *   f                      unchanged
 *
 * Return value
 *   0 with buf filled, -1 on error.
 *
 * Macros and helpers to use
 *   PGO_RELSEG_SIZE, PGH_BLCKSZ, PGH_FAIL
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_file_read_block(pgh_file *f, uint32_t blkno, uint8_t *buf, pgh_err *e)
{
    PGH_FAIL(e, "pgh_file_read_block: not implemented");
}

/* ---------------------------------------------------------------------
 * slotted page -> line pointers
 * ---------------------------------------------------------------------
 */

/*
 * pgh_page_verify -- decide whether a block holds a usable heap page
 *
 * Arguments
 *   page   PGH_BLCKSZ bytes as read by pgh_file_read_block.  Read only;
 *          do not modify it.
 *   blkno  The block's global number.  It plays no part in the check;
 *          it is only an input for the error message, if any.
 *   e      Error sink.
 *
 * Page layout (PageHeaderData; PGO_PAGEHDR_* in pg_ondisk_format.h)
 *   The first PGO_PAGEHDR_SIZE (24) bytes are the header.  Three uint16
 *   fields in it delimit the page; pgo_read_u16(page, offset) reads them:
 *     pd_lower    (PGO_PAGEHDR_LOWER)   end of the line pointer array,
 *                                       which starts right after the
 *                                       header at byte 24
 *     pd_upper    (PGO_PAGEHDR_UPPER)   start of the tuple area, which
 *                                       grows downward from pd_special
 *     pd_special  (PGO_PAGEHDR_SPECIAL) start of the special space; heap
 *                                       pages have none, so PGH_BLCKSZ
 *   Free space is [pd_lower, pd_upper).
 *
 * What the function must decide
 *   A page is usable when
 *       24 <= pd_lower <= pd_upper <= pd_special <= PGH_BLCKSZ.
 *   These inequalities are what the rest of the reader relies on: the
 *   line pointer array [24, pd_lower) and the tuple area [pd_upper,
 *   pd_special) do not overlap and both lie inside the buffer, so
 *   pgh_page_item and pgh_tuple_deform never read outside the page.
 *
 *   One exception.  When PostgreSQL extends a relation in bulk it may
 *   append blocks that were never initialised.  On disk they are all
 *   zero, so pd_upper reads 0 (this is what PageIsNew tests) and the
 *   inequalities fail.  Such a page is usable: it has no line pointers
 *   (pd_lower is 0, pgh_page_nitems returns 0), so the scan finds nothing
 *   on it and moves on.  But only if every one of its PGH_BLCKSZ bytes is
 *   zero, the check PostgreSQL's PageIsVerified makes; a page whose
 *   pd_upper is 0 while other bytes are not is damaged and is refused.
 *
 * State on success
 *   None.  The page is untouched and nothing is remembered; the result is
 *   the return value alone.
 *
 * Return value
 *   0 = usable (the all-zero page included), -1 = refused.
 *   pgh_scan_next then fails the whole scan; a bad page is never skipped
 *   silently.
 *
 * Macros and helpers to use
 *   PGO_PAGEHDR_SIZE, PGO_PAGEHDR_LOWER/UPPER/SPECIAL,
 *   pgo_read_u16, PGH_BLCKSZ, PGH_FAIL
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_page_verify(const uint8_t *page, uint32_t blkno, pgh_err *e)
{
    PGH_FAIL(e, "pgh_page_verify: not implemented");
}

/*
 * pgh_page_item -- decode one line pointer of a verified page
 *
 * Arguments
 *   page    A page that passed pgh_page_verify.  Read only; do not
 *           modify it.
 *   itemno  Line pointer number, 1-based like PostgreSQL's OffsetNumber:
 *           item 1 is the first 4-byte word after the 24-byte header, so
 *           item N starts at byte PGO_PAGEHDR_SIZE + (N - 1) *
 *           PGO_ITEMID_SIZE.  The page has pgh_page_nitems(page) of them
 *           (provided in pgheap_support.c).  itemno may be out of range.
 *   out     Receives the decoded line pointer.  Undefined on entry; all
 *           three fields written on success; unspecified on error.
 *   e       Error sink.
 *
 * Line pointer layout (ItemIdData; pg_ondisk_format.h)
 *   One little-endian uint32 holding three bit fields:
 *     bits  0..14  lp_off    PGO_LP_OFF(w)    byte offset in the page
 *     bits 15..16  lp_flags  PGO_LP_FLAGS(w)  one of PGH_LP_*
 *     bits 17..31  lp_len    PGO_LP_LEN(w)    tuple length in bytes
 *   Read the word with memcpy into a uint32_t, then apply the macros.
 *
 * State on success (by out->state)
 *   out->state       always the lp_flags value: PGH_LP_UNUSED, NORMAL,
 *                    REDIRECT or DEAD.
 *   PGH_LP_NORMAL    a tuple is there: out->off = lp_off, out->len =
 *                    lp_len.  Before accepting them make sure the tuple
 *                    [lp_off, lp_off + lp_len) lies inside the tuple area
 *                    [pd_upper, pd_special) of this page (pgo_read_u16 at
 *                    PGO_PAGEHDR_UPPER / PGO_PAGEHDR_SPECIAL) and that
 *                    lp_off is MAXALIGNed (PGO_MAXALIGN: PostgreSQL
 *                    places every tuple on an 8-byte boundary).  A
 *                    NORMAL item that fails this is an error, not a
 *                    skip; a later memcpy would read outside the page.
 *   any other state  no tuple is there.  This is not an error: return 0
 *                    with out->state set; out->off and out->len are
 *                    unspecified and nothing is checked.  pgh_scan_next
 *                    passes such items over.  For reference:
 *                    PGH_LP_REDIRECT  a HOT redirect; lp_off is the
 *                                     item number the chain continues at
 *                    PGH_LP_UNUSED    never used, or reclaimed
 *                    PGH_LP_DEAD      the tuple is gone, only the slot
 *                                     remains
 *
 * Errors
 *   itemno < 1 or itemno > pgh_page_nitems(page); a NORMAL item outside
 *   [pd_upper, pd_special) or not MAXALIGNed.
 *
 * Return value
 *   0 with *out filled, -1 on error.
 *
 * Macros and helpers to use
 *   PGO_PAGEHDR_SIZE, PGO_ITEMID_SIZE, PGO_LP_FLAGS/OFF/LEN,
 *   PGO_PAGEHDR_UPPER/SPECIAL, pgo_read_u16, PGO_MAXALIGN,
 *   PGH_LP_NORMAL, PGH_FAIL, pgh_page_nitems
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_page_item(const uint8_t *page, uint16_t itemno, pgh_item *out, pgh_err *e)
{
    PGH_FAIL(e, "pgh_page_item: not implemented");
}

/* ---------------------------------------------------------------------
 * heap tuple -> column datums
 * ---------------------------------------------------------------------
 *
 * Assumptions of the assignment: static schema, so every tuple carries
 * exactly desc->natts attributes; every attribute is either fixed width
 * (attlen > 0) or an inline varlena (attlen == -1) with a 1-byte or
 * 4-byte header; no TOAST.
 */

/*
 * pgh_tuple_header -- read the fixed part of a heap tuple header
 *
 * Arguments
 *   tup   First byte of a tuple: page + off of a PGH_LP_NORMAL item.
 *         Read only; do not modify it.
 *   len   lp_len of that item: how many bytes of the tuple are on the
 *         page.  Every read must stay inside [tup, tup + len).
 *   out   Receives natts, hoff and hasnull.  Undefined on entry; all
 *         three written on success.
 *   e     Error sink.
 *
 * Tuple header layout (HeapTupleHeaderData; pg_ondisk_format.h)
 *   The fixed header is PGO_TUPHDR_MINSIZE (23) bytes.  The fields this
 *   reader needs, each read with memcpy at its offset:
 *     PGO_TUPHDR_INFOMASK2  uint16  its low 11 bits (PGO_HEAP_NATTS_MASK)
 *                                   are the number of attributes stored
 *                                   in the tuple
 *     PGO_TUPHDR_INFOMASK   uint16  flag bits; PGO_HEAP_HASNULL set means
 *                                   a null bitmap follows the header
 *     PGO_TUPHDR_HOFF       uint8   t_hoff: offset of the first attribute
 *                                   from the start of the tuple
 *   Byte 23 (PGO_TUPHDR_BITS) is where the null bitmap starts when
 *   HASNULL is set; it has PGO_BITMAPLEN(natts) = (natts + 7) / 8 bytes.
 *   Then padding up to t_hoff, which is a multiple of 8: the header is 23
 *   bytes, so without a bitmap t_hoff is 24 and one byte is padding.
 *   Then the attribute data.
 *   The remaining header fields (xmin, xmax, ctid, ...) are MVCC state,
 *   out of scope for this assignment; do not read them.
 *
 * What the function must check
 *   - len >= PGO_TUPHDR_MINSIZE, otherwise the header itself is not
 *     there;
 *   - t_hoff <= len: the attribute data starts inside the tuple;
 *   - t_hoff >= 23 + (hasnull ? PGO_BITMAPLEN(natts) : 0): the header and
 *     the bitmap fit before the data.  A smaller t_hoff cannot come from
 *     PostgreSQL.
 *
 * State on success
 *   out->natts    attributes stored in this tuple.  With a static
 *                 schema it equals the catalog's count; pgh_tuple_deform
 *                 compares the two.
 *   out->hoff     t_hoff
 *   out->hasnull  1 if a null bitmap is present, else 0
 *
 * Return value
 *   0, or -1 on error.
 *
 * Macros and helpers to use
 *   PGO_TUPHDR_MINSIZE,
 *   PGO_TUPHDR_INFOMASK2/INFOMASK/HOFF/BITS,
 *   PGO_HEAP_NATTS_MASK, PGO_HEAP_HASNULL, PGO_BITMAPLEN,
 *   PGH_FAIL
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_tuple_header(const uint8_t *tup, uint32_t len, pgh_tuphdr *out,
                 pgh_err *e)
{
    PGH_FAIL(e, "pgh_tuple_header: not implemented");
}

/*
 * pgh_tuple_deform -- split a heap tuple into one datum per column
 *
 * Arguments
 *   d       Column layout of the table, from the catalog (pg_attribute):
 *           d->natts columns; d->atts[i] holds attlen (> 0 fixed width,
 *           -1 varlena), attalign ('c' 1, 's' 2, 'i' 4, 'd' 8 bytes) and
 *           attname for messages.  Columns are in attnum order, which is
 *           the order the values are stored in.  Read only; do not
 *           modify it.
 *   tup     First byte of a PGH_LP_NORMAL tuple (see pgh_tuple_header).
 *           Read only; do not modify it.
 *   tuplen  Its lp_len.  No read may go past tup + tuplen.
 *   out     Array of d->natts pgh_datum, undefined on entry.  On success
 *           every entry is written.  On error the entries before the
 *           failing column may have been written; the caller treats the
 *           whole array as unspecified.
 *   e       Error sink.
 *
 * State on success
 *   NULL column      ptr = NULL, len = 0, isnull = 1
 *   present column   ptr = address of the column's first byte inside tup
 *                    (for a varlena its header byte, not the data after
 *                    it); len = bytes the column occupies in the tuple
 *                    (fixed width: attlen; varlena: header plus data);
 *                    isnull = 0
 *   ptr points into the caller's page buffer; nothing is copied, and the
 *   pointer is valid only until pgh_scan_next loads the next block.
 *
 * Tuple data layout (what heap_form_tuple wrote)
 *   pgh_tuple_header gives natts, hoff and hasnull.  After the header,
 *   at tup + hoff, the attribute values follow in attnum order, one
 *   after the other, with nothing in between except alignment padding.
 *
 *   Nulls.  With hasnull set, bit i of the bitmap at tup + PGO_TUPHDR_BITS
 *   says whether column i is present; PGO_ATT_ISNULL(i, bits) is true
 *   for a NULL.  A NULL column occupies no bytes at all: the next column
 *   follows directly.  Without hasnull every column is present.
 *
 *   Alignment.  Before a column starts, the offset is rounded up to the
 *   column's attalign boundary (pgo_align).  Offsets are counted from
 *   tup + hoff; hoff is a multiple of 8 and the tuple itself is
 *   MAXALIGNed in the page, so aligning relative to the data start is the
 *   same as aligning within the page.
 *   Exception: a varlena stored with a 1-byte header is not padded at
 *   all.  Padding bytes are always 0 and a 1-byte header byte is never 0
 *   (its low bit is set), so for a varlena column a non-zero byte at the
 *   current offset means the value starts right there, while a zero byte
 *   is padding and the value starts at the aligned offset with a 4-byte
 *   header.
 *
 *   Size of a column.
 *     attlen > 0    exactly attlen bytes (bool 1, int2 2, int4/date/
 *                   float4 4, int8/float8/timestamp 8).
 *     attlen == -1  a varlena; its first byte b says which header it has:
 *                   PGO_VARATT_IS_1B(b)    1-byte header; total size,
 *                                          header included, is
 *                                          PGO_VARSIZE_1B(b), 1 .. 127;
 *                   PGO_VARATT_IS_1B_E(b)  b == 0x01: a TOAST pointer,
 *                                          out of scope; an error;
 *                   otherwise              4-byte header; read the four
 *                                          bytes as one little-endian
 *                                          uint32 with memcpy, total
 *                                          size is PGO_VARSIZE_4B(w) and
 *                                          must be at least 4 (the
 *                                          header itself).
 *   The column's len is that size; the next column starts at off + size.
 *
 * What the function must check (each failure is -1, never a skip)
 *   - natts from the tuple header equals d->natts.  With a static
 *     schema this holds for all the data you are given; check it anyway
 *     so a wrong table fails loudly;
 *   - every column starts before tup + tuplen and ends at or before it,
 *     the header bytes of a varlena included; a size of 0 is impossible
 *     and is an error too;
 *   - a TOAST pointer is an error.
 *
 * Return value
 *   0 with out[0 .. natts) filled, -1 on error.
 *
 * Macros and helpers to use
 *   PGO_TUPHDR_BITS, PGO_ATT_ISNULL, pgo_align,
 *   PGO_VARATT_IS_1B, PGO_VARATT_IS_1B_E, PGO_VARSIZE_1B,
 *   PGO_VARSIZE_4B, PGO_VARHDRSZ, PGH_FAIL,
 *   pgh_tuple_header
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_tuple_deform(const pgh_tupdesc *d, const uint8_t *tup, uint32_t tuplen,
                 pgh_datum *out, pgh_err *e)
{
    PGH_FAIL(e, "pgh_tuple_deform: not implemented");
}

/* ---------------------------------------------------------------------
 * the scan cursor: file, page and tuple composed ("NextTuple")
 * ---------------------------------------------------------------------
 */

/*
 * pgh_scan_next -- the cursor: return the next tuple of the block range
 *
 * Arguments
 *   s    The scan handle made by pgh_scan_open (pgheap_support.c).  Its
 *        fields (pgheap_types.h) and their state when the function is
 *        entered:
 *          f, d, e      the file, column layout and error sink to use;
 *                       e is where every failure below lands
 *          st           counters, optional like the error sink:
 *                       st.pages = blocks loaded so far, st.rows =
 *                       tuples returned so far; both 0 at open.  Nothing
 *                       depends on them; keep them if you want the
 *                       numbers.
 *          blk          the block to load next; blk_begin at open
 *          blk_end      one past the last block of this scan's range,
 *                       already clamped to the file size
 *          page         one PGH_BLCKSZ buffer owned by the handle, reused
 *                       for every block
 *          page_loaded  1 while *page holds block blk and the walk over
 *                       its line pointers is in progress; 0 at open
 *          next_item    the next line pointer to look at, 1-based;
 *                       meaningful only while page_loaded
 *          nitems       pgh_page_nitems(page) of the loaded page
 *        Every call continues from exactly this state and leaves behind
 *        the state the next call needs; nothing is remembered anywhere
 *        else.
 *   row  Array of d->natts pgh_datum supplied by the caller.  When 1 is
 *        returned it holds the tuple (see pgh_tuple_deform); otherwise
 *        it is unspecified.
 *
 * What one call must do
 *   Deliver the next PGH_LP_NORMAL tuple in file order: blocks blk ..
 *   blk_end - 1 in increasing order, and inside a block the line pointers
 *   1 .. nitems in increasing order.  UNUSED, DEAD and REDIRECT line
 *   pointers hold no tuple and are passed over (heapgettup does the
 *   same).  A page with no line pointers at all, the all-zero page that
 *   pgh_page_verify accepts included, contributes nothing.  A block is
 *   loaded with pgh_file_read_block into s->page and checked with
 *   pgh_page_verify before any of its line pointers is read.  Line
 *   pointers are decoded with pgh_page_item and tuples with
 *   pgh_tuple_deform into row.  When the loaded page is used up, the
 *   next block follows within the same call: the caller never sees
 *   "page exhausted".
 *
 * State after the call (success and end of range included)
 *   returned 1   row holds the tuple.  page_loaded = 1 and page still
 *                holds its block (row's pointers point into it),
 *                next_item is the line pointer after the one returned,
 *                blk is unchanged.
 *   returned 0   blk >= blk_end and page_loaded == 0: the range is used
 *                up.  Calling again returns 0 again.
 *   returned -1  one of the functions called failed.  The handle is not
 *                used again after -1.
 *
 * Return value
 *   1 = row filled, 0 = end of range, -1 = error.
 *
 * Macros and helpers to use
 *   PGH_LP_NORMAL, pgh_file_read_block, pgh_page_verify,
 *   pgh_page_nitems, pgh_page_item, pgh_tuple_deform,
 *   pgh_err_append
 *   Anything else: pg_ondisk_format.h, pgheap.h, pgheap_types.h.
 */
int
pgh_scan_next(pgh_scan *s, pgh_datum *row)
{
    PGH_FAIL(s->e, "pgh_scan_next: not implemented");
}
