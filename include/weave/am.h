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
 * Metapage and channel-descriptor readers (src/am/am.c).  Exported because
 * src/am/amsize.c and src/am/amcheck.c are separate translation units -- am.c is
 * already a 7,000-line unity build of four files (AGENTS.md rule 5) and task L1
 * is to shrink it, not to grow it.
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

/* scan functions (pg_weave_am_scan.c, #included into pg_weave_am.c) */
extern void weave_init_reloptions(void);extern IndexScanDesc weave_beginscan(Relation r, int nkeys, int norderbys);
extern void weave_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
						ScanKey orderbys, int norderbys);
extern int64 weave_getbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool weave_gettuple(IndexScanDesc scan, ScanDirection dir);
extern bool weave_canreturn(Relation index, int attno);
extern void weave_endscan(IndexScanDesc scan);

#endif							/* WEAVE_AM_H */
