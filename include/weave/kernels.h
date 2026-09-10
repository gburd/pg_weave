/*-------------------------------------------------------------------------
 *
 * kernels.h
 *		Block-scoring kernels for the vector channel -- the backend-independent
 *		half of task V6.
 *
 * weave/vector.h declares the backend-facing dispatch shell (WeaveVecKernelOps,
 * weave_vec_kernels_init).  This header declares the thing that actually does
 * the arithmetic, in plain C with no PostgreSQL includes, for the reason stated
 * at the top of weave/quantize.h and again in doc/TESTING.md: a differential
 * test must link the SHIPPED kernels, not a copy of them.  A copy would agree
 * with itself forever.
 *
 * EXACTNESS.  Every kernel registered here produces results BIT-IDENTICAL to
 * weave_score_block_scalar(), and test/hegel/test_kernels.c asserts it with
 * memcmp over a randomized grid.  That is stronger than the "a scoring kernel
 * may differ in the last bit" latitude src/vector/kernels.c describes, and it is
 * affordable for one specific structural reason:
 *
 *		the parallelism is across LANES, not across coordinates.
 *
 * Each lane's score is sum_j lut[j][code_s[j]] summed in strict ascending j
 * order into a double, exactly as weave_lut_score_code() does it for one lane.
 * Running 8 lanes at once does not reassociate any one lane's sum, so there is
 * no rounding difference to argue about and no tolerance to tune.  A kernel that
 * reassociated the j loop (multiple partial sums per lane, or FMA) would be
 * "close enough" instead, and then every future disagreement becomes a judgement
 * call.  Do not do that.
 *
 * WHAT IS DELIBERATELY NOT HERE.  doc/specs/VECTOR_CHANNEL.md sect. 8 lists
 * nibble-split byte-LUT and int8-dot strategies (SSE2/SSSE3, NEON TBL, NEON
 * SDOT, AVX-512 VNNI).  Those quantize the query lookup table to 8 bits before
 * gathering, so they are not one ULP from this reference, they are one
 * QUANTIZATION STEP from it -- and V6's gate as written ("identical to the
 * scalar path") cannot be met by any of them.  They need their own error budget,
 * their own recall measurement, and their own gate.  None of that exists yet, so
 * none of them is implemented.  See doc/PHASES.md V6.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/kernels.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_KERNELS_H
#define WEAVE_KERNELS_H

#include <math.h>

#include "weave/quantize.h"

/*
 * The (C5) "this position can never contribute" sentinel, spelled without a
 * backend.  weave/channel.h defines WEAVE_SCORE_NEVER as
 * -get_float4_infinity(); this is the same value.  weave_vec_kernels_init()
 * Asserts the two agree so the two spellings cannot drift apart unnoticed.
 */
#define WEAVE_KERNEL_NEVER		(-INFINITY)

/*
 * Largest single-vector code buffer.  The scalar path unpacks one lane at a time
 * through the pack API and needs somewhere to put it; sizing that buffer from
 * the frozen dimension ceiling keeps the scan path allocation-free, the same
 * argument weave/quantize.h makes for the encoder's stack buffers.
 */
#define WEAVE_CODE_MAX_BYTES	((((WEAVE_MAX_DIM) * (WEAVE_BITS_MAX)) + 7) / 8)

/*
 * One block's worth of work.
 *
 * `scales` is a float pointer plus a stride rather than a WeaveVecLane array so
 * that this header does not have to know a backend-typed struct: the shuttle
 * passes &lanes[0].scale with stride sizeof(WeaveVecLane)/sizeof(float).  The
 * norms are not here because scoring does not need them -- only the L2 bound
 * does, and the bound lives in weave/quantize.h.
 *
 * `layout` is explicit and mandatory.  Which layout a segment used is a
 * correctness fact recorded in WeaveVecMeta (doc/specs/VECTOR_CHANNEL.md
 * sect. 7); a kernel that guessed would return wrong distances rather than an
 * error.  Note that WeaveVecKernelOps.score_block in weave/vector.h has no
 * layout parameter, which is a gap in that interface -- see the comment on the
 * adapter in src/vector/kernel_ops.c.
 *
 * `firstwarp` is the warp position of lane 0, and lane s is at warp
 * firstwarp + s.  That contiguity is what makes the `allow` test 32 bits of one
 * shift instead of 32 bitmap probes, and it is what the vector weft must
 * guarantee when it assigns warp positions (task V13 assigns them in cluster
 * order, which is contiguous per block by construction).
 */
typedef struct WeaveScoreBlock
{
	const WeaveQueryLut *lut;	/* dim, nlevels, and the table itself */
	WeavePackLayout layout;
	const weave_uint8 *codes;	/* the packed block, WEAVE_VEC_BLOCK lanes */
	const float *scales;		/* renormalization scale per lane */
	int			scalestride;	/* floats between consecutive lanes' scales */
	int			nlanes;			/* 1 .. WEAVE_VEC_BLOCK */
	weave_uint32 livemask;		/* bit s set = lane s occupied */
	weave_uint32 firstwarp;		/* warp position of lane 0 */
	const weave_uint64 *allow;	/* warp-indexed allowlist, or NULL */
} WeaveScoreBlock;

/*
 * Score one block.  Writes exactly blk->nlanes floats, WEAVE_KERNEL_NEVER for
 * any lane that is dead or masked out so the caller's indexing stays positional,
 * and returns blk->nlanes.  Returns -1 without writing anything if the block
 * description is inconsistent (bad dim, bad nlevels, nlanes out of range) --
 * these numbers come from a page and pages are not trusted
 * (doc/CONVENTIONS.md); the backend adapter turns -1 into a clean ERROR.
 *
 * A masked lane is SKIPPED, not scored and discarded, and a block with no
 * live-and-allowed lane at all returns without touching a code byte.  That is
 * the mechanism by which a selective predicate makes this channel faster rather
 * than slower (doc/specs/VECTOR_CHANNEL.md sect. 9), so it is not an
 * optimization to be traded away for simpler code.
 */
typedef int (*weave_score_block_fn) (const WeaveScoreBlock *blk, float *out);

typedef struct WeaveScoreKernel
{
	/* Reported verbatim by weave_vec_kernel_name(); it lands in bug reports, so
	 * it names the ISA and the strategy, e.g. "lut-avx2". */
	const char *name;
	weave_score_block_fn score_block;
} WeaveScoreKernel;

/* The oracle.  Always available, layout-agnostic, and the fallback forever. */
extern const WeaveScoreKernel weave_score_kernel_scalar;

/*
 * Every kernel usable on THIS host, best first, scalar last.  Fills at most
 * `max` entries and returns how many.  The differential test walks this list,
 * which is why it is public: a path that dispatch can select but the test cannot
 * see is exactly the hole AGENTS.md rule 8 exists to close.
 */
extern int	weave_score_kernel_list(const WeaveScoreKernel **out, int max);

/* Best available, and lookup by the name above (NULL if this host cannot run
 * it).  Both are pure; the backend resolves once and caches. */
extern const WeaveScoreKernel *weave_score_kernel_best(void);
extern const WeaveScoreKernel *weave_score_kernel_lookup(const char *name);

/* Convenience: dispatch through the best available kernel.  Used by the
 * benchmark harness; the backend goes through weave_vec_kernels instead so that
 * the GUC can force a path. */
extern int	weave_score_block(const WeaveScoreBlock *blk, float *out);

#endif							/* WEAVE_KERNELS_H */
