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

/* palloc/repalloc that transparently use the Huge variants past MaxAllocSize.
 * Build-time posting arrays for a very high-df term (e.g. tokens present in
 * millions of JSON-log lines) can exceed 1 GB, especially when the final merge
 * concatenates a term's postings across several segments before dedup.
 *
 * Note: this only lifts the *byte-size* ceiling (>1 GB). The element
 * counts (nposts/npos, int) still cap at INT_MAX (~2.1 G postings/positions
 * per term); a single term that common would overflow the int counters (and
 * their doubling) first. Not hit even at 20M diverse docs; widen these counts
 * to int64 if a term ever approaches that df. */
#define WEAVE_ALLOC_MAYBE_HUGE(sz) \
	(((Size) (sz)) > MaxAllocSize \
	 ? MemoryContextAllocHuge(CurrentMemoryContext, (sz)) \
	 : palloc((sz)))
#define WEAVE_REALLOC_MAYBE_HUGE(p, sz) \
	(((Size) (sz)) > MaxAllocSize \
	 ? repalloc_huge((p), (sz)) \
	 : repalloc((p), (sz)))

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
static bool weave_index_wants_positions(Relation index);
static bool weave_index_wants_trigrams(Relation index);
static bool weave_index_wants_doclen_sidecar(Relation index);

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

/* ----- build: collect postings from the heap ----- */

typedef struct BuildTerm
{
	char	   *term;
	int			len;
	/* postings for this term */
	ItemPointerData *tids;
	uint32	   *tfs;
	uint32	   *doclens;
	/* token positions for this term, when the index carries positions.  Flat
	 * arena of all postings' positions; posting i owns positions[posoff[i] ..
	 * posoff[i]+tfs[i]).  Never reordered (postings are sorted by copying these
	 * offsets into the sort struct), so posoff stays valid across the sort. */
	uint32	   *positions;
	uint32	   *posoff;		/* per-posting start index into positions[] */
	uint32	   *poscnt;		/* per-posting stored position count (0 if dropped;
								 * may be < tf when a source block dropped positions) */
	int			npos;		/* total positions stored (== Sum poscnt) */
	int			maxpos;
	int			nposts;
	int			maxposts;
	uint32		max_tf;		/* max tf across postings; set by weave_write_postings
								 * so weave_write_dictionary reads it instead of
								 * rescanning tfs[] -- lets a streaming merge keep
								 * only term metadata (no tfs[] body) and still write
								 * a correct max_tf */
	int			next;			/* next BuildTerm sharing the same hash key, or -1 */
} BuildTerm;

typedef struct WeaveBuildState
{
	MemoryContext ctx;
	BuildTerm  *terms;			/* sorted-on-flush; kept in a simple array */
	int			nterms;
	int			maxterms;
	bool		want_positions;	/* index built WITH (positions=on): carry token
								 * positions through build/merge into the postings */
	bool		want_trigrams;	/* index built WITH (trigrams=on): write the
								 * per-segment trigram index (regex/long-fuzzy accel) */
	bool		want_sidecar;	/* index built WITH (doclen_sidecar=on, the default):
								 * write doclen to the per-segment quantized sidecar and
								 * omit the inline posting column.  false = inline doclen. */
	/* build-time term list: an unsorted array collected during the heap scan,
	 * sorted once before the dictionary is written */
	double		ndocs;
	double		sumdoclen;
	Size		flush_budget;	/* current in-memory budget before a segment is
								 * flushed; grows as this participant flushes more,
								 * so the flush count stays far under
								 * WEAVE_MAX_SEGMENTS (0 = use the default) */
	int			nflushes;		/* segments this participant has flushed so far */
} WeaveBuildState;

static int
cmp_buildterm(const void *a, const void *b)
{
	const BuildTerm *ta = (const BuildTerm *) a;
	const BuildTerm *tb = (const BuildTerm *) b;
	int			min = Min(ta->len, tb->len);
	int			c = memcmp(ta->term, tb->term, min);

	if (c != 0)
		return c;
	return ta->len - tb->len;
}

/*
 * Find or create a BuildTerm for (term,len).  We use a dynahash keyed by the
 * term's length plus a bounded copy of its bytes to avoid an O(n^2) linear
 * scan.  Terms longer than the key buffer fall back to exact comparison via
 * the stored BuildTerm, which is correct though it may hash-collide slightly;
 * term length is bounded by MAXSTRLEN in practice.
 *
 * L15: the key is NOT a HASH_BLOBS blob.  A build does one lookup per term
 * occurrence (~240M on a 2M x 120-word corpus) and a typical term is ~10
 * bytes, so hashing and memcmp-ing a fixed 64-byte blob cost ~6x more than
 * the data requires and showed up as 37.5% of build time
 * (bench/RESULTS_BUILD_PROFILE.md).  The hash, compare and copy callbacks
 * below touch only `len` bytes.
 */
#include "utils/hsearch.h"
#include "common/hashfn.h"

#define WEAVE_TERMKEYLEN 64

typedef struct TermKey
{
	int32		len;			/* true term length (clamped >= 0) */
	char		key[WEAVE_TERMKEYLEN];	/* first Min(len, WEAVE_TERMKEYLEN) bytes */
} TermKey;

static inline int
termkey_nbytes(const TermKey *k)
{
	return Min(k->len, WEAVE_TERMKEYLEN);
}

static uint32
termkey_hash(const void *key, Size keysize)
{
	const TermKey *k = (const TermKey *) key;

	return hash_bytes((const unsigned char *) k->key, termkey_nbytes(k))
		^ (uint32) k->len;
}

static int
termkey_match(const void *key1, const void *key2, Size keysize)
{
	const TermKey *a = (const TermKey *) key1;
	const TermKey *b = (const TermKey *) key2;

	if (a->len != b->len)
		return 1;
	return memcmp(a->key, b->key, termkey_nbytes(a));
}

static void *
termkey_copy(void *dest, const void *src, Size keysize)
{
	const TermKey *s = (const TermKey *) src;
	TermKey    *d = (TermKey *) dest;

	d->len = s->len;
	memcpy(d->key, s->key, termkey_nbytes(s));
	return dest;
}

typedef struct TermHashEntry
{
	TermKey		key;			/* must be first: dynahash key */
	int			termidx;		/* head of a chain of BuildTerms sharing this key */
} TermHashEntry;

static HTAB *build_ht;

/*
 * (Re)initialize the build hash table that maps a term key to its BuildTerm
 * index.  Created in bs->ctx so it is freed when that context is reset between
 * segment flushes during a large build.
 */
static void
weave_build_ht_init(WeaveBuildState *bs)
{
	HASHCTL		ctl;

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(TermKey);
	ctl.entrysize = sizeof(TermHashEntry);
	ctl.hash = termkey_hash;
	ctl.match = termkey_match;
	ctl.keycopy = termkey_copy;
	ctl.hcxt = bs->ctx;
	build_ht = hash_create("weave build terms", 1024, &ctl,
						   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
						   HASH_KEYCOPY | HASH_CONTEXT);
}

static void
make_termkey(TermKey *k, const char *term, int len)
{
	int			n = len;

	/* defensive: `len` ultimately derives from an on-disk/pending entries[].len
	 * (a uint32 read into an int).  A corrupt value could be negative or absurd;
	 * clamp to [0, WEAVE_TERMKEYLEN] so this fixed-size key copy can never turn
	 * into a wild memcpy (a _FORTIFY_SOURCE abort).  Callers validate the doc
	 * first (weave_doc_is_valid); this is the last line of defense. */
	if (n < 0)
		n = 0;
	k->len = n;
	memcpy(k->key, term, Min(n, WEAVE_TERMKEYLEN));
}

/*
 * Append a new BuildTerm for (term,len) to bs->terms, chained after `next`,
 * and return it.  No hash involvement: callers that already know the term is
 * new (the merge, which processes one term at a time) use this directly.
 */
static BuildTerm *
build_term_new(WeaveBuildState *bs, const char *term, int len, int next)
{
	BuildTerm  *bt;

	if (bs->nterms >= bs->maxterms)
	{
		bs->maxterms = bs->maxterms ? bs->maxterms * 2 : 1024;
		if (bs->terms == NULL)
			bs->terms = (BuildTerm *) WEAVE_ALLOC_MAYBE_HUGE(bs->maxterms * sizeof(BuildTerm));
		else
			bs->terms = (BuildTerm *) WEAVE_REALLOC_MAYBE_HUGE(bs->terms,
													   bs->maxterms * sizeof(BuildTerm));
	}
	bt = &bs->terms[bs->nterms];
	bt->term = (char *) palloc(len);
	memcpy(bt->term, term, len);
	bt->len = len;
	bt->maxposts = 4;
	bt->nposts = 0;
	bt->max_tf = 0;			/* filled by weave_write_postings */
	bt->tids = (ItemPointerData *) WEAVE_ALLOC_MAYBE_HUGE(bt->maxposts * sizeof(ItemPointerData));
	bt->tfs = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE(bt->maxposts * sizeof(uint32));
	bt->doclens = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE(bt->maxposts * sizeof(uint32));
	bt->positions = NULL;
	bt->posoff = NULL;
	bt->poscnt = NULL;
	bt->npos = 0;
	bt->maxpos = 0;
	if (bs->want_positions)
	{
		bt->posoff = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE(bt->maxposts * sizeof(uint32));
		bt->poscnt = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE(bt->maxposts * sizeof(uint32));
		bt->maxpos = 8;
		bt->positions = (uint32 *) palloc(bt->maxpos * sizeof(uint32));	/* alloc-ok: seed=8, grown huge-safe below; one term in a budget-bounded segment */
	}
	bt->next = next;
	bs->nterms++;
	return bt;
}

/*
 * Append one posting to an already-located BuildTerm.  Hash-free: the merge
 * calls this once per posting of the term it is currently gathering.
 */
static void
build_term_append(WeaveBuildState *bs, BuildTerm *bt,
				  ItemPointer tid, uint32 tf, uint32 doclen,
				  const uint32 *pos, int npos)
{
	if (bt->nposts >= bt->maxposts)
	{
		bt->maxposts *= 2;
		bt->tids = (ItemPointerData *) WEAVE_REALLOC_MAYBE_HUGE(bt->tids,
												bt->maxposts * sizeof(ItemPointerData));
		bt->tfs = (uint32 *) WEAVE_REALLOC_MAYBE_HUGE(bt->tfs, bt->maxposts * sizeof(uint32));
		bt->doclens = (uint32 *) WEAVE_REALLOC_MAYBE_HUGE(bt->doclens, bt->maxposts * sizeof(uint32));
		if (bt->posoff != NULL)
		{
			bt->posoff = (uint32 *) WEAVE_REALLOC_MAYBE_HUGE(bt->posoff, bt->maxposts * sizeof(uint32));
			bt->poscnt = (uint32 *) WEAVE_REALLOC_MAYBE_HUGE(bt->poscnt, bt->maxposts * sizeof(uint32));
		}
	}
	bt->tids[bt->nposts] = *tid;
	bt->tfs[bt->nposts] = tf;
	bt->doclens[bt->nposts] = doclen;
	/*
	 * Carry positions when the index wants them and the caller supplied a full
	 * set (npos == tf).  A per-(term,doc) position count is bounded by the
	 * analyzer's MAXENTRYPOS cap, so appending tf values here cannot blow up a
	 * single posting; the segment total is bounded by the build memory budget
	 * (checked between tuples in weave_build_callback), which flushes before the
	 * arena grows unbounded -- so this never materializes the whole corpus'
	 * positions in one array.
	 */
	if (bs->want_positions && pos != NULL && npos == (int) tf && tf > 0)
	{
		if (bt->npos + (int) tf > bt->maxpos)
		{
			while (bt->npos + (int) tf > bt->maxpos)
				bt->maxpos *= 2;
			bt->positions = (uint32 *) WEAVE_REALLOC_MAYBE_HUGE(bt->positions,
												bt->maxpos * sizeof(uint32));
		}
		bt->posoff[bt->nposts] = (uint32) bt->npos;
		bt->poscnt[bt->nposts] = tf;
		memcpy(bt->positions + bt->npos, pos, tf * sizeof(uint32));
		bt->npos += (int) tf;
	}
	else if (bt->posoff != NULL)
	{
		/* want positions but this posting has none (tf==0, or a source block
		 * dropped them on a prior write): record an empty run + zero count so
		 * posoff stays aligned with the posting index and the writer knows this
		 * posting stores no positions (it will drop the block's positions). */
		bt->posoff[bt->nposts] = (uint32) bt->npos;
		bt->poscnt[bt->nposts] = 0;
	}
	bt->nposts++;
}

/*
 * Find or create the BuildTerm for (term,len) through the build hash, then
 * append one posting.  This is the per-occurrence path of the heap scan.
 */
static void
add_posting(WeaveBuildState *bs, const char *term, int len,
			ItemPointer tid, uint32 tf, uint32 doclen,
			const uint32 *pos, int npos)
{
	TermKey		key;
	TermHashEntry *entry;
	bool		found;
	BuildTerm  *bt = NULL;
	int			idx;

	make_termkey(&key, term, len);
	entry = (TermHashEntry *) hash_search(build_ht, &key, HASH_ENTER, &found);

	/*
	 * On a hash hit, walk the chain of BuildTerms sharing this key and pick the
	 * truly-equal one.  The key is (len, bounded prefix), so two DISTINCT
	 * terms > WEAVE_TERMKEYLEN bytes sharing length and prefix can land on the same
	 * key; chaining keeps them as separate BuildTerms instead of clobbering the
	 * entry (which previously fragmented a term's postings across unreachable
	 * dictionary entries).
	 */
	if (found)
	{
		for (idx = entry->termidx; idx >= 0; idx = bs->terms[idx].next)
		{
			BuildTerm  *cand = &bs->terms[idx];

			if (cand->len == len && memcmp(cand->term, term, len) == 0)
			{
				bt = cand;
				break;
			}
		}
	}

	if (bt == NULL)
	{
		/* push onto the head of this key's chain (-1 = end of chain) */
		bt = build_term_new(bs, term, len, found ? entry->termidx : -1);
		entry->termidx = bs->nterms - 1;
	}

	build_term_append(bs, bt, tid, tf, doclen, pos, npos);
}

/* forward decls: segment writers are defined later; the build flush uses them */
static void weave_write_segment(Relation index, WeaveBuildState *bs, WeaveSegMeta *seg);
static void weave_meta_from_page(Page page, WeaveMetaPageData *out);
static void weave_meta_upcast_page(Page page);
static bool weave_meta_add_segment(Relation index, const WeaveSegMeta *seg);
static void weave_add_segment_with_room(Relation index, const WeaveSegMeta *seg);
static void weave_free_page(Relation index, BlockNumber blk);
static bool weave_page_recyclable(Relation index, Page page);

/*
 * Memory budget for the in-memory build state before it is flushed to a
 * segment.  A very large CREATE INDEX would otherwise accumulate the whole
 * corpus's terms + postings in bs->ctx and exhaust memory; instead, once the
 * build context grows past this, we write the accumulated terms as a segment
 * and start fresh.  Derived from maintenance_work_mem (bounded so a small
 * setting still makes progress).  The later size-tiered merge compacts the
 * resulting segments.
 */
static Size
weave_build_mem_budget(void)
{
	Size		budget = (Size) maintenance_work_mem * (Size) 1024;

	if (budget < (Size) 32 * 1024 * 1024)
		budget = (Size) 32 * 1024 * 1024;	/* floor: 32MB */
	return budget;
}

/*
 * Ceiling the per-participant flush budget may grow to.  Default (GUC == 0) is
 * 2 * maintenance_work_mem -- the memory-safe cap from 1.0.6 that prevents a
 * geometrically-doubling budget from driving a parallel build into swap death.
 * When pg_weave.build_mem_ceiling_mb > 0 the operator raises the ceiling to trade
 * RAM for fewer, larger segments (so a huge build stays under the segment cap);
 * peak build memory is ~(workers+1) * ceiling.  Never below 2*mwm so setting a
 * small value can't make the build flush more often than the default.
 */
static Size
weave_build_mem_ceiling(void)
{
	Size		default_ceiling = (Size) 2 *weave_build_mem_budget();
	Size		guc_ceiling;

	if (pg_weave_build_mem_ceiling_mb <= 0)
		return default_ceiling;
	guc_ceiling = (Size) pg_weave_build_mem_ceiling_mb * 1024 * 1024;
	return guc_ceiling > default_ceiling ? guc_ceiling : default_ceiling;
}

/*
 * Bound a build participant's flush-segment count without merging mid-build.
 *
 * Each participant (the serial builder, or the leader + each parallel worker)
 * flushes an in-memory segment every time its accumulator reaches the flush
 * budget, plus one residual at the end.  All participants append into the same
 * fixed WEAVE_MAX_SEGMENTS metapage directory, so a large build could otherwise
 * overflow it (weave_meta_add_segment errors) even though the data is fine and
 * merges to a single segment at the end.  A parallel worker cannot merge
 * mid-build to reclaim slots -- it runs while IsInParallelMode() and the
 * metapage swap is single-writer only -- so instead each participant caps its
 * own flush count by DOUBLING its budget every WEAVE_BUILD_FLUSHES_PER_TIER
 * flushes, UP TO A CEILING of 2 * maintenance_work_mem.  The doubling keeps the
 * flush (segment) count low on a huge build; the ceiling keeps peak MEMORY
 * bounded -- memory is the hard limit (exhausting it degrades the host),
 * segment-count overflow is a clean error backstopped by WEAVE_MAX_SEGMENTS.
 * With the cap a participant of total in-memory volume V flushes about
 *   V / (2 * maintenance_work_mem) + O(WEAVE_BUILD_FLUSHES_PER_TIER) rampup
 * segments, which stays well under the cap for realistic corpora, while peak
 * memory is bounded to (max_parallel_maintenance_workers + 1) * 2 * mwm.  An
 * UNcapped budget (2GB->4->8->16->32GB ...) instead let a participant grow to
 * tens of GB before flushing, driving a real 1.8M-doc / 19GB build into swap
 * death several hours in.  This needs no up-front size estimate (the in-memory
 * arena / heap-bytes ratio varies with the data), touches only this
 * participant's own state (parallel-safe), and never falls below the operator's
 * maintenance_work_mem for the first tier.  WEAVE_MAX_SEGMENTS stays as the hard
 * backstop.
 */
#define WEAVE_BUILD_FLUSHES_PER_TIER 8


/*
 * Flush the current in-memory build state as one immutable segment and reset
 * the state (freeing bs->ctx) so the heap scan can continue within a bounded
 * memory footprint.  A document's terms are always fully accumulated before a
 * flush (we only flush between tuples), so no document is split across
 * segments and each segment's ndocs/sumdoclen are self-consistent.
 */

static void
weave_build_flush_segment(Relation index, WeaveBuildState *bs)
{
	WeaveSegMeta seg;

	if (bs->nterms == 0)
		return;
	if (bs->nterms > 1)
		qsort(bs->terms, bs->nterms, sizeof(BuildTerm), cmp_buildterm);

	/*
	 * Serialize index-page writes across parallel-build participants.  During a
	 * parallel build several backends flush segments into the same index
	 * concurrently; pg_weave appends pages (weave_new_buffer -> ReadBuffer(P_NEW)),
	 * and overlapping appenders race on relation extension ("unexpected data
	 * beyond EOF").  Holding the relation extension lock around the whole
	 * segment write makes each participant's page additions atomic w.r.t. the
	 * others.  The expensive part of the build -- heap scan + tsearch analysis
	 * -- runs fully parallel, and the segment write appends pages via
	 * weave_new_buffer(), which now serializes only the P_NEW extension itself
	 * (per page) rather than the whole write -- so participants write
	 * concurrently.  In a serial build IsInParallelMode() is false and no
	 * extension lock is taken at all.
	 */
	weave_write_segment(index, bs, &seg);

	/*
	 * Install the descriptor without ever failing on a full directory.  In a
	 * PARALLEL build several participants flush concurrently and re-entering the
	 * merge machinery here risks the finalize wedge that 1.1.2 fixed, so a
	 * parallel participant uses the plain add and leaves compaction to the
	 * serial finalize; the build's flush-budget ceiling keeps its own segment
	 * count well under the cap.  Every other path (serial build, and especially
	 * live INSERT/pending-flush on a visible index) uses the room-ensuring add
	 * so a write is never refused because merging fell behind.
	 */
	if (IsInParallelMode())
	{
		if (!weave_meta_add_segment(index, &seg))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("weave index \"%s\" reached the maximum of %d segments during a parallel build",
							RelationGetRelationName(index), WEAVE_MAX_SEGMENTS),
					 errhint("Raise pg_weave.build_mem_ceiling_mb (fewer, larger segments) or lower max_parallel_maintenance_workers.")));
	}
	else
		weave_add_segment_with_room(index, &seg);

	/*
	 * Progress signal for a large build.  A high-vocabulary, heavy-tailed corpus
	 * (long email bodies, quoted chains, code/patches) makes the per-document
	 * text analysis (parse + stem) the dominant cost, so a serial build can run
	 * for many minutes between flushes while it accumulates a budget's worth of
	 * documents -- with nothing written and the segment count unchanged, which
	 * looks indistinguishable from a hang.  Emit a LOG line at each flush so an
	 * operator can see the build advancing (documents indexed, segments so far).
	 */
	elog(LOG, "pg_weave build: index \"%s\": flushed segment (%d terms, %.0f docs); %d segments so far",
		 RelationGetRelationName(index), bs->nterms, bs->ndocs, bs->nflushes + 1);

	/*
	 * Grow the flush budget geometrically so this participant's flush count
	 * stays far under WEAVE_MAX_SEGMENTS on a huge build, but CAP it at
	 * 2 * maintenance_work_mem so peak build memory stays bounded.  The cap is
	 * the hard limit: memory exhaustion crashes/degrades the host, whereas
	 * exceeding the segment count is a clean error (weave_meta_add_segment) with
	 * the end-of-build merge collapsing to one segment.  Uncapped doubling
	 * (2GB -> 4 -> 8 -> 16 -> 32GB ...) let bs->ctx grow to tens of GB before a
	 * flush; with the leader + max_parallel_maintenance_workers each holding its
	 * own budget, peak memory is (workers+1) * budget, so an uncapped budget
	 * drove a real 1.8M-doc / 19GB build into swap death several hours in (once
	 * participants crossed into the 16GB+ tier).  At the 2*mwm cap a
	 * per-participant build of accumulated in-memory volume V flushes about
	 * V/(2*mwm) + a few rampup segments; for realistic corpora that stays well
	 * under WEAVE_MAX_SEGMENTS, and peak memory is bounded to
	 * (workers+1) * 2 * maintenance_work_mem.
	 *
	 * Note: peak build memory is (max_parallel_maintenance_workers + 1) times
	 * this budget -- size maintenance_work_mem with that multiplier in mind.
	 */
	if (bs->flush_budget == 0)
		bs->flush_budget = weave_build_mem_budget();
	bs->nflushes++;
	if (bs->nflushes % WEAVE_BUILD_FLUSHES_PER_TIER == 0 &&
		bs->flush_budget < weave_build_mem_ceiling())
		bs->flush_budget *= 2;

	/* reset: free everything in the build context and start a fresh segment */
	MemoryContextReset(bs->ctx);
	bs->terms = NULL;
	bs->nterms = 0;
	bs->maxterms = 0;
	bs->ndocs = 0;
	bs->sumdoclen = 0;
	weave_build_ht_init(bs);
}

/* per-heap-tuple callback */
static void
weave_build_callback(Relation index, ItemPointer tid, Datum *values,
					bool *isnull, bool tupleIsAlive, void *state)
{
	WeaveBuildState *bs = (WeaveBuildState *) state;
	WeaveDoc		doc;
	WeaveTermEntry *entries;
	uint32		i;
	MemoryContext old;

	if (isnull[0])
		return;

	/*
	 * Bound build memory: if the accumulated segment has grown past the budget,
	 * flush it as a segment and continue with a fresh build state.  Checked
	 * between tuples so a document's terms are never split across segments.
	 *
	 * Recurse into child contexts (the `true`): the term dynahash (build_ht) is
	 * created with hcxt = bs->ctx, so dynahash puts its bucket directory and all
	 * TermHashEntry entries in a CHILD context of bs->ctx.  Counting only bs->ctx
	 * itself (the old `false`) missed the hash-table memory entirely, so on a
	 * huge-vocabulary corpus (e.g. long email/body text: quoted chains, patches,
	 * code -> millions of distinct terms) the flush undercounted the real working
	 * set and fired far too late, letting the build's memory grow for hours
	 * instead of settling at ~maintenance_work_mem.  bs->ctx and its children are
	 * exactly what MemoryContextReset frees at flush, so `true` counts precisely
	 * the reclaimable footprint the budget is meant to bound.
	 */
	if (bs->nterms > 0 &&
		MemoryContextMemAllocated(bs->ctx, true) >=
		(bs->flush_budget ? bs->flush_budget : weave_build_mem_budget()))
		weave_build_flush_segment(index, bs);

	old = MemoryContextSwitchTo(bs->ctx);

	doc = (WeaveDoc) PG_DETOAST_DATUM(values[0]);
	entries = WEAVE_DOC_ENTRIES(doc);

	for (i = 0; i < doc->nterms; i++)
	{
		const uint32 *pos = NULL;
		int			npos = 0;

		if (bs->want_positions && WEAVE_DOC_HAS_POS(doc))
		{
			pos = WEAVE_DOC_TERMPOS(doc, &entries[i]);
			npos = (int) entries[i].tf;
		}
		add_posting(bs, WEAVE_DOC_TERMTEXT(doc, &entries[i]), entries[i].len,
					tid, entries[i].tf, doc->doclen, pos, npos);
	}

	/*
	 * Corpus statistics (BM25 IDF + length normalization) must count only LIVE
	 * documents.  During CREATE INDEX/REINDEX/VACUUM FULL, PostgreSQL surfaces
	 * recently-dead tuples (deleted but not yet past the global horizon -- routine
	 * whenever any snapshot pins the horizon, e.g. a standby's feedback) to this
	 * callback with tupleIsAlive = false.  Such a tuple MUST still be indexed (an
	 * old snapshot may reach it via the index -- so the add_posting loop above
	 * runs unconditionally) but MUST NOT contribute to ndocs/sumdoclen: counting
	 * it biases IDF and average-document-length scoring and over-reports the
	 * document count.  Gate only the statistics on liveness.
	 */
	if (tupleIsAlive)
	{
		bs->ndocs += 1.0;
		bs->sumdoclen += doc->doclen;
	}

	/*
	 * Coarse progress heartbeat.  On a heavy corpus the per-document analysis
	 * dominates and a whole budget of documents accumulates between segment
	 * flushes (minutes of apparent silence); a periodic LOG line shows the scan
	 * is advancing rather than wedged.  A process-local counter is sufficient
	 * (serial build = one process; a parallel worker logs its own share).
	 */
	{
		static long	built = 0;

		if ((++built % 250000) == 0)
			elog(LOG, "pg_weave build: index \"%s\": ~%ld documents analyzed",
				 RelationGetRelationName(index), built);
	}

	MemoryContextSwitchTo(old);
}

/* ----- posting compression (delta + varint) ----- */

/*
 * Pack/unpack a heap TID into a monotonic 48-bit docid so that ascending TIDs
 * yield ascending docids and small gaps.  MaxHeapTuplesPerPage bounds the
 * offset, so block*factor+offset is monotonic in (block, offset).
 */
#define WEAVE_OFFSET_FACTOR ((uint64) MaxHeapTuplesPerPage)

static inline uint64
weave_tid_to_docid(ItemPointer tid)
{
	return (uint64) ItemPointerGetBlockNumber(tid) * WEAVE_OFFSET_FACTOR +
		(uint64) ItemPointerGetOffsetNumber(tid);
}

static inline void
weave_docid_to_tid(uint64 docid, ItemPointer tid)
{
	BlockNumber blk = (BlockNumber) (docid / WEAVE_OFFSET_FACTOR);
	OffsetNumber off = (OffsetNumber) (docid % WEAVE_OFFSET_FACTOR);

	ItemPointerSet(tid, blk, off);
}

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
static int
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
static bool weave_alloc_extend_only = false;

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
static void
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

static void
weave_alloc_end(void)
{
	if (weave_lowfree)
		pfree(weave_lowfree);
	weave_lowfree = NULL;
	weave_lowfree_n = 0;
	weave_lowfree_i = 0;
}

static Buffer
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

static void
weave_init_page(Page page, uint16 flags)
{
	WeavePageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(WeavePageOpaqueData));
	opaque = WeavePageGetOpaque(page);
	opaque->flags = flags;
	opaque->nextblk = InvalidBlockNumber;
	/* start item area at the (MAXALIGN'd) contents offset used by readers */
	((PageHeader) page)->pd_lower = (char *) PageGetContents(page) - (char *) page;
}

/*
 * Version-aware metapage read (the 1.5.0 dual-read fix).
 *
 * 1.5.0 added BlockNumber doclenstart to WeaveSegMeta, which GREW the struct
 * (v3 48 bytes -> v4 56 bytes with padding).  WeaveSegMeta is stored INLINE in
 * the metapage's segs[] array, so a v3 metapage lays segs[] out at the 48-byte
 * stride and places `generation` right after segs[128] at the v3 offset.  A v4
 * build that cast the page straight to WeaveMetaPageData read segs[1..] and
 * generation from the wrong offsets -> garbage livedocslen (palloc(-1)) and
 * garbage dictstart (wild block seek): the two upgrade regressions.
 *
 * This deserializes EITHER version into an in-memory v4 WeaveMetaPageData.  For
 * a v3 page it copies the fixed head, then expands each v3-stride segmeta into
 * the v4 struct and sets doclenstart = InvalidBlockNumber (segment carries
 * inline doclen), and reads `generation` from the v3 offset.  A v4 page is a
 * straight copy.  ALL readers use this instead of casting the page directly.
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

static void
weave_meta_from_page(Page page, WeaveMetaPageData *out)
{
	const WeaveMetaPageData *raw = WeavePageGetMeta(page);

	/*
	 * Layout contract (the 1.5.0 dual-read fix): the v3 read-struct and the
	 * live v4 struct MUST agree on every field up to and including segs[0], so a
	 * v3 metapage's head + first segment are read at identical offsets; only the
	 * segs[] STRIDE (48 vs 56 bytes) and the position of `generation` differ,
	 * which weave_meta_from_page handles explicitly.  These asserts fail the build
	 * if a future field insertion silently breaks that contract again.
	 */
	StaticAssertStmt(offsetof(WeaveMetaPageDataV3, segs) == offsetof(WeaveMetaPageData, segs),
					 "v3/v4 metapage head layout diverged");
	StaticAssertStmt(offsetof(WeaveSegMetaV3, dictindexstart) == offsetof(WeaveSegMeta, dictindexstart),
					 "v3/v4 segmeta head layout diverged");

	if (raw->version >= WEAVE_VERSION_DOCLEN_SIDECAR)
	{
		memcpy(out, raw, sizeof(WeaveMetaPageData));
		return;
	}
	/* v3 page: expand v3-stride segs[] into the v4 in-memory struct */
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
		}
	}
}

/*
 * Upcast a v3 metapage to the v4 in-place layout under the caller's exclusive
 * lock, via GenericXLog, so subsequent in-place struct writes are correct.
 * Idempotent: a no-op if the page is already v4.  MUST be called (under the
 * metapage's exclusive lock, before read-modify-writing it) by every path that
 * mutates the metapage in place (add-segment, merge, bulkdelete livedocs swap).
 * `page` is a GenericXLog-registered writable copy.
 */
static void
weave_meta_upcast_page(Page page)
{
	WeaveMetaPageData tmp;
	WeaveMetaPageData *m;

	if (WeavePageGetMeta(page)->version >= WEAVE_VERSION_DOCLEN_SIDECAR)
		return;

	weave_meta_from_page(page, &tmp);	/* read v3 into a v4-shaped temp */
	tmp.version = WEAVE_VERSION;
	m = WeavePageGetMeta(page);
	MemSet(m, 0, sizeof(WeaveMetaPageData));
	memcpy(m, &tmp, sizeof(WeaveMetaPageData));
	((PageHeader) page)->pd_lower =
		((char *) m + sizeof(WeaveMetaPageData)) - (char *) page;
}

static void
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
	weave_init_page(page, WEAVE_META);
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
static void
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

/*
 * Write all postings for one term into the segment's shared posting-page chain
 * via a WeavePostWriter, returning the term's first block + byte offset.
 * Postings are docid-sorted and packed into 128-doc FOR blocks (WeaveBlockHdr +
 * three frame-of-reference bit-packed columns: docid-gaps, tfs, doclens), which
 * compresses the common case of many clustered docids into a few bits each.
 */
typedef struct WeavePostingSort
{
	uint64		docid;
	uint32		tf;
	uint32		doclen;
	uint32		posoff;			/* start index into bt->positions (valid iff bt->positions) */
	uint32		poscnt;			/* stored position count (<= tf; 0 if dropped) */
	ItemPointerData tid;
}			WeavePostingSort;

static int
cmp_posting_docid(const void *a, const void *b)
{
	uint64		da = ((const WeavePostingSort *) a)->docid;
	uint64		db = ((const WeavePostingSort *) b)->docid;

	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

/* ---- doclen sidecar (format v4) --------------------------------------------
 *
 * One quantized length byte per document, on a WEAVE_DOCLEN page chain ordered
 * by ascending segment-local docid, in 128-doc blocks: a FOR-packed docid-gap
 * column then `count` raw length bytes.  Replaces the per-posting doclen FOR
 * column (once per doc x term) with one byte per doc.  A reader binary-locates
 * the block by first_docid then indexes the byte.
 *
 * The collector is a docid->byte hash so the map is built in O(ndocs) memory
 * regardless of Sum(df) (the P1 build-OOM was a flat Sum(df) array).  Callers
 * feed (docid, doclen) as they already iterate postings; duplicates (a doc seen
 * once per term) collapse in the hash.
 */
typedef struct DoclenEntry
{
	uint64		docid;
	uint8		byte;			/* quantized doclen */
} DoclenEntry;

/*
 * Per-heap-block leaf of the collector: doclen bytes for the offsets seen so
 * far, indexed by docid % WEAVE_OFFSET_FACTOR (the same split weave_docid_to_tid
 * uses, so every representable docid maps to exactly one slot and round-trips
 * bit-exactly through the cursor).  Grown on demand, so a block whose highest
 * live offset is 40 costs ~41 bytes rather than MaxHeapTuplesPerPage.  A zero
 * byte means "not seen" (weave_doclen_to_byte() yields 0 only for doclen 0,
 * which is coded identically, so the sentinel is exact).
 */
typedef struct DoclenLeaf
{
	uint16		nalloc;			/* bytes[] capacity */
	uint16		nseen;			/* distinct docids recorded in this leaf */
	uint8	   *bytes;
} DoclenLeaf;

/*
 * L15: docid -> quantized doclen, as a radix map keyed by heap block, then
 * offset.  This used to be a uint64-keyed dynahash probed ONCE PER POSTING by
 * both the segment writer and the merge (~240M probes into a ~2M-entry table
 * on a 2M x 120-word corpus) and was the largest single cost in CREATE INDEX
 * (bench/RESULTS_BUILD_PROFILE.md, bench/RESULTS_L15.md).  Docids are dense
 * per block (docid = block * MaxHeapTuplesPerPage + offset), so a two-level
 * array replaces the hash, iterates in docid order for free (no qsort), and
 * costs ~1 byte per live tuple plus a pointer per heap block.
 */
typedef struct DoclenCollector
{
	MemoryContext ctx;
	DoclenLeaf *leaves;			/* indexed by heap block number */
	BlockNumber nleaves;		/* leaves[] capacity */
	BlockNumber maxblk;			/* highest block with any entry, +1 */
	uint64		ndocs;			/* distinct docids recorded */
} DoclenCollector;

/* One doclen-sidecar block header: count docs, first docid for binary locate,
 * and the FOR-packed docid column length.  The `count` length bytes follow the
 * docid column.
 *
 * `count` carries the WEAVE_DOCLEN_ABS flag in its high bits (see
 * include/weave/am.h): set means the docid column holds ABSOLUTE offsets from
 * first_docid (v5, randomly addressable via weave_for_get), clear means gaps
 * from the predecessor (v4, must be prefix-summed).  Always read it through
 * WEAVE_DOCLEN_COUNT() -- the raw field is not a count.  `gapbytes` keeps its
 * name for on-disk-compatibility reasons; it is the docid column's length under
 * either encoding. */
typedef struct WeaveDoclenBlockHdr
{
	uint32		count;			/* docs in this block (<= WEAVE_BLOCK_SIZE) */
	uint32		first_docid_hi;
	uint32		first_docid_lo;
	uint32		gapbytes;		/* FOR-packed docid-gap column length */
} WeaveDoclenBlockHdr;

static void
doclen_collector_init(DoclenCollector *c, MemoryContext ctx, long nhint pg_attribute_unused())
{
	c->ctx = ctx;
	c->nleaves = 1024;
	c->maxblk = 0;
	c->ndocs = 0;
	c->leaves = (DoclenLeaf *) MemoryContextAllocZero(ctx,
														  (Size) c->nleaves * sizeof(DoclenLeaf));	/* alloc-ok: fixed 1024-entry seed; grown huge-safe in doclen_collector_add */
}

static void
doclen_collector_free(DoclenCollector *c)
{
	BlockNumber b;

	for (b = 0; b < c->maxblk; b++)
		if (c->leaves[b].bytes != NULL)
			pfree(c->leaves[b].bytes);
	pfree(c->leaves);
	c->leaves = NULL;
}

/* Record one doc's length (idempotent per docid: the byte is a function of the
 * length, identical across a doc's term postings). */
static inline void
doclen_collector_add(DoclenCollector *c, uint64 docid, uint32 doclen)
{
	BlockNumber blk = (BlockNumber) (docid / WEAVE_OFFSET_FACTOR);
	uint32		off = (uint32) (docid % WEAVE_OFFSET_FACTOR);	/* 0..FACTOR-1 */
	DoclenLeaf *leaf;

	if (blk >= c->nleaves)
	{
		BlockNumber newn = c->nleaves;
		MemoryContext old;

		while (newn <= blk)
			newn *= 2;
		old = MemoryContextSwitchTo(c->ctx);
		c->leaves = (DoclenLeaf *) WEAVE_REALLOC_MAYBE_HUGE(c->leaves,
															(Size) newn * sizeof(DoclenLeaf));
		MemoryContextSwitchTo(old);
		memset(c->leaves + c->nleaves, 0,
			   (Size) (newn - c->nleaves) * sizeof(DoclenLeaf));
		c->nleaves = newn;
	}
	if (blk >= c->maxblk)
		c->maxblk = blk + 1;
	leaf = &c->leaves[blk];
	if (off >= leaf->nalloc)
	{
		uint16		newn = leaf->nalloc ? leaf->nalloc : 16;

		while (newn <= off)
			newn = (uint16) Min((uint32) newn * 2, WEAVE_OFFSET_FACTOR);
		if (leaf->bytes == NULL)
			leaf->bytes = (uint8 *) MemoryContextAllocZero(c->ctx, newn);	/* alloc-ok: <= MaxHeapTuplesPerPage bytes */
		else
			leaf->bytes = (uint8 *) repalloc0(leaf->bytes, leaf->nalloc, newn);	/* alloc-ok: <= MaxHeapTuplesPerPage bytes */
		leaf->nalloc = newn;
	}
	if (leaf->bytes[off] == 0)
	{
		uint8		byte = weave_doclen_to_byte(doclen);

		/* doclen 0 codes to byte 0, the "unseen" sentinel, and is deliberately
		 * NOT stored: weave_doclen_lookup() returns 0 for an absent docid, so
		 * readers cannot tell the two apart and the sidecar only gets smaller. */
		if (byte == 0)
			return;
		leaf->bytes[off] = byte;
		leaf->nseen++;
		c->ndocs++;
	}
}

/*
 * Ascending-docid cursor over a DoclenCollector.  Because docids are
 * (block, offset) and the map is indexed the same way, in-order iteration
 * is a plain nested walk -- no materialized array, no sort.
 */
typedef struct DoclenCursor
{
	const DoclenCollector *c;
	BlockNumber blk;
	uint32		off;			/* 0-based index into leaf bytes[] */
} DoclenCursor;

static inline void
doclen_cursor_init(DoclenCursor *cur, const DoclenCollector *c)
{
	cur->c = c;
	cur->blk = 0;
	cur->off = 0;
}

static bool
doclen_cursor_next(DoclenCursor *cur, DoclenEntry *out)
{
	const DoclenCollector *c = cur->c;

	while (cur->blk < c->maxblk)
	{
		const DoclenLeaf *leaf = &c->leaves[cur->blk];

		if (leaf->nseen > 0)
		{
			while (cur->off < leaf->nalloc)
			{
				uint8		b = leaf->bytes[cur->off];
				uint32		off = cur->off;

				cur->off++;
				if (b != 0)
				{
					out->docid = (uint64) cur->blk * WEAVE_OFFSET_FACTOR + off;
					out->byte = b;
					return true;
				}
			}
		}
		cur->blk++;
		cur->off = 0;
	}
	return false;
}

/*
 * Write the collected docid->byte map to a WEAVE_DOCLEN page chain and return
 * the first page (Invalid if empty).  Defined after the posting-page writer
 * (WeavePostWriter) it reuses; forward-declared here.
 */
static BlockNumber weave_write_doclen_sidecar(Relation index, DoclenCollector *c);


/*
 * Shared posting-page writer.  All terms in a segment append their blocks into
 * ONE chain of posting pages, so a rare term (a handful of postings) costs a
 * few dozen bytes instead of a whole 8 KB page -- critical for a Zipfian
 * vocabulary where most terms are tiny.  Each term records its start (block +
 * byte offset); the reader decodes blocks from there, counting postings until
 * it has read the term's df, following nextblk across page boundaries.
 */
typedef struct WeavePostWriter
{
	Relation	index;
	Buffer		buffer;
	GenericXLogState *state;
	Page		page;
	bool		no_doclen_col;	/* v4: omit the per-posting doclen FOR column
									 * (it lives in the segment doclen sidecar); the
									 * block header still carries min_doclen for the
									 * block-max WAND bound. */
} WeavePostWriter;

static void
pw_begin(WeavePostWriter *pw, Relation index)
{
	pw->index = index;
	pw->buffer = InvalidBuffer;
	pw->state = NULL;
	pw->page = NULL;
	pw->no_doclen_col = false;
}

static void
pw_finish(WeavePostWriter *pw)
{
	if (pw->buffer != InvalidBuffer)
	{
		GenericXLogFinish(pw->state);
		UnlockReleaseBuffer(pw->buffer);
		pw->buffer = InvalidBuffer;
	}
}

/*
 * Append one term's postings to the shared writer.  Returns the block and byte
 * offset where the term's first block begins (for its dictionary entry).
 */
static void
weave_write_postings(WeavePostWriter *pw, BuildTerm *bt,
					BlockNumber *firstblk, uint32 *firstoff)
{
	Relation	index = pw->index;
	WeavePostingSort *sorted;
	int			i;
	bool		start_recorded = false;
	uint32		term_max_tf = 0;

	*firstblk = InvalidBlockNumber;
	*firstoff = 0;

	sorted = (WeavePostingSort *) ((Size) bt->nposts * sizeof(WeavePostingSort) > MaxAllocSize
								  ? MemoryContextAllocHuge(CurrentMemoryContext,
													  Max(bt->nposts, 1) * sizeof(WeavePostingSort))
								  : palloc(Max(bt->nposts, 1) * sizeof(WeavePostingSort)));	/* alloc-ok: huge branch of the > MaxAllocSize ternary above */
	for (i = 0; i < bt->nposts; i++)
	{
		sorted[i].docid = weave_tid_to_docid(&bt->tids[i]);
		sorted[i].tf = bt->tfs[i];
		sorted[i].doclen = bt->doclens[i];
		sorted[i].posoff = bt->positions ? bt->posoff[i] : 0;
		sorted[i].poscnt = bt->positions ? bt->poscnt[i] : 0;
		sorted[i].tid = bt->tids[i];
	}
	if (bt->nposts > 1)
		qsort(sorted, bt->nposts, sizeof(WeavePostingSort), cmp_posting_docid);

	i = 0;
	while (i < bt->nposts)
	{
		uint64		gaps[WEAVE_BLOCK_SIZE];
		uint64		tfs[WEAVE_BLOCK_SIZE];
		uint64		dls[WEAVE_BLOCK_SIZE];
		unsigned char scratch[3 * (1 + (WEAVE_BLOCK_SIZE * 64 + 7) / 8)];
		int			sclen = 0;
		uint32		blk_max_tf = 0;
		uint32		blk_min_dl = UINT32_MAX;
		uint64		blk_first_docid = sorted[i].docid;
		uint64		prev_docid = sorted[i].docid;
		int			bcount = 0;
		int			blk_first = i;
		int			poslen = 0;
		uint64	   *posdeltas = NULL;
		unsigned char *posbuf = NULL;
		char	   *pageend;
		Size		need;
		Size		usable;
		char	   *dst;
		WeaveBlockHdr *bh;

		/* gather up to WEAVE_BLOCK_SIZE postings into columns (SoA) */
		while (i < bt->nposts && bcount < WEAVE_BLOCK_SIZE)
		{
			gaps[bcount] = sorted[i].docid - prev_docid;	/* first gap is 0 */
			tfs[bcount] = sorted[i].tf;
			dls[bcount] = sorted[i].doclen;
			if (sorted[i].tf > blk_max_tf)
				blk_max_tf = sorted[i].tf;
			if (sorted[i].tf > term_max_tf)
				term_max_tf = sorted[i].tf;
			if (sorted[i].doclen < blk_min_dl)
				blk_min_dl = sorted[i].doclen;
			prev_docid = sorted[i].docid;
			bcount++;
			i++;
		}

		sclen += weave_for_pack(gaps, bcount, scratch + sclen);
		sclen += weave_for_pack(tfs, bcount, scratch + sclen);
		if (!pw->no_doclen_col)
			sclen += weave_for_pack(dls, bcount, scratch + sclen);	/* v3: inline doclen */

		/*
		 * Build the positions blob for this block (WITH positions=on).  It packs
		 * Sum(tf) delta values -- each posting's positions delta-coded from 0 and
		 * reset at the posting boundary.  Sum(tf) per block is unbounded in
		 * principle (P1's build-alloc trap), so size the scratch from the ACTUAL
		 * Sum(tf) with a huge-safe alloc, never a fixed stack array.  If the
		 * resulting block would not fit even an empty page, write the block WITH
		 * NO positions (posbytelen=0) -- decode then yields NULL positions for
		 * these postings and phrase eval correctly falls back to recheck for
		 * those docids (bounded, and only for pathological Sum(tf)).
		 */
		usable = BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(WeavePageOpaqueData));
		if (bt->positions)
		{
			int64		sumtf = 0;
			int			k;
			int			j = 0;
			bool		all_pos = true;

			/* only build the positions blob if EVERY posting in this block has a
			 * complete position set (poscnt == tf).  A posting whose positions
			 * were dropped on a prior write (poscnt < tf) cannot contribute tf
			 * deltas, so drop the whole block's positions (posbytelen=0) and let
			 * phrase fall back to recheck for these docids. */
			for (k = 0; k < bcount; k++)
			{
				sumtf += (int64) tfs[k];
				if (sorted[blk_first + k].poscnt != (uint32) tfs[k])
					all_pos = false;
			}
			if (sumtf > 0 && all_pos)
			{
				Size		dlbytes = (Size) sumtf * sizeof(uint64);
				Size		pbbytes = 1 + ((Size) sumtf * 32 + 7) / 8;	/* FOR worst case */

				posdeltas = (uint64 *) (dlbytes > MaxAllocSize
										? MemoryContextAllocHuge(CurrentMemoryContext, dlbytes)
										: palloc(dlbytes));
				posbuf = (unsigned char *) (pbbytes > MaxAllocSize
											? MemoryContextAllocHuge(CurrentMemoryContext, pbbytes)
											: palloc(pbbytes));
				for (k = 0; k < bcount; k++)
				{
					const uint32 *pp = bt->positions + sorted[blk_first + k].posoff;
					uint32		tf = (uint32) tfs[k];
					uint32		prev = 0;
					uint32		t;

					for (t = 0; t < tf; t++)
					{
						/* positions are ascending within a posting; delta-code,
						 * reset prev to 0 at each posting boundary.  The index posting
						 * positions carry only the ORDINAL (low 30 bits); the weight
						 * label lives in the heap wdoc value and a field-restricted
						 * (term:LABEL) query applies the label filter via the heap
						 * recheck path, so the on-disk posting format is UNCHANGED from
						 * v3 (no reindex).  Mask the label defensively in case a v4
						 * value ever reaches the build with labels set. */
						uint32		ord = WEAVE_POS_ORD(pp[t]);

						posdeltas[j++] = (uint64) (ord - prev);
						prev = ord;
					}
				}
				poslen = weave_for_pack(posdeltas, (int) sumtf, posbuf);
			}
		}

		need = MAXALIGN(sizeof(WeaveBlockHdr) + sclen + poslen);
		if (poslen > 0 && need > usable)
		{
			/* positions push the block past a whole page: drop them for this
			 * block (recheck fallback keeps phrase correct for these docids) */
			poslen = 0;
			need = MAXALIGN(sizeof(WeaveBlockHdr) + sclen);
		}

		/* need a page with room for this block? */
		if (pw->buffer != InvalidBuffer)
		{
			pageend = (char *) pw->page + BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData));
			if ((char *) pw->page + ((PageHeader) pw->page)->pd_lower + need > pageend)
			{
				Buffer		next = weave_new_buffer(index);
				BlockNumber nextblk = BufferGetBlockNumber(next);

				WeavePageGetOpaque(pw->page)->nextblk = nextblk;
				GenericXLogFinish(pw->state);
				UnlockReleaseBuffer(pw->buffer);
				pw->buffer = next;
				pw->state = GenericXLogStart(index);
				pw->page = GenericXLogRegisterBuffer(pw->state, pw->buffer, GENERIC_XLOG_FULL_IMAGE);
				weave_init_page(pw->page, WEAVE_POSTING);
			}
		}
		if (pw->buffer == InvalidBuffer)
		{
			pw->buffer = weave_new_buffer(index);
			pw->state = GenericXLogStart(index);
			pw->page = GenericXLogRegisterBuffer(pw->state, pw->buffer, GENERIC_XLOG_FULL_IMAGE);
			weave_init_page(pw->page, WEAVE_POSTING);
		}

		/* record the term's start at its first block */
		if (!start_recorded)
		{
			*firstblk = BufferGetBlockNumber(pw->buffer);
			*firstoff = (uint32) ((PageHeader) pw->page)->pd_lower;
			start_recorded = true;
		}

		dst = (char *) pw->page + ((PageHeader) pw->page)->pd_lower;
		bh = (WeaveBlockHdr *) dst;
		bh->count = (uint32) bcount;
		bh->max_tf = blk_max_tf;
		bh->min_doclen = (blk_min_dl == UINT32_MAX ? 0 : blk_min_dl);
		bh->first_docid_hi = (uint32) (blk_first_docid >> 32);
		bh->first_docid_lo = (uint32) (blk_first_docid & 0xFFFFFFFF);
		bh->bytelen = (uint32) sclen;
		bh->posbytelen = (uint32) poslen;
		memcpy((char *) (bh + 1), scratch, sclen);
		if (poslen > 0)
			memcpy((char *) (bh + 1) + sclen, posbuf, poslen);
		((PageHeader) pw->page)->pd_lower += need;
		if (posdeltas)
			pfree(posdeltas);
		if (posbuf)
			pfree(posbuf);
	}

	pfree(sorted);
	bt->max_tf = term_max_tf;	/* dictionary reads this; no tfs[] rescan */
}

/*
 * Write the collected docid->byte map to a WEAVE_DOCLEN page chain and return
 * the first page (Invalid if empty).  Reuses WeavePostWriter and the same
 * page-fit idiom as weave_write_postings.
 */
static BlockNumber
weave_write_doclen_sidecar(Relation index, DoclenCollector *c)
{
	DoclenCursor cur;
	DoclenEntry e;
	bool		have;
	WeavePostWriter pw;
	BlockNumber first = InvalidBlockNumber;
	bool		start_recorded = false;

	if (c->ndocs == 0)
		return InvalidBlockNumber;

	doclen_cursor_init(&cur, c);
	have = doclen_cursor_next(&cur, &e);

	pw_begin(&pw, index);
	while (have)
	{
		uint64		offs[WEAVE_BLOCK_SIZE];
		uint8		bytes[WEAVE_BLOCK_SIZE];
		unsigned char gapscratch[1 + (WEAVE_BLOCK_SIZE * 64 + 7) / 8];
		uint64		first_docid = e.docid;
		int			bcount = 0;
		int			gapbytes;
		Size		need;
		char	   *pageend;
		char	   *dst;
		WeaveDoclenBlockHdr *bh;

		/*
		 * v5: store each docid as its ABSOLUTE offset from this block's
		 * first_docid, not as a gap from its predecessor.  Same codec, same
		 * column, different values -- but the column is now monotone and
		 * FIXED-WIDTH-addressable, so a reader can weave_for_get() any entry in
		 * O(1) and binary-search the block instead of FOR-unpacking all 128
		 * entries and prefix-summing them.  That decode was ~72% of a ranked
		 * scan (bench/RESULTS_SCAN_PROFILE.md): a single-term scan probes
		 * ascending docids with stride ndocs/df, so a 128-docid block covers
		 * ~2.6 candidates and was fully decoded to answer each of them.
		 *
		 * Cost: the coded width grows from ~log2(stride) to ~log2(block span),
		 * about 6 -> 13 bits on a 2M-doc corpus, measured as sidecar 4,672 kB ->
		 * ~9 MB, i.e. +0.7% of a 625 MB index.  Bought a ~18x cut in in-block
		 * decode work.
		 */
		while (have && bcount < WEAVE_BLOCK_SIZE)
		{
			offs[bcount] = e.docid - first_docid;	/* offs[0] == 0 */
			bytes[bcount] = e.byte;
			bcount++;
			have = doclen_cursor_next(&cur, &e);
		}
		gapbytes = weave_for_pack(offs, bcount, gapscratch);
		need = MAXALIGN(sizeof(WeaveDoclenBlockHdr) + gapbytes + bcount);

		if (pw.buffer != InvalidBuffer)
		{
			pageend = (char *) pw.page + BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData));
			if ((char *) pw.page + ((PageHeader) pw.page)->pd_lower + need > pageend)
			{
				Buffer		next = weave_new_buffer(index);
				BlockNumber nextblk = BufferGetBlockNumber(next);

				WeavePageGetOpaque(pw.page)->nextblk = nextblk;
				GenericXLogFinish(pw.state);
				UnlockReleaseBuffer(pw.buffer);
				pw.buffer = next;
				pw.state = GenericXLogStart(index);
				pw.page = GenericXLogRegisterBuffer(pw.state, pw.buffer, GENERIC_XLOG_FULL_IMAGE);
				weave_init_page(pw.page, WEAVE_DOCLEN);
			}
		}
		if (pw.buffer == InvalidBuffer)
		{
			pw.buffer = weave_new_buffer(index);
			pw.state = GenericXLogStart(index);
			pw.page = GenericXLogRegisterBuffer(pw.state, pw.buffer, GENERIC_XLOG_FULL_IMAGE);
			weave_init_page(pw.page, WEAVE_DOCLEN);
		}
		if (!start_recorded)
		{
			first = BufferGetBlockNumber(pw.buffer);
			start_recorded = true;
		}
		dst = (char *) pw.page + ((PageHeader) pw.page)->pd_lower;
		bh = (WeaveDoclenBlockHdr *) dst;
		bh->count = (uint32) bcount | WEAVE_DOCLEN_ABS;	/* v5: absolute offsets */
		bh->first_docid_hi = (uint32) (first_docid >> 32);
		bh->first_docid_lo = (uint32) (first_docid & 0xFFFFFFFF);
		bh->gapbytes = (uint32) gapbytes;
		memcpy((char *) (bh + 1), gapscratch, gapbytes);
		memcpy((char *) (bh + 1) + gapbytes, bytes, bcount);
		((PageHeader) pw.page)->pd_lower += need;
	}
	pw_finish(&pw);
	return first;
}

/* ---- doclen sidecar reader (format v4) -------------------------------------
 *
 * A resident, decoded copy of one segment's sidecar: ascending docids and their
 * quantized length bytes.  Loaded ONCE per (segment, scan) -- like tombstones
 * -- then binary-searched per scored posting.  Callers that need a doc's exact
 * length (scoring, merge) use weave_doclen_lookup.
 */
typedef struct WeaveDoclens
{
	uint64	   *docids;		/* ascending; NULL if the segment is v3 (inline) */
	uint8	   *bytes;			/* parallel to docids */
	int			n;
} WeaveDoclens;

/* Load a segment's WEAVE_DOCLEN chain into a resident array.  doclenstart ==
 * Invalid (a v3 segment) yields an empty map (n=0, docids=NULL) -- the caller
 * then falls back to the inline posting doclen. */
static void
weave_doclens_load(Relation index, BlockNumber doclenstart, WeaveDoclens *d)
{
	BlockNumber blk = doclenstart;
	int			cap = 0;
	BlockNumber nblocks;
	uint32		visited = 0;

	d->docids = NULL;
	d->bytes = NULL;
	d->n = 0;
	if (doclenstart == InvalidBlockNumber)
		return;
	nblocks = RelationGetNumberOfBlocks(index);

	while (blk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();
		/*
		 * Concurrency guard (the A1 race): a scan reads the sidecar chain under
		 * only per-page SHARE locks off a metapage snapshot, so a concurrent
		 * merge/vacuum can free + recycle these pages mid-walk and leave a
		 * garbage nextblk that points anywhere (a cycle, a non-sidecar page, or
		 * out of range).  The caller discards + retries on a generation change,
		 * but this decode must not crash or spin first.  So: bound the walk to
		 * the relation's block count (no runaway/cycle) and skip any page that
		 * is no longer a WEAVE_DOCLEN page (a recycled/other-type page ends the
		 * walk).  Bounds inside the block loop already guard a torn page. */
		if (blk >= nblocks || visited++ > nblocks)
			break;
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) ||
			!(WeavePageGetOpaque(page)->flags & WEAVE_DOCLEN))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		ptr = (char *) page + MAXALIGN(SizeOfPageHeaderData);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr + sizeof(WeaveDoclenBlockHdr) <= end)
		{
			WeaveDoclenBlockHdr *bh = (WeaveDoclenBlockHdr *) ptr;
			uint64		first_docid;
			uint64		vals[WEAVE_BLOCK_SIZE];
			uint8	   *bytes;
			uint32		bcount;
			bool		isabs;
			int			j;
			char	   *blkend;

			/* bounds-guard a possibly-recycled/corrupt page (same contract as the
			 * dict/posting readers): a bad count/gapbytes must not run past end.
			 * The v5 absolute-offset flag lives in the high bits of `count`, so
			 * strip it before any arithmetic (WEAVE_DOCLEN_COUNT). */
			isabs = WEAVE_DOCLEN_IS_ABS(bh->count);
			bcount = WEAVE_DOCLEN_COUNT(bh->count);
			if (bcount == 0 || bcount > WEAVE_BLOCK_SIZE)
				break;
			blkend = (char *) (bh + 1) + bh->gapbytes + bcount;
			if (blkend > end)
				break;
			first_docid = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
			weave_for_unpack((unsigned char *) (bh + 1), (int) bcount, vals);
			bytes = (uint8 *) ((char *) (bh + 1) + bh->gapbytes);

			if (d->n + (int) bcount > cap)
			{
				cap = Max(cap * 2, d->n + (int) bcount + 128);
				d->docids = d->docids
					? (uint64 *) repalloc(d->docids, (Size) cap * sizeof(uint64))
					: (uint64 *) palloc((Size) cap * sizeof(uint64));
				d->bytes = d->bytes
					? (uint8 *) repalloc(d->bytes, (Size) cap * sizeof(uint8))
					: (uint8 *) palloc((Size) cap * sizeof(uint8));
			}
			/* v5 stores offsets from first_docid; v4 stores gaps.  This path
			 * materializes the whole sidecar either way (it feeds the merge, which
			 * wants every docid), so the only difference is how a value maps to a
			 * docid: add to the base, or accumulate. */
			if (isabs)
			{
				for (j = 0; j < (int) bcount; j++)
				{
					d->docids[d->n] = first_docid + vals[j];	/* vals[0] == 0 */
					d->bytes[d->n] = bytes[j];
					d->n++;
				}
			}
			else
			{
				uint64		acc = first_docid;

				for (j = 0; j < (int) bcount; j++)
				{
					if (j > 0)
						acc += vals[j];	/* v4: gaps[0] == 0 */
					d->docids[d->n] = acc;
					d->bytes[d->n] = bytes[j];
					d->n++;
				}
			}
			ptr = (char *) MAXALIGN((char *) (bh + 1) + bh->gapbytes + bcount);
		}
		UnlockReleaseBuffer(buf);
		blk = next;
	}
}

static void
weave_doclens_free(WeaveDoclens *d)
{
	if (d->docids)
		pfree(d->docids);
	if (d->bytes)
		pfree(d->bytes);
	d->docids = NULL;
	d->bytes = NULL;
	d->n = 0;
}

/* Exact per-doc length for a docid from the resident sidecar, or 0 if absent
 * (a v3 segment's empty map, or a docid not in the sidecar -- caller falls back
 * to the inline posting doclen).  Binary search over ascending docids. */
static inline uint32
weave_doclen_lookup(const WeaveDoclens *d, uint64 docid)
{
	int			lo = 0,
				hi = d->n - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) >> 1;

		if (d->docids[mid] < docid)
			lo = mid + 1;
		else if (d->docids[mid] > docid)
			hi = mid - 1;
		else
			return weave_byte_to_doclen(d->bytes[mid]);
	}
	return 0;
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
/*
 * Resident decoded doclen block, SHARED by every cursor of one scan that reads
 * the same segment sidecar.
 *
 * Each (term, segment) gets its own WeaveDoclenCursor, but for a multi-term query
 * every cursor sitting at the scored pivot docid looks up THE SAME docid -- so
 * with per-cursor resident blocks an N-term match decoded the same sidecar block
 * N times.  Hoisting the resident block into a per-segment shared slot makes the
 * 2nd..Nth lookup of a docid a pure in-memory binary search.  Single-term scans
 * are unaffected (one cursor, one slot).
 */
typedef struct WeaveDoclenResident
{
	BlockNumber blk;			/* page the resident block came from, or Invalid */
	uint64	   *docid;			/* v4 only: decoded docids (palloc'd) */
	uint8	   *byte;			/* v4 only: parallel quantized bytes (palloc'd) */
	int			n;				/* docs in the resident block */
	int			cap;			/* capacity of docid/byte */
	uint64		first;			/* lowest docid in the resident block */
	uint64		last;			/* highest docid in the resident block */
	int			hint;			/* resume index: the scan probes ASCENDING docids, so the
								 * next hit is usually at/just after the previous one */

	/*
	 * v5 fast path.  A v5 block's docid column is absolute offsets from `base`
	 * and is therefore fixed-width addressable, so instead of decoding the block
	 * we keep a COPY OF ITS PACKED BYTES and binary-search them in place with
	 * weave_for_get().  The copy is a few hundred bytes (a 128-entry column at
	 * ~13 bits plus 128 length bytes) against the 128 FOR-unpacks + 128
	 * prefix-sum stores it replaces -- which were ~72% of a ranked scan
	 * (bench/RESULTS_SCAN_PROFILE.md).  `docid`/`byte` stay unused for v5.
	 *
	 * A copy rather than a pin: load_page releases the buffer before returning,
	 * and holding a pin across executor calls to keep the block addressable
	 * would be a materially bigger change to the scan's locking story for no
	 * measured gain (buffer lookup was 1.9% of the scan).
	 */
	bool		isabs;			/* resident block is v5 (absolute offsets) */
	unsigned char *raw;			/* packed docid column, copied (palloc'd) */
	int			rawcap;			/* capacity of raw */
	const uint8 *rawbyte;		/* length bytes, inside raw[] after the column */
	uint64		base;			/* block's first_docid */
} WeaveDoclenResident;

typedef struct WeaveDoclenCursor
{
	Relation	index;
	BlockNumber start;			/* sidecar chain head; Invalid = v3 (inline) */
	const uint64 *dir_docid;	/* per-page first docid, ascending (borrowed) */
	const BlockNumber *dir_blk;	/* per-page block number (parallel) */
	int			dir_n;			/* directory entries (= sidecar pages) */
	int			dir_hint;		/* last page index hit (ascending-resume) */
	WeaveDoclenResident *res;	/* SHARED resident block for this segment (borrowed
								 * from the scan's per-segment slot; never freed
								 * by the cursor) */
} WeaveDoclenCursor;

/*
 * Relation-level doclen page-directory cache (ONE contiguous chunk, the
 * rd_amcache single-chunk contract).  Layout: header, then for each live
 * sidecar segment its (first_docid[], blk[]) page directory, packed back to
 * back.  Rebuilt only when the metapage `generation` moves.  The whole thing is
 * a few KB (one 12-byte entry per sidecar PAGE, ~534 for a 2.19M-doc segment),
 * so a single palloc satisfies rd_amcache's "single chunk, pfree()d wholesale
 * on relcache invalidation" rule (the 1.5.4 20MB multi-chunk decoded array
 * violated it -- this does not).
 */
typedef struct WeaveDoclenDir
{
	BlockNumber start;			/* segment doclenstart this directory describes */
	int			n;				/* number of pages (entries) */
	int			docid_off;		/* uint64 index into the packed docid[] region */
	int			blk_off;		/* BlockNumber index into the packed blk[] region */
} WeaveDoclenDir;

typedef struct WeaveDoclenDirCache
{
	uint32		generation;		/* metapage generation this cache was built at */
	int			nsegs;
	int			ndocid;			/* total entries across all segs (docid[] len) */
	WeaveDoclenDir segs[WEAVE_MAX_SEGMENTS];
	/* packed regions follow in the SAME allocation: uint64 docid[ndocid] then
	 * BlockNumber blk[ndocid].  Accessed via the *_off indices above. */
	uint64		data[FLEXIBLE_ARRAY_MEMBER];
} WeaveDoclenDirCache;

#define WEAVE_DOCLENDIR_DOCIDS(dc) ((dc)->data)
#define WEAVE_DOCLENDIR_BLKS(dc)   ((BlockNumber *) ((dc)->data + (dc)->ndocid))

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
		if (PageIsNew(page) || !(WeavePageGetOpaque(page)->flags & WEAVE_DOCLEN))
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
		if (PageIsNew(page) || !(WeavePageGetOpaque(page)->flags & WEAVE_DOCLEN))
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
static WeaveDoclenDirCache *
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

static void
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
		/* Attach the SHARED resident block for this segment.  Lazily size it to
		 * one 128-doc block (we decode only the block covering the sought docid,
		 * never a whole page). */
		if (res->docid == NULL)
		{
			res->cap = WEAVE_BLOCK_SIZE;
			res->docid = (uint64 *) palloc(res->cap * sizeof(uint64));
			res->byte = (uint8 *) palloc(res->cap * sizeof(uint8));
			/* v5: room for one block's packed docid column plus its length bytes.
			 * The column is at most WEAVE_BLOCK_SIZE 64-bit values plus the codec's
			 * leading width byte, which is also the writer's own scratch bound. */
			res->rawcap = 1 + (WEAVE_BLOCK_SIZE * 64 + 7) / 8 + WEAVE_BLOCK_SIZE;
			res->raw = (unsigned char *) palloc(res->rawcap);
			res->rawbyte = NULL;
			res->isabs = false;
			res->base = 0;
			res->blk = InvalidBlockNumber;
			res->n = 0;
			res->first = 0;
			res->last = 0;
			res->hint = 0;
		}
		c->res = res;
	}
}

static void
weave_doclen_cursor_free(WeaveDoclenCursor *c)
{
	/* the directory arrays are borrowed from the relcache cache and the resident
	 * block from the scan's per-segment slot -- nothing here is cursor-owned */
	c->dir_docid = NULL;
	c->dir_blk = NULL;
	c->dir_n = 0;
	c->res = NULL;
}

/* Decode the ONE sidecar block on page `blkno` whose docid range covers `docid`
 * into the cursor's resident arrays.
 *
 * A sidecar page holds many 128-doc blocks (~31 of them, ~4000 docs).  Decoding
 * the WHOLE page per lookup was a large amplification for a scattered rare term:
 * 10,875 postings spread over 2.19M docids touch essentially every sidecar page,
 * and decoding ~4000 entries to answer each lookup meant ~2.2M doc-decodes to
 * score 10,875 postings (~200x amplification; measured as ~7.9 ms of the 10.2 ms
 * rare-term ranked latency).  So: walk only the block HEADERS (first_docid +
 * count + gapbytes -- no FOR-unpack) to find the covering block, then take that
 * single block.  Same page read, ~31x less work, no format change.
 *
 * v5 goes further and does not decode the block at all.  Its docid column is
 * absolute offsets from first_docid, hence fixed-width addressable, so we copy
 * the packed column (a few hundred bytes) and let the caller binary-search it
 * with weave_for_get().  v4's gap-coded column still has to be unpacked and
 * prefix-summed into r->docid[]/r->byte[], which is the path this function took
 * for every block change and which measured ~72% of a ranked scan
 * (bench/RESULTS_SCAN_PROFILE.md).
 *
 * `res_first`/`res_last` record the resident block's docid range so the caller's
 * fast path can tell whether a later docid is still covered. */
static void
weave_doclen_cursor_load_page(WeaveDoclenCursor *c, BlockNumber blkno, uint64 docid)
{
	WeaveDoclenResident *r = c->res;
	Buffer		buf;
	Page		page;
	char	   *ptr,
			   *end;
	char	   *cand = NULL;		/* best (largest first_docid <= docid) block */

	if (r == NULL)
		return;
	r->n = 0;
	r->blk = blkno;
	r->first = 0;
	r->last = 0;
	r->hint = 0;				/* new block: the ascending-resume hint restarts */
	r->isabs = false;
	r->rawbyte = NULL;
	r->base = 0;
	if (blkno == InvalidBlockNumber || r->docid == NULL ||
		blkno >= RelationGetNumberOfBlocks(c->index))
		return;
	buf = ReadBuffer(c->index, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page) || !(WeavePageGetOpaque(page)->flags & WEAVE_DOCLEN))
	{
		UnlockReleaseBuffer(buf);
		return;
	}
	end = (char *) page + ((PageHeader) page)->pd_lower;

	/* pass 1: headers only -- find the last block whose first_docid <= docid */
	ptr = (char *) page + MAXALIGN(SizeOfPageHeaderData);
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

	/* pass 2: take ONLY the covering block */
	if (cand != NULL)
	{
		WeaveDoclenBlockHdr *bh = (WeaveDoclenBlockHdr *) cand;
		uint32		bcount = WEAVE_DOCLEN_COUNT(bh->count);
		uint8	   *bytes = (uint8 *) ((char *) (bh + 1) + bh->gapbytes);

		if (WEAVE_DOCLEN_IS_ABS(bh->count))
		{
			/*
			 * v5: copy the packed column + its length bytes and leave them
			 * packed.  first/last come from the column's endpoints, which are
			 * O(1) reads -- offs[0] is always 0, so `first` is the base.
			 */
			Size		need = (Size) bh->gapbytes + bcount;

			if (need <= (Size) r->rawcap)
			{
				memcpy(r->raw, (unsigned char *) (bh + 1), need);
				r->rawbyte = (const uint8 *) (r->raw + bh->gapbytes);
				r->base = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
				r->n = (int) bcount;
				r->isabs = true;
				r->first = r->base;
				r->last = r->base + weave_for_get(r->raw, (int) bcount - 1);
			}
		}
		else
		{
			/* v4: gap-coded, so the block must be unpacked and prefix-summed */
			uint64		gaps[WEAVE_BLOCK_SIZE];
			uint64		acc;
			int			j;

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
	UnlockReleaseBuffer(buf);
}

/* Exact doclen for docid via the page-directory cursor.  Robust to ANY docid
 * order; the ascending-resume hint makes the common monotone WAND scan land on
 * the resident page.  Returns 0 if absent (v3 cursor, or docid not present). */
static inline uint32
weave_doclen_cursor_lookup(WeaveDoclenCursor *c, uint64 docid)
{
	WeaveDoclenResident *r = c->res;
	int			lo,
				hi,
				pg;

	if (c->dir_n == 0 || c->dir_docid == NULL || r == NULL)
		return 0;

	/* Fast path: docid is inside the resident BLOCK's range.  This now also hits
	 * when a DIFFERENT term's cursor of the same segment already decoded the
	 * block for this pivot docid (the multi-term win). */
	if (r->n > 0 && docid >= r->first && docid <= r->last)
	{
		/* fall through to the in-block search below */
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
	 * v5 searches the block's PACKED column in place via weave_for_get (O(1) per
	 * probe, fixed-width offsets from r->base), so no block decode happened at
	 * all.  v4 searches the arrays that load_page had to materialize.  Both use
	 * the same walk-then-bisect shape; only the accessor differs.
	 */
	if (r->isabs)
	{
		int			rlo = 0,
					rhi = r->n - 1;
		int			i = r->hint;
		int			lim;
		uint64		want;

		/* offsets are relative to r->base; a docid below it cannot be here */
		if (docid < r->base)
			return 0;
		want = docid - r->base;

		if (i < 0 || i >= r->n)
			i = 0;
		lim = i + 8;
		if (lim > r->n)
			lim = r->n;
		for (; i < lim; i++)
		{
			uint64		v = weave_for_get(r->raw, i);

			if (v == want)
			{
				r->hint = i + 1;
				return weave_byte_to_doclen(r->rawbyte[i]);
			}
			if (v > want)
				break;			/* overshot: docid is absent (gap) or behind us */
		}

		while (rlo <= rhi)
		{
			int			mid = (rlo + rhi) >> 1;
			uint64		v = weave_for_get(r->raw, mid);

			if (v < want)
				rlo = mid + 1;
			else if (v > want)
				rhi = mid - 1;
			else
			{
				r->hint = mid + 1;
				return weave_byte_to_doclen(r->rawbyte[mid]);
			}
		}
	}
	else
	{
		int			rlo = 0,
					rhi = r->n - 1;
		int			i = r->hint;
		int			lim;

		if (i < 0 || i >= r->n)
			i = 0;
		lim = i + 8;
		if (lim > r->n)
			lim = r->n;
		for (; i < lim; i++)
		{
			if (r->docid[i] == docid)
			{
				r->hint = i + 1;
				return weave_byte_to_doclen(r->byte[i]);
			}
			if (r->docid[i] > docid)
				break;			/* overshot: docid is absent (gap) or behind us */
		}

		while (rlo <= rhi)
		{
			int			mid = (rlo + rhi) >> 1;

			if (r->docid[mid] < docid)
				rlo = mid + 1;
			else if (r->docid[mid] > docid)
				rhi = mid - 1;
			else
			{
				r->hint = mid + 1;
				return weave_byte_to_doclen(r->byte[mid]);
			}
		}
	}
	return 0;
}

/*
 * Write the dictionary: sorted (term, df, firstposting) entries packed into a
 * chain of dictionary pages.  Returns the first dictionary block, and via
 * *indexstart the first page of the sparse block index (Invalid if empty).
 */
/*
 * One dictionary record streamed into weave_write_dictionary_iter: the term
 * bytes plus the metadata a WeaveDictEntry needs.  `term` need only stay valid
 * until the iterator's next() call.
 */
typedef struct DictRec
{
	const char *term;
	int			len;
	uint32		df;
	uint32		max_tf;
	BlockNumber firstposting;
	uint32		firstoffset;
} DictRec;

/* Iterator: fill *r with the next term in sorted order, return false at end. */
typedef bool (*DictNextFn) (void *state, DictRec *r);

/*
 * Write a segment's on-disk dictionary (dict pages + sparse block index) by
 * pulling terms from an iterator in sorted order.  O(1) caller memory: the only
 * state retained across the stream is the per-DICT-PAGE block-index metadata
 * (one entry per ~8KB page, i.e. index_size/BLCKSZ entries -- tiny), including a
 * copy of each page's first term's bytes so the block-index pass needs no
 * random access back into the (possibly spilled) term stream.
 */
static BlockNumber
weave_write_dictionary_iter(Relation index, DictNextFn next, void *nstate,
						   BlockNumber *indexstart)
{
	BlockNumber first = InvalidBlockNumber;
	Buffer		buffer = InvalidBuffer;
	GenericXLogState *state = NULL;
	Page		page = NULL;
	DictRec		r;

	/* block index: (blk, first-term bytes) per dict page -- bounded by #pages */
	BlockNumber *pgblk = NULL;
	char	  **pgfirst = NULL;	/* first term bytes of each page (palloc'd) */
	int		   *pgfirstlen = NULL;
	int			npages = 0;
	int			pgcap = 0;
	int			j;

	*indexstart = InvalidBlockNumber;

	while (next(nstate, &r))
	{
		Size		need = MAXALIGN(sizeof(WeaveDictEntry) + r.len);
		char	   *dst;
		bool		newpage = false;

		CHECK_FOR_INTERRUPTS();		/* per-term; page-copy semantics keep it safe */

		if (buffer == InvalidBuffer ||
			((PageHeader) page)->pd_lower + need >
			BLCKSZ - sizeof(WeavePageOpaqueData))
		{
			Buffer		nextbuf = weave_new_buffer(index);
			BlockNumber nextblk = BufferGetBlockNumber(nextbuf);

			if (buffer != InvalidBuffer)
			{
				WeavePageGetOpaque(page)->nextblk = nextblk;
				GenericXLogFinish(state);
				UnlockReleaseBuffer(buffer);
			}
			else
				first = nextblk;

			buffer = nextbuf;
			state = GenericXLogStart(index);
			page = GenericXLogRegisterBuffer(state, buffer, GENERIC_XLOG_FULL_IMAGE);
			weave_init_page(page, WEAVE_DICT);
			newpage = true;
		}

		if (newpage)
		{
			if (npages >= pgcap)
			{
				pgcap = Max(pgcap * 2, 64);
				pgblk = pgblk ? repalloc(pgblk, pgcap * sizeof(BlockNumber))
					: palloc(pgcap * sizeof(BlockNumber));
				pgfirst = pgfirst ? repalloc(pgfirst, pgcap * sizeof(char *))
					: palloc(pgcap * sizeof(char *));
				pgfirstlen = pgfirstlen ? repalloc(pgfirstlen, pgcap * sizeof(int))
					: palloc(pgcap * sizeof(int));
			}
			pgblk[npages] = BufferGetBlockNumber(buffer);
			pgfirstlen[npages] = r.len;
			pgfirst[npages] = (char *) palloc(Max(r.len, 1));
			memcpy(pgfirst[npages], r.term, r.len);
			npages++;
		}

		dst = (char *) page + ((PageHeader) page)->pd_lower;
		{
			WeaveDictEntry *de = (WeaveDictEntry *) dst;

			de->termlen = r.len;
			de->df = r.df;
			de->max_tf = r.max_tf;
			de->firstposting = r.firstposting;
			de->firstoffset = r.firstoffset;
			memcpy(de->term, r.term, r.len);
		}
		((PageHeader) page)->pd_lower += need;
	}

	if (buffer != InvalidBuffer)
	{
		GenericXLogFinish(state);
		UnlockReleaseBuffer(buffer);
	}

	/* write the sparse block index: one entry per dict page, in term order */
	if (npages > 0)
	{
		BlockNumber ifirst = InvalidBlockNumber;
		Buffer		ib = InvalidBuffer;
		Page		ip = NULL;
		GenericXLogState *istate = NULL;

		for (j = 0; j < npages; j++)
		{
			int			flen = pgfirstlen[j];
			Size		need = MAXALIGN(offsetof(WeaveDictIndexEntry, term) + flen);
			char	   *dst;
			WeaveDictIndexEntry *ie;

			if (ib == InvalidBuffer ||
				((PageHeader) ip)->pd_lower + need >
				BLCKSZ - sizeof(WeavePageOpaqueData))
			{
				Buffer		nextbuf = weave_new_buffer(index);
				BlockNumber nextblk = BufferGetBlockNumber(nextbuf);

				if (ib != InvalidBuffer)
				{
					WeavePageGetOpaque(ip)->nextblk = nextblk;
					GenericXLogFinish(istate);
					UnlockReleaseBuffer(ib);
				}
				else
					ifirst = nextblk;
				ib = nextbuf;
				istate = GenericXLogStart(index);
				ip = GenericXLogRegisterBuffer(istate, ib, GENERIC_XLOG_FULL_IMAGE);
				weave_init_page(ip, WEAVE_DICTINDEX);
			}
			dst = (char *) ip + ((PageHeader) ip)->pd_lower;
			ie = (WeaveDictIndexEntry *) dst;
			ie->blk = pgblk[j];
			ie->termlen = flen;
			memcpy(ie->term, pgfirst[j], flen);
			((PageHeader) ip)->pd_lower += need;
		}
		if (ib != InvalidBuffer)
		{
			GenericXLogFinish(istate);
			UnlockReleaseBuffer(ib);
		}
		*indexstart = ifirst;
	}
	for (j = 0; j < npages; j++)
		pfree(pgfirst[j]);
	if (pgblk)
		pfree(pgblk);
	if (pgfirst)
		pfree(pgfirst);
	if (pgfirstlen)
		pfree(pgfirstlen);

	return first;
}

/*
 * Iterator over an in-memory bs->terms[] (the segment-flush path): postings[]/
 * offsets[] carry the firstposting/firstoffset for each term.
 */
typedef struct DictArrayIter
{
	WeaveBuildState *bs;
	BlockNumber *postings;
	uint32	   *offsets;
	int			i;
} DictArrayIter;

static bool
dict_array_next(void *st, DictRec *r)
{
	DictArrayIter *it = (DictArrayIter *) st;
	BuildTerm  *bt;

	if (it->i >= it->bs->nterms)
		return false;
	bt = &it->bs->terms[it->i];
	r->term = bt->term;
	r->len = bt->len;
	r->df = bt->nposts;
	r->max_tf = bt->max_tf;
	r->firstposting = it->postings[it->i];
	r->firstoffset = it->offsets[it->i];
	it->i++;
	return true;
}

/* Thin wrapper: write a dictionary from an in-memory bs->terms[] array. */
static BlockNumber
weave_write_dictionary(Relation index, WeaveBuildState *bs,
					  BlockNumber *postings, uint32 *offsets,
					  BlockNumber *indexstart)
{
	DictArrayIter it;

	it.bs = bs;
	it.postings = postings;
	it.offsets = offsets;
	it.i = 0;
	return weave_write_dictionary_iter(index, dict_array_next, &it, indexstart);
}

/*
 * Iterator over an in-memory bs->terms[] yielding only term bytes (the trigram
 * writer needs term/len + the running ordinal; not postings/offsets).
 */
typedef struct DictTermArrayIter
{
	WeaveBuildState *bs;
	int			i;
} DictTermArrayIter;

static bool
dict_term_array_next(void *st, DictRec *r)
{
	DictTermArrayIter *it = (DictTermArrayIter *) st;
	BuildTerm  *bt;

	if (it->i >= it->bs->nterms)
		return false;
	bt = &it->bs->terms[it->i];
	r->term = bt->term;
	r->len = bt->len;
	r->df = bt->nposts;
	r->max_tf = bt->max_tf;
	r->firstposting = InvalidBlockNumber;
	r->firstoffset = 0;
	it->i++;
	return true;
}

/* forward decl: trigram index writer (pg_weave_trgm_index.c, included below) */
static BlockNumber weave_write_trigrams(Relation index, WeaveBuildState *bs);
static BlockNumber weave_write_trigrams_iter(Relation index, DictNextFn next,
											void *nstate);
/* forward decls: blob read/write live in pg_weave_trgm_index.c (included below) */
static BlockNumber weave_write_blob(Relation index, const uint8 *data, Size len);
static uint8 *weave_read_blob(Relation index, BlockNumber blk, Size len);

/*
 * Write one immutable segment (dictionary + postings + trigram index) from a
 * populated build state, filling *seg.  The build state's terms must already
 * be sorted.  livedocs starts empty (no tombstones); segments share the global
 * docid space via heap TIDs.
 */
static void
weave_write_segment(Relation index, WeaveBuildState *bs, WeaveSegMeta *seg)
{
	BlockNumber *postings;
	uint32	   *offsets;
	WeavePostWriter pw;
	DoclenCollector dc;
	int			i;

	postings = (BlockNumber *) palloc(Max(bs->nterms, 1) * sizeof(BlockNumber));	/* alloc-ok: bs->nterms is a single build/pending segment, bounded by maintenance_work_mem (the merge path spills to disk instead) */
	offsets = (uint32 *) palloc(Max(bs->nterms, 1) * sizeof(uint32));	/* alloc-ok: see postings[] above */
	doclen_collector_init(&dc, CurrentMemoryContext, (long) bs->ndocs);
	pw_begin(&pw, index);
	pw.no_doclen_col = bs->want_sidecar;	/* v4: doclen -> sidecar; off = inline */
	for (i = 0; i < bs->nterms; i++)
	{
		BuildTerm  *bt = &bs->terms[i];
		int			p;

		/* per-term: safe to cancel here (GenericXLog works on a page copy, so a
		 * throw mid-write leaves on-disk pages untouched; unwind releases the
		 * buffer lock and leaks at most the new segment's pages) */
		CHECK_FOR_INTERRUPTS();
		/* collect each doc's length for the sidecar (idempotent per docid) */
		for (p = 0; p < bt->nposts; p++)
			doclen_collector_add(&dc, weave_tid_to_docid(&bt->tids[p]), bt->doclens[p]);
		weave_write_postings(&pw, bt, &postings[i], &offsets[i]);
	}
	pw_finish(&pw);

	MemSet(seg, 0, sizeof(WeaveSegMeta));
	seg->dictstart = weave_write_dictionary(index, bs, postings, offsets, &seg->dictindexstart);
	seg->trgmstart = bs->want_trigrams ? weave_write_trigrams(index, bs)
		: InvalidBlockNumber;	/* trigrams opt-in (WITH (trigrams=on)); see weave_index_wants_trigrams */
	seg->doclenstart = bs->want_sidecar ? weave_write_doclen_sidecar(index, &dc) : InvalidBlockNumber;
	seg->livedocs = InvalidBlockNumber;
	seg->ndocs = bs->ndocs;
	seg->sumdoclen = bs->sumdoclen;
	seg->nterms = bs->nterms;
	seg->ndeleted = 0;
	seg->livedocslen = 0;
	doclen_collector_free(&dc);
	pfree(postings);
	pfree(offsets);
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
static bool
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

/* forward decl: bounded merge that reduces the live segment count */
static void weave_merge_segments(Relation index);
static bool weave_merge_all(Relation index, bool try_parallel);
/* maintenance serialization (defined in the vacuum/merge section below) */
static inline void weave_maintenance_lock(Relation index);
static inline bool weave_maintenance_lock_conditional(Relation index);
static inline void weave_maintenance_unlock(Relation index);

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
static void
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


/* ---- bounded, streaming k-way segment merge --------------------------------
 *
 * weave_read_segment_into (above) decodes an ENTIRE segment's postings into the
 * build state; merging K segments that way buffers all their live postings at
 * once, so the final full compaction of a large index holds the whole index in
 * RAM and can OOM the server.  The streaming merge below bounds peak memory to
 * ONE term's merged postings at a time (plus the per-segment dictionary
 * metadata, which is small relative to the postings).
 *
 * Every segment's dictionary is written term-sorted (weave_write_dictionary
 * emits bs->terms in cmp_buildterm order), so a merge is a k-way merge of
 * sorted streams: read each source's dictionary METADATA (term/df/firstposting/
 * firstoffset, no posting bodies), then sweep the distinct terms in sorted
 * order; for each term decode ONLY that term's postings from the segments that
 * carry it, tombstone-filter + merge into one single-term build state, write
 * that term's postings immediately, record its dict metadata, and free the
 * term's postings before advancing.  The rare huge stopword is covered by the
 * WEAVE_ALLOC_MAYBE_HUGE path in add_posting/weave_write_postings.
 */
typedef struct MergeDictTerm
{
	char	   *term;			/* term bytes (points INTO the pinned dict page) */
	uint32		termlen;
	uint32		df;
	BlockNumber firstposting;
	uint32		firstoffset;
} MergeDictTerm;

/*
 * A merge source is a forward cursor over one segment's term-sorted dictionary.
 * It keeps only the CURRENT dict page pinned (~8KB) and exposes the current
 * term; the k-way merge peeks the current term of each source and advances the
 * ones carrying the smallest.  This keeps the merge's per-source footprint O(1)
 * (one page) instead of loading the segment's entire vocabulary into memory --
 * critical for a large, high-vocabulary corpus where the sum of all input
 * dictionaries would otherwise be many GB, unbounded by maintenance_work_mem.
 *
 * `cur` is valid (points into `curbuf`'s page) iff `valid`; the term bytes it
 * references stay live until the next merge_source_advance() on this source, so
 * the merge must consume/copy them (add_posting does) before advancing.
 */
typedef struct MergeSource
{
	Relation	index;
	BlockNumber nextblk;			/* next dict page to read, or Invalid */
	MergeDictTerm *page;			/* decoded entries of the current page (copied) */
	char	   *pagebytes;			/* backing store for this page's term bytes */
	int			npage;				/* entries in page[] */
	int			pcur;				/* index of current entry within page[] */
	int			pagecap;			/* capacity of page[]/pagebytes reuse */
	Size		bytescap;
	MergeDictTerm cur;				/* current term (points into pagebytes) */
	bool		valid;				/* cur holds a term (source not exhausted) */
	MemoryContext ctx;
	uint8	   *tombbuf;			/* tombstone bitmap blob, or NULL */
	sm_t		tomb;
	bool		hastomb;
	sm_cursor_cached_t tombcache;
	WeaveDoclens doclens;			/* v4 source doclen sidecar (empty for v3) */
	bool		has_doclen_col;		/* v3 source: doclen inline in postings */
} MergeSource;

/*
 * Load the next dict page (src->nextblk) into src->page[] / src->pagebytes,
 * copying each entry's metadata and term bytes so the page buffer can be
 * released immediately (no page pin held across posting reads).  Skips empty
 * pages.  Sets src->npage = 0 and returns when the dictionary is exhausted.
 * page[]/pagebytes are sized by ONE page's contents (bounded by BLCKSZ), reused
 * across pages -- so a source's footprint stays O(one page), not O(vocabulary).
 */
static void
merge_source_load_page(MergeSource *src)
{
	MemoryContext old = MemoryContextSwitchTo(src->ctx);

	src->npage = 0;
	src->pcur = 0;

	while (src->npage == 0 && src->nextblk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		int			n;
		Size		used;

		CHECK_FOR_INTERRUPTS();		/* between pages, no lock held across yields */
		buffer = ReadBuffer(src->index, src->nextblk);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		/* count entries + term bytes on this page (bounded by BLCKSZ) */
		n = 0;
		used = 0;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;

			n++;
			used += de->termlen;
			ptr += MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
		}

		if (n > src->pagecap)
		{
			src->pagecap = Max(n, src->pagecap ? src->pagecap * 2 : 256);
			src->page = src->page
				? repalloc(src->page, src->pagecap * sizeof(MergeDictTerm))
				: palloc(src->pagecap * sizeof(MergeDictTerm));
		}
		if (used > src->bytescap || (n > 0 && src->pagebytes == NULL))
		{
			/* floor at BLCKSZ so pagebytes is non-NULL for any n>0 page, even the
			 * degenerate all-zero-length-term case (avoids memcpy(NULL,...,0)) */
			src->bytescap = Max(Max(used, (Size) 1), src->bytescap ? src->bytescap * 2 : (Size) BLCKSZ);
			src->pagebytes = src->pagebytes
				? repalloc(src->pagebytes, src->bytescap)
				: palloc(src->bytescap);
		}

		ptr = (char *) PageGetContents(page);
		used = 0;
		n = 0;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			MergeDictTerm *mt = &src->page[n++];

			mt->termlen = de->termlen;
			mt->df = de->df;
			mt->firstposting = de->firstposting;
			mt->firstoffset = de->firstoffset;
			mt->term = src->pagebytes + used;
			memcpy(mt->term, de->term, de->termlen);
			used += de->termlen;
			ptr += MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
		}
		src->npage = n;
		src->nextblk = next;
		UnlockReleaseBuffer(buffer);
	}

	if (src->npage > 0)
	{
		src->cur = src->page[0];
		src->valid = true;
	}
	else
		src->valid = false;

	MemoryContextSwitchTo(old);
}

/* Advance the cursor to the next term, loading the next page as needed. */
static void
merge_source_advance(MergeSource *src)
{
	src->pcur++;
	if (src->pcur < src->npage)
		src->cur = src->page[src->pcur];
	else
		merge_source_load_page(src);	/* refills page[], sets cur/valid */
}

/*
 * Open a merge source as a forward, page-at-a-time cursor over one segment's
 * term-sorted dictionary metadata (no posting bodies).  Positions it on the
 * first term.  Only one dict page's worth of metadata is resident at a time.
 */
static void
merge_source_open(Relation index, const WeaveSegMeta *seg, MergeSource *src,
				  MemoryContext ctx)
{
	MemoryContext old = MemoryContextSwitchTo(ctx);

	src->index = index;
	src->ctx = ctx;
	src->nextblk = seg->dictstart;
	src->page = NULL;
	src->pagebytes = NULL;
	src->npage = 0;
	src->pcur = 0;
	src->pagecap = 0;
	src->bytescap = 0;
	src->valid = false;
	src->tombbuf = NULL;
	src->hastomb = false;
	memset(&src->tombcache, 0, sizeof(src->tombcache));

	/* v4 source: doclen lives in the segment sidecar, not the postings.  Load it
	 * once so the merge can re-attach each posting's exact length.  A v3 source
	 * (doclenstart Invalid) keeps has_doclen_col=true and reads it inline. */
	src->has_doclen_col = (seg->doclenstart == InvalidBlockNumber);
	weave_doclens_load(index, seg->doclenstart, &src->doclens);

	if (seg->livedocs != InvalidBlockNumber && seg->livedocslen > 0)
	{
		src->tombbuf = weave_read_blob(index, seg->livedocs, seg->livedocslen);
		sm_open(&src->tomb, (uint8_t *) src->tombbuf, seg->livedocslen);
		src->hastomb = true;
	}

	merge_source_load_page(src);	/* position on the first term */
	MemoryContextSwitchTo(old);

}

/* Order two term keys the same way cmp_buildterm orders BuildTerms. */
static int
merge_cmp_term(const char *a, uint32 alen, const char *b, uint32 blen)
{
	uint32		min = Min(alen, blen);
	int			c = memcmp(a, b, min);

	if (c != 0)
		return c;
	return (int) alen - (int) blen;
}

/*
 * Dictionary-metadata spill for the streaming merge.  As the k-way merge
 * produces each output term (in sorted order) we append its dict record --
 * term bytes + df/max_tf/firstposting/firstoffset -- to a temp BufFile instead
 * of an in-memory array.  The whole merged VOCABULARY is thus never resident;
 * only one record at a time is, both when writing the spill and when streaming
 * it back into weave_write_dictionary_iter and the trigram writer.  This is what
 * bounds the merge's memory on a huge, high-vocabulary corpus.
 *
 * Record layout: [int termlen][uint32 df][uint32 max_tf][BlockNumber fp]
 *                [uint32 fo][termlen term bytes].
 */
typedef struct DictSpill
{
	BufFile    *bf;
	char	   *tbuf;			/* reusable read buffer for term bytes */
	int			tcap;
	DictRec		cur;			/* last record read back (term points into tbuf) */
	int			ordinal;		/* ordinal of cur among all spilled records */
} DictSpill;

static void
dict_spill_begin(DictSpill *sp)
{
	sp->bf = BufFileCreateTemp(false);
	sp->tbuf = NULL;
	sp->tcap = 0;
	sp->ordinal = -1;
}

static void
dict_spill_write(DictSpill *sp, const DictRec *r)
{
	BufFileWrite(sp->bf, (void *) &r->len, sizeof(int));
	BufFileWrite(sp->bf, (void *) &r->df, sizeof(uint32));
	BufFileWrite(sp->bf, (void *) &r->max_tf, sizeof(uint32));
	BufFileWrite(sp->bf, (void *) &r->firstposting, sizeof(BlockNumber));
	BufFileWrite(sp->bf, (void *) &r->firstoffset, sizeof(uint32));
	if (r->len > 0)
		BufFileWrite(sp->bf, (void *) r->term, r->len);
}

/* Rewind to the start for a (re)read pass. */
static void
dict_spill_rewind(DictSpill *sp)
{
	if (BufFileSeek(sp->bf, 0, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not rewind pg_weave merge dictionary spill file")));
	sp->ordinal = -1;
}

/* DictNextFn over the spill: reads the next record into sp->cur. */
static bool
dict_spill_next(void *st, DictRec *out)
{
	DictSpill  *sp = (DictSpill *) st;
	size_t		n;

	n = BufFileReadMaybeEOF(sp->bf, &sp->cur.len, sizeof(int), true);
	if (n == 0)
		return false;			/* clean EOF */
	BufFileReadExact(sp->bf, &sp->cur.df, sizeof(uint32));
	BufFileReadExact(sp->bf, &sp->cur.max_tf, sizeof(uint32));
	BufFileReadExact(sp->bf, &sp->cur.firstposting, sizeof(BlockNumber));
	BufFileReadExact(sp->bf, &sp->cur.firstoffset, sizeof(uint32));
	if (sp->cur.len > sp->tcap)
	{
		sp->tcap = Max(sp->cur.len, sp->tcap ? sp->tcap * 2 : 256);
		sp->tbuf = sp->tbuf ? repalloc(sp->tbuf, sp->tcap) : palloc(sp->tcap);
	}
	if (sp->cur.len > 0)
		BufFileReadExact(sp->bf, sp->tbuf, sp->cur.len);
	sp->cur.term = sp->tbuf;
	sp->ordinal++;
	if (out != &sp->cur)
		*out = sp->cur;
	return true;
}

static void
dict_spill_end(DictSpill *sp)
{
	BufFileClose(sp->bf);
	if (sp->tbuf)
		pfree(sp->tbuf);
}


/*
 * Streaming k-way merge of the `chosen` segments into ONE new segment, bounded
 * to one term's postings at a time.  Writes the new segment's pages and fills
 * *seg (dictstart/dictindexstart) plus the corpus totals in bs (ndocs,
 * sumdoclen).  All allocations live in bs->ctx, which the caller owns.
 */
static void
weave_merge_segments_streaming(Relation index, const WeaveSegMeta *chosen,
							  uint32 nsel, WeaveBuildState *bs, WeaveSegMeta *seg)
{
	MergeSource *srcv;
	WeavePostWriter pw;
	DoclenCollector mergedc;		/* v4: docid->byte for the merged output segment */
	DictSpill	spill;			/* per-output-term dict metadata, spilled to disk */
	uint32		nout = 0;
	uint32		i;
	MemoryContext old = MemoryContextSwitchTo(bs->ctx);

	srcv = (MergeSource *) palloc0(nsel * sizeof(MergeSource));
	for (i = 0; i < nsel; i++)
	{
		bs->sumdoclen += chosen[i].sumdoclen;
		bs->ndocs += chosen[i].ndocs - chosen[i].ndeleted;
		merge_source_open(index, &chosen[i], &srcv[i], bs->ctx);
	}

	dict_spill_begin(&spill);

	pw_begin(&pw, index);
	pw.no_doclen_col = bs->want_sidecar;	/* v4 output: doclen -> sidecar; off = inline */
	doclen_collector_init(&mergedc, CurrentMemoryContext, 65536);

	for (;;)
	{
		const char *smterm = NULL;
		uint32		smlen = 0;
		MemoryContext termctx;
		MemoryContext told;
		WeaveBuildState tbs;
		BuildTerm  *bt;
		BuildTerm  *mbt;		/* the one term this pass gathers */
		BlockNumber fb;
		uint32		fo;

		CHECK_FOR_INTERRUPTS();		/* per output term; no lock/window held */

		/* smallest current term across all live cursors */
		for (i = 0; i < nsel; i++)
		{
			MergeSource *s = &srcv[i];

			if (!s->valid)
				continue;
			if (smterm == NULL ||
				merge_cmp_term(s->cur.term, s->cur.termlen,
							   smterm, smlen) < 0)
			{
				smterm = s->cur.term;
				smlen = s->cur.termlen;
			}
		}
		if (smterm == NULL)
			break;				/* all cursors exhausted */

		/* gather this term's postings from every segment that carries it into a
		 * fresh single-term build state in its own child context */
		termctx = AllocSetContextCreate(bs->ctx, "weave merge term",
										ALLOCSET_DEFAULT_SIZES);
		told = MemoryContextSwitchTo(termctx);

		/*
		 * Copy the smallest term into termctx-owned memory: smterm points into
		 * one source's page, which merge_source_advance() below overwrites as
		 * cursors advance, so we must not alias it during the gather loop.  Sized
		 * to the real term length (no fixed cap), freed with termctx each pass.
		 */
		{
			char	   *smcopy = (char *) palloc(Max(smlen, 1u));

			memcpy(smcopy, smterm, smlen);
			smterm = smcopy;
		}

		tbs.ctx = termctx;
		tbs.want_positions = bs->want_positions;
		tbs.want_trigrams = bs->want_trigrams;
		tbs.terms = NULL;
		tbs.nterms = 0;
		tbs.maxterms = 0;
		tbs.ndocs = 0;
		tbs.sumdoclen = 0;

		/*
		 * L15: the merge gathers exactly one term per pass, so it needs no
		 * term hash at all.  It used to create a fresh dynahash here and probe
		 * it once per posting of the term -- 19% of total build time spent
		 * looking up the one entry the table could ever hold
		 * (bench/RESULTS_BUILD_PROFILE.md).  The BuildTerm is created lazily on
		 * the first surviving posting so a fully tombstoned term still yields
		 * tbs.nterms == 0 below.
		 */
		mbt = NULL;

		for (i = 0; i < nsel; i++)
		{
			MergeSource *s = &srcv[i];
			MergeDictTerm *mt;
			WeavePosting *post;
			uint32	   *posarena = NULL;
			int			np,
						k;

			if (!s->valid)
				continue;
			mt = &s->cur;
			if (merge_cmp_term(mt->term, mt->termlen, smterm, smlen) != 0)
				continue;		/* this segment lacks the smallest term */

			np = weave_decode_term(index, mt->firstposting, mt->firstoffset,
								  mt->df, &post, NULL, bs->want_positions,
								  &posarena, false, s->has_doclen_col);
			for (k = 0; k < np; k++)
			{
				uint32		doclen = post[k].doclen;

				if (s->hastomb &&
					sm_contains_cached(&s->tomb,
									   weave_tid_to_docid(&post[k].tid),
									   &s->tombcache))
					continue;	/* tombstoned: physically drop */
				/* v4 source: post[k].doclen is 0 (no inline column); recover the
				 * exact length from the source's sidecar so the merged segment
				 * carries correct doclen (and re-quantizes it into its own sidecar). */
				if (!s->has_doclen_col)
					doclen = weave_doclen_lookup(&s->doclens,
											   weave_tid_to_docid(&post[k].tid));
				/* feed the merged segment's doclen sidecar (idempotent per docid) --
				 * WITHOUT this the merged sidecar is empty, doclenstart comes back
				 * Invalid, and the merged 2-column postings are then mis-read as
				 * inline (garbage doclen, WAND pruning defeated). */
				doclen_collector_add(&mergedc, weave_tid_to_docid(&post[k].tid), doclen);
				if (mbt == NULL)
					mbt = build_term_new(&tbs, smterm, smlen, -1);
				build_term_append(&tbs, mbt,
								  &post[k].tid, post[k].tf, doclen,
								  post[k].pos, post[k].pos ? (int) post[k].tf : 0);
			}
			pfree(post);
			if (posarena)
				pfree(posarena);
			merge_source_advance(s);	/* advance the cursors that matched this term */
		}
		MemoryContextSwitchTo(told);

		/* the term may have been entirely tombstoned away */
		if (tbs.nterms == 0)
		{
			MemoryContextDelete(termctx);
			continue;
		}
		Assert(tbs.nterms == 1);
		bt = &tbs.terms[0];

		/* write this term's postings now (sets bt->max_tf), then spill only its
		 * small dictionary metadata to disk and free the postings arena */
		weave_write_postings(&pw, bt, &fb, &fo);

		{
			DictRec		rec;

			rec.term = bt->term;
			rec.len = bt->len;
			rec.df = bt->nposts;
			rec.max_tf = bt->max_tf;
			rec.firstposting = fb;
			rec.firstoffset = fo;
			dict_spill_write(&spill, &rec);
		}
		nout++;

		MemoryContextDelete(termctx);	/* frees this term's postings */
	}

	pw_finish(&pw);

	/*
	 * Emit the dictionary and trigram index by streaming the spilled per-term
	 * metadata back from disk -- one record resident at a time, so neither the
	 * merged vocabulary's dict metadata nor the term bytes are ever fully in
	 * memory.  weave_write_trigrams_iter still accumulates its trigram->ordinal
	 * map (bounded by the vocabulary's trigram content, huge-safe), but no
	 * longer needs a resident bs->terms[] array.
	 */
	bs->nterms = (int) nout;

	MemSet(seg, 0, sizeof(WeaveSegMeta));
	seg->doclenstart = bs->want_sidecar ? weave_write_doclen_sidecar(index, &mergedc) : InvalidBlockNumber;
	doclen_collector_free(&mergedc);
	dict_spill_rewind(&spill);
	seg->dictstart = weave_write_dictionary_iter(index, dict_spill_next, &spill,
												&seg->dictindexstart);
	if (bs->want_trigrams)
	{
		dict_spill_rewind(&spill);
		seg->trgmstart = weave_write_trigrams_iter(index, dict_spill_next, &spill);
		/* both passes must consume exactly the nout spilled records; a mismatch would
		 * mean the trigram->term-ordinal mapping diverged from the dict write order */
		Assert(spill.ordinal + 1 == (int) nout);
	}
	else
		seg->trgmstart = InvalidBlockNumber;	/* trigrams opt-in; see weave_index_wants_trigrams */
	seg->livedocs = InvalidBlockNumber;
	seg->ndocs = bs->ndocs;
	seg->sumdoclen = bs->sumdoclen;
	seg->nterms = bs->nterms;
	seg->ndeleted = 0;
	seg->livedocslen = 0;

	for (i = 0; i < nsel; i++)
		weave_doclens_free(&srcv[i].doclens);

	dict_spill_end(&spill);
	MemoryContextSwitchTo(old);
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
static void
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
	if (!(op->flags & WEAVE_FREED))
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
static void
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
static void
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
}

/*
 * Size-tiered segment merge (a Lucene TieredMergePolicy in miniature).
 *
 * Rather than merging the whole directory into one segment on every trigger
 * (O(index) write amplification under steady inserts), we merge only a RUN of
 * similarly-sized segments at a time: sort the live segments by size (live doc
 * count) and, if the smallest ones fall within a size factor of each other,
 * merge just those into one new segment.  Small flushes coalesce cheaply while
 * large segments are rarely rewritten.  We loop until no tier qualifies and the
 * count is within budget, so query cost stays O(nsegments) small.  Tombstoned
 * docs are dropped as segments are read.  Called after a flush, from build, and
 * from VACUUM.
 */
#define WEAVE_MERGE_THRESHOLD 8		/* keep the live segment count at or below this */

/*
 * Leveled (HanoiDB/LSM-style) merge parameters.  A segment's LEVEL is derived
 * from its live size: level = floor(log_FANOUT(size)) (size in live docs).  Each
 * level holds up to WEAVE_MERGE_FANOUT runs; when a level fills, its runs are
 * merged into one that lands in the next level.  This bounds the fan-in of any
 * single merge to ~FANOUT segments -- unlike the old size-tiered selector, which
 * merged an entire same-size run at once (all ~N segments when a build produced
 * many near-equal segments), i.e. one giant single-backend pass over the whole
 * index.  Bounded fan-in gives O(N log N) total merge work with bounded write
 * amplification and small, discrete, observable merges.  Level is COMPUTED from
 * size (not stored), so there is no on-disk format change.
 */
#define WEAVE_MERGE_FANOUT 8			/* runs per level before it compacts + promotes */
#define WEAVE_MAX_LEVELS 24			/* FANOUT^24 = 8^24 docs -- far beyond any real corpus */

/* Derive a segment's level from its live doc count (level 0 = smallest). */
static int
weave_seg_level(double livesize)
{
	double		s = livesize < 1.0 ? 1.0 : livesize;
	int			level = 0;
	double		cap = (double) WEAVE_MERGE_FANOUT;

	/* level L covers sizes [FANOUT^L, FANOUT^(L+1)); clamp to WEAVE_MAX_LEVELS-1 */
	while (s >= cap && level < WEAVE_MAX_LEVELS - 1)
	{
		cap *= (double) WEAVE_MERGE_FANOUT;
		level++;
	}
	return level;
}

/* segment (index,size) pair for sorting merge candidates by size */
typedef struct MergeCand
{
	uint32		idx;
	double		size;
}			MergeCand;

static int
cmp_mergecand(const void *a, const void *b)
{
	double		sa = ((const MergeCand *) a)->size;
	double		sb = ((const MergeCand *) b)->size;

	return (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
}

/*
 * Merge one selected set of segments (by directory index) into a single new
 * segment, rewrite the metapage directory to drop the merged ones (preserving
 * the order of the rest) and append the new segment, then recycle the merged
 * segments' pages.  Returns true on success, false if the directory changed
 * underneath (caller stops).
 */
/*
 * Merge a specific set of segment descriptors (by CONTENT, not directory index)
 * into one new segment, writing its pages but NOT touching the metapage
 * directory.  Returns the new descriptor in *out.  Safe to run concurrently
 * with other callers merging DISJOINT descriptor sets: page appends are
 * serialized by the relation extension lock (in weave_build_flush_segment's
 * peer path we lock explicitly; here weave_write_segment appends under the same
 * discipline when IsInParallelMode()).  The caller (leader) removes the
 * consumed descriptors and installs *out in a single metapage update.
 */
static void
weave_merge_group_to_seg(Relation index, const WeaveSegMeta *group, uint32 ngroup,
						WeaveSegMeta *out)
{
	WeaveBuildState bs;

	bs.ctx = AllocSetContextCreate(CurrentMemoryContext, "weave merge group",
								   ALLOCSET_DEFAULT_SIZES);
	bs.want_positions = weave_index_wants_positions(index);
	bs.want_trigrams = weave_index_wants_trigrams(index);
	bs.want_sidecar = weave_index_wants_doclen_sidecar(index);
	bs.terms = NULL;
	bs.nterms = 0;
	bs.maxterms = 0;
	bs.ndocs = 0;
	bs.sumdoclen = 0;

	/* Streaming k-way merge (bounded memory); page appends are serialized
	 * per-page inside weave_new_buffer under the extension lock. */
	weave_merge_segments_streaming(index, group, ngroup, &bs, out);
	out->ndocs = bs.ndocs;
	out->sumdoclen = bs.sumdoclen;

	MemoryContextDelete(bs.ctx);
}

static bool
weave_merge_selected(Relation index, const uint32 *sel, uint32 nsel)
{
	WeaveMetaPageData meta;
	WeaveBuildState bs;
	WeaveSegMeta newseg;
	WeaveSegMeta chosen[WEAVE_MAX_SEGMENTS];
	uint32		i;
	double		indocs = 0;
	instr_time	t0;

	INSTR_TIME_SET_CURRENT(t0);

	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
	}
	for (i = 0; i < nsel; i++)
	{
		if (sel[i] >= meta.nsegments)
			return false;		/* directory changed under us */
		chosen[i] = meta.segs[sel[i]];
		indocs += chosen[i].ndocs - chosen[i].ndeleted;
	}

	elog(DEBUG1, "pg_weave merge: index \"%s\": merging %u of %u segments (%.0f live docs) into one",
		 RelationGetRelationName(index), nsel, meta.nsegments, indocs);

	bs.ctx = AllocSetContextCreate(CurrentMemoryContext, "weave merge segs",
								   ALLOCSET_DEFAULT_SIZES);
	bs.want_positions = weave_index_wants_positions(index);
	bs.want_trigrams = weave_index_wants_trigrams(index);
	bs.want_sidecar = weave_index_wants_doclen_sidecar(index);
	bs.terms = NULL;
	bs.nterms = 0;
	bs.maxterms = 0;
	bs.ndocs = 0;
	bs.sumdoclen = 0;

	/* Streaming k-way merge: bounded to one term's postings at a time, so a
	 * full compaction of a large index does not buffer the whole index in RAM
	 * (see weave_merge_segments_streaming). */
	weave_merge_segments_streaming(index, chosen, nsel, &bs, &newseg);
	newseg.ndocs = bs.ndocs;
	newseg.sumdoclen = bs.sumdoclen;

	{
		instr_time	t1;

		INSTR_TIME_SET_CURRENT(t1);
		INSTR_TIME_SUBTRACT(t1, t0);
		elog(DEBUG1, "pg_weave merge: index \"%s\": wrote merged segment (%d terms, %.0f docs) in %.1f s",
			 RelationGetRelationName(index), bs.nterms, bs.ndocs,
			 INSTR_TIME_GET_DOUBLE(t1));
	}

	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
		GenericXLogState *state;
		Page		mp;
		WeaveMetaPageData *m;
		bool		allfound = true;
		bool		consumed[WEAVE_MAX_SEGMENTS];

		LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		mp = GenericXLogRegisterBuffer(state, mb, 0);
		weave_meta_upcast_page(mp);	/* v3 -> v4 before struct write */
		m = WeavePageGetMeta(mp);

		/*
		 * Re-locate each chosen input segment by CONTENT (not by its old
		 * positional index) in the current directory.  A concurrent flush during
		 * this (possibly long) merge appends new segments and changes nsegments
		 * and shifts positions -- but our chosen inputs are immutable until we
		 * free them, so they are still present.  Matching by content lets the
		 * merge COMMIT alongside newly-flushed segments instead of aborting on
		 * any directory change (which, on a large corpus where each merge takes
		 * minutes and the scan keeps flushing, caused the merge to discard its
		 * output and re-read forever -- never converging).  We only abort if a
		 * chosen input is genuinely gone (another merge already consumed it).
		 */
		{
			uint32		j;

			memset(consumed, 0, sizeof(bool) * m->nsegments);
			for (i = 0; i < nsel; i++)
			{
				bool		found = false;

				for (j = 0; j < m->nsegments; j++)
				{
					if (!consumed[j] &&
						memcmp(&m->segs[j], &chosen[i], sizeof(WeaveSegMeta)) == 0)
					{
						consumed[j] = true;	/* claim this slot for this input */
						found = true;
						break;
					}
				}
				if (!found)
				{
					allfound = false;
					break;
				}
			}
		}

		if (allfound)
		{
			WeaveSegMeta kept[WEAVE_MAX_SEGMENTS];
			uint32		nkept = 0;
			uint32		j;

			/* keep every segment NOT consumed as an input, preserving order
			 * (this retains any segment a concurrent flush appended) */
			for (j = 0; j < m->nsegments; j++)
				if (!consumed[j])
					kept[nkept++] = m->segs[j];
			kept[nkept++] = newseg;	/* append the merged segment */
			memcpy(m->segs, kept, nkept * sizeof(WeaveSegMeta));
			m->nsegments = nkept;
			m->generation++;	/* directory changed: invalidate concurrent scan snapshots */
			/* corpus totals unchanged (same docs, tombstones already excluded) */
			GenericXLogFinish(state);
			UnlockReleaseBuffer(mb);
			for (i = 0; i < nsel; i++)
				weave_free_segment(index, &chosen[i]);
			IndexFreeSpaceMapVacuum(index);
			MemoryContextDelete(bs.ctx);
			return true;
		}
		else
		{
			/* an input was already consumed by another merge; abandon (the new
			 * segment leaks until the next merge/REINDEX -- rare) */
			GenericXLogAbort(state);
			UnlockReleaseBuffer(mb);
			MemoryContextDelete(bs.ctx);
			return false;
		}
	}
}

/*
 * Merge ALL live segments into a single segment (explicit full compaction).
 * Used by weave_merge() so an on-demand call actually produces an optimal,
 * single-segment index (the tiered weave_merge_segments only coalesces
 * same-size tiers and may deliberately leave several segments).  Merges in
 * bounded batches (WEAVE_MAX_SEGMENTS worth of selection at a time is fine since
 * a build/merge never exceeds the cap) and loops until one segment remains.
 * Returns true if it changed anything.
 */
/* ---- parallel merge (compact many segments into few, in parallel) ----
 *
 * The leader partitions the live segments into W disjoint groups; each worker
 * merges ONE group into one new segment (weave_merge_group_to_seg -- writes
 * pages only, no directory touch) and reports the new descriptor via DSM.  The
 * leader then performs a SINGLE metapage update: drop all the consumed source
 * descriptors and install the W new ones.  This confines the expensive
 * decode/re-encode to parallel workers and keeps the directory swap serial and
 * atomic (no concurrent-swap race).  Result: W segments; caller may run a
 * final (cheap, W-way) pass if it wants exactly one.
 *
 * Future work: Level-2 could recurse the parallel merge (W -> W/2 -> ... -> 1) so
 * even the final combine parallelizes; deferred -- one parallel pass already
 * removes the dominant per-segment decode cost from the serial path.
 */
#define PARALLEL_KEY_BM25_MERGE		UINT64CONST(0xB250000000000010)

typedef struct WeaveMergeShared
{
	Oid			heaprelid;
	Oid			indexrelid;
	int			ngroups;		/* number of worker groups */
	int			nsrc;			/* total source segments */
	slock_t		mutex;
	/* filled by workers: the merged-segment descriptor per group */
	WeaveSegMeta outseg[WEAVE_MAX_SEGMENTS];
	bool		outvalid[WEAVE_MAX_SEGMENTS];
	/* group layout: src[groupoff[g] .. groupoff[g+1]) are group g's sources */
	int			groupoff[WEAVE_MAX_SEGMENTS + 1];
	WeaveSegMeta src[WEAVE_MAX_SEGMENTS];
}			WeaveMergeShared;

static void weave_merge_one_group(Relation index, WeaveMergeShared *ms, int g);
static void weave_merge_segments(Relation index);	/* size-tiered LSM merge (defined below) */

PGDLLEXPORT void weave_parallel_merge_main(dsm_segment *seg, shm_toc *toc);

void
weave_parallel_merge_main(dsm_segment *seg, shm_toc *toc)
{
	WeaveMergeShared *ms;
	Relation	heap;
	Relation	index;

	ms = (WeaveMergeShared *) shm_toc_lookup(toc, PARALLEL_KEY_BM25_MERGE, false);
	heap = table_open(ms->heaprelid, AccessShareLock);
	index = index_open(ms->indexrelid, RowExclusiveLock);

	/* worker N handles group (N+1); group 0 is the leader's */
	if (ParallelWorkerNumber + 1 < ms->ngroups)
		weave_merge_one_group(index, ms, ParallelWorkerNumber + 1);

	index_close(index, RowExclusiveLock);
	table_close(heap, AccessShareLock);
}

/* merge group g's sources into one segment, store descriptor in shared state */
static void
weave_merge_one_group(Relation index, WeaveMergeShared *ms, int g)
{
	int			lo = ms->groupoff[g];
	int			hi = ms->groupoff[g + 1];
	WeaveSegMeta out;

	if (hi - lo <= 0)
		return;
	if (hi - lo == 1)
	{
		/* singleton group: nothing to merge, keep the source as-is */
		ms->outseg[g] = ms->src[lo];
		ms->outvalid[g] = false;	/* signals "source kept, no new seg" */
		return;
	}
	weave_merge_group_to_seg(index, &ms->src[lo], (uint32) (hi - lo), &out);
	SpinLockAcquire(&ms->mutex);
	ms->outseg[g] = out;
	ms->outvalid[g] = true;
	SpinLockRelease(&ms->mutex);
}

/*
 * Parallel merge-all: partition live segments into (workers+1) groups, each
 * participant merges its group into a new segment, then the leader installs the
 * results with a single metapage update.  Returns true if it ran (and did the
 * directory swap), false to signal the caller to fall back to serial.
 */
static bool
weave_merge_all_parallel(Relation index, int request)
{
	ParallelContext *pcxt;
	WeaveMergeShared *ms;
	WeaveMetaPageData meta;
	Size		estms;
	int			ngroups;
	int			nsrc;
	int			g,
				i;
	Relation	heap;
	Oid			heaprelid;

	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
	}
	if (meta.nsegments <= 2)
		return false;			/* not worth parallelizing; serial handles it */

	heaprelid = index->rd_index->indrelid;

	EnterParallelMode();
	pcxt = CreateParallelContext("pg_weave", "weave_parallel_merge_main", request);
	estms = BUFFERALIGN(sizeof(WeaveMergeShared));
	shm_toc_estimate_chunk(&pcxt->estimator, estms);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	InitializeParallelDSM(pcxt);

	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return false;
	}

	ms = (WeaveMergeShared *) shm_toc_allocate(pcxt->toc, estms);
	ms->heaprelid = heaprelid;
	ms->indexrelid = RelationGetRelid(index);
	SpinLockInit(&ms->mutex);

	/* collect the live source segments */
	nsrc = 0;
	for (i = 0; i < (int) meta.nsegments; i++)
		if (meta.segs[i].dictstart != InvalidBlockNumber)
			ms->src[nsrc++] = meta.segs[i];
	ms->nsrc = nsrc;

	/* groups = min(participants, nsrc); participant 0 = leader */
	ngroups = request + 1;
	if (ngroups > nsrc)
		ngroups = nsrc;
	ms->ngroups = ngroups;

	/* even contiguous partition of the nsrc sources into ngroups */
	for (g = 0; g <= ngroups; g++)
		ms->groupoff[g] = (int) ((int64) g * nsrc / ngroups);
	for (g = 0; g < ngroups; g++)
		ms->outvalid[g] = false;

	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BM25_MERGE, ms);
	LaunchParallelWorkers(pcxt);

	if (pcxt->nworkers_launched == 0)
	{
		WaitForParallelWorkersToFinish(pcxt);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return false;			/* no workers; serial fallback */
	}

	/* leader merges group 0 itself while workers handle groups 1..n */
	heap = table_open(heaprelid, AccessShareLock);
	weave_merge_one_group(index, ms, 0);
	table_close(heap, AccessShareLock);

	WaitForParallelWorkersToFinish(pcxt);

	/*
	 * Single atomic directory update: drop every consumed source descriptor
	 * (content-match) and install each group's merged descriptor.  Groups that
	 * did not actually merge (singleton) keep their one source, so we simply
	 * don't drop it.
	 */
	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
		GenericXLogState *state;
		Page		mp;
		WeaveMetaPageData *m;
		WeaveSegMeta kept[WEAVE_MAX_SEGMENTS];
		uint32		nkept = 0;
		uint32		j;
		int			k;

		LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		mp = GenericXLogRegisterBuffer(state, mb, 0);
		weave_meta_upcast_page(mp);	/* v3 -> v4 before struct write */
		m = WeavePageGetMeta(mp);

		/* keep any segment that is NOT a consumed source of a merged group */
		for (j = 0; j < m->nsegments; j++)
		{
			bool		consumed = false;

			for (g = 0; g < ngroups && !consumed; g++)
			{
				if (!ms->outvalid[g])
					continue;	/* singleton group merged nothing */
				for (k = ms->groupoff[g]; k < ms->groupoff[g + 1]; k++)
					if (memcmp(&m->segs[j], &ms->src[k], sizeof(WeaveSegMeta)) == 0)
					{
						consumed = true;
						break;
					}
			}
			if (!consumed)
				kept[nkept++] = m->segs[j];
		}
		/* append each merged group's new segment */
		for (g = 0; g < ngroups; g++)
			if (ms->outvalid[g])
				kept[nkept++] = ms->outseg[g];

		memcpy(m->segs, kept, nkept * sizeof(WeaveSegMeta));
		m->nsegments = nkept;
		m->generation++;		/* directory changed: invalidate concurrent scan snapshots */
		GenericXLogFinish(state);
		UnlockReleaseBuffer(mb);

		/* recycle the consumed source segments' pages */
		for (g = 0; g < ngroups; g++)
			if (ms->outvalid[g])
				for (k = ms->groupoff[g]; k < ms->groupoff[g + 1]; k++)
					weave_free_segment(index, &ms->src[k]);
	}

	DestroyParallelContext(pcxt);
	ExitParallelMode();
	IndexFreeSpaceMapVacuum(index);
	return true;
}

static bool
weave_merge_all(Relation index, bool try_parallel)
{
	bool		didwork = false;
	int			guard;
	bool		saved_extend_only = weave_alloc_extend_only;

	/*
	 * Try a parallel merge first (unless already inside a parallel operation,
	 * e.g. the parallel build leader -- no nested parallelism).  It compacts
	 * the sources into (workers+1) groups in one parallel pass; the serial
	 * loop below then finishes to a single segment.
	 *
	 * NB: iterating the parallel pass to one segment was measured WORSE at 2M
	 * (each pass rewrites data -> write amplification) and did not cut the
	 * tail: the final reduction is the write of ONE multi-GB output segment by
	 * a single backend, which no group-partition scheme parallelizes.  The
	 * merge tail is a single-output-write cost, not a parallelism-partition
	 * one -- see ROADMAP.md (codec / streamed-write direction).
	 */
	if (try_parallel && !IsInParallelMode() && max_parallel_maintenance_workers > 0)
	{
		int			request = Min(max_parallel_maintenance_workers,
								 max_parallel_workers);

		if (request > 0 && weave_merge_all_parallel(index, request))
			didwork = true;
	}

	/* extend-only serial collapse (same recycle-race avoidance as
	 * weave_merge_segments; freed inputs are reclaimed later) */
	weave_alloc_extend_only = true;

	PG_TRY();
	{
	for (guard = 0; guard < WEAVE_MAX_SEGMENTS; guard++)
	{
		WeaveMetaPageData meta;
		MergeCand	cand[WEAVE_MAX_SEGMENTS];
		uint32		sel[WEAVE_MAX_SEGMENTS];
		uint32		nsel = 0;
		uint32		ncand = 0;
		uint32		i;

		{
			Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

			LockBuffer(mb, BUFFER_LOCK_SHARE);
			weave_meta_from_page(BufferGetPage(mb), &meta);
			UnlockReleaseBuffer(mb);
		}
		if (meta.nsegments <= 1)
			break;				/* already optimal */

		/*
		 * Collapse toward one segment in BOUNDED FAN-IN batches: sort populated
		 * segments by live size and merge the smallest <= WEAVE_MERGE_FANOUT of
		 * them each pass.  Merging all N at once (the old behavior) was a single
		 * multi-GB single-backend pass over the whole index -- on a large,
		 * high-vocabulary corpus that terminal merge is the field-reported
		 * non-converging "one segment rewritten forever".  Smallest-first bounded
		 * batches keep each pass cheap and observable (nsegments falls a bounded
		 * step per pass) and still reach a single segment.
		 */
		for (i = 0; i < meta.nsegments; i++)
			if (meta.segs[i].dictstart != InvalidBlockNumber)
			{
				cand[ncand].idx = i;
				cand[ncand].size = meta.segs[i].ndocs - meta.segs[i].ndeleted;
				if (cand[ncand].size < 1)
					cand[ncand].size = 1;
				ncand++;
			}
		if (ncand <= 1)
			break;
		qsort(cand, ncand, sizeof(MergeCand), cmp_mergecand);
		for (i = 0; i < ncand && nsel < WEAVE_MERGE_FANOUT; i++)
			sel[nsel++] = cand[i].idx;

		if (nsel < 2)
			break;
		if (!weave_merge_selected(index, sel, nsel))
			break;				/* directory changed underneath; stop */
		didwork = true;
	}
	}
	PG_FINALLY();
	{
		weave_alloc_extend_only = saved_extend_only;
	}
	PG_END_TRY();
	return didwork;
}

/*
 * Finalize an index BUILD's segment layout.
 *
 * The scan phase leaves many segments (each parallel participant flushes
 * several, budget-triggered).  Collapsing them all into ONE segment is optimal
 * for ranked-scan latency, but on a huge, high-vocabulary corpus that final
 * single-backend merge writes the entire multi-GB index in one shot and can run
 * for hours with no incremental progress -- the field-reported non-convergence.
 *
 * So finalize adaptively, LSM-style, and SERIALLY (no parallel merge context --
 * see the extension-lock hazard note in the body):
 *   1. Size-tiered merge (weave_merge_segments) to a bounded, geometrically
 *      spread set -- always a bounded amount of work per merge, always
 *      converges.  The 1.1.1 O(N) trigram build made this tractable even on a
 *      huge, high-vocabulary corpus.
 *   2. Collapse to a single segment ONLY when the whole index is small enough
 *      (<= pg_weave.build_collapse_max_mb) that the single-backend collapse is
 *      quick.  Above that, stop at the bounded tiered set: the index is valid
 *      and queryable (ranked scans traverse a bounded handful of segments, a
 *      small fixed cost), and weave_merge() collapses to one on demand in a
 *      maintenance window.
 *
 * This makes a large build ALWAYS terminate in bounded, observable steps
 * (each weave_merge_selected logs a DEBUG1 progress line) instead of a single
 * open-ended collapse.
 */
static void
weave_build_finalize(Relation index)
{
	BlockNumber nblocks;
	uint64		sizemb;
	int			nseg;
	WeaveMetaPageData meta;

	/*
	 * Bounded size-tiered merge (LSM), serial.  We do NOT start a parallel
	 * merge context here: this runs inside ambuild, after the build-scan's own
	 * parallel context was torn down (weave_end_parallel), and re-entering
	 * parallel mode to merge a very large index inside ambuild -- especially
	 * under CREATE INDEX CONCURRENTLY on a busy, memory-pressured host -- risks
	 * wedging the leader in WaitForParallelWorkersToFinish while participants
	 * contend on / hold the relation-extension lock (a field-reported hang:
	 * leader parked in poll(), a sibling blocked on Lock:extend, indisvalid=f
	 * for hours).  The 1.1.1 O(N) trigram fix made the serial merge tractable,
	 * so the parallel pass is no longer needed for convergence; an explicit
	 * weave_merge() (run outside ambuild) still parallelizes on demand.
	 */
	weave_merge_segments(index);

	/*
	 * Collapse to one only if the whole index is small enough that the
	 * single-backend collapse is quick.  pg_weave.build_collapse_max_mb == 0 forces
	 * the historical always-collapse behavior for callers who want it.
	 */
	nblocks = RelationGetNumberOfBlocks(index);
	sizemb = ((uint64) nblocks * BLCKSZ) / (1024 * 1024);
	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
	}
	nseg = (int) meta.nsegments;

	if (pg_weave_build_collapse_max_mb == 0 ||
		sizemb <= (uint64) pg_weave_build_collapse_max_mb)
	{
		if (nseg > 1)
			elog(DEBUG1, "pg_weave build: index \"%s\": collapsing %d segments to one (%lu MB <= collapse cap %d MB)",
				 RelationGetRelationName(index), nseg, (unsigned long) sizemb,
				 pg_weave_build_collapse_max_mb);
		weave_merge_all(index, false);	/* serial: no parallel re-entry inside ambuild */
	}
	else
		elog(LOG, "pg_weave build: index \"%s\": leaving %d size-tiered segments (%lu MB > collapse cap %d MB); run weave_merge('%s') to collapse to one",
			 RelationGetRelationName(index), nseg, (unsigned long) sizemb,
			 pg_weave_build_collapse_max_mb, RelationGetRelationName(index));
}

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
static BlockNumber
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
 * Is the index already at its compaction floor -- i.e. would a vacate+pack
 * rewrite be pure waste?  True only when BOTH:
 *   (1) the live data is already front-packed (negligible free space below the
 *       highest live block), so a rewrite would only re-grow then re-truncate
 *       to the same size, and
 *   (2) there is at most ONE live segment, so there is nothing to coalesce
 *       (weave_vacuum's other job is to merge segments to one for scan speed).
 * If either fails, the vacate+pack pass still has work to do.  Scan-only for
 * the FSM part; a brief shared lock on the metapage for the segment count.
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

static bool
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
		 * Phase 1: vacate -- push the live segment onto fresh high blocks so the
		 * freed old pages form one contiguous low free region >= live size.
		 *
		 * SKIPPED when the low free region already exceeds the live size, which is
		 * exactly the end-of-build shape (~70% freed below ~30% live).  The vacate
		 * is a full extra rewrite of the whole segment; skipping it halves the
		 * compaction I/O of a fresh build.  Task L12 / gap G5.
		 */
		if (!weave_low_free_fits_live(index))
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

static void
weave_merge_segments(Relation index)
{
	int			guard;
	bool		saved_extend_only = weave_alloc_extend_only;

	/*
	 * Leveled (HanoiDB/LSM) compaction: each pass, assign every live segment a
	 * level from its size, and if any level holds >= WEAVE_MERGE_FANOUT runs,
	 * merge JUST that level's runs into one (which lands in the next level).
	 * Merging only one level at a time bounds the fan-in of any single merge to
	 * ~FANOUT segments, so no pass is ever the giant "merge all N segments at
	 * once" that the old size-tiered selector produced on a build of many
	 * near-equal segments.  The loop compacts the lowest over-capacity level
	 * first (cheapest), converging in O(log) passes; the guard bounds it (each
	 * successful merge strictly reduces nsegments).
	 *
	 * Allocate merge output EXTEND-ONLY for the whole loop: a committed merge
	 * frees its input pages to the FSM, and without this the NEXT merge's
	 * weave_new_buffer would recycle those freed blocks for its output while it is
	 * still reading input posting/dict chains -- whose on-page nextblk pointers
	 * may thread through a just-recycled (rewritten, or past-EOF) block, giving a
	 * wrong read or a SIGBUS.  Extending to fresh high blocks means no in-flight
	 * read chain ever points at a block this loop hands out; freed pages are
	 * reclaimed later (VACUUM / weave_truncate_free_tail).
	 */
	weave_alloc_extend_only = true;

	PG_TRY();
	{
	for (guard = 0; guard < WEAVE_MAX_SEGMENTS; guard++)
	{
		WeaveMetaPageData meta;
		uint32		sel[WEAVE_MAX_SEGMENTS];
		uint32		nsel = 0;
		int			lvlcount[WEAVE_MAX_LEVELS];
		int			target = -1;
		uint32		i;

		{
			Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

			LockBuffer(mb, BUFFER_LOCK_SHARE);
			weave_meta_from_page(BufferGetPage(mb), &meta);
			UnlockReleaseBuffer(mb);
		}
		if (meta.nsegments <= 1)
			break;

		/* count runs per level */
		memset(lvlcount, 0, sizeof(lvlcount));
		for (i = 0; i < meta.nsegments; i++)
		{
			if (meta.segs[i].dictstart == InvalidBlockNumber)
				continue;
			lvlcount[weave_seg_level(meta.segs[i].ndocs - meta.segs[i].ndeleted)]++;
		}

		/* lowest level that is over capacity (>= FANOUT runs) is compacted first */
		for (i = 0; i < WEAVE_MAX_LEVELS; i++)
			if (lvlcount[i] >= WEAVE_MERGE_FANOUT)
			{
				target = (int) i;
				break;
			}

		/*
		 * If no level is over capacity but the total count still exceeds the
		 * budget, compact the lowest level that has >= 2 runs -- this bounds the
		 * live segment count (query cost) without ever selecting more than one
		 * level's worth (bounded fan-in).
		 */
		if (target < 0 && meta.nsegments > WEAVE_MERGE_THRESHOLD)
			for (i = 0; i < WEAVE_MAX_LEVELS; i++)
				if (lvlcount[i] >= 2)
				{
					target = (int) i;
					break;
				}

		if (target < 0)
			break;				/* every level within capacity + count OK */

		/* select up to WEAVE_MERGE_FANOUT smallest segments in the target level
		 * (bounded fan-in: never merge a whole over-full level at once, which for
		 * pg_weave's near-equal flushed segments would be an all-at-once pass) */
		{
			MergeCand	lc[WEAVE_MAX_SEGMENTS];
			uint32		nlc = 0;

			for (i = 0; i < meta.nsegments; i++)
			{
				double		sz;

				if (meta.segs[i].dictstart == InvalidBlockNumber)
					continue;
				sz = meta.segs[i].ndocs - meta.segs[i].ndeleted;
				if (weave_seg_level(sz) != target)
					continue;
				lc[nlc].idx = i;
				lc[nlc].size = sz < 1 ? 1 : sz;
				nlc++;
			}
			qsort(lc, nlc, sizeof(MergeCand), cmp_mergecand);
			for (i = 0; i < nlc && nsel < WEAVE_MERGE_FANOUT; i++)
				sel[nsel++] = lc[i].idx;
		}

		if (nsel < 2)
			break;				/* nothing to do (shouldn't happen: count >= 2) */
		if (!weave_merge_selected(index, sel, nsel))
			break;				/* directory changed underneath */
	}
	}
	PG_FINALLY();
	{
		weave_alloc_extend_only = saved_extend_only;
	}
	PG_END_TRY();
}

/* ---- parallel index build (level 1: parallel heap scan + per-worker segment
 * flush; the leader merges the workers' segments at the end) ---- */

#define PARALLEL_KEY_BM25_SHARED		UINT64CONST(0xB250000000000001)
#define PARALLEL_KEY_QUERY_TEXT			UINT64CONST(0xB250000000000002)
#define PARALLEL_KEY_WAL_USAGE			UINT64CONST(0xB250000000000003)
#define PARALLEL_KEY_BUFFER_USAGE		UINT64CONST(0xB250000000000004)

/*
 * Shared state for a parallel weave build, in the DSM segment.  Workers write
 * their own segments straight into the index (segments are self-contained and
 * appended to the metapage under its exclusive lock), so unlike a btree build
 * there is no central sort or result hand-off -- the only shared state is the
 * parallel table scan and a done-counter.
 */
typedef struct WeaveShared
{
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	ConditionVariable workersdonecv;
	slock_t		mutex;
	int			nparticipantsdone;
	double		reltuples;
	/* ParallelTableScanDescData follows (alignment: allocated separately) */
}			WeaveShared;

#define ParallelTableScanFromBM25Shared(shared) \
	(ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(WeaveShared)))

typedef struct WeaveLeader
{
	ParallelContext *pcxt;
	int			nparticipanttuplesorts;
	WeaveShared *weaveshared;
	Snapshot	snapshot;
	BufferUsage *bufferusage;
	WalUsage   *walusage;
}			WeaveLeader;

/*
 * Run the heap scan (serial or, if pscan != NULL, a parallel slice) building
 * segments into `index`.  Returns the number of heap tuples this participant
 * saw.  Flushing the residual terms is left to the caller so the leader can
 * account the total before the final merge.
 */
static double
weave_scan_and_build(Relation heap, Relation index, IndexInfo *indexInfo,
					WeaveBuildState *bs, ParallelTableScanDesc pscan)
{
	TableScanDesc scan = NULL;

	if (pscan != NULL)
#if PG_VERSION_NUM >= 190000
		scan = table_beginscan_parallel(heap, pscan, SO_NONE);
#else
		scan = table_beginscan_parallel(heap, pscan);
#endif
	return table_index_build_scan(heap, index, indexInfo, true, true,
								  weave_build_callback, (void *) bs, scan);
}

/*
 * Worker entry point (registered as "pg_weave"/"weave_parallel_build_main").
 */
PGDLLEXPORT void weave_parallel_build_main(dsm_segment *seg, shm_toc *toc);

void
weave_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
	WeaveShared *weaveshared;
	Relation	heap;
	Relation	index;
	IndexInfo  *indexInfo;
	ParallelTableScanDesc pscan;
	WeaveBuildState bs;
	LOCKMODE	heapLockmode;
	LOCKMODE	indexLockmode;
	double		reltuples;
	char	   *sharedquery;
	BufferUsage *bufferusage;
	WalUsage   *walusage;

	weaveshared = (WeaveShared *) shm_toc_lookup(toc, PARALLEL_KEY_BM25_SHARED, false);

	sharedquery = shm_toc_lookup(toc, PARALLEL_KEY_QUERY_TEXT, true);
	debug_query_string = sharedquery;

	if (!weaveshared->isconcurrent)
	{
		heapLockmode = ShareLock;
		indexLockmode = AccessExclusiveLock;
	}
	else
	{
		heapLockmode = ShareUpdateExclusiveLock;
		indexLockmode = RowExclusiveLock;
	}

	heap = table_open(weaveshared->heaprelid, heapLockmode);
	index = index_open(weaveshared->indexrelid, indexLockmode);
	indexInfo = BuildIndexInfo(index);
	indexInfo->ii_Concurrent = weaveshared->isconcurrent;

	/* report buffer/WAL usage so EXPLAIN ANALYZE etc. account worker I/O */
	bufferusage = shm_toc_lookup(toc, PARALLEL_KEY_BUFFER_USAGE, false);
	walusage = shm_toc_lookup(toc, PARALLEL_KEY_WAL_USAGE, false);
	InstrStartParallelQuery();

	pscan = ParallelTableScanFromBM25Shared(weaveshared);

	bs.ctx = AllocSetContextCreate(CurrentMemoryContext, "weave parallel worker",
								   ALLOCSET_DEFAULT_SIZES);
	bs.want_positions = weave_index_wants_positions(index);
	bs.want_trigrams = weave_index_wants_trigrams(index);
	bs.want_sidecar = weave_index_wants_doclen_sidecar(index);
	bs.terms = NULL;
	bs.nterms = 0;
	bs.maxterms = 0;
	bs.ndocs = 0;
	bs.sumdoclen = 0;
	bs.flush_budget = 0;
	bs.nflushes = 0;
	weave_build_ht_init(&bs);
	reltuples = weave_scan_and_build(heap, index, indexInfo, &bs, pscan);
	weave_build_flush_segment(index, &bs);	/* worker's residual -> a segment */
	MemoryContextDelete(bs.ctx);

	InstrEndParallelQuery(&bufferusage[ParallelWorkerNumber],
						  &walusage[ParallelWorkerNumber]);

	/* report done + this worker's tuple count */
	SpinLockAcquire(&weaveshared->mutex);
	weaveshared->nparticipantsdone++;
	weaveshared->reltuples += reltuples;
	SpinLockRelease(&weaveshared->mutex);
	ConditionVariableSignal(&weaveshared->workersdonecv);

	index_close(index, indexLockmode);
	table_close(heap, heapLockmode);
}

/*
 * Set up the parallel context, DSM shared state, and launch workers.  Returns
 * the leader struct, or NULL if no workers could be launched (fall back to a
 * serial build).
 */
static WeaveLeader *
weave_begin_parallel(Relation heap, Relation index, bool isconcurrent,
					int request)
{
	ParallelContext *pcxt;
	Snapshot	snapshot;
	Size		estbm25shared;
	Size		estscan;
	WeaveShared *weaveshared;
	ParallelTableScanDesc pscan;
	WeaveLeader *weaveleader;
	BufferUsage *bufferusage;
	WalUsage   *walusage;
	char	   *sharedquery;
	int			querylen;
	bool		leaderparticipates = true;

	EnterParallelMode();
	Assert(request > 0);
	pcxt = CreateParallelContext("pg_weave", "weave_parallel_build_main", request);

	if (!isconcurrent)
		snapshot = SnapshotAny;
	else
		snapshot = RegisterSnapshot(GetTransactionSnapshot());

	estbm25shared = BUFFERALIGN(sizeof(WeaveShared));
	estscan = table_parallelscan_estimate(heap, snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, estbm25shared + estscan);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/* query text for worker debug/reporting */
	if (debug_query_string)
	{
		querylen = strlen(debug_query_string);
		shm_toc_estimate_chunk(&pcxt->estimator, querylen + 1);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}
	else
		querylen = 0;

	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	shm_toc_estimate_chunk(&pcxt->estimator,
						   mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	InitializeParallelDSM(pcxt);

	if (pcxt->seg == NULL)
	{
		if (IsMVCCSnapshot(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return NULL;
	}

	weaveshared = (WeaveShared *) shm_toc_allocate(pcxt->toc,
												 estbm25shared + estscan);
	weaveshared->heaprelid = RelationGetRelid(heap);
	weaveshared->indexrelid = RelationGetRelid(index);
	weaveshared->isconcurrent = isconcurrent;
	weaveshared->nparticipantsdone = 0;
	weaveshared->reltuples = 0.0;
	ConditionVariableInit(&weaveshared->workersdonecv);
	SpinLockInit(&weaveshared->mutex);

	pscan = ParallelTableScanFromBM25Shared(weaveshared);
	table_parallelscan_initialize(heap, pscan, snapshot);
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BM25_SHARED, weaveshared);

	if (debug_query_string)
	{
		sharedquery = (char *) shm_toc_allocate(pcxt->toc, querylen + 1);
		memcpy(sharedquery, debug_query_string, querylen + 1);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_QUERY_TEXT, sharedquery);
	}

	bufferusage = shm_toc_allocate(pcxt->toc,
								   mul_size(sizeof(BufferUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_BUFFER_USAGE, bufferusage);
	walusage = shm_toc_allocate(pcxt->toc,
								mul_size(sizeof(WalUsage), pcxt->nworkers));
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_WAL_USAGE, walusage);

	LaunchParallelWorkers(pcxt);

	if (pcxt->nworkers_launched == 0)
	{
		/* no workers actually started; caller will do a serial build */
		WaitForParallelWorkersToFinish(pcxt);
		if (IsMVCCSnapshot(snapshot))
			UnregisterSnapshot(snapshot);
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return NULL;
	}

	weaveleader = (WeaveLeader *) palloc0(sizeof(WeaveLeader));
	weaveleader->pcxt = pcxt;
	weaveleader->nparticipanttuplesorts = pcxt->nworkers_launched;
	if (leaderparticipates)
		weaveleader->nparticipanttuplesorts++;
	weaveleader->weaveshared = weaveshared;
	weaveleader->snapshot = snapshot;
	weaveleader->bufferusage = bufferusage;
	weaveleader->walusage = walusage;
	return weaveleader;
}

/*
 * Wait for all workers to finish, accumulate their I/O stats + tuple count,
 * and tear down.  Returns the total heap tuples the workers scanned (read from
 * the DSM before it is unmapped).
 */
static double
weave_end_parallel(WeaveLeader *weaveleader)
{
	int			i;
	double		worker_tuples;

	WaitForParallelWorkersToFinish(weaveleader->pcxt);

	for (i = 0; i < weaveleader->pcxt->nworkers_launched; i++)
		InstrAccumParallelQuery(&weaveleader->bufferusage[i], &weaveleader->walusage[i]);

	/* read the workers' accumulated tuple count while the DSM is still mapped */
	worker_tuples = weaveleader->weaveshared->reltuples;

	if (IsMVCCSnapshot(weaveleader->snapshot))
		UnregisterSnapshot(weaveleader->snapshot);
	DestroyParallelContext(weaveleader->pcxt);
	ExitParallelMode();
	return worker_tuples;
}

static IndexBuildResult *
weave_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult *result;
	WeaveBuildState bs;
	double		reltuples;
	WeaveLeader *weaveleader = NULL;

	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* metapage must be block 0 -- write it before workers or the scan touch it */
	weave_init_metapage(index);

	/* Try a parallel build if the planner requested workers. */
	if (indexInfo->ii_ParallelWorkers > 0)
		weaveleader = weave_begin_parallel(heap, index, indexInfo->ii_Concurrent,
										 indexInfo->ii_ParallelWorkers);

	bs.ctx = AllocSetContextCreate(CurrentMemoryContext, "weave build",
								   ALLOCSET_DEFAULT_SIZES);
	bs.want_positions = weave_index_wants_positions(index);
	bs.want_trigrams = weave_index_wants_trigrams(index);
	bs.want_sidecar = weave_index_wants_doclen_sidecar(index);
	bs.terms = NULL;
	bs.nterms = 0;
	bs.maxterms = 0;
	bs.ndocs = 0;
	bs.sumdoclen = 0;
	bs.flush_budget = 0;
	bs.nflushes = 0;
	weave_build_ht_init(&bs);

	if (weaveleader != NULL)
	{
		/*
		 * Parallel build: the leader also scans a slice (leaderparticipates),
		 * using the same shared parallel scan the workers use, and flushes its
		 * residual as a segment.  Workers write their own segments directly.
		 */
		ParallelTableScanDesc pscan =
			ParallelTableScanFromBM25Shared(weaveleader->weaveshared);

		reltuples = weave_scan_and_build(heap, index, indexInfo, &bs, pscan);
		weave_build_flush_segment(index, &bs);

		/* add the workers' tuple counts BEFORE tearing down the DSM */
		reltuples += weave_end_parallel(weaveleader);

		/*
		 * Finalize the participants' segments.  Rather than always collapsing to
		 * a single segment (a single-backend O(index) merge that does not converge
		 * in bounded time on a huge, high-vocabulary corpus), weave_build_finalize
		 * runs a serial bounded size-tiered merge and collapses to one only when the
		 * index is small enough (pg_weave.build_collapse_max_mb).  A large build thus
		 * always terminates, leaving a bounded tiered set; weave_merge() collapses to
		 * one on demand.  It does NOT start a parallel merge context inside ambuild
		 * (that risked wedging CONCURRENTLY builds on large/pressured hosts).
		 */
		weave_build_finalize(index);
	}
	else
	{
		/* Serial build. */
		reltuples = weave_scan_and_build(heap, index, indexInfo, &bs, NULL);
		weave_build_flush_segment(index, &bs);

		/*
		 * Same adaptive finalization as the parallel path (a serial build makes
		 * fewer segments, so this usually collapses to one; a very large serial
		 * build stays tiered rather than spinning on an open-ended collapse).
		 */
		weave_build_finalize(index);
	}

	/*
	 * Reclaim the space the end-of-build merge left on disk.
	 *
	 * The merge writes its output before freeing its inputs (write-before-free,
	 * required for crash safety: the old pages must stay valid until the metapage
	 * swap commits).  So a finished build has the live segment at the HIGH end of
	 * the file and the freed input pages as a LOW free region -- exactly the
	 * layout weave_vacuum_compact's header describes as the hard case, and one
	 * where truncating the free TAIL reclaims nothing because the tail is live.
	 *
	 * Measured on a 1M-document build before this call was added: 156 MB on disk
	 * of which 110 MB (70.7%) were freed pages, against 46 MB of live content --
	 * a 3.4x bloat that went away only if the user knew to call weave_vacuum()
	 * afterwards.  It also made pg_weave look 1.7-1.9x LARGER than tsvector+GIN
	 * in bench/RESULTS_LEXICAL.md when the live content is in fact 1.76x SMALLER.
	 * `CREATE INDEX` has to produce a finished index; requiring a follow-up
	 * maintenance call to reach the natural size is a defect, not a tuning knob.
	 * Gap G6 / task L8 in doc/GAPS.md.
	 *
	 * Safe here for the reasons the free-tail truncation was: we hold the index
	 * AccessExclusiveLock, parallel workers are already torn down
	 * (weave_end_parallel), and during ambuild the index is not yet visible to
	 * other backends (indisready=false, even under CONCURRENTLY), so this backend
	 * is the sole writer and the page-recycle gate's exclusive-lock precondition
	 * holds.  The vacate phase grows the file transiently before truncating; that
	 * is the price of a single-pass in-place shrink and is bounded by the live
	 * index size.
	 */
	if (!weave_vacuum_compact(index))
		weave_truncate_free_tail(index);

	MemoryContextDelete(bs.ctx);

	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));
	result->heap_tuples = reltuples;
	result->index_tuples = reltuples;
	return result;
}

static void
weave_buildempty(Relation index)
{
	weave_init_metapage(index);
}

/*
 * Index one oversized document (its analyzed wdoc does not fit on a single
 * pending page) directly as its own one-document segment, bypassing the
 * verbatim pending buffer.  Segment posting storage is a chain of FOR-packed
 * pages with no per-document size limit, so arbitrarily large documents (e.g.
 * long Wikipedia articles) can be indexed.  Rare, so building a whole segment
 * per such document is acceptable.
 */
static void
weave_insert_oversized_as_segment(Relation index, WeaveDoc doc, ItemPointer tid)
{
	WeaveBuildState bs;
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	uint32		j;

	bs.ctx = AllocSetContextCreate(CurrentMemoryContext, "weave oversized",
								   ALLOCSET_DEFAULT_SIZES);
	bs.want_positions = weave_index_wants_positions(index);
	bs.want_trigrams = weave_index_wants_trigrams(index);
	bs.want_sidecar = weave_index_wants_doclen_sidecar(index);
	bs.terms = NULL;
	bs.nterms = 0;
	bs.maxterms = 0;
	bs.ndocs = 0;
	bs.sumdoclen = 0;
	bs.nflushes = 0;
	bs.flush_budget = 0;
	weave_build_ht_init(&bs);

	{
		MemoryContext old = MemoryContextSwitchTo(bs.ctx);

		for (j = 0; j < doc->nterms; j++)
		{
			const uint32 *pos = (bs.want_positions && WEAVE_DOC_HAS_POS(doc))
				? WEAVE_DOC_TERMPOS(doc, &entries[j]) : NULL;

			add_posting(&bs, WEAVE_DOC_TERMTEXT(doc, &entries[j]), entries[j].len,
						tid, entries[j].tf, doc->doclen,
						pos, pos ? (int) entries[j].tf : 0);
		}
		bs.ndocs = 1.0;
		bs.sumdoclen = doc->doclen;
		MemoryContextSwitchTo(old);
	}

	/* write the one-doc segment (updates corpus N/sumdoclen via add_segment) */
	weave_build_flush_segment(index, &bs);
	MemoryContextDelete(bs.ctx);

	/*
	 * Keep the tiered segment set compacted as documents arrive.  On a
	 * continuously-written table whose rows are mostly larger than one pending
	 * page (long email bodies, articles, code -- the common case for a body
	 * index), EVERY insert lands here and mints a segment, so without ongoing
	 * compaction the directory climbs to the hard cap within the hour (a field
	 * deployment did exactly this: 8 -> 128 segments in ~1h of ingestion, then
	 * could neither merge nor VACUUM).  weave_merge_segments() is the leveled LSM
	 * compactor: it merges only a level that is over its fan-in capacity and is
	 * a cheap metapage read when every level is within capacity, so calling it
	 * after each flush keeps nsegments bounded (O(log N) tiers) instead of
	 * letting it grow unbounded toward the cap.  This IS the background
	 * auto-compaction: no VACUUM or manual weave_merge() is required to stay
	 * healthy under continuous writes.
	 *
	 * The merge frees + recycles the input segments' pages, which is unsafe to
	 * run concurrently with another flush/merge/compact on this index, so take
	 * the maintenance mutex.  CONDITIONALLY: this is an opportunistic insert-time
	 * compaction under only RowExclusiveLock -- if a cleanup is already running
	 * (autovacuum, weave_merge, or another inserter) just skip; that writer, or the
	 * next insert/vacuum, keeps nsegments bounded.  (Adding the segment above is
	 * an extend-only metapage write and needs no mutex; only the recycling merge
	 * does.)
	 */
	if (weave_maintenance_lock_conditional(index))
	{
		PG_TRY();
		{
			weave_merge_segments(index);
		}
		PG_FINALLY();
		{
			weave_maintenance_unlock(index);
		}
		PG_END_TRY();
	}
}

/*
 * aminsert: append the new document to the pending list.
 *
 * The document is stored verbatim (its wdoc bytes) on a chain of pending
 * pages and is searched directly at scan time, so newly inserted rows are
 * immediately visible to @@@ without a REINDEX.  The metapage N and sum(doclen)
 * are updated so BM25 length-normalization stays correct; per-term df in the
 * dictionary is not updated until a merge (REINDEX), matching GIN fastupdate's
 * documented staleness.
 */
static bool
weave_insert(Relation index, Datum *values, bool *isnull,
			ItemPointer ht_ctid, Relation heapRel,
			IndexUniqueCheck checkUnique, bool indexUnchanged,
			IndexInfo *indexInfo)
{
	WeaveDoc		doc;
	Size		doclen;
	Size		need;
	Buffer		metabuf;
	GenericXLogState *state;
	Page		metapage;
	WeaveMetaPageData *meta;
	BlockNumber tailblk;
	Buffer		tailbuf;
	Page		tailpage;
	bool		appended = false;

	if (isnull[0])
		return false;

	doc = (WeaveDoc) PG_DETOAST_DATUM(values[0]);
	doclen = VARSIZE(doc);
	need = MAXALIGN(sizeof(WeavePendingItem) + doclen);

	if (need > BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - MAXALIGN(sizeof(WeavePageOpaqueData)))
	{
		/* Too large for the verbatim pending buffer: index it directly as its
		 * own one-document segment (no per-doc size limit there). */
		weave_insert_oversized_as_segment(index, doc, ht_ctid);
		return true;
	}

	/* Lock the metapage for the whole append (serializes inserters; a
	 * per-inserter fast path is a later optimization). */
	metabuf = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	weave_check_meta(metapage, index);
	meta = WeavePageGetMeta(metapage);
	tailblk = meta->pendingtail;

	/* Try to append to the current tail page. */
	if (tailblk != InvalidBlockNumber)
	{
		tailbuf = ReadBuffer(index, tailblk);
		LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
		tailpage = BufferGetPage(tailbuf);
		if (((PageHeader) tailpage)->pd_lower + need <=
			BLCKSZ - MAXALIGN(sizeof(WeavePageOpaqueData)))
		{
			WeavePendingItem *pi;

			state = GenericXLogStart(index);
			tailpage = GenericXLogRegisterBuffer(state, tailbuf, 0);
			pi = (WeavePendingItem *) ((char *) tailpage +
									 ((PageHeader) tailpage)->pd_lower);
			pi->tid = *ht_ctid;
			pi->doclen = doclen;
			memcpy((char *) pi + sizeof(WeavePendingItem), doc, doclen);
			((PageHeader) tailpage)->pd_lower += need;
			metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
			meta = WeavePageGetMeta(metapage);
			meta->ndocs += 1.0;
			meta->sumdoclen += doc->doclen;
			meta->npending += 1;
			GenericXLogFinish(state);
			appended = true;
		}
		if (!appended)
			UnlockReleaseBuffer(tailbuf);	/* re-read below as oldtail */
	}

	/* Need a fresh pending page (either none yet, or the tail is full). */
	if (!appended)
	{
		Buffer		newbuf = weave_new_buffer(index);
		BlockNumber newblk = BufferGetBlockNumber(newbuf);
		WeavePendingItem *pi;

		state = GenericXLogStart(index);
		{
			Page		np = GenericXLogRegisterBuffer(state, newbuf,
													   GENERIC_XLOG_FULL_IMAGE);

			weave_init_page(np, WEAVE_PENDING);
			pi = (WeavePendingItem *) ((char *) np +
									 ((PageHeader) np)->pd_lower);
			pi->tid = *ht_ctid;
			pi->doclen = doclen;
			memcpy((char *) pi + sizeof(WeavePendingItem), doc, doclen);
			((PageHeader) np)->pd_lower += need;
		}

		/* link previous tail (if any) to the new page */
		if (tailblk != InvalidBlockNumber)
		{
			Buffer		oldtail = ReadBuffer(index, tailblk);
			Page		op;

			LockBuffer(oldtail, BUFFER_LOCK_EXCLUSIVE);
			op = GenericXLogRegisterBuffer(state, oldtail, 0);
			WeavePageGetOpaque(op)->nextblk = newblk;
			metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
			meta = WeavePageGetMeta(metapage);
			meta->pendingtail = newblk;
			meta->ndocs += 1.0;
			meta->sumdoclen += doc->doclen;
			meta->npending += 1;
			GenericXLogFinish(state);
			UnlockReleaseBuffer(oldtail);
		}
		else
		{
			metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
			meta = WeavePageGetMeta(metapage);
			meta->pendinghead = newblk;
			meta->pendingtail = newblk;
			meta->ndocs += 1.0;
			meta->sumdoclen += doc->doclen;
			meta->npending += 1;
			GenericXLogFinish(state);
		}
		UnlockReleaseBuffer(newbuf);
	}
	else
		UnlockReleaseBuffer(tailbuf);

	UnlockReleaseBuffer(metabuf);
	return true;
}

/* ----- scan ----- */

#include "../query/lev.c"
#include "amscan.c"
#include "../pages/trgm_page.c"

/* ----- vacuum / flush / merge / cost / options ----- */

/*
 * Maintenance serialization lock (the GIN ginInsertCleanup pattern).
 *
 * Every operation that MUTATES the segment directory or frees + recycles a
 * segment's pages -- weave_flush_pending, weave_merge_segments/_all,
 * weave_vacuum_compact, bulkdelete's livedocs swap -- must run one-at-a-time per
 * index.  The obvious candidate, the table/index relation lock, does NOT serve:
 * autovacuum's index cleanup holds ShareUpdateExclusiveLock on the TABLE while a
 * user weave_merge()/weave_vacuum() holds it on the INDEX -- different lock tags
 * that do not conflict -- and an INSERT's oversized-doc segment path holds only
 * RowExclusiveLock.  So two of these could run at once: one frees + recycles a
 * segment's dict/posting pages while the other's streaming merge is still
 * reading them, yielding a garbage read (a SIGSEGV in merge_source_load_page
 * under heavy concurrent insert+merge+vacuum churn -- the t/006 crash).
 *
 * A heavyweight page lock on the metapage block, used for NOTHING else, gives a
 * per-index mutex independent of the relation lock (exactly how GIN serializes
 * pending-list cleanup).  Explicit/required maintenance (VACUUM cleanup,
 * weave_merge, weave_vacuum) takes it blocking; opportunistic maintenance (the
 * insert-triggered tiered merge) takes it CONDITIONALLY and simply skips when a
 * cleanup is already running -- another writer or the next insert/vacuum will
 * compact, so nsegments still stays bounded.
 */
static inline void
weave_maintenance_lock(Relation index)
{
	LockPage(index, WEAVE_METAPAGE_BLKNO, ExclusiveLock);
}

static inline bool
weave_maintenance_lock_conditional(Relation index)
{
	return ConditionalLockPage(index, WEAVE_METAPAGE_BLKNO, ExclusiveLock);
}

static inline void
weave_maintenance_unlock(Relation index)
{
	UnlockPage(index, WEAVE_METAPAGE_BLKNO, ExclusiveLock);
}

/*
 * Flush the pending write buffer into a NEW immutable segment.
 *
 * O(pending), not O(index): only the pending documents are folded into a fresh
 * segment appended to the directory.  (The old monolithic design re-read and
 * rewrote the entire index on every merge -- O(index) and quadratic under
 * steady inserts.)  Pending pages are then recycled to the FSM.  Tiered
 * compaction of many small segments is a separate operation.  Returns true if
 * a flush happened.
 */
static bool
weave_flush_pending(Relation index)
{
	WeaveMetaPageData meta;
	WeaveBuildState bs;
	WeaveSegMeta seg;
	BlockNumber blk;

	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(mb, BUFFER_LOCK_SHARE);
		weave_meta_from_page(BufferGetPage(mb), &meta);
		UnlockReleaseBuffer(mb);
	}
	if (meta.npending == 0)
		return false;

	bs.ctx = AllocSetContextCreate(CurrentMemoryContext, "weave flush",
								   ALLOCSET_DEFAULT_SIZES);
	bs.want_positions = weave_index_wants_positions(index);
	bs.want_trigrams = weave_index_wants_trigrams(index);
	bs.want_sidecar = weave_index_wants_doclen_sidecar(index);
	bs.terms = NULL;
	bs.nterms = 0;
	bs.maxterms = 0;
	bs.ndocs = 0;
	bs.sumdoclen = 0;
	bs.nflushes = 0;
	bs.flush_budget = 0;
	weave_build_ht_init(&bs);

	/* fold only the pending documents into the build state */
	blk = meta.pendinghead;
	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer = ReadBuffer(index, blk);
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		MemoryContext old = MemoryContextSwitchTo(bs.ctx);

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeavePendingItem *pi = (WeavePendingItem *) ptr;
			WeaveDoc		pdoc;
			WeaveTermEntry *entries;
			uint32		j;

			/* Stop if the item header or its doclen-sized body runs past the page
			 * (a torn/recycled page with a garbage doclen would otherwise advance
			 * ptr off the page and read out of bounds). */
			if ((char *) pi + sizeof(WeavePendingItem) > end ||
				(char *) pi + MAXALIGN(sizeof(WeavePendingItem) + (Size) pi->doclen) > end)
				break;
			pdoc = (WeaveDoc) ((char *) pi + sizeof(WeavePendingItem));

			/* Never trust raw pending-page bytes: a torn page or any producing
			 * bug could give a bad nterms/len/posoff that turns into a wild
			 * write in add_posting.  Validate against the item's own doclen and
			 * skip (not crash) a corrupt doc so autovacuum can make progress. */
			if (!weave_doc_is_valid(pdoc, pi->doclen))
			{
				ereport(WARNING,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_weave: skipping malformed pending document in index \"%s\" during flush",
								RelationGetRelationName(index)),
						 errhint("REINDEX the index to rebuild it from the heap.")));
				ptr += MAXALIGN(sizeof(WeavePendingItem) + pi->doclen);
				continue;
			}
			entries = WEAVE_DOC_ENTRIES(pdoc);

			for (j = 0; j < pdoc->nterms; j++)
			{
				const uint32 *pos = (bs.want_positions && WEAVE_DOC_HAS_POS(pdoc))
					? WEAVE_DOC_TERMPOS(pdoc, &entries[j]) : NULL;

				add_posting(&bs, WEAVE_DOC_TERMTEXT(pdoc, &entries[j]),
							entries[j].len, &pi->tid, entries[j].tf,
							pdoc->doclen, pos, pos ? (int) entries[j].tf : 0);
			}
			bs.ndocs += 1.0;
			bs.sumdoclen += pdoc->doclen;
			ptr += MAXALIGN(sizeof(WeavePendingItem) + pi->doclen);
		}
		UnlockReleaseBuffer(buffer);
		MemoryContextSwitchTo(old);
		blk = next;
	}

	if (bs.nterms > 1)
		qsort(bs.terms, bs.nterms, sizeof(BuildTerm), cmp_buildterm);

	weave_write_segment(index, &bs, &seg);
	weave_add_segment_with_room(index, &seg);

	/*
	 * Clear the pending list.  Pending docs were already counted into the
	 * corpus totals at insert time; add_segment counted them again, so subtract
	 * the segment's contribution to avoid a double count.
	 */
	{
		Buffer		mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
		GenericXLogState *state;
		Page		mp;
		WeaveMetaPageData *m;

		LockBuffer(mb, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		mp = GenericXLogRegisterBuffer(state, mb, 0);
		weave_meta_upcast_page(mp);	/* v3 -> v4 before struct write */
		m = WeavePageGetMeta(mp);
		m->ndocs -= seg.ndocs;
		m->sumdoclen -= seg.sumdoclen;
		m->pendinghead = InvalidBlockNumber;
		m->pendingtail = InvalidBlockNumber;
		m->npending = 0;
		GenericXLogFinish(state);
		UnlockReleaseBuffer(mb);
	}

	/* recycle the old pending pages */
	blk = meta.pendinghead;
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
	IndexFreeSpaceMapVacuum(index);

	MemoryContextDelete(bs.ctx);

	/* keep the segment count bounded (query cost is O(nsegments) per term) */
	weave_merge_segments(index);
	return true;
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
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
			WeavePosting *post;
			int			np,
						k;

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
static IndexBulkDeleteResult *
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
					carrycap = carrycap ? carrycap * 2 : 1024;
					carry = carry ? repalloc(carry, carrycap * sizeof(uint64))
						: palloc(carrycap * sizeof(uint64));
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

			for (v = sm_next_member(seen, (uint64_t) -1, &cur);
				 v != SM_IDX_MAX;
				 v = sm_next_member(seen, v, &cur))
			{
				ItemPointerData tid;
				sm_cursor_t ccur = SM_CURSOR_INIT;

				num_index_tuples++;
				if (sm_contains(dead, v, &ccur))
					continue;		/* already tombstoned (carried forward) */
				weave_docid_to_tid(v, &tid);
				if (callback(&tid, callback_state))
				{
					if (nnew >= newcap)
					{
						newcap = newcap ? newcap * 2 : 1024;
						newdead = newdead ? repalloc(newdead, newcap * sizeof(uint64))
							: palloc(newcap * sizeof(uint64));
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

static IndexBulkDeleteResult *
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

	PG_RETURN_BOOL(done);
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
static bool
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
static bool
weave_index_wants_trigrams(Relation index)
{
	WeaveOptions *opts = (WeaveOptions *) index->rd_options;

	return opts ? opts->trigrams : false;
}

/* Default ON: a new index uses the quantized doclen sidecar (v4).  WITH
 * (doclen_sidecar=off) stores doclen inline in each posting instead (the
 * pre-1.5 layout) -- an escape hatch for workloads that prefer the
 * pre-sidecar ranked-scan behavior. */
static bool
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
