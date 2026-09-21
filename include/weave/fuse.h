/*-------------------------------------------------------------------------
 *
 * fuse.h -- the fused-threshold top-k scorer: doc/specs/FUSED_TOPK.md sect. 3
 *
 * This is the project's novel algorithm and claim 2 of doc/ARCHITECTURE.md
 * sect. 9: one threshold, one pass, several channels, instead of running each
 * channel to k' = k * oversample and reconciling ranks afterwards (RRF).  The
 * scorer knows nothing about BM25, quantizers, tries or regexes.  It advances
 * shuttles (include/weave/channel.h), asks them for bounds, and prunes.
 *
 * THIS FILE REPLACED A DESIGN-STAGE HEADER, AND THE SHAPE CHANGED.  The version in
 * commit cba5ae6 declared a backend-coupled API -- `weave_fuse_begin(MemoryContext,
 * ...)` over `WeaveShuttle *chan[]`, with `weave_fuse_run()` writing warp and score
 * arrays and `weave_fuse_end()` freeing.  That shape is unbuildable by a bare
 * compiler, which the property test requires (see below), so the scan state is now a
 * caller-owned struct of plain types, the shuttle layer moved to
 * src/am/fuseshuttle.c, and `MemoryContext` left the interface entirely.  Three
 * names from the old header are deliberately kept alive because they name real
 * obligations rather than implementation: the 64-channel cap (`WEAVE_FUSE_MAX_CHAN`,
 * was `WEAVE_FUSE_MAX_CHANNELS`), the single `int split` partition boundary, and the
 * score()-call counter that FUSED_TOPK.md sect. 8 gates on -- now per channel rather
 * than one total, since the ratio is only interpretable per channel.  The one
 * declaration NOT carried over is `weave_fuse_score_parts()`: that is task F3, which
 * is blocked behind F2, and a declaration with no implementation and no caller is how
 * a stale API outlives the design that wanted it.
 *
 * WHY THE CORE IS BACKEND-FREE, AND IT IS HARD RULE 7's MITIGATION.  Phase F is
 * being started under a scoped waiver of AGENTS.md hard rule 7 (see doc/PHASES.md
 * "Phase F"), whose argument against starting early is that a wrong answer in a
 * fused scan has several possible causes and you chase the wrong one.  The
 * mitigation is structural: everything that can produce a wrong answer -- pivot
 * selection, the partition, the block prune, incremental abandonment, the top-k
 * heap and its tie-break -- is here, driven by plain structs, and is linked by
 * test/hegel/test_fuse_props.c with a bare compiler against a brute-force
 * reference over SYNTHETIC shuttles.  A failure is localized to the scorer by
 * construction and never needs a live channel, a heap or a page cache to
 * reproduce.  If debugging F ever requires a real channel, that is the signal to
 * stop and close the Z and V gates first.  The seam follows
 * include/weave/vecscan.h (V8) and include/weave/gate.h (Z7).
 *
 * WHAT THE SCORER GETS WRONG IF IT FOLLOWS THE SPEC LITERALLY.  Four things,
 * found while implementing sect. 3 and recorded next to the code that avoids
 * them, because three of the four are silent:
 *
 *   1. PIVOT SELECTION AS WRITTEN PERFORMS A BACKWARD SEEK.  The spec says
 *      "p <- min over ESSENTIAL i of shuttle_i.seek(p)".  If channel A answers
 *      50 and channel B answers 10, the pivot is 10, and the next iteration
 *      calls A->seek(11) -- below the 50 that A already returned.  channel.h
 *      says under "NOT IDEMPOTENT" that a channel is entitled to REFUSE such a
 *      target, and both implemented shuttles do (gate.h returns
 *      WEAVE_GATE_BACKWARD, V8's raises).  So the scorer must remember each
 *      channel's last returned position and seek only the channels standing
 *      below the target.  That is what weave_fuse_pivot() does.  This is not an
 *      optimization; the literal loop errors out on the second iteration of any
 *      multi-channel query where the channels are not aligned.
 *
 *   2. SECT. 3 HAS NO NOTION OF A BOOLEAN CHANNEL, AND (C5)'s PROMISE THAT ONE
 *      NEEDS NO SPECIAL CASE IS FALSE.  This is the serious one, and it is a
 *      correction to channel.h rather than to the spec's prose.  (C5) says a gate
 *      reports score() == -INF at a non-match and "the scorer's arithmetic then
 *      does the right thing without a special case".  It does not.  Sect. 3 sums
 *      only the CONTRIBUTING channels -- those with cur <= p <= blkend -- and a
 *      gate that has advanced past the pivot is not in that set, so its -INF is
 *      never added and the predicate is SILENTLY NOT APPLIED.  Rows that fail the
 *      filter come back, and they come back with plausible scores.  The -INF
 *      convention only works if every gate is evaluated at every candidate
 *      position, which is another way of saying gates are CONJUNCTIVE, which is
 *      not what a weighted sum of contributions expresses.
 *
 *      So the core splits channels into REQUIRED and SCORED, and the split is
 *      what makes the algorithm correct rather than a tuning choice:
 *
 *        - REQUIRED (a gate, contract (C5)).  The document must match.  Required
 *          channels drive the pivot by INTERSECTION -- p advances to the max of
 *          their positions until they all agree -- and they are excluded from the
 *          partition and from the block-prune sum, because at a position they all
 *          match they contribute 0.0, and at one they do not the running sum is
 *          -INF and the document is gone.  Excluding them from `ub` is what keeps
 *          the block prune alive at all: a gate's block_max() is +INF, so adding
 *          it would make ub infinite and no block would ever be skipped.
 *        - SCORED.  Additive.  A scored channel that does not reach p contributes
 *          nothing, i.e. 0, which is exactly what "absent from the contributing
 *          set" already means.  MaxScore's partition, the block prune and
 *          incremental abandonment all apply to these and only these.
 *
 *      A required channel is still asked for score() at a position it reaches,
 *      and must be: (C5) permits a COARSE gate whose block says "may match" and
 *      whose score() is the per-document confirmation -- a docvalues predicate
 *      over a block of values is exactly that -- so cur == p means "candidate",
 *      not "match".  gate.h's cursor is the exact case, where the confirmation is
 *      a comparison it has already made, and paying for it uniformly is cheaper
 *      than a second contract clause distinguishing the two.
 *
 *      THE PAYOFF, AND IT IS WHERE CLAIM 3 ACTUALLY LIVES.  Conjunctive
 *      advancement is strictly stronger than the alternative of leaning on the
 *      +INF ceiling to keep gates "essential": the pivot jumps to the maximum of
 *      the required positions, so a selective predicate skips the scan forward
 *      past everything between its keys, and the scored channels are seeked
 *      straight there.  That is claim 3 of doc/ARCHITECTURE.md sect. 9 -- queries
 *      get FASTER as predicates get more selective -- and it needed a mechanism
 *      after the filter-steered graph traversal was withdrawn with the graph
 *      channel (FUSED_TOPK.md sect. 6, task F4).  It is UNMEASURED; F's benchmark
 *      is what tests it, and the counters below are what it reads.
 *
 *   3. THREE NaN TRAPS, ALL OF WHICH EXIST BECAUSE (C5) PUTS INFINITIES IN AN
 *      ADDITIVE EXPRESSION.  0 * INF (a zero weight on a gate's +INF ceiling);
 *      INF - INF (sect. 3 maintains `remaining` by SUBTRACTING each scored
 *      channel's bound from ub, which with two contributing gates subtracts INF
 *      from INF); -INF + INF (the abandonment test "S + remaining <= theta" with
 *      S already -INF and an unscored gate leaving remaining at +INF).  Every one
 *      of them evaluates false against theta, so every one of them silently
 *      DISABLES a prune while leaving the answers plausible.
 *
 *      The required/scored split removes all three by keeping infinities out of
 *      every sum.  Two defences are kept anyway, because they are nearly free and
 *      because a future channel kind could reintroduce an infinity: weights must
 *      be > 0 and FINITE (not merely non-negative as sect. 7 says -- a user who
 *      wants a weight of zero wants the channel out of the fuse() call), and
 *      `remaining` is built as a per-document SUFFIX SUM rather than by
 *      subtraction.  Both are O(m), and m is a few dozen.
 *
 *   4. -INF MUST SHORT-CIRCUIT BEFORE IT MEETS ANYTHING.  A required channel's
 *      veto is checked as an unconditional discard the moment the running sum
 *      goes to -INF, ahead of any comparison against theta.  A NaN score from any
 *      channel is an error, not a row: the heap's comparator would treat it as
 *      "better than everything" and it would displace real answers.
 *
 * WHAT SURVIVES OF SECT. 5's WITHDRAWN HEURISTIC.  It proposed biasing the sort
 * to put graph channels last.  Besides having lost its subject, it would have
 * fought the partition: any reordering away from descending B_i has to
 * re-establish the invariant that the non-essential suffix sums to <= theta.  The
 * required/scored split is not such a reordering -- required channels are not in
 * the partition at all.
 *
 * TERMINATION, WHICH SECT. 3 LEAVES TO THE WARP RUNNING OUT.  The scan also stops
 * the moment the heap is full and suffix[0] <= theta, i.e. when the GLOBAL
 * ceiling over every scored channel cannot beat the threshold.  One check
 * subsumes three cases that would otherwise each need their own: every channel
 * demoted to non-essential (sect. 5's split reaching 0), a pure boolean query
 * where every surviving document scores 0.0 and the first k win on the warp
 * tie-break, and a query whose k-th best is already the maximum achievable score.
 *
 * THE TIE-BREAK, AND WHY THE SPEC'S "<= theta DISCARDS" IS STILL SOUND WITH ONE.
 * Ties must break deterministically or the same query returns different rows on
 * different plans; sql/edist.sql already pins (distance, id) for that reason.
 * The reference order is (score DESC, warp ASC), and the scorer reproduces it
 * with a min-heap whose "worst" is (score ASC, warp DESC) plus the rule that a
 * candidate is accepted only when S > theta, strictly.  Those agree because the
 * scan visits warp positions in ASCENDING order: everything already in the heap
 * has a lower warp than anything not yet visited, so for equal scores the heap's
 * entry always wins, and discarding a candidate whose score merely EQUALS theta
 * can never discard a row the reference would have kept.  Both prunes in sect. 3
 * are "<= theta", so this is the step that licenses them; it is checked directly
 * by the property test rather than argued from here alone.
 *
 * WHAT IS DELIBERATELY NOT HERE.  score_block() (channel.h) lets the vector
 * scan produce 32 lanes in a few SIMD instructions and channel.h says the
 * scorer prefers it when present.  F1 is document-at-a-time only.  Using
 * score_block() turns the loop inside out -- the block becomes the unit and the
 * pivot has to be recomputed against a range rather than a position -- and doing
 * that before the document-at-a-time loop is proven against brute force would
 * mean debugging two algorithms at once.  Recorded as owed rather than dropped.
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

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/*
 * PostgreSQL's c.h defines the fixed-width names; only supply the weave_ft_*
 * aliases from <stdint.h> when compiled outside the backend.  Mirrors
 * weave/gate.h and weave/uleven.h.
 */
#ifndef POSTGRES_H
typedef uint32_t weave_ft_uint32;
typedef uint64_t weave_ft_uint64;
typedef int64_t weave_ft_int64;
#else
typedef uint32 weave_ft_uint32;
typedef uint64 weave_ft_uint64;
typedef int64 weave_ft_int64;
#endif

/*
 * End-of-warp sentinel.  MUST equal WEAVE_WARP_END in channel.h and
 * WEAVE_GATE_END in gate.h; the glue asserts it at compile time.  Kept as its
 * own name so the standalone test does not need channel.h.
 */
#define WEAVE_FUSE_END			((weave_ft_uint32) 0xFFFFFFFF)

/* The largest number of channels one fused scan may carry.  FUSED_TOPK.md
 * sect. 5 says m is "at most a few dozen (query terms plus channels)"; the cap
 * exists so the core can size its scratch from the caller's arrays and refuse
 * rather than allocate.  A query that needs more is a query whose plan should
 * have been a bitmap AND. */
#define WEAVE_FUSE_MAX_CHAN		64

typedef enum WeaveFuseError
{
	WEAVE_FUSE_OK = 0,
	WEAVE_FUSE_BAD_K,			/* k < 1, or k > the caller's heap capacity */
	WEAVE_FUSE_BAD_NCHAN,		/* m < 1 or m > WEAVE_FUSE_MAX_CHAN */
	WEAVE_FUSE_BAD_WEIGHT,		/* weight <= 0 or not finite; see note 3 */
	WEAVE_FUSE_BAD_MAXSCORE,	/* a scored channel's maxscore is NaN or not finite */
	WEAVE_FUSE_C1_VIOLATION,	/* a channel's seek went backwards */
	WEAVE_FUSE_C2_VIOLATION,	/* score() exceeded block_max(); checked only
								 * when check_bounds is set */
	WEAVE_FUSE_NAN_SCORE		/* a channel returned NaN; see note 4 */
} WeaveFuseError;

typedef struct WeaveFuseChan WeaveFuseChan;

/*
 * The channel face the core drives.  It is a near-copy of WeaveShuttleOps from
 * channel.h, and the duplication is deliberate: channel.h needs postgres.h, and
 * a core that needs postgres.h cannot be linked by a bare compiler at 10^6
 * cases, which is the whole point (see the header comment).  src/am/fuseshuttle.c
 * forwards each slot to a WeaveShuttle in three lines.
 *
 * THE WEIGHT IS APPLIED HERE AND NOWHERE ELSE.  These three functions return
 * UNWEIGHTED values; the core multiplies.  channel.h's convenience wrappers
 * (weave_shuttle_score() and friends) apply the weight themselves and are
 * therefore NOT used by the fused path -- the forwarding thunks call the raw
 * ops.  Applying it in one place is what makes (C2) survive weighting, since
 * w * bound >= w * score requires w > 0 and nothing else.
 */
typedef struct WeaveFuseChanOps
{
	/* (C1) monotone: the smallest position >= target this channel can
	 * contribute at, or WEAVE_FUSE_END.  The core never calls this with a
	 * target below what the channel last returned (see note 1), so an
	 * implementation is free to refuse one. */
	weave_ft_uint32 (*seek) (WeaveFuseChan *c, weave_ft_uint32 target);

	/* (C2) an upper bound on score() over the closed interval [cur, blkend],
	 * and (C3) cheap: no buffer reads. */
	float		(*block_max) (WeaveFuseChan *c);

	/* (C4) the exact contribution at `cur`.  -INF means "does not match" and is
	 * how a gate reports a non-match (C5). */
	float		(*score) (WeaveFuseChan *c);
} WeaveFuseChanOps;

struct WeaveFuseChan
{
	const WeaveFuseChanOps *ops;

	/* Where the channel stands, and the last position covered by the block its
	 * bound describes.  `cur` is maintained by the core from seek()'s return
	 * value -- it is the core's memory of what it was given back, which note 1
	 * requires.  `blkend` is the channel's to publish: a forwarding thunk copies
	 * it from the shuttle after every seek, and a synthetic channel sets it
	 * directly.  blkend >= cur always; blkend == cur is a degenerate
	 * per-document bound, which is correct and is what a gate uses. */
	weave_ft_uint32 cur;
	weave_ft_uint32 blkend;

	/* Unweighted ceiling over the whole bolt: >= block_max() everywhere.  Must
	 * be finite for a scored channel.  IGNORED for a required channel, whose
	 * (C5) ceiling of +INF is exactly the value that must not reach the
	 * partition arithmetic (note 3). */
	float		maxscore;

	/* Strictly positive and finite; see note 3. */
	float		weight;

	/* Contract (C5): this channel is a PREDICATE, not a score source.  The
	 * document must match it, the core advances it conjunctively, and it is kept
	 * out of the partition and out of the block-prune sum (note 2).  Set by the
	 * glue from the shuttle's kind -- FUZZY, REGEX, DOCVALS and CGRAM are the
	 * four gate kinds gate.h enumerates -- and NOT inferred by the core, because
	 * a kind is not a contract: a cgram channel could in principle be scored, and
	 * the day it is, an inference here would silently keep filtering. */
	int			required;

	void	   *state;			/* channel-private */

	/* Per-channel counters.  nscore is not bookkeeping: the ratio of score()
	 * calls to RRF's is the row in FUSED_TOPK.md sect. 8 that decides whether
	 * the bounds are tight enough for the design to mean anything. */
	weave_ft_int64 nseek;
	weave_ft_int64 nscore;
};

/* One heap entry, and the scorer's output row. */
typedef struct WeaveFuseHit
{
	weave_ft_uint32 warp;
	float		score;
} WeaveFuseHit;

typedef struct WeaveFuseState
{
	/* The SCORED channels, sorted by descending weighted ceiling, and the
	 * REQUIRED ones in the order the caller supplied.  Borrowed pointers: the
	 * caller owns the WeaveFuseChan objects.
	 *
	 * Required channels are deliberately NOT reordered.  The obvious idea is to
	 * put the most selective first so the intersection converges in fewer probes,
	 * but selectivity is only knowable from the key count, which the core cannot
	 * see through the ops table, and ordering on a guess would make the pivot
	 * loop's cost depend on something no test controls.  The conjunctive loop
	 * converges regardless -- it takes the max -- so this is a throughput
	 * question, and an UNMEASURED one. */
	WeaveFuseChan *sc[WEAVE_FUSE_MAX_CHAN];
	int			nsc;
	WeaveFuseChan *rq[WEAVE_FUSE_MAX_CHAN];
	int			nrq;

	/* ceil[i] = sc[i]->weight * sc[i]->maxscore, and suffix[j] = sum of ceil[i]
	 * for i >= j, so suffix[nsc] == 0.  The partition is
	 * essential = [0, split), non-essential = [split, nsc), maintained as the
	 * largest suffix whose sum is <= theta.  split only ever DECREASES, because
	 * theta only ever grows (FUSED_TOPK.md sect. 5's "single int split"). */
	float		ceil[WEAVE_FUSE_MAX_CHAN];
	float		suffix[WEAVE_FUSE_MAX_CHAN + 1];
	int			split;

	/* Per-document scratch: the contributing SCORED channels at the current
	 * pivot, their weighted block bounds, and the suffix sum of those bounds
	 * (note 3 -- a suffix sum, never a subtraction). */
	int			contrib[WEAVE_FUSE_MAX_CHAN];
	float		cbound[WEAVE_FUSE_MAX_CHAN];
	float		csuffix[WEAVE_FUSE_MAX_CHAN + 1];
	int			ncontrib;

	/* The top-k min-heap, caller-allocated with room for k.  "Worst" is
	 * (score ASC, warp DESC); see the tie-break note in the header. */
	WeaveFuseHit *heap;
	int			k;
	int			nheap;
	float		theta;			/* heap[0].score once full, else -INF */

	/* (C6): tombstones are applied ONCE, here, and a channel must never look.
	 * NULL means every position is live.  Otherwise a bitmap indexed by
	 * ABSOLUTE warp -- the weave/kernels.h convention -- of which the caller
	 * guarantees at least `nwarp` bits exist.  Positions >= nwarp are dead,
	 * which is also how the scan terminates when the bolt is shorter than the
	 * key space. */
	const weave_ft_uint64 *live;
	weave_ft_uint32 nwarp;

	/* Turn each score() into a checked (C2) assertion: score <= block_max over
	 * the block the channel is standing on, within a relative tolerance.  Set
	 * by the property test always, and by the backend only under
	 * USE_ASSERT_CHECKING -- the same choice already made for the score()
	 * return-value convention in src/am/amscan.c.  A tolerance is needed
	 * because a channel may compute its bound by folding the same quantities in
	 * a different order, and an exact test would fail on rounding; it is
	 * relative so it does not silently accept a slack bound at large
	 * magnitudes. */
	int			check_bounds;

	/* Scan-wide counters.  nblkskip and nrqskip are the two prunes the design
	 * claims RRF cannot do -- a block whose bound cannot beat theta, and a range
	 * a required channel's intersection jumped over.  nlivedrop separates
	 * tombstones from pruning so a vacuum-heavy corpus cannot flatter the prune
	 * rate, and nveto separates predicate failures from both. */
	weave_ft_int64 npivot;
	weave_ft_int64 nblkskip;
	weave_ft_int64 nrqskip;
	weave_ft_int64 nlivedrop;
	weave_ft_int64 nveto;
	weave_ft_int64 nabandon;

	/* Set when the run stopped on an error: the offending channel, as a pointer
	 * rather than an index, since the two arrays are separately indexed. */
	WeaveFuseChan *badchan;
} WeaveFuseState;

/*
 * Validate and initialize.  `chan` is an array of `nchan` channel pointers in
 * ANY order -- the core partitions and sorts them -- each with ops, `required`,
 * maxscore, weight and an initial cur/blkend of 0.  `heap` must have room for
 * `k` entries.
 *
 * Every rejection here is a silent-wrong-answer prevented rather than a taste
 * enforced: notes 3 and 4 in the header explain why a zero weight and a NaN
 * ceiling both end with the prunes quietly disabling themselves.
 */
extern WeaveFuseError weave_fuse_init(WeaveFuseState *st,
									  WeaveFuseChan **chan, int nchan,
									  WeaveFuseHit *heap, int k,
									  const weave_ft_uint64 *live,
									  weave_ft_uint32 nwarp);

/*
 * Run the whole scan.  Returns WEAVE_FUSE_OK with the heap holding the top-k, or
 * a violation with st->badchan naming the channel.  Run-to-completion rather
 * than a step function because top-k cannot emit its first row before the scan
 * ends: the threshold is still rising.
 */
extern WeaveFuseError weave_fuse_run(WeaveFuseState *st);

/*
 * Drain the heap into `out` in final order -- (score DESC, warp ASC) -- and
 * return how many rows were written (min(k, matches)).  Destroys the heap.
 */
extern int	weave_fuse_drain(WeaveFuseState *st, WeaveFuseHit *out);

/*
 * The brute-force reference: visit every live position, apply every required
 * channel, sum every scored channel, take the top k under the same tie-break.
 * O(nwarp * m) and exported ON PURPOSE -- it is what test/hegel/test_fuse_props.c
 * compares against, and keeping it beside the fast path in the same TU is what
 * stops the two drifting into different definitions of "top k".  It uses only
 * seek() and score(), never block_max(), so a wrong bound cannot corrupt the
 * reference the way it corrupts the scorer -- which is the whole point, since a
 * loose bound is the failure mode (C2) exists for.
 *
 * `out` needs room for k.  The channels must be freshly positioned, exactly as
 * for weave_fuse_run(), and because seeks are monotone a caller comparing the
 * two needs TWO independent sets of channels, not one rewound.
 */
extern int	weave_fuse_reference(WeaveFuseChan **chan, int nchan,
								 const weave_ft_uint64 *live,
								 weave_ft_uint32 nwarp,
								 int k, WeaveFuseHit *out);

/* ---------------------------------------------------------------------------
 * The WeaveShuttle face -- backend only (src/am/fuseshuttle.c)
 * ------------------------------------------------------------------------- */
#ifdef POSTGRES_H

#include "weave/channel.h"

/*
 * Wrap `ns` shuttles as fused channels in caller-supplied storage.  Each
 * channel's ops forward to the shuttle's RAW ops -- unweighted, because the core
 * applies the weight -- and copy s->blkend after every seek.  `weight` is taken
 * from the shuttle's own weight field, which is where fuse() parsing puts it,
 * and `required` from the shuttle's kind: WEAVE_CH_FUZZY, WEAVE_CH_REGEX,
 * WEAVE_CH_DOCVALS and WEAVE_CH_CGRAM are the (C5) gate kinds, the same four
 * weave_gate_shuttle_begin() accepts, and the mapping lives in one place so the
 * two cannot disagree about what a gate is.
 *
 * ERRORs on a weight the core would reject, naming the channel kind, rather than
 * letting weave_fuse_init() fail an assertion-free build with an enum.
 */
extern void weave_fuse_wrap_shuttles(WeaveFuseChan *chan, WeaveShuttle **ss,
									 int ns);

/* Map a core error onto an ereport().  Separate from the core so the core stays
 * free of elog, and in one place so every call site reports a (C1)/(C2)
 * violation with the same wording and the channel kind. */
extern void weave_fuse_error(const WeaveFuseState *st, WeaveFuseError err);

#endif							/* POSTGRES_H */

#endif							/* WEAVE_FUSE_H */
