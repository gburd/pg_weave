/*-------------------------------------------------------------------------
 *
 * lev.h
 *		Levenshtein automaton over a sorted term dictionary (src/query/lev.c).
 *
 * The algorithm, and why it is an automaton over the sorted dictionary rather
 * than a trigram funnel with a heap recheck, is documented at the top of
 * src/query/lev.c.  This header exists only because task L1 made lev.c an
 * ordinary translation unit: it used to be #included into src/am/am.c, so
 * src/am/amscan.c could reach weave_lev_match_prefix() without anyone writing
 * the interface down.
 *
 * Sibling of weave/uleven.h, which declares the imported pg_tre unicode
 * Levenshtein routines.  The two are not interchangeable: this one walks a
 * sorted dictionary BYTE-wise and reports a dead prefix so a whole contiguous
 * run of terms can be skipped; uleven.h computes a distance between two given
 * strings.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  include/weave/lev.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_LEV_H
#define WEAVE_LEV_H

#include "postgres.h"

/* Maximum query length the byte-wise automaton handles; longer falls back. */
#define WEAVE_LEV_MAXQ 255

typedef struct WeaveLevAut
{
	const unsigned char *q;		/* query bytes */
	int			m;				/* query length */
	int			k;				/* max edits */
} WeaveLevAut;

/*
 * Match one candidate term and report the DEAD-PREFIX length.  The automaton's
 * row state, the pruning rule and the dead-prefix contract are all described in
 * src/query/lev.c; this is the file's only export.
 */
extern bool weave_lev_match_prefix(const WeaveLevAut *aut,
								   const unsigned char *cand,
								   int candlen, int *deadlen);

#endif							/* WEAVE_LEV_H */
