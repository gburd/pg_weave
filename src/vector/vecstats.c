/*-------------------------------------------------------------------------
 *
 * vecstats.c
 *		The five bound fields of a block, computed once and in one place.
 *
 * SEPARATE FROM src/vector/vecpage.c ON PURPOSE.  vecpage.c is page geometry and
 * depends on nothing but <string.h>, so test/hegel/test_vecpage.c links it alone.
 * This file needs the codec (weave_encode/weave_decode) and libm, and folding it
 * into vecpage.c dragged both into every test and every future caller of the page
 * layout -- which is how a "pure geometry" translation unit stops being pure.
 *
 * Why the computation is a function rather than a loop at each write site:
 * contract (C2) in include/weave/channel.h requires block_max() >= score() for
 * every position in the block, and a bound 1 % too low silently drops rows -- the
 * answers stay plausible, so no fixed-output regression test can see it (AGENTS.md
 * hard rule 1).  Insert, merge and vacuum all mutate blocks, and three independent
 * transcriptions of these five formulas is three chances to get one wrong.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vecstats.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "weave/vecpage.h"

/* ---------------------------------------------------------------------------
 * Block statistics: the five bound fields, computed once
 *
 * See the declaration in weave/vecpage.h for the two traps.  Both produce a
 * TIGHTER bound, which is why they are correctness bugs and not quality ones:
 * contract (C2) requires block_max() >= score() for every position, and a bound
 * 1 % low drops rows while leaving the answers plausible.
 * ------------------------------------------------------------------------- */
int
weave_vecblock_stats(WeaveVecDirRec *rec, const WeaveQuantizer *q,
					 const float *recon, const float *lane_scale,
					 const float *lane_norm, const int *slot,
					 int nlive, weave_uint32 firstwarp,
					 float *cen, weave_uint8 *cencode)
{
	int			i,
				j,
				dim;
	float		censcale = 0;
	double		maxrad = 0;
	float	   *chat;

	if (rec == NULL || q == NULL || recon == NULL || lane_scale == NULL ||
		lane_norm == NULL || slot == NULL || cen == NULL || cencode == NULL)
		return -1;
	if (nlive < 1 || nlive > WEAVE_VEC_BLOCK)
		return -1;
	dim = q->dim;
	if (dim <= 0)
		return -1;

	memset(rec, 0, sizeof(*rec));
	rec->firstwarp = firstwarp;

	/* livemask and the per-lane pairs, indexed by LANE rather than by the packed
	 * order the caller passed them in. */
	for (i = 0; i < nlive; i++)
	{
		int			s = slot[i];

		if (s < 0 || s >= WEAVE_VEC_BLOCK)
			return -1;
		if (!(lane_scale[i] == lane_scale[i]) || lane_scale[i] < 0.0f)
			return -1;			/* NaN or negative scale is not a bound input */
		if (!(lane_norm[i] == lane_norm[i]) || lane_norm[i] < 0.0f)
			return -1;
		if ((rec->livemask & (1u << s)) != 0)
			return -1;			/* two entries claim the same lane */
		rec->livemask |= 1u << s;
		rec->lane[2 * s] = lane_scale[i];
		rec->lane[2 * s + 1] = lane_norm[i];
	}

	/*
	 * smax feeds bound (B1), minnorm the L2 bound, maxrecnorm (B2).  maxrecnorm is
	 * over the RECONSTRUCTIONS' norms and is not lane_norm: lane_norm is the
	 * original vector's norm, and reconstruction can lengthen it, so using
	 * lane_norm here would be the third way to get a too-tight bound.
	 */
	rec->smax = 0;
	rec->maxrecnorm = 0;
	rec->minnorm = lane_norm[0];
	for (i = 0; i < nlive; i++)
	{
		double		rn = 0;

		if (lane_scale[i] > rec->smax)
			rec->smax = lane_scale[i];
		if (lane_norm[i] < rec->minnorm)
			rec->minnorm = lane_norm[i];
		for (j = 0; j < dim; j++)
		{
			double		t = recon[(size_t) i * dim + j];

			rn += t * t;
		}
		rn = sqrt(rn);
		if (rn > rec->maxrecnorm)
			rec->maxrecnorm = (float) rn;
	}

	/* The centroid of the reconstructions, and its code. */
	for (j = 0; j < dim; j++)
	{
		double		a = 0;

		for (i = 0; i < nlive; i++)
			a += recon[(size_t) i * dim + j];
		cen[j] = (float) (a / nlive);
	}
	if (weave_encode(q, cen, cencode, NULL, &censcale) != 0)
	{
		/*
		 * A degenerate all-zero centroid cannot be encoded.  Zero the code and
		 * give the bound a scale of 0, which makes <q,c> zero and leaves (B1)/(B2)
		 * to carry the block -- looser, never unsound.  bench/code_scan.c makes
		 * the same choice for the same reason.
		 */
		memset(cencode, 0, (size_t) q->codebytes);
		censcale = 0;
	}
	rec->censcale = censcale;

	/*
	 * The radius, measured against the DEQUANTIZED centroid.  This is trap 2:
	 * measuring against `cen` gives a smaller radius and an UNSOUND bound, because
	 * a scan only ever has the code.  Sound as written because
	 *
	 *	 <q, r_s> = <q, chat> + <q, r_s - chat> <= <q, chat> + ||q|| * ||r_s - chat||
	 *
	 * which is exactly (B3) with R = max_s ||r_s - chat||.
	 */
	chat = (float *) malloc((size_t) dim * sizeof(float));
	if (chat == NULL)
		return -1;
	weave_decode(q, cencode, censcale, chat);
	for (i = 0; i < nlive; i++)
	{
		double		d = 0;

		for (j = 0; j < dim; j++)
		{
			double		t = (double) recon[(size_t) i * dim + j] - chat[j];

			d += t * t;
		}
		d = sqrt(d);
		if (d > maxrad)
			maxrad = d;
	}
	free(chat);
	rec->cenrad = (float) maxrad;

	return weave_vecdir_floats_ok(rec) ? 0 : -1;
}
