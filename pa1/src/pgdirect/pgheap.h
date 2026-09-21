/*-------------------------------------------------------------------------
 *
 * pgheap.h
 *    Direct PostgreSQL heap-file reader: public C interface.
 *
 * This is the ONLY header shared between the C reader and the DuckDB
 * extension (C++17, duckdb.hpp).  It uses <stdint.h> types only.  The
 * reader is built from pg_ondisk_format.h, never from the PostgreSQL
 * headers.
 *
 * The reader is a pipeline; each stage is an independent function with no
 * global state.  The seven functions of the assignment are in
 * pgheap_reader.c, their companions in pgheap_support.c:
 *
 *  pgh_file_*          relation file + 1 GB segments -> 8 KB blocks
 *  pgh_page_*          slotted page -> line pointers (items)
 *  pgh_tuple_*         heap tuple header + deform -> per-column datums
 *  pgh_varlena_get     variable-length value -> its bytes
 *  pgh_decode_numeric  numeric header -> digits
 *  pgh_scan_*          file + page + tuple composed into a block-range
 *                      cursor (the "NextTuple()" of the assignment)
 *
 * Assumptions (documented in the spec, not verified up front): the table
 * was created and bulk loaded once, then VACUUM FREEZEd and CHECKPOINTed,
 * and never altered; every value is stored inline and uncompressed.  A
 * violation is reported only where the reader happens to notice it (a
 * tuple with another number of attributes, a TOAST pointer, a compressed
 * value).  Otherwise the reader trusts the files: only the bounds checks
 * that keep a corrupt page from reading outside the buffer are made.
 *
 * Conventions: functions return 0 on success and -1 on error (message in
 * the pgh_err they were given); other non-negative results are documented
 * per function.  Datums point INTO the page buffer of the scan and are
 * valid only until the next pgh_scan_next call on the same handle.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGHEAP_H
#define PGHEAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PGH_BLCKSZ 8192u

/* ---------------------------------------------------------------------
 * errors
 * ---------------------------------------------------------------------
 */
typedef struct pgh_err
{
    char        msg[512];
} pgh_err;

/*
 * printf-style formatting into e->msg.  Always returns -1 so that callers
 * can write "return pgh_errf(e, ...)".
 */
int         pgh_errf(pgh_err *e, const char *fmt,...)
            __attribute__((format(printf, 2, 3)));

/* formats an error into e and returns -1 from the calling function */
#define PGH_FAIL(e, ...) return pgh_errf((e), __VA_ARGS__)

/* appends " <context>" (e.g. the block and offset) to an existing message */
void        pgh_err_append(pgh_err *e, const char *fmt,...)
            __attribute__((format(printf, 2, 3)));

/* ---------------------------------------------------------------------
 * relation file
 * ---------------------------------------------------------------------
 */
typedef struct pgh_file pgh_file;

/*
 * Opens <relpath>, <relpath>.1, <relpath>.2, ... and stops at the first
 * missing or short segment (like md.c).
 */
int         pgh_file_open(pgh_file **out, const char *relpath, pgh_err *e);

/* total number of blocks over all segments */
uint32_t    pgh_file_nblocks(const pgh_file *f);

/* reads global block number blkno (across segments) into buf[PGH_BLCKSZ] */
int         pgh_file_read_block(pgh_file *f, uint32_t blkno, uint8_t *buf,
                                pgh_err *e);

void        pgh_file_close(pgh_file *f);

/* ---------------------------------------------------------------------
 * page
 * ---------------------------------------------------------------------
 */
enum
{
    PGH_LP_UNUSED = 0,
    PGH_LP_NORMAL = 1,
    PGH_LP_REDIRECT = 2,
    PGH_LP_DEAD = 3
};

typedef struct pgh_item
{
    uint16_t    off;            /* byte offset of the tuple in the page */
    uint16_t    len;            /* length of the tuple in bytes */
    uint8_t     state;          /* PGH_LP_* */
} pgh_item;

/*
 * 0 = usable page, -1 = bad page.  A page with pd_upper 0 was never written
 * (no line pointers yet, what PageIsNew tests) and is usable only if all
 * PGH_BLCKSZ bytes are zero.  Any other page must satisfy
 * 24 <= pd_lower <= pd_upper <= pd_special <= PGH_BLCKSZ.  blkno is only
 * used in the error message.
 */
int         pgh_page_verify(const uint8_t *page, uint32_t blkno, pgh_err *e);

/* number of line pointers on the page */
uint16_t    pgh_page_nitems(const uint8_t *page);

/*
 * Reads line pointer itemno (1-based).  For PGH_LP_NORMAL items the
 * (off, len) pair is validated against the page bounds.
 */
int         pgh_page_item(const uint8_t *page, uint16_t itemno, pgh_item *out,
                          pgh_err *e);

/* ---------------------------------------------------------------------
 * tuple
 * ---------------------------------------------------------------------
 */
typedef struct pgh_tuphdr
{
    uint16_t    natts;          /* attributes stored in the tuple */
    uint8_t     hoff;           /* t_hoff: offset of the first attribute */
    uint8_t     hasnull;        /* HEAP_HASNULL: a null bitmap follows */
} pgh_tuphdr;

/* validates lp_len >= header size and t_hoff; fills *out */
int         pgh_tuple_header(const uint8_t *tup, uint32_t len, pgh_tuphdr *out,
                             pgh_err *e);

typedef struct pgh_att
{
    int16_t     attlen;         /* > 0 fixed width, -1 varlena */
    char        attalign;       /* 'c', 's', 'i' or 'd' */
    uint32_t    atttypid;
    int32_t     atttypmod;
    char        attname[64];    /* for error messages only */
    char        typname[64];
} pgh_att;

typedef struct pgh_tupdesc
{
    int         natts;
    const pgh_att *atts;
} pgh_tupdesc;

typedef struct pgh_datum
{
    const uint8_t *ptr;         /* first byte of the attribute inside the
                                 * tuple (varlena: its header byte) */
    int32_t     len;            /* bytes occupied in the tuple (varlena: full
                                 * size including the header); 0 when null */
    uint8_t     isnull;         /* SQL NULL */
} pgh_datum;

/*
 * Deforms the tuple bytes [tup, tup + len) using desc; out must hold
 * desc->natts entries.  Attributes are aligned by attalign (pgo_align),
 * except a varlena with a 1-byte header, which is stored unpadded (see
 * pg_ondisk_format.h).  Every attribute is bounds-checked against the end
 * of the tuple.
 */
int         pgh_tuple_deform(const pgh_tupdesc *d, const uint8_t *tup,
                             uint32_t len, pgh_datum *out, pgh_err *e);

/* ---------------------------------------------------------------------
 * values
 * ---------------------------------------------------------------------
 */

/*
 * The bytes of a variable-length value (text, varchar, char(n), numeric):
 * the 1-byte or 4-byte header is skipped.  A compressed value or a TOAST
 * pointer is an error, both are out of scope.
 */
int         pgh_varlena_get(const pgh_datum *d, const uint8_t **data,
                            int32_t *len, pgh_err *e);

/*
 * numeric: the on-disk header decoded; digits are native int16 base-10000
 * (ndigits of them).
 */
enum
{
    PGH_NUM_FINITE = 0,
    PGH_NUM_NAN = 1,
    PGH_NUM_PINF = 2,
    PGH_NUM_NINF = 3
};

typedef struct pgh_numeric
{
    uint8_t     special;        /* PGH_NUM_* */
    uint8_t     negative;
    int16_t     weight;         /* of the first digit, in base-10000 units */
    int16_t     dscale;         /* display scale */
    int32_t     ndigits;
    const uint8_t *digits;      /* ndigits * 2 bytes, native-endian int16 */
} pgh_numeric;

/* d / len: the bytes returned by pgh_varlena_get */
int         pgh_decode_numeric(const uint8_t *d, int32_t len, pgh_numeric *out,
                               pgh_err *e);

/* ---------------------------------------------------------------------
 * scan cursor
 * ---------------------------------------------------------------------
 */
typedef struct pgh_scan pgh_scan;

typedef struct pgh_stats
{
    uint64_t    pages;          /* blocks read */
    uint64_t    rows;           /* tuples returned */
} pgh_stats;

/*
 * Blocks [blk_begin, blk_end) of file f (blk_end is clamped to
 * pgh_file_nblocks).  e receives the message of every later error.
 */
int         pgh_scan_open(pgh_scan **out, pgh_file *f, const pgh_tupdesc *d,
                          uint32_t blk_begin, uint32_t blk_end, pgh_err *e);

/*
 * 1 = *row (desc->natts datums) filled from the next tuple, 0 = range
 * exhausted, -1 = error (message in the pgh_err given to pgh_scan_open).
 */
int         pgh_scan_next(pgh_scan *s, pgh_datum *row);

const pgh_stats *pgh_scan_stats(const pgh_scan *s);

void        pgh_scan_close(pgh_scan *s);

#ifdef __cplusplus
}
#endif

#endif                          /* PGHEAP_H */
