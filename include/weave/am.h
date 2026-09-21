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

#include "weave/cgram.h"		/* Z8: the cgram weft's root-page layout */
#include "weave/chandesc.h"
#include "weave/pagebound.h"
#include "weave/pagekind.h"
#include "weave/surftrie.h"
/* WeaveDoc, for the pending-item cursor below.  weave/weave.h does not include
 * this header, so this is not a cycle -- unlike weave/vector.h, which does. */
#include "weave/weave.h"

#define WEAVE_MAGIC			0x42324635	/* "B2F5" */
#define WEAVE_VERSION		9		/* v9: a PENDING page carries each inserted
										 * row's vector, on a WEAVE_PK_PENDING_V9 page
										 * whose item layout differs from v8's (task
										 * V7, doc/GAPS.md G23).  The metapage is
										 * BYTE-IDENTICAL to v8 -- the bump is about
										 * REFUSAL, not parsing: a v8 .so does not know
										 * page kind 29, so it would fold such a page
										 * with the v8 item stride, hand
										 * weave_doc_is_valid() garbage, and silently
										 * WARN-and-skip documents the metapage has
										 * already counted into ndocs.  One index can
										 * hold pages of BOTH layouts (weave_insert()
										 * does not upcast the metapage), which is why
										 * the per-page discriminator exists as well as
										 * this word.
										 * v8: a bolt carries the VECTOR weft -- a
										 * WEAVE_PK_VMETA page naming a WEAVE_PK_VDIR
										 * block directory and a WEAVE_PK_VCODES strip
										 * chain, registered as a WEAVE_WK_VECTOR
										 * descriptor (task V7,
										 * doc/specs/VECTOR_CHANNEL.md sect. 7.1).
										 * The metapage is again BYTE-IDENTICAL to the
										 * previous generation, and the bump is again
										 * about FREEING rather than parsing: a v7
										 * .so reads the descriptor page fine and
										 * frees only the wefts it knows, so every
										 * merge under it would leak the whole weft --
										 * thousands of pages per bolt, not one.
										 * SEGMENT_FORMAT.md sect. 8 item 1
										 * generalizes it: a new weft kind needs a
										 * version bump even when it moves no field.
										 * v7: a bolt carries the FUZZY weft -- the
										 * LOUDS-Sparse SuRF trie over its vocabulary, on
										 * a WEAVE_PK_SURF page chain, registered as a
										 * WEAVE_WK_FUZZY descriptor.  The metapage is
										 * BYTE-IDENTICAL to v6 (no struct changed), so
										 * why bump at all: a v6 .so understands the
										 * descriptor page and would happily read such a
										 * bolt, but weave_free_segment() in that build
										 * frees only the wefts it knows, so every merge
										 * would LEAK the whole trie -- exactly the
										 * reasoning doc/specs/SEGMENT_FORMAT.md sect. 8
										 * item 2 used for v5 -> v6 ("that binary would
										 * read chandesc as padding and leak one page per
										 * merged bolt").  Refusing is correct.
										 * v6: every bolt SELF-DESCRIBES its wefts.
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
#define WEAVE_VERSION_SURF		7	/* first version writing the fuzzy (SuRF) weft */
#define WEAVE_VERSION_VECTOR	8	/* first version writing the vector weft */
#define WEAVE_VERSION_PENDING_VEC 9	/* first version whose pending items carry a wvec */

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
 * page.  The wdoc varlena follows the header inline (doclen bytes), then the
 * row's wvec (veclen bytes) if the index has a vector column.  Pending
 * documents are searched directly at scan time and folded into a new segment by
 * a flush -- triggered by weave_merge() or automatically during VACUUM cleanup.
 *
 * WHY THE VECTOR IS STORED RAW AND NOT PRE-QUANTIZED.  A code is ~8x smaller
 * (480 B against 3,848 B at 960-d/4-bit), which is a real cost paid on every
 * insert.  It is paid anyway, because quantizing at insert time would bake the
 * `bits` reloption into the pending buffer: an ALTER INDEX ... SET (bits = ...)
 * between the insert and the flush would leave a pending code whose width
 * disagrees with the segment being written, and a code cannot be re-quantized to
 * another width without reconstructing the vector first -- which recomputes the
 * (scale, norm) pair from a reconstruction, the one thing doc/specs/
 * VECTOR_CHANNEL.md forbids.  Storing the vector verbatim means a flush runs
 * PRODUCER 1 exactly as a build does: one quantization path, not two.
 */
typedef struct WeavePendingItem
{
	ItemPointerData tid;
	uint32		doclen;			/* byte length of the wdoc that follows */
	uint32		veclen;			/* byte length of the wvec after the wdoc; 0 for
								 * a NULL vector AND for an index with no vector
								 * column.  The two need no distinguishing: both
								 * leave the lane dead, and producer 1 is
								 * inactive in the second case anyway. */
	/* char wdoc[doclen], then at MAXALIGN(sizeof(hdr) + doclen): char wvec[veclen] */
} WeavePendingItem;

/*
 * The v8 layout, still on disk in any index that was written by a build before
 * WEAVE_VERSION_PENDING_VEC and has un-flushed pending documents.  Declared as a
 * struct rather than a bare `12` so the old stride is checked by the compiler
 * and readable by a human; see WEAVE_PK_PENDING_V9 in weave/pagekind.h for why
 * the two layouts are told apart by PAGE KIND and not by a version word.
 */
typedef struct WeavePendingItemV8
{
	ItemPointerData tid;
	uint32		doclen;
	/* char wdoc[doclen] follows, MAXALIGN'd */
} WeavePendingItemV8;

StaticAssertDecl(sizeof(WeavePendingItemV8) == 12,
				 "the v8 pending item header is on-disk ABI");
StaticAssertDecl(sizeof(WeavePendingItem) == 16,
				 "the pending item header is on-disk ABI");

/*
 * Bytes one pending item occupies, both layouts.  MAXALIGN(veclen) is ADDED to
 * an already-aligned offset rather than folded into one MAXALIGN of the total,
 * so that the vector itself starts aligned and veclen == 0 reduces to exactly
 * the doc-only stride.
 */
static inline Size
weave_pending_item_size(uint32 doclen, uint32 veclen)
{
	return MAXALIGN(sizeof(WeavePendingItem) + doclen) + MAXALIGN(veclen);
}

static inline Size
weave_pending_item_size_v8(uint32 doclen)
{
	return MAXALIGN(sizeof(WeavePendingItemV8) + doclen);
}

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
/* --- page-entry walk bounds guards (every AM translation unit) ----------- */
/*
 * Where do the variable-length entries a writer appended to `page` end?
 *
 * VALIDATE pd_lower AS AN INTEGER BEFORE FORMING A POINTER FROM IT.  Every
 * dict/trigram/doclen walk in this AM reads its end bound from the page header
 * of a page held under only BUFFER_LOCK_SHARE, and that value comes off disk:
 * a recycled, torn or corrupt page can report anything.  `page + pd_lower` for
 * an out-of-range pd_lower is undefined behaviour *at the point the pointer is
 * formed*, before anything is dereferenced -- so a walk written as the
 * pointer comparison `ptr < (char *) page + pd_lower` is already UB and a
 * sanitizer build will say so.  An implausible pd_lower is treated as an EMPTY
 * page (walk terminates immediately) rather than an error: these walks run in
 * VACUUM, cleanup and merge, and an ereport there is how an index becomes
 * permanently unvacuumable.
 *
 * THE ARITHMETIC ITSELF LIVES IN include/weave/pagebound.h, which has no
 * PostgreSQL dependency, so test/fuzz/fuzz_pagebound.c drives the same code this
 * runs instead of a transcription of it.  This function's whole job is to read
 * pd_lower in its proper backend context, hand it over as an integer, and turn
 * the returned offset back into a pointer.
 */
static inline char *
weave_page_entry_end(Page page)
{
	Size		contents = (Size) ((char *) PageGetContents(page) - (char *) page);

	return (char *) page +
		weave_page_entry_end_off((size_t) BLCKSZ, (size_t) contents,
								 ((PageHeader) page)->pd_lower);
}

/*
 * A forward cursor over one pending page's items, in EITHER layout.
 *
 * The two layouts live here and nowhere else.  A WEAVE_PK_PENDING_V9 page's
 * items have a 16-byte header with a veclen word and may carry a vector after
 * the document; a legacy WEAVE_PK_PENDING page's have a 12-byte header and never
 * do.  One index can hold both (see WEAVE_PK_PENDING_V9 in weave/pagekind.h).
 * It is in this header rather than in a .c file because there are TWO readers --
 * weave_flush_pending() folds pending items into a segment and the bitmap scan
 * matches them live -- and this arithmetic existing in both files independently
 * is exactly the shape of bug the consolidation prevents.
 */
typedef struct WeavePendingIter
{
	char	   *ptr;
	char	   *end;
	bool		v9layout;			/* page kind is WEAVE_PK_PENDING_V9 */
} WeavePendingIter;

typedef struct WeavePendingRec
{
	ItemPointer tid;
	WeaveDoc	doc;
	uint32		doclen;
	const void *vec;			/* NULL unless the item carries one */
	uint32		veclen;
} WeavePendingRec;

static inline void
weave_pending_iter_init(WeavePendingIter *it, Page page)
{
	it->ptr = (char *) PageGetContents(page);
	it->end = weave_page_entry_end(page);
	it->v9layout = WeavePageHasKind(page, WEAVE_PK_PENDING_V9);
}

/*
 * Next item, or false at the end of the item area -- and also false for the
 * first item whose own lengths do not fit inside that area, which is how a torn
 * or recycled page with a garbage doclen/veclen is stopped rather than followed
 * off the page.  Both callers read pending pages under only BUFFER_LOCK_SHARE
 * while a concurrent flush can free and an insert can recycle them, so this is
 * load-bearing and not belt-and-braces: without it the pointer advance runs off
 * the page (observed as a wild multi-gigabyte allocation under concurrent merge
 * plus ingestion).
 *
 * A record returned here is BOUNDED, not VALIDATED: the bytes are still
 * untrusted, and the caller runs weave_doc_is_valid() and weave_wvec_is_valid()
 * on them.  The two jobs are separate because they have different answers -- a
 * bad length means STOP READING THIS PAGE, a bad document means SKIP THIS ITEM.
 */
static inline bool
weave_pending_iter_next(WeavePendingIter *it, WeavePendingRec *rec)
{
	Size		hdrsz = it->v9layout ? sizeof(WeavePendingItem)
		: sizeof(WeavePendingItemV8);
	Size		stride;

	if (it->ptr + hdrsz > it->end)
		return false;

	rec->vec = NULL;
	rec->veclen = 0;

	if (it->v9layout)
	{
		WeavePendingItem *pi = (WeavePendingItem *) it->ptr;

		rec->tid = &pi->tid;
		rec->doclen = pi->doclen;
		rec->veclen = pi->veclen;
		stride = weave_pending_item_size(pi->doclen, pi->veclen);
		if (it->ptr + stride > it->end)
			return false;
		rec->doc = (WeaveDoc) (it->ptr + sizeof(WeavePendingItem));
		if (pi->veclen > 0)
			rec->vec = it->ptr +
				MAXALIGN(sizeof(WeavePendingItem) + (Size) pi->doclen);
	}
	else
	{
		WeavePendingItemV8 *pi = (WeavePendingItemV8 *) it->ptr;

		rec->tid = &pi->tid;
		rec->doclen = pi->doclen;
		stride = weave_pending_item_size_v8(pi->doclen);
		if (it->ptr + stride > it->end)
			return false;
		rec->doc = (WeaveDoc) (it->ptr + sizeof(WeavePendingItemV8));
	}

	it->ptr += stride;
	return true;
}

/*
 * How many bytes may a blob reader copy out of `page`, when it is `off` bytes
 * into a blob of `len` bytes spread over a page chain?
 *
 * Both bounds -- the bytes this page actually carries, and the bytes the
 * destination still wants -- are applied by weave_page_blob_chunk_len(), so no
 * caller can apply only one.  Applying only the second is doc/GAPS.md G22: it
 * bounds the destination across the WHOLE CHAIN and therefore says nothing about
 * this page, which is why an underflowed `avail` clamped against it produced a
 * plausible length and an out-of-bounds READ.
 */
static inline Size
weave_page_blob_chunk(Page page, Size len, Size off)
{
	Size		contents = (Size) ((char *) PageGetContents(page) - (char *) page);

	return (Size) weave_page_blob_chunk_len((size_t) BLCKSZ, (size_t) contents,
											((PageHeader) page)->pd_lower,
											(size_t) len, (size_t) off);
}

/*
 * Does a dictionary entry starting at `de` fit within a page ending at `end`?
 *
 * Dictionary pages are read under only BUFFER_LOCK_SHARE.  A concurrent
 * merge/vacuum can free a segment's pages while a concurrent insert recycles
 * and overwrites them, so a scan that snapshotted the segment directory before
 * that change can read a recycled page mid-walk.  If de->termlen is then
 * garbage, the entry stride and the term-compare run past the page
 * (out-of-bounds read -> SIGSEGV), a decoded df can be a multi-gigabyte
 * "invalid memory alloc request size", and a *count* accumulated by the walk
 * can size an allocation from pure garbage.  Every walk must confirm the entry
 * header AND its term bytes fit before trusting de->termlen; on a miss it stops
 * the page walk (a bounded incomplete result), and the scan's generation
 * re-check then detects the stale read and restarts.
 *
 * The stride these guards license, MAXALIGN(offsetof(..., term) + termlen), is
 * always >= MAXALIGN(offsetof(..., term)) > 0, so a guarded walk cannot spin.
 */
static inline bool
weave_dict_entry_fits(const WeaveDictEntry *de, const char *end)
{
	const char *t = (const char *) de + offsetof(WeaveDictEntry, term);

	return t <= end && t + de->termlen <= end;
}

/* Same contract for the sparse dict *index* entries walked by a term seek.
 * Unguarded, a garbage termlen here both oversteps the page and yields a
 * garbage `blk` that the seek then reads as if it were a dictionary page. */
static inline bool
weave_dictindex_entry_fits(const WeaveDictIndexEntry *ie, const char *end)
{
	const char *t = (const char *) ie + offsetof(WeaveDictIndexEntry, term);

	return t <= end && t + ie->termlen <= end;
}

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
 * weave_merge, weave_vacuum, a build's finalize, and the insert path's
 * full-directory merge) takes it blocking; opportunistic maintenance (the
 * insert-triggered tiered merge) takes it CONDITIONALLY and simply skips when a
 * cleanup is already running -- another writer or the next insert/vacuum will
 * compact, so nsegments still stays bounded.
 *
 * Blocking and conditional differ in whether the caller has an alternative.  The
 * opportunistic merge does: skipping it costs a slightly longer segment
 * directory.  weave_add_segment_with_room() does not: its alternative is to
 * refuse the INSERT, which is the outage that function exists to prevent.
 *
 * The lock is re-entrant in the ordinary heavyweight sense -- a caller that
 * already holds it (weave_flush_pending -> weave_add_segment_with_room) gets it
 * again for free -- so a nested acquisition is a reference count, not a wait.
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
 * Every merger must run under the maintenance mutex above -- or on an index no
 * other backend can reach, which for this AM means AccessExclusiveLock (plain
 * CREATE INDEX, REINDEX, weave_vacuum).
 *
 * ENFORCED RATHER THAN DOCUMENTED, because the comment form of this rule was
 * already in the tree and two callers still did not follow it: the insert path's
 * full-directory merge (weave_add_segment_with_room) and CREATE INDEX
 * CONCURRENTLY's weave_build_finalize, which holds only
 * ShareUpdateExclusiveLock and so can run beside an autovacuum merge on the same
 * index.  The sibling project found its second site the same way -- by adding
 * this check, not by reading the code again.
 *
 * elog(ERROR), not Assert(): the gate that matters is a release build, and an
 * unserialized merger's symptom (two mergers handing out the same block once
 * merges reuse freed pages) is a hang, which is the single hardest failure to
 * attribute after the fact.  ERRCODE_INTERNAL_ERROR is right here -- no client
 * can do anything about it and reaching it is a programming error, which is
 * exactly what elog is for.
 */
static inline void
weave_assert_merge_serialized(Relation index)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag, index->rd_lockInfo.lockRelId.dbId,
					 index->rd_lockInfo.lockRelId.relId, WEAVE_METAPAGE_BLKNO);
	if (unlikely(!LockHeldByMe(&tag, ExclusiveLock, false) &&
				 !CheckRelationLockedByMe(index, AccessExclusiveLock, true)))
		elog(ERROR, "pg_weave: merge entered without the maintenance mutex on index \"%s\"",
			 RelationGetRelationName(index));
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
 * a parameter: weave_new_buffer() consults it, and the caller that needs pages to
 * come off the END of the file rather than the free list
 * (weave_compact_to_one's relocation pass in amvacuum.c) sets it around a region
 * and restores the previous value.  It was a file-scope static in the unity build
 * and is a global for exactly the same reason everything else in this section is
 * declared: it now crosses a file boundary.  Save-and-restore, not
 * set-and-clear -- the regions nest.
 *
 * weave_alloc_no_fsm is the other half of SNAPSHOT allocation (see
 * weave_alloc_snapshot_enter below): it suppresses the live-FSM fallback, so the
 * only reuse is from the snapshot gathered at scope entry.
 */
extern bool weave_alloc_extend_only;
extern bool weave_alloc_no_fsm;

/*
 * Saved allocator state for one nested allocation scope.  The allocator's state
 * is a handful of file-scope variables in am.c (the low-free snapshot and the two
 * mode flags), and the regions nest, so a scope saves the whole of it rather than
 * one flag.  Opaque to callers except as a stack variable.
 */
typedef struct WeaveAllocScope
{
	BlockNumber *lowfree;
	int			lowfree_n;
	int			lowfree_i;
	bool		extend_only;
	bool		no_fsm;
	bool		giveup;			/* snapshot abandoned by the bounded probe */
	int			probe_at_entry;
	uint64		reuse_at_entry;
} WeaveAllocScope;

/*
 * Enter SNAPSHOT allocation: gather the free list ONCE, now, and for the rest of
 * the scope hand out only from that snapshot, then extend.  Never re-consult the
 * live FSM.  Leave with weave_alloc_scope_leave(), which restores the enclosing
 * scope's state.
 *
 * THIS IS WHAT MAKES REUSE SAFE INSIDE A MERGE LOOP, and it replaced the
 * extend-only mode that made it merely impossible.  The hazard is precise: a
 * committed merge frees its input pages, and if the NEXT merge in the same loop
 * took one of those blocks for its output while a reader of ours still threads
 * through it (an on-page nextblk pointing into a rewritten or past-EOF block),
 * the result is a wrong read or a SIGBUS.  Extend-only is exact against that and
 * over-approximates badly: it also refuses pages freed by EARLIER calls, already
 * committed, with no reader of ours anywhere near them -- which on a body index
 * over term-rich documents is nearly all of them, because every oversized
 * document mints a segment and merges, so the merge is the dominant writer.
 *
 * A snapshot taken BEFORE this call frees anything cannot contain a page this
 * call goes on to free, so merge N cannot hand its freed pages to merge N+1 by
 * construction, while pages from previous calls are in the snapshot and are
 * reused.  weave_page_recyclable() still gates every candidate.
 *
 * Two preconditions, both held by every caller today and neither optional:
 *   - the caller holds the maintenance mutex (weave_maintenance_lock), so no
 *     other backend frees, merges, compacts or TRUNCATES this index while the
 *     snapshot is in hand.  A truncation would leave the snapshot holding blocks
 *     past EOF.
 *   - no parallel worker of this operation is allocating concurrently.  The
 *     allocator is backend-local, so the leader's snapshot does not remove a
 *     block from the FSM that a worker can then also take.  weave_merge_all()
 *     therefore enters the scope AFTER its parallel pass, not around it.
 */
extern void weave_alloc_snapshot_enter(Relation index, WeaveAllocScope *saved);
extern void weave_alloc_scope_leave(const WeaveAllocScope *saved);

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

/*
 * Attach a descriptor page to a just-written bolt.  Call AFTER every other chain
 * of the bolt is written, so every root is known.  `surfroot` and `vecroot` are
 * InvalidBlockNumber when the bolt carries no such weft, which is what makes an
 * absent weft cost zero bytes -- including its descriptor slot.
 */
extern void weave_attach_chandesc(Relation index, WeaveSegMeta *seg,
								  BlockNumber surfroot, BlockNumber vecroot,
								  BlockNumber cgramroot);

/* ---------------------------------------------------------------------------
 * The cgram weft: corpus byte trigrams -> docids (task Z8)
 *
 * See include/weave/cgram.h for what the channel is and why it is shaped like
 * the lexical weft.  These are the AM half: the root page that names the weft's
 * three chains, the reader that resolves it, and the free path.
 *
 * Declared here rather than as an extern at the top of a .c file because the
 * seam is real: ambuild.c writes the root, am.c owns the page and the free path,
 * amscan.c reads it, amcheck.c walks it and amsize.c counts it.  AGENTS.md hard
 * rule 5's replacement says a symbol that has to cross files is declared here
 * with a reason.
 * ------------------------------------------------------------------------- */

/* The resolved weft, as read from its WEAVE_PK_CGRAM root page. */
typedef struct WeaveCgramWeft
{
	BlockNumber root;
	BlockNumber dictstart;
	BlockNumber dictindexstart;
	BlockNumber postingstart;
	uint32		nterms;
} WeaveCgramWeft;

extern BlockNumber weave_write_cgram_root(Relation index, BlockNumber dictstart,
										  BlockNumber dictindexstart,
										  BlockNumber postingstart,
										  uint32 nterms);

/*
 * Which block is this bolt's cgram weft rooted at, or InvalidBlockNumber when it
 * carries none -- a pre-Z8 bolt, an index with no gram_ops column, or a bolt
 * whose cgram weft the writer refused to emit.  Not an error: ABSENT IS SAFE on
 * this channel (the route then treats every document in the bolt as a candidate
 * and the mandatory recheck sorts it out), and INCOMPLETE is not, which is why
 * the writer omits the weft entirely rather than writing part of one.
 */
extern BlockNumber weave_cgram_weft_root(Relation index, const WeaveSegMeta *seg);

/*
 * Open the root page at `root`.  Returns false with *why set to a constant
 * string when the page is out of bounds, uninitialized, the wrong kind, or fails
 * its magic/version/reserved/bounds checks.  Never throws: weave_check() must
 * REPORT a bad weft and a scan must be free to IGNORE it (absent is safe), so
 * neither behaviour may be baked in here.
 */
extern bool weave_cgram_weft_open(Relation index, BlockNumber root,
								  WeaveCgramWeft *out, const char **why);

/* Free the weft's root page and all three of its chains.  Like the vector weft,
 * the descriptor names only the root, so a descriptor-driven free that called
 * weave_free_chain() on it alone would reclaim ONE page and leak the whole
 * dictionary and every posting page -- the doc/GAPS.md G24 failure mode. */
extern void weave_cgram_free_weft(Relation index, BlockNumber root);

/* ---------------------------------------------------------------------------
 * The fuzzy weft: the SuRF trie over the bolt vocabulary (task Z3)
 *
 * The image itself is format-defined and backend-independent
 * (include/weave/surftrie.h, src/query/surftrie.c).  These three are the AM half:
 * the page chain it lives on, and the two ways to get it back.
 *
 * WHY NOT weave_write_blob().  doc/specs/FUZZY_CHANNEL.md sect. 3.3 said "the AM
 * lays it on a WEAVE_PK_SURF page chain with the existing blob writer", which
 * cannot be done: weave_write_blob() hard-codes WEAVE_PK_TRGM_DATA (that is
 * exactly why the livedocs blob lands on trigram-data pages and
 * WEAVE_PK_LIVEDOCS has no writer -- see amcheck.c).  Reusing it would have put
 * the trie on pages whose kind says "trigram data", and weave_check() and
 * weave_index_size_detail() would then have had no way to tell the two apart.
 * Worse, weave_read_blob() validates NO page kind at all: it follows nextblk and
 * trusts pd_lower.  For a structure whose whole job is to be a filter that never
 * produces a false negative, the reader must refuse bytes that are not
 * demonstrably its own.
 *
 * WHY THERE IS NO LENGTH FIELD ANYWHERE.  The chain's pages carry exactly the
 * image bytes and nothing else, so the image length is the sum of the pages'
 * payloads -- which weave_read_surf() computes by walking the chain BEFORE it
 * allocates.  The alternative (a length in the descriptor, or a per-chain
 * header) is a second source of truth for a number the bytes already determine,
 * and weave_surftrie_open() rejects any image whose declared counts disagree with
 * its length, so a torn chain is caught with no extra field to keep in step.
 * It also means the allocation is bounded by real relation pages rather than by
 * a count read out of a possibly-corrupt header.
 * ------------------------------------------------------------------------- */

/* Image bytes one WEAVE_PK_SURF page carries.  In am.h rather than in am.c
 * because weave_surf_stats() reports the page count and must derive it from the
 * same constant the writer used -- two independent expressions for "how many
 * pages does this image take" is how a report starts disagreeing with the
 * relation. */
#define WEAVE_SURFPAGE_PAYLOAD \
	(BLCKSZ - (int) MAXALIGN(SizeOfPageHeaderData) - (int) MAXALIGN(sizeof(WeavePageOpaqueData)))

extern BlockNumber weave_write_surf(Relation index, const uint8 *img, Size len);

/*
 * Read the image on the chain rooted at `root` into a palloc'd buffer.  Returns
 * NULL and sets *detail to a constant explanatory string on any page-level
 * problem (out of bounds, uninitialized, wrong kind, cyclic chain, empty).
 * Does NOT validate the image -- that is weave_surftrie_check()'s job, and the
 * split is deliberate: weave_check() must REPORT a bad image as a violated
 * invariant while a scan must THROW, so neither half may ereport on its own.
 */
extern uint8 *weave_read_surf(Relation index, BlockNumber root, Size *len_out,
							  const char **detail);

/*
 * The scan-side loader: resolve the bolt's WEAVE_WK_FUZZY descriptor, read the
 * chain, and open+validate the image.  Returns false when the bolt carries no
 * fuzzy weft (a pre-v7 bolt, which is not an error); ereports
 * ERRCODE_INDEX_CORRUPTED when it carries one that does not validate.  *img
 * receives the palloc'd buffer the returned handle aliases, so the caller frees
 * it when done -- WeaveSurfTrie holds pointers INTO the image and copies
 * nothing.
 */
extern bool weave_surf_load(Relation index, const WeaveSegMeta *seg,
							WeaveSurfTrie *t, uint8 **img, Size *len);

/*
 * THE RESIDENT CONSULT (Z4 part 2), and the reason weave_surf_load() above still
 * exists next to it.
 *
 * weave_surf_load() pays the whole image every call: ~5.52 B/term, about 11 MB
 * per bolt at a 2M-term vocabulary, with no reuse.  That is affordable for a
 * diagnostic function (weave_surf_stats) and for amcheck, and it is NOT
 * affordable for the per-query consults Z4 part 3, Z5 and Z6 are built on -- none
 * of which is even measurable while one consult costs a whole-image load.  So the
 * loader stays as the "I want to own these bytes" entry point and this is the
 * "I want to look at the trie" one.
 *
 * `generation` is the caller's metapage snapshot generation, the third part of
 * the cache key, and it must come from the SAME snapshot `seg` came from -- that
 * is what makes a cached image safe to serve.  A bolt is immutable once written,
 * but freed pages ARE recycled, so a root block can later belong to a different
 * bolt; every path that frees a bolt's pages bumps `generation` first (am.c's
 * weave_meta_add_segment, ambuild.c's two merge commits, amvacuum.c's livedocs
 * rewrite), so within one generation (relfilenode, root) -> image bytes is a
 * function.
 *
 * LIFETIME RULE, and it is the whole of the danger here: WeaveSurfTrie holds
 * pointers INTO the image and copies nothing, so *t aliases bytes the cache owns.
 * The trie is valid until this backend's NEXT weave_surf_consult() call, and no
 * longer.  A caller that wants it for longer must take the owned path.
 *
 * *owned is set when the caller must pfree the image itself -- the cache is
 * disabled (pg_weave.surf_cache_mb = 0) or the image is larger than the whole
 * budget, in which case there is nothing to reuse and caching it would evict
 * everything else to hold something that cannot be kept.  When *owned is NULL the
 * caller must NOT free anything.  Returns false exactly when weave_surf_load()
 * does: the bolt carries no fuzzy weft.
 */
extern bool weave_surf_consult(Relation index, const WeaveSegMeta *seg,
							   uint32 generation, WeaveSurfTrie *t,
							   Size *len, uint8 **owned);

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

/* The vector code width for this index's next weft, from the `bits` reloption.
 * Here rather than in weave/vector.h because it reads WeaveOptions, which is
 * private to src/am/am.c, and ambuild.c is its only caller. */
extern int	weave_index_vec_bits(Relation index);

/*
 * The distance metric this index's next weft is written with, from the `metric`
 * reloption -- a WeaveMetric value, spelled int here because weave/am.h must not
 * depend on weave/vector.h.
 *
 * THROWS for a metric the vector channel cannot serve (cosine, l1), and it throws
 * from HERE rather than from the reloption validator on purpose: a value is only
 * unserviceable because no bound formulation exists for it
 * (doc/specs/VECTOR_CHANNEL.md sect. 8b), which is a property of the channel and
 * not of the catalog, and the build is the first place that property is actually
 * needed.  The refusal therefore lands on CREATE INDEX, which is where a user can
 * still choose differently, instead of on the first query against a finished
 * index.
 */
extern int	weave_index_vec_metric(Relation index);

/*
 * THE VECTOR CHANNEL'S ORDER BY STRATEGY NUMBER (task F7).
 *
 * `<=>` on (wvec, wvec), added to the wvec_weave_ops family as an ORDER BY member
 * by sql/pg_weave--0.14.0--0.15.0.sql.  1, and the number is a decision rather than
 * the next free integer:
 *
 *	 - STRATEGY NUMBERS ARE SCOPED TO AN OPERATOR FAMILY, not to the access method.
 *	   sk_strategy is resolved against the family of the index COLUMN the key was
 *	   matched to, so gram_ops's `@~` and wdoc_lex_ops's `@@@` are both strategy 1
 *	   and are different operators.  amscan.c's weave_rescan() carries the full
 *	   argument for why that is a correctness hazard and not bookkeeping: the number
 *	   is the only thing distinguishing a wquery argument from a text one, and
 *	   DatumGetWQuery() on the wrong datum reads a varlena header as a WeaveQuery
 *	   and walks garbage.
 *	 - SO WHY NOT 2 OR 3, which are equally unused inside wvec_weave_ops (the family
 *	   has no members at all -- 0.7.0--0.8.0 created it STORAGE-only).  Because
 *	   wdoc_lex_ops already uses 2 for `<=>` (BM25 distance) and 3 for `<@>` (edit
 *	   distance) as ORDER BY members, and weave_rescan() dispatches ORDER BY keys on
 *	   sk_strategy.  Picking 2 or 3 would make one number mean two different
 *	   order-by operators over two different argument types in one access method,
 *	   and the dispatch would then depend entirely on getting the attribute check
 *	   right in every branch forever.  1 is used by no ORDER BY member anywhere in
 *	   this access method, so a vector ordering key is unambiguous on its strategy
 *	   alone -- and weave_rescan() checks the attribute as well, belt and braces.
 *	 - IT HAS TO BE IN 1..amstrategies (3).  ALTER OPERATOR FAMILY validates the
 *	   number against the access method's amstrategies, so "use 11 and avoid the
 *	   question" is not available; include/weave/cgram.h records the same constraint.
 *
 * The other two live in include/weave/edist.h with the `<@>` machinery they were
 * added for.  This one is here because it belongs to no channel header: the thing
 * that reads it is the AM's key dispatch, weave_index_layout() below is the other
 * half of that dispatch, and weave/am.h must not depend on weave/vector.h.
 */
#define WEAVE_STRAT_VEC_DISTANCE	1

/*
 * Which index column feeds which channel.
 *
 * Until task V7 the access method was single-attribute: every write and recheck
 * path read `values[0]` and `isnull[0]` and assumed a `wdoc`.  There were exactly
 * four such sites (the build callback and weave_insert in ambuild.c, the scan-side
 * recheck in amscan.c, and the planner's first-column match in customscan.c); the
 * other `values[0]` uses in the AM are tuplestore output arrays and are unrelated.
 *
 * THE DISCRIMINATOR IS THE OPCLASS, NOT THE COLUMN TYPE.  A type-keyed mapping
 * works today (`wdoc` is lexical, `wvec` is vector) and stops working at Z4/Z8:
 * doc/specs/FUZZY_CHANNEL.md declares the corpus-n-gram channel as
 * `USING weave (sku gram_ops)` over an ordinary text column, so two different weft
 * kinds will share one input type.  Keyed on the opclass, adding a channel is one
 * row in weave_opfamily_kinds[] in src/am/am.c.
 *
 * The key within that table is the operator FAMILY name (`pg_opfamily.opfname`),
 * for two reasons.  Not an OID, because the extension is `relocatable = true` and an
 * OID would have to be resolved by name anyway.  The family rather than the class,
 * because the relcache caches `rd_opfamily[]` per index column and does not cache
 * opclass OIDs at all -- `pg_index.indclass` is a varlena attribute, reachable only
 * by deforming the catalog tuple.  CREATE OPERATOR CLASS makes an implicit family of
 * the same name, so the registry's entries are the names users write.
 *
 * weave_validate() rejects a family it does not know, so a stray third-party opclass
 * on this access method is reported by `amvalidate()` (which core's opr_sanity and
 * sql/vecindex.sql both call) instead of misrouting an attribute at build time.
 */
typedef struct WeaveIndexLayout
{
	int			nkeys;			/* key attributes (INCLUDE columns excluded; the
								 * AM sets amcaninclude = false, so they are equal) */
	uint16		kind[INDEX_MAX_KEYS];	/* WeaveWeftKind of each key attribute */
	AttrNumber	lexattno;		/* 1-based index attnum of the lexical column;
								 * never 0 -- weave_index_layout() throws if the
								 * index has no lexical column, because every
								 * write path builds the docid space from it */
	AttrNumber	vecattno;		/* 1-based index attnum of the vector column, or
								 * 0 if the index has none */
	AttrNumber	cgramattno;		/* 1-based index attnum of the cgram (text,
								 * gram_ops) column, or 0 if the index has none.
								 * Resolved here and not inferred from the column
								 * TYPE for the reason this whole struct's block
								 * comment gives: the discriminator is the opclass.
								 * A text column is not by itself a cgram column --
								 * `USING weave (sku gram_ops)` is the request. */
} WeaveIndexLayout;

extern void weave_index_layout(Relation index, WeaveIndexLayout *out);

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

/* Non-static so amvacuum.c can ask it before starting a relocation pass: the
 * page-recycle predicate it wraps is static in am.c (allocator-private), and the
 * answer decides whether that pass can pack into the space it frees or can only
 * extend the relation.  See weave_vacuum_compact(). */
extern bool weave_any_free_page_recyclable(Relation index);

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
extern bool weave_trgm_ordinals(Relation index, BlockNumber trgmstart,
								uint32 trgm, uint64 **ords, int *nords);
extern bool weave_trgm_candidates(Relation index, BlockNumber trgmstart,
								  BlockNumber dictstart,
								  const char *term, int termlen,
								  int min_trigrams,
								  bool has_doclen_col, TidSet *out);

/* amscan.c's only export to a non-scan file: src/pages/trgm_page.c normalizes
 * the candidate set it returns. */
extern void tidset_sort_uniq(TidSet *s);

/* ---------------------------------------------------------------------------
 * F6: the lexical channel's shuttle, over one query TERM's posting cursor
 *
 * WHY THESE DECLARATIONS ARE HERE, which AGENTS.md hard rule 5 requires an
 * answer to.  Ranked lexical retrieval is a Broder/BlockMax-WAND over
 * WandCursor -- src/am/amscan.c's private per-(term, segment) cursor -- while the
 * fused core (src/am/fuse.c) consumes only WeaveShuttles.  That mismatch is the
 * scope discovery in doc/specs/FUSED_TOPK.md sect. 7a (3), and the fix is glue:
 * the cursor already carries the current docid, the decoded block (so blkend is
 * its last docid), the per-block bound inputs and the term-wide ceiling, so no
 * on-disk field changes.
 *
 * The glue lives in src/query/lexshuttle.c, beside src/query/gate.c and
 * src/query/edist.c, because it is a channel face rather than scan machinery.
 * It therefore has to reach a cursor it must NOT be able to take apart, so
 * WandCursor stays an incomplete type everywhere except amscan.c and the five
 * calls a shuttle needs are declared here, in the file that owns the seam,
 * instead of as externs at the top of a .c file.  Nothing else in the tree may
 * use them; a second traversal over a posting cursor is a second thing to keep
 * in step with the format.
 *
 * One shuttle per TERM, not per query: include/weave/channel.h says a channel
 * "with many concurrent shuttles (one per query term, typically)" shares one
 * vtable, and one shuttle per term is what lets a multi-term BM25 query become
 * several scored channels under a single fused threshold instead of a sub-scan
 * with its own.
 *
 * WeaveShuttle is spelled `struct WeaveShuttle` below rather than through its
 * typedef because include/weave/channel.h includes THIS header, so this one
 * cannot include channel.h, and repeating channel.h's typedef would be a
 * duplicate typedef (legal only from C11).  The tag is the same type.
 * ------------------------------------------------------------------------- */
typedef struct WandCursor WandCursor;

/* The cursor's current docid and the last docid of its decoded block, without
 * moving it: what the shuttle publishes as (cur, blkend) at begin() time.  The
 * cursor must already be primed. */
extern uint64 weave_wand_cursor_tell(WandCursor *c, uint64 *blkend);

/* (C1) Advance to the first posting with docid >= target, reporting the new
 * position and the new block end together, because a seek that does not
 * republish blkend leaves the bound describing the previous block.  Returns
 * UINT64_MAX when the cursor is exhausted.  Does NOT itself refuse a backward
 * target -- the WAND tolerates one; the shuttle refuses it, per channel.h's
 * "NOT IDEMPOTENT" note. */
extern uint64 weave_wand_cursor_seek(WandCursor *c, uint64 target,
									 uint64 *blkend);

/* (C2)+(C3) The block bound from the block header values the cursor already
 * holds: arithmetic only, no buffer read. */
extern double weave_wand_cursor_block_max(WandCursor *c);

/* (C4) The exact contribution at the current posting.  May read a buffer: on a
 * v4 segment the doclen comes from the cursor's own doclen-sidecar cursor. */
extern double weave_wand_cursor_contrib(WandCursor *c);

/* The term-wide ceiling, i.e. WeaveShuttle.maxscore.  Static per cursor. */
extern double weave_wand_cursor_max_contrib(WandCursor *c);

/*
 * Wrap one PRIMED cursor as a scored WeaveShuttle.  The cursor is BORROWED: the
 * shuttle never frees its block buffer, its doclen cursor or its tombstone map,
 * because the ranked scan that built the cursor array already owns all three and
 * two owners is how a double pfree() happens.  end() drops only the shuttle's
 * own context.
 *
 * `kind` is not a parameter: a posting cursor is always WEAVE_CH_LEXICAL.
 * `required` is false -- this is a SCORED channel, and per (C5) and
 * FUSED_TOPK.md sect. 7a it is a field the channel sets rather than something
 * inferred from the kind.
 */
extern struct WeaveShuttle *weave_lex_shuttle_begin(WandCursor *c,
													MemoryContext cxt);
extern void weave_lex_shuttle_end(struct WeaveShuttle *s);

#endif							/* WEAVE_AM_H */
