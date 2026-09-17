/*-------------------------------------------------------------------------
 *
 * vecscan.h
 *		The code-scan decision core: what happens to one 32-lane block.
 *
 * Task V8 in doc/PHASES.md; design in doc/specs/VECTOR_CHANNEL.md sect. 8b.
 *
 * WHY THIS IS A SEPARATE, BACKEND-FREE TRANSLATION UNIT.  The vector channel's
 * shuttle is the first implementation of the contract in include/weave/channel.h,
 * and two of that contract's clauses -- (C1) monotone ascent and (C2) a true
 * upper bound -- are exactly the kind of requirement no fixed-expected-output
 * test can check: a bound 1 % too low silently drops rows and the answers stay
 * plausible (AGENTS.md hard rule 1).  A property test is therefore mandatory, and
 * a property test that needs a running backend, a heap, a build and a page cache
 * to reach the decision it is testing will not be run at the scale that finds
 * anything.  So the decision -- mask, then bound, then score -- lives here, is
 * driven by plain structs, and is linked by test/hegel/test_vecscan.c with a
 * bare compiler.  src/vector/vecshuttle.c is the part that reads pages, and it
 * contains no policy.
 *
 * The seam follows include/weave/vecweft.h, which split the vector weft's
 * geometry out of its writer for the same reason.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/vecscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_VECSCAN_H
#define WEAVE_VECSCAN_H

#include "weave/kernels.h"
#include "weave/vecpage.h"
#include "weave/vecweft.h"

/*
 * What to do with the block the cursor is sitting on.
 *
 * The order of the three live outcomes is the order of their cost, and the
 * decision function evaluates them in that order deliberately:
 *
 *	 SKIP_MASK	 one AND against the allowlist.  No code byte is read, no strip
 *				 is scattered into a block buffer, no kernel is called.  This is
 *				 the mechanism behind claim 3 in doc/ARCHITECTURE.md sect. 9, and
 *				 it is the only one of the three that is measured to pay
 *				 (doc/specs/VECTOR_CHANNEL.md sect. 8b).
 *	 SKIP_BOUND	 one LUT pass over the centroid code (dim gathers, 1/32 of the
 *				 cost of scoring the block) plus three comparisons.  Required by
 *				 (C2) whether or not it prunes; measured to prune 0.00 % of
 *				 blocks on both real corpora (bench/RESULTS_CODE_SCAN.md), so it
 *				 is a cost this channel pays for a contract, not a speedup.
 *	 SCORE		 32 lanes x dim gathers.
 *
 * REFUSE is not a fourth outcome on the same axis: it means the directory record
 * or the geometry that located it cannot be trusted, and the backend caller must
 * turn it into an ERROR (doc/CONVENTIONS.md rule 2).  It is never a reason to
 * skip a block quietly -- skipping a block because its header looked wrong is a
 * wrong answer that looks like a repair.
 */
typedef enum WeaveVecScanAct
{
	WEAVE_VSCAN_SCORE = 0,
	WEAVE_VSCAN_SKIP_MASK,
	WEAVE_VSCAN_SKIP_BOUND,
	WEAVE_VSCAN_REFUSE
} WeaveVecScanAct;

/*
 * Scan-wide state.  Deliberately holds no page, no buffer and no relation: the
 * only thing it remembers between blocks is what (C1) needs in order to be
 * checked rather than assumed.
 *
 * ON (C1) BEING CHECKED AT ALL.  `firstwarp` comes off a page.  The writer sets
 * it to blockno * WEAVE_VEC_BLOCK for every block (src/vector/vecwrite.c) and
 * weave_check() asserts that equality, but a scan that *assumes* it is reading a
 * value it was handed by a corrupt page and using it to index an allowlist.  So
 * this core requires both halves -- the O(1) formula and a strictly ascending
 * sequence -- and refuses a mismatch.  When task V13 assigns warps in cluster
 * order the formula half has to be lifted; the ascent half must not be, because
 * it is what (C1) says.
 */
typedef struct WeaveVecScanState
{
	/* Immutable after weave_vec_scan_begin(). */
	const WeaveVecWeftGeom *geom;
	int			metric;			/* WeaveMetric; selects the bound formulation */

	/*
	 * The allowlist and its length.  NULL means "everything is allowed", which
	 * is NOT the same as an all-zero bitmap: an all-zero bitmap skips every
	 * block, and conflating the two turns an empty candidate set into a full
	 * scan.  `nwarp` is meaningless when allow is NULL and mandatory when it is
	 * not, because firstwarp indexes the bitmap (include/weave/kernels.h).
	 */
	const weave_uint64 *allow;
	weave_uint32 nwarp;

	/* (C1) witness.  `started` distinguishes "no block yet" from "block 0". */
	int			started;
	weave_uint32 lastwarp;

	/*
	 * Counters, so the mask short-circuit can be measured rather than asserted.
	 * A skipped block still costs a page read -- the codes chain is a linked
	 * list with no block-to-page index -- so nblk_mask counts saved SCORING,
	 * not saved I/O.  Stating that here keeps the number from being quoted as
	 * something it is not (doc/specs/VECTOR_CHANNEL.md sect. 8b).
	 */
	weave_uint64 nblk_seen;
	weave_uint64 nblk_mask;
	weave_uint64 nblk_bound;
	weave_uint64 nblk_score;
	weave_uint64 nlane_score;

	/* Set whenever the result is WEAVE_VSCAN_REFUSE.  Never NULL then. */
	const char *why;
} WeaveVecScanState;

/*
 * Initialize.  Returns -1 (and sets st->why) for a geometry this core cannot
 * scan: a non-LANE pack layout, a metric with no compressed-domain bound, or an
 * allowlist shorter than the weft's own lane count while `allow` is non-NULL --
 * the last because a bitmap that does not cover the segment cannot be intended
 * for it, and trimming it would score real lanes against absent bits.
 */
extern int	weave_vec_scan_begin(WeaveVecScanState *st,
								 const WeaveVecWeftGeom *geom,
								 int metric,
								 const weave_uint64 *allow,
								 weave_uint32 nwarp);

/*
 * Decide what to do with block `blockno`, whose directory record is `rec` and
 * whose centroid code is `cencode` (geom->codebytes bytes).
 *
 * `threshold` is the caller's current top-k floor, and the comparison is
 * `bound <= threshold` because a bounded top-k rejects on `s <= theta` -- an
 * equal score does not displace an incumbent, so a block that can only equal the
 * floor cannot contribute.  Pass -INFINITY to disable bound pruning without
 * disabling the bound's computation.
 *
 * *bound_out receives the bound whenever the return is SCORE or SKIP_BOUND, so
 * that a caller implementing WeaveShuttleOps.block_max() can cache it and
 * satisfy (C3) -- block_max() must not read a page, and it must not recompute a
 * dim-length LUT pass either, because the fused loop calls it more than once per
 * block.
 *
 * Does NOT read `codes`; it is not a parameter.  That is what makes SKIP_MASK
 * free.
 */
extern WeaveVecScanAct weave_vec_scan_block(WeaveVecScanState *st,
											weave_uint32 blockno,
											const WeaveVecDirRec *rec,
											const WeaveQueryLut *lut,
											const weave_uint8 *cencode,
											float threshold,
											float *bound_out);

/*
 * Fill a WeaveScoreBlock for the block weave_vec_scan_block() just returned
 * SCORE for.  The conversion is here rather than at the call site because the
 * directory record stores the per-lane (scale, norm) pairs INTERLEAVED --
 * rec->lane[2*s] is lane s's scale -- while the kernel interface takes a base
 * pointer and a stride, and getting that stride wrong does not fail, it returns
 * wrong distances (src/vector/pack.c).  One conversion, one place, used by both
 * the backend and the property test.
 *
 * `nlanes` must be weave_vecweft_block_lanes(geom, blockno): the final block of
 * a weft is short when nvec is not a multiple of 32.
 */
extern void weave_vec_scan_scoreblk(WeaveScoreBlock *blk,
									const WeaveVecScanState *st,
									const WeaveVecDirRec *rec,
									const WeaveQueryLut *lut,
									const weave_uint8 *codes,
									int nlanes);

/*
 * The bolt-wide ceiling required by WeaveShuttle.maxscore, folded one directory
 * record at a time.
 *
 * It is bound (B2) -- max||recon|| * ||q|| -- and not (B3), because (B3) needs
 * the centroid CODE, which lives on the code pages rather than in the directory,
 * so folding (B3) over every block would cost a full pass over the weft before
 * the scan starts.  (B2) needs one float per record.  It is looser, and looser is
 * the correct trade for a value whose only job is the MaxScore partition
 * (doc/specs/FUSED_TOPK.md sect. 5).
 *
 * Call with *acc = 0.0 before the first record.  The scan's maxscore is then
 * lut->qnorm * *acc for an inner-product metric.
 */
extern void weave_vec_scan_maxscore_fold(float *acc, const WeaveVecDirRec *rec);

#endif							/* WEAVE_VECSCAN_H */
