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
 * K1-K5 are about the EXACT kernels.  Task V16 added an approximate family (the
 * byte-LUT kernels, whose 8-bit query table costs a rounding residual per
 * coordinate), so the list is split on WeaveScoreKernel.approximate and the
 * approximate half gets its own properties, K6-K11, described in full at
 * test_lut8_build() and byte_case() below:
 *
 *	 K6  weave_query_lut_build()'s byte table == an independent transcription of
 *		 the formula documented at WeaveQueryLut.lut8, NULL at every width but 4
 *		 bits, and inside the single `_alloc` block.
 *	 K7  lut-byte (AVX2) is BIT-IDENTICAL to lut-byte-ref (scalar) and to a byte
 *		 score computed here from the codes before they were packed.
 *	 K8  the deviation from the exact oracle is REPORTED, not asserted.  An
 *		 approximate kernel cannot be gated on equality with an exact one; doing
 *		 it anyway and then loosening the comparison to a tolerance would weaken
 *		 K2 as well.
 *	 K9  a deliberately saturating table (255 per coordinate) across the
 *		 256-coordinate widening boundary, compared against a CLOSED FORM.
 *	 K10 every block the byte kernels cannot score as specified is refused with
 *		 -1 -- not delegated to the oracle -- with a positive control so the
 *		 rejections are not vacuous.
 *	 K11 weave_score_kernel_best() never returns an approximate kernel, and
 *		 weave_score_kernel_lookup() still finds them by name.
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
 * Splitting the registry on `approximate`
 *
 * K1-K5 assert BIT-IDENTITY with the oracle, which is a statement about the
 * exact kernels only.  Handing an approximate kernel to those properties would
 * not fail honestly -- it would fail always, and the obvious repair (loosen the
 * comparison to a tolerance) would silently weaken the exact kernels' gate too,
 * which is the one thing weave/kernels.h insists must not happen.  So the list is
 * split at the source and each half gets the property that applies to it: memcmp
 * against the oracle for the exact half (K2), memcmp against each OTHER plus a
 * reported deviation for the approximate half (K6-K8).
 * ------------------------------------------------------------------------- */

static int
kernel_list_exact(const WeaveScoreKernel **out, int max)
{
	const WeaveScoreKernel *all[16];
	int			n = weave_score_kernel_list(all, 16);
	int			m = 0;
	int			i;

	for (i = 0; i < n && m < max; i++)
		if (!all[i]->approximate)
			out[m++] = all[i];
	return m;
}

static int
kernel_list_approx(const WeaveScoreKernel **out, int max)
{
	const WeaveScoreKernel *all[16];
	int			n = weave_score_kernel_list(all, 16);
	int			m = 0;
	int			i;

	for (i = 0; i < n && m < max; i++)
		if (all[i]->approximate)
			out[m++] = all[i];
	return m;
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

	/* K2 + K3 for every other available EXACT path.  The approximate paths are
	 * gated separately, against each other, in byte_case(). */
	nkernel = kernel_list_exact(kernels, 8);
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
	int			nkernel = kernel_list_exact(kernels, 8);
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
	int			nkernel = kernel_list_exact(kernels, 8);
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
	int			nkernel = kernel_list_exact(kernels, 8);
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
	int			nkernel = kernel_list_exact(kernels, 8);
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

/* ---------------------------------------------------------------------------
 * K6-K10: the byte-LUT family (task V16)
 *
 * These kernels are APPROXIMATE -- their 8-bit query table costs one rounding
 * residual per coordinate -- so the gate is deliberately shaped differently from
 * K1-K5 and it matters that each part is doing its own job:
 *
 *	 K6  weave_query_lut_build()'s byte table equals an independent transcription
 *		 of the formula documented at WeaveQueryLut.lut8, and it is NULL at every
 *		 width but 4 bits, and it lives inside the single `_alloc` block.  Nothing
 *		 else tests the builder; without this, K7 would gate the kernels against a
 *		 table that could itself be wrong in a way both kernels reproduce.
 *	 K7  lut-byte (AVX2) is BIT-IDENTICAL to lut-byte-ref (scalar), and both are
 *		 bit-identical to a byte score this file computes from the codes BEFORE
 *		 they were packed.  Bit-identity is the right bar between these two
 *		 because they are the same integer arithmetic twice -- integer addition is
 *		 associative, so there is no rounding to excuse a difference.  The
 *		 test-local third opinion is what makes it more than self-consistency: it
 *		 shares no addressing with either (it reads the pre-pack code buffers),
 *		 so a nibble order or lane mixup that both kernels agreed on still fails.
 *	 K8  the deviation from the EXACT oracle is REPORTED, not asserted.  There is
 *		 no correct constant to assert here: the deviation is the error budget
 *		 this kernel family trades for speed, and its size is a number we want on
 *		 the record.  Asserting equality against `scalar` would be asserting that
 *		 the approximation is not an approximation.
 *	 K9  a deliberately SATURATING table (lut[j][c] = c, every code nibble 15, so
 *		 every coordinate contributes exactly 255) at dims that cross 256.  The
 *		 16-bit accumulator overflow the AVX2 path has to widen around survives
 *		 ordinary random input -- random codes average ~128 per coordinate and the
 *		 wrap needs a sustained maximum -- so it needs a set built to force it.
 *		 The expected value is a closed form, 255 * dim, not another kernel's
 *		 output, so this fails even if every path wrapped identically.
 *	 K10 every block these kernels cannot score EXACTLY AS SPECIFIED is refused
 *		 with -1: any width but 4 bits, WEAVE_PACK_VECMAJOR, an absent byte
 *		 table, nlanes out of range, a firstwarp past nwarp.  Paired with a
 *		 positive control on the same block so the rejections are not vacuous.
 *		 They must not delegate to the oracle -- see the note at the byte family
 *		 in src/vector/kernels.c.
 * ------------------------------------------------------------------------- */

/*
 * Independent transcription of the byte quantization documented at
 * WeaveQueryLut.lut8 in weave/quantize.h.  Deliberately NOT a call into
 * weave_query_lut_build(): K6 exists to check that the shipped builder implements
 * the documented formula, and a check that called the shipped builder would only
 * prove it equals itself.
 */
static void
lut8_build_ref(const WeaveQueryLut *lut, weave_uint8 *tbl, float *step,
			   float *offset)
{
	int			dim = lut->dim;
	int			n = lut->nlevels;
	double		off = 0.0;
	float		range = 0.0f;
	int			j,
				c;

	for (j = 0; j < dim; j++)
	{
		const float *row = lut->lut + (size_t) j * n;
		float		mn = row[0];
		float		mx = row[0];

		for (c = 1; c < n; c++)
		{
			if (row[c] < mn)
				mn = row[c];
			if (row[c] > mx)
				mx = row[c];
		}
		if (mx - mn > range)
			range = mx - mn;
	}
	*step = (range > 0.0f) ? range / 255.0f : 0.0f;

	for (j = 0; j < dim; j++)
	{
		const float *row = lut->lut + (size_t) j * n;
		float		mn = row[0];

		for (c = 1; c < n; c++)
			if (row[c] < mn)
				mn = row[c];
		off += (double) mn;

		for (c = 0; c < n; c++)
		{
			long		v;

			if (*step <= 0.0f)
			{
				tbl[(size_t) j * n + c] = 0;
				continue;
			}
			v = lrintf((row[c] - mn) / *step);
			if (v < 0)
				v = 0;
			if (v > 255)
				v = 255;
			tbl[(size_t) j * n + c] = (weave_uint8) v;
		}
	}
	*offset = (float) off;
}

/* The reference byte score for one lane, from the code buffer as it was BEFORE
 * packing.  The final (float) ((step * acc + offset) * scale) is the one
 * float-producing expression both kernels share (byte_lane_score in
 * src/vector/kernels.c), so this is the third, independent transcription of it. */
static float
byte_ref_score(const weave_uint8 *tbl, int dim, float step, float offset,
			   const weave_uint8 *code, float scale)
{
	weave_uint32 acc = 0;
	int			j;

	for (j = 0; j < dim; j++)
		acc += tbl[(size_t) j * 16 + code_at(code, j, 4)];
	return (float) (((double) step * (double) acc + (double) offset) *
					(double) scale);
}

/* K6 */
static void
test_lut8_build(void)
{
	int			dims[] = {4, 8, 15, 16, 17, 63, 64, 200, 257, 512, 960};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	int			bits,
				i;

	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		for (i = 0; i < ndims; i++)
		{
			int			dim = dims[i];
			int			n = 1 << bits;
			WeaveQuantizer q;
			WeaveQueryLut lut;
			float	   *query;
			weave_uint8 *want;
			float		step,
						offset;
			int			j;

			if (weave_quantizer_init(&q, dim, bits, NULL, malloc, free) != 0)
			{
				CHECK(0, "K6 quantizer_init failed dim=%d bits=%d", dim, bits);
				continue;
			}
			query = malloc(sizeof(float) * (size_t) dim);
			for (j = 0; j < dim; j++)
				query[j] = (float) rnd_normal();
			CHECK(weave_query_lut_build(&lut, &q, query, malloc) == 0,
				  "K6 lut_build failed dim=%d bits=%d", dim, bits);

			if (bits != 4)
			{
				/* A byte LUT is one 16-entry vpshufb table, so 4 bits is a hard
				 * requirement.  Absent, not approximated at some other width. */
				CHECK(lut.lut8 == NULL,
					  "K6 lut8 built at bits=%d, where no byte kernel can use it",
					  bits);
				free(lut._alloc);
				free(query);
				weave_quantizer_free(&q, free);
				continue;
			}

			CHECK(lut.lut8 != NULL, "K6 no lut8 at 4 bits, dim=%d", dim);

			/* ONE allocation, still: `lut` at the front, `lut8` immediately past
			 * the floats, `_alloc` at the block start.  A caller frees once, and
			 * this is the assertion that keeps that true. */
			CHECK((void *) lut.lut == lut._alloc,
				  "K6 lut is not at the start of _alloc");
			CHECK(lut.lut8 == (weave_uint8 *) lut.lut +
				  sizeof(float) * (size_t) dim * n,
				  "K6 lut8 is not immediately past the float table (dim=%d)", dim);

			want = malloc((size_t) dim * n);
			lut8_build_ref(&lut, want, &step, &offset);
			CHECK(memcmp(want, lut.lut8, (size_t) dim * n) == 0,
				  "K6 lut8 != the documented quantization, dim=%d", dim);
			CHECK(memcmp(&step, &lut.lut8_step, sizeof(float)) == 0,
				  "K6 lut8_step %.9g != reference %.9g, dim=%d",
				  (double) lut.lut8_step, (double) step, dim);
			CHECK(memcmp(&offset, &lut.lut8_offset, sizeof(float)) == 0,
				  "K6 lut8_offset %.9g != reference %.9g, dim=%d",
				  (double) lut.lut8_offset, (double) offset, dim);

			free(want);
			free(lut._alloc);
			free(query);
			weave_quantizer_free(&q, free);
		}
	}
}

/*
 * K8's accumulators: reported, never asserted.
 *
 * TWO POPULATIONS, kept apart deliberately.  Index 0 is a REAL table --
 * weave_query_lut_build() on a real quantizer, so every row is q_j * C[c] for one
 * Lloyd-Max codebook and the whole table's dynamic range is the one the codec
 * actually produces.  Index 1 is SYNTHETIC: every entry an independent N(0,1)
 * draw, which makes the global range several times wider than any single row's,
 * so the shared step is coarse relative to what each coordinate contributes and
 * the error is correspondingly larger.  Pooling them would report the synthetic
 * arm's number as if it described the codec, which is the shape of dishonesty
 * AGENTS.md rule 8 is about.  The synthetic arm is here to exercise the kernels
 * over tables the codec does not make, not to characterize the error.
 */
static double byte_maxabs[2] = {0.0, 0.0};
static double byte_maxrel[2] = {0.0, 0.0};
static int	byte_maxabs_dim[2] = {0, 0};
static int	byte_maxrel_dim[2] = {0, 0};
static long byte_devcmp[2] = {0, 0};
static long byte_bitcmp = 0;

typedef struct ByteCase
{
	int			dim;
	int			nlanes;
	weave_uint32 livemask;
	weave_uint32 firstwarp;
	int			allowkind;		/* 0 NULL, 1 all-ones, 2 all-zero, 3 random */
	int			scalestride;	/* 1..3; the sidecar hands the kernels a stride */
	int			real;			/* 1 = real quantizer LUT, 0 = synthetic floats */
	int			scalekind;		/* 0 positive, 1 mixed sign, 2 with zeros */
} ByteCase;

/* K7 + K8 + K3 for one block. */
static void
byte_case(const ByteCase *c)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel = kernel_list_approx(kernels, 8);
	const int	bits = 4;
	int			dim = c->dim;
	int			codebytes = (dim * bits + 7) / 8;
	size_t		blockbytes = (size_t) weave_block_codebytes(dim, bits);
	weave_uint32 nwarp = c->firstwarp + (weave_uint32) c->nlanes;
	size_t		allowwords = (size_t) ((nwarp + 63) / 64);
	WeaveQuantizer q;
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	weave_uint8 *block = malloc(blockbytes);
	weave_uint8 *codes = malloc((size_t) codebytes * WEAVE_VEC_BLOCK);
	weave_uint64 *allow = calloc(allowwords, sizeof(weave_uint64));
	float	   *scales = malloc(sizeof(float) * WEAVE_VEC_BLOCK * 3);
	float	   *exact = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *want = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *first = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *got = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
	float	   *synthlut = NULL;
	weave_uint8 *synthtbl = NULL;
	weave_uint32 avail;
	int			haveq = 0;
	int			s,
				j,
				k;

	memset(&lut, 0, sizeof(lut));
	if (c->real && weave_quantizer_init(&q, dim, bits, NULL, malloc, free) == 0)
	{
		float	   *query = malloc(sizeof(float) * (size_t) dim);
		double		nrm = 0.0;

		haveq = 1;
		for (j = 0; j < dim; j++)
			query[j] = (float) rnd_normal();

		/*
		 * NORMALIZED, so K8's absolute deviation is on the scale of a
		 * unit-vector score and is therefore comparable to the figure
		 * bench/code_scan.c's self-check reports.  An unnormalized N(0,1) query
		 * has ||q|| ~ sqrt(dim), which multiplies every score -- and every
		 * deviation -- by ~22 at dim = 513 and would make the recorded number a
		 * fact about this generator rather than about the byte table.
		 */
		for (j = 0; j < dim; j++)
			nrm += (double) query[j] * (double) query[j];
		nrm = (nrm > 0.0) ? 1.0 / sqrt(nrm) : 1.0;
		for (j = 0; j < dim; j++)
			query[j] = (float) ((double) query[j] * nrm);

		CHECK(weave_query_lut_build(&lut, &q, query, malloc) == 0,
			  "K7 lut_build failed dim=%d", dim);
		free(query);
	}
	else
	{
		/*
		 * Synthetic tables, with the byte table built by this file's own
		 * transcription.  Two reasons this arm exists rather than testing only
		 * real LUTs: the quantizer declines dim < 4, and a real table's rows are
		 * all q_j * C[c] for one codebook C, which is a narrower shape than the
		 * kernels are required to handle.
		 */
		float		step,
					offset;

		synthlut = malloc(sizeof(float) * (size_t) dim * 16);
		synthtbl = malloc((size_t) dim * 16);
		for (j = 0; j < dim * 16; j++)
			synthlut[j] = (float) rnd_normal();
		lut.dim = dim;
		lut.nlevels = 16;
		lut.lut = synthlut;
		lut8_build_ref(&lut, synthtbl, &step, &offset);
		lut.lut8 = synthtbl;
		lut.lut8_step = step;
		lut.lut8_offset = offset;
	}

	memset(block, 0, blockbytes);
	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
	{
		weave_uint8 *code = codes + (size_t) s * codebytes;

		for (j = 0; j < codebytes; j++)
			code[j] = (weave_uint8) (rnd64() & 0xFF);
		if ((dim * bits) % 8 != 0)
			code[codebytes - 1] &=
				(weave_uint8) ((1u << ((dim * bits) % 8)) - 1);
		weave_pack_lane(WEAVE_PACK_LANE, dim, bits, block, s, code);
	}

	for (s = 0; s < WEAVE_VEC_BLOCK * 3; s++)
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
		case 3:
			for (j = 0; j < (int) allowwords; j++)
				allow[j] = rnd64() & rnd64();
			break;
		default:
			break;
	}

	memset(&blk, 0, sizeof(blk));
	blk.lut = &lut;
	blk.layout = WEAVE_PACK_LANE;
	blk.codes = block;
	blk.scales = scales;
	blk.scalestride = c->scalestride;
	blk.nlanes = c->nlanes;
	blk.livemask = c->livemask;
	blk.firstwarp = c->firstwarp;
	blk.allow = (c->allowkind == 0) ? NULL : allow;
	blk.nwarp = nwarp;

	avail = c->livemask;
	if (c->nlanes < WEAVE_VEC_BLOCK)
		avail &= (weave_uint32) ((1u << c->nlanes) - 1);
	if (blk.allow != NULL)
		for (s = 0; s < c->nlanes; s++)
		{
			weave_uint32 w = c->firstwarp + (weave_uint32) s;

			if (((allow[w >> 6] >> (w & 63)) & 1) == 0)
				avail &= ~(1u << s);
		}

	/* The independent byte reference, and the exact oracle for K8. */
	for (s = 0; s < c->nlanes; s++)
	{
		if ((avail & (1u << s)) == 0)
			want[s] = WEAVE_KERNEL_NEVER;
		else
			want[s] = byte_ref_score(lut.lut8, dim, lut.lut8_step,
									 lut.lut8_offset,
									 codes + (size_t) s * codebytes,
									 scales[(size_t) s * c->scalestride]);
	}
	CHECK(weave_score_kernel_scalar.score_block(&blk, exact) == c->nlanes,
		  "K8 the oracle refused a well-formed 4-bit block, dim=%d", dim);

	for (k = 0; k < nkernel; k++)
	{
		for (s = 0; s < WEAVE_VEC_BLOCK; s++)
			got[s] = 1234.5f;	/* poison: a path that writes nothing must fail */

		CHECK(kernels[k]->score_block(&blk, got) == c->nlanes,
			  "K7 %s wrong return dim=%d nlanes=%d", kernels[k]->name, dim,
			  c->nlanes);

		byte_bitcmp += c->nlanes;
		CHECK(memcmp(got, want, sizeof(float) * (size_t) c->nlanes) == 0,
			  "K7 %s != the independent byte reference (dim=%d nlanes=%d "
			  "live=%08x firstwarp=%u allow=%d stride=%d real=%d)",
			  kernels[k]->name, dim, c->nlanes, c->livemask, c->firstwarp,
			  c->allowkind, c->scalestride, c->real);
		if (memcmp(got, want, sizeof(float) * (size_t) c->nlanes) != 0)
			for (s = 0; s < c->nlanes; s++)
				if (memcmp(&got[s], &want[s], sizeof(float)) != 0)
					printf("    lane %d: reference %.9g %s %.9g\n", s,
						   (double) want[s], kernels[k]->name, (double) got[s]);

		/* Every approximate kernel against the FIRST one, which is what makes
		 * this a statement about lut-byte's SIMD addressing specifically rather
		 * than about the byte table both of them read. */
		if (k == 0)
			memcpy(first, got, sizeof(float) * (size_t) c->nlanes);
		else
			CHECK(memcmp(got, first, sizeof(float) * (size_t) c->nlanes) == 0,
				  "K7 %s != %s (dim=%d nlanes=%d)", kernels[k]->name,
				  kernels[0]->name, dim, c->nlanes);

		/* K3 still applies: positional indexing is a contract, not a
		 * convenience, and an approximate score does not excuse a wrong lane. */
		for (s = 0; s < c->nlanes; s++)
		{
			if ((avail & (1u << s)) == 0)
				CHECK(is_never(got[s]),
					  "K3 %s lane %d should be NEVER, got %.9g",
					  kernels[k]->name, s, (double) got[s]);
			else
				CHECK(!is_never(got[s]),
					  "K3 %s lane %d is live+allowed but NEVER",
					  kernels[k]->name, s);
		}

		/* K8: recorded, not asserted, and recorded per population. */
		if (k == 0)
		{
			int			p = haveq ? 0 : 1;

			for (s = 0; s < c->nlanes; s++)
			{
				double		e,
							a;

				if ((avail & (1u << s)) == 0)
					continue;
				e = (double) exact[s];
				a = fabs((double) got[s] - e);
				byte_devcmp[p]++;
				if (a > byte_maxabs[p])
				{
					byte_maxabs[p] = a;
					byte_maxabs_dim[p] = dim;
				}
				if (e != 0.0 && a / fabs(e) > byte_maxrel[p])
				{
					byte_maxrel[p] = a / fabs(e);
					byte_maxrel_dim[p] = dim;
				}
			}
		}
	}

	if (haveq)
	{
		free(lut._alloc);
		weave_quantizer_free(&q, free);
	}
	free(synthlut);
	free(synthtbl);
	free(block);
	free(codes);
	free(allow);
	free(scales);
	free(exact);
	free(want);
	free(first);
	free(got);
}

/*
 * K9: the saturating set.
 *
 * lut[j][c] = c makes the global range 15, so step = 15/255 and every row
 * quantizes to c * 17 -- level 15 maps to exactly 255.  Codes of all-0xFF bytes
 * are all-15 nibbles, so EVERY coordinate contributes the maximum and the true
 * accumulator is 255 * dim: 244800 at dim = 960, nearly four times what a 16-bit
 * lane holds.  A kernel that accumulates in 16 bits without widening inside the
 * loop wraps, and the wrap is a plausible-looking score, not a crash.
 *
 * The expectation is a CLOSED FORM computed here, not another kernel's output, so
 * this catches the overflow even if every path wrapped in the same way.  The dims
 * bracket the 256-coordinate flush boundary from both sides.
 */
static void
test_byte_saturation(void)
{
	int			dims[] = {1, 2, 3, 255, 256, 257, 258, 511, 512, 513,
		959, 960, 961, 1024, 2048};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	const WeaveScoreKernel *kernels[8];
	int			nkernel = kernel_list_approx(kernels, 8);
	int			i,
				k;

	for (i = 0; i < ndims; i++)
	{
		int			dim = dims[i];
		size_t		blockbytes = (size_t) weave_block_codebytes(dim, 4);
		float	   *lutvals = malloc(sizeof(float) * (size_t) dim * 16);
		weave_uint8 *tbl = malloc((size_t) dim * 16);
		weave_uint8 *block = malloc(blockbytes);
		float	   *scales = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
		float	   *got = malloc(sizeof(float) * WEAVE_VEC_BLOCK);
		WeaveQueryLut lut;
		WeaveScoreBlock blk;
		float		step,
					offset;
		float		expect;
		int			j,
					c,
					s;

		for (j = 0; j < dim; j++)
			for (c = 0; c < 16; c++)
				lutvals[j * 16 + c] = (float) c;
		memset(block, 0xFF, blockbytes);
		for (s = 0; s < WEAVE_VEC_BLOCK; s++)
			scales[s] = 1.0f;

		memset(&lut, 0, sizeof(lut));
		lut.dim = dim;
		lut.nlevels = 16;
		lut.lut = lutvals;
		lut8_build_ref(&lut, tbl, &step, &offset);
		lut.lut8 = tbl;
		lut.lut8_step = step;
		lut.lut8_offset = offset;

		/* The table this set depends on: level 15 must be exactly 255, or the
		 * accumulator does not reach its ceiling and the test proves nothing. */
		CHECK(tbl[15] == 255, "K9 dim=%d: lut8[0][15] is %d, not 255",
			  dim, (int) tbl[15]);
		CHECK(memcmp(&offset, &lut.lut8_offset, sizeof(float)) == 0 &&
			  offset == 0.0f, "K9 dim=%d: offset %.9g, expected 0",
			  dim, (double) offset);

		expect = (float) (((double) step * (double) (255u * (unsigned) dim) +
						   (double) offset) * 1.0);

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
			CHECK(kernels[k]->score_block(&blk, got) == WEAVE_VEC_BLOCK,
				  "K9 %s refused a saturating block, dim=%d",
				  kernels[k]->name, dim);
			for (s = 0; s < WEAVE_VEC_BLOCK; s++)
				CHECK(memcmp(&got[s], &expect, sizeof(float)) == 0,
					  "K9 %s dim=%d lane %d: %.9g, expected %.9g "
					  "(acc should be 255 * %d = %u; a 16-bit accumulator "
					  "that never widens wraps here)",
					  kernels[k]->name, dim, s, (double) got[s],
					  (double) expect, dim, 255u * (unsigned) dim);
		}

		free(lutvals);
		free(tbl);
		free(block);
		free(scales);
		free(got);
	}
}

/*
 * K10: the byte kernels REFUSE what they cannot score as specified, and score
 * what they can.
 *
 * The positive control at the end is not decoration.  Every negative case here
 * would also pass against a kernel that refused everything, and "refuses
 * everything" is a much easier bug to write than the ones being tested for.
 */
static void
test_byte_rejects(void)
{
	const WeaveScoreKernel *kernels[8];
	int			nkernel = kernel_list_approx(kernels, 8);
	const int	dim = 64;
	const weave_uint32 nwarp = 200;
	size_t		allowwords = (size_t) ((nwarp + 63) / 64);
	weave_uint64 *allow = malloc(allowwords * sizeof(weave_uint64));
	weave_uint8 *block = malloc((size_t) weave_block_codebytes(dim, 4));
	float	   *query = malloc(sizeof(float) * (size_t) dim);
	float		scales[WEAVE_VEC_BLOCK];
	float		out[WEAVE_VEC_BLOCK];
	WeaveQuantizer q;
	WeaveQueryLut lut;
	WeaveScoreBlock blk;
	int			bits,
				i,
				k;

	memset(allow, 0xFF, allowwords * sizeof(weave_uint64));
	memset(block, 0x5A, (size_t) weave_block_codebytes(dim, 4));
	for (i = 0; i < WEAVE_VEC_BLOCK; i++)
		scales[i] = 1.0f;
	for (i = 0; i < dim; i++)
		query[i] = (float) rnd_normal();

	/*
	 * EVERY OTHER WIDTH IS REFUSED, and refused because the table is absent
	 * rather than because a width check happened to fire: these are real LUTs
	 * from the shipped builder, so lut8 == NULL is the builder's own decision.
	 */
	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		if (bits == 4)
			continue;
		CHECK(weave_quantizer_init(&q, dim, bits, NULL, malloc, free) == 0,
			  "K10 quantizer_init failed bits=%d", bits);
		CHECK(weave_query_lut_build(&lut, &q, query, malloc) == 0,
			  "K10 lut_build failed bits=%d", bits);

		memset(&blk, 0, sizeof(blk));
		blk.lut = &lut;
		blk.layout = WEAVE_PACK_LANE;
		blk.codes = block;
		blk.scales = scales;
		blk.scalestride = 1;
		blk.nlanes = WEAVE_VEC_BLOCK;
		blk.livemask = 0xFFFFFFFFu;

		for (k = 0; k < nkernel; k++)
			CHECK(kernels[k]->score_block(&blk, out) == -1,
				  "K10 %s accepted a %d-bit block; a 16-entry vpshufb table "
				  "cannot hold %d levels", kernels[k]->name, bits, 1 << bits);

		free(lut._alloc);
		weave_quantizer_free(&q, free);
	}

	/* The 4-bit block everything below perturbs. */
	CHECK(weave_quantizer_init(&q, dim, 4, NULL, malloc, free) == 0,
		  "K10 quantizer_init failed at 4 bits");
	CHECK(weave_query_lut_build(&lut, &q, query, malloc) == 0,
		  "K10 lut_build failed at 4 bits");
	CHECK(lut.lut8 != NULL, "K10 no byte table at 4 bits");

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
	blk.firstwarp = 0;

	for (k = 0; k < nkernel; k++)
	{
		WeaveScoreBlock bad;
		weave_uint8 *savetbl;

		/*
		 * VECMAJOR.  lut-wide and lut-avx2 delegate this to the oracle; these
		 * must NOT, because a byte kernel that quietly ran the exact scalar path
		 * would report the oracle's scores and the oracle's speed under its own
		 * name -- the retracted-claim shape of AGENTS.md rule 8.
		 */
		bad = blk;
		bad.layout = WEAVE_PACK_VECMAJOR;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted WEAVE_PACK_VECMAJOR instead of refusing it",
			  kernels[k]->name);
		bad.layout = (WeavePackLayout) 7;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted pack layout 7", kernels[k]->name);

		/* nlanes out of range, both ends. */
		bad = blk;
		bad.nlanes = 0;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted nlanes=0", kernels[k]->name);
		bad.nlanes = WEAVE_VEC_BLOCK + 1;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted nlanes=33", kernels[k]->name);
		bad.nlanes = -1;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted nlanes=-1", kernels[k]->name);

		/* firstwarp past the end of `allow`.  Same argument as K4's: the bitmap
		 * is exactly nwarp bits, so removing the bound is an ASan report. */
		bad = blk;
		bad.firstwarp = nwarp;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s read allow[] with firstwarp=nwarp", kernels[k]->name);
		bad.firstwarp = nwarp - WEAVE_VEC_BLOCK + 1;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted a block ending one warp past nwarp",
			  kernels[k]->name);
		bad.firstwarp = 0xFFFFFFFFu;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted firstwarp=UINT32_MAX (the wrap case)",
			  kernels[k]->name);

		bad = blk;
		bad.scalestride = 0;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted scalestride=0", kernels[k]->name);
		bad.codes = NULL;
		bad.scalestride = 1;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s accepted a NULL code pointer", kernels[k]->name);

		/* nlevels == 16 but no byte table: the query side did not build one, so
		 * there is nothing to gather from and guessing is not an option. */
		bad = blk;
		savetbl = lut.lut8;
		lut.lut8 = NULL;
		CHECK(kernels[k]->score_block(&bad, out) == -1,
			  "K10 %s scored a block with no byte table", kernels[k]->name);
		lut.lut8 = savetbl;

		/* THE POSITIVE CONTROL. */
		CHECK(kernels[k]->score_block(&blk, out) == WEAVE_VEC_BLOCK,
			  "K10 %s refused the well-formed 4-bit block every case above "
			  "perturbs; the rejections prove nothing without this",
			  kernels[k]->name);
	}

	free(lut._alloc);
	weave_quantizer_free(&q, free);
	free(allow);
	free(block);
	free(query);
}

static void
test_byte_kernels(void)
{
	int			dims[] = {1, 2, 3, 4, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128,
		254, 255, 256, 257, 258, 511, 512, 513, 959, 960, 961, 1024};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	const WeaveScoreKernel *kernels[8];
	int			napprox = kernel_list_approx(kernels, 8);
	int			i,
				t,
				k;

	printf("  approximate kernels under test:");
	for (k = 0; k < napprox; k++)
		printf(" %s", kernels[k]->name);
	printf("\n");
	CHECK(napprox >= 1, "K7: no approximate kernel is registered at all");

	/* Every dim, both table sources, all lanes live and unfiltered. */
	for (i = 0; i < ndims; i++)
	{
		ByteCase	c;
		int			real;

		for (real = 0; real <= 1; real++)
		{
			memset(&c, 0, sizeof(c));
			c.dim = dims[i];
			c.nlanes = WEAVE_VEC_BLOCK;
			c.livemask = 0xFFFFFFFFu;
			c.scalestride = 1;
			c.real = real;
			byte_case(&c);

			/* nothing live: no code byte may be read, sentinels still written */
			c.livemask = 0;
			byte_case(&c);

			/* one live lane per group, and one straddling a group boundary */
			c.livemask = 0x01010101u;
			byte_case(&c);
			c.livemask = 0x00000180u;
			byte_case(&c);

			/* short blocks, the tail of the last block in a weft */
			c.livemask = 0xFFFFFFFFu;
			c.nlanes = 1;
			byte_case(&c);
			c.nlanes = 17;
			byte_case(&c);
			c.nlanes = 31;
			byte_case(&c);
		}
	}

	/* Random walk: masks, allowlists, strides and scale shapes together. */
	for (t = 0; t < 400; t++)
	{
		ByteCase	c;

		memset(&c, 0, sizeof(c));
		c.dim = dims[rnd_below(ndims)];
		c.nlanes = 1 + rnd_below(WEAVE_VEC_BLOCK);
		c.livemask = (weave_uint32) rnd64();
		if (rnd_below(4) == 0)
			c.livemask &= (weave_uint32) rnd64();
		c.firstwarp = (weave_uint32) (rnd64() % 4096);
		c.allowkind = rnd_below(4);
		c.scalestride = 1 + rnd_below(3);
		c.real = rnd_below(2);
		c.scalekind = rnd_below(3);
		byte_case(&c);
	}

	printf("  K7 lane comparisons: %ld, bit-identical\n", byte_bitcmp);
	printf("  K8 deviation from the EXACT oracle (REPORTED, not asserted: this is\n"
		   "     the 8-bit table's rounding, the price this family pays).  The\n"
		   "     relative figure is |dev| / |exact| per lane, so it is dominated by\n"
		   "     lanes whose exact score is a near-cancellation of dim terms that\n"
		   "     are not near zero; the absolute figure is the one to read.\n");
	printf("     REAL tables (weave_query_lut_build on a real quantizer)\n");
	printf("       live lanes %ld  max |dev| %.6g (dim=%d)  max rel %.6g (dim=%d)\n",
		   byte_devcmp[0], byte_maxabs[0], byte_maxabs_dim[0],
		   byte_maxrel[0], byte_maxrel_dim[0]);
	printf("     SYNTHETIC tables (every entry an independent N(0,1) draw; a\n"
		   "     wider global range than the codec makes, so a coarser step)\n");
	printf("       live lanes %ld  max |dev| %.6g (dim=%d)  max rel %.6g (dim=%d)\n",
		   byte_devcmp[1], byte_maxabs[1], byte_maxabs_dim[1],
		   byte_maxrel[1], byte_maxrel_dim[1]);
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
	printf("kernels available on this host (exact best first, then approximate, "
		   "scalar last):");
	for (k = 0; k < nkernel; k++)
		printf(" %s%s", kernels[k]->name,
			   kernels[k]->approximate ? "(approx)" : "");
	printf("\n");
	CHECK(nkernel >= 1, "no kernel at all is registered");
	CHECK(strcmp(kernels[nkernel - 1]->name, "scalar") == 0,
		  "scalar must be the last resort in the registry, found %s",
		  kernels[nkernel - 1]->name);
	CHECK(weave_score_kernel_lookup("scalar") == &weave_score_kernel_scalar,
		  "lookup(\"scalar\") did not find the oracle");
	CHECK(weave_score_kernel_lookup("no-such-kernel") == NULL,
		  "lookup of an unknown name must fail");

	/*
	 * best() is the first EXACT kernel in the list, not simply the first one.
	 * This used to read `== kernels[0]`, which was the same statement while every
	 * registered kernel was exact; stating it against the first non-approximate
	 * entry instead means a future registry reorder that legitimately puts an
	 * approximate kernel earlier does not fail here spuriously, while the
	 * property that matters still holds.
	 */
	for (k = 0; k < nkernel; k++)
		if (!kernels[k]->approximate)
			break;
	CHECK(k < nkernel, "no exact kernel is registered at all");
	CHECK(weave_score_kernel_best() == kernels[k],
		  "best() is %s, not the first exact kernel in the list (%s)",
		  weave_score_kernel_best()->name, kernels[k]->name);

	/*
	 * K11: the fence around the approximate kernels.
	 *
	 * best() is what `auto` resolves to, and an approximate kernel is only sound
	 * behind an exact rerank that no query path in this tree can yet guarantee
	 * (WeaveScoreKernel.approximate).  If this assertion ever fails, every
	 * unreranked vector query on the host silently changed its answers -- which is
	 * the class of bug AGENTS.md rule 1 says no fixed-output test can catch, so it
	 * is asserted here.
	 */
	CHECK(!weave_score_kernel_best()->approximate,
		  "best() returned the approximate kernel %s; `auto` must never select "
		  "one without a guaranteed exact rerank",
		  weave_score_kernel_best()->name);

	/* Forced by name, though: that is how a host is A/B'd and how a bug report
	 * from another machine is reproduced.  Every listed kernel must resolve. */
	for (k = 0; k < nkernel; k++)
		CHECK(weave_score_kernel_lookup(kernels[k]->name) == kernels[k],
			  "lookup(\"%s\") did not resolve to the listed kernel",
			  kernels[k]->name);

	CHECK(weave_score_kernel_lookup("lut-byte-ref") != NULL,
		  "lookup(\"lut-byte-ref\") failed; the scalar byte reference is "
		  "unconditional");
	CHECK(weave_score_kernel_lookup("lut-byte-ref")->approximate,
		  "lut-byte-ref must be marked approximate");
	{
		/*
		 * lut-byte exists iff this host can run AVX2, and it is NEVER aliased to
		 * lut-byte-ref where it cannot: a scalar fallback answering to a SIMD
		 * kernel's name is how the sibling project published a headline number it
		 * had to retract (AGENTS.md rule 8).  The list is the authority for
		 * whether the host has it, so the two are cross-checked rather than
		 * either one being trusted alone.
		 */
		const WeaveScoreKernel *byte = weave_score_kernel_lookup("lut-byte");
		int			listed = 0;

		for (k = 0; k < nkernel; k++)
			if (strcmp(kernels[k]->name, "lut-byte") == 0)
				listed = 1;
		printf("lut-byte (AVX2 byte-LUT): %s\n",
			   byte != NULL ? "present" : "absent on this host");
		CHECK((byte != NULL) == (listed != 0),
			  "lookup(\"lut-byte\") and the kernel list disagree about "
			  "availability");
		if (byte != NULL)
		{
			CHECK(byte->approximate, "lut-byte must be marked approximate");
			CHECK(byte != weave_score_kernel_lookup("lut-byte-ref"),
				  "lut-byte must not be an alias for the scalar reference");
		}
	}

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

	printf("K6 the 8-bit query table == the documented quantization\n");
	test_lut8_build();

	printf("K7/K8 byte-LUT kernels: bit-identical to each other, deviation from "
		   "the oracle reported\n");
	test_byte_kernels();

	printf("K9 saturating tables: 255 per coordinate, across the 256-coordinate "
		   "widening boundary\n");
	test_byte_saturation();

	printf("K10 the byte kernels refuse what they cannot score exactly\n");
	test_byte_rejects();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
