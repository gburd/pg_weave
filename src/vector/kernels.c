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
 * WHY THE ISA MATRIX IS STILL SHORTER THAN THE SPEC'S.  doc/specs/VECTOR_CHANNEL.md
 * sect. 8 tabulates SSE2 / AVX2 / AVX-512BW / VNNI / NEON / SDOT, all of them
 * byte-LUT or int8-dot strategies.  Both families quantize the float query table
 * to 8 bits before gathering, which is an approximation, not a rounding
 * difference -- so none of them can pass V6's stated gate ("results identical to
 * the scalar path").  Held to exactness, scoring is GATHER-bound: 32 independent
 * table lookups per coordinate.  SSE2 has no gather and no variable shift, so an
 * exact SSE2 kernel is the portable wide-word kernel below plus register
 * shuffling; the same is true of baseline NEON.  AVX2 is the first x86 ISA with
 * both (vpgatherdd, vpsrlvd), which is why it is the one EXACT vector path here.
 * That is the same shape of conclusion pg_turbovec reached for its Hamming kernel
 * (sect. 8: they measured AVX2 and declined it because the wide-word scalar form
 * already extracted the available ILP), and it is why "lut-wide" exists as a
 * first-class kernel rather than as an afterthought -- and, measured at 960-d, why
 * it is now the head of the registry.
 *
 * TASK V16 ADDED THE BYTE-LUT FAMILY ANYWAY, under a different gate.  lut-byte-ref
 * and lut-byte below are approximate by construction and say so
 * (WeaveScoreKernel.approximate), so they are gated against EACH OTHER for
 * bit-identity and against the oracle only for a REPORTED deviation, and
 * weave_score_kernel_best() refuses to hand one out.  V6's exactness gate is
 * therefore intact and unweakened; what changed is that an approximate kernel is
 * now allowed to exist next to it, fenced.  The int8-dot family and any NEON
 * byte-LUT path are still absent.  See doc/PHASES.md V6 and V16.
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
	int			bits;

	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		if (nlevels == (1 << bits))
			return bits;
	}
	return -1;
}

/*
 * Widest code the group-gather fast paths can address.
 *
 * This is a STRUCTURAL limit of the addressing scheme, not a validation
 * boundary, so it is a separate constant from WEAVE_BITS_MAX and does not move
 * when that one does.  group_word() reads the `bits` bytes that hold the codes of
 * EIGHT lanes for one coordinate into a weave_uint32 and wide_group()/avx2_group()
 * shift them out at 8 * bits; 8 * 4 == 32, so 4 bits exactly fills the word and
 * 5 does not fit.  Supporting wider codes there means a 64-bit gather word and a
 * second set of shift constants -- a real kernel change with its own
 * bit-identity argument to make -- not a bumped constant.
 *
 * Until that exists, 5-8 bit blocks go to weave_score_block_scalar(), which
 * reaches codes only through the pack API and is width-agnostic.  That is a
 * throughput decision with no correctness content: the scalar path IS the oracle
 * the fast paths are required to match, so falling back to it cannot change an
 * answer.  See doc/PHASES.md V6.
 */
#define KERNEL_GROUP_BITS_MAX	4

/*
 * The 32 `allow` bits covering this block, as a lane-indexed mask, plus the
 * dead-lane and short-block trims.
 *
 * The arithmetic itself is weave_lane_avail_mask() in weave/kernels.h, because
 * the code-scan decision core (weave/vecscan.h) performs the same test one step
 * earlier and there must be exactly one copy of it; the reasons are stated there.
 * This is the projection from a WeaveScoreBlock onto its four arguments, kept so
 * the five call sites below read as they did.
 */
static inline weave_uint32
lane_avail_mask(const WeaveScoreBlock *blk)
{
	return weave_lane_avail_mask(blk->livemask, blk->nlanes, blk->allow,
								 blk->firstwarp);
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
	.approximate = 0,			/* it IS the definition of exact here */
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
	 *
	 * Codes wider than KERNEL_GROUP_BITS_MAX are declined the same way and for a
	 * reason of the same kind: the gather word is 32 bits and holds 8 lanes, so
	 * it runs out at 4 bits per lane.  See the comment on that constant.
	 */
	if (blk->layout != WEAVE_PACK_LANE || bits > KERNEL_GROUP_BITS_MAX)
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
	.approximate = 0,
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

	/* Declined explicitly, for the two reasons spelled out in
	 * weave_score_block_wide(): this path assumes the LANE layout's byte-aligned
	 * 8-lane groups, and its gather word holds only KERNEL_GROUP_BITS_MAX bits per
	 * lane.  The oracle reads either layout at any width. */
	if (blk->layout != WEAVE_PACK_LANE || bits > KERNEL_GROUP_BITS_MAX)
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
	.approximate = 0,
};

#endif							/* WEAVE_KERNEL_X86_GNUC */

/* ---------------------------------------------------------------------------
 * The byte-LUT family: lut-byte-ref (scalar) and lut-byte (AVX2 vpshufb)
 *
 * Task V16.  These are the first APPROXIMATE kernels in this file: they gather
 * from WeaveQueryLut.lut8, an 8-bit quantization of the float table, so a lane's
 * score carries one rounding residual per coordinate instead of none.  The
 * quantization, and the argument that it is rank-preserving up to that residual,
 * are at WeaveQueryLut.lut8 in weave/quantize.h.  What is enforced HERE is the
 * fencing: `approximate` is set, so weave_score_kernel_best() will not return
 * either of them and `auto` cannot select one (see
 * WeaveScoreKernel.approximate).
 *
 * THEY REFUSE RATHER THAN DELEGATE.  Both return -1 for anything but 4 bits in
 * WEAVE_PACK_LANE, where lut-wide and lut-avx2 would fall back to the oracle.
 * The difference is deliberate.  A fallback inside an approximate kernel would
 * make it report the ORACLE's numbers -- both its scores and its speed -- under
 * this kernel's name, and a fast-but-wrong result published under the wrong name
 * is the exact failure AGENTS.md rule 8 was written about.  -1 is already the
 * contract for a block description this path cannot honour (see
 * weave_score_block_fn in weave/kernels.h) and the backend adapter turns it into
 * a clean ERROR.
 * ------------------------------------------------------------------------- */

/*
 * Accept only what the byte table can score exactly as specified.
 *
 * Everything block_bits() rejects is rejected here too, plus: 4 bits exactly
 * (equivalently nlevels == 16, which is the only width weave_query_lut_build()
 * builds a byte table for, and the only one a 16-entry vpshufb table can hold);
 * WEAVE_PACK_LANE only; and a lut8 that is actually present, which is the same
 * condition seen from the table's side rather than the width's.  Checking both
 * is not redundant: `nlevels` comes off a page and `lut8` comes from the query
 * side, and a mismatch between them means one of the two is not what this kernel
 * was handed.
 */
static inline int
byte_block_ok(const WeaveScoreBlock *blk)
{
	if (block_bits(blk) != 4)
		return 0;
	if (blk->layout != WEAVE_PACK_LANE)
		return 0;
	if (blk->lut->lut8 == NULL)
		return 0;
	return 1;
}

/*
 * The one place a lane's integer accumulator becomes a float.
 *
 * BOTH byte kernels call this, so their outputs are bit-identical by construction
 * rather than by two expressions happening to agree -- which is what makes the
 * differential assertion between them (test/hegel/test_kernels.c K6) a statement
 * about the SIMD addressing and nothing else.  The trailing multiply by the
 * lane's renormalization scale in double, then one cast to float, is what
 * weave_lut_score_code() and group_store() above do.
 */
static inline float
byte_lane_score(const WeaveQueryLut *lut, weave_uint32 acc, float scale)
{
	return (float) (((double) lut->lut8_step * (double) acc +
					 (double) lut->lut8_offset) * (double) scale);
}

/*
 * Scalar reference for the byte table.
 *
 * It exists to separate the two things a byte-LUT kernel can get wrong.  Any
 * disagreement between this and the exact oracle is the 8-bit table's rounding;
 * any disagreement between this and lut-byte is a SIMD bug.  One number each,
 * instead of one number confounding both.
 *
 * Like the oracle, it reaches codes only through weave_unpack_lane(), so it does
 * NOT share the nibble/128-bit-half addressing assumptions of the AVX2 kernel
 * below.  That independence is the whole value of the comparison.
 *
 * SKIP GRANULARITY: one lane, as the oracle.
 */
static int
weave_score_block_byte_ref(const WeaveScoreBlock *blk, float *out)
{
	weave_uint8 code[WEAVE_CODE_MAX_BYTES];
	const weave_uint8 *tbl;
	weave_uint32 avail;
	int			dim;
	int			s,
				j;

	if (!byte_block_ok(blk))
		return -1;

	dim = blk->lut->dim;
	tbl = blk->lut->lut8;
	avail = lane_avail_mask(blk);
	if (avail == 0)
	{
		fill_never(out, 0, blk->nlanes);
		return blk->nlanes;
	}

	for (s = 0; s < blk->nlanes; s++)
	{
		weave_uint32 acc = 0;

		if ((avail & (1u << s)) == 0)
		{
			out[s] = WEAVE_KERNEL_NEVER;
			continue;
		}
		weave_unpack_lane(blk->layout, dim, 4, blk->codes, s, code);
		for (j = 0; j < dim; j++)
		{
			/* bits == 4: coordinate j of one vector's code is nibble j, low
			 * nibble first (src/vector/pack.c put_bits writes LSB first). */
			weave_uint32 cix = (j & 1) ? (weave_uint32) (code[j >> 1] >> 4)
				: (weave_uint32) (code[j >> 1] & 0x0F);

			acc += tbl[(size_t) j * 16 + cix];
		}

		/*
		 * 32 bits is not a budget, it is headroom: 255 * WEAVE_MAX_DIM = 4.2e6.
		 * The AVX2 path below is the one that has to work at it, because its
		 * accumulators start out 16 bits wide.
		 */
		out[s] = byte_lane_score(blk->lut, acc,
								 blk->scales[(size_t) s * blk->scalestride]);
	}
	return blk->nlanes;
}

static const WeaveScoreKernel weave_score_kernel_byte_ref = {
	.name = "lut-byte-ref",
	.score_block = weave_score_block_byte_ref,
	.approximate = 1,
};

/* ---------------------------------------------------------------------------
 * lut-byte: AVX2 vpshufb byte-LUT gather
 *
 * THE ADDRESSING, spelled out because a wrong permutation here produces
 * plausible-but-wrong scores and nothing else would catch it.
 *
 * At bits == 4 in WEAVE_PACK_LANE, code (coordinate j, lane s) is at bit
 * (j * 32 + s) * 4, so coordinate j's 32 codes are the 16 CONTIGUOUS bytes at
 * offset j * 16, and byte b of those holds lane 2b in its LOW nibble and lane
 * 2b + 1 in its HIGH nibble.
 *
 * _mm256_shuffle_epi8 indexes within each 128-bit half INDEPENDENTLY, and a
 * 16-entry byte table is exactly one half.  So rather than broadcasting one
 * coordinate's table into both halves and wasting half the register on duplicate
 * work, this processes coordinates IN PAIRS: one 32-byte load covers coordinates
 * j and j+1, one 32-byte table load covers rows j and j+1, and each half of the
 * shuffle uses its OWN coordinate's table.  Byte B of the result therefore
 * belongs to coordinate j + B/16 and lane 2 * (B mod 16), + 1 for the high-nibble
 * shuffle.
 *
 * OVERFLOW IS THE TRAP.  Gathered values are bytes 0..255 and dim reaches 960 in
 * the corpora this is measured on, so 255 * 960 = 244800 does not fit the 16-bit
 * lanes the byte widening naturally lands in.  Each 16-bit slot receives ONE byte
 * per pair-iteration, so 255 * 257 is the true ceiling; this widens into 32-bit
 * accumulators every LUT8_FLUSH_PAIRS = 128 pair-iterations, i.e. every 256
 * coordinates (255 * 256 = 65280 < 65536).  Saturating adds are NOT used to paper
 * over this: saturation silently changes a score, and a silently changed score is
 * indistinguishable from a working kernel.
 *
 * SKIP GRANULARITY: the whole block.  Coarser than lut-wide's 8 lanes and
 * necessarily so -- one coordinate's 16 code bytes cover all 32 lanes, so there
 * is no 8-lane subset this path could decline to load.  A block with no
 * live-and-allowed lane is skipped entirely and touches no code byte; anything
 * else scores all 32 lanes and writes the sentinel over the masked ones.
 *
 * NO perm0 lane interleave, as in weave_score_block_avx2(): the 128-bit-half
 * bookkeeping vpshufb forces is dealt with by an explicit index map at flush time
 * (lut8_flush) instead, which is readable on the page.
 * ------------------------------------------------------------------------- */

#ifdef WEAVE_KERNEL_X86_GNUC

/* 255 * 256 = 65280 < 65536; see the overflow note above. */
#define LUT8_FLUSH_PAIRS	128

/*
 * Fold four vectors of 16-bit lane accumulators into 32 lane-indexed 32-bit
 * accumulators.  THIS is the un-permutation, written out once per flush rather
 * than as a chain of shuffles, because being able to read the index map off the
 * page is worth more than the instructions it costs (one flush per 256
 * coordinates, against 8192 table lookups).
 *
 * Element k of a 16 x u16 vector is element k & 7 of 128-bit half k >> 3.  Half 0
 * carries coordinate j and half 1 carries coordinate j+1 -- two different
 * coordinates' contributions to the SAME lane -- so both halves add into the same
 * acc32 slot, which is why the map ignores k >> 3:
 *
 *	 e0[k] -> lane 2*(k&7)			(low nibble, bytes 0-7	 of the half)
 *	 e1[k] -> lane 2*(k&7) + 16		(low nibble, bytes 8-15  of the half)
 *	 o0[k] -> lane 2*(k&7) + 1		(high nibble, bytes 0-7)
 *	 o1[k] -> lane 2*(k&7) + 17		(high nibble, bytes 8-15)
 */
__attribute__((target("avx2")))
static inline void
lut8_flush(__m256i e0, __m256i e1, __m256i o0, __m256i o1, weave_uint32 *acc32)
{
	weave_uint16 t[4][16];
	int			k;

	_mm256_storeu_si256((__m256i *) t[0], e0);
	_mm256_storeu_si256((__m256i *) t[1], e1);
	_mm256_storeu_si256((__m256i *) t[2], o0);
	_mm256_storeu_si256((__m256i *) t[3], o1);

	for (k = 0; k < 16; k++)
	{
		int			i = k & 7;

		acc32[2 * i] += t[0][k];
		acc32[2 * i + 16] += t[1][k];
		acc32[2 * i + 1] += t[2][k];
		acc32[2 * i + 17] += t[3][k];
	}
}

__attribute__((target("avx2")))
static int
weave_score_block_byte_avx2(const WeaveScoreBlock *blk, float *out)
{
	const __m256i nib = _mm256_set1_epi8(0x0F);
	const __m256i zero = _mm256_setzero_si256();
	weave_uint32 acc32[WEAVE_VEC_BLOCK];
	weave_uint32 avail;
	__m256i		e0,
				e1,
				o0,
				o1;
	const weave_uint8 *codes;
	const weave_uint8 *tbl;
	int			dim;
	int			j,
				s,
				pairs;

	if (!byte_block_ok(blk))
		return -1;

	avail = lane_avail_mask(blk);
	if (avail == 0)
	{
		fill_never(out, 0, blk->nlanes);
		return blk->nlanes;
	}

	dim = blk->lut->dim;
	codes = blk->codes;
	tbl = blk->lut->lut8;
	memset(acc32, 0, sizeof(acc32));
	e0 = e1 = o0 = o1 = zero;
	pairs = 0;

	for (j = 0; j + 1 < dim; j += 2)
	{
		__m256i		cv = _mm256_loadu_si256((const __m256i *) (codes + (size_t) j * 16));
		__m256i		tv = _mm256_loadu_si256((const __m256i *) (tbl + (size_t) j * 16));
		__m256i		lo = _mm256_and_si256(cv, nib);
		__m256i		hi = _mm256_and_si256(_mm256_srli_epi16(cv, 4), nib);
		__m256i		vlo = _mm256_shuffle_epi8(tv, lo);
		__m256i		vhi = _mm256_shuffle_epi8(tv, hi);

		e0 = _mm256_add_epi16(e0, _mm256_unpacklo_epi8(vlo, zero));
		e1 = _mm256_add_epi16(e1, _mm256_unpackhi_epi8(vlo, zero));
		o0 = _mm256_add_epi16(o0, _mm256_unpacklo_epi8(vhi, zero));
		o1 = _mm256_add_epi16(o1, _mm256_unpackhi_epi8(vhi, zero));

		if (++pairs == LUT8_FLUSH_PAIRS)
		{
			lut8_flush(e0, e1, o0, o1, acc32);
			e0 = e1 = o0 = o1 = zero;
			pairs = 0;
		}
	}
	if (pairs > 0)
		lut8_flush(e0, e1, o0, o1, acc32);

	/*
	 * Odd dim: the last coordinate has no partner.  Done scalar rather than with
	 * a 128-bit load because weave_block_codebytes() rounds a 4-bit block up to
	 * more than 16 * dim bytes when dim is odd, so a vector load here would reach
	 * into the slack tail -- in bounds, but never written by any pack function
	 * (weave/quantize.h), which is a valgrind report and a reader's doubt for no
	 * gain on one coordinate out of dim.  The byte table has no such tail either:
	 * it is exactly dim * 16 bytes.
	 */
	if (j < dim)
	{
		const weave_uint8 *p = codes + (size_t) j * 16;
		const weave_uint8 *row = tbl + (size_t) j * 16;
		int			b;

		for (b = 0; b < 16; b++)
		{
			acc32[2 * b] += row[p[b] & 0x0F];
			acc32[2 * b + 1] += row[p[b] >> 4];
		}
	}

	for (s = 0; s < blk->nlanes; s++)
	{
		if ((avail & (1u << s)) == 0)
			out[s] = WEAVE_KERNEL_NEVER;
		else
			out[s] = byte_lane_score(blk->lut, acc32[s],
									 blk->scales[(size_t) s * blk->scalestride]);
	}
	return blk->nlanes;
}

static const WeaveScoreKernel weave_score_kernel_byte = {
	.name = "lut-byte",
	.score_block = weave_score_block_byte_avx2,
	.approximate = 1,
};

#endif							/* WEAVE_KERNEL_X86_GNUC */

/* ---------------------------------------------------------------------------
 * The registry
 *
 * Exact paths best first, then the approximate ones, and scalar always last.
 * Only paths that have passed test/hegel/test_kernels.c on a host that can run
 * them appear here at all: a path dispatch can pick but the differential test
 * never exercised is the fast-but-wrong shape AGENTS.md rule 8 is about, and
 * leaving it out of the table is a stronger guarantee than leaving it in and
 * hoping.
 *
 * "BEST" IS NOW A MEASUREMENT, AT ONE DIMENSION.  This comment used to say best
 * meant widest ISA -- "a convention borrowed from core, NOT a measurement" -- and
 * put lut-avx2 ahead of lut-wide on that basis.  Task V16 measured it on
 * GIST-960d at n = 50k / 200k / 1M, two independent runs on r7i.2xlarge, and
 * lut-wide wins at every point (297.7 vs 418.8 ns/vector at n = 1M).  So the
 * order is reversed and it is no longer a convention.
 *
 * THE CAVEAT, recorded rather than left implicit: that is one dimension.  Whether
 * lut-wide still wins at LOW dim is UNMEASURED -- the gather's setup cost is
 * per-coordinate while its win is per-lane, so the crossover, if there is one,
 * would be at small dim.  Read this as "measured at 960-d", not "measured
 * everywhere".  pg_weave.vec_kernel exists so a host can be A/B'd and a bug
 * report reproduced without a rebuild.
 *
 * The approximate entries sit between the exact SIMD paths and scalar.  Their
 * position is inert for dispatch, because weave_score_kernel_best() skips them
 * outright (WeaveScoreKernel.approximate); they are in the table so that
 * weave_score_kernel_list() reports them and the differential test cannot miss a
 * path a caller could force.  lut-byte is registered ONLY where AVX2 is present
 * and is never aliased to lut-byte-ref: a scalar fallback answering to a SIMD
 * kernel's name is how the sibling project published a headline number it had to
 * retract.
 * ------------------------------------------------------------------------- */

typedef struct WeaveScoreKernelReg
{
	const WeaveScoreKernel *k;
	int			(*available) (void); /* NULL = always */
} WeaveScoreKernelReg;

static const WeaveScoreKernelReg kernel_registry[] = {
	{&weave_score_kernel_wide, NULL},
#ifdef WEAVE_KERNEL_X86_GNUC
	{&weave_score_kernel_avx2, host_has_avx2},
#endif
	{&weave_score_kernel_byte_ref, NULL},
#ifdef WEAVE_KERNEL_X86_GNUC
	{&weave_score_kernel_byte, host_has_avx2},
#endif
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

		/*
		 * APPROXIMATE KERNELS ARE NOT AUTO-SELECTABLE.  Their soundness depends
		 * on an exact rerank downstream and no query path here can guarantee one
		 * yet (V7/V8/V15); see WeaveScoreKernel.approximate for the full
		 * argument.  Skipping them here rather than omitting them from the table
		 * keeps them reachable by name, which is what the A/B and the
		 * differential test need.
		 */
		if (kernel_registry[i].k->approximate)
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
