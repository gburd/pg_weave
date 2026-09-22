/*-------------------------------------------------------------------------
 *
 * amscan.c
 *		Bitmap scan for the weave access method.
 *
 * A separate translation unit since task L1; it used to be #included into
 * src/am/am.c, which is why it shared that file's static page helpers -- those
 * are now declared in include/weave/am.h.  It
 * evaluates an wquery by set algebra over posting lists (a term yields the
 * TIDs whose document contains it; AND intersects, OR unions, NOT complements
 * against the indexed universe) for the bitmap and index-only scans, and runs
 * block-max WAND / MaxScore top-k for the <=> ordering scan.  Fuzzy and regex
 * are decided on the dictionary (a Levenshtein automaton, resp. core's regex
 * engine narrowed by the trigram weft); counts use a visibility-map-aware
 * bulk path.  Results are exact against @@@ semantics; the boolean and ranked
 * paths need no heap access beyond MVCC visibility.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/am/amscan.c
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

#include "access/heapam.h"	/* heap_get_root_tuples: an AM must return HOT-chain
								 * ROOT tids, see weave_cgram_heapscan() */
#include "weave/weave.h"
#include "weave/am.h"
#include "weave/sparsemap.h"			/* namespaced sparsemap (tombstones, trigrams) */
#include "weave/edist.h"			/* Z9: the <@> edit-distance shuttle */
#include "weave/bm25bound.h"		/* F6: the single copy of the BM25 contribution
									 * and its per-block (C2) bound; this file used
									 * to carry a transcription of both */
#include "weave/vector.h"			/* F7: the shared vector top-k the <-> / <#> ordering scan drives */
#include "weave/channel.h"			/* F2.2: the shuttle contract the fused pass drives */
#include "weave/fuse.h"				/* F2.2: the fused-threshold top-k core */
#include "weave/vecdocmap.h"		/* F8: the vector channel's docid-space adapter */
#include "weave/gate.h"				/* F2.2: the boolean gate shuttle a WHERE qual becomes */
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
#include "mb/pg_wchar.h"		/* pg_database_encoding_max_length: the fuzzy edit unit */
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
#include "weave/for.h"			/* FOR codec + doclen quantizer (the WAND cursor) */
#include "weave/uleven.h"		/* the universal-Levenshtein core the fuzzy walk runs */
#include "weave/regex_ast.h"	/* pg_tre parser + trigram extractor (the regex walk's narrowing) */
#include "catalog/pg_collation.h"	/* C_COLLATION_OID: the regex walk's collation */
#include "regex/regex.h"		/* pg_regcomp/pg_regexec: the regex walk's engine */

/* forward decls: defined later in this file */
static void weave_collect_matches(Relation index, WeaveQuery query, TidSet *out, bool *recheck);
static void weave_recheck_exact(Relation index, WeaveQuery query, TidSet *set);
static double weave_query_maxhits(Relation index, WeaveQuery q, double N);

/*
 * EOF-tolerant page read for the scan's directory-following chain walks.
 *
 * A scan reads the segment directory (metapage + per-segment chain heads) under
 * a transient SHARE lock, releases it, then follows dict/posting chains by
 * block number, re-checking the metapage `generation` afterward and retrying if
 * it moved (the A1 race).  But a concurrent weave_vacuum can TRUNCATE the index
 * tail; a block number from the pre-truncation snapshot then points past EOF and
 * ReadBuffer raises a hard "could not read blocks N: read only 0 of 8192" ERROR
 * before the generation re-check can discard the stale result.  So a chain walk
 * must treat an out-of-range block as END OF CHAIN: return InvalidBuffer, let
 * the walk stop, and let the caller's generation guard restart from a fresh
 * (post-truncation) directory.  RelationGetNumberOfBlocks reads the smgr size
 * cache (refreshed on the truncation's relcache invalidation), so this is cheap.
 */
static inline Buffer
weave_scan_readbuf(Relation index, BlockNumber blk)
{
	if (blk == InvalidBlockNumber || blk >= RelationGetNumberOfBlocks(index))
		return InvalidBuffer;
	return ReadBuffer(index, blk);
}

/* Max terms in a phrase chain we evaluate positionally, and the per-docid
 * position scratch bound.  A phrase with more terms, or a per-(term,doc) tf
 * beyond WEAVE_PHRASE_POSBUF, falls back to the (correct) recheck path.  16383
 * matches the analyzer's MAXENTRYPOS cap, so a well-formed posting never
 * exceeds it. */
#define WEAVE_QUERY_MAX_PHRASE_TERMS 32
#define WEAVE_PHRASE_POSBUF 16384

/* weave_dict_entry_fits() and weave_dictindex_entry_fits() moved to
 * include/weave/am.h: the same guard is required by the merge, vacuum and
 * trigram walks, and while it was static here those four files each walked
 * dictionary entries with no bounds check at all. */

/* A scored heap tuple (score, or distance in an ordering scan). */
typedef struct ScoredTid
{
	ItemPointerData tid;
	double		score;
}			ScoredTid;

static int weave_topk_visible(Relation index, WeaveQuery q, int k,
							 bool as_distance, ScoredTid **out);
static int weave_topk_candidates_range(Relation index, WeaveQuery q, int wantk,
									  uint64 docid_lo, uint64 docid_hi,
									  ScoredTid **out);

typedef struct WeaveScanOpaqueData
{
	WeaveQuery	query;			/* copied into the scan's context */
	bool		queryValid;
	/* ordering-scan (amgettuple) state, materialized on first call.
	 *
	 * Two arrays, and the split between them is what makes a widening
	 * incremental (task L14):
	 *
	 *  - cand[]    the ranked candidates of the CURRENT WAND pass, descending
	 *              score, not yet visibility-checked.  candpos is how far the
	 *              MVCC probe has walked.
	 *  - ordered[] the visible rows already materialized, ascending distance.
	 *              ACCUMULATED ACROSS PASSES: a widening keeps this array,
	 *              re-probes none of it, and filters the new pass's candidates
	 *              against it, so a recompute extends the previous pass instead
	 *              of repeating its output. */
	bool		orderInit;		/* has the first pass run? */
	ScoredTid  *ordered;		/* visible results so far, ascending distance */
	int			nordered;		/* how many are materialized */
	int			maxordered;		/* allocated slots in ordered[] */
	int			ordpos;			/* next result to return */
	ScoredTid  *cand;			/* current pass's candidates, descending score */
	int			ncand;
	int			candpos;		/* next candidate to visibility-probe */
	bool		candfull;		/* the pass filled its top-k heap, so matches
								 * may exist beyond cand[] */
	int			curk;			/* candidate width of the current pass */
	double		maxhits;		/* provable upper bound on result size (cap growth) */
	/* plain-scan (amgettuple, no ORDER BY) state for index-only counts */
	bool		plainInit;		/* have we materialized the matching TIDs? */
	ItemPointerData *plainTids; /* sorted matching TIDs */
	int			nplain;
	int			plainpos;
	bool		plainRecheck;	/* results need a heap recheck (fuzzy/regex) */
	IndexTuple	plainItup;		/* cached all-NULL itup for index-only scans */
	TupleDesc	plainItupDesc;

	/*
	 * <@> edit-distance ordering scan (task Z9).  A DIFFERENT order-by operator
	 * over the same ordered[]/cand[] machinery: `edistScan` says the order-by
	 * argument is a text PATTERN (strategy WEAVE_STRAT_EDIST) rather than a
	 * wquery, so the pass that fills cand[] is weave_edist_pass() and the value
	 * in ScoredTid.score is already a DISTANCE, not a BM25 score to be inverted.
	 *
	 * The widening ladder is over a DISTANCE THRESHOLD rather than a candidate
	 * width, because that is what the channel's bound prunes on: a pass at
	 * threshold `edistThr` collects every document whose distance is <= it, and
	 * that set is exact and complete -- any closer term would also be within the
	 * threshold and so present.  `edistNext` is the smallest distance anything
	 * NOT collected could have (the minimum over the exact distances seen above
	 * the threshold and the block bounds of the pages the bound let us skip), so
	 * the next pass jumps straight to it and no pass repeats a distance.
	 * `edistDone` is INT_MAX arriving there: nothing further exists anywhere, an
	 * exact stop rather than an estimate.
	 */
	bool		edistScan;
	char	   *edistPat;
	int			edistPatLen;
	int			edistThr;
	int			edistNext;
	bool		edistDone;

	/*
	 * `<=>` vector ordering scan over a wvec column (task F7).  A THIRD order-by
	 * operator over the same ordered[]/cand[] machinery, and the shape is the
	 * lexical one rather than the `<@>` one: the ladder is over a CANDIDATE WIDTH,
	 * because that is what the vector channel's top-k takes as its parameter and
	 * what its block bound prunes against (a wider k is a lower top-k floor, so it
	 * prunes less and finds more).
	 *
	 * `vecQuery` is the query vector, DETOASTED AND COPIED into the scan's own
	 * context, because sk_argument is only guaranteed for the duration of the
	 * rescan and every pass of the ladder re-reads it.
	 *
	 * `vecAttno` is the index attribute the order-by key named, and it is carried
	 * rather than re-derived so that the pass scores the weft recorded against THAT
	 * column: an index may carry more than one wvec column, and scoring the wrong
	 * weft is a wrong answer with a correct row count (include/weave/vector.h).
	 *
	 * `vecDone` is the ladder's exact stop: a pass that returned fewer hits than it
	 * asked for never filled its top-k heap, so its floor stayed -INFINITY, so no
	 * block could be skipped on the bound and no lane was passed over -- the pass
	 * IS the complete set of live lanes.  Same argument as `candfull` on the lexical
	 * side, and `vecLanes` (total lanes in every bolt scanned) is the cheap early
	 * stop that `maxhits` is there.
	 */
	bool		vecScan;
	WVec	   *vecQuery;
	AttrNumber	vecAttno;
	int			veck;			/* candidate width of the current vector pass */
	uint64		vecLanes;		/* lanes the last pass saw, live or not */
	bool		vecDone;

	/*
	 * `@~` / `@~*` corpus-trigram restriction (task Z8).  A RESTRICTION key, not
	 * an ordering one, and its argument is a raw LIKE PATTERN rather than a
	 * wquery -- which is why weave_rescan() must decide WHICH KIND OF KEY it has
	 * before it touches sk_argument.  DatumGetWQuery() on a text datum is not a
	 * type error, it reads a varlena header as a WeaveQuery and walks garbage;
	 * the ORDER BY dispatch in the same function already says exactly this.
	 *
	 * The discriminator is the COLUMN, via weave_index_layout(): strategy numbers
	 * are per operator family, so gram_ops's 1 and wdoc_lex_ops's 1 are different
	 * operators with the same number, and only the attribute tells them apart.
	 */
	bool		cgramScan;
	char	   *cgramPat;
	int			cgramPatLen;
	bool		cgramCI;		/* the case-insensitive operator (`@~*`) */
	bool		cgramLossy;		/* the scan carried MORE keys than the one cgram key
								 * this route honours (`d @@@ q AND body @~ p`, or two
								 * `@~`s on the same column).  Then the returned set is
								 * a SUPERSET of the answer and the executor MUST
								 * re-evaluate the whole qual, so recheck goes out as
								 * true.  Without this flag such a query returns rows
								 * that fail the ignored clause -- a wrong answer, and
								 * one the planner is entitled to produce because
								 * amcanmulticol is true. */

	/*
	 * THE FUSED MULTI-CHANNEL ORDERING SCAN (task F2.2), which is a FOURTH
	 * order-by shape over the same ordered[]/cand[] machinery.  It is recognized
	 * by a transport key -- strategy WEAVE_STRAT_FUSE_WEIGHTS, `<~>`, whose right
	 * operand is the float4[] of per-channel weights -- standing beside two or
	 * more scored order-by keys.  src/am/fusepath.c builds that shape and
	 * doc/specs/FUSED_TOPK.md sect. 7a records why the weights travel on an ORDER
	 * BY key rather than a qual.
	 *
	 * The ladder is over a CANDIDATE WIDTH, the lexical/vector shape rather than
	 * the `<@>` threshold shape, because a width is what the fused core takes as
	 * its parameter and what its three prunes measure against: a wider k is a
	 * lower theta, so it prunes less and finds more.
	 *
	 * `fuseDone` is the ladder's EXACT stop, and the proof is the one `candfull`
	 * rests on: a pass whose top-k heap never filled kept theta at -INFINITY for
	 * its whole run, and every one of the three prunes in src/am/fuse.c compares
	 * against theta (`ub <= theta`, `s + csuffix <= theta`, and the partition's
	 * `suffix[split-1] <= theta`).  Nothing was skipped, so that pass scored every
	 * candidate the channels can generate.  It is an AND over the segments,
	 * because the pass is run per bolt and one bolt proving completeness says
	 * nothing about another.
	 *
	 * EVERY QUERY AND WEIGHT IS COPIED into the scan's context.  sk_argument belongs
	 * to the executor's ScanKey and every rung of the ladder re-reads them all --
	 * arbitrarily later than the rescan that installed them -- which is the same
	 * reason the single-channel vector path copies its query vector.
	 */
	bool		fuseScan;
	WeaveQuery *fuseQ;			/* nfuse queries, in scored-key order */
	float4	   *fuseW;			/* nfuse weights, same order, from the transport */

	/*
	 * PER-CHANNEL KIND, and task F8 is why it exists.  Until F8 the only scored
	 * channel a fused run could drive was the lexical one, so the kind was implicit
	 * in `fuseQ` and weave_fuse_rescan() refused everything else.  A fused run can
	 * now carry the vector channel too, and a vector key's query is a WVec rather
	 * than a WeaveQuery -- so the arrays are parallel and exactly one of fuseQ[i]
	 * and fuseV[i] is non-NULL, keyed by fuseStrat[i].  Keeping the kind EXPLICIT
	 * rather than inferring it from which pointer is set is deliberate: the day a
	 * third channel arrives with a text query, an inference would be silently wrong
	 * where a switch is a compile-time reminder.
	 *
	 * fuseA[i] is the key's index ATTRIBUTE number, which the vector channel needs
	 * and the lexical one does not: a weft records the column it was built from, and
	 * scoring a weft belonging to a different column is a wrong answer with a
	 * correct row count (include/weave/vector.h).
	 */
	int		   *fuseStrat;		/* nfuse sk_strategy values, same order */
	WVec	  **fuseV;			/* nfuse query vectors; NULL for a lexical key */
	AttrNumber *fuseA;			/* nfuse index attribute numbers */
	int			nfuse;
	int			fusek;			/* candidate width of the current fused pass */
	bool		fuseDone;
} WeaveScanOpaqueData;

typedef WeaveScanOpaqueData *WeaveScanOpaque;

/* The cgram route (task Z8).  Defined near the bottom, next to the recheck and
 * the fallback heap pass it composes; declared here because weave_getbitmap()
 * and weave_gettuple() are above it. */
static bool weave_cgram_collect(Relation index, const char *pat, int patlen,
								bool ci, TidSet *out);

/* ranked-scan growth (L14); defined next to the visibility machinery */
static int weave_topk_candidates_guarded(Relation index, WeaveQuery q, int wantk,
										ScoredTid **out);
static int weave_ord_width(int k);
static void weave_ord_pass(Relation index, WeaveScanOpaque so);
static void weave_ord_probe(Relation index, WeaveScanOpaque so, int want);
static bool weave_ord_grow(Relation index, WeaveScanOpaque so);
static void weave_edist_pass(Relation index, WeaveScanOpaque so);
static bool weave_edist_grow(Relation index, WeaveScanOpaque so);

/* the <=> vector ordering ladder (F7); defined next to the lexical one */
static void weave_vec_pass(Relation index, WeaveScanOpaque so);
static bool weave_vec_grow(Relation index, WeaveScanOpaque so);

/* the fused multi-channel ordering ladder (F2.2); defined next to the others.
 * weave_fuse_rescan() is up in weave_rescan() because that is where scan keys
 * arrive, and it is the only one of the three that reads a ScanKey. */
static bool weave_fuse_rescan(IndexScanDesc scan, WeaveScanOpaque so);
static void weave_fuse_pass(Relation index, WeaveScanOpaque so);
static bool weave_fuse_grow(Relation index, WeaveScanOpaque so);

static int
cmp_tid(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

void
tidset_sort_uniq(TidSet *s)
{
	int			i,
				j;

	/* A garbage .n (from a stale/recycled read under concurrent merge) would
	 * qsort/scan s->tids out of bounds; treat an implausible count as empty and
	 * let the scan's generation re-check restart.  See tidset_sane(). */
	if (s->n < 0 || (Size) s->n > MaxAllocSize / sizeof(ItemPointerData))
	{
		s->tids = NULL;
		s->n = 0;
		return;
	}
	if (s->n <= 1)
		return;
	qsort(s->tids, s->n, sizeof(ItemPointerData), cmp_tid);
	for (i = 0, j = 1; j < s->n; j++)
		if (ItemPointerCompare(&s->tids[i], &s->tids[j]) != 0)
			s->tids[++i] = s->tids[j];
	s->n = i + 1;
}

/*
 * Tombstone (deleted-docid) sets, one per segment, loaded from each segment's
 * livedocs blob.  VACUUM (weave_bulkdelete) records docids of vacuumed heap
 * tuples here; scans/counts MUST subtract them, because the index-only and
 * count paths trust the visibility map and would otherwise report a
 * vacuumed-and-reused heap slot as a match.  hasany is false (the common,
 * delete-free case) => zero overhead: no membership checks at all.
 */
typedef struct WeaveTombstones
{
	bool		hasany;
	uint32		nseg;
	uint8	  **blobs;			/* per-segment palloc'd blob, or NULL */
	sm_t	   *maps;			/* per-segment opened sm_t (valid iff blobs[i]) */
	bool	   *present;		/* whether segment i has a tombstone map */
}			WeaveTombstones;

static void
weave_tombstones_load(Relation index, const WeaveMetaPageData *meta, WeaveTombstones *t)
{
	uint32		s;

	t->hasany = false;
	t->nseg = meta->nsegments;
	t->blobs = NULL;
	t->maps = NULL;
	t->present = NULL;
	for (s = 0; s < meta->nsegments; s++)
		if (meta->segs[s].livedocs != InvalidBlockNumber &&
			meta->segs[s].livedocslen > 0)
		{
			t->hasany = true;
			break;
		}
	if (!t->hasany)
		return;

	t->blobs = (uint8 **) palloc0(meta->nsegments * sizeof(uint8 *));
	/*
	 * sm_t (struct sparsemap) is declared with 8-byte alignment; plain palloc
	 * only guarantees MAXALIGN (4 on ILP32), so allocate the array 8-aligned to
	 * satisfy that requirement (a misaligned sm_t trips -fsanitize=alignment).
	 */
	t->maps = (sm_t *) palloc_aligned(meta->nsegments * sizeof(sm_t), 8, 0);
	memset(t->maps, 0, meta->nsegments * sizeof(sm_t));
	t->present = (bool *) palloc0(meta->nsegments * sizeof(bool));
	for (s = 0; s < meta->nsegments; s++)
	{
		const WeaveSegMeta *sg = &meta->segs[s];

		if (sg->livedocs != InvalidBlockNumber && sg->livedocslen > 0)
		{
			t->blobs[s] = weave_read_blob(index, sg->livedocs, sg->livedocslen);
			sm_open(&t->maps[s], (uint8_t *) t->blobs[s], sg->livedocslen);
			t->present[s] = true;
		}
	}
}

static void
weave_tombstones_free(WeaveTombstones *t)
{
	uint32		s;

	if (!t->hasany)
		return;
	for (s = 0; s < t->nseg; s++)
		if (t->present[s])
			pfree(t->blobs[s]);
	pfree(t->blobs);
	pfree(t->maps);
	pfree(t->present);
}

/*
 * Drop TIDs tombstoned in ONE specific segment from a TidSet in place.
 * Tombstones are per-segment: a docid deleted in segment A must only be
 * suppressed among matches produced BY segment A -- the same heap TID may have
 * been reused by a live document in a newer segment or the pending list, and
 * that document must not be filtered.  Applied to each segment's own match
 * contribution at collection time.
 */
static void
weave_filter_tombstoned_seg(WeaveTombstones *t, uint32 segidx, TidSet *s)
{
	int			i,
				j = 0;
	uint64		stackids[256];
	bool		stackres[256];
	uint64	   *ids;
	bool	   *res;

	if (!t->hasany || s->n == 0 || segidx >= t->nseg || !t->present[segidx])
		return;
	if (s->n < 0 || (Size) s->n > MaxAllocSize / sizeof(ItemPointerData))
	{
		/* garbage count from a stale/recycled read: treat as empty (the scan's
		 * generation re-check restarts).  Guards the s->tids[] sweep below. */
		s->tids = NULL;
		s->n = 0;
		return;
	}

	/*
	 * Batched membership: extract this set's docids (already ascending, since
	 * the TidSet is TID-sorted and docid is monotonic in TID) and test them
	 * all in one left-to-right sweep with sm_contains_many -- O(chunks + n)
	 * instead of n independent head-walks.  Use a stack buffer for the common
	 * small case to avoid palloc; fall back to palloc only for large sets.
	 */
	if (s->n <= (int) lengthof(stackids))
	{
		ids = stackids;
		res = stackres;
	}
	else
	{
		ids = (uint64 *) palloc(s->n * sizeof(uint64));
		res = (bool *) palloc(s->n * sizeof(bool));
	}

	for (i = 0; i < s->n; i++)
		ids[i] = weave_tid_to_docid(&s->tids[i]);

	sm_contains_many(&t->maps[segidx], ids, res, (size_t) s->n);

	for (i = 0; i < s->n; i++)
		if (!res[i])
			s->tids[j++] = s->tids[i];
	s->n = j;

	if (ids != stackids)
	{
		pfree(ids);
		pfree(res);
	}
}

/* Read the metapage for corpus stats + dictstart. */
static void
weave_read_meta(Relation index, WeaveMetaPageData *out)
{
	Buffer		buffer = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	Page		page;
	uint32		s;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	weave_check_meta(page, index);
	weave_meta_from_page(page, out);	/* version-aware: expands a v3 metapage */
	UnlockReleaseBuffer(buffer);

	/*
	 * Every v6 bolt is self-describing (SEGMENT_FORMAT.md sect. 6): when
	 * segs[i].chandesc is set, later code is entitled to trust the weft array
	 * on that page.  This is the one place every scan, build, merge and vacuum
	 * path reads the segment directory before acting on it, so it is where a
	 * corrupt descriptor page must turn into a clean ERROR
	 * (doc/CONVENTIONS.md decision 2: on-disk bytes are not trusted) rather
	 * than only being caught later by an explicit weave_check() call, or not
	 * at all. weave_chandesc_required() is the throwing wrapper built for
	 * exactly this call site.
	 */
	for (s = 0; s < out->nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveSegMeta *seg = &out->segs[s];
		WeaveChannelDesc weft[WEAVE_MAX_WEFTS];

		if (seg->dictstart == InvalidBlockNumber)
			continue;			/* consumed slot */
		if (seg->chandesc == InvalidBlockNumber)
			continue;			/* pre-v6 bolt, not yet merged */
		(void) weave_chandesc_required(index, seg->chandesc, weft, WEAVE_MAX_WEFTS);
	}
}

/*
 * Read just the current segment-directory generation (cheap SHARE-locked
 * metapage peek).  A scan records this at its metapage snapshot and re-checks
 * it after collecting; if it moved, a concurrent merge/vacuum may have freed +
 * recycled pages the scan read from a now-stale segment descriptor, so the scan
 * must discard its result and restart from a fresh snapshot.
 */
static uint32
weave_read_meta_generation(Relation index)
{
	Buffer		buffer = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	uint32		gen;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	/* generation is at the SAME offset only within one format version; read it
	 * version-aware so a v3 metapage's generation (at the v3 offset) is correct. */
	{
		WeaveMetaPageData m;

		weave_meta_from_page(BufferGetPage(buffer), &m);
		gen = m.generation;
	}
	UnlockReleaseBuffer(buffer);
	return gen;
}

/*
 * Use a segment's sparse block index to find the single dictionary page that
 * could contain `term`: the last index entry whose term <= target.  Returns
 * that page's block number, or `dictstart` if the segment has no block index
 * (empty segment or pre-index format).  The located page is the ONLY page that
 * can hold the term (the next page's first term is > target), so point lookups
 * scan just that page.
 */
static BlockNumber
weave_dict_seek_at(Relation index, BlockNumber dictstart,
				   BlockNumber dictindexstart, const char *term, int termlen)
{
	BlockNumber iblk = dictindexstart;
	BlockNumber best = dictstart;

	while (iblk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		bool		overshot = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buf = ReadBuffer(index, iblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveDictIndexEntry *ie = (WeaveDictIndexEntry *) ptr;
			int			cmplen;
			int			c;

			if (!weave_dictindex_entry_fits(ie, end))
				break;			/* recycled/corrupt page: stop (see the helper) */
			cmplen = Min((int) ie->termlen, termlen);
			c = memcmp(ie->term, term, cmplen);

			if (c == 0)
				c = (int) ie->termlen - termlen;
			if (c <= 0)
				best = ie->blk;		/* entry term <= target: candidate page */
			else
			{
				overshot = true;	/* entries are sorted; no need to go further */
				break;
			}
			ptr += MAXALIGN(offsetof(WeaveDictIndexEntry, term) + ie->termlen);
		}
		UnlockReleaseBuffer(buf);
		if (overshot)
			break;
		iblk = next;
	}
	return best;
}

/*
 * The lexical weft's dictionary, by name.  Split from the body above by task Z8,
 * which needs the SAME seek over a DIFFERENT dictionary chain: the cgram weft is
 * a dictionary + block index in exactly this format (include/weave/cgram.h), and
 * its roots live on its own WEAVE_PK_CGRAM page rather than in WeaveSegMeta.  One
 * implementation, because two copies of a binary search that must agree with the
 * writer's term order is how a probe starts landing on the wrong page -- a false
 * negative, hence a silently dropped row.
 */
static BlockNumber
weave_dict_seek(Relation index, const WeaveSegMeta *seg,
			   const char *term, int termlen)
{
	return weave_dict_seek_at(index, seg->dictstart, seg->dictindexstart,
							  term, termlen);
}

/*
 * Look up a term in the dictionary; on hit, read its full posting list into a
 * TidSet.  Returns true if found.  weave_dict_seek uses the segment's sparse
 * block index to jump straight to the one dictionary page that can hold the
 * term (scanning the whole chain only for a segment that predates the index).
 */
static bool
weave_lookup_term_at(Relation index, BlockNumber dictstart,
					 BlockNumber dictindexstart, bool has_doclen_col,
					 const char *term, int termlen, TidSet *out)
{
	BlockNumber blk = weave_dict_seek_at(index, dictstart, dictindexstart,
										 term, termlen);

	bool		onlyone = (dictindexstart != InvalidBlockNumber);

	out->tids = NULL;
	out->n = 0;

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr;
		char	   *end;
		BlockNumber firstposting = InvalidBlockNumber;
		uint32		firstoffset = 0;
		uint32		df = 0;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				firstposting = de->firstposting;
				firstoffset = de->firstoffset;
				df = de->df;
				found = true;
				break;
			}
			ptr += esize;
		}
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buffer);

		if (found)
		{
			/* read exactly this term's df postings from the shared chain */
			WeavePosting *post;
			int			np = weave_decode_term(index, firstposting, firstoffset,
										  df, &post, NULL, false, NULL, true,
										  has_doclen_col);
			ItemPointerData *tids = palloc(Max(np, 1) * sizeof(ItemPointerData));
			int			n = 0;
			int			i;

			for (i = 0; i < np; i++)
				tids[n++] = post[i].tid;
			pfree(post);
			out->tids = tids;
			out->n = n;
			tidset_sort_uniq(out);
			return true;
		}
		if (onlyone)
			break;				/* block index located the only possible page */
	}
	return false;
}

/* The lexical weft's version.  Z8 split the body above so the cgram weft can
 * reuse it; this wrapper keeps the lexical call sites (and the lex_term counter,
 * which is about the LEXICAL channel and must not move when a cgram probe runs)
 * exactly as they were. */
static bool
weave_lookup_term(Relation index, const WeaveSegMeta *seg,
				 const char *term, int termlen, TidSet *out)
{
	weave_chan_lex_term++;
	return weave_lookup_term_at(index, seg->dictstart, seg->dictindexstart,
								seg->doclenstart == InvalidBlockNumber,
								term, termlen, out);
}

/* set operations on sorted TidSets */

/*
 * Galloping (exponential) search: return the least index >= lo in t[0..n) whose
 * tid >= key.  Used to skip runs when intersecting a small set against a large
 * one (O(|small| * log|large|) instead of O(|small|+|large|)).
 */
static inline int
tidset_gallop(const ItemPointerData *t, int n, int lo, const ItemPointerData *key)
{
	int			step = 1;
	int			hi;

	while (lo < n && ItemPointerCompare((ItemPointer) &t[lo], (ItemPointer) key) < 0)
	{
		if (lo + step < n &&
			ItemPointerCompare((ItemPointer) &t[lo + step], (ItemPointer) key) < 0)
		{
			lo += step;
			step <<= 1;
		}
		else
			break;
	}
	/* binary search in (lo, min(lo+step, n)] */
	hi = Min(lo + step, n - 1);
	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (ItemPointerCompare((ItemPointer) &t[mid], (ItemPointer) key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * A TidSet's .n must be a plausible element count.  Under concurrent merge a
 * scan can read a segment whose pages were freed and recycled (pg_weave recycles
 * freed pages with no deletion-xid gate), yielding a decoded set whose .n is
 * garbage (e.g. leftover pointer bytes -> ~1.4 billion).  Feeding that to the
 * set-algebra primitives below made them palloc a multi-gigabyte result
 * ("invalid memory alloc request size") and walk a.tids[]/b.tids[] out of
 * bounds (SIGSEGV) -- the field-reported crash under read+insert+merge.  The
 * primitives treat an implausible operand as EMPTY: a bounded wrong (under-
 * count) result, never a crash, and the scan's generation re-check then detects
 * the stale read and restarts with a fresh directory snapshot.
 */
#define TIDSET_MAX_N ((int) (MaxAllocSize / sizeof(ItemPointerData)))
static inline TidSet
tidset_sane(TidSet s)
{
	if (s.n < 0 || s.n > TIDSET_MAX_N)
	{
		s.tids = NULL;
		s.n = 0;
	}
	return s;
}

static TidSet
tidset_and(TidSet a, TidSet b)
{
	TidSet		r;
	int			i = 0,
				j = 0,
				k = 0;

	a = tidset_sane(a);
	b = tidset_sane(b);
	r.tids = palloc(Min(a.n, b.n) * sizeof(ItemPointerData) + 1);

	/*
	 * When the sets differ greatly in size, gallop the smaller through the
	 * larger (skip-list style) so a highly selective AND does not touch every
	 * posting of the common term.  Otherwise a linear merge is cheapest.
	 */
	if (a.n > 0 && b.n > 0 && (a.n > 4 * b.n || b.n > 4 * a.n))
	{
		const ItemPointerData *sm = a.n <= b.n ? a.tids : b.tids;
		const ItemPointerData *lg = a.n <= b.n ? b.tids : a.tids;
		int			sn = Min(a.n, b.n);
		int			ln = Max(a.n, b.n);
		int			li = 0;
		int			si;

		for (si = 0; si < sn; si++)
		{
			li = tidset_gallop(lg, ln, li, &sm[si]);
			if (li >= ln)
				break;
			if (ItemPointerCompare((ItemPointer) &lg[li], (ItemPointer) &sm[si]) == 0)
				r.tids[k++] = sm[si];
		}
		r.n = k;
		return r;
	}

	while (i < a.n && j < b.n)
	{
		int			c = ItemPointerCompare(&a.tids[i], &b.tids[j]);

		if (c == 0)
		{
			r.tids[k++] = a.tids[i];
			i++;
			j++;
		}
		else if (c < 0)
			i++;
		else
			j++;
	}
	r.n = k;
	return r;
}

static TidSet
tidset_or(TidSet a, TidSet b)
{
	TidSet		r;
	int			i = 0,
				j = 0,
				k = 0;

	a = tidset_sane(a);
	b = tidset_sane(b);
	r.tids = palloc((a.n + b.n) * sizeof(ItemPointerData) + 1);
	while (i < a.n && j < b.n)
	{
		int			c = ItemPointerCompare(&a.tids[i], &b.tids[j]);

		if (c == 0)
		{
			r.tids[k++] = a.tids[i];
			i++;
			j++;
		}
		else if (c < 0)
			r.tids[k++] = a.tids[i++];
		else
			r.tids[k++] = b.tids[j++];
	}
	while (i < a.n)
		r.tids[k++] = a.tids[i++];
	while (j < b.n)
		r.tids[k++] = b.tids[j++];
	r.n = k;
	return r;
}

/* a AND NOT b (b subtracted from a) */
static TidSet
tidset_andnot(TidSet a, TidSet b)
{
	TidSet		r;
	int			i = 0,
				j = 0,
				k = 0;

	a = tidset_sane(a);
	b = tidset_sane(b);
	r.tids = palloc(a.n * sizeof(ItemPointerData) + 1);
	while (i < a.n)
	{
		if (j >= b.n)
			r.tids[k++] = a.tids[i++];
		else
		{
			int			c = ItemPointerCompare(&a.tids[i], &b.tids[j]);

			if (c == 0)
			{
				i++;
				j++;
			}
			else if (c < 0)
				r.tids[k++] = a.tids[i++];
			else
				j++;
		}
	}
	r.n = k;
	return r;
}

/*
 * weave_lookup_prefix -- union the posting lists of every dictionary term that
 * begins with the given prefix.  Dictionary entries are byte-sorted, so the
 * matching terms are contiguous: seek (via the sparse per-page block index) to
 * the page that can hold the prefix, then scan forward only while entries could
 * still start with the prefix, stopping at the first term that sorts past it.
 * Sublinear in the dictionary rather than a full scan.
 */
static void
weave_lookup_prefix(Relation index, const WeaveSegMeta *seg,
				   const char *prefix, int prefixlen, TidSet *out)
{
	BlockNumber blk = weave_dict_seek(index, seg, prefix, prefixlen);

	/*
	 * THE MECHANISM, counted where it is chosen rather than where it is
	 * described.  One increment per (leaf, bolt) pair: a prefix leaf is
	 * resolved once per bolt, and per-bolt is the unit any later comparison
	 * against a trie route would have to be in.  doc/PHASES.md Z4.
	 */
	weave_chan_prefix_dict++;
	int			cap = 32;
	int			n = 0;
	/* sized by the number of matching tuples, which is corpus-scale (a
	 * prefix can match every row); query path, so a throw here loses one
	 * query rather than a vacuum */
	ItemPointerData *tids = WEAVE_ALLOC_MAYBE_HUGE((Size) cap * sizeof(ItemPointerData));
	bool		done = false;

	while (blk != InvalidBlockNumber && !done)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		weave_chan_dict_pages++;
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;
			int			cmplen;
			int			c;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
			cmplen = Min((int) de->termlen, prefixlen);
			c = memcmp(de->term, prefix, cmplen);

			if (c < 0 || (c == 0 && (int) de->termlen < prefixlen))
			{
				/* term sorts before the prefix: not there yet, keep scanning */
				ptr += esize;
				continue;
			}
			if (c > 0)
			{
				/* first prefixlen bytes exceed the prefix: sorted, so no more
				 * matches can follow -- stop */
				done = true;
				break;
			}
			/* c == 0 and de->termlen >= prefixlen: a prefix match */
			weave_chan_terms_expanded++;
			{
				WeavePosting *post;
				int			np = weave_decode_term(index, de->firstposting,
												  de->firstoffset, de->df,
												  &post, NULL, false, NULL, true,
												  seg->doclenstart == InvalidBlockNumber);
				int			k;

				for (k = 0; k < np; k++)
				{
					if (n >= cap)
					{
						cap *= 2;
						/* corpus-scale, query path: see the palloc above */
						tids = WEAVE_REALLOC_MAYBE_HUGE(tids, (Size) cap * sizeof(ItemPointerData));
					}
					tids[n++] = post[k].tid;
				}
				pfree(post);
			}
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		blk = next;
	}

	out->tids = tids;
	out->n = n;
	tidset_sort_uniq(out);
}

/*
 * Evaluate the query into a TidSet via a stack machine over the RPN items.
 * NOT is handled specially: a bare NOT is only meaningful as "a AND NOT b", so
 * we track whether each stack entry is "positive" (a TID set) or "negative"
 * (the complement of a TID set).  AND/OR combine them with De Morgan; a top-
 * level negative result is complemented against all indexed TIDs (the universe).
 *
 * THE UNIVERSE IS BUILT LAZILY, and that is a performance requirement rather
 * than a style preference.  Building it reads EVERY posting list of EVERY term
 * in the segment -- Sum(df) work, the whole expanded inverted index, typically
 * two orders of magnitude more than ndocs.  It was previously built eagerly
 * whenever the query contained any NOT at all, but De Morgan means the common
 * shape `a & !b` reduces to andnot(a, b) and never consults it: the universe is
 * needed only when the TOP-LEVEL result is still negated, i.e. for a purely
 * negative query like `!b`.  Measured cost of getting this wrong, on a 200k-row
 * corpus: `count(*) WHERE d @@@ 'common & !rare'` took 603.9 ms against
 * tsvector+GIN's 14.7 ms -- a 41x loss, entirely spent materialising a set the
 * evaluation then discarded.  Boolean NOT was in no predecessor's benchmark
 * matrix, which is why it went unmeasured for the whole life of the fork.
 */
typedef struct EvalVal
{
	TidSet		set;
	bool		negated;		/* true => set represents docs NOT to include */
} EvalVal;

/*
 * Everything weave_universe_bounded needs, so the evaluator can defer the call.
 * `built` memoises within one evaluation; a query can only need the universe
 * once, but the flag keeps that a local fact rather than an assumption.
 */
typedef struct UniverseSrc
{
	Relation	index;
	BlockNumber dictstart;
	double		ndocs;
	bool		has_doclen_col;
	bool		built;
	TidSet		set;
} UniverseSrc;

static TidSet weave_universe_bounded(Relation index, BlockNumber dictstart,
									 double ndocs, bool has_doclen_col);

static TidSet
universe_get(UniverseSrc *u)
{
	if (!u->built)
	{
		u->set = weave_universe_bounded(u->index, u->dictstart, u->ndocs,
										u->has_doclen_col);
		u->built = true;
	}
	return u->set;
}

static TidSet
weave_eval_query(Relation index, const WeaveSegMeta *seg, WeaveQuery q,
				UniverseSrc *universe)
{
	EvalVal    *stack;
	int			top = 0;
	uint32		i;
	TidSet		result;

	if (q->nitems == 0)
	{
		result.tids = NULL;
		result.n = 0;
		return result;
	}

	stack = palloc(q->nitems * sizeof(EvalVal));

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			TidSet		s;

			if (it->flags & WEAVE_QF_PREFIX)
				weave_lookup_prefix(index, seg,
								   WEAVE_QUERY_ITEMTEXT(q, it), it->termlen, &s);
			else if (!weave_lookup_term(index, seg,
									   WEAVE_QUERY_ITEMTEXT(q, it), it->termlen, &s))
			{
				/*
				 * weave_lookup_term writes *out ONLY when the term is present in
				 * this segment; on a miss it returns false and leaves `s`
				 * UNINITIALIZED.  Pushing that uninitialized TidSet left stale
				 * stack bytes in .tids/.n (e.g. .n picking up a leftover
				 * pointer's low word), which a later tidset_or/tidset_and read as
				 * a multi-billion-element set -> "invalid memory alloc request
				 * size" / SIGSEGV.  A term absent from a segment is the empty set.
				 * (weave_lookup_prefix always writes *out, so it needs no guard.)
				 */
				s.tids = NULL;
				s.n = 0;
			}
			stack[top].set = s;
			stack[top].negated = false;
			top++;
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			Assert(top >= 1);
			stack[top - 1].negated = !stack[top - 1].negated;
		}
		else					/* AND / OR */
		{
			EvalVal		b = stack[--top];
			EvalVal		a = stack[--top];
			EvalVal		res;

			if (it->op == WEAVE_OP_AND || it->op == WEAVE_OP_PHRASE)
			{
				/* PHRASE is treated as AND for candidate generation; the
				 * bitmap heap recheck (@@@) enforces adjacency exactly. */
				if (!a.negated && !b.negated)
				{
					res.set = tidset_and(a.set, b.set);
					res.negated = false;
				}
				else if (!a.negated && b.negated)
				{
					res.set = tidset_andnot(a.set, b.set);
					res.negated = false;
				}
				else if (a.negated && !b.negated)
				{
					res.set = tidset_andnot(b.set, a.set);
					res.negated = false;
				}
				else			/* !a AND !b = !(a OR b) */
				{
					res.set = tidset_or(a.set, b.set);
					res.negated = true;
				}
			}
			else				/* OR */
			{
				if (!a.negated && !b.negated)
				{
					res.set = tidset_or(a.set, b.set);
					res.negated = false;
				}
				else if (a.negated && b.negated)	/* !a OR !b = !(a AND b) */
				{
					res.set = tidset_and(a.set, b.set);
					res.negated = true;
				}
				else
				{
					/* positive OR negative: !x OR y = !(x AND NOT y) */
					TidSet		pos = a.negated ? b.set : a.set;
					TidSet		neg = a.negated ? a.set : b.set;

					res.set = tidset_andnot(neg, pos);
					res.negated = true;
				}
			}
			stack[top++] = res;
		}
	}

	Assert(top == 1);
	if (stack[0].negated)
		result = tidset_andnot(universe_get(universe), stack[0].set);
	else
		result = stack[0].set;

	return result;
}

/* ---------------------------------------------------------------------------
 * The dictionary page chain, presented to the universal-Levenshtein core as a
 * WeaveUlevVocab (include/weave/uleven.h).
 *
 * WHY AN ITERATOR AND NOT A HAND-WRITTEN WALK.  The walk this replaced inlined
 * a byte-wise automaton (include/weave/lev.h) into the page loop, which made the
 * matcher and the storage layout one lump of code: the only test that could see
 * the matcher was a regression test with an index in it.  uleven.h's core is the
 * matcher alone, with 7.4M property checks behind it in test/hegel/test_uleven.c,
 * and it takes the vocabulary as `next` + optional `skip`.  So this struct is the
 * whole of the storage half, and the Z3 SuRF trie can be substituted for it
 * later without touching the matcher.
 *
 * THE PAGE STAYS SHARE-LOCKED ACROSS THE HIT CALLBACK, deliberately.  `term`
 * points into the buffer, and weave_uleven_expand_vocab() documents that it is
 * valid until the following next() -- which is exactly when this iterator
 * releases the page.  The hit callback then reads other pages (the posting
 * chain) while holding that share lock, which is what the previous walk did too;
 * it is a read-only share lock on a buffer, taken in no particular order, so
 * there is no lock-ordering claim to preserve.
 *
 * EVERY GUARD IN THE OLD WALK IS STILL HERE AND EACH IS LOAD-BEARING, because a
 * dictionary page is read under BUFFER_LOCK_SHARE while a concurrent merge can
 * free it and an insert recycle it (see weave_dict_entry_fits in weave/am.h):
 *	 - weave_scan_readbuf() returning InvalidBuffer means a concurrent
 *	   weave_vacuum truncated the block; that is END OF CHAIN, not an error.
 *	 - weave_dict_entry_fits() before trusting de->termlen, or the stride and the
 *	   term compare run off the page.
 *	 - weave_page_entry_end() as the limit, never a raw pd_lower (make
 *	   check-pdlower).
 *	 - CHECK_FOR_INTERRUPTS() between pages, with no buffer lock held.
 * ------------------------------------------------------------------------- */
typedef struct WeaveDictVocab
{
	Relation	index;
	const WeaveSegMeta *seg;
	BlockNumber blk;			/* the open page, or the next one to open */
	Buffer		buf;			/* pinned + share-locked while positioned */
	char	   *ptr;			/* next entry to yield on the open page */
	char	   *end;			/* weave_page_entry_end() of the open page */
	BlockNumber nextblk;		/* successor, read off the open page */
	WeaveDictEntry *cur;		/* the entry next() last yielded; see below */
	weave_ul_uint32 nyielded;	/* echoed as `ord`: the walk's term ordinal */

	/*
	 * Scratch for skip()'s successor key.  It is BLCKSZ because the dead prefix
	 * is a prefix of a CANDIDATE term, whose length weave_dict_entry_fits()
	 * bounds only by the page -- not by the query.  The walk this replaced sized
	 * the same buffer WEAVE_LEV_MAXQ + 2 (257) and memcpy'd deadlen bytes into
	 * it; deadlen is bounded by min(candlen, m + k + 1), so a query like
	 * 'ab~300' against a 400-byte dictionary term overran that stack array.  Not
	 * reachable from any test in the tree, which is why it survived: k has no
	 * upper bound in the parser (src/query/parse.c) and no test uses a k above 3.
	 */
	unsigned char *nextkey;
} WeaveDictVocab;

/* Drop the page we are sitting on, if any.  Idempotent. */
static void
weave_dictvocab_release(WeaveDictVocab *v)
{
	if (v->buf != InvalidBuffer)
		UnlockReleaseBuffer(v->buf);
	v->buf = InvalidBuffer;
	v->ptr = v->end = NULL;
	v->cur = NULL;
}

/*
 * WeaveUlevVocab.next: the next dictionary term in ascending unsigned-byte
 * order across the whole chain.  Returns 0 at end of vocabulary.
 */
static int
weave_dictvocab_next(void *arg, const char **term, weave_ul_uint32 *len,
					 weave_ul_uint32 *ord)
{
	WeaveDictVocab *v = (WeaveDictVocab *) arg;

	for (;;)
	{
		if (v->buf == InvalidBuffer)
		{
			Page		page;

			if (v->blk == InvalidBlockNumber)
				return 0;
			CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
			v->buf = weave_scan_readbuf(v->index, v->blk);
			if (v->buf == InvalidBuffer)
			{
				/* block truncated by a concurrent weave_vacuum: end of chain */
				v->blk = InvalidBlockNumber;
				return 0;
			}
			LockBuffer(v->buf, BUFFER_LOCK_SHARE);
			weave_chan_dict_pages++;	/* the work counter the prefix walk keeps too */
			page = BufferGetPage(v->buf);
			v->ptr = (char *) PageGetContents(page);
			v->end = weave_page_entry_end(page);
			v->nextblk = WeavePageGetOpaque(page)->nextblk;
		}

		if (v->ptr < v->end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) v->ptr;

			if (weave_dict_entry_fits(de, v->end))
			{
				v->ptr += MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
				v->cur = de;
				*term = de->term;
				*len = de->termlen;
				*ord = v->nyielded++;
				return 1;
			}
			/* recycled/corrupt page: abandon it (see weave_dict_entry_fits) */
		}

		/* page exhausted, or refused by the fits guard: follow the chain */
		v->blk = v->nextblk;
		weave_dictvocab_release(v);
	}
}

/*
 * WeaveUlevVocab.skip: advance past every remaining term beginning with
 * prefix[0..plen), which weave_uleven_match() has proven no within-k string can
 * extend.  Terms are byte-sorted, so those terms are one contiguous run and the
 * successor key is the prefix with its last non-0xff byte incremented.
 *
 * CORRECTNESS MUST NOT DEPEND ON THIS FUNCTION -- uleven.h says so, and the
 * mutation leg that makes it a no-op is the check.  It is what turns an
 * O(vocabulary) scan into roughly O(matching terms + boundaries), the effect an
 * FST/DFA intersection gives.
 */
static void
weave_dictvocab_skip(void *arg, const char *prefix, weave_ul_uint32 plen)
{
	WeaveDictVocab *v = (WeaveDictVocab *) arg;
	int			kl = (int) plen;
	BlockNumber tgt;

	if (kl <= 0 || kl > BLCKSZ)
		return;					/* no claim to act on */
	memcpy(v->nextkey, prefix, (Size) kl);
	while (kl > 0 && v->nextkey[kl - 1] == 0xff)
		kl--;
	if (kl == 0)
	{
		/* prefix is all 0xff: no term sorts after it, so the walk is over */
		v->blk = InvalidBlockNumber;
		weave_dictvocab_release(v);
		return;
	}
	v->nextkey[kl - 1]++;

	/*
	 * If the very next entry is already >= nextkey the run was one term long and
	 * a seek would only re-read pages we are holding.
	 */
	if (v->buf != InvalidBuffer && v->ptr < v->end)
	{
		WeaveDictEntry *nde = (WeaveDictEntry *) v->ptr;

		if (weave_dict_entry_fits(nde, v->end))
		{
			int			cmplen = Min((int) nde->termlen, kl);
			int			c = memcmp(nde->term, v->nextkey, cmplen);

			if (c > 0 || (c == 0 && (int) nde->termlen >= kl))
				return;
		}
	}

	/*
	 * Jump to the page that can hold nextkey, but only if it is a DIFFERENT
	 * page: seeking onto the page we are already on would restart it from the
	 * top and make no progress.  We do not reposition ptr inside the target
	 * page -- the entries before nextkey there are dead too, so the automaton
	 * kills them again and the second skip resolves to this same page and falls
	 * through to the linear step.  Parity with the walk this replaced; the
	 * CHECK_FOR_INTERRUPTS() in next() is what bounds the pathological case
	 * where a stale block index keeps handing back an earlier page.
	 */
	tgt = weave_dict_seek(v->index, v->seg, (const char *) v->nextkey, kl);
	if (tgt != InvalidBlockNumber && tgt != v->blk)
	{
		v->blk = tgt;
		weave_dictvocab_release(v);
	}
}

/*
 * The per-matching-term posting runs a dictionary walk collects and merges at
 * the end.  Shared by the fuzzy walk (weave_fuzzy_terms) and the regex walk
 * (weave_regex_terms): both yield one docid-sorted run per matching dictionary
 * term and want one sorted, de-duplicated TidSet out.
 */
typedef struct WeaveTermRuns
{
	Relation	index;
	const WeaveSegMeta *seg;
	WeaveDictVocab *voc;		/* for the current entry's posting locator */
	ItemPointerData **runs;
	int		   *runlen;
	int			nruns;
	int			runcap;
	int64		total;
} WeaveTermRuns;

static void
weave_termruns_init(WeaveTermRuns *h, Relation index, const WeaveSegMeta *seg,
					WeaveDictVocab *voc)
{
	h->index = index;
	h->seg = seg;
	h->voc = voc;
	h->runs = NULL;
	h->runlen = NULL;
	h->nruns = 0;
	h->runcap = 0;
	h->total = 0;
}

/*
 * The dictionary entry the iterator is positioned on matched: read its posting
 * list and keep the TIDs as a docid-sorted run for the k-way merge.  Valid only
 * while voc->cur is -- i.e. before the next weave_dictvocab_next(), on the same
 * still-share-locked page.
 */
static void
weave_termruns_add_current(WeaveTermRuns *h)
{
	WeaveDictEntry *de = h->voc->cur;
	WeavePosting *post;
	int			np;

	Assert(de != NULL);
	np = weave_decode_term(h->index, de->firstposting, de->firstoffset, de->df,
						   &post, NULL, false, NULL, true,
						   h->seg->doclenstart == InvalidBlockNumber);
	if (np > 0)
	{
		ItemPointerData *run = palloc(np * sizeof(ItemPointerData));
		int			i;

		for (i = 0; i < np; i++)
			run[i] = post[i].tid;
		if (h->nruns >= h->runcap)
		{
			h->runcap = Max(h->runcap * 2, 16);
			/* runcap tracks the number of matching terms, each contributing a
			 * run of matching tuples -- corpus-scale; query path, so a throw
			 * here loses one query */
			h->runs = h->runs ? WEAVE_REALLOC_MAYBE_HUGE(h->runs, (Size) h->runcap * sizeof(ItemPointerData *))
				: WEAVE_ALLOC_MAYBE_HUGE((Size) h->runcap * sizeof(ItemPointerData *));
			h->runlen = h->runlen ? WEAVE_REALLOC_MAYBE_HUGE(h->runlen, (Size) h->runcap * sizeof(int))
				: WEAVE_ALLOC_MAYBE_HUGE((Size) h->runcap * sizeof(int));
		}
		h->runs[h->nruns] = run;
		h->runlen[h->nruns] = np;
		h->nruns++;
		h->total += np;
	}
	pfree(post);
}

/*
 * WeaveUlevHitCb: one dictionary term within k.  Read its posting list and keep
 * the TIDs as a docid-sorted run for the k-way merge.
 *
 * WHY THIS REACHES BACK INTO THE ITERATOR for the posting locator.  A posting
 * list is addressed by three fields (firstposting, firstoffset, df) and
 * WeaveUlevVocab echoes exactly one uint32, so `ord` cannot carry it.  The
 * callback is invoked from inside weave_uleven_expand_vocab() immediately after
 * the next() that produced `term`, so voc->cur is exactly as valid as `term`
 * itself -- same page, still share-locked. `ord` stays the term ordinal, which
 * is what Z9's <@> ordering will want alongside `dist`.
 */
static int
weave_fuzzy_hit(void *arg, const char *term, weave_ul_uint32 len,
				weave_ul_uint32 ord, int dist)
{
	WeaveTermRuns *h = (WeaveTermRuns *) arg;

	Assert(h->voc->cur != NULL && h->voc->cur->term == term &&
		   h->voc->cur->termlen == len);
	weave_termruns_add_current(h);
	(void) ord;
	(void) dist;
	return 0;					/* no fanout cap on this route */
}

/*
 * k-way merge the per-term docid-sorted runs into one sorted, de-duplicated TID
 * array.  Each posting list is already docid-ordered, so merging avoids the
 * O(n log n) qsort over the whole (up to ~1.3M) union -- which a profiler showed
 * was the dominant fuzzy-count cost (a 1.28M qsort with a function-pointer
 * comparator is ~400ms) -- replacing it with O(n log k) and cheap inline
 * comparisons.  Must be called with no buffer lock held (the walk's chain is
 * released first): it checks for interrupts per merged posting.
 */
static void
weave_termruns_merge(const WeaveTermRuns *h, TidSet *out)
{
	ItemPointerData **runs = h->runs;
	int		   *runlen = h->runlen;
	int			nruns = h->nruns;
	int64		total = h->total;
	ItemPointerData *tids;
	int		   *pos;			/* current index into each run */
	int		   *heap;			/* min-heap of run indices by current head TID */
	int			hn = 0;
	int			nout = 0;
	int			r;

	tids = palloc(Max(total, 1) * sizeof(ItemPointerData));
	pos = palloc0(Max(nruns, 1) * sizeof(int));
	heap = palloc(Max(nruns, 1) * sizeof(int));

#define RUN_HEAD(ri) (&runs[(ri)][pos[(ri)]])
#define HEAP_LESS(x, y) (ItemPointerCompare(RUN_HEAD(heap[x]), RUN_HEAD(heap[y])) < 0)
	/* build the heap with each non-empty run's head */
	for (r = 0; r < nruns; r++)
	{
		if (runlen[r] > 0)
		{
			int			c = hn++;

			heap[c] = r;
			while (c > 0 && HEAP_LESS(c, (c - 1) / 2))
			{
				int			t = heap[c];

				heap[c] = heap[(c - 1) / 2];
				heap[(c - 1) / 2] = t;
				c = (c - 1) / 2;
			}
		}
	}
	while (hn > 0)
	{
		int			best = heap[0];
		int			c = 0;

		CHECK_FOR_INTERRUPTS();	/* per merged posting; no lock held (chain released above) */
		if (nout == 0 ||
			ItemPointerCompare(&tids[nout - 1], RUN_HEAD(best)) != 0)
			tids[nout++] = *RUN_HEAD(best);
		pos[best]++;
		if (pos[best] >= runlen[best])
		{
			heap[0] = heap[--hn];	/* drop exhausted run */
		}
		/* sift down heap[0] */
		for (;;)
		{
			int			l = 2 * c + 1,
						ri = 2 * c + 2,
						sm = c;

			if (hn == 0)
				break;
			if (l < hn && HEAP_LESS(l, sm))
				sm = l;
			if (ri < hn && HEAP_LESS(ri, sm))
				sm = ri;
			if (sm == c)
				break;
			{
				int			t = heap[c];

				heap[c] = heap[sm];
				heap[sm] = t;
				c = sm;
			}
		}
	}
#undef RUN_HEAD
#undef HEAP_LESS
	pfree(pos);
	pfree(heap);
	out->tids = tids;
	out->n = nout;
}

/*
 * weave_fuzzy_terms -- collect the postings of every dictionary term within edit
 * distance k of `term`, by running the universal-Levenshtein core
 * (include/weave/uleven.h) over the segment's dictionary chain presented as a
 * WeaveUlevVocab.  EXACT in BOTH directions, so no heap recheck is needed --
 * unlike the trigram funnel in src/pages/trgm_page.c, which over-generates
 * candidates that must be re-verified per document.  Returns true; *out is a
 * sorted, de-duplicated TidSet.  Returns false when the core cannot serve the
 * input, so the caller falls back to that funnel.
 *
 * WHAT CHANGED WHEN THIS STOPPED USING include/weave/lev.h, because it changes
 * which rows a query returns and no ASCII test can see it.  lev.h's automaton
 * counts BYTE edits; this one counts CHARACTER edits (WEAVE_ULEVEN_UTF8).
 * Substituting one two-byte character for another is ONE character edit and TWO
 * byte edits, so under the old matcher `naive~1` did not match `naïve` while
 * `levenshtein('naive','naïve') = 1` says it should.  That was a FALSE NEGATIVE,
 * and the recheck could not repair it: this route is exact, so nothing rechecks.
 * It was also an internal disagreement -- weave_doc_has_fuzzy()
 * (src/query/doc.c) has always used core's varstr_levenshtein_less_equal(),
 * which counts characters, so the same query answered from a wdoc value and from
 * the index disagreed on non-ASCII input.  sql/fuzzyuleven.sql pins both halves.
 *
 * KNOWN GAP, and it is not a regression: WEAVE_ULEVEN_UTF8 decodes UTF-8.  On a
 * multi-byte server encoding that is not UTF-8 (EUC_JP, SJIS) the core's decoder
 * escapes each byte on its own, so the unit is effectively the byte there -- the
 * same answer the old walk gave, still short of levenshtein()'s pg_mblen()
 * characters.  Closing it means a third WeaveUlevUnit, not a change here.
 */
static bool
weave_fuzzy_terms(Relation index, const WeaveSegMeta *seg,
				 const char *term, int termlen, int k, TidSet *out)
{
	WeaveUlevAut aut;
	WeaveUlevVocab voc;
	WeaveDictVocab dv;
	WeaveTermRuns h;

	/*
	 * THE APPLICABILITY TEST IS NOW THE CORE'S OWN INIT, and the bound it
	 * enforces is not the one this function used to advertise.  lev.h bounded
	 * the query at WEAVE_LEV_MAXQ = 255 BYTES; weave_uleven_init() bounds it at
	 * WEAVE_ULEVEN_MAX_UNITS = 255 edit UNITS (and k at WEAVE_ULEVEN_MAX_K =
	 * 255).  Under UTF-8 that is strictly more permissive -- a 400-byte term of
	 * 150 characters is now served exactly here instead of being handed to the
	 * over-generating funnel -- so asking the core rather than re-deriving a
	 * byte threshold is both correct and the only way the two stay in step.
	 * Refused rather than truncated: a truncated query accepts a different
	 * language, and the difference lands in the false-negative direction.
	 */
	if (weave_uleven_init(&aut, term, (size_t) termlen, k,
						  pg_database_encoding_max_length() == 1 ?
						  WEAVE_ULEVEN_BYTE : WEAVE_ULEVEN_UTF8) != WEAVE_ULEVEN_OK)
		return false;			/* fall back to trigram funnel + recheck */

	/*
	 * THE FUZZY MECHANISM.  Counted after the applicability test above, not
	 * before it, because a term the core refuses leaves through `return false`
	 * and is served by the trigram funnel instead -- counting on entry would
	 * attribute the funnel's work to this route.  See include/weave/weave.h for
	 * why these counters exist.
	 */
	weave_chan_fuzzy_dict++;

	dv.index = index;
	dv.seg = seg;
	dv.blk = seg->dictstart;
	dv.buf = InvalidBuffer;
	dv.ptr = dv.end = NULL;
	dv.nextblk = InvalidBlockNumber;
	dv.cur = NULL;
	dv.nyielded = 0;
	dv.nextkey = (unsigned char *) palloc(BLCKSZ);

	voc.next = weave_dictvocab_next;
	voc.skip = weave_dictvocab_skip;
	voc.arg = &dv;

	weave_termruns_init(&h, index, seg, &dv);

	PG_TRY();
	{
		(void) weave_uleven_expand_vocab(&aut, &voc, weave_fuzzy_hit, &h,
										 NULL, NULL);
	}
	PG_FINALLY();
	{
		/*
		 * The iterator holds a pinned, share-locked buffer between next()
		 * calls, so an ERROR or a cancel inside the callback (weave_decode_term
		 * reads more pages; WEAVE_ALLOC_MAYBE_HUGE can throw) would leak the
		 * lock out of the query.  The walk this replaced could not have this
		 * problem because its buffer lifetime was a lexical block.
		 */
		weave_dictvocab_release(&dv);
	}
	PG_END_TRY();
	pfree(dv.nextkey);

	weave_termruns_merge(&h, out);
	return true;
}

/* -------------------------------------------------------------------------
 * The regex route: an EXACT dictionary-side walk, narrowed by the trigram weft
 * when the index has one.
 *
 * WHAT IT REPLACED, AND WHY THE REPLACEMENT IS EXACT WHERE THE OLD ROUTE WAS
 * NOT.  A regex leaf used to go to weave_trgm_candidates() with a literal-run
 * scanner (weave_regex_trigrams, deleted) supplying "required" trigrams, then
 * every candidate ROW was rechecked on the heap by weave_doc_has_regex().  Two
 * things were wrong with that shape.  The scanner read `\\d` as the literal `d`
 * and required a trigram no matching term contains, so `/ab\\dcd/` returned no
 * rows from the index while the heap predicate matched `ab5cd` -- a FALSE
 * NEGATIVE (G32) that no recheck can repair, because a recheck only
 * removes rows.  And the recheck ran per DOCUMENT, so a class pattern such as
 * /e12[0-9]{2}/ -- no literal run of three, so nothing to funnel -- fell back
 * to weave_universe_bounded(), i.e. every document in the segment, at ~5 s per
 * query on 1M rows.
 *
 * The regex is a predicate on TERMS, so it is decided on the dictionary, once
 * per distinct term, with the SAME engine, flags and collation as
 * weave_doc_has_regex() (pg_regcomp/pg_regexec, REG_ADVANCED, C_COLLATION_OID):
 * the index and the heap predicate agree by construction, and the route needs no
 * recheck -- like weave_fuzzy_terms() above, and unlike the funnel.
 *
 * THE NARROWING IS A PURE OPTIMISATION AND MUST STAY ONE.  With a trigram weft
 * present, the pg_tre extractor (src/query/extract.c) turns the pattern into a
 * CNF of trigrams every matching string must contain, and the walk runs the
 * engine only over terms whose ordinal survives that CNF.  A trigram wrongly
 * "required" is a false negative the exact walk cannot see, so the narrowing is
 * refused wherever pg_tre's dialect and core's ARE could disagree
 * (weave_regex_narrowable); removing that refusal makes /\yabc\y/ lose rows in
 * sql/regexdict.sql, which is the mutation that shows it is load-bearing.
 * ------------------------------------------------------------------------- */

/*
 * weave_regex_narrowable -- may the pg_tre extractor's "required trigrams" be
 * trusted for THIS pattern as core's ARE engine will read it?
 *
 * pg_tre's tokenizer (src/query/regex_tokens.c) is a subset dialect: its
 * `default:` case reads an unknown escape as the escaped character literally
 * (so `\\d` is `d`, `\\y` is `y`, `\\1` is `1`), it has no `(?flags)` or `(?:`
 * groups, no POSIX classes ([:digit:]), collating elements ([.x.]) or
 * equivalence classes ([=x=]), reads a `]` right after `[` as closing an empty
 * class where ARE reads it as a member, and reads `{~k}` as its own approximate
 * bound where ARE reads a literal.  Each of those can make it publish a literal
 * run -- hence a required trigram -- that ARE never demands.  The scan is over
 * the RAW pattern, before parsing, because a whitelist of what the two dialects
 * agree on is checkable by reading; a blacklist of what they disagree on is
 * checkable only by having found every disagreement.
 *
 * Also refused: a non-ASCII byte in the pattern on a server encoding other than
 * UTF-8.  pg_tre decodes the pattern as UTF-8 unconditionally, and the weft is
 * keyed by SERVER-ENCODED bytes, so re-encoding its codepoints would produce
 * keys the weft never stored.  ASCII round-trips through every server encoding.
 */
static bool
weave_regex_narrowable(const char *re, int relen)
{
	bool		utf8 = (GetDatabaseEncoding() == PG_UTF8);
	int			i;

	/* ARE directors (***: / ***=) change the dialect of everything after them */
	if (relen >= 3 && memcmp(re, "***", 3) == 0)
		return false;

	for (i = 0; i < relen; i++)
	{
		unsigned char c = (unsigned char) re[i];
		unsigned char n = (i + 1 < relen) ? (unsigned char) re[i + 1] : 0;

		if (c >= 0x80 && !utf8)
			return false;
		switch (c)
		{
			case '\\':
				/* the escapes both dialects read as "that character, literally" */
				if (n == 0 || strchr("\\.[](){}|*+?^$-", n) == NULL)
					return false;
				i++;			/* consumed the escaped character */
				break;
			case '(':
				if (n == '?')
					return false;
				break;
			case '[':
				if (n == ':' || n == '.' || n == '=' || n == ']')
					return false;
				if (n == '^' && i + 2 < relen && re[i + 2] == ']')
					return false;
				break;
			case '{':
				if (n == '~')
					return false;
				break;
			default:
				break;
		}
	}
	return true;
}

/* Merge-intersection of two ascending unique uint64 arrays into a new one. */
static uint64 *
weave_ords_and(const uint64 *a, int na, const uint64 *b, int nb, int *nout)
{
	uint64	   *r = palloc(Max(Min(na, nb), 1) * sizeof(uint64));
	int			i = 0,
				j = 0,
				k = 0;

	while (i < na && j < nb)
	{
		if (a[i] < b[j])
			i++;
		else if (a[i] > b[j])
			j++;
		else
		{
			r[k++] = a[i];
			i++;
			j++;
		}
	}
	*nout = k;
	return r;
}

/* Merge-union of two ascending unique uint64 arrays into a new one. */
static uint64 *
weave_ords_or(const uint64 *a, int na, const uint64 *b, int nb, int *nout)
{
	/* alloc-ok: two vocabulary-bounded ordinal sets; query path */
	uint64	   *r = palloc(Max(na + nb, 1) * sizeof(uint64));
	int			i = 0,
				j = 0,
				k = 0;

	while (i < na || j < nb)
	{
		if (j >= nb || (i < na && a[i] < b[j]))
			r[k++] = a[i++];
		else if (i >= na || b[j] < a[i])
			r[k++] = b[j++];
		else
		{
			r[k++] = a[i];
			i++;
			j++;
		}
	}
	*nout = k;
	return r;
}

/*
 * weave_regex_narrow -- the candidate TERM ORDINALS for a regex leaf, from the
 * segment's trigram weft, or false when the pattern gives nothing to narrow on.
 *
 * regex_extract_query() at max_cost 0 yields CNF: the query is the AND of its
 * conjuncts, a conjunct the OR of its alternatives, and each alternative names
 * one CODEPOINT trigram (TrigramDisjunct.cp) every matching string contains.
 * The weft is keyed by BYTE trigrams (weave_trigrams over the server-encoded
 * term), so each codepoint triple is re-encoded and its byte trigrams -- all of
 * them, because a term containing the codepoints contiguously contains every
 * byte trigram of their encoding -- are ANDed.  A trigram with no directory
 * entry is the EMPTY set, which is exact: weave_write_trigrams_iter skips
 * nothing, so "no entry" means "no indexed term contains it".
 *
 * *cands is ascending and unique, in the caller's memory context; everything
 * the parse allocated is freed here.
 */
static bool
weave_regex_narrow(Relation index, const WeaveSegMeta *seg,
				   const char *re, int relen, uint64 **cands, int *ncands)
{
	MemoryContext cxt,
				old;
	WeaveParseCtx pctx;
	TrigramQuery tq;
	uint64	   *result = NULL;
	int			nresult = 0;
	bool		have_result = false;
	int			ci;

	*cands = NULL;
	*ncands = 0;
	if (!weave_regex_narrowable(re, relen))
		return false;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "weave regex narrowing",
								ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(cxt);
	if (!weave_parse_regex(&pctx, re, relen) ||
		!regex_extract_query(&pctx, 0, &tq) ||
		tq.always_true || tq.n <= 0 || tq.mode != TRIGRAM_QUERY_CNF)
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
		return false;
	}

	for (ci = 0; ci < tq.n && (!have_result || nresult > 0); ci++)
	{
		const TrigramConjunct *c = &tq.conjuncts[ci];
		uint64	   *cunion = NULL;
		int			ncunion = 0;
		int			ai;

		for (ai = 0; ai < c->n; ai++)
		{
			const TrigramDisjunct *d = &c->alts[ai];
			pg_wchar	cps[3];
			char		bytes[3 * MAX_MULTIBYTE_CHAR_LEN + 1];
			int			nbytes;
			uint32		trg[WEAVE_MAX_TRIGRAMS];
			int			ntrg;
			uint64	   *inter = NULL;
			int			ninter = 0;
			int			ti;

			cps[0] = (pg_wchar) d->cp[0];
			cps[1] = (pg_wchar) d->cp[1];
			cps[2] = (pg_wchar) d->cp[2];
			nbytes = pg_wchar2mb_with_len(cps, bytes, 3);
			ntrg = weave_trigrams(bytes, nbytes, trg, WEAVE_MAX_TRIGRAMS);
			/* AND across the alternative's byte trigrams */
			for (ti = 0; ti < ntrg; ti++)
			{
				uint64	   *one;
				int			none;

				(void) weave_trgm_ordinals(index, seg->trgmstart, trg[ti],
										   &one, &none);
				if (ti == 0)
				{
					inter = one;
					ninter = none;
				}
				else
				{
					inter = weave_ords_and(inter, ninter, one, none, &ninter);
				}
				if (ninter == 0)
					break;
			}
			/* OR across the conjunct's alternatives */
			cunion = (ai == 0) ? inter :
				weave_ords_or(cunion, ncunion, inter, ninter, &ncunion);
			if (ai == 0)
				ncunion = ninter;
		}
		/* AND across conjuncts */
		if (!have_result)
		{
			result = cunion;
			nresult = ncunion;
			have_result = true;
		}
		else
			result = weave_ords_and(result, nresult, cunion, ncunion, &nresult);
	}
	MemoryContextSwitchTo(old);

	if (nresult > 0)
	{
		/* alloc-ok: a vocabulary-bounded ordinal set; query path */
		*cands = palloc(nresult * sizeof(uint64));
		memcpy(*cands, result, nresult * sizeof(uint64));
	}
	*ncands = nresult;
	MemoryContextDelete(cxt);
	return true;
}

/*
 * weave_regex_terms -- collect the postings of every dictionary term matching
 * the regular expression `re`, by compiling it once and running core's engine
 * over the segment's dictionary chain -- narrowed to the terms the trigram weft
 * says can match when the index has one.  EXACT in both directions, so no heap
 * recheck is needed.  Returns true; *out is a sorted, de-duplicated TidSet.
 * Returns false only when the segment has no dictionary.
 *
 * SAME ENGINE, SAME ANSWER.  The pattern is compiled with pg_regcomp() on its
 * pg_wchar form under REG_ADVANCED and C_COLLATION_OID, which is precisely what
 * weave_doc_has_regex() asks RE_compile_and_execute() for; a compile failure is
 * reported the way core's ~ reports it.  The engine is the only thing that
 * decides a match here -- the narrowing above only decides which terms it is
 * asked about -- so `(?i)`, `\\d`, `\\y`, [[:digit:]] and every other ARE
 * construct mean exactly what they mean to `~`.
 *
 * THE DICTIONARY IS FOLDED.  Terms went through the text search configuration
 * at index time (doc/specs/FUZZY_CHANNEL.md sect. 3.2), and the pattern text is
 * NOT folded (src/query/parse.c: "read until the closing slash"), so /ABC/
 * matches nothing while /(?i)ABC/ and /abc/ match a document that said "ABC".
 * That is the same answer weave_doc_has_regex() gives against the same folded
 * terms, and it is a property of the channel, not of this route.
 *
 * WHAT IS COUNTED.  weave_chan_regex_dict once per call that serves the leaf
 * (i.e. once per segment per leaf, like fuzzy_dict); weave_chan_regex_trgm once
 * per call in which the weft narrowing was applied -- the funnel's old meaning,
 * "the weft narrowed the candidates", kept.  Both can be nonzero for one leaf.
 * terms_expanded counts terms the engine accepted; dict_pages the pages read.
 */
static bool
weave_regex_terms(Relation index, const WeaveSegMeta *seg,
				  const char *re, int relen, TidSet *out)
{
	regex_t		cre;
	pg_wchar   *wpat;
	int			wpatlen;
	int			rc;
	WeaveDictVocab dv;
	WeaveTermRuns h;
	pg_wchar   *wterm;
	uint64	   *cands = NULL;	/* candidate ordinals, or NULL: every term */
	int			ncands = 0;
	bool		narrowed = false;

	out->tids = NULL;
	out->n = 0;
	if (seg->dictstart == InvalidBlockNumber)
		return false;

	/*
	 * Compile ONCE per (segment, leaf), not once per term: 260k terms is 260k
	 * compiles otherwise, and core's own cache (RE_compile_and_cache) is not
	 * reachable from here without going through text datums per term.
	 */
	wpat = (pg_wchar *) palloc((relen + 1) * sizeof(pg_wchar));
	wpatlen = pg_mb2wchar_with_len(re, wpat, relen);
	rc = pg_regcomp(&cre, wpat, wpatlen, REG_ADVANCED, C_COLLATION_OID);
	pfree(wpat);
	if (rc != REG_OKAY)
	{
		char		errMsg[100];

		/* re did not compile: no pg_regfree needed, same as core */
		pg_regerror(rc, &cre, errMsg, sizeof(errMsg));
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression: %s", errMsg)));
	}

	/* THE REGEX-DICTIONARY MECHANISM: counted once the route has committed. */
	weave_chan_regex_dict++;

	dv.index = index;
	dv.seg = seg;
	dv.blk = seg->dictstart;
	dv.buf = InvalidBuffer;
	dv.ptr = dv.end = NULL;
	dv.nextblk = InvalidBlockNumber;
	dv.cur = NULL;
	dv.nyielded = 0;
	dv.nextkey = NULL;			/* skip() is the automaton's; this walk has none */
	weave_termruns_init(&h, index, seg, &dv);

	/*
	 * A term's pg_wchar form is at most its byte length in characters, and
	 * weave_dict_entry_fits() bounds a term by the page, so one BLCKSZ buffer
	 * serves every term of the walk without a per-term allocation.
	 */
	wterm = (pg_wchar *) palloc((BLCKSZ + 1) * sizeof(pg_wchar));

	PG_TRY();
	{
		const char *term;
		weave_ul_uint32 len,
					ord;
		int			ci = 0;

		if (seg->trgmstart != InvalidBlockNumber)
			narrowed = weave_regex_narrow(index, seg, re, relen, &cands, &ncands);
		if (narrowed)
			weave_chan_regex_trgm++;	/* the weft narrowed the candidates */

		/*
		 * The walk itself.  weave_dictvocab_next() is the fuzzy walk's iterator
		 * and carries every guard that walk documents (InvalidBuffer = end of
		 * chain, weave_dict_entry_fits before termlen, weave_page_entry_end,
		 * CHECK_FOR_INTERRUPTS between pages with no lock held); `ord` is the
		 * yield count, which is the ordinal the weft was written with.  An
		 * empty candidate set means no term can match and the chain is not
		 * read at all.
		 */
		while ((!narrowed || ci < ncands) &&
			   weave_dictvocab_next(&dv, &term, &len, &ord))
		{
			int			wlen;

			if (narrowed)
			{
				/* both ascending: advance past ordinals the walk never yielded */
				while (ci < ncands && cands[ci] < (uint64) ord)
					ci++;
				if (ci >= ncands)
					break;
				if (cands[ci] != (uint64) ord)
					continue;
				ci++;
			}

			wlen = pg_mb2wchar_with_len(term, wterm, (int) len);
			rc = pg_regexec(&cre, wterm, wlen, 0, NULL, 0, NULL, 0);
			if (rc == REG_NOMATCH)
				continue;
			if (rc != REG_OKAY)
			{
				char		errMsg[100];

				/* REG_CANCEL arrives here; let the cancel win if it is one */
				CHECK_FOR_INTERRUPTS();
				pg_regerror(rc, &cre, errMsg, sizeof(errMsg));
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression failed: %s", errMsg)));
			}
			weave_chan_terms_expanded++;
			weave_termruns_add_current(&h);
		}
	}
	PG_FINALLY();
	{
		/*
		 * The iterator holds a pinned, share-locked buffer between next() calls
		 * and the compiled regex is malloc'd, not palloc'd: an ERROR or a cancel
		 * anywhere above (the narrowing allocates, weave_decode_term reads more
		 * pages, the engine can report REG_CANCEL) would otherwise leak both out
		 * of the query.
		 */
		weave_dictvocab_release(&dv);
		pg_regfree(&cre);
	}
	PG_END_TRY();
	pfree(wterm);
	if (cands)
		pfree(cands);

	weave_termruns_merge(&h, out);
	return true;
}

/* Build the universe: the set of DISTINCT TIDs present in any posting list of
 * the segment.  Used by top-level NOT and by the regex/fuzzy fallback when no
 * trigram acceleration is available.
 *
 * The distinct TID count is bounded by the segment's live+dead doc count
 * (ndocs), but the raw postings summed across every term total Sum(df) -- the
 * whole expanded inverted index, often two orders of magnitude larger than
 * ndocs.  Accumulating all of them before a single final sort_uniq made the
 * scratch buffer grow to Sum(df) TIDs and could reach a multi-gigabyte
 * "invalid memory alloc request size" on a high-vocabulary corpus.  We instead
 * fold (sort_uniq) whenever the buffer grows past a small multiple of ndocs,
 * so peak memory stays O(ndocs) regardless of Sum(df). */
static TidSet
weave_universe_bounded(Relation index, BlockNumber dictstart, double ndocs,
					  bool has_doclen_col)
{
	TidSet		u;
	BlockNumber blk = dictstart;
	int			cap = 64;
	int			n = 0;
	ItemPointerData *tids = palloc(cap * sizeof(ItemPointerData));	/* alloc-ok: guarded by the explicit cap*2*sizeof > MaxAllocSize ereport just above */

	/* Fold threshold: once the buffer holds more than fold_at raw TIDs,
	 * sort_uniq collapses it back to the <= ndocs distinct ones.  A distinct
	 * count can never exceed ndocs, so this keeps peak memory O(ndocs) instead
	 * of O(Sum(df)).  Guard against a bogus/zero ndocs with a floor. */
	int			fold_at;

	{
		double		f = ndocs * 2.0 + 1024.0;

		if (f > (double) TIDSET_MAX_N)
			f = (double) TIDSET_MAX_N;
		fold_at = (f < 1024.0) ? 1024 : (int) f;
	}

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
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
			int			np;
			int			k;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
			np = weave_decode_term(index, de->firstposting,
								  de->firstoffset, de->df,
								  &post, NULL, false, NULL, true,
								  has_doclen_col);

			for (k = 0; k < np; k++)
			{
				if (n >= cap)
				{
					/* Fold before growing past the ndocs-relative threshold:
					 * collapse duplicates so the buffer stays O(ndocs), never
					 * O(Sum df).  After folding, either there is now room (the
					 * common case -- fall through to store) or the distinct set
					 * itself is genuinely near cap, in which case we grow. */
					if (n >= fold_at)
					{
						u.tids = tids;
						u.n = n;
						tidset_sort_uniq(&u);
						tids = u.tids;
						n = u.n;
					}
					if (n >= cap)
					{
						if ((Size) cap * 2 * sizeof(ItemPointerData) > MaxAllocSize)
							ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("pg_weave: candidate set for this query is too large"),
									 errhint("Build the index WITH (trigrams = on) so fuzzy/regex/NOT queries can be accelerated.")));
						cap *= 2;
						tids = repalloc(tids, cap * sizeof(ItemPointerData));	/* alloc-ok: guarded by the explicit cap*2*sizeof > MaxAllocSize ereport just above */
					}
				}
				tids[n++] = post[k].tid;
			}
			pfree(post);
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		blk = next;
	}

	u.tids = tids;
	u.n = n;
	tidset_sort_uniq(&u);
	return u;
}

IndexScanDesc
weave_beginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(r, nkeys, norderbys);
	WeaveScanOpaque so = (WeaveScanOpaque) palloc0(sizeof(WeaveScanOpaqueData));

	so->query = NULL;
	so->queryValid = false;
	so->orderInit = false;
	so->ordered = NULL;
	so->nordered = 0;
	so->maxordered = 0;
	so->ordpos = 0;
	so->cand = NULL;
	so->ncand = 0;
	so->candpos = 0;
	so->candfull = false;
	so->curk = 0;
	so->maxhits = 0;
	so->plainInit = false;
	so->plainTids = NULL;
	so->nplain = 0;
	so->plainpos = 0;
	so->plainRecheck = false;
	so->edistScan = false;
	so->edistPat = NULL;
	so->edistPatLen = 0;
	so->edistThr = 0;
	so->edistNext = INT_MAX;
	so->edistDone = false;
	so->vecScan = false;
	so->vecQuery = NULL;
	so->vecAttno = 0;
	so->veck = 0;
	so->vecLanes = 0;
	so->vecDone = false;
	so->cgramScan = false;
	so->cgramPat = NULL;
	so->cgramPatLen = 0;
	so->cgramCI = false;
	so->cgramLossy = false;
	so->fuseScan = false;
	so->fuseQ = NULL;
	so->fuseW = NULL;
	so->nfuse = 0;
	so->fusek = 0;
	so->fuseDone = false;
	scan->opaque = so;
	/* the AM owns allocation of the order-by result arrays */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals = palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = palloc(sizeof(bool) * norderbys);
	}
	return scan;
}

void
weave_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			ScanKey orderbys, int norderbys)
{
	WeaveScanOpaque so = (WeaveScanOpaque) scan->opaque;

	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));

	so->queryValid = false;
	so->cgramScan = false;
	so->cgramPat = NULL;
	so->cgramPatLen = 0;
	so->cgramCI = false;
	so->cgramLossy = false;
	if (scan->numberOfKeys >= 1)
	{
		/*
		 * WHICH KIND OF RESTRICTION KEY IS THIS?  Dispatch on the CHANNEL of the
		 * key's index attribute, NOT on sk_strategy: strategy numbers are scoped
		 * to an operator family, so gram_ops's `@~` and wdoc_lex_ops's `@@@` are
		 * both strategy 1.  Reading the wrong one is not a type error --
		 * DatumGetWQuery() on a text datum interprets a varlena header as a
		 * WeaveQuery and walks garbage -- which is the same hazard, and the same
		 * remedy, as the ORDER BY dispatch below.
		 *
		 * THE WHOLE KEY ARRAY IS SCANNED, not just keyData[0], because
		 * amcanmulticol is true: the planner may hand this AM
		 * `d @@@ q AND body @~ p` as two keys in either order.  Honouring one and
		 * silently discarding the other returns rows that fail the discarded
		 * clause -- and for a non-lossy bitmap the executor does not re-check an
		 * index qual, so nothing downstream would catch it.  The first cgram key
		 * becomes the route; ANY other key sets cgramLossy, which turns the
		 * returned set into an admitted superset with recheck on.
		 */
		WeaveIndexLayout layout;
		int			k;

		weave_index_layout(scan->indexRelation, &layout);
		for (k = 0; k < scan->numberOfKeys; k++)
		{
			AttrNumber	att = scan->keyData[k].sk_attno;
			bool		iscgram = (att >= 1 && att <= layout.nkeys &&
								   layout.kind[att - 1] == (uint16) WEAVE_WK_CGRAM);

			if (iscgram && !so->cgramScan)
			{
				MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(so));
				text	   *pat = DatumGetTextPP(scan->keyData[k].sk_argument);

				so->cgramPatLen = (int) VARSIZE_ANY_EXHDR(pat);
				so->cgramPat = (char *) palloc(so->cgramPatLen + 1);	/* alloc-ok: one query pattern, bounded by the query text */
				memcpy(so->cgramPat, VARDATA_ANY(pat), so->cgramPatLen);
				so->cgramPat[so->cgramPatLen] = '\0';
				so->cgramCI = (scan->keyData[k].sk_strategy ==
							   WEAVE_STRAT_CGRAM_ILIKE);
				so->cgramScan = true;
				/*
				 * The pattern IS the query on this path; so->query stays NULL and
				 * queryValid keeps its existing meaning ("a WHERE clause supplied
				 * a wquery"), which here it did not.
				 */
				MemoryContextSwitchTo(old);
			}
			else if (!iscgram && !so->queryValid)
			{
				so->query = DatumGetWQuery(scan->keyData[k].sk_argument);
				so->queryValid = true;
			}
			else
				so->cgramLossy = true;	/* a key this scan does not honour */
		}

		/*
		 * A cgram key WINS over a lexical one when both are present, and the
		 * lexical clause becomes the executor's business (cgramLossy is already
		 * set by the loop, because whichever came second fell into the `else`).
		 * Fusing the two sets is Phase F's job, not this task's -- doing it here
		 * would be the fused scorer built against a half-working channel that
		 * AGENTS.md hard rule 7 forbids.
		 */
		if (so->cgramScan && so->queryValid)
		{
			so->queryValid = false;
			so->query = NULL;
			so->cgramLossy = true;
		}
	}

	/* ordering scan: the query is the <=> operator's right operand */
	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));
	so->orderInit = false;
	so->ordered = NULL;
	so->nordered = 0;
	so->maxordered = 0;
	so->ordpos = 0;
	so->cand = NULL;
	so->ncand = 0;
	so->candpos = 0;
	so->candfull = false;
	so->curk = 0;
	so->maxhits = 0;
	so->plainInit = false;
	so->plainTids = NULL;
	so->nplain = 0;
	so->plainpos = 0;
	so->plainRecheck = false;
	so->edistScan = false;
	so->edistPat = NULL;
	so->edistPatLen = 0;
	so->edistThr = 0;
	so->edistNext = INT_MAX;
	so->edistDone = false;
	/*
	 * F7's per-rescan state, reset HERE with all the rest and not lazily on first
	 * use.  A rescan that leaks one field from the previous scan is a wrong answer
	 * that appears only under a nested loop -- the outer row changes, the inner
	 * scan keeps the previous query vector or the previous ladder width, and every
	 * arm of the join returns plausible rows.  sql/vecorderby.sql drives a
	 * correlated subquery for exactly this.
	 */
	so->vecScan = false;
	so->vecQuery = NULL;
	so->vecAttno = 0;
	so->veck = 0;
	so->vecLanes = 0;
	so->vecDone = false;
	/*
	 * F2.2's per-rescan state, reset HERE with all the rest and for the reason the
	 * paragraph above gives: a nested loop whose inner scan kept the previous
	 * outer row's queries or the previous rung's width returns plausible rows on
	 * every arm of the join.  sql/fuse_pushdown.sql drives a correlated subquery
	 * for exactly this.
	 */
	so->fuseScan = false;
	so->fuseQ = NULL;
	so->fuseW = NULL;
	so->nfuse = 0;
	so->fusek = 0;
	so->fuseDone = false;
	if (scan->numberOfOrderBys >= 1)
	{
		/*
		 * WHICH order-by operator this is decides how to read its argument, and
		 * reading it wrong is not a type error -- DatumGetWQuery() on a text
		 * datum would interpret a varlena header as a WeaveQuery and walk
		 * garbage.  So dispatch on sk_strategy, and treat an unknown strategy as
		 * an error rather than as one of the known ones.
		 *
		 * The vector members are tested FIRST because one of their numbers is the
		 * one shared with another family's member.  WEAVE_STRAT_VEC_L2 is 1, which
		 * wdoc_lex_ops spends on `@@@`; `@@@` is a restriction operator and so can
		 * never arrive in orderByData, which is exactly why 1 was chosen for a
		 * vector member (include/weave/am.h).  WEAVE_STRAT_VEC_IP is 4 and is
		 * shared with nothing.  The strategy is therefore unambiguous here, and
		 * reading a wvec datum as a wvec is safe because the operator's left
		 * argument type is what put the key on this column.
		 *
		 * WHICH COLUMN it names is answered one level down instead of here, and
		 * that is a deliberate choice rather than an omission: sk_attno is carried
		 * into so->vecAttno and weave_vec_topk_run() scores only a weft whose
		 * channel DESCRIPTOR records that attribute.  The descriptor's attnum is
		 * the stronger check -- it catches a weft WRITTEN against the wrong column,
		 * which is the mutation include/weave/vector.h says the field exists to
		 * catch, and which a catalog-derived comparison here would miss -- and it
		 * is also what makes the no-vector-channel and no-vector-weft cases FALL
		 * THROUGH to zero rows rather than to an error: no bolt matches, the pass
		 * returns nothing, and an ordering path over a channel this index does not
		 * carry is a legitimate plan over an empty channel.
		 */
		if (weave_fuse_rescan(scan, so))
		{
			/*
			 * A FUSED scan (task F2.2), recognized by the weights transport key
			 * standing beside two or more scored keys.  Tested FIRST because the
			 * dispatch below reads orderByData[0] ALONE, and in a fused shape that
			 * key is one channel of several: honouring it and discarding the rest
			 * would answer a fused ordering with a single-channel one -- plausible
			 * rows in a ranking nobody asked for, which is the failure class this
			 * project treats as worse than an error.
			 *
			 * weave_fuse_rescan() returns false when no transport key is present,
			 * and RAISES when there is one it cannot honour.  A raise here is a bug
			 * report about src/am/fusepath.c rather than anything a user did: that
			 * file's hard invariant is that it never offers a path this function
			 * refuses, and doc/GAPS.md G39 is what breaking it looks like.
			 */
		}
		else if (scan->orderByData[0].sk_strategy == WEAVE_STRAT_VEC_L2 ||
			scan->orderByData[0].sk_strategy == WEAVE_STRAT_VEC_IP)
		{
			bool		wantip = (scan->orderByData[0].sk_strategy ==
								  WEAVE_STRAT_VEC_IP);
			int			want = wantip ? WEAVE_METRIC_IP : WEAVE_METRIC_L2;
			int			have = weave_index_vec_metric(scan->indexRelation);
			MemoryContext old;
			WVec	   *q;

			/*
			 * THE OPERATOR NAMES A METRIC, THE WEFT IS SCORED IN EXACTLY ONE, AND A
			 * MISMATCH IS REFUSED RATHER THAN ANSWERED.  Serving an l2 ordering
			 * under `<#>` (or an ip one under `<->`) is a WRONG ANSWER, not an
			 * approximation like the quantizer's reordering is: every row comes back
			 * in a ranking the query did not ask for, plausibly, with no error.
			 * That is the defect F7 shipped -- it made `<=>` the member while the
			 * weft ordered by `metric` -- so the refusal is the correction.
			 *
			 * WHY THIS IS A RUNTIME ERROR AND NOT A PLANNER DECISION.  The planner
			 * matches a pathkey against an operator FAMILY, and the metric is not in
			 * the family -- it is a reloption, which path generation never looks at.
			 * So there is no point at which the planner could decline to build this
			 * path, and the last place that can still refuse is the scan.  The fix
			 * that turns this into a planner decision is the one
			 * doc/specs/VECTOR_CHANNEL.md sect. 8b names: ONE OPERATOR FAMILY PER
			 * METRIC (wvec_l2_ops, wvec_ip_ops), so an `ip` index simply has no
			 * `<->` member and no path is generated at all.  The metric belongs in
			 * the opclass, the way pgvector does it, and not in a reloption the
			 * planner cannot see.
			 *
			 * THIS CHECK ONLY EVER SEPARATES l2 FROM ip.  A cosine or l1 index
			 * cannot exist: weave_index_vec_metric() refuses both at CREATE INDEX
			 * (expected/vecscan.out lines 303 and 307), because neither has a sound
			 * compressed-domain bound.  The scan core serves IP and L2 and refuses
			 * the rest (include/weave/vecscan.h), which is also why `<=>` is not a
			 * member of any family on wvec.
			 *
			 * WHERE THE METRIC IS READ FROM, and it is the weaker of the two
			 * available authorities because it is the only one reachable here: the
			 * `metric` RELOPTION.  The scoring authority is WeaveVecMeta.metric,
			 * recorded per weft, and no weft is open at rescan time -- bolts are
			 * visited by weave_vec_topk_run() off a metapage snapshot, one level
			 * down and once per pass.  The two can legitimately DISAGREE: `metric`
			 * carries AccessExclusiveLock, so `ALTER INDEX ... SET (metric = ...)`
			 * is accepted WITHOUT a REINDEX and rewrites nothing, leaving every
			 * existing weft scored in the old metric.  That is a second, independent
			 * reason the reloption is the wrong home for this, and it is why the
			 * per-family split above is the fix rather than a deeper check here: a
			 * check against each weft's own metric could only turn the same
			 * disagreement into a per-bolt error, mid-scan.
			 */
			if (have != want)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("operator %s cannot order a weave index whose metric is %s",
								wantip ? "<#>" : "<->",
								have == WEAVE_METRIC_IP ? "ip" : "l2"),
						 errdetail("The vector weft is scored in one metric only, so an ordering by %s would be returned in %s order.",
								   wantip ? "<#>" : "<->",
								   have == WEAVE_METRIC_IP ? "ip" : "l2"),
						 errhint("Order by %s instead, or build the index WITH (metric = '%s').",
								 have == WEAVE_METRIC_IP ? "<#>" : "<->",
								 wantip ? "ip" : "l2")));

			old = MemoryContextSwitchTo(GetMemoryChunkContext(so));
			q = DatumGetWVec(scan->orderByData[0].sk_argument);

			/*
			 * COPIED, not aliased.  DatumGetWVec() hands back the datum itself
			 * when it is not toasted, sk_argument belongs to the executor's
			 * ScanKey, and every rung of the widening ladder re-reads the vector
			 * from weave_gettuple() -- arbitrarily later than this call.
			 */
			so->vecQuery = (WVec *) palloc(VARSIZE_ANY(q));	/* alloc-ok: one query vector, bounded by WVEC_MAX_DIM */
			memcpy(so->vecQuery, q, VARSIZE_ANY(q));
			so->vecAttno = scan->orderByData[0].sk_attno;
			so->vecScan = true;
			/*
			 * The vector IS the query on this path; so->query stays NULL and
			 * queryValid keeps its existing meaning ("a WHERE clause supplied a
			 * wquery"), which for a bare `ORDER BY v <-> c` it did not.
			 */
			MemoryContextSwitchTo(old);
		}
		else if (scan->orderByData[0].sk_strategy == WEAVE_STRAT_EDIST)
		{
			MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(so));
			text	   *pat = DatumGetTextPP(scan->orderByData[0].sk_argument);

			so->edistPatLen = (int) VARSIZE_ANY_EXHDR(pat);
			so->edistPat = (char *) palloc(so->edistPatLen + 1);	/* alloc-ok: one query pattern, bounded by the query text */
			memcpy(so->edistPat, VARDATA_ANY(pat), so->edistPatLen);
			so->edistPat[so->edistPatLen] = '\0';
			so->edistScan = true;
			/*
			 * The pattern IS the query on this path; so->query stays NULL and
			 * queryValid says only "a WHERE clause supplied a wquery", which for
			 * a bare `ORDER BY d <@> p` it did not.
			 */
			MemoryContextSwitchTo(old);
		}
		else if (scan->orderByData[0].sk_strategy == WEAVE_STRAT_DISTANCE)
		{
			so->query = DatumGetWQuery(scan->orderByData[0].sk_argument);
			so->queryValid = true;
		}
		else
			elog(ERROR, "weave: unsupported ORDER BY strategy %d",
				 (int) scan->orderByData[0].sk_strategy);
	}
}

/*
 * weave_canreturn: whether the index can return a column value for an
 * index-only scan.  The weave index is NOT covering, so it cannot -- see the
 * body.
 */
bool
weave_canreturn(Relation index, int attno)
{
	/*
	 * The weave index is NOT covering: it stores analyzed postings, not the
	 * original wdoc, so it cannot reproduce a column value.  Returning true
	 * caused an index-only scan that SELECTs the indexed column to yield NULLs
	 * (a placeholder tuple).  Return false so the planner never uses an
	 * index-only scan to fetch a real attribute.  (count(*)/EXISTS need no
	 * attribute but still include the @@@ restriction column in the IOS
	 * coverage check, so they run through a bitmap/plain index scan; our
	 * visibility-map-aware weave_count() is the explicit fast count.)
	 */
	return false;
}

/*
 * Fill scan->xs_itup with a cached all-NULL index tuple if the executor ever
 * requests one for an index-only scan (xs_want_itup).  Currently inactive:
 * weave_canreturn() returns false, so the planner never chooses an index-only
 * scan and xs_want_itup is never set -- this is a guarded no-op kept so the
 * gettuple paths remain correct if a covering capability is ever added.
 */
static inline void
weave_set_itup(IndexScanDesc scan, WeaveScanOpaque so)
{
	if (!scan->xs_want_itup)
		return;
	if (so->plainItup == NULL)
	{
		TupleDesc	td = RelationGetDescr(scan->indexRelation);
		Datum	   *values = palloc0(sizeof(Datum) * td->natts);
		bool	   *isnull = palloc(sizeof(bool) * td->natts);
		int			a;

		for (a = 0; a < td->natts; a++)
			isnull[a] = true;
		so->plainItup = index_form_tuple(td, values, isnull);
		so->plainItupDesc = td;
		pfree(values);
		pfree(isnull);
	}
	scan->xs_itup = so->plainItup;
	scan->xs_itupdesc = so->plainItupDesc;
}

/*
 * weave_gettuple: ordering scan for ORDER BY (wdoc <=> wquery) LIMIT k, and since
 * tasks Z9 and F7 also for (wdoc <@> text) and (wvec <=> wvec).
 * On the first call it computes the channel's top-k (visibility-filtered)
 * into scan state, then returns tuples one per call in ascending distance
 * (descending relevance), setting xs_orderbyvals so the executor can honor the
 * ORDER BY without a sort.  Only forward scans are supported.
 */
bool
weave_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	WeaveScanOpaque so = (WeaveScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "weave: only forward ordering scans are supported");

	/*
	 * amoptionalkey is true (see weave_handler), so the planner may hand us a
	 * scan with no restriction clause.  That is intended: it is how the keyless
	 * ordering scan works, and weave_rescan takes the query from the order-by
	 * argument in that case.
	 *
	 * But a scan with NEITHER a scan key NOR an order-by has no query at all,
	 * and there is nothing correct to return.  Silently returning zero rows
	 * would make such a plan produce an empty result set -- a wrong answer, not
	 * a slow one -- so fail loudly.  Reachable via a partial index, where a
	 * useful predicate alone can justify a path.
	 */
	if (scan->numberOfKeys == 0 && scan->numberOfOrderBys == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a weave index scan requires a query"),
				 errdetail("The scan has neither a @@@ restriction nor an ORDER BY <=> ordering clause."),
				 errhint("Add \"WHERE col @@@ query\" or \"ORDER BY col <=> query\".")));

	if (so->fuseScan)
	{
		/*
		 * F2.2: the fused path has SEVERAL queries and no single so->query, so
		 * every test below would reject it.  Tested before the other three
		 * because a fused scan may also carry a `@@@` restriction key, which set
		 * queryValid, and may carry order-by keys whose strategies the three
		 * below recognize.
		 */
		if (so->fuseQ == NULL || so->nfuse < 2)
			return false;
	}
	else if (so->vecScan)
	{
		/*
		 * F7: the `<=>` vector path has a query VECTOR instead of a wquery, so
		 * everything below that tests so->query would reject it.  Tested before
		 * the other two because a vector ordering scan may also carry a `@@@`
		 * restriction key, which would have set queryValid.
		 */
		if (so->vecQuery == NULL)
			return false;
	}
	else if (so->edistScan)
	{
		/*
		 * The <@> path has a pattern instead of a wquery; everything below that
		 * tests so->query would reject it.
		 */
		if (so->edistPat == NULL)
			return false;
	}
	else if (so->cgramScan)
	{
		/* Z8: same shape -- a raw LIKE pattern, no wquery. */
		if (so->cgramPat == NULL)
			return false;
	}
	else if (!so->queryValid || so->query == NULL)
		return false;

	/*
	 * Plain scan (no ORDER BY <=>): stream the matching TIDs in heap order for
	 * a plain Index Scan.  (The common @@@ path is the bitmap scan via
	 * amgetbitmap; this amgettuple path serves a plain index scan when the
	 * planner chooses one, e.g. with bitmap scans disabled.)  The matches come
	 * from the same evaluator, so results are identical; the executor applies
	 * MVCC visibility on the heap fetch.
	 */
	if (scan->numberOfOrderBys == 0)
	{
		if (!so->plainInit)
		{
			TidSet		m;

			pgstat_count_index_scan(scan->indexRelation);
			if (so->cgramScan)
			{
				/* The cgram route returns an EXACT set (it rechecks on the heap
				 * itself), so plainRecheck stays false and a plain Index Scan
				 * and a Bitmap Heap Scan return the same rows -- which is what
				 * lets sql/cgram.sql assert parity without pinning a plan. */
				(void) weave_cgram_collect(scan->indexRelation, so->cgramPat,
										   so->cgramPatLen, so->cgramCI, &m);
				so->plainRecheck = so->cgramLossy;
			}
			else
				weave_collect_matches(scan->indexRelation, so->query, &m, &so->plainRecheck);
			so->plainTids = m.tids;
			so->nplain = m.n;
			so->plainpos = 0;
			so->plainInit = true;
		}
		if (so->plainpos >= so->nplain)
			return false;
		scan->xs_heaptid = so->plainTids[so->plainpos++];
		scan->xs_recheck = so->plainRecheck;
		weave_set_itup(scan, so);
		return true;
	}

	if (!so->orderInit)
	{
		if (so->fuseScan)
		{
			/*
			 * Task F2.2.  The ladder's rungs are CANDIDATE WIDTHS, the lexical and
			 * vector shape, and it exists for the reason both of those state:
			 * PostgreSQL gives an access method no way to learn the query's LIMIT,
			 * so CORRECTNESS MUST NOT DEPEND ON THE PLANNER'S LIMIT ESTIMATE.  The
			 * first rung is the same weave_ord_width() over-fetch of
			 * pg_weave.wand_initial_k the other two use -- one knob for "how wide is
			 * the first rung of an ordering pass", because two knobs drift -- and
			 * weave_fuse_grow() widens x4 whenever the executor drains what is
			 * materialized.
			 *
			 * maxhits is the union bound over the fused queries: each query's
			 * provable match ceiling, summed, is a ceiling on the set of documents
			 * ANY of them reaches, which is the fused candidate set
			 * (include/weave/fuse.h: the union of the scored channels' positions,
			 * intersected with the required ones').  Summing over-counts documents
			 * several queries match, which is the safe direction -- it can only
			 * make the cheap early stop fire later.
			 */
			double		N;
			WeaveMetaPageData m0;
			int			qi;

			pgstat_count_index_scan(scan->indexRelation);
			weave_read_meta(scan->indexRelation, &m0);
			N = m0.ndocs < 1.0 ? 1.0 : m0.ndocs;
			so->maxhits = 0.0;
			for (qi = 0; qi < so->nfuse; qi++)
			{
				/*
				 * A VECTOR CHANNEL'S MATCH CEILING IS EVERY DOCUMENT, and it gets
				 * `N` rather than a call to weave_query_maxhits() -- which takes a
				 * WeaveQuery and would be handed NULL, since a vector key's query is
				 * a WVec (task F8).  That NULL was a segfault in the first cut of
				 * this loop, reached from the flagship two-channel query and from
				 * nothing else: the lexical-only shapes F2.2 shipped never produce a
				 * NULL here, so the whole regression suite passed over it.
				 *
				 * N is not a placeholder, it is the right number.  A vector shuttle
				 * publishes a position for EVERY live lane of every bolt it reaches
				 * -- there is no query-dependent posting list to bound it with -- so
				 * "every document" is its provable ceiling, and this sum is a union
				 * bound whose over-counting only makes the cheap early stop fire
				 * later.
				 */
				if (so->fuseStrat[qi] == WEAVE_STRAT_DISTANCE)
					so->maxhits += weave_query_maxhits(scan->indexRelation,
													   so->fuseQ[qi], N);
				else
					so->maxhits += N;
			}
			so->fusek = weave_ord_width(pg_weave_wand_initial_k);
			weave_fuse_pass(scan->indexRelation, so);
			so->ordpos = 0;
			so->orderInit = true;
		}
		else if (so->vecScan)
		{
			/*
			 * Task F7.  The vector ladder's rungs are CANDIDATE WIDTHS, the same
			 * shape as the lexical one and for the same reason: PostgreSQL gives an
			 * access method no way to learn the query's LIMIT, so
			 * CORRECTNESS MUST NOT DEPEND ON THE PLANNER'S LIMIT ESTIMATE.  The
			 * scan starts at a width chosen for the first page an interactive
			 * `ORDER BY v <=> $1 LIMIT 10` asks for, and weave_vec_grow() widens x4
			 * whenever the executor drains what is materialized -- a cursor, a
			 * LIMIT-less query, or a LIMIT the planner under-estimated all reach the
			 * same code, and an amcanorderbyop scan that stopped at a ceiling of its
			 * own would silently truncate the result (see weave_ord_grow).
			 *
			 * The initial width is pg_weave.wand_initial_k through the same
			 * weave_ord_width() over-fetch the lexical pass uses, rather than a
			 * constant or a second GUC.  One knob for "how wide is the first rung of
			 * an ordering pass" is a knob a benchmark can sweep; two are a pair that
			 * drifts, and the x4 over-fetch is there for the same reason in both
			 * channels -- MVCC filtering, which discards candidates after scoring.
			 */
			pgstat_count_index_scan(scan->indexRelation);
			so->veck = weave_ord_width(pg_weave_wand_initial_k);
			weave_vec_pass(scan->indexRelation, so);
			so->ordpos = 0;
			so->orderInit = true;
		}
		else if (so->edistScan)
		{
			/*
			 * Task Z9.  The narrowest useful threshold is 0 -- documents holding
			 * the pattern verbatim -- because that is where the channel's block
			 * bound prunes hardest, and the first page of an interactive `<@>`
			 * query is usually satisfied by distance 0 or 1.  If it is not, the
			 * pass reports the exact next distance at which a row can appear and
			 * weave_edist_grow() jumps straight to it, so no threshold is visited
			 * that cannot produce a row.  The cost this shape carries is one
			 * dictionary walk per DISTINCT distance the executor drains, which an
			 * impact-ordered dictionary would remove and which nothing in the
			 * current format supports.
			 */
			pgstat_count_index_scan(scan->indexRelation);
			so->edistThr = 0;
			weave_edist_pass(scan->indexRelation, so);
			so->ordpos = 0;
			so->orderInit = true;
		}
		else
		{
			/*
			 * Adaptive-width WAND.  Start narrow so a small LIMIT (the common first
			 * page) does minimal work -- WAND prunes hard for a small k -- and widen
			 * on demand.  The pass's candidates are handed out LAZILY (visibility is
			 * checked a batch at a time in weave_ord_probe), so the whole width of
			 * the pass is available to the executor, not just the first
			 * pg_weave.wand_initial_k rows of it: the x4 over-fetch that used to
			 * exist only to survive MVCC filtering now also sets how deep a single
			 * pass can serve.  See weave_ord_pass and doc/GAPS.md G13.
			 */
			double		N;
			WeaveMetaPageData m0;

			pgstat_count_index_scan(scan->indexRelation);
			weave_read_meta(scan->indexRelation, &m0);
			N = m0.ndocs < 1.0 ? 1.0 : m0.ndocs;
			so->maxhits = weave_query_maxhits(scan->indexRelation, so->query, N);
			/*
			 * Initial WAND k.  PostgreSQL gives an access method no way to learn the
			 * query's LIMIT, so the scan starts at some k and grows x4 on demand.
			 *
			 * This value was 100, chosen so the whole LIMIT 11..100 range is served by
			 * ONE pass rather than a pass-then-recompute.  The competitive benchmark
			 * showed what that costs: pg_weave's ranked latency is IDENTICAL at
			 * LIMIT 10 and LIMIT 100 -- measured k100/k10 ratios of 1.003, 1.007 and
			 * 0.999 across the rare, mid and common bands -- because a LIMIT 10 query
			 * does a k=100 pass.  Timescale pg_textsearch, over the same corpus and
			 * query shape, scales 1.9x-4.1x with k and is 10-21x faster at k=10.
			 * A top-k engine that does not get cheaper as k shrinks is not pruning for
			 * the dominant query shape, which is a first page of ten results.
			 *
			 * Made a GUC so the trade can be swept in one benchmark run instead of
			 * guessed at: bench/compete sweeps it and bench/RESULTS_WAND_K.md records
			 * the frontier the default is chosen from.  See doc/GAPS.md G13.
			 */
			so->curk = weave_ord_width(pg_weave_wand_initial_k);
			weave_ord_pass(scan->indexRelation, so);
			so->ordpos = 0;
			so->orderInit = true;
		}
	}

	/*
	 * Produce the next visible row.  Three sources, in increasing cost:
	 *
	 *  1. already materialized in ordered[] -- free;
	 *  2. the current pass's remaining candidates -- one heap probe each;
	 *  3. a wider pass (weave_ord_grow / weave_edist_grow / weave_vec_grow) -- a
	 *     full recompute of the channel's top-k.
	 *
	 * (3) EXTENDS the scan rather than repeating it: ordered[] survives, so the
	 * rows already handed out are neither re-probed nor re-emitted.
	 */
	while (so->ordpos >= so->nordered)
	{
		if (so->candpos < so->ncand)
		{
			/*
			 * Probe a geometrically growing batch rather than exactly one row,
			 * so the heap open and fetch setup amortize over the rows that
			 * follow.  batch > nordered always (ordpos >= nordered here), so
			 * each iteration consumes at least one candidate and cannot spin.
			 */
			int			batch = so->nordered < INT_MAX / 2 ? so->nordered * 2 : INT_MAX;

			batch = Max(batch, 16);
			batch = Max(batch, so->ordpos + 1);
			weave_ord_probe(scan->indexRelation, so, batch);
			if (so->ordpos < so->nordered)
				break;
		}
		/* candidates exhausted: widen the pass, or the scan is complete */
		if (!(so->fuseScan ? weave_fuse_grow(scan->indexRelation, so)
			  : so->vecScan ? weave_vec_grow(scan->indexRelation, so)
			  : so->edistScan ? weave_edist_grow(scan->indexRelation, so)
			  : weave_ord_grow(scan->indexRelation, so)))
			return false;
	}

	scan->xs_heaptid = so->ordered[so->ordpos].tid;
	scan->xs_recheck = false;	/* score computed exactly from the index */
	if (so->edistScan && scan->numberOfKeys > 0)
		/*
		 * A `WHERE d @@@ q ORDER BY d <@> p` scan pushed the @@@ key down, and
		 * an Index Scan does not re-evaluate a pushed-down qual unless the AM
		 * asks.  weave_edist_pass() restricted its hits to the @@@ match set,
		 * which for a fuzzy/regex/NOT query is the OVER-generating set (the same
		 * one amgetbitmap reports recheck for), so say so and let the executor
		 * re-run the original qual against the heap tuple.
		 */
		scan->xs_recheck = so->plainRecheck;
	if (so->fuseScan && scan->numberOfKeys > 0)
		/*
		 * `WHERE d @@@ q ORDER BY fuse(...)` pushed the @@@ key down, and unlike
		 * the vector case below the fused pass DID honour it -- as a REQUIRED gate
		 * channel inside the scan, which is the conjunctive-gate path and the whole
		 * point of fusing a predicate with a ranking (include/weave/fuse.h note 2).
		 * So recheck carries only what the gate's own key set carries: the set came
		 * from weave_collect_matches(), which OVER-generates for a fuzzy, regex or
		 * phrase query (the same set amgetbitmap reports recheck for), and an Index
		 * Scan does not re-evaluate a pushed-down qual unless the AM asks.  Exactly
		 * the `<@>` arrangement, deliberately: one flag, set by one collector.
		 */
		scan->xs_recheck = so->plainRecheck;
	if (so->vecScan && scan->numberOfKeys > 0)
		/*
		 * `WHERE d @@@ q ORDER BY v <=> $1` pushed the @@@ key down and the vector
		 * pass DID NOT HONOUR IT: fusing a lexical restriction into the vector
		 * channel's top-k is the fused scorer, i.e. Phase F, and building a second
		 * one here is what AGENTS.md hard rule 7 forbids.  So the returned set is an
		 * admitted SUPERSET of the answer and recheck goes out as true, which makes
		 * the executor re-evaluate the original qual against the heap tuple.  The
		 * result is still COMPLETE rather than merely correct, because the executor
		 * discarding rows drives the ladder on: weave_vec_grow() widens until it can
		 * prove there is nothing further, not until it has produced k rows.
		 */
		scan->xs_recheck = true;
	weave_set_itup(scan, so);
	if (scan->numberOfOrderBys > 0)
	{
		/*
		 * ONE ENTRY PER ORDER-BY KEY, AND IT MUST BE EXACTLY THAT MANY.
		 * index_store_float8_orderby_distances() loops over
		 * scan->numberOfOrderBys and reads distances[i] for each one, so the
		 * single-element array this used to pass is only correct while every
		 * ordering shape has exactly one key.  A FUSED scan (task F2.2) has one
		 * key per channel PLUS the weights transport key, so that array would be
		 * read off the end -- an out-of-bounds stack read whose symptom is a
		 * plausible float, which is the class doc/CONVENTIONS.md rule 2 refuses.
		 *
		 * The fused value belongs to the ordering AS A WHOLE and not to any one
		 * key, so it goes in slot 0 and the rest are NULL.  Nothing reads them:
		 * xs_recheckorderby is false on every path here, and the only consumer of
		 * these values is the executor's reorder queue, which exists only for a
		 * recheck.  INDEX_MAX_KEYS bounds numberOfOrderBys, so the arrays are
		 * stack-sized without an allocation.
		 */
		IndexOrderByDistance dist[INDEX_MAX_KEYS];
		Oid			typ[INDEX_MAX_KEYS];
		int			k;

		for (k = 0; k < scan->numberOfOrderBys; k++)
		{
			typ[k] = FLOAT8OID;
			dist[k].value = 0.0;
			dist[k].isnull = (k > 0);
		}
		dist[0].value = so->ordered[so->ordpos].score;
		dist[0].isnull = false;
		index_store_float8_orderby_distances(scan, typ, dist, false);
	}
	so->ordpos++;
	return true;
}

/*
 * ------------------- positional phrase evaluation (from postings) -------------
 *
 * When the index is built WITH (positions=on), a phrase/NEAR query can be
 * answered DIRECTLY from the posting lists -- no heap access, no recheck.  We
 * intersect the phrase's terms on docid (cheap and selective), then verify
 * adjacency per surviving docid using the stored token positions via the shared
 * weave_phrase_step_pos() -- the exact adjacency logic the heap recheck uses, so
 * the result is identical.  need_recheck is then false and the recheck cliff is
 * gone.
 *
 * This fast path handles a PURE PHRASE CHAIN: an RPN of plain (non prefix/
 * fuzzy/regex) term operands combined only by WEAVE_OP_PHRASE -- i.e. the shape
 * to_wquery produces for "a b c" and NEAR(a b c, k).  Anything mixing phrase
 * with boolean AND/OR/NOT, or a phrase term that is a prefix/fuzzy/regex, is
 * left to the existing AND + recheck path (still correct, just slower).
 */

/* one term's postings decoded with positions, docid-sorted */
typedef struct PosPosting
{
	uint64		docid;
	uint32	   *pos;			/* tf ascending positions (into an arena) */
	int			npos;
}			PosPosting;

typedef struct PosTermList
{
	PosPosting *posts;
	int			nposts;
	WeavePosting *raw;			/* decoder output (owns tids/pos slots) */
	uint32	   *arena;			/* positions arena to free */
}			PosTermList;

/*
 * Is the query a pure phrase chain we can evaluate positionally?  Returns the
 * ordered list of term-operand indices (into query->items) via *termidx and
 * their count via *nterms, plus the max phrase distance (all PHRASE ops share
 * the chain).  Requires: items are VAL/PHRASE only, every VAL is a plain term
 * (no PREFIX/FUZZY/REGEX), and the RPN is the canonical left-deep phrase chain
 * (v1 v2 PHRASE v3 PHRASE ...).  For NEAR the per-op distance may differ per
 * step; we carry each step's distance in *dist[].
 */
static bool
weave_phrase_chain(WeaveQuery q, int *termidx, uint32 *stepdist, int *nterms)
{
	int			nt = 0;
	uint32		i;
	int			stack = 0;

	if (q->nitems < 3)
		return false;			/* need at least v v PHRASE */

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX))
				return false;
			if (nt >= WEAVE_QUERY_MAX_PHRASE_TERMS)
				return false;
			/*
			 * Only the CANONICAL LEFT-DEEP chain (v1 v2 PHRASE v3 PHRASE ...)
			 * is safe here: it evaluates as phrase(phrase(v1,v2),v3), pairing
			 * consecutive terms -- which is what "a b c" emits and what this
			 * loop's left-to-right stepdist chaining computes.  NEAR(a b c,k)
			 * instead emits all terms first then the PHRASE ops (v1 v2 v3
			 * PHRASE PHRASE), which the recheck stack machine evaluates
			 * RIGHT-deep as phrase(v1,phrase(v2,v3)) -- a different match set.
			 * A VAL pushed while >=2 operands are already pending is that
			 * non-left-deep shape: bail to the (correct) recheck path.
			 */
			if (stack >= 2)
				return false;
			termidx[nt++] = (int) i;
			stack++;
		}
		else if (it->type == WEAVE_QI_OPR && it->op == WEAVE_OP_PHRASE)
		{
			if (stack < 2)
				return false;
			/* step k joins term (nt-1) to its predecessor: record its distance */
			stepdist[nt - 2] = it->distance;
			stack--;			/* phrase collapses two operands to one */
		}
		else
			return false;		/* any boolean op / other operator: not pure */
	}
	*nterms = nt;
	return (stack == 1 && nt >= 2);
}

static int
cmp_pospost_docid(const void *a, const void *b)
{
	uint64		da = ((const PosPosting *) a)->docid;
	uint64		db = ((const PosPosting *) b)->docid;

	return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/*
 * Look up one term in a segment and decode its postings WITH positions,
 * docid-sorted.  Returns:
 *   WEAVE_POSLOOKUP_OK      -- found, positions present (out is populated)
 *   WEAVE_POSLOOKUP_ABSENT  -- term not in this segment (clean empty phrase)
 *   WEAVE_POSLOOKUP_NOPOS   -- found but a block dropped positions (Sum(tf)
 *                             overflowed a page): caller MUST fall back to the
 *                             recheck path for correctness.
 */
typedef enum
{
	WEAVE_POSLOOKUP_OK = 0,
	WEAVE_POSLOOKUP_ABSENT,
	WEAVE_POSLOOKUP_NOPOS
}			WeavePosLookup;

static WeavePosLookup
weave_lookup_term_pos(Relation index, const WeaveSegMeta *seg,
					 const char *term, int termlen, PosTermList *out)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	out->posts = NULL;
	out->nposts = 0;
	out->raw = NULL;
	out->arena = NULL;

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber firstposting = InvalidBlockNumber;
		uint32		firstoffset = 0;
		uint32		df = 0;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				firstposting = de->firstposting;
				firstoffset = de->firstoffset;
				df = de->df;
				found = true;
				break;
			}
			ptr += esize;
		}
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buffer);

		if (found)
		{
			WeavePosting *post;
			uint32	   *arena = NULL;
			int			np = weave_decode_term(index, firstposting, firstoffset,
											  df, &post, NULL, true, &arena, false,
										  seg->doclenstart == InvalidBlockNumber);
			PosPosting *pp = (PosPosting *) palloc(Max(np, 1) * sizeof(PosPosting));
			int			k;

			for (k = 0; k < np; k++)
			{
				if (post[k].pos == NULL && post[k].tf > 0)
				{
					/* a block dropped positions: cannot verify adjacency here */
					pfree(pp);
					pfree(post);
					if (arena)
						pfree(arena);
					return WEAVE_POSLOOKUP_NOPOS;
				}
				pp[k].docid = weave_tid_to_docid(&post[k].tid);
				pp[k].pos = post[k].pos;
				pp[k].npos = (int) post[k].tf;
			}
			if (np > 1)
				qsort(pp, np, sizeof(PosPosting), cmp_pospost_docid);
			out->posts = pp;
			out->nposts = np;
			out->raw = post;
			out->arena = arena;
			return WEAVE_POSLOOKUP_OK;
		}
		if (onlyone)
			break;
	}
	return WEAVE_POSLOOKUP_ABSENT;	/* term absent in this segment */
}

static void
weave_free_posterm(PosTermList *pl)
{
	if (pl->posts)
		pfree(pl->posts);
	if (pl->raw)
		pfree(pl->raw);
	if (pl->arena)
		pfree(pl->arena);
	pl->posts = NULL;
	pl->raw = NULL;
	pl->arena = NULL;
	pl->nposts = 0;
}

/* find the PosPosting for docid via binary search; NULL if absent */
static PosPosting *
weave_pospost_find(PosTermList *pl, uint64 docid)
{
	int			lo = 0,
				hi = pl->nposts - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (pl->posts[mid].docid < docid)
			lo = mid + 1;
		else if (pl->posts[mid].docid > docid)
			hi = mid - 1;
		else
			return &pl->posts[mid];
	}
	return NULL;
}

/*
 * Evaluate a pure phrase chain positionally over one segment, appending exact
 * matches to *out (a growable TidSet-like buffer).  Returns true on success;
 * false means "fall back to recheck" (a term lacked positions).  seg_tombs/s
 * are used to drop tombstoned docids from this segment's contribution.
 */
static bool
weave_phrase_eval_seg(Relation index, const WeaveSegMeta *seg, WeaveQuery q,
					 const int *termidx, const uint32 *stepdist, int nterms,
					 ItemPointerData **tids, int *ntids, int *captids)
{
	PosTermList *tl = (PosTermList *) palloc0(nterms * sizeof(PosTermList));	/* alloc-ok: nterms = query term count */
	int			t;
	int			base = -1;		/* index of the smallest-df (driving) term */
	PosPosting *driver;
	int			di;
	bool		ok = true;
	uint32	   *acc = (uint32 *) palloc(WEAVE_PHRASE_POSBUF * sizeof(uint32));
	uint32	   *tmp = (uint32 *) palloc(WEAVE_PHRASE_POSBUF * sizeof(uint32));

	for (t = 0; t < nterms; t++)
	{
		WeaveQueryItem *it = &q->items[termidx[t]];
		WeavePosLookup rc = weave_lookup_term_pos(index, seg,
												WEAVE_QUERY_ITEMTEXT(q, it),
												it->termlen, &tl[t]);

		if (rc == WEAVE_POSLOOKUP_NOPOS)
		{
			ok = false;			/* found but positions missing: fall back to recheck */
			goto done;
		}
		if (rc == WEAVE_POSLOOKUP_ABSENT)
			goto done;			/* term absent: phrase cannot match this segment */
		if (base < 0 || tl[t].nposts < tl[base].nposts)
			base = t;
	}

	/* drive the docid intersection from the smallest posting list */
	driver = tl[base].posts;
	for (di = 0; di < tl[base].nposts; di++)
	{
		uint64		docid = driver[di].docid;
		PosPosting *pp[WEAVE_QUERY_MAX_PHRASE_TERMS];
		bool		allpresent = true;
		int			nacc;
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();	/* per driver posting; posting lists in memory, no lock held */

		for (t = 0; t < nterms; t++)
		{
			pp[t] = (t == base) ? &driver[di] : weave_pospost_find(&tl[t], docid);
			if (pp[t] == NULL)
			{
				allpresent = false;
				break;
			}
		}
		if (!allpresent)
			continue;

		/* chain phrase_step across the terms: acc starts as term 0's positions */
		if (pp[0]->npos > WEAVE_PHRASE_POSBUF)
		{
			ok = false;			/* pathological tf: fall back (bounded buffer) */
			goto done;
		}
		memcpy(acc, pp[0]->pos, pp[0]->npos * sizeof(uint32));
		nacc = pp[0]->npos;
		for (t = 1; t < nterms && nacc > 0; t++)
		{
			int			nout = 0;

			if (pp[t]->npos > WEAVE_PHRASE_POSBUF)
			{
				ok = false;
				goto done;
			}
			weave_phrase_step_pos(acc, nacc, pp[t]->pos, pp[t]->npos,
								stepdist[t - 1], tmp, &nout);
			memcpy(acc, tmp, nout * sizeof(uint32));
			nacc = nout;
		}
		if (nacc > 0)
		{
			weave_docid_to_tid(docid, &tid);
			if (*ntids >= *captids)
			{
				*captids = Max(*captids * 2, 16);
				/* corpus-scale (one matching tid per doc); query path */
				*tids = WEAVE_REALLOC_MAYBE_HUGE(*tids, (Size) *captids * sizeof(ItemPointerData));
			}
			(*tids)[(*ntids)++] = tid;
		}
	}

done:
	for (t = 0; t < nterms; t++)
		weave_free_posterm(&tl[t]);
	pfree(tl);
	pfree(acc);
	pfree(tmp);
	return ok;
}

/*
 * weave_collect_matches: evaluate the scan's query across all segments + the
 * pending list; return matching TIDs (sorted, unique) and a *recheck flag
 * (true iff any term used the over-generating trigram funnel / NOT-universe
 * path, or the query mixes several fuzzy/regex leaves).  Shared by the bitmap scan and the plain gettuple scan.
 */
static void
weave_collect_matches(Relation index, WeaveQuery query, TidSet *out, bool *recheck)
{
	WeaveMetaPageData meta;
	TidSet		acc;
	TidSet		pending_acc;
	WeaveTombstones seg_tombs;
	bool		has_fuzzy_regex = false;
	bool		has_not = false;
	bool		has_phrase = false;
	bool		has_weighted = false;
	bool		need_recheck = false;
	bool		use_pos_phrase = false;	/* positional phrase fast path applies */
	int			pterm[WEAVE_QUERY_MAX_PHRASE_TERMS];
	uint32		pstep[WEAVE_QUERY_MAX_PHRASE_TERMS] = {0};
	int			npterm = 0;
	ItemPointerData *ptids = NULL;
	int			nptids = 0;
	int			captids = 0;
	uint32		i;
	uint32		s;
	uint32		gen0;			/* directory generation at the metapage snapshot */
	int			gen_retries = 0;

	acc.tids = NULL;
	acc.n = 0;
	*recheck = false;
	if (query == NULL)
	{
		*out = acc;
		return;
	}

	weave_read_meta(index, &meta);

#ifdef WEAVE_TEST_HOOKS
	/*
	 * TEST-ONLY window: after snapshotting the segment directory but before
	 * reading any segment page, briefly acquire+release the configured advisory
	 * lock.  A blocker session holding pg_advisory_lock(key) stalls the scan
	 * here while a merger frees the snapshotted segment and an inserter recycles
	 * its pages -- exposing the A1 scan-vs-merge recycle race deterministically.
	 * No effect when the key is 0 (default / production).
	 */
	if (pg_weave_test_pause_advisory_key != 0)
	{
		LOCKTAG		tag;
		int64		k = pg_weave_test_pause_advisory_key;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId,
							 (uint32) (k >> 32), (uint32) k, 1);
		(void) LockAcquire(&tag, ShareLock, true, false);
		LockRelease(&tag, ShareLock, true);
	}
#endif

collect_retry:
	gen0 = meta.generation;
	acc.tids = NULL;
	acc.n = 0;

	for (i = 0; i < query->nitems; i++)
	{
		WeaveQueryItem *it = &query->items[i];

		if (it->type == WEAVE_QI_OPR && it->op == WEAVE_OP_NOT)
			has_not = true;
		if (it->type == WEAVE_QI_OPR && it->op == WEAVE_OP_PHRASE)
			has_phrase = true;
		if (it->type == WEAVE_QI_VAL && (it->flags & (WEAVE_QF_FUZZY | WEAVE_QF_REGEX)))
			has_fuzzy_regex = true;
		if (it->type == WEAVE_QI_VAL && (it->flags & WEAVE_QF_WEIGHTED))
			has_weighted = true;
	}

	/*
	 * A weight-restricted (term:LABEL) query cannot be answered from the index
	 * alone -- the posting positions carry only the ordinal, not the field
	 * label (labels live in the heap wdoc value).  So the index over-generates
	 * (every doc containing the term) and the heap recheck applies the label
	 * filter, exactly as it does for fuzzy/regex.  Force recheck.
	 */
	if (has_weighted)
		need_recheck = true;

	/*
	 * Positional phrase fast path: if the index carries token positions
	 * (WITH positions=on) and the query is a pure phrase chain, evaluate it
	 * DIRECTLY from the posting lists -- intersect on docid, verify adjacency
	 * from the stored positions -- with NO heap access and need_recheck=false.
	 * This is the cliff fix.  When positions are off, or the phrase mixes with
	 * boolean operators, we keep the AND + heap-recheck path below (correct,
	 * slower).
	 */
	if (has_phrase && !has_fuzzy_regex && !has_not && !has_weighted &&
		weave_index_wants_positions(index) &&
		weave_phrase_chain(query, pterm, pstep, &npterm))
		use_pos_phrase = true;

	/*
	 * Load per-segment tombstones once.  Each segment's match contribution is
	 * filtered against THAT segment's own tombstone map before being unioned,
	 * because a heap TID deleted in one segment may be reused by a live doc in
	 * another segment or the pending list.
	 */
	weave_tombstones_load(index, &meta, &seg_tombs);

	for (s = 0; s < meta.nsegments; s++)
	{
		WeaveSegMeta *sg = &meta.segs[s];
		TidSet		universe;			/* only for the fuzzy/regex fallback below */
		UniverseSrc uni;				/* lazy source for the boolean evaluator */

		CHECK_FOR_INTERRUPTS();	/* per segment; no lock/window held (meta is in memory) */
		if (sg->dictstart == InvalidBlockNumber)
			continue;

		if (has_fuzzy_regex)
		{
			TidSet		cands;
			bool		any_trgm = false;
			bool		exact = (query->nitems == 1);
			uint32		qi;

			cands.tids = NULL;
			cands.n = 0;
			for (qi = 0; qi < query->nitems; qi++)
			{
				WeaveQueryItem *it = &query->items[qi];
				TidSet		ts;

				if (it->type != WEAVE_QI_VAL ||
					!(it->flags & (WEAVE_QF_FUZZY | WEAVE_QF_REGEX)))
					continue;

				if (it->flags & WEAVE_QF_FUZZY)
				{
					if (weave_fuzzy_terms(index, sg,
										 WEAVE_QUERY_ITEMTEXT(query, it),
										 it->termlen, (int) it->distance, &ts))
					{
						cands = tidset_or(cands, ts);
						any_trgm = true;
						continue;
					}
				}
				else
				{
					/*
					 * REGEX IS EXACT, LIKE FUZZY, AND DOES NOT CLEAR `exact`.
					 * weave_regex_terms() decides every dictionary term with
					 * the same engine weave_doc_has_regex() would use on the
					 * heap, so the candidate set IS the answer and a recheck
					 * would only re-derive it per row.  It declines only for a
					 * segment with no dictionary, which the `continue` at the
					 * top of this loop already skipped -- so the fallback
					 * below is the universe, never the trigram funnel: the
					 * funnel's trigrams come from weave_trigrams() over the
					 * raw text, and a pattern's raw text is not a set of
					 * trigrams every match contains.
					 */
					if (weave_regex_terms(index, sg,
										  WEAVE_QUERY_ITEMTEXT(query, it),
										  it->termlen, &ts))
					{
						cands = tidset_or(cands, ts);
						any_trgm = true;
						continue;
					}
					any_trgm = false;
					break;
				}
				exact = false;

				/*
				 * THE FUNNEL'S MINIMUM TRIGRAM COUNT DEPENDS ON k, and must.
				 * weave_trgm_candidates() keeps only terms sharing at least one
				 * trigram with the query, which is sound only while k edits
				 * cannot destroy them all -- and one edit destroys THREE
				 * trigrams (the ones starting at p-2, p-1 and p), so the bound
				 * is 3k+1, not the flat 3 this passed before.  With k = 1 a
				 * five-byte term has exactly 3 trigrams, cleared the old gate,
				 * and a single substitution in its middle destroyed every one of
				 * them: the funnel then returned candidates that did not include
				 * the matching row, and since this route is the INEXACT one its
				 * heap recheck can only remove rows, never restore them.
				 * Refusing (returning false here) costs a full-dictionary scan
				 * and keeps the answer right.
				 *
				 * Only fuzzy reaches this path now, and only when
				 * weave_fuzzy_terms() above declines (a query over
				 * WEAVE_ULEVEN_MAX_UNITS units, or k over WEAVE_ULEVEN_MAX_K),
				 * so the added scans are rare by construction.
				 */
				if (weave_trgm_candidates(index, sg->trgmstart,
										 sg->dictstart,
										 WEAVE_QUERY_ITEMTEXT(query, it),
										 it->termlen,
										 3 * (int) it->distance + 1,
										 sg->doclenstart == InvalidBlockNumber, &ts))
				{
					cands = tidset_or(cands, ts);
					any_trgm = true;
				}
				else
				{
					any_trgm = false;
					break;
				}
			}
			if (any_trgm)
			{
				if (!exact)
					need_recheck = true;
				if (cands.n > 0)
				{
					weave_filter_tombstoned_seg(&seg_tombs, s, &cands);
					if (cands.n > 0)
						acc = tidset_or(acc, cands);
				}
			}
			else
			{
				/*
				 * No trigram acceleration for this over-long fuzzy term (index
				 * built without trigrams, or the term has too few trigrams for
				 * 3k+1) -- or, in principle, a regex over a segment with no
				 * dictionary.  We fall back to rechecking every document in the
				 * segment against the pattern -- correct, just slower, and the
				 * documented behavior of a trigrams-off index (see the
				 * "trigrams reloption" regression test).  weave_universe_bounded
				 * folds duplicates as it goes so the candidate scratch stays
				 * O(ndocs); the older unbounded collect grew to Sum(df) and could
				 * reach a multi-gigabyte "invalid memory alloc request size" on a
				 * high-vocabulary corpus.
				 */
				need_recheck = true;
				universe = weave_universe_bounded(index, sg->dictstart, sg->ndocs,
												 sg->doclenstart == InvalidBlockNumber);
				if (universe.n > 0)
				{
					weave_filter_tombstoned_seg(&seg_tombs, s, &universe);
					if (universe.n > 0)
						acc = tidset_or(acc, universe);
				}
			}
			continue;
		}

		/*
		 * Do NOT build the universe here.  It is handed to the evaluator as a
		 * lazy source and materialised only if the top-level result is still
		 * negated after De Morgan -- see the comment on weave_eval_query.  The
		 * eager build cost 603.9 ms on `common & !rare` at 200k rows for a set
		 * that was then discarded.
		 */
		uni.index = index;
		uni.dictstart = sg->dictstart;
		uni.ndocs = sg->ndocs;
		uni.has_doclen_col = (sg->doclenstart == InvalidBlockNumber);
		uni.built = false;
		uni.set.tids = NULL;
		uni.set.n = 0;
		universe.tids = NULL;
		universe.n = 0;

		if (use_pos_phrase)
		{
			/* evaluate the phrase from this segment's positional postings; the
			 * matched TIDs accumulate in ptids across segments, and are folded
			 * into acc after the loop.  A false return means a term lacked
			 * positions (a rare page-overflow block) -- abandon the fast path
			 * and fall back to the AND + recheck path for correctness. */
			int			seg_start = nptids;

			if (ptids == NULL)
			{
				captids = 64;
				/* corpus-scale (one tid per matching doc); query path, so a
				 * throw here loses one query, not a vacuum */
				ptids = (ItemPointerData *) WEAVE_ALLOC_MAYBE_HUGE((Size) captids * sizeof(ItemPointerData));
			}
			if (!weave_phrase_eval_seg(index, sg, query, pterm, pstep, npterm,
									  &ptids, &nptids, &captids))
			{
				/* fall back: restart collection from scratch via the AND path */
				use_pos_phrase = false;
				if (ptids)
				{
					pfree(ptids);
					ptids = NULL;
				}
				nptids = 0;
				if (acc.tids)
				{
					pfree(acc.tids);
					acc.tids = NULL;
				}
				acc.n = 0;
				s = (uint32) -1;	/* restart the segment loop (s++ -> 0) */
				continue;
			}
			/* filter ONLY this segment's new hits (ptids[seg_start..nptids])
			 * against THIS segment's tombstones -- they are docid-ascending (the
			 * driver posting list is docid-sorted).  Prior segments' hits were
			 * already filtered against their own maps. */
			if (nptids > seg_start)
			{
				TidSet		phr;

				phr.tids = ptids + seg_start;
				phr.n = nptids - seg_start;
				weave_filter_tombstoned_seg(&seg_tombs, s, &phr);
				nptids = seg_start + phr.n;
			}
			continue;
		}

		{
			TidSet		result = weave_eval_query(index,
												 sg, query, &uni);

			if (result.n > 0)
			{
				weave_filter_tombstoned_seg(&seg_tombs, s, &result);
				if (result.n > 0)
				{
					/* PHRASE/NEAR is evaluated as AND here (positions=off or a
					 * non-pure-phrase query); the heap wdoc carries positions,
					 * so a heap recheck of @@@ enforces adjacency exactly. */
					if (has_phrase)
						need_recheck = true;
					acc = tidset_or(acc, result);
				}
			}
		}
	}

	/* fold in the positional-phrase matches (already tombstone-filtered per
	 * segment); need_recheck stays false -- the positions gave the exact set.
	 * ptids is per-segment-ascending but not globally sorted (segments overlap
	 * in docid range), so sort+uniq before the merge. */
	if (use_pos_phrase && nptids > 0)
	{
		TidSet		phr;

		phr.tids = ptids;
		phr.n = nptids;
		tidset_sort_uniq(&phr);
		acc = tidset_or(acc, phr);
	}
	if (ptids)
		pfree(ptids);

	/* pending list: verbatim docs matched by the exact per-doc matcher.
	 * Collect these separately from the segment matches: a pending doc is a
	 * live heap tuple that was just inserted, so it must NOT be subjected to
	 * the segment tombstone filter below -- a reused heap slot (same TID as a
	 * previously deleted, tombstoned doc) would otherwise be wrongly dropped. */
	pending_acc.tids = NULL;
	pending_acc.n = 0;
	if (meta.pendinghead != InvalidBlockNumber)
	{
		BlockNumber blk = meta.pendinghead;

		while (blk != InvalidBlockNumber)
		{
			Buffer		buffer;
			Page		page;
			WeavePendingIter it;
			WeavePendingRec rec;
			BlockNumber next;

			CHECK_FOR_INTERRUPTS();	/* between pages, no buffer lock held: safe to let a cancel unwind */
			buffer = weave_scan_readbuf(index, blk);
			if (buffer == InvalidBuffer)
				break;		/* block truncated by a concurrent weave_vacuum: end of chain */
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);
			next = WeavePageGetOpaque(page)->nextblk;

			/*
			 * The iterator bounds-guards each item before pi->doclen is trusted,
			 * and stops the page walk when one does not fit.  That guard is
			 * load-bearing here: the pending list is read under only
			 * BUFFER_LOCK_SHARE, and a concurrent flush (INSERT pending-buffer ->
			 * segment, VACUUM, weave_merge) clears the list and frees these pages,
			 * which a concurrent insert can recycle and overwrite (pg_weave
			 * recycles freed pages with no deletion-xid gate).  A scan that
			 * snapshotted pendinghead before that then walks a recycled page whose
			 * doclen is arbitrary.  When the walk stops early, the scan's
			 * generation re-check detects the stale read and restarts.
			 *
			 * The VECTOR half of an item is not read here.  This is the lexical
			 * match over not-yet-flushed documents; the vector channel's scan is a
			 * per-bolt shuttle and a pending document is in no bolt yet, which is
			 * a recall gap of its own -- see doc/GAPS.md G29.
			 */
			weave_pending_iter_init(&it, page);
			while (weave_pending_iter_next(&it, &rec))
			{
				/* A pending doc is raw page bytes; validate before the matcher
				 * walks its offsets, so a torn/corrupt page cannot segfault a
				 * SELECT.  A malformed doc is simply not matched (and flagged). */
				if (!weave_doc_is_valid(rec.doc, rec.doclen))
					ereport(WARNING,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("pg_weave: skipping malformed pending document in index \"%s\" during scan",
									RelationGetRelationName(index)),
							 errhint("REINDEX the index to rebuild it from the heap.")));
				else if (weave_doc_matches(rec.doc, query))
				{
					TidSet		one;

					one.tids = rec.tid;
					one.n = 1;
					pending_acc = tidset_or(pending_acc, one);	/* exact per-doc match */
				}
			}
			UnlockReleaseBuffer(buffer);
			blk = next;
		}
	}

	tidset_sort_uniq(&acc);
	/*
	 * Segment matches were already filtered against each segment's own
	 * tombstone map above; pending matches are live tuples and are never
	 * tombstoned.  Just release the loaded maps.
	 */
	weave_tombstones_free(&seg_tombs);
	/* fold in the (unfiltered) pending matches and re-uniq */
	if (pending_acc.n > 0)
	{
		acc = tidset_or(acc, pending_acc);
		tidset_sort_uniq(&acc);
	}
	/*
	 * Concurrency guard: if the segment directory changed while we were reading
	 * (a concurrent merge/vacuum/bulkdelete may have freed + recycled pages we
	 * read from a now-stale segment descriptor), our result may be a stale read.
	 * Discard it and redo from a fresh snapshot.  Bounded so a pathological
	 * merge storm can't spin forever; after the cap we proceed with the last
	 * result (still no worse than the pre-fix behavior, and merges are rare
	 * relative to a scan).
	 */
	if (weave_read_meta_generation(index) != gen0 && gen_retries++ < 10)
	{
		if (acc.tids)
			pfree(acc.tids);
		if (pending_acc.tids)
			pfree(pending_acc.tids);
		if (ptids)
			pfree(ptids);
		ptids = NULL;
		nptids = 0;
		captids = 0;
		need_recheck = false;
		weave_tombstones_free(&seg_tombs);
		weave_read_meta(index, &meta);
		goto collect_retry;
	}

	*out = acc;
	*recheck = need_recheck;
}

int64
weave_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	WeaveScanOpaque so = (WeaveScanOpaque) scan->opaque;
	TidSet		matches;
	bool		recheck;

	/*
	 * amoptionalkey is true (see weave_handler), so the planner may hand us a
	 * scan with no restriction clause.  That is intended: it is how the keyless
	 * ordering scan works, and weave_rescan takes the query from the order-by
	 * argument in that case.
	 *
	 * But a scan with NEITHER a scan key NOR an order-by has no query at all,
	 * and there is nothing correct to return.  Silently returning zero rows
	 * would make such a plan produce an empty result set -- a wrong answer, not
	 * a slow one -- so fail loudly.  Reachable via a partial index, where a
	 * useful predicate alone can justify a path.
	 */
	if (scan->numberOfKeys == 0 && scan->numberOfOrderBys == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a weave index scan requires a query"),
				 errdetail("The scan has neither a @@@ restriction nor an ORDER BY <=> ordering clause."),
				 errhint("Add \"WHERE col @@@ query\" or \"ORDER BY col <=> query\".")));

	/*
	 * Z8: a `@~` / `@~*` restriction on the cgram column.  BEFORE the so->query
	 * test below, because this path has no wquery at all.
	 *
	 * recheck = FALSE, and that is the load-bearing half.  weave_cgram_collect()
	 * has ALREADY rechecked every candidate against its live heap tuple, so the
	 * set is exact and asking the executor to re-evaluate the operator would be
	 * duplicated work.  It also means the C recheck is the ONLY thing keeping the
	 * answer exact: with recheck = true the executor's own bitmap-heap recheck
	 * would mask a broken route, which is precisely the trap AGENTS.md records
	 * under "the suite running is not the SITE running" -- a mutation that
	 * removed our recheck passed the whole suite because the executor
	 * re-evaluated the qual itself.  A lossy bitmap page still forces an
	 * executor recheck, and that recheck calls the very same matcher through the
	 * operator, so the two cannot disagree.
	 */
	if (so->cgramScan)
	{
		if (so->cgramPat == NULL)
			return 0;
		pgstat_count_index_scan(scan->indexRelation);
		(void) weave_cgram_collect(scan->indexRelation, so->cgramPat,
								   so->cgramPatLen, so->cgramCI, &matches);
		if (matches.n > 0)
			tbm_add_tuples(tbm, matches.tids, matches.n, so->cgramLossy);
		return matches.n;
	}

	if (!so->queryValid || so->query == NULL)
		return 0;
	/* Count the index scan for pg_stat_user_indexes.idx_scan; idx_tup_read is
	 * added by index_getbitmap() from our return value. */
	pgstat_count_index_scan(scan->indexRelation);
	weave_collect_matches(scan->indexRelation, so->query, &matches, &recheck);
	if (matches.n > 0)
		tbm_add_tuples(tbm, matches.tids, matches.n, recheck);
	return matches.n;
}

/*
 * weave_recheck_exact: shrink `set` to the EXACT @@@ match set.
 *
 * weave_collect_matches returns recheck=true for queries the index over-
 * generates (PHRASE/NEAR: adjacency not enforced by the positionless posting
 * lists; FUZZY/REGEX: the trigram funnel yields candidates).  The bitmap-heap
 * scan hands these to the executor with a recheck flag so it re-evaluates @@@
 * against the heap wdoc.  The ranked <=> scan and weave_count() have no
 * executor recheck, so they must do it here: recompute the indexed wdoc from
 * each candidate's live heap tuple (evaluating the index expression, exactly
 * as build/insert do) and drop any that fail weave_doc_matches.  After this the
 * TID set is precisely what "WHERE d @@@ q" (with heap recheck) admits.
 *
 * Non-live tuples (not visible / vacuumed) are dropped too; the caller's own
 * MVCC visibility pass would drop them anyway, so this never widens the set.
 */
static void
weave_recheck_exact(Relation index, WeaveQuery query, TidSet *set)
{
	Relation	heap;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	Snapshot	snap = GetActiveSnapshot();
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	WeaveIndexLayout layout;
	int			lexidx;
	int			i,
				keep = 0;

	if (set->n == 0)
		return;

	/* FormIndexDatum fills every key column, so pick the wdoc out by attnum
	 * rather than by position -- a multicolumn weave index may list the vector
	 * column first. */
	weave_index_layout(index, &layout);
	lexidx = layout.lexattno - 1;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	indexInfo = BuildIndexInfo(index);
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heap, NULL);
	econtext->ecxt_scantuple = slot;
#if PG_VERSION_NUM >= 190000
	fetch = table_index_fetch_begin(heap, SO_NONE);
#else
	fetch = table_index_fetch_begin(heap);
#endif

	for (i = 0; i < set->n; i++)
	{
		ItemPointerData tid = set->tids[i];
		bool		call_again = false;
		bool		all_dead = false;

		CHECK_FOR_INTERRUPTS();	/* per candidate; no index buffer lock held (heap fetch self-manages) */
		ExecClearTuple(slot);
		if (table_index_fetch_tuple(fetch, &tid, snap, slot,
									&call_again, &all_dead))
		{
			WeaveDoc		doc;

			FormIndexDatum(indexInfo, slot, estate, values, isnull);
			if (!isnull[lexidx])
			{
				doc = (WeaveDoc) PG_DETOAST_DATUM(values[lexidx]);
				if (weave_doc_matches(doc, query))
					set->tids[keep++] = set->tids[i];
			}
		}
		ResetExprContext(econtext);
	}

	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	table_close(heap, AccessShareLock);
	set->n = keep;
}

/* ---------------------------------------------------------------------------
 * The cgram route (task Z8): `col @~ '%tion refu%'`
 *
 * THE SMALLEST SURFACE THAT DEMONSTRATES THE CHANNEL, and the choice is
 * deliberate on three counts:
 *
 *  1. ONE OPERATOR PAIR, not `LIKE` itself.  See the migration script
 *     sql/pg_weave--0.12.0--0.13.0.sql: putting `~~` in the family would route
 *     every LIKE on the column here, including anchored patterns the planner has
 *     btree-oriented machinery for and patterns this AM's cost estimator does not
 *     model.  A query asks for this channel by name.
 *  2. BITMAP-ORIENTED, but returning an EXACT set.  The route ANDs the docid
 *     posting lists of the pattern's required trigrams and then rechecks on the
 *     heap, so what comes out is precisely what LIKE admits -- which means
 *     weave_getbitmap() can add it with recheck = false and weave_gettuple() can
 *     serve from the same function.  One code path, one answer.
 *  3. THE RECHECK IS MANDATORY, not an optimization.  Byte trigrams
 *     OVER-GENERATE by construction: a document may contain every required
 *     trigram in the wrong order, in the wrong runs, spanning a `%` boundary, or
 *     under a hash collision, and the ASCII case fold adds more of the same (see
 *     WEAVE_CGRAM_FOLD).  This is NOT Z6's exact dictionary walk, where the
 *     dictionary entry IS the answer.  Delete the recheck and the channel returns
 *     false positives -- which is what the first leg of /scratch/pg_weave/z8-mut.sh
 *     proves.
 *
 * WHEN IT REFUSES TO NARROW, and what happens then.  Two cases: the pattern has
 * no literal run of three or more bytes (`'%ab%'`, `'%'`, `'%a_c%'`), or a live
 * bolt carries no cgram weft (built before Z8, written by a pending flush, or
 * refused by the pair cap).  Either way the candidate set would not be a superset
 * of the answer, so the route does not produce one: it falls back to the
 * universe, which here means a sequential pass over the heap evaluating the
 * predicate.  Correct and slow, which is the same stance weave_trgm_candidates()
 * and weave_regex_terms() take when their own soundness condition fails.
 * ------------------------------------------------------------------------- */

/*
 * The exact LIKE/ILIKE test against each candidate's live heap tuple.
 *
 * Distinct from weave_recheck_exact() above, which evaluates a wquery against the
 * indexed wdoc; this one evaluates the pattern against the gram_ops column's raw
 * text.  Non-live tuples are dropped, which never widens the set (the caller's
 * own MVCC pass would drop them anyway) -- the same reasoning weave_recheck_exact
 * records.
 */
static void
weave_cgram_recheck(Relation index, const char *pat, int patlen, bool ci,
					TidSet *set)
{
	Relation	heap;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	Snapshot	snap = GetActiveSnapshot();
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	WeaveIndexLayout layout;
	int			cgidx;
	int			i,
				keep = 0;

	if (set->n == 0)
		return;

	weave_index_layout(index, &layout);
	Assert(layout.cgramattno != 0);
	cgidx = layout.cgramattno - 1;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	indexInfo = BuildIndexInfo(index);
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heap, NULL);
	econtext->ecxt_scantuple = slot;
#if PG_VERSION_NUM >= 190000
	fetch = table_index_fetch_begin(heap, SO_NONE);
#else
	fetch = table_index_fetch_begin(heap);
#endif

	for (i = 0; i < set->n; i++)
	{
		ItemPointerData tid = set->tids[i];
		bool		call_again = false;
		bool		all_dead = false;

		CHECK_FOR_INTERRUPTS();	/* per candidate; no index buffer lock held */
		ExecClearTuple(slot);
		if (table_index_fetch_tuple(fetch, &tid, snap, slot,
									&call_again, &all_dead))
		{
			FormIndexDatum(indexInfo, slot, estate, values, isnull);
			if (!isnull[cgidx])
			{
				text	   *v = (text *) PG_DETOAST_DATUM(values[cgidx]);

				if (weave_cgram_match(VARDATA_ANY(v), (int) VARSIZE_ANY_EXHDR(v),
									  pat, patlen, ci))
					set->tids[keep++] = set->tids[i];
			}
		}
		ResetExprContext(econtext);
	}

	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	table_close(heap, AccessShareLock);
	set->n = keep;
}

/*
 * The fallback universe: one sequential pass over the heap, evaluating the
 * predicate.  Returns the EXACT answer, so a caller that lands here needs no
 * further recheck.
 *
 * A heap scan inside an access method reads oddly, so: the alternative is an
 * in-index enumeration of every document, and a weave index does not have one.
 * The lexical weft omits a document whose analyzed text yields no postings (empty
 * or stopword-only -- the fact WEAVE_PK_VWARP exists to record), the doclen
 * sidecar is fed from those same postings, and the cgram weft itself is what we
 * are falling back FROM.  A candidate set built from any of those would be
 * missing rows, i.e. wrong, and "slow" is the only acceptable way to be unable.
 */
static void
weave_cgram_heapscan(Relation index, const char *pat, int patlen, bool ci,
					 TidSet *out)
{
	Relation	heap;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	TableScanDesc scan;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	WeaveIndexLayout layout;
	int			cgidx;
	int			cap = 0;
	int			n = 0;
	ItemPointerData *tids = NULL;
	OffsetNumber *roots;
	BlockNumber rootblk = InvalidBlockNumber;

	out->tids = NULL;
	out->n = 0;

	weave_index_layout(index, &layout);
	Assert(layout.cgramattno != 0);
	cgidx = layout.cgramattno - 1;
	roots = (OffsetNumber *) palloc(MaxHeapTuplesPerPage * sizeof(OffsetNumber));	/* alloc-ok: one heap page's line pointers, a compile-time bound */

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	indexInfo = BuildIndexInfo(index);
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heap, NULL);
	econtext->ecxt_scantuple = slot;
	scan = table_beginscan(heap, GetActiveSnapshot(), 0, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		if (!isnull[cgidx])
		{
			text	   *v = (text *) PG_DETOAST_DATUM(values[cgidx]);

			if (weave_cgram_match(VARDATA_ANY(v), (int) VARSIZE_ANY_EXHDR(v),
								  pat, patlen, ci))
			{
				/*
				 * THE TID AN ACCESS METHOD MAY RETURN IS THE HOT-CHAIN ROOT LINE
				 * POINTER, NOT THE PHYSICAL TUPLE'S.  This cost a debugging round
				 * and is worth the paragraph.
				 *
				 * A heap scan hands back the physical version's TID.  After a HOT
				 * update that version is a HEAP-ONLY tuple, and
				 * heap_hot_search_buffer() -- which is what both a bitmap heap
				 * scan and table_index_fetch_tuple() use to resolve a TID an index
				 * gave them -- REFUSES a heap-only tuple as a chain start ("if
				 * at_chain_start && HeapTupleHeaderIsHeapOnly, break").  So a
				 * bitmap built from physical TIDs resolves to NOTHING: the Bitmap
				 * Index Scan reports the right row count and the Bitmap Heap Scan
				 * above it reports zero.  No error, no warning, just an empty
				 * result -- and only for rows that have been updated, so a test
				 * corpus built with INSERT alone never sees it.  (This tree
				 * reaches it immediately: sql/cgram.sql populates the wdoc column
				 * with an UPDATE, which HOT-updates every row.)
				 *
				 * heap_get_root_tuples() maps every line pointer on a page to its
				 * chain root; it is what CREATE INDEX CONCURRENTLY uses for the
				 * same reason.  Computed once per heap block because the scan
				 * visits blocks in order, so it is one extra page read per block,
				 * not per tuple.
				 */
				ItemPointerData rtid = slot->tts_tid;
				BlockNumber blk = ItemPointerGetBlockNumber(&rtid);
				OffsetNumber off = ItemPointerGetOffsetNumber(&rtid);

				if (blk != rootblk)
				{
					Buffer		rb = ReadBuffer(heap, blk);

					LockBuffer(rb, BUFFER_LOCK_SHARE);
					heap_get_root_tuples(BufferGetPage(rb), roots);
					UnlockReleaseBuffer(rb);
					rootblk = blk;
				}
				if (off >= 1 && off <= MaxHeapTuplesPerPage &&
					roots[off - 1] != InvalidOffsetNumber)
					ItemPointerSetOffsetNumber(&rtid, roots[off - 1]);

				if (n >= cap)
				{
					/* relation-scale: every row of the heap can match */
					cap = cap ? cap * 2 : 256;
					tids = tids
						? WEAVE_REALLOC_MAYBE_HUGE(tids, (Size) cap * sizeof(ItemPointerData))
						: WEAVE_ALLOC_MAYBE_HUGE((Size) cap * sizeof(ItemPointerData));
				}
				tids[n++] = rtid;
			}
		}
		ResetExprContext(econtext);
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	table_close(heap, AccessShareLock);
	pfree(roots);

	out->tids = tids;
	out->n = n;
	tidset_sort_uniq(out);
}

/*
 * The whole route.  Fills *out with the EXACT set of TIDs whose gram_ops column
 * satisfies the pattern.  Returns true when the trigram weft served it (i.e. the
 * candidate set was narrowed before the recheck), false when it fell back.
 */
static bool
weave_cgram_collect(Relation index, const char *pat, int patlen, bool ci,
					TidSet *out)
{
	WeaveMetaPageData meta;
	WeaveTombstones seg_tombs;
	TidSet		acc;
	uint32		req[WEAVE_CGRAM_MAX_REQ];
	int			nreq;
	uint32		s;
	int			t;
	bool		served = true;
	uint32		gen0;

	out->tids = NULL;
	out->n = 0;

	nreq = weave_cgram_required(pat, patlen, ci, req, WEAVE_CGRAM_MAX_REQ);
	if (nreq <= 0)
	{
		/* No sound requirement: `'%ab%'`, `'%a_c%'`, `'%'`, or a case-insensitive
		 * pattern over non-ASCII bytes.  This is the refusal the route owes its
		 * caller, and the counter deliberately does NOT move. */
		weave_cgram_heapscan(index, pat, patlen, ci, out);
		return false;
	}

	if (RelationGetNumberOfBlocks(index) == 0)
	{
		/* buildempty(): no metapage, hence no bolts and no documents */
		weave_cgram_heapscan(index, pat, patlen, ci, out);
		return false;
	}

	gen0 = weave_read_meta_generation(index);
	weave_read_meta(index, &meta);

	/*
	 * EVERY live bolt must carry a cgram weft, or the narrowed set is not a
	 * superset of the answer and the route must not produce one.  Checked before
	 * a single posting list is read, so the fallback costs one descriptor probe
	 * per bolt rather than a wasted intersection.
	 */
	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		if (meta.segs[s].dictstart == InvalidBlockNumber)
			continue;			/* consumed slot */
		if (weave_cgram_weft_root(index, &meta.segs[s]) == InvalidBlockNumber)
		{
			served = false;
			break;
		}
	}
	/* A pending document is in no bolt, so it has no weft either -- but its TID
	 * is known and cheap to add as a candidate, which the recheck then filters
	 * exactly.  That is enough; it does not force the fallback. */

	if (!served)
	{
		weave_cgram_heapscan(index, pat, patlen, ci, out);
		return false;
	}

	weave_tombstones_load(index, &meta, &seg_tombs);
	acc.tids = NULL;
	acc.n = 0;

	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveCgramWeft w;
		const char *why = NULL;
		TidSet		cands;
		bool		first = true;

		CHECK_FOR_INTERRUPTS();
		if (meta.segs[s].dictstart == InvalidBlockNumber)
			continue;
		if (!weave_cgram_weft_open(index, weave_cgram_weft_root(index, &meta.segs[s]),
								   &w, &why))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("corrupt cgram weft in index \"%s\"",
							RelationGetRelationName(index)),
					 errdetail("%s", why),
					 errhint("REINDEX the index to rebuild it.")));

		cands.tids = NULL;
		cands.n = 0;
		for (t = 0; t < nreq; t++)
		{
			TidSet		one;
			char		key[WEAVE_CGRAM_KEYLEN];

			weave_cgram_key(req[t], key);
			/*
			 * THE INTERSECTION IS THE POINT.  Every required trigram must be
			 * present in a matching document (weave_cgram_required states why),
			 * so the candidate set is the AND of their posting lists.  A UNION
			 * would also be CORRECT -- a superset of the answer, which the
			 * mandatory recheck then filters -- and merely slower, which is what
			 * the second leg of z8-mut.sh measures rather than "catches".
			 *
			 * has_doclen_col is a constant `true`: the cgram writer keeps the
			 * inline doclen FOR column (all zeros, ~2 bytes per 128-posting
			 * block) precisely so this argument cannot be got wrong.
			 */
			if (!weave_lookup_term_at(index, w.dictstart, w.dictindexstart, true,
									  key, WEAVE_CGRAM_KEYLEN, &one))
			{
				/* a required trigram is absent from this bolt: no document here
				 * can match, so the whole intersection is empty */
				if (cands.tids)
					pfree(cands.tids);
				cands.tids = NULL;
				cands.n = 0;
				first = false;
				break;
			}
			cands = first ? one : tidset_and(cands, one);
			first = false;
			if (cands.n == 0)
				break;
		}

		if (cands.n > 0)
		{
			weave_filter_tombstoned_seg(&seg_tombs, s, &cands);
			if (cands.n > 0)
				acc = tidset_or(acc, cands);
		}
	}

	weave_tombstones_free(&seg_tombs);

	/*
	 * The pending list: documents inserted since the last flush live in no bolt,
	 * so no posting list mentions them.  Their TIDs are candidates unconditionally
	 * and the recheck decides -- a pending doc is a live heap tuple, so unlike a
	 * segment match it must NOT be tombstone-filtered (a reused heap slot would
	 * otherwise be wrongly dropped, which is the reasoning weave_collect_matches
	 * records for its own pending pass).
	 */
	if (meta.pendinghead != InvalidBlockNumber)
	{
		BlockNumber blk = meta.pendinghead;

		while (blk != InvalidBlockNumber)
		{
			Buffer		buffer;
			Page		page;
			WeavePendingIter it;
			WeavePendingRec rec;
			BlockNumber next;

			CHECK_FOR_INTERRUPTS();
			buffer = weave_scan_readbuf(index, blk);
			if (buffer == InvalidBuffer)
				break;
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);
			next = WeavePageGetOpaque(page)->nextblk;
			weave_pending_iter_init(&it, page);
			while (weave_pending_iter_next(&it, &rec))
			{
				TidSet		one;

				one.tids = rec.tid;
				one.n = 1;
				acc = tidset_or(acc, one);
			}
			UnlockReleaseBuffer(buffer);
			blk = next;
		}
	}

	tidset_sort_uniq(&acc);

	/*
	 * Same stale-read guard the lexical collector uses: a concurrent
	 * merge/vacuum can free and recycle the pages we just read.  One retry level
	 * only -- the fallback is exact anyway, so on a generation change we simply
	 * take the slow correct path rather than spinning.
	 */
	if (weave_read_meta_generation(index) != gen0)
	{
		if (acc.tids)
			pfree(acc.tids);
		weave_cgram_heapscan(index, pat, patlen, ci, out);
		return false;
	}

	/* THE MANDATORY RECHECK.  See the block comment at the top of this section:
	 * byte trigrams over-generate by construction, so this is what makes the
	 * answer exact -- not a tightening of an already-correct set. */
	weave_cgram_recheck(index, pat, patlen, ci, &acc);

	*out = acc;
	weave_chan_cgram_scan++;	/* the route SERVED this scan (weave/weave.h) */
	return true;
}
void
weave_endscan(IndexScanDesc scan)
{
	/* memory is freed with the scan's context */
}

/* ----- index-maintained corpus statistics (stage 5) ----- */

/*
 * Look up a term's dictionary entry (df, max_tf, first posting block) without
 * reading any postings.  Returns true if found.  This is what the lazy WAND
 * cursors need to start; postings are then paged in on demand.
 */
static bool
weave_lookup_dict(Relation index, const WeaveSegMeta *seg,
				 const char *term, int termlen,
				 uint32 *df, uint32 *max_tf, BlockNumber *firstposting,
				 uint32 *firstoffset)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;			/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				*df = de->df;
				*max_tf = de->max_tf;
				*firstposting = de->firstposting;
				*firstoffset = de->firstoffset;
				found = true;
				break;
			}
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		if (found)
			return true;
		if (onlyone)
			break;				/* block index located the only possible page */
		blk = next;
	}
	*df = 0;
	*max_tf = 0;
	*firstposting = InvalidBlockNumber;
	*firstoffset = 0;
	return false;
}

/* Look up the document frequency of a term in the index, 0 if absent. */
static uint32
weave_lookup_df(Relation index, const WeaveSegMeta *seg,
			   const char *term, int termlen)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		uint32		df = 0;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				df = de->df;
				found = true;
				break;
			}
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		if (found)
			return df;
		if (onlyone)
			break;
		blk = next;
	}
	return 0;
}

PG_FUNCTION_INFO_V1(weave_index_nsegments);

/* weave_index_nsegments(regclass) -> int : number of live segments.
 * Works on an in-progress (indisvalid=f) index so a build can be polled; returns
 * NULL if the metapage is not yet a valid pg_weave metapage (very early in a build
 * or a non-weave relation) rather than erroring, so a monitoring query is safe. */
Datum
weave_index_nsegments(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	WeaveMetaPageData meta;
	bool		ok = false;

	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam == get_index_am_oid("weave", true) &&
		RelationGetNumberOfBlocks(index) > WEAVE_METAPAGE_BLKNO)
	{
		Buffer		buf = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (WeavePageGetMeta(BufferGetPage(buf))->magic == WEAVE_MAGIC)
		{
			memcpy(&meta, WeavePageGetMeta(BufferGetPage(buf)), sizeof(meta));
			ok = true;
		}
		UnlockReleaseBuffer(buf);
	}
	index_close(index, AccessShareLock);
	if (!ok)
		PG_RETURN_NULL();
	PG_RETURN_INT32((int32) meta.nsegments);
}

PG_FUNCTION_INFO_V1(weave_index_stats);

/* weave_index_stats(regclass) -> (ndocs float8, avgdl float8, nterms bigint) */
Datum
weave_index_stats(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	WeaveMetaPageData meta;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(index))));
	weave_read_meta(index, &meta);
	index_close(index, AccessShareLock);

	values[0] = Float8GetDatum(meta.ndocs);
	values[1] = Float8GetDatum(meta.ndocs > 0 ?
							   meta.sumdoclen / meta.ndocs : 0.0);
	{
		uint32		s;
		int64		nterms = 0;

		for (s = 0; s < meta.nsegments; s++)
			nterms += meta.segs[s].nterms;
		values[2] = Int64GetDatum(nterms);	/* bigint: no int32 wrap at scale */
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(weave_index_df);

/* weave_index_df(regclass, wquery) -> float8[] of df per distinct query term */
Datum
weave_index_df(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	WeaveQuery	q = PG_GETARG_WQUERY(1);
	Relation	index;
	WeaveMetaPageData meta;
	Datum	   *elems;
	int			n = 0;
	uint32		i;
	ArrayType  *result;

	index = index_open(indexoid, AccessShareLock);
	weave_read_meta(index, &meta);

	elems = (Datum *) palloc(q->nitems * sizeof(Datum));
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			uint64		df = 0;		/* summed across segments; uint64 so a term in
									 * >2^32 docs does not wrap (consumed as double) */
			uint32		s;

			/* document frequency is summed across all segments */
			for (s = 0; s < meta.nsegments; s++)
				df += weave_lookup_df(index, &meta.segs[s],
									 WEAVE_QUERY_ITEMTEXT(q, it), it->termlen);
			elems[n++] = Float8GetDatum((double) (df == 0 ? 1 : df));
		}
	}
	index_close(index, AccessShareLock);

	result = construct_array(elems, n, FLOAT8OID, 8, true, 'd');
	PG_FREE_IF_COPY(q, 1);
	PG_RETURN_ARRAYTYPE_P(result);
}

/* ----- index-only scored top-K search (WAND-style) ----- */

#include "funcapi.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

/*
 * weave_search(index regclass, query wquery, k int)
 *   -> setof (ctid tid, score float8)
 *
 * Index-only BM25 top-k: scores are computed entirely from the index (postings
 * give per-doc tf, the dictionary gives df and the max-tf impact bound, the
 * metapage gives N and avgdl) with no heap access.  A WAND-style upper-bound
 * check on each document's best possible score prunes documents that cannot
 * enter the current top-k, which is the early-termination win.
 *
 * Cursors load posting pages lazily and use each page's block-max_tf (stored in
 * the page opaque) to skip entire pages whose best possible contribution cannot
 * beat the current top-k threshold -- block-max WAND -- so most of a long
 * posting list is never decoded.  Per-document |D| is read from the postings
 * for exact BM25 length normalization.
 */
/* ----- document-at-a-time block-max WAND top-k (item 2) ----- */

static int
cmp_scored_desc(const void *a, const void *b)
{
	double		sa = ((const ScoredTid *) a)->score;
	double		sb = ((const ScoredTid *) b)->score;

	if (sa < sb)
		return 1;
	if (sa > sb)
		return -1;
	return 0;
}

/*
 * A per-term cursor for the WAND merge.  posts is the term's docid-sorted
 * posting list; cursors load posting pages lazily from the index and skip
 * whole pages via the page block-max when they cannot beat the threshold.
 *
 * The TYPEDEF is in include/weave/am.h, which leaves the struct INCOMPLETE:
 * src/query/lexshuttle.c (F6) dresses a cursor as a WeaveShuttle and must not be
 * able to take one apart.  Defining the struct here and naming it there is what
 * keeps the posting cursor's internals inside the scan.
 */
struct WandCursor
{
	Relation	index;
	BlockNumber curblk;			/* page holding the current block */
	uint32		curoff;			/* byte offset of the CURRENT block on curblk */
	int			nread;			/* postings consumed so far (stop at df) */
	BlockNumber firstblk;		/* first posting block for the term */
	uint32		firstoff;		/* byte offset of the term's first block */
	uint32		df;				/* term document frequency (postings to read) */
	int			termidx;		/* ordinal VAL/term index (for the BoolGate) */

	/*
	 * Current block only, decoded LAZILY: docids are unpacked eagerly (needed
	 * to pivot/skip), but tf and doclen stay bit-packed in blkbuf and are
	 * extracted per-posting on demand (weave_for_get) only when a posting is
	 * actually scored -- so blocks pruned by block-max never pay for tf/dl.
	 */
	unsigned char *blkbuf;		/* copy of the current block's FOR payload */
	uint64		docids[WEAVE_BLOCK_SIZE];	/* decoded docids of current block */
	uint32		tfoff;			/* offset of tf column within blkbuf */
	uint32		dloff;			/* offset of doclen column within blkbuf (v3 only) */
	bool		has_doclen_col;	/* v3 segment: doclen inline in the block (read at
								 * dloff).  v4 segment: false -- doclen comes from
								 * the doclen sidecar cursor. */
	WeaveDoclenCursor doclenc;	/* v4: forward-cursored sidecar reader (reads only
								 * the pages covering scored docids, not the whole
								 * segment sidecar up front). */
	int			blkcount;		/* postings in the current block */
	uint32		blk_max_tf;		/* block-max tf (from header) */
	uint32		blk_min_dl;		/* block-min |D| (from header) */
	int			cur;			/* index within the current block */
	uint64		docid;			/* current docid (UINT64_MAX = exhausted) */

	/* The saturation function's per-(term, segment) constants -- idf, k1, b,
	 * avgdl and the three products the norm is precomputed into.  These were
	 * five fields of this struct until F6 moved the arithmetic that reads them
	 * into include/weave/bm25bound.h, so that the property test and the scan
	 * share one formula instead of two transcriptions of it. */
	WeaveBm25Factors bm;
	double		max_contrib;	/* term-wide upper bound (shortest-doc norm) */
	WeaveTombstones *tombs;		/* loaded per-segment tombstones (or NULL) */
	uint32		segidx;			/* which segment this cursor's postings belong to */
	sm_cursor_t tombcursor;		/* forward-resume cursor for tombstone lookups.
								 * The WAND scan visits this cursor's docids in
								 * monotonically non-decreasing order and the
								 * tombstone map is read-only for the scan's
								 * lifetime, so a single-chunk resume cursor turns
								 * the otherwise O(postings x chunks) head-walk
								 * into O(postings + chunks).  (An 8-way MRU cache
								 * degenerates to a per-lookup head-walk once an
								 * ascending scan runs past its 8 cached chunks --
								 * the cause of the tombstone-bloat blowup.) */
	/*
	 * Optional docid range [docid_lo, docid_hi) for a parallel worker's slice.
	 * docid_lo = 0 and docid_hi = UINT64_MAX means "whole term" (the serial
	 * path).  The cursor seeks to docid_lo at prime time and reports itself
	 * exhausted (docid = UINT64_MAX) once it reaches docid_hi, so the WAND/
	 * MaxScore loops need no range awareness -- they already stop when every
	 * cursor is exhausted.  Ranges partition the corpus disjointly across
	 * workers, so each worker scores a disjoint candidate set exactly.
	 */
	uint64		docid_lo;
	uint64		docid_hi;
};

static inline void wand_skip_own_tombstoned(WandCursor *c);
static void wand_seek(WandCursor *c, uint64 target);

/*
 * Lazily load the next page-worth of THIS TERM's postings into the cursor.
 * Decodes blocks starting at (c->curblk, c->curoff) until the page ends or the
 * term's df is exhausted, remembering where to resume (curblk/curoff) so a huge
 * term is streamed a page at a time -- WAND/BMW can then skip most of it without
 * ever decoding it (the whole point of block-max WAND).
 */
static void
wand_load_block(WandCursor *c)
{
	Buffer		buf;
	Page		page;
	char	   *p,
			   *pend;
	WeaveBlockHdr *bh;
	const unsigned char *stream;
	uint64		gaps[WEAVE_BLOCK_SIZE];
	uint64		base;
	int			cnt;
	int			glen;
	int			tflen;
	int			i;

	if (c->blkbuf)
	{
		pfree(c->blkbuf);
		c->blkbuf = NULL;
	}
	if (c->curblk == InvalidBlockNumber || c->nread >= (int) c->df)
	{
		c->blkcount = 0;
		c->cur = 0;
		c->docid = UINT64_MAX;
		return;
	}

	buf = ReadBuffer(c->index, c->curblk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	pend = weave_page_entry_end(page);
	p = (char *) page + c->curoff;

	/* skip any empty tail; advance across pages until a real block or EOF */
	while (!(p + sizeof(WeaveBlockHdr) <= pend) ||
		   ((WeaveBlockHdr *) p)->count == 0)
	{
		BlockNumber next = WeavePageGetOpaque(page)->nextblk;

		UnlockReleaseBuffer(buf);
		if (next == InvalidBlockNumber)
		{
			c->curblk = InvalidBlockNumber;
			c->blkcount = 0;
			c->cur = 0;
			c->docid = UINT64_MAX;
			return;
		}
		c->curblk = next;
		c->curoff = MAXALIGN(SizeOfPageHeaderData);
		CHECK_FOR_INTERRUPTS();	/* buffer already released above, next not yet read: no lock held */
		buf = ReadBuffer(c->index, c->curblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		pend = weave_page_entry_end(page);
		p = (char *) page + c->curoff;
	}

	bh = (WeaveBlockHdr *) p;
	stream = (const unsigned char *) (bh + 1);
	cnt = (int) bh->count;
	if (bh->count == 0 || bh->count > (uint32) WEAVE_BLOCK_SIZE)
		cnt = WEAVE_BLOCK_SIZE;	/* defensive: uint32 count, guard both ends */

	/*
	 * Validate the block payload fits within the page BEFORE trusting bytelen.
	 * This page is pinned only BUFFER_LOCK_SHARE; a scan can run concurrently
	 * with a merge/vacuum that frees this segment's pages and a concurrent
	 * insert/flush that recycles and OVERWRITES the freed block (pg_weave recycles
	 * freed pages with no deletion-xid gate).  If that happened between the
	 * segment-directory snapshot and this read, bh->bytelen/posbytelen are
	 * whatever bytes now occupy the page -- e.g. a multi-gigabyte length, which
	 * turned palloc(bh->bytelen) into "invalid memory alloc request size" and
	 * the following memcpy/decode into an out-of-bounds read (SIGSEGV) under
	 * concurrent merge + ingestion.  A genuine block's payload always fits the
	 * page; if it does not, treat it as end-of-chain for this cursor (the
	 * scan's generation re-check catches the stale read and restarts).  Same
	 * bounded-wrong-result-not-a-crash contract as the other decode guards.
	 */
	if (stream + (Size) bh->bytelen + (Size) bh->posbytelen > (const unsigned char *) pend)
	{
		UnlockReleaseBuffer(buf);
		c->curblk = InvalidBlockNumber;
		c->blkcount = 0;
		c->cur = 0;
		c->docid = UINT64_MAX;
		return;
	}

	/* copy the block's FOR payload so tf/dl bytes stay valid after we unlock */
	c->blkbuf = (unsigned char *) palloc(bh->bytelen);
	memcpy(c->blkbuf, stream, bh->bytelen);

	/* eagerly decode ONLY docids (gaps); record tf/dl column offsets for lazy
	 * per-posting access -- pruned blocks never touch tf/dl */
	glen = weave_for_unpack(c->blkbuf, cnt, gaps);
	tflen = weave_for_bytelen(c->blkbuf + glen, cnt);
	c->tfoff = (uint32) glen;
	c->dloff = (uint32) (glen + tflen);	/* v3: doclen column follows tf; unused for v4 */

	base = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
	for (i = 0; i < cnt; i++)
	{
		base += gaps[i];		/* first gap is 0 from first_docid */
		c->docids[i] = base;
	}
	c->blkcount = cnt;
	c->blk_max_tf = bh->max_tf;
	c->blk_min_dl = bh->min_doclen;
	c->cur = 0;
	c->docid = c->docids[0];
	/* advance the resume pointer to the next block (or next page).  Skip past
	 * BOTH the three FOR columns (bytelen) and the trailing positions column
	 * (posbytelen) -- the WAND scan never decodes positions, but it must step
	 * over them to find the next block's header. */
	{
		char	   *nextp = (char *) MAXALIGN((char *) (bh + 1) + bh->bytelen + bh->posbytelen);

		if (nextp + sizeof(WeaveBlockHdr) <= pend &&
			c->nread + cnt < (int) c->df)
			c->curoff = (uint32) (nextp - (char *) page);
		else
		{
			c->curblk = WeavePageGetOpaque(page)->nextblk;
			c->curoff = MAXALIGN(SizeOfPageHeaderData);
		}
	}
	c->nread += cnt;
	UnlockReleaseBuffer(buf);
}

/* Prime the cursor at the term's first block and load it. */
static void
wand_prime(WandCursor *c)
{
	c->blkbuf = NULL;
	c->curblk = c->firstblk;
	c->curoff = c->firstoff;
	c->nread = 0;
	if (c->firstblk == InvalidBlockNumber || c->df == 0)
	{
		c->blkcount = 0;
		c->cur = 0;
		c->docid = UINT64_MAX;
		return;
	}
	wand_load_block(c);
	/* seek to this cursor's docid range start (parallel worker slice); for the
	 * serial path docid_lo == 0 so this is a no-op */
	if (c->docid_lo > 0 && c->docid != UINT64_MAX)
		wand_seek(c, c->docid_lo);
	else
		wand_skip_own_tombstoned(c);
}

/* The block-max contribution upper bound for the current 128-block.
 * Uses the block's max_tf AND min |D|: impact is increasing in tf and
 * decreasing in |D|, so impact(max_tf, min_dl) is a sound (and much tighter
 * than the shortest-possible-doc) upper bound for every posting in the block.
 *
 * SOUNDNESS with the v4 doclen sidecar: scoring reads the QUANTIZED doclen
 * (weave_byte_to_doclen, which truncates -> the effective |D| is <= the exact
 * |D|, i.e. a doc can score HIGHER than its exact length implies).  The block
 * header stores the EXACT min |D| from build time; using it directly would make
 * the bound too small (a quantized-down doc could out-score the bound and be
 * wrongly skipped -- WAND unsoundness that misses true top-k docs on multi-term
 * AND).  So for a sidecar (v4) cursor we bound with the quantized-floor of the
 * block min |D|, matching the smallest effective |D| scoring can produce. */
static inline double
wand_block_max_contrib(WandCursor *c)
{
	double		mtf = (double) c->blk_max_tf;
	uint32		mindl_raw = c->blk_min_dl;
	double		mindl = (double) (c->has_doclen_col
									 ? mindl_raw			/* v3: exact inline doclen */
									 : weave_byte_to_doclen(weave_doclen_to_byte(mindl_raw)));

	/* The quantized-floor adjustment above stays HERE, not in bm25bound.h: it is
	 * a property of what the v4 sidecar can hand the scorer, not of BM25, and the
	 * bound's inputs must be the extremes scoring can actually produce. */
	return weave_bm25_block_bound(&c->bm, mtf, mindl);
}

/* True if the cursor's CURRENT docid is tombstoned in the cursor's OWN
 * segment.  Tombstones are per-segment, so a cursor must ignore only its own
 * segment's deletions -- a reused heap TID that is live in another segment or
 * the pending list must still be produced by the segments that legitimately
 * contain it. */
static inline bool
wand_cur_own_tombstoned(WandCursor *c)
{
	if (c->tombs == NULL || !c->tombs->hasany || c->docid == UINT64_MAX)
		return false;
	if (c->segidx >= c->tombs->nseg || !c->tombs->present[c->segidx])
		return false;
	/* Forward-resume cursor: the WAND scan feeds this cursor docids in
	 * monotonically non-decreasing order, so sm_contains resumes the chunk
	 * walk from the last located chunk instead of head-walking from chunk 0.
	 * This is O(1) amortized even when the segment carries millions of
	 * tombstones (the tombstone-bloat pathology). */
	return sm_contains(&c->tombs->maps[c->segidx], c->docid,
					   &c->tombcursor);
}

/* After the current docid is (re)positioned, skip forward over any docids
 * deleted in this cursor's own segment.  Loads successive blocks as needed;
 * wand_load_block does not itself skip, so there is no recursion. */
static inline void
wand_skip_own_tombstoned(WandCursor *c)
{
	while (wand_cur_own_tombstoned(c))
	{
		c->cur++;
		if (c->cur < c->blkcount)
			c->docid = c->docids[c->cur];
		else
			wand_load_block(c);
	}
	/* enforce the worker's docid range upper bound: past it, this cursor is
	 * done (its slice ends before docid_hi; another worker owns the rest) */
	if (c->docid >= c->docid_hi)
		c->docid = UINT64_MAX;
}

/* Advance the cursor to the next posting, loading the next block if needed. */
static void
wand_next(WandCursor *c)
{
	c->cur++;
	if (c->cur < c->blkcount)
		c->docid = c->docids[c->cur];
	else
		wand_load_block(c);		/* stream the next block of this term */
	wand_skip_own_tombstoned(c);
}

/* Exact BM25 contribution of the current posting.  tf and |D| are extracted
 * from the block's still-packed FOR columns ON DEMAND (weave_for_get) -- only
 * for postings actually scored, so pruned blocks never decode tf/dl. */
static inline double
wand_contrib_cur(WandCursor *c)
{
	double		tf = (double) weave_for_get(c->blkbuf + c->tfoff, c->cur);
	double		dl = c->has_doclen_col
		? (double) weave_for_get(c->blkbuf + c->dloff, c->cur)	/* v3: inline */
		: (double) weave_doclen_cursor_lookup(&c->doclenc, c->docid);	/* v4: sidecar */

	return weave_bm25_contrib(&c->bm, tf, dl);
}

/*
 * Skip the cursor past the rest of its current 128-block: load the next block.
 * Sound to call ONLY when this cursor is the single one at/before the pivot
 * (its block is then a contiguous docid run that blocksum bounded whole, and no
 * other cursor holds a docid in the block's range).  The abandoned block never
 * had its tf/doclen decoded (block-max pruning pays only for docids), so this
 * is O(1) amortized and is what keeps common-term BMW fast.  Always makes
 * forward progress.
 */
static void
wand_skip_block(WandCursor *c)
{
	wand_load_block(c);
	wand_skip_own_tombstoned(c);
}

/*
 * Advance the cursor's paging state (curblk/curoff/nread) past whole 128-blocks
 * whose docids are all < target, reading only block HEADERS (no FOR decode).
 * A block is entirely below target when the NEXT block's first_docid <= target
 * (blocks are docid-ordered); the last block on a chain we cannot prove-skip
 * this way, so we stop and let the caller decode it.  This is what lets a seek
 * over a high-df term skip hundreds of thousands of postings without decoding.
 */
static void
wand_skip_blocks(WandCursor *c, uint64 target)
{
	while (c->curblk != InvalidBlockNumber && c->nread < (int) c->df)
	{
		Buffer		buf;
		Page		page;
		char	   *p,
				   *pend;
		BlockNumber nextblk;
		bool		stopped = false;

		CHECK_FOR_INTERRUPTS();	/* between posting-list pages, no buffer lock held: safe to unwind */
		buf = ReadBuffer(c->index, c->curblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		pend = weave_page_entry_end(page);
		nextblk = WeavePageGetOpaque(page)->nextblk;
		p = (char *) page + c->curoff;
		while (p + sizeof(WeaveBlockHdr) <= pend && c->nread < (int) c->df)
		{
			WeaveBlockHdr *bh = (WeaveBlockHdr *) p;
			char	   *nextp;

			if (bh->count == 0)
			{
				stopped = true;
				break;
			}
			nextp = (char *) MAXALIGN((char *) (bh + 1) + bh->bytelen + bh->posbytelen);
			/* can we prove this whole block is < target? need the next block's
			 * first_docid (on this page) to be <= target. */
			if (nextp + sizeof(WeaveBlockHdr) <= pend)
			{
				WeaveBlockHdr *nb = (WeaveBlockHdr *) nextp;
				uint64		nbfirst = ((uint64) nb->first_docid_hi << 32) | nb->first_docid_lo;

				if (nbfirst <= target)
				{
					/* whole block < target: skip it (headers only) */
					c->nread += (int) bh->count;
					c->curoff = (uint32) (nextp - (char *) page);
					p = nextp;
					continue;
				}
			}
			/* this block may contain target (or is the page's last block): stop
			 * so the caller decodes from here */
			stopped = true;
			break;
		}
		if (!stopped)
		{
			/* consumed all blocks on this page as skippable; move to next page */
			c->curblk = nextblk;
			c->curoff = MAXALIGN(SizeOfPageHeaderData);
			UnlockReleaseBuffer(buf);
			continue;
		}
		UnlockReleaseBuffer(buf);
		return;
	}
}

/* Advance a cursor to the first posting with docid >= target (or exhaust). */
static void
wand_seek(WandCursor *c, uint64 target)
{
	if (c->docid >= target)
	{
		wand_skip_own_tombstoned(c);
		return;
	}
	/* first, fast-forward within the current (already-decoded) block's docids */
	while (c->cur < c->blkcount && c->docids[c->cur] < target)
		c->cur++;
	if (c->cur < c->blkcount)
	{
		c->docid = c->docids[c->cur];
		wand_skip_own_tombstoned(c);
		return;
	}
	/* current block exhausted: skip whole undecoded blocks by header, then
	 * load the block containing target and land on it */
	wand_skip_blocks(c, target);
	for (;;)
	{
		wand_load_block(c);
		if (c->docid == UINT64_MAX)
			return;
		while (c->cur < c->blkcount && c->docids[c->cur] < target)
			c->cur++;
		if (c->cur < c->blkcount)
		{
			c->docid = c->docids[c->cur];
			wand_skip_own_tombstoned(c);
			return;
		}
		/* target beyond this block; loop to load/skip the next */
	}
}

/* ---------------------------------------------------------------------------
 * F6: the five calls the lexical shuttle needs from a posting cursor
 *
 * src/query/lexshuttle.c dresses ONE WandCursor as a WeaveShuttle so the fused
 * core can consume ranked lexical (doc/specs/FUSED_TOPK.md sect. 7a (3)).  It
 * sees the cursor as an incomplete type, so everything it needs comes through
 * these five functions; they are declared in include/weave/am.h with the reason
 * (AGENTS.md hard rule 5).  Each is a two-line skin over the static primitives
 * above -- no new traversal logic lives here, because a second traversal is the
 * thing that would drift from the one the regression suite exercises.
 * ------------------------------------------------------------------------- */

/*
 * The last warp position the CURRENT block covers, which a shuttle must publish
 * as blkend after every seek: block_max() bounds the closed interval
 * [cur, blkend], and the header values it reads (blk_max_tf, blk_min_dl) are
 * exactly this block's.  The cursor does not store it -- it stores the decoded
 * docids -- so it is the last of those.
 *
 * Max()'d against the current docid so blkend >= cur holds even if a corrupt
 * block decoded a non-ascending docid run, since a blkend below cur would make
 * the shuttle's bound cover an empty interval and (C2) vacuous.
 */
static inline uint64
wand_cur_blkend(WandCursor *c)
{
	uint64		last;

	if (c->docid == UINT64_MAX)
		return UINT64_MAX;
	if (c->blkcount <= 0)
		return c->docid;
	last = c->docids[c->blkcount - 1];
	return last > c->docid ? last : c->docid;
}

/* The cursor's current docid and block end without moving it: what the shuttle
 * publishes at begin() time, since the cursor arrives already primed. */
uint64
weave_wand_cursor_tell(WandCursor *c, uint64 *blkend)
{
	*blkend = wand_cur_blkend(c);
	return c->docid;
}

/* (C1) forward to the first posting with docid >= target, reporting the new
 * position and the new block end together -- one call, because a shuttle that
 * seeks without republishing blkend is a shuttle whose bound describes the
 * block it used to be on. */
uint64
weave_wand_cursor_seek(WandCursor *c, uint64 target, uint64 *blkend)
{
	wand_seek(c, target);
	*blkend = wand_cur_blkend(c);
	return c->docid;
}

/* (C2)+(C3): the block bound, from the block header values the cursor already
 * holds.  No buffer is read; wand_block_max_contrib() is arithmetic only. */
double
weave_wand_cursor_block_max(WandCursor *c)
{
	return wand_block_max_contrib(c);
}

/* (C4): the exact contribution at the current posting.  May read a buffer -- on
 * a v4 segment the doclen comes from the sidecar cursor, which is the cursor's
 * own resident-block cache and not a second lookup path. */
double
weave_wand_cursor_contrib(WandCursor *c)
{
	return wand_contrib_cur(c);
}

/* The term-wide ceiling, i.e. WeaveShuttle.maxscore.  Static for the cursor's
 * life; the shuttle reads it once. */
double
weave_wand_cursor_max_contrib(WandCursor *c)
{
	return c->max_contrib;
}

/*
 * DocidFilter: an optional docid-membership gate for the ranked scan.
 *
 * A candidate doc may enter the top-k heap only if its docid is a member.
 * `docids` is sorted ascending (membership = binary search).  A NULL filter
 * means "admit everything" -- the pure-OR fast path, where the term
 * disjunction the WAND engine ranks IS the boolean match set, so no gating is
 * needed and there is zero overhead.  For any non-pure-OR query (AND/NOT/
 * PHRASE/prefix/fuzzy/regex) the filter carries the exact @@@ boolean match
 * set (from weave_collect_matches), so the ranked traversal -- otherwise
 * disjunctive -- returns only docs @@@ accepts.
 */
typedef struct DocidFilter
{
	const uint64 *docids;		/* sorted ascending, or NULL for "admit all" */
	int			n;
} DocidFilter;

/* True if docid is admitted by the filter (NULL filter admits everything). */
static inline bool
docid_admitted(const DocidFilter *f, uint64 docid)
{
	int			lo,
				hi;

	if (f == NULL || f->docids == NULL)
		return true;			/* pure-OR fast path: no gating */
	lo = 0;
	hi = f->n - 1;
	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		uint64		v = f->docids[mid];

		if (v == docid)
			return true;
		else if (v < docid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/*
 * weave_query_is_pure_or: true iff the query's boolean structure is a plain
 * disjunction of plain terms -- only OR operators, and every operand is an
 * exact term (no PREFIX/FUZZY/REGEX flag, no AND/NOT/PHRASE).  For such a
 * query the WAND term-disjunction == the @@@ match set, so the ranked scan
 * needs no membership filter.  Any other shape (AND/NOT/PHRASE, or a
 * prefix/fuzzy/regex operand, whose contribution the scorer flattens but @@@
 * evaluates as a set) is NOT pure OR and must be filtered.
 */
static bool
weave_query_is_pure_or(WeaveQuery q)
{
	uint32		i;

	if (q == NULL || q->nitems == 0)
		return true;
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX))
				return false;
		}
		else					/* operator */
		{
			if (it->op != WEAVE_OP_OR)
				return false;
		}
	}
	return true;
}

/*
 * weave_query_is_pure_boolean: true iff the query is a boolean combination
 * (AND/OR/NOT, no PHRASE/NEAR) of PLAIN term operands (no PREFIX/FUZZY/REGEX).
 * For such a query, a doc's @@@ membership is a pure function of WHICH query
 * terms are present in the doc -- exactly what the WAND scan knows at each
 * pivot (which cursors sit at the pivot docid).  So membership can be decided
 * LAZILY at heap-admission time by evaluating the query's RPN over term
 * presence, instead of pre-collecting the whole @@@ match set.
 *
 * Broader than weave_query_is_pure_or (which is the all-OR special case).  Any
 * PHRASE/NEAR operator, or any prefix/fuzzy/regex operand, is NOT pure boolean
 * -- those need positions or term-expansion the cursor-presence test cannot
 * see, so they keep the collect+recheck+DocidFilter path.
 */
static bool
weave_query_is_pure_boolean(WeaveQuery q)
{
	uint32		i;

	if (q == NULL || q->nitems == 0)
		return true;
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX))
				return false;
		}
		else					/* operator */
		{
			if (it->op != WEAVE_OP_AND && it->op != WEAVE_OP_OR &&
				it->op != WEAVE_OP_NOT)
				return false;	/* PHRASE/NEAR */
		}
	}
	return true;
}

/*
 * BoolGate: lazy boolean @@@ membership over term presence at a pivot docid.
 *
 * For a pure-boolean query (see weave_query_is_pure_boolean) the DocidFilter's
 * pre-collected match set is unnecessary: at each scored pivot the WAND scan
 * already knows which query terms are present (a cursor sits at pivot_docid).
 * `present[t]` (indexed by the ordinal VAL/term index that weave_query_terms
 * assigns) is set by the caller, then bool_gate_admits evaluates the query's
 * RPN with each VAL leaf = present[its term index] and AND/OR/NOT combining.
 * This is the exact @@@ truth value for the doc, computed WITHOUT a collect
 * pass.  `q` is pure boolean, so the stack machine mirrors weave_doc_matches's
 * boolean cases (no phrase, no prefix/fuzzy/regex leaves).
 */
typedef struct BoolGate
{
	WeaveQuery	q;				/* pure-boolean query (NULL => no gate) */
	bool	   *present;			/* present[termidx], nterms entries */
	bool	   *stack;			/* scratch RPN stack, nitems entries */
	int			nterms;
}			BoolGate;

/*
 * Evaluate the pure-boolean query's RPN over the current present[] flags.
 * Leaf VAL i (i = ordinal among VAL items) is true iff present[i]; AND/OR/NOT
 * combine.  Returns the doc's @@@ membership.  NULL gate admits everything.
 */
static inline bool
bool_gate_admits(const BoolGate *g)
{
	WeaveQuery	q;
	bool	   *st;
	int			top = 0;
	int			vi = 0;
	uint32		i;

	if (g == NULL || g->q == NULL)
		return true;
	q = g->q;
	st = g->stack;
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
			st[top++] = g->present[vi++];
		else if (it->op == WEAVE_OP_NOT)
			st[top - 1] = !st[top - 1];
		else if (it->op == WEAVE_OP_AND)
		{
			st[top - 2] = st[top - 2] && st[top - 1];
			top--;
		}
		else					/* WEAVE_OP_OR */
		{
			st[top - 2] = st[top - 2] || st[top - 1];
			top--;
		}
	}
	return top == 1 ? st[0] : false;
}

/*
 * Fill the gate's present[] from which cursors sit at `pivot_docid`, then
 * evaluate.  A term is present iff ANY of its (per-segment) cursors is at the
 * pivot.  NULL gate admits everything (pure-OR / DocidFilter path).
 */
static inline bool
bmw_gate_admits(BoolGate *g, WandCursor *cursors, int nterms, uint64 pivot_docid)
{
	int			i;

	if (g == NULL || g->q == NULL)
		return true;
	for (i = 0; i < g->nterms; i++)
		g->present[i] = false;
	for (i = 0; i < nterms; i++)
		if (cursors[i].docid == pivot_docid)
			g->present[cursors[i].termidx] = true;
	return bool_gate_admits(g);
}

/*
 * weave_search_wand: exact top-k identical to the accumulate path, but using
 * document-at-a-time WAND so that documents which cannot enter the current
 * top-k are skipped via the per-term max-contribution bounds.  Returns the
 * number of (tid, score) results written to *out (palloc'd), capped at k.
 *
 * `filter` (may be NULL) gates heap admission by docid membership: a
 * fully-scored candidate enters the top-k only if the filter admits its docid.
 * This threads the exact @@@ boolean match set into the proven single-pass
 * WAND/MaxScore traversal WITHOUT changing the traversal or the block-skip /
 * threshold math -- filtered docs simply never reach the heap (and so never
 * raise the threshold).
 */
static int
weave_search_bmw(WandCursor *cursors, int nterms, int k, const DocidFilter *filter,
			   BoolGate *gate, ScoredTid **out)
{
	ScoredTid  *heap;			/* min-heap of current top-k by score */
	int			nheap = 0;
	double		threshold = 0.0;
	int			t;

	heap = (ScoredTid *) palloc(Max(k, 1) * sizeof(ScoredTid));

	/* prime each cursor with its first page */
	for (t = 0; t < nterms; t++)
		wand_prime(&cursors[t]);

	for (;;)
	{
		int			i,
					j;
		uint64		pivot_docid;
		double		maxsum;
		double		score;

		CHECK_FOR_INTERRUPTS();	/* per document-at-a-time step; cursor blocks are palloc'd copies, no lock held */

		/* selection-sort cursors by current docid (nterms is small) */
		for (i = 0; i < nterms; i++)
			for (j = i + 1; j < nterms; j++)
				if (cursors[j].docid < cursors[i].docid)
				{
					WandCursor	tmp = cursors[i];

					cursors[i] = cursors[j];
					cursors[j] = tmp;
				}

		if (cursors[0].docid == UINT64_MAX)
			break;				/* all exhausted */

		/*
		 * WAND pivot: accumulate max_contrib in docid order until the running
		 * sum could exceed the threshold; that cursor's docid is the pivot.
		 */
		maxsum = 0.0;
		pivot_docid = UINT64_MAX;
		for (i = 0; i < nterms; i++)
		{
			if (cursors[i].docid == UINT64_MAX)
				break;
			maxsum += cursors[i].max_contrib;
			if (maxsum > threshold || nheap < k)
			{
				pivot_docid = cursors[i].docid;
				break;
			}
		}
		if (pivot_docid == UINT64_MAX)
			break;				/* no document can beat the threshold */

		/*
		 * Block-max WAND (BMW) refinement: the pivot passed the term-wide
		 * bound, but the *current blocks* may bound tighter.  Sum the per-block
		 * max contribution of every cursor whose docid <= pivot; if even that
		 * cannot beat the threshold, no document up to the pivot can enter the
		 * top-k, so skip the earliest cursor past its current 128-block instead
		 * of scoring.  Sound because block max_tf >= every tf in the block.
		 */
		if (nheap >= k)
		{
			double		blocksum = 0.0;
			int			nle = 0;		/* cursors with docid <= pivot */
			int			lei = -1;		/* index of the (single) such cursor */

			for (i = 0; i < nterms; i++)
			{
				if (cursors[i].docid == UINT64_MAX)
					break;
				if (cursors[i].docid <= pivot_docid)
				{
					blocksum += wand_block_max_contrib(&cursors[i]);
					nle++;
					lei = i;
				}
			}
			if (blocksum <= threshold)
			{
				/*
				 * FAST PATH -- exactly one cursor is at/before the pivot AND
				 * no other term's cursor falls within that cursor's current
				 * block's docid range.  Then blocksum bounded the WHOLE block,
				 * every docid in it contains ONLY this term (all other cursors
				 * sit strictly past the block's last docid), so no document in
				 * the block -- even under a conjunctive/AND score -- can beat the
				 * threshold.  Skipping the entire block is exact and O(1).
				 *
				 * The block-range guard is essential for correctness with
				 * multi-term AND: "other cursors are past the PIVOT" does NOT
				 * imply "past the block"; a cursor at docid in (pivot, blocklast]
				 * means a document in the skipped range could contain BOTH terms
				 * and score sum-of-both -- which blocksum (one term) never
				 * bounded.  Skipping it then drops true top-k AND docs (the
				 * pre-existing multi-term AND recall gap).
				 *
				 * SAFE PATH -- two or more cursors sit at/before the pivot, OR a
				 * cursor overlaps the block's docid range: advance every
				 * at-or-before cursor to just past the pivot instead; blocksum
				 * proved nothing up to and including pivot_docid can win, so this
				 * is exact regardless of conjunction.
				 */
				bool		block_isolated = false;

				if (nle == 1 && cursors[lei].blkcount > 0)
				{
					uint64		blocklast = cursors[lei].docids[cursors[lei].blkcount - 1];

					block_isolated = true;
					for (i = 0; i < nterms; i++)
						if (i != lei && cursors[i].docid != UINT64_MAX &&
							cursors[i].docid <= blocklast)
						{
							block_isolated = false;
							break;
						}
				}
				if (block_isolated)
					wand_skip_block(&cursors[lei]);
				else
					for (i = 0; i < nterms; i++)
						if (cursors[i].docid != UINT64_MAX &&
							cursors[i].docid <= pivot_docid)
							wand_seek(&cursors[i], pivot_docid + 1);
				continue;
			}
		}

		/* if the smallest docid equals the pivot, score it fully */
		if (cursors[0].docid == pivot_docid)
		{
			ItemPointerData tid;

			weave_docid_to_tid(pivot_docid, &tid);

			score = 0.0;
			for (i = 0; i < nterms; i++)
				if (cursors[i].docid == pivot_docid)
					score += wand_contrib_cur(&cursors[i]);

			/* push into the top-k min-heap -- but only if the docid is admitted
			 * by the boolean match-set gate.  Two equivalent gates:
			 *  - DocidFilter (filter): binary-search a pre-collected @@@ set
			 *    (used for non-pure-boolean: phrase/near/prefix/fuzzy/regex).
			 *  - BoolGate (gate): evaluate the query's RPN LAZILY over which
			 *    terms have a cursor at the pivot (pure-boolean AND/NOT); no
			 *    collect pass.  For pure-OR both are NULL (admit all).
			 * A rejected doc does NOT count toward k and does NOT raise the
			 * threshold; the traversal and its block-skip math are unchanged
			 * (the threshold may just stay lower longer, costing pruning only,
			 * never correctness). */
			if (docid_admitted(filter, pivot_docid) &&
				bmw_gate_admits(gate, cursors, nterms, pivot_docid))
			{
			if (nheap < k)
			{
				heap[nheap].tid = tid;
				heap[nheap].score = score;
				nheap++;
				if (nheap == k)
				{
					threshold = heap[0].score;
					for (i = 1; i < nheap; i++)
						if (heap[i].score < threshold)
							threshold = heap[i].score;
				}
			}
			else if (score > threshold)
			{
				int			minpos = 0;

				for (i = 1; i < nheap; i++)
					if (heap[i].score < heap[minpos].score)
						minpos = i;
				heap[minpos].tid = tid;
				heap[minpos].score = score;
				threshold = heap[0].score;
				for (i = 1; i < nheap; i++)
					if (heap[i].score < threshold)
						threshold = heap[i].score;
			}
			}					/* end docid_admitted gate */

			/* advance every cursor positioned at the pivot */
			for (i = 0; i < nterms; i++)
				if (cursors[i].docid == pivot_docid)
					wand_next(&cursors[i]);
		}
		else
		{
			/*
			 * Advance every cursor before the pivot up to pivot_docid.  Use a
			 * seek (block-skipping) rather than stepping one posting at a time:
			 * for a high-df term this skips entire 128-blocks whose docids are
			 * all below the pivot, instead of decoding hundreds of thousands of
			 * postings individually (the Q5/Q7 cost).
			 */
			for (i = 0; i < nterms; i++)
				if (cursors[i].docid < pivot_docid)
					wand_seek(&cursors[i], pivot_docid);
		}
	}

	/* release any still-loaded block buffers + doclen page directories */
	for (t = 0; t < nterms; t++)
	{
		if (cursors[t].blkbuf)
			pfree(cursors[t].blkbuf);
		weave_doclen_cursor_free(&cursors[t].doclenc);
	}

	qsort(heap, nheap, sizeof(ScoredTid), cmp_scored_desc);
	*out = heap;
	return nheap;
}

/*
 * weave_search_maxscore: exact top-k via the MaxScore algorithm.  Cursors are
 * split into ESSENTIAL and NON-ESSENTIAL sets by ascending max_contrib: a
 * suffix of low-impact terms whose cumulative max_contrib cannot, by itself,
 * reach the current threshold is non-essential -- a document containing only
 * non-essential terms can never enter the top-k.  We therefore iterate
 * candidate docids from the ESSENTIAL cursors only (document-at-a-time over the
 * smallest essential docid), then add the non-essential terms' contributions by
 * seeking.  As the threshold rises, more terms become non-essential, so long
 * queries do progressively less work.  Complements BMW (which excels on short
 * queries); identical exact top-k.
 */
static int
weave_search_maxscore(WandCursor *cursors, int nterms, int k,
					const DocidFilter *filter, BoolGate *gate, ScoredTid **out)
{
	ScoredTid  *heap;
	int			nheap = 0;
	double		threshold = 0.0;
	double	   *suffix;			/* suffix[i] = sum of max_contrib[i..nterms) */
	int			t,
				i,
				j;
	int			first_essential;	/* cursors[first_essential..) are essential */

	heap = (ScoredTid *) palloc(Max(k, 1) * sizeof(ScoredTid));
	suffix = (double *) palloc((nterms + 1) * sizeof(double));	/* alloc-ok: nterms = query term count */

	for (t = 0; t < nterms; t++)
		wand_prime(&cursors[t]);

	/* order cursors by ascending term-wide max_contrib (once; it is static) */
	for (i = 0; i < nterms; i++)
		for (j = i + 1; j < nterms; j++)
			if (cursors[j].max_contrib < cursors[i].max_contrib)
			{
				WandCursor	tmp = cursors[i];

				cursors[i] = cursors[j];
				cursors[j] = tmp;
			}
	suffix[nterms] = 0.0;
	for (i = nterms - 1; i >= 0; i--)
		suffix[i] = suffix[i + 1] + cursors[i].max_contrib;

	first_essential = 0;

	for (;;)
	{
		uint64		cand = UINT64_MAX;
		double		score;

		CHECK_FOR_INTERRUPTS();	/* per document-at-a-time step; cursor blocks are palloc'd copies, no lock held */

		/* recompute the essential boundary from the current threshold: the
		 * longest low-impact prefix whose max_contrib sum <= threshold is
		 * non-essential */
		if (nheap >= k)
		{
			while (first_essential < nterms &&
				   suffix[first_essential + 1] <= threshold)
				first_essential++;
		}

		/* smallest docid among essential cursors drives the iteration */
		for (i = first_essential; i < nterms; i++)
			if (cursors[i].docid < cand)
				cand = cursors[i].docid;
		if (cand == UINT64_MAX)
			break;				/* essential cursors exhausted */

		/* score cand: essential contributions + upper bound of non-essentials */
		score = 0.0;
		for (i = first_essential; i < nterms; i++)
			if (cursors[i].docid == cand)
				score += wand_contrib_cur(&cursors[i]);

		/* early-exit check: essential score + all non-essential max <= threshold
		 * => cand cannot make the top-k, skip the non-essential lookups */
		if (!(nheap >= k && score + suffix[first_essential] <= threshold))
		{
			/* add exact non-essential contributions by seeking to cand */
			for (i = 0; i < first_essential; i++)
			{
				wand_seek(&cursors[i], cand);
				if (cursors[i].docid == cand)
					score += wand_contrib_cur(&cursors[i]);
			}

			/* gate heap admission by the boolean match-set (DocidFilter for
			 * non-pure-boolean, or the lazy BoolGate over term-presence at cand
			 * for pure-boolean AND/NOT).  A rejected doc never enters the heap
			 * and never raises the threshold.  The essential/non-essential
			 * traversal (incl. the non-essential seeks above) is unchanged --
			 * only heap insertion is gated. */
			if (docid_admitted(filter, cand) &&
				bmw_gate_admits(gate, cursors, nterms, cand))
			{
			if (nheap < k)
			{
				ItemPointerData tid;

				weave_docid_to_tid(cand, &tid);
				heap[nheap].tid = tid;
				heap[nheap].score = score;
				nheap++;
				if (nheap == k)
				{
					threshold = heap[0].score;
					for (i = 1; i < nheap; i++)
						if (heap[i].score < threshold)
							threshold = heap[i].score;
				}
			}
			else if (score > threshold)
			{
				ItemPointerData tid;
				int			minpos = 0;

				weave_docid_to_tid(cand, &tid);
				for (i = 1; i < nheap; i++)
					if (heap[i].score < heap[minpos].score)
						minpos = i;
				heap[minpos].tid = tid;
				heap[minpos].score = score;
				threshold = heap[0].score;
				for (i = 1; i < nheap; i++)
					if (heap[i].score < threshold)
						threshold = heap[i].score;
			}
			}					/* end docid_admitted gate */
		}

		/* advance every essential cursor sitting at cand */
		for (i = first_essential; i < nterms; i++)
			if (cursors[i].docid == cand)
				wand_next(&cursors[i]);
	}

	for (t = 0; t < nterms; t++)
	{
		if (cursors[t].blkbuf)
			pfree(cursors[t].blkbuf);
		weave_doclen_cursor_free(&cursors[t].doclenc);
	}

	qsort(heap, nheap, sizeof(ScoredTid), cmp_scored_desc);
	*out = heap;
	return nheap;
}

/*
 * Dispatch to the top-k algorithm best suited to the query shape.  BMW excels
 * on short queries (tight block-max pruning, cheap pivot); MaxScore does
 * progressively less work as terms become non-essential, winning on long
 * queries / large k.  Both return the identical exact top-k.
 */
static int
weave_search_wand(WandCursor *cursors, int nterms, int k,
				const DocidFilter *filter, BoolGate *gate, ScoredTid **out)
{
	if (nterms >= 4)
		return weave_search_maxscore(cursors, nterms, k, filter, gate, out);
	return weave_search_bmw(cursors, nterms, k, filter, gate, out);
}

/*
 * weave_query_maxhits: an upper bound on the number of documents a query can
 * match, computed by walking the RPN with a stack -- VAL pushes the term's
 * global df; AND/PHRASE -> min of operands; OR -> sum; NOT -> corpus N (a NOT
 * can match almost everything).  Fuzzy/regex terms over-generate and have no
 * cheap df, so any such term makes the bound N (unbounded for our purposes).
 * Used to decide whether the ordering scan can compute the WHOLE result set in
 * one WAND pass (avoiding the adaptive-k recompute) when the result is small.
 */
static double
weave_query_maxhits(Relation index, WeaveQuery q, double N)
{
	WeaveMetaPageData meta;
	double	   *stack;
	int			top = 0;
	uint32		i;
	double		result;

	if (q->nitems == 0)
		return 0;
	weave_read_meta(index, &meta);
	stack = (double *) palloc(q->nitems * sizeof(double));

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_FUZZY | WEAVE_QF_REGEX | WEAVE_QF_PREFIX))
				stack[top++] = N;	/* over-generating: no cheap bound */
			else
			{
				uint64		gdf = 0;	/* summed across segments; uint64 (consumed as double) */
				uint32		s;

				for (s = 0; s < meta.nsegments; s++)
				{
					uint32		df,
								mtf;
					BlockNumber fb;
					uint32		fo;

					if (weave_lookup_dict(index, &meta.segs[s],
										 WEAVE_QUERY_ITEMTEXT(q, it), it->termlen,
										 &df, &mtf, &fb, &fo))
						gdf += df;
				}
				stack[top++] = (double) gdf;
			}
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			/* !x can match up to N docs */
			if (top >= 1)
				stack[top - 1] = N;
		}
		else					/* AND / OR / PHRASE: binary */
		{
			double		b = (top >= 1) ? stack[--top] : 0;
			double		a = (top >= 1) ? stack[--top] : 0;

			if (it->op == WEAVE_OP_OR)
				stack[top++] = a + b;
			else				/* AND, PHRASE: bounded by the smaller side */
				stack[top++] = Min(a, b);
		}
	}
	result = (top >= 1) ? stack[top - 1] : N;
	pfree(stack);
	return Min(result, N);
}

/*
 * weave_topk_visible: shared top-k engine for both the weave_search SRF and the
 * amgettuple ordering scan.  Runs block-max WAND / MaxScore over the index's
 * SEGMENTS for `q`, over-fetches candidates so MVCC visibility filtering still
 * yields k visible rows, drops tombstoned docs, and returns them (palloc'd in
 * the current context) sorted by descending score.  When as_distance is true,
 * each result's .score field is replaced by the ordering distance 1/(1+score)
 * (ascending distance = the same order).  The index must already be open; the
 * base table is opened here for the visibility check.  Returns the number of
 * visible results.
 *
 * NOTE: ranked results cover the merged SEGMENTS only; documents still in the
 * pending write buffer (inserted since the last flush) are searchable by @@@
 * and counted by weave_count(), but are not ranked here until a flush folds them
 * into a segment (automatic on VACUUM, or immediate via weave_merge()).  Ranking
 * pending docs would require per-doc scoring outside the WAND cursors; deferred
 * intentionally, since pending is transient and bounded.
 */
static int
weave_topk_candidates_range(Relation index, WeaveQuery q, int wantk,
						   uint64 docid_lo, uint64 docid_hi, ScoredTid **out)
{
	WeaveMetaPageData meta;
	double		N,
				avgdl;
	const char **terms;
	int		   *lens;
	int			nterms;
	WandCursor *cursors;
	ScoredTid  *cand;
	WeaveDoclenDirCache *doclendir;	/* relcache-cached page directory over the v4
									 * doclen sidecars (built once per backend, keyed
									 * by generation); borrowed by the cursors below */
	WeaveDoclenResident *doclenres = NULL;	/* one shared resident doclen block per
											 * SEGMENT, so a multi-term query does not
											 * decode the same block once per term */
	WeaveTombstones tombs;
	DocidFilter filter;
	DocidFilter *filterp = NULL;
	uint64	   *filter_docids = NULL;
	BoolGate	gate;
	BoolGate   *gatep = NULL;
	int			ncand;
	int			t,
				nactive = 0;
	double		k1 = 1.2;

	if (wantk < 1)
		wantk = 1;

	weave_read_meta(index, &meta);
	N = meta.ndocs < 1.0 ? 1.0 : meta.ndocs;
	avgdl = meta.ndocs > 0 ? meta.sumdoclen / meta.ndocs : 1.0;
	/* relcache page directory over the v4 doclen sidecars (built once per backend,
	 * keyed by meta.generation); NULL if no v4 sidecar segment exists */
	doclendir = weave_doclendir_cache(index, &meta);
	if (doclendir != NULL)
		doclenres = (WeaveDoclenResident *)
			palloc0(sizeof(WeaveDoclenResident) * Max((int) meta.nsegments, 1));

	/*
	 * Boolean-structure gating.  The WAND cursors below rank the term
	 * DISJUNCTION (weave_query_terms flattens operators), a SUPERSET of the @@@
	 * match set for any non-pure-OR query, so heap admission must be gated to
	 * the exact @@@ set.  Two gates, both byte-identical top-k:
	 *
	 *  - PURE-BOOLEAN (AND/OR/NOT over plain terms, no phrase/prefix/fuzzy/
	 *    regex): the LAZY BoolGate.  A doc's @@@ truth is a pure function of
	 *    which query terms are present, which the WAND scan already knows at
	 *    each pivot (which cursors sit at the pivot).  So evaluate the query's
	 *    RPN over cursor-presence at admission time -- NO collect pass.  This
	 *    removes the redundant weave_collect_matches materialization that made
	 *    ranked AND/NOT slow (e.g. `year & hungary` no longer materializes all
	 *    735k `year` postings before the WAND scan).
	 *
	 *  - NON-PURE-BOOLEAN (phrase/near/prefix/fuzzy/regex present): the exact
	 *    @@@ set must be computed (positions / term-expansion the cursor test
	 *    cannot see) -- keep weave_collect_matches + recheck + DocidFilter.
	 *
	 *  - PURE-OR: neither gate (the disjunction IS the @@@ set); NULL, zero
	 *    overhead, as before.
	 */
	if (!weave_query_is_pure_or(q) && weave_query_is_pure_boolean(q))
	{
		/* lazy path: gate built after cursors so nterms is known */
		gatep = &gate;
	}
	else if (!weave_query_is_pure_or(q))
	{
		TidSet		matches;
		bool		recheck;
		int			i;

		weave_collect_matches(index, q, &matches, &recheck);
		/*
		 * matches is the SAME boolean set @@@ uses through this index (it is
		 * built by the identical evaluator).  For the fuzzy funnel/NOT-universe
		 * and PHRASE/NEAR it OVER-generates (recheck=true): an over-long fuzzy
		 * term is a trigram-funnel candidate set, and a PHRASE is the AND-set
		 * (the positionless posting lists cannot enforce adjacency).  The bitmap-
		 * heap scan resolves this with an executor recheck of @@@; the ranked
		 * scan has none, so we recheck here -- shrink matches to the EXACT set
		 * against the heap wdoc.  After this the docid filter is precise, so
		 * the ranked scan never admits a doc "WHERE d @@@ q" would reject.
		 *
		 * COMPLETENESS caveat: the WAND cursors are built from the LITERAL query
		 * terms (weave_query_terms), so a doc that matches only via a fuzzy/prefix/
		 * regex EXPANSION (no posting for the literal term) is never generated as
		 * a ranked candidate.  The recheck only shrinks, so ranked fuzzy/prefix/
		 * regex results are a correct SUBSET of the @@@ matches, not the full set.
		 * PHRASE/NEAR/boolean are exact.  Use @@@ for exhaustive fuzzy/prefix.
		 *
		 * TidSet is TID-sorted and weave_tid_to_docid is monotonic in TID
		 * order, so the docid array comes out sorted (binary-searchable).
		 */
		if (recheck)
			weave_recheck_exact(index, q, &matches);
		if (matches.n > 0)
		{
			filter_docids = (uint64 *) palloc(matches.n * sizeof(uint64));
			for (i = 0; i < matches.n; i++)
				filter_docids[i] = weave_tid_to_docid(&matches.tids[i]);
		}
		filter.docids = filter_docids;
		filter.n = matches.n;
		filterp = &filter;
		/* empty match set: nothing satisfies @@@, so no candidates */
		if (matches.n == 0)
		{
			if (matches.tids)
				pfree(matches.tids);
			*out = NULL;
			return 0;
		}
	}

	nterms = weave_query_terms(q, &terms, &lens);
	/* up to one cursor per (term, segment) */
	cursors = (WandCursor *) palloc(Max(nterms * Max((int) meta.nsegments, 1), 1) *	/* alloc-ok: query terms x <=128 segments */
									sizeof(WandCursor));

	/* per-segment tombstones: each cursor skips docids deleted in its own
	 * segment, so reused heap TIDs live in another segment still rank */
	weave_tombstones_load(index, &meta, &tombs);

	/* v4 doclen sidecar: each cursor gets a FORWARD cursor over its segment's
	 * sidecar chain (initialised per cursor below), reading only the pages that
	 * cover the docids it actually scores -- NOT a whole-segment preload, which
	 * on a many-segment index dominated the scan (the 1.5.0 slowdown report). */

	for (t = 0; t < nterms; t++)
	{
		uint64		gdf = 0;	/* summed across segments; uint64 (feeds IDF as double) */
		uint32		s;
		double		idf;
		double		b = 0.75;

		/* global df across all segments -> IDF (segments share the corpus) */
		for (s = 0; s < meta.nsegments; s++)
		{
			uint32		df,
						max_tf;
			BlockNumber firstblk;
			uint32		firstoff;

			if (weave_lookup_dict(index, &meta.segs[s], terms[t], lens[t],
								 &df, &max_tf, &firstblk, &firstoff))
				gdf += df;
		}
		if (gdf == 0)
			continue;			/* term absent in every segment */
		idf = log(1.0 + (N - (double) gdf + 0.5) / ((double) gdf + 0.5));

		/* one cursor per segment that contains the term */
		for (s = 0; s < meta.nsegments; s++)
		{
			uint32		df,
						max_tf;
			BlockNumber firstblk;
			uint32		firstoff;
			double		mtf;

			if (!weave_lookup_dict(index, &meta.segs[s], terms[t], lens[t],
								  &df, &max_tf, &firstblk, &firstoff))
				continue;
			mtf = (double) max_tf;
			cursors[nactive].index = index;
			cursors[nactive].firstblk = firstblk;
			cursors[nactive].firstoff = firstoff;
			cursors[nactive].df = df;
			cursors[nactive].termidx = t;
			cursors[nactive].blkbuf = NULL;
			cursors[nactive].blkcount = 0;
			cursors[nactive].cur = 0;
			cursors[nactive].docid = 0;
			weave_bm25_factors_init(&cursors[nactive].bm, idf, k1, b, avgdl);
			cursors[nactive].max_contrib =
				weave_bm25_term_bound(&cursors[nactive].bm, mtf);
			cursors[nactive].tombs = &tombs;
			cursors[nactive].segidx = s;
			cursors[nactive].has_doclen_col =
				(meta.segs[s].doclenstart == InvalidBlockNumber);
			weave_doclen_cursor_init(&cursors[nactive].doclenc, index,
									meta.segs[s].doclenstart, doclendir,
									doclenres ? &doclenres[s] : NULL);
			cursors[nactive].docid_lo = docid_lo;
			cursors[nactive].docid_hi = docid_hi;
			{
				sm_cursor_t ini = SM_CURSOR_INIT;

				cursors[nactive].tombcursor = ini;
			}
			nactive++;
		}
	}

	/* build the lazy BoolGate now that nterms is known (pure-boolean path) */
	if (gatep != NULL)
	{
		gate.q = q;
		gate.nterms = nterms;
		gate.present = (bool *) palloc0(Max(nterms, 1) * sizeof(bool));	/* alloc-ok: nterms = query term count */
		gate.stack = (bool *) palloc(Max(q->nitems, 1) * sizeof(bool));
	}

	ncand = weave_search_wand(cursors, nactive, wantk, filterp, gatep, &cand);
	weave_tombstones_free(&tombs);
	if (filter_docids)
		pfree(filter_docids);
	if (gatep != NULL)
	{
		pfree(gate.present);
		pfree(gate.stack);
	}

	*out = cand;
	return ncand;
}

/*
 * weave_topk_visible: serial top-k for the weave_search SRF.  (The amgettuple
 * ordering scan drives the pass itself -- see weave_ord_pass -- so that it can
 * hand out candidates lazily and keep them across a widening.)  Generates
 * candidates over the WHOLE corpus (docid range
 * [0, MAX)) then applies MVCC visibility, over-fetching (wantk = k*4) so k
 * visible rows survive.  When as_distance is true each result's .score is the
 * ordering distance 1/(1+score).  Returns visible results (palloc'd) sorted by
 * descending score.
 *
 * A heavy-delete workload can make more than the over-fetch fraction of the
 * top candidates invisible, leaving nvis < k.  The amgettuple path retries and
 * grows on its own, but the weave_search SRF calls this once, so we grow wantk
 * and re-generate here: double wantk (capped) and retry whenever the loop ends
 * short AND the last batch was full (ncand == wantk, i.e. more candidates
 * existed).  If ncand < wantk the corpus is exhausted, so we stop.
 */
static int
weave_topk_visible(Relation index, WeaveQuery q, int k, bool as_distance,
				  ScoredTid **out)
{
	ScoredTid  *cand;
	ScoredTid  *results = NULL;
	int			ncand;
	int			nvis = 0;
	int			i;
	int			wantk = Max(k * 4, 64);
	int			wantk_cap;
	Snapshot	snap = GetActiveSnapshot();
	Relation	heap;
	IndexFetchTableData *fetch;

	if (k < 1)
		k = 1;
	wantk_cap = Max(k * 64, 4096);

	for (;;)
	{
		/*
		 * Candidate generation reads segment pages under only per-page SHARE
		 * locks off a metapage snapshot, so a concurrent merge/vacuum can free
		 * and recycle them mid-scan (the A1 race); weave_topk_candidates_guarded
		 * brackets that with the directory generation.  The MVCC visibility loop
		 * below reads the heap, not the index, so it needs no guard.
		 */
		ncand = weave_topk_candidates_guarded(index, q, wantk, &cand);

		/* Drop any results from a prior (short) attempt before re-filling. */
		if (results)
			pfree(results);
		results = (ScoredTid *) palloc(k * sizeof(ScoredTid));
		nvis = 0;

		heap = table_open(index->rd_index->indrelid, AccessShareLock);
#if PG_VERSION_NUM >= 190000
		fetch = table_index_fetch_begin(heap, SO_NONE);
#else
		fetch = table_index_fetch_begin(heap);
#endif
		for (i = 0; i < ncand && nvis < k; i++)
		{
			ItemPointerData tid = cand[i].tid;
			bool		call_again = false;
			bool		all_dead = false;
			TupleTableSlot *slot = table_slot_create(heap, NULL);

			if (table_index_fetch_tuple(fetch, &tid, snap, slot,
										&call_again, &all_dead))
			{
				results[nvis] = cand[i];
				if (as_distance)
					results[nvis].score = 1.0 / (1.0 + cand[i].score);
				nvis++;
			}
			ExecDropSingleTupleTableSlot(slot);
		}
		table_index_fetch_end(fetch);
		table_close(heap, AccessShareLock);
		if (cand)
			pfree(cand);

		/*
		 * Enough visible rows, or the corpus is exhausted (ncand < wantk means
		 * generation returned fewer than we asked for -- no more candidates),
		 * or we have hit the growth cap: stop.  Otherwise double wantk and redo.
		 */
		if (nvis >= k || ncand < wantk || wantk >= wantk_cap)
			break;
		wantk = Min(wantk * 2, wantk_cap);
	}

	*out = results;
	return nvis;
}

/*
 * ---------------------- incremental ranked-scan growth (L14) ----------------
 *
 * PostgreSQL gives an index access method no way to learn the query's LIMIT, so
 * a ranked scan must guess how deep to go and widen when the executor asks for
 * more.  Before L14 a widening RECOMPUTED THE WHOLE PASS and threw the previous
 * one away, which made the cost of a deep page a function of HOW MANY TIMES the
 * scan had been redone rather than of the depth reached: from
 * wand_initial_k = 16 a LIMIT 100 query ran three complete passes (16, 64, 256),
 * from 32 two, from 100 one -- so the k frontier came out non-monotonic and the
 * knob traded a small LIMIT against a deep one (bench/RESULTS_WAND_K.md).
 *
 * Two changes make a widening EXTEND the previous pass:
 *
 *  1. Visibility is checked LAZILY, a batch at a time, across the pass's whole
 *     candidate list.  That list was always Max(k*4, 64) wide -- the over-fetch
 *     existed so k rows survived MVCC filtering -- but everything past the k-th
 *     visible row was discarded.  Keeping it lets one pass serve ~4x deeper, and
 *     a small LIMIT now probes the heap FEWER times than before (as many rows as
 *     are actually pulled, not k).
 *
 *  2. The accumulated visible rows survive a widening.  They are not re-probed
 *     and, crucially, not re-emitted: the new pass's candidates are filtered
 *     against the TIDs already materialized, so the scan continues where it
 *     stopped instead of replaying its own output at an offset.
 *
 * What is NOT incremental: the per-term posting cursors still restart at docid 0
 * on a widening, so the wider pass re-reads postings.  That is not fixable by
 * bookkeeping -- a document pruned under the narrow pass's threshold can belong
 * in the wider top-k, so the wider pass must be able to revisit it.  The saving
 * here is the output side (heap probes, re-emission) plus the ~4x deeper reach
 * per pass; the posting decode is task L2's (impact-ordered postings) problem.
 */

/*
 * weave_ord_width: candidate width for a ranked pass at nominal k.
 *
 * Identical to the over-fetch weave_topk_visible has always used, so the FIRST
 * pass of a scan reads exactly what it read before this change; what differs is
 * that all of it is now reachable by the executor.  Clamped so that
 * width * sizeof(ScoredTid) cannot overflow along the x4 widening ladder.
 */
#define WEAVE_ORD_WIDTH_MAX (INT_MAX / 32)

static int
weave_ord_width(int k)
{
	if (k < 1)
		k = 1;
	if (k > WEAVE_ORD_WIDTH_MAX / 4)
		return WEAVE_ORD_WIDTH_MAX;
	return Max(k * 4, 64);
}

/*
 * weave_ord_pass: run one ranked candidate pass at width so->curk and install
 * its result as the scan's candidate list.
 *
 * Candidates already materialized into so->ordered by an earlier, narrower pass
 * are removed from the new list by TID.  That is what makes a widening an
 * extension: those rows keep their original position and distance, are not
 * probed against the heap again, and cannot be emitted twice.  (The predecessor
 * of this code resumed by INDEX -- it recomputed the whole visible list and
 * restarted at the count already returned -- which assumed the wider pass
 * reproduced the narrower one's prefix exactly, including among tied scores.
 * Filtering by TID needs no such assumption.)
 *
 * Ordering across the boundary does still rely on the earlier pass being exact,
 * which it is: a document in an exact top-W1 has fewer than W1 <= W2 documents
 * scoring above it, so nothing the wider pass newly finds outranks a row already
 * handed out.  A document merged into the index BETWEEN two passes could; a
 * ranked scan is not order-stable under concurrent modification and was not
 * before this change either.
 */
static void
weave_ord_pass(Relation index, WeaveScanOpaque so)
{
	ScoredTid  *cand = NULL;
	int			ncand;
	int			i;

	ncand = weave_topk_candidates_guarded(index, so->query, so->curk, &cand);

	/*
	 * COMPLETENESS SIGNAL.  A pass whose top-k heap never filled scored and
	 * admitted EVERY matching document: the threshold stays 0.0 until the heap
	 * holds k entries, and every pruning rule in weave_search_bmw() and
	 * weave_search_maxscore() is gated on nheap >= k, so nothing was skipped.
	 * ncand < curk therefore means "this is the entire match set" -- an exact
	 * stop for the widening ladder, stronger than the weave_query_maxhits
	 * estimate it supplements.
	 */
	so->candfull = (ncand >= so->curk);

	if (ncand > 0 && so->nordered > 0)
	{
		ItemPointerData *seen;
		int			j = 0;

		seen = (ItemPointerData *)
			palloc(so->nordered * sizeof(ItemPointerData));	/* alloc-ok: one TID per row already materialized, bounded by the previous pass width, which allocated a wider array itself */
		for (i = 0; i < so->nordered; i++)
			seen[i] = so->ordered[i].tid;
		qsort(seen, so->nordered, sizeof(ItemPointerData), cmp_tid);
		for (i = 0; i < ncand; i++)
			if (bsearch(&cand[i].tid, seen, so->nordered,
						sizeof(ItemPointerData), cmp_tid) == NULL)
				cand[j++] = cand[i];
		ncand = j;
		pfree(seen);
	}

	if (so->cand)
		pfree(so->cand);
	so->cand = cand;
	so->ncand = ncand;
	so->candpos = 0;
}

/*
 * weave_ord_probe: extend so->ordered with MVCC-visible rows drawn from the
 * candidate list, until it holds `want` rows or the candidates run out.
 *
 * The candidate list is the exact top-N by score, so its visible members are
 * the exact top-M visible in the same order for every M it reaches: appending
 * them in candidate order keeps ordered[] a correct score-ordered prefix.
 */
static void
weave_ord_probe(Relation index, WeaveScanOpaque so, int want)
{
	Snapshot	snap = GetActiveSnapshot();
	Relation	heap;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;

	if (so->candpos >= so->ncand || so->nordered >= want)
		return;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
#if PG_VERSION_NUM >= 190000
	fetch = table_index_fetch_begin(heap, SO_NONE);
#else
	fetch = table_index_fetch_begin(heap);
#endif
	slot = table_slot_create(heap, NULL);

	while (so->candpos < so->ncand && so->nordered < want)
	{
		ScoredTid  *c = &so->cand[so->candpos];
		ItemPointerData tid = c->tid;
		bool		call_again = false;
		bool		all_dead = false;

		so->candpos++;
		if (!table_index_fetch_tuple(fetch, &tid, snap, slot,
									 &call_again, &all_dead))
			continue;

		if (so->nordered >= so->maxordered)
		{
			int			newmax;

			if (so->maxordered == 0)
				/* size to the pass, but do not front-load a very wide one for a
				 * scan that may only pull a handful of rows */
				newmax = Min(Max(so->ncand, 64), 4096);
			else if (so->maxordered > INT_MAX / 2)
				newmax = INT_MAX;
			else
				newmax = so->maxordered * 2;
			if (so->ordered == NULL)
				so->ordered = (ScoredTid *)
					palloc(newmax * sizeof(ScoredTid));	/* alloc-ok: one entry per visible row materialized, bounded by the pass width, which allocated a same-sized array itself */
			else
				so->ordered = (ScoredTid *)
					repalloc(so->ordered, newmax * sizeof(ScoredTid));	/* alloc-ok: as above */
			so->maxordered = newmax;
		}
		so->ordered[so->nordered].tid = c->tid;
		/* ordering distance: 1/(1+score); ascending distance == descending score.
		 * On the <@> path (Z9) cand[].score is ALREADY the edit distance -- a
		 * distance, ascending, in the units the operator returns -- so it is
		 * carried through unchanged.  Inverting it here would have produced a
		 * plausible monotone value and a wrong xs_orderbyvals.  The `<=>` vector
		 * path (F7) is the same case: weave_vec_pass() has already turned the
		 * channel's higher-is-better score into an ascending distance.  So is the
		 * fused path (F2.2): weave_fuse_pass() stores -S, which is fuse()'s own
		 * value and is what the ORDER BY pathkey the planner matched sorts on --
		 * inverting it here would hand the executor a monotone but DIFFERENT
		 * number than the expression it elided the Sort for. */
		so->ordered[so->nordered].score = (so->edistScan || so->vecScan ||
										   so->fuseScan) ? c->score
			: 1.0 / (1.0 + c->score);
		so->nordered++;
	}

	ExecDropSingleTupleTableSlot(slot);
	table_index_fetch_end(fetch);
	table_close(heap, AccessShareLock);
}

/*
 * weave_ord_grow: widen the ranked pass because the executor wants more rows
 * than the current one can supply.  Returns false when the scan is complete.
 *
 * The ONLY correct stops are ones that PROVE no further match exists.  An
 * ORDER BY <=> index scan is an amcanorderbyop (KNN) scan and MUST be able to
 * return EVERY matching tuple in score order -- the executor's LIMIT bounds how
 * many are actually pulled, so the access method must not impose a ceiling of
 * its own (doing so silently truncated a query matching more than the ceiling to
 * that ceiling; see the "orderby distance scan undercounts" report).  Two proofs
 * are used, in this order:
 *
 *  1. !candfull -- the pass's top-k heap never filled, so it pruned nothing and
 *     its candidate list IS the complete match set.  Exact.
 *  2. curk >= maxhits -- the pass was at least as wide as the provable upper
 *     bound on the query's match count (weave_query_maxhits), so no wider pass
 *     can find more.  Nearly redundant now: at that width the heap cannot fill,
 *     so (1) fires anyway.  Kept as a cheap early stop.
 *
 * A broad query with a large (or no) LIMIT is inherently expensive here, but
 * correctness wins; a small-LIMIT first page is still served by the narrow first
 * pass alone.
 */
static bool
weave_ord_grow(Relation index, WeaveScanOpaque so)
{
	if (!so->candfull)
		return false;			/* the pass returned the complete match set */
	if ((double) so->curk >= so->maxhits)
		return false;			/* already as wide as every possible match */
	if (so->curk >= WEAVE_ORD_WIDTH_MAX)
		return false;			/* cannot widen further */

	so->curk = (so->curk > WEAVE_ORD_WIDTH_MAX / 4)
		? WEAVE_ORD_WIDTH_MAX : so->curk * 4;
	weave_ord_pass(index, so);
	return true;
}

/* -------------------------------------------------------------------------
 * The `<=>` vector ordering pass -- task F7
 *
 * One pass is the vector channel's top-k at width so->veck, computed by
 * weave_vec_topk_run() -- the SAME loop the weave_vec_scan() SRF drives, shared
 * rather than copied so that a drift between the operator and its own oracle is
 * impossible (include/weave/vector.h).  This file adds only what a scan needs and
 * an SRF does not: TID resolution, MVCC, and the widening ladder.
 *
 * THE LADDER IS OVER A CANDIDATE WIDTH, and it exists because
 * CORRECTNESS MUST NOT DEPEND ON THE PLANNER'S LIMIT ESTIMATE.  PostgreSQL gives
 * an access method no way to learn a query's LIMIT, and an amcanorderbyop scan must
 * be able to return EVERY matching tuple in order -- a ceiling of the access
 * method's own silently truncates a query that asks for more, which on the lexical
 * side was a real reported bug ("orderby distance scan undercounts", see
 * weave_ord_grow).  So the scan starts narrow, and when the executor drains what is
 * materialized -- via a LIMIT larger than the first pass, via a cursor, via a
 * LIMIT-less query, or via rows the executor's own recheck discarded --
 * weave_vec_grow() runs a wider pass.
 *
 * THE STOP IS A PROOF, not an estimate, and there are two of them:
 *
 *	 1. nhit < veck.  The pass's top-k heap never filled, so its floor stayed at
 *	    -INFINITY for the whole scan; WEAVE_VSCAN_SKIP_BOUND is only reachable when
 *	    a block's bound is <= that floor, and no allowlist is in play so
 *	    WEAVE_VSCAN_SKIP_MASK is not either.  Nothing was skipped, so the pass IS
 *	    every live lane in the index.  Exact, and the analogue of `candfull`.
 *	 2. veck >= nlane.  The pass was at least as wide as the total lane count of
 *	    every bolt it scanned, so no wider pass can find more.  The analogue of
 *	    `curk >= maxhits`, and unlike that one it is a count rather than a bound, so
 *	    it is also what keeps the hit array from growing past the relation.
 *
 * WHERE THE TIDs COME FROM, and it is the paragraph AGENTS.md asks for because
 * getting it wrong FAILS SILENTLY -- a physical (heap-only) TID resolves to no
 * visible tuple, so the plan shows a row count and no error.  These TIDs are NOT
 * manufactured from the heap: weave_docid_to_tid() inverts weave_tid_to_docid(),
 * and the docid it inverts came out of the weft's warp map, which was written from
 * the TID the BUILD CALLBACK or weave_insert() was handed.  Those are already
 * HOT-chain roots -- table_index_build_scan() reports the root for a HOT-updated
 * tuple, and an aminsert TID is the root of a chain that starts with it -- so the
 * mapping is root-preserving and no heap_get_root_tuples() pass is needed or
 * wanted.  (Contrast weave_cgram_heapscan(), which reads the heap itself and
 * therefore must call it; that is the site whose omission cost a debugging round.)
 *
 * WHAT THE DISTANCE IS.  The channel scores in the metric's domain, higher is
 * better (-||q-v||^2 for l2, the inner product for ip), so the ordering distance
 * handed to the executor is the NEGATED score: monotone, ascending, and for l2
 * exactly ||q-v||^2 -- the square of what `<->` computes on the heap, which is the
 * same ordering, and for ip exactly what `<#>` computes.  It is still not the
 * operator's own value, because the index scores QUANTIZED reconstructions, and
 * that one divergence is recorded rather than papered over.  xs_recheckorderby
 * therefore stays false: setting it would ask the executor to re-sort within a
 * reorder queue, which requires the AM's value to be a proven LOWER BOUND on the
 * operator's, and a quantized score is not one.  This is an approximate (ANN)
 * ordering, which is what an ANN index is; sql/vecorderby.sql RECORDS the
 * divergence from an exact float ordering instead of asserting it away.  (A
 * mismatch between the operator's metric and the index's is a different matter
 * entirely and is refused in weave_rescan() -- see the comment there, and
 * doc/specs/VECTOR_CHANNEL.md sect. 8b for the per-metric opclass split that would
 * make it a planner decision.)
 * ------------------------------------------------------------------------- */

/*
 * One pass, bracketed against the A1 race.
 *
 * The bolt loop reads VDIR/VCODES/VWARP pages under per-page SHARE locks off a
 * metapage snapshot, so a concurrent merge or vacuum can free and recycle those
 * pages mid-pass and the scan would score bytes that are no longer the weft.
 * Re-read the directory generation afterwards and redo from a fresh snapshot if it
 * moved, bounded at 10 attempts -- exactly weave_topk_candidates_guarded()'s
 * contract, including its outcome on total failure: no candidates rather than a
 * nonzero count over an array that may be garbage.  (The weave_vec_scan() SRF does
 * NOT do this and never has; it is a diagnostic that reads what is there.)
 */
static WeaveVecTopK *
weave_vec_topk_guarded(Relation index, WeaveScanOpaque so)
{
	int			gen_retries = 0;

	do
	{
		WeaveMetaPageData meta;
		uint32		gen0 = weave_read_meta_generation(index);
		WeaveVecTopK *r;

		weave_read_meta(index, &meta);
		r = weave_vec_topk_run(index, &meta, so->vecQuery, so->veck,
							   (uint16) so->vecAttno, NULL, 0, false);
		if (weave_read_meta_generation(index) == gen0)
			return r;
		pfree(r->hit);
		pfree(r->ctr);
		pfree(r);
	} while (gen_retries++ < 10);

	return NULL;
}

static void
weave_vec_pass(Relation index, WeaveScanOpaque so)
{
	WeaveVecTopK *r = weave_vec_topk_guarded(index, so);
	ScoredTid  *cand;
	int			ncand;
	int			i;

	if (r == NULL)
	{
		/* every attempt raced a merge: no candidates, and the ladder stops */
		if (so->cand)
			pfree(so->cand);
		so->cand = NULL;
		so->ncand = 0;
		so->candpos = 0;
		so->candfull = false;
		so->vecLanes = 0;
		so->vecDone = true;
		return;
	}

	so->vecLanes = r->nlane;
	so->vecDone = (r->nhit < so->veck ||
				   (uint64) so->veck >= r->nlane);
	ncand = r->nhit;

	cand = (ScoredTid *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(ncand, 1) *
												sizeof(ScoredTid));
	for (i = 0; i < ncand; i++)
	{
		weave_docid_to_tid(r->hit[i].docid, &cand[i].tid);
		cand[i].score = -(double) r->hit[i].score;
	}
	pfree(r->hit);
	pfree(r->ctr);
	pfree(r);

	/*
	 * Rows an earlier, narrower pass already materialized are removed by TID, so a
	 * widening EXTENDS the scan instead of repeating its output -- the same
	 * argument weave_ord_pass() makes.  Ordering across the boundary is safe for
	 * the same reason too: a pass at width W1 is the EXACT top-W1 of the channel's
	 * (quantized) score function, because the block bound is sound by contract
	 * (C2), so a document in it has fewer than W1 <= W2 documents scoring above it
	 * and nothing a wider pass newly finds can outrank a row already handed out.
	 * A document merged into the index between two passes could; a ranked scan is
	 * not order-stable under concurrent modification, here or on the lexical side.
	 */
	if (ncand > 0 && so->nordered > 0)
	{
		ItemPointerData *seen;
		int			j = 0;

		seen = (ItemPointerData *)
			WEAVE_ALLOC_MAYBE_HUGE((Size) so->nordered * sizeof(ItemPointerData));
		for (i = 0; i < so->nordered; i++)
			seen[i] = so->ordered[i].tid;
		qsort(seen, so->nordered, sizeof(ItemPointerData), cmp_tid);
		for (i = 0; i < ncand; i++)
			if (bsearch(&cand[i].tid, seen, so->nordered,
						sizeof(ItemPointerData), cmp_tid) == NULL)
				cand[j++] = cand[i];
		ncand = j;
		pfree(seen);
	}

	if (so->cand)
		pfree(so->cand);
	so->cand = cand;
	so->ncand = ncand;
	so->candpos = 0;
	so->candfull = false;		/* the <=> vector ladder stops on vecDone */
}

/*
 * Widen the vector pass x4 because the executor wants more rows than the current
 * one can supply.  Returns false only when a pass PROVED there is nothing further,
 * which is the standard weave_ord_grow() and weave_edist_grow() hold themselves to.
 */
static bool
weave_vec_grow(Relation index, WeaveScanOpaque so)
{
	int64		next;

	if (so->vecDone)
		return false;			/* the pass returned every live lane */
	if (so->veck >= WEAVE_ORD_WIDTH_MAX)
		return false;			/* cannot widen further */

	next = (int64) so->veck * 4;
	if (next > (int64) WEAVE_ORD_WIDTH_MAX)
		next = WEAVE_ORD_WIDTH_MAX;
	/*
	 * Clamp to the lane count the last pass reported.  Without this a x4 step past
	 * the end of the index asks weave_vec_topk_run() for a hit array wider than the
	 * relation -- pure waste, and at relation scale a large one.  vecLanes is 0
	 * only when no bolt carried a matching weft, in which case vecDone is already
	 * set and this line is unreachable.
	 */
	if (so->vecLanes > 0 && (uint64) next > so->vecLanes)
		next = (int64) so->vecLanes;
	if (next <= (int64) so->veck)
		return false;			/* progress is not optional: no wider pass exists */

	so->veck = (int) next;
	weave_vec_pass(index, so);
	return true;
}

/* -------------------------------------------------------------------------
 * The `<@>` edit-distance ordering pass -- task Z9
 *
 * One pass collects EVERY document whose edit distance to the pattern is <= the
 * pass's threshold, exactly, and that completeness is what makes the ladder
 * correct: a document's distance is the minimum over its terms, so if its
 * distance is within the threshold then the term achieving it is within the
 * threshold too and the walk saw it.  Documents beyond the threshold may be
 * missing or (having been reached only through a farther term) mis-scored, and
 * are simply not in this pass's output.
 *
 * The pass also reports `nextthr`: the smallest distance at which anything it did
 * NOT collect could sit.  Two sources, and the second is where the channel's
 * bound earns its place:
 *
 *	 - a term whose exact distance came out above the threshold contributes that
 *	   distance;
 *	 - a dictionary PAGE the bound ruled out contributes the page's bound, which
 *	   is by (C2) a lower bound on every distance on it.
 *
 * So the next pass jumps straight to the next distance that can produce a row.
 * nextthr == INT_MAX means nothing anywhere is farther than the threshold, i.e.
 * this pass returned the complete match set -- the same kind of exact stop
 * weave_ord_grow() gets from !candfull, and stronger than an estimate.
 * ------------------------------------------------------------------------- */

typedef struct EdistHit
{
	ItemPointerData tid;
	int32		dist;
}			EdistHit;

typedef struct EdistAcc
{
	EdistHit   *hits;
	int			n;
	int			cap;
}			EdistAcc;

/* (tid, dist) ascending: the dedup order, so the first of each TID run carries
 * that document's MINIMUM distance -- which is the definition of a document's
 * distance under `<@>`. */
static int
cmp_edist_tid(const void *a, const void *b)
{
	const EdistHit *x = (const EdistHit *) a;
	const EdistHit *y = (const EdistHit *) b;
	int			c = ItemPointerCompare((ItemPointer) &x->tid,
									   (ItemPointer) &y->tid);

	if (c != 0)
		return c;
	return x->dist < y->dist ? -1 : (x->dist > y->dist ? 1 : 0);
}

/* (dist, tid) ascending: the output order.  The TID tie-break is not cosmetic --
 * it is what makes the candidate list a TOTAL order, so a LIMIT that cuts inside
 * a run of equal distances cuts at the same place every time.  Edit distance ties
 * are the common case (a corpus has many terms two edits from anything), so
 * without it the k-th row of `ORDER BY d <@> p LIMIT k` would be arbitrary and
 * sql/edist.sql could not compare against a reference at all. */
static int
cmp_edist_dist(const void *a, const void *b)
{
	const EdistHit *x = (const EdistHit *) a;
	const EdistHit *y = (const EdistHit *) b;

	if (x->dist != y->dist)
		return x->dist < y->dist ? -1 : 1;
	return ItemPointerCompare((ItemPointer) &x->tid, (ItemPointer) &y->tid);
}

static void
edist_acc_add(EdistAcc *a, ItemPointer tid, int dist)
{
	if (a->n >= a->cap)
	{
		if (a->cap > INT_MAX / 2 ||
			(Size) a->cap * 2 * sizeof(EdistHit) > MaxAllocHugeSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_weave: candidate set for this <@> query is too large"),
					 errhint("Add a LIMIT, or restrict the scan with a WHERE clause.")));
		a->cap = Max(a->cap * 2, 256);
		/* One entry per (term, document) pair within the threshold: corpus-scale
		 * for a threshold that admits a common term, hence the huge-safe
		 * variant (make check-alloc). */
		a->hits = a->hits
			? (EdistHit *) WEAVE_REALLOC_MAYBE_HUGE(a->hits, (Size) a->cap * sizeof(EdistHit))
			: (EdistHit *) WEAVE_ALLOC_MAYBE_HUGE((Size) a->cap * sizeof(EdistHit));
	}
	a->hits[a->n].tid = *tid;
	a->hits[a->n].dist = (int32) dist;
	a->n++;
}

/*
 * The term the shuttle is positioned on is within the threshold: read its
 * posting list and record (document, distance) for every posting.
 *
 * Valid only while the shuttle is still positioned on that term -- the entry
 * points into a share-locked buffer, the same lifetime weave_fuzzy_hit()
 * documents.
 */
static void
edist_collect_term(Relation index, const WeaveSegMeta *sg, WeaveShuttle *sh,
				   EdistAcc *acc, int dist, WeaveTombstones *tombs,
				   uint32 segidx)
{
	const WeaveDictEntry *de = weave_edist_shuttle_entry(sh);
	WeavePosting *post;
	TidSet		one;
	int			np;
	int			i;

	if (de == NULL)
		return;
	np = weave_decode_term(index, de->firstposting, de->firstoffset, de->df,
						   &post, NULL, false, NULL, true,
						   sg->doclenstart == InvalidBlockNumber);
	if (np > 0)
	{
		one.tids = (ItemPointerData *)
			WEAVE_ALLOC_MAYBE_HUGE((Size) np * sizeof(ItemPointerData));
		for (i = 0; i < np; i++)
			one.tids[i] = post[i].tid;
		one.n = np;
		/* Per-segment tombstones, applied to THIS segment's contribution only:
		 * a docid deleted in segment A must not suppress a live document that
		 * reused the same heap slot in a newer segment (see
		 * weave_filter_tombstoned_seg). */
		weave_filter_tombstoned_seg(tombs, segidx, &one);
		for (i = 0; i < one.n; i++)
			edist_acc_add(acc, &one.tids[i], dist);
		if (one.tids != NULL)
			pfree(one.tids);
	}
	pfree(post);
}

/* The pending list: documents inserted but not yet folded into a segment, so in
 * no dictionary and invisible to the shuttle.  Scored with weave_doc_min_edist()
 * -- the SAME function the `<@>` operator evaluates on a heap wdoc -- so the two
 * cannot disagree.  Pending tuples are live and are never tombstoned. */
static void
edist_collect_pending(Relation index, const WeaveMetaPageData *meta,
					  WeaveScanOpaque so, EdistAcc *acc, int *nextthr)
{
	BlockNumber blk = meta->pendinghead;

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		WeavePendingIter it;
		WeavePendingRec rec;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();	/* between pages, no buffer lock held */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;				/* truncated by a concurrent weave_vacuum */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		next = WeavePageGetOpaque(page)->nextblk;
		weave_pending_iter_init(&it, page);
		while (weave_pending_iter_next(&it, &rec))
		{
			int			d;

			if (!weave_doc_is_valid(rec.doc, rec.doclen))
				continue;		/* weave_collect_matches warns about these */
			d = weave_doc_min_edist(rec.doc, so->edistPat, so->edistPatLen);
			if (d < 0)
				continue;		/* term-free document: no distance */
			if (d <= so->edistThr)
				edist_acc_add(acc, rec.tid, d);
			else if (d < *nextthr)
				*nextthr = d;
		}
		UnlockReleaseBuffer(buffer);
		blk = next;
	}
}

static void
weave_edist_pass(Relation index, WeaveScanOpaque so)
{
	EdistAcc	acc;
	int			nextthr = INT_MAX;
	int			attempt;
	ScoredTid  *cand;
	int			i;
	int			j;

	/*
	 * A @@@ restriction alongside the ordering clause.  An Index Scan does not
	 * re-evaluate a pushed-down qual, so the pass must apply it; collected once
	 * and cached in the (otherwise unused on this path) plain-scan slots,
	 * because the widening ladder calls this function repeatedly.
	 */
	if (!so->plainInit && so->queryValid && so->query != NULL)
	{
		TidSet		m;

		weave_collect_matches(index, so->query, &m, &so->plainRecheck);
		so->plainTids = m.tids;
		so->nplain = m.n;
		so->plainInit = true;
	}

	acc.hits = NULL;
	acc.n = 0;
	acc.cap = 0;

	for (attempt = 0; attempt < 10; attempt++)
	{
		WeaveMetaPageData meta;
		WeaveTombstones seg_tombs;
		uint32		gen0;
		uint32		s;

		gen0 = weave_read_meta_generation(index);
		weave_read_meta(index, &meta);
		acc.n = 0;
		nextthr = INT_MAX;
		weave_tombstones_load(index, &meta, &seg_tombs);

		for (s = 0; s < meta.nsegments; s++)
		{
			const WeaveSegMeta *sg = &meta.segs[s];
			WeaveShuttle *sh;

			if (sg->dictstart == InvalidBlockNumber)
				continue;
			sh = weave_edist_shuttle_begin(index, sg, so->edistPat,
										   so->edistPatLen,
										   CurrentMemoryContext);
			PG_TRY();
			{
				WeaveWarp	warp = weave_shuttle_seek(sh, 0);

				while (warp != WEAVE_WARP_END)
				{
					int			lower = weave_edist_shuttle_block_lower(sh);
					int			d;

					if (lower > so->edistThr)
					{
						/*
						 * (C2) doing its job: no term on this dictionary page
						 * can be within the threshold, so not one Levenshtein
						 * computation is paid for it.  This is the skip whose
						 * rate bench/RESULTS_EDIST_BOUND.md measures.
						 */
						if (lower < nextthr)
							nextthr = lower;
						warp = weave_edist_shuttle_skip_block(sh);
						continue;
					}
					d = weave_edist_shuttle_dist_le(sh, so->edistThr);
					if (d <= so->edistThr)
					{
						/*
						 * Take the value through the CONTRACT face.
						 * dist_le() is the bounded twin, used to reject
						 * cheaply; score() is (C4), and taking the admitted
						 * term's distance from it puts channel.h's sign
						 * convention -- bigger is better, so a distance is
						 * negated -- on the QUERY PATH rather than in a
						 * function nothing calls.  A mutation run showed why
						 * that matters: with the pass reading dist_le() only,
						 * inverting score()'s sign changed no answer, so the
						 * contract's own convention was untested.  The second
						 * Levenshtein is paid only for terms within the
						 * threshold, which is a handful of the vocabulary --
						 * not for the terms the cheap reject discards.
						 */
						int			exact = -(int) weave_shuttle_score(sh);

						Assert(exact == d);
						edist_collect_term(index, sg, sh, &acc, exact,
										   &seg_tombs, s);
					}
					else if (d < nextthr)
						nextthr = d;
					if (warp >= WEAVE_WARP_END - 1)
						break;
					warp = weave_shuttle_seek(sh, warp + 1);
				}
			}
			PG_FINALLY();
			{
				/* The shuttle holds a pinned, share-locked dictionary buffer
				 * between seeks, so an ERROR or a cancel anywhere in the loop
				 * would otherwise leak the lock out of the query -- the
				 * weave_fuzzy_terms() rationale, verbatim. */
				weave_edist_shuttle_end(sh);
			}
			PG_END_TRY();
		}

		weave_tombstones_free(&seg_tombs);
		edist_collect_pending(index, &meta, so, &acc, &nextthr);

		/*
		 * Concurrency guard, the same one weave_collect_matches applies: the
		 * pages were read under per-page SHARE locks off a metapage snapshot, so
		 * a concurrent merge/vacuum may have freed and recycled them.  If the
		 * directory generation moved, redo from a fresh snapshot.
		 */
		if (weave_read_meta_generation(index) == gen0)
			break;
	}

	/* One entry per document, carrying its MINIMUM distance. */
	if (acc.n > 1)
	{
		qsort(acc.hits, acc.n, sizeof(EdistHit), cmp_edist_tid);
		for (i = 0, j = 1; j < acc.n; j++)
			if (ItemPointerCompare(&acc.hits[i].tid, &acc.hits[j].tid) != 0)
				acc.hits[++i] = acc.hits[j];
		acc.n = i + 1;
	}

	/* the @@@ restriction, if any */
	if (so->plainInit && acc.n > 0)
	{
		for (i = 0, j = 0; i < acc.n; i++)
			if (so->nplain > 0 &&
				bsearch(&acc.hits[i].tid, so->plainTids, so->nplain,
						sizeof(ItemPointerData), cmp_tid) != NULL)
				acc.hits[j++] = acc.hits[i];
		acc.n = j;
	}

	if (acc.n > 1)
		qsort(acc.hits, acc.n, sizeof(EdistHit), cmp_edist_dist);

	/*
	 * Rows an earlier, narrower pass already materialized are removed by TID, so
	 * a widening EXTENDS the scan instead of repeating its output -- the same
	 * argument weave_ord_pass() makes, and here the ordering across the boundary
	 * is trivially safe: every row already handed out had distance <= the
	 * previous threshold, and every row this pass newly finds has distance
	 * above it.
	 */
	if (acc.n > 0 && so->nordered > 0)
	{
		ItemPointerData *seen = (ItemPointerData *)
			palloc(so->nordered * sizeof(ItemPointerData));	/* alloc-ok: one TID per row already materialized, bounded by the previous pass, which allocated a wider array itself */

		for (i = 0; i < so->nordered; i++)
			seen[i] = so->ordered[i].tid;
		qsort(seen, so->nordered, sizeof(ItemPointerData), cmp_tid);
		for (i = 0, j = 0; i < acc.n; i++)
			if (bsearch(&acc.hits[i].tid, seen, so->nordered,
						sizeof(ItemPointerData), cmp_tid) == NULL)
				acc.hits[j++] = acc.hits[i];
		acc.n = j;
		pfree(seen);
	}

	cand = (ScoredTid *) palloc(Max(acc.n, 1) * sizeof(ScoredTid));	/* alloc-ok: one entry per candidate document, and the EdistHit array it is copied from is the same length and was huge-safe */
	for (i = 0; i < acc.n; i++)
	{
		cand[i].tid = acc.hits[i].tid;
		cand[i].score = (double) acc.hits[i].dist;
	}
	if (acc.hits != NULL)
		pfree(acc.hits);

	if (so->cand)
		pfree(so->cand);
	so->cand = cand;
	so->ncand = acc.n;
	so->candpos = 0;
	so->candfull = false;		/* the <@> ladder stops on edistDone, not on this */
	so->edistNext = nextthr;
	so->edistDone = (nextthr == INT_MAX);
}

/*
 * Widen the `<@>` pass to the next distance that can produce a row.  Returns
 * false only when the previous pass PROVED there is nothing farther out, which
 * is the same standard weave_ord_grow() holds itself to: an amcanorderbyop scan
 * must be able to return every matching tuple in order, so a ceiling of the
 * access method's own would silently truncate the result.
 */
static bool
weave_edist_grow(Relation index, WeaveScanOpaque so)
{
	if (so->edistDone)
		return false;			/* the pass returned the complete match set */
	if (so->edistThr >= INT_MAX - 1)
		return false;
	/* Progress is not optional: a nextthr that failed to exceed the current
	 * threshold would repeat the pass forever. */
	if (so->edistNext <= so->edistThr)
		so->edistNext = so->edistThr + 1;
	so->edistThr = so->edistNext;
	weave_edist_pass(index, so);
	return true;
}

/* -------------------------------------------------------------------------
 * The fused multi-channel ordering pass -- task F2.2
 *
 * One pass is the fused top-k (src/am/fuse.c, doc/specs/FUSED_TOPK.md sect. 3) at
 * width so->fusek, run ONCE PER BOLT and merged.  What this file adds to the core
 * is everything the core deliberately does not know about: which shuttles to
 * build, where the weights go, TID resolution, MVCC, and the widening ladder.
 *
 * PER BOLT, AND THAT IS NOT AN OPTIMIZATION.  A fused run advances every one of
 * its channels through ONE position space, and a channel's cursor is per segment:
 * a WandCursor is a (term, segment) pair.  Running one fused loop over the whole
 * index would mean several cursors of the same term standing at the same position
 * and each contributing its own score, which double-counts.  So each bolt gets its
 * own run, its own threshold and its own top-k, and the per-bolt results are merged
 * afterwards -- which is sound for the same reason the lexical ladder's widening is
 * sound: a bolt's exact top-W contains that bolt's entire contribution to the
 * global top-W, so merging exact prefixes yields an exact prefix.
 *
 * THE POSITION SPACE IS THE DOCID SPACE, and choosing it is the decision
 * include/weave/gate.h left to Phase F in as many words ("reconciling them is
 * Phase F's decision, not this task's").  The lexical shuttle publishes
 * weave_tid_to_docid() docids as its warp positions and so does a gate shuttle
 * built from a TidSet, so those two channels already agree and need no
 * translation.  The vector shuttle's warp is a segment-local dense LANE INDEX and
 * the `<@>` shuttle's warp is a position in the DICTIONARY; neither is a docid, so
 * neither can join a run in this space without an adapter that does not exist yet.
 * src/am/fusepath.c therefore offers no path containing one, and
 * doc/specs/FUSED_TOPK.md sect. 7b records the finding, the adapter design, and
 * why refusing beats translating badly.
 *
 * LIVEDOCS ARE THE ONE PLACE THIS RUN DEVIATES FROM (C6), stated rather than
 * quietly done.  channel.h's (C6) says tombstones are applied once, by the scorer,
 * from a warp-indexed bitmap -- which presumes a DENSE warp.  A docid is sparse:
 * a bitmap covering it would need one bit per (heap block x MaxHeapTuplesPerPage)
 * slot, tens of megabytes on a large heap, to carry the same information the
 * channels already hold.  So `live` is NULL and each channel applies its own:
 * a WandCursor skips its OWN SEGMENT's tombstones (wand_skip_own_tombstoned, which
 * is also the per-segment semantics a shared bitmap could not express -- a docid
 * deleted in segment A must not suppress a live document that reused the heap slot
 * in a newer segment), and a gate's key set is already tombstone-filtered by
 * weave_collect_matches().  A VECTOR channel (task F8) had no tombstone logic at all
 * -- the single-channel ORDER BY path leaves dead rows to the heap probe and refills
 * through its widening ladder -- so weave_fuse_vec_warpmap() gives it an `allow`
 * bitmap built on the warp-map pass it already makes.  Inside a fused run the heap
 * probe is not enough: a dead docid the vector channel published would occupy one of
 * the k heap slots and DISPLACE a live document, so the symptom is a missing row
 * rather than a wrong one.  The MVCC check that decides what the user sees is the
 * heap probe in weave_ord_probe() either way; this only decides what is scored.
 *
 * WHERE THE TIDs COME FROM, the paragraph AGENTS.md asks for because getting it
 * wrong FAILS SILENTLY -- a physical (heap-only) TID resolves to no visible tuple,
 * so the plan shows a row count and no error.  They are not manufactured from the
 * heap: a fused hit's position IS a docid, weave_docid_to_tid() inverts
 * weave_tid_to_docid(), and the docid came out of a posting list that was written
 * from the TID the BUILD CALLBACK or weave_insert() was handed.  Both of those are
 * already HOT-chain roots -- table_index_build_scan() reports the root for a
 * HOT-updated tuple, and an aminsert TID is the root of a chain starting with it --
 * so the mapping is root-preserving and no heap_get_root_tuples() pass is needed or
 * wanted.  (Contrast weave_cgram_heapscan(), which reads the heap itself and
 * therefore must call it; that is the site whose omission cost a debugging round.)
 *
 * THE VALUE HANDED TO THE EXECUTOR is -S, the negated weighted sum, which is
 * exactly what fuse() returns (src/am/fusepath.c explains why negation and not
 * 1/(1+S)).  It is ascending, best first, and it is the SAME number the pathkey the
 * planner matched sorts on -- so the elided Sort is honest.  It is not
 * bit-identical to the fallback's value and cannot be: sect. 7a (1) records that
 * weave_distance() outside an index has no corpus, and sect. 3a records that float
 * addition is not associative, so the fused sum's order (required first, then
 * scored by descending weighted ceiling) differs from the fallback's left-to-right.
 * ------------------------------------------------------------------------- */

/*
 * The scored keys and the transport key.  Returns false when this is not a fused
 * scan, which is every scan with no WEAVE_STRAT_FUSE_WEIGHTS key.
 *
 * EVERY REFUSAL HERE IS AN ERROR AND NOT A FALL-BACK, which is the opposite of the
 * rule in src/am/fusepath.c and for the same reason: by the time a key reaches
 * amrescan the planner has already chosen this plan, so the honest failure is loud.
 * None of these is reachable from SQL through the path that file builds; each is a
 * check on that file.
 */
static bool
weave_fuse_rescan(IndexScanDesc scan, WeaveScanOpaque so)
{
	MemoryContext old;
	ArrayType  *arr;
	float4	   *w;
	int			nw;
	int			transport = -1;
	int			nsc = 0;
	int			i;
	int			j;

	for (i = 0; i < scan->numberOfOrderBys; i++)
		if (scan->orderByData[i].sk_strategy == WEAVE_STRAT_FUSE_WEIGHTS)
			transport = i;
	if (transport < 0)
		return false;

	if (scan->orderByData[transport].sk_flags & SK_ISNULL)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("a fused weave index scan was given no weights")));

	arr = DatumGetArrayTypeP(scan->orderByData[transport].sk_argument);
	if (ARR_NDIM(arr) > 1 || ARR_HASNULL(arr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fused weave scan weights must be a one-dimensional array without NULLs")));
	nw = (ARR_NDIM(arr) == 0) ? 0 : ARR_DIMS(arr)[0];
	w = (float4 *) ARR_DATA_PTR(arr);

	/*
	 * WHICH SCORED CHANNELS A FUSED RUN CAN DRIVE, and an order-by key of any other
	 * strategy beside a transport key is a bug in src/am/fusepath.c rather than a
	 * query to serve in some reduced form.  Serving it reduced would silently answer
	 * a two-channel ordering with a one-channel one.
	 *
	 * Three since F8: the lexical `<=>` and the two vector orderings, whose lane
	 * space is relabelled into this run's docid space by include/weave/vecdocmap.h.
	 * `<@>` is still absent and is task F9 -- its positions are DICTIONARY entries,
	 * for which no relabelling exists.
	 */
	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		int			strat;

		if (i == transport)
			continue;
		strat = scan->orderByData[i].sk_strategy;
		if (strat != WEAVE_STRAT_DISTANCE && strat != WEAVE_STRAT_VEC_L2 &&
			strat != WEAVE_STRAT_VEC_IP)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a fused weave index scan cannot serve ORDER BY strategy %d",
							strat),
					 errdetail("The lexical <=> channel and the vector <-> and <#> channels can be fused; see doc/specs/FUSED_TOPK.md sect. 7b and 7c.")));
		if (scan->orderByData[i].sk_flags & SK_ISNULL)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("a fused weave index scan channel was given a NULL query")));

		/*
		 * THE METRIC, CHECKED AGAIN.  src/am/fusepath.c reads the reloption at plan
		 * time and declines to offer a fused path when it disagrees with the
		 * operator, which is what keeps this out of doc/GAPS.md G39 -- so this is
		 * unreachable from SQL and is a check on that file, like every other refusal
		 * in this function.  It is worth its four lines because the failure it
		 * catches is an ORDERING, not an error: a `<->` key answered out of an `ip`
		 * weft returns every row, plausibly, in the wrong order.
		 */
		if (strat == WEAVE_STRAT_VEC_L2 || strat == WEAVE_STRAT_VEC_IP)
		{
			int			want = (strat == WEAVE_STRAT_VEC_IP) ?
				WEAVE_METRIC_IP : WEAVE_METRIC_L2;

			if (weave_index_vec_metric_raw(scan->indexRelation) != want)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a fused weave index scan cannot order by %s on an index whose metric is not %s",
								strat == WEAVE_STRAT_VEC_IP ? "<#>" : "<->",
								strat == WEAVE_STRAT_VEC_IP ? "ip" : "l2")));
		}
		nsc++;
	}

	if (nsc < 2)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a fused weave index scan needs at least two scored channels, got %d",
						nsc)));
	if (nsc != nw)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a fused weave index scan has %d channel(s) but %d weight(s)",
						nsc, nw)));

	/*
	 * COPIED into the scan's own context, all of them.  sk_argument belongs to the
	 * executor's ScanKey and every rung of the widening ladder re-reads the queries
	 * -- arbitrarily later than this call -- which is the same reason the
	 * single-channel vector path copies its query vector.
	 */
	old = MemoryContextSwitchTo(GetMemoryChunkContext(so));
	so->fuseQ = (WeaveQuery *) palloc0(nsc * sizeof(WeaveQuery));	/* alloc-ok: one per fuse() score argument, at most WEAVE_FUSE_MAX_CHAN */
	so->fuseW = (float4 *) palloc(nsc * sizeof(float4));	/* alloc-ok: as above */
	so->fuseStrat = (int *) palloc(nsc * sizeof(int));	/* alloc-ok: as above */
	so->fuseV = (WVec **) palloc0(nsc * sizeof(WVec *));	/* alloc-ok: as above */
	so->fuseA = (AttrNumber *) palloc(nsc * sizeof(AttrNumber));	/* alloc-ok: as above */
	j = 0;
	for (i = 0; i < scan->numberOfOrderBys; i++)
	{
		if (i == transport)
			continue;

		so->fuseStrat[j] = scan->orderByData[i].sk_strategy;
		so->fuseA[j] = scan->orderByData[i].sk_attno;

		if (so->fuseStrat[j] == WEAVE_STRAT_DISTANCE)
		{
			WeaveQuery	q = DatumGetWQuery(scan->orderByData[i].sk_argument);

			so->fuseQ[j] = (WeaveQuery) palloc(VARSIZE_ANY(q));	/* alloc-ok: one query, bounded by the query text */
			memcpy(so->fuseQ[j], q, VARSIZE_ANY(q));
		}
		else
		{
			WVec	   *v = DatumGetWVec(scan->orderByData[i].sk_argument);

			/* Copied for the reason the single-channel vector path copies it
			 * (weave_rescan()): DatumGetWVec() hands back the datum itself when it
			 * is not toasted, sk_argument belongs to the executor's ScanKey, and
			 * every rung of the widening ladder re-reads the vector arbitrarily
			 * later than this call. */
			so->fuseV[j] = (WVec *) palloc(VARSIZE_ANY(v));	/* alloc-ok: one query vector, bounded by WVEC_MAX_DIM */
			memcpy(so->fuseV[j], v, VARSIZE_ANY(v));
		}
		/*
		 * The weight is validated here, before the core can see it, because
		 * include/weave/fuse.h note 3 is about silent damage rather than taste: a
		 * zero weight on a gate's +INF ceiling is 0 * INF = NaN, a NaN threshold
		 * comparison is false, and every prune then quietly switches itself off
		 * while the answers stay plausible.  src/am/fusepath.c validates a Const
		 * array at plan time through the same rules; this is the second line of
		 * the same defence, because a weights array can also arrive from a
		 * catalog this backend did not plan against.
		 */
		if (!isfinite(w[j]) || w[j] <= 0.0f)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("fused weave scan weight %d is %g: weights must be finite and greater than zero",
							j + 1, (double) w[j])));
		so->fuseW[j] = w[j];
		j++;
	}
	so->nfuse = nsc;
	so->fuseScan = true;
	MemoryContextSwitchTo(old);
	return true;
}

/*
 * One fused key's term list and the corpus-global IDF of each term.  IDF is
 * global because segments share one corpus (weave_topk_candidates_range() makes
 * the same choice for the same reason): a per-segment IDF would score the same
 * document differently depending on which bolt it landed in.
 *
 * idf < 0 is the "absent from every segment" sentinel.  A term with df 0 anywhere
 * has no posting cursor to build, and log() of its would-be IDF is not a number
 * worth computing.
 */
typedef struct FuseKeyTerms
{
	const char **terms;
	int		   *lens;
	int			nterms;
	double	   *idf;
} FuseKeyTerms;

/*
 * One VECTOR channel of one bolt (task F8).
 *
 * A lexical key becomes one shuttle per term and needs no per-bolt record; a
 * vector key becomes exactly one shuttle per bolt plus the two arrays that
 * relabel it into the docid space, which do need one.  `adapt` is storage, not a
 * pointer: the core is handed &adapt.chan and must outlive nothing, so keeping it
 * here rather than pallocing it separately means the whole per-bolt vector state
 * is one array recycled with segctx.
 */
typedef struct FuseVecChan
{
	int			slot;			/* this channel's index in ss[] and chans[] */
	uint64	   *docid;			/* the bolt's warp map: lane -> docid, ascending */
	uint32		nlane;
	WeaveVecDocChan adapt;
} FuseVecChan;

/*
 * Materialize a bolt's warp map, and the allowlist that keeps the vector channel's
 * tombstone semantics equal to the lexical channel's.
 *
 * ONE FORWARD PASS, which is all the warp map supports: it is a page chain with no
 * index over its pages (which is exactly why the relabelling needs an array and
 * not a lookup).  The same pass vec_allow_from_docids() makes for the filtered
 * top-k, and the cost doc/specs/FUSED_TOPK.md sect. 7b priced at 8 bytes per lane
 * per bolt per pass -- nine with the bitmap.
 *
 * WHY THE ALLOWLIST IS BUILT HERE AND NOT LEFT NULL.  sect. 7b records that (C6)
 * is not applied by the scorer on the fused path -- `live` is NULL, because a
 * bitmap over the SPARSE docid space would cost tens of megabytes to carry what the
 * channels already hold -- so each channel filters its own segment's tombstones.
 * The lexical channel does (wand_skip_own_tombstoned()).  If the vector channel did
 * not, a docid deleted in this bolt would still be published by it, enter the
 * candidate union, occupy one of the k heap slots, and displace a live document
 * that the heap fetch would then never see.  No wrong row is RETURNED -- the
 * visibility check drops it -- so the symptom would be a MISSING row, which is the
 * failure mode hard rule 1 is about.  The mask is also the cheap kind of filter:
 * weave_vec_scan_block() tests it before it is handed any code bytes.
 *
 * A forward-resume sm_cursor_t is used because the map is docid-ascending, so the
 * tombstone probes arrive in non-decreasing order -- the same amortization
 * wand_cur_own_tombstoned() relies on, and it matters for the same reason: a bolt
 * can carry millions of tombstones.
 *
 * Returns the live lane count.  `*docid_out` and `*allow_out` are allocated in the
 * current context.
 */
static uint32
weave_fuse_vec_warpmap(Relation index, const WeaveVecWeft *w, int segidx,
					   const WeaveTombstones *tombs, uint64 **docid_out,
					   uint64 **allow_out)
{
	WeaveVecWarpCursor wc;
	uint64	   *docid;
	uint64	   *allow;
	uint32		nlane = (uint32) w->meta.nvec;
	uint32		nword = (nlane + 63) / 64;
	uint32		nlive = 0;
	uint32		i;
	sm_cursor_t tombcursor = SM_CURSOR_INIT;
	bool		havetombs = (tombs != NULL && tombs->hasany &&
							 segidx < tombs->nseg && tombs->present[segidx]);

	/* Relation-scale, one entry per document of this bolt: huge-safe, and the
	 * ascending check in weave_vecdoc_chan_init() reads every entry, so nothing is
	 * left uninitialized by not zeroing it. */
	docid = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(nlane, 1) * sizeof(uint64));
	allow = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(nword, 1) * sizeof(uint64));
	memset(allow, 0, (Size) Max(nword, 1) * sizeof(uint64));

	weave_vec_warp_begin(&wc, w);
	for (i = 0; i < nlane; i++)
	{
		uint64		d;
		const char *why = NULL;

		if (!weave_vec_warp_next(&wc, &d, &why))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("bolt %d of index \"%s\" has an unreadable vector warp map",
							segidx, RelationGetRelationName(index)),
					 errdetail("%s.", why != NULL ? why : "unknown reason")));
		docid[i] = d;
		if (havetombs && sm_contains(&tombs->maps[segidx], d, &tombcursor))
			continue;
		allow[i / 64] |= UINT64CONST(1) << (i % 64);
		nlive++;
	}
	weave_vec_warp_end(&wc);

	*docid_out = docid;
	*allow_out = allow;
	return nlive;
}

/* (ascending distance, ascending TID): the fused candidate list's total order.
 * The TID tie-break is not cosmetic, it is what makes a LIMIT that cuts inside a
 * run of equal fused scores cut at the same place every time -- the argument
 * cmp_edist_dist() makes, and ties are reachable here too (two documents matching
 * the same terms with the same tf and length). */
static int
cmp_fuse_cand(const void *a, const void *b)
{
	const ScoredTid *x = (const ScoredTid *) a;
	const ScoredTid *y = (const ScoredTid *) b;

	if (x->score != y->score)
		return x->score < y->score ? -1 : 1;
	return ItemPointerCompare((ItemPointer) &x->tid, (ItemPointer) &y->tid);
}

static void
weave_fuse_pass(Relation index, WeaveScanOpaque so)
{
	MemoryContext socxt = GetMemoryChunkContext(so);
	MemoryContext passctx;
	ScoredTid  *acc = NULL;
	int			nacc = 0;
	int			capacc = 0;
	bool		complete = true;
	bool		stable = false;
	int			attempt;
	int			i;
	int			j;

	/*
	 * The @@@ restriction, collected ONCE and cached in the (otherwise unused on
	 * this path) plain-scan slots, because the widening ladder calls this function
	 * repeatedly -- weave_edist_pass()'s arrangement verbatim.  What differs is
	 * what the set is used FOR: there it is intersected with the pass's output
	 * afterwards, here it becomes a REQUIRED gate channel inside the run, so the
	 * predicate drives pivot selection and a selective one makes the scan SKIP
	 * instead of discarding rows it has already scored.  That is claim 3 of
	 * doc/ARCHITECTURE.md sect. 9 and include/weave/fuse.h note 2's payoff.
	 */
	if (!so->plainInit && so->queryValid && so->query != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(socxt);
		TidSet		m;

		weave_collect_matches(index, so->query, &m, &so->plainRecheck);
		so->plainTids = m.tids;
		so->nplain = m.n;
		so->plainInit = true;
		MemoryContextSwitchTo(old);
	}

	/*
	 * THE PREDICATE ADMITS NOTHING, ANYWHERE.  A required channel is conjunctive,
	 * so an empty gate set makes the whole fused answer empty whatever the scored
	 * channels find.  Answered here, once, rather than per bolt, so that the bolt
	 * loop below has no channel-less special case to get wrong -- and `fuseDone`
	 * is set, because widening a pass cannot make an empty intersection non-empty.
	 */
	if (so->plainInit && so->nplain == 0)
	{
		if (so->cand)
			pfree(so->cand);
		so->cand = NULL;
		so->ncand = 0;
		so->candpos = 0;
		so->candfull = false;
		so->fuseDone = true;
		return;
	}

	passctx = AllocSetContextCreate(CurrentMemoryContext,
									"weave fused pass",
									ALLOCSET_DEFAULT_SIZES);

	/*
	 * The A1 race, bracketed exactly as weave_edist_pass() and
	 * weave_topk_candidates_guarded() do: the bolt loop reads dictionary and
	 * posting pages under per-page SHARE locks off a metapage snapshot, so a
	 * concurrent merge or vacuum can free and recycle them mid-pass.  Re-read the
	 * directory generation and redo from a fresh snapshot if it moved, bounded at
	 * 10 attempts.
	 */
	for (attempt = 0; attempt < 10; attempt++)
	{
		WeaveMetaPageData meta;
		WeaveTombstones tombs;
		WeaveDoclenDirCache *doclendir;
		WeaveDoclenResident *doclenres = NULL;
		FuseKeyTerms *kt;
		MemoryContext old;
		uint32		gen0;
		uint32		s;
		double		N;
		double		avgdl;
		int			qi;
		int			t;
		WandCursor *cursors;
		WeaveShuttle **ss;
		WeaveFuseChan *chans;
		WeaveFuseChan **cp;
		FuseVecChan *vc;
		WeaveFuseHit *heap;
		MemoryContext segctx;

		MemoryContextReset(passctx);
		old = MemoryContextSwitchTo(passctx);

		nacc = 0;
		complete = true;
		gen0 = weave_read_meta_generation(index);
		weave_read_meta(index, &meta);
		N = meta.ndocs < 1.0 ? 1.0 : meta.ndocs;
		avgdl = meta.ndocs > 0 ? meta.sumdoclen / meta.ndocs : 1.0;

		/* the relcache page directory over the v4 doclen sidecars, and one shared
		 * resident block per segment so a multi-term run does not decode the same
		 * block once per term -- both borrowed by the cursors below */
		doclendir = weave_doclendir_cache(index, &meta);
		if (doclendir != NULL)
			doclenres = (WeaveDoclenResident *)
				palloc0(sizeof(WeaveDoclenResident) *
						Max((int) meta.nsegments, 1));
		weave_tombstones_load(index, &meta, &tombs);

		kt = (FuseKeyTerms *) palloc0(so->nfuse * sizeof(FuseKeyTerms));	/* alloc-ok: one per fuse() score argument, at most WEAVE_FUSE_MAX_CHAN */
		for (qi = 0; qi < so->nfuse; qi++)
		{
			/* A vector key has no terms and no IDF: its channel is one shuttle per
			 * bolt, built below.  palloc0 above already left nterms zero, and every
			 * loop over kt[qi] is bounded by it, so the vector keys simply fall
			 * through -- which is why this is a `continue` and not a parallel
			 * structure. */
			if (so->fuseStrat[qi] != WEAVE_STRAT_DISTANCE)
				continue;

			kt[qi].nterms = weave_query_terms(so->fuseQ[qi], &kt[qi].terms,
											  &kt[qi].lens);
			kt[qi].idf = (double *)
				palloc(Max(kt[qi].nterms, 1) * sizeof(double));	/* alloc-ok: one per query term */
			for (t = 0; t < kt[qi].nterms; t++)
			{
				uint64		gdf = 0;

				for (s = 0; s < meta.nsegments; s++)
				{
					uint32		df;
					uint32		max_tf;
					BlockNumber firstblk;
					uint32		firstoff;

					if (weave_lookup_dict(index, &meta.segs[s], kt[qi].terms[t],
										  kt[qi].lens[t], &df, &max_tf,
										  &firstblk, &firstoff))
						gdf += df;
				}
				kt[qi].idf[t] = (gdf == 0) ? -1.0
					: log(1.0 + (N - (double) gdf + 0.5) / ((double) gdf + 0.5));
			}
		}

		/*
		 * PER-BOLT SCRATCH, ALLOCATED ONCE PER ATTEMPT.  The cursor array, the
		 * shuttle array and the top-k heap are sized by the core's channel cap and
		 * by the pass width, never by the bolt count, so allocating them inside the
		 * bolt loop would hold nsegments copies of all three alive until the pass
		 * ended -- up to 128 x fusek heap entries at the top of the ladder.  What
		 * actually has to be recycled per bolt is `segctx`: the shuttles, their own
		 * contexts, and the posting block buffers wand_load_block() pallocs while a
		 * cursor walks.
		 */
		cursors = (WandCursor *)
			palloc(WEAVE_FUSE_MAX_CHAN * sizeof(WandCursor));	/* alloc-ok: fixed at the core's channel cap */
		ss = (WeaveShuttle **)
			palloc(WEAVE_FUSE_MAX_CHAN * sizeof(WeaveShuttle *));	/* alloc-ok: as above */
		chans = (WeaveFuseChan *)
			palloc(WEAVE_FUSE_MAX_CHAN * sizeof(WeaveFuseChan));	/* alloc-ok: as above */
		cp = (WeaveFuseChan **)
			palloc(WEAVE_FUSE_MAX_CHAN * sizeof(WeaveFuseChan *));	/* alloc-ok: as above */
		vc = (FuseVecChan *)
			palloc(WEAVE_FUSE_MAX_CHAN * sizeof(FuseVecChan));	/* alloc-ok: as above */

		/* One heap entry per requested candidate.  fusek climbs the x4 ladder to
		 * WEAVE_ORD_WIDTH_MAX, so this is a corpus-scale allocation and takes the
		 * huge-safe variant (make check-alloc). */
		heap = (WeaveFuseHit *)
			WEAVE_ALLOC_MAYBE_HUGE((Size) so->fusek * sizeof(WeaveFuseHit));

		segctx = AllocSetContextCreate(passctx, "weave fused bolt",
									   ALLOCSET_DEFAULT_SIZES);

		for (s = 0; s < meta.nsegments; s++)
		{
			const WeaveSegMeta *sg = &meta.segs[s];
			MemoryContext segold;
			WeaveFuseState st;
			WeaveFuseError err;
			int			nch = 0;
			int			nscored = 0;
			int			nvc = 0;
			int			nh;

			if (sg->dictstart == InvalidBlockNumber)
				continue;

			MemoryContextReset(segctx);
			segold = MemoryContextSwitchTo(segctx);

			/*
			 * ONE SHUTTLE PER QUERY TERM, which is the whole reason the lexical
			 * channel has a shuttle at all (src/query/lexshuttle.c): a single
			 * shuttle wrapping the whole WAND would put a second top-k and a second
			 * threshold inside the fused loop's threshold, which IS the
			 * over-fetch-and-reconcile shape fuse.h exists to replace.  So a
			 * three-term query is three scored channels that the one fused
			 * threshold prunes against, alongside any gate.
			 */
			for (qi = 0; qi < so->nfuse; qi++)
			{
				for (t = 0; t < kt[qi].nterms; t++)
				{
					uint32		df;
					uint32		max_tf;
					BlockNumber firstblk;
					uint32		firstoff;
					WandCursor *c;
					WeaveShuttle *sh;
					sm_cursor_t ini = SM_CURSOR_INIT;

					if (kt[qi].idf[t] < 0.0)
						continue;	/* absent from every segment */
					if (!weave_lookup_dict(index, sg, kt[qi].terms[t],
										   kt[qi].lens[t], &df, &max_tf,
										   &firstblk, &firstoff))
						continue;	/* absent from THIS segment */

					/*
					 * Unreachable through src/am/fusepath.c, which refuses a shape
					 * whose plan-time term count plus one gate exceeds the cap.
					 * Checked anyway, because the alternative to an error is
					 * DROPPING a channel, and a dropped scored channel is a
					 * different ranking returned without a word.  One slot is held
					 * back for the gate for the same reason.
					 */
					if (nch >= WEAVE_FUSE_MAX_CHAN - 1)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("a fused weave index scan needs more than %d channels",
										WEAVE_FUSE_MAX_CHAN),
								 errhint("Use fewer query terms, or move a term into a WHERE clause so it becomes a gate.")));

					c = &cursors[nch];
					c->index = index;
					c->firstblk = firstblk;
					c->firstoff = firstoff;
					c->df = df;
					c->termidx = t;
					c->blkbuf = NULL;
					c->blkcount = 0;
					c->cur = 0;
					c->docid = 0;
					weave_bm25_factors_init(&c->bm, kt[qi].idf[t], 1.2, 0.75,
											avgdl);
					c->max_contrib = weave_bm25_term_bound(&c->bm,
														   (double) max_tf);
					c->tombs = &tombs;
					c->segidx = s;
					c->has_doclen_col =
						(sg->doclenstart == InvalidBlockNumber);
					weave_doclen_cursor_init(&c->doclenc, index,
											 sg->doclenstart, doclendir,
											 doclenres ? &doclenres[s] : NULL);
					c->docid_lo = 0;
					c->docid_hi = UINT64_MAX;
					c->tombcursor = ini;
					wand_prime(c);

					sh = weave_lex_shuttle_begin(c, segctx);

					/*
					 * HERE IS WHERE A WEIGHT REACHES A CHANNEL, and it is the only
					 * place: the transport array's j-th entry became so->fuseW[qi]
					 * in weave_fuse_rescan(), and every term of that key carries it.
					 * weave_fuse_wrap_shuttles() copies the field and the core does
					 * the multiplying, exactly once, which is what makes (C2)
					 * survive weighting (channel.h: w * bound >= w * score needs
					 * only w > 0).  A shuttle that applied the weight itself would
					 * double-count AND scale the bound, so (C2) would still hold
					 * and the scores would simply be wrong -- the undetectable kind.
					 */
					sh->weight = so->fuseW[qi];
					ss[nch++] = sh;
					nscored++;
				}
			}

			/*
			 * ONE SHUTTLE PER VECTOR KEY (task F8), ADAPTED INTO THE DOCID SPACE.
			 *
			 * The asymmetry with the lexical loop above is the channels', not this
			 * code's: a lexical key's positions come from one posting list per term,
			 * so a term is a channel, whereas a vector key's come from the bolt's
			 * whole code weft, so a KEY is a channel.  That is also why a vector key
			 * contributes exactly 1 to the plan-time channel count in
			 * src/am/fusepath.c while a lexical key contributes its term count.
			 *
			 * A BOLT WITHOUT A WEFT FOR THIS COLUMN IS SKIPPED, not an error, and it
			 * is the same semantics as a query term absent from a segment: the
			 * channel contributes nothing here.  It is reachable without corruption
			 * -- a bolt written before the vector column existed, or one whose every
			 * row had a NULL vector -- and the alternative, refusing the scan, would
			 * make one such bolt break every fused query on the index.
			 */
			for (qi = 0; qi < so->nfuse; qi++)
			{
				WeaveVecWeft w;
				BlockNumber root;
				const char *why = NULL;
				uint16		wattnum = 0;
				uint64	   *dmap;
				uint64	   *allow;
				uint32		nlane;
				WeaveShuttle *sh;

				if (so->fuseStrat[qi] == WEAVE_STRAT_DISTANCE)
					continue;

				root = weave_vec_weft_locate(index, sg, &wattnum);
				if (root == InvalidBlockNumber)
					continue;	/* this bolt carries no vector weft */

				/* ROUTE BY ATTRIBUTE.  An index may carry more than one wvec
				 * column, and scoring the weft of a different column is a wrong
				 * answer with a correct row count -- the failure
				 * include/weave/vector.h says the recorded attnum exists to
				 * prevent, and the reason so->fuseA[] is carried at all. */
				if (wattnum != 0 && so->fuseA[qi] != 0 &&
					wattnum != (uint16) so->fuseA[qi])
					continue;

				if (!weave_vec_weft_open(index, root, &w, &why))
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("bolt %u of index \"%s\" has an unreadable vector weft at block %u",
									s, RelationGetRelationName(index), root),
							 errdetail("%s.", why != NULL ? why : "unknown reason")));

				if (w.meta.nvec == 0)
					continue;	/* nothing to publish; see vecdocmap.h's refusals */

				if (nch >= WEAVE_FUSE_MAX_CHAN - 1)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("a fused weave index scan needs more than %d channels",
									WEAVE_FUSE_MAX_CHAN),
							 errhint("Use fewer query terms, or move a term into a WHERE clause so it becomes a gate.")));

				nlane = weave_fuse_vec_warpmap(index, &w, (int) s, &tombs,
											   &dmap, &allow);
				if (nlane == 0)
					continue;	/* every document of this bolt is tombstoned */

				/*
				 * The allowlist is the shuttle's, not the core's: `live` stays NULL
				 * for the reason weave_fuse_vec_warpmap() gives.  nwarp is the LANE
				 * count, because that is the space the bitmap indexes -- the
				 * relabelling happens one level up, in the adapter.
				 */
				sh = weave_vec_shuttle_begin(&w, (int) s, so->fuseV[qi]->x,
											 (int) so->fuseV[qi]->dim,
											 allow, (WeaveWarp) w.meta.nvec,
											 so->fuseW[qi]);
				sh->weight = so->fuseW[qi];

				vc[nvc].slot = nch;
				vc[nvc].docid = dmap;
				vc[nvc].nlane = (uint32) w.meta.nvec;
				nvc++;

				ss[nch++] = sh;
				nscored++;
			}

			/*
			 * nscored == 0 means no scored channel reaches this bolt, so this bolt
			 * has no candidate at all: include/weave/fuse.h defines the candidate
			 * set as the UNION of the scored channels' positions intersected with
			 * the required ones', and a run with gates alone would pad the top-k
			 * with documents containing none of the query.  `complete` is untouched
			 * -- there was nothing here to prune.
			 */
			if (nscored > 0)
			{
				if (so->plainInit)
				{
					TidSet		gateset;

					/*
					 * `required` COMES FROM WHERE THE KEY ARRIVED, never from the
					 * channel's kind: this set came from a WHERE clause, so it is a
					 * predicate, and weave_gate_shuttle_from_tidset() sets
					 * `required` on the shuttle it returns.  sect. 3a (2) and
					 * channel.h's (C5) note both record the falsification --
					 * src/query/edist.c labels a SCORED channel WEAVE_CH_FUZZY, so
					 * a kind-based inference would apply a conjunctive veto to a
					 * distance channel and silently drop every row it ranks.
					 *
					 * WEAVE_CH_DOCVALS is the kind, and it is a reporting choice
					 * with one constraint: weave_gate_shuttle_begin() refuses a
					 * SCORED kind outright (its counters would claim posting work
					 * that did not happen) and WEAVE_CH_LEXICAL is scored.  What
					 * this gate walks is a materialized set of document ids, which
					 * is what a docvalues predicate is; the kind reaches nothing
					 * but EXPLAIN and the per-channel counters.
					 *
					 * The empty-set case cannot arrive here: it is answered once,
					 * before the bolt loop, so this loop has no channel-less
					 * special case to get wrong.
					 */
					gateset.tids = so->plainTids;
					gateset.n = so->nplain;
					ss[nch++] = weave_gate_shuttle_from_tidset(WEAVE_CH_DOCVALS,
															   &gateset,
															   segctx);
				}

				weave_fuse_wrap_shuttles(chans, ss, nch);
				for (i = 0; i < nch; i++)
					cp[i] = &chans[i];

				/*
				 * AND THE VECTOR CHANNELS ARE HANDED TO THE CORE THROUGH THEIR
				 * ADAPTERS (task F8), which is the one place this pass departs from
				 * "wrap every shuttle and go".  The wrapped channel publishes
				 * segment-local LANE indices; the core's positions are docids.
				 * include/weave/vecdocmap.h relabels one into the other through the
				 * bolt's warp map and argues why (C1) and (C2) both survive.
				 */
				for (i = 0; i < nvc; i++)
				{
					const char *why = NULL;

					if (weave_vecdoc_chan_init(&vc[i].adapt, &chans[vc[i].slot],
											   vc[i].docid, vc[i].nlane,
											   &why) != 0)
						ereport(ERROR,
								(errcode(ERRCODE_INDEX_CORRUPTED),
								 errmsg("bolt %u of index \"%s\" cannot drive a fused vector channel",
										s, RelationGetRelationName(index)),
								 errdetail("%s.", why != NULL ? why : "unknown reason")));
					cp[vc[i].slot] = &vc[i].adapt.chan;
				}

				/*
				 * nwarp is the END SENTINEL, not a document count, because the
				 * position space is the SPARSE docid space (see this section's
				 * header): there is no dense upper bound to give, the channels
				 * themselves run out, and `live` is NULL so nwarp is not doing
				 * double duty as a bitmap extent.
				 */
				err = weave_fuse_init(&st, cp, nch, heap, so->fusek, NULL,
									  WEAVE_FUSE_END);
				if (err != WEAVE_FUSE_OK)
					weave_fuse_error(&st, err);
#ifdef USE_ASSERT_CHECKING
				/* Turn every score() into a checked (C2) assertion under cassert,
				 * the same choice already made for the score()-returns-distance
				 * convention elsewhere in this file.  Set AFTER init, which zeroes
				 * it. */
				st.check_bounds = 1;
#endif
				err = weave_fuse_run(&st);
				if (err != WEAVE_FUSE_OK)
					weave_fuse_error(&st, err);
				nh = weave_fuse_drain(&st, heap);

				/*
				 * THE COMPLETENESS SIGNAL, and it is a proof rather than an
				 * estimate.  A run whose heap never filled kept theta at -INFINITY
				 * for its whole length, and all three prunes in src/am/fuse.c
				 * compare against theta -- the block bound (`ub <= theta`),
				 * incremental abandonment (`s + csuffix <= theta`) and the
				 * essential/non-essential partition (`suffix[split-1] <= theta`,
				 * which cannot move while theta is -INF).  So nothing was skipped
				 * and this run scored every candidate its channels can generate.
				 * It is an AND over the bolts: one bolt proving completeness says
				 * nothing about another.
				 */
				if (nh >= so->fusek)
					complete = false;

				if (nh > 0)
				{
					MemoryContext accold = MemoryContextSwitchTo(socxt);

					if (nacc + nh > capacc)
					{
						int			newcap = Max(capacc * 2,
												 Max(nacc + nh, 64));

						/* One entry per merged candidate: bolts x the pass width,
						 * so corpus-scale at the top of the ladder (make
						 * check-alloc). */
						acc = (acc == NULL)
							? (ScoredTid *) WEAVE_ALLOC_MAYBE_HUGE((Size) newcap * sizeof(ScoredTid))
							: (ScoredTid *) WEAVE_REALLOC_MAYBE_HUGE(acc, (Size) newcap * sizeof(ScoredTid));
						capacc = newcap;
					}
					MemoryContextSwitchTo(accold);

					for (i = 0; i < nh; i++)
					{
						/*
						 * A fused hit's position IS a docid, so the TID is the
						 * inverse of the mapping the build callback applied to a
						 * HOT-CHAIN ROOT -- see this section's header for why that
						 * matters and why no heap_get_root_tuples() pass belongs
						 * here.  The score is fuse()'s own value, -S, ascending.
						 */
						weave_docid_to_tid((uint64) heap[i].warp,
										   &acc[nacc].tid);
						acc[nacc].score = -(double) heap[i].score;
						nacc++;
					}
				}

				for (i = 0; i < nch; i++)
					ss[i]->ops->end(ss[i]);
			}

			MemoryContextSwitchTo(segold);
		}

		weave_tombstones_free(&tombs);
		MemoryContextSwitchTo(old);

		if (weave_read_meta_generation(index) == gen0)
		{
			stable = true;
			break;
		}
	}

	MemoryContextDelete(passctx);

	/*
	 * EVERY ATTEMPT RACED A MERGE.  Then the last attempt's hits may have come off
	 * recycled pages, so they are DISCARDED rather than returned -- the outcome
	 * weave_topk_candidates_guarded() and weave_vec_pass() both specify, and the
	 * one weave_edist_pass() does not (it keeps the last attempt; that is a
	 * pre-existing divergence, noted here rather than changed by this task).  The
	 * ladder stops, because widening would only race again.
	 */
	if (!stable)
	{
		nacc = 0;
		complete = true;
	}

	if (nacc > 1)
		qsort(acc, nacc, sizeof(ScoredTid), cmp_fuse_cand);

	/*
	 * Truncate to the pass width only when the pass PRUNED.  Merging exact
	 * per-bolt top-fusek lists gives an exact global top-fusek, so cutting there
	 * loses nothing the ladder cannot recover by widening; when every bolt proved
	 * completeness the merged list is the whole match set and cutting it would
	 * drop rows the ladder is about to declare there are none of.
	 */
	if (!complete && nacc > so->fusek)
		nacc = so->fusek;

	/*
	 * Rows an earlier, narrower pass already materialized are removed by TID, so a
	 * widening EXTENDS the scan instead of repeating its output -- the argument
	 * weave_ord_pass() makes, and it holds here for the same reason: a pass at
	 * width W1 is the exact top-W1 of the fused score (the bounds are sound by
	 * (C2), which is what the property test asserts), so a document in it has
	 * fewer than W1 <= W2 documents scoring above it and nothing a wider pass
	 * newly finds can outrank a row already handed out.  A document merged into
	 * the index between two passes could; a ranked scan is not order-stable under
	 * concurrent modification, here or on any other channel.
	 */
	if (nacc > 0 && so->nordered > 0)
	{
		ItemPointerData *seen = (ItemPointerData *)
			WEAVE_ALLOC_MAYBE_HUGE((Size) so->nordered * sizeof(ItemPointerData));

		for (i = 0; i < so->nordered; i++)
			seen[i] = so->ordered[i].tid;
		qsort(seen, so->nordered, sizeof(ItemPointerData), cmp_tid);
		for (i = 0, j = 0; i < nacc; i++)
			if (bsearch(&acc[i].tid, seen, so->nordered,
						sizeof(ItemPointerData), cmp_tid) == NULL)
				acc[j++] = acc[i];
		nacc = j;
		pfree(seen);
	}

	if (so->cand)
		pfree(so->cand);
	so->cand = acc;
	so->ncand = nacc;
	so->candpos = 0;
	so->candfull = !complete;
	so->fuseDone = complete;
}

/*
 * Widen the fused pass x4 because the executor wants more rows than the current
 * one can supply.  Returns false only when a pass PROVED there is nothing
 * further, which is the standard weave_ord_grow(), weave_edist_grow() and
 * weave_vec_grow() all hold themselves to: an amcanorderbyop scan must be able to
 * return EVERY matching tuple in order, so a ceiling of the access method's own
 * silently truncates a query that asks for more.
 */
static bool
weave_fuse_grow(Relation index, WeaveScanOpaque so)
{
	if (so->fuseDone)
		return false;			/* every bolt scored its whole candidate set */
	if ((double) so->fusek >= so->maxhits)
		return false;			/* already as wide as every possible match */
	if (so->fusek >= WEAVE_ORD_WIDTH_MAX)
		return false;			/* cannot widen further */

	so->fusek = (so->fusek > WEAVE_ORD_WIDTH_MAX / 4)
		? WEAVE_ORD_WIDTH_MAX : so->fusek * 4;
	weave_fuse_pass(index, so);
	return true;
}

/*
 * weave_topk_candidates_guarded: weave_topk_candidates_range over the whole
 * corpus, bracketed against the A1 race.
 *
 * Candidate generation reads segment pages under only per-page SHARE locks off a
 * metapage snapshot, so a concurrent merge/vacuum can free and recycle those
 * pages mid-scan.  Re-read the directory generation afterwards: if it moved the
 * candidates may have come off recycled pages, so discard them and redo from a
 * fresh snapshot, bounded at 10 attempts.  If every attempt races, return zero
 * candidates -- never a nonzero count with a NULL array, which is what the
 * inline copy of this loop this function replaced could do.
 */
static int
weave_topk_candidates_guarded(Relation index, WeaveQuery q, int wantk,
							 ScoredTid **out)
{
	int			gen_retries = 0;

	do
	{
		ScoredTid  *cand = NULL;
		uint32		gen0 = weave_read_meta_generation(index);
		int			ncand = weave_topk_candidates_range(index, q, wantk, 0,
														UINT64_MAX, &cand);

		if (weave_read_meta_generation(index) == gen0)
		{
			*out = cand;
			return ncand;
		}
		if (cand)
			pfree(cand);
	} while (gen_retries++ < 10);

	*out = NULL;
	return 0;
}

/*
 * weave_count_dictdf_fastpath: answer count(*) for a SINGLE plain positive term
 * straight from the dictionary df, with ZERO posting decode and ZERO heap
 * probe, when it is provably exact.  Returns the count, or -1 when any gate
 * fails (caller then takes the full weave_collect_matches path).
 *
 * A term's dictionary df is the number of documents that contained the term
 * AT INDEX TIME, summed here across all segments.  Sum(df) equals the count of
 * documents visible to the current snapshot ONLY under all of these gates --
 * each closes a way Sum(df) could diverge from the live/visible truth:
 *
 *  (1) The query is exactly one plain positive term: nitems==1, the item is a
 *      WEAVE_QI_VAL, and it carries none of PREFIX/FUZZY/REGEX (a prefix matches
 *      many dict terms, fuzzy/regex fan out via the trigram funnel -- none is a
 *      single df).  No operators means no AND/OR/NOT/PHRASE.  So the match set
 *      is exactly this one term's postings, i.e. exactly Sum(df) documents.
 *      (This term also never sets recheck, so the full path would be exact too.)
 *
 *  (2) Every segment has ndeleted==0 AND livedocslen==0 (no tombstones).  A
 *      tombstone marks an index posting whose heap doc was deleted; df still
 *      counts it, so ANY tombstone makes Sum(df) an OVERCOUNT.  Requiring zero
 *      tombstones everywhere means every counted posting is a doc that was
 *      never deleted-then-vacuumed.
 *
 *  (3) npending==0.  Newly inserted docs live verbatim in the pending list
 *      until merged; they are NOT in any segment df.  A nonzero pending list
 *      could add matches df does not see (undercount), so bail.
 *
 *  (4) The heap is ENTIRELY all-visible to this snapshot: every heap page is
 *      marked all-visible in the visibility map.  The VM all-visible bit is set
 *      only when every tuple on the page is visible to ALL snapshots (it is the
 *      same guarantee the full path relies on to skip a heap probe), so this is
 *      snapshot-safe -- not a stale pg_class heuristic.  This gate is what makes
 *      Sum(df) equal the VISIBLE count rather than the index-time count: with no
 *      tombstones (2) the index believes every posting doc is live, and an all-
 *      visible heap confirms every one of those docs is in fact visible now and
 *      none was deleted-but-not-yet-vacuumed (a dead tuple would leave its page
 *      NOT all-visible) nor inserted-after-build outside pending (also not all-
 *      visible).  Hence each of the Sum(df) postings is one visible document,
 *      one-to-one, and the count is exact.
 *
 * Any concurrent change to tombstones/pending/segments between our metapage
 * read and returning would invalidate the arithmetic; we re-check the directory
 * generation at the end (as the full path does) and bail to it on any change.
 *
 * Note: cost is one O(nblocks) VM scan + one dict lookup per segment, replacing
 * a decode of every posting of a common term.  Falls back on any doubt -- a
 * wrong count is worse than a slow one.
 */
static int64
weave_count_dictdf_fastpath(Relation index, WeaveQuery q)
{
	WeaveMetaPageData meta;
	WeaveQueryItem *it;
	const char *term;
	int			termlen;
	uint32		gen0;
	uint64		sumdf = 0;
	uint32		s;
	Relation	heap;
	BlockNumber nblocks;
	bool		all_visible = true;

	/* Gate (1): exactly one plain positive term. */
	if (q == NULL || q->nitems != 1)
		return -1;
	it = &q->items[0];
	if (it->type != WEAVE_QI_VAL ||
		(it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX | WEAVE_QF_WEIGHTED)) != 0)
		return -1;				/* weighted (term:LABEL) needs the heap recheck, not Sum(df) */

	weave_read_meta(index, &meta);
	gen0 = meta.generation;

	/* Gate (3): no unmerged pending docs. */
	if (meta.npending != 0)
		return -1;

	/* Gate (2): no tombstones in any segment. */
	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
		if (meta.segs[s].ndeleted != 0 || meta.segs[s].livedocslen != 0)
			return -1;

	/* Sum the term's df across the segments where it is present. */
	term = WEAVE_QUERY_ITEMTEXT(q, it);
	termlen = (int) it->termlen;
	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveSegMeta *sg = &meta.segs[s];
		uint32		df,
					max_tf,
					foff;
		BlockNumber fpost;

		if (sg->dictstart == InvalidBlockNumber)
			continue;
		if (weave_lookup_dict(index, sg, term, termlen,
							 &df, &max_tf, &fpost, &foff))
			sumdf += df;
	}

	/*
	 * A ZERO df NEEDS NO VISIBILITY PROOF.  Gate (3) has already established
	 * npending == 0, so no document exists outside the segments; if no segment's
	 * dictionary holds the term, nothing matches and the answer is 0 whatever the
	 * visibility map says.  Returning here skips gate (4) entirely.
	 *
	 * This was measured, not assumed: before it, `count(*)` for a term that does
	 * not exist in the corpus cost 0.280 ms on an 87,486-page heap -- the same as
	 * a term matching 196,785 documents -- because the whole-heap VM walk below
	 * ran anyway (doc/GAPS.md G38).
	 */
	if (sumdf == 0)
		return 0;

	/*
	 * Gate (4): the whole heap must be all-visible to this snapshot.
	 *
	 * visibilitymap_count() reads the VM PAGES and counts set bits, which is
	 * O(heap_pages / 32672) buffer reads and a popcount per word.  The first
	 * version of this gate called VM_ALL_VISIBLE() once per HEAP BLOCK, and that
	 * made the whole fast path O(heap pages) with a large constant: measured at
	 * 0.278 ms on an 87,486-page heap, ~3.2 ns per block, all of it CPU -- the
	 * plan reported `shared hit=8`, so it was never I/O, just 87,486 calls.
	 *
	 * That cost is INDEPENDENT OF SELECTIVITY, which is what made it a defect
	 * rather than a trade: `count(*)` was flat at 0.278-0.291 ms from df 0 to df
	 * 196,785, while the ordinary (non-fast) path answered df 25 in 0.072 ms.  So
	 * below roughly df 6,500 on that heap the "fast" path was a 3.9x
	 * PESSIMIZATION, and the crossover moved with heap size rather than with
	 * anything the user could see.  doc/GAPS.md G38 has the measurements.
	 *
	 * THE ONE HAZARD THIS SWAP INTRODUCES, STATED RATHER THAN ARGUED AWAY.
	 * visibilitymap_count() counts set bits over the WHOLE map, including any bit
	 * belonging to a block past the current end of the relation.  A count that
	 * merely EQUALS nblocks could therefore, in principle, be made up of
	 * (nblocks - k) real all-visible pages plus k stale bits past EOF -- and the
	 * cost of believing it would be a WRONG COUNT, not a slow one.  The per-block
	 * loop it replaces could not be fooled that way.
	 *
	 * Why it is not reachable: visibilitymap_truncate() runs inside
	 * RelationTruncate()'s critical section and the XLOG_SMGR_TRUNCATE record
	 * covers heap, VM and FSM together, so a crash cannot leave the VM longer than
	 * the heap; and TRUNCATE TABLE allocates a new relfilenode, which has no VM at
	 * all.  But "not reachable through any supported path" is the kind of reasoning
	 * doc/CONVENTIONS.md rule 2 exists to distrust, so it is CHECKED rather than
	 * trusted: under USE_ASSERT_CHECKING the authoritative per-block scan runs and
	 * must agree.  A cassert build therefore re-derives this gate over the whole
	 * regression suite, which is the same arrangement already used for the
	 * score()-returns-distance convention.  Production pays O(VM pages).
	 */
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(heap);
	{
		BlockNumber nallvisible;
		BlockNumber nallfrozen;

		visibilitymap_count(heap, &nallvisible, &nallfrozen);
		all_visible = (nblocks > 0 && nallvisible == nblocks);

#ifdef USE_ASSERT_CHECKING
		{
			Buffer		vmbuf = InvalidBuffer;
			bool		exact = (nblocks > 0);
			BlockNumber blk;

			for (blk = 0; blk < nblocks; blk++)
			{
				if (!VM_ALL_VISIBLE(heap, blk, &vmbuf))
				{
					exact = false;
					break;
				}
			}
			if (vmbuf != InvalidBuffer)
				ReleaseBuffer(vmbuf);
			Assert(all_visible == exact);
		}
#endif
	}
	table_close(heap, AccessShareLock);
	if (!all_visible)
		return -1;

	/*
	 * Concurrency: if the segment directory changed while we read it (a merge/
	 * vacuum could have added tombstones or folded pending docs), our gates and
	 * df sum may be stale.  Bail to the full path, which restarts on generation
	 * change itself.
	 */
	if (weave_read_meta_generation(index) != gen0)
		return -1;

	return (int64) sumdf;
}

/*
 * weave_count_visible: MVCC-correct count of documents matching `q`, computed in
 * bulk from the index without the per-tuple executor round-trips of an
 * index(-only) scan.  Collect the matching TIDs (sorted), then count those
 * visible to the snapshot: on an all-visible heap page (visibility map) the
 * whole run of TIDs counts without touching the heap; only pages the VM does
 * not mark all-visible are probed with table_index_fetch_tuple.  This is the
 * count-pushdown path the CustomScan uses to answer count(*) at index speed.
 * If `recheck` is set (fuzzy/regex/NOT/PHRASE over-generation), the collected
 * TIDs are a SUPERSET of the exact matches, so weave_recheck_exact() shrinks
 * them to the precise set (recomputing the heap wdoc and re-running @@@)
 * before the visibility count.  Without recheck the collected TIDs are already
 * exact and we only need visibility.
 */
static int64
weave_count_visible(Relation index, WeaveQuery q)
{
	TidSet		matches;
	bool		recheck;
	Snapshot	snap = GetActiveSnapshot();
	Relation	heap;
	IndexFetchTableData *fetch = NULL;
	Buffer		vmbuf = InvalidBuffer;
	int64		count = 0;
	int			i;

	/*
	 * The count pushdown (CustomScan), weave_count() and weave_count_visible_oid()
	 * all funnel through here and bypass the executor's index-scan machinery, so
	 * account for the scan explicitly: one index scan, and idx_tup_read = the
	 * matching index entries (like a bitmap index scan reports its bitmap size).
	 */
	pgstat_count_index_scan(index);

	/*
	 * Fast path: a single plain term over a fully-visible, tombstone-free,
	 * pending-free index is counted from the dictionary df alone -- no posting
	 * decode, no heap probe.  Returns -1 (fall through) on any doubt.
	 */
	{
		int64		fast = weave_count_dictdf_fastpath(index, q);

		if (fast >= 0)
		{
			pgstat_count_index_tuples(index, fast);
			return fast;
		}
	}

	weave_collect_matches(index, q, &matches, &recheck);
	/*
	 * Shrink over-generated sets (fuzzy/regex/PHRASE/NEAR) to the exact @@@
	 * match set against the heap wdoc; after this the count is precise.
	 */
	if (recheck)
		weave_recheck_exact(index, q, &matches);
	pgstat_count_index_tuples(index, matches.n);
	if (matches.n == 0)
		return 0;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	/*
	 * The collected TIDs are now exactly the matches (over-generation already
	 * rechecked above), so we only need visibility.  Count visible via the VM,
	 * heap-probing only pages the map does not mark all-visible.
	 */
	for (i = 0; i < matches.n; i++)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&matches.tids[i]);

		if (VM_ALL_VISIBLE(heap, blk, &vmbuf))
		{
			/* whole page visible: this TID counts, no heap access */
			count++;
			continue;
		}
		/* page not all-visible: probe the heap for this TID's visibility */
		if (fetch == NULL)
		{
#if PG_VERSION_NUM >= 190000
			fetch = table_index_fetch_begin(heap, SO_NONE);
#else
			fetch = table_index_fetch_begin(heap);
#endif
		}
		{
			ItemPointerData tid = matches.tids[i];
			bool		ca = false,
						ad = false;
			TupleTableSlot *slot = table_slot_create(heap, NULL);

			if (table_index_fetch_tuple(fetch, &tid, snap, slot, &ca, &ad))
				count++;
			ExecDropSingleTupleTableSlot(slot);
		}
	}
	if (fetch != NULL)
		table_index_fetch_end(fetch);
	if (vmbuf != InvalidBuffer)
		ReleaseBuffer(vmbuf);
	table_close(heap, AccessShareLock);
	return count;
}

/*
 * weave_count_visible_oid: same as weave_count() but callable from C with an index
 * OID (used by the COUNT-pushdown CustomScan).  Opens the index under
 * AccessShareLock, counts, closes.
 */
int64
weave_count_visible_oid(Oid indexoid, WeaveQuery q)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	int64		c = weave_count_visible(index, q);

	index_close(index, AccessShareLock);
	return c;
}

PG_FUNCTION_INFO_V1(weave_count);

/* weave_count(regclass, wquery) -> bigint : MVCC-correct count via the index */
Datum
weave_count(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	WeaveQuery	q = PG_GETARG_WQUERY(1);
	Relation	index;
	int64		c;

	index = index_open(indexoid, AccessShareLock);
	c = weave_count_visible(index, q);
	index_close(index, AccessShareLock);
	PG_RETURN_INT64(c);
}

PG_FUNCTION_INFO_V1(weave_search);

Datum
weave_search(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ScoredTid  *results;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			indexoid = PG_GETARG_OID(0);
		WeaveQuery	q = PG_GETARG_WQUERY(1);
		int			k = PG_GETARG_INT32(2);
		MemoryContext oldctx;
		Relation	index;
		int			nvis;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		index = index_open(indexoid, AccessShareLock);
		/* This native top-k bypasses the executor's index-scan machinery; count
		 * the scan and the returned index entries ourselves. */
		pgstat_count_index_scan(index);
		nvis = weave_topk_visible(index, q, k, false, &results);
		pgstat_count_index_tuples(index, nvis);
		index_close(index, AccessShareLock);

		funcctx->max_calls = nvis;
		funcctx->user_fctx = results;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	results = (ScoredTid *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};
		HeapTuple	tuple;
		ItemPointer tidcopy = palloc(sizeof(ItemPointerData));

		*tidcopy = results[funcctx->call_cntr].tid;
		values[0] = PointerGetDatum(tidcopy);
		values[1] = Float8GetDatum(results[funcctx->call_cntr].score);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ----- lexical anomaly detection: rare-term (low-df) dictionary tail ----- */

/* term-df hash key width; matches the build side's WEAVE_TERMKEYLEN ceiling */
#define WEAVE_ANOM_TERMKEYLEN 64

/*
 * weave_anomalous_docs(index regclass, k int, max_df int)
 *   -> setof (ctid tid, score float8, rarest_term text, min_df int)
 *
 * Surface the top-k most lexically-anomalous documents: those containing
 * globally rare terms.  A document's anomaly score is the MAX idf over its
 * terms -- i.e. it is driven by the document's single rarest term.  Because the
 * rarest terms have the SHORTEST posting lists, this is answered by walking the
 * LOW-df tail of the dictionary and emitting those few documents; the vast bulk
 * of the dictionary (common, high-df terms) is skipped before any posting is
 * decoded, so it is cheap and NOT a full-corpus scan.
 *
 * idf = log(1 + (N - df + 0.5)/(df + 0.5)), the same rarity value BM25 uses.
 * `df` is the GLOBAL document frequency: a term is summed across all segments
 * (segments share one corpus) before its idf is computed, so a doc split across
 * two segments is not made to look artificially rare.
 *
 * `max_df` caps which terms count as "rare": only terms with global df <=
 * max_df contribute.  When NULL (-1 from the strict SQL wrapper's default) it
 * defaults to max(N/1000, 1) -- a small fraction of the corpus, keeping the
 * walk on the low-df tail.
 *
 * MVCC: the returned ctids are index-resident heap pointers (like weave_search);
 * this is an analytic/heuristic result, not a query result, so no per-doc heap
 * visibility check is done -- the caller should join ctid back to the table and
 * filter for visibility if needed.  Per-segment tombstones ARE honored so
 * deleted docs are not reported as anomalies.
 */

/* term -> summed-across-segments df; key is the NUL-terminated term text.
 * Note: a term longer than WEAVE_ANOM_TERMKEYLEN-1 is truncated for the df
 * key (same ceiling the build side takes at 64); two such terms sharing that
 * prefix would share a df bucket -> slightly conservative rarity.  Widen the
 * key or chain (like add_posting) if long distinct tokens must rank exactly. */
typedef struct AnomTermDf
{
	char		term[WEAVE_ANOM_TERMKEYLEN]; /* dynahash string key */
	uint64		gdf;			/* global df, summed across segments (uint64 so a
								 * term in >2^32 docs does not wrap) */
} AnomTermDf;

/* per-document running max: its best (rarest) term's idf and that term's df */
typedef struct AnomDoc
{
	uint64		docid;			/* dynahash blob key */
	double		score;			/* best idf seen for this doc */
	uint32		min_df;			/* the driving term's global df */
	char	   *rarest_term;	/* palloc'd copy of the driving term */
	int			rarest_len;
}			AnomDoc;

typedef struct AnomResult
{
	ItemPointerData tid;
	double		score;
	char	   *rarest_term;
	int			rarest_len;
	int32		min_df;
}			AnomResult;

static int
cmp_anom_desc(const void *a, const void *b)
{
	double		sa = ((const AnomResult *) a)->score;
	double		sb = ((const AnomResult *) b)->score;

	if (sa < sb)
		return 1;
	if (sa > sb)
		return -1;
	return 0;
}

PG_FUNCTION_INFO_V1(weave_anomalous_docs);

Datum
weave_anomalous_docs(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AnomResult *results;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			indexoid;
		int			k = PG_ARGISNULL(1) ? 100 : PG_GETARG_INT32(1);
		/* max_df NULL -> auto (small fraction of N); the func is non-STRICT so a
		 * NULL default reaches C rather than short-circuiting to no rows */
		bool		max_df_null = PG_ARGISNULL(2);
		int			max_df_arg = max_df_null ? -1 : PG_GETARG_INT32(2);
		MemoryContext oldctx;
		Relation	index;
		WeaveMetaPageData meta;
		WeaveTombstones tombs;
		TupleDesc	tupdesc;
		HTAB	   *dfht;		/* term -> global df */
		HTAB	   *docht;		/* docid -> AnomDoc */
		HASHCTL		ctl;
		double		N;
		int			max_df;
		uint32		s;
		HASH_SEQ_STATUS seq;
		AnomDoc    *dslot;
		int			ndocs_found;
		int			nout;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		if (PG_ARGISNULL(0))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("index argument must not be null")));
		indexoid = PG_GETARG_OID(0);

		if (k <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("k must be positive")));

		index = index_open(indexoid, AccessShareLock);
		if (index->rd_rel->relam != get_index_am_oid("weave", true))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a weave index",
							RelationGetRelationName(index))));
		weave_read_meta(index, &meta);
		N = meta.ndocs;

		/* effective max_df: arg, else a small fraction of N (floor 1) */
		if (!max_df_null && max_df_arg >= 0)
			max_df = max_df_arg;
		else
			max_df = Max((int) (N / 1000.0), 1);

		/* empty index (or nothing to rank): return no rows */
		if (N <= 0 || meta.nsegments == 0)
		{
			index_close(index, AccessShareLock);
			funcctx->max_calls = 0;
			MemoryContextSwitchTo(oldctx);
			funcctx = SRF_PERCALL_SETUP();
			SRF_RETURN_DONE(funcctx);
		}

		weave_tombstones_load(index, &meta, &tombs);

		/*
		 * Pass 1 -- walk every segment's DICTIONARY (no postings decoded) and
		 * sum df per distinct term text into dfht.  This is cheap: dict pages
		 * only, one entry per distinct term.  It gives the true global df so a
		 * term appearing in several segments is not mistaken for rarer than it
		 * is.
		 */
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = WEAVE_ANOM_TERMKEYLEN;
		ctl.entrysize = sizeof(AnomTermDf);
		ctl.hcxt = CurrentMemoryContext;
		dfht = hash_create("weave anomaly term df", 4096, &ctl,
						   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

		for (s = 0; s < meta.nsegments; s++)
		{
			BlockNumber blk = meta.segs[s].dictstart;

			while (blk != InvalidBlockNumber)
			{
				Buffer		buffer;
				Page		page;
				char	   *ptr,
						   *end;
				BlockNumber next;

				CHECK_FOR_INTERRUPTS();	/* between dict pages, no buffer lock held: safe to unwind */
				buffer = weave_scan_readbuf(index, blk);
				if (buffer == InvalidBuffer)
					break;	/* block truncated by a concurrent weave_vacuum: end of chain */
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
				page = BufferGetPage(buffer);
				ptr = (char *) PageGetContents(page);
				end = weave_page_entry_end(page);
				next = WeavePageGetOpaque(page)->nextblk;

				while (ptr < end)
				{
					WeaveDictEntry *de = (WeaveDictEntry *) ptr;
					Size		esize;
					char		key[WEAVE_ANOM_TERMKEYLEN];
					int			klen;
					AnomTermDf *te;
					bool		found;

					if (!weave_dict_entry_fits(de, end))
						break;	/* recycled/corrupt page: stop (see the helper) */
					esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
					klen = Min((int) de->termlen, WEAVE_ANOM_TERMKEYLEN - 1);
					memcpy(key, de->term, klen);
					key[klen] = '\0';
					te = (AnomTermDf *) hash_search(dfht, key, HASH_ENTER,
													&found);
					if (!found)
						te->gdf = 0;
					te->gdf += de->df;
					ptr += esize;
				}
				UnlockReleaseBuffer(buffer);
				blk = next;
			}
		}

		/*
		 * Pass 2 -- walk the dicts again; for terms whose GLOBAL df <= max_df
		 * (the rare tail only), decode their (few) postings and keep, per
		 * document, the maximum idf seen (the doc's rarest term).  Common terms
		 * are skipped BEFORE any posting decode -- this is the cheap-tail
		 * filter that keeps the whole thing off a full-corpus scan.
		 */
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(AnomDoc);
		ctl.hcxt = CurrentMemoryContext;
		docht = hash_create("weave anomaly docs", 4096, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		for (s = 0; s < meta.nsegments; s++)
		{
			BlockNumber blk = meta.segs[s].dictstart;

			while (blk != InvalidBlockNumber)
			{
				Buffer		buffer;
				Page		page;
				char	   *ptr,
						   *end;
				BlockNumber next;

				CHECK_FOR_INTERRUPTS();	/* between dict pages, no buffer lock held: safe to unwind */
				buffer = weave_scan_readbuf(index, blk);
				if (buffer == InvalidBuffer)
					break;	/* block truncated by a concurrent weave_vacuum: end of chain */
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
				page = BufferGetPage(buffer);
				ptr = (char *) PageGetContents(page);
				end = weave_page_entry_end(page);
				next = WeavePageGetOpaque(page)->nextblk;

				while (ptr < end)
				{
					WeaveDictEntry *de = (WeaveDictEntry *) ptr;
					Size		esize;
					char		key[WEAVE_ANOM_TERMKEYLEN];
					int			klen;
					AnomTermDf *te;
					uint64		gdf;
					double		idf;
					WeavePosting *post;
					int			np,
								j;

					if (!weave_dict_entry_fits(de, end))
						break;	/* recycled/corrupt page: stop (see the helper) */
					esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
					klen = Min((int) de->termlen, WEAVE_ANOM_TERMKEYLEN - 1);
					memcpy(key, de->term, klen);
					key[klen] = '\0';
					te = (AnomTermDf *) hash_search(dfht, key, HASH_FIND, NULL);
					gdf = te ? te->gdf : de->df;

					/* THE cheap-tail filter: skip common terms before decode.
					 * Compare as unsigned against the (non-negative) max_df so a
					 * huge gdf cannot cast to a negative int and slip past the
					 * filter (max_df is >= 1 here). */
					if (gdf == 0 || gdf > (uint64) max_df)
					{
						ptr += esize;
						continue;
					}

					idf = log(1.0 + (N - (double) gdf + 0.5) /
							  ((double) gdf + 0.5));

					np = weave_decode_term(index, de->firstposting,
										  de->firstoffset, de->df,
										  &post, NULL, false, NULL, true,
										  meta.segs[s].doclenstart == InvalidBlockNumber);
					for (j = 0; j < np; j++)
					{
						uint64		docid = weave_tid_to_docid(&post[j].tid);
						AnomDoc    *doc;
						bool		found;

						/* skip docs tombstoned in THIS segment */
						if (tombs.hasany && tombs.present[s] &&
							sm_contains(&tombs.maps[s], docid, NULL))
							continue;

						doc = (AnomDoc *) hash_search(docht, &docid, HASH_ENTER,
													  &found);
						if (!found || idf > doc->score)
						{
							if (!found)
								doc->docid = docid;
							doc->score = idf;
							doc->min_df = gdf;
							doc->rarest_term = (char *) palloc(de->termlen + 1);
							memcpy(doc->rarest_term, de->term, de->termlen);
							doc->rarest_term[de->termlen] = '\0';
							doc->rarest_len = de->termlen;
						}
					}
					pfree(post);
					ptr += esize;
				}
				UnlockReleaseBuffer(buffer);
				blk = next;
			}
		}

		weave_tombstones_free(&tombs);
		index_close(index, AccessShareLock);

		/* collect the rare-tail doc set and take the top-k by score */
		ndocs_found = (int) hash_get_num_entries(docht);
		results = (AnomResult *) palloc(Max(ndocs_found, 1) *	/* alloc-ok: rare-tail result set (low-df anomaly scan) */
										sizeof(AnomResult));
		nout = 0;
		hash_seq_init(&seq, docht);
		while ((dslot = (AnomDoc *) hash_seq_search(&seq)) != NULL)
		{
			weave_docid_to_tid(dslot->docid, &results[nout].tid);
			results[nout].score = dslot->score;
			results[nout].min_df = (int32) dslot->min_df;
			results[nout].rarest_term = dslot->rarest_term;
			results[nout].rarest_len = dslot->rarest_len;
			nout++;
		}

		qsort(results, nout, sizeof(AnomResult), cmp_anom_desc);
		if (nout > k)
			nout = k;

		funcctx->max_calls = nout;
		funcctx->user_fctx = results;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	results = (AnomResult *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		AnomResult *r = &results[funcctx->call_cntr];
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};
		HeapTuple	tuple;
		ItemPointer tidcopy = palloc(sizeof(ItemPointerData));

		*tidcopy = r->tid;
		values[0] = PointerGetDatum(tidcopy);
		values[1] = Float8GetDatum(r->score);
		values[2] = PointerGetDatum(cstring_to_text_with_len(r->rarest_term,
															 r->rarest_len));
		values[3] = Int32GetDatum(r->min_df);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

