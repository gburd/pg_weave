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
 * EXACTNESS, AND WHICH KERNELS IT BINDS.  Every kernel whose `approximate` flag
 * is clear produces results BIT-IDENTICAL to weave_score_block_scalar(), and
 * test/hegel/test_kernels.c asserts it with memcmp over a randomized grid.  That
 * is stronger than the "a scoring kernel may differ in the last bit" latitude
 * src/vector/kernels.c describes, and it is affordable for one specific
 * structural reason:
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
 * THE ONE FAMILY THAT IS NOT EXACT, and how it is fenced off.  Task V16 added
 * the nibble-split byte-LUT kernels doc/specs/VECTOR_CHANNEL.md sect. 8
 * tabulates: "lut-byte-ref" (scalar) and "lut-byte" (AVX2 vpshufb).  They
 * quantize the query table to 8 bits before gathering, so they are not one ULP
 * from the oracle, they are one QUANTIZATION STEP from it -- an error budget, not
 * a rounding difference.  Both therefore carry `approximate` set, and the fence
 * is:
 *
 *	 - weave_score_kernel_best() never returns one, so `auto` cannot select one;
 *	 - the differential test gates them against EACH OTHER bit for bit (they are
 *	   the same integer arithmetic twice) and merely REPORTS their deviation from
 *	   the oracle, because an approximate kernel cannot be gated on equality with
 *	   an exact one and pretending otherwise would delete the gate;
 *	 - they refuse any block they cannot score exactly as specified -- 4 bits and
 *	   WEAVE_PACK_LANE only -- rather than delegating to the oracle, so a
 *	   measurement can never be another path's number wearing this one's name.
 *
 * The int8-dot family (NEON SDOT/SMMLA, AVX-512 VNNI) is still absent, as is any
 * NEON byte-LUT path.  See doc/PHASES.md V6 and V16.
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
 * `layout` is explicit and mandatory, and it is threaded all the way from the
 * segment descriptor: WeaveVecKernelOps.score_block in weave/vector.h takes it
 * too.  Which layout a segment used is a correctness fact recorded in
 * WeaveVecMeta (doc/specs/VECTOR_CHANNEL.md sect. 7) and src/vector/pack.c says
 * the consequence of guessing plainly -- a reader that guesses wrong "does not
 * fail, it returns wrong distances".  A kernel therefore either handles the
 * layout it was given or declines it explicitly by delegating to the oracle,
 * which reaches codes only through the pack API; an unrecognized layout value is
 * rejected outright, because this field ultimately comes off a page.
 *
 * `firstwarp` is the warp position of lane 0, and lane s is at warp
 * firstwarp + s.  That contiguity is what makes the `allow` test 32 bits of one
 * shift instead of 32 bitmap probes, and it is what the vector weft must
 * guarantee when it assigns warp positions (task V13 assigns them in cluster
 * order, which is contiguous per block by construction).
 *
 * `nwarp` is the LENGTH of the allowlist, in warp positions -- the same nwarp
 * the shuttle was opened with (weave/vector.h).  It is mandatory whenever
 * `allow` is non-NULL and it exists because `firstwarp` comes off a page: a
 * bitmap with no length next to an untrusted index into it is an unbounded
 * out-of-bounds read on a corrupt page, which doc/CONVENTIONS.md forbids
 * outright ("a corrupt page produces a clean ERROR, never a crash and never a
 * wrong answer").  A block whose lanes are not entirely inside [0, nwarp) is
 * rejected rather than clamped: clamping would silently score lanes against
 * bits belonging to other warps.
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
	weave_uint32 nwarp;			/* warps `allow` covers; unused if allow==NULL */
} WeaveScoreBlock;

/*
 * The 32 `allow` bits covering one block, as a lane-indexed mask, with the
 * dead-lane and short-block trims already applied.  Zero means the block has no
 * live-and-allowed lane, which is the one case in which not a single code byte
 * has to be read.
 *
 * IT IS SHARED, AND THAT IS THE POINT.  Every scoring kernel needs this mask, and
 * so does the code-scan decision core (weave/vecscan.h), which asks the same
 * question one step EARLIER -- before it holds any codes -- so that a fully
 * masked block costs one AND and nothing else.  Two transcriptions of the word
 * addressing below would be two chances to get it wrong, and getting it wrong
 * reads a bitmap out of bounds on a corrupt page instead of failing
 * (doc/CONVENTIONS.md decision 2).  So there is one, here, inline because it sits
 * inside the per-block loop of every kernel.
 *
 * It takes four scalars rather than a WeaveScoreBlock so that the decision core,
 * which has neither codes nor a LUT at the moment it asks, does not have to
 * fabricate a half-filled block to reach it.
 *
 * Lane s sits at warp firstwarp + s, so the block's slice of the allowlist is at
 * most two 64-bit words and is extracted with one shift.  A NULL allowlist means
 * "everything is allowed" -- the unfiltered scan -- and must not be confused with
 * an all-zero bitmap, which means the opposite.
 *
 * The second word is addressed from firstwarp + nlanes - 1 and NOT from
 * firstwarp + WEAVE_VEC_BLOCK - 1: the caller has already established that warp
 * firstwarp + nlanes - 1 is inside the bitmap, whereas a short block at the end
 * of a weft can have firstwarp + WEAVE_VEC_BLOCK past its end.  Bits above
 * nlanes are trimmed off `m` before the allowlist is consulted, so reading fewer
 * words loses nothing.
 */
static inline weave_uint32
weave_lane_avail_mask(weave_uint32 livemask, int nlanes,
					  const weave_uint64 *allow, weave_uint32 firstwarp)
{
	weave_uint32 m = livemask;

	if (nlanes < WEAVE_VEC_BLOCK)
		m &= (weave_uint32) ((1u << nlanes) - 1);

	if (allow != NULL)
	{
		size_t		w0 = (size_t) (firstwarp >> 6);
		size_t		w1 = (size_t) ((firstwarp + (weave_uint32) nlanes - 1) >> 6);
		int			off = (int) (firstwarp & 63);
		weave_uint64 a = allow[w0] >> off;

		if (w1 != w0 && off != 0)
			a |= allow[w1] << (64 - off);
		m &= (weave_uint32) a;
	}
	return m;
}

/*
 * Score one block.  Writes exactly blk->nlanes floats, WEAVE_KERNEL_NEVER for
 * any lane that is dead or masked out so the caller's indexing stays positional,
 * and returns blk->nlanes.  Returns -1 without writing anything if the block
 * description is inconsistent -- bad dim, bad nlevels, nlanes out of range, an
 * unrecognized pack layout, or a firstwarp that would read `allow` past nwarp --
 * because these numbers come from a page and pages are not trusted
 * (doc/CONVENTIONS.md); the backend adapter turns -1 into a clean ERROR.
 *
 * A masked lane is SKIPPED, not scored and discarded, and a block with no
 * live-and-allowed lane at all returns without touching a code byte.  That is
 * the mechanism by which a selective predicate makes this channel faster rather
 * than slower (doc/specs/VECTOR_CHANNEL.md sect. 9), so it is not an
 * each path skips differs and is stated where each path implements it: the
 * oracle skips per lane, the wide and AVX2 float paths skip per group of 8 lanes
 * because a group is the unit their addressing is built on, and the byte-LUT AVX2
 * path skips per whole block because one coordinate's 16 code bytes cover all 32
 * lanes and there is no 8-lane subset it could decline to load.
 */
typedef int (*weave_score_block_fn) (const WeaveScoreBlock *blk, float *out);

typedef struct WeaveScoreKernel
{
	/* Reported verbatim by weave_vec_kernel_name(); it lands in bug reports, so
	 * it names the ISA and the strategy, e.g. "lut-avx2". */
	const char *name;
	weave_score_block_fn score_block;

	/*
	 * Nonzero if this kernel's scores are an APPROXIMATION of the oracle's
	 * rather than a bit-identical reproduction of them (today: the byte-LUT
	 * family, whose 8-bit query table costs one quantization step per
	 * coordinate).
	 *
	 * WHY THIS IS A PROPERTY OF THE KERNEL AND NOT REGISTRY BOOKKEEPING.  An
	 * approximate kernel is only SOUND when something downstream repairs the
	 * ranking it perturbed -- an exact rerank over a window.  The ratified shape
	 * does exactly that (4 bits, exact rerank of a top-25 window, recall@10
	 * 0.9920 at n = 1M on GIST-960d; weave/quantize.h and
	 * bench/RESULTS_BITWIDTH_SWEEP.md), but tasks V7/V8/V15 do not exist yet, so
	 * there is no query path in this tree that can GUARANTEE the rerank is
	 * present.  A caller that needs to know whether a rerank is mandatory reads
	 * this flag; that is a question about the kernel it was handed, so the answer
	 * belongs on the kernel.
	 *
	 * Consequences, enforced in src/vector/kernels.c:
	 *
	 *	 - weave_score_kernel_best() SKIPS approximate kernels, so `auto` cannot
	 *	   select one.  Promoting lut-byte to `auto` is a decision that belongs
	 *	   with V8/V15, where the rerank exists to justify it -- not here.
	 *	 - weave_score_kernel_lookup() still finds them by name, so a path can be
	 *	   forced deliberately and A/B'd.
	 *	 - weave_score_kernel_list() still lists them, so the differential test
	 *	   and any diagnostic see every path dispatch can reach.
	 */
	int			approximate;
} WeaveScoreKernel;

/* The oracle.  Always available, layout-agnostic, and the fallback forever. */
extern const WeaveScoreKernel weave_score_kernel_scalar;

/*
 * Every kernel usable on THIS host: the exact paths best first, then the
 * approximate ones, and scalar always last.  Fills at most `max` entries and
 * returns how many.  The differential test walks this list, which is why it is
 * public: a path that dispatch can select but the test cannot see is exactly the
 * hole AGENTS.md rule 8 exists to close.
 *
 * A caller that compares kernels against each other MUST split the list on
 * `approximate` first.  Asserting that an approximate kernel equals the oracle
 * does not fail honestly -- it fails always, and the usual repair (loosen it to a
 * tolerance) would silently weaken the exact kernels' gate too.
 */
extern int	weave_score_kernel_list(const WeaveScoreKernel **out, int max);

/*
 * Best available, and lookup by the name above (NULL if this host cannot run
 * it).  Both are pure; the backend resolves once and caches.
 *
 * best() considers EXACT kernels only, for the reason given at
 * WeaveScoreKernel.approximate.  lookup() does not filter: naming a kernel is
 * how a host gets A/B'd and how a bug report from another machine is reproduced,
 * and refusing to resolve a name that the list reports would make both
 * impossible.
 */
extern const WeaveScoreKernel *weave_score_kernel_best(void);
extern const WeaveScoreKernel *weave_score_kernel_lookup(const char *name);

/* Convenience: dispatch through the best available kernel.  Used by the
 * benchmark harness; the backend goes through weave_vec_kernels instead so that
 * the GUC can force a path. */
extern int	weave_score_block(const WeaveScoreBlock *blk, float *out);

#endif							/* WEAVE_KERNELS_H */
