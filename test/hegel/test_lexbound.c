/*-------------------------------------------------------------------------
 *
 * test_lexbound.c
 *		Contract (C2) for the BM25 lexical channel: the per-block bound in
 *		include/weave/bm25bound.h is a true, attained upper bound on the
 *		per-posting contribution.  Task F6.
 *
 * THIS IS THE TEST AGENTS.md HARD RULE 1 REQUIRES for the lexical channel, and
 * it is the one channel where the rule's own example is literal: "a bound that
 * is 1 % too low silently drops rows, and no fixed-expected-output regression
 * test can catch it".  The WAND prune that consumes this bound
 * (weave_search_bmw in src/am/amscan.c) abandons whole 128-posting blocks on it;
 * a bound a hair too low abandons a block containing a true top-k document and
 * returns k plausible rows, one of which is wrong.  test/hegel/test_fuse_props.c
 * measures how silent that is -- a bound scaled to 0.99 changes a fraction of a
 * percent of answers -- which is exactly why it needs a property test and not a
 * pinned query.
 *
 * WHAT MAKES THIS A TEST OF PRODUCTION CODE.  Before F6, amscan.c carried its
 * own transcription of both formulas, so a test like this one would have been
 * testing a copy nobody runs -- the failure include/weave/for.h's header comment
 * records for the doclen walk, where a real change to a gate shipped with zero
 * coverage because the test had its own copy and kept passing.  F6 routed the
 * WAND through bm25bound.h, so the functions below are the functions the scan
 * calls.
 *
 * Properties, over randomized (idf, k1, b, avgdl, max_tf, min_doclen) block
 * descriptions and randomized (tf, doclen) postings drawn INSIDE the block's
 * declared extremes (1 <= tf <= max_tf, doclen >= min_doclen):
 *
 *	L1	(C2) block_bound >= contribution, for every posting the block may hold
 *	L2	TIGHTNESS: the bound is ATTAINED at (tf = max_tf, doclen = min_doclen),
 *		checked as BITWISE equality.  A bound that is never attained is a bound
 *		that is silently loose, and in this header the equality is structural --
 *		weave_bm25_block_bound() forwards to weave_bm25_contrib() -- so this
 *		property is the guard against someone "simplifying" it back into an
 *		independent expression, which is what amscan.c used to contain and what
 *		would reintroduce the last-bit hazard L1 is then exposed to
 *	L3	MONOTONICITY: the contribution is non-decreasing in tf and non-increasing
 *		in doclen.  This is the fact the bound rests on -- given L2, (C2) IS this
 *		-- so it is checked directly and not inferred from L1 passing
 *	L4	FINITENESS: no NaN and no infinity anywhere, including the degenerate
 *		avgdl = 0, b = 0, b = 1, k1 = 0 and min_doclen = 0 configurations
 *	L5	maxscore >= block_max (include/weave/channel.h): the term-wide ceiling
 *		weave_bm25_term_bound() is never below the block bound.  This is the one
 *		inequality in the header between two DIFFERENT roundings of idf*(k1+1),
 *		so it is the one that could fail in the last bit; see the header's note
 *	L6	the (idf, tf, doclen, k1, b, avgdl) convenience forms agree BITWISE with
 *		the precomputed-factors forms the scan uses, so the two cannot drift
 *
 * THE LAST-BIT RULE, AND WHY IT IS TWO LEGS RATHER THAN A TOLERANCE.  Each of
 * L1, L3 and L5 is an inequality between two floating-point evaluations, and
 * floating point does not make a composed expression monotone just because the
 * real function is.  Two legs, so the strong claim is not weakened to cover the
 * degenerate case:
 *
 *	 - THE MAIN LEG (k1 >= 0.1, min_doclen >= 1, avgdl in [1, 4096]) demands
 *	   EXACT inequalities, no tolerance.  It can: the real gap between the bound
 *	   and a contribution one whole tf below it is at least
 *	   A/(max_tf*(tf + A)) relative with A = k1*(1-b) + (k1*b/avgdl)*min_doclen,
 *	   which over these ranges is at worst ~7e-13 -- about a thousand times the
 *	   ~5 ulp (5.5e-16) the five-operation expression can accumulate.  That is
 *	   the reachable domain: the scan hardcodes k1 = 1.2 and b = 0.75, and a
 *	   document containing a term has |D| >= 1.
 *	 - THE DEGENERATE LEG (k1 = 0, or b = 1 with min_doclen = 0, or avgdl = 0)
 *	   permits an 8-ulp relative slack and COUNTS how often it was needed, which
 *	   is reported.  With k1 = 0 the saturation function is constant in tf --
 *	   contrib is idf regardless -- so "non-decreasing in tf" is a statement
 *	   about rounding and nothing else.  A violation larger than the slack still
 *	   fails.
 *
 * COVERAGE IS ASSERTED, NOT HOPED FOR, which is F5's lesson (see the
 * COVERAGE FAIL arm of test/hegel/test_fuse_props.c and AGENTS.md hard rule 11:
 * a harness can make a number up).  A generator that only ever drew
 * tf = max_tf would make L1 pass while testing nothing but L2, and one that
 * never drew the extreme would never test tightness at all.  So the run FAILS
 * if it produced no near-tight case (contribution within 1 % of the bound) or no
 * slack case (bound more than 2x the contribution).
 *
 * Build and run:
 *		cc -O2 -Wall -Wextra -I include -o /tmp/tl test/hegel/test_lexbound.c -lm
 *		/tmp/tl [ntrials]
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_lexbound.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "weave/bm25bound.h"

static long failures = 0;
static long checks = 0;
static long prop_checks[8];

#define CHECK(prop, cond, ...) \
	do { \
		checks++; \
		prop_checks[(prop)]++; \
		if (!(cond)) \
		{ \
			failures++; \
			if (failures <= 20) \
			{ \
				printf("FAIL L%d %s:%d: ", (prop), __FILE__, __LINE__); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

/* xorshift64*, seeded fixed so a failure reproduces. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t
rnd64(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static uint32_t
rnd_below(uint32_t n)
{
	return n == 0 ? 0 : (uint32_t) (rnd64() % n);
}

/* Uniform in [lo, hi]. */
static double
rnd_double(double lo, double hi)
{
	double		u = (double) (rnd64() >> 11) / 9007199254740992.0;	/* 2^53 */

	return lo + (hi - lo) * u;
}

/* ---------------------------------------------------------------------------
 * The last-bit rule (see the header comment): a >= b exactly, or -- in the
 * degenerate leg only -- within 8 ulp of b, in which case the caller counts it.
 * ------------------------------------------------------------------------- */

#define ULP_SLACK	(8.0 * 2.220446049250313e-16)	/* 8 * DBL_EPSILON */

static long slack_used = 0;

static int
ge_ok(double a, double b, int allow_slack)
{
	if (a >= b)
		return 1;
	if (!allow_slack)
		return 0;
	if (b - a <= ULP_SLACK * fabs(b))
	{
		slack_used++;
		return 1;
	}
	return 0;
}

static int
finite_ok(double v)
{
	return !isnan(v) && !isinf(v);
}

/*
 * L5 is the one comparison in bm25bound.h between two different roundings of
 * idf*(k1+1) -- the term-wide ceiling is the verbatim expression amscan.c has
 * always used, the block bound routes through the precomputed factor -- so its
 * margin is not a rounding argument but the length-norm term the ceiling omits:
 * the block bound's denominator exceeds it by (k1*b/avgdl)*min_doclen.  Where
 * that margin is comfortable the inequality is exact and this test demands it;
 * where it vanishes (b = 0, k1 = 0, or min_doclen = 0 -- none reachable from a
 * scan, which hardcodes k1 = 1.2, b = 0.75 and cannot have a posting in a
 * zero-length document) the two can differ in the last bit and the slack
 * applies.  1e-9 relative is ~4e6 ulp: far below the reachable configuration's
 * margin (b = 0.75 with |D| >= 1 gives ~2e-4 relative) and far above the
 * five-operation rounding error of ~1e-15.
 */
static int
l5_needs_slack(const WeaveBm25Factors *f, double max_tf, double min_doclen)
{
	double		margin = f->k1b_inv_avgdl * min_doclen;

	return !(margin > 1e-9 * (max_tf + f->k1_1mb));
}

/* ---------------------------------------------------------------------------
 * Coverage counters, asserted at the end
 * ------------------------------------------------------------------------- */

static long n_near_tight = 0;	/* contribution within 1 % of the bound */
static long n_attained = 0;		/* contribution EQUAL to the bound */
static long n_slack2x = 0;		/* bound more than 2x the contribution */
static long n_degenerate = 0;	/* trials in the degenerate leg */

/* ---------------------------------------------------------------------------
 * One trial: one block description, several postings inside it
 * ------------------------------------------------------------------------- */

typedef struct BlockDraw
{
	double		idf;
	double		k1;
	double		b;
	double		avgdl;
	double		max_tf;
	double		min_doclen;
	int			degenerate;
} BlockDraw;

/*
 * The main leg's ranges are the reachable domain, and each bound is the reason
 * the exact-inequality claim above holds: k1 >= 0.1 keeps the length norm from
 * vanishing, min_doclen >= 1 does the same when b = 1 (a document containing the
 * term has |D| >= 1, and for a v4 sidecar the quantized floor of 1 is still 1),
 * and max_tf <= 4096 with avgdl >= 1 caps the relative gap's denominator.
 */
static void
gen_main(BlockDraw *d)
{
	uint32_t	r = rnd_below(4);

	d->degenerate = 0;
	d->idf = rnd_below(8) == 0 ? 0.0 : rnd_double(0.0, 12.0);

	/* The defaults the scan actually uses, a quarter of the time, so the
	 * reachable configuration is not a rare draw. */
	if (r == 0)
	{
		d->k1 = 1.2;
		d->b = 0.75;
	}
	else
	{
		d->k1 = rnd_double(0.1, 3.0);
		d->b = rnd_below(6) == 0 ? (double) rnd_below(2) : rnd_double(0.0, 1.0);
	}
	d->avgdl = rnd_double(1.0, 4096.0);
	d->max_tf = (double) (1 + rnd_below(4096));
	d->min_doclen = (double) (1 + rnd_below(1u << 20));
}

/*
 * The degenerate leg: every configuration the header calls out as outside the
 * reachable domain but inside the function's declared totality.  avgdl = 0 is
 * the interesting one -- weave_bm25_factors_init() clamps it to 1.0, which is
 * what src/query/rank.c:130 already does, and the clamp is what makes L4
 * assertable at all instead of the test having to avoid the case.
 */
static void
gen_degenerate(BlockDraw *d)
{
	d->degenerate = 1;
	d->idf = rnd_below(4) == 0 ? 0.0 : rnd_double(0.0, 12.0);
	d->k1 = rnd_double(0.1, 3.0);
	d->b = rnd_double(0.0, 1.0);
	d->avgdl = rnd_double(1.0, 4096.0);
	d->max_tf = (double) (1 + rnd_below(4096));
	d->min_doclen = (double) rnd_below(1u << 20);

	switch (rnd_below(6))
	{
		case 0:
			d->k1 = 0.0;		/* no saturation: contrib is idf, flat in tf */
			break;
		case 1:
			d->b = 0.0;			/* no length normalization at all */
			break;
		case 2:
			d->b = 1.0;			/* full length normalization */
			d->min_doclen = 0.0;
			break;
		case 3:
			d->avgdl = 0.0;		/* clamped to 1.0; see the comment above */
			break;
		case 4:
			d->min_doclen = 0.0;	/* a zero-length document in the block */
			break;
		case 5:
			d->max_tf = 1.0;	/* a block where every posting is at the extreme */
			d->min_doclen = 0.0;
			break;
	}
}

static void
one_trial(int degenerate)
{
	BlockDraw	d;
	WeaveBm25Factors f;
	double		bound;
	double		at_extreme;
	double		term;
	int			p;

	if (degenerate)
	{
		gen_degenerate(&d);
		n_degenerate++;
	}
	else
		gen_main(&d);

	weave_bm25_factors_init(&f, d.idf, d.k1, d.b, d.avgdl);
	bound = weave_bm25_block_bound(&f, d.max_tf, d.min_doclen);
	at_extreme = weave_bm25_contrib(&f, d.max_tf, d.min_doclen);
	term = weave_bm25_term_bound(&f, d.max_tf);

	/* L4 on the three block-level values. */
	CHECK(4, finite_ok(bound), "bound %g not finite (idf=%g k1=%g b=%g avgdl=%g mtf=%g mdl=%g)",
		  bound, d.idf, d.k1, d.b, d.avgdl, d.max_tf, d.min_doclen);
	CHECK(4, finite_ok(term), "term bound %g not finite (idf=%g k1=%g b=%g avgdl=%g mtf=%g)",
		  term, d.idf, d.k1, d.b, d.avgdl, d.max_tf);

	/*
	 * L2 TIGHTNESS, bitwise.  Not "within a tolerance": the bound IS the
	 * contribution at the extremes, by one forwarding call, and that is what
	 * makes (C2) a monotonicity statement rather than a comparison of two
	 * independent roundings.  If this ever fails, someone gave the bound its own
	 * expression again and L1 needs re-deriving from scratch.
	 */
	CHECK(2, bound == at_extreme,
		  "bound %.17g != contribution at the extreme %.17g", bound, at_extreme);
	if (bound == at_extreme)
		n_attained++;

	/* L5 maxscore >= block_max (include/weave/channel.h).  Exact wherever the
	 * length norm the ceiling omits is a real margin; see l5_needs_slack(). */
	CHECK(5, ge_ok(term, bound,
				   d.degenerate || l5_needs_slack(&f, d.max_tf, d.min_doclen)),
		  "term bound %.17g below block bound %.17g (k1=%g b=%g avgdl=%g mtf=%g mdl=%g)",
		  term, bound, d.k1, d.b, d.avgdl, d.max_tf, d.min_doclen);

	/* L6: the convenience forms are the same numbers, bitwise. */
	CHECK(6, weave_bm25_block_bound_at(d.idf, d.max_tf, d.min_doclen,
									   d.k1, d.b, d.avgdl) == bound,
		  "block_bound_at %.17g != block_bound %.17g",
		  weave_bm25_block_bound_at(d.idf, d.max_tf, d.min_doclen,
									d.k1, d.b, d.avgdl), bound);

	/* The postings.  Probe 0 is always the extreme, so the near-tight coverage
	 * the run asserts is produced by construction rather than by luck. */
	for (p = 0; p < 8; p++)
	{
		double		tf;
		double		doclen;
		double		c;

		if (p == 0)
		{
			tf = d.max_tf;
			doclen = d.min_doclen;
		}
		else
		{
			tf = (double) (1 + rnd_below((uint32_t) d.max_tf));
			switch (rnd_below(4))
			{
				case 0:
					doclen = d.min_doclen;	/* shortest document, random tf */
					break;
				case 1:
					doclen = d.min_doclen + (double) rnd_below(64);
					break;
				case 2:
					doclen = d.min_doclen + (double) rnd_below(1u << 20);
					break;
				default:
					doclen = d.min_doclen + rnd_double(0.0, 1.0e6);
					break;
			}
		}

		c = weave_bm25_contrib(&f, tf, doclen);

		CHECK(4, finite_ok(c),
			  "contribution %g not finite (tf=%g dl=%g idf=%g k1=%g b=%g avgdl=%g)",
			  c, tf, doclen, d.idf, d.k1, d.b, d.avgdl);

		/* L1 (C2). */
		CHECK(1, ge_ok(bound, c, d.degenerate),
			  "bound %.17g below contribution %.17g (tf=%g<=%g dl=%g>=%g idf=%g k1=%g b=%g avgdl=%g)",
			  bound, c, tf, d.max_tf, doclen, d.min_doclen,
			  d.idf, d.k1, d.b, d.avgdl);

		/* L6 for the per-posting form. */
		CHECK(6, weave_bm25_contrib_at(d.idf, tf, doclen, d.k1, d.b, d.avgdl) == c,
			  "contrib_at %.17g != contrib %.17g",
			  weave_bm25_contrib_at(d.idf, tf, doclen, d.k1, d.b, d.avgdl), c);

		/* Coverage.  idf == 0 makes both zero and the ratio meaningless. */
		if (bound > 0.0)
		{
			if (c > 0.99 * bound)
				n_near_tight++;
			if (c * 2.0 < bound)
				n_slack2x++;
		}

		/*
		 * L3 MONOTONICITY, both directions, from this posting: one more tf at
		 * the same length must not score lower, and one longer document at the
		 * same tf must not score higher.  Given L2 this IS (C2) -- L1 above is
		 * the same fact checked end to end -- and it is asserted separately
		 * because a bound can be correct while the function it bounds has
		 * stopped being monotone, at which point the NEXT bound derived from
		 * max_tf and min_doclen is wrong for a reason no L1 draw need reveal.
		 */
		if (tf + 1.0 <= d.max_tf)
		{
			double		up = weave_bm25_contrib(&f, tf + 1.0, doclen);

			CHECK(3, ge_ok(up, c, d.degenerate),
				  "contribution fell when tf rose: %.17g -> %.17g (tf=%g dl=%g k1=%g b=%g)",
				  c, up, tf, doclen, d.k1, d.b);
			CHECK(4, finite_ok(up), "tf+1 contribution %g not finite", up);
		}
		{
			double		longer = weave_bm25_contrib(&f, tf, doclen + 1.0);

			CHECK(3, ge_ok(c, longer, d.degenerate),
				  "contribution rose when |D| rose: %.17g -> %.17g (tf=%g dl=%g k1=%g b=%g avgdl=%g)",
				  c, longer, tf, doclen, d.k1, d.b, d.avgdl);
			CHECK(4, finite_ok(longer), "longer-doc contribution %g not finite", longer);
		}
	}
}

int
main(int argc, char **argv)
{
	long		trials = 0;
	long		i;
	long		ntrial = 200000;

	if (argc > 1)
		ntrial = atol(argv[1]);

	printf("== F6 lexical bound (C2): block_bound >= contribution, attained at "
		   "(max_tf, min_doclen) ==\n");

	for (i = 0; i < ntrial; i++)
	{
		/* One trial in six is degenerate: enough to exercise every case
		 * gen_degenerate() can produce many times over, few enough that the
		 * exact-inequality leg is the bulk of the evidence. */
		one_trial(rnd_below(6) == 0);
		trials++;
	}

	printf("trials: %ld (%ld degenerate)\n", trials, n_degenerate);
	printf("L1 (C2) bound >= contrib : %8ld checks\n", prop_checks[1]);
	printf("L2 attained at extremes  : %8ld checks\n", prop_checks[2]);
	printf("L3 monotone in tf, |D|   : %8ld checks\n", prop_checks[3]);
	printf("L4 finite everywhere     : %8ld checks\n", prop_checks[4]);
	printf("L5 maxscore >= block_max : %8ld checks\n", prop_checks[5]);
	printf("L6 params == factors form: %8ld checks\n", prop_checks[6]);
	printf("coverage: near-tight=%ld attained=%ld bound>2x=%ld "
		   "last-bit slack used=%ld (degenerate leg only)\n",
		   n_near_tight, n_attained, n_slack2x, slack_used);

	/*
	 * Assert the coverage rather than hoping for it.  Either of these at zero
	 * means the generator stopped producing the case, so the property it was
	 * meant to test is untested rather than established -- and both failure
	 * modes are invisible in a pass: no near-tight case and L1 is satisfied by a
	 * bound many times too large, no loose case and L1 is only ever checked
	 * where it is an equality.
	 */
	if (n_near_tight == 0 || n_slack2x == 0)
	{
		printf("COVERAGE FAIL: near-tight=%ld bound>2x=%ld -- a bound is only "
			   "tested by drawing postings both at and far from the extreme\n",
			   n_near_tight, n_slack2x);
		failures++;
	}

	if (failures > 0)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
