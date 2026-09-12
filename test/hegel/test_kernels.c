/*-------------------------------------------------------------------------
 *
 * test_kernels.c
 *		Differential test: every available block-scoring kernel == the scalar
 *		oracle, bit for bit.
 *
 * Links src/vector/kernels.c, src/vector/quantize.c and src/vector/pack.c
 * directly, with no backend -- the same standalone shape as test_quantize.c, and
 * for the same reason (doc/TESTING.md): the test must exercise the SHIPPED
 * kernels.  A test that linked its own copy of the arithmetic would agree with
 * itself forever, which is precisely the fast-but-wrong failure mode AGENTS.md
 * rule 8 cites.
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tk test/hegel/test_kernels.c \
 *			src/vector/kernels.c src/vector/quantize.c src/vector/pack.c -lm
 *		/tmp/tk
 *
 * Also required to pass under -fsanitize=address,undefined: the fast paths do
 * unaligned multi-byte loads out of a packed block, and "reads one byte past the
 * last coordinate's group" is a bug that only shows up at a page boundary in
 * production.
 *
 * Properties:
 *
 *	 K1  the scalar kernel equals an independent reimplementation of
 *		 scale * sum_j lut[j][code_j], computed from the codes BEFORE they were
 *		 packed.  This is the oracle's own oracle: it catches a lane/slot mixup
 *		 or a layout misreading, which K2 alone could not (every kernel could
 *		 agree on the same wrong lane).
 *	 K2  every kernel from weave_score_kernel_list() writes bytes identical to the
 *		 scalar kernel's, compared with memcmp rather than a tolerance.  See the
 *		 EXACTNESS note in weave/kernels.h for why identity is the right bar
 *		 here and not a convenient one.
 *	 K3  a lane that is dead (livemask bit clear) or masked out (allow bit clear)
 *		 is exactly WEAVE_KERNEL_NEVER in every path, and a lane that is live and
 *		 allowed is never WEAVE_KERNEL_NEVER.  Positional indexing is a contract
 *		 (C5), not a convenience.
 *	 K4  an inconsistent block description is rejected (-1) by every path rather
 *		 than scored.  These numbers come off a page and pages are not trusted.
 *		 Includes the two that are memory safety and not merely arithmetic: an
 *		 unrecognized pack layout, and a `firstwarp` that would index the
 *		 `allow` bitmap past its `nwarp` end.  The firstwarp case is run against
 *		 an exactly-sized heap allocation so that ASan sees the read if the
 *		 bound is ever removed -- a check that only asserted the return value
 *		 would still pass with the bound deleted on most runs.
 *	 K5  the kernel's score for a REAL quantized vector equals the inner product
 *		 of the query with the RECONSTRUCTION, within tolerance -- the definition
 *		 in doc/specs/VECTOR_CHANNEL.md sect. 2, computed from float vectors that
 *		 never went near a lookup table.  K1 compares two transcriptions of the
 *		 same expression and so can only catch a typo; K5 is what would catch a
 *		 wrong scoring DEFINITION.
 *
 * The grid deliberately includes the awkward cases: dim not a multiple of
 * anything, 3-bit codes (whose groups are byte-aligned only because the group is
 * 8 lanes wide), an allow bitmap whose 32-bit window straddles two 64-bit words,
 * a block with one live lane, a block with none, negative and zero scales, and
 * lookup tables containing zeros, denormals and 1e30.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_kernels.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/kernels.h"

static int	failures = 0;
static int	checks = 0;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			if (failures <= 40) \
			{ \
				printf("FAIL %s:%d: ", __FILE__, __LINE__); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

/* xoshiro256** -- test-local, so the corpus is reproducible and independent of
 * libc.  Same generator as test_quantize.c. */
static weave_uint64 rngs[4] = {0x9e3779b97f4a7c15ULL, 0xbf58476d1ce4e5b9ULL,
	0x94d049bb133111ebULL, 0x2545f4914f6cdd1dULL};

static weave_uint64
rotl64(weave_uint64 x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static weave_uint64
rnd64(void)
{
	weave_uint64 r = rotl64(rngs[1] * 5, 7) * 9;
	weave_uint64 t = rngs[1] << 17;

	rngs[2] ^= rngs[0];
	rngs[3] ^= rngs[1];
	rngs[1] ^= rngs[2];
	rngs[0] ^= rngs[3];
	rngs[2] ^= t;
	rngs[3] = rotl64(rngs[3], 45);
	return r;
}

static double
rnd_unit(void)
{
	return (double) (rnd64() >> 11) * (1.0 / 9007199254740992.0);
}

static int
rnd_below(int bound)
{
	return (int) (rnd64() % (weave_uint64) bound);
}

static double
rnd_normal(void)
{
	double		u1 = rnd_unit();
	double		u2 = rnd_unit();

	if (u1 < 1e-300)
		u1 = 1e-300;
	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/*
 * Read coordinate j out of a single vector's code buffer.
 *
 * A deliberate reimplementation of pack.c's bit order (LSB first, coordinate
 * major, `bits` bits per coordinate) rather than a call into it: property K1
 * exists to check that the kernels agree with the FORMAT, and a check that
 * called the same accessor the code under test calls would only prove
 * self-consistency.  If this loop is wrong, K1 fails loudly on the first block,
 * which is the desired failure mode.
 */
static weave_uint32
code_at(const weave_uint8 *code, int j, int bits)
{
	size_t		bit = (size_t) j * bits;
	weave_uint32 v = 0;
	int			k;

	for (k = 0; k < bits; k++)
		if (code[(bit + k) >> 3] & (weave_uint8) (1u << ((bit + k) & 7)))
			v |= (1u << k);
	return v;
}

/* The reference score for one lane: strictly ascending j, accumulated in double,
 * multiplied by the scale last -- the order weave_lut_score_code() uses, which is
 * what makes bit-exact comparison meaningful rather than lucky. */
static float
ref_score(const WeaveQueryLut *lut, int bits, const weave_uint8 *code, float scale)
{
	double		acc = 0.0;
	int			j;

	for (j = 0; j < lut->dim; j++)
		acc += lut->lut[(size_t) j * lut->nlevels + code_at(code, j, bits)];
	return (float) (acc * (double) scale);
}

/* Is `x` exactly the never-sentinel?  memcmp, not ==, because == on -inf is true
 * for a NaN-free comparison but says nothing about the bits. */
static int
is_never(float x)
{
	float		never = WEAVE_KERNEL_NEVER;

	return memcmp(&x, &never, sizeof(float)) == 0;
}

/* ---------------------------------------------------------------------------
 * One randomized block, scored by every path
 * ------------------------------------------------------------------------- */

typedef struct KernelCase
{
	int			bits;
	int			dim;
	WeavePackLayout layout;
	int			nlanes;
	weave_uint32 livemask;
	weave_uint32 firstwarp;
	int			allowkind;		/* 0 NULL, 1 all-ones, 2 all-zero, 3 random */
	int			nwarpslack;		/* 0 bitmap ends exactly at the block, 1 slack */
	int			lutkind;		/* 0 normal, 1 with zeros/denormals/huge */
	int			scalekind;		/* 0 positive, 1 mixed sign, 2 with zeros */
} KernelCase;

static void
run_case(const KernelCase *c)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel;
	int			nlevels = 1 << c->bits;
	int			codebytes = (c->dim * c->bits + 7) / 8;
	size_t		blockbytes = (size_t) weave_block_codebytes(c->dim, c->bits);

	/*
	 * The allowlist is allocated EXACTLY as long as nwarp says it is, and by
	 * default nwarp ends at the block's last lane -- the tightest legal case.
	 * A loose allocation would hide precisely the bug this bound exists to
	 * prevent: with slack, an over-read lands in the malloc'd region and ASan
	 * stays quiet.  Tight, it is a heap-buffer-overflow report.
	 */
	weave_uint32 nwarp = c->firstwarp + (weave_uint32) c->nlanes +
		(c->nwarpslack ? 77u : 0u);
	size_t		allowwords = (size_t) ((nwarp + 63) / 64);
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	weave_uint8 *block = malloc(blockbytes);
	weave_uint8 *codes = malloc((size_t) codebytes * WEAVE_VEC_BLOCK);
	weave_uint64 *allow = calloc(allowwords, sizeof(weave_uint64));
	float	   *scales = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *expected = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *ref = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *got = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	weave_uint32 avail;
	int			s,
				j,
				k,
				n;

	memset(&lut, 0, sizeof(lut));
	lut.dim = c->dim;
	lut.nlevels = nlevels;
	lut.lut = malloc(sizeof(float) * (size_t) c->dim * nlevels);
	lut._alloc = lut.lut;

	for (j = 0; j < c->dim * nlevels; j++)
	{
		double		x = rnd_normal();

		if (c->lutkind == 1)
		{
			switch (rnd_below(8))
			{
				case 0:
					x = 0.0;
					break;
				case 1:
					x = -0.0;
					break;
				case 2:
					x = 1e30;
					break;
				case 3:
					x = -1e30;
					break;
				case 4:
					x = 1e-42;	/* denormal as a float */
					break;
				default:
					break;
			}
		}
		lut.lut[j] = (float) x;
	}

	/* Codes, then the block built through the pack API so that the layout under
	 * test is the layout pack.c defines rather than one this file invented. */
	memset(block, 0, blockbytes);
	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
	{
		weave_uint8 *code = codes + (size_t) s * codebytes;

		for (j = 0; j < codebytes; j++)
			code[j] = (weave_uint8) (rnd64() & 0xFF);
		if ((c->dim * c->bits) % 8 != 0)
			code[codebytes - 1] &=
				(weave_uint8) ((1u << ((c->dim * c->bits) % 8)) - 1);
		weave_pack_lane(c->layout, c->dim, c->bits, block, s, code);
	}

	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
	{
		double		x = 0.25 + rnd_unit() * 4.0;

		if (c->scalekind == 1 && (rnd64() & 1))
			x = -x;
		else if (c->scalekind == 2 && rnd_below(4) == 0)
			x = 0.0;
		scales[s] = (float) x;
	}

	switch (c->allowkind)
	{
		case 1:
			memset(allow, 0xFF, allowwords * sizeof(weave_uint64));
			break;
		case 2:
			break;				/* already zero */
		case 3:
			for (j = 0; j < (int) allowwords; j++)
				allow[j] = rnd64() & rnd64();	/* sparse-ish */
			break;
		default:
			break;
	}

	memset(&blk, 0, sizeof(blk));
	blk.lut = &lut;
	blk.layout = c->layout;
	blk.codes = block;
	blk.scales = scales;
	blk.scalestride = 1;
	blk.nlanes = c->nlanes;
	blk.livemask = c->livemask;
	blk.firstwarp = c->firstwarp;
	blk.allow = (c->allowkind == 0) ? NULL : allow;
	blk.nwarp = nwarp;

	/* K1: the oracle against an independent reimplementation. */
	n = weave_score_kernel_scalar.score_block(&blk, expected);
	CHECK(n == c->nlanes, "scalar returned %d, expected %d lanes", n, c->nlanes);

	avail = c->livemask;
	if (c->nlanes < WEAVE_VEC_BLOCK)
		avail &= (weave_uint32) ((1u << c->nlanes) - 1);
	for (s = 0; s < c->nlanes; s++)
	{
		int			allowed = 1;

		if (blk.allow != NULL)
		{
			weave_uint32 w = c->firstwarp + (weave_uint32) s;

			allowed = (allow[w >> 6] >> (w & 63)) & 1;
		}
		if (!allowed)
			avail &= ~(1u << s);
	}

	for (s = 0; s < c->nlanes; s++)
	{
		if ((avail & (1u << s)) == 0)
			ref[s] = WEAVE_KERNEL_NEVER;
		else
			ref[s] = ref_score(&lut, c->bits, codes + (size_t) s * codebytes,
							   scales[s]);
	}
	CHECK(memcmp(ref, expected, sizeof(float) * (size_t) c->nlanes) == 0,
		  "K1 scalar != independent reference (bits=%d dim=%d layout=%d nlanes=%d)",
		  c->bits, c->dim, (int) c->layout, c->nlanes);
	if (memcmp(ref, expected, sizeof(float) * (size_t) c->nlanes) != 0)
		for (s = 0; s < c->nlanes; s++)
			if (memcmp(&ref[s], &expected[s], sizeof(float)) != 0)
				printf("    lane %d: reference %.9g scalar %.9g\n",
					   s, (double) ref[s], (double) expected[s]);

	/* K3 on the oracle. */
	for (s = 0; s < c->nlanes; s++)
	{
		if ((avail & (1u << s)) == 0)
			CHECK(is_never(expected[s]),
				  "K3 scalar lane %d should be NEVER, got %.9g",
				  s, (double) expected[s]);
		else
			CHECK(!is_never(expected[s]),
				  "K3 scalar lane %d is live+allowed but NEVER", s);
	}

	/* K2 + K3 for every other available path. */
	nkernel = weave_score_kernel_list(kernels, 8);
	for (k = 0; k < nkernel; k++)
	{
		for (s = 0; s < WEAVE_VEC_BLOCK; s++)
			got[s] = 1234.5f;	/* poison: a path that writes nothing must fail */

		n = kernels[k]->score_block(&blk, got);
		CHECK(n == c->nlanes, "%s returned %d, expected %d",
			  kernels[k]->name, n, c->nlanes);
		CHECK(memcmp(got, expected, sizeof(float) * (size_t) c->nlanes) == 0,
			  "K2 %s != scalar (bits=%d dim=%d layout=%d nlanes=%d live=%08x "
			  "firstwarp=%u allow=%d)",
			  kernels[k]->name, c->bits, c->dim, (int) c->layout, c->nlanes,
			  c->livemask, c->firstwarp, c->allowkind);
		if (memcmp(got, expected, sizeof(float) * (size_t) c->nlanes) != 0)
			for (s = 0; s < c->nlanes; s++)
				if (memcmp(&got[s], &expected[s], sizeof(float)) != 0)
					printf("    lane %d: scalar %.9g %s %.9g\n", s,
						   (double) expected[s], kernels[k]->name,
						   (double) got[s]);

		for (s = 0; s < c->nlanes; s++)
			if ((avail & (1u << s)) == 0)
				CHECK(is_never(got[s]), "K3 %s lane %d should be NEVER, got %.9g",
					  kernels[k]->name, s, (double) got[s]);
	}

	free(lut._alloc);
	free(block);
	free(codes);
	free(allow);
	free(scales);
	free(expected);
	free(ref);
	free(got);
}

/* ---------------------------------------------------------------------------
 * K4: a block description that cannot have come from a valid page is rejected,
 * not scored.  On-disk bytes are not trusted (doc/CONVENTIONS.md), and the
 * backend adapter turns -1 into a clean ERROR.
 * ------------------------------------------------------------------------- */
static void
test_rejects(void)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel = weave_score_kernel_list(kernels, 8);
	float		out[WEAVE_VEC_BLOCK];
	float		scales[WEAVE_VEC_BLOCK];
	weave_uint8 block[WEAVE_VEC_BLOCK * 8];
	float		lutvals[8 * 16];
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	int			i,
				k;

	for (i = 0; i < WEAVE_VEC_BLOCK; i++)
		scales[i] = 1.0f;
	memset(block, 0, sizeof(block));
	for (i = 0; i < 8 * 16; i++)
		lutvals[i] = 0.5f;

	memset(&lut, 0, sizeof(lut));
	lut.dim = 8;
	lut.nlevels = 16;
	lut.lut = lutvals;

	memset(&blk, 0, sizeof(blk));
	blk.lut = &lut;
	blk.layout = WEAVE_PACK_LANE;
	blk.codes = block;
	blk.scales = scales;
	blk.scalestride = 1;
	blk.nlanes = WEAVE_VEC_BLOCK;
	blk.livemask = 0xFFFFFFFFu;

	for (k = 0; k < nkernel; k++)
	{
		WeaveScoreBlock bad;

		/* nlevels that is not 1 << bits for a supported width */
		bad = blk;
		lut.nlevels = 5;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted nlevels=5", kernels[k]->name);
		lut.nlevels = 2;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted nlevels=2 (1-bit codes are not supported)",
			  kernels[k]->name);
		/* One power of two ABOVE the supported ceiling.  This used to be 32,
		 * which was correct while WEAVE_BITS_MAX was 4 and is wrong now that it
		 * is 8 -- 32 levels is a legitimate 5-bit table.  Derived from the
		 * constant rather than written as a literal so it cannot go stale the
		 * same way twice; the widths that ARE supported are covered positively by
		 * the randomized grid, which sweeps WEAVE_BITS_MIN..WEAVE_BITS_MAX. */
		lut.nlevels = 1 << (WEAVE_BITS_MAX + 1);
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted nlevels=%d, above WEAVE_BITS_MAX",
			  kernels[k]->name, 1 << (WEAVE_BITS_MAX + 1));
		lut.nlevels = 16;

		bad = blk;
		bad.nlanes = 0;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted nlanes=0", kernels[k]->name);
		bad.nlanes = WEAVE_VEC_BLOCK + 1;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted nlanes=33", kernels[k]->name);

		bad = blk;
		bad.scalestride = 0;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted scalestride=0", kernels[k]->name);

		bad = blk;
		bad.codes = NULL;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted a NULL code pointer", kernels[k]->name);

		bad = blk;
		lut.dim = 0;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted dim=0", kernels[k]->name);
		lut.dim = WEAVE_MAX_DIM + 1;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted dim > WEAVE_MAX_DIM", kernels[k]->name);
		lut.dim = 8;

		/*
		 * A pack layout that is not one of the two defined values.  Not a
		 * pedantic check: the layout selects a bit-addressing scheme, and
		 * src/vector/pack.c states the consequence of getting it wrong -- the
		 * reader "does not fail, it returns wrong distances".  Defaulting an
		 * unknown value to either layout would be exactly that.
		 */
		bad = blk;
		bad.layout = (WeavePackLayout) 2;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted pack layout 2", kernels[k]->name);
		bad.layout = (WeavePackLayout) 255;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K4 %s accepted pack layout 255", kernels[k]->name);
	}
}

/* ---------------------------------------------------------------------------
 * K4, the memory-safety half: a firstwarp that does not fit inside the `allow`
 * bitmap is rejected, and not read.
 *
 * firstwarp comes out of a WeaveVecBlockHdr, i.e. off a page, and it indexes
 * `allow`.  Before nwarp existed the bitmap carried no length, so a corrupt
 * header was an unbounded out-of-bounds read -- doc/CONVENTIONS.md forbids that
 * outright ("a corrupt page produces a clean ERROR, never a crash and never a
 * wrong answer"), and this codebase has already had one such read turn into a
 * real SEGV (weave_page_recyclable).
 *
 * The bitmap here is heap-allocated at EXACTLY nwarp bits and the offending
 * firstwarps are far past its end, so if the bound is ever removed this function
 * is a heap-buffer-overflow under -fsanitize=address rather than a quiet pass.
 * That is why it allocates instead of using a stack array with slack.
 * ------------------------------------------------------------------------- */
static void
test_allow_bounds(void)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel = weave_score_kernel_list(kernels, 8);
	const int	dim = 8;
	const int	bits = 4;
	const weave_uint32 nwarp = 200;	/* 4 words minus 56 bits: a tight tail */
	size_t		allowwords = (size_t) ((nwarp + 63) / 64);
	weave_uint64 *allow = malloc(allowwords * sizeof(weave_uint64));
	weave_uint8 *block = malloc((size_t) weave_block_codebytes(dim, bits));
	float	   *lutvals = malloc(sizeof(float) * (size_t) dim * 16);
	float		scales[WEAVE_VEC_BLOCK];
	float		out[WEAVE_VEC_BLOCK];
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	weave_uint32 bad_firstwarp[] = {
		nwarp,					/* one warp past the end */
		nwarp - WEAVE_VEC_BLOCK + 1, /* starts inside, ends one past */
		nwarp + 1,
		4096,
		0x7FFFFFFFu,
		0xFFFFFFFFu,			/* the wrap-around case: +nlanes overflows 32 bits */
		0xFFFFFFE0u
	};
	int			nbad = (int) (sizeof(bad_firstwarp) / sizeof(bad_firstwarp[0]));
	int			i,
				k;

	memset(allow, 0xFF, allowwords * sizeof(weave_uint64));
	memset(block, 0x5A, (size_t) weave_block_codebytes(dim, bits));
	for (i = 0; i < dim * 16; i++)
		lutvals[i] = 0.25f;
	for (i = 0; i < WEAVE_VEC_BLOCK; i++)
		scales[i] = 1.0f;

	memset(&lut, 0, sizeof(lut));
	lut.dim = dim;
	lut.nlevels = 16;
	lut.lut = lutvals;

	memset(&blk, 0, sizeof(blk));
	blk.lut = &lut;
	blk.layout = WEAVE_PACK_LANE;
	blk.codes = block;
	blk.scales = scales;
	blk.scalestride = 1;
	blk.nlanes = WEAVE_VEC_BLOCK;
	blk.livemask = 0xFFFFFFFFu;
	blk.allow = allow;
	blk.nwarp = nwarp;

	for (k = 0; k < nkernel; k++)
	{
		for (i = 0; i < nbad; i++)
		{
			WeaveScoreBlock bad = blk;

			bad.firstwarp = bad_firstwarp[i];
			CHECK(kernels[k]->score_block(&bad, out) == -1,
				  "K4 %s read allow[] with firstwarp=%u against nwarp=%u",
				  kernels[k]->name, bad_firstwarp[i], nwarp);
		}

		/* The last legal block, which must still be SCORED: an off-by-one in
		 * the other direction would reject valid pages. */
		{
			WeaveScoreBlock ok = blk;

			ok.firstwarp = nwarp - WEAVE_VEC_BLOCK;
			CHECK(kernels[k]->score_block(&ok, out) == WEAVE_VEC_BLOCK,
				  "K4 %s rejected the last legal block (firstwarp=%u nwarp=%u)",
				  kernels[k]->name, ok.firstwarp, nwarp);
		}

		/* A short tail block whose lanes end exactly at nwarp.  This is the case
		 * that makes lane_avail_mask() address its second word from
		 * firstwarp + nlanes - 1 rather than from firstwarp + 31. */
		{
			WeaveScoreBlock ok = blk;

			ok.nlanes = 9;
			ok.firstwarp = nwarp - 9;
			CHECK(kernels[k]->score_block(&ok, out) == 9,
				  "K4 %s rejected a legal 9-lane tail block", kernels[k]->name);
		}

		/* nwarp is ignored when there is no bitmap, so an absurd firstwarp with
		 * allow == NULL is not an error: nothing indexes it. */
		{
			WeaveScoreBlock ok = blk;

			ok.allow = NULL;
			ok.nwarp = 0;
			ok.firstwarp = 0xFFFFFFFFu;
			CHECK(kernels[k]->score_block(&ok, out) == WEAVE_VEC_BLOCK,
				  "K4 %s rejected an unfiltered block over firstwarp",
				  kernels[k]->name);
		}
	}

	free(allow);
	free(block);
	free(lutvals);
}

/* ---------------------------------------------------------------------------
 * K5: the kernels compute the thing the SPEC defines, not merely the thing the
 * reference loop transcribes.
 *
 * K1 checks the kernels against a reimplementation of
 * scale * sum_j lut[j][code_j] over a random table.  That catches a typo, a lane
 * mixup or a layout misreading -- but both sides are transcriptions of the same
 * expression, so if that expression were the WRONG DEFINITION of a score, K1
 * would pass.
 *
 * doc/specs/VECTOR_CHANNEL.md sect. 2 defines the score of a code as the inner
 * product of the query with the RECONSTRUCTION:
 *
 *		score = <q, scale * R^-1(dequant(code))>
 *
 * so this builds a real query and real quantized vectors through
 * weave_quantizer_init / weave_encode, reconstructs each one with
 * weave_decode(), takes the dot product in float space -- no lookup table
 * anywhere on that side -- and compares.
 *
 * TOLERANCE, and why it is not memcmp.  This side goes through the inverse
 * rotation and dim float multiplies; the kernel side goes through a float
 * lookup table built from the forward-rotated query.  They are the same value in
 * exact arithmetic and differ by rounding, whose natural scale is the
 * Cauchy-Schwarz magnitude ||q|| * ||rec|| rather than the score itself (a score
 * near zero is a cancellation of dim terms that are not near zero).  The bound
 * used is 1e-5 * ||q|| * ||rec||, which is ~100x the worst deviation actually
 * observed; the observed ratio is printed so the constant is a measured margin
 * and not a wish, and so a host whose rounding differs shows up as a shrinking
 * margin before it shows up as a failure.  This is the one property in this file
 * that is a tolerance, and it is the one that could not be anything else.  It is
 * still tight enough to catch a wrong DEFINITION by orders of magnitude: dropping
 * the renormalization scale, or scoring against dequant(code) instead of the
 * reconstruction, is an O(1) relative error.
 * ------------------------------------------------------------------------- */

static double
dot_f(const float *a, const float *b, int dim)
{
	double		s = 0.0;
	int			j;

	for (j = 0; j < dim; j++)
		s += (double) a[j] * (double) b[j];
	return s;
}

static double worst_recon_ratio = 0.0;

static void
recon_case(int dim, int bits, WeavePackLayout layout)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel = weave_score_kernel_list(kernels, 8);
	WeaveQuantizer q;
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	float	   *query = malloc(sizeof(float) * (size_t) dim);
	float	   *v = malloc(sizeof(float) * (size_t) dim);
	float	   *rec = malloc(sizeof(float) * (size_t) dim);
	weave_uint8 *block;
	float		scales[WEAVE_VEC_BLOCK];
	float		want[WEAVE_VEC_BLOCK];
	float		out[WEAVE_VEC_BLOCK];
	double		tol[WEAVE_VEC_BLOCK];
	double		qnorm;
	int			s,
				j,
				k;

	if (weave_quantizer_init(&q, dim, bits, NULL, malloc, free) != 0)
	{
		CHECK(0, "K5 quantizer_init failed dim=%d bits=%d", dim, bits);
		free(query);
		free(v);
		free(rec);
		return;
	}
	block = malloc((size_t) weave_block_codebytes(dim, bits));
	memset(block, 0, (size_t) weave_block_codebytes(dim, bits));

	for (j = 0; j < dim; j++)
		query[j] = (float) rnd_normal();
	qnorm = sqrt(dot_f(query, query, dim));

	CHECK(weave_query_lut_build(&lut, &q, query, malloc) == 0,
		  "K5 query_lut_build failed dim=%d bits=%d", dim, bits);

	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
	{
		weave_uint8 code[WEAVE_CODE_MAX_BYTES];
		float		norm,
					scale;
		double		recnorm;

		for (j = 0; j < dim; j++)
			v[j] = (float) rnd_normal();
		CHECK(weave_encode(&q, v, code, &norm, &scale) == 0,
			  "K5 encode failed dim=%d bits=%d lane=%d", dim, bits, s);
		weave_pack_lane(layout, dim, bits, block, s, code);
		scales[s] = scale;

		weave_decode(&q, code, scale, rec);
		recnorm = sqrt(dot_f(rec, rec, dim));
		want[s] = (float) dot_f(query, rec, dim);
		tol[s] = 1e-5 * qnorm * recnorm + 1e-9;
	}

	memset(&blk, 0, sizeof(blk));
	blk.lut = &lut;
	blk.layout = layout;
	blk.codes = block;
	blk.scales = scales;
	blk.scalestride = 1;
	blk.nlanes = WEAVE_VEC_BLOCK;
	blk.livemask = 0xFFFFFFFFu;

	for (k = 0; k < nkernel; k++)
	{
		CHECK(kernels[k]->score_block(&blk, out) == WEAVE_VEC_BLOCK,
			  "K5 %s wrong return dim=%d bits=%d", kernels[k]->name, dim, bits);

		for (s = 0; s < WEAVE_VEC_BLOCK; s++)
		{
			double		err = fabs((double) out[s] - (double) want[s]);
			double		scale_of_err = err / (tol[s] > 0 ? tol[s] : 1.0);

			if (scale_of_err > worst_recon_ratio)
				worst_recon_ratio = scale_of_err;
			CHECK(err <= tol[s],
				  "K5 %s dim=%d bits=%d layout=%d lane %d: kernel %.9g but "
				  "<q,reconstruct> %.9g (err %.4g, tol %.4g)",
				  kernels[k]->name, dim, bits, (int) layout, s,
				  (double) out[s], (double) want[s], err, tol[s]);
		}
	}

	free(lut._alloc);
	free(block);
	free(query);
	free(v);
	free(rec);
	weave_quantizer_free(&q, free);
}

static void
test_reconstruction(void)
{
	int			dims[] = {8, 32, 64, 256, 384};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	int			bits,
				i,
				layout;

	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
		for (i = 0; i < ndims; i++)
			for (layout = 0; layout <= 1; layout++)
				recon_case(dims[i], bits, (WeavePackLayout) layout);

	printf("  worst |kernel - <q,reconstruct>| was %.3f of the tolerance\n",
		   worst_recon_ratio);
}

/*
 * The lane sidecar is an array of WeaveVecLane {scale, norm}, so the shuttle
 * hands the kernels a float pointer with stride 2.  Exercised explicitly because
 * a kernel that ignored the stride would still pass every stride-1 case, and
 * would then score every lane with lane 0's magnitude.
 */
static void
test_scalestride(void)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel = weave_score_kernel_list(kernels, 8);
	const int	dim = 24;
	const int	bits = 4;
	int			codebytes = (dim * bits + 7) / 8;
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	weave_uint8 *block = malloc((size_t) weave_block_codebytes(dim, bits));
	weave_uint8 *codes = malloc((size_t) codebytes * WEAVE_VEC_BLOCK);
	float		pairs[WEAVE_VEC_BLOCK * 2];
	float		expected[WEAVE_VEC_BLOCK];
	float		got[WEAVE_VEC_BLOCK];
	int			s,
				j,
				k;

	memset(&lut, 0, sizeof(lut));
	lut.dim = dim;
	lut.nlevels = 1 << bits;
	lut.lut = malloc(sizeof(float) * (size_t) dim * lut.nlevels);
	for (j = 0; j < dim * lut.nlevels; j++)
		lut.lut[j] = (float) rnd_normal();

	memset(block, 0, (size_t) weave_block_codebytes(dim, bits));
	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
	{
		weave_uint8 *code = codes + (size_t) s * codebytes;

		for (j = 0; j < codebytes; j++)
			code[j] = (weave_uint8) (rnd64() & 0xFF);
		weave_pack_lane(WEAVE_PACK_LANE, dim, bits, block, s, code);
		pairs[s * 2] = (float) (0.5 + rnd_unit());	/* scale */
		pairs[s * 2 + 1] = (float) (10.0 + rnd_unit());	/* norm: must be ignored */
	}

	memset(&blk, 0, sizeof(blk));
	blk.lut = &lut;
	blk.layout = WEAVE_PACK_LANE;
	blk.codes = block;
	blk.scales = pairs;
	blk.scalestride = 2;
	blk.nlanes = WEAVE_VEC_BLOCK;
	blk.livemask = 0xFFFFFFFFu;

	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
		expected[s] = ref_score(&lut, bits, codes + (size_t) s * codebytes,
								pairs[s * 2]);

	for (k = 0; k < nkernel; k++)
	{
		CHECK(kernels[k]->score_block(&blk, got) == WEAVE_VEC_BLOCK,
			  "stride: %s wrong return", kernels[k]->name);
		CHECK(memcmp(got, expected, sizeof(expected)) == 0,
			  "stride: %s != reference with scalestride=2", kernels[k]->name);
	}

	free(lut.lut);
	free(block);
	free(codes);
}

int
main(void)
{
	const WeaveScoreKernel *kernels[8];
	int			dims[] = {1, 3, 8, 17, 32, 64, 200, 256, 384, 768};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	weave_uint32 firstwarps[] = {0, 1, 31, 32, 33, 45, 63, 64, 96, 1000};
	int			nfw = (int) (sizeof(firstwarps) / sizeof(firstwarps[0]));
	int			nkernel;
	int			bits,
				i,
				k,
				t;

	nkernel = weave_score_kernel_list(kernels, 8);
	printf("kernels available on this host (best first):");
	for (k = 0; k < nkernel; k++)
		printf(" %s", kernels[k]->name);
	printf("\n");
	CHECK(nkernel >= 1, "no kernel at all is registered");
	CHECK(strcmp(kernels[nkernel - 1]->name, "scalar") == 0,
		  "scalar must be the last resort in the registry, found %s",
		  kernels[nkernel - 1]->name);
	CHECK(weave_score_kernel_lookup("scalar") == &weave_score_kernel_scalar,
		  "lookup(\"scalar\") did not find the oracle");
	CHECK(weave_score_kernel_lookup("no-such-kernel") == NULL,
		  "lookup of an unknown name must fail");
	CHECK(weave_score_kernel_best() == kernels[0],
		  "best() disagrees with the head of the list");

	/* The interesting corners, enumerated rather than sampled. */
	printf("corner cases\n");
	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		for (i = 0; i < ndims; i++)
		{
			int			layout;

			for (layout = 0; layout <= 1; layout++)
			{
				KernelCase	c;

				memset(&c, 0, sizeof(c));
				c.bits = bits;
				c.dim = dims[i];
				c.layout = (WeavePackLayout) layout;

				/* all lanes live, no filter */
				c.nlanes = WEAVE_VEC_BLOCK;
				c.livemask = 0xFFFFFFFFu;
				c.allowkind = 0;
				run_case(&c);

				/* nothing live at all: no code byte may be read, but the
				 * sentinels must still be written */
				c.livemask = 0;
				run_case(&c);

				/* one live lane in each group, and one straddling a group
				 * boundary */
				c.livemask = 0x01010101u;
				run_case(&c);
				c.livemask = 0x00000180u;
				run_case(&c);

				/* short blocks: the tail of the last block in a weft */
				c.livemask = 0xFFFFFFFFu;
				c.nlanes = 1;
				run_case(&c);
				c.nlanes = 7;
				run_case(&c);
				c.nlanes = 8;
				run_case(&c);
				c.nlanes = 9;
				run_case(&c);
				c.nlanes = 31;
				run_case(&c);
				c.nlanes = WEAVE_VEC_BLOCK;

				/* the allowlist, including the two-word window.  The bitmap is
				 * allocated to exactly nwarp bits and nwarp ends at the block's
				 * last lane unless nwarpslack says otherwise, so an over-read is
				 * an ASan report rather than a quiet pass. */
				for (k = 0; k < nfw; k++)
				{
					c.firstwarp = firstwarps[k];
					c.allowkind = 1;
					run_case(&c);
					c.allowkind = 2;
					run_case(&c);
					c.allowkind = 3;
					run_case(&c);
					c.nwarpslack = 1;
					run_case(&c);
					c.nwarpslack = 0;
				}
				c.firstwarp = 0;
				c.allowkind = 0;

				/* awkward numbers */
				c.lutkind = 1;
				run_case(&c);
				c.scalekind = 1;
				run_case(&c);
				c.scalekind = 2;
				run_case(&c);
			}
		}
	}

	/* Random walk over the whole space, to catch what the enumeration did not
	 * think of. */
	printf("randomized grid\n");
	for (t = 0; t < 400; t++)
	{
		KernelCase	c;

		memset(&c, 0, sizeof(c));
		c.bits = WEAVE_BITS_MIN + rnd_below(WEAVE_BITS_MAX - WEAVE_BITS_MIN + 1);
		c.dim = dims[rnd_below(ndims)];
		c.layout = (WeavePackLayout) rnd_below(2);
		c.nlanes = 1 + rnd_below(WEAVE_VEC_BLOCK);
		c.livemask = (weave_uint32) rnd64();
		if (rnd_below(4) == 0)
			c.livemask &= (weave_uint32) rnd64();	/* sparser */
		c.firstwarp = (weave_uint32) (rnd64() % 4096);
		c.allowkind = rnd_below(4);
		c.nwarpslack = rnd_below(2);
		c.lutkind = rnd_below(2);
		c.scalekind = rnd_below(3);
		run_case(&c);
	}

	printf("lane sidecar stride\n");
	test_scalestride();

	printf("scores against <query, reconstruct(code)>\n");
	test_reconstruction();

	printf("rejection of impossible block descriptions\n");
	test_rejects();

	printf("rejection of a firstwarp that would read past allow[]\n");
	test_allow_bounds();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
