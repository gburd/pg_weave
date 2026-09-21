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
 *   (C5) GATES USE INFINITIES, AND DECLARE THEMSELVES.  A boolean channel
 *        (fuzzy match, scalar predicate) has no score.  It reports
 *        block_max() == +INF when the block may contain a match and -INF when it
 *        provably cannot, score() == 0.0 for a match / -INF for a non-match, and
 *        it sets `required` on its shuttle.  The reference implementation is
 *        include/weave/gate.h (task Z7): a cursor over a sorted key set whose
 *        block is one position, because for a predicate the seek IS the skip.
 *
 *        CORRECTED 2026-09-20 BY TASK F1.  This clause used to end "the scorer's
 *        arithmetic then does the right thing without a special case."  It does
 *        not, and the mistake was load-bearing enough to be worth leaving on the
 *        record.  The fused loop sums the CONTRIBUTING channels -- those whose
 *        block covers the pivot -- and a gate that has advanced past the pivot is
 *        not in that set, so its -INF is never added and the predicate is
 *        SILENTLY NOT APPLIED: rows that fail the filter come back, with
 *        plausible scores.  Infinities make a gate's bound exactly tight, which
 *        is all they were ever good for; they do not make a conjunction out of a
 *        weighted sum.  The scorer therefore has a special case, it is the
 *        required/scored split in include/weave/fuse.h note 2, and `required` is
 *        how a channel opts into it.  `kind` cannot carry that meaning: the Z9
 *        `<@>` distance channel in src/query/edist.c is SCORED and labels itself
 *        WEAVE_CH_FUZZY, so inferring the contract from the weft would veto every
 *        row it ranks.
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
	WEAVE_CH_VECTOR_GRAPH,		/* WITHDRAWN: Vamana traversal over quantized codes.
								 * The enumerator is kept so the numbering of the kinds
								 * after it does not shift (they appear in
								 * weave_scan_stats() output), but no channel reports
								 * it and none is planned.  The graph kind was dropped
								 * after pg_turbovec deprecated theirs -- at R@10 >= 0.98
								 * on GIST-10M/960-d, IVF reached 28.4 ms while the graph
								 * could not reach 0.98 at ANY latency (ceiling 0.873 at
								 * 181 ms) and built 57-90x slower.  The ratified vector
								 * shape is a flat 32-lane code scan plus an exact
								 * float32 top-25 rerank.  doc/specs/FUSED_TOPK.md sect. 6,
								 * task F4 (withdrawn). */
	WEAVE_CH_FUZZY,				/* uleven walk over the dictionary (Z5); gate shuttle, gate.h */
	WEAVE_CH_REGEX,				/* core regex engine over the dictionary, trigram-narrowed (Z6); gate shuttle */
	WEAVE_CH_DOCVALS,			/* scalar / facet predicate from docvalues */
	WEAVE_CH_CGRAM,				/* opt-in corpus-level character trigrams */
	WEAVE_NUM_CHANNEL_KINDS
} WeaveChannelKind;

/*
 * A kind's name, for EXPLAIN, for weave_scan_stats() and for the error messages a
 * contract violation produces.  static inline in the header rather than a function
 * in some TU, so it sits beside the enum it names and adding a kind without a name
 * is a compile warning here (no default label) instead of a silent "unknown".
 */
static inline const char *
weave_channel_kind_name(WeaveChannelKind kind)
{
	switch (kind)
	{
		case WEAVE_CH_LEXICAL:
			return "lexical";
		case WEAVE_CH_POSITION:
			return "position";
		case WEAVE_CH_VECTOR_SCAN:
			return "vector-scan";
		case WEAVE_CH_VECTOR_GRAPH:
			return "vector-graph";
		case WEAVE_CH_FUZZY:
			return "fuzzy";
		case WEAVE_CH_REGEX:
			return "regex";
		case WEAVE_CH_DOCVALS:
			return "docvalues";
		case WEAVE_CH_CGRAM:
			return "cgram";
		case WEAVE_NUM_CHANNEL_KINDS:
			break;
	}
	return "unknown";
}

typedef struct WeaveShuttle WeaveShuttle;

/*
 * The three-function vtable.  Kept separate from WeaveShuttle so a channel with
 * many concurrent shuttles (one per query term, typically) shares one static
 * const vtable rather than copying three pointers per term.
 */
typedef struct WeaveShuttleOps
{
	/*
	 * Advance to the first position >= target that this channel can contribute
	 * to, or WEAVE_WARP_END.  Monotone: (C1).
	 *
	 * NOT IDEMPOTENT, and V8's implementation made that concrete: a channel is
	 * entitled to reject a target below its current position outright, because a
	 * forward-only cursor over a page chain cannot honour one and returning the
	 * current position instead would let a fused-loop bug become a wrong answer
	 * rather than an error.  So seek(p) twice is not guaranteed to be legal --
	 * the second call is a backward seek whenever the first returned more than p.
	 * A caller must remember what it was given back.
	 */
	WeaveWarp	(*seek) (WeaveShuttle *s, WeaveWarp target);
	float4		(*block_max) (WeaveShuttle *s);
	float4		(*score) (WeaveShuttle *s);

	/* Optional.  Non-NULL only for channels that can enumerate a whole block's
	 * worth of candidates more cheaply than one seek() per position -- the
	 * vector code scan does 32 lanes in a few SIMD instructions.  When present,
	 * the scorer prefers it.  Writes at most nwarp entries into out[] and
	 * returns the count.
	 *
	 * `nwarp` carries TWO obligations and V8 found them conflated, so they are
	 * spelled out: it is the capacity of out[], AND -- when `allow` is non-NULL --
	 * it fixes the extent the bitmap must cover.  `allow` is indexed by ABSOLUTE
	 * warp, the same convention as weave/kernels.h, so a caller passing `allow`
	 * must guarantee at least `first + nwarp` bits exist.  Saying only "writes at
	 * most nwarp entries" left the bitmap's length unstated next to an index that
	 * comes off a page, which is the unbounded out-of-bounds read that
	 * kernels.h refuses to permit and doc/CONVENTIONS.md rule 2 forbids.  A
	 * channel that cannot satisfy it must raise, not clamp. */
	int			(*score_block) (WeaveShuttle *s, WeaveWarp first, int nwarp,
								const uint64 *allow, float4 *out);

	/* Optional, and CURRENTLY UNFILLED BY EVERY CHANNEL.  It exists for a
	 * WEAVE_CH_VECTOR_GRAPH shuttle: hand the traversal a warp-indexed
	 * allowlist so the greedy descent is steered into the surviving region
	 * instead of post-filtered.
	 *
	 * This slot's comment used to read "this is the mechanism behind claim 3 in
	 * doc/ARCHITECTURE.md sect. 9", and that was CORRECTED 2026-09-20: the graph
	 * channel is withdrawn, so if it were the mechanism, claim 3 would have
	 * none.  Claim 3 -- queries get faster as predicates get more selective --
	 * now rests on the two mechanisms in FUSED_TOPK.md sect. 3 that need no
	 * graph: a boolean gate shuttle narrowing pivot selection, and the block
	 * bound skipping whole intersected block ranges.  Neither is measured yet.
	 * Kept in the vtable because a re-justified graph channel would need it and
	 * removing it would silently change the struct layout of every channel. */
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
	 * bound, does so undetectably.
	 *
	 * The multiplication happens in exactly one place, and as of task F1 that
	 * place is src/am/fuse.c -- the fused core -- not the weave_shuttle_score()
	 * wrappers below, which the fused path deliberately does not use.  A wrapper
	 * AND the core would double-apply it, and because the bound is scaled too,
	 * (C2) would still hold and the scores would simply be wrong. */
	float4		weight;

	/*
	 * TRUE if this channel is a PREDICATE under (C5): it has no score, it reports
	 * +/-INF, and the fused scorer must treat it conjunctively -- the document
	 * must match it, and a position it does not reach is a veto rather than a
	 * zero contribution (doc/specs/FUSED_TOPK.md, include/weave/fuse.h note 2).
	 *
	 * A CHANNEL MUST DECLARE THIS ITSELF; IT IS NOT INFERABLE FROM `kind`, and the
	 * tree already proves it.  src/query/edist.c builds the Z9 `<@>` KNN shuttle
	 * -- a SCORED channel returning a Levenshtein distance -- and labels it
	 * WEAVE_CH_FUZZY, because that is the weft it walks.  Any scorer that decided
	 * "fuzzy means predicate" would apply a conjunctive veto to a distance
	 * channel and silently drop every row it ranked.  So the contract is a field,
	 * the field defaults to false through the MemoryContextAllocZero every
	 * shuttle is built with, and a predicate channel sets it explicitly.
	 */
	bool		required;

	/* Counters, reported by weave_scan_stats() and EXPLAIN (ANALYZE, VERBOSE).
	 * Maintained by the scorer, not the channel. */
	int64		nseek;
	int64		nscore;
	int64		nblkskip;

	void	   *state;			/* channel-private */
};

/*
 * Convenience wrappers.  Apply the weight in exactly one place, here, so that
 * (C2) survives weighting: w * bound >= w * score for w >= 0, and w < 0 is
 * rejected at parse time.
 */
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

/*
 * The bulk path, weighted -- and it did not exist until V8 pointed out that its
 * absence was a trap.  score() and block_max() are weighted by their wrappers
 * above; score_block() is the path the scorer PREFERS, so a fused loop that
 * reached for the fast one got unweighted scores sitting next to weighted
 * bounds, and the arithmetic would have been wrong in the direction that keeps
 * looking plausible.  Weighting here rather than in each channel is the same
 * argument as above: one place, so (C2) survives it.
 *
 * WEAVE_SCORE_NEVER must survive the multiply, which it does for w > 0 and which
 * is why w = 0 has no business reaching a shuttle: 0 * -inf is NaN, and a NaN
 * score compares false against every threshold, so it would be silently dropped
 * rather than skipped.  A zero-weighted channel must be left out of the fusion,
 * not weighted to nothing.
 */
static inline int
weave_shuttle_score_block(WeaveShuttle *s, WeaveWarp first, int nwarp,
						  const uint64 *allow, float4 *out)
{
	int			n;
	int			i;

	if (s->ops->score_block == NULL)
		return -1;

	n = s->ops->score_block(s, first, nwarp, allow, out);
	s->nscore += n;

	if (s->weight != 1.0f)
	{
		for (i = 0; i < n; i++)
			out[i] = s->weight * out[i];
	}
	return n;
}

#endif							/* WEAVE_CHANNEL_H */
