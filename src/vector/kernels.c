/*-------------------------------------------------------------------------
 *
 * kernels.c
 *		Runtime-dispatched SIMD kernels for the vector channel.
 *
 * Task V6 in doc/PHASES.md.  Currently the scalar reference only: it is the
 * oracle every other path must reproduce, so it exists first and it is the
 * fallback forever.
 *
 * Dispatch follows PostgreSQL's own pattern for pg_popcount and CRC32C
 * (src/port/pg_popcount_*.c): resolve once at _PG_init into a function-pointer
 * table.  Following core rather than inventing a scheme keeps "which path ran?"
 * answerable from weave_vec_kernel_name() in a bug report, which matters because
 * a wrong kernel produces wrong distances rather than a crash.
 *
 * TWO DIFFERENT STANDARDS APPLY HERE, and conflating them is a correctness bug:
 *
 *	 - A SCORING kernel that differs from scalar in the last bit changes a score
 *	   slightly.  Tolerable, though it should be understood.
 *	 - A ROTATION kernel that differs from scalar in the last bit changes a CODE,
 *	   and therefore what the index contains.  A row inserted on one machine and
 *	   queried on another would then give different answers.  Bit-identical or
 *	   rejected.  See the determinism warning in weave/quantize.h.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/kernels.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/vector.h"

int			weave_vec_kernel = WEAVE_KERNEL_AUTO;

/*
 * Scalar reference implementation of a block score.
 *
 * Computes, for each live lane s of the block, scale_s * sum_j lut[j][code_s[j]],
 * writing WEAVE_SCORE_NEVER for lanes that are dead or masked out so the
 * caller's indexing stays positional.
 *
 * `allow` is a warp-indexed bitmap or NULL.  A masked lane must be SKIPPED, not
 * scored and discarded: skipping is the mechanism by which a selective predicate
 * makes this channel faster rather than slower (doc/specs/VECTOR_CHANNEL.md
 * sect. 9).
 */
static int
weave_score_block_scalar(const WeaveQueryLut *lut,
						 const WeaveVecBlockHdr *hdr,
						 const uint8 *codes,
						 const WeaveVecLane *lanes,
						 const uint64 *allow,
						 float4 *out)
{
	elog(ERROR, "weave vector code scan not implemented: "
		 "see doc/specs/VECTOR_CHANNEL.md task V6 and V8");
	return 0;					/* keep the compiler quiet */
}

static const WeaveVecKernelOps weave_kernel_scalar = {
	.name = "scalar",
	.score_block = weave_score_block_scalar,
	.rotate = weave_rotate,
};

const WeaveVecKernelOps *weave_vec_kernels = &weave_kernel_scalar;

/*
 * Resolve the kernel table.  Called from _PG_init, once.
 *
 * The ISA matrix and the two scoring strategies (nibble-split byte LUT versus
 * int8 dot product) are specified in doc/specs/VECTOR_CHANNEL.md sect. 8.  Do
 * not guess which wins: bench/kernels.c is supposed to A/B them per host, and
 * the weave.vec_kernel GUC exists so a bug report from a different machine can
 * be reproduced.
 */
void
weave_vec_kernels_init(void)
{
	/*
	 * Task V6.  Until the SIMD paths exist and pass bit-exact equivalence
	 * against the scalar reference (test/hegel/test_kernels.c), dispatch stays
	 * on scalar.  Shipping a fast kernel that has not passed that test is how
	 * pg_turbovec produced a retracted benchmark; see AGENTS.md rule 8.
	 */
	weave_vec_kernels = &weave_kernel_scalar;
}

const char *
weave_vec_kernel_name(void)
{
	return weave_vec_kernels->name;
}
