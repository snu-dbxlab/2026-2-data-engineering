/* contrib/snudbx/snudbx--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION snudbx" to load this file. \quit

CREATE FUNCTION snudbx_pool_count()
RETURNS integer
AS 'MODULE_PATHNAME', 'snudbx_pool_count'
LANGUAGE C STRICT;
