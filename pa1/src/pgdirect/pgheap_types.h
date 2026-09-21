/*-------------------------------------------------------------------------
 *
 * pgheap_types.h
 *    The two handle structures, shared by pgheap_reader.c (the functions
 *    of the assignment) and pgheap_support.c (the provided ones).
 *
 * Internal to the reader: nothing outside src/pgdirect includes it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGHEAP_TYPES_H
#define PGHEAP_TYPES_H

#include "pgheap.h"

/*
 * PGH_MAXSEG caps how many segments we open (PGH_MAXSEG * 1 GB per
 * relation); opening beyond it is an error, never a silently truncated
 * scan.
 */
#define PGH_MAXSEG 100

struct pgh_file
{
    char        relpath[4096];          /* path of segment 0, e.g.
                                         * <PGDATA>/base/<dboid>/<relfilenode>;
                                         * ".N" is appended for the others */
    int         nseg;                   /* segment files found and opened */
    int         fds[PGH_MAXSEG];        /* one open descriptor per segment;
                                         * fds[N] belongs to <relpath>.N */
    uint32_t    segblocks[PGH_MAXSEG];  /* blocks in each segment; all but
                                         * the last hold PGO_RELSEG_SIZE */
    uint32_t    nblocks;                /* sum of segblocks; block numbers
                                         * run 0..nblocks-1 */
};

struct pgh_scan
{
    pgh_file   *f;              /* relation file */
    const pgh_tupdesc *d;       /* column layout for deform */
    pgh_err    *e;              /* receives every error message */
    pgh_stats   st;

    uint32_t    blk;            /* next block to load */
    uint32_t    blk_end;        /* one past the last block of the range */

    uint8_t    *page;           /* one PGH_BLCKSZ buffer, reused per block */
    int         page_loaded;    /* 1 while *page holds block blk */
    uint16_t    next_item;      /* next line pointer to look at (1-based) */
    uint16_t    nitems;         /* line pointers on the loaded page */
};

#endif                          /* PGHEAP_TYPES_H */
