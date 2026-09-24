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
 * Single-writer only (holds a lock that excludes concurrent writers, e.g.
 * VACUUM's ShareUpdateExclusiveLock or CIC's AccessExclusiveLock).
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

	if (extend_only)
		weave_alloc_extend_only = true;
	else
		weave_alloc_begin(index);	/* gather + hand out lowest free first */

	PG_TRY();
	{
		/* rewrite all live segments once (relocates their pages) ... */
		{
			WeaveMetaPageData meta;
			uint32		sel[WEAVE_MAX_SEGMENTS];
			uint32		nsel = 0;
			uint32		i;
			Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

			LockBuffer(mb, BUFFER_LOCK_SHARE);
			weave_meta_from_page(BufferGetPage(mb), &meta);
			UnlockReleaseBuffer(mb);
			for (i = 0; i < meta.nsegments; i++)
				if (meta.segs[i].dictstart != InvalidBlockNumber)
					sel[nsel++] = i;
			if (nsel >= 1 && weave_merge_selected(index, sel, nsel))
				didwork = true;
		}

		/* ... then coalesce any remaining segments down to one */
		for (guard = 0; guard < WEAVE_MAX_SEGMENTS; guard++)
		{
			WeaveMetaPageData meta;
			uint32		sel[WEAVE_MAX_SEGMENTS];
			uint32		nsel = 0;
			uint32		i;
			Buffer		mb;

			CHECK_FOR_INTERRUPTS();	/* between merges (no lock/window held) */
			mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
			LockBuffer(mb, BUFFER_LOCK_SHARE);
			weave_meta_from_page(BufferGetPage(mb), &meta);
			UnlockReleaseBuffer(mb);
			if (meta.nsegments <= 1)
				break;
			for (i = 0; i < meta.nsegments; i++)
				if (meta.segs[i].dictstart != InvalidBlockNumber)
					sel[nsel++] = i;
			if (nsel <= 1)
				break;
			if (!weave_merge_selected(index, sel, nsel))
				break;
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

/* Truncate the contiguous run of free blocks at the end of the file back to
 * the OS.  Returns the new block count.  Scan is cancel-safe (no lock held). */
BlockNumber
weave_truncate_free_tail(Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber truncpoint = nblocks;
	BlockNumber blk;

	for (blk = nblocks; blk > 1; blk--)
	{
		CHECK_FOR_INTERRUPTS();		/* scan-only, no lock held */
		if (GetRecordedFreeSpace(index, blk - 1) >= BLCKSZ / 2)
			truncpoint = blk - 1;	/* free -> part of the truncatable tail */
		else
			break;				/* first live block from the end; stop */
	}
	if (truncpoint < nblocks)
	{
		FreeSpaceMapVacuumRange(index, truncpoint, nblocks);
		RelationTruncate(index, truncpoint);
		nblocks = truncpoint;
	}
	return nblocks;
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
	if (nlive > 1)
		return false;				/* multiple segments: pack must coalesce */

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
	return freebelow <= threshold;
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
	 * WEAVE_VACUUM_MAX_PASSES.  A final backstop guarantees we never return above
	 * the pre-call size even if the cap is hit mid-vacate.
	 *
	 * Single-writer only (holds a lock that excludes concurrent writers, e.g.
	 * VACUUM's ShareUpdateExclusiveLock or CIC's AccessExclusiveLock).
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
	 * Backstop: never return larger than we started.  Phase 1 grows the file
	 * transiently; if the pass cap were somehow hit right after a vacate, the
	 * pack phase would still have run, but guard anyway by truncating any free
	 * tail down to at most the pre-call size.
	 */
	nblocks = RelationGetNumberOfBlocks(index);
	if (nblocks > startblocks)
	{
		BlockNumber truncpoint = nblocks;
		BlockNumber blk;

		for (blk = nblocks; blk > startblocks; blk--)
		{
			CHECK_FOR_INTERRUPTS();		/* scan-only, no lock held */
			if (GetRecordedFreeSpace(index, blk - 1) >= BLCKSZ / 2)
				truncpoint = blk - 1;
			else
				break;
		}
		if (truncpoint < nblocks)
		{
			FreeSpaceMapVacuumRange(index, truncpoint, nblocks);
			RelationTruncate(index, truncpoint);
			didwork = true;
		}
	}

	return didwork;
}

/*
 * Collect the distinct docids present in a segment into a sparsemap (the
 * segment's docid "universe").  Used by bulkdelete to enumerate the TIDs the
 * vacuum callback must be asked about.
 */
static sm_t *
weave_segment_docids(Relation index, const WeaveSegMeta *seg)
{
	sm_t	   *seen = sm_create(256);
	sm_t *volatile seen_v;
	BlockNumber blk = seg->dictstart;
	uint64	   *ids = NULL;
	int			nids = 0;
	int			capids = 0;

	if (seen == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory building weave tombstone map")));

	/* seen is a libc-malloc sparsemap: free it if the page reads / bulk add
	 * below throw, else it would leak past transaction abort.  volatile: the
	 * pointer is rewritten inside PG_TRY (sm_add_many_grow may realloc) and read
	 * in PG_FINALLY. */
	seen_v = seen;
	PG_TRY();
	{
	/*
	 * Collect EVERY posting's docid across all terms into one array, then do a
	 * SINGLE bulk add.  A high-vocabulary segment has millions of low-frequency
	 * (often single-doc) terms; adding each term's postings with its own
	 * sm_add_many_grow call restarts the sparsemap cursor per call, so the adds
	 * are effectively unsorted and each re-walks the chunk chain -> O(N^2) (the
	 * CIC-validate / VACUUM spin observed at scale).  One bulk add over the full
	 * array sorts once and threads the cursor across the whole ascending run =
	 * true O(N).
	 */
	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer = ReadBuffer(index, blk);
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;
			WeavePosting *post;
			int			np,
						k;

			/*
			 * This walk runs on EVERY vacuum and every CIC validate.  Unguarded,
			 * a garbage termlen oversteps the page AND a garbage de->df is handed
			 * to weave_decode_term below.  Because the failure is inside the
			 * vacuum path it does not merely produce a wrong answer: it makes the
			 * index permanently unvacuumable, which is how the same defect
			 * presented upstream.  See doc/GAPS.md G15.
			 */
			if (!weave_dict_entry_fits(de, end))
				break;
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			np = weave_decode_term(index, de->firstposting, de->firstoffset,
								  de->df, &post, NULL, false, NULL, true,
								  seg->doclenstart == InvalidBlockNumber);
			if (np > 0)
			{
				if (nids + np > capids)
				{
					capids = Max(nids + np, capids ? capids * 2 : 4096);
					ids = ids ? WEAVE_REALLOC_MAYBE_HUGE(ids, (Size) capids * sizeof(uint64))
						: (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) capids * sizeof(uint64));
				}
				for (k = 0; k < np; k++)
					ids[nids++] = weave_tid_to_docid(&post[k].tid);
			}
			pfree(post);
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		blk = next;
	}

	if (nids > 0 && !sm_add_many_grow(&seen, ids, nids))
	{
		/* sm_add_many_grow updates *map even on a partial grow-then-fail, so the
		 * live pointer is `seen`, not the pre-call value; resync BEFORE the throw
		 * so PG_FINALLY frees the current (not a freed-by-realloc) map. */
		seen_v = seen;
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory building weave livedocs set")));
	}
	seen_v = seen;			/* sm_add_many_grow may realloc: resync cleanup ptr */
	if (ids)
		pfree(ids);
	seen_v = NULL;			/* success: ownership passes to the caller, do not free */
	}
	PG_FINALLY();
	{
		/* runs on error only (seen_v NULLed on the success path above); frees the
		 * libc-malloc map before FINALLY re-throws */
		if (seen_v)
			sm_free((sm_t *) seen_v);
	}
	PG_END_TRY();
	return seen;
}

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
	sm_t *volatile seen_v = NULL;
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
		sm_t	   *seen;
		sm_t	   *dead;
		sm_cursor_t cur = SM_CURSOR_INIT;
		uint64		v;
		uint32		ndead = 0;
		BlockNumber oldlivedocs;
		uint32		oldlen;

		if (sg->dictstart == InvalidBlockNumber)
			continue;

		seen = weave_segment_docids(index, sg);
		seen_v = seen;
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

			sm_open(&old, (uint8_t *) buf, sg->livedocslen);
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
		 * each docid in `seen` is visited exactly once, so the in-loop
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
			for (v = sm_next_member(seen, (uint64_t) -1, &cur);
				 v != SM_IDX_MAX;
				 v = sm_next_member(seen, v, &cur))
			{
				ItemPointerData tid;

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
		sm_free(seen);
		seen_v = NULL;

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
					m->segs[s].livedocs = newblk;
					m->segs[s].livedocslen = newlen;
					m->segs[s].ndeleted = ndead;
					m->generation++;	/* livedocs blob pages freed: invalidate scan snapshots */
				}
				GenericXLogFinish(st);
				UnlockReleaseBuffer(mb);
			}
		}

		/* recycle the previous tombstone blob pages */
		if (oldlivedocs != InvalidBlockNumber && oldlen > 0)
			weave_free_chain(index, oldlivedocs);
	}

	/* refresh corpus N so IDF/avgdl reflect the deletions */
	if (tuples_removed > 0)
	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
		GenericXLogState *st;
		Page		mp;
		WeaveMetaPageData *m;
		uint32		i;
		double		nd = 0;

		LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
		st = GenericXLogStart(index);
		mp = GenericXLogRegisterBuffer(st, mb, 0);
		weave_meta_upcast_page(mp);	/* v3 -> v4 before reading segs[] and writing */
		m = WeavePageGetMeta(mp);
		for (i = 0; i < m->nsegments; i++)
			nd += m->segs[i].ndocs - m->segs[i].ndeleted;
		m->ndocs = nd + m->npending;
		GenericXLogFinish(st);
		UnlockReleaseBuffer(mb);
	}
	}
	PG_FINALLY();
	{
		if (seen_v)
			sm_free((sm_t *) seen_v);
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

IndexBulkDeleteResult *
weave_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

	/* Fold any pending documents into a new segment, then compact segments. */
	if (!info->analyze_only)
	{
		/* serialize against any concurrent flush/merge/compact on this index
		 * (a user weave_merge/weave_vacuum, or another autovacuum worker): they take
		 * different relation locks that do not conflict, so this heavyweight
		 * page lock is the actual mutex.  Blocking: cleanup should run. */
		weave_maintenance_lock(info->index);
		PG_TRY();
		{
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
			 */
			{
				BlockNumber nblocks = RelationGetNumberOfBlocks(info->index);
				BlockNumber freeblks = 0;
				BlockNumber b;

				for (b = 1; b < nblocks; b++)
					if (GetRecordedFreeSpace(info->index, b) >= BLCKSZ / 2)
						freeblks++;
				/* reclaim when >= 25% of the file is free (bloated after merges) */
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
	weave_maintenance_lock(index);
	PG_TRY();
	{
		done = weave_flush_pending(index);
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
