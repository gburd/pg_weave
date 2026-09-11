/*-------------------------------------------------------------------------
 *
 * pg_weave_am.c
 *		The "weave" index access method for pg_weave.
 *
 * A segmented inverted index over an wdoc column, answering the @@@ operator
 * (boolean / phrase / NEAR / prefix / fuzzy / regex) and the <=> ordering
 * operator (block-max WAND / MaxScore top-k), plus a fast weave_count() path.
 * It maintains the corpus statistics BM25 needs (document count N, sum of
 * document lengths, per-term document frequency) and scores index-only.
 *
 * On-disk layout (the Lucene/Tantivy-style segmented design):
 *
 *	 block 0            metapage: N, sum(doclen), a directory of segments, and
 *							the pending write buffer pointers
 *	 per segment        a term dictionary (+ sparse block index), FOR-packed
 *							128-doc posting blocks with per-block max-tf/min-|D|
 *							impacts, a trigram index, and a livedocs tombstone
 *							bitmap
 *	 pending pages      newly inserted docs stored verbatim, searched directly
 *							until folded into a new segment by a flush
 *
 * Inserts append to the pending buffer and are immediately visible; a flush
 * (weave_merge() or VACUUM cleanup) folds pending docs into a new segment, and a
 * size-tiered merge compacts segments (dropping tombstoned docs).  Deletes are
 * recorded as per-segment livedocs tombstones by ambulkdelete.  All page writes
 * go through GenericXLog, so the index is crash-safe and replicated without a
 * custom resource manager.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_am.c
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


PG_FUNCTION_INFO_V1(weave_handler);

/*
 * Reloptions for the weave index.  Only one knob: `positions` -- whether to
 * store per-token positions in the postings so phrase/NEAR is answered
 * directly from the index (no heap recheck).  Registered once from _PG_init.
 */
typedef struct WeaveOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	bool		positions;		/* store token positions in postings (default off) */
	bool		trigrams;		/* store the per-segment trigram index (default off).
								 * The trigram index accelerates ONLY regex and
								 * over-long fuzzy terms; plain/boolean/ranked/phrase
								 * /prefix/short-fuzzy do not use it (fuzzy walks the
								 * dictionary with a Levenshtein automaton directly, and
								 * regex/long-fuzzy fall back to a full dictionary scan
								 * when it is absent -- correct, just slower).  It is ~18%
								 * of the index, so it is OFF by default; turn it on with
								 * WITH (trigrams=on) for regex/long-fuzzy-heavy workloads. */
	bool		doclen_sidecar; /* store doclen in the per-segment quantized sidecar
								 * (v4, default on); OFF stores doclen inline in each
								 * posting (the pre-1.5 layout) -- an escape hatch for a
								 * workload that wants the pre-sidecar ranked-scan
								 * behavior.  Both are read by the same self-describing
								 * decoder, so an index can mix sidecar and inline
								 * segments and needs no REINDEX to change the option
								 * (new segments follow the current setting). */
} WeaveOptions;

static relopt_kind weave_relopt_kind;

void		weave_init_reloptions(void);

void
weave_init_reloptions(void)
{
	weave_relopt_kind = add_reloption_kind();
	add_bool_reloption(weave_relopt_kind, "positions",
					   "store token positions in postings for index-only phrase/NEAR",
					   false, AccessExclusiveLock);
	add_bool_reloption(weave_relopt_kind, "trigrams",
					   "store the per-segment trigram index for regex/long-fuzzy acceleration",
					   false, AccessExclusiveLock);
	add_bool_reloption(weave_relopt_kind, "doclen_sidecar",
					   "store doclen in a per-segment quantized sidecar (on) or inline in postings (off)",
					   true, AccessExclusiveLock);
}

/* ----- posting compression (delta + varint) ----- */


/*
 * FOR (frame-of-reference) bit-packing of a block's three columns.  The codec
 * (weave_bitwidth / weave_for_pack / weave_for_unpack / weave_for_bytelen /
 * weave_for_get) lives in pg_weave_for.h as pure standalone C so the standalone
 * property tests (test/hegel/) share this exact copy -- single source of truth.
 */
#include "weave/for.h"

/*
 * Decode exactly one term's postings from the shared posting chain: start at
 * (firstblk, firstoff) and decode consecutive blocks -- following nextblk
 * across pages -- until `df` postings have been read.  A term's blocks are
 * written contiguously, so its run is delimited purely by df.  Returns the
 * count (== df on a consistent index); *out (and *blockmax if non-NULL) are
 * palloc'd.  `off` on pages after the first is the contents start.
 *
 * When want_positions is true and a block carries a positions column
 * (posbytelen>0), each posting's `pos` is set to point into a single palloc'd
 * positions arena (*posarena, returned so the caller can free it); the pointer
 * is valid until that arena is freed.  When want_positions is false the
 * positions column is SKIPPED with a pointer add (posbytelen) and never
 * decoded -- so plain BM25/AND/count queries pay ~zero for positions existing,
 * mirroring the tf/doclen bytelen-skip.
 *
 * When docids_only is true the caller wants ONLY the matching TIDs (a TidSet):
 * we still decode the gaps (docids) column and honor every corruption guard,
 * but SKIP the tf and doclen weave_for_unpack calls (about 2/3 of the per-block
 * decode work) and never decode positions.  posts[].tf/.doclen/.pos are left 0/
 * NULL, so a docids_only caller MUST NOT read them.  This is the count / set-
 * membership fast path (weave_collect_matches and the docid-only dict walks);
 * the ranked scan scores via the WAND cursor (weave_for_get), not this decoder,
 * so it is unaffected.  docids_only forces want_positions off internally.
 */
int
weave_decode_term(Relation index, BlockNumber firstblk, uint32 firstoff,
				 uint32 df, WeavePosting **out, uint32 **blockmax,
				 bool want_positions, uint32 **posarena, bool docids_only,
				 bool has_doclen_col)
{
	WeavePosting *posts;
	uint32	   *bmax = NULL;
	uint32	   *parena = NULL;
	int		   *pos_start = NULL;	/* per-posting arena offset (fixed to ptr below) */
	int			parena_n = 0;
	int			parena_cap = 0;
	int			n = 0;
	BlockNumber blk = firstblk;
	uint32		off = firstoff;

	/*
	 * docids_only implies positions are irrelevant: force want_positions off so
	 * the whole positions-column decode/arena path below is skipped along with
	 * the tf/doclen unpack.
	 */
	if (docids_only)
		want_positions = false;

	/*
	 * Clamp df to a sane ceiling before sizing the allocation.  df is read from
	 * a dictionary entry on a page pinned only BUFFER_LOCK_SHARE; a concurrent
	 * merge/vacuum can free this segment's pages while a concurrent insert
	 * recycles and overwrites them (pg_weave recycles freed pages with no
	 * deletion-xid gate), so a scan that snapshotted the directory before that
	 * can read a recycled dict page whose "df" is arbitrary -- which turned
	 * the posting allocation below into an "invalid memory alloc request size"
	 * (a multi-gigabyte request) and aborted a live query.
	 *
	 * The ceiling must be a bound no LEGITIMATE df can exceed, or we truncate
	 * real postings.  A term's df counts documents at index time, so it can
	 * exceed the current LIVE corpus size once rows are tombstoned -- the live
	 * metapage ndocs is therefore the WRONG bound (it under-counts and drops
	 * postings for a term whose docs were partly deleted).  The correct bound is
	 * the total documents ever recorded across all segments INCLUDING tombstoned
	 * ones (WeaveSegMeta.ndocs is defined as docs incl. tombstones), i.e. the sum
	 * of seg.ndocs; no term appears in more documents than exist.  A garbage df
	 * is clamped to that; the block loop then decodes only the real posting
	 * chain (delimited by nextblk), and the scan's generation re-check detects
	 * the stale read and restarts.  The metapage is effectively always resident.
	 */
	{
		WeaveMetaPageData cmeta;
		double		total = 0;
		uint32		maxdf;
		uint32		s;
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &cmeta);
		UnlockReleaseBuffer(mb);
		for (s = 0; s < cmeta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
			total += cmeta.segs[s].ndocs;	/* incl. tombstoned */
		total += cmeta.npending;		/* unmerged docs can match too */
		maxdf = (total >= (double) UINT32_MAX) ? UINT32_MAX : (uint32) total;
		if (maxdf < 1)
			maxdf = 1;
		if (df > maxdf)
			df = maxdf;
	}

	posts = (WeavePosting *) ((Size) df * sizeof(WeavePosting) > MaxAllocSize
							 ? MemoryContextAllocHuge(CurrentMemoryContext,
													 Max(df, 1u) * sizeof(WeavePosting))
							 : palloc(Max(df, 1u) * sizeof(WeavePosting)));	/* alloc-ok: huge branch of the > MaxAllocSize ternary above */
	if (blockmax)
		bmax = (uint32 *) ((Size) df * sizeof(uint32) > MaxAllocSize
						   ? MemoryContextAllocHuge(CurrentMemoryContext, Max(df, 1u) * sizeof(uint32))
						   : palloc(Max(df, 1u) * sizeof(uint32)));	/* alloc-ok: huge branch of the > MaxAllocSize ternary above */
	if (want_positions)
		pos_start = (int *) ((Size) df * sizeof(int) > MaxAllocSize
							 ? MemoryContextAllocHuge(CurrentMemoryContext, Max(df, 1u) * sizeof(int))
							 : palloc(Max(df, 1u) * sizeof(int)));	/* alloc-ok: huge branch of the > MaxAllocSize ternary above */

	while (blk != InvalidBlockNumber && n < (int) df)
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		char	   *p,
				   *pend;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		pend = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		p = (char *) page + off;
		while (p + sizeof(WeaveBlockHdr) <= pend && n < (int) df)
		{
			WeaveBlockHdr *bh = (WeaveBlockHdr *) p;
			const unsigned char *stream = (const unsigned char *) (bh + 1);
			uint64		docid = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
			uint64		gaps[WEAVE_BLOCK_SIZE];
			uint64		tfs[WEAVE_BLOCK_SIZE];
			uint64		dls[WEAVE_BLOCK_SIZE];
			int			cnt = (int) bh->count;
			int			pos = 0;
			int			i;

			if (cnt == 0)
				break;

			/*
			 * Never trust the on-disk block header's own count/lengths: a torn
			 * page, a stale-format image, or any producing bug could give a
			 * count > WEAVE_BLOCK_SIZE (which would overflow the fixed gaps/tfs/
			 * dls stack arrays via weave_for_unpack) or a bytelen/posbytelen that
			 * runs the FOR columns past the page (an out-of-bounds read).  Clamp
			 * the count (as the WAND block loader already does) and stop
			 * decoding this term at the first block whose declared payload does
			 * not fit within the page -- returning the postings decoded so far
			 * rather than reading off the end.  A corrupt block is thus a
			 * bounded, non-crashing miss; REINDEX rebuilds it from the heap.
			 *
			 * bh->count is uint32: test the unsigned value (a >2^31 count would
			 * cast to a negative int and slip past a `cnt > WEAVE_BLOCK_SIZE`
			 * check).  Anything not in [1, WEAVE_BLOCK_SIZE] is a corrupt block.
			 */
			if (bh->count == 0 || bh->count > (uint32) WEAVE_BLOCK_SIZE)
				cnt = WEAVE_BLOCK_SIZE;
			if (stream + (Size) bh->bytelen + (Size) bh->posbytelen > (const unsigned char *) pend)
			{
				ereport(WARNING,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_weave: truncated posting block in index \"%s\"; stopping term decode",
								RelationGetRelationName(index)),
						 errhint("REINDEX the index to rebuild it from the heap.")));
				UnlockReleaseBuffer(buf);
				goto done;
			}

			/*
			 * The three FOR columns must fit within the block's own declared
			 * bytelen (which the guard above proved fits within the page).
			 * weave_for_unpack consumes a byte count driven by the on-disk width
			 * byte; a corrupt width could otherwise read past the page even with
			 * a small bytelen.  Sum the three columns' declared consumption and
			 * reject the block if it overruns bytelen, before decoding any of it.
			 */
			{
				int			gl = weave_for_bytelen(stream, cnt);
				int			tl = (gl <= (int) bh->bytelen)
					? weave_for_bytelen(stream + gl, cnt) : 0;
				int			dl;

				/*
				 * Column count is SELF-DESCRIBING from bytelen: a v3 block packs
				 * three FOR columns (docid|tf|doclen) so bytes remain after gl+tl;
				 * a v4 block packs two (docid|tf, doclen is in the segment sidecar)
				 * so gl+tl == bytelen exactly.  Detecting it here -- rather than
				 * trusting the caller's has_doclen_col -- is robust across every
				 * decode path (build/merge/scan/count) and mixed v3+v4 segments.
				 */
				has_doclen_col = (gl + tl < (int) bh->bytelen);
				dl = (has_doclen_col && gl + tl <= (int) bh->bytelen)
					? weave_for_bytelen(stream + gl + tl, cnt) : 0;

				if ((Size) gl + tl + dl > (Size) bh->bytelen)
				{
					ereport(WARNING,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("pg_weave: corrupt posting block (columns overrun bytelen) in index \"%s\"; stopping term decode",
									RelationGetRelationName(index)),
							 errhint("REINDEX the index to rebuild it from the heap.")));
					UnlockReleaseBuffer(buf);
					goto done;
				}
			}
			pos += weave_for_unpack(stream + pos, cnt, gaps);
			if (!docids_only)
			{
				/*
				 * docids_only: skip the tf and doclen columns entirely.  The
				 * bytelen/column-overrun guards above already ran on all three
				 * columns, and nothing downstream in docids_only mode consumes
				 * `pos` past this point (positions use stream+bh->bytelen and the
				 * block advance uses bh->bytelen+bh->posbytelen), so leaving the
				 * tf/dl bytes undecoded is safe.  posts[].tf/.doclen stay 0.
				 */
				pos += weave_for_unpack(stream + pos, cnt, tfs);
				if (has_doclen_col)
					pos += weave_for_unpack(stream + pos, cnt, dls);	/* v3: inline doclen */
				else
					memset(dls, 0, sizeof(uint64) * cnt);	/* v4: caller fills from sidecar */
			}

			if (want_positions && bh->posbytelen > 0)
			{
				/* the positions column packs Sum(tf) delta values over the whole
				 * block; decode them once, then un-delta per posting below.  n0
				 * is the first posting index of this block. */
				const unsigned char *pstream = stream + bh->bytelen;
				uint64		deltas[WEAVE_BLOCK_SIZE * 4];
				uint64	   *dbuf = deltas;
				int			sumtf = 0;
				int			n0 = n;
				int			j;

				for (i = 0; i < cnt; i++)
					sumtf += (int) tfs[i];

				/*
				 * Sanity-bound sumtf against the declared positions bytes before
				 * trusting it: the positions column is one FOR block of sumtf
				 * values at width pstream[0], occupying exactly
				 *   width==0 ? 1 : 1 + ceil(sumtf*width/8)   bytes.
				 * A corrupt/inflated tfs[] (each value in range, but summing huge)
				 * can push sumtf far above what posbytelen actually encodes;
				 * without this check weave_for_unpack would read past the block and
				 * we would size a bogus multi-GB arena.  The existing bh->count /
				 * FOR-column guards do not catch an inflated tfs[].  Compute the
				 * exact required length in 64-bit Size arithmetic (NOT via
				 * weave_for_bytelen, whose int n*width would itself overflow on a
				 * corrupt sumtf); comparing the exact length avoids false positives
				 * on a legitimate width-0 (all-zero-delta) block with large sumtf.
				 * pstream[0] is in-bounds: the guard above proved
				 * stream+bytelen+posbytelen <= pend and posbytelen>0 here.
				 */
				{
					unsigned int pw = pstream[0];	/* FOR width byte */
					Size		need;

					if (sumtf < 0)
						need = MaxAllocSize + 1;	/* overflow -> force reject */
					else
						need = (pw == 0) ? 1
							: (Size) 1 + (((Size) sumtf * pw + 7) / 8);

					if (need > (Size) bh->posbytelen)
					{
						ereport(WARNING,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("pg_weave: corrupt posting block (positions count exceeds declared bytes) in index \"%s\"; stopping term decode",
										RelationGetRelationName(index)),
								 errhint("REINDEX the index to rebuild it from the heap.")));
						UnlockReleaseBuffer(buf);
						goto done;
					}
				}

				/*
				 * A legitimately huge sumtf (a term repeated very many times in
				 * one document) needs a huge-safe alloc: a plain palloc throws
				 * "invalid memory alloc request size" once sumtf*8 crosses
				 * MaxAllocSize, aborting any decode caller (scan/merge/bulkdelete/
				 * CIC validation).  Mirrors the write-side guard in
				 * weave_write_postings.
				 */
				if (sumtf > (int) (sizeof(deltas) / sizeof(deltas[0])))
					dbuf = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) sumtf * sizeof(uint64));
				(void) weave_for_unpack(pstream, sumtf, dbuf);

				/* grow the arena to hold this block's positions.  parena_cap*4 is
				 * likewise huge-safe (accumulated across the term's blocks). */
				if (parena_n + sumtf > parena_cap)
				{
					parena_cap = Max(parena_cap * 2, parena_n + sumtf);
					parena = parena == NULL
						? (uint32 *) WEAVE_ALLOC_MAYBE_HUGE((Size) parena_cap * sizeof(uint32))
						: (uint32 *) WEAVE_REALLOC_MAYBE_HUGE(parena, (Size) parena_cap * sizeof(uint32));
				}

				/* un-delta each posting's run (delta reset at posting boundaries) */
				j = 0;
				for (i = 0; i < cnt; i++)
				{
					int			tf = (int) tfs[i];
					uint32		run = 0;
					int			t;

					if (n0 + i < (int) df)
						pos_start[n0 + i] = parena_n;
					for (t = 0; t < tf; t++)
					{
						run += (uint32) dbuf[j++];
						parena[parena_n++] = run;
					}
				}
				if (dbuf != deltas)
					pfree(dbuf);
			}
			else if (want_positions)
			{
				/* positions absent for this block (posbytelen==0: a page-overflow
				 * block dropped them).  Mark each posting -1 so the pointer
				 * conversion yields NULL -- NOT a valid arena offset, which would
				 * alias another block's positions and misread adjacency. */
				for (i = 0; i < cnt && n + i < (int) df; i++)
					pos_start[n + i] = -1;
			}

			for (i = 0; i < cnt && n < (int) df; i++)
			{
				docid += gaps[i];
				weave_docid_to_tid(docid, &posts[n].tid);
				/* docids_only: tfs/dls were not unpacked; leave tf/doclen 0 */
				posts[n].tf = docids_only ? 0 : (uint32) tfs[i];
				posts[n].doclen = docids_only ? 0 : (uint32) dls[i];
				posts[n].pos = NULL;
				if (bmax)
					bmax[n] = bh->max_tf;
				n++;
			}
			/* skip past the three columns AND the positions column (posbytelen)
			 * -- a non-positions reader never touches the blob, only adds it */
			p = (char *) (bh + 1) + bh->bytelen + bh->posbytelen;
			p = (char *) MAXALIGN(p);
		}
		UnlockReleaseBuffer(buf);
		blk = next;
		off = MAXALIGN(SizeOfPageHeaderData);	/* later pages: contents start */
	}
done:
	/* convert per-posting arena offsets to stable pointers now the arena is final */
	if (want_positions)
	{
		int			k;

		for (k = 0; k < n; k++)
			posts[k].pos = (parena != NULL && posts[k].tf > 0 && pos_start[k] >= 0)
				? parena + pos_start[k] : NULL;
		if (pos_start)
			pfree(pos_start);
	}
	*out = posts;
	if (blockmax)
		*blockmax = bmax;
	if (posarena)
		*posarena = parena;
	else if (parena)
		pfree(parena);
	return n;
}

/* ----- writing the index pages ----- */

/*
 * Low-page-biased allocation context.  Normally weave_new_buffer() hands out
 * whatever free page the FSM offers (unordered), then extends.  During a
 * space-reclaiming compaction we instead want to pack live pages toward the
 * FRONT of the file so the dead tail can be truncated.  weave_alloc_begin()
 * gathers all currently-free blocks, sorts them ascending, and
 * weave_new_buffer() hands them out low-first; when the low-free list is
 * exhausted it falls back to the ordinary FSM/extend path.  The context is a
 * single backend-scoped hint (compaction is single-writer), reset by
 * weave_alloc_end().
 */
static BlockNumber *weave_lowfree = NULL;
static int	weave_lowfree_n = 0;
static int	weave_lowfree_i = 0;

/*
 * Extend-only allocation mode.  When set, weave_new_buffer() skips ALL free-page
 * reuse (the low-free list AND the FSM) and only extends the relation, so a
 * rewrite writes its whole output to fresh high blocks.  Used by the vacuum
 * compactor's "vacate" phase to push a live segment above the free region,
 * turning the freed old pages into one contiguous low-free run big enough for
 * the following "pack" phase to relocate the segment to the front and truncate.
 */
bool		weave_alloc_extend_only = false;

/* forward decl: the recycle gate, defined with the page-free code below */
static bool weave_page_recyclable(Relation index, Page page);

/* GUC: build finalizes to one segment only when total index <= this many MB;
 * above it the build stops at a bounded tiered set so it always converges.
 * Defined here, registered in _PG_init (pg_weave_customscan.c). */
/* Initial top-k width for a ranked WAND scan; see amscan.c and doc/GAPS.md G13.
 * Default 16 rather than the historical 100: the competitive benchmark measured
 * ranked latency to be completely k-independent (k100/k10 ratio 1.00) because a
 * LIMIT 10 query was doing a k=100 pass, while the best competitor scaled with k
 * and was 10-21x faster at k=10. */
int			pg_weave_wand_initial_k = 32;
int			pg_weave_build_collapse_max_mb = 4096;

/* GUC: per-participant flush-budget growth ceiling, in MB.  0 = keep the safe
 * default ceiling of 2 * maintenance_work_mem (unchanged behavior).  When set
 * larger, a build lets each participant's flush budget grow up to this, so a
 * large corpus flushes FEWER, LARGER segments and the live segment count stays
 * well under WEAVE_MAX_SEGMENTS (which would otherwise abort a very large
 * parallel build).  Peak build memory is about (max_parallel_maintenance_workers
 * + 1) * this ceiling -- size it against available RAM.  Defined here,
 * registered in _PG_init (pg_weave_customscan.c). */
int			pg_weave_build_mem_ceiling_mb = 0;

static int
cmp_blocknumber(const void *a, const void *b)
{
	BlockNumber x = *(const BlockNumber *) a;
	BlockNumber y = *(const BlockNumber *) b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/*
 * Gather all free blocks (via a linear FSM probe) into an ascending array so
 * subsequent weave_new_buffer() calls reuse the lowest blocks first.  Single
 * writer only.  Cheap relative to the segment rewrite it precedes.
 */
void
weave_alloc_begin(Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber blk;

	weave_lowfree_i = 0;
	weave_lowfree_n = 0;
	weave_lowfree = NULL;
	if (nblocks <= 1)
		return;
	weave_lowfree = (BlockNumber *) palloc(sizeof(BlockNumber) * nblocks);
	for (blk = 1; blk < nblocks; blk++)	/* block 0 = metapage, never free */
		if (GetRecordedFreeSpace(index, blk) >= BLCKSZ / 2)
			weave_lowfree[weave_lowfree_n++] = blk;
	if (weave_lowfree_n > 1)
		qsort(weave_lowfree, weave_lowfree_n, sizeof(BlockNumber), cmp_blocknumber);
}

void
weave_alloc_end(void)
{
	if (weave_lowfree)
		pfree(weave_lowfree);
	weave_lowfree = NULL;
	weave_lowfree_n = 0;
	weave_lowfree_i = 0;
}

Buffer
weave_new_buffer(Relation index)
{
	Buffer		buffer;

	/*
	 * Low-bias reuse: during a compaction, prefer the lowest free block so
	 * live pages pack at the front of the file.
	 */
	while (!weave_alloc_extend_only && weave_lowfree && weave_lowfree_i < weave_lowfree_n)
	{
		BlockNumber blk = weave_lowfree[weave_lowfree_i++];

		buffer = ReadBuffer(index, blk);
		if (ConditionalLockBuffer(buffer))
		{
			if (!weave_page_recyclable(index, BufferGetPage(buffer)))
			{
				/* a scan may still reference this just-freed page; leave it in
				 * the FSM for a later allocation once its horizon passes */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				RecordFreeIndexPage(index, blk);
				continue;
			}
			RecordUsedIndexPage(index, blk);
			return buffer;
		}
		ReleaseBuffer(buffer);
	}

	/* Try to reuse a page freed by a previous merge before extending. */
	while (!weave_alloc_extend_only)
	{
		BlockNumber blk = GetFreeIndexPage(index);

		if (blk == InvalidBlockNumber)
			break;				/* no free page; extend below */
		buffer = ReadBuffer(index, blk);
		if (ConditionalLockBuffer(buffer))
		{
			if (!weave_page_recyclable(index, BufferGetPage(buffer)))
			{
				/* not yet safe to reuse (a concurrent scan could still be
				 * reading it); re-record so it is handed out later, and try the
				 * next free page.  Terminates: extension is the backstop when no
				 * currently-recyclable free page exists. */
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				RecordFreeIndexPage(index, blk);
				break;
			}
			return buffer;		/* got it */
		}
		/* someone else is using it; try the next free page */
		ReleaseBuffer(buffer);
	}

	/*
	 * Extend the relation.  The relation extension lock MUST be held around the
	 * P_NEW extension whenever ANY other backend might extend the same index
	 * concurrently -- not just parallel-build participants.  A live index is
	 * extended by several unrelated, non-parallel backends at once: an INSERT
	 * flushing its pending buffer into a new segment, weave_merge() writing merged
	 * output, and VACUUM/bulkdelete rewriting.  Without the lock, two such
	 * backends race on ReadBuffer(P_NEW) and one trips "unexpected data beyond
	 * EOF in block N" (a reader/extender hitting a block past its cached EOF
	 * while another backend extends).  A field report hit exactly this running
	 * weave_merge() concurrently with live ingestion.  (This used to be gated on
	 * IsInParallelMode(), which covered only the parallel-build case and left
	 * concurrent serial extenders racing.)  The lock is held ONLY around the
	 * single P_NEW call, not the whole segment write, so concurrent writers
	 * still write their pages in parallel -- only the one-block extend
	 * serializes, which is how heap and every core index AM extend.
	 */
	LockRelationForExtension(index, ExclusiveLock);
	buffer = ReadBuffer(index, P_NEW);
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	UnlockRelationForExtension(index, ExclusiveLock);
	return buffer;
}

/*
 * Initialize a fresh page as `kind`.
 *
 * Takes a WeavePageKind, not a raw flag word, on purpose: since v6 the kind is
 * not always a bit (see the escape-bit rationale in weave/am.h), so the encoding
 * must happen in exactly one place.  A caller that passed WEAVE_VMETA as a
 * bitmask would silently produce a page with reserved bits set and no kind.
 */
void
weave_init_page(Page page, WeavePageKind kind)
{
	WeavePageOpaque opaque;

	Assert(weave_page_kind_legacy_bit(kind) != 0 ||
		   (kind >= WEAVE_PK_EXT_FIRST && kind < WEAVE_PK_NKINDS));

	PageInit(page, BLCKSZ, sizeof(WeavePageOpaqueData));
	opaque = WeavePageGetOpaque(page);
	/* One of the ten shipped kinds keeps writing the legacy one-hot bitmap, so a
	 * v6-written lexical page is byte-identical to a v5-written one; only a new
	 * kind uses the escape bit.  weave/pagekind.h owns that choice. */
	weave_page_kind_encode(kind, &opaque->flags, &opaque->kind);
	opaque->nextblk = InvalidBlockNumber;
	/* start item area at the (MAXALIGN'd) contents offset used by readers */
	((PageHeader) page)->pd_lower = (char *) PageGetContents(page) - (char *) page;
}

/*
 * Human-readable page-kind name, for weave_index_size_detail() and for
 * weave_check()'s corruption reports.  Non-static because amsize.c and amcheck.c
 * are separate translation units and duplicating this table there is how the
 * copies drift apart.
 */
const char *
weave_page_kind_name(WeavePageKind kind)
{
	switch (kind)
	{
		case WEAVE_PK_UNKNOWN:
			return "unclassified";
		case WEAVE_PK_META:
			return "meta";
		case WEAVE_PK_DICT:
			return "dictionary";
		case WEAVE_PK_POSTING:
			return "postings";
		case WEAVE_PK_PENDING:
			return "pending";
		case WEAVE_PK_TRGM:
			return "trigram_dir";
		case WEAVE_PK_TRGM_DATA:
			return "trigram_data";
		case WEAVE_PK_LIVEDOCS:
			return "livedocs";
		case WEAVE_PK_DICTINDEX:
			return "dict_index";
		case WEAVE_PK_DOCLEN:
			return "doclen_sidecar";
		case WEAVE_PK_CHANDESC:
			return "chandesc";
		case WEAVE_PK_VMETA:
			return "vector_meta";
		case WEAVE_PK_VCODES:
			return "vector_codes";
		case WEAVE_PK_VGRAPH:
			return "vector_graph";
		case WEAVE_PK_VRERANK:
			return "vector_rerank";
		case WEAVE_PK_SURF:
			return "surf_trie";
		case WEAVE_PK_ULEV:
			return "uleven_aux";
		case WEAVE_PK_REGEX:
			return "regex_cache";
		case WEAVE_PK_FUZZY_SPARE:
			return "fuzzy_spare";
		case WEAVE_PK_DOCVALS:
			return "docvalues";
		case WEAVE_PK_CGRAM:
			return "corpus_trigram";
		case WEAVE_PK_NKINDS:
			break;
	}
	return "unclassified";
}

/*
 * Version-aware metapage read (the 1.5.0 dual-read fix, extended for v6).
 *
 * 1.5.0 added BlockNumber doclenstart to WeaveSegMeta, which GREW the struct
 * (v3 48 bytes -> v4 56 bytes with padding).  WeaveSegMeta is stored INLINE in
 * the metapage's segs[] array, so a v3 metapage lays segs[] out at the 48-byte
 * stride and places `generation` right after segs[128] at the v3 offset.  A v4
 * build that cast the page straight to WeaveMetaPageData read segs[1..] and
 * generation from the wrong offsets -> garbage livedocslen (palloc(-1)) and
 * garbage dictstart (wild block seek): the two upgrade regressions.
 *
 * v6 adds BlockNumber chandesc.  It lands in the four bytes of TAIL PADDING the
 * v4/v5 struct already had (offset 52; sizeof stays 56), so unlike doclenstart it
 * does NOT change the segs[] stride and does not move `generation`.  That was
 * worth checking rather than assuming -- doc/specs/SEGMENT_FORMAT.md sect. 6
 * predicted a stride change -- and it is asserted below so a future field that
 * does not fit the padding fails the build instead of silently re-striding.
 *
 * A v3/v4/v5 page's chandesc bytes are padding and MUST NOT be trusted even
 * though every historical writer zeroed them, so the reader overwrites the field
 * with InvalidBlockNumber ("lexical only") for any version < 6.  That is the
 * whole of the v6 upgrade path for existing indexes: no page rewrite, and each
 * bolt is still read according to its own descriptor.
 *
 * This deserializes ANY supported version into an in-memory v6
 * WeaveMetaPageData.  ALL readers use this instead of casting the page directly.
 */
typedef struct WeaveSegMetaV3
{
	BlockNumber dictstart;
	BlockNumber trgmstart;
	BlockNumber livedocs;
	double		ndocs;
	double		sumdoclen;
	uint32		nterms;
	uint32		ndeleted;
	uint32		livedocslen;
	BlockNumber dictindexstart;
} WeaveSegMetaV3;

/* v3 metapage layout: same head as v4 up to segs[], then v3-stride segs[], then
 * generation.  We only need the head fields + segs[] + generation. */
typedef struct WeaveMetaPageDataV3
{
	uint32		magic;
	uint32		version;
	double		ndocs;
	double		sumdoclen;
	uint32		nsegments;
	BlockNumber pendinghead;
	BlockNumber pendingtail;
	uint32		npending;
	WeaveSegMetaV3 segs[WEAVE_MAX_SEGMENTS];
	uint32		generation;
} WeaveMetaPageDataV3;

/*
 * The v4/v5 bolt descriptor and metapage, i.e. WeaveSegMeta/WeaveMetaPageData
 * as they were before `chandesc`.  Spelled out rather than inferred from the
 * live struct so that (a) the reader below is written against the OLD layout
 * explicitly, field by field, and (b) the static asserts have something to
 * compare against.  Keeping these even though the stride happens not to have
 * moved is the point: the next field added to WeaveSegMeta will not fit the
 * padding, and at that moment this reader is already correct.
 */
typedef struct WeaveSegMetaV5
{
	BlockNumber dictstart;
	BlockNumber trgmstart;
	BlockNumber livedocs;
	double		ndocs;
	double		sumdoclen;
	uint32		nterms;
	uint32		ndeleted;
	uint32		livedocslen;
	BlockNumber dictindexstart;
	BlockNumber doclenstart;
} WeaveSegMetaV5;

typedef struct WeaveMetaPageDataV5
{
	uint32		magic;
	uint32		version;
	double		ndocs;
	double		sumdoclen;
	uint32		nsegments;
	BlockNumber pendinghead;
	BlockNumber pendingtail;
	uint32		npending;
	WeaveSegMetaV5 segs[WEAVE_MAX_SEGMENTS];
	uint32		generation;
} WeaveMetaPageDataV5;

void
weave_meta_from_page(Page page, WeaveMetaPageData *out)
{
	const WeaveMetaPageData *raw = WeavePageGetMeta(page);

	/*
	 * Layout contract (the 1.5.0 dual-read fix): the v3 read-struct and the
	 * live struct MUST agree on every field up to and including segs[0], so a
	 * v3 metapage's head + first segment are read at identical offsets; only the
	 * segs[] STRIDE (48 vs 56 bytes) and the position of `generation` differ,
	 * which weave_meta_from_page handles explicitly.  These asserts fail the build
	 * if a future field insertion silently breaks that contract again.
	 */
	StaticAssertStmt(offsetof(WeaveMetaPageDataV3, segs) == offsetof(WeaveMetaPageData, segs),
					 "v3/v4 metapage head layout diverged");
	StaticAssertStmt(offsetof(WeaveSegMetaV3, dictindexstart) == offsetof(WeaveSegMeta, dictindexstart),
					 "v3/v4 segmeta head layout diverged");

	/*
	 * v5 -> v6 contract.  chandesc must sit in the old tail padding, so the
	 * stride, the segs[] offset and the generation offset are all unchanged and a
	 * v5 metapage needs no re-striding.  If any of these ever fails, the v5 branch
	 * below must switch from a whole-struct memcpy to a per-segment expansion --
	 * which is exactly the shape the v3 branch already has, so copy that.
	 */
	StaticAssertStmt(sizeof(WeaveSegMetaV5) == sizeof(WeaveSegMeta),
					 "v6 chandesc changed the segs[] stride: expand segs[] per-segment");
	StaticAssertStmt(offsetof(WeaveMetaPageDataV5, segs) == offsetof(WeaveMetaPageData, segs),
					 "v5/v6 metapage head layout diverged");
	StaticAssertStmt(offsetof(WeaveMetaPageDataV5, generation) == offsetof(WeaveMetaPageData, generation),
					 "v6 chandesc moved `generation`: pre-v6 metapages need re-striding");
	StaticAssertStmt(offsetof(WeaveSegMetaV5, doclenstart) == offsetof(WeaveSegMeta, doclenstart),
					 "v5/v6 segmeta head layout diverged");
	StaticAssertStmt(offsetof(WeaveSegMeta, chandesc) == sizeof(WeaveSegMetaV5) - sizeof(BlockNumber),
					 "chandesc is not in the v5 tail padding");

	if (raw->version >= WEAVE_VERSION_CHANDESC)
	{
		memcpy(out, raw, sizeof(WeaveMetaPageData));
		return;
	}

	if (raw->version >= WEAVE_VERSION_DOCLEN_SIDECAR)
	{
		/*
		 * v4/v5 page.  The stride is identical (asserted above), so the head and
		 * every segs[] field through doclenstart come across in one memcpy; only
		 * chandesc has to be synthesized, because on such a page those four bytes
		 * are padding.  The cast to the V5 struct is what makes that statement
		 * checkable rather than a comment.
		 */
		const WeaveMetaPageDataV5 *v5 PG_USED_FOR_ASSERTS_ONLY =
			(const WeaveMetaPageDataV5 *) raw;
		uint32		s;

		memcpy(out, raw, sizeof(WeaveMetaPageData));
		for (s = 0; s < WEAVE_MAX_SEGMENTS; s++)
		{
			Assert(out->segs[s].doclenstart == v5->segs[s].doclenstart);
			out->segs[s].chandesc = InvalidBlockNumber; /* lexical only */
		}
		return;
	}

	/* v3 page: expand v3-stride segs[] into the current in-memory struct */
	{
		const WeaveMetaPageDataV3 *v3 = (const WeaveMetaPageDataV3 *) raw;
		uint32		s;

		MemSet(out, 0, sizeof(WeaveMetaPageData));
		out->magic = v3->magic;
		out->version = v3->version;
		out->ndocs = v3->ndocs;
		out->sumdoclen = v3->sumdoclen;
		out->nsegments = v3->nsegments;
		out->pendinghead = v3->pendinghead;
		out->pendingtail = v3->pendingtail;
		out->npending = v3->npending;
		out->generation = v3->generation;
		for (s = 0; s < WEAVE_MAX_SEGMENTS; s++)
		{
			out->segs[s].doclenstart = InvalidBlockNumber;
			out->segs[s].chandesc = InvalidBlockNumber;
		}
		for (s = 0; s < v3->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
		{
			out->segs[s].dictstart = v3->segs[s].dictstart;
			out->segs[s].trgmstart = v3->segs[s].trgmstart;
			out->segs[s].livedocs = v3->segs[s].livedocs;
			out->segs[s].ndocs = v3->segs[s].ndocs;
			out->segs[s].sumdoclen = v3->segs[s].sumdoclen;
			out->segs[s].nterms = v3->segs[s].nterms;
			out->segs[s].ndeleted = v3->segs[s].ndeleted;
			out->segs[s].livedocslen = v3->segs[s].livedocslen;
			out->segs[s].dictindexstart = v3->segs[s].dictindexstart;
			out->segs[s].doclenstart = InvalidBlockNumber;	/* v3: inline doclen */
			out->segs[s].chandesc = InvalidBlockNumber; /* v3: lexical only */
		}
	}
}

/*
 * Upcast an older metapage to the current in-place layout under the caller's
 * exclusive lock, via GenericXLog, so subsequent in-place struct writes are
 * correct.  Idempotent: a no-op if the page is already current.  MUST be called
 * (under the metapage's exclusive lock, before read-modify-writing it) by every
 * path that mutates the metapage in place (add-segment, merge, bulkdelete
 * livedocs swap).  `page` is a GenericXLog-registered writable copy.
 *
 * For a v4/v5 page this rewrites nothing but the version word and the per-segment
 * chandesc (Invalid), because the stride did not move -- but it must still run,
 * or a v4/v5 metapage's padding bytes would be left as the chandesc of every
 * bolt and a merge would then write a real chandesc into a directory whose other
 * entries still hold padding.  For a v6 page it rewrites ONLY the version word,
 * and it must still run for a reason that is not about layout at all: the moment
 * a bolt in this directory carries a fuzzy weft, an older .so reading the
 * relation would free every weft it knows and leak the trie on each merge.  The
 * version word is what stops it (weave_check_meta), so the gate has to be tested
 * against WEAVE_VERSION and not against the last version that moved a field.
 */
void
weave_meta_upcast_page(Page page)
{
	WeaveMetaPageData tmp;
	WeaveMetaPageData *m;

	if (WeavePageGetMeta(page)->version >= WEAVE_VERSION)
		return;

	weave_meta_from_page(page, &tmp);	/* read old into a current-shaped temp */
	tmp.version = WEAVE_VERSION;
	m = WeavePageGetMeta(page);
	MemSet(m, 0, sizeof(WeaveMetaPageData));
	memcpy(m, &tmp, sizeof(WeaveMetaPageData));
	((PageHeader) page)->pd_lower =
		((char *) m + sizeof(WeaveMetaPageData)) - (char *) page;
}

void
weave_init_metapage(Relation index)
{
	Buffer		buffer;
	GenericXLogState *state;
	Page		page;
	WeaveMetaPageData *meta;

	buffer = weave_new_buffer(index);
	Assert(BufferGetBlockNumber(buffer) == WEAVE_METAPAGE_BLKNO);

	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
	weave_init_page(page, WEAVE_PK_META);
	meta = WeavePageGetMeta(page);
	MemSet(meta, 0, sizeof(WeaveMetaPageData));
	meta->magic = WEAVE_MAGIC;
	meta->version = WEAVE_VERSION;
	meta->ndocs = 0;
	meta->sumdoclen = 0;
	meta->nsegments = 0;
	meta->pendinghead = InvalidBlockNumber;
	meta->pendingtail = InvalidBlockNumber;
	meta->npending = 0;
	((PageHeader) page)->pd_lower =
		((char *) meta + sizeof(WeaveMetaPageData)) - (char *) page;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buffer);
}

/*
 * Validate a metapage's magic and format version before trusting its contents.
 * Guards against a pg_weave shared library reading an index written by an
 * incompatible on-disk format (e.g. a .so upgraded/downgraded out of step with
 * the physical index) — the classic ".so vs catalog/on-disk skew".  Callers
 * pass the metapage of an index being opened for scan/insert/maintenance; a
 * mismatch raises a clear, actionable error rather than silently misreading
 * bytes.
 */
void
weave_check_meta(Page page, Relation index)
{
	WeaveMetaPageData *meta = WeavePageGetMeta(page);

	if (meta->magic != WEAVE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a valid pg_weave index",
						RelationGetRelationName(index)),
				 errdetail("Metapage magic 0x%08X does not match the expected 0x%08X.",
						   meta->magic, WEAVE_MAGIC)));

	if (meta->version < WEAVE_VERSION_DOCLEN_INLINE || meta->version > WEAVE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has pg_weave on-disk format version %u, but this build supports versions %u..%u",
						RelationGetRelationName(index),
						meta->version, WEAVE_VERSION_DOCLEN_INLINE, WEAVE_VERSION),
				 errhint("REINDEX the index to rebuild it in the current format.")));
}

/* ---- cursored doclen sidecar lookup (scan path) ----------------------------
 *
 * The doclen sidecar (v4) stores one quantized length byte per doc on a
 * WEAVE_DOCLEN page chain (128-doc blocks).  A ranked scan needs a doc's length
 * to score it, but doclen no longer travels with the posting, so the sidecar
 * must be probed by docid.  Two failed approaches bracket this one:
 *   - per-posting page walk (<=1.5.3): ~1 buffer per scored posting -> whole
 *     chain per query (16,887 buffers on a common term).
 *   - decode the WHOLE segment sidecar once per scan (1.5.4-1.5.7): a fixed
 *     ~18ms tax per ranked scan on a 2.19M-doc segment (534 page reads + a
 *     FOR-unpack of every block) that dwarfs rare/mid-term scoring -- the
 *     1.5.7 5-way regression (rare ranked 25ms vs 1.6ms inline).
 *
 * This is a PAGE-DIRECTORY cursor: a tiny (first_docid, blk) entry per sidecar
 * PAGE, built by walking only page HEADERS (no block decode), cached in the
 * relcache (rd_amcache, ONE contiguous chunk, keyed by metapage generation) so
 * it is built at most once per backend, not per scan.  A lookup binary-searches
 * the directory to the covering page, reads+decodes ONLY that page, and keeps
 * it resident (the WAND scan visits docids ascending, so consecutive lookups
 * usually hit the resident page).  Cost: O(log pages) + ~1 page read per ~128
 * scored docids, with 0 up-front full decode.  A v3 (inline-doclen) segment has
 * no sidecar (start == Invalid); its cursor returns 0 and the caller reads the
 * inline posting doclen.
 */

/* Walk ONE segment's sidecar chain reading only page headers, appending a
 * (first_docid, blk) entry per page into docid[]/blk[] starting at *pos.
 * Returns the entry count for this segment. */
static int
weave_doclendir_scan_seg(Relation index, BlockNumber start,
						uint64 *docid, BlockNumber *blk, int cap, int *pos)
{
	BlockNumber b = start;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint32		visited = 0;
	int			n = 0;

	while (b != InvalidBlockNumber && *pos < cap)
	{
		Buffer		buf;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();
		if (b >= nblocks || visited++ > nblocks)
			break;
		buf = ReadBuffer(index, b);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_DOCLEN))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		ptr = (char *) page + MAXALIGN(SizeOfPageHeaderData);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		/* the page's first docid = its first block's first_docid */
		if (ptr + sizeof(WeaveDoclenBlockHdr) <= end)
		{
			WeaveDoclenBlockHdr *bh = (WeaveDoclenBlockHdr *) ptr;
			uint32		bcount = WEAVE_DOCLEN_COUNT(bh->count);

			/* strip WEAVE_DOCLEN_ABS: `count` is not a bare count.  Validating
			 * the raw field here silently skipped every v5 page, leaving an EMPTY
			 * page directory -- and an empty directory makes the cursor return 0
			 * for every docid, i.e. BM25 scores with the wrong document lengths
			 * rather than an error. */
			if (bcount > 0 && bcount <= WEAVE_BLOCK_SIZE)
			{
				docid[*pos] = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
				blk[*pos] = b;
				(*pos)++;
				n++;
			}
		}
		UnlockReleaseBuffer(buf);
		b = next;
	}
	return n;
}

/* Count sidecar pages in a segment (header-only walk), to size the cache. */
static int
weave_doclendir_count_seg(Relation index, BlockNumber start)
{
	BlockNumber b = start;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	uint32		visited = 0;
	int			n = 0;

	while (b != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();
		if (b >= nblocks || visited++ > nblocks)
			break;
		buf = ReadBuffer(index, b);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_DOCLEN))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		next = WeavePageGetOpaque(page)->nextblk;
		n++;
		UnlockReleaseBuffer(buf);
		b = next;
	}
	return n;
}

/*
 * Get the relcache page-directory cache, (re)building it as ONE chunk in
 * CacheMemoryContext if absent or stale (generation moved).  `meta` supplies
 * the live segment doclenstarts + generation.  Returns NULL if no v4 segment
 * has a sidecar (nothing to cache).
 */
WeaveDoclenDirCache *
weave_doclendir_cache(Relation index, const WeaveMetaPageData *meta)
{
	WeaveDoclenDirCache *dc = (WeaveDoclenDirCache *) index->rd_amcache;
	int			total = 0;
	uint32		s;
	int			pos = 0;
	Size		sz;
	MemoryContext old;

	if (dc != NULL && dc->generation == meta->generation)
		return dc;

	/* stale or absent: drop the old single chunk, rebuild */
	if (dc != NULL)
	{
		pfree(dc);
		index->rd_amcache = NULL;
		dc = NULL;
	}

	/* size: total sidecar pages across all v4 segments (header-only walk) */
	for (s = 0; s < meta->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
		if (meta->segs[s].doclenstart != InvalidBlockNumber)
			total += weave_doclendir_count_seg(index, meta->segs[s].doclenstart);
	if (total == 0)
		return NULL;			/* no v4 sidecar segment */

	/* one contiguous allocation: header + uint64 docid[total] + BlockNumber
	 * blk[total] (blk stored in the uint64 tail region, 2 BlockNumbers per
	 * uint64 slot would misalign -- keep it simple: allocate docid[] as uint64
	 * and blk[] as uint64-sized slots too, wasting 4B/entry but trivially small
	 * and single-chunk).  data[] holds total uint64 docids then total uint64
	 * slots each carrying one BlockNumber. */
	sz = offsetof(WeaveDoclenDirCache, data) +
		(Size) total *sizeof(uint64) +		/* docid[] */
		(Size) total *sizeof(uint64);		/* blk[] (one BlockNumber per uint64 slot) */
	old = MemoryContextSwitchTo(CacheMemoryContext);
	dc = (WeaveDoclenDirCache *) palloc0(sz);
	MemoryContextSwitchTo(old);
	dc->generation = meta->generation;
	dc->ndocid = total;
	dc->nsegs = 0;

	{
		uint64	   *docid = dc->data;
		BlockNumber *blk = (BlockNumber *) (dc->data + total);	/* NB: BlockNumber slots */

		for (s = 0; s < meta->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
		{
			int			startpos = pos;
			int			n;

			if (meta->segs[s].doclenstart == InvalidBlockNumber)
				continue;
			n = weave_doclendir_scan_seg(index, meta->segs[s].doclenstart,
										docid, blk, total, &pos);
			dc->segs[dc->nsegs].start = meta->segs[s].doclenstart;
			dc->segs[dc->nsegs].n = n;
			dc->segs[dc->nsegs].docid_off = startpos;
			dc->segs[dc->nsegs].blk_off = startpos;
			dc->nsegs++;
		}
	}
	index->rd_amcache = (void *) dc;
	return dc;
}

void
weave_doclen_cursor_init(WeaveDoclenCursor *c, Relation index, BlockNumber start,
						WeaveDoclenDirCache *dc, WeaveDoclenResident *res)
{
	c->index = index;
	c->start = start;
	c->dir_docid = NULL;
	c->dir_blk = NULL;
	c->dir_n = 0;
	c->dir_hint = 0;
	c->res = NULL;

	if (start == InvalidBlockNumber || dc == NULL)
		return;					/* v3 segment (inline doclen) or no cache */

	{
		int			i;

		for (i = 0; i < dc->nsegs; i++)
			if (dc->segs[i].start == start)
			{
				c->dir_docid = WEAVE_DOCLENDIR_DOCIDS(dc) + dc->segs[i].docid_off;
				c->dir_blk = WEAVE_DOCLENDIR_BLKS(dc) + dc->segs[i].blk_off;
				c->dir_n = dc->segs[i].n;
				break;
			}
	}
	if (c->dir_n > 0 && res != NULL)
	{
		/* Attach the SHARED resident page/block for this segment.  `page` is
		 * sized to one whole sidecar page (WeaveDoclenResident's comment
		 * explains why a whole page, not one block); `docid`/`byte` remain
		 * one-block-sized because a v4 block is decoded into them, not
		 * addressed in place. */
		if (res->page == NULL)
		{
			res->cap = WEAVE_BLOCK_SIZE;
			res->docid = (uint64 *) palloc(res->cap * sizeof(uint64));
			res->byte = (uint8 *) palloc(res->cap * sizeof(uint8));
			res->pagecap = BLCKSZ;
			res->page = (unsigned char *) palloc(res->pagecap);
			res->pageblk = InvalidBlockNumber;
			res->pagefirst = 0;
			res->pagelast = 0;
			res->raw = NULL;
			res->rawbyte = NULL;
			res->isabs = false;
			res->base = 0;
			res->n = 0;
			res->first = 0;
			res->last = 0;
			res->hint = 0;
		}
		c->res = res;
	}
}

void
weave_doclen_cursor_free(WeaveDoclenCursor *c)
{
	/* the directory arrays are borrowed from the relcache cache and the resident
	 * block from the scan's per-segment slot -- nothing here is cursor-owned */
	c->dir_docid = NULL;
	c->dir_blk = NULL;
	c->dir_n = 0;
	c->res = NULL;
}

/* Header-only walk of the resident page copy `r->page`, from `start` to the
 * page's end, finding the LAST block whose first_docid <= docid.  Used both
 * right after a page copy (start = the page's first block) and by the
 * same-page relocation fast path.  Never touches the buffer manager: `r->page`
 * is already a private copy. */
static inline char *
weave_doclen_resident_find_block(char *start, char *end, uint64 docid)
{
	char	   *ptr = start;
	char	   *cand = NULL;

	while (ptr + sizeof(WeaveDoclenBlockHdr) <= end)
	{
		WeaveDoclenBlockHdr *bh = (WeaveDoclenBlockHdr *) ptr;
		uint64		first;
		uint32		bcount = WEAVE_DOCLEN_COUNT(bh->count);
		char	   *blkend;

		if (bcount == 0 || bcount > WEAVE_BLOCK_SIZE)
			break;
		blkend = (char *) (bh + 1) + bh->gapbytes + bcount;
		if (blkend > end)
			break;
		first = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
		if (first <= docid)
			cand = ptr;			/* still a candidate; a later block may be closer */
		else
			break;				/* blocks are docid-ascending: no later block fits */
		ptr = (char *) MAXALIGN(blkend);
	}
	return cand;
}

/* Populate the block-level fields of `r` (isabs/raw/rawbyte/base/first/last/n)
 * from the block header at `bh`, an address INSIDE r->page.  `bytes` is the
 * on-disk length column that follows the docid column. */
static inline void
weave_doclen_resident_load_block(WeaveDoclenResident *r, WeaveDoclenBlockHdr *bh)
{
	uint32		bcount = WEAVE_DOCLEN_COUNT(bh->count);
	uint8	   *bytes = (uint8 *) ((char *) (bh + 1) + bh->gapbytes);

	r->hint = 0;				/* new block: the ascending-resume hint restarts */
	if (WEAVE_DOCLEN_IS_ABS(bh->count))
	{
		/*
		 * v5: point at the packed column + its length bytes IN PLACE inside
		 * the resident page copy -- no per-block copy at all.  first/last
		 * come from the column's endpoints, which are O(1) reads -- offs[0]
		 * is always 0, so `first` is the base.
		 */
		r->raw = (const unsigned char *) (bh + 1);
		r->rawbyte = (const uint8 *) (r->raw + bh->gapbytes);
		r->base = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
		r->n = (int) bcount;
		r->isabs = true;
		r->first = r->base;
		r->last = r->base + weave_for_get(r->raw, (int) bcount - 1);
	}
	else
	{
		/* v4: gap-coded, so the block must be unpacked and prefix-summed */
		uint64		gaps[WEAVE_BLOCK_SIZE];
		uint64		acc;
		int			j;

		r->isabs = false;
		r->raw = NULL;
		r->rawbyte = NULL;
		r->n = 0;
		weave_for_unpack((unsigned char *) (bh + 1), (int) bcount, gaps);
		acc = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
		for (j = 0; j < (int) bcount && r->n < r->cap; j++)
		{
			acc += gaps[j];		/* gaps[0] == 0 */
			r->docid[r->n] = acc;
			r->byte[r->n] = bytes[j];
			r->n++;
		}
		if (r->n > 0)
		{
			r->first = r->docid[0];
			r->last = r->docid[r->n - 1];
		}
	}
}

/* Relocate the resident BLOCK to the one covering `docid` within the resident
 * PAGE copy `r->page`, WITHOUT touching the buffer manager.  Callable whenever
 * `docid` is known to fall within [r->pagefirst, r->pagelast]: right after
 * weave_doclen_cursor_load_page() copies a new page, and from the cursor's
 * same-page-different-block fast path. */
static void
weave_doclen_cursor_relocate(WeaveDoclenResident *r, uint64 docid)
{
	char	   *end = (char *) r->page + ((PageHeader) r->page)->pd_lower;
	char	   *start = (char *) r->page + MAXALIGN(SizeOfPageHeaderData);
	char	   *cand = weave_doclen_resident_find_block(start, end, docid);

	r->n = 0;
	r->first = 0;
	r->last = 0;
	r->hint = 0;
	r->isabs = false;
	r->raw = NULL;
	r->rawbyte = NULL;
	r->base = 0;
	if (cand != NULL)
		weave_doclen_resident_load_block(r, (WeaveDoclenBlockHdr *) cand);
}

/* Copy the sidecar PAGE `blkno` into the cursor's resident page cache and
 * relocate to the block covering `docid`.
 *
 * Earlier versions of this function copied only the ONE covering block, not
 * the whole page: a sidecar page holds ~31 128-doc blocks (~4000 docs), and
 * for v4 (gap-coded, must be FOR-unpacked + prefix-summed to be usable)
 * copying-and-decoding all ~31 to serve one lookup was ~31x amplification --
 * tried and reverted; see the comment on WeaveDoclenResident.  v5's docid
 * column is fixed-width addressable and is NEVER decoded, so the objection
 * does not apply: copying the whole page costs one 8 KB memcpy, and every
 * block on that page becomes reachable by weave_doclen_cursor_relocate()
 * without another trip through the buffer manager.  Measured: buffer hits for
 * one ranked mid k=10 query fell from ~15,000 to ~584
 * (bench/RESULTS_L17.md's follow-up 2).
 *
 * `r->pagefirst`/`r->pagelast` record the resident PAGE's docid range (not
 * just the resident block's) so the caller's fast path can relocate within
 * the page instead of re-reading it. */
static void
weave_doclen_cursor_load_page(WeaveDoclenCursor *c, BlockNumber blkno, uint64 docid)
{
	WeaveDoclenResident *r = c->res;
	Buffer		buf;
	Page		page;
	char	   *ptr,
			   *end;
	char	   *lastblk;

	if (r == NULL)
		return;
	r->n = 0;
	r->first = 0;
	r->last = 0;
	r->hint = 0;
	r->isabs = false;
	r->raw = NULL;
	r->rawbyte = NULL;
	r->base = 0;
	r->pageblk = InvalidBlockNumber;
	r->pagefirst = 0;
	r->pagelast = 0;
	if (blkno == InvalidBlockNumber || r->page == NULL ||
		blkno >= RelationGetNumberOfBlocks(c->index))
		return;
	buf = ReadBuffer(c->index, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_DOCLEN))
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	memcpy(r->page, page, BLCKSZ);
	UnlockReleaseBuffer(buf);		/* r->page is a private copy from here on */
	r->pageblk = blkno;

	end = (char *) r->page + ((PageHeader) r->page)->pd_lower;
	ptr = (char *) r->page + MAXALIGN(SizeOfPageHeaderData);

	/* header-only walk to find the page's first and last docid, and the last
	 * block header (reused below to compute pagelast) -- ~31 header hops,
	 * done once per PAGE change rather than once per lookup */
	lastblk = weave_doclen_resident_find_block(ptr, end, PG_UINT64_MAX);
	if (ptr + sizeof(WeaveDoclenBlockHdr) <= end)
	{
		WeaveDoclenBlockHdr *bh0 = (WeaveDoclenBlockHdr *) ptr;
		uint32		bcount0 = WEAVE_DOCLEN_COUNT(bh0->count);

		/* Same validation weave_doclen_resident_find_block() applies to every
		 * block it walks (and that the page-directory builder applies above):
		 * `count` carries the WEAVE_DOCLEN_ABS flag, so the raw field is not a
		 * bare count, and an out-of-range value means a corrupt or foreign
		 * page.  pagelast already gets this check for free by going through
		 * weave_doclen_resident_find_block(); pagefirst read the header
		 * directly and skipped it -- doc/CONVENTIONS.md's "every decoder
		 * validates" applies here too, even though no crash or wrong nonzero
		 * doclen was observed from the gap. */
		if (bcount0 > 0 && bcount0 <= WEAVE_BLOCK_SIZE)
			r->pagefirst = ((uint64) bh0->first_docid_hi << 32) | bh0->first_docid_lo;
	}
	if (lastblk != NULL)
	{
		WeaveDoclenBlockHdr *bh = (WeaveDoclenBlockHdr *) lastblk;
		uint32		bcount = WEAVE_DOCLEN_COUNT(bh->count);
		uint64		first = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;

		if (WEAVE_DOCLEN_IS_ABS(bh->count))
			r->pagelast = first + weave_for_get((const unsigned char *) (bh + 1),
												 (int) bcount - 1);
		else
		{
			/* v4: the last block's last docid needs one unpack */
			uint64		gaps[WEAVE_BLOCK_SIZE];
			uint64		acc = first;
			int			j;

			weave_for_unpack((unsigned char *) (bh + 1), (int) bcount, gaps);
			for (j = 0; j < (int) bcount; j++)
				acc += gaps[j];
			r->pagelast = acc;
		}
	}

	weave_doclen_cursor_relocate(r, docid);
}

/*
 * The in-block gated walk-then-bisect (weave_doclen_walk_abs/_arr, called
 * below) and its WEAVE_DOCLEN_WALK_WINDOW gate live in include/weave/for.h,
 * not here -- pulled out to backend-independent code, alongside weave_for_get
 * and weave_byte_to_doclen, specifically so test/hegel/test_doclen_block.c can
 * link and property-test the SAME code this function calls instead of a hand
 * transcription of it.  See that header's comment on the two functions for
 * the full rationale and bench/RESULTS_L17.md's follow-up 1 for the
 * measurement that motivated the gate.
 */

/* Exact doclen for docid via the page-directory cursor.  Robust to ANY docid
 * order; the ascending-resume hint makes the common monotone WAND scan land on
 * the resident page.  Returns 0 if absent (v3 cursor, or docid not present). */
inline uint32
weave_doclen_cursor_lookup(WeaveDoclenCursor *c, uint64 docid)
{
	WeaveDoclenResident *r = c->res;
	int			lo,
				hi,
				pg;

	if (c->dir_n == 0 || c->dir_docid == NULL || r == NULL)
		return 0;

	/* Fast path 1: docid is inside the resident BLOCK's range.  This now also
	 * hits when a DIFFERENT term's cursor of the same segment already decoded
	 * the block for this pivot docid (the multi-term win). */
	if (r->n > 0 && docid >= r->first && docid <= r->last)
	{
		/* fall through to the in-block search below */
	}
	/* Fast path 2: docid is outside the resident BLOCK but still inside the
	 * resident PAGE's range.  weave_doclen_cursor_load_page() copies the
	 * WHOLE page (see its comment and WeaveDoclenResident's), so every other
	 * block on that page is already in local memory -- relocate to it with a
	 * header re-walk instead of a fresh ReadBuffer. */
	else if (r->pageblk != InvalidBlockNumber &&
			 docid >= r->pagefirst && docid <= r->pagelast)
	{
		weave_doclen_cursor_relocate(r, docid);
	}
	else
	{
		/* binary-search the directory for the page whose first_docid <= docid
		 * (the largest such), with an ascending-resume hint */
		if (c->dir_hint < c->dir_n && c->dir_docid[c->dir_hint] <= docid)
			lo = c->dir_hint;
		else
			lo = 0;
		hi = c->dir_n - 1;
		pg = -1;
		while (lo <= hi)
		{
			int			mid = (lo + hi) >> 1;

			if (c->dir_docid[mid] <= docid)
			{
				pg = mid;
				lo = mid + 1;
			}
			else
				hi = mid - 1;
		}
		if (pg < 0)
			return 0;			/* docid precedes the first page's first docid */
		c->dir_hint = pg;
		weave_doclen_cursor_load_page(c, c->dir_blk[pg], docid);
	}

	/*
	 * Locate docid within the resident block.  The WAND scan probes docids in
	 * strictly ASCENDING order, so the answer is usually at or just after the
	 * previous hit: try a short linear walk from the resume hint first and only
	 * fall back to a binary search when that misses (a seek, or a new block).
	 * Profiling showed the unconditional binary search here was ~45% of the
	 * common-term ranked query -- 7 branchy iterations per posting, on a term
	 * whose docids are consecutive.
	 *
	 * The walk itself is gated by WEAVE_DOCLEN_WALK_WINDOW (see its comment):
	 * one O(1) read at the hint decides whether the walk can plausibly reach
	 * the target, so a term whose stride makes the walk hopeless costs 1
	 * wasted read instead of 8 before falling back to the bisect.
	 *
	 * v5 searches the block's PACKED column in place via weave_for_get (O(1) per
	 * probe, fixed-width offsets from r->base), so no block decode happened at
	 * all.  v4 searches the arrays that load_page had to materialize.  Both use
	 * the same walk-then-bisect shape; only the accessor differs.
	 */
	if (r->isabs)
	{
		int			byte = weave_doclen_walk_abs(r->raw, r->rawbyte, r->base,
												 r->n, docid, &r->hint);

		if (byte >= 0)
			return weave_byte_to_doclen((uint8) byte);
	}
	else
	{
		int			byte = weave_doclen_walk_arr(r->docid, r->byte, r->n,
												 docid, &r->hint);

		if (byte >= 0)
			return weave_byte_to_doclen((uint8) byte);
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * WEAVE_CHANDESC: the per-bolt weft descriptor page (v6)
 *
 * One page per bolt, holding a WeaveChanDescPageData header plus a
 * (kind, attnum)-ascending array of WeaveChannelDesc.  It answers "which wefts
 * does this bolt carry, and where does each start" from the bolt itself, so an
 * index built without a vector column stores no vector structures at all and a
 * reader never derives a weft's geometry from a GUC that may have changed since
 * the build (doc/specs/SEGMENT_FORMAT.md sect. 6).
 *
 * WHY THE ROOTS ARE STORED, NOT COMPUTED.  SEGMENT_FORMAT.md sect. 8 item 5
 * records that a sibling project has now four times shipped a bug where a
 * running chain-offset sum omitted one count field and one chain was written
 * over another's data.  The structural defence taken here is that a weft's root
 * is an EXPLICIT BlockNumber written by the code that allocated the chain -- there
 * is no running sum to omit a term from.  The residual risk, two descriptors
 * naming the same root, is checked by weave_chandesc_check() on every read and
 * again by weave_check() over the whole relation.
 * ------------------------------------------------------------------------- */

static BlockNumber
weave_write_chandesc(Relation index, const WeaveChannelDesc *weft, int nweft)
{
	Buffer		buffer;
	GenericXLogState *state;
	Page		page;
	WeaveChanDescPageData *cd;
	BlockNumber blk;

	Assert(nweft >= 1 && nweft <= WEAVE_MAX_WEFTS);

	buffer = weave_new_buffer(index);
	blk = BufferGetBlockNumber(buffer);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
	weave_init_page(page, WEAVE_PK_CHANDESC);

	cd = WeavePageGetChanDesc(page);
	MemSet(cd, 0, offsetof(WeaveChanDescPageData, weft));
	cd->magic = WEAVE_CHANDESC_MAGIC;
	cd->version = WEAVE_CHANDESC_VERSION;
	cd->nweft = (uint16) nweft;
	cd->reserved = 0;
	memcpy(cd->weft, weft, (Size) nweft * sizeof(WeaveChannelDesc));
	((PageHeader) page)->pd_lower =
		((char *) cd->weft + (Size) nweft * sizeof(WeaveChannelDesc)) - (char *) page;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(buffer);
	return blk;
}

/*
 * Read and VALIDATE a bolt's descriptor page into out[max].  Returns WEAVE_CD_OK
 * and sets *nweft_out on success; on any failure returns the specific error and
 * sets *nweft_out to 0.
 *
 * NEVER THROWS, on purpose.  The read path must refuse to use a corrupt
 * descriptor page -- weave_chandesc_required() in weave/am.h turns any error into
 * an ERROR with the errdetail, which is the "a corrupt page produces a clean
 * ERROR" half of doc/CONVENTIONS.md decision 2 -- but weave_check() must REPORT a
 * corrupt page as a violated invariant and carry on to the remaining invariants,
 * and doing that with PG_CATCH around a throwing reader means catching an error
 * without a subtransaction.  Returning a code is the better trade.
 *
 * The validation body itself is weave_chandesc_check() in weave/chandesc.h, which
 * is backend-independent so test/fuzz/fuzz_chandesc.c and
 * test/hegel/test_chandesc.c hammer the exact code this path runs.
 */
WeaveCdError
weave_read_chandesc(Relation index, BlockNumber blk,
					WeaveChannelDesc *out, int max, int *nweft_out)
{
	Buffer		buffer;
	Page		page;
	WeaveCdError err;
	uint16		nweft = 0;
	BlockNumber nblocks;
	Size		avail;
	int			n;

	/* Layout contract with the pure validator.  These are the guard; the comment
	 * in weave/chandesc.h is not. */
	StaticAssertStmt(sizeof(WeaveCdDesc) == sizeof(WeaveChannelDesc),
					 "WeaveCdDesc and WeaveChannelDesc layouts diverged");
	StaticAssertStmt(sizeof(WeaveCdPage) == offsetof(WeaveChanDescPageData, weft),
					 "WeaveCdPage and WeaveChanDescPageData headers diverged");
	StaticAssertStmt(offsetof(WeaveCdDesc, root) == offsetof(WeaveChannelDesc, root),
					 "WeaveCdDesc.root offset diverged");
	StaticAssertStmt(WEAVE_CD_INVALID_BLK == InvalidBlockNumber,
					 "WEAVE_CD_INVALID_BLK != InvalidBlockNumber");

	if (nweft_out != NULL)
		*nweft_out = 0;

	nblocks = RelationGetNumberOfBlocks(index);
	if (blk == InvalidBlockNumber || blk == WEAVE_METAPAGE_BLKNO || blk >= nblocks)
		return WEAVE_CD_BLKRANGE;

	buffer = ReadBuffer(index, blk);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);

	if (PageIsNew(page))
	{
		UnlockReleaseBuffer(buffer);
		return WEAVE_CD_PAGENEW;
	}
	if (!WeavePageHasKind(page, WEAVE_PK_CHANDESC))
	{
		UnlockReleaseBuffer(buffer);
		return WEAVE_CD_PAGEKIND;
	}

	/* pd_lower bounds what the writer actually wrote; never read past it, and
	 * never trust it to be sane either (a torn header can make it small). */
	avail = 0;
	if (((PageHeader) page)->pd_lower >=
		(char *) PageGetContents(page) - (char *) page)
		avail = ((PageHeader) page)->pd_lower -
			((char *) PageGetContents(page) - (char *) page);

	err = weave_chandesc_check(PageGetContents(page), avail, nblocks, &nweft);
	if (err != WEAVE_CD_OK)
	{
		UnlockReleaseBuffer(buffer);
		return err;
	}

	n = Min((int) nweft, max);
	if (n > 0)
		memcpy(out, WeavePageGetChanDesc(page)->weft,
			   (Size) n * sizeof(WeaveChannelDesc));
	UnlockReleaseBuffer(buffer);
	if (nweft_out != NULL)
		*nweft_out = (int) nweft;
	return WEAVE_CD_OK;
}

/*
 * The descriptor array a freshly written bolt gets.
 *
 * Every v6 bolt gets a descriptor page, even one that carries nothing but the
 * lexical weft.  The alternative -- write the page only once a second channel
 * exists -- would leave the writer, the WAL path, the free path, the validator
 * and weave_check()'s reachability rule completely unexercised until the vector
 * channel lands, which is the failure mode blocking gate 5 exists to prevent.
 * The measured cost is one page per bolt (see doc/PHASES.md X2), and it buys one
 * thing v4 never recorded: WHICH index attribute the lexical weft indexes.
 *
 * Since v7 a second weft can appear here: the fuzzy one.  Note what did NOT
 * happen -- WeaveSegMeta gained no field.  The whole point of the v6 descriptor
 * page is that a weft's root is recorded in the bolt's self-description, so a
 * new weft costs zero bytes in the metapage and zero bytes in a bolt that does
 * not carry it (doc/specs/SEGMENT_FORMAT.md sect. 6).
 */
static int
weave_chandesc_for_segment(Relation index, const WeaveSegMeta *seg,
						   BlockNumber surfroot, WeaveChannelDesc *weft)
{
	int			n = 0;

	Assert(seg->dictstart != InvalidBlockNumber);
	weft[n].kind = (uint16) WEAVE_WK_LEXICAL;
	weft[n].attnum = 1;			/* the lexical weft is over attribute 1 today;
								 * multi-attribute wefts are what attnum is for */
	weft[n].flags = 0;
	weft[n].root = seg->dictstart;
	n++;

	/*
	 * v7: the fuzzy weft, i.e. the SuRF trie over this bolt's vocabulary.  It is
	 * absent -- and then costs literally zero bytes, including this descriptor
	 * slot -- when the bolt has no vocabulary at all, or when the vocabulary is
	 * one the format cannot represent completely (see weave_build_surf_weft() in
	 * ambuild.c: an INCOMPLETE trie would be a false negative, and no descriptor
	 * is the only safe way to say "there is nothing here to consult").
	 *
	 * Emitted after LEXICAL because weave_chandesc_check() requires the array to
	 * be strictly ascending by (kind, attnum) and WEAVE_WK_FUZZY (3) is above
	 * WEAVE_WK_LEXICAL (1).  Adding a weft with a kind BELOW an existing one
	 * means sorting here, not appending.
	 */
	if (surfroot != InvalidBlockNumber)
	{
		weft[n].kind = (uint16) WEAVE_WK_FUZZY;
		weft[n].attnum = 1;
		weft[n].flags = 0;
		weft[n].root = surfroot;
		n++;
	}
	return n;
}

/* Attach a descriptor page to a just-written bolt.  Call AFTER every other
 * chain of the bolt has been written, so each root is known. */
void
weave_attach_chandesc(Relation index, WeaveSegMeta *seg, BlockNumber surfroot)
{
	WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
	int			nweft = weave_chandesc_for_segment(index, seg, surfroot, weft);

	seg->chandesc = weave_write_chandesc(index, weft, nweft);
}

/* ---------------------------------------------------------------------------
 * The fuzzy weft's page chain (task Z3)
 *
 * See the block comment on these three in include/weave/am.h for why this is not
 * weave_write_blob(), and why no length is stored anywhere.
 * ------------------------------------------------------------------------- */

/*
 * Lay `len` bytes across a fresh chain of WEAVE_PK_SURF pages, one page per
 * GenericXLog cycle so there is no page-count limit and no oversized WAL record.
 * Returns the first block.
 *
 * 100% GenericXLog (AGENTS.md hard rule 2): every page is registered with
 * GENERIC_XLOG_FULL_IMAGE because every page is brand new, so a delta against
 * the pre-image would be the whole page anyway.  t/012_surf_crash_recovery.pl is
 * the proof that the chain survives an immediate shutdown.
 */
BlockNumber
weave_write_surf(Relation index, const uint8 *img, Size len)
{
	BlockNumber first = InvalidBlockNumber;
	Buffer		prevbuf = InvalidBuffer;
	Page		prevpage = NULL;
	GenericXLogState *prevstate = NULL;
	Size		off = 0;

	Assert(len > 0);

	do
	{
		Buffer		buf = weave_new_buffer(index);
		BlockNumber blk = BufferGetBlockNumber(buf);
		GenericXLogState *state = GenericXLogStart(index);
		Page		page = GenericXLogRegisterBuffer(state, buf,
													 GENERIC_XLOG_FULL_IMAGE);
		Size		chunk = Min(len - off, (Size) WEAVE_SURFPAGE_PAYLOAD);

		weave_init_page(page, WEAVE_PK_SURF);
		memcpy((char *) PageGetContents(page), img + off, chunk);
		((PageHeader) page)->pd_lower =
			((char *) PageGetContents(page) - (char *) page) + chunk;

		if (prevbuf != InvalidBuffer)
		{
			WeavePageGetOpaque(prevpage)->nextblk = blk;
			GenericXLogFinish(prevstate);
			UnlockReleaseBuffer(prevbuf);
		}
		else
			first = blk;

		prevbuf = buf;
		prevpage = page;
		prevstate = state;
		off += chunk;
	} while (off < len);

	GenericXLogFinish(prevstate);
	UnlockReleaseBuffer(prevbuf);
	return first;
}

/*
 * One pass over the chain from `root`.  With `dst` NULL it only measures and
 * validates the pages; with `dst` set it copies at most `cap` bytes into it.
 * Returns the payload byte count, or -1 with *detail set.
 *
 * ONE walker, TWO modes, for the same reason weave_surftrie_build() has one:
 * the measure pass sizes the buffer the copy pass fills, and a disagreement
 * between two separately-written walkers is a buffer overrun.
 */
static int64
weave_surf_walk(Relation index, BlockNumber root, uint8 *dst, Size cap,
				const char **detail)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber blk = root;
	int64		total = 0;
	int64		npages = 0;

	*detail = NULL;
	if (blk == InvalidBlockNumber || blk == WEAVE_METAPAGE_BLKNO)
	{
		*detail = "fuzzy weft root block is invalid";
		return -1;
	}

	while (blk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		Size		avail;
		Size		contoff;

		CHECK_FOR_INTERRUPTS();	/* between pages, no buffer lock held */
		if (blk >= nblocks)
		{
			*detail = "surf trie chain leaves the relation";
			return -1;
		}
		if (++npages > (int64) nblocks)
		{
			*detail = "surf trie chain exceeds the relation length (cycle?)";
			return -1;
		}

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			*detail = "surf trie chain reaches an uninitialized page";
			return -1;
		}

		/*
		 * WeavePageHasKind(), never `flags & WEAVE_SURF_PAGE`: WEAVE_PK_SURF is
		 * an INTEGER id under the escape bit, so a bitwise AND compiles and is
		 * always false (weave/pagekind.h).  This is the L17 class of bug and it
		 * has already been made twice in this tree.
		 */
		if (!WeavePageHasKind(page, WEAVE_PK_SURF))
		{
			UnlockReleaseBuffer(buf);
			*detail = "a block on the surf trie chain is not a surf page";
			return -1;
		}

		contoff = (Size) ((char *) PageGetContents(page) - (char *) page);
		avail = 0;
		if ((Size) ((PageHeader) page)->pd_lower >= contoff)
			avail = (Size) ((PageHeader) page)->pd_lower - contoff;
		if (avail > (Size) WEAVE_SURFPAGE_PAYLOAD)
			avail = (Size) WEAVE_SURFPAGE_PAYLOAD;	/* a torn pd_lower cannot make us
												 * read into the opaque area */
		if (dst != NULL)
		{
			if ((Size) total + avail > cap)
			{
				UnlockReleaseBuffer(buf);
				*detail = "surf trie chain grew between the measure and copy passes";
				return -1;
			}
			memcpy(dst + total, PageGetContents(page), avail);
		}
		total += (int64) avail;
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
	}

	if (total == 0)
	{
		*detail = "surf trie chain carries no bytes";
		return -1;
	}
	return total;
}

uint8 *
weave_read_surf(Relation index, BlockNumber root, Size *len_out,
				const char **detail)
{
	int64		len;
	uint8	   *img;

	*len_out = 0;
	len = weave_surf_walk(index, root, NULL, 0, detail);
	if (len < 0)
		return NULL;

	/*
	 * Vocabulary-scale: a trie over the whole vocabulary is precisely the
	 * allocation class behind four real crashes in this extension's ancestor
	 * (AGENTS.md's lint table, `make check-alloc`).  The size here is bounded by
	 * pages that actually exist rather than by a count read out of the image, so
	 * a corrupt header cannot ask for 1.3 GB -- but the honest bound is still the
	 * relation, so this goes through the huge-safe path.
	 */
	img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE((Size) len);
	if (weave_surf_walk(index, root, img, (Size) len, detail) != len)
	{
		pfree(img);
		if (*detail == NULL)
			*detail = "surf trie chain length changed between passes";
		return NULL;
	}
	*len_out = (Size) len;
	return img;
}

bool
weave_surf_load(Relation index, const WeaveSegMeta *seg, WeaveSurfTrie *t,
				uint8 **img, Size *len)
{
	WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
	int			nweft;
	int			i;
	BlockNumber root = InvalidBlockNumber;
	const char *detail = NULL;
	WeaveSurfError err;

	*img = NULL;
	*len = 0;
	if (seg->chandesc == InvalidBlockNumber)
		return false;			/* pre-v6 bolt: lexical only, by definition */

	nweft = weave_chandesc_required(index, seg->chandesc, weft, WEAVE_MAX_WEFTS);
	for (i = 0; i < nweft; i++)
		if (weft[i].kind == (uint16) WEAVE_WK_FUZZY)
			root = weft[i].root;
	if (root == InvalidBlockNumber)
		return false;			/* v6 bolt, or a v7 bolt whose vocabulary the
								 * format cannot represent: nothing to consult */

	*img = weave_read_surf(index, root, len, &detail);
	if (*img == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupt surf trie page chain in index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("%s (block %u)", detail, root),
				 errhint("REINDEX the index to rebuild it.")));

	/*
	 * open() + validate(), never open() alone.  open() is the memory-safety
	 * layer; validate() is the one that catches a corrupt accelerator table,
	 * whose symptom is navigation to the WRONG NODE -- a false negative, which is
	 * a silently dropped row rather than an error (weave/surftrie.h).
	 */
	err = weave_surftrie_open(*img, *len, t);
	if (err == WEAVE_SURF_OK)
		err = weave_surftrie_validate(t);
	if (err != WEAVE_SURF_OK)
	{
		pfree(*img);
		*img = NULL;
		*len = 0;
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupt surf trie image in index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("%s (chain at block %u)",
						   weave_surftrie_errstr(err), root),
				 errhint("REINDEX the index to rebuild it.")));
	}
	return true;
}

/*
 * Append a segment descriptor to the metapage directory and fold its doc stats
 * into the corpus totals.  Returns true on success, false if the fixed-size
 * directory is already full (WEAVE_MAX_SEGMENTS).  The caller must react to a
 * false return by merging to free a slot and retrying -- see
 * weave_add_segment_with_room().  A full directory must NEVER become a failed
 * write: this is an index access method, and refusing an INSERT because merging
 * fell behind under load is an outage, not an acceptable limit.  (A field
 * deployment had to disable the index when live ingestion outran merging and
 * hit the old hard error here.)
 */
bool
weave_meta_add_segment(Relation index, const WeaveSegMeta *seg)
{
	Buffer		buf = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	GenericXLogState *state;
	Page		page;
	WeaveMetaPageData *m;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);
	weave_meta_upcast_page(page);	/* v3 -> v4 in-place before any struct write */
	m = WeavePageGetMeta(page);
	if (m->nsegments >= WEAVE_MAX_SEGMENTS)
	{
		GenericXLogAbort(state);
		UnlockReleaseBuffer(buf);
		return false;			/* directory full: caller merges + retries */
	}
	m->segs[m->nsegments] = *seg;
	m->nsegments++;
	m->generation++;			/* directory changed: invalidate concurrent scan snapshots */
	m->ndocs += seg->ndocs;
	m->sumdoclen += seg->sumdoclen;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	return true;
}

/*
 * Add a segment, guaranteeing the write cannot fail because the directory is
 * full.  If weave_meta_add_segment reports no room, merge to free slots and
 * retry.  Merging k>=2 segments into one strictly reduces the count, and a full
 * directory always has >=2 mergeable segments, so a bounded number of merge
 * passes always makes room.  We escalate: the cheap bounded-fan-in
 * weave_merge_segments first, then the more aggressive collapse if a concurrent
 * flurry of flushes keeps the directory full.  This runs OUTSIDE the metapage
 * lock (merging takes that lock itself), so concurrent inserters serialize
 * naturally on the actual add.
 */
void
weave_add_segment_with_room(Relation index, const WeaveSegMeta *seg)
{
	int			try;

	if (weave_meta_add_segment(index, seg))
		return;

	for (try = 0; try < WEAVE_MAX_SEGMENTS; try++)
	{
		/*
		 * Bounded-fan-in leveled merge first (cheapest); if that did not free a
		 * slot in time (a concurrent flush refilled it, or every level was at
		 * capacity so the leveled selector picked a small batch), fall back to
		 * the smallest-first collapse, which always reduces the count while any
		 * two segments remain.
		 */
		if ((try & 1) == 0)
			weave_merge_segments(index);
		else
			weave_merge_all(index, false);
		if (weave_meta_add_segment(index, seg))
			return;
	}

	/*
	 * Unreachable in practice: a full directory always has >=2 segments to
	 * merge, and each successful merge frees a slot, so one of the retries above
	 * makes room unless another backend is adding segments faster than this one
	 * can merge for WEAVE_MAX_SEGMENTS passes.  If we somehow get here the data
	 * is intact (this segment simply is not yet in the directory); surface a
	 * clear error rather than silently drop it.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("weave index \"%s\": could not free a segment-directory slot after %d merge passes",
					RelationGetRelationName(index), WEAVE_MAX_SEGMENTS),
			 errhint("Reduce write concurrency briefly or run weave_merge(), then retry.")));
}

/*
 * Recycle gate (format-preserving deletion-xid stamp).
 *
 * pg_weave frees a segment's pages to the FSM as soon as a merge/vacuum commits
 * the new directory.  But a concurrent scan (AccessShareLock does NOT conflict
 * with merge/vacuum's ShareUpdateExclusiveLock) may still be walking those
 * pages from a directory snapshot it took before the commit.  If the allocator
 * hands a just-freed page back to a concurrent inserter that overwrites it, the
 * scan reads garbage -> wrong result / "invalid memory alloc" / SIGSEGV (a
 * field-reported crash under concurrent read+insert+merge).
 *
 * Fix, mirroring nbtree's btpo.xact recycle gate: when a page is freed, stamp
 * it with the current next-XID and mark it WEAVE_FREED, then hand it to the FSM.
 * Before REUSING a free page, require that stamp to be "old enough" that no
 * snapshot which could still reference it remains (GlobalVisCheckRemovableXid);
 * otherwise skip the page and leave it in the FSM for later.  The XID lives in
 * the freed page's nextblk field (dead once the page is off every chain), so
 * the on-disk page layout is unchanged and existing indexes need no REINDEX; a
 * page freed by an older build lacks WEAVE_FREED and is recyclable at once.
 */
void
weave_free_page(Relation index, BlockNumber blk)
{
	Buffer		buf = ReadBuffer(index, blk);
	GenericXLogState *state;
	Page		page;
	WeavePageOpaque op;

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buf, 0);
	op = WeavePageGetOpaque(page);
	op->flags |= WEAVE_FREED;
	/* reuse nextblk as the free-time XID horizon (page is now off all chains) */
	op->nextblk = (BlockNumber) ReadNextTransactionId();
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	RecordFreeIndexPage(index, blk);
}

/*
 * May a page fetched from the free list be reused now?  True if it was not
 * gated by this mechanism (old-format free page, or a brand-new page), or if
 * its free-XID stamp is old enough that no in-progress scan can still hold a
 * directory snapshot referencing it.  `page` must be pinned + locked.
 */
static bool
weave_page_recyclable(Relation index, Page page)
{
	WeavePageOpaque op;

	if (PageIsNew(page))
		return true;
	/*
	 * The recycle gate protects a CONCURRENT scan from reading a page we free
	 * and hand back to the allocator (the scan holds only AccessShareLock, which
	 * does not conflict with a merge/vacuum's ShareUpdateExclusiveLock).  It is
	 * safe to bypass ONLY when no concurrent scan can exist -- i.e. we hold
	 * AccessExclusiveLock on the index (CIC finalize, or weave_vacuum which now
	 * takes AccessExclusiveLock).  Under ShareUpdateExclusiveLock (autovacuum
	 * cleanup, plain VACUUM) a scan CAN be running, so the gate must stand even
	 * during compaction -- bypassing it there let weave_vacuum recycle a segment's
	 * pages while a concurrent reader was still copying them (e.g. a livedocs
	 * blob), corrupting the read and crashing (a rare SIGSEGV under heavy
	 * read+insert+merge+vacuum churn).  The weave_lowfree/extend-only compaction
	 * state alone is NOT sufficient license to bypass; the LOCK is.
	 */
	if ((weave_lowfree != NULL || weave_alloc_extend_only) &&
		CheckRelationLockedByMe(index, AccessExclusiveLock, true))
		return true;
	op = WeavePageGetOpaque(page);
	if ((op->flags & WEAVE_FREED) == 0)
		return true;			/* not gated (older free, or in-use race) */
	/*
	 * Is the freeing xid old enough that no snapshot can still reference this
	 * page?  Use the GLOBAL visibility horizon (NULL relation): the per-relation
	 * form wants the HEAP (an index has no xid horizon -- passing the index trips
	 * GlobalVisHorizonKindForRel's relkind assert under --enable-cassert, and is
	 * a latent API misuse in a non-assert build).  The global horizon is a sound
	 * upper bound -- it may keep a page unrecyclable slightly longer than a
	 * heap-scoped horizon would, never shorter -- so it is always safe here.
	 */
	return GlobalVisCheckRemovableXid(NULL, (TransactionId) op->nextblk);
}

/* Recycle a chained page list (dict/trigram/posting/data) to the FSM. */
void
weave_free_chain(Relation index, BlockNumber blk)
{
	while (blk != InvalidBlockNumber)
	{
		Buffer		buf = ReadBuffer(index, blk);
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		next = WeavePageGetOpaque(BufferGetPage(buf))->nextblk;
		UnlockReleaseBuffer(buf);
		weave_free_page(index, blk);
		blk = next;
	}
}

/* Free all pages of a segment (dict + each term's postings + trigram dir+data). */
void
weave_free_segment(Relation index, const WeaveSegMeta *seg)
{
	BlockNumber blk = seg->dictstart;
	BlockNumber postchain = InvalidBlockNumber;

	/* dictionary pages; capture the shared posting chain's first block */
	while (blk != InvalidBlockNumber)
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			/* all terms share ONE posting chain; the first term names its head */
			if (postchain == InvalidBlockNumber)
				postchain = de->firstposting;
			ptr += esize;
		}
		UnlockReleaseBuffer(buf);
		weave_free_page(index, blk);
		blk = next;
	}
	if (postchain != InvalidBlockNumber)
		weave_free_chain(index, postchain);	/* free the shared posting chain once */

	/* trigram directory pages (+ their data blobs) */
	blk = seg->trgmstart;
	while (blk != InvalidBlockNumber)
	{
		Buffer		buf = ReadBuffer(index, blk);
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveTrgmEntry *te = (WeaveTrgmEntry *) ptr;

			weave_free_chain(index, te->firstdata);
			ptr += MAXALIGN(sizeof(WeaveTrgmEntry));
		}
		UnlockReleaseBuffer(buf);
		weave_free_page(index, blk);
		blk = next;
	}

	if (seg->livedocs != InvalidBlockNumber)
		weave_free_chain(index, seg->livedocs);
	if (seg->dictindexstart != InvalidBlockNumber)
		weave_free_chain(index, seg->dictindexstart);
	if (seg->doclenstart != InvalidBlockNumber)
		weave_free_chain(index, seg->doclenstart);	/* v4 doclen sidecar */

	/*
	 * v7: every weft the descriptor page names, then the descriptor page itself.
	 *
	 * DRIVEN BY THE DESCRIPTOR, not by a hard-coded list, and that is the point.
	 * Every chain above is named by a WeaveSegMeta field, so a new weft that is
	 * NOT in WeaveSegMeta -- which is every weft from v6 onward -- would be
	 * invisible to a free path written that way, and the symptom is a leak of the
	 * whole structure on every merge with nothing but weave_check(deep)'s
	 * pages_reachable_or_freed to notice.  Walking the descriptor means the next
	 * weft is freed by this code as written.
	 *
	 * The read is deliberately the non-throwing one: freeing a bolt whose
	 * descriptor page is corrupt must still free everything it can rather than
	 * abort the merge, and the reachability invariant reports what was left
	 * behind.
	 */
	if (seg->chandesc != InvalidBlockNumber)
	{
		WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
		int			nweft = 0;
		int			i;

		if (weave_read_chandesc(index, seg->chandesc, weft, WEAVE_MAX_WEFTS,
								&nweft) == WEAVE_CD_OK)
		{
			for (i = 0; i < nweft; i++)
			{
				/* LEXICAL's root is dictstart, freed above; freeing it twice
				 * would push the same block on the free list twice. */
				if (weft[i].kind == (uint16) WEAVE_WK_LEXICAL)
					continue;
				weave_free_chain(index, weft[i].root);
			}
		}
		weave_free_chain(index, seg->chandesc);	/* v6 weft descriptor page */
	}
}

static void
weave_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				  Cost *indexStartupCost, Cost *indexTotalCost,
				  Selectivity *indexSelectivity, double *indexCorrelation,
				  double *indexPages)
{
	GenericCosts costs = {0};

	/* baseline: generic estimate gives selectivity, pages, and row counts */
	genericcostestimate(root, path, loop_count, &costs);

	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;

	/*
	 * A scan with neither a restriction clause nor an ordering clause is a full
	 * index scan, and this index CANNOT serve one correctly: weave_build_callback
	 * and weave_insert both skip NULL values, so the index has no entry for a row
	 * whose indexed column is NULL.  A full scan would therefore undercount, and
	 * an undercount from `SELECT count(*)` is a wrong answer rather than a slow
	 * one.
	 *
	 * amoptionalkey has to be true so the keyless ORDERING path exists (task L7),
	 * but that also lets the planner consider an Index Only Scan for an
	 * unqualified aggregate -- `count(*)` needs no columns at all, so
	 * check_index_only() succeeds regardless of amcanreturn.  Price that shape
	 * out of consideration.  weave_gettuple and weave_getbitmap additionally
	 * reject it at runtime, which is what catches the residual case where a
	 * competing path has been disabled by a GUC and is therefore never preferred
	 * on cost at all (PostgreSQL 18 compares disabled-node counts before costs).
	 *
	 * Setting a prohibitive cost rather than returning an error from here is
	 * deliberate: costing is called during planning for paths that may never be
	 * chosen, so erroring would break queries that would have planned fine.
	 */
	if (path->indexclauses == NIL && path->indexorderbys == NIL)
	{
		*indexStartupCost = 1.0e12;
		*indexTotalCost = 1.0e12;
		return;
	}

	if (path->indexorderbys != NIL)
	{
		/*
		 * Ordering scan (ORDER BY wdoc <=> wquery): the AM runs block-max
		 * WAND / MaxScore and, with a LIMIT pushed down, the executor pulls only
		 * about k best results -- work is sublinear in the match set, unlike a
		 * generic full index scan.  Price it as mostly a modest startup plus a
		 * small per-tuple cost, so the planner prefers the index (which honours
		 * the ORDER BY) over a seqscan + sort.  We deliberately keep this low
		 * but nonzero; the LIMIT is applied by the caller (limit_tuples), so a
		 * cheap-per-tuple total lets a small LIMIT win and a large one still
		 * scale.
		 */
		double		ntuples = costs.numIndexTuples;

		*indexStartupCost = costs.indexStartupCost + 2.0 * cpu_operator_cost;
		/* WAND touches ~log(N)*k blocks, not all matches: charge a fraction of
		 * a page fetch per matching tuple plus the per-tuple CPU */
		*indexTotalCost = *indexStartupCost +
			ntuples * (cpu_index_tuple_cost + cpu_operator_cost) +
			0.25 * costs.numIndexPages * costs.spc_random_page_cost;
	}
	else
	{
		/*
		 * Plain @@@ scan: the generic estimate (selectivity from clause
		 * selectivity, pages from the posting lists) is a reasonable model of
		 * decoding the matching TID sets, so use it as-is.
		 */
		*indexStartupCost = costs.indexStartupCost;
		*indexTotalCost = costs.indexTotalCost;
	}
}

static bytea *
weave_options(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"positions", RELOPT_TYPE_BOOL, offsetof(WeaveOptions, positions)},
		{"trigrams", RELOPT_TYPE_BOOL, offsetof(WeaveOptions, trigrams)},
		{"doclen_sidecar", RELOPT_TYPE_BOOL, offsetof(WeaveOptions, doclen_sidecar)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  weave_relopt_kind,
									  sizeof(WeaveOptions),
									  tab, lengthof(tab));
}

/*
 * Does this index carry token positions in its postings?  Reads the
 * `positions` reloption (default OFF -- positions ~double the posting bytes,
 * so the size-sensitive majority who never phrase-search pay nothing; phrase
 * users opt in with WITH (positions=on)).  Phrase/NEAR is CORRECT either way:
 * positions=on evaluates it from the postings with no recheck; positions=off
 * falls back to the (correct, slower) heap recheck.
 */
bool
weave_index_wants_positions(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;

	return opts ? opts->positions : false;
}

/*
 * Does this index build the per-segment trigram index?  Reads the `trigrams`
 * reloption (default OFF).  The trigram index accelerates only regex and
 * over-long fuzzy terms and is ~18% of the on-disk index, so it is opt-in:
 * plain/boolean/ranked/phrase/prefix/short-fuzzy queries never consult it, and
 * regex/long-fuzzy remain CORRECT without it (they fall back to a full
 * dictionary scan when trgmstart is InvalidBlockNumber).  Turn it on with
 * WITH (trigrams=on) for regex- or long-fuzzy-heavy workloads.
 */
bool
weave_index_wants_trigrams(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;

	return opts ? opts->trigrams : false;
}

/* Default ON: a new index uses the quantized doclen sidecar (v4).  WITH
 * (doclen_sidecar=off) stores doclen inline in each posting instead (the
 * pre-1.5 layout) -- an escape hatch for workloads that prefer the
 * pre-sidecar ranked-scan behavior. */
bool
weave_index_wants_doclen_sidecar(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;
	bool		r = opts ? opts->doclen_sidecar : true;

	return r;
}

static bool
weave_validate(Oid opclassoid)
{
	return true;
}

Datum
weave_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies = 2;
	amroutine->amsupport = 0;
	amroutine->amoptsprocnum = 0;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = true;
#if PG_VERSION_NUM >= 180000
	amroutine->amcanhash = false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering = false;
#endif
	amroutine->amcanbackward = false;
	amroutine->amcanunique = false;
	amroutine->amcanmulticol = false;
	/*
	 * amoptionalkey: can a scan run with no restriction clause on the first
	 * index column?
	 *
	 * TRUE, and this single flag is the whole of task L7.  With it false the
	 * planner refuses to generate ANY index path when there is no `WHERE d @@@ q`
	 * clause, so the pgvector idiom
	 *
	 *		SELECT ... ORDER BY d <=> 'query'::wquery LIMIT 10
	 *
	 * silently fell back to a Seq Scan plus a top-N Sort that evaluated <=> on
	 * every row of the table: measured 83 ms with 4 parallel workers and 362 ms
	 * serial on 1M documents, against 0.05 ms for the same intent written with
	 * the redundant WHERE clause (bench/RESULTS_LEXICAL.md, doc/GAPS.md G1).
	 * Correct results, a 7,000x cliff, and no diagnostic -- on the form every
	 * user writes first, because that is what pgvector taught them.
	 *
	 * Nothing else in the AM needed changing, which is why this went unnoticed:
	 * weave_rescan already takes the query from the order-by argument when there
	 * are no scan keys, and the ordering path already rechecks the exact @@@ test
	 * itself rather than relying on an executor recheck (it has to, since an
	 * ordering scan gets none).  So the keyless ordering scan runs the identical
	 * candidate-generation and ranking code as the qualified form.
	 *
	 * The hazard this flag introduces is a scan with NEITHER a key NOR an
	 * order-by -- reachable via a partial index, where a useful predicate alone
	 * justifies a path.  weave_gettuple and weave_getbitmap reject that
	 * explicitly rather than returning zero rows, because an empty result from a
	 * full-index-scan plan is a wrong answer, not a slow one.
	 */
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amcanparallel = false;
#if PG_VERSION_NUM >= 170000
	amroutine->amcanbuildparallel = true;
#endif
	amroutine->amcaninclude = false;
	amroutine->amusemaintenanceworkmem = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_NO_PARALLEL;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = weave_build;
	amroutine->ambuildempty = weave_buildempty;
	amroutine->aminsert = weave_insert;
#if PG_VERSION_NUM >= 170000
	amroutine->aminsertcleanup = NULL;
#endif
	amroutine->ambulkdelete = weave_bulkdelete;
	amroutine->amvacuumcleanup = weave_vacuumcleanup;
	amroutine->amcanreturn = weave_canreturn;
	amroutine->amcostestimate = weave_costestimate;
#if PG_VERSION_NUM >= 180000
	amroutine->amgettreeheight = NULL;
#endif
	amroutine->amoptions = weave_options;
	amroutine->amproperty = NULL;
	amroutine->ambuildphasename = NULL;
	amroutine->amvalidate = weave_validate;
	amroutine->amadjustmembers = NULL;
	amroutine->ambeginscan = weave_beginscan;
	amroutine->amrescan = weave_rescan;
	amroutine->amgettuple = weave_gettuple;
	amroutine->amgetbitmap = weave_getbitmap;
	amroutine->amendscan = weave_endscan;
	amroutine->ammarkpos = NULL;
	amroutine->amrestrpos = NULL;
	amroutine->amestimateparallelscan = NULL;
	amroutine->aminitparallelscan = NULL;
	amroutine->amparallelrescan = NULL;

	PG_RETURN_POINTER(amroutine);
}
