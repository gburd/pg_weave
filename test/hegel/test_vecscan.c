/*-------------------------------------------------------------------------
 *
 * test_vecscan.c
 *		The V8 code-scan decision core: (C1), (C2), and what masking may skip.
 *
 * THE ONE THAT MATTERS IS (C2), AND IT IS ASSERTED IN THE DOMAIN THE CALLER SEES.
 * test_vecbound.c already asserts that weave_block_bound_ip() dominates a kernel
 * score.  That is not the same statement as the one the fused loop depends on,
 * because a kernel returns an INNER PRODUCT while weave_block_bound_l2() bounds
 * -||q - v||^2, and a channel that reported the second next to the first would not
 * have a bound that is slightly wrong -- it would have two numbers in different
 * units, and (C2) would be meaningless rather than violated
 * (include/weave/vecscan.h, "THE DOMAIN RULE").  So this test asserts
 *
 *		weave_vec_scan_block()'s bound  >=  weave_vec_scan_lane_score(kernel ip)
 *
 * for both metrics, with and without an allowlist, and separately WITNESSES that
 * the L2 run really took the L2 branch of both the bound and the conversion --
 * counted and printed, because a test that would have passed with the two branches
 * fused is exactly the test that missed the bug the contract was fixed for.
 *
 * The other properties, and why each is here rather than left to review:
 *
 *	P1	(C1): firstwarp ascends strictly across a walk, and a record whose
 *		firstwarp is not blockno * 32, or not above the last one accepted, is
 *		REFUSED.  Both halves, because firstwarp comes off a page.
 *	P2	(C2) as above, over both metrics x allowlist/no allowlist, including
 *		blocks the bound skipped: a bound is asserted whether or not it pruned.
 *	P3	the mask short-circuit really short-circuits.  Proven, not asserted: the
 *		block's codes AND its centroid code are overwritten with a poison pattern
 *		first, and the test establishes that reading either would produce a
 *		different number, so "no code byte was read" has teeth.  `bound_out` is
 *		left at a sentinel the core never writes.
 *	P4	masking does not perturb survivors: with exactly one lane allowed, that
 *		lane's score is bit-identical to its score with no allowlist at all.
 *	P5	maxscore dominates every block bound seen, for both metrics -- the
 *		WeaveShuttle.maxscore >= block_max() half of the contract.
 *	P6	the sentinel survives: WEAVE_KERNEL_NEVER in, WEAVE_KERNEL_NEVER out.
 *	P7	refusals: a lane past nwarp, a NaN or a negative float in the record, a
 *		blockno past nblocks, a metric with no bound, a non-LANE layout, an
 *		allowlist shorter than the weft.
 *
 * THE ALLOWLIST IS ALLOCATED TO EXACTLY ceil(nwarp / 64) WORDS, so that an
 * over-read of the bitmap is an AddressSanitizer report instead of a quiet pass.
 * That is the point of running this file under -fsanitize=address,undefined as
 * well as -O2; a short tail block (nvec not a multiple of 32) is in the shape
 * list for the same reason.
 *
 * Build and run:
 *		cc -O2 -I include -o /tmp/tvs test/hegel/test_vecscan.c \
 *			src/vector/vecscan.c src/vector/vecweft.c src/vector/vecpage.c \
 *			src/vector/vecstats.c src/vector/kernels.c src/vector/quantize.c \
 *			src/vector/pack.c -lm && /tmp/tvs
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_vecscan.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/vecscan.h"

/* The page payload the backend passes: BLCKSZ 8192 less the 24-byte page header
 * and the 8-byte weave opaque area.  Hard-coded rather than derived, because the
 * point is to test the geometry the shipped writer uses. */
#define PAYLOAD			8160

/* A value weave_vec_scan_block() must never write, so that "did not write
 * *bound_out" is checkable rather than assumed. */
#define BOUND_SENTINEL	12345.0f

/* What the code and centroid buffers are overwritten with in P3.  Any pattern
 * would do; this one is not a valid float and not all-zero, so a block scored
 * from it lands nowhere near the real scores. */
#define POISON			0xA5

static long failures = 0;
static long checks = 0;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			if (failures <= 20) \
			{ \
				printf("FAIL %s:%d: ", __FILE__, __LINE__); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

static weave_uint64 rng_state = 0x9E3779B97F4A7C15ULL;

static weave_uint64
rng(void)
{
	weave_uint64 x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* Uniform in [-1, 1). */
static float
rnd_unit(void)
{
	return (float) ((double) (rng() >> 11) / 9007199254740992.0) * 2.0f - 1.0f;
}

static void *
xalloc(size_t n)
{
	void	   *p = malloc(n);

	if (p == NULL)
	{
		printf("out of memory\n");
		exit(2);
	}
	return p;
}

/*
 * Witnesses that the L2 arithmetic was actually reached and actually differs from
 * the IP arithmetic.  Printed, and required to be nonzero: if a future refactor
 * fused the two branches, (C2) would still "hold" everywhere and these would go
 * to zero.  That is the only mechanical difference between a test of the domain
 * rule and a test that happens to pass under it.
 */
static long l2_bound_witness = 0;
static long l2_score_witness = 0;
static long ip_identity_witness = 0;

/* ---------------------------------------------------------------------------
 * A whole weft in memory: the directory records, the packed code blocks and the
 * centroid codes, built from real encoded vectors so the bound fields are the
 * ones weave_vecblock_stats() computes rather than numbers this file made up.
 * ------------------------------------------------------------------------- */

typedef struct Weft
{
	WeaveVecWeftGeom g;
	WeaveQuantizer q;
	WeaveVecDirRec *recs;		/* nblocks records */
	weave_uint8 *blocks;		/* nblocks * blockbytes */
	weave_uint8 *cencodes;		/* nblocks * codebytes */
	weave_uint64 *allow;		/* EXACTLY nwords words, or NULL */
	size_t		nwords;
} Weft;

static WeaveVecDirRec *
weft_rec(Weft *w, weave_uint32 b)
{
	return &w->recs[b];
}

static weave_uint8 *
weft_codes(Weft *w, weave_uint32 b)
{
	return w->blocks + (size_t) b * (size_t) w->g.blockbytes;
}

static weave_uint8 *
weft_cencode(Weft *w, weave_uint32 b)
{
	return w->cencodes + (size_t) b * (size_t) w->q.codebytes;
}

/*
 * Build a weft of `nvec` lanes.  `deadpct` of the lanes are dead, chosen
 * independently per lane so the holes are SCATTERED through the interior of a
 * block rather than trailing -- which is the state vacuum leaves blocks in, and
 * the state in which a mask that trims the wrong end still passes an
 * all-lanes-live test.  Every block keeps at least one live lane, because a block
 * with none has no statistics to compute (weave_vecblock_stats() refuses it) and
 * the empty-block decision is tested separately.
 *
 * Returns 0, or -1 if the codec declines this (dim, bits) -- not our test.
 */
static int
weft_build(Weft *w, int dim, int bits, weave_uint32 nvec, int deadpct)
{
	weave_uint32 b;
	float	   *v = (float *) xalloc((size_t) dim * sizeof(float));
	float	   *recon;
	float	   *cen = (float *) xalloc((size_t) dim * sizeof(float));
	float	   *lane_scale;
	float	   *lane_norm;
	int		   *slot;
	weave_uint8 *code;

	memset(w, 0, sizeof(*w));
	if (weave_vecweft_geom(&w->g, PAYLOAD, dim, bits, WEAVE_PACK_LANE, nvec) != 0)
	{
		free(v);
		free(cen);
		return -1;
	}
	if (weave_quantizer_init(&w->q, dim, bits, NULL, malloc, free) != 0)
	{
		free(v);
		free(cen);
		return -1;
	}

	w->recs = (WeaveVecDirRec *) xalloc((size_t) w->g.nblocks *
										sizeof(WeaveVecDirRec));
	w->blocks = (weave_uint8 *) xalloc((size_t) w->g.nblocks *
									   (size_t) w->g.blockbytes);
	w->cencodes = (weave_uint8 *) xalloc((size_t) w->g.nblocks *
										 (size_t) w->q.codebytes);
	memset(w->blocks, 0, (size_t) w->g.nblocks * (size_t) w->g.blockbytes);

	recon = (float *) xalloc((size_t) WEAVE_VEC_BLOCK * (size_t) dim *
							 sizeof(float));
	lane_scale = (float *) xalloc((size_t) WEAVE_VEC_BLOCK * sizeof(float));
	lane_norm = (float *) xalloc((size_t) WEAVE_VEC_BLOCK * sizeof(float));
	slot = (int *) xalloc((size_t) WEAVE_VEC_BLOCK * sizeof(int));
	code = (weave_uint8 *) xalloc((size_t) w->q.codebytes);

	for (b = 0; b < w->g.nblocks; b++)
	{
		int			nlanes = weave_vecweft_block_lanes(&w->g, b);
		int			nlive = 0;
		int			s;
		int			j;

		for (s = 0; s < nlanes; s++)
		{
			float		norm,
						scale;

			if (deadpct > 0 && (int) (rng() % 100) < deadpct &&
				!(s == nlanes - 1 && nlive == 0))
				continue;		/* a dead lane, and never the whole block */

			for (j = 0; j < dim; j++)
				v[j] = rnd_unit();
			if (weave_encode(&w->q, v, code, &norm, &scale) != 0)
			{
				s--;			/* an all-zero draw is refused by design; retry */
				continue;
			}
			weave_pack_lane(WEAVE_PACK_LANE, dim, bits, weft_codes(w, b), s,
							code);
			weave_decode(&w->q, code, scale, recon + (size_t) nlive * dim);
			lane_scale[nlive] = scale;
			lane_norm[nlive] = norm;
			slot[nlive] = s;
			nlive++;
		}

		if (weave_vecblock_stats(weft_rec(w, b), &w->q, recon, lane_scale,
								 lane_norm, slot, nlive,
								 b * (weave_uint32) WEAVE_VEC_BLOCK, cen,
								 weft_cencode(w, b)) != 0)
		{
			printf("weave_vecblock_stats refused a well-formed block\n");
			exit(2);
		}
	}

	free(code);
	free(slot);
	free(lane_norm);
	free(lane_scale);
	free(recon);
	free(cen);
	free(v);
	return 0;
}

static void
weft_free(Weft *w)
{
	free(w->allow);
	free(w->cencodes);
	free(w->blocks);
	free(w->recs);
	weave_quantizer_free(&w->q, free);
	memset(w, 0, sizeof(*w));
}

/*
 * An allowlist of EXACTLY ceil(nvec / 64) words -- not one byte more, so that a
 * read past the last word is an ASan report.
 *
 * `blockpct` of the blocks have every one of their lanes excluded, which is the
 * only way to reach the mask short-circuit at block granularity; within the other
 * blocks each lane is allowed with probability `lanepct`, so partially masked
 * blocks are covered too.
 */
static void
weft_make_allow(Weft *w, int blockpct, int lanepct)
{
	weave_uint32 b;

	w->nwords = ((size_t) w->g.nvec + 63) / 64;
	free(w->allow);
	w->allow = (weave_uint64 *) xalloc(w->nwords * sizeof(weave_uint64));
	memset(w->allow, 0, w->nwords * sizeof(weave_uint64));

	for (b = 0; b < w->g.nblocks; b++)
	{
		int			nlanes = weave_vecweft_block_lanes(&w->g, b);
		int			s;

		if ((int) (rng() % 100) < blockpct)
			continue;			/* every lane of this block excluded */
		for (s = 0; s < nlanes; s++)
		{
			weave_uint32 warp = b * (weave_uint32) WEAVE_VEC_BLOCK +
				(weave_uint32) s;

			if ((int) (rng() % 100) < lanepct)
				w->allow[warp >> 6] |= (weave_uint64) 1 << (warp & 63);
		}
	}
}

/* The mask this test expects, computed independently of the core's copy. */
static weave_uint32
expect_avail(const Weft *w, weave_uint32 b, const weave_uint64 *allow)
{
	int			nlanes = weave_vecweft_block_lanes(&w->g, b);
	weave_uint32 m = w->recs[b].livemask;
	int			s;

	if (nlanes < WEAVE_VEC_BLOCK)
		m &= (weave_uint32) ((1u << nlanes) - 1);
	if (allow == NULL)
		return m;
	for (s = 0; s < nlanes; s++)
	{
		weave_uint32 warp = b * (weave_uint32) WEAVE_VEC_BLOCK +
			(weave_uint32) s;

		if ((allow[warp >> 6] & ((weave_uint64) 1 << (warp & 63))) == 0)
			m &= ~(1u << s);
	}
	return m;
}

static int
popcount32(weave_uint32 m)
{
	int			n = 0;

	while (m != 0)
	{
		m &= m - 1;
		n++;
	}
	return n;
}

/* Score a block through the shipping scalar oracle, exactly as a shuttle would:
 * the WeaveScoreBlock comes from weave_vec_scan_scoreblk() and nothing else. */
static int
score_block(const WeaveVecScanState *st, Weft *w, weave_uint32 b,
			const WeaveQueryLut *lut, float *out)
{
	const WeaveScoreKernel *k = weave_score_kernel_lookup("scalar");
	WeaveScoreBlock blk;

	if (k == NULL)
		return -1;
	weave_vec_scan_scoreblk(&blk, st, weft_rec(w, b), lut, weft_codes(w, b),
							weave_vecweft_block_lanes(&w->g, b));
	return k->score_block(&blk, out);
}

/* ---------------------------------------------------------------------------
 * P1, P2, P5: one walk over the whole weft
 * ------------------------------------------------------------------------- */

typedef struct WalkRes
{
	float		maxbound;		/* the largest bound any block reported */
	int			nbounded;		/* blocks that reported one */
	weave_uint64 nblk_mask;
	weave_uint64 nblk_bound;
	weave_uint64 nblk_score;
	weave_uint64 nlane_score;
} WalkRes;

static void
walk(Weft *w, int metric, const weave_uint64 *allow, const WeaveQueryLut *lut,
	 float threshold, WalkRes *res)
{
	WeaveVecScanState st;
	weave_uint32 b;
	weave_uint64 want_lanes = 0;
	int			started = 0;
	weave_uint32 prevwarp = 0;
	const char *mname = metric == WEAVE_METRIC_IP ? "ip" : "l2";
	double		tolf = metric == WEAVE_METRIC_IP ? 1e-5 : 1e-4;

	memset(res, 0, sizeof(*res));
	res->maxbound = -INFINITY;

	CHECK(weave_vec_scan_begin(&st, &w->g, metric, allow,
							   allow != NULL ? w->g.nvec : 0) == 0,
		  "dim=%d bits=%d metric=%s: begin refused a scannable weft (%s)",
		  w->g.dim, w->g.bits, mname, st.why ? st.why : "no reason");

	for (b = 0; b < w->g.nblocks; b++)
	{
		WeaveVecDirRec *rec = weft_rec(w, b);
		int			nlanes = weave_vecweft_block_lanes(&w->g, b);
		weave_uint32 avail = expect_avail(w, b, allow);
		float		bound = BOUND_SENTINEL;
		WeaveVecScanAct act;

		act = weave_vec_scan_block(&st, b, rec, lut, weft_cencode(w, b),
								   threshold, &bound);
		CHECK(act != WEAVE_VSCAN_REFUSE,
			  "dim=%d bits=%d metric=%s block=%u: REFUSED a well-formed block (%s)",
			  w->g.dim, w->g.bits, mname, b, st.why ? st.why : "no reason");
		if (act == WEAVE_VSCAN_REFUSE)
			break;

		/* P1: the (C1) witness advanced, and strictly. */
		CHECK(st.lastwarp == rec->firstwarp,
			  "block=%u: the (C1) witness is %u, not this block's firstwarp %u",
			  b, st.lastwarp, rec->firstwarp);
		if (started)
			CHECK(rec->firstwarp > prevwarp,
				  "block=%u: firstwarp %u did not ascend past %u", b,
				  rec->firstwarp, prevwarp);
		prevwarp = rec->firstwarp;
		started = 1;

		if (act == WEAVE_VSCAN_SKIP_MASK)
		{
			CHECK(avail == 0,
				  "block=%u: SKIP_MASK for a block with %d live-and-allowed lanes",
				  b, popcount32(avail));

			/*
			 * P3, the cheap half: no bound was computed, so no LUT pass over the
			 * centroid code happened either.  The expensive half -- proving the
			 * bytes themselves were never touched -- is prop_poison().
			 */
			CHECK(bound == BOUND_SENTINEL,
				  "block=%u: SKIP_MASK wrote a bound (%.9g)", b, (double) bound);
			continue;
		}

		CHECK(avail != 0, "block=%u: act=%d for a fully masked block", b,
			  (int) act);
		CHECK(bound != BOUND_SENTINEL && bound == bound && bound < 3.4e38f,
			  "block=%u: bound %.9g is not a usable number", b, (double) bound);

		/*
		 * The bound is in the metric's domain, and this recomputes it from the
		 * record to pin WHICH domain.  The IP and L2 forms differ by
		 * -||q||^2 - minnorm^2 plus a factor of two, so an implementation that
		 * returned the wrong one would have to be caught here or in the
		 * inequality below -- and the inequality alone would not catch it, since
		 * the L2 bound is far LARGER than an IP score at these magnitudes.
		 */
		{
			float		censcore = weave_lut_score_code(lut, w->g.bits,
														weft_cencode(w, b),
														rec->censcale);
			float		bip = weave_block_bound_ip(lut, rec->smax,
												   rec->maxrecnorm, censcore,
												   rec->cenrad);
			float		bl2 = weave_block_bound_l2(lut, rec->smax,
												   rec->maxrecnorm, censcore,
												   rec->cenrad, rec->minnorm);

			if (metric == WEAVE_METRIC_IP)
				CHECK(bound == bip, "block=%u: IP bound %.9g != %.9g", b,
					  (double) bound, (double) bip);
			else
			{
				CHECK(bound == bl2, "block=%u: L2 bound %.9g != %.9g", b,
					  (double) bound, (double) bl2);
				if (bl2 != bip)
					l2_bound_witness++;
			}
		}

		if (act == WEAVE_VSCAN_SKIP_BOUND)
			CHECK(bound <= threshold,
				  "block=%u: SKIP_BOUND with bound %.9g above threshold %.9g",
				  b, (double) bound, (double) threshold);
		else
			CHECK(!(bound <= threshold),
				  "block=%u: SCORE with bound %.9g at or below threshold %.9g",
				  b, (double) bound, (double) threshold);

		if (act == WEAVE_VSCAN_SCORE)
			want_lanes += (weave_uint64) popcount32(avail);

		/*
		 * P2, THE ASSERTION THIS FILE EXISTS FOR.  Every block gets scored here,
		 * including the ones the bound skipped: (C2) is a statement about the
		 * bound, not about the blocks that happened to survive it, and a bound
		 * that is unsound exactly where it prunes is the worst case.
		 */
		{
			float		out[WEAVE_VEC_BLOCK];
			weave_uint8 lanecode[WEAVE_CODE_MAX_BYTES];
			int			n;
			int			s;

			memset(out, 0, sizeof(out));
			n = score_block(&st, w, b, lut, out);
			CHECK(n == nlanes, "block=%u: kernel returned %d for %d lanes", b, n,
				  nlanes);
			for (s = 0; s < n && s < WEAVE_VEC_BLOCK; s++)
			{
				float		sc = weave_vec_scan_lane_score(&st, lut, out[s],
														   rec->lane[2 * s + 1]);

				if ((avail & (1u << s)) == 0)
				{
					/* P6: a dead or masked lane is a sentinel in and out. */
					CHECK(out[s] == WEAVE_KERNEL_NEVER,
						  "block=%u lane=%d: kernel scored a lane it was told to"
						  " skip", b, s);
					CHECK(sc == WEAVE_KERNEL_NEVER,
						  "block=%u lane=%d: the sentinel became %.9g", b, s,
						  (double) sc);
					continue;
				}

				CHECK(sc == sc && sc > WEAVE_KERNEL_NEVER,
					  "block=%u lane=%d: a live lane scored %.9g", b, s,
					  (double) sc);

				/*
				 * The kernel was pointed at the lane's scale by
				 * weave_vec_scan_scoreblk(), whose whole reason for existing is
				 * that the record stores (scale, norm) INTERLEAVED.  Recompute
				 * the lane's inner product from rec->lane[2*s] explicitly:
				 * a scalestride of 1 does not fail, it feeds every second lane a
				 * NORM as its scale and returns wrong distances
				 * (src/vector/pack.c) -- and the block bound still dominates the
				 * result, so (C2) alone cannot see it.  Verified: mutating the
				 * stride to 1 leaves every other assertion in this file passing.
				 */
				weave_unpack_lane(WEAVE_PACK_LANE, w->g.dim, w->g.bits,
								  weft_codes(w, b), s, lanecode);
				CHECK(out[s] == weave_lut_score_code(lut, w->g.bits, lanecode,
													rec->lane[2 * s]),
					  "block=%u lane=%d: the kernel scored %.9g where this lane's"
					  " own (scale, norm) pair gives %.9g -- a stride error", b, s,
					  (double) out[s],
					  (double) weave_lut_score_code(lut, w->g.bits, lanecode,
													rec->lane[2 * s]));

				if (metric == WEAVE_METRIC_IP)
				{
					CHECK(sc == out[s],
						  "block=%u lane=%d: IP conversion is not the identity"
						  " (%.9g vs %.9g)", b, s, (double) sc,
						  (double) out[s]);
					ip_identity_witness++;
				}
				else
				{
					float		want = -lut->qnorm2 + 2.0f * out[s] -
						rec->lane[2 * s + 1] * rec->lane[2 * s + 1];

					CHECK(sc == want,
						  "block=%u lane=%d: L2 conversion %.9g != %.9g", b, s,
						  (double) sc, (double) want);
					if (sc != out[s])
						l2_score_witness++;
				}

				CHECK((double) bound >= (double) sc -
					  tolf * (fabs((double) sc) + 1.0),
					  "dim=%d bits=%d metric=%s block=%u lane=%d: BOUND"
					  " VIOLATED, bound %.9g < score %.9g", w->g.dim, w->g.bits,
					  mname, b, s, (double) bound, (double) sc);
			}
		}

		if (bound > res->maxbound)
			res->maxbound = bound;
		res->nbounded++;
	}

	/* The counters partition the blocks that were accepted.  Stated as an
	 * invariant because the header does not say whether a refusal counts as seen,
	 * and this core's answer is that it does not: nblk_seen is what the three
	 * outcome counters add up to. */
	CHECK(st.nblk_seen == st.nblk_mask + st.nblk_bound + st.nblk_score,
		  "counters do not partition: %llu seen vs %llu + %llu + %llu",
		  (unsigned long long) st.nblk_seen,
		  (unsigned long long) st.nblk_mask,
		  (unsigned long long) st.nblk_bound,
		  (unsigned long long) st.nblk_score);
	CHECK(st.nblk_seen == (weave_uint64) w->g.nblocks,
		  "walked %u blocks but the core counted %llu", w->g.nblocks,
		  (unsigned long long) st.nblk_seen);
	CHECK(st.nlane_score == want_lanes,
		  "nlane_score %llu != the %llu live-and-allowed lanes of the scored"
		  " blocks", (unsigned long long) st.nlane_score,
		  (unsigned long long) want_lanes);

	/*
	 * P5: WeaveShuttle.maxscore must dominate block_max() everywhere.  Folded
	 * over every record exactly as begin()'s directory pass does, then converted
	 * once.  No tolerance: every step of both expressions is monotone in float, so
	 * the inequality is exact, and a tolerance here would hide the one thing this
	 * check is for.
	 */
	{
		float		acc = 0.0f;
		float		ms;
		weave_uint32 i;

		for (i = 0; i < w->g.nblocks; i++)
			weave_vec_scan_maxscore_fold(&acc, weft_rec(w, i));
		ms = weave_vec_scan_maxscore(&st, lut, acc);
		CHECK(ms == ms && ms < 3.4e38f, "metric=%s: maxscore %.9g is not finite",
			  mname, (double) ms);
		if (res->nbounded > 0)
			CHECK(ms >= res->maxbound,
				  "dim=%d bits=%d metric=%s: maxscore %.9g < the largest block"
				  " bound %.9g", w->g.dim, w->g.bits, mname, (double) ms,
				  (double) res->maxbound);
	}

	res->nblk_mask = st.nblk_mask;
	res->nblk_bound = st.nblk_bound;
	res->nblk_score = st.nblk_score;
	res->nlane_score = st.nlane_score;
}

/* ---------------------------------------------------------------------------
 * P3: the mask short-circuit, PROVEN rather than asserted
 * ------------------------------------------------------------------------- */

/*
 * The codes and the centroid code of block 0 are overwritten with POISON before
 * the fully-masked decision is asked for, and the test first establishes that
 * reading either of them would produce a DIFFERENT number -- a different lane
 * score from the codes, a different bound from the centroid.  Only then is the
 * all-zero allowlist applied.  So "no code byte was read" is a consequence of
 * observable behaviour and not of reading the implementation.
 *
 * The all-zero bitmap is also the case that must not be confused with a NULL one:
 * NULL means everything is allowed, all-zero means nothing is, and conflating them
 * turns an empty candidate set into a full scan (include/weave/vecscan.h).
 */
static void
prop_poison(int dim, int bits, weave_uint32 nvec)
{
	Weft		w;
	WeaveQueryLut lut;
	WeaveVecScanState st;
	float	   *qv;
	float		out_real[WEAVE_VEC_BLOCK];
	float		out_poison[WEAVE_VEC_BLOCK];
	float		bound_real = BOUND_SENTINEL;
	float		bound_poison = BOUND_SENTINEL;
	float		bound_masked = BOUND_SENTINEL;
	float		cen_real;
	float		cen_poison;
	weave_uint8 *pristine;
	int			differs = 0;
	int			j;
	int			s;

	if (weft_build(&w, dim, bits, nvec, 0) != 0)
		return;

	qv = (float *) xalloc((size_t) dim * sizeof(float));
	for (j = 0; j < dim; j++)
		qv[j] = rnd_unit();
	if (weave_query_lut_build(&lut, &w.q, qv, malloc) != 0)
	{
		free(qv);
		weft_free(&w);
		return;
	}

	pristine = (weave_uint8 *) xalloc((size_t) w.g.blockbytes);
	memcpy(pristine, weft_codes(&w, 0), (size_t) w.g.blockbytes);
	cen_real = weave_lut_score_code(&lut, bits, weft_cencode(&w, 0),
									weft_rec(&w, 0)->censcale);

	/* The real thing: no allowlist, no threshold. */
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_L2, NULL, 0) == 0,
		  "poison: begin refused");
	CHECK(weave_vec_scan_block(&st, 0, weft_rec(&w, 0), &lut,
							   weft_cencode(&w, 0), -INFINITY,
							   &bound_real) == WEAVE_VSCAN_SCORE,
		  "poison: block 0 did not score before poisoning");
	CHECK(score_block(&st, &w, 0, &lut, out_real) ==
		  weave_vecweft_block_lanes(&w.g, 0), "poison: kernel refused block 0");

	/* Poison both the codes and the centroid code, in place. */
	memset(weft_codes(&w, 0), POISON, (size_t) w.g.blockbytes);
	memset(weft_cencode(&w, 0), POISON, (size_t) w.q.codebytes);

	/* Teeth 1: reading the codes now would change at least one lane's score. */
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_L2, NULL, 0) == 0,
		  "poison: begin refused");
	CHECK(weave_vec_scan_block(&st, 0, weft_rec(&w, 0), &lut,
							   weft_cencode(&w, 0), -INFINITY,
							   &bound_poison) == WEAVE_VSCAN_SCORE,
		  "poison: block 0 did not score after poisoning");
	CHECK(score_block(&st, &w, 0, &lut, out_poison) ==
		  weave_vecweft_block_lanes(&w.g, 0), "poison: kernel refused block 0");
	for (s = 0; s < weave_vecweft_block_lanes(&w.g, 0); s++)
		if ((weft_rec(&w, 0)->livemask & (1u << s)) != 0 &&
			out_poison[s] != out_real[s])
			differs = 1;
	CHECK(differs,
		  "poison: the pattern scores identically to the real codes, so this test"
		  " has no teeth");

	/*
	 * Teeth 2: a LUT pass over the poisoned centroid returns a different number,
	 * so a bound computed from those bytes would be computed from observably
	 * different input.
	 *
	 * THIS IS NOT STATED AS "THE BOUND DIFFERS", which is what this test asserted
	 * first and which is FALSE.  weave_block_bound_ip() takes the min of (B3),
	 * (B2) and (B1), and on blocks of uniform random vectors the radius is large
	 * enough that (B2) -- maxrecnorm * ||q||, which does not involve the centroid
	 * at all -- is the minimum, so poisoning the centroid code changes the bound
	 * by nothing.  That is the same structural fact
	 * bench/RESULTS_BOUND_PRUNING.md measured as (B3) pruning 0.0 % under a
	 * random warp order, arriving here as a test that had to be weakened to stay
	 * true.  The observables this property therefore rests on are censcore and
	 * the UNTOUCHED *bound_out below, which together say the centroid pass did
	 * not happen at all.
	 */
	cen_poison = weave_lut_score_code(&lut, bits, weft_cencode(&w, 0),
									  weft_rec(&w, 0)->censcale);
	CHECK(cen_poison != cen_real,
		  "poison: the pattern scores identically to the real centroid code"
		  " (%.9g), so a skipped LUT pass would be invisible", (double) cen_real);
	(void) bound_poison;

	/* The property.  An allowlist of exactly ceil(nvec/64) words, all zero. */
	w.nwords = ((size_t) nvec + 63) / 64;
	free(w.allow);
	w.allow = (weave_uint64 *) xalloc(w.nwords * sizeof(weave_uint64));
	memset(w.allow, 0, w.nwords * sizeof(weave_uint64));

	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_L2, w.allow, nvec) == 0,
		  "poison: begin refused an all-zero allowlist -- which is a legal empty"
		  " candidate set, not an absent one");
	CHECK(weave_vec_scan_block(&st, 0, weft_rec(&w, 0), &lut,
							   weft_cencode(&w, 0), -INFINITY,
							   &bound_masked) == WEAVE_VSCAN_SKIP_MASK,
		  "poison: a fully masked block was not SKIP_MASK");
	CHECK(st.nblk_mask == 1 && st.nblk_seen == 1,
		  "poison: nblk_mask %llu, nblk_seen %llu",
		  (unsigned long long) st.nblk_mask,
		  (unsigned long long) st.nblk_seen);
	CHECK(st.nblk_score == 0 && st.nlane_score == 0,
		  "poison: the scoring counters moved for a skipped block (%llu blocks,"
		  " %llu lanes)", (unsigned long long) st.nblk_score,
		  (unsigned long long) st.nlane_score);
	CHECK(bound_masked == BOUND_SENTINEL,
		  "poison: SKIP_MASK wrote a bound (%.9g), so it read the poisoned"
		  " centroid code", (double) bound_masked);

	/* And the whole weft under the same all-zero bitmap: every block skipped, not
	 * one lane scored.  An all-zero allowlist is not a full scan. */
	{
		weave_uint32 b;

		CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, w.allow,
								   nvec) == 0, "poison: begin refused");
		for (b = 0; b < w.g.nblocks; b++)
		{
			float		bo = BOUND_SENTINEL;

			CHECK(weave_vec_scan_block(&st, b, weft_rec(&w, b), &lut,
									   weft_cencode(&w, b), -INFINITY,
									   &bo) == WEAVE_VSCAN_SKIP_MASK,
				  "poison: block %u was not skipped under an all-zero allowlist",
				  b);
			CHECK(bo == BOUND_SENTINEL, "poison: block %u wrote a bound", b);
		}
		CHECK(st.nblk_mask == (weave_uint64) w.g.nblocks && st.nlane_score == 0,
			  "poison: %llu of %u blocks skipped, %llu lanes scored",
			  (unsigned long long) st.nblk_mask, w.g.nblocks,
			  (unsigned long long) st.nlane_score);
	}

	/* Restore, so a later property does not inherit the poison. */
	memcpy(weft_codes(&w, 0), pristine, (size_t) w.g.blockbytes);
	free(pristine);
	free(qv);
	free(lut._alloc);
	weft_free(&w);
}

/* ---------------------------------------------------------------------------
 * P4: masking does not perturb the lanes that survive it
 * ------------------------------------------------------------------------- */

/*
 * One surviving lane, and its score must be BIT-IDENTICAL to its score with no
 * allowlist at all.  Not "close": the mask decides which lanes are computed and
 * must not change how one is, and the scalar oracle sums a lane's coordinates in
 * a fixed order independently of its neighbours, so equality is the honest
 * assertion.  A kernel that folded the mask into the arithmetic -- multiplying by
 * a 0/1 weight, say -- would fail this and should.
 */
static void
prop_survivor(int dim, int bits, weave_uint32 nvec, int metric)
{
	Weft		w;
	WeaveQueryLut lut;
	WeaveVecScanState st;
	float	   *qv;
	float		out_open[WEAVE_VEC_BLOCK];
	float		out_one[WEAVE_VEC_BLOCK];
	float		bound_open = BOUND_SENTINEL;
	float		bound_one = BOUND_SENTINEL;
	weave_uint32 b;
	weave_uint32 warp;
	int			nlanes;
	int			lane = -1;
	int			j;
	int			s;

	if (weft_build(&w, dim, bits, nvec, 30) != 0)
		return;

	qv = (float *) xalloc((size_t) dim * sizeof(float));
	for (j = 0; j < dim; j++)
		qv[j] = rnd_unit();
	if (weave_query_lut_build(&lut, &w.q, qv, malloc) != 0)
	{
		free(qv);
		weft_free(&w);
		return;
	}

	b = w.g.nblocks / 2;
	nlanes = weave_vecweft_block_lanes(&w.g, b);
	for (s = 0; s < nlanes; s++)
		if ((weft_rec(&w, b)->livemask & (1u << s)) != 0)
		{
			lane = s;
			break;
		}
	if (lane < 0)
	{
		free(qv);
		free(lut._alloc);
		weft_free(&w);
		return;
	}

	/* No allowlist at all. */
	CHECK(weave_vec_scan_begin(&st, &w.g, metric, NULL, 0) == 0,
		  "survivor: begin refused");
	CHECK(weave_vec_scan_block(&st, b, weft_rec(&w, b), &lut, weft_cencode(&w, b),
							   -INFINITY, &bound_open) == WEAVE_VSCAN_SCORE,
		  "survivor: block %u did not score unfiltered", b);
	CHECK(score_block(&st, &w, b, &lut, out_open) == nlanes,
		  "survivor: kernel refused block %u", b);

	/* Exactly one lane allowed, in a bitmap of exactly ceil(nvec/64) words. */
	w.nwords = ((size_t) nvec + 63) / 64;
	free(w.allow);
	w.allow = (weave_uint64 *) xalloc(w.nwords * sizeof(weave_uint64));
	memset(w.allow, 0, w.nwords * sizeof(weave_uint64));
	warp = b * (weave_uint32) WEAVE_VEC_BLOCK + (weave_uint32) lane;
	w.allow[warp >> 6] |= (weave_uint64) 1 << (warp & 63);

	CHECK(weave_vec_scan_begin(&st, &w.g, metric, w.allow, nvec) == 0,
		  "survivor: begin refused");
	CHECK(weave_vec_scan_block(&st, b, weft_rec(&w, b), &lut, weft_cencode(&w, b),
							   -INFINITY, &bound_one) == WEAVE_VSCAN_SCORE,
		  "survivor: a block with one allowed lane was not SCORE");
	CHECK(st.nlane_score == 1, "survivor: nlane_score is %llu, not 1",
		  (unsigned long long) st.nlane_score);
	CHECK(bound_one == bound_open,
		  "survivor: the bound changed with the allowlist (%.9g vs %.9g)",
		  (double) bound_one, (double) bound_open);
	CHECK(score_block(&st, &w, b, &lut, out_one) == nlanes,
		  "survivor: kernel refused the masked block");

	for (s = 0; s < nlanes; s++)
	{
		if (s == lane)
		{
			CHECK(out_one[s] == out_open[s],
				  "survivor: lane %d scored %.9g masked and %.9g unmasked", s,
				  (double) out_one[s], (double) out_open[s]);
			CHECK(weave_vec_scan_lane_score(&st, &lut, out_one[s],
											weft_rec(&w, b)->lane[2 * s + 1]) ==
				  weave_vec_scan_lane_score(&st, &lut, out_open[s],
											weft_rec(&w, b)->lane[2 * s + 1]),
				  "survivor: lane %d's converted score depends on the allowlist",
				  s);
		}
		else
			CHECK(out_one[s] == WEAVE_KERNEL_NEVER,
				  "survivor: lane %d was scored despite being masked out", s);
	}

	free(qv);
	free(lut._alloc);
	weft_free(&w);
}

/* ---------------------------------------------------------------------------
 * P7: every refusal, and none of them a clamp
 * ------------------------------------------------------------------------- */

static void
prop_refusals(void)
{
	Weft		w;
	WeaveQueryLut lut;
	WeaveVecScanState st;
	WeaveVecWeftGeom bad;
	WeaveVecDirRec rec;
	float	   *qv;
	float		bound;
	int			j;

	/* 70 lanes: three blocks, the last of them a six-lane tail. */
	if (weft_build(&w, 32, 4, 70, 20) != 0)
	{
		printf("refusals: the codec declined dim=32 bits=4\n");
		exit(2);
	}
	CHECK(w.g.nblocks == 3 && weave_vecweft_block_lanes(&w.g, 2) == 6,
		  "refusals: the tail-block shape is not what this test assumes"
		  " (%u blocks, tail %d)", w.g.nblocks,
		  weave_vecweft_block_lanes(&w.g, 2));

	qv = (float *) xalloc(32 * sizeof(float));
	for (j = 0; j < 32; j++)
		qv[j] = rnd_unit();
	if (weave_query_lut_build(&lut, &w.q, qv, malloc) != 0)
	{
		printf("refusals: the query LUT would not build\n");
		exit(2);
	}

	/* --- begin() --- */
	CHECK(weave_vec_scan_begin(&st, NULL, WEAVE_METRIC_IP, NULL, 0) == -1 &&
		  st.why != NULL, "refusals: begin accepted a NULL geometry");
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_COSINE, NULL, 0) == -1 &&
		  st.why != NULL,
		  "refusals: begin accepted cosine, whose bound needs a maximum true norm"
		  " that is not stored");
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_L1, NULL, 0) == -1,
		  "refusals: begin accepted L1");
	CHECK(weave_vec_scan_begin(&st, &w.g, 0, NULL, 0) == -1,
		  "refusals: begin accepted 0, which is not a WeaveMetric at all");
	CHECK(weave_vec_scan_begin(&st, &w.g, 99, NULL, 0) == -1,
		  "refusals: begin accepted an out-of-range metric");

	bad = w.g;
	bad.layout = WEAVE_PACK_VECMAJOR;
	CHECK(weave_vec_scan_begin(&st, &bad, WEAVE_METRIC_IP, NULL, 0) == -1 &&
		  st.why != NULL, "refusals: begin accepted a VECMAJOR weft");
	bad = w.g;
	bad.nblocks = 0;
	CHECK(weave_vec_scan_begin(&st, &bad, WEAVE_METRIC_IP, NULL, 0) == -1,
		  "refusals: begin accepted a weft with no blocks");

	w.nwords = ((size_t) w.g.nvec + 63) / 64;
	free(w.allow);
	w.allow = (weave_uint64 *) xalloc(w.nwords * sizeof(weave_uint64));
	memset(w.allow, 0xFF, w.nwords * sizeof(weave_uint64));
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, w.allow,
							   w.g.nvec - 1) == -1 && st.why != NULL,
		  "refusals: begin accepted an allowlist shorter than the weft, which"
		  " would score real lanes against absent bits");
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, w.allow,
							   w.g.nvec) == 0,
		  "refusals: begin rejected an allowlist of exactly the weft's length");

	/* --- block(): the geometry --- */
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, NULL, 0) == 0,
		  "refusals: begin refused");
	CHECK(weave_vec_scan_block(&st, w.g.nblocks, weft_rec(&w, 0), &lut,
							   weft_cencode(&w, 0), -INFINITY,
							   &bound) == WEAVE_VSCAN_REFUSE && st.why != NULL,
		  "refusals: a blockno past nblocks was accepted");
	CHECK(st.nblk_seen == 0,
		  "refusals: a refused block was counted as seen (%llu)",
		  (unsigned long long) st.nblk_seen);
	CHECK(weave_vec_scan_block(&st, 0, weft_rec(&w, 0), &lut,
							   weft_cencode(&w, 0), -INFINITY,
							   NULL) == WEAVE_VSCAN_REFUSE,
		  "refusals: a NULL bound_out was accepted");
	CHECK(weave_vec_scan_block(&st, 0, weft_rec(&w, 0), &lut, NULL, -INFINITY,
							   &bound) == WEAVE_VSCAN_REFUSE,
		  "refusals: a NULL centroid code was accepted");

	/* --- block(): (C1), both halves --- */
	rec = *weft_rec(&w, 1);
	rec.firstwarp = 33;			/* not blockno * 32 */
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, NULL, 0) == 0,
		  "refusals: begin refused");
	CHECK(weave_vec_scan_block(&st, 1, &rec, &lut, weft_cencode(&w, 1),
							   -INFINITY, &bound) == WEAVE_VSCAN_REFUSE &&
		  st.why != NULL,
		  "refusals: a firstwarp that is not blockno * 32 was accepted -- the"
		  " value indexes an allowlist and comes off a page");

	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, NULL, 0) == 0,
		  "refusals: begin refused");
	CHECK(weave_vec_scan_block(&st, 1, weft_rec(&w, 1), &lut,
							   weft_cencode(&w, 1), -INFINITY,
							   &bound) != WEAVE_VSCAN_REFUSE,
		  "refusals: block 1 was refused as the first block of a walk");
	CHECK(weave_vec_scan_block(&st, 0, weft_rec(&w, 0), &lut,
							   weft_cencode(&w, 0), -INFINITY,
							   &bound) == WEAVE_VSCAN_REFUSE && st.why != NULL,
		  "refusals: a descending firstwarp was accepted, which is (C1)");
	CHECK(weave_vec_scan_block(&st, 1, weft_rec(&w, 1), &lut,
							   weft_cencode(&w, 1), -INFINITY,
							   &bound) == WEAVE_VSCAN_REFUSE,
		  "refusals: the same block twice was accepted; ascent is STRICT");

	/* --- block(): the floats --- */
	{
		int			which;

		for (which = 0; which < 4; which++)
		{
			rec = *weft_rec(&w, 0);
			switch (which)
			{
				case 0:
					rec.smax = -1.0f;
					break;
				case 1:
					rec.minnorm = -1.0f;
					break;
				case 2:
					rec.cenrad = -1.0f;
					break;
				default:
					rec.lane[0] = -1.0f;
					break;
			}
			CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_L2, NULL, 0) == 0,
				  "refusals: begin refused");
			CHECK(weave_vec_scan_block(&st, 0, &rec, &lut, weft_cencode(&w, 0),
									   -INFINITY,
									   &bound) == WEAVE_VSCAN_REFUSE &&
				  st.why != NULL,
				  "refusals: a NEGATIVE bound field (case %d) was accepted -- it"
				  " makes the bound smaller than the true maximum, which drops"
				  " rows", which);
		}
	}

	rec = *weft_rec(&w, 0);
	rec.cenrad = (float) NAN;
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, NULL, 0) == 0,
		  "refusals: begin refused");
	CHECK(weave_vec_scan_block(&st, 0, &rec, &lut, weft_cencode(&w, 0),
							   -INFINITY, &bound) == WEAVE_VSCAN_REFUSE,
		  "refusals: a NaN radius was accepted; a NaN is not an upper bound");

	rec = *weft_rec(&w, 0);
	rec.lane[3] = (float) NAN;
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_L2, NULL, 0) == 0,
		  "refusals: begin refused");
	CHECK(weave_vec_scan_block(&st, 0, &rec, &lut, weft_cencode(&w, 0),
							   -INFINITY, &bound) == WEAVE_VSCAN_REFUSE,
		  "refusals: a NaN lane norm was accepted; the L2 conversion would return"
		  " a NaN score against a finite bound");

	/*
	 * A lane past the end of the allowlist.
	 *
	 * Reaching it needs the state poked, and that is worth explaining rather than
	 * hiding: begin() already refuses an allowlist shorter than the weft, and
	 * block() already requires firstwarp == blockno * 32, so the two together
	 * imply firstwarp + nlanes <= nvec <= nwarp and the range check cannot fire
	 * from any input a caller can supply TODAY.  It becomes reachable the moment
	 * task V13 assigns warps in cluster order and the formula half is lifted --
	 * which is exactly when an unchecked firstwarp becomes an out-of-bounds read
	 * of the bitmap.  Lowering st.nwarp here tests the check that will be
	 * load-bearing then, instead of deleting it now and rediscovering it as a
	 * crash.
	 */
	CHECK(weave_vec_scan_begin(&st, &w.g, WEAVE_METRIC_IP, w.allow,
							   w.g.nvec) == 0, "refusals: begin refused");
	st.nwarp = 33;
	CHECK(weave_vec_scan_block(&st, 1, weft_rec(&w, 1), &lut,
							   weft_cencode(&w, 1), -INFINITY,
							   &bound) == WEAVE_VSCAN_REFUSE && st.why != NULL,
		  "refusals: a block whose lanes run past the allowlist was accepted"
		  " rather than rejected -- clamping would score lanes against bits"
		  " belonging to other warps");

	/* --- P6: the sentinel, in both domains --- */
	{
		int			mi;

		for (mi = 0; mi < 2; mi++)
		{
			int			metric = mi == 0 ? WEAVE_METRIC_IP : WEAVE_METRIC_L2;

			CHECK(weave_vec_scan_begin(&st, &w.g, metric, NULL, 0) == 0,
				  "refusals: begin refused");
			CHECK(weave_vec_scan_lane_score(&st, &lut, WEAVE_KERNEL_NEVER,
											3.0f) == WEAVE_KERNEL_NEVER,
				  "refusals: metric %d turned the never-contributes sentinel into"
				  " a finite score", metric);
			CHECK(weave_vec_scan_lane_score(&st, &lut, WEAVE_KERNEL_NEVER,
											0.0f) == WEAVE_KERNEL_NEVER,
				  "refusals: metric %d, zero norm: the sentinel did not survive",
				  metric);
		}
	}

	free(qv);
	free(lut._alloc);
	weft_free(&w);
}

/* ---------------------------------------------------------------------------
 * The shapes
 * ------------------------------------------------------------------------- */

static long nmask_seen = 0;
static long nbound_seen = 0;

static void
one_shape(int dim, int bits, weave_uint32 nvec, int deadpct, int iters)
{
	Weft		w;
	int			it;

	if (weft_build(&w, dim, bits, nvec, deadpct) != 0)
		return;					/* the codec declines this geometry */

	for (it = 0; it < iters; it++)
	{
		WeaveQueryLut lut;
		float	   *qv = (float *) xalloc((size_t) dim * sizeof(float));
		int			mi;
		int			j;

		for (j = 0; j < dim; j++)
			qv[j] = rnd_unit();
		if (weave_query_lut_build(&lut, &w.q, qv, malloc) != 0)
		{
			free(qv);
			continue;
		}
		weft_make_allow(&w, 25, 70);

		for (mi = 0; mi < 2; mi++)
		{
			int			metric = mi == 0 ? WEAVE_METRIC_IP : WEAVE_METRIC_L2;
			WalkRes		open;
			WalkRes		filt;

			/* Unfiltered, unpruned: (C2) with nothing else going on. */
			walk(&w, metric, NULL, &lut, -INFINITY, &open);
			CHECK(open.nblk_mask == 0,
				  "dim=%d nvec=%u: a block was mask-skipped with NO allowlist,"
				  " which conflates absent with empty", dim, nvec);
			CHECK(open.nblk_bound == 0,
				  "dim=%d nvec=%u: a -INFINITY threshold pruned %llu blocks",
				  dim, nvec, (unsigned long long) open.nblk_bound);

			/* Filtered: (C2) again, and the mask must only ever remove work. */
			walk(&w, metric, w.allow, &lut, -INFINITY, &filt);
			CHECK(filt.nlane_score <= open.nlane_score,
				  "dim=%d nvec=%u: the allowlist increased the scored lane count"
				  " (%llu > %llu)", dim, nvec,
				  (unsigned long long) filt.nlane_score,
				  (unsigned long long) open.nlane_score);
			nmask_seen += (long) filt.nblk_mask;

			/*
			 * Pruned: a threshold at the largest bound the unfiltered walk saw
			 * must skip at least the block that produced it, because the
			 * comparison is `bound <= threshold`.  This is the only way SKIP_BOUND
			 * is reachable at all -- a shuttle driven by the fused loop passes
			 * -INFINITY (include/weave/vecscan.h).
			 */
			if (open.nbounded > 0)
			{
				WalkRes		pruned;

				walk(&w, metric, NULL, &lut, open.maxbound, &pruned);
				CHECK(pruned.nblk_bound >= 1,
					  "dim=%d nvec=%u metric=%d: a threshold at the largest bound"
					  " seen pruned nothing, so `bound <= threshold` is `<`", dim,
					  nvec, metric);
				nbound_seen += (long) pruned.nblk_bound;
			}
		}
		free(qv);
		free(lut._alloc);
	}
	weft_free(&w);
}

int
main(void)
{
	static const int dims[] = {32, 64, 128, 384};
	static const weave_uint32 nvecs[] = {1, 32, 33, 70, 200, 257};
	int			di;
	int			ni;
	int			bits;

	printf("== V8 code-scan core: (C1), (C2) in the metric's domain, masking ==\n");

	for (di = 0; di < (int) (sizeof(dims) / sizeof(dims[0])); di++)
		for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits += 2)
			for (ni = 0; ni < (int) (sizeof(nvecs) / sizeof(nvecs[0])); ni++)
			{
				/* deadpct 0 and 35: every lane live, and scattered interior
				 * holes of the kind vacuum leaves behind. */
				one_shape(dims[di], bits, nvecs[ni], 0, 2);
				one_shape(dims[di], bits, nvecs[ni], 35, 2);
			}

	prop_poison(64, 4, 96);
	prop_poison(31, 3, 33);
	prop_survivor(64, 4, 96, WEAVE_METRIC_IP);
	prop_survivor(64, 4, 96, WEAVE_METRIC_L2);
	prop_survivor(128, 2, 70, WEAVE_METRIC_L2);
	prop_refusals();

	/*
	 * The witnesses.  Nonzero is a requirement, not a report: if the L2 bound and
	 * the L2 conversion ever stopped differing from their IP counterparts, every
	 * (C2) assertion above would still pass and this file would have stopped
	 * testing the domain rule -- which is the bug the contract was fixed for.
	 */
	CHECK(l2_bound_witness > 0,
		  "the L2 bound never differed from the IP bound, so no L2 branch was"
		  " exercised");
	CHECK(l2_score_witness > 0,
		  "the L2 conversion never differed from the raw kernel inner product, so"
		  " no L2 conversion was exercised");
	CHECK(ip_identity_witness > 0, "no IP lane was scored at all");
	CHECK(nmask_seen > 0, "no block was ever skipped on the allowlist");
	CHECK(nbound_seen > 0, "no block was ever skipped on the bound");

	printf("L2 branch witnesses: %ld block bounds and %ld lane scores differed"
		   " from the IP domain\n", l2_bound_witness, l2_score_witness);
	printf("skips exercised: %ld on the allowlist, %ld on the bound\n",
		   nmask_seen, nbound_seen);
	printf("checks: %ld, failures: %ld\n", checks, failures);
	if (failures > 0)
	{
		printf("== FAILED ==\n");
		return 1;
	}
	printf("== ALL PROPERTIES HOLD ==\n");
	return 0;
}
