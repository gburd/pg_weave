/*-------------------------------------------------------------------------
 *
 * kernels.c
 *		Runtime-dispatched block-scoring kernels for the vector channel.
 *
 * Task V6 in doc/PHASES.md.  The scalar reference is the oracle every other path
 * must reproduce, so it exists first and it is the fallback forever.
 *
 * Dispatch follows PostgreSQL's own pattern for pg_popcount and CRC32C
 * (src/port/pg_popcount_*.c): resolve once into a function-pointer table.
 * Following core rather than inventing a scheme keeps "which path ran?"
 * answerable from weave_vec_kernel_name() in a bug report, which matters because
 * a wrong kernel produces wrong distances rather than a crash.  The backend half
 * of that -- the WeaveVecKernelOps table and the pg_weave.vec_kernel GUC -- is in
 * src/vector/kernel_ops.c; this file has no PostgreSQL includes so that
 * test/hegel/test_kernels.c can link the shipped kernels with a plain gcc, the
 * same argument weave/quantize.h makes for the codec.
 *
 * TWO DIFFERENT STANDARDS APPLY HERE, and conflating them is a correctness bug:
 *
 *	 - A SCORING kernel that differs from scalar in the last bit changes a score
 *	   slightly.  Tolerable, though it should be understood.  In fact every
 *	   kernel in this file is bit-identical to scalar and the test asserts it with
 *	   memcmp; see the EXACTNESS note in weave/kernels.h for why that is
 *	   affordable, and why it would stop being affordable the moment a kernel
 *	   reassociated a lane's sum.
 *	 - A ROTATION kernel that differs from scalar in the last bit changes a CODE,
 *	   and therefore what the index contains.  A row inserted on one machine and
 *	   queried on another would then give different answers.  Bit-identical or
 *	   rejected.  See the determinism warning in weave/quantize.h.  There is no
 *	   SIMD rotation here and V6 does not add one: the scalar rotation is already
 *	   +/- and one multiply in a fixed order, and V2's gate is a cross-architecture
 *	   fixture hash that a vector path would have to reproduce exactly.
 *
 * WHY THE ISA MATRIX IS SHORTER THAN THE SPEC'S.  doc/specs/VECTOR_CHANNEL.md
 * sect. 8 tabulates SSE2 / AVX2 / AVX-512BW / VNNI / NEON / SDOT, all of them
 * byte-LUT or int8-dot strategies.  Both of those families quantize the float
 * query table to 8 bits before gathering, which is an approximation, not a
 * rounding difference -- so none of them can pass V6's stated gate ("results
 * identical to the scalar path") and each one needs a recall budget that does not
 * exist yet.  Held to exactness, scoring is GATHER-bound: 32 independent
 * table lookups per coordinate.  SSE2 has no gather and no variable shift, so an
 * exact SSE2 kernel is the portable wide-word kernel below plus register
 * shuffling; the same is true of baseline NEON.  AVX2 is the first x86 ISA with
 * both (vpgatherdd, vpsrlvd), which is why it is the one vector path here.  That
 * is the same shape of conclusion pg_turbovec reached for its Hamming kernel
 * (sect. 8: they measured AVX2 and declined it because the wide-word scalar form
 * already extracted the available ILP), and it is why "lut-wide" exists as a
 * first-class kernel rather than as an afterthought.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/kernels.c
 *
 *-------------------------------------------------------------------------
 */
#include "weave/kernels.h"

/* ---------------------------------------------------------------------------
 * Host ISA detection
 *
 * __builtin_cpu_supports() rather than raw CPUID: libgcc's implementation also
 * consults XGETBV for the AVX feature bits, which raw CPUID does not, and
 * getting that wrong yields a SIGILL on a kernel that has not enabled YMM state.
 * MSVC has neither, and the meson/MSVC recipe does not compile this file today,
 * so the vector paths simply do not exist there.
 * ------------------------------------------------------------------------- */

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#define WEAVE_KERNEL_X86_GNUC	1
#include <immintrin.h>
#endif

/* ---------------------------------------------------------------------------
 * Shared plumbing
 * ------------------------------------------------------------------------- */

/*
 * Code width from the table width.  The kernels take a WeaveQueryLut, not a
 * WeaveQuantizer, and nlevels == 1 << bits by construction
 * (weave_query_lut_build), so bits is recoverable and does not need to be
 * threaded through a second parameter that could disagree with the table.
 * Returns -1 for anything that is not a supported width.
 */
static inline int
bits_from_nlevels(int nlevels)
{
	switch (nlevels)
	{
		case 4:
			return 2;
		case 8:
			return 3;
		case 16:
			return 4;
	}
	return -1;
}

/*
 * The 32 `allow` bits covering this block, as a lane-indexed mask, plus the
 * dead-lane and short-block trims.
 *
 * Lane s sits at warp firstwarp + s, so the block's slice of the allowlist is at
 * most two 64-bit words and is extracted with one shift.  A NULL allowlist means
 * "everything is allowed" -- the unfiltered scan -- and must not be confused with
 * an all-zero bitmap, which means the opposite.
 *
 * The second word is addressed from firstwarp + nlanes - 1 and NOT from
 * firstwarp + WEAVE_VEC_BLOCK - 1: block_bits() has already established that
 * warp firstwarp + nlanes - 1 is inside the bitmap, whereas a short block at the
 * end of a weft can have firstwarp + WEAVE_VEC_BLOCK past its end.  Bits above
 * nlanes are trimmed off `m` before the allowlist is consulted, so reading fewer
 * words loses nothing.
 */
static inline weave_uint32
lane_avail_mask(const WeaveScoreBlock *blk)
{
	weave_uint32 m = blk->livemask;

	if (blk->nlanes < WEAVE_VEC_BLOCK)
		m &= (weave_uint32) ((1u << blk->nlanes) - 1);

	if (blk->allow != NULL)
	{
		size_t		w0 = (size_t) (blk->firstwarp >> 6);
		size_t		w1 = (size_t) ((blk->firstwarp + (weave_uint32) blk->nlanes - 1) >> 6);
		int			off = (int) (blk->firstwarp & 63);
		weave_uint64 a = blk->allow[w0] >> off;

		if (w1 != w0 && off != 0)
			a |= blk->allow[w1] << (64 - off);
		m &= (weave_uint32) a;
	}
	return m;
}

/*
 * Validate what came off the page before indexing anything with it.
 *
 * Returns the code width, or -1 for a description no valid page can have
 * produced.  Three of these checks are about untrusted numbers reaching an
 * index rather than about arithmetic:
 *
 *	 - `layout` selects a bit-addressing scheme.  pack.c: a reader that guesses
 *	   the layout wrong "does not fail, it returns wrong distances".  An
 *	   out-of-range enum value must therefore be an error and not a default.
 *	 - `firstwarp` indexes `allow`, and `firstwarp` comes off a block header.
 *	   Without nwarp there is nothing to compare it against and a corrupt header
 *	   is an unbounded out-of-bounds read; with it, the block is rejected.  The
 *	   comparison is done in 64 bits so that a firstwarp near UINT32_MAX cannot
 *	   wrap into a passing value.
 *	 - `nlanes` and `scalestride` bound the two other indexed arrays.
 */
static inline int
block_bits(const WeaveScoreBlock *blk)
{
	int			bits;

	if (blk->lut == NULL || blk->codes == NULL || blk->scales == NULL)
		return -1;
	if (blk->lut->dim < 1 || blk->lut->dim > WEAVE_MAX_DIM || blk->lut->lut == NULL)
		return -1;
	if (blk->nlanes < 1 || blk->nlanes > WEAVE_VEC_BLOCK)
		return -1;
	if (blk->scalestride < 1)
		return -1;
	if (blk->layout != WEAVE_PACK_LANE && blk->layout != WEAVE_PACK_VECMAJOR)
		return -1;
	if (blk->allow != NULL &&
		(weave_uint64) blk->firstwarp + (weave_uint64) blk->nlanes >
		(weave_uint64) blk->nwarp)
		return -1;
	bits = bits_from_nlevels(blk->lut->nlevels);
	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		return -1;
	return bits;
}

static inline void
fill_never(float *out, int from, int to)
{
	int			s;

	for (s = from; s < to; s++)
		out[s] = WEAVE_KERNEL_NEVER;
}

/* ---------------------------------------------------------------------------
 * The oracle
 * ------------------------------------------------------------------------- */

/*
 * Scalar reference implementation of a block score.
 *
 * Computes, for each live lane s of the block, scale_s * sum_j lut[j][code_s[j]],
 * writing WEAVE_SCORE_NEVER for lanes that are dead or masked out so the
 * caller's indexing stays positional.
 *
 * `allow` is a warp-indexed bitmap of blk->nwarp warps, or NULL.  A masked lane
 * must be SKIPPED, not scored and discarded: skipping is the mechanism by which a
 * selective predicate makes this channel faster rather than slower
 * (doc/specs/VECTOR_CHANNEL.md sect. 9).
 *
 * SKIP GRANULARITY: one lane.  The oracle unpacks lane by lane, so it can skip
 * lane by lane, and it is therefore the strictest possible statement of what the
 * masking is allowed to save.  The fast paths below skip in groups of 8 and are
 * consequently allowed to do arithmetic for a masked lane whose group has any
 * live-and-allowed member; that difference is invisible in the OUTPUT (the
 * sentinel is written either way, which is what the differential test compares)
 * and visible only in the work done, which is why it is written down at each
 * path rather than left to be inferred.
 *
 * It reaches the codes ONLY through weave_unpack_lane() and scores them only
 * through weave_lut_score_code(), which is what makes it a trustworthy oracle:
 * it knows nothing about either pack layout's strides, so it cannot share a
 * layout mistake with the kernels below (which do assume the layout), and it
 * keeps working unchanged if a third layout is ever added.  The cost is a
 * bit-at-a-time unpack per lane, which is why it is the reference and not the
 * production path.
 */
static int
weave_score_block_scalar(const WeaveScoreBlock *blk, float *out)
{
	weave_uint8 code[WEAVE_CODE_MAX_BYTES];
	weave_uint32 avail;
	int			bits = block_bits(blk);
	int			s;

	if (bits < 0)
		return -1;

	avail = lane_avail_mask(blk);
	if (avail == 0)
	{
		/* No live-and-allowed lane: not one code byte is read.  (C5) still
		 * requires the positional sentinels. */
		fill_never(out, 0, blk->nlanes);
		return blk->nlanes;
	}

	for (s = 0; s < blk->nlanes; s++)
	{
		if ((avail & (1u << s)) == 0)
		{
			out[s] = WEAVE_KERNEL_NEVER;
			continue;
		}
		weave_unpack_lane(blk->layout, blk->lut->dim, bits, blk->codes, s, code);
		out[s] = weave_lut_score_code(blk->lut, bits, code,
									  blk->scales[(size_t) s * blk->scalestride]);
	}
	return blk->nlanes;
}

const WeaveScoreKernel weave_score_kernel_scalar = {
	.name = "scalar",
	.score_block = weave_score_block_scalar,
};

/* ---------------------------------------------------------------------------
 * WEAVE_PACK_LANE group addressing, shared by every fast path
 *
 * pack.c defines the LANE layout as: code (coordinate j, lane s) occupies bits
 * [(j * 32 + s) * bits, ... + bits), least significant bit first.  Two
 * consequences the fast paths lean on:
 *
 *	 - One coordinate's 32 codes are 4 * bits contiguous bytes.
 *	 - A group of 8 consecutive lanes is exactly `bits` whole bytes, starting at
 *	   byte j * 4 * bits + g * bits.  Byte-aligned at 2, 3 and 4 bits alike,
 *	   which is why the group width is 8 lanes rather than a register width: at
 *	   3 bits, 4 lanes would start mid-byte.
 *
 * This is a duplicate of knowledge that lives in pack.c, which is the price of a
 * fast path.  It is not an unchecked duplicate: test/hegel/test_kernels.c builds
 * every block through weave_pack_lane() and compares against the oracle, which
 * reads it back through weave_unpack_lane(), so a layout change that this file
 * failed to follow fails the test rather than silently returning wrong
 * distances.
 *
 * AND THE CONSEQUENCE FOR FILTERING, which is worth stating explicitly because
 * it is the mechanism doc/specs/VECTOR_CHANNEL.md sect. 9 -- and the project
 * thesis -- rests on: the group is also the unit of SKIPPING.  These paths test
 * the 8 avail bits of a group and skip the whole group when none is set; the
 * oracle tests one lane at a time.  So a 1-in-8-selective filter saves the
 * oracle 7/8 of the scoring work and may save these paths nothing at all, while
 * a filter selective enough to empty whole groups (or whole blocks, which is the
 * case the shuttle short-circuits before it gets here) saves them the same
 * proportion.  Finer masking inside a group is possible -- gather anyway, then
 * blend -- but it costs the same gathers, so the work saved would be the adds
 * only.  Nothing here is measured; bench/kernels.c is owed (doc/PHASES.md V6)
 * and it is the thing that should decide whether per-lane masking inside a group
 * is worth writing.  The output is identical either way, which is why the
 * differential test cannot answer this question and why this note exists.
 * ------------------------------------------------------------------------- */

#define LANE_ROW_BYTES(bits)	((size_t) (WEAVE_VEC_BLOCK * (bits)) / 8)

/*
 * The `bits` bytes holding the 8 codes of lanes [8g, 8g+8) for coordinate j,
 * as a little-endian word.  Reads exactly `bits` bytes: a wider load would run
 * past the end of the block on the last coordinate of the last group at 2 and
 * 3 bits, which ASan reports and a page-boundary read would eventually turn into
 * a segfault.
 */
static inline weave_uint32
group_word(const weave_uint8 *codes, int j, int g, int bits)
{
	const weave_uint8 *p = codes + (size_t) j * LANE_ROW_BYTES(bits) +
		(size_t) g * bits;
	weave_uint32 w = (weave_uint32) p[0] | ((weave_uint32) p[1] << 8);

	if (bits >= 3)
		w |= (weave_uint32) p[2] << 16;
	if (bits >= 4)
		w |= (weave_uint32) p[3] << 24;
	return w;
}

/*
 * Emit one group of 8 lanes from its double accumulators.
 *
 * The final (float) (acc * (double) scale) is exactly what
 * weave_lut_score_code() does, in the same order, which is the last link in the
 * bit-identity argument.
 */
static inline void
group_store(const WeaveScoreBlock *blk, int g, weave_uint32 avail,
			const double *acc, float *out)
{
	int			i;

	for (i = 0; i < 8; i++)
	{
		int			s = g * 8 + i;

		if (s >= blk->nlanes)
			return;
		if ((avail & (1u << s)) == 0)
			out[s] = WEAVE_KERNEL_NEVER;
		else
			out[s] = (float) (acc[i] *
							  (double) blk->scales[(size_t) s * blk->scalestride]);
	}
}

/* ---------------------------------------------------------------------------
 * lut-wide: portable wide-word float-LUT gather
 *
 * One 2-4 byte load per coordinate per 8 lanes replaces the oracle's
 * bit-at-a-time unpack, and the eight accumulators are independent so the
 * out-of-order engine has eight chains to work on.  No ISA, so this is the path
 * every aarch64 and pre-AVX2 x86 host takes, and the baseline any vector path
 * has to beat.  bench/kernels.c (owed, V6) is where that is settled; nothing in
 * this file claims it.
 * ------------------------------------------------------------------------- */

static inline void
wide_group(const WeaveQueryLut *lut, const weave_uint8 *codes, int g,
		   const int bits, double *acc)
{
	const weave_uint32 mask = (weave_uint32) ((1u << bits) - 1);
	const int	nlev = lut->nlevels;
	const int	dim = lut->dim;
	int			j;

	for (j = 0; j < dim; j++)
	{
		weave_uint32 w = group_word(codes, j, g, bits);
		const float *row = lut->lut + (size_t) j * nlev;

		acc[0] += row[w & mask];
		acc[1] += row[(w >> bits) & mask];
		acc[2] += row[(w >> (2 * bits)) & mask];
		acc[3] += row[(w >> (3 * bits)) & mask];
		acc[4] += row[(w >> (4 * bits)) & mask];
		acc[5] += row[(w >> (5 * bits)) & mask];
		acc[6] += row[(w >> (6 * bits)) & mask];
		acc[7] += row[(w >> (7 * bits)) & mask];
	}
}

static int
weave_score_block_wide(const WeaveScoreBlock *blk, float *out)
{
	weave_uint32 avail;
	int			bits = block_bits(blk);
	int			g;

	if (bits < 0)
		return -1;

	/*
	 * DECLINE, rather than assume.  VECMAJOR packs a lane's coordinates
	 * contiguously and, when dim * bits is not a multiple of 8, starts each lane
	 * mid-byte.  The wide and vector paths are built on the LANE layout's
	 * byte-aligned lane groups, so a VECMAJOR block goes to the oracle -- which
	 * reaches codes only through the pack API and so handles either layout --
	 * rather than to a second, less-tested addressing scheme here.  int8-dot
	 * kernels are what VECMAJOR is for, and none exists.
	 */
	if (blk->layout != WEAVE_PACK_LANE)
		return weave_score_block_scalar(blk, out);

	avail = lane_avail_mask(blk);
	if (avail == 0)
	{
		fill_never(out, 0, blk->nlanes);
		return blk->nlanes;
	}

	for (g = 0; g < WEAVE_VEC_BLOCK / 8; g++)
	{
		double		acc[8];
		int			i;

		if (g * 8 >= blk->nlanes)
			break;
		if (((avail >> (g * 8)) & 0xFFu) == 0)
		{
			/* Whole group masked out or dead: skipped, not scored and discarded.
			 * 8 LANES is this path's skip granularity, versus the oracle's 1 --
			 * see the group-addressing note above for why, and for what that
			 * costs a filter that is selective but scattered. */
			fill_never(out, g * 8,
					   (g * 8 + 8 < blk->nlanes) ? g * 8 + 8 : blk->nlanes);
			continue;
		}

		for (i = 0; i < 8; i++)
			acc[i] = 0.0;

		/* Constant `bits` per instantiation: the shift amounts and the mask fold
		 * into immediates, which is the whole point of specializing. */
		switch (bits)
		{
			case 2:
				wide_group(blk->lut, blk->codes, g, 2, acc);
				break;
			case 3:
				wide_group(blk->lut, blk->codes, g, 3, acc);
				break;
			default:
				wide_group(blk->lut, blk->codes, g, 4, acc);
				break;
		}

		group_store(blk, g, avail, acc, out);
	}
	return blk->nlanes;
}

static const WeaveScoreKernel weave_score_kernel_wide = {
	.name = "lut-wide",
	.score_block = weave_score_block_wide,
};

/* ---------------------------------------------------------------------------
 * lut-avx2: AVX2 float-LUT gather
 *
 * Per coordinate, per group of 8 lanes: one 2-4 byte load, one broadcast, one
 * variable shift (vpsrlvd), one mask, one vpgatherdps, two vcvtps2pd, two
 * vaddpd.  The gather is the reason this needs AVX2 and not SSE2 -- with an
 * exact float table there is nothing else to do about 8 independent lookups --
 * and vpsrlvd is the reason the code extraction is 3 instructions instead of 16.
 *
 * The accumulators stay in double and each lane's sum stays in ascending j
 * order, so the result is bit-identical to the oracle rather than merely close;
 * weave/kernels.h explains why that is worth insisting on.
 *
 * Note what is NOT here: the perm0 lane interleave weave/quantize.h mentions for
 * the LANE layout on x86.  That interleave exists so a VPSHUFB byte-LUT gather
 * can cross the 128-bit lane boundary in one instruction.  A vpgatherdps kernel
 * indexes lanes directly and needs no relabelling, so introducing one here would
 * only create an opportunity to permute the output.
 * ------------------------------------------------------------------------- */

#ifdef WEAVE_KERNEL_X86_GNUC

static inline int
host_has_avx2(void)
{
	return __builtin_cpu_supports("avx2") ? 1 : 0;
}

__attribute__((target("avx2")))
static inline void
avx2_group(const WeaveQueryLut *lut, const weave_uint8 *codes, int g,
		   const int bits, double *acc)
{
	const __m256i shifts = _mm256_setr_epi32(0, bits, 2 * bits, 3 * bits,
											 4 * bits, 5 * bits, 6 * bits,
											 7 * bits);
	const __m256i mask = _mm256_set1_epi32((int) ((1u << bits) - 1));
	const int	nlev = lut->nlevels;
	const int	dim = lut->dim;
	__m256d		a0 = _mm256_loadu_pd(acc);
	__m256d		a1 = _mm256_loadu_pd(acc + 4);
	int			j;

	for (j = 0; j < dim; j++)
	{
		weave_uint32 w = group_word(codes, j, g, bits);
		const float *row = lut->lut + (size_t) j * nlev;
		__m256i		idx = _mm256_and_si256(_mm256_srlv_epi32(_mm256_set1_epi32((int) w),
															shifts), mask);
		__m256		v = _mm256_i32gather_ps(row, idx, 4);

		a0 = _mm256_add_pd(a0, _mm256_cvtps_pd(_mm256_castps256_ps128(v)));
		a1 = _mm256_add_pd(a1, _mm256_cvtps_pd(_mm256_extractf128_ps(v, 1)));
	}

	_mm256_storeu_pd(acc, a0);
	_mm256_storeu_pd(acc + 4, a1);
}

__attribute__((target("avx2")))
static int
weave_score_block_avx2(const WeaveScoreBlock *blk, float *out)
{
	weave_uint32 avail;
	int			bits = block_bits(blk);
	int			g;

	if (bits < 0)
		return -1;

	/* Declined explicitly, for the reason spelled out in
	 * weave_score_block_wide(): this path assumes the LANE layout's byte-aligned
	 * 8-lane groups, and the oracle is the one that reads either layout. */
	if (blk->layout != WEAVE_PACK_LANE)
		return weave_score_block_scalar(blk, out);

	avail = lane_avail_mask(blk);
	if (avail == 0)
	{
		fill_never(out, 0, blk->nlanes);
		return blk->nlanes;
	}

	for (g = 0; g < WEAVE_VEC_BLOCK / 8; g++)
	{
		double		acc[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

		if (g * 8 >= blk->nlanes)
			break;
		if (((avail >> (g * 8)) & 0xFFu) == 0)
		{
			/* Skipped at 8-lane granularity, as in lut-wide: no gather is issued
			 * for a group with no live-and-allowed lane. */
			fill_never(out, g * 8,
					   (g * 8 + 8 < blk->nlanes) ? g * 8 + 8 : blk->nlanes);
			continue;
		}

		switch (bits)
		{
			case 2:
				avx2_group(blk->lut, blk->codes, g, 2, acc);
				break;
			case 3:
				avx2_group(blk->lut, blk->codes, g, 3, acc);
				break;
			default:
				avx2_group(blk->lut, blk->codes, g, 4, acc);
				break;
		}

		group_store(blk, g, avail, acc, out);
	}
	return blk->nlanes;
}

static const WeaveScoreKernel weave_score_kernel_avx2 = {
	.name = "lut-avx2",
	.score_block = weave_score_block_avx2,
};

#endif							/* WEAVE_KERNEL_X86_GNUC */

/* ---------------------------------------------------------------------------
 * The registry
 *
 * Best first, scalar last.  Only paths that have passed
 * test/hegel/test_kernels.c on a host that can run them appear here at all: a
 * path dispatch can pick but the differential test never exercised is the
 * fast-but-wrong shape AGENTS.md rule 8 is about, and leaving it out of the
 * table is a stronger guarantee than leaving it in and hoping.
 *
 * "Best" here means widest ISA, which is a convention borrowed from core, NOT a
 * measurement -- and for an exact float-LUT gather it is a weaker assumption
 * than usual, because the work is gathers rather than arithmetic.  bench/kernels.c
 * is owed (doc/PHASES.md V6) and pg_weave.vec_kernel exists so a host can be
 * A/B'd and a bug report reproduced without a rebuild.
 * ------------------------------------------------------------------------- */

typedef struct WeaveScoreKernelReg
{
	const WeaveScoreKernel *k;
	int			(*available) (void); /* NULL = always */
} WeaveScoreKernelReg;

static const WeaveScoreKernelReg kernel_registry[] = {
#ifdef WEAVE_KERNEL_X86_GNUC
	{&weave_score_kernel_avx2, host_has_avx2},
#endif
	{&weave_score_kernel_wide, NULL},
	{&weave_score_kernel_scalar, NULL},
};

#define KERNEL_REGISTRY_LEN \
	((int) (sizeof(kernel_registry) / sizeof(kernel_registry[0])))

int
weave_score_kernel_list(const WeaveScoreKernel **out, int max)
{
	int			n = 0;
	int			i;

	for (i = 0; i < KERNEL_REGISTRY_LEN && n < max; i++)
	{
		if (kernel_registry[i].available != NULL &&
			!kernel_registry[i].available())
			continue;
		out[n++] = kernel_registry[i].k;
	}
	return n;
}

const WeaveScoreKernel *
weave_score_kernel_best(void)
{
	int			i;

	for (i = 0; i < KERNEL_REGISTRY_LEN; i++)
	{
		if (kernel_registry[i].available != NULL &&
			!kernel_registry[i].available())
			continue;
		return kernel_registry[i].k;
	}
	return &weave_score_kernel_scalar;	/* unreachable: scalar is unconditional */
}

const WeaveScoreKernel *
weave_score_kernel_lookup(const char *name)
{
	int			i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < KERNEL_REGISTRY_LEN; i++)
	{
		if (strcmp(kernel_registry[i].k->name, name) != 0)
			continue;
		if (kernel_registry[i].available != NULL &&
			!kernel_registry[i].available())
			return NULL;		/* known kernel, not runnable here */
		return kernel_registry[i].k;
	}
	return NULL;
}

int
weave_score_block(const WeaveScoreBlock *blk, float *out)
{
	return weave_score_kernel_best()->score_block(blk, out);
}
