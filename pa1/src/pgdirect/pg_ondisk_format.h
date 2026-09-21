/*-------------------------------------------------------------------------
 *
 * pg_ondisk_format.h
 *    PostgreSQL on-disk format facts, as plain constants.
 *
 * Everything here is copied from the PostgreSQL headers named in the
 * comments so that the reader can be built without them.  Reference
 * facts only, no logic beyond a one-line helper.  Little-endian machines
 * (x86-64, arm64) only.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ONDISK_FORMAT_H
#define PG_ONDISK_FORMAT_H

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * relation files (src/include/pg_config.h)
 * ---------------------------------------------------------------------
 */
#define PGO_RELSEG_SIZE         131072  /* blocks per segment file (1 GB) */

/* ---------------------------------------------------------------------
 * page header (src/include/storage/bufpage.h)
 * ---------------------------------------------------------------------
 *
 *  byte  0  pd_lsn       8 bytes  WAL position of the last change
 *        8  pd_checksum  uint16
 *       10  pd_flags     uint16
 *       12  pd_lower     uint16   end of the line pointer array
 *       14  pd_upper     uint16   start of the tuple area
 *       16  pd_special   uint16   start of the special area (heap: BLCKSZ)
 *       18  pd_pagesize_version  uint16
 *       20  pd_prune_xid uint32
 *       24  pd_linp[]             line pointers, 4 bytes each
 */
#define PGO_PAGEHDR_LOWER       12
#define PGO_PAGEHDR_UPPER       14
#define PGO_PAGEHDR_SPECIAL     16
#define PGO_PAGEHDR_SIZE        24      /* SizeOfPageHeaderData */

/* reads the uint16 field at byte offset off of the page */
static inline uint16_t
pgo_read_u16(const uint8_t *page, int off)
{
    uint16_t    v;

    memcpy(&v, page + off, sizeof(v));
    return v;
}

/* ---------------------------------------------------------------------
 * line pointer (src/include/storage/itemid.h): one uint32, little-endian
 * ---------------------------------------------------------------------
 *
 *  bits  0-14  lp_off    byte offset of the tuple from the page start
 *       15-16  lp_flags  0 UNUSED, 1 NORMAL, 2 REDIRECT, 3 DEAD (= PGH_LP_*)
 *       17-31  lp_len    tuple length in bytes
 */
#define PGO_ITEMID_SIZE         4
#define PGO_LP_OFF(w)           ((w) & 0x7FFF)
#define PGO_LP_FLAGS(w)         (((w) >> 15) & 0x03)
#define PGO_LP_LEN(w)           (((w) >> 17) & 0x7FFF)

/* MAXALIGN (src/include/c.h): tuples start at multiples of 8 bytes */
#define PGO_MAXALIGN(x)         (((x) + 7) & ~(uint32_t) 7)

/* ---------------------------------------------------------------------
 * heap tuple header (src/include/access/htup_details.h)
 * ---------------------------------------------------------------------
 *
 *  byte  0  t_xmin       uint32   inserting transaction
 *        4  t_xmax       uint32   deleting transaction, or 0
 *        8  t_cid        uint32   command id
 *       12  t_ctid       6 bytes  (block, offset) of this or a newer version
 *       18  t_infomask2  uint16   number of attributes + flags
 *       20  t_infomask   uint16   flags
 *       22  t_hoff       uint8    offset of the first attribute
 *       23  t_bits[]              null bitmap, only if HEAP_HASNULL is set
 *  t_hoff   attribute data
 */
#define PGO_TUPHDR_INFOMASK2    18
#define PGO_TUPHDR_INFOMASK     20
#define PGO_TUPHDR_HOFF         22
#define PGO_TUPHDR_BITS         23
#define PGO_TUPHDR_MINSIZE      PGO_TUPHDR_BITS /* SizeofHeapTupleHeader */

#define PGO_HEAP_NATTS_MASK     0x07FF  /* t_infomask2: number of attributes */
#define PGO_HEAP_HASNULL        0x0001  /* t_infomask: has a null bitmap */

/* null bitmap: one bit per attribute, bit set means the value is present */
#define PGO_BITMAPLEN(natts)    (((natts) + 7) / 8)
#define PGO_ATT_ISNULL(i, nullbits) \
    (!((nullbits)[(i) >> 3] & (1 << ((i) & 7))))

/* ---------------------------------------------------------------------
 * attribute alignment (src/include/access/tupmacs.h)
 * ---------------------------------------------------------------------
 */

/* rounds off up to the alignment that pg_attribute.attalign asks for */
static inline uint32_t
pgo_align(uint32_t off, char attalign)
{
    uint32_t    a;

    switch (attalign)
    {
        case 'c':
            a = 1;
            break;
        case 's':
            a = 2;
            break;
        case 'i':
            a = 4;
            break;
        default:
            a = 8;
            break;
    }
    return (off + a - 1) & ~(a - 1);
}

/*
 * Exception (att_align_pointer in tupmacs.h): a varlena (attlen -1) whose
 * first byte is a 1-byte header is stored with no padding in front of it.
 * Padding bytes are always zero and a 1-byte header never is, so when the
 * byte at the unaligned offset is non-zero the value starts right there;
 * only a zero byte means padding up to attalign.
 */

/* ---------------------------------------------------------------------
 * variable-length values (src/include/varatt.h)
 * ---------------------------------------------------------------------
 *
 * A varlena starts with a 1-byte header (bit 0 set: the total size, header
 * included, is in bits 1-7, so at most 127 bytes) or a 4-byte header (bit 0
 * clear: the total size is in bits 2-31).  The single byte 0x01 marks a
 * TOAST pointer instead.  b is the first byte, w the first four bytes read
 * as one uint32.
 */
#define PGO_VARATT_IS_1B(b)     (((b) & 0x01) == 0x01)
#define PGO_VARATT_IS_1B_E(b)   ((b) == 0x01)
#define PGO_VARATT_IS_4B_C(b)   (((b) & 0x03) == 0x02)  /* compressed */
#define PGO_VARSIZE_1B(b)       (((b) >> 1) & 0x7F)
#define PGO_VARSIZE_4B(w)       (((w) >> 2) & 0x3FFFFFFF)
#define PGO_VARHDRSZ_SHORT      1
#define PGO_VARHDRSZ            4

/* ---------------------------------------------------------------------
 * numeric (src/backend/utils/adt/numeric.c)
 * ---------------------------------------------------------------------
 *
 * A numeric starts with a uint16 header word.  Its top two bits say which
 * form follows: 00/01 long (sign in the top bits, dscale in the low 14,
 * then an int16 weight), 10 short (sign, dscale and weight packed into the
 * word), 11 special (NaN, +Infinity, -Infinity).  The base-10000 digits
 * (int16 each) follow the header.
 */
#define PGO_NUMERIC_SIGN_MASK       0xC000
#define PGO_NUMERIC_NEG             0x4000
#define PGO_NUMERIC_SHORT           0x8000
#define PGO_NUMERIC_SPECIAL         0xC000
#define PGO_NUMERIC_NAN             0xC000
#define PGO_NUMERIC_PINF            0xD000
#define PGO_NUMERIC_NINF            0xF000
#define PGO_NUMERIC_DSCALE_MASK     0x3FFF  /* long form */
/*
 * Bytes before the digits, counted from the first byte after the varlena
 * header (PostgreSQL's NUMERIC_HDRSZ / NUMERIC_HDRSZ_SHORT are 4 larger:
 * they include the varlena header).
 */
#define PGO_NUMERIC_HEADER_LONG     4       /* uint16 header + int16 weight */
#define PGO_NUMERIC_HEADER_SHORT    2       /* uint16 header */
#define PGO_NUMERIC_SHORT_SIGN_MASK         0x2000
#define PGO_NUMERIC_SHORT_DSCALE_MASK       0x1F80
#define PGO_NUMERIC_SHORT_DSCALE_SHIFT      7
#define PGO_NUMERIC_SHORT_WEIGHT_SIGN_MASK  0x0040
#define PGO_NUMERIC_SHORT_WEIGHT_MASK       0x003F

#endif                          /* PG_ONDISK_FORMAT_H */
