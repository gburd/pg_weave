/*-------------------------------------------------------------------------
 *
 * vecshuttle.c
 *		The vector code scan's backend half: three lockstep cursors, the
 *		WeaveShuttleOps implementation, and the SQL entry point that reaches
 *		them without a planner in the way.
 *
 * Task V8.  The policy is NOT here -- mask, then bound, then score lives in the
 * backend-free decision core (include/weave/vecscan.h, src/vector/vecscan.c) so
 * that contracts (C1) and (C2) can be property-tested at scale with a bare
 * compiler.  This file reads pages and turns every refusal into an ERROR.  It is
 * the FIRST implementation of include/weave/channel.h in the tree; where the
 * contract turned out to be under-specified that is recorded here and in
 * doc/specs/VECTOR_CHANNEL.md sect. 8b rather than worked around silently.
 *
 * THE TRAVERSAL IS THREE FORWARD-ONLY CURSORS, and that is the load-bearing
 * design decision of the task.  weave_vec_block_read() walks the ENTIRE code
 * chain on every call, because the ratified format has no block -> page index and
 * WeaveVecDirRec has no room for one (sect. 7.1).  Using it once per block is
 * O(blocks x pages), which is quadratic in the weft.  So the shuttle carries one
 * cursor over the directory chain, one over the code chain and one over the warp
 * map, advanced in lockstep, and a full scan is O(pages) as it must be.  Each
 * cursor holds one page at a time -- ReadBuffer, LockBuffer(SHARE), copy out,
 * UnlockReleaseBuffer -- and no buffer is ever pinned across a yield to the
 * caller, which is the shape weave_vec_warp_begin/next/end already had.
 *
 * THE CODE CURSOR SCATTERS LAZILY, and the exact granularity is what claim 3 in
 * doc/ARCHITECTURE.md sect. 9 rests on: a block whose live lanes do not intersect
 * the allowlist costs its chain walk and NOTHING else -- no strip is scattered
 * into a block buffer, no centroid is assembled, no kernel runs.  What it does
 * not save is the ReadBuffer: the chain has to be walked to find the next link,
 * so the mask short-circuit saves SCORING, never I/O (sect. 8b).
 *
 * WHAT DECIDES "LAZILY", though, is the mask and not the bound, and the reason is
 * a property of the on-disk order rather than a choice.  A block's strips are
 * block-major with the lane strips FIRST and the centroid strips after them
 * (weave_vecweft_strip_plan), while the bound needs the centroid.  So by the time
 * the core could tell us the bound prunes this block, its lane strips are already
 * behind the cursor.  The mask, by contrast, needs only the directory record,
 * which the directory cursor has produced already -- and the shuttle asks the
 * question with weave_lane_avail_mask(), the SAME shared inline the core and every
 * kernel use, so this is one function answering a resource question, not a second
 * copy of a policy.  A block skipped on the BOUND therefore pays its scatter.
 * That is a cost that is currently never incurred: the bound is measured to prune
 * 0.00 % of blocks (bench/RESULTS_CODE_SCAN.md) and a fused-loop driver passes
 * -INFINITY as the threshold anyway (sect. 8b).
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vecshuttle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "weave/am.h"
#include "weave/vector.h"

PG_FUNCTION_INFO_V1(weave_vec_scan);
PG_FUNCTION_INFO_V1(weave_vec_scan_stats);

/* ---------------------------------------------------------------------------
 * Cursor 1: the block directory
 *
 * The records of one page are copied out and the buffer released, so a caller may
 * hold a code page at the same time without a lock-ordering question -- the same
 * trade weave_vec_warp_next() makes.  One page is 28 records at 4 bits, so the
 * copy is 7,952 bytes per 28 blocks and the cursor is not the cost of anything.
 * ------------------------------------------------------------------------- */

typedef struct VecDirCursor
{
	const WeaveVecWeft *w;
	BlockNumber blk;			/* next chain page to read */
	uint32		page;			/* ordinal of that page in the chain */
	uint32		blockno;		/* the block the next next() will return */
	WeaveVecDirRec *buf;		/* the records of the page last read */
	int			nbuf;
	int			pos;
} VecDirCursor;

static void
dir_cur_begin(VecDirCursor *c, const WeaveVecWeft *w)
{
	c->w = w;
	c->blk = w->meta.dirstart;
	c->page = 0;
	c->blockno = 0;
	c->buf = (WeaveVecDirRec *) palloc((Size) w->geom.rpp * sizeof(WeaveVecDirRec));
	c->nbuf = 0;
	c->pos = 0;
}

static void
dir_cur_end(VecDirCursor *c)
{
	if (c->buf != NULL)
		pfree(c->buf);
	c->buf = NULL;
}

/*
 * The next directory record, in ascending block order.  Returns false with *why
 * set for any structural problem; the caller turns that into an ERROR.
 */
static bool
dir_cur_next(VecDirCursor *c, WeaveVecDirRec *out, const char **why)
{
	*why = NULL;
	if (c->blockno >= c->w->geom.nblocks)
	{
		*why = "the vector directory cursor is asked for a block past the weft";
		return false;
	}

	if (c->pos >= c->nbuf)
	{
		BlockNumber nrel = RelationGetNumberOfBlocks(c->w->index);
		Buffer		buf;
		Page		page;
		const WeaveVecDirHdr *h;
		int			nrecs;
		int			i;

		CHECK_FOR_INTERRUPTS();
		if (c->blk == InvalidBlockNumber || c->blk == WEAVE_METAPAGE_BLKNO ||
			c->blk >= nrel)
		{
			*why = "the vector directory chain ends before the weft's last block";
			return false;
		}
		buf = ReadBuffer(c->w->index, c->blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_VDIR))
		{
			UnlockReleaseBuffer(buf);
			*why = "a block on the vector directory chain is not a directory page";
			return false;
		}
		h = (const WeaveVecDirHdr *) PageGetContents(page);
		if (h->first_blockno != c->page * (uint32) c->w->geom.rpp)
		{
			/*
			 * The same cross-check weave_vec_dir_read() makes, and it matters more
			 * here: a mislinked chain would answer for the wrong 28 blocks
			 * silently, and every bound the scan derived from them would belong to
			 * somebody else's documents.
			 */
			UnlockReleaseBuffer(buf);
			*why = "a vector directory page does not start where the O(1) formula says it does";
			return false;
		}
		nrecs = (int) h->nrecs;
		if (nrecs < 1 || nrecs > c->w->geom.rpp)
		{
			UnlockReleaseBuffer(buf);
			*why = "a vector directory page declares an impossible record count";
			return false;
		}
		c->nbuf = 0;
		for (i = 0; i < nrecs; i++)
		{
			if (weave_vecdir_read(PageGetContents(page), WEAVE_VECPAGE_PAYLOAD,
								  WEAVE_VECPAGE_PAYLOAD, i, &c->buf[i], why) != 0)
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
			c->nbuf++;
		}
		c->blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
		c->page++;
		c->pos = 0;

		/*
		 * weave_vecdir_read() sets *why for a record with no live lanes while
		 * still returning success, because that is a legitimate state a vacuum can
		 * produce.  Only the return value decides, so clear it.
		 */
		*why = NULL;
	}

	*out = c->buf[c->pos++];
	c->blockno++;
	return true;
}

/*
 * The record for block `target`, skipping the ones between.  Forward-only:
 * skipping within the buffered page is free and skipping past it reads the pages
 * in between, because following the chain is the only way to find the next link.
 */
static bool
dir_cur_advance(VecDirCursor *c, uint32 target, WeaveVecDirRec *out,
				const char **why)
{
	if (target < c->blockno)
	{
		*why = "the vector directory cursor cannot go backwards";
		return false;
	}
	while (c->blockno < target)
	{
		WeaveVecDirRec junk;

		if (!dir_cur_next(c, &junk, why))
			return false;
	}
	return dir_cur_next(c, out, why);
}

/* ---------------------------------------------------------------------------
 * Cursor 2: the codes
 *
 * One strip per page, strips_per_block pages per block, block-major -- the order
 * weave_vecweft_strip_plan() defines and src/vector/vecwrite.c emits.  This cursor
 * VALIDATES that order rather than filtering on it the way weave_vec_block_read()
 * does: page k of block b must claim block b.  That is a strictly stronger check
 * and it is what makes a lockstep cursor sound, because a cursor that merely
 * filtered would silently fall out of step with the directory on a mislinked
 * chain and score one block's codes against another's bounds.
 * ------------------------------------------------------------------------- */

typedef struct VecCodeCursor
{
	const WeaveVecWeft *w;
	BlockNumber blk;			/* first strip page of block `blockno` */
	uint32		blockno;		/* the block the next advance() will assemble */
	uint32		npages;			/* pages walked, the cycle guard */
	bool	   *seen;			/* strips_per_block, reset per block */
	uint8	   *block;			/* geom.blockbytes, valid after a want=true call */
	uint8	   *cencode;		/* geom.codebytes, same */
} VecCodeCursor;

/*
 * There is no code_cur_end(): the three buffers live in the shuttle's private
 * context and are reclaimed by the one MemoryContextDelete() in end(), on the
 * error path as well as the normal one.  dir_cur_end() exists only because the
 * maxscore pass in weave_vec_shuttle_begin() runs a SECOND, transient directory
 * cursor inside that long-lived context, so its page buffer is worth returning.
 */
static void
code_cur_begin(VecCodeCursor *c, const WeaveVecWeft *w)
{
	c->w = w;
	c->blk = w->meta.codestart;
	c->blockno = 0;
	c->npages = 0;
	c->seen = (bool *) palloc0((Size) w->geom.strips_per_block * sizeof(bool));
	c->block = (uint8 *) palloc(w->geom.blockbytes);
	c->cencode = (uint8 *) palloc(w->geom.codebytes);
}

/*
 * Walk the strips of the block the cursor is sitting on, scattering them into
 * c->block / c->cencode only when `want`.
 *
 * `want == false` is the whole point of the cursor: the chain is walked, every
 * strip is still PARSED and still checked against the strip plan -- a corrupt page
 * must not be mistaken for an absent one just because nobody wanted its bytes --
 * and not one byte is scattered.  weave_vec_strip_take() is shared with
 * weave_vec_block_read() so there is exactly one implementation of the strip
 * format; two would not crash, they would return wrong distances
 * (src/vector/pack.c).
 */
static bool
code_cur_block(VecCodeCursor *c, bool want, const char **why)
{
	const WeaveVecWeftGeom *g = &c->w->geom;
	BlockNumber nrel = RelationGetNumberOfBlocks(c->w->index);
	int			nseen = 0;
	int			k;

	*why = NULL;
	if (c->blockno >= g->nblocks)
	{
		*why = "the vector code cursor is asked for a block past the weft";
		return false;
	}

	if (want)
	{
		MemSet(c->block, 0, g->blockbytes);
		MemSet(c->cencode, 0, g->codebytes);
	}
	MemSet(c->seen, 0, (Size) g->strips_per_block * sizeof(bool));

	for (k = 0; k < g->strips_per_block; k++)
	{
		Buffer		buf;
		Page		page;
		const WeaveVecStripHdr *raw;
		bool		ok;

		CHECK_FOR_INTERRUPTS();
		if (c->blk == InvalidBlockNumber || c->blk == WEAVE_METAPAGE_BLKNO ||
			c->blk >= nrel || ++c->npages > g->nstrips)
		{
			*why = "the vector code chain ends early, leaves the relation, or cycles";
			return false;
		}
		buf = ReadBuffer(c->w->index, c->blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_VCODES))
		{
			UnlockReleaseBuffer(buf);
			*why = "a block on the vector code chain is not a code page";
			return false;
		}
		raw = (const WeaveVecStripHdr *) PageGetContents(page);
		if (raw->blockno != c->blockno)
		{
			UnlockReleaseBuffer(buf);
			*why = "a vector code page does not carry the block the chain's block-major order calls for";
			return false;
		}
		ok = weave_vec_strip_take(c->w, PageGetContents(page), c->blockno,
								  want ? c->block : NULL,
								  want ? c->cencode : NULL,
								  c->seen, &nseen, why);
		c->blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
		if (!ok)
			return false;
	}

	if (nseen != g->strips_per_block)
	{
		*why = "the vector code chain is missing strips for this block";
		return false;
	}
	c->blockno++;
	return true;
}

/*
 * Assemble block `target`, walking (and validating) every block between without
 * scattering any of them.  Forward-only.
 *
 * `firstpage` is the target block's own first strip page, from its directory record
 * (G27).  When it is usable the intervening blocks are not visited at all, which is
 * the whole point: a gated fused scan scores a small fraction of the blocks and used
 * to read every page of every one it skipped.  `0` means "no pointer" and keeps the
 * original walk, so the two paths differ only in which pages are READ -- the block
 * this returns, and every validation it passes, are identical either way.
 *
 * THE JUMP IS NOT TRUSTED.  A pointer off disk could name any page, so it is bounds
 * checked here and then verified by code_cur_block()'s existing refusal of a page
 * whose header does not carry the block the block-major order calls for.  A corrupt
 * pointer is therefore a refusal, never a distance computed from another block's
 * codes.  Seeking also resets the cycle guard: `npages` counts pages this cursor has
 * read, and a seek makes the walked-page count no longer an upper bound on progress.
 */
static bool
code_cur_advance(VecCodeCursor *c, uint32 target, weave_uint32 firstpage,
				 bool want, const char **why)
{
	if (target < c->blockno)
	{
		*why = "the vector code cursor cannot go backwards";
		return false;
	}
	if (firstpage != 0 && firstpage != WEAVE_METAPAGE_BLKNO &&
		(BlockNumber) firstpage < RelationGetNumberOfBlocks(c->w->index))
	{
		c->blk = (BlockNumber) firstpage;
		c->blockno = target;
		c->npages = 0;
		return code_cur_block(c, want, why);
	}
	while (c->blockno < target)
		if (!code_cur_block(c, false, why))
			return false;
	return code_cur_block(c, want, why);
}

/* ---------------------------------------------------------------------------
 * The shuttle
 * ------------------------------------------------------------------------- */

/*
 * WeaveShuttle is the FIRST member so that one palloc holds both the public
 * handle and the private state, and end() is one MemoryContextDelete.
 */
typedef struct VecShuttle
{
	WeaveShuttle sh;
	MemoryContext ctx;			/* everything below lives here */
	int			segno;			/* for error messages only */
	WeaveVecWeft w;
	WeaveVecScanState core;
	WeaveQuantizer q;
	WeaveQueryLut lut;

	VecDirCursor dir;
	VecCodeCursor code;
	WeaveVecWarpCursor warp;
	bool		warpstarted;	/* the warp cursor has returned a docid */
	uint32		warplast;		/* the warp that docid belongs to */
	uint64		warpdocid;

	const uint64 *allow;
	uint32		nwarp;
	float		threshold;		/* the driver's top-k floor, -inf by default */
	bool		seeked;			/* distinguishes "no seek yet" from "at warp 0" */

	/* The block the shuttle is sitting on. */
	bool		onblock;
	uint32		blockno;
	WeaveVecDirRec rec;
	int			nlanes;
	uint32		avail;			/* live-and-allowed lanes of that block */
	float		bound;			/* the core's bound for it, cached for (C3) */
	bool		scorable;		/* the core said SCORE for it */
	bool		scored;			/* lanescore[] is filled for that block */
	float		lanescore[WEAVE_VEC_BLOCK];
} VecShuttle;

/* palloc through a function pointer, for the codec cores that take an allocator
 * (include/weave/quantize.h).  The LUT and the quantizer tables are therefore in
 * the shuttle's private context, which is what makes an ERROR between begin() and
 * end() unable to leak them -- a pfree in an error path would not run at all. */
static void *
vec_palloc(size_t sz)
{
	return palloc(sz);
}

static void
vec_pfree(void *p)
{
	pfree(p);
}

/* Every REFUSE from the core and every false+why from a page reader lands here,
 * so the shape of the message is decided once. */
static void
vec_shuttle_fail(const VecShuttle *vs, const char *what, const char *why)
{
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("weave vector scan cannot read bolt %d of index \"%s\"",
					vs->segno, RelationGetRelationName(vs->w.index)),
			 errdetail("%s: %s.", what, why != NULL ? why : "unknown reason")));
}

/*
 * The live-and-allowed lanes of block `b`, or all-ones when the record cannot be
 * trusted to index the allowlist.
 *
 * THE ALL-ONES ANSWER IS NOT A GUESS, it is a refusal to read a bitmap at an
 * offset a page told us.  weave_lane_avail_mask() requires its caller to have
 * established that firstwarp .. firstwarp + nlanes - 1 lies inside the bitmap
 * (include/weave/kernels.h), and firstwarp comes off a page.  When either the O(1)
 * formula or the length test fails, this returns all-ones -- which makes the
 * caller want the codes, which makes the decision core see the same record and
 * refuse it with the reason.  So the authoritative judgement stays in the core and
 * this function never reads out of bounds.
 */
static uint32
vec_block_avail(const VecShuttle *vs, uint32 b, const WeaveVecDirRec *rec,
				int nlanes)
{
	if (rec->firstwarp != b * (uint32) WEAVE_VEC_BLOCK)
		return 0xFFFFFFFFu;
	if (vs->allow != NULL &&
		(uint64) rec->firstwarp + (uint64) nlanes > (uint64) vs->nwarp)
		return 0xFFFFFFFFu;
	return weave_lane_avail_mask(rec->livemask, nlanes, vs->allow,
								 rec->firstwarp);
}

/*
 * Position the shuttle on block `b` and let the core decide what to do with it.
 * Returns true when the block is to be scored.
 */
static bool
vec_load_block(VecShuttle *vs, uint32 b)
{
	const char *why = NULL;
	WeaveVecScanAct act;
	uint32		avail;
	bool		want;

	if (!dir_cur_advance(&vs->dir, b, &vs->rec, &why))
		vec_shuttle_fail(vs, "the block directory", why);

	vs->nlanes = weave_vecweft_block_lanes(&vs->w.geom, b);
	if (vs->nlanes <= 0)
		vec_shuttle_fail(vs, "the block directory",
						 "a block inside the weft covers no lanes");

	avail = vec_block_avail(vs, b, &vs->rec, vs->nlanes);
	want = (avail != 0);
	if (!code_cur_advance(&vs->code, b, vs->rec.firstpage, want, &why))
		vec_shuttle_fail(vs, "the code chain", why);

	vs->onblock = true;
	vs->blockno = b;
	vs->avail = avail;
	vs->scored = false;
	vs->scorable = false;

	/*
	 * A fully masked block leaves c->cencode holding nothing, so the core must not
	 * reach the bound.  It does not: include/weave/vecscan.h states the order as a
	 * contract -- "SKIP_MASK: one AND against the allowlist.  No code byte is read,
	 * no strip is scattered" -- and weave_vec_scan_block() computes the bound after
	 * that branch, not before it.  The dependency is CHECKED below rather than
	 * trusted, because the alternative to checking it is a bound computed from a
	 * stale centroid, which is a wrong number in the right units: exactly the
	 * failure (C2) exists to prevent.
	 */
	vs->bound = WEAVE_SCORE_NEVER;
	act = weave_vec_scan_block(&vs->core, b, &vs->rec, &vs->lut,
							   vs->code.cencode, vs->threshold, &vs->bound);
	if (!want && act != WEAVE_VSCAN_SKIP_MASK)
		elog(ERROR, "weave vector scan skipped the strip scatter for block %u of bolt %d but the decision core did not skip it on the allowlist",
			 b, vs->segno);

	switch (act)
	{
		case WEAVE_VSCAN_SCORE:
			vs->scorable = true;
			return true;

		case WEAVE_VSCAN_SKIP_BOUND:

			/*
			 * The core wrote the bound before deciding, so block_max() can still
			 * report it -- and it is a true upper bound whether or not the block
			 * is visited.
			 */
			return false;

		case WEAVE_VSCAN_SKIP_MASK:

			/*
			 * -inf, per (C5), and it is a TRUE bound rather than a convenient
			 * sentinel: every lane of this block is dead or disallowed, and a
			 * kernel scores such a lane WEAVE_KERNEL_NEVER, so -inf >= score
			 * holds at every position in it.
			 */
			vs->bound = WEAVE_SCORE_NEVER;
			return false;

		case WEAVE_VSCAN_REFUSE:
			vec_shuttle_fail(vs, "the code-scan decision core refused a block",
							 vs->core.why);
			break;
	}
	return false;				/* unreachable; vec_shuttle_fail() throws */
}

/*
 * Fill lanescore[] for the block the shuttle sits on: one kernel call, then the
 * inner-product -> metric-domain conversion the decision core owns.
 *
 * Lazy, so that a driver which asks for block_max() and then prunes never pays for
 * a kernel call, and so that score() and score_block() cannot each pay for one.
 */
static void
vec_ensure_scores(VecShuttle *vs)
{
	WeaveScoreBlock blk;
	float		raw[WEAVE_VEC_BLOCK];
	int			n;
	int			i;

	if (vs->scored)
		return;
	if (!vs->onblock)
		elog(ERROR, "weave vector scan asked for scores with no block in hand");
	if (vs->avail == 0)
		elog(ERROR, "weave vector scan asked for the scores of a block whose codes it never read");

	weave_vec_scan_scoreblk(&blk, &vs->core, &vs->rec, &vs->lut,
							vs->code.block, vs->nlanes);
	n = weave_vec_score_block(&blk, raw);
	if (n != vs->nlanes)
		elog(ERROR, "weave vector kernel scored %d of %d lanes", n, vs->nlanes);

	for (i = 0; i < vs->nlanes; i++)
		vs->lanescore[i] = weave_vec_scan_lane_score(&vs->core, &vs->lut, raw[i],
													vs->rec.lane[2 * i + 1]);
	vs->scored = true;
}

/* The first available lane at or after `target`, or -1. */
static int
vec_first_lane(uint32 avail, int nlanes, uint32 firstwarp, WeaveWarp target)
{
	int			l = 0;

	if (target > firstwarp)
		l = (int) (target - firstwarp);
	for (; l < nlanes; l++)
		if ((avail & (1u << l)) != 0)
			return l;
	return -1;
}

/* ---------------------------------------------------------------------------
 * WeaveShuttleOps -- the first implementation of include/weave/channel.h
 * ------------------------------------------------------------------------- */

/*
 * (C1) MONOTONE SEEK: the smallest warp >= target at which this channel could
 * contribute, or WEAVE_WARP_END.
 *
 * A BACKWARD TARGET IS AN ERROR, not a no-op, and the contract did not say which.
 * It could not: (C1) constrains what seek RETURNS and says nothing about what an
 * out-of-order call does.  The traversal here is three forward-only cursors over
 * chains with no index, so a backward seek is not merely unsupported, it is
 * unimplementable without re-walking the weft -- and "return the current position"
 * would let a fused-loop bug become a wrong answer instead of an error
 * (doc/specs/VECTOR_CHANNEL.md sect. 8b).
 *
 * A BLOCK SKIPPED ON THE BOUND ADVANCES THE SEEK, which is worth knowing before
 * wondering why a shuttle never prunes: `threshold` is -INFINITY unless a driver
 * that owns a top-k sets it (weave_vec_shuttle_set_threshold), and with -INFINITY
 * `bound <= threshold` is false for every finite bound, so this only ever fires
 * for the SRF below.  A shuttle driven by the fused loop prunes through
 * block_max() instead, which is where channel.h puts that decision.
 */
static WeaveWarp
vec_shuttle_seek(WeaveShuttle *s, WeaveWarp target)
{
	VecShuttle *vs = (VecShuttle *) s->state;
	uint32		b;

	if (vs->seeked && target < s->cur)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("weave vector shuttle cannot seek backwards"),
				 errdetail("Target warp %u is below the current position %u.",
						   (unsigned) target, (unsigned) s->cur)));
	vs->seeked = true;

	if (target >= vs->w.meta.nvec)
	{
		s->cur = WEAVE_WARP_END;
		s->blkend = WEAVE_WARP_END;
		vs->onblock = false;
		vs->bound = WEAVE_SCORE_NEVER;
		return WEAVE_WARP_END;
	}

	for (b = target / (uint32) WEAVE_VEC_BLOCK; b < vs->w.geom.nblocks; b++)
	{
		bool		scorable;
		int			l;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Re-seeking inside the block already in hand must not advance the
		 * cursors: they sit one block ahead by construction, so re-loading would
		 * read the NEXT block's pages and answer with them.
		 */
		if (vs->onblock && vs->blockno == b)
			scorable = vs->scorable;
		else
			scorable = vec_load_block(vs, b);

		if (!scorable)
			continue;

		l = vec_first_lane(vs->avail, vs->nlanes, vs->rec.firstwarp, target);
		if (l < 0)
			continue;			/* every allowed lane of this block is behind us */

		s->cur = vs->rec.firstwarp + (uint32) l;
		s->blkend = vs->rec.firstwarp + (uint32) vs->nlanes - 1;
		return s->cur;
	}

	s->cur = WEAVE_WARP_END;
	s->blkend = WEAVE_WARP_END;
	vs->onblock = false;
	vs->bound = WEAVE_SCORE_NEVER;
	return WEAVE_WARP_END;
}

/*
 * (C2) and (C3): a true upper bound over [cur, blkend], with no buffer read and no
 * recomputation.
 *
 * The value is whatever weave_vec_scan_block() computed when this block was loaded
 * -- one LUT pass over the centroid code, already paid.  (C3) forbids I/O here, and
 * the note on weave_vec_scan_block() extends that to the dim-length LUT pass,
 * because the fused loop calls block_max() more than once per block.  So it is
 * cached, and the cache is invalidated by loading a block and by nothing else.
 */
static float4
vec_shuttle_block_max(WeaveShuttle *s)
{
	VecShuttle *vs = (VecShuttle *) s->state;

	if (!vs->onblock || s->cur == WEAVE_WARP_END)
		return WEAVE_SCORE_NEVER;
	return (float4) vs->bound;
}

/* (C4) EXACT SCORE at s->cur, in the metric's domain. */
static float4
vec_shuttle_score(WeaveShuttle *s)
{
	VecShuttle *vs = (VecShuttle *) s->state;
	int			l;

	if (!vs->onblock || s->cur == WEAVE_WARP_END)
		return WEAVE_SCORE_NEVER;

	l = (int) (s->cur - vs->rec.firstwarp);
	if (s->cur < vs->rec.firstwarp || l >= vs->nlanes)
		elog(ERROR, "weave vector shuttle asked to score warp %u, which is not in the block at warps %u..%u",
			 (unsigned) s->cur, (unsigned) vs->rec.firstwarp,
			 (unsigned) (vs->rec.firstwarp + (uint32) vs->nlanes - 1));

	vec_ensure_scores(vs);
	return vs->lanescore[l];
}

/*
 * The optional whole-block enumeration: 32 lanes for the price of one kernel call,
 * which is why channel.h says the scorer prefers it.
 *
 * A CONTRACT HOLE, REPORTED RATHER THAN GUESSED AT: WeaveShuttleOps.score_block()
 * takes an `allow` bitmap and NO length for it, while every other consumer of a
 * warp-indexed bitmap in this channel requires one -- include/weave/kernels.h is
 * explicit that "an untrusted index into a bitmap with no length is an unbounded
 * out-of-bounds read", and firstwarp comes off a page.  So a non-NULL `allow` is
 * accepted here only when the shuttle was opened with its own allowlist, whose
 * nwarp bounds the read; otherwise it is refused.  A caller that wants to filter an
 * unfiltered shuttle passes its bitmap to weave_vec_shuttle_begin() instead, which
 * takes the length the contract omits.
 */
static int
vec_shuttle_score_block(WeaveShuttle *s, WeaveWarp first, int nwarp,
						const uint64 *allow, float4 *out)
{
	VecShuttle *vs = (VecShuttle *) s->state;
	int			off;
	int			n;
	int			i;

	if (!vs->onblock || nwarp <= 0 || out == NULL)
		return 0;
	if (allow != NULL && vs->allow == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("weave vector shuttle cannot apply an allowlist of unknown length"),
				 errdetail("WeaveShuttleOps.score_block() passes no length for its "
						   "allow bitmap, so it can only be honoured within the "
						   "length the shuttle was opened with.")));

	if (first < vs->rec.firstwarp)
		elog(ERROR, "weave vector shuttle asked for warps from %u, before the block at %u",
			 (unsigned) first, (unsigned) vs->rec.firstwarp);
	off = (int) (first - vs->rec.firstwarp);
	if (off >= vs->nlanes)
		return 0;
	n = vs->nlanes - off;
	if (n > nwarp)
		n = nwarp;

	vec_ensure_scores(vs);
	for (i = 0; i < n; i++)
	{
		int			l = off + i;
		WeaveWarp	warp = first + (WeaveWarp) i;
		bool		ok = (vs->avail & (1u << l)) != 0;

		if (ok && allow != NULL)
			ok = (allow[warp >> 6] & (UINT64CONST(1) << (warp & 63))) != 0;
		out[i] = ok ? vs->lanescore[l] : (float4) WEAVE_SCORE_NEVER;
	}
	return n;
}

/*
 * Cheap by construction: one MemoryContextDelete.
 *
 * Nothing is freed individually, and that is the point -- the LUT's single backing
 * allocation (WeaveQueryLut._alloc), the quantizer's rotation and codebook tables,
 * and all three cursors' page buffers are in the shuttle's own context, so an ERROR
 * anywhere between begin() and end() reclaims them at transaction abort without
 * this function running at all.  A pfree-per-object end() would leak in exactly the
 * case that matters.
 */
static void
vec_shuttle_end(WeaveShuttle *s)
{
	VecShuttle *vs = (VecShuttle *) s->state;

	/*
	 * HARVEST THE PER-SCAN COUNTERS HERE, because this is the one place every
	 * vector shuttle passes through: the fused pass ends its shuttles in
	 * src/am/amscan.c, weave_vec_bolt_pass() ends its own a few hundred lines
	 * below, and weave_vec_scan_stats() ends the one it opened.  One site means the
	 * fused arm and the single-channel arm are counted by the same code in the same
	 * unit, which is the only way FUSED_TOPK.md sect. 8's ratio is a measurement
	 * rather than two measurements.  Until now the ORDER BY path pfree'd these
	 * unread.
	 *
	 * LANES, not score() calls: the kernel scores a 32-lane block at a time.  See
	 * include/weave/weave.h.
	 *
	 * An ERROR between begin() and end() skips this, so the counters undercount a
	 * failed query -- the same "a zero is not evidence unless the query ran" caveat
	 * every counter in this project carries, and the safe direction.
	 */
	weave_vecwork_shuttles++;
	weave_vecwork_lanes += (uint64) vs->core.nlane_score;
	weave_vecwork_blocks += (uint64) vs->core.nblk_score;
	weave_vecwork_blk_bound += (uint64) vs->core.nblk_bound;

	MemoryContextDelete(vs->ctx);
}

static const WeaveShuttleOps vec_shuttle_ops = {
	vec_shuttle_seek,
	vec_shuttle_block_max,
	vec_shuttle_score,
	vec_shuttle_score_block,
	NULL,						/* set_visit_filter: graph-only (C-comment in
								 * channel.h); a code scan has no traversal to
								 * steer, it consults `allow` per block instead */
	vec_shuttle_end
};

WeaveShuttle *
weave_vec_shuttle_begin(const WeaveVecWeft *w, int segno, const float *query,
						int qdim, const uint64 *allow, WeaveWarp nwarp,
						float4 weight)
{
	MemoryContext ctx;
	MemoryContext old;
	VecShuttle *vs;
	VecDirCursor mc;
	const char *why = NULL;
	float		acc = 0.0f;

	uint32		b;

	if (w == NULL || query == NULL)
		elog(ERROR, "weave vector shuttle needs a weft and a query");

	/* The vector channel's mechanism counter.  One per shuttle opened, i.e.
	 * per bolt scanned, which is the same unit the lexical counters use.
	 * include/weave/weave.h, doc/PHASES.md Z4. */
	weave_chan_vector_scan++;

	/*
	 * THE CALIBRATION GUARD, and it is unreachable from SQL today on purpose.
	 *
	 * The scan rebuilds the quantizer from (dim, bits) alone, which is exact
	 * because weave_codebook_get() and weave_rotation_init() are pure functions of
	 * those two numbers -- and it is exact ONLY while TQ+ calibration is off.
	 * There is no on-disk format for a calibration blob and the writer always
	 * writes calibstart = InvalidBlockNumber, so nothing can reach this branch from
	 * SQL.  It is here anyway because the day something does, cal = NULL would
	 * produce a syntactically valid and silently WRONG quantizer -- every score off
	 * by the calibration, every answer plausible.  One comparison converts that
	 * into a startup error (doc/specs/VECTOR_CHANNEL.md sect. 8b).
	 */
	if (w->meta.calibstart != InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bolt %d of index \"%s\" carries a TQ+ calibration this build cannot read",
						segno, RelationGetRelationName(w->index)),
				 errdetail("The scan reconstructs the quantizer from the weft's "
						   "dimension and code width, which is exact only for an "
						   "uncalibrated weft."),
				 errhint("Rebuild the index with calibration off.")));

	if (qdim != (int) w->meta.dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("query has %d dimensions, bolt %d of index \"%s\" has %d",
						qdim, segno, RelationGetRelationName(w->index),
						(int) w->meta.dim)));

	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"weave vector code-scan shuttle",
								ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(ctx);

	vs = (VecShuttle *) palloc0(sizeof(VecShuttle));
	vs->ctx = ctx;
	vs->segno = segno;
	vs->w = *w;
	vs->allow = allow;
	vs->nwarp = allow != NULL ? (uint32) nwarp : 0;
	vs->threshold = -get_float4_infinity();

	/*
	 * The decision core validates the geometry, the metric and the allowlist's
	 * length before a page is read, so a weft this channel cannot scan fails here
	 * rather than on its first block -- by which point a bound has already been
	 * handed to a caller.  THE METRIC COMES FROM THE WEFT, never from the caller:
	 * see "THE DOMAIN RULE" in include/weave/vecscan.h.
	 */
	if (weave_vec_scan_begin(&vs->core, &vs->w.geom, (int) vs->w.meta.metric,
							 allow, vs->nwarp) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("weave cannot code-scan bolt %d of index \"%s\"",
						segno, RelationGetRelationName(w->index)),
				 errdetail("%s.", vs->core.why != NULL ? vs->core.why : "unknown reason")));

	if (weave_quantizer_init(&vs->q, (int) vs->w.meta.dim, (int) vs->w.meta.bits,
							 NULL, vec_palloc, vec_pfree) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("weave cannot build a %d-bit quantizer for %d dimensions",
						(int) vs->w.meta.bits, (int) vs->w.meta.dim)));
	if (weave_query_lut_build(&vs->lut, &vs->q, query, vec_palloc) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("weave cannot build a query table for a %d-dimensional query",
						qdim)));

	dir_cur_begin(&vs->dir, &vs->w);
	code_cur_begin(&vs->code, &vs->w);
	weave_vec_warp_begin(&vs->warp, &vs->w);

	vs->sh.ops = &vec_shuttle_ops;
	vs->sh.kind = WEAVE_CH_VECTOR_SCAN;
	vs->sh.cur = 0;
	vs->sh.blkend = 0;
	vs->sh.weight = weight;
	vs->sh.state = vs;
	vs->bound = WEAVE_SCORE_NEVER;

	/*
	 * maxscore is bound (B2) folded over ONE directory pass, on its own cursor.
	 *
	 * (B3) would be tighter and needs every block's centroid CODE, which lives on
	 * the code pages: a full pass over the weft -- 56x the directory at 960
	 * dimensions -- to tighten a value whose only job is the MaxScore partition.
	 * Looser is the right trade there (doc/specs/FUSED_TOPK.md sect. 5).
	 *
	 * The pass does NOT validate (C1) across the weft, which sect. 8b said it
	 * would.  It cannot: weave_vec_scan_maxscore_fold() takes only an accumulator
	 * and a record, has no state and no `why`, and the core's (C1) witness lives
	 * inside weave_vec_scan_block(), which needs a LUT and a centroid code.  So
	 * (C1) is checked incrementally, one block at a time, as the scan reaches it.
	 */
	dir_cur_begin(&mc, &vs->w);
	for (b = 0; b < vs->w.geom.nblocks; b++)
	{
		WeaveVecDirRec r;

		CHECK_FOR_INTERRUPTS();
		if (!dir_cur_next(&mc, &r, &why))
			vec_shuttle_fail(vs, "the block directory", why);
		weave_vec_scan_maxscore_fold(&acc, &r);
	}
	dir_cur_end(&mc);
	vs->sh.maxscore = (float4) weave_vec_scan_maxscore(&vs->core, &vs->lut, acc);

	MemoryContextSwitchTo(old);
	return &vs->sh;
}

/*
 * THE BOLT-WIDE CEILING WITHOUT OPENING A SHUTTLE, for the fused objective's
 * per-key normalizer (doc/specs/FUSED_TOPK.md sect. 8d).
 *
 * WHY THIS EXISTS RATHER THAN READING sh->maxscore OFF A SHUTTLE.  The normalizer
 * has to be the same constant for every bolt of one query, because
 * weave_fuse_pass() in src/am/amscan.c merges the per-bolt top-k lists BY SCORE.  A
 * per-bolt normalizer would score each bolt against a slightly different objective,
 * and the merge would then interleave two rankings -- an answer that changes with
 * the segment count, which is to say with VACUUM and merge.  So the maximum over
 * bolts has to be known before the FIRST bolt is scanned, and a shuttle cannot
 * supply it: holding one open per bolt up front is exactly what the per-bolt
 * scratch context exists to avoid.
 *
 * It is the same value weave_vec_shuttle_begin() computes -- bound (B2) folded over
 * one directory pass, then weave_vec_scan_maxscore() to reach the metric's domain --
 * and it calls those same two functions rather than reproducing their arithmetic,
 * because a second copy of the domain conversion is the failure
 * include/weave/vecscan.h's "THE DOMAIN RULE" is about.
 *
 * Cost is one LUT build and one directory pass per bolt per vector key.  The
 * directory is one record per 32 lanes, so it is kilobytes against a code weft of
 * megabytes; the pass this duplicates is the one begin() already runs.
 *
 * It RETURNS FALSE and sets *why instead of throwing.  Every refusal here is one
 * weave_vec_shuttle_begin() is about to make again for the same bolt, with a message
 * that names the bolt and the index; throwing from a normalizer pass would report
 * the same fault from a place the reader cannot connect to their query.  The caller
 * decides, and src/am/amscan.c's decision is to leave the bolt out of the maximum
 * and let the scan raise the real error.
 */
bool
weave_vec_weft_maxscore(const WeaveVecWeft *w, const float *query, int qdim,
						float *out, const char **why)
{
	MemoryContext ctx;
	MemoryContext old;
	WeaveVecScanState core;
	WeaveQuantizer q;
	WeaveQueryLut lut;
	VecDirCursor mc;
	const char *lw = NULL;
	float		acc = 0.0f;
	uint32		b;
	bool		ok = false;

	if (w == NULL || query == NULL || out == NULL)
	{
		if (why != NULL)
			*why = "a null weft, query or output pointer";
		return false;
	}

	/* begin() raises on this; here it is a refusal, for the reason above. */
	if (qdim != (int) w->meta.dim || w->meta.calibstart != InvalidBlockNumber)
	{
		if (why != NULL)
			*why = (qdim != (int) w->meta.dim)
				? "the query's dimension is not the weft's"
				: "the weft carries a calibration this build cannot read";
		return false;
	}

	/*
	 * Its own context, reset by the caller's next call rather than growing with
	 * the bolt count: the LUT is dim x 2^bits floats, so a hundred-bolt index
	 * would otherwise hold a hundred of them alive for the length of the scan.
	 */
	ctx = AllocSetContextCreate(CurrentMemoryContext,
								"weave vector normalizer",
								ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(ctx);

	/*
	 * allow = NULL, nwarp = 0: no allowlist.  A ceiling over the bolt's live lanes
	 * only would be tighter, and it would also make the normalizer depend on which
	 * rows a predicate admits -- so the same document would score differently
	 * depending on the WHERE clause it arrived under.  The normalizer is a property
	 * of the query and the data, and this is where that is decided.
	 */
	if (weave_vec_scan_begin(&core, &w->geom, (int) w->meta.metric,
							 NULL, 0) != 0)
		lw = core.why != NULL ? core.why : "the weft cannot be code-scanned";
	else if (weave_quantizer_init(&q, (int) w->meta.dim, (int) w->meta.bits,
								  NULL, vec_palloc, vec_pfree) != 0)
		lw = "the quantizer geometry is unsupported";
	else if (weave_query_lut_build(&lut, &q, query, vec_palloc) != 0)
		lw = "the query table could not be built";
	else
	{
		ok = true;
		dir_cur_begin(&mc, w);
		for (b = 0; b < w->geom.nblocks; b++)
		{
			WeaveVecDirRec r;

			CHECK_FOR_INTERRUPTS();
			if (!dir_cur_next(&mc, &r, &lw))
			{
				ok = false;
				break;
			}
			weave_vec_scan_maxscore_fold(&acc, &r);
		}
		dir_cur_end(&mc);

		if (ok)
			*out = weave_vec_scan_maxscore(&core, &lut, acc);
	}

	MemoryContextSwitchTo(old);
	MemoryContextDelete(ctx);

	if (!ok && why != NULL)
		*why = lw != NULL ? lw : "unknown reason";
	return ok;
}

void
weave_vec_shuttle_set_threshold(WeaveShuttle *s, float4 theta)
{
	VecShuttle *vs = (VecShuttle *) s->state;

	vs->threshold = (float) theta;
}

uint64
weave_vec_shuttle_docid(WeaveShuttle *s, WeaveWarp warp)
{
	VecShuttle *vs = (VecShuttle *) s->state;
	const char *why = NULL;

	if (warp >= vs->w.meta.nvec)
		elog(ERROR, "weave vector scan asked for the document behind warp %u of a %u-lane weft",
			 (unsigned) warp, (unsigned) vs->w.meta.nvec);
	if (vs->warpstarted && warp < vs->warplast)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("weave vector scan cannot resolve warp %u after warp %u",
						(unsigned) warp, (unsigned) vs->warplast),
				 errdetail("The warp map has no index over its pages, so the "
						   "cursor over it is forward-only; a driver must resolve "
						   "a document when its candidate is admitted, not after "
						   "sorting by score.")));
	if (vs->warpstarted && warp == vs->warplast)
		return vs->warpdocid;

	while (vs->warp.warp <= warp)
	{
		uint64		d;

		CHECK_FOR_INTERRUPTS();
		if (!weave_vec_warp_next(&vs->warp, &d, &why))
			vec_shuttle_fail(vs, "the warp map", why);
		vs->warpdocid = d;
	}
	vs->warplast = warp;
	vs->warpstarted = true;
	return vs->warpdocid;
}

const WeaveVecScanState *
weave_vec_shuttle_stats(WeaveShuttle *s)
{
	VecShuttle *vs = (VecShuttle *) s->state;

	return &vs->core;
}

/* ---------------------------------------------------------------------------
 * weave_vec_scan() and weave_vec_scan_stats(): the direct entry point
 *
 * WHY A SQL FUNCTION EXISTS AT ALL, which when this was written was because V8
 * wired no operator and no ORDER BY.  Task F7 wired both, and the answer did not
 * change -- it got sharper.  On 2026-09-16 a mutation that reintroduced a known
 * scan-side bug passed the
 * whole regression suite twice, the second time because the planner answered the
 * query with a bitmap heap scan whose executor recheck re-evaluated the operator
 * itself -- the right answer by a path that never entered the mutated code
 * (AGENTS.md).  Only weave_count() and weave_search() reach the lexical scan
 * machinery directly, and that is the only reason mutations in it are catchable.
 * The vector scan needs the same door, and it needs it in the commit that adds
 * the scan rather than in a later one.  Now that a plan CAN reach the channel, this
 * function is also the oracle sql/vecorderby.sql compares that plan against, which
 * is why F7 factored the bolt loop into weave_vec_topk_run() instead of writing a
 * second one: an oracle that drifts from the thing it checks is worse than none.
 *
 * TWO FUNCTIONS, NOT ONE WITH EXTRA COLUMNS, and the reason is the case the
 * counters matter most in: an allowlist that admits nothing returns ZERO ROWS,
 * and so does a scan pruned to nothing.  Counter columns attached to result rows
 * would vanish in exactly the two measurements V8's gate is about -- "how many
 * blocks did the mask skip" is unanswerable from a query that returned nothing.
 * They are per-bolt scalars, so they get a per-bolt row.  Both functions run the
 * identical scan through vec_scan_run(), so the numbers describe the work the
 * other function did.
 *
 * MVCC AND TOMBSTONES ARE NOT APPLIED, deliberately and per (C6): livedocs are
 * the fused scorer's job, this is a view of what the weft contains, and
 * weave_vecwork_lanes() reports the same way.  A docid here is an index-resident
 * document id, not a proof that a visible row exists.
 * ------------------------------------------------------------------------- */

/*
 * WeaveVecTopKHit / WeaveVecTopKCtr / WeaveVecTopK were declared here, file-static,
 * while this SRF was the only driver of the per-bolt top-k.  Task F7 added a second
 * driver -- the access method's ORDER BY path -- so they moved to
 * include/weave/vector.h, where the reason they are shared rather than copied is
 * written next to them.  Nothing else about them changed.
 */

static int
vec_cmp_u64(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	if (x < y)
		return -1;
	return x > y ? 1 : 0;
}

static bool
vec_docid_member(const uint64 *sorted, int n, uint64 d)
{
	int			lo = 0;
	int			hi = n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (sorted[mid] == d)
			return true;
		if (sorted[mid] < d)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/*
 * A DOCID allowlist, converted to this bolt's warps.
 *
 * THE ARGUMENT IS DOCIDS AND NOT WARPS, and that is not a convenience: a warp is
 * SEGMENT-LOCAL (doc/ARCHITECTURE.md sect. 3), so warp 7 names a different
 * document in every bolt and an array of warps is ambiguous the moment an index
 * has two.  A docid is the bolt-independent name, it is what
 * weave_vecwork_lanes() reports so a test can obtain one, and it is what a real
 * filter would arrive as -- the fused scorer's allowlist comes from another
 * channel over the shared docid space.  The conversion is one pass of the warp
 * map per bolt, which the scan walks anyway.
 */
static uint64 *
vec_allow_from_docids(const WeaveVecWeft *w, int segno, const uint64 *sorted,
					  int nwant)
{
	uint32		nvec = w->meta.nvec;
	Size		nwords = ((Size) nvec + 63) / 64;
	uint64	   *bm = (uint64 *) palloc0(nwords * sizeof(uint64));
	WeaveVecWarpCursor c;
	uint32		i;

	weave_vec_warp_begin(&c, w);
	for (i = 0; i < nvec; i++)
	{
		uint64		d;
		const char *why = NULL;

		CHECK_FOR_INTERRUPTS();
		if (!weave_vec_warp_next(&c, &d, &why))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("weave vector scan cannot read the warp map of bolt %d of index \"%s\"",
							segno, RelationGetRelationName(w->index)),
					 errdetail("%s.", why != NULL ? why : "unknown reason")));
		if (nwant > 0 && vec_docid_member(sorted, nwant, d))
			bm[i >> 6] |= UINT64CONST(1) << (i & 63);
	}
	weave_vec_warp_end(&c);
	return bm;
}

/*
 * The top-k floor, and the direction of its comparison.
 *
 * REJECTION IS ON `s <= theta`, so an equal score does NOT displace an incumbent.
 * Getting this backwards is not a tie-breaking preference: it moved recall 1.5
 * points in an earlier benchmark, because a plateau of equal scores then evicts
 * the whole heap one candidate at a time and the survivors are the LAST equal
 * scores seen rather than the first.  The same comparison is what
 * weave_vec_scan_block() takes as its threshold, which is why the value handed to
 * the shuttle is the k-th best score itself.
 */
static float4
vec_topk_theta(const WeaveVecTopK *r)
{
	if (r->nhit < r->k)
		return -get_float4_infinity();
	return r->hit[r->k - 1].score;
}

static bool
vec_topk_rejects(const WeaveVecTopK *r, float4 s)
{
	return r->nhit == r->k && !(s > r->hit[r->k - 1].score);
}

static void
vec_topk_admit(WeaveVecTopK *r, int32 segno, uint32 warp, uint64 docid, float4 s)
{
	int			i;
	int			j;

	for (i = 0; i < r->nhit; i++)
		if (s > r->hit[i].score)
			break;
	if (i >= r->k)
		return;
	if (r->nhit < r->k)
		r->nhit++;
	for (j = r->nhit - 1; j > i; j--)
		r->hit[j] = r->hit[j - 1];
	r->hit[i].segno = segno;
	r->hit[i].warp = warp;
	r->hit[i].docid = docid;
	r->hit[i].score = s;
}

/* Drive one bolt's shuttle to exhaustion, folding its lanes into the top-k. */
static void
vec_scan_bolt(WeaveVecTopK *r, const WeaveVecWeft *w, int segno, const WVec *query,
			  const uint64 *allow)
{
	WeaveShuttle *sh = weave_vec_shuttle_begin(w, segno, query->x, (int) query->dim,
											   allow, (WeaveWarp) w->meta.nvec,
											   1.0f);
	WeaveVecTopKCtr *ctr = &r->ctr[r->nctr];
	const WeaveVecScanState *st;
	WeaveWarp	warp;

	warp = weave_shuttle_seek(sh, 0);
	while (warp != WEAVE_WARP_END)
	{
		float4		out[WEAVE_VEC_BLOCK];
		WeaveWarp	blkend = sh->blkend;
		int			n;
		int			i;

		CHECK_FOR_INTERRUPTS();

		/*
		 * score_block() rather than a seek+score() per lane, because that is the
		 * path channel.h says a scorer prefers and therefore the path a mutation
		 * has to be able to reach: 32 lanes for one kernel call.
		 */
		n = sh->ops->score_block(sh, warp, WEAVE_VEC_BLOCK, NULL, out);
		for (i = 0; i < n; i++)
		{
			float4		s = out[i];
			uint64		docid;

			if (s == (float4) WEAVE_SCORE_NEVER)
				continue;		/* dead lane, or one the allowlist excluded */
			if (vec_topk_rejects(r, s))
				continue;

			/*
			 * The docid is resolved HERE, while the candidate is being admitted,
			 * and not after the top-k is sorted: the warp map cursor is
			 * forward-only and the scan visits warps in ascending order, so this
			 * is the only point at which the answer is one cursor step away
			 * instead of a re-walk (doc/specs/VECTOR_CHANNEL.md sect. 8b).
			 */
			docid = weave_vec_shuttle_docid(sh, warp + (WeaveWarp) i);
			vec_topk_admit(r, (int32) segno, (uint32) (warp + (WeaveWarp) i),
						   docid, s);
			weave_vec_shuttle_set_threshold(sh, vec_topk_theta(r));
		}

		if ((uint64) blkend + 1 >= (uint64) w->meta.nvec)
			break;
		warp = weave_shuttle_seek(sh, blkend + 1);
	}

	/* Copied field by field before end(), which frees the core along with the
	 * shuttle's context. */
	st = weave_vec_shuttle_stats(sh);
	ctr->segno = (int32) segno;
	ctr->maxscore = sh->maxscore;
	ctr->nblk_seen = (int64) st->nblk_seen;
	ctr->nblk_mask = (int64) st->nblk_mask;
	ctr->nblk_bound = (int64) st->nblk_bound;
	ctr->nblk_score = (int64) st->nblk_score;
	ctr->nlane_score = (int64) st->nlane_score;
	r->nctr++;

	sh->ops->end(sh);
}

/*
 * Run the top-k over every bolt of an already-open index.  The contract, and why
 * this is not static, is on the declaration in include/weave/vector.h.
 */
WeaveVecTopK *
weave_vec_topk_run(Relation index, const WeaveMetaPageData *meta,
				   const WVec *query, int k, uint16 attnum,
				   const uint64 *want, int nwant, bool filtered)
{
	WeaveVecTopK *r;
	uint32		s;

	if (k < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("k must be at least 1")));

	r = (WeaveVecTopK *) palloc0(sizeof(WeaveVecTopK));
	r->k = k;
	/*
	 * HUGE-SAFE since F7, and the reason is that `k`'s provenance changed.  It used
	 * to be an SRF argument a human typed; the ORDER BY driver's widening ladder now
	 * raises it geometrically until it can prove completeness, so at the top of that
	 * ladder it is the index's lane count -- relation scale.  Not zeroed, unlike the
	 * palloc0 this replaced: every reader of hit[] (vec_topk_theta, vec_topk_admit,
	 * both SRFs, weave_vec_pass) is bounded by nhit, and memset-ing a
	 * relation-scale array for slots nothing reads is the cost this allocation is
	 * trying to avoid.
	 */
	r->hit = (WeaveVecTopKHit *) WEAVE_ALLOC_MAYBE_HUGE((Size) k * sizeof(WeaveVecTopKHit));
	r->ctr = (WeaveVecTopKCtr *) palloc0((Size) WEAVE_MAX_SEGMENTS * sizeof(WeaveVecTopKCtr));

	for (s = 0; s < meta->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveVecWeft w;
		BlockNumber root;
		const char *why = NULL;
		uint64	   *allow = NULL;
		uint16		wattnum = 0;

		if (meta->segs[s].dictstart == InvalidBlockNumber)
			continue;			/* consumed slot */
		root = weave_vec_weft_locate(index, &meta->segs[s], &wattnum);
		if (root == InvalidBlockNumber)
			continue;			/* this bolt carries no vector weft */

		/*
		 * ROUTE BY ATTRIBUTE when the caller named one.  A weft records the index
		 * attribute it was built from, and a driver that scores a weft belonging to
		 * a different column produces a wrong answer with a correct row count --
		 * which is the exact failure include/weave/vector.h says this field exists
		 * to prevent.  attnum 0 is "any", which is what the SRF asks for.
		 */
		if (attnum != 0 && wattnum != attnum)
			continue;

		if (!weave_vec_weft_open(index, root, &w, &why))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("bolt %u of index \"%s\" has an unreadable vector weft at block %u",
							s, RelationGetRelationName(index), root),
					 errdetail("%s.", why != NULL ? why : "unknown reason")));

		r->nlane += (uint64) w.meta.nvec;
		if (filtered)
			allow = vec_allow_from_docids(&w, (int) s, want, nwant);
		vec_scan_bolt(r, &w, (int) s, query, allow);
		if (allow != NULL)
			pfree(allow);
	}

	return r;
}

/*
 * The SRFs' entry: turn the SQL arguments into the shared runner's arguments.
 * Shared by both SRFs so that the counters one reports describe the work the other
 * did.
 */
static WeaveVecTopK *
vec_scan_run(Oid indexoid, const WVec *query, int k, ArrayType *arr)
{
	WeaveMetaPageData meta;
	Relation	index;
	WeaveVecTopK *r;
	uint64	   *want = NULL;
	int			nwant = 0;
	bool		filtered = false;

	if (k < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("k must be at least 1")));

	if (arr != NULL)
	{
		Datum	   *dats;
		bool	   *nulls;
		int			nd;
		int			i;

		if (ARR_NDIM(arr) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("the docid allowlist must be a one-dimensional array")));
		deconstruct_array(arr, INT8OID, 8, FLOAT8PASSBYVAL, TYPALIGN_DOUBLE,
						  &dats, &nulls, &nd);
		want = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) (nd > 0 ? nd : 1) *
												 sizeof(uint64));
		for (i = 0; i < nd; i++)
		{
			int64		v;

			if (nulls[i])
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("the docid allowlist may not contain NULLs"),
						 errdetail("An allowlist with an unknown element admits "
								   "an unknown set.")));
			v = DatumGetInt64(dats[i]);
			if (v < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("%lld is not a docid", (long long) v)));
			want[i] = (uint64) v;
		}
		if (nd > 1)
			qsort(want, (size_t) nd, sizeof(uint64), vec_cmp_u64);
		nwant = nd;

		/*
		 * An EMPTY array is an allowlist that admits nothing, and it must not be
		 * confused with the absence of one -- NULL means "no filter".  The
		 * decision core says the same thing about a NULL bitmap versus an all-zero
		 * bitmap: conflating them turns an empty candidate set into a full scan.
		 */
		filtered = true;
	}

	index = weave_vec_introspect_open(indexoid, &meta);
	pgstat_count_index_scan(index);

	/*
	 * attnum 0: this function names no column, so it scores whatever vector weft a
	 * bolt carries.  That is what every existing caller (sql/vecscan.sql) asserts
	 * against and it is the behaviour of this function before F7 split the loop
	 * out of it.  The ORDER BY driver passes the attribute the scan key named.
	 */
	r = weave_vec_topk_run(index, &meta, query, k, 0, want, nwant, filtered);

	pgstat_count_index_tuples(index, r->nhit);
	index_close(index, AccessShareLock);
	if (want != NULL)
		pfree(want);
	return r;
}

/*
 * Both SRFs take the same four arguments and neither is STRICT, because the
 * fourth one's NULL is MEANINGFUL: NULL is "no allowlist" and '{}' is "an
 * allowlist that admits nothing", and a STRICT function cannot tell a caller the
 * difference.  Strictness in the other three is emulated -- a NULL index, query
 * or k returns no rows, which is what STRICT would have done.
 */
static WeaveVecTopK *
vec_scan_from_args(FunctionCallInfo fcinfo)
{
	ArrayType  *arr = NULL;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		return NULL;
	if (!PG_ARGISNULL(3))
		arr = PG_GETARG_ARRAYTYPE_P(3);

	return vec_scan_run(PG_GETARG_OID(0), PG_GETARG_WVEC(1),
						PG_GETARG_INT32(2), arr);
}

Datum
weave_vec_scan(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	WeaveVecTopK *r;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		r = vec_scan_from_args(fcinfo);
		funcctx->max_calls = r != NULL ? (uint64) r->nhit : 0;
		funcctx->user_fctx = r;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	r = (WeaveVecTopK *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		const WeaveVecTopKHit *h = &r->hit[funcctx->call_cntr];
		Datum		values[4];
		bool		nulls[4];
		HeapTuple	tuple;

		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum(h->segno);
		values[1] = Int64GetDatum((int64) h->warp);
		values[2] = Int64GetDatum((int64) h->docid);
		values[3] = Float4GetDatum(h->score);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

Datum
weave_vec_scan_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	WeaveVecTopK *r;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		r = vec_scan_from_args(fcinfo);
		funcctx->max_calls = r != NULL ? (uint64) r->nctr : 0;
		funcctx->user_fctx = r;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	r = (WeaveVecTopK *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		const WeaveVecTopKCtr *c = &r->ctr[funcctx->call_cntr];
		Datum		values[7];
		bool		nulls[7];
		HeapTuple	tuple;

		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum(c->segno);
		values[1] = Int64GetDatum(c->nblk_seen);
		values[2] = Int64GetDatum(c->nblk_mask);
		values[3] = Int64GetDatum(c->nblk_bound);
		values[4] = Int64GetDatum(c->nblk_score);
		values[5] = Int64GetDatum(c->nlane_score);
		values[6] = Float4GetDatum(c->maxscore);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}
