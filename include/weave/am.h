/*-------------------------------------------------------------------------
 *
 * pg_weave_am.h
 *		On-disk page layout for the weave index access method.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_am.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_AM_H
#define WEAVE_AM_H

#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/htup_details.h"	/* MaxHeapTuplesPerPage (WEAVE_OFFSET_FACTOR) */
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"		/* LockPage (the maintenance serialization lock) */
#include "utils/memutils.h"		/* MemoryContextAllocHuge (WEAVE_ALLOC_MAYBE_HUGE) */
#include "utils/rel.h"			/* RelationGetRelationName, rd_options */

#include "weave/chandesc.h"
#include "weave/pagekind.h"

#define WEAVE_MAGIC			0x42324635	/* "B2F5" */
#define WEAVE_VERSION		6		/* v6: every bolt SELF-DESCRIBES its wefts.
										 * WeaveSegMeta gains `chandesc`, naming a
										 * WEAVE_CHANDESC page, and the page-kind space
										 * stops being a flat uint16 bitmap (see
										 * WEAVE_PAGE_KIND_EXT below).  v5: the doclen
										 * sidecar's docid column stores
										 * ABSOLUTE offsets from each block's first_docid
										 * instead of gaps, so the column is randomly
										 * addressable (weave_for_get) and an in-block
										 * lookup is a ~7-step binary search instead of a
										 * 128-entry FOR-unpack + prefix sum -- ~72% of a
										 * ranked scan was that decode
										 * (bench/RESULTS_SCAN_PROFILE.md).  v4: per-segment
										 * doclen sidecar (1 quantized byte/doc) replacing
										 * the per-posting doclen FOR column; v4 sidecars are
										 * still read (each BLOCK self-describes, see
										 * WEAVE_DOCLEN_ABS).  v3: inline doclen, segmented
										 * layout + optional token positions; still read. */
#define WEAVE_VERSION_DOCLEN_INLINE 3	/* oldest format we dual-read */
#define WEAVE_VERSION_DOCLEN_SIDECAR 4	/* first version with the doclen sidecar */
#define WEAVE_VERSION_DOCLEN_ABS 5	/* first version writing absolute-offset sidecars */
#define WEAVE_VERSION_CHANDESC	6	/* first version with per-bolt weft descriptors */

/*
 * Set in WeaveDoclenBlockHdr.count to mark a sidecar block whose docid column is
 * absolute offsets from first_docid rather than gaps.
 *
 * The flag lives in `count` rather than in a new header field on purpose. The
 * discriminator has to be PER BLOCK, not per index: an index built by <= 0.5.0
 * and then upgraded keeps its v4 gap-coded sidecar pages while later inserts and
 * merges write v5 ones, so the two encodings coexist in one relation and a
 * per-index version cannot describe them. `count` is bounded by
 * WEAVE_BLOCK_SIZE (128), so its high bits are free, and every existing reader
 * already validates `count == 0 || count > WEAVE_BLOCK_SIZE` and stops -- so an
 * older .so meeting a v5 block fails closed (block treated as absent) instead of
 * misreading the offsets as gaps. The metapage version bump to 5 is what
 * actually stops that .so first, with the REINDEX hint (weave_meta_validate);
 * this is the second line of defence.
 */
#define WEAVE_DOCLEN_ABS		0x80000000u
#define WEAVE_DOCLEN_COUNT(c)	((c) & ~WEAVE_DOCLEN_ABS)
#define WEAVE_DOCLEN_IS_ABS(c)	(((c) & WEAVE_DOCLEN_ABS) != 0)
#define WEAVE_METAPAGE_BLKNO	0

/* ---------------------------------------------------------------------------
 * Page kinds
 *
 * The kind space, the escape bit that ended the flat bitmap, the WeavePageKind
 * enum and the pure encode/decode pair are in weave/pagekind.h -- backend-
 * independent so test/hegel/test_pagekind.c can prove the fail-closed property
 * the v6 design rests on exhaustively.  READ THAT HEADER before adding a kind.
 *
 * Here: only the Page-level accessors, which need PostgreSQL's Page type.
 * ------------------------------------------------------------------------- */

typedef struct WeavePageOpaqueData
{
	uint16		flags;
	uint16		kind;			/* extended WeavePageKind; meaningful ONLY when
								 * WEAVE_PAGE_KIND_EXT is set in flags.  Was
								 * `unused`, and reading it is gated on the escape
								 * bit precisely so that we never have to assume
								 * what an older build left in these two bytes. */
	BlockNumber nextblk;		/* next page in a dict/posting/pending chain */
} WeavePageOpaqueData;

typedef WeavePageOpaqueData *WeavePageOpaque;

#define WeavePageGetOpaque(page) \
	((WeavePageOpaque) PageGetSpecialPointer(page))

#define WeavePageGetKind(page) \
	(weave_page_kind_decode(WeavePageGetOpaque(page)->flags, \
							WeavePageGetOpaque(page)->kind))

/* Is this page of kind k?  USE THIS, not `flags & WEAVE_FOO`: an extended kind
 * is not a bit and a bitwise test against one silently reads false. */
#define WeavePageHasKind(page, k)	(WeavePageGetKind(page) == (k))

/* WEAVE_FREED is a state, orthogonal to kind, under both encodings. */
#define WeavePageIsFreed(page) \
	((WeavePageGetOpaque(page)->flags & WEAVE_FREED) != 0)

extern const char *weave_page_kind_name(WeavePageKind kind);

/*
 * A segment: an immutable, self-contained mini-index built from one flush of
 * the write buffer (or the merge of several segments).  The weave index is a set
 * of these plus a small pending write buffer.  Each segment has its own term
 * dictionary, posting lists, trigram index, and a live-docs tombstone bitmap;
 * deletes set a tombstone bit, and a background tiered merge rewrites groups of
 * segments dropping tombstoned docs.  This is the Lucene/Tantivy consensus
 * design; it replaces the old single monolithic structure whose in-memory
 * build OOMed and whose full-rewrite merge was O(index).
 */
typedef struct WeaveSegMeta
{
	BlockNumber dictstart;		/* first dictionary page of this segment */
	BlockNumber trgmstart;		/* first trigram directory page, or Invalid */
	BlockNumber livedocs;		/* first live-docs tombstone page, or Invalid */
	double		ndocs;			/* documents in this segment (incl. tombstoned) */
	double		sumdoclen;		/* sum of doclen in this segment */
	uint32		nterms;			/* distinct terms in this segment */
	uint32		ndeleted;		/* tombstoned docs (for merge accounting) */
	uint32		livedocslen;	/* serialized size of the livedocs tombstone blob */
	BlockNumber dictindexstart; /* sparse block index over dict pages (Invalid = none) */
	BlockNumber doclenstart;	/* first doclen-sidecar page (v4), or Invalid for a
								 * v3 segment whose postings still carry inline
								 * doclen.  Dual-read keys off this per segment. */
	BlockNumber chandesc;		/* v6: the bolt's WEAVE_CHANDESC page, listing every
								 * weft it carries; Invalid means "lexical only",
								 * which is exactly what a v3/v4/v5 bolt is.  So a
								 * pre-v6 bolt and a v6 lexical-only bolt are the
								 * same thing to a reader, and dual-read is
								 * per-bolt -- the doclenstart pattern again.
								 *
								 * This field lands at offset 52, inside the four
								 * bytes of tail padding WeaveSegMeta already had
								 * for its double members, so sizeof() and hence
								 * the segs[] STRIDE do not change and neither does
								 * the offset of the metapage's `generation`.  See
								 * doc/specs/SEGMENT_FORMAT.md sect. 6: the spec
								 * predicted a stride change and was wrong.  The
								 * version bump is still required -- an older build
								 * would read those four bytes as padding and leak
								 * every chandesc page on merge -- and the versioned
								 * reader still exists, because the NEXT added field
								 * will not be free. */
} WeaveSegMeta;

/* Static asserts on the above live in src/am/am.c (weave_meta_from_page). */

/*
 * The backend view of a WEAVE_CHANDESC page.  Byte-identical to WeaveCdPage /
 * WeaveCdDesc in weave/chandesc.h, which carry the validator that both the
 * backend and the fuzz harness call; the asserts that keep the two in step are
 * in src/am/am.c.  Declared here because weave/am.h is the header of record for
 * what is on disk.
 */
typedef struct WeaveChannelDesc
{
	uint16		kind;			/* WeaveWeftKind (weave/chandesc.h); never 0 */
	uint16		attnum;			/* 1-based index attribute, or 0 if not per-attr */
	uint32		flags;			/* WEAVE_WEFT_F_* */
	BlockNumber root;			/* first page of the weft */
} WeaveChannelDesc;

typedef struct WeaveChanDescPageData
{
	uint32		magic;			/* WEAVE_CHANDESC_MAGIC */
	uint16		version;		/* WEAVE_CHANDESC_VERSION */
	uint16		nweft;			/* 1..WEAVE_MAX_WEFTS */
	uint32		reserved;		/* must be zero */
	WeaveChannelDesc weft[FLEXIBLE_ARRAY_MEMBER];
} WeaveChanDescPageData;

#define WeavePageGetChanDesc(page) \
	((WeaveChanDescPageData *) PageGetContents(page))

#define WEAVE_MAX_SEGMENTS 128	/* fits the metapage (~6KB of ~8KB); the size-
										 * tiered merge keeps the live count far below
										 * this, so it is only a safety backstop */

/* Max vacate+pack+truncate passes weave_vacuum makes to converge to the size
 * floor.  One pass reaches the floor in the common single-segment case (phase 1
 * vacates the live segment onto fresh high blocks so the freed pages form one
 * contiguous low free region >= live size; phase 2 packs the segment to the
 * front and truncates the freed high tail); the loop exits early once a pass
 * stops shrinking, so this is only a backstop against a pathological layout. */
#define WEAVE_VACUUM_MAX_PASSES 6

typedef struct WeaveMetaPageData
{
	uint32		magic;
	uint32		version;
	double		ndocs;			/* corpus N (all live segments + pending, minus tombstones) */
	double		sumdoclen;		/* corpus sum(doclen) -> avgdl */
	uint32		nsegments;		/* number of live segment descriptors */
	BlockNumber pendinghead;	/* first pending page, or InvalidBlockNumber */
	BlockNumber pendingtail;	/* last pending page, for O(1) append */
	uint32		npending;		/* number of pending (unmerged) documents */
	WeaveSegMeta segs[WEAVE_MAX_SEGMENTS];
	/*
	 * Directory generation: bumped on every change to the segment directory
	 * (add/merge/free of segments, and bulkdelete's livedocs-pointer swap).  A
	 * scan records this at its metapage snapshot and re-checks it before
	 * trusting results; if it changed, a concurrent merge may have freed +
	 * recycled pages the scan read from a now-stale descriptor, so the scan
	 * restarts from a fresh snapshot.  Placed AFTER segs[] so the on-disk offset
	 * of every existing field is unchanged (no format bump / REINDEX): an index
	 * written by an older build simply has whatever bytes were here, and only
	 * the *change* in this value across a scan matters, not its absolute value.
	 */
	uint32		generation;
} WeaveMetaPageData;

#define WeavePageGetMeta(page) \
	((WeaveMetaPageData *) PageGetContents(page))

/* a dictionary entry; term text is inline, length termlen */
typedef struct WeaveDictEntry
{
	uint32		termlen;
	uint32		df;				/* document frequency */
	uint32		max_tf;			/* max tf across postings (WAND impact bound) */
	uint32		firstoffset;	/* byte offset of the term's first block in firstposting */
	BlockNumber firstposting;	/* first posting page for this term */
	char		term[FLEXIBLE_ARRAY_MEMBER];
} WeaveDictEntry;

/*
 * Sparse block index over a segment's dictionary pages: one entry per dict
 * page, recording that page's FIRST term and its block number.  Entries are in
 * term order (dict pages are written in term order), so a term lookup binary-
 * searches the (small) index to the one dict page that could hold the term,
 * then scans just that page -- O(log P + 1) instead of scanning all P dict
 * pages.  This is the same point-lookup complexity an FST gives; prefix/range
 * scans still walk the dict chain from the located page.
 */
typedef struct WeaveDictIndexEntry
{
	BlockNumber blk;			/* dictionary page this entry points at */
	uint32		termlen;		/* length of that page's first term */
	char		term[FLEXIBLE_ARRAY_MEMBER];
} WeaveDictIndexEntry;

/* a posting: which heap tuple, its term frequency, and the document length.
 * When positions are decoded (want_positions), `pos` points at the posting's
 * `tf` ascending token positions (owned by the decoder's parallel array). */
typedef struct WeavePosting
{
	ItemPointerData tid;
	uint32		tf;
	uint32		doclen;			/* |D|: total tokens in the document */
	uint32	   *pos;			/* tf token positions, or NULL if not decoded */
} WeavePosting;

/*
 * Posting pages hold one or more fixed-size BLOCKS of up to WEAVE_BLOCK_SIZE
 * postings (the Lucene/Tantivy 128-doc block design).  Each block is a
 * WeaveBlockHdr followed by three FOR (frame-of-reference) bit-packed columns --
 * docid-gaps, tfs, doclens; docid gaps are relative to first_docid within the
 * block.  Per-block max_tf/min_doclen give block-max WAND a tight impact bound
 * (finer than per-page), and first_docid lets a cursor skip an entire block
 * whose docids are all below a target.  (The columns are already narrow -- tf
 * and doclen are small and docid gaps are tiny within a common term's dense
 * blocks -- so patched-FOR/PFOR outlier extraction was measured to save only
 * ~7% of the column bytes, under ~0.5% of the whole index, not worth the
 * hot-path decode complexity; plain FOR is kept.)
 * Posting pages hold one or more fixed-size BLOCKS of up to WEAVE_BLOCK_SIZE
 * postings (the Lucene/Tantivy 128-doc block design).  Each block is a
 * WeaveBlockHdr followed by three FOR (frame-of-reference) bit-packed columns --
 * docid-gaps, tfs, doclens; docid gaps are relative to first_docid within the
 * block.  When the index is built WITH (positions=on) a FOURTH, lazily-decoded
 * FOR column follows: the per-posting token positions, delta-coded within each
 * posting (reset at each posting boundary) and FOR-packed over the whole
 * block's Sum(tf) position values.  posbytelen in the header lets a reader that
 * does not need positions (plain BM25/AND/count) skip the whole column in one
 * add -- the same bytelen-skip the tf/doclen columns already use, so a
 * non-phrase query pays ~zero for positions existing.  A single posting's
 * positions are located by decoding the tf column (needed anyway) and walking
 * the prefix sum, so no on-disk per-posting offset directory is stored.
 */
#define WEAVE_BLOCK_SIZE 128

typedef struct WeaveBlockHdr
{
	uint32		count;			/* postings in this block (<= WEAVE_BLOCK_SIZE) */
	uint32		max_tf;			/* max tf in this block (block-max WAND bound) */
	uint32		min_doclen;		/* min |D| in this block (tightens block-max WAND) */
	uint32		first_docid_hi;
	uint32		first_docid_lo;
	uint32		bytelen;		/* byte length of the three FOR columns (docid|tf|doclen) */
	uint32		posbytelen;		/* byte length of the trailing positions FOR column
								 * (Sum tf deltas), or 0 when the index has no
								 * positions (WITH positions=off).  A reader that
								 * does not need positions skips this many bytes --
								 * the same bytelen-skip the tf/doclen columns use. */
} WeaveBlockHdr;

/*
 * A pending record: a not-yet-merged document stored verbatim on a pending
 * page.  The wdoc varlena follows the header inline (doclen bytes).  Pending
 * documents are searched directly at scan time and folded into a new segment by
 * a flush -- triggered by weave_merge() or automatically during VACUUM cleanup.
 */
typedef struct WeavePendingItem
{
	ItemPointerData tid;
	uint32		doclen;			/* byte length of the wdoc that follows */
	/* char wdoc[doclen] follows, MAXALIGN'd */
} WeavePendingItem;

/*
 * A trigram-index entry: a trigram hash and, inline, a serialized sparsemap of
 * the docids of documents containing at least one term with that trigram.  Used
 * to narrow fuzzy/regex candidates instead of scanning the whole index.
 *
 * The trigram index is inverted over the VOCABULARY, not the corpus: a trigram
 * maps to the set of dictionary term ordinals whose term contains it.  The
 * vocabulary is far smaller than the document set (Heaps' law), so these sets
 * are small and dense -- unlike docid sets, where a common trigram would cover
 * most of the corpus.  No trigrams are skipped, so the candidate union stays a
 * sound superset for fuzzy/regex.
 * Each entry is fixed-size; the term-ordinal sparsemap is a data-page blob.
 */
typedef struct WeaveTrgmEntry
{
	uint32		trgm;			/* trigram hash */
	uint32		smlen;			/* serialized sparsemap length in bytes */
	BlockNumber firstdata;		/* first WEAVE_TRGM_DATA page of the term-ord set */
} WeaveTrgmEntry;

/*
 * Metapage and channel-descriptor readers (src/am/am.c).  Exported because every
 * other translation unit of the access method needs them -- src/am/amsize.c and
 * src/am/amcheck.c predate task L1, ambuild.c/amvacuum.c/amscan.c arrived with
 * it.
 *
 * weave_meta_from_page() is the ONLY correct way to read a metapage: it
 * deserializes v3/v4/v5/v6 into the current in-memory struct.  Casting the page
 * to WeaveMetaPageData directly is the 1.5.0 upgrade bug.
 */
extern void weave_check_meta(Page page, Relation index);
extern void weave_meta_from_page(Page page, WeaveMetaPageData *out);
extern WeaveCdError weave_read_chandesc(Relation index, BlockNumber blk,
										WeaveChannelDesc *out, int max,
										int *nweft_out);

/*
 * The read-path wrapper: a corrupt descriptor page must produce a clean ERROR,
 * never a best-effort read that hands a caller a wild root block
 * (doc/CONVENTIONS.md decision 2, doc/specs/SEGMENT_FORMAT.md sect. 8 item 3).
 * Channel code reads descriptors through THIS; only weave_check() calls the
 * code-returning form, because it has to report and continue.
 */
static inline int
weave_chandesc_required(Relation index, BlockNumber blk,
						WeaveChannelDesc *out, int max)
{
	int			nweft = 0;
	WeaveCdError err = weave_read_chandesc(index, blk, out, max, &nweft);

	if (err != WEAVE_CD_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has a corrupt channel-descriptor page at block %u",
						RelationGetRelationName(index), blk),
				 errdetail("%s", weave_chandesc_errstr(err)),
				 errhint("REINDEX the index to rebuild it.")));
	return nweft;
}

/* The six amhandler scan callbacks (src/am/amscan.c).  A real translation unit
 * since task L1; it was #included into am.c before that. */
extern void weave_init_reloptions(void);
extern IndexScanDesc weave_beginscan(Relation r, int nkeys, int norderbys);
extern void weave_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						ScanKey orderbys, int norderbys);
extern int64 weave_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool weave_gettuple(IndexScanDesc scan, ScanDirection dir);
extern bool weave_canreturn(Relation index, int attno);
extern void weave_endscan(IndexScanDesc scan);

/* ===========================================================================
 * The access method's cross-translation-unit interface (task L1).
 *
 * WHY THIS SECTION EXISTS.  Until task L1, src/am/am.c was a ~7,300-line unity
 * build: it #included src/am/amscan.c, src/query/lev.c and src/pages/trgm_page.c
 * as text, so every helper could stay `static` and no interface had to be
 * written down.  That is why AGENTS.md carried a hard rule forbidding anyone from
 * adding those three files to OBJS, and why `make check-unity` existed: the
 * casual fix produces duplicate symbols.  The cost of the unity build was not
 * compile time, it was that phase Z (fuzzy/regex/prefix/n-gram) and phase V
 * (vector) both have to add page kinds, scan paths and check invariants to the
 * same file -- a permanent merge conflict between two channels that are
 * otherwise independent.
 *
 * L1 splits it into src/am/am.c (AM core, page/segment/metapage machinery),
 * src/am/ambuild.c (build, insert, segment writers, merge), src/am/amvacuum.c
 * (bulkdelete, cleanup, compaction, the maintenance SQL functions) and
 * src/am/amscan.c (scan), with src/query/lev.c and src/pages/trgm_page.c as
 * ordinary translation units.  This is where the seams are declared.
 *
 * HOW TO READ IT.  Everything below is grouped by the file that DEFINES it, and
 * every group says which files consume it.  A declaration here means exactly one
 * thing: this symbol lost `static` in order to cross a file boundary, and
 * nothing else about it changed.  It is not a public API, it is not a stable
 * interface, and it is not an invitation -- if a new consumer appears in a
 * different subsystem, that is a signal the seam is in the wrong place, not that
 * the declaration should be reused.
 *
 * Three kinds of thing live here rather than in a .c file:
 *
 *   1. `static inline` helpers (docid <-> TID, the maintenance lock trio).
 *      These are in the HEADER, still `static inline`, rather than exported from
 *      am.c, precisely so their linkage does NOT change: they are one or two
 *      instructions and every caller still inlines them exactly as it did
 *      inside the unity build.  weave_chandesc_required() above is the same
 *      pattern and predates L1.
 *   2. Types that two files must agree on byte-for-byte (the doclen sidecar's
 *      block header and cursor, the dictionary-stream record).  These were
 *      already shared -- the unity build just let them be shared implicitly.
 *   3. Prototypes for functions that lost `static`.
 * ======================================================================== */

/* --- huge-safe allocation (definitions used by am.c, ambuild.c,
 * src/pages/trgm_page.c; ci/check-alloc.sh keys on these names) ---------- */
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
/* --- docid <-> heap TID (used by every AM translation unit) -------------- */
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
/* --- maintenance serialization lock (used by ambuild.c and amvacuum.c) --- */
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
/* --- doclen sidecar: the on-page block header (written by ambuild.c,
 * read by am.c's cursor) -------------------------------------------------- */
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
/* --- doclen sidecar: the scan-path cursor and its relcache directory.
 * Types defined here because am.c owns the cursor and amscan.c embeds one per
 * (term, segment) plus one shared resident block per segment. ------------- */
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
	/*
	 * v5 whole-PAGE cache (follow-up to L17; bench/RESULTS_L17.md "Copy the
	 * whole page instead of the block").  A sidecar page holds ~31 128-doc
	 * blocks (~4000 docs); at rare/mid stride (~50-200 docids) a lookup's
	 * next docid usually falls on a DIFFERENT BLOCK of the SAME PAGE, and
	 * re-running ReadBuffer/LockBuffer/UnlockReleaseBuffer just to reach a
	 * block already sitting in shared buffers measured ~15,000 buffer hits
	 * for one ranked mid k=10 query. Copying the WHOLE page once per page
	 * change turns that into ~1 buffer touch per ~31 candidates instead of
	 * per 1, at the cost of one 8 KB memcpy instead of a few-hundred-byte
	 * one -- see weave_doclen_cursor_load_page().
	 *
	 * This is the SAME idea that was tried and reverted for v4 (see that
	 * function's history): a v4 block has to be FOR-unpacked and
	 * prefix-summed before it is usable, so copying ~31 blocks to answer one
	 * lookup was ~31x decode amplification for no benefit. v5's column is
	 * fixed-width addressable and is never decoded -- weave_for_get() reads
	 * packed bytes directly -- so a whole-page copy costs exactly one memcpy
	 * and nothing downstream gets bigger. The objection that killed this for
	 * v4 does not apply to v5, which is why this is safe now and was not
	 * safe at L17 time.
	 */
	BlockNumber pageblk;		/* page currently copied into `page`, or Invalid */
	unsigned char *page;		/* palloc'd copy of the whole sidecar page (BLCKSZ) */
	int			pagecap;		/* capacity of page (== BLCKSZ once allocated) */
	uint64		pagefirst;		/* first docid covered by the resident page */
	uint64		pagelast;		/* last docid covered by the resident page */

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
	 * we binary-search its packed bytes in place with weave_for_get().  `raw`
	 * and `rawbyte` point INTO the whole-page copy above -- there is no
	 * separate per-block copy any more -- so relocating to a different block
	 * of the same resident page is pointer arithmetic, not a memcpy.
	 * `docid`/`byte` stay unused for v5.
	 *
	 * A copy rather than a pin: load_page releases the buffer before returning,
	 * and holding a pin across executor calls to keep the page addressable
	 * would be a materially bigger change to the scan's locking story for no
	 * measured gain (buffer lookup was 1.9% of the scan).
	 */
	bool		isabs;			/* resident block is v5 (absolute offsets) */
	const unsigned char *raw;	/* packed docid column, pointing into `page` */
	const uint8 *rawbyte;		/* length bytes, inside `page` after the column */
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
/* --- dictionary streaming: the record and the pull iterator.  ambuild.c
 * defines the writers; src/pages/trgm_page.c drives the same iterator to build
 * the trigram index from the same term stream. -------------------------- */
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
/* --- A materialized TID set: amscan.c's currency, also produced by
 * src/pages/trgm_page.c's trigram candidate lookup. ---------------------- */
/* A materialized, sorted, duplicate-free set of TIDs. */
typedef struct TidSet
{
	ItemPointerData *tids;
	int			n;
} TidSet;
/*
 * src/am/am.c -- page allocation, page kinds, the segment directory, and the
 * doclen-sidecar read cursor.  Consumed by ambuild.c, amvacuum.c, amscan.c and
 * src/pages/trgm_page.c.
 *
 * weave_alloc_extend_only is a process-wide mode flag on the page allocator, not
 * a parameter: weave_new_buffer() consults it, and the three callers that need
 * pages to come off the END of the file rather than the free list
 * (weave_merge_all's low-bias end-of-build merge in ambuild.c, weave_merge_
 * segments' likewise, and weave_compact_to_one's relocation pass in amvacuum.c)
 * set it around a region and restore the previous value.  It was a file-scope
 * static in the unity build and is a global for exactly the same reason
 * everything else in this section is declared: it now crosses a file boundary.
 * Save-and-restore, not set-and-clear -- the regions nest.
 */
extern bool weave_alloc_extend_only;

extern void weave_alloc_begin(Relation index);
extern void weave_alloc_end(void);
extern Buffer weave_new_buffer(Relation index);
extern void weave_init_page(Page page, WeavePageKind kind);
extern void weave_free_page(Relation index, BlockNumber blk);
extern void weave_free_chain(Relation index, BlockNumber blk);
extern void weave_free_segment(Relation index, const WeaveSegMeta *seg);

extern void weave_init_metapage(Relation index);
extern void weave_meta_upcast_page(Page page);
extern bool weave_meta_add_segment(Relation index, const WeaveSegMeta *seg);
extern void weave_add_segment_with_room(Relation index, const WeaveSegMeta *seg);

extern void weave_attach_chandesc(Relation index, WeaveSegMeta *seg);

/*
 * The one posting decoder.  Every channel reads a term's postings through this:
 * ambuild.c's merge, amscan.c's boolean/phrase/ranked paths, amvacuum.c's
 * tombstone pass, and src/pages/trgm_page.c's candidate walk.  Kept in am.c
 * (with the FOR codec include it needs) rather than duplicated per caller.
 */
extern int weave_decode_term(Relation index, BlockNumber firstblk,
							 uint32 firstoff, uint32 df, WeavePosting **out,
							 uint32 **blockmax, bool want_positions,
							 uint32 **posarena, bool docids_only,
							 bool has_doclen_col);

extern void weave_doclen_cursor_init(WeaveDoclenCursor *c, Relation index,
									 BlockNumber start, WeaveDoclenDirCache *dc,
									 WeaveDoclenResident *res);
extern void weave_doclen_cursor_free(WeaveDoclenCursor *c);
extern uint32 weave_doclen_cursor_lookup(WeaveDoclenCursor *c, uint64 docid);
extern WeaveDoclenDirCache *weave_doclendir_cache(Relation index,
												  const WeaveMetaPageData *meta);

/* Reloption accessors.  Non-static only so ambuild.c and amscan.c can ask what
 * the index they were handed carries; WeaveOptions itself stays private to
 * am.c, which is the file that registers the reloption kind. */
extern bool weave_index_wants_positions(Relation index);
extern bool weave_index_wants_trigrams(Relation index);
extern bool weave_index_wants_doclen_sidecar(Relation index);

/*
 * src/am/ambuild.c -- ambuild/aminsert, the segment writers, and the size-tiered
 * merge.  weave_build/_buildempty/_insert are consumed by am.c (the amhandler
 * fills them in); the merge entry points are also consumed by amvacuum.c, which
 * is what VACUUM's cleanup and weave_merge() drive.
 */
extern IndexBuildResult *weave_build(Relation heap, Relation index,
									 struct IndexInfo *indexInfo);
extern void weave_buildempty(Relation index);
extern bool weave_insert(Relation index, Datum *values, bool *isnull,
						 ItemPointer ht_ctid, Relation heapRel,
						 IndexUniqueCheck checkUnique,
						 bool indexUnchanged,
						 struct IndexInfo *indexInfo);
extern bool weave_flush_pending(Relation index);
extern void weave_merge_segments(Relation index);
extern bool weave_merge_all(Relation index, bool try_parallel);
extern bool weave_merge_selected(Relation index, const uint32 *sel, uint32 nsel);

/*
 * src/am/amvacuum.c -- ambulkdelete/amvacuumcleanup and the size-floor
 * compaction.  weave_bulkdelete/_vacuumcleanup are consumed by am.c (amhandler);
 * the compaction pair is consumed by ambuild.c, because an end-of-build merge
 * finishes by compacting and truncating.
 */
extern IndexBulkDeleteResult *weave_bulkdelete(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats,
											   IndexBulkDeleteCallback callback,
											   void *callback_state);
extern IndexBulkDeleteResult *weave_vacuumcleanup(IndexVacuumInfo *info,
												  IndexBulkDeleteResult *stats);
extern bool weave_vacuum_compact(Relation index);
extern BlockNumber weave_truncate_free_tail(Relation index);

/*
 * src/pages/trgm_page.c -- the blob writer/reader (also used for the livedocs
 * tombstone blob, which is not a trigram thing but shares the byte-stream page
 * chain) and the trigram index.  Consumed by ambuild.c, amvacuum.c and amscan.c.
 */
extern BlockNumber weave_write_blob(Relation index, const uint8 *data, Size len);
extern uint8 *weave_read_blob(Relation index, BlockNumber blk, Size len);
extern BlockNumber weave_write_trigrams_iter(Relation index, DictNextFn next,
											 void *nstate);
extern bool weave_trgm_candidates(Relation index, BlockNumber trgmstart,
								  BlockNumber dictstart,
								  const char *term, int termlen,
								  int min_trigrams, bool is_regex,
								  bool has_doclen_col, TidSet *out);

/* amscan.c's only export to a non-scan file: src/pages/trgm_page.c normalizes
 * the candidate set it returns. */
extern void tidset_sort_uniq(TidSet *s);

#endif							/* WEAVE_AM_H */
