/*-------------------------------------------------------------------------
 *
 * snudbx.c
 *	  Observability functions for the buffer pools.
 *
 * contrib/snudbx/snudbx.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(snudbx_pool_count);

/*
 * Number of buffer pools the server was started with.
 */
Datum
snudbx_pool_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(NBufferPools);
}
