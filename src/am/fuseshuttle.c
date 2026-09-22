/*-------------------------------------------------------------------------
 *
 * fuseshuttle.c
 *		The backend face of the fused scorer: WeaveShuttle -> WeaveFuseChan.
 *
 * src/am/fuse.c is backend-free by design (see include/weave/fuse.h), so
 * something has to carry a WeaveShuttle's vtable across that seam.  This is it,
 * and it contains no policy beyond one mapping and one refusal.  The pattern is
 * src/query/gate.c, which does the same for the boolean-gate cursor.
 *
 * WHO CALLS THIS.  Nothing yet, on purpose, exactly as with gate.c: there is no
 * planner support to build a fuse() scan (task F2, blocked under the rule-7
 * waiver in doc/PHASES.md), and inventing a SQL probe so this file has a caller
 * would be more surface than the value warrants.  Its first real caller is F2.
 * What it buys today is that the kind -> (C5) mapping exists in ONE place, so the
 * scorer and weave_gate_shuttle_begin() cannot come to disagree about which
 * channels are predicates.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/am/fuseshuttle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/channel.h"
#include "weave/fuse.h"

/* The core's sentinel and channel.h's must be the same integer, or a scan would
 * end at a real position or run past the warp.  Both are 0xFFFFFFFF; this is the
 * assertion fuse.h promises. */
StaticAssertDecl(WEAVE_FUSE_END == WEAVE_WARP_END,
				 "WEAVE_FUSE_END must equal WEAVE_WARP_END");

/*
 * THERE IS NO KIND -> PREDICATE MAPPING HERE, AND THE FIRST DRAFT HAD ONE.
 *
 * It switched on WeaveChannelKind and called FUZZY, REGEX, DOCVALS and CGRAM
 * predicates -- the four kinds weave_gate_shuttle_begin() accepts.  That is wrong
 * TODAY, not hypothetically: src/query/edist.c builds the Z9 `<@>` KNN shuttle,
 * which is SCORED (it returns a Levenshtein distance), and labels it
 * WEAVE_CH_FUZZY because that is the weft it walks.  The mapping would have given
 * that channel a conjunctive veto and silently dropped every row it ranked -- the
 * exact failure mode fuse.h note 2 exists to prevent, arriving through the fix for
 * it.
 *
 * So the contract lives on the shuttle (WeaveShuttle.required, channel.h) where
 * the channel that knows the answer sets it, and this file just copies the field.
 * A kind is a description of a weft; it is not a contract.
 */

/*
 * The three forwarding thunks.  They call the shuttle's RAW ops rather than the
 * weighted convenience wrappers in channel.h, because the core applies the
 * weight; calling the wrappers here would apply it twice, and because it also
 * scales the bound, the result would still satisfy (C2) and simply return wrong
 * scores.  That is the undetectable kind, so it is stated at both ends.
 */
static weave_ft_uint32
fuse_sh_seek(WeaveFuseChan *c, weave_ft_uint32 target)
{
	WeaveShuttle *s = (WeaveShuttle *) c->state;
	WeaveWarp	p = s->ops->seek(s, (WeaveWarp) target);

	/* The shuttle owns blkend; the core only ever raises it to cur.  Copy it on
	 * every seek, which is the obligation channel.h's struct comment describes
	 * and the one a hand-written thunk is most likely to forget. */
	c->blkend = s->blkend;
	return (weave_ft_uint32) p;
}

static float
fuse_sh_block_max(WeaveFuseChan *c)
{
	WeaveShuttle *s = (WeaveShuttle *) c->state;

	return s->ops->block_max(s);
}

static float
fuse_sh_score(WeaveFuseChan *c)
{
	WeaveShuttle *s = (WeaveShuttle *) c->state;

	/* The shuttle scores at ITS cur, and the core moved it there through
	 * fuse_sh_seek, so keep the two in step before asking. */
	s->cur = (WeaveWarp) c->cur;
	return s->ops->score(s);
}

static const WeaveFuseChanOps fuse_shuttle_ops = {
	fuse_sh_seek, fuse_sh_block_max, fuse_sh_score
};

void
weave_fuse_wrap_shuttles(WeaveFuseChan *chan, WeaveShuttle **ss, int ns)
{
	int			i;

	if (ns < 1 || ns > WEAVE_FUSE_MAX_CHAN)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fused scan supports 1 to %d channels, got %d",
						WEAVE_FUSE_MAX_CHAN, ns)));

	for (i = 0; i < ns; i++)
	{
		WeaveShuttle *s = ss[i];

		/*
		 * Refused here rather than in the core, so the message can name the
		 * channel kind.  A weight of zero is not merely useless: on a gate's +INF
		 * ceiling it is 0 * INF = NaN, and a NaN threshold comparison is false, so
		 * every prune would quietly switch itself off (fuse.h note 3).
		 */
		if (!isfinite(s->weight) || s->weight <= 0.0f)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("channel weight must be finite and greater than zero"),
					 errdetail("Channel %d (%s) has weight %g.", i,
							   weave_channel_kind_name(s->kind),
							   (double) s->weight)));

		chan[i].ops = &fuse_shuttle_ops;
		chan[i].state = s;
		chan[i].cur = 0;
		chan[i].blkend = 0;
		chan[i].maxscore = s->maxscore;
		chan[i].weight = s->weight;
		chan[i].required = s->required ? 1 : 0;
		chan[i].nseek = 0;
		chan[i].nscore = 0;
	}
}

void
weave_fuse_error(const WeaveFuseState *st, WeaveFuseError err)
{
	const char *kind = "unknown";

	if (st->badchan != NULL)
	{
		WeaveShuttle *s = (WeaveShuttle *) st->badchan->state;

		kind = weave_channel_kind_name(s->kind);
	}

	switch (err)
	{
		case WEAVE_FUSE_OK:
			return;

		case WEAVE_FUSE_C1_VIOLATION:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("weave channel violated the monotone-seek contract"),
					 errdetail("Channel kind %s returned a position below the "
							   "seek target.", kind),
					 errhint("This is contract (C1) in include/weave/channel.h. "
							 "The index is not necessarily damaged; the channel "
							 "implementation is wrong.")));
			break;

		case WEAVE_FUSE_C2_VIOLATION:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("weave channel's block bound is below its score"),
					 errdetail("Channel kind %s scored above the upper bound it "
							   "reported for the block.", kind),
					 errhint("This is contract (C2) in include/weave/channel.h. "
							 "A bound that is too low silently drops rows.")));
			break;

		case WEAVE_FUSE_C2_ABANDON:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("weave fused scan abandoned a document it should have kept"),
					 errdetail("Incremental abandonment discarded a document on a "
							   "sum of bounds, but the actual scores put it above "
							   "the threshold; channel kind %s scored furthest "
							   "above the bound it reported.", kind),
					 errhint("This is contract (C2) in include/weave/channel.h, "
							 "seen through the one prune the per-score check "
							 "cannot audit. A bound that is too low silently "
							 "drops rows. Only reachable with "
							 "pg_weave.fuse_check_bounds on.")));
			break;

		case WEAVE_FUSE_NAN_SCORE:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("weave channel returned NaN"),
					 errdetail("Channel kind %s returned a score or bound that "
							   "is not a number.", kind)));
			break;

		case WEAVE_FUSE_BAD_WEIGHT:
		case WEAVE_FUSE_BAD_MAXSCORE:
		case WEAVE_FUSE_BAD_K:
		case WEAVE_FUSE_BAD_NCHAN:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("fused scan rejected its inputs (code %d)",
							(int) err),
					 errdetail("Channel kind %s.", kind)));
			break;
	}

	/* Not reached, but a fused scan that fell through must not continue as if it
	 * had succeeded. */
	elog(ERROR, "unrecognized fused-scan error %d", (int) err);
}
