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
#include "weave/vector.h"			/* V7: the vector weft's writer, reader and free path */
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
#include "catalog/pg_opclass.h"		/* Form_pg_opclass (amvalidate) */
#include "catalog/pg_opfamily.h"		/* Form_pg_opfamily (opfamily -> weft kind) */
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
#include "utils/inval.h"		/* CacheRegisterRelcacheCallback (surf trie cache) */
#include "utils/lsyscache.h"	/* get_rel_name (maintenance-fn guard) */
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"		/* SearchSysCache1(CLAOID) in weave_index_layout */
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
	int			bits;			/* vector code width, 2..8, default
								 * WEAVE_VEC_DEFAULT_BITS (4).  A RELOPTION and not a
								 * GUC because it changes the bytes on disk
								 * (doc/CONVENTIONS.md decision 1): a GUC would make
								 * one index's segments disagree about their code
								 * width depending on which session wrote them.  The
								 * value used is recorded in each weft's WEAVE_VMETA
								 * page, so a reader never consults this. */
	int			metric;			/* WeaveMetric the next vector weft is scored with,
								 * default WEAVE_METRIC_L2.  A reloption for the
								 * SECOND clause of doc/CONVENTIONS.md decision 1
								 * rather than the first: it changes no stored byte
								 * (codes are metric-independent), but "the value
								 * used is recorded in the segment so a reader never
								 * has to guess" is exactly what
								 * WeaveVecMeta.metric is for, and a GUC would let
								 * two bolts of one index disagree about what their
								 * scores MEAN (doc/GAPS.md G26).  The eventual
								 * user-facing form is one operator family per
								 * metric -- wvec_l2_ops, wvec_ip_ops -- which
								 * belongs with the ORDER BY ... <-> ... path that
								 * task V8 does not build; see
								 * doc/specs/VECTOR_CHANNEL.md sect. 8b. */
} WeaveOptions;

static relopt_kind weave_relopt_kind;

/*
 * The `metric` reloption's members.  Every WeaveMetric is accepted by the parser;
 * two of them are refused by weave_index_vec_metric() with the reason, which the
 * comment at add_enum_reloption() below explains.
 */
static relopt_enum_elt_def weave_metric_options[] =
{
	{"l2", WEAVE_METRIC_L2},
	{"ip", WEAVE_METRIC_IP},
	{"cosine", WEAVE_METRIC_COSINE},
	{"l1", WEAVE_METRIC_L1},
	{(const char *) NULL}
};

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
	/*
	 * 4 bits is the ratified Phase V shape: with an exact top-25 rerank it reaches
	 * recall@10 0.9920 at n = 1M on GIST-960d for 512 B/vector, and 4 is the
	 * widest width that still has a SIMD scoring kernel.  Widths up to 8 are
	 * accepted because the codec supports them and the recall ceiling at each is
	 * measured (bench/RESULTS_BITWIDTH_SWEEP.md); they are not recommended.  See
	 * include/weave/vector.h.
	 */
	add_int_reloption(weave_relopt_kind, "bits",
					  "vector code width in bits (2..8)",
					  WEAVE_VEC_DEFAULT_BITS, WEAVE_BITS_MIN, WEAVE_BITS_MAX,
					  AccessExclusiveLock);

	/*
	 * The metric, default l2.  Every weft already on disk says l2 (V7 wrote it as
	 * a placeholder), so the default is the value that keeps existing bolts
	 * meaning what they already meant.
	 *
	 * COSINE AND L1 ARE MEMBERS HERE AND REFUSED AT BUILD TIME, which looks
	 * redundant and is not.  Leaving them out of the member list makes
	 * WITH (metric = 'cosine') fail with "invalid value", which tells a user
	 * nothing about WHY -- and the why is specific and worth saying: cosine's
	 * bound has to switch on the sign of its numerator and no maximum true norm is
	 * stored, and L1 admits no compressed-domain bound at all
	 * (doc/specs/VECTOR_CHANNEL.md sect. 8b, sect. 11).  So they parse, and
	 * weave_index_vec_metric() refuses them with the reason.
	 */
	add_enum_reloption(weave_relopt_kind, "metric",
					   "distance metric the vector weft is scored with",
					   weave_metric_options, WEAVE_METRIC_L2,
					   "Valid values are \"l2\", \"ip\", \"cosine\" and \"l1\".",
					   AccessExclusiveLock);
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
		pend = weave_page_entry_end(page);
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
 * Snapshot-scope probe guard.  See the bounded-probe comment in
 * weave_new_buffer(): a snapshot whose first WEAVE_ALLOC_SNAPSHOT_PROBE_MAX
 * candidates all fail the recycle gate is abandoned for the rest of the scope,
 * because probing costs a buffer read each and inside one long transaction the
 * whole snapshot is usually doomed.  `_at_entry` are the values the scope started
 * with, so the test is per scope and not cumulative across a backend.
 */
#define WEAVE_ALLOC_SNAPSHOT_PROBE_MAX 64

static bool weave_lowfree_giveup = false;
static int	weave_lowfree_probe_at_entry = 0;
static uint64 weave_lowfree_reuse_at_entry = 0;

/*
 * Extend-only allocation mode.  When set, weave_new_buffer() skips ALL free-page
 * reuse (the low-free list AND the FSM) and only extends the relation, so a
 * rewrite writes its whole output to fresh high blocks.  Used by the vacuum
 * compactor's "vacate" phase to push a live segment above the free region,
 * turning the freed old pages into one contiguous low-free run big enough for
 * the following "pack" phase to relocate the segment to the front and truncate.
 */
bool		weave_alloc_extend_only = false;

/*
 * SNAPSHOT allocation mode: hand out only what weave_alloc_begin() gathered, then
 * extend, and never consult the live FSM.  See weave_alloc_snapshot_enter() in
 * include/weave/am.h for why a merge loop needs exactly this and why extend-only
 * was the wrong shape of the same guard.
 */
bool		weave_alloc_no_fsm = false;

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

/* GUC: tombstone fraction above which a SINGLE segment is no longer considered
 * to be at its compaction floor.  See weave_index_is_compacted() in
 * src/am/amvacuum.c for why this term has to exist at all (task L18): every
 * other term in that predicate is about free space, and a tombstone is not free
 * space -- it is a live page holding a posting nobody can see.
 *
 * THE DEFAULT IS NOT MEASURED.  0.2 is the conventional choice and it is a
 * guess; the frontier has not been swept.  It trades rewrite cost (a rewrite
 * streams the whole segment through the buffer pool twice) against space held by
 * invisible postings, and the right value certainly depends on segment size.
 * Recorded as a guess rather than presented as a tuned default. */
double		pg_weave_vacuum_tombstone_frac = 0.2;

/* GUC: per-participant flush-budget growth ceiling, in MB.  0 = keep the safe
 * default ceiling of 2 * maintenance_work_mem (unchanged behavior).  When set
 * larger, a build lets each participant's flush budget grow up to this, so a
 * large corpus flushes FEWER, LARGER segments and the live segment count stays
 * well under WEAVE_MAX_SEGMENTS (which would otherwise abort a very large
 * parallel build).  Peak build memory is about (max_parallel_maintenance_workers
 * + 1) * this ceiling -- size it against available RAM.  Defined here,
 * registered in _PG_init (pg_weave_customscan.c). */
int			pg_weave_build_mem_ceiling_mb = 0;

/* GUC: backend-local budget, in MB, for resident SuRF trie images.  0 disables
 * the cache and restores the pre-Z4-part-2 behaviour exactly (every consult is a
 * whole-image load).  Defined here because the cache it bounds is below in this
 * file; registered in _PG_init (src/am/customscan.c) with the other GUCs. */
int			pg_weave_surf_cache_mb = 32;

/* Off by default: it costs a comparison and a branch per fused score() call.  Why it
 * exists at all is on the extern in include/weave/weave.h -- doc/GAPS.md G43. */
bool		pg_weave_fuse_check_bounds = false;

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

/*
 * Enter/leave a SNAPSHOT allocation scope.  The contract, the hazard it is exact
 * against, and the two preconditions are in include/weave/am.h; this is only the
 * state handling.
 *
 * The snapshot IS the low-free list -- the same gather, the same ascending order,
 * the same per-candidate recycle gate -- with the live-FSM fallback switched off.
 * Reusing it rather than adding a second gathering path is deliberate: there is
 * one place where a free-page candidate can be produced, so there is one place to
 * get the recycle gate wrong.  It also means a merge's reuse shows up in the
 * lowfree_* allocator counters rather than the fsm_* ones -- see the counter
 * comment, which exists because a zero in the wrong column has been misread as
 * "the free list was never consulted" before.
 */
void
weave_alloc_snapshot_enter(Relation index, WeaveAllocScope *saved)
{
	saved->lowfree = weave_lowfree;
	saved->lowfree_n = weave_lowfree_n;
	saved->lowfree_i = weave_lowfree_i;
	saved->extend_only = weave_alloc_extend_only;
	saved->no_fsm = weave_alloc_no_fsm;
	saved->giveup = weave_lowfree_giveup;
	saved->probe_at_entry = weave_lowfree_probe_at_entry;
	saved->reuse_at_entry = weave_lowfree_reuse_at_entry;

	/* NULL first so the gather cannot be mistaken for owning the saved array */
	weave_lowfree = NULL;
	weave_alloc_begin(index);	/* gather ONCE, before this scope frees anything */
	weave_alloc_extend_only = false;
	weave_alloc_no_fsm = true;
	weave_lowfree_giveup = false;
	weave_lowfree_probe_at_entry = weave_lowfree_i;		/* 0, from the gather */
	weave_lowfree_reuse_at_entry = weave_alloc_lowfree_reuse;
}

void
weave_alloc_scope_leave(const WeaveAllocScope *saved)
{
	if (weave_lowfree != NULL && weave_lowfree != saved->lowfree)
		pfree(weave_lowfree);
	weave_lowfree = saved->lowfree;
	weave_lowfree_n = saved->lowfree_n;
	weave_lowfree_i = saved->lowfree_i;
	weave_alloc_extend_only = saved->extend_only;
	weave_alloc_no_fsm = saved->no_fsm;
	weave_lowfree_giveup = saved->giveup;
	weave_lowfree_probe_at_entry = saved->probe_at_entry;
	weave_lowfree_reuse_at_entry = saved->reuse_at_entry;
}

/*
 * ALLOCATOR OUTCOME COUNTERS.
 *
 * Backend-local, always compiled in, read via weave_alloc_stats().  They exist
 * because two separate bloat investigations in this lineage were blocked on not
 * knowing which of three things the allocator was doing, and both wasted a cycle
 * guessing.  Every page pg_weave allocates comes from exactly one of:
 *
 *   lowfree_reuse  -- the compaction low-bias list (weave_alloc_begin gathered it)
 *   fsm_reuse      -- the free space map, via GetFreeIndexPage
 *   extend         -- P_NEW, the relation grew
 *
 * plus two ways a reuse candidate is passed over:
 *
 *   *_defer        -- weave_page_recyclable() said a concurrent scan might still
 *                     hold it, so it went back to the FSM unused
 *   *_contended    -- ConditionalLockBuffer failed; someone else had the buffer
 *
 * WHY ALWAYS-ON RATHER THAN BEHIND A BUILD FLAG.  The counters are one increment
 * per ReadBuffer, i.e. unmeasurable next to the buffer read itself.  A build flag
 * would mean the numbers are only available from a binary nobody is running, and
 * the sibling project recorded "verify counters are present in the shipped .so
 * before trusting results" as a lesson learned the hard way.  Reading them from
 * SQL rather than elog(LOG) is deliberate for the same reason: log_min_messages
 * = warning silences elog(LOG), which cost that project a whole run.
 *
 * These answer a question, they are not a diagnosis.  A high extend count with a
 * high defer count means the recycle gate is the constraint; a high extend count
 * with defer = 0 means the free list was never even consulted, which is a
 * different bug entirely.  Distinguishing those two by reasoning is exactly what
 * has failed here before.
 *
 * WHICH COLUMN A MERGE LANDS IN CHANGED, and reading the old column will tell you
 * a merge stopped reusing pages when it started.  A merge now allocates in
 * SNAPSHOT mode (weave_alloc_snapshot_enter), whose candidates come from the
 * gathered list, so merge reuse is counted as lowfree_reuse / lowfree_defer and
 * a merge's fsm_reuse is 0 BY CONSTRUCTION -- it never consults the live FSM.
 * Before that change the same pages were counted in fsm_*, and the mode before
 * THAT (extend-only) consulted neither.  Compare the sum, or compare extend.
 *
 * THE WAY THESE LIE TO YOU, and it is not hypothetical -- the sibling project hit
 * it and read the result as "the allocator is never called".  Because they are
 * backend-local, a counter read in a DIFFERENT session than the operation reports
 * that session's zeros.  A shell loop of `psql -c` is a new backend per -c, so
 * every read is zero no matter how much the index grew.  Zero here means "nothing
 * happened in THIS backend", never "nothing happened".  Reset, operate and read in
 * ONE session; t/015_alloc_outcomes.pl's bracket() exists to make that structural
 * rather than remembered.
 */
uint64		weave_alloc_lowfree_reuse = 0;
uint64		weave_alloc_lowfree_defer = 0;
uint64		weave_alloc_lowfree_contended = 0;
uint64		weave_alloc_fsm_reuse = 0;
uint64		weave_alloc_fsm_defer = 0;
uint64		weave_alloc_fsm_contended = 0;
uint64		weave_alloc_extend = 0;

/*
 * CHANNEL-MECHANISM COUNTERS.  Contract, rationale and the list of which ones are
 * structurally zero today are on the extern declarations in include/weave/weave.h.
 * Defined here beside the allocator counters because they share every property
 * that makes those readable from SQL rather than logged.
 */
uint64		weave_chan_lex_term = 0;
uint64		weave_chan_prefix_dict = 0;
uint64		weave_chan_prefix_surf = 0;
uint64		weave_chan_fuzzy_dict = 0;
uint64		weave_chan_fuzzy_surf = 0;
uint64		weave_chan_fuzzy_trgm = 0;
uint64		weave_chan_regex_dict = 0;
uint64		weave_chan_regex_trgm = 0;
uint64		weave_chan_regex_surf = 0;
uint64		weave_chan_vector_scan = 0;
uint64		weave_chan_cgram_scan = 0;
uint64		weave_chan_terms_expanded = 0;
uint64		weave_chan_dict_pages = 0;
uint64		weave_chan_surf_loads = 0;
uint64		weave_chan_surf_bytes = 0;
uint64		weave_chan_surf_cache_hits = 0;
uint64		weave_chan_surf_cache_misses = 0;
uint64		weave_chan_surf_cache_evicts = 0;
uint64		weave_chan_surf_cache_bytes = 0;

/*
 * FUSED-SCORER WORK COUNTERS.  Contract and the two ways to misread `scores` are
 * on the extern declarations in include/weave/weave.h; the accumulation is in
 * src/am/amscan.c, next to the pass that produces the numbers.
 */
uint64		weave_fuse_passes = 0;
uint64		weave_fuse_runs = 0;
uint64		weave_fuse_chans = 0;
uint64		weave_fuse_seeks = 0;
uint64		weave_fuse_scores = 0;
uint64		weave_fuse_bounds = 0;
uint64		weave_fuse_pivots = 0;
uint64		weave_fuse_blkskip = 0;
uint64		weave_fuse_rqskip = 0;
uint64		weave_fuse_livedrop = 0;
uint64		weave_fuse_veto = 0;
uint64		weave_fuse_abandon = 0;
uint64		weave_fuse_vec_scores = 0;
uint64		weave_fuse_gate_scores = 0;

/*
 * CHANNEL WORK COUNTERS.  Contract, the reason they are a separate function, and
 * the reason the lexical one deliberately excludes the fused path are all on the
 * extern declarations in include/weave/weave.h.
 */
uint64		weave_lex_contribs = 0;
uint64		weave_vecwork_lanes = 0;
uint64		weave_vecwork_blocks = 0;
uint64		weave_vecwork_blk_bound = 0;
uint64		weave_vecwork_shuttles = 0;

Buffer
weave_new_buffer(Relation index)
{
	Buffer		buffer;

	/*
	 * Low-bias reuse: during a compaction, prefer the lowest free block so
	 * live pages pack at the front of the file.  In SNAPSHOT mode this same list
	 * is the merge's whole supply of reusable pages (weave_alloc_snapshot_enter).
	 *
	 * A deferred candidate here does `continue`, not `break`: the list is ours and
	 * a block we skip is not handed back to us on the next iteration, so one
	 * not-yet-recyclable page costs one page and not the rest of the sequence.
	 * That is the difference from the FSM loop below, and it is the whole reason
	 * the snapshot is a list rather than repeated GetFreeIndexPage() calls.
	 */
	while (!weave_alloc_extend_only && weave_lowfree &&
		   weave_lowfree_i < weave_lowfree_n && !weave_lowfree_giveup)
	{
		BlockNumber blk = weave_lowfree[weave_lowfree_i++];

		buffer = ReadBuffer(index, blk);
		if (ConditionalLockBuffer(buffer))
		{
			if (!weave_page_recyclable(index, BufferGetPage(buffer)))
			{
				/* a scan may still reference this just-freed page; leave it in
				 * the FSM for a later allocation once its horizon passes */
				weave_alloc_lowfree_defer++;
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				RecordFreeIndexPage(index, blk);
				/*
				 * BOUNDED PROBE, snapshot mode only, and it is a measured cost
				 * rather than a precaution.  Every candidate costs a buffer read
				 * plus an FSM update, and a merge inside ONE long transaction
				 * frees its own input pages, so its snapshot is almost entirely
				 * pages the recycle gate must refuse -- and the scope is re-entered
				 * per merge call, which at one merge per inserted document means
				 * the same doomed candidates are re-probed every time.  Measured
				 * on the G20 bulk arm (6,000 documents at 1,660 terms, six
				 * transactions): 33,915,333 deferred candidates and 0 reuses in
				 * the first two batches, with the arm's wall clock rising from
				 * ~1m20s to ~2m10s while the page count fell.
				 *
				 * So: if a scope has probed this many candidates without a single
				 * reuse, treat its snapshot as unusable and extend for the rest of
				 * the scope.  The list is sorted ascending and the lowest free
				 * blocks are the OLDEST frees -- the most likely to be past the
				 * horizon -- so a run of failures at the front is evidence about
				 * the whole list, not just its head.  The compaction path is
				 * deliberately exempt (it must pack into the low region to be able
				 * to truncate, and under AccessExclusiveLock its candidates pass
				 * the gate anyway).
				 */
				if (weave_alloc_no_fsm && weave_alloc_lowfree_reuse == weave_lowfree_reuse_at_entry &&
					weave_lowfree_i - weave_lowfree_probe_at_entry >= WEAVE_ALLOC_SNAPSHOT_PROBE_MAX)
					weave_lowfree_giveup = true;
				continue;
			}
			RecordUsedIndexPage(index, blk);
			weave_alloc_lowfree_reuse++;
			return buffer;
		}
		weave_alloc_lowfree_contended++;
		ReleaseBuffer(buffer);
	}

	/*
	 * Try to reuse a page freed by a previous merge before extending.
	 *
	 * SKIPPED IN SNAPSHOT MODE.  The live FSM may by now hold pages THIS
	 * operation freed a moment ago, while a reader chain of ours still threads
	 * through them; the snapshot gathered at scope entry provably does not.
	 */
	while (!weave_alloc_extend_only && !weave_alloc_no_fsm)
	{
		BlockNumber blk = GetFreeIndexPage(index);

		if (blk == InvalidBlockNumber)
			break;				/* no free page; extend below */
		buffer = ReadBuffer(index, blk);
		if (ConditionalLockBuffer(buffer))
		{
			if (!weave_page_recyclable(index, BufferGetPage(buffer)))
			{
				/*
				 * Not yet safe to reuse (a concurrent scan could still be reading
				 * it): re-record it so a later allocation gets it, and STOP.
				 *
				 * THE `break` IS DELIBERATE AND ITS COST IS REAL.  This used to say
				 * "and try the next free page", which is not what the code does and
				 * could not be: GetFreeIndexPage() *removes* the page from the FSM
				 * and RecordFreeIndexPage() puts it back, so a `continue` would
				 * hand out the same block forever.  The consequence of stopping is
				 * that ONE not-yet-recyclable page abandons FSM reuse for the whole
				 * rest of this allocation sequence and everything after it extends
				 * -- which is a plausible mechanism for freed-but-never-reused
				 * growth, and is why weave_alloc_fsm_defer is counted separately
				 * from weave_alloc_extend.  Do not "fix" this into a loop without
				 * a way to skip a block rather than re-queue it.
				 *
				 * THE MERGE PATH NO LONGER COMES THROUGH HERE, which is how that
				 * mechanism was dealt with: a merge allocates in SNAPSHOT mode over
				 * a gathered list it owns, where a deferred candidate is skipped
				 * with `continue` and costs one page.  The remaining users of this
				 * loop are ordinary segment writes, for which one deferred page
				 * ending the sequence is a bounded cost, not a ratchet.
				 */
				weave_alloc_fsm_defer++;
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				RecordFreeIndexPage(index, blk);
				break;
			}
			weave_alloc_fsm_reuse++;
			return buffer;		/* got it */
		}
		/* someone else is using it; try the next free page */
		weave_alloc_fsm_contended++;
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
	weave_alloc_extend++;
	return buffer;
}

PG_FUNCTION_INFO_V1(weave_alloc_stats);
PG_FUNCTION_INFO_V1(weave_alloc_stats_reset);

/*
 * weave_alloc_stats() -> record : this backend's page-allocation outcomes.
 *
 * Backend-local and cumulative since backend start or the last
 * weave_alloc_stats_reset().  NOT per-index: the counters sit on the allocator,
 * which is backend-scoped state (weave_lowfree, weave_alloc_extend_only), so a
 * measurement run should touch one index in one session.  That is a real
 * limitation and it is the honest shape -- attributing a count to an index would
 * mean shared memory and a stats collector for something whose only job is to
 * answer "which of three branches ran".
 *
 * PARALLEL RESTRICTED, not safe: a parallel build's workers each allocate pages
 * into their own counters and the leader would report only its own, which is a
 * wrong answer rather than a slow one.
 *
 * A ZERO FROM THIS FUNCTION IS NOT EVIDENCE unless the operation ran in the same
 * session as the read; see the note on the counters themselves.
 */
Datum
weave_alloc_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7] = {false, false, false, false, false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) weave_alloc_lowfree_reuse);
	values[1] = Int64GetDatum((int64) weave_alloc_lowfree_defer);
	values[2] = Int64GetDatum((int64) weave_alloc_lowfree_contended);
	values[3] = Int64GetDatum((int64) weave_alloc_fsm_reuse);
	values[4] = Int64GetDatum((int64) weave_alloc_fsm_defer);
	values[5] = Int64GetDatum((int64) weave_alloc_fsm_contended);
	values[6] = Int64GetDatum((int64) weave_alloc_extend);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* Zero this backend's allocator counters, so a measurement can bracket one
 * operation instead of reporting everything since connect. */
Datum
weave_alloc_stats_reset(PG_FUNCTION_ARGS)
{
	weave_alloc_lowfree_reuse = 0;
	weave_alloc_lowfree_defer = 0;
	weave_alloc_lowfree_contended = 0;
	weave_alloc_fsm_reuse = 0;
	weave_alloc_fsm_defer = 0;
	weave_alloc_fsm_contended = 0;
	weave_alloc_extend = 0;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(weave_channel_stats);
PG_FUNCTION_INFO_V1(weave_channel_stats_reset);

/*
 * weave_channel_stats() -> record : which mechanism inside the index served the
 * query leaves this backend evaluated, and how much vocabulary work they did.
 *
 * Same shape, same limitations and the same "a zero is not evidence unless the
 * query ran in this session" caveat as weave_alloc_stats(); see the counters in
 * include/weave/weave.h.
 *
 * PARALLEL RESTRICTED for the reason weave_alloc_stats() is: a parallel scan's
 * workers count into their own copies and the leader would report only its own,
 * which is a wrong answer rather than a slow one.
 */
Datum
weave_channel_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[19];
	bool		nulls[19];
	HeapTuple	tuple;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	for (i = 0; i < 19; i++)
		nulls[i] = false;

	values[0] = Int64GetDatum((int64) weave_chan_lex_term);
	values[1] = Int64GetDatum((int64) weave_chan_prefix_dict);
	values[2] = Int64GetDatum((int64) weave_chan_prefix_surf);
	values[3] = Int64GetDatum((int64) weave_chan_fuzzy_dict);
	values[4] = Int64GetDatum((int64) weave_chan_fuzzy_trgm);
	values[5] = Int64GetDatum((int64) weave_chan_fuzzy_surf);
	values[6] = Int64GetDatum((int64) weave_chan_regex_dict);
	values[7] = Int64GetDatum((int64) weave_chan_regex_trgm);
	values[8] = Int64GetDatum((int64) weave_chan_regex_surf);
	values[9] = Int64GetDatum((int64) weave_chan_vector_scan);
	values[10] = Int64GetDatum((int64) weave_chan_terms_expanded);
	values[11] = Int64GetDatum((int64) weave_chan_dict_pages);
	values[12] = Int64GetDatum((int64) weave_chan_surf_loads);
	values[13] = Int64GetDatum((int64) weave_chan_surf_bytes);
	values[14] = Int64GetDatum((int64) weave_chan_surf_cache_hits);
	values[15] = Int64GetDatum((int64) weave_chan_surf_cache_misses);
	values[16] = Int64GetDatum((int64) weave_chan_surf_cache_evicts);
	values[17] = Int64GetDatum((int64) weave_chan_surf_cache_bytes);
	values[18] = Int64GetDatum((int64) weave_chan_cgram_scan);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* Zero this backend's channel counters, so a measurement can bracket one query
 * instead of reporting everything since connect. */
Datum
weave_channel_stats_reset(PG_FUNCTION_ARGS)
{
	weave_chan_lex_term = 0;
	weave_chan_prefix_dict = 0;
	weave_chan_prefix_surf = 0;
	weave_chan_fuzzy_dict = 0;
	weave_chan_fuzzy_surf = 0;
	weave_chan_fuzzy_trgm = 0;
	weave_chan_regex_dict = 0;
	weave_chan_regex_trgm = 0;
	weave_chan_regex_surf = 0;
	weave_chan_vector_scan = 0;
	weave_chan_cgram_scan = 0;
	weave_chan_terms_expanded = 0;
	weave_chan_dict_pages = 0;
	weave_chan_surf_loads = 0;
	weave_chan_surf_bytes = 0;
	weave_chan_surf_cache_hits = 0;
	weave_chan_surf_cache_misses = 0;
	weave_chan_surf_cache_evicts = 0;
	/* surf_cache_bytes is NOT reset: it is a gauge of memory this backend is
	 * still holding, and zeroing it here would report a false zero for as long as
	 * the images stay resident.  weave.h says the same thing next to the counter. */
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(weave_fuse_stats);
PG_FUNCTION_INFO_V1(weave_fuse_stats_reset);

/*
 * weave_fuse_stats() -> record : how much work the fused scorer did in this
 * backend, and how much of it the bounds removed.
 *
 * The contract, and above all the two ways `scores` can be misread into a wrong
 * ratio, are on the extern declarations in include/weave/weave.h.  Read them
 * before quoting any number from here in doc/specs/FUSED_TOPK.md sect. 8: `passes`
 * and `runs` are not decoration, they are the denominators that make `scores`
 * mean something.
 *
 * PARALLEL RESTRICTED for the reason weave_alloc_stats() is: workers would count
 * into their own copies and the leader would report only its own, which is a wrong
 * answer rather than a slow one.
 */
Datum
weave_fuse_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[14];
	bool		nulls[14];
	HeapTuple	tuple;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	for (i = 0; i < 14; i++)
		nulls[i] = false;

	values[0] = Int64GetDatum((int64) weave_fuse_passes);
	values[1] = Int64GetDatum((int64) weave_fuse_runs);
	values[2] = Int64GetDatum((int64) weave_fuse_chans);
	values[3] = Int64GetDatum((int64) weave_fuse_seeks);
	values[4] = Int64GetDatum((int64) weave_fuse_scores);
	values[5] = Int64GetDatum((int64) weave_fuse_bounds);
	values[6] = Int64GetDatum((int64) weave_fuse_pivots);
	values[7] = Int64GetDatum((int64) weave_fuse_blkskip);
	values[8] = Int64GetDatum((int64) weave_fuse_rqskip);
	values[9] = Int64GetDatum((int64) weave_fuse_livedrop);
	values[10] = Int64GetDatum((int64) weave_fuse_veto);
	values[11] = Int64GetDatum((int64) weave_fuse_abandon);
	values[12] = Int64GetDatum((int64) weave_fuse_vec_scores);
	values[13] = Int64GetDatum((int64) weave_fuse_gate_scores);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* Zero this backend's fused-scorer counters, so a measurement can bracket one
 * query.  Every column here is a cumulative count rather than a gauge, so unlike
 * weave_channel_stats_reset() this one has nothing it must leave alone. */
Datum
weave_fuse_stats_reset(PG_FUNCTION_ARGS)
{
	weave_fuse_passes = 0;
	weave_fuse_runs = 0;
	weave_fuse_chans = 0;
	weave_fuse_seeks = 0;
	weave_fuse_scores = 0;
	weave_fuse_bounds = 0;
	weave_fuse_pivots = 0;
	weave_fuse_blkskip = 0;
	weave_fuse_rqskip = 0;
	weave_fuse_livedrop = 0;
	weave_fuse_veto = 0;
	weave_fuse_abandon = 0;
	weave_fuse_vec_scores = 0;
	weave_fuse_gate_scores = 0;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(weave_work_stats);
PG_FUNCTION_INFO_V1(weave_work_stats_reset);

/*
 * weave_work_stats() -> record : what the CHANNELS did, on whatever path asked them.
 *
 * The contract is on the extern declarations in include/weave/weave.h, and two parts
 * of it decide whether a number taken from here means anything: `lex_contribs` counts
 * the single-channel WAND path ONLY (the fused path's lexical work is
 * weave_fuse_stats().scores minus its vec_scores and gate_scores), and `vec_lanes`
 * rather than a score() count is the vector channel's unit, because the kernel scores
 * a 32-lane block at a time.
 *
 * PARALLEL RESTRICTED for the reason every other counter function here is.
 */
Datum
weave_work_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5];
	HeapTuple	tuple;
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	for (i = 0; i < 5; i++)
		nulls[i] = false;

	values[0] = Int64GetDatum((int64) weave_lex_contribs);
	values[1] = Int64GetDatum((int64) weave_vecwork_lanes);
	values[2] = Int64GetDatum((int64) weave_vecwork_blocks);
	values[3] = Int64GetDatum((int64) weave_vecwork_blk_bound);
	values[4] = Int64GetDatum((int64) weave_vecwork_shuttles);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* Zero this backend's channel work counters. */
Datum
weave_work_stats_reset(PG_FUNCTION_ARGS)
{
	weave_lex_contribs = 0;
	weave_vecwork_lanes = 0;
	weave_vecwork_blocks = 0;
	weave_vecwork_blk_bound = 0;
	weave_vecwork_shuttles = 0;
	PG_RETURN_VOID();
}

/*
 * Is ANY currently-free page recyclable right now?
 *
 * Lives here, next to weave_page_recyclable() which is static, because the
 * question is allocator knowledge: the answer is what decides whether a
 * relocation pass can pack into the space it frees or can only extend the
 * relation.  amvacuum.c asks it before starting a vacate+pack under a lock weaker
 * than AccessExclusiveLock (see the long comment at that call site for the two
 * measured behaviours this protects against).
 *
 * Returns false when there are no free pages at all, which is the right answer for
 * the caller: with nothing to pack into, a relocation can only extend.
 *
 * BOUNDED PROBE.  It stops after WEAVE_RECYCLE_PROBE_MAX candidates because each
 * one costs a buffer read, and the caller may run on every autovacuum cycle.  The
 * bound can only produce a FALSE NEGATIVE -- "nothing recyclable" when a page
 * beyond the probe window was -- which makes the caller skip a pass it could have
 * done.  That is self-correcting on the next cycle and is the safe direction: a
 * false positive would start a relocation that can only grow the file.
 *
 * NECESSARY, NOT SUFFICIENT, and deliberately so.  "Some page is recyclable" does
 * not mean "enough pages are recyclable to hold the live data", so a pass can still
 * start, pack into the few pages it has, and extend for the rest.  The sibling
 * project asks the stronger question -- count recyclable pages against the live
 * size -- and that stronger version is what produced its own no-reclaim regression:
 * the count is taken from free-space records, stale records overstate the live
 * size, and a compaction that WOULD have reclaimed is then skipped forever.  This
 * probe asks about RECYCLABILITY instead, which is a property of the freeing
 * transaction's xid and becomes true on its own as the horizon advances, so a
 * skipped pass cannot become a permanently skipped pass.  Tightening this to a
 * count therefore trades a bounded overshoot for an unbounded stall, and must not
 * be done without a measurement that shows the overshoot matters.
 */
#define WEAVE_RECYCLE_PROBE_MAX 256

bool
weave_any_free_page_recyclable(Relation index)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber blk;
	int			probed = 0;

	for (blk = 1; blk < nblocks && probed < WEAVE_RECYCLE_PROBE_MAX; blk++)
	{
		Buffer		buf;
		bool		ok;

		CHECK_FOR_INTERRUPTS();	/* no buffer lock held across the FSM check */
		if (GetRecordedFreeSpace(index, blk) < BLCKSZ / 2)
			continue;
		probed++;
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		ok = weave_page_recyclable(index, BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		if (ok)
			return true;
	}
	return false;
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
			return "pending_v8";
		case WEAVE_PK_PENDING_V9:
			return "pending_v9";
		case WEAVE_PK_PENDING_V10:
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
			return "cgram_root";
		case WEAVE_PK_CGRAM_DICT:
			return "cgram_dictionary";
		case WEAVE_PK_CGRAM_DICTINDEX:
			return "cgram_dict_index";
		case WEAVE_PK_CGRAM_POST:
			return "cgram_postings";
		case WEAVE_PK_VDIR:
			return "vector_dir";
		case WEAVE_PK_VWARP:
			return "vector_warp";
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
		end = weave_page_entry_end(page);
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
			res->docid = (uint64 *) palloc(res->cap * sizeof(uint64));	/* alloc-ok: fixed size -- WEAVE_BLOCK_SIZE entries and one BLCKSZ page, not corpus-scale */
			res->byte = (uint8 *) palloc(res->cap * sizeof(uint8));	/* alloc-ok: fixed size -- WEAVE_BLOCK_SIZE entries and one BLCKSZ page, not corpus-scale */
			res->pagecap = BLCKSZ;
			res->page = (unsigned char *) palloc(res->pagecap);	/* alloc-ok: fixed size -- WEAVE_BLOCK_SIZE entries and one BLCKSZ page, not corpus-scale */
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
	char	   *end = weave_page_entry_end((Page) r->page);
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

	end = weave_page_entry_end((Page) r->page);
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
	 * never trust it to be sane either (a torn header can make it small).  The
	 * one validated reader of pd_lower in this access method is
	 * weave_page_entry_end(); `make check-pdlower` keeps it the only one. */
	avail = (Size) (weave_page_entry_end(page) -
					(char *) PageGetContents(page));

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
						   BlockNumber surfroot, BlockNumber vecroot,
						   BlockNumber cgramroot, WeaveChannelDesc *weft)
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
	/*
	 * v8: the VECTOR weft, rooted at its WEAVE_VMETA page.
	 *
	 * EMITTED HERE, BETWEEN LEXICAL AND FUZZY, NOT APPENDED.
	 * weave_chandesc_check() requires the array to be strictly ascending by
	 * (kind, attnum), and WEAVE_WK_VECTOR is 2 while WEAVE_WK_FUZZY is 3.
	 * Appending it after the fuzzy entry produces a descriptor page the validator
	 * rejects with WEAVE_CD_ORDER -- which means the bolt cannot be read at all,
	 * so its wefts cannot be freed either and every merge leaks the lot.  The
	 * order is a correctness property of the page, not a formatting choice, and
	 * the reason the check is strict rather than tolerant is that it lets a reader
	 * binary-search for a kind and lets weave_chandesc_check() detect two
	 * descriptors for the same weft in one pass.
	 *
	 * attnum is the INDEX attribute the weft indexes, from weave_index_layout():
	 * unlike the lexical weft's hard-coded 1, this one genuinely varies --
	 * sql/vecindex.sql builds the vector column first, so it is attribute 1 there
	 * and attribute 2 in the documented column order.
	 */
	if (vecroot != InvalidBlockNumber)
	{
		WeaveIndexLayout layout;

		weave_index_layout(index, &layout);
		Assert(layout.vecattno != 0);
		weft[n].kind = (uint16) WEAVE_WK_VECTOR;
		weft[n].attnum = (uint16) layout.vecattno;
		weft[n].flags = 0;
		weft[n].root = vecroot;
		n++;
	}

	if (surfroot != InvalidBlockNumber)
	{
		weft[n].kind = (uint16) WEAVE_WK_FUZZY;
		weft[n].attnum = 1;
		weft[n].flags = 0;
		weft[n].root = surfroot;
		n++;
	}

	/*
	 * Z8: the CGRAM weft, rooted at its WEAVE_PK_CGRAM page.
	 *
	 * LAST, and that is not an accident either: weave_chandesc_check() requires
	 * strictly ascending (kind, attnum), and WEAVE_WK_CGRAM is 5, above FUZZY's
	 * 3.  It happens to be appendable today; the next weft with a kind below 5
	 * will have to be INSERTED, and the consequence of getting it wrong is the
	 * one the VECTOR comment above spells out -- a descriptor page the validator
	 * rejects, hence a bolt whose wefts can never be freed.
	 *
	 * attnum genuinely varies: the gram_ops column may be listed before or after
	 * the wdoc, so it comes from weave_index_layout() rather than a constant.
	 */
	if (cgramroot != InvalidBlockNumber)
	{
		WeaveIndexLayout layout;

		weave_index_layout(index, &layout);
		Assert(layout.cgramattno != 0);
		weft[n].kind = (uint16) WEAVE_WK_CGRAM;
		weft[n].attnum = (uint16) layout.cgramattno;
		weft[n].flags = 0;
		weft[n].root = cgramroot;
		n++;
	}
	return n;
}

/* Attach a descriptor page to a just-written bolt.  Call AFTER every other
 * chain of the bolt has been written, so each root is known. */
void
weave_attach_chandesc(Relation index, WeaveSegMeta *seg, BlockNumber surfroot,
					  BlockNumber vecroot, BlockNumber cgramroot)
{
	WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
	int			nweft = weave_chandesc_for_segment(index, seg, surfroot, vecroot,
												   cgramroot, weft);

	seg->chandesc = weave_write_chandesc(index, weft, nweft);
}

/* ---------------------------------------------------------------------------
 * The cgram weft's root page (task Z8)
 *
 * One page, four block numbers and a count.  See include/weave/cgram.h for why
 * each field is there, and include/weave/am.h for why these live in am.c.
 * ------------------------------------------------------------------------- */

BlockNumber
weave_write_cgram_root(Relation index, BlockNumber dictstart,
					   BlockNumber dictindexstart, BlockNumber postingstart,
					   uint32 nterms)
{
	Buffer		buf = weave_new_buffer(index);
	BlockNumber blk = BufferGetBlockNumber(buf);
	GenericXLogState *state = GenericXLogStart(index);
	Page		page = GenericXLogRegisterBuffer(state, buf,
												 GENERIC_XLOG_FULL_IMAGE);
	WeaveCgramPageData *cp;

	/* 100% GenericXLog (AGENTS.md hard rule 2).  FULL_IMAGE because the page is
	 * brand new, so a delta against the pre-image would be the whole page. */
	weave_init_page(page, WEAVE_PK_CGRAM);
	cp = (WeaveCgramPageData *) PageGetContents(page);
	cp->magic = WEAVE_CGRAM_MAGIC;
	cp->version = WEAVE_CGRAM_VERSION;
	cp->reserved = 0;
	cp->dictstart = (uint32) dictstart;
	cp->dictindexstart = (uint32) dictindexstart;
	cp->postingstart = (uint32) postingstart;
	cp->nterms = nterms;
	/* pd_lower must cover exactly the bytes written, because every reader of
	 * this page bounds itself with weave_page_entry_end() -- which is also the
	 * only sanctioned way to look at pd_lower (make check-pdlower). */
	((PageHeader) page)->pd_lower =
		((char *) PageGetContents(page) - (char *) page) +
		WEAVE_CGRAM_PAGEDATA_SIZE;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	return blk;
}

bool
weave_cgram_weft_open(Relation index, BlockNumber root, WeaveCgramWeft *out,
					  const char **why)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	Buffer		buf;
	Page		page;
	const WeaveCgramPageData *cp;
	char	   *contents;
	char	   *end;

	*why = NULL;
	memset(out, 0, sizeof(*out));
	out->root = InvalidBlockNumber;
	out->dictstart = InvalidBlockNumber;
	out->dictindexstart = InvalidBlockNumber;
	out->postingstart = InvalidBlockNumber;

	if (root == InvalidBlockNumber || root == WEAVE_METAPAGE_BLKNO ||
		root >= nblocks)
	{
		*why = "cgram weft root block is invalid or out of relation bounds";
		return false;
	}

	buf = ReadBuffer(index, root);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page))
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft root block is uninitialized";
		return false;
	}

	/*
	 * WeavePageHasKind(), never `flags & something`: WEAVE_PK_CGRAM is an
	 * INTEGER id under the escape bit, so a bitwise AND compiles and is always
	 * false.  That is the L17 class of bug and it has been made twice in this
	 * tree already (see weave_surf_walk).
	 */
	if (!WeavePageHasKind(page, WEAVE_PK_CGRAM))
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft root block is not a cgram page";
		return false;
	}

	/*
	 * The fits-guard every other reader in this file uses.  These bytes come off
	 * disk on a page held under a share lock, and a concurrent merge can free
	 * this page while a concurrent insert recycles it, so the readable extent is
	 * pd_lower via weave_page_entry_end() and nothing else.
	 */
	contents = (char *) PageGetContents(page);
	end = weave_page_entry_end(page);
	if (end < contents || (Size) (end - contents) < WEAVE_CGRAM_PAGEDATA_SIZE)
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft root page is too short for its header";
		return false;
	}

	cp = (const WeaveCgramPageData *) contents;
	if (cp->magic != WEAVE_CGRAM_MAGIC)
	{
		UnlockReleaseBuffer(buf);
		*why = "bad cgram weft magic";
		return false;
	}
	if (cp->version != WEAVE_CGRAM_VERSION)
	{
		UnlockReleaseBuffer(buf);
		*why = "unsupported cgram weft version";
		return false;
	}
	if (cp->reserved != 0)
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft reserved word is not zero";
		return false;
	}
	if (cp->nterms == 0 || cp->nterms > WEAVE_CGRAM_MAX_TERMS)
	{
		/* Zero is a corrupt weft and not an empty one: a weft with no trigrams
		 * is not WRITTEN at all (absent is safe), so a root page claiming zero
		 * is a page that should not exist. */
		UnlockReleaseBuffer(buf);
		*why = "cgram weft term count is zero or above the 2^24 trigram bound";
		return false;
	}
	if ((BlockNumber) cp->dictstart == InvalidBlockNumber ||
		cp->dictstart == 0 || (BlockNumber) cp->dictstart >= nblocks)
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft dictionary start is invalid or out of bounds";
		return false;
	}
	if ((BlockNumber) cp->dictindexstart != InvalidBlockNumber &&
		((BlockNumber) cp->dictindexstart >= nblocks || cp->dictindexstart == 0))
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft block-index start is out of bounds";
		return false;
	}
	if ((BlockNumber) cp->postingstart != InvalidBlockNumber &&
		((BlockNumber) cp->postingstart >= nblocks || cp->postingstart == 0))
	{
		UnlockReleaseBuffer(buf);
		*why = "cgram weft posting start is out of bounds";
		return false;
	}

	out->root = root;
	out->dictstart = (BlockNumber) cp->dictstart;
	out->dictindexstart = (BlockNumber) cp->dictindexstart;
	out->postingstart = (BlockNumber) cp->postingstart;
	out->nterms = cp->nterms;
	UnlockReleaseBuffer(buf);
	return true;
}

BlockNumber
weave_cgram_weft_root(Relation index, const WeaveSegMeta *seg)
{
	WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
	int			nweft;
	int			i;
	BlockNumber root = InvalidBlockNumber;

	if (seg->chandesc == InvalidBlockNumber)
		return InvalidBlockNumber;	/* pre-v6 bolt: lexical only, by definition */

	nweft = weave_chandesc_required(index, seg->chandesc, weft, WEAVE_MAX_WEFTS);
	for (i = 0; i < nweft; i++)
		if (weft[i].kind == (uint16) WEAVE_WK_CGRAM)
			root = weft[i].root;
	return root;
}

void
weave_cgram_free_weft(Relation index, BlockNumber root)
{
	WeaveCgramWeft w;
	const char *why = NULL;

	if (root == InvalidBlockNumber)
		return;

	/*
	 * Free the three chains FIRST and the root LAST, so a throw part-way leaves
	 * the root still naming what is left rather than orphaning it.  If the root
	 * does not open, free only the root: the alternative is guessing block
	 * numbers out of a page that just failed validation, and freeing live pages
	 * from a corrupt read is the worst outcome in this file.
	 * weave_check(deep)'s pages_reachable_or_freed then reports what was left.
	 */
	if (weave_cgram_weft_open(index, root, &w, &why))
	{
		weave_free_chain(index, w.dictstart);
		if (w.dictindexstart != InvalidBlockNumber)
			weave_free_chain(index, w.dictindexstart);
		if (w.postingstart != InvalidBlockNumber)
			weave_free_chain(index, w.postingstart);
	}
	weave_free_page(index, root);
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

		avail = (Size) (weave_page_entry_end(page) -
						(char *) PageGetContents(page));
		if (avail > (Size) WEAVE_SURFPAGE_PAYLOAD)
			avail = (Size) WEAVE_SURFPAGE_PAYLOAD;	/* a torn page header cannot make
												 * us read into the opaque area */
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

/*
 * Which block is this bolt's fuzzy weft rooted at?  InvalidBlockNumber when the
 * bolt has no fuzzy weft at all (a pre-v6 bolt, or a v7 bolt whose vocabulary the
 * format cannot represent) -- not an error, just nothing to consult.
 *
 * Split out of weave_surf_load() because the resident cache (below) needs the
 * root as part of its KEY, i.e. before it decides whether to read anything.
 */
static BlockNumber
weave_surf_root(Relation index, const WeaveSegMeta *seg)
{
	WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
	int			nweft;
	int			i;
	BlockNumber root = InvalidBlockNumber;

	if (seg->chandesc == InvalidBlockNumber)
		return InvalidBlockNumber;	/* pre-v6 bolt: lexical only, by definition */

	nweft = weave_chandesc_required(index, seg->chandesc, weft, WEAVE_MAX_WEFTS);
	for (i = 0; i < nweft; i++)
		if (weft[i].kind == (uint16) WEAVE_WK_FUZZY)
			root = weft[i].root;
	return root;
}

/*
 * Read + open + validate the image on the chain at `root`.  Split from
 * weave_surf_load() so the resident cache can load an image whose root it has
 * already resolved for its key, instead of walking the channel descriptor twice.
 */
static void
weave_surf_load_at(Relation index, BlockNumber root, WeaveSurfTrie *t,
				   uint8 **img, Size *len)
{
	const char *detail = NULL;
	WeaveSurfError err;

	*img = weave_read_surf(index, root, len, &detail);
	/* Counted here and not at the callers, because this is the ONE place the
	 * whole-image cost is paid: weave_read_surf() reassembles the entire trie
	 * into one contiguous palloc, ~5.52 B/term.  A cache HIT must not touch
	 * these two -- "the trie is resident" is precisely the claim that consults
	 * outnumber loads, and it is unreadable if a hit counts as a load. */
	weave_chan_surf_loads++;
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
	weave_chan_surf_bytes += (uint64) *len;
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
}

bool
weave_surf_load(Relation index, const WeaveSegMeta *seg, WeaveSurfTrie *t,
				uint8 **img, Size *len)
{
	BlockNumber root;

	*img = NULL;
	*len = 0;
	root = weave_surf_root(index, seg);
	if (root == InvalidBlockNumber)
		return false;

	weave_surf_load_at(index, root, t, img, len);
	return true;
}

/* ---------------------------------------------------------------------------
 * THE RESIDENT SuRF TRIE CACHE (task Z4 part 2)
 *
 * WHY.  weave_surf_load() above reassembles the whole image every call -- about
 * 5.52 bytes per vocabulary term, ~11 MB per bolt at a 2M-term vocabulary -- and
 * until this cache existed nothing reused it.  Nothing in production paid that
 * per query yet, because the only SQL-reachable caller was the weave_surf_stats()
 * diagnostic; the point is that EVERY remaining Z-phase task (Z4 part 3's prefix
 * routing, Z5's trie-accelerated fuzzy, Z6's regex tiling) has to consult the trie
 * at QUERY time, and none of them is measurable while one consult costs a whole
 * image.  Making the image resident is the prerequisite, not an optimization.
 *
 * WHY (relfilenode, root, generation) IS A SAFE KEY.  A bolt is immutable once
 * written, so an image never changes in place -- but freed pages ARE recycled, so
 * a root block number can later belong to a DIFFERENT bolt.  What rules that out
 * is that every path which frees a bolt's pages bumps the metapage `generation`
 * first, under the metapage's exclusive lock, in the same GenericXLog record that
 * removes the segment from the directory: weave_meta_add_segment() above,
 * ambuild.c's two merge commit points, and amvacuum.c's livedocs rewrite.  So
 * within one generation, (relfilenode, root) -> image bytes is a function, and a
 * generation change makes every entry for that relation unusable by key mismatch
 * rather than by anybody remembering to invalidate it.  relfilenode rather than
 * relation OID because REINDEX keeps the OID and changes the file.
 *
 * WHAT A STALE SNAPSHOT GETS.  The caller passes the generation of the metapage
 * snapshot its `seg` came from, so if that snapshot is stale the entry served is
 * the image that WAS at that root at that generation -- which is exactly what an
 * uncached read of a stale snapshot was trying to get and is strictly better than
 * what it would actually have got (whatever now lives on those recycled pages).
 * The scan's own generation re-check is what discards a stale result;
 * see weave_read_meta_generation() in src/am/amscan.c.
 *
 * MEMORY.  One dedicated MemoryContext child of TopMemoryContext, created lazily,
 * so the resident bytes are visible in a memory-context dump under their own name
 * instead of hiding inside CacheMemoryContext.
 *
 * WHO FREES WHAT, and this is the load-bearing rule (see the LIFETIME RULE in
 * include/weave/am.h): the trie a consult hands back points INTO an entry's image,
 * so an entry must not be freed while a caller might still be reading it.  Entries
 * are therefore freed ONLY from inside weave_surf_consult().  The relcache callback
 * merely MARKS entries dead -- an invalidation message can arrive at any
 * CHECK_FOR_INTERRUPTS, and a callback that pfree'd would be a use-after-free with
 * no call site to audit -- and the GUC has no assign hook, because lowering the
 * budget mid-query would otherwise free an image the running query is walking.  The
 * next consult enforces the new budget, which is the first moment at which no
 * caller can be holding anything.
 * ------------------------------------------------------------------------- */

/*
 * Cap on the number of resident images, independent of the byte budget, because
 * the entry list is searched LINEARLY.  128 = WEAVE_MAX_SEGMENTS: one image per
 * bolt of one index is the shape a query actually consults, and at that size a
 * hash table would be more code than the thing it indexes.  Without the cap a
 * small-vocabulary index under a large budget could accumulate thousands of
 * entries and turn every lookup into a scan of them.
 */
#define WEAVE_SURF_CACHE_MAX_IMAGES WEAVE_MAX_SEGMENTS

typedef struct WeaveSurfCacheEntry
{
	struct WeaveSurfCacheEntry *next;
	RelFileNumber relnumber;	/* key part 1 (REINDEX changes it, OID does not) */
	BlockNumber root;			/* key part 2: the fuzzy weft's root block */
	uint32		generation;		/* key part 3: the directory generation */
	Oid			reloid;			/* NOT part of the key: the relcache callback's
								 * only handle on which entries to mark */
	bool		dead;			/* invalidated; freed at the next consult */
	uint64		used;			/* LRU clock stamp */
	Size		len;
	uint8	   *img;			/* the image, in weave_surf_cache_cxt */
	WeaveSurfTrie trie;			/* opened against img, which it only points into */
} WeaveSurfCacheEntry;

static MemoryContext weave_surf_cache_cxt = NULL;
static WeaveSurfCacheEntry *weave_surf_cache_head = NULL;
static int	weave_surf_cache_nimages = 0;
static uint64 weave_surf_cache_clock = 0;

/*
 * DROP/REINDEX/anything that invalidates the relcache entry: mark, never free.
 * Generation-keying is the correctness mechanism; this is hygiene, so that a
 * dropped relation's image does not sit resident for the life of the backend.
 */
static void
weave_surf_cache_inval(Datum arg, Oid relid)
{
	WeaveSurfCacheEntry *e;

	for (e = weave_surf_cache_head; e != NULL; e = e->next)
		if (!OidIsValid(relid) || e->reloid == relid)
			e->dead = true;
}

/* Unlink and free *pp (which points at the head, or at a predecessor's next). */
static void
weave_surf_cache_drop(WeaveSurfCacheEntry **pp)
{
	WeaveSurfCacheEntry *e = *pp;

	*pp = e->next;
	Assert(weave_chan_surf_cache_bytes >= (uint64) e->len);
	weave_chan_surf_cache_bytes -= (uint64) e->len;
	weave_surf_cache_nimages--;
	pfree(e->img);
	pfree(e);
}

/*
 * Make the cache fit `budget` bytes with `extra` more about to be added, and free
 * anything already known to be unusable.  The ONE place entries are freed.
 */
static void
weave_surf_cache_enforce(Size budget, Size extra)
{
	WeaveSurfCacheEntry **pp;

	/* A dead entry can never be served again, so it is the cheapest thing to
	 * give up and it goes first -- it is not counted as an eviction, because it
	 * was not the budget that cost us the image. */
	pp = &weave_surf_cache_head;
	while (*pp != NULL)
	{
		if ((*pp)->dead)
			weave_surf_cache_drop(pp);
		else
			pp = &(*pp)->next;
	}

	while (weave_surf_cache_head != NULL &&
		   (weave_chan_surf_cache_bytes + (uint64) extra > (uint64) budget ||
			(extra > 0 &&
			 weave_surf_cache_nimages >= WEAVE_SURF_CACHE_MAX_IMAGES)))
	{
		WeaveSurfCacheEntry **victim = &weave_surf_cache_head;
		uint64		oldest = PG_UINT64_MAX;

		for (pp = &weave_surf_cache_head; *pp != NULL; pp = &(*pp)->next)
			if ((*pp)->used < oldest)
			{
				oldest = (*pp)->used;
				victim = pp;
			}
		weave_surf_cache_drop(victim);
		weave_chan_surf_cache_evicts++;
	}
}

bool
weave_surf_consult(Relation index, const WeaveSegMeta *seg, uint32 generation,
				   WeaveSurfTrie *t, Size *len, uint8 **owned)
{
	Size		budget = (Size) pg_weave_surf_cache_mb * (Size) 1024 * 1024;
	RelFileNumber relnumber = index->rd_locator.relNumber;
	BlockNumber root;
	WeaveSurfCacheEntry **pp;
	WeaveSurfCacheEntry *e;
	uint8	   *img = NULL;
	WeaveSurfError err;
	MemoryContext oldcxt;

	*owned = NULL;
	*len = 0;

	/* Here, and only here: see WHO FREES WHAT above.  Runs before the key is even
	 * resolved so that setting the budget to 0 gives the memory back at the next
	 * consult rather than at the next hit. */
	if (weave_surf_cache_head != NULL)
		weave_surf_cache_enforce(budget, 0);

	root = weave_surf_root(index, seg);
	if (root == InvalidBlockNumber)
		return false;			/* no fuzzy weft: nothing to consult */

	if (budget == 0)
	{
		/* Cache disabled: the pre-Z4-part-2 path exactly -- one whole-image load
		 * per consult, which is what makes surf_cache_mb=0 usable as the control
		 * arm of a measurement rather than merely a slower cache. */
		weave_surf_load_at(index, root, t, &img, len);
		*owned = img;
		return true;
	}

	for (pp = &weave_surf_cache_head; *pp != NULL; pp = &(*pp)->next)
	{
		e = *pp;
		if (e->relnumber == relnumber && e->root == root && !e->dead)
		{
			if (e->generation == generation)
			{
				e->used = ++weave_surf_cache_clock;
				*t = e->trie;
				*len = e->len;
				weave_chan_surf_cache_hits++;
				return true;	/* the whole point: no load, no surf_bytes */
			}

			/* Same root, older generation: that bolt is provably gone (the
			 * generation bump happens in the commit that frees its pages), so
			 * the image can never be served again.  Dropping it here is what
			 * keeps an insert-heavy session from accumulating stale images until
			 * the budget notices. */
			e->dead = true;
		}
	}

	weave_chan_surf_cache_misses++;
	weave_surf_load_at(index, root, t, &img, len);

	if (*len > budget)
	{
		/* Bigger than the entire budget: caching it would evict everything to
		 * hold something that cannot be kept, so use it and let the caller free
		 * it.  Recognizable from SQL as misses climbing with surf_cache_bytes
		 * pinned at zero. */
		*owned = img;
		return true;
	}

	weave_surf_cache_enforce(budget, *len);

	if (weave_surf_cache_cxt == NULL)
	{
		weave_surf_cache_cxt = AllocSetContextCreate(TopMemoryContext,
													"pg_weave surf trie cache",
													ALLOCSET_DEFAULT_SIZES);
		/* Registered with the cache rather than from _PG_init: there is nothing
		 * to invalidate before the first entry exists, and this keeps the
		 * registration next to the thing it protects. */
		CacheRegisterRelcacheCallback(weave_surf_cache_inval, (Datum) 0);
	}

	oldcxt = MemoryContextSwitchTo(weave_surf_cache_cxt);
	e = (WeaveSurfCacheEntry *) palloc0(sizeof(WeaveSurfCacheEntry));
	/* A COPY, not a reparent of the loaded chunk.  weave_surf_load() allocates in
	 * the caller's context (and through the huge-safe path), and stealing that
	 * chunk would mean either switching contexts around a function that can
	 * ereport or reaching into MemoryContext internals.  The cost is a transient
	 * second copy of at most `budget` bytes, paid once per miss. */
	e->img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE(*len);
	MemoryContextSwitchTo(oldcxt);
	memcpy(e->img, img, *len);

	/* open() only, no validate(): the bytes are a memcpy of an image
	 * weave_surf_load() just validated, so re-deriving the semantic invariants
	 * would be asserting that memcpy works.  open() must be re-run because
	 * WeaveSurfTrie is all pointers INTO the image and they have to point at the
	 * copy. */
	err = weave_surftrie_open(e->img, *len, &e->trie);
	if (err != WEAVE_SURF_OK)
	{
		/* Unreachable unless open() is byte-position-dependent, which it is not.
		 * If it ever happens the thing to give up is the CACHE, not the query. */
		pfree(e->img);
		pfree(e);
		*owned = img;
		return true;
	}

	e->relnumber = relnumber;
	e->root = root;
	e->generation = generation;
	e->reloid = RelationGetRelid(index);
	e->len = *len;
	e->used = ++weave_surf_cache_clock;
	e->next = weave_surf_cache_head;
	weave_surf_cache_head = e;
	weave_surf_cache_nimages++;
	weave_chan_surf_cache_bytes += (uint64) e->len;

	pfree(img);					/* the caller's copy is now redundant */
	*t = e->trie;
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
 *
 * THE MERGES RUN UNDER THE MAINTENANCE MUTEX, BLOCKING.  Until now this was the
 * one merger in the tree that took no mutex at all, so a full directory on the
 * insert path could merge CONCURRENTLY with autovacuum's cleanup merge on the
 * same index.  Today's exposure is bounded and should not be overstated: both
 * mergers allocate extend-only, so they are never handed the same block, and
 * weave_merge_selected() re-verifies its inputs against the live directory under
 * the metapage buffer lock and abandons if another merge consumed them.  The cost
 * today is therefore a wasted merge plus a leaked output segment -- not a
 * deadlock and not corruption.  It becomes a deadlock the moment a merge is
 * allowed to reuse freed pages, because then both mergers draw from one free list
 * and hand out the same block (the sibling project observed exactly that: an
 * INSERT and an autovacuum worker both parked on the same buffer's content lock).
 * Taking the mutex before that change lands is the whole point of the ordering.
 *
 * BLOCKING, NOT CONDITIONAL, and the latency cost is real but narrow.  An INSERT
 * that reaches here may now wait for another backend's merge to finish, and on a
 * large index that merge is measured in seconds.  Conditional would be worse than
 * useless: skipping the merge leaves the directory full, so the retry loop would
 * spin WEAVE_MAX_SEGMENTS times changing nothing and then ereport -- i.e. it would
 * convert a wait into the failed INSERT this function exists to prevent.  What
 * bounds the cost is that only a FULL directory arrives here (the ordinary
 * insert-time compaction is the conditional one in weave_insert_oversized_as_
 * segment), and a full directory is already a degraded state.  Acquired per retry
 * rather than around the loop so a concurrent backend's segment adds and the
 * weave_meta_add_segment below still interleave.
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
		weave_maintenance_lock(index);
		PG_TRY();
		{
			if ((try & 1) == 0)
				weave_merge_segments(index);
			else
				weave_merge_all(index, false);
		}
		PG_FINALLY();
		{
			weave_maintenance_unlock(index);
		}
		PG_END_TRY();
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
	op = WeavePageGetOpaque(page);

	/*
	 * LIVENESS FIRST, before any lock-based bypass.  A page without WEAVE_FREED
	 * is a LIVE page -- part of some segment's chain -- and must never be handed
	 * out, whatever the free-space records say about it.  The FSM is a free-SPACE
	 * hint, not a liveness oracle, and there are two ways a live page reaches a
	 * candidate list: an entry in a gathered low-free/snapshot list that another
	 * backend has since taken and filled (the snapshot is held for the length of a
	 * whole merge loop, and an insert in another backend allocates from the live
	 * FSM throughout), and GetPageWithFreeSpace()'s approximate answer handing the
	 * same block to two backends at once.  Until now this function returned TRUE
	 * for exactly that case ("older free, or in-use race") -- so the second taker
	 * would initialize and write a page the first taker was already using.
	 *
	 * Upstream hit the consequence in the field rather than in review: a live
	 * mid-chain posting page handed out as merge output while the same merge held
	 * it pinned as input, self-deadlocking on its own buffer (the sibling
	 * project, 1.8.3).
	 *
	 * Cost of the stricter rule: a page freed by a build older than the flag is no
	 * longer reusable through the free list.  It is still reclaimed by
	 * weave_truncate_free_tail() when it sits in the tail, which is where old
	 * frees accumulate.  Safety over that corner.
	 */
	if ((op->flags & WEAVE_FREED) == 0)
		return false;

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
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			/* Unguarded, a garbage termlen oversteps the page and a garbage
			 * de->firstposting is handed to weave_free_chain() below -- which
			 * would walk an arbitrary block chain marking pages free.  Freeing
			 * live pages from a corrupt read is the worst outcome in this file. */
			if (!weave_dict_entry_fits(de, end))
				break;
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

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
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr + MAXALIGN(sizeof(WeaveTrgmEntry)) <= end)
		{
			WeaveTrgmEntry *te = (WeaveTrgmEntry *) ptr;

			/* fixed stride, so only the header-fits test is needed -- but it IS
			 * needed: te->firstdata goes straight to weave_free_chain(). */
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

				/*
				 * The VECTOR weft is THREE chains, not one: its descriptor root is
				 * the WEAVE_VMETA page, which NAMES the directory and code chains.
				 * weave_free_chain() on the root alone would reclaim one page and
				 * leak every strip and directory page behind it -- thousands of
				 * pages per merged bolt, unreachable and reclaimable by nothing
				 * short of a REINDEX.  This is exactly the failure mode
				 * doc/specs/SEGMENT_FORMAT.md sect. 6 describes for a weft the free
				 * path cannot see, and the reason the descriptor-driven loop is not
				 * by itself sufficient for a weft with internal structure.
				 */
				if (weft[i].kind == (uint16) WEAVE_WK_VECTOR)
				{
					weave_vec_free_weft(index, weft[i].root);
					continue;
				}

				/*
				 * The CGRAM weft is THREE chains behind one root page, exactly
				 * like the vector weft and for exactly the same reason.  Z8
				 * DELIBERATELY DOES NOT MERGE this weft (see the comment at its
				 * writer in ambuild.c), so every merge frees one -- which makes
				 * this arm the hottest of the three, not the coldest, and a leak
				 * here would compound on every merge rather than never.
				 */
				if (weft[i].kind == (uint16) WEAVE_WK_CGRAM)
				{
					weave_cgram_free_weft(index, weft[i].root);
					continue;
				}
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
		{"bits", RELOPT_TYPE_INT, offsetof(WeaveOptions, bits)},
		{"metric", RELOPT_TYPE_ENUM, offsetof(WeaveOptions, metric)},
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

/*
 * The vector code width this index's next weft is written at.
 *
 * Clamped rather than trusted: rd_options comes from the catalog, and while
 * add_int_reloption() validates the range on CREATE INDEX, a pg_class row written
 * by a future version must not reach weave_quantizer_init() with a width the codec
 * cannot pack.  The width actually used is recorded in the weft's WEAVE_VMETA page,
 * so changing this reloption never invalidates an existing segment -- a reader
 * takes the geometry from the page, never from here.
 */
int
weave_index_vec_bits(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;
	int			bits = opts ? opts->bits : WEAVE_VEC_DEFAULT_BITS;

	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		bits = WEAVE_VEC_DEFAULT_BITS;
	return bits;
}

/*
 * The metric this index's next vector weft is scored with.
 *
 * Refuses the two metrics the channel has no sound compressed-domain bound for.
 * That refusal is HERE and not in the reloption validator because it is a
 * statement about this channel's arithmetic rather than about the catalog: a
 * bound for cosine has to switch on the sign of its numerator and divide by a
 * maximum true norm, which is not stored, and L1 has no compressed-domain bound
 * at all.  Inventing either would put an unsound bound behind contract (C2),
 * which silently drops rows and which no fixed-expected-output test can catch
 * (AGENTS.md hard rule 1).
 *
 * An out-of-range value is clamped to the default rather than refused, for the
 * reason weave_index_vec_bits() gives: rd_options comes from a pg_class row a
 * future version may have written, and the value actually used is recorded in the
 * weft's WEAVE_VMETA page, so a reader never consults this.
 */
int
weave_index_vec_metric(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;
	int			metric = opts ? opts->metric : WEAVE_METRIC_L2;
	if (metric == WEAVE_METRIC_COSINE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the weave vector channel cannot index the cosine metric"),
				 errdetail("A sound block bound for cosine has to divide by a "
						   "maximum vector norm when its numerator is negative, and "
						   "no maximum true norm is stored."),
				 errhint("Normalize the vectors and use metric = ip, which is "
						 "equal to cosine similarity on unit vectors.")));
	if (metric == WEAVE_METRIC_L1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the weave vector channel cannot index the l1 metric"),
				 errdetail("The quantizer is built around inner products and L1 "
						   "admits no compressed-domain bound, so an L1 weft could "
						   "not satisfy the channel contract."),
				 errhint("Use metric = l2 or metric = ip.")));
	if (metric != WEAVE_METRIC_L2 && metric != WEAVE_METRIC_IP)
		metric = WEAVE_METRIC_L2;
	return metric;
}

/*
 * The same value, reported rather than refused.  See the header comment on the
 * declaration in weave/am.h for why the planner needs a non-throwing accessor,
 * and note that this is NOT merely the function above minus two ereports: it maps
 * an unserviceable metric to itself, so the caller can tell "l2" from "cosine,
 * which no fused path may carry" instead of being handed a default.
 */
int
weave_index_vec_metric_raw(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;

	return opts ? opts->metric : WEAVE_METRIC_L2;
}
/*
 * The operator-family -> weft-kind registry.  See WeaveIndexLayout in weave/am.h
 * for why the key is a family NAME rather than a type OID or an OID of any kind.
 * Add a channel by adding a row.
 *
 * The family and not the opclass, because the relcache caches `rd_opfamily[]` for
 * every index column and does NOT cache the opclass OIDs -- `pg_index.indclass` is
 * a varlena, reachable only by deforming the catalog tuple.  CREATE OPERATOR CLASS
 * creates an implicit family of the same name, so the names below are the opclass
 * names users write, and an explicit `FAMILY wvec_weave_ops` on some future opclass
 * gets vector routing, which is what sharing a family means.
 *
 * `wdoc_lex_ops` predates the registry and keeps its name; the newer ones follow
 * `<type>_weave_ops`, which is the pgvector-era convention for "this opclass is for
 * that access method" and disambiguates from the btree `wvec_ops` the type already
 * has.  Renaming the lexical one would break every existing index for no gain.
 */
static const struct
{
	const char *opfname;
	WeaveWeftKind kind;
}			weave_opfamily_kinds[] = {
	{"wdoc_lex_ops", WEAVE_WK_LEXICAL},
	{"wvec_weave_ops", WEAVE_WK_VECTOR},
	/*
	 * Z8.  `gram_ops` keeps the short name rather than becoming
	 * `text_weave_ops`: the family says WHICH CHANNEL, not which type, and text
	 * is exactly the type a future second text channel would also want.  It is
	 * also the name the WeaveIndexLayout block comment already uses as its
	 * worked example of why the opclass and not the column type is the
	 * discriminator.
	 */
	{"gram_ops", WEAVE_WK_CGRAM},
};

/*
 * The kind an operator-family name declares, or WEAVE_WK_INVALID if it is not ours.
 */
static WeaveWeftKind
weave_opfname_kind(const char *opfname)
{
	int			i;

	for (i = 0; i < (int) lengthof(weave_opfamily_kinds); i++)
		if (strcmp(opfname, weave_opfamily_kinds[i].opfname) == 0)
			return weave_opfamily_kinds[i].kind;
	return WEAVE_WK_INVALID;
}

/*
 * The kind declared by an operator family.  Throws if the family is not in the
 * registry, which is what keeps a misrouted attribute from being possible: every
 * caller either gets a kind or an error, never a default.
 */
static WeaveWeftKind
weave_opfamily_kind(Oid opfamilyoid)
{
	HeapTuple	tup;
	Form_pg_opfamily form;
	WeaveWeftKind kind;
	char		opfname[NAMEDATALEN];

	tup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfamilyoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for operator family %u", opfamilyoid);
	form = (Form_pg_opfamily) GETSTRUCT(tup);
	strlcpy(opfname, NameStr(form->opfname), sizeof(opfname));
	ReleaseSysCache(tup);

	kind = weave_opfname_kind(opfname);
	if (kind == WEAVE_WK_INVALID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator family \"%s\" is not a pg_weave channel family",
						opfname),
				 errdetail("The \"weave\" access method routes each index column to a retrieval channel by its operator class."),
				 errhint("Use wdoc_lex_ops for a wdoc column, wvec_weave_ops for a wvec column, or gram_ops for a text column.")));
	return kind;
}

/*
 * amvalidate.  The opclasses carry no support procedures (amsupport = 0) and their
 * operator members are checked by the generic catalog machinery, so the one thing
 * left to validate is the thing this AM actually depends on: that the opclass
 * belongs to a family that names a channel.  An opclass that does not is one whose
 * columns the access method would have no route for.
 */
static bool
weave_validate(Oid opclassoid)
{
	HeapTuple	tup;
	Oid			opfamilyoid;

	tup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	opfamilyoid = ((Form_pg_opclass) GETSTRUCT(tup))->opcfamily;
	ReleaseSysCache(tup);

	(void) weave_opfamily_kind(opfamilyoid);
	return true;
}

/*
 * Resolve the index's per-column channel routing.  See WeaveIndexLayout.
 *
 * Cheap enough to call per statement (one syscache probe per key column) but not
 * free, so the build path resolves it once into WeaveBuildState rather than per
 * heap tuple.
 */
void
weave_index_layout(Relation index, WeaveIndexLayout *out)
{
	int			i;

	memset(out, 0, sizeof(*out));
	out->nkeys = IndexRelationGetNumberOfKeyAttributes(index);
	Assert(out->nkeys > 0 && out->nkeys <= INDEX_MAX_KEYS);

	for (i = 0; i < out->nkeys; i++)
	{
		Oid			opfamilyoid = index->rd_opfamily[i];
		WeaveWeftKind kind = weave_opfamily_kind(opfamilyoid);

		out->kind[i] = (uint16) kind;
		switch (kind)
		{
			case WEAVE_WK_LEXICAL:
				if (out->lexattno != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("index \"%s\" has more than one lexical column",
									RelationGetRelationName(index)),
							 errdetail("Columns %d and %d both use a lexical operator class.",
									   out->lexattno, i + 1),
							 errhint("Build one weave index per document column.")));
				out->lexattno = (AttrNumber) (i + 1);
				break;
			case WEAVE_WK_VECTOR:
				if (out->vecattno != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("index \"%s\" has more than one vector column",
									RelationGetRelationName(index)),
							 errdetail("Columns %d and %d both use a vector operator class.",
									   out->vecattno, i + 1)));
				out->vecattno = (AttrNumber) (i + 1);
				break;
			case WEAVE_WK_CGRAM:
				/*
				 * One cgram column, for the same reason as the other two: the
				 * bolt's channel descriptor keys a weft by (kind, attnum) and
				 * the writer emits exactly one CGRAM descriptor, so a second
				 * gram_ops column would silently index only one of them.
				 * Refusing is the difference between an error and a column that
				 * looks indexed and answers nothing.
				 */
				if (out->cgramattno != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("index \"%s\" has more than one cgram column",
									RelationGetRelationName(index)),
							 errdetail("Columns %d and %d both use gram_ops.",
									   out->cgramattno, i + 1)));
				out->cgramattno = (AttrNumber) (i + 1);
				break;
			default:
				elog(ERROR, "unhandled weave weft kind %d for index \"%s\" column %d",
					 (int) kind, RelationGetRelationName(index), i + 1);
		}
	}

	/*
	 * A lexical column is required, and the reason is structural rather than a
	 * policy choice: the docid space every other channel indexes into is assigned
	 * by the lexical build (weave_build_callback assigns one docid per document
	 * and the segment writers lay the postings out in that order).  Until a vector
	 * weft can create a bolt on its own, a vector-only index would build an index
	 * with no documents in it and answer every query with zero rows -- a wrong
	 * answer, not a slow one.  Refuse it instead.
	 */
	if (out->lexattno == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a weave index requires a lexical column"),
				 errdetail("Every channel shares the document-id space that the lexical column's build assigns."),
				 errhint("Add a wdoc column with wdoc_lex_ops as the first index column.")));
}

Datum
weave_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	/*
	 * 5, and the fifth member is WEAVE_STRAT_FUSE_WEIGHTS (`<~>`, task F2.2):
	 * ALTER OPERATOR FAMILY validates a member number against this value, so a
	 * new ORDER BY member is unregisterable while this says one less than its
	 * number.  It has been raised twice for that reason: 3 -> 4 for `<#>` (one
	 * vector member per metric the scan core serves, task F7) and 4 -> 5 for the
	 * fused scan's weights transport key.  1..4 are `@@@`/`@~`/`<->` (three
	 * families sharing the number), WEAVE_STRAT_DISTANCE, WEAVE_STRAT_EDIST and
	 * WEAVE_STRAT_VEC_IP; see include/weave/am.h.
	 */
	amroutine->amstrategies = 5;
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

	/*
	 * amcanmulticol: TRUE since task V7.  A weave index carries one column per
	 * channel -- `USING weave (body wdoc_lex_ops, embedding wvec_weave_ops)` is one
	 * index, one WAL stream, one vacuum, which is claim 1 in doc/ARCHITECTURE.md
	 * sect. 9.  Flipping the flag is the small half; the routing is
	 * weave_index_layout(), because a column's ORDINAL says nothing about its
	 * channel (the vector column may be written first) and four write/recheck sites
	 * had `values[0]` compiled into them.
	 *
	 * Column order is not a capability question either way: no channel's on-disk
	 * structure is shared with another's, so unlike btree there is no leading-column
	 * prefix rule to respect.
	 */
	amroutine->amcanmulticol = true;
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
