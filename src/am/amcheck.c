/*-------------------------------------------------------------------------
 *
 * amcheck.c
 *		weave_check(): mechanical verification of the on-disk invariants.
 *
 * doc/specs/SEGMENT_FORMAT.md sect. 9 lists the invariants a weave index must
 * satisfy, one per line, each mechanically testable.  This file implements the
 * subset the v6 format break owns -- the metapage version gate, the bolt
 * directory, the page-kind space, and channel-descriptor reachability -- and is
 * the frame the remaining lexical, vector and fuzzy invariants get added to.
 *
 * WHAT IS NOT HERE YET, stated so nobody reads a green weave_check() as a clean
 * bill of health: the dictionary term ordering, the block-max bound recompute
 * (the one that is a live contract-(C2) soundness check), the livedocs popcount,
 * the trigram ordinals, and the doclen sidecar coverage are all still owed.  They
 * are task M6 in doc/PHASES.md.  The blocking-gate list in
 * doc/PRODUCTION_READINESS.md gate 4 claims weave_check() "covers the inherited
 * lexical ones only"; before this file there was no weave_check() at all, only
 * weave_check_meta() on the metapage header.
 *
 * REPORTING MODEL.  One row per invariant, with ok/violated and a detail string,
 * rather than an ERROR on the first violation.  Two reasons: a corruption test
 * wants to assert that a specific injected fault is detected (and that the others
 * still pass), and an operator running this on a suspect index wants the whole
 * list, not the first line of it.  The one exception is a metapage whose magic or
 * version is unrecognized: that is an ERROR, because every subsequent invariant
 * would be reading the segs[] array at offsets it cannot justify
 * (doc/CONVENTIONS.md decision 3).
 *
 * A standalone translation unit on purpose, like src/am/amsize.c: src/am/am.c is
 * already a unity build of four files and task L1 is to split it.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/am/amcheck.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "weave/am.h"

PG_FUNCTION_INFO_V1(weave_check);

/* Per-block marks for the reachability walk. */
#define WVCK_SEEN		0x01	/* reached from the metapage */
#define WVCK_TWICE		0x02	/* reached twice: two chains overlap */

typedef struct WeaveCheckCtx
{
	Relation	index;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	BlockNumber nblocks;
	uint8	   *mark;			/* nblocks entries, or NULL when !deep */
	int64		noverlap;
} WeaveCheckCtx;

static void
wvck_emit(WeaveCheckCtx *cx, const char *invariant, bool ok, const char *detail)
{
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	values[0] = CStringGetTextDatum(invariant);
	values[1] = BoolGetDatum(ok);
	if (detail == NULL)
		nulls[2] = true;
	else
		values[2] = CStringGetTextDatum(detail);
	tuplestore_putvalues(cx->tupstore, cx->tupdesc, values, nulls);
}

/*
 * Mark one block as reached.  Double-marking is how the "no two chains overlap"
 * invariant is enforced: doc/specs/SEGMENT_FORMAT.md sect. 8 item 5 records that
 * a sibling project shipped a chain-overlap bug four separate times, so the check
 * is structural rather than a matter of care at each write site.
 */
static void
wvck_mark(WeaveCheckCtx *cx, BlockNumber blk)
{
	if (cx->mark == NULL || blk == InvalidBlockNumber || blk >= cx->nblocks)
		return;
	if ((cx->mark[blk] & WVCK_SEEN) != 0)
	{
		if ((cx->mark[blk] & WVCK_TWICE) == 0)
		{
			cx->mark[blk] |= WVCK_TWICE;
			cx->noverlap++;
		}
		return;
	}
	cx->mark[blk] |= WVCK_SEEN;
}

/*
 * Walk a nextblk chain from `blk`, marking each page, and verify every page on it
 * decodes as `want`.  Returns the number of pages, or -1 on a violation (which is
 * described into `err`).  Bounded by nblocks so a corrupt chain that loops or
 * points forward forever terminates.
 */
static int64
wvck_walk_chain(WeaveCheckCtx *cx, BlockNumber blk, WeavePageKind want,
				StringInfo err)
{
	int64		n = 0;

	while (blk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		WeavePageKind pk;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();
		if (blk >= cx->nblocks)
		{
			appendStringInfo(err, "block %u is past the end of the relation (%u blocks)",
							 blk, cx->nblocks);
			return -1;
		}
		if (n > (int64) cx->nblocks)
		{
			appendStringInfo(err, "chain from a %s page exceeds the relation length (cycle?)",
							 weave_page_kind_name(want));
			return -1;
		}

		buf = ReadBuffer(cx->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			appendStringInfo(err, "block %u is uninitialized but is on a %s chain",
							 blk, weave_page_kind_name(want));
			return -1;
		}
		pk = WeavePageGetKind(page);
		next = WeavePageGetOpaque(page)->nextblk;
		if (WeavePageIsFreed(page))
		{
			UnlockReleaseBuffer(buf);
			appendStringInfo(err, "block %u is on a live %s chain but is flagged freed",
							 blk, weave_page_kind_name(want));
			return -1;
		}
		if (pk != want)
		{
			UnlockReleaseBuffer(buf);
			appendStringInfo(err, "block %u on a %s chain has kind \"%s\"",
							 blk, weave_page_kind_name(want),
							 weave_page_kind_name(pk));
			return -1;
		}
		UnlockReleaseBuffer(buf);
		wvck_mark(cx, blk);
		n++;
		blk = next;
	}
	return n;
}

/*
 * Invariant: every page decodes to a KNOWN kind.
 *
 * This is the invariant the v6 kind space owes.  Under the old flat bitmap an
 * unrecognized page was simply one with no bit set; now it is also one with a
 * reserved bit set, two kind bits set, or an extended id outside the allocated
 * range -- and each of those is a distinct way for a torn write or a
 * newer-than-us writer to be caught.  A page that decodes as WEAVE_PK_UNKNOWN is
 * reported rather than guessed at.
 *
 * Block 0 is exempt: the metapage does not carry WEAVE_META in every format
 * generation, which is why weave_index_size_detail() classifies it by position.
 */
static void
wvck_page_kinds(WeaveCheckCtx *cx)
{
	BlockNumber blk;
	int64		nunknown = 0;
	int64		nnew = 0;
	BlockNumber first_unknown = InvalidBlockNumber;
	uint16		first_flags = 0;
	uint16		first_kind = 0;
	StringInfoData d;

	for (blk = 1; blk < cx->nblocks; blk++)
	{
		Buffer		buf;
		Page		page;

		CHECK_FOR_INTERRUPTS();
		buf = ReadBuffer(cx->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
			nnew++;
		else if (WeavePageGetKind(page) == WEAVE_PK_UNKNOWN)
		{
			if (first_unknown == InvalidBlockNumber)
			{
				first_unknown = blk;
				first_flags = WeavePageGetOpaque(page)->flags;
				first_kind = WeavePageGetOpaque(page)->kind;
			}
			nunknown++;
		}
		UnlockReleaseBuffer(buf);
	}

	initStringInfo(&d);
	if (nunknown > 0)
		appendStringInfo(&d,
						 "%lld page(s) do not decode to a known kind; first is block %u (flags 0x%04X, kind %u)",
						 (long long) nunknown, first_unknown, first_flags,
						 first_kind);
	wvck_emit(cx, "page_kinds_decodable", nunknown == 0,
			  nunknown > 0 ? d.data : NULL);
	pfree(d.data);

	/* An extended-but-never-initialized page is not corruption -- a crash between
	 * the extend and the GenericXLog commit leaves one -- but it is worth
	 * reporting, because a growing count means a write path is losing pages. */
	if (nnew > 0)
	{
		initStringInfo(&d);
		appendStringInfo(&d, "%lld uninitialized page(s)", (long long) nnew);
		wvck_emit(cx, "no_uninitialized_pages", false, d.data);
		pfree(d.data);
	}
	else
		wvck_emit(cx, "no_uninitialized_pages", true, NULL);
}

/*
 * Invariants over one bolt's descriptor page.
 *
 * chandesc == InvalidBlockNumber means "lexical only", which is exactly what a
 * v3/v4/v5 bolt is -- so a pre-v6 bolt and a v6 lexical-only bolt are
 * indistinguishable here, which is the whole point of putting the discriminator
 * in the bolt rather than in the metapage version.
 *
 * When it is set, weave_read_chandesc() validates the page (and ERRORs on a
 * corrupt one, which is why this runs inside a subtransaction-free PG_TRY:
 * weave_check must report a corrupt descriptor page as a violated invariant, not
 * abort the whole report).
 */
static void
wvck_chandesc(WeaveCheckCtx *cx, const WeaveMetaPageData *meta)
{
	uint32		s;
	int64		nwith = 0;
	int64		nwithout = 0;
	bool		ok_reach = true;
	bool		ok_root = true;
	bool		ok_ver = true;
	StringInfoData reach;
	StringInfoData root;
	StringInfoData ver;

	initStringInfo(&reach);
	initStringInfo(&root);
	initStringInfo(&ver);

	for (s = 0; s < meta->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		const WeaveSegMeta *seg = &meta->segs[s];
		WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
		int			nweft;
		int			i;
		bool		saw_lexical = false;

		if (seg->dictstart == InvalidBlockNumber)
			continue;			/* consumed slot */

		if (seg->chandesc == InvalidBlockNumber)
		{
			nwithout++;
			/*
			 * A v6 metapage may legitimately hold a bolt with no descriptor
			 * page: it is a pre-v6 bolt that survived the metapage upcast and has
			 * not been merged yet.  What would be inconsistent is the reverse --
			 * a PRE-v6 metapage naming a descriptor page, which cannot exist --
			 * and that is caught below.
			 */
			continue;
		}

		if (meta->version < WEAVE_VERSION_CHANDESC)
		{
			ok_ver = false;
			appendStringInfo(&ver,
							 "%sbolt %u names channel descriptor block %u but the metapage is format v%u",
							 ver.len > 0 ? "; " : "", s, seg->chandesc,
							 meta->version);
			continue;
		}

		nweft = 0;
		{
			WeaveCdError err = weave_read_chandesc(cx->index, seg->chandesc,
												   weft, WEAVE_MAX_WEFTS, &nweft);

			if (err != WEAVE_CD_OK)
			{
				ok_reach = false;
				appendStringInfo(&reach, "%sbolt %u block %u: %s",
								 reach.len > 0 ? "; " : "", s, seg->chandesc,
								 weave_chandesc_errstr(err));
				continue;
			}
		}

		nwith++;
		wvck_mark(cx, seg->chandesc);

		/*
		 * Every weft the bolt claims must be rooted where the bolt directory says
		 * it is.  Today that is one claim -- the lexical weft's root must equal
		 * dictstart -- and it is worth checking precisely because it is the
		 * cross-check between the two places a bolt's geometry is written down.
		 * When the vector weft lands, its root must equal the WEAVE_VMETA page and
		 * this is where that gets asserted.
		 */
		for (i = 0; i < nweft && i < WEAVE_MAX_WEFTS; i++)
		{
			if (weft[i].kind == (uint16) WEAVE_WK_LEXICAL)
			{
				saw_lexical = true;
				if (weft[i].root != seg->dictstart)
				{
					ok_root = false;
					appendStringInfo(&root,
									 "%sbolt %u lexical weft root %u != dictstart %u",
									 root.len > 0 ? "; " : "", s,
									 weft[i].root, seg->dictstart);
				}
			}
		}
		if (!saw_lexical)
		{
			ok_root = false;
			appendStringInfo(&root,
							 "%sbolt %u has a descriptor page with no lexical weft",
							 root.len > 0 ? "; " : "", s);
		}
	}

	wvck_emit(cx, "chandesc_reachable", ok_reach, ok_reach ? NULL : reach.data);
	wvck_emit(cx, "chandesc_roots_agree", ok_root, ok_root ? NULL : root.data);
	wvck_emit(cx, "chandesc_version_consistent", ok_ver, ok_ver ? NULL : ver.data);

	/* Not an invariant, a fact worth surfacing: how many bolts self-describe.  On
	 * an index upgraded in place this is 0 until the first merge, which is what
	 * makes "read a pre-v6 index with the new code" a meaningful test. */
	{
		StringInfoData d;

		initStringInfo(&d);
		appendStringInfo(&d, "%lld self-describing, %lld lexical-only",
						 (long long) nwith, (long long) nwithout);
		wvck_emit(cx, "chandesc_coverage", true, d.data);
		pfree(d.data);
	}

	pfree(reach.data);
	pfree(root.data);
	pfree(ver.data);
}

/*
 * Bolt-directory invariants: every root block is in bounds and has the kind the
 * directory says it has.  This is the sect. 9 line "every segs[i] root block is
 * within relation bounds and has the expected kind", and walking the chains (not
 * just probing the head) is what makes the reachability set below complete.
 */
static void
wvck_segments(WeaveCheckCtx *cx, const WeaveMetaPageData *meta)
{
	uint32		s;
	bool		ok = true;
	StringInfoData d;

	initStringInfo(&d);

	if (meta->nsegments > WEAVE_MAX_SEGMENTS)
	{
		StringInfoData b;

		initStringInfo(&b);
		appendStringInfo(&b, "nsegments %u exceeds WEAVE_MAX_SEGMENTS %d",
						 meta->nsegments, WEAVE_MAX_SEGMENTS);
		wvck_emit(cx, "nsegments_bound", false, b.data);
		pfree(b.data);
		pfree(d.data);
		return;					/* segs[] cannot be walked */
	}
	wvck_emit(cx, "nsegments_bound", true, NULL);

	for (s = 0; s < meta->nsegments; s++)
	{
		const WeaveSegMeta *seg = &meta->segs[s];
		struct
		{
			BlockNumber blk;
			WeavePageKind kind;
		}			chains[] = {
			{seg->dictstart, WEAVE_PK_DICT},
			{seg->dictindexstart, WEAVE_PK_DICTINDEX},
			{seg->doclenstart, WEAVE_PK_DOCLEN},
			/*
			 * FINDING, recorded here because it is only visible from a validator:
			 * the livedocs blob is written by weave_write_blob(), which lays it on
			 * WEAVE_TRGM_DATA pages -- so WEAVE_LIVEDOCS (bit 6) is allocated in
			 * the header and in doc/specs/SEGMENT_FORMAT.md sect. 2 but NO WRITER
			 * EVER SETS IT.  weave_index_size_detail() consequently reports 0
			 * livedocs pages and counts tombstone bytes as trigram_data.  This
			 * check asserts what is actually on disk rather than what the table
			 * says; correcting the writer is a separate change, because it moves
			 * page bytes for a kind that reads fine either way.
			 */
			{seg->livedocs, WEAVE_PK_TRGM_DATA},
			{seg->trgmstart, WEAVE_PK_TRGM},
		};
		int			c;

		if (seg->dictstart == InvalidBlockNumber)
			continue;

		for (c = 0; c < (int) lengthof(chains); c++)
		{
			StringInfoData e;

			if (chains[c].blk == InvalidBlockNumber)
				continue;
			initStringInfo(&e);
			if (wvck_walk_chain(cx, chains[c].blk, chains[c].kind, &e) < 0)
			{
				ok = false;
				appendStringInfo(&d, "%sbolt %u: %s",
								 d.len > 0 ? "; " : "", s, e.data);
			}
			pfree(e.data);
		}
	}

	wvck_emit(cx, "segment_roots_have_expected_kind", ok, ok ? NULL : d.data);
	pfree(d.data);
}

/*
 * Invariant: every page is reachable from the metapage, or flagged WEAVE_FREED.
 * An unreachable unflagged page is a leak.
 *
 * Expensive (it walks every chain plus every page), so it is behind `deep`, the
 * same treatment doc/specs/SEGMENT_FORMAT.md sect. 9 prescribes for the graph
 * connectivity check.  It is also the check that actually proves the v6
 * descriptor page is both reachable while live and freed on merge -- a page kind
 * that leaked would show up here and nowhere else.
 *
 * The posting chain is walked from the FIRST dictionary entry's firstposting,
 * because all terms in a bolt share one chain; this mirrors weave_free_segment(),
 * and if the two ever disagree the leak shows up here.
 */
static void
wvck_reachable(WeaveCheckCtx *cx, const WeaveMetaPageData *meta)
{
	uint32		s;
	BlockNumber blk;
	int64		nleak = 0;
	BlockNumber firstleak = InvalidBlockNumber;
	StringInfoData d;

	wvck_mark(cx, WEAVE_METAPAGE_BLKNO);

	/* pending chain */
	if (meta->pendinghead != InvalidBlockNumber)
	{
		StringInfoData e;

		initStringInfo(&e);
		(void) wvck_walk_chain(cx, meta->pendinghead, WEAVE_PK_PENDING, &e);
		pfree(e.data);
	}

	for (s = 0; s < meta->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		const WeaveSegMeta *seg = &meta->segs[s];
		BlockNumber postchain = InvalidBlockNumber;
		StringInfoData e;

		if (seg->dictstart == InvalidBlockNumber)
			continue;

		initStringInfo(&e);
		(void) wvck_walk_chain(cx, seg->dictstart, WEAVE_PK_DICT, &e);
		(void) wvck_walk_chain(cx, seg->dictindexstart, WEAVE_PK_DICTINDEX, &e);
		(void) wvck_walk_chain(cx, seg->doclenstart, WEAVE_PK_DOCLEN, &e);
		(void) wvck_walk_chain(cx, seg->livedocs, WEAVE_PK_TRGM_DATA, &e);
		if (seg->chandesc != InvalidBlockNumber)
			(void) wvck_walk_chain(cx, seg->chandesc, WEAVE_PK_CHANDESC, &e);

		/* the shared posting chain: named by the first dict entry */
		if (seg->dictstart < cx->nblocks)
		{
			Buffer		buf = ReadBuffer(cx->index, seg->dictstart);
			Page		page;

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!PageIsNew(page) && WeavePageHasKind(page, WEAVE_PK_DICT))
			{
				char	   *ptr = (char *) PageGetContents(page);
				char	   *end = (char *) page + ((PageHeader) page)->pd_lower;

				if (ptr < end)
					postchain = ((WeaveDictEntry *) ptr)->firstposting;
			}
			UnlockReleaseBuffer(buf);
		}
		if (postchain != InvalidBlockNumber)
			(void) wvck_walk_chain(cx, postchain, WEAVE_PK_POSTING, &e);

		/* trigram directory pages plus each entry's sparsemap blob chain */
		blk = seg->trgmstart;
		while (blk != InvalidBlockNumber && blk < cx->nblocks)
		{
			Buffer		buf = ReadBuffer(cx->index, blk);
			Page		page;
			BlockNumber next = InvalidBlockNumber;

			CHECK_FOR_INTERRUPTS();
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!PageIsNew(page) && WeavePageHasKind(page, WEAVE_PK_TRGM))
			{
				char	   *ptr = (char *) PageGetContents(page);
				char	   *pend = (char *) page + ((PageHeader) page)->pd_lower;

				next = WeavePageGetOpaque(page)->nextblk;
				wvck_mark(cx, blk);
				while (ptr + sizeof(WeaveTrgmEntry) <= pend)
				{
					BlockNumber db = ((WeaveTrgmEntry *) ptr)->firstdata;

					while (db != InvalidBlockNumber && db < cx->nblocks)
					{
						Buffer		dbuf = ReadBuffer(cx->index, db);
						Page		dpage;
						BlockNumber dnext;

						LockBuffer(dbuf, BUFFER_LOCK_SHARE);
						dpage = BufferGetPage(dbuf);
						if (PageIsNew(dpage))
						{
							UnlockReleaseBuffer(dbuf);
							break;
						}
						dnext = WeavePageGetOpaque(dpage)->nextblk;
						UnlockReleaseBuffer(dbuf);
						wvck_mark(cx, db);
						db = dnext;
					}
					ptr += MAXALIGN(sizeof(WeaveTrgmEntry));
				}
			}
			UnlockReleaseBuffer(buf);
			blk = next;
		}
		pfree(e.data);
	}

	for (blk = 1; blk < cx->nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		bool		freed;
		bool		isnew;

		CHECK_FOR_INTERRUPTS();
		if ((cx->mark[blk] & WVCK_SEEN) != 0)
			continue;
		buf = ReadBuffer(cx->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		isnew = PageIsNew(page);
		freed = !isnew && WeavePageIsFreed(page);
		UnlockReleaseBuffer(buf);
		if (freed || isnew)
			continue;			/* freed pages and never-written pages are not leaks */
		if (firstleak == InvalidBlockNumber)
			firstleak = blk;
		nleak++;
	}

	initStringInfo(&d);
	if (nleak > 0)
		appendStringInfo(&d, "%lld unreachable page(s) not flagged freed; first is block %u",
						 (long long) nleak, firstleak);
	wvck_emit(cx, "pages_reachable_or_freed", nleak == 0,
			  nleak > 0 ? d.data : NULL);
	pfree(d.data);

	initStringInfo(&d);
	if (cx->noverlap > 0)
		appendStringInfo(&d, "%lld page(s) reached from two different chains",
						 (long long) cx->noverlap);
	wvck_emit(cx, "chains_do_not_overlap", cx->noverlap == 0,
			  cx->noverlap > 0 ? d.data : NULL);
	pfree(d.data);
}

/*
 * weave_check(regclass, deep boolean DEFAULT false)
 *   -> setof (invariant text, ok boolean, detail text)
 */
Datum
weave_check(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	bool		deep = PG_GETARG_BOOL(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext oldcontext;
	WeaveCheckCtx cx;
	WeaveMetaPageData meta;
	Buffer		mb;

	if (rsinfo == NULL || !(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	MemSet(&cx, 0, sizeof(cx));
	if (get_call_result_type(fcinfo, NULL, &cx.tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	cx.tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = cx.tupstore;
	rsinfo->setDesc = cx.tupdesc;
	MemoryContextSwitchTo(oldcontext);

	cx.index = index_open(indexoid, AccessShareLock);
	if (cx.index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(cx.index))));

	cx.nblocks = RelationGetNumberOfBlocks(cx.index);
	if (cx.nblocks == 0)
	{
		/* buildempty()/an unbuilt index: nothing on disk to check, and saying so
		 * is better than reporting invariants over zero pages as satisfied. */
		wvck_emit(&cx, "metapage_present", false, "relation has no blocks");
		index_close(cx.index, AccessShareLock);
		return (Datum) 0;
	}

	/*
	 * The metapage gate ERRORs on an unrecognized magic or version rather than
	 * reporting a violated invariant, because with the version unknown every
	 * later invariant would be reading segs[] at an offset it cannot justify.
	 * doc/specs/SEGMENT_FORMAT.md sect. 8 item 2.
	 */
	mb = ReadBuffer(cx.index, WEAVE_METAPAGE_BLKNO);
	LockBuffer(mb, BUFFER_LOCK_SHARE);
	weave_check_meta(BufferGetPage(mb), cx.index);
	weave_meta_from_page(BufferGetPage(mb), &meta);
	UnlockReleaseBuffer(mb);

	{
		StringInfoData d;

		initStringInfo(&d);
		appendStringInfo(&d, "format v%u (this build reads v%u..v%u, writes v%u)",
						 meta.version, WEAVE_VERSION_DOCLEN_INLINE,
						 WEAVE_VERSION, WEAVE_VERSION);
		wvck_emit(&cx, "metapage_version_recognized", true, d.data);
		pfree(d.data);
	}

	wvck_page_kinds(&cx);
	wvck_segments(&cx, &meta);
	wvck_chandesc(&cx, &meta);

	if (deep)
	{
		Size		sz = (Size) cx.nblocks * sizeof(uint8);

		cx.mark = (uint8 *) (sz > MaxAllocSize
							 ? MemoryContextAllocHuge(CurrentMemoryContext, sz)
							 : palloc(sz));
		MemSet(cx.mark, 0, sz);
		/* re-walk with marking on, so the chain walks populate the bitmap */
		cx.noverlap = 0;
		wvck_reachable(&cx, &meta);
		pfree(cx.mark);
		cx.mark = NULL;
	}

	index_close(cx.index, AccessShareLock);
	return (Datum) 0;
}
