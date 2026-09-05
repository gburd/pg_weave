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
#include "storage/bufpage.h"
#include "storage/itemptr.h"

#define WEAVE_MAGIC			0x42324635	/* "B2F5" */
#define WEAVE_VERSION		4		/* v4: per-segment doclen sidecar (1 quantized
										 * byte/doc) replacing the per-posting doclen FOR
										 * column; v3 (inline doclen) still read.  v3:
										 * segmented layout + optional token positions. */
#define WEAVE_VERSION_DOCLEN_INLINE 3	/* oldest format we dual-read */
#define WEAVE_VERSION_DOCLEN_SIDECAR 4	/* first version with the doclen sidecar */
#define WEAVE_METAPAGE_BLKNO	0

/* page opaque flags */
#define WEAVE_META			(1 << 0)
#define WEAVE_DICT			(1 << 1)
#define WEAVE_POSTING		(1 << 2)
#define WEAVE_PENDING		(1 << 3)
#define WEAVE_TRGM			(1 << 4)	/* trigram directory page */
#define WEAVE_TRGM_DATA		(1 << 5)	/* trigram sparsemap blob page */
#define WEAVE_LIVEDOCS		(1 << 6)	/* per-segment tombstone bitmap page */
#define WEAVE_DICTINDEX		(1 << 7)	/* sparse block index over dict pages */
#define WEAVE_DOCLEN			(1 << 9)	/* per-segment doclen sidecar page (v4):
										 * a chain of 128-doc blocks, each a
										 * FOR-packed docid-gap column + one quantized
										 * length byte per doc, ordered by segment-local
										 * ascending docid.  Replaces the per-posting
										 * doclen column; scoring reads the byte and a
										 * precomputed 256-entry length-norm table. */
#define WEAVE_FREED			(1 << 8)	/* page freed & pending recycle: nextblk
										 * holds the free-time TransactionId (see
										 * weave_free_page / the recycle gate in
										 * weave_new_buffer).  Reusing nextblk (dead on
										 * an off-chain freed page) keeps the page
										 * opaque layout unchanged -- no format change,
										 * so existing indexes need no REINDEX.  A page
										 * freed by an older version lacks this flag and
										 * is treated as immediately recyclable. */

typedef struct WeavePageOpaqueData
{
	uint16		flags;
	uint16		unused;
	BlockNumber nextblk;		/* next page in a dict/posting/pending chain */
} WeavePageOpaqueData;

typedef WeavePageOpaqueData *WeavePageOpaque;

#define WeavePageGetOpaque(page) \
	((WeavePageOpaque) PageGetSpecialPointer(page))

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
} WeaveSegMeta;

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

/* scan functions (pg_weave_am_scan.c, #included into pg_weave_am.c) */
extern void weave_init_reloptions(void);
extern IndexScanDesc weave_beginscan(Relation r, int nkeys, int norderbys);
extern void weave_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						ScanKey orderbys, int norderbys);
extern int64 weave_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool weave_gettuple(IndexScanDesc scan, ScanDirection dir);
extern bool weave_canreturn(Relation index, int attno);
extern void weave_endscan(IndexScanDesc scan);

#endif							/* WEAVE_AM_H */
