/*-------------------------------------------------------------------------
 *
 * pgheap_numeric.c
 *    The numeric header.
 *
 * Built from pgheap.h and pg_ondisk_format.h only, without the PostgreSQL
 * headers.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "pgheap.h"
#include "pg_ondisk_format.h"

int
pgh_decode_numeric(const uint8_t *d, int32_t len, pgh_numeric *out,
                   pgh_err *e)
{
    uint16_t    h;

    if (len < PGO_NUMERIC_HEADER_SHORT)
        PGH_FAIL(e, "numeric shorter than header");
    memcpy(&h, d, sizeof(h));
    memset(out, 0, sizeof *out);

    if ((h & PGO_NUMERIC_SIGN_MASK) == PGO_NUMERIC_SPECIAL)
    {
        /* NaN / +-Inf: the dscale bits of the short form are still there */
        if (h == PGO_NUMERIC_NAN)
            out->special = PGH_NUM_NAN;
        else if (h == PGO_NUMERIC_PINF)
            out->special = PGH_NUM_PINF;
        else if (h == PGO_NUMERIC_NINF)
            out->special = PGH_NUM_NINF;
        else
            PGH_FAIL(e, "unknown numeric special header 0x%04x", h);
        out->dscale = (h & PGO_NUMERIC_SHORT_DSCALE_MASK) >>
            PGO_NUMERIC_SHORT_DSCALE_SHIFT;
        out->negative = out->special == PGH_NUM_NINF;
        return 0;
    }

    if (h & PGO_NUMERIC_SHORT)
    {
        /* short form: a 6-bit weight with its own sign bit */
        int         w;

        out->negative = (h & PGO_NUMERIC_SHORT_SIGN_MASK) != 0;
        out->dscale = (h & PGO_NUMERIC_SHORT_DSCALE_MASK) >>
            PGO_NUMERIC_SHORT_DSCALE_SHIFT;
        w = h & PGO_NUMERIC_SHORT_WEIGHT_MASK;
        if (h & PGO_NUMERIC_SHORT_WEIGHT_SIGN_MASK)
            w |= ~PGO_NUMERIC_SHORT_WEIGHT_MASK;
        out->weight = (int16_t) w;
        out->digits = d + PGO_NUMERIC_HEADER_SHORT;
        out->ndigits = (len - PGO_NUMERIC_HEADER_SHORT) / 2;
    }
    else
    {
        /* long form: the weight is a separate int16 */
        int16_t     w;

        if (len < PGO_NUMERIC_HEADER_LONG)
            PGH_FAIL(e, "long numeric shorter than header");
        out->negative = (h & PGO_NUMERIC_SIGN_MASK) == PGO_NUMERIC_NEG;
        out->dscale = h & PGO_NUMERIC_DSCALE_MASK;
        memcpy(&w, d + PGO_NUMERIC_HEADER_SHORT, sizeof(w));
        out->weight = w;
        out->digits = d + PGO_NUMERIC_HEADER_LONG;
        out->ndigits = (len - PGO_NUMERIC_HEADER_LONG) / 2;
    }

    if (out->ndigits < 0)
        PGH_FAIL(e, "numeric ndigits negative");
    return 0;
}
