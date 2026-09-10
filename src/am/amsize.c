/*-------------------------------------------------------------------------
 *
 * amsize.c
 *		weave_index_size_detail(): where the index bytes actually go.
 *
 * Task L10 in doc/PHASES.md, and the prerequisite for L8 and G2.
 *
 * pg_weave's index measured 115-156 MB against tsvector+GIN's 68-81 MB on a
 * 1M-document corpus (bench/RESULTS_LEXICAL.md).  That 1.7-1.9x gap cannot be
 * attacked by guessing which structure is fat: some of it is the price of storing
 * what BM25 needs (term frequencies, document lengths) and is not recoverable,
 * and some of it is per-segment fixed overhead multiplied by the segment count,
 * which is entirely recoverable.  Telling those apart requires counting.
 *
 * The implementation is deliberately the dumb, reliable one: walk every block,
 * read its page-opaque flags, bucket by page kind.  It is O(relation) and it
 * cannot disagree with pg_total_relation_size, which a bookkeeping-counter
 * approach eventually would.  This is a diagnostic run by a human or a benchmark,
 * not a hot path.
 *
 * A standalone translation unit.  It predates task L1, which split src/am/am.c the
 * same way; what it needs from am.c is declared in include/weave/am.h.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/am/amsize.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "weave/am.h"

/*
 * One row per page kind.  Order is the order the rows come back in, chosen to
 * read top-down as "metadata, term lookup, postings, per-document sidecars,
 * fuzzy support, free space".
 */
typedef struct WeaveSizeBucket
{
	const char *kind;
	WeavePageKind pk;
	int64		npages;
	int64		freebytes;		/* PageGetFreeSpace, summed */
} WeaveSizeBucket;

PG_FUNCTION_INFO_V1(weave_index_size_detail);

/*
 * weave_index_size_detail(regclass)
 *   -> (kind text, npages bigint, bytes bigint, pct float8, free_bytes bigint,
 *       free_pct float8)
 *
 * `free_bytes` is the unused space inside the pages of that kind.  It is the
 * number that says whether a structure is genuinely large or merely badly packed
 * -- a posting kind at 40% free is a packing problem, at 2% free it is honest
 * data.  Reporting size without it would leave the most actionable half of the
 * answer out.
 */
Datum
weave_index_size_detail(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;
	BlockNumber nblocks;
	BlockNumber blk;
	int64		total_pages;
	int			i;

	/*
	 * Buckets.  "freed" is separate from "unclassified" on purpose: a freed page
	 * is one that merge or vacuum released but has not returned to the
	 * filesystem, and telling that apart from a page we failed to classify is the
	 * difference between "reclaimable" and "a bug in this function".
	 *
	 * WEAVE_FREED IS A STATE, NOT A KIND.  weave_free_page sets it while LEAVING
	 * the page's kind in place, so a freed posting page decodes as POSTING and is
	 * ALSO freed.  Under the old flat bitmap this function encoded that by
	 * listing "freed" first and taking the first bit that matched; listing
	 * postings before freed counted every reclaimable page as live postings --
	 * which on a 1M-document build reported 108 MB of postings where ~37 MB were
	 * live and the rest were awaiting truncation, turning a reclaimable-space
	 * problem into an apparent data-size problem.  Since v6 the kind is decoded
	 * once by weave_page_kind_decode() and freed-ness is a separate explicit
	 * test, so the ordering of this array no longer carries that meaning -- but
	 * "freed" stays first so the report reads the same way.
	 */
	WeaveSizeBucket buckets[] = {
		{"freed", WEAVE_PK_UNKNOWN, 0, 0},	/* by state, not by kind; see below */
		{"meta", WEAVE_PK_META, 0, 0},
		{"dictionary", WEAVE_PK_DICT, 0, 0},
		{"dict_index", WEAVE_PK_DICTINDEX, 0, 0},
		{"postings", WEAVE_PK_POSTING, 0, 0},
		{"doclen_sidecar", WEAVE_PK_DOCLEN, 0, 0},
		{"livedocs", WEAVE_PK_LIVEDOCS, 0, 0},
		{"trigram_dir", WEAVE_PK_TRGM, 0, 0},
		{"trigram_data", WEAVE_PK_TRGM_DATA, 0, 0},
		{"pending", WEAVE_PK_PENDING, 0, 0},
		{"chandesc", WEAVE_PK_CHANDESC, 0, 0},
	};
	int			nbuckets = lengthof(buckets);
	int64		unknown_pages = 0;
	int64		unknown_free = 0;

	if (rsinfo == NULL || !(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(index))));

	nblocks = RelationGetNumberOfBlocks(index);
	total_pages = (int64) nblocks;

	for (blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;
		Size		freespace;
		WeavePageKind pk;
		bool		matched = false;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (PageIsNew(page))
		{
			/* An extended-but-never-initialized page still occupies a block on
			 * disk, so it counts -- as free space, under its own name, rather
			 * than silently vanishing from a report that must sum to the
			 * relation size. */
			unknown_pages++;
			unknown_free += BLCKSZ;
			UnlockReleaseBuffer(buf);
			continue;
		}

		freespace = PageGetFreeSpace(page);

		/*
		 * The metapage is block 0 and does not carry WEAVE_META in every format
		 * generation, so classify it by position.  Doing it by kind alone
		 * misfiled block 0 as unknown on an older index.
		 */
		if (blk == WEAVE_METAPAGE_BLKNO)
		{
			buckets[1].npages++;	/* "meta"; index 0 is "freed" */
			buckets[1].freebytes += (int64) freespace;
			UnlockReleaseBuffer(buf);
			continue;
		}

		if (WeavePageIsFreed(page))
		{
			buckets[0].npages++;	/* "freed": the state wins over the kind */
			buckets[0].freebytes += (int64) freespace;
			UnlockReleaseBuffer(buf);
			continue;
		}

		pk = WeavePageGetKind(page);
		for (i = 1; i < nbuckets; i++)
		{
			if (pk == buckets[i].pk)
			{
				buckets[i].npages++;
				buckets[i].freebytes += (int64) freespace;
				matched = true;
				break;			/* a page is exactly one kind */
			}
		}
		if (!matched)
		{
			unknown_pages++;
			unknown_free += (int64) freespace;
		}

		UnlockReleaseBuffer(buf);
	}

	index_close(index, AccessShareLock);

	for (i = 0; i <= nbuckets; i++)
	{
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};
		const char *kind;
		int64		npages;
		int64		freebytes;
		int64		bytes;

		if (i < nbuckets)
		{
			kind = buckets[i].kind;
			npages = buckets[i].npages;
			freebytes = buckets[i].freebytes;
		}
		else
		{
			kind = "unclassified";
			npages = unknown_pages;
			freebytes = unknown_free;
		}

		/* Emit every kind, including the zero rows.  A missing row reads as "I
		 * did not look", and the absence of, say, a trigram_dir row is
		 * informative: it means the index was built WITHOUT (trigrams=on). */
		bytes = npages * (int64) BLCKSZ;

		values[0] = CStringGetTextDatum(kind);
		values[1] = Int64GetDatum(npages);
		values[2] = Int64GetDatum(bytes);
		/* Max(,1) guards an empty relation; a report that divides by zero is
		 * worse than one that says 0%. */
		values[3] = Float8GetDatum(100.0 * (double) npages /
								   (double) Max(total_pages, 1));
		values[4] = Int64GetDatum(freebytes);
		values[5] = Float8GetDatum(100.0 * (double) freebytes /
								   (double) Max(bytes, 1));

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	return (Datum) 0;
}
