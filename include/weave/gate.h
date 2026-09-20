/*-------------------------------------------------------------------------
 *
 * gate.h -- the boolean-gate shuttle: contract (C5) over a sorted key set
 *
 * Fuzzy, regex, prefix and LIKE are PREDICATES, not scores.  A route answers
 * them by producing the set of documents that satisfy the predicate --
 * weave_fuzzy_terms() and weave_regex_terms() in src/am/amscan.c each yield a
 * sorted, de-duplicated TidSet -- and what the fused scorer needs from such a
 * channel is only "does document p pass?", asked in warp order.  Under (C5) of
 * include/weave/channel.h that is:
 *
 *		block_max()  =  +INF   the block may contain a match
 *		             =  -INF   it provably cannot
 *		score()      =   0.0   at a match
 *		             =  -INF   at a non-match
 *
 * and the scorer's arithmetic does the right thing with no special case: -INF
 * propagates through the weighted sum and the document is discarded.  There is
 * no bound-tightness work to do -- +/-INF is EXACTLY tight for a predicate --
 * which is why doc/specs/FUZZY_CHANNEL.md sect. 4 calls this the cheapest
 * channel to add, and why the whole thing is a cursor over a sorted array.
 *
 * WHY THE BLOCK IS DEGENERATE, AND WHY THAT IS NOT A WEAKNESS HERE.  channel.h
 * says a channel with no natural blocking sets blkend = cur, making its bound
 * "correct, just not useful for skipping".  True for a scored channel, where
 * the skip comes from block_max() being below the threshold over a RANGE.  For
 * a gate the skip is the seek itself: seek(t) lands on the smallest key >= t,
 * and every position in (t, key) is a non-match the scorer never has to ask
 * about.  A wider block would let block_max() say +INF over an interval that is
 * mostly non-matches -- weaker, not stronger.  So cur == blkend, block_max() is
 * +INF at a key and -INF once the keys are exhausted, and (C2) holds trivially:
 * the closed interval [cur, blkend] is one position, and score() there is 0.0.
 *
 * KEY-SPACE AGNOSTIC, and the open question that makes it so.  The cursor
 * walks caller-supplied uint64 keys and does not care what they mean.  Today
 * the only producer of a key set is a TidSet, so the keys are sparse docids
 * from weave_tid_to_docid() (include/weave/am.h).  The vector weft's warp is a
 * DENSE lane index with a WEAVE_PK_VWARP chain mapping it to those docids
 * (doc/specs/VECTOR_CHANNEL.md, "warp -> docid"), so the two key spaces are NOT
 * the same, and reconciling them is Phase F's decision, not this task's.  If F
 * settles on the warp, the translation is a one-line map at construction and
 * nothing below changes.
 *
 * THE WIDTH DECISION, stated so F inherits it as a fact and not a surprise.
 * WeaveWarp is uint32 and a docid is uint64.  The cursor stores uint64 keys and
 * exposes positions as WeaveWarp ONLY IF every key fits; a key that does not is
 * refused at construction with an ERROR naming it (weave_gate_check_keys()).
 * Widening WeaveWarp in channel.h is F's call and is deliberately not made here.
 * The bound: docid = block * MaxHeapTuplesPerPage + offset, so the first docid
 * that does not fit is at heap block UINT32_MAX / MaxHeapTuplesPerPage; with
 * 8 KiB pages MaxHeapTuplesPerPage is 291, so that is block 14,759,337 -- a heap
 * of about 113 GiB.  A table that large with a fuzzy or regex predicate is
 * exactly the case that forces F to choose the warp space (or widen the type),
 * and this refusal is where it will surface, as an error rather than a wrong
 * answer.  0xFFFFFFFF itself is also refused, because it IS WEAVE_WARP_END and
 * a key equal to the end sentinel could not be returned by seek().
 *
 * WHO CALLS THIS.  Nothing in the query path yet, on purpose.  There is no fused
 * scorer to drive a shuttle (AGENTS.md hard rule 7), and exposing a SQL probe
 * for the sake of having a caller is more surface than the value warrants.  The
 * property test in test/hegel/test_bounds.c IS this task's gate
 * (FUZZY_CHANNEL.md sect. 8, row Z7), and the shuttle's first real caller is
 * Phase F.  Until then the standalone core below is what is tested, and the
 * backend glue in src/query/gate.c is a thin ereport() skin over it.
 *
 * TWO HALVES, ONE FILE.  The cursor core is header-only inline with no backend
 * dependency, in the pattern of include/weave/uleven.h, so the property test
 * compiles it with nothing but -I include and can run millions of checks.  The
 * WeaveShuttle face -- which needs get_float4_infinity(), MemoryContext and
 * ereport() -- is declared under POSTGRES_H and defined in src/query/gate.c.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/gate.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_GATE_H
#define WEAVE_GATE_H

#include <stddef.h>
#include <stdint.h>

/*
 * PostgreSQL's c.h defines uint32/uint64; only supply the weave_gt_* aliases
 * from <stdint.h> when compiled outside the backend.  Mirrors weave/uleven.h.
 */
#ifndef POSTGRES_H
typedef uint32_t weave_gt_uint32;
typedef uint64_t weave_gt_uint64;
#else
typedef uint32 weave_gt_uint32;
typedef uint64 weave_gt_uint64;
#endif

/*
 * The core's end-of-keys sentinel.  MUST equal WEAVE_WARP_END in channel.h; the
 * glue asserts it at compile time.  Kept as its own name so the standalone test
 * does not need channel.h.
 */
#define WEAVE_GATE_END			((weave_gt_uint32) 0xFFFFFFFF)

typedef enum WeaveGateError
{
	WEAVE_GATE_OK = 0,
	WEAVE_GATE_UNSORTED,		/* keys[i] < keys[i-1] at bad = i */
	WEAVE_GATE_DUPLICATE,		/* keys[i] == keys[i-1] at bad = i */
	WEAVE_GATE_TOO_WIDE,		/* keys[i] >= WEAVE_GATE_END at bad = i */
	WEAVE_GATE_BACKWARD			/* seek target below the last position returned */
} WeaveGateError;

/*
 * A monotone cursor over a strictly ascending key array.  `pos` is the index
 * of the current key, or nkeys once exhausted.  `seeked` and `last` implement
 * the backward-seek refusal: after the first seek, a target below `last` -- the
 * position the previous seek RETURNED, not the one it was given -- is refused,
 * exactly as V8's vector shuttle does and for the reason channel.h records
 * under "NOT IDEMPOTENT".  The array is borrowed; the owner is the glue.
 */
typedef struct WeaveGateCursor
{
	const weave_gt_uint64 *keys;
	weave_gt_uint32 nkeys;
	weave_gt_uint32 pos;
	weave_gt_uint32 last;
	int			seeked;
} WeaveGateCursor;

/*
 * Validate a key array for the cursor: strictly ascending, and every key below
 * WEAVE_GATE_END.  On failure, *bad is the offending index.  O(n), once, at
 * construction; the alternative -- trusting the caller -- turns a sort bug in a
 * producer into a seek that silently skips keys, which is the silent-missing-
 * rows failure the whole contract exists to prevent.
 */
static inline WeaveGateError
weave_gate_check_keys(const weave_gt_uint64 *keys, weave_gt_uint32 nkeys,
					  weave_gt_uint32 *bad)
{
	weave_gt_uint32 i;

	for (i = 0; i < nkeys; i++)
	{
		if (keys[i] >= (weave_gt_uint64) WEAVE_GATE_END)
		{
			if (bad)
				*bad = i;
			return WEAVE_GATE_TOO_WIDE;
		}
		if (i > 0 && keys[i] <= keys[i - 1])
		{
			if (bad)
				*bad = i;
			return keys[i] == keys[i - 1] ? WEAVE_GATE_DUPLICATE
				: WEAVE_GATE_UNSORTED;
		}
	}
	return WEAVE_GATE_OK;
}

/* Position the cursor before the first key.  `keys` must have passed
 * weave_gate_check_keys(); the cursor does not re-validate. */
static inline void
weave_gate_init(WeaveGateCursor *c, const weave_gt_uint64 *keys,
				weave_gt_uint32 nkeys)
{
	c->keys = keys;
	c->nkeys = nkeys;
	c->pos = 0;
	c->last = 0;
	c->seeked = 0;
}

/* True while the cursor sits on a key, i.e. block_max() is +INF. */
static inline int
weave_gate_on_key(const WeaveGateCursor *c)
{
	return c->pos < c->nkeys;
}

/*
 * (C5) as two integer predicates the glue maps to infinities, so the decision
 * is HERE, under the property test, and the glue only translates.
 *
 * block_may_match: +INF if true, -INF if false -- the block [cur, blkend] is
 * one position, and that position is a key whenever the cursor is on one.
 * matches(pos): 0.0 if true, -INF if false -- exact at `pos`, which the glue
 * passes as s->cur.  Both are pure reads of state the cursor holds (C3).
 */
static inline int
weave_gate_block_may_match(const WeaveGateCursor *c)
{
	return weave_gate_on_key(c);
}

static inline int
weave_gate_matches(const WeaveGateCursor *c, weave_gt_uint32 pos)
{
	return weave_gate_on_key(c) && c->keys[c->pos] == (weave_gt_uint64) pos;
}

/*
 * (C1): advance to the smallest key >= target, or WEAVE_GATE_END.
 *
 * Galloping from the current position rather than a binary search over the
 * whole array: a fused loop's targets are non-decreasing and mostly near, so
 * the expected cost is O(log gap), not O(log n), and a run of consecutive
 * targets is O(1) each.  The doubling probe stops at the first key >= target,
 * then a binary search settles inside the last (lo, hi] window.
 *
 * A backward target is refused, not clamped.  `target < c->last` after a seek
 * would have to answer with a position the cursor has already passed, and
 * returning c->last instead would let a fused-loop bug become a wrong answer
 * rather than an error (the V8 rationale, src/vector/vecshuttle.c).  The
 * cursor is left unchanged on refusal.
 */
static inline WeaveGateError
weave_gate_seek(WeaveGateCursor *c, weave_gt_uint32 target,
				weave_gt_uint32 *out)
{
	weave_gt_uint64 t = (weave_gt_uint64) target;
	weave_gt_uint32 lo;
	weave_gt_uint32 hi;
	weave_gt_uint32 step;

	if (c->seeked && target < c->last)
		return WEAVE_GATE_BACKWARD;
	c->seeked = 1;

	/* Gallop: find the first probe at or past the target. */
	lo = c->pos;
	hi = lo;
	step = 1;
	while (hi < c->nkeys && c->keys[hi] < t)
	{
		lo = hi + 1;
		if (step > c->nkeys - hi)
			hi = c->nkeys;
		else
			hi += step;
		step <<= 1;
	}

	/* Now keys[lo-1] < t (or lo == c->pos) and (hi == nkeys or keys[hi] >= t):
	 * binary-search the smallest index in [lo, hi] whose key is >= t. */
	while (lo < hi)
	{
		weave_gt_uint32 mid = lo + (hi - lo) / 2;

		if (c->keys[mid] < t)
			lo = mid + 1;
		else
			hi = mid;
	}
	c->pos = lo;

	if (lo >= c->nkeys)
	{
		c->last = WEAVE_GATE_END;
		*out = WEAVE_GATE_END;
		return WEAVE_GATE_OK;
	}
	c->last = (weave_gt_uint32) c->keys[lo];
	*out = c->last;
	return WEAVE_GATE_OK;
}

/* ---------------------------------------------------------------------------
 * The WeaveShuttle face -- backend only (src/query/gate.c)
 * ------------------------------------------------------------------------- */
#ifdef POSTGRES_H

#include "utils/memutils.h"

#include "weave/am.h"
#include "weave/channel.h"

/*
 * Open a gate shuttle over `keys`, which must be strictly ascending and fit a
 * WeaveWarp; otherwise ERROR (see the header).  The keys are COPIED into the
 * shuttle's own memory context, a child of `cxt`, so the caller may free its
 * array the moment this returns and an ERROR between begin() and end()
 * reclaims everything at abort without end() running.  `kind` is recorded for
 * EXPLAIN and the per-channel counters and must be a gate kind -- one whose
 * contract is (C5): WEAVE_CH_FUZZY, WEAVE_CH_REGEX, WEAVE_CH_DOCVALS or
 * WEAVE_CH_CGRAM.  Prefix and LIKE routes report under the kind that produced
 * their key set.  A scored kind is refused: its counters would be lying.
 * maxscore is +INF if
 * nkeys > 0 and -INF otherwise; weight is 1.0 and is applied by the wrappers
 * in channel.h, never here.
 */
extern WeaveShuttle *weave_gate_shuttle_begin(WeaveChannelKind kind,
											  const uint64 *keys, int nkeys,
											  MemoryContext cxt);

/* The same, keyed by weave_tid_to_docid() over a sorted, de-duplicated
 * TidSet -- today's only producer of a gate's key set. */
extern WeaveShuttle *weave_gate_shuttle_from_tidset(WeaveChannelKind kind,
													const TidSet *ts,
													MemoryContext cxt);

#endif							/* POSTGRES_H */

#endif							/* WEAVE_GATE_H */
