/*-------------------------------------------------------------------------
 *
 * test_vecbound.c
 *		Contract (C2) for the vector channel: block_max() >= score(), always.
 *
 * THIS IS THE TEST AGENTS.md HARD RULE 1 REQUIRES, and the reason it is separate
 * from test_vecpage.c is that it needs the whole encode/score stack -- the
 * quantizer, the packer, the query LUT and a scoring kernel -- where the strip
 * tests need none of it.
 *
 * Why a property test and not a regression test: a bound that is 1 % too low
 * silently drops rows.  The answers stay plausible, just incomplete, so no
 * fixed-expected-output test can see it.  The only mechanical defence is to
 * generate blocks and queries and assert the inequality directly.
 *
 * The two ways to get this wrong both make the bound TIGHTER, which is why they are
 * correctness bugs:
 *
 *	1. Computing the centroid and radius over the ORIGINAL vectors instead of the
 *	   reconstructions.  The bound is asserted about reconstructed scores, which is
 *	   what a scan computes.
 *	2. Measuring the radius against the FLOAT centroid instead of the dequantized
 *	   centroid code.  A reader only has the code.  include/weave/vector.h states
 *	   this above WeaveVecBlockHdr, and bench/code_scan.c did it the wrong way --
 *	   harmless there only because that bound pruned 0.00 % of blocks, so an unsound
 *	   bound never dropped anything.  M3 below plants exactly that bug.
 *
 * Properties, over a grid of dim x bits x block occupancy x random queries:
 *
 *	B1	the IP bound is >= the exact reconstructed score of every live lane
 *	B2	it is >= the score the shipping kernel computes for every live lane
 *		(the same assertion against the code path a scan actually runs)
 *	B3	the L2 bound is >= the exact reconstructed L2 similarity of every live lane
 *	B4	a partially-live block bounds only its live lanes, and stays sound when
 *		lanes are dead -- vacuum leaves blocks in that state
 *	B5	the bound is not vacuously large: it must be finite, and on average it must
 *		be within a stated factor of the true maximum.  A test that only asserts
 *		`bound >= score` passes for `bound = +inf`, which prunes nothing and is the
 *		other way to make this channel useless
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tvb test/hegel/test_vecbound.c \
 *			src/vector/vecpage.c src/vector/quantize.c src/vector/pack.c \
 *			src/vector/kernels.c -lm && /tmp/tvb
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_vecbound.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/kernels.h"
#include "weave/vecpage.h"

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

static weave_uint64 rng_state = 0xB5026F5AA96619E9ULL;

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

/*
 * Looseness accumulated over every block/query pair, as a RATIO of the bound to
 * the true block maximum, so B5 reports a scale-free number.
 *
 * This is NOT a pruning-rate claim.  These blocks are uniform random vectors,
 * which have no cluster structure at all, and bench/RESULTS_BOUND_PRUNING.md
 * measured that the bound's pruning rate depends entirely on that structure
 * (99.6 % with a coherent warp order, 0.0 % with a random one).  What the ratio is
 * for here is vacuity: `bound >= score` is also satisfied by `bound = +inf`, and a
 * property test that cannot tell those apart would pass a channel that prunes
 * nothing.
 */
static double ratio_sum = 0;
static double ratio_n = 0;
static double ratio_max = 0;

static void
one_case(int dim, int bits, int nlive)
{
	WeaveQuantizer q;
	WeaveVecDirRec rec;
	weave_uint8 *block,
			   *code,
			   *cencode;
	float	   *orig,
			   *recon,
			   *cen,
			   *qv,
			   *lane_scale,
			   *lane_norm;
	int		   *slot;
	int			i,
				j,
				iter;

	if (weave_quantizer_init(&q, dim, bits, NULL, malloc, free) != 0)
		return;					/* the codec declines this geometry; not our test */

	block = calloc(1, (size_t) weave_block_codebytes(dim, bits));
	code = malloc((size_t) q.codebytes);
	cencode = malloc((size_t) q.codebytes);
	orig = malloc((size_t) nlive * dim * sizeof(float));
	recon = malloc((size_t) nlive * dim * sizeof(float));
	cen = malloc((size_t) dim * sizeof(float));
	qv = malloc((size_t) dim * sizeof(float));
	lane_scale = malloc((size_t) nlive * sizeof(float));
	lane_norm = malloc((size_t) nlive * sizeof(float));
	slot = malloc((size_t) nlive * sizeof(int));
	if (!block || !code || !cencode || !orig || !recon || !cen || !qv ||
		!lane_scale || !lane_norm || !slot)
	{
		printf("out of memory\n");
		exit(2);
	}

	/*
	 * Scatter the live lanes rather than filling 0..nlive-1.  A block after vacuum
	 * has holes, and a bound computed as if the live lanes were contiguous would
	 * still pass a test that only ever built contiguous ones.
	 */
	{
		int			used = 0;

		for (i = 0; i < nlive; i++)
		{
			int			s;

			do
			{
				s = (int) (rng() % WEAVE_VEC_BLOCK);
			} while ((used & (1 << s)) != 0);
			used |= 1 << s;
			slot[i] = s;
		}
	}

	for (i = 0; i < nlive; i++)
	{
		float		norm,
					scale;

		for (j = 0; j < dim; j++)
			orig[(size_t) i * dim + j] = rnd_unit();
		if (weave_encode(&q, orig + (size_t) i * dim, code, &norm, &scale) != 0)
		{
			/* An all-zero draw is refused by design; retry this lane. */
			i--;
			continue;
		}
		lane_scale[i] = scale;
		lane_norm[i] = norm;
		weave_pack_lane(WEAVE_PACK_LANE, dim, bits, block, slot[i], code);
		weave_decode(&q, code, scale, recon + (size_t) i * dim);
	}

	CHECK(weave_vecblock_stats(&rec, &q, recon, lane_scale, lane_norm, slot,
							   nlive, 0, cen, cencode) == 0,
		  "dim=%d bits=%d nlive=%d: stats refused a well-formed block",
		  dim, bits, nlive);

	/*
	 * FIELD-LEVEL CHECKS, recomputed here rather than inferred from the inequality.
	 *
	 * The inequality alone cannot see some wrong definitions.  Two mutations proved
	 * it: taking maxrecnorm from lane_norm instead of from the reconstruction is
	 * numerically almost identity for this codec -- the renormalization keeps
	 * ||rec|| close to ||v|| -- and a radius loop that skips the last lane usually
	 * leaves the bound larger than the remaining maximum anyway.  Both are still
	 * WRONG DEFINITIONS: the first stops being near-identity the moment TQ+
	 * calibration changes the reconstruction, and the second is only ever right by
	 * luck.  So each field is checked against its definition, with a strict
	 * tolerance, instead of only through its consequence.
	 */
	{
		double		want_recnorm = 0,
					want_rad = 0;
		float		want_smax = 0,
					want_minnorm = lane_norm[0];
		float	   *chat = malloc((size_t) dim * sizeof(float));
		weave_uint32 want_mask = 0;

		if (!chat)
		{
			printf("out of memory\n");
			exit(2);
		}
		weave_decode(&q, cencode, rec.censcale, chat);
		for (i = 0; i < nlive; i++)
		{
			double		rn = 0,
						d = 0;

			for (j = 0; j < dim; j++)
			{
				double		r = recon[(size_t) i * dim + j];
				double		t = r - chat[j];

				rn += r * r;
				d += t * t;
			}
			if (sqrt(rn) > want_recnorm)
				want_recnorm = sqrt(rn);
			if (sqrt(d) > want_rad)
				want_rad = sqrt(d);
			if (lane_scale[i] > want_smax)
				want_smax = lane_scale[i];
			if (lane_norm[i] < want_minnorm)
				want_minnorm = lane_norm[i];
			want_mask |= 1u << slot[i];

			CHECK(rec.lane[2 * slot[i]] == lane_scale[i] &&
				  rec.lane[2 * slot[i] + 1] == lane_norm[i],
				  "dim=%d bits=%d: lane %d's (scale, norm) pair is not at its lane"
				  " index", dim, bits, slot[i]);
		}
		CHECK(rec.livemask == want_mask, "dim=%d bits=%d: livemask 0x%08X != 0x%08X",
			  dim, bits, rec.livemask, want_mask);
		CHECK(rec.smax == want_smax, "dim=%d bits=%d: smax %.9g != %.9g",
			  dim, bits, (double) rec.smax, (double) want_smax);
		CHECK(rec.minnorm == want_minnorm, "dim=%d bits=%d: minnorm %.9g != %.9g",
			  dim, bits, (double) rec.minnorm, (double) want_minnorm);
		CHECK(fabs((double) rec.maxrecnorm - want_recnorm) <=
			  1e-6 * (want_recnorm + 1.0),
			  "dim=%d bits=%d: maxrecnorm %.9g != max ||recon|| %.9g -- a field"
			  " whose DEFINITION is wrong even when the inequality survives",
			  dim, bits, (double) rec.maxrecnorm, want_recnorm);
		CHECK(fabs((double) rec.cenrad - want_rad) <= 1e-6 * (want_rad + 1.0),
			  "dim=%d bits=%d: cenrad %.9g != max ||recon - dequant(cencode)|| %.9g",
			  dim, bits, (double) rec.cenrad, want_rad);
		free(chat);
	}

	for (iter = 0; iter < 4; iter++)
	{
		WeaveQueryLut lut;
		double		best_exact = -1e30,
					best_l2 = -1e30;
		float		censcore,
					bound,
					boundl2;

		for (j = 0; j < dim; j++)
			qv[j] = rnd_unit();
		if (weave_query_lut_build(&lut, &q, qv, malloc) != 0)
			continue;

		/* The <q, c> term comes from the centroid CODE, which is what a scan has
		 * (contract C3: the bound reads only header-resident data). */
		censcore = weave_lut_score_code(&lut, bits, cencode, rec.censcale);
		bound = weave_block_bound_ip(&lut, rec.smax, rec.maxrecnorm, censcore,
									 rec.cenrad);
		boundl2 = weave_block_bound_l2(&lut, rec.smax, rec.maxrecnorm, censcore,
									   rec.cenrad, rec.minnorm);

		CHECK(bound == bound && bound < 3.4e38f,
			  "dim=%d bits=%d: the IP bound is not finite", dim, bits);

		for (i = 0; i < nlive; i++)
		{
			double		ip = 0,
						l2;

			for (j = 0; j < dim; j++)
				ip += (double) qv[j] * recon[(size_t) i * dim + j];

			/* B1: the bound must cover the exact reconstructed score.  The
			 * tolerance is one float epsilon scaled by the magnitudes involved --
			 * NOT a fudge factor: the bound is computed in float from float
			 * inputs, so demanding exact >= would fail on rounding alone. */
			CHECK((double) bound >= ip - 1e-5 * (fabs(ip) + 1.0),
				  "dim=%d bits=%d nlive=%d lane=%d: BOUND VIOLATED, bound %.9g < score %.9g",
				  dim, bits, nlive, slot[i], (double) bound, ip);
			if (ip > best_exact)
				best_exact = ip;

			/* B3: the L2 form.  weave_block_bound_l2 bounds -||q-v||^2, i.e. the
			 * similarity, so the comparison is in that sign convention. */
			l2 = -(double) lut.qnorm2 + 2.0 * ip;
			{
				double		vn = 0;

				for (j = 0; j < dim; j++)
					vn += (double) recon[(size_t) i * dim + j] *
						recon[(size_t) i * dim + j];
				l2 -= vn;
			}
			CHECK((double) boundl2 >= l2 - 1e-4 * (fabs(l2) + 1.0),
				  "dim=%d bits=%d lane=%d: L2 BOUND VIOLATED, %.9g < %.9g",
				  dim, bits, slot[i], (double) boundl2, l2);
			if (l2 > best_l2)
				best_l2 = l2;
		}

		/* B2: the same assertion against the kernel a scan actually runs, so a
		 * bound that holds for the reference and not for the shipping scorer is
		 * caught here rather than in production. */
		{
			const WeaveScoreKernel *k = weave_score_kernel_lookup("scalar");

			if (k != NULL)
			{
				float		out[WEAVE_VEC_BLOCK];
				WeaveScoreBlock blk;
				int			n;

				memset(out, 0, sizeof(out));
				memset(&blk, 0, sizeof(blk));
				blk.lut = &lut;
				blk.layout = WEAVE_PACK_LANE;
				blk.codes = block;
				/* The record stores (scale, norm) interleaved per lane, so the
				 * kernel's scale stride is 2 -- passing 1 would feed it norms as
				 * scales on every other lane, which is exactly the kind of silent
				 * wrongness the stride parameter exists to make explicit. */
				blk.scales = rec.lane;
				blk.scalestride = 2;
				blk.nlanes = WEAVE_VEC_BLOCK;
				blk.livemask = rec.livemask;
				blk.firstwarp = rec.firstwarp;
				blk.allow = NULL;
				blk.nwarp = 0;
				n = k->score_block(&blk, out);
				for (i = 0; i < n && i < WEAVE_VEC_BLOCK; i++)
				{
					if ((rec.livemask & (1u << i)) == 0)
						continue;
					CHECK((double) bound >= (double) out[i] -
						  1e-5 * (fabs((double) out[i]) + 1.0),
						  "dim=%d bits=%d lane=%d: bound %.9g < KERNEL score %.9g",
						  dim, bits, i, (double) bound, (double) out[i]);
				}
			}
		}

		/* B5: not vacuous.  The ratio is recorded so looseness is a number rather
		 * than an impression, and bounded so an infinite bound cannot pass. */
		if (best_exact > 1e-6)
		{
			double		r = (double) bound / best_exact;

			ratio_sum += r;
			ratio_n += 1;
			if (r > ratio_max)
				ratio_max = r;
			CHECK(r < 1000.0,
				  "dim=%d bits=%d nlive=%d: bound is %.1fx the true block max,"
				  " which is vacuous", dim, bits, nlive, r);
		}
		(void) best_l2;

		free(lut._alloc);
	}

	free(slot);
	free(lane_norm);
	free(lane_scale);
	free(qv);
	free(cen);
	free(recon);
	free(orig);
	free(cencode);
	free(code);
	free(block);
	weave_quantizer_free(&q, free);
}

int
main(void)
{
	static const int dims[] = {8, 16, 31, 64, 128, 256, 384, 960};
	static const int lives[] = {1, 2, 7, 16, 31, 32};
	int			di,
				bits,
				li;

	printf("== V7/V8 contract (C2): the block bound is an upper bound ==\n");

	for (di = 0; di < (int) (sizeof(dims) / sizeof(dims[0])); di++)
		for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
			for (li = 0; li < (int) (sizeof(lives) / sizeof(lives[0])); li++)
				one_case(dims[di], bits, lives[li]);

	if (ratio_n > 0)
		printf("bound / true block max: mean %.3fx, worst %.3fx"
			   " (uniform random blocks, NOT a pruning-rate claim)\n",
			   ratio_sum / ratio_n, ratio_max);
	printf("checks: %ld, failures: %ld\n", checks, failures);
	if (failures > 0)
	{
		printf("== FAILED ==\n");
		return 1;
	}
	printf("== (C2) HOLDS ==\n");
	return 0;
}
