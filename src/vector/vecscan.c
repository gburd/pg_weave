/*-------------------------------------------------------------------------
 *
 * vecscan.c
 *		The code-scan decision core: mask, then bound, then score.
 *
 * Backend-free, like src/vector/vecweft.c and src/vector/kernels.c, and for the
 * reason include/weave/vecscan.h states at length: contracts (C1) and (C2) are
 * the class of requirement whose violation returns plausible answers, so the
 * property test that gates them is mandatory (AGENTS.md hard rule 1) and must be
 * runnable at scale by a bare compiler.  test/hegel/test_vecscan.c links this
 * file directly.  src/vector/vecshuttle.c reads the pages and holds no policy.
 *
 * THE THREE THINGS THIS FILE IS CAREFUL ABOUT, none of which is arithmetic:
 *
 * 1. It never clamps and never skips quietly.  Every input that cannot be
 *	  trusted -- a float that cannot be a bound, a firstwarp that is not where the
 *	  formula says, a firstwarp that did not ascend, a blockno past the weft, a
 *	  lane outside the allowlist -- returns WEAVE_VSCAN_REFUSE with `why` set, and
 *	  the backend adapter turns that into an ERROR.  Skipping a block because its
 *	  header looked wrong is a wrong answer wearing the costume of a repair.
 *
 * 2. Bounds and scores are produced in ONE domain, chosen by one switch.  The
 *	  kernels return inner products and are handed no norms; weave_block_bound_l2()
 *	  bounds -||q - v||^2.  Reporting one next to the other makes (C2) meaningless
 *	  rather than violated, which is why weave_vec_scan_lane_score() exists and why
 *	  scan_bound() below switches on exactly the same metric.
 *
 * 3. The mask test comes FIRST and touches no code byte.  weave_vec_scan_block()
 *	  does not even take a `codes` pointer, so "SKIP_MASK read no codes" is a
 *	  property of the signature rather than a claim about the body.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vecscan.c
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "weave/vecscan.h"

/*
 * Lanes a kernel will actually compute for this block: the live-and-allowed ones.
 *
 * Kernighan's loop rather than a builtin because this file must compile with any
 * C compiler (it is linked by the standalone property test with no configure
 * step), and it runs once per block against a 32-bit word -- next to a block's
 * 32 x dim gathers it is not measurable.
 */
static int
lane_popcount(weave_uint32 m)
{
	int			n = 0;

	while (m != 0)
	{
		m &= m - 1;
		n++;
	}
	return n;
}

/*
 * The block bound, in the metric's domain.  Returns 0, or -1 for a metric this
 * core does not serve or a bound that is not a number.
 *
 * A NaN bound is a REFUSAL and not a pass-through, and the reason is the same one
 * weave_vecdir_floats_ok() gives for rejecting a NaN in the record: `bound <=
 * threshold` is false for a NaN, so a NaN bound would be reported as a SCORE
 * decision carrying a bound that dominates nothing, and (C2) would be silently
 * void.  The record's floats have already been validated by the caller's read
 * path and again below, so a NaN here comes from the query side -- a LUT built
 * from a query containing a NaN -- which is exactly a case worth refusing rather
 * than scanning.
 */
static int
scan_bound(const WeaveVecScanState *st, const WeaveVecDirRec *rec,
		   const WeaveQueryLut *lut, const weave_uint8 *cencode, float *out)
{
	float		censcore;
	float		b;

	censcore = weave_lut_score_code(lut, st->geom->bits, cencode, rec->censcale);

	switch (st->metric)
	{
		case WEAVE_METRIC_IP:
			b = weave_block_bound_ip(lut, rec->smax, rec->maxrecnorm, censcore,
									 rec->cenrad);
			break;
		case WEAVE_METRIC_L2:
			b = weave_block_bound_l2(lut, rec->smax, rec->maxrecnorm, censcore,
									 rec->cenrad, rec->minnorm);
			break;
		default:
			return -1;
	}

	if (!(b == b))
		return -1;
	*out = b;
	return 0;
}

int
weave_vec_scan_begin(WeaveVecScanState *st, const WeaveVecWeftGeom *geom,
					 int metric, const weave_uint64 *allow, weave_uint32 nwarp)
{
	if (st == NULL)
		return -1;

	memset(st, 0, sizeof(*st));

	if (geom == NULL)
	{
		st->why = "no weft geometry";
		return -1;
	}

	/*
	 * The layout is refused here as well as by every kernel, not merely relied
	 * upon to be refused there.  src/vector/pack.c: a reader that guesses the
	 * layout wrong "does not fail, it returns wrong distances", and a scan that
	 * discovers that on its first block has already handed the fused loop a
	 * bound.  Refusing before the walk starts is the difference between an error
	 * and a wrong answer.
	 */
	if (geom->layout != WEAVE_PACK_LANE)
	{
		st->why = "vector weft is not in the lane pack layout";
		return -1;
	}
	if (geom->bits < WEAVE_BITS_MIN || geom->bits > WEAVE_BITS_MAX ||
		geom->dim <= 0 || geom->dim > WEAVE_MAX_DIM || geom->nblocks == 0)
	{
		st->why = "weft geometry is not scannable";
		return -1;
	}

	/*
	 * IP and L2 only.  Cosine is refused rather than approximated -- a sound
	 * cosine bound must divide by a maximum true norm when the numerator is
	 * negative and no maximum true norm is stored -- and L1 has no
	 * compressed-domain bound at all.  See the domain rule in weave/vecscan.h.
	 */
	if (metric != WEAVE_METRIC_IP && metric != WEAVE_METRIC_L2)
	{
		st->why = "metric has no compressed-domain bound in this core";
		return -1;
	}

	/*
	 * An allowlist shorter than the weft's own lane count cannot have been built
	 * for this weft.  Trimming it would score real lanes against absent bits,
	 * which is the clamp this core never does.
	 */
	if (allow != NULL && nwarp < geom->nvec)
	{
		st->why = "allowlist is shorter than the weft's lane count";
		return -1;
	}

	st->geom = geom;
	st->metric = metric;
	st->allow = allow;
	st->nwarp = allow != NULL ? nwarp : 0;
	return 0;
}

WeaveVecScanAct
weave_vec_scan_block(WeaveVecScanState *st, weave_uint32 blockno,
					 const WeaveVecDirRec *rec, const WeaveQueryLut *lut,
					 const weave_uint8 *cencode, float threshold,
					 float *bound_out)
{
	weave_uint32 avail;
	int			nlanes;
	float		bound;

	if (st == NULL)
		return WEAVE_VSCAN_REFUSE;
	if (rec == NULL || lut == NULL || cencode == NULL || bound_out == NULL ||
		st->geom == NULL)
	{
		st->why = "null argument";
		return WEAVE_VSCAN_REFUSE;
	}

	if (blockno >= st->geom->nblocks)
	{
		st->why = "block number past the end of the weft";
		return WEAVE_VSCAN_REFUSE;
	}
	nlanes = weave_vecweft_block_lanes(st->geom, blockno);
	if (nlanes <= 0 || nlanes > WEAVE_VEC_BLOCK)
	{
		st->why = "block covers no lanes";
		return WEAVE_VSCAN_REFUSE;
	}

	/*
	 * Every float in the record is validated again even though the page reader
	 * validated it on the way in.  It is five comparisons plus 64, this is the
	 * only place they are USED as a bound, and the alternative is a core whose
	 * soundness depends on a caller it cannot see.
	 */
	if (!weave_vecdir_floats_ok(rec))
	{
		st->why = "directory record carries a float that cannot be a bound";
		return WEAVE_VSCAN_REFUSE;
	}

	/*
	 * (C1), both halves.  The formula half is what makes firstwarp an index into
	 * the allowlist rather than a number off a page; the ascent half is what (C1)
	 * actually says.  Task V13 assigns warps in cluster order and will have to
	 * lift the formula half -- the ascent half must survive that.
	 */
	if (rec->firstwarp != blockno * (weave_uint32) WEAVE_VEC_BLOCK)
	{
		st->why = "block's firstwarp is not blockno * 32";
		return WEAVE_VSCAN_REFUSE;
	}
	if (st->started && rec->firstwarp <= st->lastwarp)
	{
		st->why = "firstwarp did not ascend";
		return WEAVE_VSCAN_REFUSE;
	}

	/*
	 * The lanes this block would probe must lie inside the bitmap.  Rejected, not
	 * clamped: clamping scores lanes against bits belonging to other warps
	 * (weave/kernels.h).  The arithmetic is 64-bit because firstwarp + nlanes is
	 * a uint32 sum that a corrupt firstwarp could wrap.
	 *
	 * Today this is implied by the two checks above plus begin()'s length check --
	 * firstwarp == blockno * 32 and blockno < nblocks give firstwarp + nlanes <=
	 * nvec <= nwarp -- so it can only fire once V13 lifts the formula half.  It is
	 * here anyway because the thing it protects is an out-of-bounds read, and a
	 * bounds check that is only correct because of a check three branches up is a
	 * bounds check waiting to be deleted.
	 */
	if (st->allow != NULL &&
		(weave_uint64) rec->firstwarp + (weave_uint64) nlanes >
		(weave_uint64) st->nwarp)
	{
		st->why = "block's lanes run past the end of the allowlist";
		return WEAVE_VSCAN_REFUSE;
	}

	st->why = NULL;
	st->nblk_seen++;
	st->started = 1;
	st->lastwarp = rec->firstwarp;

	/*
	 * Outcome 1, and the only one that is free: one AND.  No code byte is read --
	 * this function is not even given the codes -- no strip is scattered and no
	 * LUT pass over the centroid happens either, because the bound comes after
	 * this branch and not before it.
	 *
	 * A block whose livemask is empty lands here too, with or without an
	 * allowlist.  That is not a corruption (vacuum can empty a block and
	 * weave_vecdir_read() returns such a record with a note rather than an error),
	 * and it is the same decision for the same reason: there is nothing to score.
	 */
	avail = weave_lane_avail_mask(rec->livemask, nlanes, st->allow,
								  rec->firstwarp);
	if (avail == 0)
	{
		st->nblk_mask++;
		return WEAVE_VSCAN_SKIP_MASK;
	}

	if (scan_bound(st, rec, lut, cencode, &bound) != 0)
	{
		st->why = "block bound is not a number";
		return WEAVE_VSCAN_REFUSE;
	}
	*bound_out = bound;

	/*
	 * Outcome 2.  `bound <= threshold` and not `<`: a bounded top-k rejects on
	 * s <= theta, so a block that can only equal the floor cannot displace an
	 * incumbent.  With threshold == -INFINITY this is false for every finite
	 * bound, which is how a caller disables pruning without disabling the bound.
	 */
	if (bound <= threshold)
	{
		st->nblk_bound++;
		return WEAVE_VSCAN_SKIP_BOUND;
	}

	st->nblk_score++;
	st->nlane_score += (weave_uint64) lane_popcount(avail);
	return WEAVE_VSCAN_SCORE;
}

void
weave_vec_scan_scoreblk(WeaveScoreBlock *blk, const WeaveVecScanState *st,
						const WeaveVecDirRec *rec, const WeaveQueryLut *lut,
						const weave_uint8 *codes, int nlanes)
{
	if (blk == NULL)
		return;

	memset(blk, 0, sizeof(*blk));
	if (st == NULL || st->geom == NULL || rec == NULL)
		return;

	blk->lut = lut;
	blk->layout = (WeavePackLayout) st->geom->layout;
	blk->codes = codes;

	/*
	 * The stride is 2 because the record stores the (scale, norm) pairs
	 * INTERLEAVED and the kernel interface takes a base pointer plus a stride.
	 * Passing 1 would feed the kernel norms as scales on every other lane, and it
	 * would not fail -- it would return wrong distances (src/vector/pack.c).  That
	 * is the whole reason this conversion is a function instead of nine
	 * assignments at each call site.
	 */
	blk->scales = &rec->lane[0];
	blk->scalestride = 2;

	blk->nlanes = nlanes;
	blk->livemask = rec->livemask;
	blk->firstwarp = rec->firstwarp;
	blk->allow = st->allow;
	blk->nwarp = st->nwarp;
}

float
weave_vec_scan_lane_score(const WeaveVecScanState *st, const WeaveQueryLut *lut,
						  float ip, float norm)
{
	/*
	 * The sentinel survives arithmetic-free.  For L2 the algebra below would
	 * happen to carry -inf through, but "happens to" is not a contract: a dead or
	 * masked lane must stay a lane that can never contribute, and a future domain
	 * with a bounded transform must not quietly turn it into a finite number.
	 */
	if (ip == WEAVE_KERNEL_NEVER)
		return WEAVE_KERNEL_NEVER;

	if (st == NULL || lut == NULL)
		return WEAVE_KERNEL_NEVER;

	switch (st->metric)
	{
		case WEAVE_METRIC_IP:
			return ip;
		case WEAVE_METRIC_L2:

			/*
			 * -||q - v||^2 = -||q||^2 + 2<q,v> - ||v||^2, with ||v|| the lane's
			 * STORED norm.  The kernel is not given the norms
			 * (weave/kernels.h: "the norms are not here because scoring does not
			 * need them"), so this is the only place the L2 domain can be
			 * reached, and weave_block_bound_l2() bounds this same expression
			 * with the block's minnorm substituted for ||v||.
			 */
			return -lut->qnorm2 + 2.0f * ip - norm * norm;
		default:

			/*
			 * Unreachable: weave_vec_scan_begin() refused any other metric.  It
			 * fails closed rather than returning `ip` in a domain nobody asked
			 * for, because a wrong-domain score is the exact bug the domain rule
			 * exists to prevent.
			 */
			return WEAVE_KERNEL_NEVER;
	}
}

void
weave_vec_scan_maxscore_fold(float *acc, const WeaveVecDirRec *rec)
{
	if (acc == NULL || rec == NULL)
		return;

	/*
	 * Bound (B2): max over blocks of maxrecnorm, times ||q|| once at the end.
	 * (B3) would be tighter and needs every centroid CODE, which lives on the
	 * code pages -- a full pass over the weft before the scan starts, to tighten a
	 * value whose only job is the MaxScore partition (doc/specs/FUSED_TOPK.md
	 * sect. 5).
	 *
	 * A record whose maxrecnorm is a NaN leaves the accumulator alone rather than
	 * poisoning it, because `NaN > acc` is false.  That is the safe direction only
	 * because the same record is refused by weave_vec_scan_block() the moment it
	 * is scanned; the fold has no `why` to report through, so it must not be the
	 * place that decides.
	 */
	if (rec->maxrecnorm > *acc)
		*acc = rec->maxrecnorm;
}

float
weave_vec_scan_maxscore(const WeaveVecScanState *st, const WeaveQueryLut *lut,
						float acc)
{
	float		ip;

	if (st == NULL || lut == NULL)
		return (float) INFINITY;

	ip = acc * lut->qnorm;

	switch (st->metric)
	{
		case WEAVE_METRIC_IP:
			return ip;
		case WEAVE_METRIC_L2:

			/*
			 * Norm 0 substituted for the lane's stored norm: it is the only value
			 * guaranteed not to exceed any lane's, since a stored norm is
			 * validated non-negative, and -||v||^2 is decreasing in ||v||.  Every
			 * block bound is therefore dominated -- weave_block_bound_l2()'s
			 * minnorm^2 term is >= 0 and its inner-product half is <= this acc *
			 * qnorm, and both operations are monotone in float.
			 */
			return -lut->qnorm2 + 2.0f * ip;
		default:

			/*
			 * Unreachable, and +inf is the fail-closed direction for a ceiling:
			 * it disables the MaxScore partition instead of dropping rows.
			 */
			return (float) INFINITY;
	}
}
