/*-------------------------------------------------------------------------
 *
 * fuse.h
 *		Fused-threshold top-k across heterogeneous retrieval channels.
 *
 * The one genuinely novel component of pg_weave.  Everything else here is
 * forked, imported, or a C reimplementation of a published algorithm.
 *
 * Read doc/specs/FUSED_TOPK.md before touching src/am/fuse.c, and
 * weave/channel.h before either.  The short version: every existing hybrid
 * retrieval system over-fetches each channel to depth k' >> k and combines with
 * Reciprocal Rank Fusion, which discards score magnitude and forces the
 * over-fetch.  Because every channel here can supply a provable per-block upper
 * bound (channel.h contract C2), the classic block-max WAND argument
 * generalizes: one document-at-a-time loop, one threshold, three levels of
 * pruning, no over-fetch.
 *
 * The scorer knows nothing about BM25, quantizers, or tries.  It advances
 * shuttles, sums bounds, and prunes.  That decoupling is why adding a channel
 * does not touch this file.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/fuse.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_FUSE_H
#define WEAVE_FUSE_H

#include "postgres.h"

#include "weave/channel.h"

/*
 * Upper limit on channels in one fused query.  Query terms each get their own
 * shuttle, so this is not as generous as it looks; it is a bound on the
 * partition array in WeaveFuseState, not a user-facing limit on query
 * complexity.
 */
#define WEAVE_FUSE_MAX_CHANNELS		64

typedef struct WeaveFuseHeapEntry
{
	WeaveWarp	warp;
	float4		score;
} WeaveFuseHeapEntry;

/*
 * State of one fused scan.
 *
 * `order` holds shuttle indexes sorted by descending bolt-wide ceiling
 * (shuttle->maxscore * shuttle->weight).  `split` is the boundary between the
 * ESSENTIAL prefix and the NON-ESSENTIAL suffix, maintained so that the suffix's
 * ceiling sum is <= theta.  Because theta only ever rises, `split` only ever
 * moves in one direction, so the partition is maintained incrementally rather
 * than re-sorted -- see doc/specs/FUSED_TOPK.md sect. 5.
 */
typedef struct WeaveFuseState
{
	int			nchan;
	WeaveShuttle *chan[WEAVE_FUSE_MAX_CHANNELS];

	int			order[WEAVE_FUSE_MAX_CHANNELS];
	float4		ceil[WEAVE_FUSE_MAX_CHANNELS];	/* by shuttle index */
	float4		suffixsum[WEAVE_FUSE_MAX_CHANNELS + 1];	/* by order position */
	int			split;

	int			k;
	float4		theta;			/* heap minimum; -inf until the heap fills */
	int			nheap;
	WeaveFuseHeapEntry *heap;	/* min-heap on score, k entries */

	/* The bolt's tombstone bitmap.  Applied HERE and only here: contract (C6)
	 * forbids a channel from consulting it, so that two channels can never
	 * disagree about visibility. */
	const uint64 *livedocs;
	WeaveWarp	nwarp;

	/* Counters for EXPLAIN (ANALYZE, VERBOSE) and weave_scan_stats().  The
	 * score()-call total is the mechanism metric: doc/specs/FUSED_TOPK.md sect. 8
	 * gates on it, because a latency win without a call-count win means the
	 * bounds are loose and the result will not survive a different corpus. */
	int64		npivot;
	int64		nblkskip;
	int64		nabandon;
	int64		nscore;

	MemoryContext ctx;
} WeaveFuseState;

extern WeaveFuseState *weave_fuse_begin(MemoryContext ctx, int k,
										WeaveShuttle **chan, int nchan,
										const uint64 *livedocs, WeaveWarp nwarp);

/*
 * Run the loop to completion and return the top k in DESCENDING score order.
 * Returns the count, which may be < k.
 *
 * Degenerate cases must reduce exactly, and sql/fuse_degenerate.sql asserts it:
 * one lexical channel is byte-identical to today's ranked scan; all-boolean
 * channels are a bitmap AND with no scoring; k = 1 is best-match with maximal
 * pruning.  If a degenerate case differs from the path it replaces, the general
 * case is wrong too and the difference is just easier to see.
 */
extern int	weave_fuse_run(WeaveFuseState *fs, WeaveWarp *out_warp,
						   float4 *out_score);

extern void weave_fuse_end(WeaveFuseState *fs);

/*
 * Per-channel breakdown for one returned row, for debugging relevance.  Costs
 * one extra score() call per channel and is NEVER called during pruning -- it
 * must not perturb the counters the gate is measured against.
 */
extern void weave_fuse_score_parts(WeaveFuseState *fs, WeaveWarp warp,
								   float4 *out, int *nout);

#endif							/* WEAVE_FUSE_H */
