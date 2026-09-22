/*-------------------------------------------------------------------------
 *
 * vecdocmap.c
 *		Task F8: the vector channel's relabelling into the fused docid space.
 *
 * The contract, the (C1)/(C2) arguments and the reason this adapts a
 * WeaveFuseChan rather than a WeaveShuttle are all in include/weave/vecdocmap.h.
 * This file is BACKEND-FREE for the same reason src/am/fuse.c is: it is linked by
 * test/hegel/test_vecdocmap.c with a bare compiler at 10^6 random cases, which is
 * the only kind of test that can see a monotonicity bug (hard rule 1).
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/am/vecdocmap.c
 *
 *-------------------------------------------------------------------------
 */
#include "weave/vecdocmap.h"

/*
 * (C1) seek, in the docid space.
 *
 * THE INNER CHANNEL'S `cur` IS MAINTAINED HERE, and forgetting it would be the
 * one bug this file can have that nothing else would notice.  fuse.h says `cur`
 * is "the core's memory of what it was given back" -- but the core does not know
 * the inner channel exists, so nobody else is positioned to write it, and
 * src/am/fuseshuttle.c's score thunk puts the SHUTTLE on chan->cur before
 * scoring.  Leave it at zero and every score() would be taken at lane 0.
 */
static weave_ft_uint32
vecdoc_seek(WeaveFuseChan *c, weave_ft_uint32 target)
{
	WeaveVecDocChan *a = (WeaveVecDocChan *) c->state;
	weave_ft_uint32 lo;
	weave_ft_uint32 l;
	weave_ft_uint32 last;

	/*
	 * END IS LATCHED, AND LEAVING THIS OUT IS A BUG THE PROPERTY TEST FOUND.
	 *
	 * The core may ask an exhausted channel again -- targets only ever increase,
	 * which is all fuse.h note 1 promises -- and `lo < nlane` does NOT imply the
	 * inner channel can still be asked.  The two spaces run out independently: the
	 * inner stops when it has no contribution left, which can happen while plenty
	 * of lanes (and therefore plenty of docids >= the target) remain.  Calling the
	 * inner after it has returned END then hands it a target BELOW the position it
	 * last returned, and every real shuttle refuses that -- src/vector/vecshuttle.c
	 * raises "cannot resolve warp %u after warp %u", so the symptom would be an
	 * ERROR mid-scan on a perfectly good index rather than a wrong answer.
	 */
	if (a->inner->cur == WEAVE_FUSE_END)
		return WEAVE_FUSE_END;

	lo = weave_vecdoc_lower_bound(a->docid, a->nlane, (weave_ft_uint64) target);
	if (lo >= a->nlane)
		return WEAVE_FUSE_END;	/* every lane of this bolt is behind the target */

	l = a->inner->ops->seek(a->inner, lo);
	a->inner->cur = l;
	if (l == WEAVE_FUSE_END || l >= a->nlane)
	{
		/*
		 * `l >= nlane` folded into the END case rather than reported as
		 * corruption.  The lane space is dense over [0, nlane) by construction, so
		 * a lane past the end means the inner channel ran out; a shuttle that
		 * returned a real position past its own warp would already have violated
		 * its own contract, and the honest thing HERE is to stop rather than index
		 * docid[] out of bounds on the way to complaining about it.
		 */
		a->inner->cur = WEAVE_FUSE_END;
		return WEAVE_FUSE_END;
	}

	a->lane = l;

	/*
	 * blkend, CLAMPED TO THE LAST LANE.  The inner channel's blkend is the last
	 * lane of the fixed-size block it stands on, and the final block of a weft is
	 * usually a short tail -- so an unclamped read would run off docid[] on every
	 * bolt whose lane count is not a multiple of the block size, which is nearly
	 * all of them.  Clamping is also correct rather than merely safe: a lane that
	 * does not exist carries no score, so shrinking the interval to the lanes that
	 * do exist keeps (C2) and narrows nothing the channel can contribute at.
	 */
	last = a->inner->blkend;
	if (last >= a->nlane)
		last = a->nlane - 1;
	if (last < l)
		last = l;				/* a degenerate per-document bound, per fuse.h */
	c->blkend = (weave_ft_uint32) a->docid[last];

	return (weave_ft_uint32) a->docid[l];
}

/* (C2)/(C3) the bound, unchanged: a relabelling of positions does not touch
 * scores, and the interval it describes is argued in the header. */
static float
vecdoc_block_max(WeaveFuseChan *c)
{
	WeaveVecDocChan *a = (WeaveVecDocChan *) c->state;

	return a->inner->ops->block_max(a->inner);
}

/* (C4) the exact score, at the LANE that chan->cur denotes. */
static float
vecdoc_score(WeaveFuseChan *c)
{
	WeaveVecDocChan *a = (WeaveVecDocChan *) c->state;

	a->inner->cur = a->lane;
	return a->inner->ops->score(a->inner);
}

static const WeaveFuseChanOps vecdoc_chan_ops = {
	vecdoc_seek, vecdoc_block_max, vecdoc_score
};

int
weave_vecdoc_chan_init(WeaveVecDocChan *a, WeaveFuseChan *inner,
					   const weave_ft_uint64 *docid, weave_ft_uint32 nlane,
					   const char **why)
{
	weave_ft_uint32 i;

	if (why != NULL)
		*why = NULL;
	if (a == NULL || inner == NULL || docid == NULL)
	{
		if (why != NULL)
			*why = "vector docid adapter given a NULL argument";
		return -1;
	}
	if (nlane == 0)
	{
		if (why != NULL)
			*why = "vector weft has no lanes";
		return -1;
	}

	/*
	 * THE STRICTLY-ASCENDING CHECK, over the whole array.  O(nlane) on a pass that
	 * already touched every entry to build it, so it is free in practice, and it is
	 * the premise of every inequality in this file's header.
	 *
	 * `<=` AND NOT `<`, i.e. a duplicate is refused too, and the header records at
	 * length why: an earlier version of this file accepted equal neighbours on an
	 * argument that the property test refuted in both (C2) and (C4).  A docid with
	 * two lanes is not a relabelling but an aggregation.
	 */
	for (i = 1; i < nlane; i++)
		if (docid[i] <= docid[i - 1])
		{
			if (why != NULL)
				*why = "vector warp map is not in strictly ascending docid order";
			return -1;
		}

	/* The maximum, which the loop above has just proved is the last element. */
	if (docid[nlane - 1] >= (weave_ft_uint64) WEAVE_FUSE_END)
	{
		if (why != NULL)
			*why = "document id exceeds the fused scan's 32-bit position space";
		return -1;
	}

	/*
	 * Everything the core reads off a channel is copied, not forwarded: it reads
	 * these as struct fields rather than through the ops table, so an adapter that
	 * left them zero would give the vector channel a zero weight (which
	 * weave_fuse_init() refuses) or a zero ceiling (which would put it last in the
	 * partition and let the non-essential prune skip it entirely -- a silently
	 * dropped channel).
	 */
	a->chan.ops = &vecdoc_chan_ops;
	a->chan.cur = 0;
	a->chan.blkend = 0;
	a->chan.maxscore = inner->maxscore;
	a->chan.weight = inner->weight;
	a->chan.required = inner->required;
	a->chan.state = a;
	a->chan.nseek = 0;
	a->chan.nscore = 0;

	a->inner = inner;
	a->docid = docid;
	a->nlane = nlane;
	a->lane = 0;
	return 0;
}
