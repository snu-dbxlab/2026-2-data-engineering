/*-------------------------------------------------------------------------
 *
 * pgheap_varlena.c
 *    The bytes of a variable-length value.
 *
 * Built from pgheap.h and pg_ondisk_format.h only, without the PostgreSQL
 * headers.
 *
 *-------------------------------------------------------------------------
 */
#include "pgheap.h"
#include "pg_ondisk_format.h"

int
pgh_varlena_get(const pgh_datum *d, const uint8_t **data, int32_t *len,
                pgh_err *e)
{
    uint8_t     b = d->ptr[0];

    if (PGO_VARATT_IS_1B_E(b))
        PGH_FAIL(e, "TOAST pointer (out of project scope)");

    if (PGO_VARATT_IS_1B(b))
    {
        *data = d->ptr + PGO_VARHDRSZ_SHORT;
        *len = d->len - PGO_VARHDRSZ_SHORT;
        return 0;
    }

    if (PGO_VARATT_IS_4B_C(b))
        PGH_FAIL(e, "compressed value (out of project scope)");

    *data = d->ptr + PGO_VARHDRSZ;
    *len = d->len - PGO_VARHDRSZ;
    return 0;
}
