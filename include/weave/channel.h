/*-------------------------------------------------------------------------
 *
 * channel.h -- the shuttle contract every retrieval channel implements
 *
 * A pg_weave index is a sequence of bolts (immutable segments).  Within a bolt,
 * every document has a dense segment-local id -- its position on the "warp".
 * A "weft" is one retrieval channel woven across that warp: lexical postings,
 * token positions, quantized vector codes, the vector proximity graph, the
 * vocabulary trigram map, the SuRF trie, or docvalues.  A "shuttle" is a cursor
 * that carries one weft across the warp during a scan.
 *
 * This header defines the only interface the fused top-k scorer knows about.
 * The scorer has no idea what BM25 is, what a quantizer is, or what a trie is.
 * It advances shuttles, asks them for bounds, and prunes.  That decoupling is
 * what makes the algorithm in doc/specs/FUSED_TOPK.md channel-agnostic, and it
 * is why adding a channel does not touch the scorer.
 *
 * THE CONTRACT.  Read this before implementing a channel.  Violating it does
 * not produce a slow query, it produces silently missing rows.
 *
 *   (C1) MONOTONE SEEK.  seek(s, t) returns the smallest warp position p >= t
 *        at which this channel could contribute, or WEAVE_WARP_END.  It must
 *        never return a position less than the previous return value.
 *
 *   (C2) TRUE UPPER BOUND.  block_max(s) returns a value that is >= score(s)
 *        for EVERY warp position in [s->cur, s->blkend].  Not "usually", not
 *        "in expectation".  The pruning proof in doc/specs/FUSED_TOPK.md is a
 *        chain of inequalities and a bound that is ever too low breaks it.
 *        For a quantized channel this means the bound must be computed from the
 *        code-domain extremes, not from a sampled or estimated distance.
 *
 *   (C3) CHEAP BOUND.  block_max(s) must not read a buffer.  It reads state the
 *        shuttle already has from its current block header.  If computing your
 *        bound needs I/O, your block granularity is wrong.
 *
 *   (C4) EXACT SCORE.  score(s) returns this channel's exact contribution at
 *        s->cur.  It may read buffers.  It is called at most once per surviving
 *        warp position per channel.
 *
 *   (C5) GATES USE INFINITIES.  A boolean channel (fuzzy match, scalar
 *        predicate) has no score.  It reports block_max() == +INF when the
 *        block may contain a match and -INF when it provably cannot, and
 *        score() == 0.0 for a match / -INF for a non-match.  The scorer's
 *        arithmetic then does the right thing without a special case.
 *
 *   (C6) LIVEDOCS ARE NOT YOUR JOB.  Tombstones are applied once, by the
 *        scorer, from the bolt's shared livedocs bitmap.  A channel must never
 *        consult livedocs itself; doing so is redundant work and, worse, hides
 *        a class of bug where two channels disagree about visibility.
 *
 * Every channel MUST have a property test in test/hegel/ asserting (C1) and
 * (C2) against its own score() on random input.  See doc/TESTING.md.  A channel
 * without that test is not merged.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/channel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_CHANNEL_H
#define WEAVE_CHANNEL_H

#include "postgres.h"

#include "weave/am.h"

/*
 * A position on the warp: a dense, segment-local document id in [0, nlive).
 *
 * Deliberately NOT a ctid.  Cross-modal skipping is only possible because every
 * channel in a bolt agrees on this integer, and it is only cheap because the
 * integer is dense and small enough to index a bitmap directly.  The
 * warp -> ctid translation happens exactly once, in the scorer, after a row has
 * survived the heap.
 */
typedef uint32 WeaveWarp;

#define WEAVE_WARP_END		((WeaveWarp) 0xFFFFFFFF)

/* An unreachable-block / non-matching sentinel, per (C5). */
#define WEAVE_SCORE_NEVER	(-get_float4_infinity())
#define WEAVE_SCORE_ALWAYS	(get_float4_infinity())

/*
 * Which weft a shuttle is carrying.  Used for EXPLAIN output, for the per-
 * channel counters in weave_scan_stats(), and by the scorer to pick an
 * essential/non-essential split heuristic (see FUSED_TOPK.md sect. 5).
 */
typedef enum WeaveChannelKind
{
	WEAVE_CH_LEXICAL = 0,		/* BM25 over FOR-packed postings */
	WEAVE_CH_POSITION,			/* phrase/NEAR over the lazily-decoded 4th column */
	WEAVE_CH_VECTOR_SCAN,		/* quantized code scan, 32-lane blocks */
	WEAVE_CH_VECTOR_GRAPH,		/* Vamana traversal over quantized codes */
	WEAVE_CH_FUZZY,				/* vocabulary funnel: trigram + SuRF + Levenshtein */
	WEAVE_CH_REGEX,				/* regex AST -> trigram tiling -> vocabulary funnel */
	WEAVE_CH_DOCVALS,			/* scalar / facet predicate from docvalues */
	WEAVE_CH_CGRAM,				/* opt-in corpus-level character trigrams */
	WEAVE_NUM_CHANNEL_KINDS
} WeaveChannelKind;

typedef struct WeaveShuttle WeaveShuttle;

/*
 * The three-function vtable.  Kept separate from WeaveShuttle so a channel with
 * many concurrent shuttles (one per query term, typically) shares one static
 * const vtable rather than copying three pointers per term.
 */
typedef struct WeaveShuttleOps
{
	WeaveWarp	(*seek) (WeaveShuttle *s, WeaveWarp target);
	float4		(*block_max) (WeaveShuttle *s);
	float4		(*score) (WeaveShuttle *s);

	/* Optional.  Non-NULL only for channels that can enumerate a whole block's
	 * worth of candidates more cheaply than one seek() per position -- the
	 * vector code scan does 32 lanes in a few SIMD instructions.  When present,
	 * the scorer prefers it.  Writes at most nwarp entries into out[] and
	 * returns the count. */
	int			(*score_block) (WeaveShuttle *s, WeaveWarp first, int nwarp,
								const uint64 *allow, float4 *out);

	/* Optional.  Non-NULL only for WEAVE_CH_VECTOR_GRAPH: hand the traversal a
	 * warp-indexed allowlist so the greedy descent is steered into the
	 * surviving region instead of post-filtered.  This is the mechanism behind
	 * claim 3 in doc/ARCHITECTURE.md sect. 9. */
	void		(*set_visit_filter) (WeaveShuttle *s, const uint64 *allow,
									 WeaveWarp nwarp);

	void		(*end) (WeaveShuttle *s);
} WeaveShuttleOps;

struct WeaveShuttle
{
	const WeaveShuttleOps *ops;
	WeaveChannelKind kind;

	/* Current position, and the last warp position covered by the block the
	 * shuttle is presently sitting on.  block_max() bounds the closed interval
	 * [cur, blkend].  A channel with no natural blocking sets blkend = cur,
	 * which makes its bound degenerate to a per-document bound -- correct, just
	 * not useful for skipping. */
	WeaveWarp	cur;
	WeaveWarp	blkend;

	/* Static per-shuttle score ceiling over the WHOLE bolt.  Used for the
	 * essential/non-essential partition before the scan starts (MaxScore).
	 * Must satisfy maxscore >= block_max() everywhere. */
	float4		maxscore;

	/* User-supplied weight from the fuse() call.  The scorer multiplies both
	 * score() and block_max() by this, so a channel implementation must NOT
	 * apply it itself -- doing so double-counts and, because it also scales the
	 * bound, does so undetectably. */
	float4		weight;

	/* Counters, reported by weave_scan_stats() and EXPLAIN (ANALYZE, VERBOSE).
	 * Maintained by the scorer, not the channel. */
	int64		nseek;
	int64		nscore;
	int64		nblkskip;

	void	   *state;			/* channel-private */
};

/* Convenience wrappers.  Apply the weight in exactly one place, here, so that
 * (C2) survives weighting: w * bound >= w * score for w >= 0, and w < 0 is
 * rejected at parse time. */
static inline WeaveWarp
weave_shuttle_seek(WeaveShuttle *s, WeaveWarp target)
{
	s->nseek++;
	return s->ops->seek(s, target);
}

static inline float4
weave_shuttle_block_max(WeaveShuttle *s)
{
	return s->weight * s->ops->block_max(s);
}

static inline float4
weave_shuttle_score(WeaveShuttle *s)
{
	s->nscore++;
	return s->weight * s->ops->score(s);
}

#endif							/* WEAVE_CHANNEL_H */
