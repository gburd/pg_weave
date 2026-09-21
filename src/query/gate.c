/*-------------------------------------------------------------------------
 *
 * gate.c
 *		The boolean-gate shuttle's backend half: a WeaveShuttleOps skin over
 *		the header-only cursor in include/weave/gate.h.
 *
 * Task Z7.  Everything that decides anything -- validation, the galloping
 * seek, the backward refusal -- is in the header so that contracts (C1), (C2)
 * and (C5) can be property-tested at scale with a bare compiler
 * (test/hegel/test_bounds.c).  This file allocates, copies the keys, turns each
 * core error code into an ereport(), and hands out infinities.  It has no
 * caller in the query path yet; see "WHO CALLS THIS" in gate.h.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/query/gate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/float.h"
#include "utils/memutils.h"

#include "weave/am.h"
#include "weave/channel.h"
#include "weave/gate.h"

/* The core's sentinel must be the contract's, or seek() would return a value
 * the scorer does not recognise as the end. */
StaticAssertDecl(WEAVE_GATE_END == WEAVE_WARP_END,
				 "WEAVE_GATE_END must equal WEAVE_WARP_END");

typedef struct GateShuttle
{
	WeaveShuttle sh;
	MemoryContext ctx;
	uint64	   *keys;			/* our copy; the cursor borrows it */
	WeaveGateCursor cur;
} GateShuttle;

/*
 * (C1).  The core does the work; this is the ereport() V8's shuttle raises for
 * the same condition, with the same wording, so a fused-loop bug reads the same
 * whichever channel catches it.
 */
static WeaveWarp
gate_shuttle_seek(WeaveShuttle *s, WeaveWarp target)
{
	GateShuttle *gs = (GateShuttle *) s->state;
	weave_gt_uint32 out = WEAVE_GATE_END;
	WeaveGateError rc;

	rc = weave_gate_seek(&gs->cur, (weave_gt_uint32) target, &out);
	if (rc == WEAVE_GATE_BACKWARD)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("weave gate shuttle cannot seek backwards"),
				 errdetail("Target warp %u is below the current position %u.",
						   (unsigned) target, (unsigned) s->cur)));
	Assert(rc == WEAVE_GATE_OK);

	/* Degenerate block: the seek IS the skip (gate.h, "WHY THE BLOCK IS
	 * DEGENERATE"). */
	s->cur = (WeaveWarp) out;
	s->blkend = (WeaveWarp) out;
	return s->cur;
}

/* (C5) via (C2)/(C3): +INF while a key remains, -INF once none does.  Reads
 * two integers the cursor already holds; no buffer, no arithmetic. */
static float4
gate_shuttle_block_max(WeaveShuttle *s)
{
	GateShuttle *gs = (GateShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END || !weave_gate_block_may_match(&gs->cur))
		return WEAVE_SCORE_NEVER;
	return WEAVE_SCORE_ALWAYS;
}

/* (C5) via (C4): 0.0 at a key, -INF anywhere else.  s->cur is a key whenever
 * the cursor is on one, because seek() only ever lands on keys. */
static float4
gate_shuttle_score(WeaveShuttle *s)
{
	GateShuttle *gs = (GateShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END ||
		!weave_gate_matches(&gs->cur, (weave_gt_uint32) s->cur))
		return WEAVE_SCORE_NEVER;
	return 0.0f;
}

/* One context, one delete -- the V8 argument: an ERROR between begin() and
 * end() reclaims the copy at abort without this running at all. */
static void
gate_shuttle_end(WeaveShuttle *s)
{
	GateShuttle *gs = (GateShuttle *) s->state;

	MemoryContextDelete(gs->ctx);
}

static const WeaveShuttleOps gate_shuttle_ops = {
	gate_shuttle_seek,
	gate_shuttle_block_max,
	gate_shuttle_score,
	NULL,						/* score_block: one position per block, so
								 * the per-position path is already the
								 * bulk path */
	NULL,						/* set_visit_filter: graph-only */
	gate_shuttle_end
};

static const char *
gate_error_text(WeaveGateError rc)
{
	switch (rc)
	{
		case WEAVE_GATE_UNSORTED:
			return "is below its predecessor";
		case WEAVE_GATE_DUPLICATE:
			return "duplicates its predecessor";
		case WEAVE_GATE_TOO_WIDE:
			return "does not fit a 32-bit warp position";
		default:
			return "is invalid";
	}
}

WeaveShuttle *
weave_gate_shuttle_begin(WeaveChannelKind kind, const uint64 *keys, int nkeys,
						 MemoryContext cxt)
{
	MemoryContext ctx;
	GateShuttle *gs;
	weave_gt_uint32 bad = 0;
	WeaveGateError rc;

	if (nkeys < 0 || (nkeys > 0 && keys == NULL))
		elog(ERROR, "weave gate shuttle needs a key array");
	if (kind != WEAVE_CH_FUZZY && kind != WEAVE_CH_REGEX &&
		kind != WEAVE_CH_DOCVALS && kind != WEAVE_CH_CGRAM)
		elog(ERROR, "weave gate shuttle cannot carry channel kind %d",
			 (int) kind);

	/*
	 * Validate BEFORE allocating, and name the key: a producer that hands over
	 * an unsorted or over-wide set has a bug upstream of here, and "key 4017 of
	 * 9000 is 4294967296" is what finds it.  The width case is the one gate.h
	 * says Phase F will eventually meet.
	 */
	rc = weave_gate_check_keys(keys, (weave_gt_uint32) nkeys, &bad);
	if (rc != WEAVE_GATE_OK)
		ereport(ERROR,
				(errcode(rc == WEAVE_GATE_TOO_WIDE ?
						 ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE :
						 ERRCODE_DATA_CORRUPTED),
				 errmsg("weave gate shuttle refuses its key set"),
				 errdetail("Key %u of %d (" UINT64_FORMAT ") %s.",
						   (unsigned) bad, nkeys, keys[bad],
						   gate_error_text(rc))));

	ctx = AllocSetContextCreate(cxt, "weave gate shuttle",
								ALLOCSET_SMALL_SIZES);
	gs = (GateShuttle *) MemoryContextAllocZero(ctx, sizeof(GateShuttle));
	gs->ctx = ctx;
	if (nkeys > 0)
	{
		MemoryContext old = MemoryContextSwitchTo(ctx);

		/* A regex that matches every term matches every document, so this is
		 * corpus-scale: 8 bytes x nkeys crosses MaxAllocSize at 128 M rows. */
		gs->keys = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) nkeys * sizeof(uint64));
		memcpy(gs->keys, keys, (Size) nkeys * sizeof(uint64));
		MemoryContextSwitchTo(old);
	}
	weave_gate_init(&gs->cur, gs->keys, (weave_gt_uint32) nkeys);

	gs->sh.ops = &gate_shuttle_ops;
	gs->sh.kind = kind;
	gs->sh.cur = 0;
	gs->sh.blkend = 0;
	gs->sh.weight = 1.0f;
	gs->sh.maxscore = nkeys > 0 ? WEAVE_SCORE_ALWAYS : WEAVE_SCORE_NEVER;

	/* (C5): this IS the reference predicate channel, so it declares itself one.
	 * The fused scorer reads this field and not `kind` -- src/query/edist.c builds
	 * a SCORED shuttle under WEAVE_CH_FUZZY, so the kind cannot carry the
	 * contract.  See the field's comment in include/weave/channel.h. */
	gs->sh.required = true;

	gs->sh.state = gs;
	return &gs->sh;
}

WeaveShuttle *
weave_gate_shuttle_from_tidset(WeaveChannelKind kind, const TidSet *ts,
							   MemoryContext cxt)
{
	uint64	   *keys;
	WeaveShuttle *s;
	int			i;

	if (ts == NULL || ts->n < 0 || (ts->n > 0 && ts->tids == NULL))
		elog(ERROR, "weave gate shuttle needs a TID set");

	/* The docids come out ascending because the TidSet is TID-sorted and
	 * weave_tid_to_docid() is monotone in (block, offset); begin() checks
	 * rather than trusts that. */
	keys = ts->n > 0 ?
		(uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) ts->n * sizeof(uint64)) : NULL;
	for (i = 0; i < ts->n; i++)
		keys[i] = weave_tid_to_docid(&ts->tids[i]);
	s = weave_gate_shuttle_begin(kind, keys, ts->n, cxt);
	if (keys != NULL)
		pfree(keys);
	return s;
}
