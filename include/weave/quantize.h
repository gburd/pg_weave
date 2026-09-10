/*-------------------------------------------------------------------------
 *
 * quantize.h
 *		TurboQuant-style scalar quantization core for the weave vector channel.
 *
 * The compression core of the vector wefts, written as pure standalone C (no
 * PostgreSQL includes) so it can be exercised by standalone property tests
 * (test/hegel/) and libFuzzer harnesses (test/fuzz/) while remaining the single
 * source of truth.  Same design intent as weave/for.h: the codec is the part
 * most likely to be subtly wrong, so it is the part that must be testable
 * without a backend.
 *
 * The pipeline, per vector v of dimension d:
 *
 *		1. norm = ||v||,  u = v / norm					unit direction + magnitude
 *		2. x = R(u)										deterministic rotation
 *		3. x' = (x - shift) * cscale					optional TQ+ calibration
 *		4. code[j] = argmin_c |x'[j] - C[c]|			Lloyd-Max scalar quantize
 *		5. pack code[] at `bits` bits per coordinate
 *		6. scale = norm / <x, dequant(code)>			renormalization
 *
 * Step 6 is the whole trick.  Scalar quantization systematically shrinks a
 * vector toward the origin, so <q, dequant(code)> underestimates <q, u>.
 * Storing scale and reconstructing as scale * dequant(code) forces the
 * reconstruction's projection onto the true direction to equal norm exactly,
 * which makes the compressed-domain inner-product estimator unbiased.  An
 * unbiased estimator is what removes the need for a float32 rerank pass at
 * moderate k -- see doc/specs/VECTOR_CHANNEL.md sect. 2.
 *
 * DETERMINISM IS PART OF THE CONTRACT.  Encoding the same vector must produce
 * byte-identical codes on x86-64 and aarch64, under any thread count, forever.
 * That is why the rotation uses only +, -, and a single multiply by a
 * precomputed reciprocal, in a fixed reduction order, with no FMA and no BLAS.
 * turbovec reached this design by abandoning a QR/Householder rotation whose
 * output depended on RAYON_NUM_THREADS and the host libm; that change also
 * deleted a 42 MB OpenBLAS dependency.  Do not "optimize" this file with
 * fused-multiply-add or a reassociated sum.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/quantize.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_QUANTIZE_H
#define WEAVE_QUANTIZE_H

#include <stdint.h>
#include <string.h>

/*
 * PostgreSQL's c.h defines these; only supply them when compiled outside the
 * backend.  Mirrors the guard in weave/for.h.
 */
#ifndef POSTGRES_H
typedef uint64_t weave_uint64;
typedef uint32_t weave_uint32;
typedef uint16_t weave_uint16;
typedef uint8_t weave_uint8;
#else
typedef uint64 weave_uint64;
typedef uint32 weave_uint32;
typedef uint16 weave_uint16;
typedef uint8 weave_uint8;
#endif

/* ---------------------------------------------------------------------------
 * Wire-format constants.  Every one of these is frozen: changing any of them
 * changes the bytes on disk and therefore requires a segment format bump and a
 * dual-read path.  They are collected here so that fact is impossible to miss.
 * ------------------------------------------------------------------------- */

/* Rotation rounds.  Two rounds of (permute, sign-flip, block-Hadamard) are
 * enough to make coordinates behave like independent draws from the sphere
 * marginal for the dimensions we care about (d >= 64); one round leaves visible
 * structure on energy-ordered embeddings. */
#define WEAVE_ROT_ROUNDS		2

/* Minimum Hadamard block.  The transform is applied on contiguous blocks of
 * size B = largest power of two dividing d, clamped to [WEAVE_ROT_MIN_BLOCK,
 * WEAVE_ROT_MAX_BLOCK].  A power-of-two d therefore gets one whole-vector
 * transform; d = 1536 (a common embedding width) gets B = 512. */
#define WEAVE_ROT_MIN_BLOCK		8
#define WEAVE_ROT_MAX_BLOCK		1024

/* The 32-byte ChaCha8 seed that generates the permutation and the sign flips.
 * Frozen.  Any change silently invalidates every index ever built. */
#define WEAVE_ROT_SEED_BYTES	32

/* Supported code widths, in bits per coordinate.  The Lloyd-Max solver is only
 * validated for these; 1 bit is deliberately excluded because at 1 bit the
 * renormalization trick degenerates and RaBitQ-style sign coding with an
 * explicit error term is the better design (not implemented; see
 * doc/specs/VECTOR_CHANNEL.md sect. 11). */
#define WEAVE_BITS_MIN			2
#define WEAVE_BITS_MAX			4

/* Vectors per SIMD block.  The scan kernels process this many lanes at a time
 * and the block bound in weave/channel.h terms covers exactly this range. */
#define WEAVE_VEC_BLOCK			32

/* Hard dimension ceiling.  Bounded so every stack buffer in the codec is a
 * fixed size and no allocation happens on the encode hot path. */
#define WEAVE_MAX_DIM			16384

/* ---------------------------------------------------------------------------
 * Codebook
 *
 * A single coordinate of a uniformly random unit vector in R^d has density
 * proportional to (1 - x^2)^((d-3)/2) on [-1, 1] -- a symmetric Beta with both
 * shape parameters (d-1)/2, affinely mapped onto [-1, 1].  Because the rotation
 * in this file makes the coordinates of ANY input look like that, the optimal
 * scalar quantizer is a pure function of (bits, d) and needs NO training data.
 * That is what "data-oblivious" means here and it is why there is no k-means,
 * no codebook page, and no build-time sample: two indexes over different
 * corpora with the same (bits, d) have bit-identical codebooks.
 *
 * The solver is Lloyd-Max: alternate (centroid = conditional mean of its cell,
 * boundary = midpoint between neighbouring centroids) to convergence, with the
 * conditional means computed by adaptive Simpson quadrature against the Beta
 * density.  Lloyd-Max is optimal for a fixed-rate scalar quantizer, and the
 * literature puts fixed-rate scalar quantization of a smooth source within
 * roughly 2.7x of the Shannon rate-distortion bound.  That factor is cited, not
 * measured by us; see doc/specs/VECTOR_CHANNEL.md sect. 4.
 * ------------------------------------------------------------------------- */

#define WEAVE_MAX_LEVELS		(1 << WEAVE_BITS_MAX)

typedef struct WeaveCodebook
{
	int			bits;			/* 2..4 */
	int			dim;			/* the d the Beta shape was derived from */
	int			nlevels;		/* 1 << bits */

	/* Reconstruction values, ascending.  centroid[0] < ... < centroid[n-1], and
	 * the set is symmetric about zero. */
	float		centroid[WEAVE_MAX_LEVELS];

	/* Decision boundaries: nlevels - 1 of them, boundary[i] separating
	 * centroid[i] from centroid[i+1].  Stored so quantization is a branchless
	 * comparison ladder rather than a distance search. */
	float		boundary[WEAVE_MAX_LEVELS - 1];

	/* max |centroid|.  Used by the block-bound derivation and by the TQ+
	 * calibration anchor. */
	float		absmax;
} WeaveCodebook;

/*
 * Solve (or fetch from the process-local memo) the codebook for (bits, dim).
 * The solve costs 25-100 ms, so it is memoized; the memo is keyed on the pair
 * and is safe to consult from multiple backends because the result is a pure
 * function of the key.
 *
 * Returns 0 on success, -1 if bits or dim is out of range.
 */
extern int	weave_codebook_get(int bits, int dim, WeaveCodebook *out);

/*
 * Solve without consulting the memo.  Exposed only so test/hegel/test_codebook.c
 * can compare a fresh solve against the committed fixture and so the fixture can
 * be regenerated deliberately.
 */
extern int	weave_codebook_solve(int bits, int dim, WeaveCodebook *out);

/* ---------------------------------------------------------------------------
 * Rotation
 *
 * One round is: global Fisher-Yates permutation of all d coordinates, then a
 * per-coordinate sign flip, then a normalized Walsh-Hadamard transform on each
 * contiguous block of B coordinates.  Both the permutation and the signs come
 * from a ChaCha8 stream seeded by WEAVE_ROT_SEED_BYTES fixed bytes, so both are
 * a pure function of d.
 *
 * The permutation MUST come before the Hadamard.  Real embeddings are often
 * energy-ordered -- Matryoshka/MRL models put the highest-variance coordinates
 * first by construction -- so a block-local transform applied to contiguous
 * coordinates would mix correlated high-energy dimensions with each other and
 * leave the tail nearly untouched.  Permuting first spreads that energy across
 * blocks.  This ordering is not an implementation detail; getting it backwards
 * produces an encoder that looks fine on random test vectors and loses recall
 * on exactly the models people use.
 * ------------------------------------------------------------------------- */

typedef struct WeaveRotation
{
	int			dim;
	int			block;			/* B, a power of two in [MIN_BLOCK, MAX_BLOCK] */
	int			nblocks;		/* dim / block; dim must be a multiple of block */
	int			tail;			/* dim - nblocks * block; untransformed remainder */
	float		invsqrtb;		/* 1 / sqrt(block), precomputed once */

	/* perm[i] = source index of output coordinate i, one array per round. */
	weave_uint16 *perm[WEAVE_ROT_ROUNDS];
	/* Packed sign bits, one bit per coordinate per round. */
	weave_uint64 *sign[WEAVE_ROT_ROUNDS];

	void	   *_alloc;			/* single backing allocation, for free() */
} WeaveRotation;

/*
 * Build the rotation for a dimension.  Deterministic: the same dim always
 * yields the same permutation and signs.  Allocation is a single block so a
 * caller in a PostgreSQL memory context can palloc it and forget it.
 *
 * alloc/free are supplied by the caller so this file stays backend-independent:
 * the backend passes palloc/pfree, the tests pass malloc/free.
 */
extern int	weave_rotation_init(WeaveRotation *rot, int dim,
								void *(*alloc) (size_t), void (*dealloc) (void *));
extern void weave_rotation_free(WeaveRotation *rot, void (*dealloc) (void *));

/*
 * Apply the rotation in place to a d-element float array.  Orthogonal, so it
 * preserves the L2 norm to within float rounding.
 *
 * Reference implementation: scalar, fixed reduction order, no FMA.  The SIMD
 * variants in src/vector/kernels.c must produce BIT-IDENTICAL output to this
 * function, and test/hegel/test_rotation.c asserts exactly that against a
 * committed cross-architecture fixture hash.  If a SIMD path is one ULP off, it
 * is wrong, not "close enough": a differing rotation means a differing code
 * means an index that returns different answers depending on which machine
 * inserted the row.
 */
extern void weave_rotate(const WeaveRotation *rot, float *x);

/* Inverse rotation.  Needed only for the query side of L2 reconstruction and
 * for the round-trip property test. */
extern void weave_rotate_inverse(const WeaveRotation *rot, float *x);

/* ---------------------------------------------------------------------------
 * TQ+ calibration (optional)
 *
 * At finite d the empirical coordinate distribution drifts from the asymptotic
 * Beta, most visibly at low d (d = 200 GloVe is the documented worst case in
 * turbovec's benchmarks).  TQ+ fits a per-coordinate affine map (shift, cscale)
 * that carries the empirical high quantile onto the codebook's outermost
 * centroid, then quantizes.  It is a one-shot fit over a sample of >= 1024 rows.
 *
 * HONEST LIMITATION, repeated in the spec: this is a manual, one-shot step with
 * no drift detection.  If the corpus distribution later moves away from the
 * calibration sample, recall degrades silently.  weave_check() should report the
 * calibration sample size and the fit date so an operator can at least see it.
 * ------------------------------------------------------------------------- */

#define WEAVE_CALIB_MIN_ROWS		256
#define WEAVE_CALIB_RECOMMENDED_ROWS 1024

typedef struct WeaveCalibration
{
	int			dim;
	int			nsample;		/* rows the fit was computed from; 0 = identity */
	float	   *shift;			/* dim entries, or NULL for identity */
	float	   *cscale;			/* dim entries, or NULL for identity */
	void	   *_alloc;
} WeaveCalibration;

/*
 * Fit a calibration from nsample already-rotated unit vectors laid out
 * row-major as sample[i * dim + j].  Bounds are enforced so the query-side
 * inverse transform can never divide by something near zero -- an uncalibrated
 * index is far better than one whose dequantization overflows.
 */
extern int	weave_calibration_fit(WeaveCalibration *cal, const WeaveCodebook *cb,
								  const float *sample, int nsample, int dim,
								  void *(*alloc) (size_t));

/* Reject a calibration whose parameters are outside the safe range.  Called on
 * load as well as on fit, because the on-disk bytes are not trusted. */
extern int	weave_calibration_validate(const WeaveCalibration *cal);

/* ---------------------------------------------------------------------------
 * Encode / decode
 * ------------------------------------------------------------------------- */

/*
 * Everything needed to encode or score, gathered so the hot paths take one
 * pointer.  Cheap to construct from (bits, dim) plus an optional calibration.
 */
typedef struct WeaveQuantizer
{
	int			dim;
	int			bits;
	int			codebytes;		/* ceil(dim * bits / 8), per vector */
	WeaveCodebook cb;
	WeaveRotation rot;
	const WeaveCalibration *cal;	/* may be NULL */
} WeaveQuantizer;

extern int	weave_quantizer_init(WeaveQuantizer *q, int dim, int bits,
								 const WeaveCalibration *cal,
								 void *(*alloc) (size_t), void (*dealloc) (void *));
extern void weave_quantizer_free(WeaveQuantizer *q, void (*dealloc) (void *));

/*
 * Encode one vector.
 *
 *	v			input, dim floats, NOT modified
 *	code		output, q->codebytes bytes, bit-packed
 *	out_norm	output, ||v||
 *	out_scale	output, the renormalization scale (step 6 above)
 *
 * Returns 0 on success.  Returns -1 for a zero (or denormal-norm) vector, which
 * the caller must handle explicitly rather than encode: a zero vector has no
 * direction, its scale is undefined, and silently substituting zeros produces a
 * row that matches every query equally well.
 */
extern int	weave_encode(const WeaveQuantizer *q, const float *v,
						 weave_uint8 *code, float *out_norm, float *out_scale);

/*
 * Reconstruct an approximation of the original vector from a code.  Used by the
 * exact-rerank tail, by the round-trip property test, and by weave_check().
 */
extern void weave_decode(const WeaveQuantizer *q, const weave_uint8 *code,
						 float scale, float *out);

/* ---------------------------------------------------------------------------
 * Query-side lookup table and the block bound
 *
 * Scoring never dequantizes.  For inner product,
 *
 *		<q, scale * xhat> = scale * sum_j q_j * C[code_j]
 *
 * so a table lut[j][c] = q_j * C[c] turns scoring into gather-and-add.  The
 * table is built once per query, costs dim * nlevels multiplies, and is the
 * input to both kernel families (byte-LUT and int8-dot).
 *
 * THE BOUND.  Three formulations are available, all provably correct.  Which one
 * to use is not a matter of taste: it was measured, and two of the three are
 * worthless.  See bench/RESULTS_BOUND_PRUNING.md for the harness and the
 * numbers; the summary, at dim=256, 4 bits, k=10, 256 blocks x 32 lanes, 200
 * realistic queries (a query near a real corpus vector, so a meaningful theta
 * exists), measuring the fraction of blocks skipped without scoring:
 *
 *		bound formulation           blocks pruned
 *		------------------------    -------------
 *		(B1) LUT / per-coordinate         0.0 %
 *		(B2) Cauchy-Schwarz               0.2 %
 *		(B3) centroid + radius           99.6 %
 *
 * (B1) LUT bound.  L(q) = sum_j max_c (q_j * C[c]).  Since the codebook is
 * symmetric about zero this is absmax * ||q||_1, and ||q||_1 ~ sqrt(dim) *
 * ||q||_2, so (B1) is loose by a factor of order sqrt(dim) against
 * Cauchy-Schwarz before you even start.  It prunes nothing.  Keep it only
 * because it is free (we already built the table) and it costs one comparison
 * to include in a min().
 *
 * (B2) Cauchy-Schwarz.  <q, r> <= ||q||_2 * ||r||_2, so the bound is
 * maxrecnorm * ||q||_2 where maxrecnorm is the largest reconstruction norm in
 * the block.  Tighter than (B1) by ~2.2x measured, and still prunes nothing:
 * it is tight only when a block member is parallel to the query, and in high
 * dimension nothing is parallel to anything.
 *
 * (B3) Centroid + radius -- THE ONE THAT WORKS.  Let c be the block's centroid
 * and R = max_s ||r_s - c||.  Then for every member s,
 *
 *		<q, r_s> = <q, c> + <q, r_s - c>  <=  <q, c> + ||q||_2 * R
 *
 * <q, c> is computed from the LUT if c is itself stored as a quantized code, so
 * (B3) costs one extra LUT gather per block and 4 bytes for R.  At dim=256, 4
 * bits, that is 128 bytes of centroid per 4096 bytes of codes: 3 % overhead for
 * a 250x improvement in pruning.
 *
 * For L2, using minnorm = min ||v|| over the block:
 *
 *		-||q - v||^2 = -||q||^2 + 2<q,v> - ||v||^2
 *		            <= -||q||^2 + 2 * (<q,c> + ||q||_2 * R) - minnorm^2
 *
 * THE CATCH, AND IT IS LOAD-BEARING.  (B3) works because R is small, and R is
 * small only if the 32 vectors sharing a block are spatially near each other.
 * The same harness with a RANDOMLY ORDERED warp measures:
 *
 *		(B3) with random warp order        0.0 %
 *
 * So warp ordering is not a performance tweak, it is what makes the vector
 * channel prunable at all.  The vector weft MUST assign warp positions in
 * clustered order -- which the Vamana build already computes, since its
 * partitioning step is a k-means.  Task V13 in doc/PHASES.md, and it is a gate,
 * not an optimization.  An implementation that stores codes in heap order will
 * pass every correctness test and then degrade the fused scorer to a full scan.
 * ------------------------------------------------------------------------- */

typedef struct WeaveQueryLut
{
	int			dim;
	int			nlevels;
	float	   *lut;			/* dim * nlevels, row-major by coordinate */
	float		lutbound;		/* L(q), bound (B1).  Nearly useless; kept
								 * because it is free. */
	float		qnorm;			/* ||q||_2, for bounds (B2) and (B3) */
	float		qnorm2;			/* ||q||^2, for the L2 bound */
	void	   *_alloc;
} WeaveQueryLut;

extern int	weave_query_lut_build(WeaveQueryLut *out, const WeaveQuantizer *q,
								  const float *query, void *(*alloc) (size_t));

/*
 * Score a block centroid that is stored as a quantized code.  This is the
 * <q, c> term of bound (B3) and it is the only page-resident data the bound
 * needs beyond the block header, so it satisfies contract (C3): the centroid
 * code lives in the header region the shuttle is already holding.
 */
extern float weave_lut_score_code(const WeaveQueryLut *lut, int bits,
								  const weave_uint8 *code, float scale);

/*
 * The per-block bounds.  Inline because they sit inside the pruning loop.
 *
 * `censcore` is <q, c> from weave_lut_score_code(); `radius` is R.  Pass
 * smax and maxrecnorm too and we take the min of all three formulations -- the
 * weak ones cost one comparison each and occasionally win on a degenerate block
 * (a block with one live lane has R = 0 and (B3) becomes exact, but a block whose
 * centroid code quantizes badly can have (B2) tighter).
 */
static inline float
weave_block_bound_ip(const WeaveQueryLut *lut, float smax, float maxrecnorm,
					 float censcore, float radius)
{
	float		b3 = censcore + lut->qnorm * radius;
	float		b2 = maxrecnorm * lut->qnorm;
	float		b1 = smax * lut->lutbound;
	float		b = b3;

	if (b2 < b)
		b = b2;
	if (b1 < b)
		b = b1;
	return b;
}

static inline float
weave_block_bound_l2(const WeaveQueryLut *lut, float smax, float maxrecnorm,
					 float censcore, float radius, float minnorm)
{
	float		ipb = weave_block_bound_ip(lut, smax, maxrecnorm, censcore, radius);

	return -lut->qnorm2 + 2.0f * ipb - minnorm * minnorm;
}

/* ---------------------------------------------------------------------------
 * Bit packing
 *
 * Two layouts, chosen at build time and recorded in the segment so a reader
 * never has to guess:
 *
 *	 WEAVE_PACK_LANE		coordinate-major within a 32-vector block, the
 *							layout the byte-LUT kernels want.  On x86 the AVX2
 *							kernel additionally shuffles lanes by `perm0` so one
 *							instruction can cross the 128-bit lane boundary; on
 *							ARM the NEON byte-LUT kernel visits lanes
 *							sequentially. Either way `perm0` is a REGISTER-level
 *							relabelling the kernel applies after loading these
 *							same on-disk bytes -- it is not a third value of
 *							this enum and it never changes what is written to
 *							the block.  `src/vector/pack.c`'s `code_index()` is
 *							the only bit-index mapping WEAVE_PACK_LANE has, on
 *							every architecture; see the comment there and
 *							`doc/specs/VECTOR_CHANNEL.md` §8.
 *	 WEAVE_PACK_VECMAJOR	vector-major, the layout the int8 dot-product
 *							kernels (NEON SDOT/SMMLA, AVX-512 VNNI) want.
 *
 * The layout choice is a performance decision, but which layout was USED is a
 * correctness fact, so it lives in the segment descriptor and not in a GUC.
 * ------------------------------------------------------------------------- */

typedef enum WeavePackLayout
{
	WEAVE_PACK_LANE = 0,
	WEAVE_PACK_VECMAJOR = 1
} WeavePackLayout;

/*
 * Bytes needed for a full WEAVE_VEC_BLOCK-vector block at this width.
 *
 * The maximum code index is 32*dim - 1 in both layouts (LANE: coordinates 0..dim-1
 * at slot 31; VECMAJOR: slot 31 has dim coordinates). At 4 bits per code, that
 * is 4*dim*32 bits = 16*dim bytes tight-bound. At 2 bits, 8*dim. Generally:
 * max_code_index = 32*dim - 1, so tight bound = ceil((32*dim - 1 + 1) * bits / 8)
 * = ceil(32*dim*bits / 8) = 4*dim*bits bytes for any bits.  The rounding
 * ((dim*bits + 7) / 8 * 32) adds 0-28 bytes of slack per layout.
 *
 * ** Slack bytes are never written by any pack function (weave_pack_lane,
 * weave_unpack_lane, weave_pack_move_lane, weave_pack_zero_lane).  An on-disk
 * block image therefore carries uninitialized bytes.  Phase V7 (on-disk page
 * format) MUST zero the block buffer before packing to ensure deterministic
 * bytes on disk.
 *
 * ** If computing lane stride as weave_block_codebytes(dim, bits) / 32 for a
 * SIMD fast path, verify that 8 divides dim*bits. When it does not, the rounding
 * introduces a skew and consecutive lanes do not stride uniformly. VECMAJOR
 * packs lanes bit-contiguously (no gap) so the skew read from disk gives bits
 * out of order compared to what contiguous lanes would hold in a SIMD vector. */
static inline int
weave_block_codebytes(int dim, int bits)
{
	return ((dim * bits + 7) / 8) * WEAVE_VEC_BLOCK;
}

/* Pack one vector's codes into lane `slot` of a block buffer, and the inverse.
 * O(1) lane update is what makes vacuum's swap-remove cheap: a deleted vector's
 * lane is zeroed and the last lane moved into it without rewriting the block. */
extern void weave_pack_lane(WeavePackLayout layout, int dim, int bits,
							weave_uint8 *block, int slot, const weave_uint8 *code);
extern void weave_unpack_lane(WeavePackLayout layout, int dim, int bits,
							  const weave_uint8 *block, int slot, weave_uint8 *code);
extern void weave_pack_zero_lane(WeavePackLayout layout, int dim, int bits,
								 weave_uint8 *block, int slot);

/*
 * Move lane `src` into lane `dst`, overwriting whatever was there.  This is
 * the O(1) half of vacuum's swap-remove: retire the deleted vector's slot by
 * moving the block's last live lane into it (`weave_pack_move_lane`), then
 * mark the vacated source slot dead in WeaveVecBlockHdr.livemask (its bits are
 * left as-is; livemask, not zero content, is what makes a lane live).  Cost is
 * one lane's worth of bits, not the block's, which is the point: vacuuming one
 * row must not touch the other 31.
 *
 * dst == src is a correctly-handled no-op, not a caller precondition -- the
 * deleted lane can already be the last live one.
 */
extern void weave_pack_move_lane(WeavePackLayout layout, int dim, int bits,
								 weave_uint8 *block, int dst, int src);

#endif							/* WEAVE_QUANTIZE_H */
