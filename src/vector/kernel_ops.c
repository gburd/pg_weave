/*-------------------------------------------------------------------------
 *
 * kernel_ops.c
 *		Backend half of the vector-channel kernel dispatch: the
 *		WeaveVecKernelOps table, the pg_weave.vec_kernel GUC, and the adapter
 *		from the shuttle's page-shaped arguments to the kernels' own.
 *
 * The kernels themselves are in src/vector/kernels.c, which has no PostgreSQL
 * includes so that test/hegel/test_kernels.c can link the shipped code with a
 * plain gcc.  doc/TESTING.md states the rule this split follows: "If an
 * algorithmic core cannot be linked into a plain gcc invocation, that is a reason
 * to restructure it, not a reason to skip the test."  Everything in this file is
 * glue -- there is no arithmetic here, on purpose, because arithmetic here would
 * be arithmetic the differential test cannot see.
 *
 * Resolution happens once, from _PG_init, into a function-pointer table, the
 * same shape PostgreSQL uses for pg_popcount and CRC32C (src/port/).  Following
 * core keeps "which path ran?" answerable from weave_vec_kernel_name() in a bug
 * report, and pg_weave.vec_kernel makes a report from a different machine
 * reproducible on this one.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/kernel_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/float.h"
#include "utils/guc.h"
#include "weave/kernels.h"
#include "weave/vector.h"

int			weave_vec_kernel = WEAVE_KERNEL_AUTO;

static const WeaveScoreKernel *vec_core = &weave_score_kernel_scalar;

/*
 * The shuttle hands us &lanes[0].scale plus a stride rather than the struct,
 * so that weave/kernels.h need not know a backend-typed struct.  That trick is
 * only valid while WeaveVecLane is exactly two floats.
 */
StaticAssertDecl(sizeof(WeaveVecLane) == 2 * sizeof(float4),
				 "WeaveVecLane is no longer two floats; the kernels' scale "
				 "stride is wrong");

/*
 * Adapt WeaveVecKernelOps.score_block to WeaveScoreBlock.
 *
 * A GAP IN THE OPS INTERFACE, deliberately not papered over: score_block() takes
 * no pack layout, but which layout a segment used is a per-segment fact recorded
 * in WeaveVecMeta, and a scorer that guesses wrong returns wrong distances rather
 * than an error (doc/specs/VECTOR_CHANNEL.md sect. 7).  This adapter therefore
 * asserts the byte-LUT layout, which is the only layout any implemented kernel
 * reads, and V8 should call weave_score_block() directly with the layout from the
 * segment descriptor -- or this signature should grow the layout -- rather than
 * inherit the assumption.  A WEAVE_PACK_VECMAJOR segment scored through here
 * would be silently wrong, so it must not be reachable from the shuttle until
 * that is settled.
 */
static int
weave_score_block_ops(const WeaveQueryLut *lut,
					  const WeaveVecBlockHdr *hdr,
					  const uint8 *codes,
					  const WeaveVecLane *lanes,
					  const uint64 *allow,
					  float4 *out)
{
	WeaveScoreBlock blk;
	int			n;

	if (lut == NULL || hdr == NULL || codes == NULL || lanes == NULL)
		elog(ERROR, "weave vector kernel called with a NULL block argument");

	memset(&blk, 0, sizeof(blk));
	blk.lut = lut;
	blk.layout = WEAVE_PACK_LANE;
	blk.codes = codes;
	blk.scales = &lanes[0].scale;
	blk.scalestride = (int) (sizeof(WeaveVecLane) / sizeof(float4));
	blk.nlanes = WEAVE_VEC_BLOCK;
	blk.livemask = hdr->livemask;
	blk.firstwarp = hdr->firstwarp;
	blk.allow = allow;

	n = vec_core->score_block(&blk, out);
	if (n < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("weave vector code block is inconsistent"),
				 errdetail("dim %d, %d levels: not a geometry this index can "
						   "have written.", lut->dim, lut->nlevels)));
	return n;
}

/*
 * The table.  Mutable because `name` tracks whichever kernel was resolved --
 * weave_vec_kernel_name() lands in bug reports and a stale name there is worse
 * than none.
 */
static WeaveVecKernelOps vec_ops = {
	.name = "scalar",
	.score_block = weave_score_block_ops,

	/*
	 * No SIMD rotation, and V6 does not add one.  The scalar rotation is already
	 * only +, - and one multiply in a fixed reduction order, and V2's gate is a
	 * cross-architecture fixture hash: a rotation kernel that differs in the last
	 * bit changes a CODE and therefore what the index contains, so the bar is
	 * bit-identical or rejected (weave/quantize.h).
	 */
	.rotate = weave_rotate,
};

const WeaveVecKernelOps *weave_vec_kernels = &vec_ops;

/*
 * Resolve the dispatch table for a given setting of pg_weave.vec_kernel.
 *
 * WEAVE_KERNEL_DOT resolves to the scalar reference rather than to the best LUT
 * kernel: no int8-dot kernel exists (doc/specs/VECTOR_CHANNEL.md sect. 8 and the
 * note at the top of src/vector/kernels.c explain why the exact-scoring
 * requirement rules that family out for now), and quietly substituting a
 * different family would make the one thing this GUC exists for -- reproducing a
 * bug report from another host -- impossible.
 */
static void
weave_vec_kernels_resolve(int setting)
{
	const WeaveScoreKernel *k;

	switch (setting)
	{
		case WEAVE_KERNEL_SCALAR:
			k = weave_score_kernel_lookup("scalar");
			break;
		case WEAVE_KERNEL_DOT:
			k = weave_score_kernel_lookup("scalar");
			break;
		case WEAVE_KERNEL_LUT:
		case WEAVE_KERNEL_AUTO:
		default:

			/*
			 * Every implemented kernel is a float-LUT gather, so "lut" and
			 * "auto" agree today.  They will stop agreeing the moment an
			 * int8-dot kernel lands, which is why they are separate values.
			 */
			k = weave_score_kernel_best();
			break;
	}

	if (k == NULL)
		k = &weave_score_kernel_scalar;

	vec_core = k;
	vec_ops.name = k->name;
}

static void
weave_vec_kernel_assign(int newval, void *extra)
{
	/* Re-resolve on SET, so forcing a path takes effect in the session that
	 * forced it rather than only at the next backend start. */
	weave_vec_kernels_resolve(newval);
}

static const struct config_enum_entry vec_kernel_options[] = {
	{"auto", WEAVE_KERNEL_AUTO, false},
	{"scalar", WEAVE_KERNEL_SCALAR, false},
	{"lut", WEAVE_KERNEL_LUT, false},
	{"dot", WEAVE_KERNEL_DOT, false},
	{NULL, 0, false}
};

/*
 * Resolve the kernel table.  Called from _PG_init, once.
 *
 * The ISA matrix and the two scoring strategies (nibble-split byte LUT versus
 * int8 dot product) are specified in doc/specs/VECTOR_CHANNEL.md sect. 8.  Do
 * not guess which wins: bench/kernels.c is supposed to A/B them per host, and
 * the pg_weave.vec_kernel GUC exists so a bug report from a different machine can
 * be reproduced.
 */
void
weave_vec_kernels_init(void)
{
	static bool done = false;
	float4		never = WEAVE_SCORE_NEVER;
	float		kernel_never = WEAVE_KERNEL_NEVER;

	if (done)
		return;					/* DefineCustom*Variable raises on a
								 * redefinition */
	done = true;

	/*
	 * weave/channel.h spells the (C5) sentinel -get_float4_infinity() and
	 * weave/kernels.h spells it -INFINITY, because the kernels compile without a
	 * backend.  If those ever stop being the same bits, every masked-out lane
	 * starts looking like a real score to the fused scorer, which is a
	 * silently-wrong-answer bug of exactly the shape AGENTS.md rule 1 describes.
	 * Refuse to load instead.
	 */
	if (memcmp(&never, &kernel_never, sizeof(float4)) != 0)
		elog(ERROR, "weave: WEAVE_SCORE_NEVER and WEAVE_KERNEL_NEVER disagree");

	DefineCustomEnumVariable("pg_weave.vec_kernel",
							 "Force a vector-channel scoring kernel family.",
							 "auto picks the widest SIMD path this host can run, and is the only value anyone should ship. The others exist to A/B kernels on one machine and to reproduce a bug report from another; every implemented path is bit-identical to scalar, so this changes speed and not answers.",
							 &weave_vec_kernel,
							 WEAVE_KERNEL_AUTO, vec_kernel_options,
							 PGC_USERSET, 0, NULL, weave_vec_kernel_assign, NULL);

	weave_vec_kernels_resolve(weave_vec_kernel);
}

const char *
weave_vec_kernel_name(void)
{
	return weave_vec_kernels->name;
}
