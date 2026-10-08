/*-------------------------------------------------------------------------
 *
 * amvacuum.c
 *		ambulkdelete, amvacuumcleanup, size-floor compaction and the maintenance
 *		SQL functions for the weave access method.
 *
 * Deletes never rewrite a segment: ambulkdelete walks each segment's docids,
 * asks the vacuum callback which are dead, and swaps in a new livedocs
 * tombstone bitmap.  Space comes back later, from the merge (which drops
 * tombstoned docs) and from the compaction here, which is the only code that
 * gives blocks back to the OPERATING SYSTEM:
 *
 *	 weave_vacuum_compact()	 vacate + pack + truncate, up to
 *							 WEAVE_VACUUM_MAX_PASSES times, converging on the
 *							 size floor.  Phase 1 relocates the live segment onto
 *							 fresh high blocks so the pages it frees form one
 *							 contiguous low free region; phase 2 packs the segment
 *							 back to the front and truncates the freed tail.
 *
 * weave_merge(regclass) and weave_vacuum(regclass) are the user-callable
 * entry points; both take the maintenance lock and both refuse to run on a
 * standby or without ownership of the index.
 *
 * Split out of src/am/am.c by task L1; the interface it exports and consumes is
 * declared in include/weave/am.h, which explains why each symbol is there.
 * Nothing in here changed in the split beyond losing `static` where a caller is
 * now in a different file.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/am/amvacuum.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * The include list below is src/am/am.c's, copied verbatim into each translation
 * unit the L1 split produced.  Deliberately NOT pruned: L1 is a pure code move
 * whose whole claim is that the object code did not change, and pruning would
 * mix an unverifiable judgement call (is this header used directly, or only
 * reachable through another?) into that claim.  It is also not free to prune
 * correctly here -- weave/am.h reaches most of the backend transitively, so
 * "still compiles" does not mean "not used".  Pruning per file is a separate,
 * reviewable change.
 */
#include "postgres.h"

#include "weave/weave.h"
#include "weave/am.h"
#include "weave/sparsemap.h"			/* namespaced sparsemap (tombstones, trigrams) */
#include <math.h>
#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/transam.h"		/* ReadNextTransactionId (recycle gate) */
#include "access/xlog.h"			/* RecoveryInProgress (maintenance-fn guard) */
#include "access/xact.h"			/* ForceSyncCommit (durability of the maintenance fns) */
#include "access/parallel.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "executor/tuptable.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/buffile.h"
#include "portability/instr_time.h"
#include "catalog/storage.h"
#include "storage/condition_variable.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/acl.h"			/* object_ownercheck, aclcheck_error (maintenance-fn guard) */
#include "utils/lsyscache.h"	/* get_rel_name (maintenance-fn guard) */
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/selfuncs.h"

/*
 * Full-compaction with tail truncation, for VACUUM FULL / an explicit
 * weave_vacuum().  Merge every live segment into one, biasing allocation toward
 * the lowest free blocks so live pages pack at the front; then truncate the
 * contiguous run of free blocks at the end of the file back to the OS.  This
 * is what reclaims the physical bloat left by ordinary merges (which recycle
 * freed pages to the FSM for later reuse but never shrink the relation).
 *
 * Runs beside concurrent INSERTs under VACUUM's ShareUpdateExclusiveLock; the
 * tail truncation alone needs AccessExclusiveLock and takes it conditionally
 * (weave_truncate_tail_above, doc/GAPS.md G67).
 */

/*
 * Coalesce every live segment into a single segment, allocating either
 * low-first (extend_only=false: pack toward the front) or extend-only
 * (extend_only=true: write the whole output to fresh high blocks, vacating the
 * free region below).  Returns true if anything was written.  Not parallel:
 * the allocator hints are backend-scoped and compaction wants a deterministic
 * layout.
 */
static bool
weave_compact_to_one(Relation index, bool extend_only)
{
	bool		didwork = false;
	int			guard;
	WeaveMergeSkip sk;

	if (extend_only)
		weave_alloc_extend_only = true;
	else
		weave_alloc_begin(index);	/* gather + hand out lowest free first */

	sk.n = 0;
	PG_TRY();
	{
		/*
		 * rewrite all live segments once (relocates their pages) ...
		 *
		 * A bolt the merge's pre-flight finds DAMAGED (L23) is left out and the
		 * rest are tried again, as the leveled merge does: before this, one such
		 * bolt made both loops stop, so weave_vacuum() compacted nothing at all.
		 */
		for (guard = 0; guard < WEAVE_MAX_SEGMENTS; guard++)
		{
			WeaveMetaPageData meta;
			uint32		sel[WEAVE_MAX_SEGMENTS];
			uint32		nsel = 0;
			uint32		i;
			bool		merged;
			Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

			LockBuffer(mb, BUFFER_LOCK_SHARE);
			weave_meta_from_page(BufferGetPage(mb), &meta);
			UnlockReleaseBuffer(mb);
			for (i = 0; i < meta.nsegments; i++)
				if (meta.segs[i].dictstart != InvalidBlockNumber &&
					!weave_merge_skipped(&sk, &meta.segs[i]))
					sel[nsel++] = i;
			if (nsel < 1 ||
				!weave_merge_selected_or_skip(index, &meta, sel, nsel, &sk, &merged))
				break;
			if (merged)
			{
				didwork = true;
				break;
			}
		}

		/* ... then coalesce any remaining segments down to one */
		for (guard = 0; guard < WEAVE_MAX_SEGMENTS; guard++)
		{
			WeaveMetaPageData meta;
			uint32		sel[WEAVE_MAX_SEGMENTS];
			uint32		nsel = 0;
			uint32		i;
			bool		merged;
			Buffer		mb;

			CHECK_FOR_INTERRUPTS();	/* between merges (no lock/window held) */
			mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
			LockBuffer(mb, BUFFER_LOCK_SHARE);
			weave_meta_from_page(BufferGetPage(mb), &meta);
			UnlockReleaseBuffer(mb);
			if (meta.nsegments <= 1)
				break;
			for (i = 0; i < meta.nsegments; i++)
				if (meta.segs[i].dictstart != InvalidBlockNumber &&
					!weave_merge_skipped(&sk, &meta.segs[i]))
					sel[nsel++] = i;
			if (nsel <= 1)
				break;
			if (!weave_merge_selected_or_skip(index, &meta, sel, nsel, &sk, &merged))
				break;
			if (merged)
				didwork = true;
		}
	}
	PG_FINALLY();
	{
		if (extend_only)
			weave_alloc_extend_only = false;
		else
			weave_alloc_end();
	}
	PG_END_TRY();

	return didwork;
}

/*
 * Truncate the run of free blocks at the end of the file, down to no lower
 * than `floor`, back to the OS.  Returns the block count afterwards.
 *
 * ONLY UNDER AccessExclusiveLock (doc/GAPS.md G67).  This used to run under
 * whatever lock the caller held -- VACUUM's cleanup holds only
 * ShareUpdateExclusiveLock on the heap and RowExclusiveLock on the index, and
 * neither excludes INSERT -- and it read the free-space map with no lock at
 * all.  Two failures follow, and t/025 hit both on PG18:
 *
 *   - ROW LOSS.  The tail scan sees block B free; a concurrent inserter is then
 *     handed B (GetFreeIndexPage, or an extension that lands on it) and links
 *     pending items onto it; RelationTruncate() drops B's buffer -- dirty or
 *     not -- and the file end.  The committed rows on B are gone, and the
 *     pending chain now names a block past EOF.
 *   - "unexpected data beyond EOF".  RelationTruncate() drops the buffers
 *     before it truncates the file, so a backend that reads B in that window
 *     leaves a VALID buffer past the new end, and the next extension to B
 *     refuses it.
 *
 * Heap truncation has the same hazard and the same answer (lazy_truncate_heap):
 * take AccessExclusiveLock CONDITIONALLY, skip if anyone else is using the
 * relation, and truncate only while holding it.  A skipped truncation costs
 * disk space until a quieter VACUUM; a racing one costs rows.  Three details
 * from review, each of which heap truncation also has:
 *
 *   - NOT IN PARALLEL MODE.  Acquiring AccessExclusiveLock on a relation logs
 *     it for hot standby, which assigns an xid BEFORE the conflict check -- and
 *     assigning an xid in parallel mode is an ERROR.  weave is
 *     VACUUM_OPTION_NO_PARALLEL, so a parallel VACUUM runs its cleanup in the
 *     LEADER, still inside parallel mode; heap truncates only after leaving it.
 *     Without this check a manual VACUUM of a table with a weave index and two
 *     parallel-capable btrees aborted outright.
 *   - A CHEAP PRECHECK.  The lock is requested only if the last block is free
 *     in the FSM, so a pass with nothing to truncate costs no xid.
 *   - YIELD TO WAITERS.  The tail is verified page by page under the lock; every
 *     WEAVE_TRUNC_CHECK_EVERY blocks, if another backend is queued on the index
 *     (LockHasWaitersRelation), stop and truncate what is verified so far.
 *
 * `sole_writer` (weave_truncate_sole_writer) is the one case that needs no lock:
 * ambuild, where the index is not yet visible to any other backend (indisready
 * is false, even under CONCURRENTLY, whose build holds only RowExclusiveLock --
 * so the conditional lock would usually FAIL there and the post-build shrink of
 * G6/L8 would silently stop happening).
 *
 * Under the lock the FSM is still not trusted: it is not crash-safe, so it can
 * claim a live page is free.  A tail block is truncated only if the FSM says
 * free AND the page itself is new or flagged WEAVE_FREED.  (A page freed by a
 * build older than the flag is therefore never truncated; REINDEX reclaims it.)
 */
#define WEAVE_TRUNC_CHECK_EVERY 32

bool		weave_truncate_sole_writer = false;

static BlockNumber
weave_truncate_tail_above(Relation index, BlockNumber minblk)
{
	bool		locked_here = false;
	BlockNumber nblocks;
	BlockNumber truncpoint;
	BlockNumber blk;
	uint32		nchecked = 0;

	if (minblk < 1)
		minblk = 1;				/* never the metapage */

	nblocks = RelationGetNumberOfBlocks(index);
	if (nblocks <= minblk ||
		GetRecordedFreeSpace(index, nblocks - 1) < BLCKSZ / 2)
		return nblocks;			/* nothing to truncate: no lock, no xid */

	if (!weave_truncate_sole_writer &&
		!CheckRelationLockedByMe(index, AccessExclusiveLock, true))
	{
		if (IsInParallelMode() ||
			!ConditionalLockRelation(index, AccessExclusiveLock))
			return nblocks;		/* parallel mode, or in use: a later VACUUM */
		locked_here = true;
	}

	nblocks = RelationGetNumberOfBlocks(index);
	truncpoint = nblocks;
	for (blk = nblocks; blk > minblk; blk--)
	{
		Buffer		buf;
		Page		page;
		bool		isfree;

		CHECK_FOR_INTERRUPTS();
		if (locked_here && ++nchecked % WEAVE_TRUNC_CHECK_EVERY == 0 &&
			LockHasWaitersRelation(index, AccessExclusiveLock))
			break;				/* someone is queued: truncate what we have */
		if (GetRecordedFreeSpace(index, blk - 1) < BLCKSZ / 2)
			break;				/* first live block from the end; stop */
		buf = ReadBuffer(index, blk - 1);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		isfree = PageIsNew(page) || WeavePageIsFreed(page);
		UnlockReleaseBuffer(buf);
		if (!isfree)
			break;				/* the FSM is stale about this page: keep it */
		truncpoint = blk - 1;
	}
	if (truncpoint < nblocks)
	{
		FreeSpaceMapVacuumRange(index, truncpoint, nblocks);
		RelationTruncate(index, truncpoint);
		nblocks = truncpoint;
	}

	if (locked_here)
		UnlockRelation(index, AccessExclusiveLock);
	return nblocks;
}

/* Truncate the contiguous run of free blocks at the end of the file back to
 * the OS.  Returns the new block count; unchanged when the truncation was
 * skipped (see weave_truncate_tail_above). */
BlockNumber
weave_truncate_free_tail(Relation index)
{
	return weave_truncate_tail_above(index, 1);
}

/*
 * What fraction of this index's indexed documents are tombstoned?
 *
 * Factored out because two independent decisions need it -- the compaction floor
 * guard (weave_index_is_compacted) and the autovacuum cleanup trigger -- and when
 * only one of them knew about tombstones, the other silently disagreed about
 * whether there was work to do.  Brief shared lock on the metapage.
 */
static double
weave_tombstone_frac(Relation index)
{
	WeaveMetaPageData meta;
	Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	double		ndocs = 0;
	double		ndeleted = 0;
	uint32		i;

	LockBuffer(mb, BUFFER_LOCK_SHARE);
	weave_meta_from_page(BufferGetPage(mb), &meta);
	UnlockReleaseBuffer(mb);
	for (i = 0; i < meta.nsegments; i++)
		if (meta.segs[i].dictstart != InvalidBlockNumber)
		{
			ndocs += meta.segs[i].ndocs;
			ndeleted += meta.segs[i].ndeleted;
		}
	return ndocs > 0 ? ndeleted / ndocs : 0.0;
}

/*
 * Is the index already at its compaction floor -- i.e. would a vacate+pack
 * rewrite be pure waste?  True only when ALL THREE:
 *   (1) the live data is already front-packed (negligible free space below the
 *       highest live block), so a rewrite would only re-grow then re-truncate
 *       to the same size, and
 *   (2) there is at most ONE live segment, so there is nothing to coalesce
 *       (weave_vacuum's other job is to merge segments to one for scan speed),
 *       and
 *   (3) that segment is not mostly TOMBSTONES.
 *
 * TERM (3) IS TASK L18, AND ITS ABSENCE MADE THE STEADY STATE UNRECLAIMABLE.
 * Terms (1) and (2) are both statements about FREE SPACE and segment count, and
 * a tombstone is neither: it is a live, allocated, fully-packed page holding a
 * posting that no scan can see.  So a single segment that is 90% deleted is
 * perfectly front-packed, has nothing to coalesce, and this function called it
 * compacted -- weave_vacuum() then truncated an empty free tail, returned false,
 * and reclaimed nothing.  Measured before the fix: delete 90% of 120k rows,
 * VACUUM, weave_vacuum() twice -> 2289 to 2296 pages (it GREW by 7), false both
 * times, nsegments = 1.  Since insert-time tiered merge drives every index
 * toward exactly one segment, THE STEADY STATE WAS THE STATE THAT COULD NOT
 * RECLAIM, and the only recovery was REINDEX.  See doc/GAPS.md G14.
 *
 * Note what the fix did NOT need.  doc/GAPS.md originally diagnosed this as
 * "compaction is implemented as a merge, and a merge of one segment is a no-op
 * guarded by an nsegments > 1 precondition".  That was wrong on both counts:
 * weave_merge_selected() has no such precondition, weave_compact_to_one()
 * already calls it with nsel >= 1, and weave_merge_segments_streaming() already
 * physically drops tombstoned postings per source (ambuild.c, "tombstoned:
 * physically drop") -- so a single-segment rewrite always reclaimed correctly.
 * The rewrite machinery was complete; nothing ever asked it to run.  The whole
 * fix is this predicate learning what a tombstone is.
 *
 * Convergence is unchanged and still one pass: the rewrite writes a segment with
 * ndeleted = 0, so on the next iteration term (3) holds and the loop stops.
 *
 * Scan-only for the FSM part; a brief shared lock on the metapage for the
 * segment count and the tombstone counts.
 */
/*
 * Would a low-bias pack leave the file SMALLER than it is now?
 *
 * Which blocks the pack phase will use is not a mystery: it allocates the lowest free
 * pages first, writes the live segment into them, frees the old copies, and truncates the
 * free tail.  So the post-pass size is computable from the free space map before any of it
 * happens -- which is what lets weave_index_is_compacted() decline a pass that cannot
 * help, instead of discovering it by rewriting a multi-gigabyte segment.
 *
 *   LIVE = blocks the FSM shows in use;  FREE = blocks it shows free (blkno > 0)
 *   FREE >= LIVE  ->  every live page fits below the LIVE-th lowest free block, and the
 *                     file ends there: predicted = that block + 1
 *   FREE <  LIVE  ->  the shortfall must be extended onto fresh high blocks, which are
 *                     LIVE and therefore not truncatable: predicted = nblocks + shortfall
 *
 * VALIDATED BEFORE IT WAS BUILT, which is the only reason it is allowed to gate work
 * (hard rule 9).  /scratch/pg_weave/g47pred.sh predicts, runs the real VACUUM, and
 * compares: 6 of 6 states EXACT, error 0, on both branches, plus the two 1M x 960-d
 * states to within one page (the metapage).  The one state it got wrong is the one the
 * caller gates it out of -- see the tombstone condition there.
 *
 * Uses the same GetRecordedFreeSpace() >= BLCKSZ/2 criterion as every other term, so all
 * four agree about what "free" means.  Two scans, no lock held, cancel-safe.
 */
static bool
weave_pack_would_shrink(Relation index, BlockNumber nblocks)
{
	BlockNumber blk;
	BlockNumber live = 0;
	BlockNumber free = 0;
	BlockNumber predicted;

	for (blk = 1; blk < nblocks; blk++)
	{
		CHECK_FOR_INTERRUPTS();		/* scan-only, no lock held */
		if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2)
			free++;
		else
			live++;
	}
	if (live == 0)
		return false;				/* nothing to relocate */

	if (free >= live)
	{
		BlockNumber seen = 0;

		predicted = nblocks;		/* fallback: the loop below always finds it */
		for (blk = 1; blk < nblocks; blk++)
		{
			CHECK_FOR_INTERRUPTS();
			if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2 && ++seen == live)
			{
				predicted = blk + 1;
				break;
			}
		}
	}
	else
		predicted = nblocks + (live - free);

	return predicted < nblocks;
}

static bool
weave_index_is_compacted(Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber lastlive = 0;
	BlockNumber freebelow = 0;
	BlockNumber threshold;
	BlockNumber blk;
	uint32		nlive = 0;

	/* (2) segment count: only a single live segment counts as coalesced */
	{
		WeaveMetaPageData meta;
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
		uint32		i;

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
		for (i = 0; i < meta.nsegments; i++)
			if (meta.segs[i].dictstart != InvalidBlockNumber)
				nlive++;
	}
	/*
	 * Several bolts: a pass would coalesce them, so terms (1) and (3) do not apply.
	 * Term (4) does, and skipping it here is what made an APPEND-ONLY index rewrite
	 * itself on every plain VACUUM (task L22, bench/RESULTS_G75_RECLAIM.md "Round
	 * 2").  Measured at 1M rows, no deletes, no crash: once a level merge has left
	 * a large bolt high in the file with its write-before-free pool below it, every
	 * later cleanup sees two bolts (that one and the cycle's flush), the trigger
	 * fires on the pool, and the share-lock pass packs ~30k free pages, EXTENDS the
	 * other ~40k (its own frees are not recyclable in its own transaction), and
	 * frees the old copy -- so the next cleanup meets the same layout.  2.0x a
	 * fresh build, permanently, and 350-1,300 s per VACUUM against 1-2 s.  The
	 * prediction says exactly that before the pass: FREE < LIVE.
	 *
	 * Same conditions as term (4) below, and for the same reasons; in particular
	 * a tombstone-bearing index keeps the pass, which is the one t/028's
	 * post-DELETE truncation needs.  Giving up the coalesce is safe: the leveled
	 * merge earlier in the cleanup already bounds the bolt count, and
	 * weave_vacuum() (AccessExclusiveLock) still compacts to one.
	 *
	 * ponytail: the prediction counts the bolts' pages as they are, but a merge's
	 * output can be smaller than its inputs (shared terms), which errs toward
	 * skipping.  Exact would need the merged size; nothing measured it mattering.
	 */
	if (nlive > 1)
	{
		if (!CheckRelationLockedByMe(index, AccessExclusiveLock, true) &&
			weave_tombstone_frac(index) == 0.0 &&
			!weave_pack_would_shrink(index, nblocks))
		{
			elog(DEBUG2, "pg_weave: index \"%s\": %u bolts, no tombstones, a pack would not shrink %u pages: no pass",
				 RelationGetRelationName(index), nlive, nblocks);
			return true;
		}
		return false;				/* multiple segments: pack must coalesce */
	}

	/*
	 * (3) tombstone load.  Above the threshold a rewrite has real work to do
	 * however well packed the file is, because the space is held by invisible
	 * postings rather than by free pages.  pg_weave.vacuum_tombstone_frac
	 * defaults to 0.2, which is a convention and not a measured optimum -- see
	 * the GUC's definition in src/am/am.c.
	 */
	if (weave_tombstone_frac(index) > pg_weave_vacuum_tombstone_frac)
		return false;

	/* (1) front-packed: highest live block, then free-below count */
	for (blk = nblocks; blk > 1; blk--)
	{
		CHECK_FOR_INTERRUPTS();		/* scan-only, no lock held */
		if (GetRecordedFreeSpace(index, blk - 1) < BLCKSZ / 2)
		{
			lastlive = blk - 1;
			break;
		}
	}
	if (lastlive <= 1)
		return true;				/* empty / only the metapage: nothing to pack */

	/* count mostly-free blocks strictly below the last live block */
	for (blk = 1; blk < lastlive; blk++)
	{
		CHECK_FOR_INTERRUPTS();
		if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2)
			freebelow++;
	}
	threshold = Max(nblocks / 50, 8);
	if (freebelow <= threshold)
		return true;

	/*
	 * (4) WOULD A PASS ACTUALLY SHRINK THE FILE?  Terms (1)-(3) all describe a state;
	 * this one predicts an OUTCOME, and it is the term that stops the index rewriting
	 * itself forever (doc/GAPS.md G47, "THE MAINTAINER CALL").
	 *
	 * Measured, at 1M x 960-d: after the vacate phase was made AEL-only, every settled
	 * VACUUM cycle still reused 94,641 pages -- the whole live segment, ~740 MB -- and
	 * took ~566 s, to move the file between two sizes 2.2 % apart, forever.  Term (1)
	 * cannot see that: it counts free pages below live (90,592 against a threshold of
	 * 3,704) and concludes there is work to do.  A hole the pass cannot fill is not a
	 * reason to run the pass.
	 *
	 * ONLY WHEN THERE ARE NO TOMBSTONES TO DROP, and that condition is the whole reason
	 * this is sound.  The prediction below assumes the live page count does not change
	 * during the pass.  When the rewrite physically drops tombstoned postings it does
	 * change -- measured 1,493 -> 1,346 live pages mid-pass -- and the prediction was
	 * then WRONG BY 409 PAGES IN THE DANGEROUS DIRECTION, forecasting growth where the
	 * pass actually reclaimed 11.2 %.  Skipping on that forecast is precisely the
	 * failure the rejected option 2 was rejected for, so the term is gated on
	 * weave_tombstone_frac() == 0 rather than on the GUC's threshold: at a 10 % delete
	 * the fraction is 0.1, which is BELOW the 0.2 default, so term (3) does not protect
	 * this and cannot be made to without changing what a rewrite is triggered by.
	 *
	 * SHARE-LOCK CALLERS ONLY, AND THE MATRIX CAUGHT THAT THE HARD WAY.  The prediction
	 * models a PACK-ONLY pass, which is what a share-lock caller performs now that the
	 * vacate phase is AEL-only.  Under AccessExclusiveLock the pass is vacate+pack and its
	 * outcome is different -- the freed pages ARE recyclable in that transaction, so it
	 * reaches the size FLOOR.  Without this condition the term told weave_vacuum() its
	 * index was already compacted and the floor became unreachable: 2,578 pages instead of
	 * 1,347, a 1.91x regression in the one path that had been working.  Six cycles of the
	 * share-lock arm looked perfect while that was true, which is the argument for running
	 * the whole {vacate on, off} x {VACUUM, weave_vacuum} matrix on a change to this
	 * predicate rather than the arm the change is about.
	 *
	 * NOT THE "SKIP FOREVER" TRAP (amvacuum.c's warning above), for two reasons that are
	 * different from option 2's.  The condition is a computed OUTCOME rather than a
	 * free-space heuristic, so it cannot be satisfied by an index that has work to do;
	 * and it is a function of the live/free layout, so any insert, merge or delete that
	 * changes the layout changes the answer.  t/015 requires a shrink on the
	 * horizon-advancing arm, so a regression into permanent skipping fails a test.
	 */
	if (!CheckRelationLockedByMe(index, AccessExclusiveLock, true) &&
		weave_tombstone_frac(index) == 0.0 &&
		!weave_pack_would_shrink(index, nblocks))
		return true;

	return false;
}


/*
 * Can a single LOW-BIAS pass front-pack the index, without the vacate phase?
 *
 * weave_vacuum_compact's two-phase relocation exists for the hard case its header
 * describes: the live segment sits HIGH with the freed pages as a LOW free region
 * SMALLER than the live segment, so a plain low-bias rewrite fills the low free
 * space and then EXTENDS, straddling the file with a live tail that cannot be
 * truncated.  Phase 1 (vacate, extend-only) exists purely to make that low free
 * region big enough.
 *
 * At END OF BUILD it is already big enough, by a wide margin.  The merge writes its
 * output before freeing its inputs (write-before-free, required for crash safety),
 * so a freshly built index is roughly 70% freed pages below 30% live -- measured at
 * 110 MB freed against 46 MB live on a 1M-document build.  Paying for the vacate
 * there streams the whole segment through the buffer pool an extra time for nothing,
 * and it is why build time went 12.6 s -> 29.2 s with L8 and stands at 495.9 s on a
 * realistic corpus: an 11.0x deficit against Timescale pg_textsearch and the
 * project's largest remaining gap (doc/GAPS.md G5, task L12).
 *
 * So count the mostly-free blocks strictly below the highest live block.  If that
 * count is at least the number of live blocks, a low-bias rewrite provably fits
 * entirely at the front and phase 1 is unnecessary.
 *
 * Conservative on purpose: it must never claim one pass suffices when it does not,
 * because the index would then be left un-truncatable and the caller's convergence
 * loop would waste a full pass discovering that.  Both counts use the free space
 * map's own BLCKSZ/2 test -- the same criterion weave_index_is_compacted uses -- so
 * the two agree about what "free" means.  The comparison needs no slack: equal is
 * enough, because the pack phase frees each source page as it copies it.
 */
static bool
weave_low_free_fits_live(Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber lastlive = 0;
	BlockNumber freebelow = 0;
	BlockNumber livebelow = 0;
	BlockNumber blk;

	if (nblocks <= 2)
		return false;			/* nothing to relocate */

	for (blk = nblocks; blk > 1; blk--)
	{
		CHECK_FOR_INTERRUPTS();		/* scan-only, no lock held */
		if (GetRecordedFreeSpace(index, blk - 1) < BLCKSZ / 2)
		{
			lastlive = blk - 1;
			break;
		}
	}
	if (lastlive <= 1)
		return false;			/* empty: the caller's compacted check handles it */

	for (blk = 1; blk < lastlive; blk++)
	{
		CHECK_FOR_INTERRUPTS();
		if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2)
			freebelow++;
		else
			livebelow++;
	}
	livebelow++;				/* the highest live block relocates too */

	return freebelow >= livebelow;
}

bool
weave_vacuum_compact(Relation index)
{
	BlockNumber startblocks;
	BlockNumber nblocks;
	BlockNumber prevblocks;
	bool		didwork = false;
	int			pass;

	/*
	 * Converge a bloated index to its size floor in ONE call, stably (repeated
	 * calls do not oscillate) and NEVER returning larger than we started.
	 *
	 * THAT CONTRACT HOLDS ONLY UNDER AccessExclusiveLock, AND THE HEADER CLAIMED IT
	 * UNCONDITIONALLY UNTIL 2026-09-24 (doc/GAPS.md G47,
	 * bench/RESULTS_G47_VACATE.md).  Measured, four arms x six cycles x two reps,
	 * bit-identical:
	 *
	 *   weave_vacuum()  (AccessExclusiveLock)  1347 pages, one call, then nothing
	 *                                          -- the floor, exactly as claimed
	 *   VACUUM          (ShareUpdateExclusive) 2578 <-> 2693 forever, 1.91x floor
	 *
	 * The share-lock caller oscillates and never reaches the floor, and it cannot:
	 * reaching the floor needs pages freed by THIS transaction to be reusable
	 * within it, and under a share lock a concurrent scan may still be reading
	 * them (weave_page_recyclable()'s gate, which exists for a field-reported
	 * crash).  What was fixable was the WASTE -- the vacate phase's 1,346 extends
	 * per cycle and a 1.568x file-size swing that reclaimed nothing -- and that is
	 * fixed, below, by running phase 1 only under the lock that makes it work.  The
	 * false half of the contract is left standing here as the statement it is, with
	 * the measurement next to it, rather than quietly reworded.
	 *
	 * The hard case (verified): after ordinary merges the single live segment
	 * sits at the HIGH end of the file with the freed dead pages as a LOW free
	 * region, and that free region is SMALLER than the live segment (the file is
	 * >50% live).  A plain low-bias rewrite then fills the low free and EXTENDS
	 * the rest, so the new segment straddles the file and its tail is live --
	 * nothing is truncatable.  Iterating that rewrite just oscillates between two
	 * layouts and never reaches the floor.  (This is the "stable but never
	 * shrinks" defect; the earlier code instead oscillated and could end larger.)
	 *
	 * The fix is a two-phase relocation per pass, because a merge writes the new
	 * segment BEFORE freeing the old one (write-before-free, required for crash
	 * safety -- the old on-disk pages must stay valid until the metapage swap
	 * commits):
	 *
	 *   Phase 1 (VACATE): rewrite the segment EXTEND-ONLY, so the new copy lands
	 *     on fresh high blocks and the old pages -- wherever they were -- are all
	 *     freed.  The free region below the new (high) copy is now contiguous and
	 *     at least as large as the live segment.  The file grows transiently.
	 *
	 *   Phase 2 (PACK): rewrite the segment LOW-BIAS.  Its free list now includes
	 *     that whole low region (>= live size), so the copy fits entirely at the
	 *     front; the phase-1 high copy is freed and becomes a contiguous free
	 *     TAIL, which we truncate.  Result: front-packed at the floor.
	 *
	 * READ PHASE 2's "its free list now includes that whole low region" AS
	 * AEL-ONLY.  That sentence is the whole premise, and under a share lock it is
	 * false for every page phase 1 freed: the gather rejects them all
	 * (weave_alloc_stats()'s lowfree_defer on a grow cycle equals phase 1's own
	 * freed count, to the page).  Phase 1 is therefore skipped under a share lock;
	 * see the comment on the condition below.
	 *
	 * One vacate+pass reaches the floor in the common single-segment case; the
	 * loop re-checks and stops as soon as a pass stops shrinking, bounded by
	 * WEAVE_VACUUM_MAX_PASSES.  A final backstop truncates back to at most
	 * the pre-call size even if the cap is hit mid-vacate -- when it can take
	 * the lock the truncation needs (doc/GAPS.md G67).
	 *
	 * Concurrency: VACUUM's ShareUpdateExclusiveLock does NOT exclude INSERT
	 * (RowExclusiveLock), so this runs beside writers.  The relocation is
	 * built for that; the truncation is not, and takes AccessExclusiveLock
	 * conditionally for itself (weave_truncate_tail_above, doc/GAPS.md G67).
	 */
	startblocks = RelationGetNumberOfBlocks(index);
	prevblocks = startblocks;

	for (pass = 0; pass < WEAVE_VACUUM_MAX_PASSES; pass++)
	{
		CHECK_FOR_INTERRUPTS();		/* between passes: no lock/window held */

		/*
		 * Pre-pass convergence guard.  If the live data is already at the front
		 * of the file, a vacate+pack rewrite would only re-grow it and truncate
		 * back to the same floor -- pure waste, and the dominant cost on a large
		 * index (each rewrite streams the whole multi-GB segment through the
		 * buffer pool twice).  Just truncate any free tail and stop.  This makes
		 * a bloated index converge in ONE vacate+pack+truncate pass and an
		 * already-compact index a near-no-op (no rewrite at all).
		 */
		if (weave_index_is_compacted(index))
		{
			nblocks = weave_truncate_free_tail(index);
			if (nblocks < prevblocks)
				didwork = true;
			prevblocks = nblocks;
			break;
		}

		/*
		 * SKIP A PASS WHOSE FREE SPACE IS NOT YET REUSABLE (task L19).
		 *
		 * The vacate+pack relocation only shrinks the file if the pack phase can
		 * allocate from the pages the vacate phase freed.  Under
		 * AccessExclusiveLock weave_page_recyclable() bypasses the visibility
		 * gate, so it always can.  Under ShareUpdateExclusiveLock (autovacuum,
		 * plain VACUUM) the gate stands, and whether it lets anything through
		 * depends on whether the transaction id horizon has advanced past the xid
		 * stamped on those pages when they were freed.
		 *
		 * MEASURED, both ways, in t/015_alloc_outcomes.pl.  With the horizon
		 * advancing normally the pass works and is stable -- 1823 -> 220 pages on
		 * the first cycle and 220 on the next four, with 73 low-bias reuses per
		 * cycle.  With the horizon STALLED (an idle database: nothing consumes
		 * xids between vacuums) every candidate is rejected, every allocation
		 * extends, and the file ratchets up by a constant 73 pages per cycle
		 * forever -- 1022 -> 1166 -> 1239 -> 1312 -> 1385 -> 1458 -- while the
		 * rejected-candidate list grows with it, so the scan gets slower too.
		 *
		 * So probe first: if nothing is currently recyclable, this pass can only
		 * extend the relation, and doing nothing is strictly better.  A later
		 * cycle, once the horizon has moved, does the work.
		 *
		 * THE OBVIOUS WAY TO GET THIS WRONG is to skip forever.  A sibling project
		 * shipped this same skip for the growth half of this bug and its own test
		 * then caught the fix degrading into never reclaiming at all, because a
		 * stale free-space map made the pass look unnecessary on every cycle.  Two
		 * defences: the probe asks about RECYCLABILITY (a property of the page's
		 * freeing xid, which advances on its own) rather than about free space, and
		 * t/015 requires a shrink on the horizon-advancing arm, so a regression
		 * into permanent skipping fails a test rather than silently stopping work.
		 */
		if (!CheckRelationLockedByMe(index, AccessExclusiveLock, true) &&
			!weave_any_free_page_recyclable(index))
		{
			elog(DEBUG2, "pg_weave: index \"%s\": no free page recyclable yet: no pass",
				 RelationGetRelationName(index));
			nblocks = weave_truncate_free_tail(index);
			if (nblocks < prevblocks)
				didwork = true;
			prevblocks = nblocks;
			break;
		}

		/*
		 * Phase 1: vacate -- push the live segment onto fresh high blocks so the
		 * freed old pages form one contiguous low free region >= live size.
		 *
		 * SKIPPED IN TWO CASES, AND THE SECOND ONE IS THE LOCK WE HOLD.
		 *
		 * (a) when the low free region already exceeds the live size, which is
		 * exactly the end-of-build shape (~70% freed below ~30% live).  The vacate
		 * is a full extra rewrite of the whole segment; skipping it halves the
		 * compaction I/O of a fresh build.  Task L12 / gap G5.
		 *
		 * (b) WHEN WE DO NOT HOLD AccessExclusiveLock, because under a share lock
		 * the phase cannot do what it exists to do.  Its premise is that phase 2's
		 * low-bias free list "now includes that whole low region" -- but a page
		 * freed by the current transaction is never recyclable within it
		 * (weave_free_page stamps ReadNextTransactionId(); weave_page_recyclable
		 * asks GlobalVisCheckRemovableXid() and bypasses that gate only under AEL,
		 * where no concurrent scan can be reading the page).  So under
		 * ShareUpdateExclusiveLock every page this phase frees is rejected by the
		 * pack phase's own gather, and its extends are pure loss.
		 *
		 * MEASURED, four arms x six cycles x two reps, bit-identical
		 * (bench/RESULTS_G47_VACATE.md, doc/GAPS.md G47).  20k x 96-d weft index,
		 * plain VACUUM: with the vacate, 2578 <-> 4039 pages at 1461 extends per
		 * grow cycle; without it, 2578 <-> 2693 at 115.  The vacate phase was
		 * 1,346 of those 1,461 extends (92.1 %) and the whole visible file-size
		 * swing, and the TROUGH IS THE SAME 2,578 EITHER WAY -- it reclaimed
		 * nothing a user can see.
		 *
		 * AND THE ABLATION REFUTES DELETING IT, which is why this is a condition
		 * rather than a removal: under AEL the phase is what reaches the floor.
		 * weave_vacuum() with it converges to 1,347 pages -- the exact floor -- in
		 * ONE call and then does nothing at all for five more cycles; with the
		 * phase ablated the same caller stalls at 2,693 (2.00x the floor) and pays
		 * 115 extends every cycle forever.  Load-bearing exactly where its freed
		 * pages are recyclable in-transaction, dead weight exactly where they are
		 * not.
		 *
		 * WHAT THIS DOES NOT FIX, so nobody looks for it here: plain VACUUM still
		 * has no fixed point (2,578 <-> 2,693) and still sits at 1.91x the floor.
		 * That half of G47 is not fixable without giving up the recycle gate, and
		 * the gate is protecting against a field-reported crash.  This removes the
		 * waste; the floor remains an AEL-only outcome, as it is today.
		 *
		 * pg_weave.vacuum_vacate=off ablates the phase outright, which is the A/B
		 * arm the numbers above came from and the baseline any future change to
		 * this reasoning has to reproduce (hard rule 10).  It is a diagnostic and
		 * not a tuning knob; see the GUC's definition in src/am/customscan.c.
		 */
		if (pg_weave_vacuum_vacate &&
			CheckRelationLockedByMe(index, AccessExclusiveLock, true) &&
			!weave_low_free_fits_live(index))
		{
			if (weave_compact_to_one(index, true))
				didwork = true;
			IndexFreeSpaceMapVacuum(index);
		}

		/* Phase 2: pack -- relocate the segment to the front (its free list now
		 * spans that whole low region), freeing the phase-1 high copy. */
		if (weave_compact_to_one(index, false))
			didwork = true;
		IndexFreeSpaceMapVacuum(index);

		/* Truncate the free tail the pack phase left above the front-packed data. */
		nblocks = weave_truncate_free_tail(index);
		if (nblocks < prevblocks)
			didwork = true;

		/* Converged: a full vacate+pack+truncate pass made no further progress. */
		if (nblocks >= prevblocks)
		{
			prevblocks = nblocks;
			break;
		}
		prevblocks = nblocks;
	}

	/*
	 * Backstop: never return larger than we started -- WHEN the truncation can
	 * run.  Phase 1 grows the file transiently; if the pass cap were somehow hit
	 * right after a vacate, the pack phase would still have run, but guard
	 * anyway by truncating any free tail down to at most the pre-call size.
	 * Since doc/GAPS.md G67 the truncation is skipped when another backend is in
	 * the index (or in parallel mode), so under load the file can end a pass
	 * larger than it began; the free pages are reused and a later quiet VACUUM
	 * truncates them.
	 */
	nblocks = RelationGetNumberOfBlocks(index);
	if (nblocks > startblocks &&
		weave_truncate_tail_above(index, startblocks) < nblocks)
		didwork = true;

	return didwork;
}

/*
 * The documents bulkdelete asks the vacuum callback about come from
 * weave_segment_docset() (src/pages/doclist_page.c): the bolt's v12 document
 * list, or for a pre-v12 bolt the union of its postings, docvalues docids and
 * warp docids.  It used to be postings alone (weave_segment_docids(), now
 * weave_segment_posting_docids() in am.c), so a document with no posting -- a
 * zero-term wdoc -- was never asked about, never tombstoned, and a new row on
 * its recycled ctid inherited its docvalue and vector lane (doc/GAPS.md G80).
 */

/*
 * weave_bulkdelete: VACUUM asks us, via `callback`, which of the TIDs in the
 * index refer to now-dead heap tuples.  Because postings live in immutable
 * segments, we cannot cheaply remove individual entries; instead we maintain a
 * per-segment livedocs TOMBSTONE bitmap (a docid sparsemap of deleted docs).
 * Scans and counts subtract tombstoned docids, and the tiered merge physically
 * drops them.  This is essential for correctness: the index-only
 * scan and weave_count paths trust the visibility map, so a vacuumed+reused heap
 * slot MUST NOT still be reported as a match -- the tombstone prevents that.
 */
IndexBulkDeleteResult *
weave_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	WeaveMetaPageData meta;
	uint32		s;
	int64		num_index_tuples = 0;
	int64		tuples_removed = 0;

	/* sm_create() uses libc malloc (no palloc allocator installed), so an
	 * ereport(ERROR) between create and sm_free would leak past transaction
	 * abort.  Track the live maps here so PG_FINALLY frees them on error too.
	 * volatile: pointers are written inside PG_TRY, read in PG_FINALLY. */
	sm_t *volatile dead_v = NULL;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Serialize against a concurrent flush/merge/compact: bulkdelete reads each
	 * segment's docids (dict + posting pages) and swaps its livedocs pointer --
	 * both racy with a merge that frees/recycles those pages under a
	 * non-conflicting relation lock.  Blocking (vacuum must make progress). */
	weave_maintenance_lock(index);
	PG_TRY();
	{
	/*
	 * FLUSH FIRST (doc/GAPS.md G69).  The loop below tombstones docids it finds
	 * in SEGMENTS; a dead row still in the pending list is in none, so it used to
	 * survive this pass untouched, and the flush in vacuumcleanup then folded it
	 * into a new bolt -- a live index entry for a heap slot VACUUM had just
	 * freed.  Once the heap truncated that page, every index scan that reached
	 * the entry failed with "could not read blocks ... read only 0 of 8192
	 * bytes", the heap's own next extension failed with "page N ... should be
	 * empty but is not", and the fast count paths returned the dead rows.
	 * Reproduced in one statement sequence: 2000 post-build INSERTs, DELETE
	 * them, VACUUM.
	 *
	 * GIN has the same shape and the same answer: ginbulkdelete() runs
	 * ginInsertCleanup() before it looks at the posting tree.  Every TID in this
	 * batch's dead set was seen by the heap scan before this call, hence
	 * inserted before it, hence is in the list this flush folds; a row appended
	 * during the flush (the G61 cut) was inserted after the scan and cannot be
	 * in this batch.
	 */
	(void) weave_flush_pending(index);

	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_check_meta(BufferGetPage(mb), index);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
	}

	for (s = 0; s < meta.nsegments; s++)
	{
		WeaveSegMeta *sg = &meta.segs[s];
		WeaveDocset ds;
		sm_t	   *dead;
		Size		di;
		uint64		v;
		uint32		ndead = 0;
		uint32		ndeadnull = 0;	/* tombstoned NULL documents: not corpus N */
		BlockNumber oldlivedocs;
		uint32		oldlen;

		if (sg->dictstart == InvalidBlockNumber)
			continue;

		weave_segment_docset(index, sg, &ds);
		dead = sm_create(256);
		dead_v = dead;
		if (dead == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory building weave tombstone map")));

		/* carry forward any docids already tombstoned in this segment */
		if (sg->livedocs != InvalidBlockNumber && sg->livedocslen > 0)
		{
			uint8	   *buf = weave_read_blob(index, sg->livedocs, sg->livedocslen);
			sm_t		old;
			sm_cursor_t oc = SM_CURSOR_INIT;
			uint64		dv;
			uint64	   *carry = NULL;
			int			ncarry = 0,
						carrycap = 0;

			weave_sm_open_checked(index, sg->livedocs, "tombstone", &old,
								  (uint8 *) buf, sg->livedocslen);	/* G85 */
			for (dv = sm_next_member(&old, (uint64_t) -1, &oc);
				 dv != SM_IDX_MAX;
				 dv = sm_next_member(&old, dv, &oc))
			{
				if (ncarry >= carrycap)
				{
					/* sized by the segment's tombstone count -- corpus-scale, and
					 * this runs INSIDE bulkdelete, so a throw here does not lose one
					 * vacuum, it blocks all reclaim for as long as the index stays
					 * that size.  Same class as the doclen resident array. */
					carrycap = carrycap ? carrycap * 2 : 1024;
					carry = carry
						? WEAVE_REALLOC_MAYBE_HUGE(carry, (Size) carrycap * sizeof(uint64))
						: WEAVE_ALLOC_MAYBE_HUGE((Size) carrycap * sizeof(uint64));
				}
				carry[ncarry++] = dv;
				if (ds.nnull > 0 && weave_docids_contains(ds.nullids, ds.nnull, dv))
					ndeadnull++;
			}
			/* bulk O(N) add; one-at-a-time sm_add_grow is O(N^2) at scale */
			if (ncarry > 0 && !sm_add_many_grow(&dead, carry, ncarry))
			{
				dead_v = dead;	/* resync before throw: *map may have been realloc'd */
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory building weave tombstone set")));
			}
			dead_v = dead;		/* sm_add_many_grow may realloc: resync cleanup ptr */
			ndead += ncarry;
			if (carry)
				pfree(carry);
			pfree(buf);
		}

		/* ask the callback about each live (not-yet-tombstoned) docid.  Collect
		 * the newly-dead docids and bulk-add them to `dead` once at the end:
		 * each docid in the docset is visited exactly once, so the in-loop
		 * sm_contains() check only needs to see the carried-forward tombstones,
		 * and adding one at a time with sm_add_grow would be O(N^2). */
		{
			uint64	   *newdead = NULL;
			int			nnew = 0,
						newcap = 0;
			sm_cursor_t ccur = SM_CURSOR_INIT;

			/*
			 * ccur is declared OUT here on purpose.  `v` ascends monotonically
			 * across this loop, which is exactly the access pattern the forward
			 * cursor is for -- but the cursor used to be declared inside the loop
			 * body, so it was reset to the head on every iteration and each
			 * sm_contains() walked O(chunks) from the start.  That made this loop
			 * O(n * chunks) instead of O(n + chunks).  Same class of defect as the
			 * merge P0 in ambuild.c's merge_source_open(), and hoisted for the same
			 * reason (see bench/RESULTS_P0_MERGE_TOMBSTONE.md).  Unlike the merge,
			 * this one is merely slow: the answer was always right.
			 */
			for (di = 0; di < ds.n; di++)
			{
				ItemPointerData tid;

				v = ds.ids[di];

				num_index_tuples++;
				if (sm_contains(dead, v, &ccur))
					continue;		/* already tombstoned (carried forward) */
				weave_docid_to_tid(v, &tid);
				if (callback(&tid, callback_state))
				{
					if (nnew >= newcap)
					{
						newcap = newcap ? newcap * 2 : 1024;	/* huge-safe: see carry above */
						newdead = newdead
							? WEAVE_REALLOC_MAYBE_HUGE(newdead, (Size) newcap * sizeof(uint64))
							: WEAVE_ALLOC_MAYBE_HUGE((Size) newcap * sizeof(uint64));
					}
					newdead[nnew++] = v;
					tuples_removed++;
					if (ds.nnull > 0 &&
						weave_docids_contains(ds.nullids, ds.nnull, v))
						ndeadnull++;
				}
			}
			if (nnew > 0 && !sm_add_many_grow(&dead, newdead, nnew))
			{
				dead_v = dead;	/* resync before throw: *map may have been realloc'd */
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory building weave tombstone set")));
			}
			dead_v = dead;		/* sm_add_many_grow may realloc: resync cleanup ptr */
			ndead += nnew;
			if (newdead)
				pfree(newdead);
		}
		weave_docset_free(&ds);

		oldlivedocs = sg->livedocs;
		oldlen = sg->livedocslen;

		/* write the updated tombstone bitmap (if any) and patch the metapage */
		{
			BlockNumber newblk = InvalidBlockNumber;
			uint32		newlen = 0;

			if (ndead > 0)
			{
				newlen = (uint32) sm_get_size(dead);
				newblk = weave_write_blob(index, (const uint8 *) sm_get_data(dead),
										 newlen);
			}
			sm_free(dead);
			dead_v = NULL;


			{
				Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
				GenericXLogState *st;
				Page		mp;
				WeaveMetaPageData *m;

				LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
				st = GenericXLogStart(index);
				mp = GenericXLogRegisterBuffer(st, mb, 0);
				weave_meta_upcast_page(mp);	/* v3 -> v4 before struct write */
				m = WeavePageGetMeta(mp);
				if (s < m->nsegments)
				{
					uint32		i;
					double		nd = 0;

					m->segs[s].livedocs = newblk;
					m->segs[s].livedocslen = newlen;
					/*
					 * ndeleted is the tombstoned share of the bolt's CORPUS
					 * documents, so ndocs - ndeleted stays BM25's N: a NULL
					 * document never counted in ndocs (doc/specs/
					 * SEGMENT_FORMAT.md sect. 6 "The document list"), so its
					 * tombstone must not be subtracted from it either.  The
					 * livedocs blob still carries it -- that is what keeps a
					 * recycled ctid from inheriting its docvalue and lane.
					 */
					m->segs[s].ndeleted = ndead - ndeadnull;
					m->generation++;	/* livedocs blob pages freed: invalidate scan snapshots */

					/*
					 * Corpus N, in THIS record (doc/GAPS.md G72).  It used to be
					 * refreshed by a separate record after the last segment, so a
					 * crash or ERROR in between left ndeleted committed and ndocs
					 * still counting those rows -- and once every dead row was
					 * carried, no later VACUUM had tuples_removed > 0 to refresh
					 * it.  Recomputed over the directory just written, under its
					 * exclusive lock, so every prefix of a VACUUM's records
					 * leaves ndocs agreeing with segs[].  Unconditional: a swap
					 * that tombstoned nothing writes the value already there, and
					 * an index the old window damaged is repaired here.
					 */
					for (i = 0; i < m->nsegments; i++)
						nd += m->segs[i].ndocs - m->segs[i].ndeleted;
					m->ndocs = nd + m->npending;
				}
				GenericXLogFinish(st);
				UnlockReleaseBuffer(mb);
			}
		}

		/* recycle the previous tombstone blob pages */
		if (oldlivedocs != InvalidBlockNumber && oldlen > 0)
			weave_free_chain(index, oldlivedocs);
	}

	/* corpus N is refreshed inside each swap record above (G72) */
	}
	PG_FINALLY();
	{
		if (dead_v)
			sm_free((sm_t *) dead_v);
		weave_maintenance_unlock(index);
	}
	PG_END_TRY();

	stats->num_index_tuples = (double) (num_index_tuples - tuples_removed);
	stats->tuples_removed += (double) tuples_removed;
	stats->num_pages = RelationGetNumberOfBlocks(index);
	return stats;
}

static XLogRecPtr weave_reclaim_prepare(Relation index);

IndexBulkDeleteResult *
weave_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Fold any pending documents into a new segment, then compact segments. */
	if (!info->analyze_only)
	{
		/* the reclaim's fence and barrier, BEFORE the mutex (see the function) */
		XLogRecPtr	fence = weave_reclaim_prepare(info->index);

		/* serialize against any concurrent flush/merge/compact on this index
		 * (a user weave_merge/weave_vacuum, or another autovacuum worker): they take
		 * different relation locks that do not conflict, so this heavyweight
		 * page lock is the actual mutex.  Blocking: cleanup should run. */
		weave_maintenance_lock(info->index);
		PG_TRY();
		{
			/*
			 * Reclaim stranded pages FIRST (doc/GAPS.md G75), so the flush below
			 * reuses the pages an earlier pass freed and this one re-records.
			 *
			 * BOTH ORDERS WERE MEASURED (t/033, crashed index vs a never-crashed
			 * twin).  After the flush, the flush can never reuse what a crash left:
			 * the FSM a crash restores is stale, so each flush extends and the
			 * excess grew ~1,300 pages per cycle.  Before the flush but recording
			 * what it freed, the flush met those just-freed pages first, could not
			 * reuse them (their XID is this transaction's), and extended anyway:
			 * ~350 per cycle.  What fixes it is the pass not RECORDING what it
			 * frees (see the free arm there), so this order is right only together
			 * with that.
			 */
			(void) weave_reclaim_unreachable(info->index, fence,
											 info->message_level);
			(void) weave_flush_pending(info->index);
			weave_merge_segments(info->index);

			/*
			 * If the relation carries substantial dead space (physical size well
			 * above the live pages), reclaim it: compact to one segment reusing
			 * low blocks, then truncate the free tail.  Gated so routine
			 * autovacuum does not pay a full rewrite every pass -- only when the
			 * free tail is a meaningful fraction of the file.
			 *
			 * THIS TRIGGER IS BLIND TO TOMBSTONES, AND THAT IS NOW A MEASURED
			 * DECISION RATHER THAN AN OVERSIGHT (task L19).
			 *
			 * A tombstone is not free space -- it is a live, fully-packed page
			 * holding a posting no scan can see -- so on a delete-heavy index
			 * `freeblks` stays small, this trigger never fires, and tombstoned
			 * space is reclaimed only by an explicit weave_vacuum().  Adding
			 * `|| weave_tombstone_frac(index) > pg_weave_vacuum_tombstone_frac`
			 * here is the obvious fix.  IT WAS TRIED, MEASURED, AND REVERTED,
			 * because it produces an unbounded ratchet:
			 *
			 *   pages:  1022 -> 1166 -> 1239 -> 1312 -> 1385 -> 1458  (+73/cycle)
			 *   lowfree_reuse:     0     0      0      0      0
			 *   lowfree_defer:  1001  1092   1165   1238   1311
			 *   extend:          144    73     73     73     73
			 *
			 * The rewrite runs, frees pages, and then CANNOT REUSE ANY OF THEM:
			 * weave_page_recyclable() bypasses GlobalVisCheckRemovableXid() only
			 * under AccessExclusiveLock, and autovacuum holds
			 * ShareUpdateExclusiveLock, where a concurrent scan may still hold a
			 * directory snapshot referencing those pages.  So every allocation
			 * extends, the file grows every cycle forever, and the deferred-page
			 * list grows with it so the scan gets slower too.  This is the same
			 * failure the sibling project shipped and had to fix (35 -> 52 -> 69 MB
			 * across three no-op cleanups).
			 *
			 * The pages freed by cycle N only become recyclable in a LATER
			 * transaction, and a rewrite needs pages before it can free any, so
			 * in-cycle reclaim under a share lock is not merely unimplemented --
			 * it is circular.  Closing L19 needs a design that does not
			 * rewrite-in-place under a share lock, not this one line.
			 * t/015_alloc_outcomes.pl asserts the no-ratchet property, so adding
			 * the term fails loudly instead of shipping.
			 *
			 * THE FSM COUNT BELOW DOES NOT MISFIRE AFTER A CRASH (task L22,
			 * measured).  The stale entries a crash leaves for live pages are
			 * marked used by the reclaim above before this reads the map.
			 * Counting free pages from the pages changed nothing in t/033.  The
			 * growth L22 measured is inside weave_vacuum_compact(), in a pass that
			 * starts with fewer reusable pages than live ones; doc/PHASES.md L22.
			 */
			{
				BlockNumber nblocks = RelationGetNumberOfBlocks(info->index);
				BlockNumber freeblks = 0;
				BlockNumber b;

				for (b = 1; b < nblocks; b++)
					if (GetRecordedFreeSpace(info->index, b) >= BLCKSZ / 2)
						freeblks++;
				/* reclaim when >= 25% of the file is free (bloated after merges) */
				elog(DEBUG2, "pg_weave: index \"%s\": cleanup trigger: %u pages, %u free, tombstone fraction %.3f: %s",
					 RelationGetRelationName(info->index), nblocks, freeblks,
					 weave_tombstone_frac(info->index),
					 (nblocks > 16 && freeblks > nblocks / 4) ? "compaction" : "no compaction");
				if (nblocks > 16 && freeblks > nblocks / 4)
					(void) weave_vacuum_compact(info->index);
			}
		}
		PG_FINALLY();
		{
			weave_maintenance_unlock(info->index);
		}
		PG_END_TRY();
	}

	return stats;
}

/*
 * RECLAIM PAGES A CRASH STRANDED (doc/GAPS.md G75, G72's leak class).
 *
 * The design and its safety argument are doc/specs/SEGMENT_FORMAT.md sect. 10,
 * "Pages a crash strands between write and link"; read that before changing
 * anything here.  In short, the caller must:
 *
 *   1. read `fence` = GetXLogInsertRecPtr(),
 *   2. pass weave_segwrite_barrier() WITHOUT holding the maintenance mutex,
 *   3. take the maintenance mutex (or hold AccessExclusiveLock), and only then
 *      call this.
 *
 * Then every page below the relation length that is not reachable from the
 * metapage is one of:
 *
 *   - initialized, not WEAVE_FREED, pd_lsn <= fence: stranded by a crash or an
 *     ERROR before its publish or after its unlink.  FREED, under the same
 *     exclusive buffer lock its state was checked with.
 *   - initialized, not WEAVE_FREED, pd_lsn > fence: written after the fence by
 *     a writer this pass does not exclude.  LEFT ALONE; a later VACUUM decides.
 *   - zero, or WEAVE_FREED, and not in the FSM: the FSM is not crash-safe.
 *     RE-RECORDED, under the page's exclusive lock (the comment at that arm
 *     says what that does and does not guarantee).  Never written.
 *
 * And every REACHABLE page the FSM lists as free has a stale entry, which a
 * crash leaves; it is marked used (the comment in the loop says why that is not
 * optional).
 *
 * Frees nothing if the map is incomplete (weave_reach_map), and says so.
 * Returns the number of pages freed.
 */
int64
weave_reclaim_unreachable(Relation index, XLogRecPtr fence, int elevel)
{
	WeaveMetaPageData meta;
	BlockNumber nblocks;
	BlockNumber blk;
	uint8	   *reach;
	bool		complete;
	int64		nfreed = 0;
	int64		nrecorded = 0;
	int64		nnewer = 0;
	int64		ncontended = 0;
	int64		nstale = 0;
	int64		nnotyet = 0;
	MemoryContext ctx;
	MemoryContext old;
	instr_time	t0,
				t1;

	INSTR_TIME_SET_CURRENT(t0);
	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
	}

	/*
	 * AFTER the metapage, so every page the snapshot reaches is below it, and
	 * under the extension lock, so no extension is in flight: weave_new_buffer()
	 * holds that lock until it has the new buffer exclusively locked, and every
	 * caller keeps it locked until the page's first record.  A zero page below
	 * this length that ConditionalLockBuffer() gets is therefore abandoned.
	 */
	LockRelationForExtension(index, ExclusiveLock);
	nblocks = RelationGetNumberOfBlocks(index);
	UnlockRelationForExtension(index, ExclusiveLock);
	if (nblocks <= 1)
		return 0;

	ctx = AllocSetContextCreate(CurrentMemoryContext, "weave reclaim",
								ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(ctx);
	reach = weave_reach_map(index, &meta, nblocks, &complete);
	MemoryContextSwitchTo(old);
	if (!complete)
	{
		/* an unfollowed chain is live pages this pass would call leaked */
		ereport(LOG,
				(errmsg("pg_weave: index \"%s\": stranded-page reclaim skipped, the reachability walk could not follow every chain",
						RelationGetRelationName(index)),
				 errhint("Run weave_check('%s', true) to see which invariant fails.",
						 RelationGetRelationName(index))));
		MemoryContextDelete(ctx);
		return 0;
	}

	for (blk = 1; blk < nblocks; blk++)
	{
		Buffer		buf;
		Page		page;

		/*
		 * A REACHABLE page the FSM calls free is a stale entry -- a crash can
		 * restore an FSM page older than the page's reuse.  Mark it used: the
		 * allocator refuses a live candidate, re-records it and stops reusing
		 * for the rest of its allocation sequence (weave_new_buffer()), so the
		 * entry costs an extension every time it is met.  Safe without the
		 * page's lock: the mutex is held, so nothing frees a reachable page
		 * during this pass, and marking a page used hands it to nobody.
		 *
		 * Observed in t/033 (about 1,340 such entries after each crash); the
		 * growth this was first credited with curing was that test's own
		 * asymmetric VACUUM schedule (doc/GAPS.md G75, SUPERSEDED note), so
		 * what it saves is unmeasured.
		 */
		if (reach[blk] != 0)
		{
			if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2)
			{
				RecordUsedIndexPage(index, blk);
				nstale++;
			}
			continue;
		}
#if PG_VERSION_NUM >= 180000
		vacuum_delay_point(false);
#else
		vacuum_delay_point();
#endif

		/*
		 * NOT skipped when the FSM already calls it free.  The FSM is not
		 * crash-safe: a page a crashed writer took from it and wrote can still
		 * be recorded free after recovery (t/031's recovery points start from a
		 * base backup's FSM and hit exactly that), and skipping it would leave
		 * it stranded for good.  Freeing it is safe against a backend that took
		 * it from the FSM a moment ago: that backend either holds the buffer
		 * lock (busy, below) or will find a page freed by a current XID, which
		 * weave_page_recyclable() defers.
		 */
		buf = ReadBuffer(index, blk);
		if (!ConditionalLockBuffer(buf))
		{
			/* someone is writing it right now, so it is not stranded */
			ncontended++;
			ReleaseBuffer(buf);
			continue;
		}
		page = BufferGetPage(buf);
		/*
		 * Every FSM read and write below happens WHILE THIS BUFFER IS LOCKED
		 * EXCLUSIVELY (GIN's cleanup records pages under the page lock too).
		 * Correctness never depends on it -- the allocator refuses a page that
		 * is initialized and not WEAVE_FREED whatever the FSM says -- but it
		 * narrows the one way this pass can leave a stale FSM entry for a live
		 * page: an allocator that called GetFreeIndexPage() before we locked
		 * and ConditionalLockBuffer() after we unlocked.  One that tries to
		 * lock in between fails and drops the page (weave_new_buffer() does
		 * not re-record a contended page).  doc/specs/SEGMENT_FORMAT.md sect.
		 * 10 records the allocator change that closed the window fully and why
		 * it was reverted (it broke the compaction trigger, measured).
		 */
		if (PageIsNew(page) ||
			(PageGetSpecialSize(page) == MAXALIGN(sizeof(WeavePageOpaqueData)) &&
			 WeavePageIsFreed(page)))
		{
			/*
			 * RE-RECORD ONLY, and only a page the allocator would take now (a
			 * freed page whose XID stamp is not past the horizon waits for a
			 * later pass).  Never REMOVE an FSM entry here: that was tried, to
			 * keep a crash-restored FSM from offering pages the crashed VACUUM
			 * had just freed, and it broke compaction -- weave_vacuum_compact()'s
			 * gates count and probe FSM-free pages, so weave_vacuum() stopped
			 * compacting and sql/weave.sql's and sql/chanstats.sql's size
			 * bounds failed (pgweave-20261005-231415-cab5).  It also bought
			 * nothing: the t/033 growth it was aimed at was that test's own
			 * asymmetric VACUUM schedule.
			 */
			if (GetRecordedFreeSpace(index, blk) < BLCKSZ / 2)
			{
				if (weave_page_reusable_now(index, page))
				{
					RecordFreeIndexPage(index, blk);
					nrecorded++;
				}
				else
					nnotyet++;
			}
			UnlockReleaseBuffer(buf);
			continue;
		}
		if (PageGetLSN(page) > fence)
		{
			UnlockReleaseBuffer(buf);
			nnewer++;
			continue;
		}
		if (PageGetSpecialSize(page) != MAXALIGN(sizeof(WeavePageOpaqueData)) ||
			WeavePageGetKind(page) == WEAVE_PK_UNKNOWN)
		{
			/* not a page this AM wrote; weave_check reports it, nothing frees it */
			UnlockReleaseBuffer(buf);
			continue;
		}
		/*
		 * FREED, BUT NOT OFFERED: kept out of the FSM until the NEXT VACUUM's
		 * pass re-records it (FREED, unreachable, absent from the FSM -- the arm
		 * above).  This is measured, not a precaution.  A page freed now is
		 * stamped with an XID that is still running, so no allocation can reuse
		 * it before this transaction ends; and the live-FSM loop in
		 * weave_new_buffer() treats the first such deferred page as the end of
		 * reuse and EXTENDS for the rest of its allocation sequence.  So
		 * recording it here poisons every allocation until the next VACUUM.
		 * t/033 measured it both ways: recorded, the crashed index's excess
		 * over a never-crashed twin grew by about one flush per cycle.  And if
		 * a crash reverted the FSM to list it as free, take it off -- before
		 * the free and under the lock, for the reason given above.
		 */
		if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2)
			RecordUsedIndexPage(index, blk);
		weave_free_page_locked(index, buf, false);
		nfreed++;
	}
	MemoryContextDelete(ctx);

	if (nfreed > 0 || nrecorded > 0 || nstale > 0)
		IndexFreeSpaceMapVacuum(index);

	INSTR_TIME_SET_CURRENT(t1);
	INSTR_TIME_SUBTRACT(t1, t0);
	ereport(nfreed > 0 ? Max(elevel, LOG) : elevel,
			(errmsg("pg_weave: index \"%s\": reclaimed %lld stranded page(s), re-recorded %lld free page(s); %lld unreachable page(s) newer than the fence, %lld busy; %lld stale free-space entr(ies) for live pages cleared, %lld free page(s) not yet recyclable; %u pages walked in %.1f ms",
					RelationGetRelationName(index), (long long) nfreed,
					(long long) nrecorded, (long long) nnewer,
					(long long) ncontended, (long long) nstale,
					(long long) nnotyet, nblocks,
					INSTR_TIME_GET_MILLISEC(t1))));
	return nfreed;
}

/*
 * The caller's half of weave_reclaim_unreachable()'s contract, steps 1 and 2:
 * the fence BEFORE the barrier, and neither under the maintenance mutex.
 */
static XLogRecPtr
weave_reclaim_prepare(Relation index)
{
	XLogRecPtr	fence = GetXLogInsertRecPtr();

	weave_segwrite_barrier(index);
	return fence;
}

PG_FUNCTION_INFO_V1(weave_merge);

/*
 * Guard shared by the SQL-callable maintenance functions (weave_merge,
 * weave_vacuum).  Both take heavy locks and write WAL, and both accept an
 * arbitrary index OID from any caller, so before doing any work:
 *
 *   - refuse to run during recovery: a hot standby is read-only, and the first
 *     WAL write during recovery would fail hard (worst case a PANIC that
 *     recycles the backend).  The AM callbacks do not need this -- core never
 *     invokes them during recovery -- so the gap is only in these SQL functions.
 *   - require the caller to own the index (same owner as the underlying table):
 *     otherwise any role could trigger a costly compaction, or an
 *     AccessExclusiveLock stall (weave_vacuum), on an index it has no rights to.
 */
static void
weave_maintenance_guard(Oid indexoid, const char *fname)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("%s() cannot run during recovery", fname)));
	if (!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX,
					   get_rel_name(indexoid));
}

/* weave_merge(regclass) -> bool : merge the pending list on demand */
Datum
weave_merge(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	bool		done;

	weave_maintenance_guard(indexoid, "weave_merge");
	index = index_open(indexoid, ShareUpdateExclusiveLock);
	if (index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(index))));
	/* serialize against a concurrent flush/merge/compact (autovacuum cleanup or
	 * another weave_merge/weave_vacuum): the index SUEL does not conflict with
	 * autovacuum's TABLE lock, so this page lock is the mutex.  Blocking. */
	weave_maintenance_lock(index);
	PG_TRY();
	{
		done = weave_flush_pending(index);
		/*
		 * Also compact the segment directory to a single optimal segment.  This is
		 * what makes weave_merge() an explicit "optimize now": after a parallel build
		 * (which leaves the workers' segments unmerged for speed) or churn, one call
		 * yields a one-segment index.  The tiered auto-merge deliberately leaves
		 * several same-size segments, so it is not enough on its own here.
		 */
		if (weave_merge_all(index, true))
			done = true;
	}
	PG_FINALLY();
	{
		weave_maintenance_unlock(index);
	}
	PG_END_TRY();
	index_close(index, ShareUpdateExclusiveLock);

	/*
	 * Make this transaction's commit durable.
	 *
	 * Everything above is WAL-logged through GenericXLog, so the records exist --
	 * but a transaction that writes WAL without ever acquiring an XID does not get
	 * an XLogFlush() at commit.  RecordTransactionCommit() only flushes when the
	 * XID was marked committed, or relations were dropped, or forceSyncCommit is
	 * set; a pure index-maintenance call satisfies none of those.  The observable
	 * consequence, and the reason this is not theoretical: `pg_ctl stop -m
	 * immediate` after a successful weave_merge() rolled the merge back.
	 *
	 * ForceSyncCommit() is the idiomatic remedy and is what core uses for the same
	 * situation.  GetCurrentTransactionId() would also work by making the commit a
	 * real XID commit, but it burns an XID and moves the wraparound horizon for a
	 * read-only-by-MVCC operation, which is a worse trade.
	 *
	 * This loses work rather than data -- an unflushed merge leaves the previous
	 * segment directory intact -- but "the optimize I just ran silently did not
	 * happen" is not a defensible thing to ship.
	 */
	ForceSyncCommit();

	PG_RETURN_BOOL(done);
}

PG_FUNCTION_INFO_V1(weave_vacuum);

/*
 * weave_vacuum(regclass) -> bool : on-demand full compaction with truncation.
 * Like weave_merge(), but after compacting to one segment it reclaims the dead
 * pages left by prior merges -- packing live pages at the front of the file
 * and truncating the free tail back to the OS.  Use this to shrink an index
 * that has grown physically larger than its live contents.
 *
 * Takes AccessExclusiveLock on the index (like REINDEX): the vacate+pack phase
 * recycles just-freed pages immediately, which is only safe when no concurrent
 * scan can still be reading them.  Under the weaker ShareUpdateExclusiveLock a
 * scan could read a page mid-recycle (a rare crash); the exclusive lock is the
 * price of single-pass in-place shrink.  Autovacuum's cleanup path compacts
 * under its own SUEL and therefore keeps the recycle gate, reclaiming across
 * passes rather than corrupting a concurrent scan.
 */
Datum
weave_vacuum(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	bool		done;
	XLogRecPtr	fence;

	weave_maintenance_guard(indexoid, "weave_vacuum");
	index = index_open(indexoid, AccessExclusiveLock);
	if (index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(index))));
	/* AccessExclusiveLock on the index blocks scans/inserts on it, but NOT
	 * autovacuum's cleanup (which locks the table) -- so still take the
	 * maintenance mutex.  Blocking. */
	fence = weave_reclaim_prepare(index);
	weave_maintenance_lock(index);
	PG_TRY();
	{
		/* the same order as weave_vacuumcleanup(), for the same reason */
		done = weave_reclaim_unreachable(index, fence, DEBUG1) > 0;
		if (weave_flush_pending(index))
			done = true;
		if (weave_vacuum_compact(index))
			done = true;
	}
	PG_FINALLY();
	{
		weave_maintenance_unlock(index);
	}
	PG_END_TRY();
	index_close(index, AccessExclusiveLock);

	/* Same durability requirement as weave_merge(); see the comment there. */
	ForceSyncCommit();

	PG_RETURN_BOOL(done);
}
