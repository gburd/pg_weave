---
name: weave-channel
description: Use when implementing, modifying, or debugging a retrieval channel in pg_weave (lexical, vector, fuzzy, regex, docvalues, cgram) — anything that provides a WeaveShuttle. Covers the shuttle contract, deriving a provable block bound, the mandatory property test, claiming a page-kind bit, and wiring the channel into the opclass. Load this before writing any code that implements seek/block_max/score.
---

# Implementing a pg_weave retrieval channel

## The contract, and why it is a correctness contract

`include/weave/channel.h` defines three functions. Read it in full before
continuing. The one that matters:

```c
float4 (*block_max) (WeaveShuttle *s);   /* >= score() for ALL of [cur, blkend] */
```

If `block_max()` is ever lower than an actual `score()` in its range, the fused
scorer in `doc/specs/FUSED_TOPK.md` will prune a document that belonged in the
top-k. The query returns fewer rows, or different rows. It does not error, it
does not warn, and **no fixed-expected-output regression test will catch it** —
the results still look like search results.

This is why every channel must have a property test asserting `bound ≥ score` on
randomized input, and why a channel without one is not merged.

## Deriving a bound: the checklist

Work through these in order. Do not skip to implementation.

1. **Write the score function symbolically.** What exactly does `score()`
   compute at one warp position?
2. **Identify what varies within a block and what is fixed.** The bound comes
   from replacing everything that varies with its extreme value.
3. **Check the bound is computable from the block header alone.** Contract (C3):
   no page reads. If your bound needs I/O, your block granularity is wrong —
   either make blocks bigger or move the needed summary into the header.
4. **Measure whether it prunes anything.** This is the step everyone skips and it
   is the one that decides whether the channel is useful.

On step 4, read `bench/RESULTS_BOUND_PRUNING.md` before you argue with it. The
vector channel's first bound was the natural analogue of block-max WAND's
`max_tf` — mathematically correct, obviously right, and it pruned **0.0 %** of
blocks. The replacement prunes 99.6 %. The difference was invisible to every
correctness test and was found only by measuring the pruning *rate* under
realistic queries.

Two traps from that experience:

- **Do not measure `bound / block_max_score` under random queries.** It looks
  informative and it is not. The right metric is: with θ set by the true top-k,
  what fraction of blocks satisfy `bound ≤ θ`? Equivalently, one minus the
  `score()`-call ratio from `doc/specs/FUSED_TOPK.md` §8.
- **Check whether your bound depends on warp ordering.** The vector bound prunes
  99.6 % when block members are spatially coherent and 0.0 % when they are in
  heap order. If your bound has a similar dependency, it is a *gated
  requirement*, not a tuning note — write it into the spec next to the bound.

Bounds already derived, as worked examples:

| channel | bound | where |
|---|---|---|
| lexical BM25 | `max_tf` + `min_doclen` into the saturation function | `include/weave/am.h` `WeaveBlockHdr`; classic block-max WAND |
| vector | `⟨q,c⟩ + ‖q‖₂·R` (centroid + radius) | `include/weave/quantize.h`, `doc/specs/VECTOR_CHANNEL.md` §6 |
| fuzzy / regex / scalar | `±∞` gate, contract (C5) | `doc/specs/FUZZY_CHANNEL.md` |
| `<@>` edit distance | trigram overlap and length difference give a lower bound on edit distance, hence an upper bound on score | `doc/specs/FUZZY_CHANNEL.md` |

## Boolean channels are the easy case

A channel with no score (a fuzzy match, a scalar predicate) reports
`block_max() = +∞` when the block *may* contain a match and `-∞` when it provably
cannot, and `score() = 0.0` / `-∞`. The scorer's arithmetic then does the right
thing with no special case. There is no tightness work to do, which makes a
boolean channel the cheapest thing to add.

## Skeleton

```c
/*-------------------------------------------------------------------------
 *
 * mychan.c
 *		<one line: what this channel retrieves>
 *
 * Bound: <the formula>.  Derivation and measured pruning rate in
 * doc/specs/<SPEC>.md sect. N.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/<subsys>/mychan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/channel.h"

typedef struct MyChanState
{
	Relation	index;
	int			segno;
	Buffer		buf;			/* the block we are sitting on, or InvalidBuffer */
	/* whatever the current block header gave us -- the bound must come from
	 * HERE, never from a fresh read */
	float4		blk_bound;
} MyChanState;

static WeaveWarp
mychan_seek(WeaveShuttle *s, WeaveWarp target)
{
	MyChanState *st = (MyChanState *) s->state;

	/* (C1): must be monotone.  An assertion is cheap and catches the whole class
	 * of bug where a block boundary is mishandled. */
	Assert(target >= s->cur || s->cur == 0);

	/* advance the block cursor until blkend >= target, updating s->cur,
	 * s->blkend, and st->blk_bound as each new block header is read */

	return s->cur;				/* or WEAVE_WARP_END */
}

static float4
mychan_block_max(WeaveShuttle *s)
{
	/* (C3): no buffer reads here.  Return what seek() already computed. */
	return ((MyChanState *) s->state)->blk_bound;
}

static float4
mychan_score(WeaveShuttle *s)
{
	/* (C4): exact contribution at s->cur.  May read pages.
	 * (C5): return -inf for a non-match if this is a gate channel.
	 * Do NOT apply s->weight -- weave_shuttle_score() does that exactly once.
	 * Do NOT consult livedocs -- (C6), the scorer already did. */
	return 0.0f;
}

static const WeaveShuttleOps mychan_ops = {
	.seek = mychan_seek,
	.block_max = mychan_block_max,
	.score = mychan_score,
	.score_block = NULL,		/* set only if you can do a whole block cheaper */
	.set_visit_filter = NULL,	/* graph channels only */
	.end = mychan_end,
};
```

## Three mistakes the contract exists to prevent

1. **Applying `s->weight` inside `score()`.** `weave_shuttle_score()` already
   multiplies by it. Doing it twice scales the score *and* the bound, so the bug
   is invisible — the bound still dominates, the ranking is just wrong.
2. **Consulting livedocs inside the channel.** Contract (C6): the scorer applies
   tombstones once. A channel that also checks them is doing redundant work, and
   worse, creates a state where two channels can disagree about visibility.
3. **Returning a bound computed from a stale header** after a concurrent merge.
   The metapage `generation` counter exists for this; a scan that sees it change
   restarts. If your channel caches anything across a block boundary, make sure it
   participates.

## Claiming a page-kind bit

Bits are allocated in `doc/specs/SEGMENT_FORMAT.md` and that table is
authoritative. Do not improvise a bit: bits 0–9 are the lexical channel and the
segment machinery, 10–13 vector, 14–17 fuzzy, 18–19 docvalues and cgram. Adding a
kind means updating that table, adding the `#define` next to its siblings, and
extending `weave_check()` with an invariant for the new page type.

Adding a *weft* to a segment also means the segment descriptor must say so, which
is a format bump — see `doc/specs/SEGMENT_FORMAT.md` on v5 channel descriptors.
An index built without your channel must cost zero bytes for it, not empty
structures.

## The mandatory test

`test/hegel/test_bounds.c` (or a channel-specific file) must:

- generate randomized blocks with the same structure the real writer produces;
- compute every lane's exact `score()`;
- assert `block_max() ≥ max(score)` with float slack only;
- assert `seek()` is monotone across a randomized sequence of targets;
- report the pruning rate, so a regression in bound *quality* is visible even
  when correctness holds.

`test/hegel/test_quantize.c` is the worked example: 17,741 checks, links the codec
with no backend, asserts (C1), (C2), and reports tightness. Copy its shape.
