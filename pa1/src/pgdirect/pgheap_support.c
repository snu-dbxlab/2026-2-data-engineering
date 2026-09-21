/*-------------------------------------------------------------------------
 *
 * pgheap_support.c
 *    The provided companions of the functions in pgheap_reader.c: the
 *    accessors of the file handle, the line pointer count, and the scan
 *    cursor's open / stats / close.
 *
 * Built from pgheap.h, pgheap_types.h and pg_ondisk_format.h only, without
 * the PostgreSQL headers.
 *
 *-------------------------------------------------------------------------
 */
#include <stdlib.h>
#include <unistd.h>

#include "pgheap.h"
#include "pgheap_types.h"
#include "pg_ondisk_format.h"

/* ---------------------------------------------------------------------
 * relation file
 * ---------------------------------------------------------------------
 */

uint32_t
pgh_file_nblocks(const pgh_file *f)
{
    return f->nblocks;
}

void
pgh_file_close(pgh_file *f)
{
    int         i;

    if (!f)
        return;
    for (i = 0; i < f->nseg; i++)
        close(f->fds[i]);
    free(f);
}

/* ---------------------------------------------------------------------
 * page
 * ---------------------------------------------------------------------
 */

uint16_t
pgh_page_nitems(const uint8_t *page)
{
    uint16_t    lower = pgo_read_u16(page, PGO_PAGEHDR_LOWER);

    if (lower <= PGO_PAGEHDR_SIZE)
        return 0;
    return (lower - PGO_PAGEHDR_SIZE) / PGO_ITEMID_SIZE;
}

/* ---------------------------------------------------------------------
 * scan cursor
 * ---------------------------------------------------------------------
 */

int
pgh_scan_open(pgh_scan **out, pgh_file *f, const pgh_tupdesc *d,
              uint32_t blk_begin, uint32_t blk_end, pgh_err *e)
{
    pgh_scan   *s;
    uint32_t    nb;

    s = calloc(1, sizeof *s);
    if (!s)
        return pgh_errf(e, "out of memory");
    s->page = malloc(PGH_BLCKSZ);
    if (!s->page)
    {
        free(s);
        return pgh_errf(e, "out of memory");
    }

    s->f = f;
    s->d = d;
    s->e = e;

    /* the range is clamped to the file: [blk_begin, min(blk_end, nblocks)) */
    nb = pgh_file_nblocks(f);
    s->blk = blk_begin;
    s->blk_end = blk_end < nb ? blk_end : nb;

    *out = s;
    return 0;
}

const pgh_stats *
pgh_scan_stats(const pgh_scan *s)
{
    return &s->st;
}

void
pgh_scan_close(pgh_scan *s)
{
    if (!s)
        return;
    free(s->page);
    free(s);
}
