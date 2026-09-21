/*-------------------------------------------------------------------------
 *
 * lexshuttle.c
 *		The BM25 lexical channel's WeaveShuttleOps skin: one shuttle per query
 *		TERM, over that term's posting cursor in one bolt.
 *
 * Task F6.  doc/specs/FUSED_TOPK.md sect. 7a (3) is the discovery this file
 * answers: ranked lexical retrieval in pg_weave is a Broder/BlockMax-WAND over
 * WandCursor (src/am/amscan.c), the fused core (src/am/fuse.c) consumes only
 * WeaveShuttles, and so the lexical channel -- the oldest and best-tested
 * channel in the index -- was the one channel the fused scorer could not use.
 * The adaptation is glue and not a format change: WandCursor already carries the
 * current docid, the decoded block, the block-bound inputs and the term-wide
 * ceiling, and WeaveBlockHdr already stores max_tf and min_doclen per block.
 *
 * ONE SHUTTLE PER TERM, WHICH IS THE WHOLE POINT.  include/weave/channel.h's
 * vtable comment and include/weave/fuse.h both already assume it.  A single
 * "lexical shuttle" wrapping the whole WAND would put a second top-k and a
 * second threshold inside the fused loop's threshold, which is precisely the
 * two-thresholds-and-reconcile shape (RRF with over-fetch) that fuse.h exists to
 * replace.  Per term, a three-term BM25 query is three scored channels whose
 * bounds the one fused threshold prunes against, alongside any gates.
 *
 * THIS FILE HAS NO CALLER, AND THAT IS DELIBERATE.  Exactly the arrangement
 * src/query/gate.c was merged under ("WHO CALLS THIS" in include/weave/gate.h)
 * and src/query/edist.c after it: the shuttle's first caller is the AM-side
 * fused scan, task F2.2, which is where the cursors are built and where the
 * weights arrive.  Inventing a SQL probe to give this a caller now would add
 * surface that F2.2 then has to remove.  What stands in for a caller is
 * test/hegel/test_lexbound.c, which tests the arithmetic this file's (C2) and
 * (C4) rest on -- include/weave/bm25bound.h -- with a bare compiler, and the
 * regression suite, which exercises the same arithmetic through the WAND because
 * F6 routed the WAND through the same header instead of leaving it a copy.
 *
 * WHAT IS THIN AND WHAT IS NOT.  Like gate.c, everything decidable is
 * elsewhere: the bound and the score are bm25bound.h, the traversal is amscan.c
 * behind the five weave_wand_cursor_* calls declared in include/weave/am.h.
 * This file owns three decisions and no arithmetic: the backward-seek refusal,
 * publishing blkend after every seek, and the uint64-docid -> WeaveWarp width
 * check.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/query/lexshuttle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/float.h"
#include "utils/memutils.h"

#include "weave/am.h"
#include "weave/channel.h"

/*
 * The cursor is BORROWED, not owned.  A WandCursor's block buffer, doclen
 * sidecar cursor and tombstone map all belong to the ranked scan that built the
 * cursor array (weave_search_bmw() frees them in its epilogue), so a shuttle
 * that freed any of them would be the second owner of memory that already has
 * one.  end() therefore drops this struct's context and nothing else.
 */
typedef struct LexShuttle
{
	WeaveShuttle sh;
	MemoryContext ctx;
	WandCursor *cur;			/* borrowed; see above */
	bool		seeked;			/* has seek() been called at least once? */
} LexShuttle;

/*
 * Publish a (docid, block-end) pair from the cursor as the shuttle's (cur,
 * blkend).  Called after every seek and once at begin(), because blkend is what
 * makes block_max() mean anything: (C2) bounds the CLOSED interval
 * [cur, blkend], and a stale blkend would have the bound describing the block
 * the cursor has left.  WandCursor does not store the interval's end -- it
 * stores the decoded docids -- so amscan.c derives it as the last of them.
 *
 * THE WIDTH DECISION, which include/weave/gate.h states in full and this file
 * inherits.  WeaveWarp is uint32, a weave docid is uint64
 * (block * MaxHeapTuplesPerPage + offset), and 0xFFFFFFFF is the end sentinel,
 * so a docid at or above it cannot be returned by seek() at all.  A cursor
 * position that wide is therefore an ERROR naming the docid, not a clamp: the
 * alternative is reporting a different document's position, which is a wrong
 * answer.  With 8 KiB pages that is a heap of about 113 GiB, and it is the case
 * gate.h says will force Phase F to choose the dense warp space or widen the
 * type.
 *
 * blkend is CLAMPED where cur is refused, and the asymmetry is sound rather than
 * convenient: shrinking the block only narrows the interval the bound covers, so
 * (C2) still holds and the shuttle merely skips less.  Widening or truncating
 * `cur` would move a document.
 */
static void
lex_publish(WeaveShuttle *s, uint64 docid, uint64 blkend)
{
	if (docid == UINT64_MAX)
	{
		s->cur = WEAVE_WARP_END;
		s->blkend = WEAVE_WARP_END;
		return;
	}

	if (docid >= (uint64) WEAVE_WARP_END)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("weave lexical shuttle cannot report a warp position for docid " UINT64_FORMAT,
						docid),
				 errdetail("A warp position is 32 bits and %u is the end-of-warp sentinel.",
						   (unsigned) WEAVE_WARP_END)));

	s->cur = (WeaveWarp) docid;

	/* blkend >= cur always (channel.h).  The clamp cannot break that -- cur is
	 * below WEAVE_WARP_END, hence at most WEAVE_WARP_END - 1 -- and the floor
	 * below holds it even for a block whose decoded docids were not ascending,
	 * because a blkend under cur makes the bound cover an empty interval and
	 * (C2) vacuous. */
	if (blkend >= (uint64) WEAVE_WARP_END)
		s->blkend = WEAVE_WARP_END - 1;
	else
		s->blkend = (WeaveWarp) blkend;
	if (s->blkend < s->cur)
		s->blkend = s->cur;
}

/*
 * (C1) MONOTONE SEEK.  The return is >= target and >= the previous return,
 * because the cursor underneath is forward-only: it lands on the smallest docid
 * >= target that its own segment has not tombstoned, or exhausts.
 *
 * A BACKWARD TARGET IS REFUSED, with the wording gate.c and vecshuttle.c use so
 * that a fused-loop bug reads the same whichever channel catches it.  Note that
 * wand_seek() itself TOLERATES one -- it returns the current posting when
 * `docid >= target` -- so the refusal is added here rather than inherited, and it
 * has to be: include/weave/fuse.h note 1 says the fused core relies on both real
 * shuttles refusing a backward seek, because that is what turns the spec's
 * literal pivot loop (which performs one on its second iteration) into an error
 * instead of a plausible wrong answer.  s->cur is the last position returned, so
 * comparing against it also refuses every target below the sentinel once the
 * cursor is exhausted.
 */
static WeaveWarp
lex_shuttle_seek(WeaveShuttle *s, WeaveWarp target)
{
	LexShuttle *ls = (LexShuttle *) s->state;
	uint64		blkend = UINT64_MAX;
	uint64		docid;

	if (ls->seeked && target < s->cur)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("weave lexical shuttle cannot seek backwards"),
				 errdetail("Target warp %u is below the current position %u.",
						   (unsigned) target, (unsigned) s->cur)));
	ls->seeked = true;

	/* Exhausted.  The only target that reaches here is WEAVE_WARP_END itself --
	 * every smaller one was refused just above -- which is gate.h's G5 case:
	 * after the end, the end is still a legal target and answers itself. */
	if (s->cur == WEAVE_WARP_END)
		return WEAVE_WARP_END;

	docid = weave_wand_cursor_seek(ls->cur, (uint64) target, &blkend);
	lex_publish(s, docid, blkend);
	return s->cur;
}

/*
 * (C2)+(C3) THE BLOCK BOUND.  Cheap, as (C3) demands: the block's max tf and min
 * |D| are already in the cursor from the block header decode, so this is six
 * floating-point operations over include/weave/bm25bound.h and reads no buffer.
 * It is the same function the WAND's own block-max prune calls, which is the
 * property that makes test/hegel/test_lexbound.c a test of production code.
 *
 * Exhausted: WEAVE_SCORE_NEVER, matching src/vector/vecshuttle.c.  A shuttle at
 * WEAVE_WARP_END covers no position, so no position can beat the value, and the
 * fused core never adds a non-contributing channel's bound (fuse.h note 2) --
 * which is what keeps the -inf out of the sums note 3 is about.
 */
static float4
lex_shuttle_block_max(WeaveShuttle *s)
{
	LexShuttle *ls = (LexShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END)
		return WEAVE_SCORE_NEVER;
	return (float4) weave_wand_cursor_block_max(ls->cur);
}

/*
 * (C4) EXACT SCORE at s->cur: this term's BM25 contribution to the document the
 * cursor sits on, weight NOT applied (channel.h: the fused core multiplies).
 *
 * This is the one entry point here permitted to read a buffer, and on a v4
 * segment it does: the doclen comes from the cursor's own forward doclen-sidecar
 * cursor and its resident-block cache (weave_doclen_cursor_lookup), which is the
 * path the ranked scan already uses.  A second lookup path would be a second
 * cache with its own hit rate and its own quantization, and the quantized doclen
 * is exactly what the block bound's v4 floor adjustment is matched to.
 */
static float4
lex_shuttle_score(WeaveShuttle *s)
{
	LexShuttle *ls = (LexShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END)
		return WEAVE_SCORE_NEVER;
	return (float4) weave_wand_cursor_contrib(ls->cur);
}

/* One context, one delete -- and nothing of the cursor's, per the struct's
 * comment.  An ERROR between begin() and end() reclaims this at abort. */
static void
lex_shuttle_end(WeaveShuttle *s)
{
	LexShuttle *ls = (LexShuttle *) s->state;

	MemoryContextDelete(ls->ctx);
}

static const WeaveShuttleOps lex_shuttle_ops = {
	lex_shuttle_seek,
	lex_shuttle_block_max,
	lex_shuttle_score,
	NULL,						/* score_block: a posting block is 128 postings
								 * of a SPARSE docid space, so there is no
								 * contiguous warp range to fill and no SIMD to
								 * win; the per-position path IS the cheap one */
	NULL,						/* set_visit_filter: graph-only */
	lex_shuttle_end
};

WeaveShuttle *
weave_lex_shuttle_begin(WandCursor *c, MemoryContext cxt)
{
	MemoryContext ctx;
	LexShuttle *ls;
	uint64		blkend = UINT64_MAX;
	uint64		docid;

	if (c == NULL)
		elog(ERROR, "weave lexical shuttle needs a posting cursor");

	ctx = AllocSetContextCreate(cxt, "weave lexical shuttle",
								ALLOCSET_SMALL_SIZES);
	ls = (LexShuttle *) MemoryContextAllocZero(ctx, sizeof(LexShuttle));
	ls->ctx = ctx;
	ls->cur = c;

	ls->sh.ops = &lex_shuttle_ops;
	ls->sh.kind = WEAVE_CH_LEXICAL;
	ls->sh.weight = 1.0f;

	/*
	 * The term-wide ceiling, set once because it is static for the cursor's
	 * life: it is the contribution at the dictionary's max tf with the length
	 * norm taken to zero, so maxscore >= block_max() everywhere as channel.h
	 * requires (see weave_bm25_term_bound() for why the two survive being
	 * rounded differently).
	 */
	ls->sh.maxscore = (float4) weave_wand_cursor_max_contrib(c);

	/*
	 * (C5): FALSE, and it is set explicitly rather than left to the zeroing
	 * above being enough.  This is a SCORED channel -- it ranks, it does not
	 * veto -- and channel.h's (C5) note plus FUSED_TOPK.md sect. 7a record why
	 * `required` is a field no scorer may infer from `kind`: src/query/edist.c
	 * labels a scored channel WEAVE_CH_FUZZY, so the kind cannot carry the
	 * contract in either direction.  A lexical channel wrongly marked required
	 * would turn a disjunctive BM25 query into a conjunction and silently drop
	 * every document missing any one term.
	 */
	ls->sh.required = false;

	/*
	 * The cursor arrives PRIMED (wand_prime() in src/am/amscan.c), so it already
	 * sits on its first posting, and the shuttle publishes THAT rather than
	 * claiming warp 0: a forward-only cursor cannot be rewound, so a cur of 0
	 * would be a position the channel can never be asked to score, and the first
	 * seek would jump over everything between 0 and it without the caller having
	 * been told.  A caller that has not primed the cursor gets its zeroed
	 * position, which is the one precondition this function cannot check.
	 */
	docid = weave_wand_cursor_tell(c, &blkend);
	lex_publish(&ls->sh, docid, blkend);

	ls->sh.state = ls;
	return &ls->sh;
}

void
weave_lex_shuttle_end(WeaveShuttle *s)
{
	if (s == NULL)
		return;
	if (s->kind != WEAVE_CH_LEXICAL || s->ops != &lex_shuttle_ops)
		elog(ERROR, "weave lexical shuttle end called on a %s shuttle",
			 weave_channel_kind_name(s->kind));
	s->ops->end(s);
}
