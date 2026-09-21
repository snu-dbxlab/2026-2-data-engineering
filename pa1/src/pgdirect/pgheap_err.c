/*-------------------------------------------------------------------------
 *
 * pgheap_err.c
 *    Error message formatting.
 *
 *-------------------------------------------------------------------------
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pgheap.h"

int
pgh_errf(pgh_err *e, const char *fmt,...)
{
    if (e)
    {
        va_list     ap;

        va_start(ap, fmt);
        vsnprintf(e->msg, sizeof e->msg, fmt, ap);
        va_end(ap);
    }
    return -1;
}

void
pgh_err_append(pgh_err *e, const char *fmt,...)
{
    size_t      n;
    va_list     ap;

    if (!e)
        return;

    n = strlen(e->msg);
    if (n + 2 >= sizeof e->msg)
        return;

    e->msg[n++] = ' ';
    va_start(ap, fmt);
    vsnprintf(e->msg + n, sizeof e->msg - n, fmt, ap);
    va_end(ap);
}
