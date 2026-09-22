/*-------------------------------------------------------------------------
 *
 * test_vecdocmap.c
 *		Task F8: the vector channel's lane-to-docid relabelling
 *		(include/weave/vecdocmap.h, src/am/vecdocmap.c) preserves (C1) and
 *		(C2) across the composition with a lane-space WeaveFuseChan.
 *
 * This file drives the REAL weave_vecdoc_lower_bound(), weave_vecdoc_chan_init()
 * and the vecdoc_chan_ops table (vecdoc_seek/vecdoc_block_max/vecdoc_score) --
 * never a copy -- against a SYNTHETIC lane-space WeaveFuseChan this file fully
 * controls, the same discipline test/hegel/test_fuse_props.c uses for the core
 * itself.  Only the two POSITIVE CONTROLS in P6 use a hand-simulated mutation,
 * and never by editing vecdocmap.c.
 *
 * Properties, over randomized (nlane, block size, docid[] gap pattern,
 * per-lane sparsity):
 *
 *	P1	weave_vecdoc_lower_bound() == a linear oracle, for target = 0, every
 *		present docid, every gap between present docids, and every value above
 *		the maximum.  This is the whole (C1) argument (vecdocmap.h "WHY (C1)
 *		SURVIVES"), so it is checked standalone before anything is composed
 *		with it
 *	P2	(C1) end to end: driven through a random INCREASING sequence of docid
 *		targets, the adapter's seek() is monotone, every non-END answer is
 *		>= the target, and WEAVE_FUSE_END is absorbing once returned (checked
 *		by construction, since the sequence never decreases)
 *	P3	the returned docid is exactly docid[l], where l is the first lane at or
 *		after lower_bound(target) at which the inner channel can contribute --
 *		the composition the header claims, checked against an independent
 *		linear oracle over the lane space rather than inferred from P2
 *	P4	(C2) interval coverage, stated exactly as vecdocmap.h states it: for
 *		EVERY lane l with docid[l] in [chan.cur, chan.blkend], the inner's
 *		published bound is >= the inner's exact score at l; chan.blkend >=
 *		chan.cur always; and chan.blkend is the docid of some lane < nlane,
 *		never a value that only exists past the map's declared length
 *	P5	chan->ops->score() equals the synthetic channel's exact score at the
 *		lane the P3 oracle says was landed on -- scoring the wrong lane is the
 *		single most likely bug in vecdoc_score() and it produces a plausible
 *		float, not a crash
 *	P6	THE POSITIVE CONTROLS, and the run FAILS if either never fires.  Two
 *		one-line mutations of the adapter's own arithmetic, each simulated by
 *		a hand-written copy of the relevant fragment in THIS file:
 *		 (a) blkend computed from a->inner->blkend with the
 *		     "if (last >= a->nlane) last = a->nlane - 1" clamp
 *		     (vecdoc_seek()'s blkend clamp) removed
 *		 (b) weave_vecdoc_lower_bound()'s comparator flipped, "docid[mid] < target"
 *		     to "docid[mid] > target" (weave_vecdoc_lower_bound()'s comparator)
 *	P7	weave_vecdoc_chan_init() refuses exactly what the header promises:
 *		nlane == 0, a descending pair at position 1, in the middle, and at the
 *		last position, an EQUAL pair (duplicate docid) at the same three
 *		positions, and a maximum docid >= WEAVE_FUSE_END -- each with -1 and a
 *		non-NULL *why.  Duplicates are refused, not merely tolerated, because a
 *		docid carried by more than one lane is not a relabelling but an
 *		aggregation over those lanes -- a different channel -- and
 *		include/weave/vecdocmap.h ("WHY *STRICTLY* ASCENDING, WHICH THIS FILE
 *		FIRST GOT WRONG") is where that argument, and the counterexamples that
 *		forced it, are made in full
 *	P8	init() copies weight, maxscore and required off the inner channel onto
 *		the adapter's own struct fields, because the fused core reads those as
 *		fields rather than through the ops table (weave_vecdoc_chan_init()'s field copying)
 *	P9	end to end through the REAL core: one vecdoc-adapted lane channel plus
 *		one native docid-space required (gate) channel, run through
 *		weave_fuse_init()/weave_fuse_run()/weave_fuse_drain() with
 *		check_bounds = 1, compared against weave_fuse_reference().  This is
 *		included (not skipped) because the extra code is small -- the docid
 *		gate channel is a five-function reuse of the lane-channel machinery
 *		already written for P2-P5 -- and it is the only property here that
 *		would notice an adapter that is individually sound but wired into
 *		fuse_init()'s channel array wrong (e.g. required/weight swapped, or
 *		blkend narrower than cur after weave_fuse_init()'s own bookkeeping).
 *
 * WHY P6(b) IS NOT LITERALLY "THE FIRST LANE WITH docid > target".  That
 * reading is upper_bound(), and upper_bound() composed with a monotone forward
 * inner seek can be shown never to return a docid < target: it always answers
 * a value strictly greater than target (or n), and docid[] ascending then
 * forces the composed result to be >= that too.  A 500000-case brute-force
 * check while writing this file (kept only as this note, not as code) found
 * zero violations from that reading.  The actual one-line bug this control
 * exercises is the realistic sibling: the comparator direction itself flipped
 * ("<" to ">" in weave_vecdoc_lower_bound()'s comparator), which breaks the loop's invariant (the
 * predicate binary search relies on is no longer monotone in the index) and
 * reliably produces docid[result] < target on real input -- about half the
 * time, in the same brute-force check.  Recording why the literal reading
 * cannot fire, rather than silently swapping to whichever mutation happened
 * to work, is the point of hard rule 1's "generator limitation" warning: a
 * control that cannot fire is a fact about the control, and it belongs in a
 * comment, not just in whichever one got shipped.
 *
 * A FINDING FROM WRITING P4, RECORDED PER HARD RULE 13.  This file found the
 * adapter unsound for duplicate docids two ways -- (C2) the bound covers only
 * the landed lane's block, (C4) score() is defined at one lane so a
 * duplicate's other lanes contribute nothing.  Real finding, fixed by
 * refusal rather than by weakening this test: weave_vecdoc_chan_init() now
 * requires strictly ascending docid[] (per weave_vecdoc_chan_init()'s ascending check), P7 asserts that
 * refusal, and the counterexample is in include/weave/vecdocmap.h's "WHY
 * *STRICTLY* ASCENDING, WHICH THIS FILE FIRST GOT WRONG".
 *
 * A SECOND FINDING FROM WRITING P9, ALSO RECORDED PER HARD RULE 13.  Driving
 * the adapter through the REAL fused core and through a long increasing sequence
 * of targets (P2) exposed a second real bug: WEAVE_FUSE_END was not latched in
 * src/am/vecdocmap.c. Once the inner lane-space channel had returned END, a
 * later seek whose lower_bound still landed inside the map called the inner
 * channel AGAIN with a lane target below the one it had last returned. The fuse
 * contract only promises that targets increase monotonically; the two spaces run
 * out independently, so "lo < nlane" does not mean the inner can still be asked.
 * Every real shuttle refuses a backward seek -- src/vector/vecshuttle.c raises
 * "cannot resolve warp %u after warp %u" -- so the symptom would have been an
 * ERROR mid-scan on a perfectly good index, not a wrong answer. Fixed by
 * returning END immediately when the inner's cur is already END. A synthetic
 * channel that tolerated a backward seek would have reported nothing; the
 * property that found it needed the inner channel to assert its own refusal.
 *
 * Build and run:
 *		cc -O2 -Wall -Wextra -I include -o /tmp/tv test/hegel/test_vecdocmap.c \
 *			src/am/vecdocmap.c src/am/fuse.c -lm && /tmp/tv
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_vecdocmap.c
 *
 *-------------------------------------------------------------------------
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/vecdocmap.h"

static long failures = 0;
static long checks = 0;
static long prop_checks[10];

#define CHECK(prop, cond, ...)											\
	do {																\
		checks++;														\
		prop_checks[prop]++;											\
		if (!(cond))													\
		{																\
			if (failures < 20)											\
			{															\
				printf("P%d FAIL %s:%d: ", (prop), __FILE__, __LINE__);	\
				printf(__VA_ARGS__);									\
				printf("\n");											\
			}															\
			failures++;													\
		}																\
	} while (0)

/* xorshift64*; matches every other property test in this tree so a seed is
 * reproducible without depending on libc's rand(). */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t
rnd64(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint32_t
rnd_below(uint32_t n)
{
	return n == 0 ? 0 : (uint32_t) (rnd64() % n);
}

static float
rnd_score(void)
{
	/* A coarse lattice makes ties between lanes common, which is exactly
	 * where a "wrong lane" bug in P5 or a coverage bug in P4 hides: a uniform
	 * float32 almost never repeats, so two lanes scoring equally is otherwise
	 * untested. */
	return (float) rnd_below(41) * 0.25f;
}

/* ---------------------------------------------------------------------------
 * Block-interval arithmetic, shared by the lane-space synthetic channel and
 * this file's hand-simulated mutations.  Mirrors test_fuse_props.c's
 * blk_start()/blk_end(), which is deliberate: it is the same fixed-width block
 * grid a real vector shuttle publishes, independent of how many lanes exist.
 * ------------------------------------------------------------------------- */

static weave_ft_uint32
blk_start(weave_ft_uint32 bs, weave_ft_uint32 p)
{
	return p - (p % bs);
}

static weave_ft_uint32
blk_end(weave_ft_uint32 bs, weave_ft_uint32 p)
{
	weave_ft_uint32 s = blk_start(bs, p);

	if (WEAVE_FUSE_END - bs < s)
		return WEAVE_FUSE_END - 1;
	return s + bs - 1;
}

/* ---------------------------------------------------------------------------
 * The synthetic lane-space channel: a per-lane sparsity mask and score array,
 * a fixed block width, and an ASSERTION that it is never seeked backwards --
 * both real shuttles vecdocmap.c adapts (fuseshuttle.c's gate and vector
 * thunks) refuse a backward seek outright, so a monotonicity bug in the
 * adapter has to be caught here rather than tolerated.
 * ------------------------------------------------------------------------- */

#define MAXLANE		4096
#define LANE_PAD	512			/* room past nlane for P6(a)'s unclamped read */
#define BUFCAP		(MAXLANE + LANE_PAD)

typedef struct LaneSpec
{
	int			has[MAXLANE];	/* whether lane l has anything to contribute */
	float		score[MAXLANE]; /* meaningful only where has[l] */
	weave_ft_uint32 nlane;
	weave_ft_uint32 bs;
	float		inflate;		/* >= 1.0: block_max may be a SLACK bound and
								 * must still satisfy P4 -- a tight bound is
								 * not the only sound bound */
} LaneSpec;

typedef struct LaneChan
{
	WeaveFuseChan ch;
	const LaneSpec *spec;
	weave_ft_uint32 last_ret;
	int			seeked;
} LaneChan;

/* First lane index >= `lo` with has[] set, or spec->nlane when there is none.
 * A pure function of the data, used both as P3's oracle and, deliberately, as
 * P6(b)'s "what would the inner channel do" simulation -- P6(b) must NOT call
 * through LaneChan's own ops, because the mutated lower_bound this file
 * simulates does not produce a monotone sequence of lane targets, and driving
 * that through the real assert-bearing seek would abort the process instead
 * of reporting a property failure. */
static weave_ft_uint32
lane_first_at(const LaneSpec *sp, weave_ft_uint32 lo)
{
	weave_ft_uint32 i;

	for (i = lo; i < sp->nlane; i++)
	{
		if (sp->has[i])
			return i;
	}
	return sp->nlane;
}

static weave_ft_uint32
lane_seek(WeaveFuseChan *c, weave_ft_uint32 target)
{
	LaneChan   *s = (LaneChan *) c->state;
	const LaneSpec *sp = s->spec;
	weave_ft_uint32 l;

	/* Both real shuttles this adapter wraps refuse a backward seek; the
	 * hazard is real (fuse.h note 1), so the synthetic stands in for that
	 * refusal rather than silently tolerating it. */
	assert(!(s->seeked && target < s->last_ret));
	s->seeked = 1;

	l = lane_first_at(sp, target);
	if (l >= sp->nlane)
	{
		c->blkend = WEAVE_FUSE_END;
		s->last_ret = WEAVE_FUSE_END;
		return WEAVE_FUSE_END;
	}

	/*
	 * blkend is reported over the FIXED block grid, unclamped to nlane.  This
	 * is deliberate and mirrors a real vector shuttle: the block width is a
	 * property of the segment's fixed layout, not of how many lanes happen to
	 * exist, so the final block of a weft not a multiple of `bs` is exactly
	 * the short tail vecdoc_seek()'s blkend clamp exists for.
	 */
	c->blkend = blk_end(sp->bs, l);
	s->last_ret = l;
	return l;
}

static float
lane_block_max(WeaveFuseChan *c)
{
	LaneChan   *s = (LaneChan *) c->state;
	const LaneSpec *sp = s->spec;
	weave_ft_uint32 lo;
	weave_ft_uint32 hi;
	weave_ft_uint32 i;
	float		m = -INFINITY;

	if (c->cur == WEAVE_FUSE_END)
		return -INFINITY;

	lo = blk_start(sp->bs, c->cur);
	hi = blk_end(sp->bs, c->cur);
	if (hi >= sp->nlane)
		hi = sp->nlane - 1;
	for (i = lo; i <= hi; i++)
	{
		if (sp->has[i] && sp->score[i] > m)
			m = sp->score[i];
	}
	if (m == -INFINITY)
		return -INFINITY;
	return m * sp->inflate;
}

static float
lane_score(WeaveFuseChan *c)
{
	LaneChan   *s = (LaneChan *) c->state;
	const LaneSpec *sp = s->spec;
	weave_ft_uint32 l = c->cur;

	if (l >= sp->nlane || !sp->has[l])
		return -INFINITY;
	return sp->score[l];
}

static const WeaveFuseChanOps lane_ops = {
	lane_seek, lane_block_max, lane_score
};

static void
lane_bind(LaneChan *s, const LaneSpec *sp)
{
	memset(s, 0, sizeof(*s));
	s->spec = sp;
	s->ch.ops = &lane_ops;
	s->ch.state = s;
	s->ch.cur = 0;
	s->ch.blkend = 0;
	s->ch.weight = 0.25f + (float) rnd_below(8) * 0.5f;
	s->ch.maxscore = 8.0f + (float) rnd_below(16);
	s->ch.required = 0;
	s->ch.nseek = 0;
	s->ch.nscore = 0;
}

/* ---------------------------------------------------------------------------
 * Generators
 * ------------------------------------------------------------------------- */

/* nlane: mostly small, sometimes a single lane, occasionally a few thousand --
 * the size weave_vecdoc_lower_bound()'s cost note is about and where a
 * quadratic mistake in weave_vecdoc_lower_bound() would actually show up in a
 * profile even though this test only checks correctness. */
static weave_ft_uint32
gen_nlane(void)
{
	uint32_t	r = rnd_below(100);

	if (r < 10)
		return 1;
	if (r < 70)
		return 2 + rnd_below(62);
	if (r < 92)
		return 64 + rnd_below(512);
	return 1500 + rnd_below(2500);	/* "a few thousand" */
}

/* Block size, biased toward the two cases the clamp in vecdoc_seek exists to
 * tell apart: bs == 1 (the degenerate per-document bound) and a width that
 * does NOT divide nlane, so the final block is a short tail. */
static weave_ft_uint32
gen_bs(weave_ft_uint32 nlane)
{
	uint32_t	r = rnd_below(3);

	if (r == 0)
		return 1;
	if (r == 1)
		return 1u << (1 + rnd_below(9));	/* 2 .. 512 */
	return 2 + rnd_below(nlane > 64 ? 190 : nlane + 3);
}

/*
 * docid[nlane + LANE_PAD) STRICTLY ascending -- minimum gap 1, never 0.
 * weave_vecdoc_chan_init() now refuses anything less (P7; the argument is in
 * include/weave/vecdocmap.h's "WHY *STRICTLY* ASCENDING"), so nothing that
 * drives the real adapter through init() may generate a map init() would
 * refuse.  The padding past nlane is filled with a continuation of the same
 * ascending sequence and exists ONLY so P6(a) can read `docid[last]` for an
 * unclamped `last` without indexing outside the array this file owns -- an
 * actual heap-buffer-overflow is not needed to demonstrate the defect class,
 * and would turn a property failure into a segfault at whatever trial number
 * the RNG happens to reach it.
 *
 * `style` selects the gap pattern: 0 dense (every step 1, the degenerate case
 * where every lane is its own block boundary), 1 small random gaps (1..6),
 * 2 mostly small gaps with an occasional huge jump, and 3 STRICTLY alternating
 * tiny (1) and huge (up to 2^16) gaps.  Style 3 replaces what used to be runs
 * of duplicate docids (no longer legal input): alternating tiny/huge gaps
 * still stresses the code the duplicate style was added for, because it puts
 * block boundaries in unpredictable places relative to docid distance -- a
 * block can straddle a huge jump or sit entirely inside a run of tiny ones.
 */
static void
gen_docid(weave_ft_uint64 *docid, weave_ft_uint32 n, int style)
{
	weave_ft_uint64 cur = rnd_below(4);
	weave_ft_uint32 i;

	for (i = 0; i < n; i++)
	{
		weave_ft_uint64 step;

		switch (style)
		{
			case 0:
				step = 1;
				break;
			case 1:
				step = 1 + rnd_below(6);
				break;
			case 2:
				step = (rnd_below(20) == 0) ?
					(1 + (weave_ft_uint64) rnd_below(1u << 18)) : (1 + rnd_below(3));
				break;
			default:
				step = ((i % 2) == 0) ? 1 : (1 + rnd_below(1u << 16));
				break;
		}
		cur += step;
		docid[i] = cur;
	}
}

static void
gen_lane_spec(LaneSpec *sp, weave_ft_uint32 nlane, weave_ft_uint32 density_pct)
{
	weave_ft_uint32 i;
	int			any = 0;

	memset(sp, 0, sizeof(*sp));
	sp->nlane = nlane;
	sp->bs = gen_bs(nlane);
	sp->inflate = (rnd_below(4) == 0) ? (1.0f + (float) rnd_below(4) * 0.5f) : 1.0f;

	for (i = 0; i < nlane; i++)
	{
		if (rnd_below(100) < density_pct)
		{
			sp->has[i] = 1;
			sp->score[i] = rnd_score();
			any = 1;
		}
	}
	if (!any)
	{
		/* Every property below needs at least one contributing lane to say
		 * anything; a channel with none is a degenerate case P2 already
		 * covers via WEAVE_FUSE_END and is not this generator's job to retry
		 * into existence. */
		sp->has[nlane - 1] = 1;
		sp->score[nlane - 1] = rnd_score();
	}
}

/* ---------------------------------------------------------------------------
 * P1: weave_vecdoc_lower_bound() == a linear oracle
 * ------------------------------------------------------------------------- */

static weave_ft_uint32
linear_lower_bound(const weave_ft_uint64 *docid, weave_ft_uint32 n,
					weave_ft_uint64 target)
{
	weave_ft_uint32 i;

	for (i = 0; i < n; i++)
	{
		if (docid[i] >= target)
			return i;
	}
	return n;
}

static weave_ft_uint64 p1_docid[BUFCAP];

static void
lower_bound_oracle_trial(void)
{
	weave_ft_uint32 n = gen_nlane();
	weave_ft_uint32 got;
	weave_ft_uint32 want;
	weave_ft_uint32 i;
	weave_ft_uint64 target;

	gen_docid(p1_docid, n, (int) rnd_below(4));

	/* Target 0, every present value, every gap immediately above a present
	 * value, and comfortably above the maximum -- the four cases the (C1)
	 * argument has to hold at. */
	for (i = 0; i < n; i++)
	{
		got = weave_vecdoc_lower_bound(p1_docid, n, p1_docid[i]);
		want = linear_lower_bound(p1_docid, n, p1_docid[i]);
		CHECK(1, got == want, "target=present[%u]=%llu got=%u want=%u",
			  i, (unsigned long long) p1_docid[i], got, want);

		target = p1_docid[i] + 1;
		got = weave_vecdoc_lower_bound(p1_docid, n, target);
		want = linear_lower_bound(p1_docid, n, target);
		CHECK(1, got == want, "target=present[%u]+1=%llu got=%u want=%u",
			  i, (unsigned long long) target, got, want);
	}

	got = weave_vecdoc_lower_bound(p1_docid, n, 0);
	want = linear_lower_bound(p1_docid, n, 0);
	CHECK(1, got == want, "target=0 got=%u want=%u", got, want);

	target = p1_docid[n - 1] + 1 + rnd_below(1000000);
	got = weave_vecdoc_lower_bound(p1_docid, n, target);
	want = linear_lower_bound(p1_docid, n, target);
	CHECK(1, got == want && got == n, "target above max got=%u want=%u n=%u",
		  got, want, n);
}

/* ---------------------------------------------------------------------------
 * The driver every property from here on shares: advance a WeaveFuseChan the
 * way the fused core does (src/am/fuse.c's fuse_advance()) -- seek(), then
 * cur = the return value, then blkend widened to at least cur if the channel
 * under-reported it.  Reproducing the core's own bookkeeping here, rather
 * than trusting the adapter to set chan.cur itself, matters: fuse.h says in so
 * many words that cur is "the core's memory of what it was given back", and
 * vecdocmap.c's header comment says forgetting that is the one bug this file
 * would not otherwise catch.
 * ------------------------------------------------------------------------- */

static weave_ft_uint32
drive_seek(WeaveFuseChan *c, weave_ft_uint32 target)
{
	weave_ft_uint32 p = c->ops->seek(c, target);

	c->cur = p;
	if (c->blkend < p)
		c->blkend = p;
	return p;
}

/* ---------------------------------------------------------------------------
 * P2-P5, P8: one adapter, driven through an increasing sequence of targets.
 * ------------------------------------------------------------------------- */

static weave_ft_uint64 main_docid[BUFCAP];

static void
main_trial(void)
{
	LaneSpec	lspec;
	LaneChan	inner;
	WeaveVecDocChan a;
	weave_ft_uint32 nlane;
	weave_ft_uint32 prev_ret = 0;
	int			prev_was_end = 0;
	int			ntargets;
	int			t;
	const char *why = NULL;
	int			rc;

	nlane = gen_nlane();
	gen_lane_spec(&lspec, nlane, 5 + rnd_below(90));
	gen_docid(main_docid, nlane, (int) rnd_below(4));

	lane_bind(&inner, &lspec);
	rc = weave_vecdoc_chan_init(&a, &inner.ch, main_docid, nlane, &why);
	CHECK(7, rc == 0 && why == NULL, "init on a valid map was refused: %s",
		  why ? why : "(no reason)");
	if (rc != 0)
		return;

	/* P8: the fused core reads weight/maxscore/required as struct fields on
	 * the adapter, never through the inner's ops -- so init() must copy them
	 * rather than leave them at whatever weave_vecdoc_chan_init()'s caller
	 * happened to zero-initialize the struct to. */
	CHECK(8, a.chan.weight == inner.ch.weight,
		  "weight not copied: adapter=%.6f inner=%.6f",
		  (double) a.chan.weight, (double) inner.ch.weight);
	CHECK(8, a.chan.maxscore == inner.ch.maxscore,
		  "maxscore not copied: adapter=%.6f inner=%.6f",
		  (double) a.chan.maxscore, (double) inner.ch.maxscore);
	CHECK(8, a.chan.required == inner.ch.required,
		  "required not copied: adapter=%d inner=%d",
		  a.chan.required, inner.ch.required);

	ntargets = 3 + (int) rnd_below(20);
	for (t = 0; t < ntargets; t++)
	{
		weave_ft_uint32 target;
		weave_ft_uint32 ret;
		weave_ft_uint32 lo;
		weave_ft_uint32 want_lane;

		/* A strictly-increasing sequence of targets, so a channel that
		 * refuses a backward seek (both real ones do) is never asked for
		 * one, and this loop tests the WEAVE_FUSE_END absorption directly:
		 * every later target in an increasing sequence must also come back
		 * END once one already has. */
		target = prev_ret + rnd_below(nlane > 200 ? 40 : 6);

		ret = drive_seek(&a.chan, target);

		/* P2 */
		CHECK(2, ret == WEAVE_FUSE_END || ret >= target,
			  "seek(%u) returned %u, below the target", target, ret);
		CHECK(2, ret == WEAVE_FUSE_END || ret >= prev_ret,
			  "seek(%u) returned %u, below the previous return %u",
			  target, ret, prev_ret);
		CHECK(2, !(prev_was_end && ret != WEAVE_FUSE_END),
			  "seek(%u) returned %u after a previous target already got END",
			  target, ret);

		if (ret == WEAVE_FUSE_END)
		{
			prev_was_end = 1;

			/*
			 * `target` AND NOT `ret`.  WEAVE_FUSE_END is 0xFFFFFFFF, so making it
			 * the base of the next target's increment wraps the uint32 back to a
			 * small value -- which is a BACKWARD target, which the inner channel
			 * then refuses with an assertion, from the test's own generator rather
			 * than from anything under test.  Continuing from the target keeps the
			 * sequence increasing and still probes END absorption.
			 */
			prev_ret = target;
			continue;
		}

		/* P3: an independent oracle over the lane space, composing
		 * lower_bound (already proven by P1) with lane_first_at (a pure
		 * function, not the channel under test). */
		lo = weave_vecdoc_lower_bound(main_docid, nlane, (weave_ft_uint64) target);
		want_lane = lane_first_at(&lspec, lo);
		CHECK(3, want_lane < nlane && main_docid[want_lane] == (weave_ft_uint64) ret,
			  "seek(%u) returned docid %u, oracle lane %u has docid %llu",
			  target, ret, want_lane,
			  want_lane < nlane ? (unsigned long long) main_docid[want_lane] : 0ULL);
		CHECK(3, a.lane == want_lane,
			  "adapter's own `lane` field is %u, oracle says %u",
			  a.lane, want_lane);

		/* P4 */
		CHECK(4, a.chan.blkend >= a.chan.cur,
			  "blkend %u < cur %u after seek(%u)", a.chan.blkend, a.chan.cur, target);
		{
			weave_ft_uint32 idx = weave_vecdoc_lower_bound(main_docid, nlane,
															(weave_ft_uint64) a.chan.blkend);

			CHECK(4, idx < nlane && main_docid[idx] == (weave_ft_uint64) a.chan.blkend,
				  "blkend %u is not the docid of any lane < nlane (%u)",
				  a.chan.blkend, nlane);
		}
		{
			float		bound = a.chan.ops->block_max(&a.chan);
			weave_ft_uint32 j;

			for (j = 0; j < nlane; j++)
			{
				float		exact;
				int			covered;

				if (main_docid[j] < a.chan.cur || main_docid[j] > a.chan.blkend)
					continue;

				exact = lspec.has[j] ? lspec.score[j] : -INFINITY;
				covered = (exact == -INFINITY || bound >= exact);

				CHECK(4, covered,
					  "lane %u (docid %llu, in [%u,%u]) scores %.6f, "
					  "block bound only %.6f (landed lane %u)",
					  j, (unsigned long long) main_docid[j], a.chan.cur,
					  a.chan.blkend, (double) exact, (double) bound, a.lane);
			}
		}

		/* P5 */
		{
			float		got_score = a.chan.ops->score(&a.chan);
			float		want_score = lspec.has[want_lane] ? lspec.score[want_lane] : -INFINITY;

			CHECK(5, got_score == want_score,
				  "score() returned %.6f at lane %u, oracle says %.6f",
				  (double) got_score, a.lane, (double) want_score);
		}

		prev_ret = ret;
	}
}

/* ---------------------------------------------------------------------------
 * P6(a): the unclamped-blkend positive control.
 *
 * A hand copy of vecdoc_seek()'s post-seek fragment with
 * the clamp "if (last >= a->nlane) last = a->nlane - 1" (vecdoc_seek()'s blkend clamp) removed.
 * Driven through the REAL inner channel's REAL ops -- there is nothing to
 * bypass here, because the lower_bound this leg uses is the correct one and
 * therefore produces a monotone sequence of lane targets, so a single seek per
 * trial is enough and never risks the backward-seek assertion.
 * ------------------------------------------------------------------------- */

static long n_ctrl_a_trials = 0;
static long n_ctrl_a_fires = 0;

static weave_ft_uint64 ctrl_a_docid[BUFCAP];

static void
control_a_trial(void)
{
	LaneSpec	lspec;
	LaneChan	inner;
	weave_ft_uint32 nlane = gen_nlane();
	weave_ft_uint32 target;
	weave_ft_uint32 lo;
	weave_ft_uint32 l;
	weave_ft_uint32 last_bad;

	if (nlane > MAXLANE - LANE_PAD)
		nlane = MAXLANE - LANE_PAD;	/* leave room to pad past nlane */

	gen_lane_spec(&lspec, nlane, 5 + rnd_below(90));

	/*
	 * A block width that does NOT divide nlane, forced rather than left to
	 * chance, so the tail block genuinely runs past nlane on every trial --
	 * the one scenario the real clamp exists to handle.
	 */
	lspec.bs = 3 + rnd_below(61);

	/*
	 * All four gap styles, including 3 (alternating tiny/huge).  This used to
	 * be rnd_below(3), deliberately excluding the old duplicate-run style so
	 * that a duplicate docid past nlane in the padding could not accidentally
	 * equal an in-range docid and make `off_map` false negative.  Every style
	 * is now strictly ascending, so no style can produce that coincidence and
	 * the exclusion no longer has a reason to exist.
	 */
	gen_docid(ctrl_a_docid, nlane + LANE_PAD, (int) rnd_below(4));
	lane_bind(&inner, &lspec);

	n_ctrl_a_trials++;

	target = rnd_below(nlane + 10);
	lo = weave_vecdoc_lower_bound(ctrl_a_docid, nlane, (weave_ft_uint64) target);
	if (lo >= nlane)
		return;

	l = drive_seek(&inner.ch, lo);
	if (l == WEAVE_FUSE_END || l >= nlane)
		return;

	/* THE MUTATION. */
	last_bad = inner.ch.blkend;
	if (last_bad < l)
		last_bad = l;

	if (last_bad >= nlane)
	{
		/*
		 * Demonstrate the P4 consequence concretely: `docid[last_bad]` is
		 * safe to read only because ctrl_a_docid[] is padded past nlane for
		 * exactly this purpose, and the value it reads is not the docid of
		 * any lane the map actually declares -- the "chan.blkend is the
		 * docid of a lane that actually exists" half of P4.
		 */
		weave_ft_uint64 blkend_bad = ctrl_a_docid[last_bad];
		weave_ft_uint32 idx = weave_vecdoc_lower_bound(ctrl_a_docid, nlane, blkend_bad);
		int			off_map = !(idx < nlane && ctrl_a_docid[idx] == blkend_bad);

		if (off_map)
			n_ctrl_a_fires++;
	}
}

/* ---------------------------------------------------------------------------
 * P6(b): the flipped-comparator positive control.  See the header comment for
 * why this, and not the literal "first lane with docid > target" reading, is
 * the mutation that actually fires.
 * ------------------------------------------------------------------------- */

static long n_ctrl_b_trials = 0;
static long n_ctrl_b_fires = 0;

static weave_ft_uint64 ctrl_b_docid[BUFCAP];

static weave_ft_uint32
broken_lower_bound_flipped(const weave_ft_uint64 *docid, weave_ft_uint32 n,
							weave_ft_uint64 target)
{
	weave_ft_uint32 lo = 0;
	weave_ft_uint32 hi = n;

	while (lo < hi)
	{
		weave_ft_uint32 mid = lo + (hi - lo) / 2;

		/* THE MUTATION: "<" flipped to ">" in weave_vecdoc_lower_bound()'s comparator), which breaks the loop's invariant (the
		 * "docid[mid] > target" is not monotone non-decreasing in mid the way
		 * "docid[mid] < target" is over an ascending array. */
		if (docid[mid] > target)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void
control_b_trial(void)
{
	LaneSpec	lspec;
	weave_ft_uint32 nlane = gen_nlane();
	weave_ft_uint32 target;
	weave_ft_uint32 lo_bad;
	weave_ft_uint32 l;

	gen_lane_spec(&lspec, nlane, 5 + rnd_below(90));
	gen_docid(ctrl_b_docid, nlane, (int) rnd_below(4));

	n_ctrl_b_trials++;

	target = rnd_below(nlane + 10);
	lo_bad = broken_lower_bound_flipped(ctrl_b_docid, nlane, (weave_ft_uint64) target);
	if (lo_bad >= nlane)
		return;

	/*
	 * lane_first_at() rather than a bound LaneChan: the mutated `lo_bad`
	 * sequence is not monotone across targets (this file's header comment
	 * records the brute-force check), and driving it through the real
	 * assert-bearing seek would abort the process on the first out-of-order
	 * call instead of reporting a property failure.  A pure lookup is what
	 * "if this were fed to the inner channel" means without needing the
	 * inner channel's own position state.
	 */
	l = lane_first_at(&lspec, lo_bad);
	if (l >= nlane)
		return;

	if (ctrl_b_docid[l] < (weave_ft_uint64) target)
		n_ctrl_b_fires++;
}

/* ---------------------------------------------------------------------------
 * P7: weave_vecdoc_chan_init() refusals
 * ------------------------------------------------------------------------- */

static weave_ft_uint64 p7_docid[16];

static void
init_refusal_trials(void)
{
	LaneSpec	lspec;
	LaneChan	inner;
	WeaveVecDocChan a;
	const char *why;
	int			rc;
	int			pos;

	gen_lane_spec(&lspec, 4, 80);
	lane_bind(&inner, &lspec);

	why = NULL;
	rc = weave_vecdoc_chan_init(&a, &inner.ch, p7_docid, 0, &why);
	CHECK(7, rc == -1 && why != NULL, "nlane == 0 was accepted");

	/* The header's third argument is a NULL sanity check as much as a
	 * documented contract: a docid[] that failed to materialize from the
	 * warp map is exactly the caller mistake this refuses. */
	why = NULL;
	rc = weave_vecdoc_chan_init(&a, &inner.ch, NULL, 4, &why);
	CHECK(7, rc == -1 && why != NULL, "a NULL docid[] was accepted");
	why = NULL;
	rc = weave_vecdoc_chan_init(&a, NULL, p7_docid, 4, &why);
	CHECK(7, rc == -1 && why != NULL, "a NULL inner channel was accepted");

	/* A descending pair at position 1, in the middle, and at the last
	 * position -- each breaks the ascending premise every inequality in the
	 * header rests on, wherever in the array it occurs. */
	{
		static const int test_positions[3] = {1, 3, 5};
		int			ti;

		for (ti = 0; ti < 3; ti++)
		{
			weave_ft_uint64 arr[6] = {10, 20, 30, 40, 50, 60};

			pos = test_positions[ti];
			arr[pos] = arr[pos - 1] - 1;	/* a descent right at `pos` */

			why = NULL;
			rc = weave_vecdoc_chan_init(&a, &inner.ch, arr, 6, &why);
			CHECK(7, rc == -1 && why != NULL,
				  "a descending pair at position %d was accepted", pos);
		}
	}

	/* An EQUAL pair (duplicate docid) at position 1, in the middle, and at
	 * the last position -- the same three positions the descending case
	 * above uses, and refused for the same reason: init() requires STRICTLY
	 * ascending order, so `docid[i] <= docid[i - 1]` is refused, not only
	 * `<`.  A docid carried by two lanes is not a relabelling, it is an
	 * aggregation over those lanes; weave_vecdoc_chan_init()'s argument in
	 * include/weave/vecdocmap.h records the counterexample this file found
	 * that is the reason for the refusal. */
	{
		static const int test_positions[3] = {1, 3, 5};
		int			ti;

		for (ti = 0; ti < 3; ti++)
		{
			weave_ft_uint64 arr[6] = {10, 20, 30, 40, 50, 60};

			pos = test_positions[ti];
			arr[pos] = arr[pos - 1];	/* a duplicate right at `pos` */

			why = NULL;
			rc = weave_vecdoc_chan_init(&a, &inner.ch, arr, 6, &why);
			CHECK(7, rc == -1 && why != NULL,
				  "a duplicate docid at position %d was accepted", pos);
		}
	}

	/* The maximum docid at or beyond WEAVE_FUSE_END: the fused position space
	 * is 32 bits and a docid is 64, so this is a real ceiling, not a
	 * formality (per weave_vecdoc_chan_init()'s WEAVE_FUSE_END ceiling check). */
	{
		weave_ft_uint64 arr[3];

		arr[0] = 1;
		arr[1] = 2;
		arr[2] = (weave_ft_uint64) WEAVE_FUSE_END;
		why = NULL;
		rc = weave_vecdoc_chan_init(&a, &inner.ch, arr, 3, &why);
		CHECK(7, rc == -1 && why != NULL, "a docid == WEAVE_FUSE_END was accepted");

		arr[2] = (weave_ft_uint64) WEAVE_FUSE_END + 1000;
		why = NULL;
		rc = weave_vecdoc_chan_init(&a, &inner.ch, arr, 3, &why);
		CHECK(7, rc == -1 && why != NULL, "a docid past WEAVE_FUSE_END was accepted");

		arr[2] = (weave_ft_uint64) WEAVE_FUSE_END - 1;
		why = NULL;
		rc = weave_vecdoc_chan_init(&a, &inner.ch, arr, 3, &why);
		CHECK(7, rc == 0 && why == NULL,
			  "the largest legal docid (WEAVE_FUSE_END - 1) was refused: %s",
			  why ? why : "(no reason)");
	}
}

/* ---------------------------------------------------------------------------
 * P9: the real core, mixing an adapted vector channel with a native
 * docid-space required channel.  A native docid-space gate is a five-function
 * reuse of the plumbing P2-P5 already needed, which is why this is written
 * rather than skipped: it is the only property here that would notice the
 * adapter wired into weave_fuse_init()'s channel array incorrectly even though
 * every one of P1-P8 passes.
 *
 * NOTE ON WHAT P9 CANNOT SEE.  Both weave_fuse_run() and weave_fuse_reference()
 * reach the vector channel's contribution ONLY through vecdoc_seek()/
 * vecdoc_score(), so both are blind to the SAME lane at a duplicate docid in
 * exactly the same way (see this file's header comment) -- they agree with
 * each other, not with an independent ground truth.  P9 is therefore not a
 * second check on that finding; P4 is the only property here with an oracle
 * independent of the adapter's own composition.
 * ------------------------------------------------------------------------- */

#define P9_MAXWARP	1024
#define P9_MAXENT	256

typedef struct DocGateSpec
{
	weave_ft_uint32 pos[P9_MAXENT];	/* ascending, distinct docids matched */
	int			n;
} DocGateSpec;

typedef struct DocGateChan
{
	WeaveFuseChan ch;
	const DocGateSpec *spec;
	weave_ft_uint32 last_ret;
	int			seeked;
} DocGateChan;

static int
docgate_first_at(const DocGateSpec *sp, weave_ft_uint32 target)
{
	int			i;

	for (i = 0; i < sp->n; i++)
	{
		if (sp->pos[i] >= target)
			return i;
	}
	return sp->n;
}

static weave_ft_uint32
docgate_seek(WeaveFuseChan *c, weave_ft_uint32 target)
{
	DocGateChan *s = (DocGateChan *) c->state;
	const DocGateSpec *sp = s->spec;
	int			i;
	weave_ft_uint32 ret;

	assert(!(s->seeked && target < s->last_ret));
	s->seeked = 1;

	i = docgate_first_at(sp, target);
	ret = (i < sp->n) ? sp->pos[i] : WEAVE_FUSE_END;
	c->blkend = ret;			/* a gate's skip IS the seek, per gate.h */
	s->last_ret = ret;
	return ret;
}

static float
docgate_block_max(WeaveFuseChan *c)
{
	(void) c;
	return INFINITY;			/* (C5): a gate's ceiling is +INF */
}

static float
docgate_score(WeaveFuseChan *c)
{
	(void) c;
	return 0.0f;				/* (C4): a match contributes nothing additive */
}

static const WeaveFuseChanOps docgate_ops = {
	docgate_seek, docgate_block_max, docgate_score
};

static void
docgate_bind(DocGateChan *s, const DocGateSpec *sp)
{
	memset(s, 0, sizeof(*s));
	s->spec = sp;
	s->ch.ops = &docgate_ops;
	s->ch.state = s;
	s->ch.weight = 1.0f;
	s->ch.maxscore = INFINITY;
	s->ch.required = 1;
}

static void
gen_docgate_spec(DocGateSpec *sp, weave_ft_uint32 nwarp, weave_ft_uint32 pct)
{
	weave_ft_uint32 i;

	sp->n = 0;
	for (i = 0; i < nwarp && sp->n < P9_MAXENT; i++)
	{
		if (rnd_below(100) < pct)
			sp->pos[sp->n++] = i;
	}
	if (sp->n == 0)
		sp->pos[sp->n++] = nwarp - 1;
}

/* A STRICTLY ascending docid[] bounded to [0, maxval], so it shares a docid
 * universe with the native docid-space channel's nwarp -- and so that this
 * generator, which drives the REAL adapter through weave_vecdoc_chan_init()
 * in p9_trial(), never hands it a map init() refuses.  The caller guarantees
 * maxval >= n - 1 (nlane <= nwarp, so nwarp - 1 >= nlane - 1), which is
 * exactly enough room for n distinct ascending values; `room` below is kept
 * non-negative by leaving at least 1 unit per remaining slot so every later
 * mandatory +1 step still fits before maxval. */
static void
gen_bounded_docid(weave_ft_uint64 *docid, weave_ft_uint32 n, weave_ft_uint32 maxval)
{
	weave_ft_uint64 cur = 0;
	weave_ft_uint32 i;

	for (i = 0; i < n; i++)
	{
		weave_ft_uint32 remaining = n - i;	/* slots left, this one included */
		weave_ft_uint64 room;

		if (i > 0)
			cur += 1;			/* the mandatory strictly-ascending step */

		room = (maxval >= cur + (remaining - 1)) ?
			(maxval - cur - (remaining - 1)) : 0;
		cur += rnd_below((uint32_t) (room + 1));
		docid[i] = cur;
	}
}

static float
lane_true_max(const LaneSpec *sp)
{
	float		m = 0.0f;
	weave_ft_uint32 i;

	for (i = 0; i < sp->nlane; i++)
	{
		if (sp->has[i] && sp->score[i] > m)
			m = sp->score[i];
	}
	return m;
}

static weave_ft_uint64 p9_docid_fast[P9_MAXWARP + 16];
static weave_ft_uint64 p9_docid_ref[P9_MAXWARP + 16];

static void
p9_trial(void)
{
	weave_ft_uint32 nwarp = 32 + rnd_below(P9_MAXWARP - 32);
	weave_ft_uint32 nlane = 1 + rnd_below(nwarp);
	LaneSpec	lspec;
	LaneChan	inner_fast;
	LaneChan	inner_ref;
	WeaveVecDocChan vfast;
	WeaveVecDocChan vref;
	DocGateSpec gspec;
	DocGateChan gfast;
	DocGateChan gref;
	WeaveFuseChan *fchan[2];
	WeaveFuseChan *rchan[2];
	WeaveFuseHit fhit[8];
	WeaveFuseHit rhit[8];
	WeaveFuseHit heap[8];
	WeaveFuseState st;
	WeaveFuseError err;
	const char *why;
	int			k = 1 + (int) rnd_below(8);
	int			nf;
	int			nr;
	int			i;
	float		tm;
	float		slack;

	if (nlane > MAXLANE)
		nlane = MAXLANE;

	gen_lane_spec(&lspec, nlane, 5 + rnd_below(90));
	gen_bounded_docid(p9_docid_fast, nlane, nwarp - 1);
	memcpy(p9_docid_ref, p9_docid_fast, nlane * sizeof(*p9_docid_fast));

	gen_docgate_spec(&gspec, nwarp, 5 + rnd_below(60));

	lane_bind(&inner_fast, &lspec);
	lane_bind(&inner_ref, &lspec);

	/*
	 * THE TWO ARMS MUST CARRY THE SAME WEIGHT, and lane_bind() draws a RANDOM one
	 * from the shared generator -- so calling it twice gives the two arms different
	 * weights and the comparison below then reports a wrong answer from a harness
	 * that asked two different questions.  This line is the whole fix, and it is
	 * worth its comment: the first run of this property blamed the adapter for a
	 * 7.5-versus-27.5 disagreement that was entirely this.
	 */
	inner_ref.ch.weight = inner_fast.ch.weight;

	/* The two channels' ceilings must be TRUE ceilings for the real core's
	 * partition and block prune to be sound, unlike P1-P8 which never call
	 * weave_fuse_init()/weave_fuse_run() and so never rely on maxscore being
	 * more than a copied field.
	 *
	 * `tm * inflate` AND NOT `tm`, and getting that wrong is what the first run of
	 * this property found: lane_block_max() returns the block's true maximum times
	 * `inflate`, so with a slack bound the CEILING has to cover the slack too.  A
	 * maxscore below some block_max() is not a bug in the core or in the adapter --
	 * it makes the core's suffix arithmetic unsound, and the fused answer then
	 * legitimately differs from the reference.  A harness that gets this wrong
	 * reports a wrong answer against code that is behaving exactly as documented. */
	tm = lane_true_max(&lspec) * lspec.inflate;
	slack = (rnd_below(4) == 0) ? (float) rnd_below(4) : 0.0f;
	inner_fast.ch.maxscore = tm + slack;
	inner_ref.ch.maxscore = tm + slack;

	why = NULL;
	if (weave_vecdoc_chan_init(&vfast, &inner_fast.ch, p9_docid_fast, nlane, &why) != 0)
		return;
	why = NULL;
	if (weave_vecdoc_chan_init(&vref, &inner_ref.ch, p9_docid_ref, nlane, &why) != 0)
		return;

	docgate_bind(&gfast, &gspec);
	docgate_bind(&gref, &gspec);

	fchan[0] = &vfast.chan;
	fchan[1] = &gfast.ch;
	rchan[0] = &vref.chan;
	rchan[1] = &gref.ch;

	err = weave_fuse_init(&st, fchan, 2, heap, k, NULL, nwarp);
	CHECK(9, err == WEAVE_FUSE_OK, "weave_fuse_init failed: %d", (int) err);
	if (err != WEAVE_FUSE_OK)
		return;
	st.check_bounds = 1;

	err = weave_fuse_run(&st);
	CHECK(9, err == WEAVE_FUSE_OK,
		  "weave_fuse_run failed: %d (nwarp=%u nlane=%u k=%d)",
		  (int) err, nwarp, nlane, k);
	if (err != WEAVE_FUSE_OK)
		return;

	nf = weave_fuse_drain(&st, fhit);
	nr = weave_fuse_reference(rchan, 2, NULL, nwarp, k, rhit);

	CHECK(9, nf == nr, "row count %d != reference %d (nwarp=%u nlane=%u k=%d)",
		  nf, nr, nwarp, nlane, k);
	for (i = 0; i < nf && i < nr; i++)
	{
		CHECK(9, fhit[i].warp == rhit[i].warp && fhit[i].score == rhit[i].score,
			  "row %d: fused (%u,%.6f) != reference (%u,%.6f) (nwarp=%u nlane=%u k=%d)",
			  i, fhit[i].warp, (double) fhit[i].score,
			  rhit[i].warp, (double) rhit[i].score, nwarp, nlane, k);
	}
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
	long		ntrial = 20000;
	long		i;

	if (argc > 1)
		ntrial = atol(argv[1]);

	printf("== F8 vecdocmap: lane-to-docid relabelling preserves (C1)/(C2) ==\n");

	for (i = 0; i < ntrial; i++)
		lower_bound_oracle_trial();

	for (i = 0; i < ntrial; i++)
		main_trial();

	/*
	 * A dedicated pass at "a few thousand" lanes.  gen_nlane() already draws
	 * that size about one trial in eight, but a dedicated pass removes any
	 * doubt that the O(nlane) oracle checks above were exercised at the size
	 * weave_vecdoc_lower_bound()'s cost note is about.
	 */
	for (i = 0; i < 300; i++)
	{
		main_trial();
		lower_bound_oracle_trial();
	}

	for (i = 0; i < ntrial; i++)
		control_a_trial();
	for (i = 0; i < ntrial; i++)
		control_b_trial();

	init_refusal_trials();

	for (i = 0; i < ntrial / 2 + 2000; i++)
		p9_trial();

	printf("trials: %ld\n", ntrial);
	printf("P1 lower_bound == oracle       : %8ld checks\n", prop_checks[1]);
	printf("P2 (C1) monotone, >= target    : %8ld checks\n", prop_checks[2]);
	printf("P3 seek lands on the right lane: %8ld checks\n", prop_checks[3]);
	printf("P4 (C2) interval coverage      : %8ld checks\n", prop_checks[4]);
	printf("P5 score() at the right lane   : %8ld checks\n", prop_checks[5]);
	printf("P7 init refusals                : %8ld checks\n", prop_checks[7]);
	printf("P8 weight/maxscore/required copied: %8ld checks\n", prop_checks[8]);
	printf("P9 real core == reference      : %8ld checks\n", prop_checks[9]);
	printf("P6(a) unclamped blkend fires   : %8ld of %8ld trials (%.3f%%)\n",
		   n_ctrl_a_fires, n_ctrl_a_trials,
		   n_ctrl_a_trials ? 100.0 * (double) n_ctrl_a_fires / (double) n_ctrl_a_trials : 0.0);
	printf("P6(b) flipped comparator fires : %8ld of %8ld trials (%.3f%%)\n",
		   n_ctrl_b_fires, n_ctrl_b_trials,
		   n_ctrl_b_trials ? 100.0 * (double) n_ctrl_b_fires / (double) n_ctrl_b_trials : 0.0);

	if (n_ctrl_a_fires == 0)
	{
		printf("P6 FAIL: the unclamped-blkend control never fired, so this "
			   "test cannot see the class of bug the clamp exists for\n");
		failures++;
	}
	if (n_ctrl_b_fires == 0)
	{
		printf("P6 FAIL: the flipped-comparator control never fired, so this "
			   "test cannot see the class of bug the comparator direction "
			   "protects against\n");
		failures++;
	}

	if (failures > 0)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		printf("F8 vecdocmap: %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	printf("F8 vecdocmap: %ld checks, %ld failures\n", checks, failures);
	return 0;
}
