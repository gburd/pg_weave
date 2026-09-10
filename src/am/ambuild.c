/*-------------------------------------------------------------------------
 *
 * ambuild.c
 *		Index build, insert, segment writers and the size-tiered merge for the
 *		weave access method.
 *
 * This is the WRITE side of the index: everything that turns heap tuples into
 * segments and then rewrites segments into fewer, larger ones.
 *
 *	 build		 collect postings into an in-memory term array (a dynahash keyed
 *				 by a length-aware TermKey), flush it to an immutable segment
 *				 when the budget is reached, and repeat -- so peak memory is
 *				 bounded by maintenance_work_mem, not by the corpus.  Optionally
 *				 parallel: each participant scans a slice of the heap and flushes
 *				 its own segments, and the leader merges at the end.
 *	 insert		 append the document verbatim to the pending buffer (immediately
 *				 visible, no dictionary rewrite); an oversized document that
 *				 cannot fit a pending page becomes a one-document segment.
 *	 writers	 the segment on-disk pieces: FOR-packed 128-doc posting blocks,
 *				 the quantized doclen sidecar, the term dictionary plus its
 *				 sparse block index, and the trigram index (the latter's page
 *				 layout is src/pages/trgm_page.c).
 *	 merge		 a bounded, streaming k-way merge of several segments' dictionaries
 *				 that spills term metadata rather than holding a whole merged
 *				 dictionary in memory, plus the size-tiered (LSM) policy that
 *				 decides which segments to merge and a parallel variant of it.
 *
 * Split out of src/am/am.c by task L1; the interface it exports and consumes is
 * declared in include/weave/am.h, which explains why each symbol is there.
 * Nothing in here changed in the split beyond losing `static` where a caller is
 * now in a different file.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/am/ambuild.c
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

/*
 * The FOR (frame-of-reference) codec and the doclen quantizer, as pure
 * standalone C shared with the standalone property tests (test/hegel/) -- single
 * source of truth.  src/am/am.c includes it too, for the read side.
 */
#include "weave/for.h"

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

/* forward decl: the segment writer is defined later; the build flush uses it.
 * Everything else this file calls across a file boundary is declared in
 * include/weave/am.h. */
static void weave_write_segment(Relation index, WeaveBuildState *bs, WeaveSegMeta *seg);

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
				weave_init_page(pw->page, WEAVE_PK_POSTING);
			}
		}
		if (pw->buffer == InvalidBuffer)
		{
			pw->buffer = weave_new_buffer(index);
			pw->state = GenericXLogStart(index);
			pw->page = GenericXLogRegisterBuffer(pw->state, pw->buffer, GENERIC_XLOG_FULL_IMAGE);
			weave_init_page(pw->page, WEAVE_PK_POSTING);
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
				weave_init_page(pw.page, WEAVE_PK_DOCLEN);
			}
		}
		if (pw.buffer == InvalidBuffer)
		{
			pw.buffer = weave_new_buffer(index);
			pw.state = GenericXLogStart(index);
			pw.page = GenericXLogRegisterBuffer(pw.state, pw.buffer, GENERIC_XLOG_FULL_IMAGE);
			weave_init_page(pw.page, WEAVE_PK_DOCLEN);
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
			!WeavePageHasKind(page, WEAVE_PK_DOCLEN))
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

/*
 * Write the dictionary: sorted (term, df, firstposting) entries packed into a
 * chain of dictionary pages.  Returns the first dictionary block, and via
 * *indexstart the first page of the sparse block index (Invalid if empty).
 */

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
			weave_init_page(page, WEAVE_PK_DICT);
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
				weave_init_page(ip, WEAVE_PK_DICTINDEX);
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

/* Thin wrapper: write trigrams from an in-memory bs->terms[] array. */
static BlockNumber
weave_write_trigrams(Relation index, WeaveBuildState *bs)
{
	DictTermArrayIter it;

	it.bs = bs;
	it.i = 0;
	return weave_write_trigrams_iter(index, dict_term_array_next, &it);
}

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
	seg->chandesc = InvalidBlockNumber;	/* set for real by weave_attach_chandesc */
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
	weave_attach_chandesc(index, seg);	/* v6: last, so every root is known */
	doclen_collector_free(&dc);
	pfree(postings);
	pfree(offsets);
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
	seg->chandesc = InvalidBlockNumber;	/* set for real by weave_attach_chandesc */
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
	weave_attach_chandesc(index, seg);	/* v6: last, so every root is known */

	for (i = 0; i < nsel; i++)
		weave_doclens_free(&srcv[i].doclens);

	dict_spill_end(&spill);
	MemoryContextSwitchTo(old);
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

bool
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

bool
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

void
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

IndexBuildResult *
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

void
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
bool
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
			/*
			 * NO weave_meta_upcast_page() HERE, deliberately.  Every field this
			 * path writes lives in the metapage HEAD (magic .. npending), which is
			 * at byte-identical offsets in v3, v4/v5 and v6 -- only the segs[]
			 * stride and the position of `generation` ever moved, and this path
			 * touches neither.  Upcasting on an insert would also be wrong in
			 * spirit: the version word should advance when the DIRECTORY changes,
			 * which is where the six upcast calls are.  Verified when v6 was added
			 * by grepping every in-place metapage writer; if a future field is
			 * added to the head, revisit this.
			 */
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

			weave_init_page(np, WEAVE_PK_PENDING);
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
bool
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
