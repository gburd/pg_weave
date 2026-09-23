/*-------------------------------------------------------------------------
 *
 * vector.h
 *		The weave vector channel: wvec type, page layouts, code-scan shuttle.
 *
 * This header is the backend-facing half of the vector channel.  The codec
 * itself is backend-independent and lives in weave/quantize.h; the graph is in
 * weave/graph.h.  Read doc/specs/VECTOR_CHANNEL.md before changing anything
 * here, and weave/channel.h before touching the shuttle.
 *
 * PAGE-KIND ALLOCATION.  The vector channel's four page kinds are NOT bits.
 * weave/am.h bits 0-9 of WeavePageOpaqueData.flags are spent on the lexical
 * channel and the segment machinery, bit 8 is the WEAVE_FREED state and bits
 * 10-14 are reserved-zero, so there is no room for a vector bit -- shipping one
 * would collide with the fuzzy channel (doc/specs/SEGMENT_FORMAT.md sect. 2,
 * blocking gate 5).  Since v6 the kinds below are INTEGER ids in the extended
 * kind space, stored in WeavePageOpaqueData.kind under the
 * WEAVE_PAGE_KIND_EXT escape bit.  Read them with WeavePageHasKind(), never with
 * a bitwise AND: `flags & WEAVE_VMETA` compiles and is always false.
 *
 * The ids themselves are allocated in one place, the WeavePageKind enum in
 * weave/am.h, so a second channel cannot quietly take a number this one used.
 * The names below are aliases kept beside the structs they describe.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/vector.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_VECTOR_H
#define WEAVE_VECTOR_H

#include "postgres.h"

#include "fmgr.h"
#include "weave/am.h"
#include "weave/channel.h"
#include "weave/quantize.h"
#include "weave/vecscan.h"
#include "weave/vecweft.h"

/* ---------------------------------------------------------------------------
 * Page kinds
 * ------------------------------------------------------------------------- */

#define WEAVE_VMETA			WEAVE_PK_VMETA	/* per-segment vector channel descriptor:
										 * dim, bits, metric, pack layout,
										 * calibration, block directory root */
#define WEAVE_VCODES		WEAVE_PK_VCODES /* packed quantized codes, 32-vector
										 * blocks, each preceded by a
										 * WeaveVecBlockHdr */
#define WEAVE_VGRAPH		WEAVE_PK_VGRAPH /* Vamana CSR adjacency (weave/graph.h) */
#define WEAVE_VWARP			WEAVE_PK_VWARP	/* warp -> docid (weave/vecpage.h) */
#define WEAVE_VRERANK		WEAVE_PK_VRERANK	/* WITHDRAWN -- see pagekind.h.  The
										 * rerank reads the heap, not a sidecar. */

/* ---------------------------------------------------------------------------
 * The wvec SQL type
 *
 * A separate type from pgvector's `vector` on purpose.  Defining a second type
 * named `vector` would make pg_weave and pgvector mutually exclusive in one
 * database, which is exactly the migration story we cannot afford to break --
 * see doc/MIGRATION.md.  Instead wvec is our own type and binary-compatible
 * casts to and from pgvector's `vector` are created conditionally when pgvector
 * is present.
 *
 * Layout deliberately mirrors pgvector's so the cast is a header rewrite rather
 * than an element loop.
 * ------------------------------------------------------------------------- */

typedef struct WVec
{
	int32		vl_len_;		/* varlena header, do not touch directly */
	int16		dim;
	int16		unused;			/* zeroed; reserved for a per-value flag word */
	float4		x[FLEXIBLE_ARRAY_MEMBER];
} WVec;

#define DatumGetWVec(X)			((WVec *) PG_DETOAST_DATUM(X))
#define PG_GETARG_WVEC(n)		DatumGetWVec(PG_GETARG_DATUM(n))
#define PG_RETURN_WVEC(x)		PG_RETURN_POINTER(x)

#define WVEC_SIZE(dim)			(offsetof(WVec, x) + sizeof(float4) * (dim))
#define WVEC_MAX_DIM			WEAVE_MAX_DIM

/*
 * Is this `len` bytes of untrusted storage a wvec we can hand to producer 1?
 *
 * The lexical half has weave_doc_is_valid() for exactly this reason and for
 * exactly this caller: a flush reads a pending page it must not trust, and a
 * garbage veclen would otherwise reach weave_vec_accum_add(), which reads dim
 * from the bytes and then reads 4*dim floats after it -- an out-of-bounds read
 * driven entirely by on-disk data.  Every field is cross-checked against `len`,
 * which the caller knows independently from the item header.
 *
 * The toast predicates come first because VARSIZE() is only meaningful on a
 * 4-byte-header datum.  A pending item always stores the DETOASTED form, so an
 * extended header here is corruption, not a case to handle.
 */
static inline bool
weave_wvec_is_valid(const void *p, uint32 len)
{
	const WVec *v = (const WVec *) p;

	if (len < (uint32) offsetof(WVec, x))
		return false;
	if (VARATT_IS_EXTERNAL(p) || VARATT_IS_COMPRESSED(p) || VARATT_IS_SHORT(p))
		return false;
	if ((uint32) VARSIZE(p) != len)
		return false;
	if (v->dim <= 0 || v->dim > WVEC_MAX_DIM)
		return false;
	if ((uint32) WVEC_SIZE(v->dim) != len)
		return false;
	return true;
}

/* ---------------------------------------------------------------------------
 * Metrics
 *
 * Operator/strategy assignment follows pgvector so that a query written against
 * pgvector keeps working after the index swap:
 *
 *		<->		L2 distance			strategy 1
 *		<#>		negative inner product	strategy 2
 *		<=>		cosine distance		strategy 3
 *		<+>		L1 distance			strategy 4
 *
 * L1 has no useful compressed-domain bound (the quantizer is built around inner
 * products), so it is supported exact-only: the opclass accepts it, the planner
 * costs it as a full scan, and the graph is not used.  Saying that here is
 * cheaper than having someone discover it from a slow query.
 *
 * WeaveMetric itself and WEAVE_METRIC_HAS_BOUND() now live in weave/quantize.h,
 * which this header includes, so every existing spelling still resolves.  Task V8
 * moved them because the code-scan decision core switches on the metric to pick a
 * bound formulation and is deliberately backend-free -- an enum behind postgres.h
 * could not be reached from it.  The strategy numbers above are the part that
 * belongs to the opclass, so they stayed.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * On-page layouts
 * ------------------------------------------------------------------------- */

/*
 * WEAVE_VMETA page contents.  One per segment that carries a vector weft.
 * Self-describing on purpose: a reader must never infer geometry from a GUC,
 * because the GUC can change between the build and the read.
 */
typedef struct WeaveVecMeta
{
	uint32		magic;			/* WEAVE_VMETA_MAGIC */
	uint16		version;		/* WEAVE_VMETA_VERSION */
	uint16		dim;
	uint8		bits;			/* 2..4 */
	uint8		metric;			/* WeaveMetric */
	uint8		layout;			/* WeavePackLayout */
	uint8		flags;			/* WEAVE_VMETA_F_* */

	uint32		nvec;			/* vectors in this segment's vector weft */
	uint32		nblocks;		/* ceil(nvec / WEAVE_VEC_BLOCK) */
	BlockNumber dirstart;		/* first block-directory page: the fixed 284-byte
								 * per-block record (firstwarp, livemask, the four
								 * bound floats, 32 WeaveVecLane).  Separate from
								 * the codes because it is O(1) addressable -- what
								 * "score block i" needs -- and because the variable
								 * part of WeaveVecBlockHdr (a dim-wide centroid
								 * code, 8,192 B at WEAVE_MAX_DIM) does not fit on a
								 * page at all, so a prologue-at-the-head-of-the-
								 * first-strip rule is unimplementable at the
								 * declared maximum dim.  See
								 * doc/specs/VECTOR_CHANNEL.md sect. 7.1. */
	BlockNumber codestart;		/* first WEAVE_VCODES page: coordinate-sliced strips,
								 * block-major.  A page holds one coordinate range
								 * of one block's 32 lanes; the centroid code is
								 * sliced the same way and follows a block's lane
								 * strips. */
	BlockNumber warpstart;		/* first WEAVE_PK_VWARP page: warp -> docid, 8 bytes
								 * per lane.  Never Invalid on a weft that exists --
								 * a weft that cannot name the document behind a warp
								 * cannot be merged and cannot return a row.  See the
								 * block comment above WeaveVecWarpHdr in
								 * weave/vecpage.h for why the derivation it replaced
								 * was unsound, and sect. 7.3 for the merge that
								 * needs it. */
	BlockNumber graphstart;		/* first WEAVE_VGRAPH page, or Invalid */
	/* No rerankstart: the exact rerank reads full precision from the HEAP, so
	 * there is no sidecar chain to point at.  Withdrawn 2026-09-13 before it was
	 * ever written to disk, so this is not a format change. */
	BlockNumber calibstart;		/* TQ+ calibration blob, or Invalid = identity */

	uint32		calibsample;	/* rows the calibration was fit from; 0 = none */
	TimestampTz calibtime;		/* when it was fit, so weave_check() can report
								 * staleness; see the honest limitation in
								 * doc/specs/VECTOR_CHANNEL.md sect. 11 */
} WeaveVecMeta;

#define WEAVE_VMETA_MAGIC		0x57565431	/* "WVT1" */
/*
 * 2 since the merge producer: the struct gained `warpstart` and a weft gained the
 * warp-map chain it names.  A v1 weft has no way to say which document a lane
 * belongs to, so it cannot be merged or scanned -- there is no best-effort read of
 * one, and vec_meta_read() refuses it (doc/CONVENTIONS.md decision 3).  No released
 * version ever wrote a v1 weft: the vector weft and this change are both after tag
 * v2026.09.06, so the refusal is reachable only from a working tree.
 */
#define WEAVE_VMETA_VERSION		3

/*
 * 3 since G27's block->page pointer: WeaveVecDirRec gained `firstpage`, which moved
 * every record's offset on a directory page (record `i` is at `i * sizeof(rec)`), so
 * a v2 directory page cannot be parsed by this build and is REFUSED exactly as a v1
 * weft is -- doc/CONVENTIONS.md decision 3 again, and for a stronger reason: a
 * best-effort read would misparse every record after the first and hand the scanner
 * bounds belonging to other blocks, which is a wrong answer rather than an error.
 *
 * THE REFUSAL COSTS NOTHING RELEASED.  The only tag that exists is v2026.09.06, at
 * extension 0.3.0, which predates the vector weft entirely (the weft writers landed
 * after it, and the weft's SQL surface first appears in the 0.7.0 -> 0.8.0 script).
 * So no released version ever wrote a v2 weft either, and the same sentence the v1
 * note above could make still holds: the refusal is reachable only from a working
 * tree, whose indexes are rebuilt with REINDEX.
 */

#define WEAVE_VMETA_F_NORMALIZED	0x01	/* inputs were unit-normalized at
											 * build; cosine == ip */
#define WEAVE_VMETA_F_HAS_GRAPH		0x02
#define WEAVE_VMETA_F_HAS_RERANK	0x04

/*
 * Header preceding each 32-vector code block on a WEAVE_VCODES page.
 *
 * cencode/cenrad/censcale are the inputs to bound (B3) in weave/quantize.h --
 * the only bound formulation measured to prune anything
 * (bench/RESULTS_BOUND_PRUNING.md: 99.6% of blocks skipped, versus 0.0% for the
 * per-coordinate LUT bound).  smax and maxrecnorm feed the two weaker bounds we
 * take a min against; they cost nothing since we are already here.
 *
 * All six fields must be maintained on every path that mutates a block --
 * insert, vacuum lane-zero, merge -- or the bound stops being an upper bound and
 * the fused scorer silently drops rows.  weave_check() recomputes them; see
 * doc/specs/SEGMENT_FORMAT.md.
 *
 * cenrad MUST be measured against the centroid as reconstructed from cencode,
 * not against the exact float centroid.  A reader only has the code.  Using the
 * exact centroid makes the radius too small and the bound unsound.
 */
typedef struct WeaveVecBlockHdr
{
	WeaveWarp	firstwarp;		/* warp position of lane 0 */
	uint32		livemask;		/* bit i set = lane i occupied */
	float4		smax;			/* max renormalization scale over live lanes */
	float4		maxrecnorm;		/* max ||scale * xhat|| over live lanes, bound (B2) */
	float4		minnorm;		/* min ||v|| over live lanes (L2 bound) */
	float4		censcale;		/* the centroid's own renormalization scale */
	float4		cenrad;			/* R = max ||rec_s - dequant(cencode)||, bound (B3) */
	/* the block centroid, quantized at the same width as the lanes; length is
	 * (dim * bits + 7) / 8 and therefore not expressible as a C array member */
	uint8		cencode[FLEXIBLE_ARRAY_MEMBER];
} WeaveVecBlockHdr;

#define WeaveVecBlockHdrSize(dim, bits) \
	(offsetof(WeaveVecBlockHdr, cencode) + (((dim) * (bits) + 7) / 8))

/* Per-lane sidecar stored alongside the packed codes: the scale is needed to
 * score and the norm to bound L2. */
typedef struct WeaveVecLane
{
	float4		scale;
	float4		norm;
} WeaveVecLane;

/* ---------------------------------------------------------------------------
 * Reloptions
 *
 * Set on the index, not by GUC, because they change the bytes on disk:
 *
 *		bits			2..8				code width			default 4
 *		metric			l2 | ip				score domain		default l2
 *		graph			bool				build the Vamana weft	default true
 *		graph_degree	int					R, out-degree		default 32
 *		graph_beam		int					L, build beam width	default 64
 *		calibrate		int					TQ+ sample rows, 0=off	default 0
 *
 * `metric` is the odd one out and is here anyway, because doc/CONVENTIONS.md
 * decision 1 has two clauses and it satisfies the second: it changes NO stored
 * byte -- the codes are metric-independent -- but "the value used is recorded in
 * the segment so a reader never has to guess" is exactly what WeaveVecMeta.metric
 * is for, and a GUC would let two bolts of one index disagree about what their
 * scores MEAN.  `cosine` and `l1` parse and are refused at build time with the
 * reason (src/am/am.c, weave_index_vec_metric): neither has a sound
 * compressed-domain bound, and a channel whose bound is unsound violates contract
 * (C2) silently.  The eventual user-facing form is one operator family per metric
 * -- wvec_l2_ops, wvec_ip_ops -- which belongs with the ORDER BY ... <-> ... path;
 * see doc/specs/VECTOR_CHANNEL.md sect. 8b.
 *
 * `bits` defaults to 4 because that is the ratified Phase V shape (doc/PHASES.md,
 * "the committed shape"): with an exact top-25 rerank it reaches recall@10 0.9920
 * at n = 1M on GIST-960d for 512 B/vector, which is 0.064x a measured pgvector
 * HNSW index, and 4 is the widest width that still has a SIMD scoring kernel --
 * 5..8 fall back to the scalar oracle (see KERNEL_GROUP_BITS_MAX).  Widths up to
 * 8 are accepted because the codec supports them and the recall ceiling at each
 * is measured (bench/RESULTS_BITWIDTH_SWEEP.md); they are not recommended.
 *
 * There is no `rerank` reloption.  It used to select a stored full-precision
 * sidecar, which is withdrawn -- see WEAVE_VRERANK above.  The rerank window is a
 * query-time GUC instead, because it changes no stored bytes.
 * ------------------------------------------------------------------------- */

#define WEAVE_VEC_DEFAULT_BITS			4
#define WEAVE_VEC_DEFAULT_GRAPH_DEGREE	32
#define WEAVE_VEC_DEFAULT_GRAPH_BEAM	64

/* ---------------------------------------------------------------------------
 * The weft writer and reader (task V7) -- src/vector/vecwrite.c
 *
 * WHY THE SEAM IS HERE and not in include/weave/am.h.  AGENTS.md rule 5 says new
 * code goes in the file that owns the seam.  This is the vector channel's own
 * on-disk structure: the AM calls in at exactly four points (the build's
 * accumulator, the descriptor emitter, the free path and weave_check()), and every
 * one of them wants the same three functions.  Putting them in am.h would make the
 * access method the owner of a format only this channel understands.
 *
 * The PAGE-GEOMETRY half is backend-independent and lives in weave/vecpage.h
 * (one strip page) and weave/vecweft.h (the strip order of a whole weft), so
 * test/hegel/test_vecweft.c proves the round trip with no backend.  What is left
 * here is what genuinely needs one: page allocation, GenericXLog, chain walks.
 * ------------------------------------------------------------------------- */

/*
 * The payload a vector page offers.  ONE expression, shared by the writer, the
 * reader and weave_check(), because `usable` is an input to
 * weave_vecdir_recs_per_page() and to weave_strip_coords_per_page(): two
 * independent expressions for it would put records at two different offsets and
 * the disagreement would read as data.
 */
#define WEAVE_VECPAGE_PAYLOAD	WEAVE_SURFPAGE_PAYLOAD

/*
 * The lane accumulator, filled by EITHER producer (doc/specs/VECTOR_CHANNEL.md
 * sect. 7.3): one lane slot per document, holding the CODE rather than the float
 * vector.  Producer 1 (the build) encodes a Datum into a slot with
 * weave_vec_accum_add(); producer 2 (the merge) appends a code it MOVED verbatim
 * with weave_vec_accum_add_encoded().  Neither knows about the other, and the
 * writer cannot tell them apart -- which is the point: one writer, one set of
 * statistics, no second implementation of the geometry.
 *
 * WHY THE CODE AND NOT THE VECTOR.  The build budget is what bounds a build
 * (MemoryContextMemAllocated(bs->ctx, true) in weave_build_callback), and a
 * dim*4-byte float vector per document is 8x the 4-bit code -- 3,840 B against 480
 * at 960-d.  Encoding in the callback also spreads the rotation cost over the heap
 * scan instead of concentrating it in the segment write.  Everything the writer
 * still needs (the reconstructions, for weave_vecblock_stats) is recoverable from
 * the code by weave_decode(), which is the same thing the merge producer will have
 * to do with moved codes.
 *
 * A NULL or zero vector is a DEAD LANE, not a skipped document.  The docid space
 * is shared with the lexical weft (doc/ARCHITECTURE.md sect. 3), so skipping would
 * shift every later document's warp position and silently mis-associate every
 * vector after the first NULL.
 *
 * THE DOCID IS KEPT, AND THE WEFT IS WRITTEN IN DOCID ORDER, and this is not
 * bookkeeping -- it was a bug found by this task's own regression test.  Lanes were
 * originally assigned in CALLBACK order, which is HEAP SCAN order, and heap scan
 * order is not ascending ctid: RelationGetBufferForTuple() consults the free space
 * map, so a row too small for the current page can land on an earlier one, and the
 * rows with a NULL vector are exactly the small ones.  Two consequences, and the
 * second is the reason this is a correctness fix rather than a tidy-up:
 *
 *	 - "the warp is the bolt's dense docid space" (doc/ARCHITECTURE.md sect. 3)
 *	   becomes false: warp i would be "the i-th tuple the scan happened to reach".
 *	 - the PAGE IMAGE STOPS BEING DETERMINISTIC.  sect. 7 requires two indexes over
 *	   identical vectors to be byte-identical on disk, and a synchronized seqscan
 *	   may start anywhere in the relation, so the same table built twice would order
 *	   its lanes differently.  Sorting by docid makes the weft a function of the
 *	   segment's CONTENTS, which is what that requirement means.
 *
 * THE DOCID IS ALSO WRITTEN TO DISK, in the warp-map chain, and the reason is that
 * the derivation this comment used to offer instead is FALSE.  It said warp i is
 * the i-th smallest docid in the bolt and the bolt's docids are all in its lexical
 * weft.  The second half does not hold: producer 1 gives a lane to every document
 * whose LEXICAL column is non-NULL, while a document only reaches the lexical weft
 * if it has at least one posting, and a non-NULL wdoc with no terms (empty text,
 * stopwords only) has none.  One such document shifts every later warp's derived
 * docid by one and nothing counts wrong.  See WeaveVecWarpHdr in weave/vecpage.h.
 */
typedef struct WeaveVecAccum
{
	MemoryContext ctx;			/* bs->ctx: the build budget must count this */
	bool		active;			/* false when the index has no vector column */
	bool		ready;			/* the quantizer is initialized (dim known) */
	int			dim;
	int			bits;
	int			codebytes;		/* ceil(dim*bits/8), one lane's code */
	WeavePackLayout layout;
	WeaveMetric metric;
	WeaveQuantizer q;

	uint8	   *code;			/* nlane * codebytes */
	float	   *scale;			/* nlane */
	float	   *norm;			/* nlane */
	uint8	   *live;			/* nlane; 0 = dead lane */
	uint64	   *docid;			/* nlane; weave_tid_to_docid of the heap tuple */
	uint32		nlane;
	uint32		lanecap;
	uint32		nlive;
} WeaveVecAccum;

extern void weave_vec_accum_init(WeaveVecAccum *acc, MemoryContext ctx,
								 bool active, int bits, WeaveMetric metric);
extern void weave_vec_accum_reset(WeaveVecAccum *acc);
extern void weave_vec_accum_add(WeaveVecAccum *acc, Relation index,
								ItemPointer tid, Datum value, bool isnull);

/* ---------------------------------------------------------------------------
 * Producer 2, the merge (doc/specs/VECTOR_CHANNEL.md sect. 7.3)
 *
 * THE MERGE MOVES CODES AND NEVER RE-ENCODES -- and the operative half of that is
 * NEVER RECOMPUTES THE (scale, norm) PAIR.  A code IS a fixed point of
 * decode-then-encode (dequantizing gives the codebook levels back and re-quantizing
 * those returns the same levels; measured in test/hegel/test_quantize.c), so
 * re-encoding does not by itself change a byte -- but a reconstruction's norm is not
 * its original's, so the scale a re-encode computes is a DIFFERENT number, and a lane
 * carrying a recomputed scale dequantizes to a different vector while every statistic
 * still recomputes self-consistently.  Idempotency is also a property of this
 * codebook at this width: it fails the moment the output's width or TQ+ calibration
 * differs from the input's, which is what the geometry guard below refuses.  See
 * doc/specs/VECTOR_CHANNEL.md sect. 7.3, which stated a different (and wrong) reason
 * until the mutation run disproved it.
 *
 * MEMORY, stated here because it is the one thing this producer does worse than
 * the lexical merge it runs beside.  weave_merge_segments_streaming() is
 * deliberately bounded to ONE TERM's postings; this accumulator holds EVERY output
 * lane's code before the writer runs, so a vector merge is
 * O(nvec * codebytes) resident: 480 MB of codes at a million documents, 960
 * dimensions and 4 bits, plus 8 bytes per lane of docid and 9 of sidecar (~497 MB
 * total), and it is charged to the merge context rather than to
 * maintenance_work_mem.  That is acceptable at the scale the AM is tested at and
 * it is NOT acceptable at 10M; the streaming shape (merge the input wefts as
 * sorted-by-docid runs and write blocks as they fill) is doc/GAPS.md G25 with the
 * threshold worked out.  Do not raise the bound without reading it.
 * ------------------------------------------------------------------------- */

/*
 * Make an accumulator ready to take PRE-ENCODED lanes of this geometry.
 *
 * Separate from weave_vec_accum_init() because producer 1 learns dim from the first
 * vector it sees and lets weave_vec_accum_add() build the quantizer, while producer
 * 2 knows the geometry up front -- it agreed on it across the inputs before a page
 * was written -- and has no Datum to learn it from.  Returns false when the
 * quantizer cannot be built for (dim, bits); the caller then skips the merge rather
 * than throwing, because this runs where VACUUM can reach it.
 */
extern bool weave_vec_accum_init_geom(WeaveVecAccum *acc, int dim, int bits,
									  WeaveMetric metric, WeavePackLayout layout);

/*
 * Append one already-encoded lane: `code` is codebytes of packed code taken
 * verbatim from an input weft, `scale` and `norm` are that lane's stored sidecar
 * pair, `docid` is the document it belongs to.  A dead lane (no vector for this
 * document) passes code = NULL.
 *
 * Does NOT sort: the writer's vec_docid_order() puts the lanes in output warp
 * order, so a caller may append in any order and MUST NOT rely on the order it
 * used -- the same guarantee producer 1 gets, from the same sort.
 */
extern void weave_vec_accum_add_encoded(WeaveVecAccum *acc, const uint8 *code,
										float scale, float norm, uint64 docid);
extern void weave_vec_accum_add_dead_docid(WeaveVecAccum *acc, uint64 docid);

/*
 * The geometry every input weft of a merge must agree on, and whether they do.
 *
 * `*nwith` counts the inputs that carry a weft.  Returns false when two of them
 * disagree on (dim, bits, layout, metric), which is REACHABLE: `bits` is a
 * reloption with AccessExclusiveLock and no REINDEX requirement, so ALTER INDEX
 * ... SET (bits = 3) followed by an INSERT produces a second bolt at a second
 * width; and a wvec column with no typmod may hold a different dim in a later
 * bolt.  Re-quantizing to a common width is forbidden (sect. 7.3), so the caller
 * SKIPS the merge -- the same shape as the interim rule and for the same reason:
 * skipping is always safe, and an ereport reachable from VACUUM's cleanup is how an
 * index becomes permanently unvacuumable (doc/GAPS.md G15, G20).
 *
 * Never throws, for that reason.
 */
extern bool weave_vec_merge_geom(Relation index, const WeaveSegMeta *segs,
								 uint32 nsegs, WeaveVecMeta *out, uint32 *nwith);

/*
 * Move one input bolt's live lanes into `acc`.
 *
 * Walks the input's blocks IN ORDER and reads each one whole, which is not a
 * pessimization: in WEAVE_PACK_LANE coordinate j of lane s is at byte j*4*bits +
 * s*bits/8, so one lane's code touches every one of the block's pages anyway.  The
 * codes come back through weave_vec_block_read() + weave_unpack_lane(), i.e. the
 * pair every build already exercises, and go out through
 * weave_vec_accum_add_encoded() unaltered.
 *
 * `dropped` is the caller's tombstone test, by docid; a lane whose document is
 * dropped is not appended, which leaves the output with fewer lanes rather than a
 * hole -- warps are dense and the warp map says which document each one is.
 * Returns false (with nothing appended past the failure) if the input weft cannot
 * be read; the caller must then abandon the vector half of the merge, which is why
 * it is checked before any page is written.
 */
extern bool weave_vec_merge_append(Relation index, const WeaveSegMeta *src,
								   WeaveVecAccum *acc,
								   bool (*dropped) (void *arg, uint64 docid),
								   void *arg, const char **why);

/*
 * Write the weft and return its WEAVE_VMETA block, or InvalidBlockNumber when
 * there is nothing to write.  That block is the VECTOR weft's root in the bolt's
 * channel descriptor, so it must be passed to weave_attach_chandesc().
 */
extern BlockNumber weave_vec_write_weft(Relation index, WeaveVecAccum *acc);

/* A weft opened for reading: the VMETA contents plus the geometry derived from
 * them.  Every offset the reader computes comes from `geom`, which the writer
 * computed the same way. */
typedef struct WeaveVecWeft
{
	Relation	index;
	BlockNumber root;			/* the WEAVE_VMETA block */
	WeaveVecMeta meta;
	WeaveVecWeftGeom geom;
} WeaveVecWeft;

/*
 * Open a weft for reading.  Returns false and sets *why (a static string) on any
 * structural problem, and never throws, so weave_check() can report rather than
 * abort -- the same trade weave_read_chandesc() makes and for the same reason.
 */
extern bool weave_vec_weft_open(Relation index, BlockNumber root,
								WeaveVecWeft *out, const char **why);

/* Block `blockno`'s directory record.  O(1): one page read, computed offset. */
extern bool weave_vec_dir_read(const WeaveVecWeft *w, uint32 blockno,
							   WeaveVecDirRec *out, const char **why);

/*
 * Scatter block `blockno`'s codes into `block` (geom.blockbytes) and its centroid
 * code into `cencode` (geom.codebytes, may be NULL).  `block` is ZEROED first, so
 * the 0-28 slack bytes weave_block_codebytes() counts and no pack function writes
 * are defined on both sides of the round trip.
 *
 * COST, stated because it is not what sect. 7.1 implies: the strips are found by
 * WALKING the code chain, so this is O(pages in the weft) and not the
 * ceil(dim/coords_per_page) the block-major argument promises.  The ratified
 * format has no index over the strips and WeaveVecDirRec has no room for one, so
 * O(1) single-block access is not available to a reader; see the note in
 * doc/specs/VECTOR_CHANNEL.md sect. 7.1.  V7's callers (weave_check(), the round
 * trip) walk the whole weft anyway.  THE SCAN DOES NOT USE THIS AT ALL: once per
 * block it is O(blocks x pages), quadratic in the weft, so task V8 carries a
 * forward-only cursor over the same chain instead (src/vector/vecshuttle.c),
 * sharing weave_vec_strip_take() below.  V10's rerank window and vacuum's lane
 * update still want single-block access, and that is the task that has to add the
 * index.
 */
extern bool weave_vec_block_read(const WeaveVecWeft *w, uint32 blockno,
								 uint8 *block, uint8 *cencode,
								 const char **why);

/*
 * Take one WEAVE_PK_VCODES page's strip into the block it belongs to, validating
 * it against the weft's strip plan first.  The shared half of the two readers of
 * the code chain: weave_vec_block_read() above, which walks the whole chain per
 * block, and the forward-only cursor the scan uses (src/vector/vecshuttle.c),
 * which cannot afford to.  Exported for that second caller and for no other
 * reason -- two implementations of the strip format would not crash, they would
 * return wrong distances (src/vector/pack.c).
 *
 * `block` and `cencode` may be NULL: a strip whose destination is NULL is still
 * parsed and still counted, so a chain walk that wants no bytes -- which is what a
 * block skipped on the allowlist is -- validates the chain without scattering.
 */
extern bool weave_vec_strip_take(const WeaveVecWeft *w, const void *contents,
								 uint32 blockno, uint8 *block, uint8 *cencode,
								 bool *seen, int *nseen, const char **why);

/*
 * A forward cursor over a weft's warp map: warp 0's docid, then warp 1's, for all
 * nvec of them.  Every caller walks the whole map in order -- the merge producer,
 * weave_check()'s ascending-docid invariant, weave_vec_lanes() -- so a cursor is
 * the honest shape, and it holds no buffer lock between calls (it copies a page's
 * entries out) so a caller may read directory and code pages in the same loop.
 *
 * O(1) random access by warp is the same arithmetic (weave_vecwarp_page_index in
 * weave/vecpage.h) and is what V8's per-returned-row lookup wants.  It is not
 * written until it has a caller: an untested reader is worse than an absent one.
 */
typedef struct WeaveVecWarpCursor
{
	const WeaveVecWeft *w;
	BlockNumber blk;			/* next page of the chain to read */
	uint64	   *buf;			/* the entries of the page last read */
	int			nbuf;
	int			pos;
	uint32		warp;			/* warp of buf[pos] */
} WeaveVecWarpCursor;

extern void weave_vec_warp_begin(WeaveVecWarpCursor *c, const WeaveVecWeft *w);
extern bool weave_vec_warp_next(WeaveVecWarpCursor *c, uint64 *docid,
								const char **why);
extern void weave_vec_warp_end(WeaveVecWarpCursor *c);

/*
 * The bolt's VECTOR weft root, from its channel descriptor, or
 * InvalidBlockNumber.  Uses the non-throwing descriptor reader: the free path and
 * weave_check() both call this and neither may throw on a corrupt page.
 */
extern BlockNumber weave_vec_weft_root(Relation index, const WeaveSegMeta *seg);

/*
 * The same lookup, also reporting the ATTRIBUTE the descriptor records the weft
 * against (*attnum, untouched when there is no vector weft; may be NULL).
 *
 * It exists because the attnum is otherwise write-only in V7: nothing reads it
 * until V8 routes a scan key by it, so a writer that recorded the wrong attribute
 * would build an index that counts correctly today and scores the wrong column
 * later.  A mutation that replaced `layout.vecattno` with the literal 1 in
 * weave_chandesc_for_segment() survived the whole suite for exactly that reason:
 * every assertion about a weft went through the root, and the root is right in
 * both worlds.  weave_vec_meta() reports it so a regression test can pin it on an
 * index whose vector column is NOT the first one.
 */
extern BlockNumber weave_vec_weft_locate(Relation index, const WeaveSegMeta *seg,
										 uint16 *attnum);

/*
 * Free the VMETA page, the directory chain and the strip chain.  Called from
 * weave_free_segment(): a weft that is written but not freed is a leak of the
 * whole structure on every merge (doc/specs/SEGMENT_FORMAT.md sect. 6).
 *
 * UNREACHABLE IN V7, and deliberately kept -- see the comment on the definition
 * in src/vector/vecwrite.c and doc/GAPS.md G24 for the enumeration that proves it
 * and the one change that ends it.
 */
extern void weave_vec_free_weft(Relation index, BlockNumber root);


/* ---------------------------------------------------------------------------
 * GUCs (query-time only; anything that changes stored bytes is a reloption)
 *
 * Defined in src/vector/kernel_ops.c, beside pg_weave.vec_kernel.  They used to
 * live in src/vector/vector.c, which task V8 DELETED: it was in neither the
 * Makefile's OBJS nor meson.build, and every other symbol in it was a stale
 * duplicate of a live one in src/vector/wvec.c -- one Makefile edit away from a
 * duplicate-symbol link failure.  Only these three definitions were unique to it,
 * so only these three moved.
 *
 * The three below back GUCs that are NOT REGISTERED YET, and that is deliberate:
 * the code each one steers is the graph traversal and the exact rerank, neither of
 * which exists (tasks V10 and the graph tasks).  A registered GUC that changes
 * nothing is worse than an unregistered variable, because a user can set it and
 * believe something happened.
 * ------------------------------------------------------------------------- */

/* Candidate multiplier for the graph traversal: visit oversample * k nodes. */
extern int	weave_vec_oversample;

/*
 * Size of the exact float32 rerank window: the top `w` candidates from the
 * compressed-domain scan are re-scored against the heap's full-precision vectors
 * and the best k of those are returned.
 *
 * Default 25, which is the ratified Phase V shape at 4 bits: recall@10 0.9920 at
 * n = 1M on GIST-960d, against 0.9810 at window 20 and 0.9980 at 30
 * (bench/RESULTS_PHASE_V_COLD.md).  A GUC and not a reloption because it changes
 * no stored bytes -- the same index answers a wider or narrower window.
 *
 * IT SHOULD GROW WITH THE CORPUS.  The window has to be wide enough to still
 * contain the true top-k after quantization perturbs the ordering, and that
 * requirement grows by about +25% per decade of row count: 25 at n = 1M implies
 * roughly 31 at 10M and 39 at 100M.  Measured at 100k and 1M and interpolated
 * beyond, so treat >1M as a starting point to verify rather than a setting.
 */
extern int	weave_vec_rerank_window;

/* "exact" disables the graph and scans every code block; "graph" uses the
 * traversal.  Exposed as an enum GUC so a session can opt into 1.000 recall for
 * an audit query without rebuilding.  This is the frontier knob from
 * doc/ARCHITECTURE.md sect. 8.1 -- it does not abolish the tradeoff, it lets the
 * caller choose a point on it. */
typedef enum WeaveVecRecall
{
	WEAVE_RECALL_GRAPH = 0,
	WEAVE_RECALL_EXACT = 1
} WeaveVecRecall;

extern int	weave_vec_recall;

/* Force a kernel family, for A/B measurement and for reproducing a bug report
 * from a different host.  "auto" is the only value anyone should ship. */
typedef enum WeaveVecKernel
{
	WEAVE_KERNEL_AUTO = 0,
	WEAVE_KERNEL_SCALAR,
	WEAVE_KERNEL_LUT,
	WEAVE_KERNEL_DOT
} WeaveVecKernel;

extern int	weave_vec_kernel;

/* ---------------------------------------------------------------------------
 * Kernel dispatch
 *
 * Resolved once, at _PG_init, into function pointers -- the same shape
 * PostgreSQL itself uses for pg_popcount and the CRC32C implementations
 * (src/port/pg_popcount_*.c).  Following core's pattern rather than inventing
 * one keeps the "which path ran?" question answerable from
 * weave_vec_kernel_name() in a bug report.
 * ------------------------------------------------------------------------- */

typedef struct WeaveVecKernelOps
{
	const char *name;			/* "avx512vnni", "neon-sdot", "scalar", ... */

	/*
	 * Score up to WEAVE_VEC_BLOCK lanes of one block against a prepared query
	 * table.  Returns the number of scores written, and writes
	 * WEAVE_SCORE_NEVER for skipped lanes so the caller's indexing stays
	 * positional.
	 *
	 * `layout` is the pack layout recorded in this segment's WeaveVecMeta, and
	 * it is a parameter rather than an assumption because a scorer that guesses
	 * it wrong does not fail, it returns wrong distances (src/vector/pack.c,
	 * doc/specs/VECTOR_CHANNEL.md sect. 7).  Callers pass the segment's value;
	 * they do not pass a constant.
	 *
	 * `allow` is a warp-indexed bitmap of `nwarp` warps, or NULL meaning
	 * "everything is allowed" -- which is not the same as an all-zero bitmap.
	 * Lanes whose warp bit is clear must be skipped without being scored.
	 * `nwarp` is mandatory when `allow` is non-NULL: lane s of the block sits at
	 * warp hdr->firstwarp + s, hdr->firstwarp comes off a page, and an untrusted
	 * index into a bitmap with no length is an unbounded out-of-bounds read.
	 * A block whose lanes fall outside [0, nwarp) is a corrupt page and raises.
	 */
	int			(*score_block) (const WeaveQueryLut *lut,
								WeavePackLayout layout,
								const WeaveVecBlockHdr *hdr,
								const uint8 *codes,
								const WeaveVecLane *lanes,
								const uint64 *allow,
								WeaveWarp nwarp,
								float4 *out);

	/* Rotation, which must be bit-identical to the scalar reference in
	 * src/vector/quantize.c.  See the warning in weave/quantize.h. */
	void		(*rotate) (const WeaveRotation *rot, float *x);
} WeaveVecKernelOps;

extern const WeaveVecKernelOps *weave_vec_kernels;

extern void weave_vec_kernels_init(void);
extern const char *weave_vec_kernel_name(void);

/*
 * Dispatch a block already in WeaveScoreBlock shape (what the decision core's
 * weave_vec_scan_scoreblk() produces) through the resolved kernel, honouring
 * pg_weave.vec_kernel and turning a rejected block description into a clean ERROR.
 * See the comment on the definition for why the scan cannot use the
 * WeaveVecBlockHdr-shaped entry point above.
 */
extern int	weave_vec_score_block(const WeaveScoreBlock *blk, float4 *out);

/* ---------------------------------------------------------------------------
 * The code-scan shuttle
 *
 * Implements weave/channel.h for WEAVE_CH_VECTOR_SCAN.  Its block_max() is
 * weave_block_bound_ip/l2 from weave/quantize.h, its score_block() is the SIMD
 * kernel above, and its seek() walks the block directory.
 *
 * When `allow` is non-NULL the kernel short-circuits any block whose livemask
 * and allow-bitmap do not intersect, which is why a selective predicate makes
 * this channel FASTER rather than slower -- the opposite of over-fetch-then-
 * filter.  See doc/specs/VECTOR_CHANNEL.md sect. 9.
 *
 * THE METRIC IS NOT A PARAMETER, and it used to be.  It comes from the weft's own
 * WeaveVecMeta, because the conversion from the kernel's inner product to the
 * metric's score domain is what makes a bound and a score commensurable, and a
 * caller that could override it could ask for an L2 bound next to an IP score --
 * two numbers in different units, which makes contract (C2) meaningless rather
 * than violated.  include/weave/vecscan.h "THE DOMAIN RULE" is the authority and
 * it says the caller does not get to override it; the parameter this prototype
 * carried until task V8 contradicted that.
 *
 * `qdim` is the length of `query` and must equal the weft's dim.  It is here
 * because `query` is a bare float pointer: without a length, a caller that passed
 * a shorter array than the weft's dim would be a read past the end of it, and the
 * rotation inside weave_query_lut_build() reads all dim of them.  The query is
 * passed RAW -- unrotated -- because the LUT builder rotates internally.
 *
 * IT TAKES AN ALREADY-OPENED WEFT rather than (Relation, segno), which is what
 * this prototype said until task V8 implemented it.  Every driver has to open the
 * weft before it can begin a shuttle, because `nwarp` must cover the weft's own
 * lane count and only the weft knows that count -- so an allowlist cannot be built
 * before weave_vec_weft_open() has run.  Opening it a second time in here would be
 * a second answer to "which root belongs to this bolt" for no gain.  `segno` is
 * carried for error messages only.  `*w` is COPIED, but it holds a Relation
 * pointer, so the caller must keep the index open for the shuttle's lifetime.
 * ------------------------------------------------------------------------- */

extern WeaveShuttle *weave_vec_shuttle_begin(const WeaveVecWeft *w, int segno,
											 const float *query, int qdim,
											 const uint64 *allow, WeaveWarp nwarp,
											 float4 weight);

/*
 * The bolt-wide score ceiling -- WeaveShuttle.maxscore -- computed WITHOUT opening
 * a shuttle, for the fused objective's per-key normalizer (FUSED_TOPK.md sect. 8d).
 *
 * The normalizer must be one constant for the whole query, because the fused pass
 * merges per-bolt top-k lists by score; a per-bolt normalizer ranks each bolt
 * against a different objective and the answer then changes with the segment count.
 * That maximum has to exist before the first bolt is scanned, which is why this is
 * not "read sh->maxscore".  The definition at src/vector/vecshuttle.c says what it
 * costs and why it refuses rather than throws.
 *
 * Returns false and sets *why (a static string) on any refusal; *out is untouched.
 */
extern bool weave_vec_weft_maxscore(const WeaveVecWeft *w, const float *query,
									int qdim, float *out, const char **why);

/*
 * The driver's current top-k floor, handed to the decision core so that
 * WEAVE_VSCAN_SKIP_BOUND is reachable at all.
 *
 * NOT part of WeaveShuttleOps, and that is the contract working as designed:
 * weave/channel.h deliberately does not tell a shuttle the fused scorer's floor,
 * because the floor is a property of the FUSION and a shuttle is a cursor.  A
 * shuttle driven by the fused loop therefore never calls this, passes -INFINITY to
 * the core, and lets block_max() do the pruning; a driver that maintains its own
 * top-k -- today the weave_vec_scan() SRF -- sets it here.  See the note above
 * weave_vec_scan_block() in weave/vecscan.h.
 *
 * Rejection is on `s <= theta`, so pass the k-th best score itself: an equal score
 * does not displace an incumbent, so a block that can only equal the floor cannot
 * contribute.
 */
extern void weave_vec_shuttle_set_threshold(WeaveShuttle *s, float4 theta);

/*
 * The document behind a warp position, from the third of the shuttle's three
 * lockstep cursors.
 *
 * FORWARD-ONLY, and a target below the last one asked for is an ERROR rather than
 * a re-read: the warp map chain has no index over its pages, so random access
 * would be O(pages) PER LOOKUP -- the doc/GAPS.md G27 shape -- where a monotone
 * cursor is O(pages) in total.  That is affordable only because the scan visits
 * warps in ascending order, and candidates therefore enter a top-k in ascending
 * warp order too (doc/specs/VECTOR_CHANNEL.md sect. 8b).  A driver must resolve a
 * docid when the candidate is admitted, not after it has sorted by score.
 */
extern uint64 weave_vec_shuttle_docid(WeaveShuttle *s, WeaveWarp warp);

/*
 * The shuttle's view of the decision core's counters, so the mask short-circuit is
 * MEASURED rather than asserted -- which is task V8's gate.  Valid until
 * ops->end().  A skipped block still costs its page reads, so nblk_mask counts
 * saved SCORING and saved strip scatter, never saved I/O (sect. 8b).
 */
extern const WeaveVecScanState *weave_vec_shuttle_stats(WeaveShuttle *s);

/* ---------------------------------------------------------------------------
 * The whole-index top-k, ONE implementation with two callers (task F7)
 *
 * These three structs and weave_vec_topk_run() were file-static inside
 * src/vector/vecshuttle.c, reached only by the weave_vec_scan() SRF, until F7 gave
 * the vector channel an ORDER BY operator and therefore a second driver -- the
 * access method's own amgettuple (src/am/amscan.c, weave_vec_pass()).
 *
 * THEY ARE PUBLIC SO THAT THERE IS ONLY ONE SCORING LOOP, and that is the point
 * rather than a convenience.  Two loops over the same bolts, one reached from SQL
 * and one reached from the planner, would be two answers to "what is the top-k of
 * this index" that agree until one of them is edited.  The SRF is
 * sql/vecscan.sql's oracle and sql/vecorderby.sql compares the index scan against
 * it, so a drift between them would present as a regression diff in a file that
 * looks like it is testing the operator.  Sharing the loop makes that comparison
 * meaningful instead of circular: it still pins the scan path -- TID resolution,
 * visibility, the widening ladder -- which is all F7 added.
 *
 * MVCC IS NOT APPLIED HERE, per (C6): dead LANES are excluded by the shuttle
 * (score WEAVE_SCORE_NEVER), but a docid is an index-resident document id, not a
 * proof that a visible row exists.  The amgettuple driver probes the heap; the SRF
 * deliberately does not.
 * ------------------------------------------------------------------------- */

typedef struct WeaveVecTopKHit
{
	int32		segno;
	uint32		warp;
	uint64		docid;
	float4		score;			/* the metric's domain, higher is better */
} WeaveVecTopKHit;

typedef struct WeaveVecTopKCtr
{
	int32		segno;
	float4		maxscore;
	int64		nblk_seen;
	int64		nblk_mask;
	int64		nblk_bound;
	int64		nblk_score;
	int64		nlane_score;
} WeaveVecTopKCtr;

typedef struct WeaveVecTopK
{
	WeaveVecTopKHit *hit;		/* best first; nhit of k slots used */
	int			nhit;
	int			k;
	WeaveVecTopKCtr *ctr;
	int			nctr;

	/*
	 * Total lanes across every bolt this run visited, live or not.  It is the
	 * provable ceiling on how many rows any wider pass could ever return, which is
	 * what lets the amgettuple driver's widening ladder stop for a reason instead
	 * of at a limit of its own -- the role weave_query_maxhits() plays for the
	 * lexical ladder.  Counted here because it is a byproduct of the bolt loop and
	 * a second walk to obtain it would be a second answer.
	 */
	uint64		nlane;
} WeaveVecTopK;

/*
 * Run the top-k over every bolt of an ALREADY-OPEN index.
 *
 * `attnum` is the index attribute the caller wants scored, or 0 for "whichever
 * vector weft the bolt carries".  A bolt whose weft is recorded against a
 * different attribute is SKIPPED, not scored: the attnum in the channel
 * descriptor exists precisely so a scan can route by it (see
 * weave_vec_weft_locate above), and scoring the wrong column is a wrong answer
 * that counts correctly.  The 0 case preserves the SRF's behaviour, which names
 * no attribute.
 *
 * `want` is a SORTED docid allowlist of `nwant` entries; `filtered` distinguishes
 * an EMPTY allowlist (admits nothing) from the ABSENCE of one (admits everything),
 * which a NULL pointer alone cannot.
 *
 * An index with no vector weft in any bolt returns nhit == 0 and nlane == 0 -- not
 * an error.  A query that reaches the operator against such an index is a
 * legitimate plan over an empty channel, and the caller turns it into zero rows.
 */
extern WeaveVecTopK *weave_vec_topk_run(Relation index,
										const WeaveMetaPageData *meta,
										const WVec *query, int k, uint16 attnum,
										const uint64 *want, int nwant,
										bool filtered);

/*
 * The scan SRFs (src/vector/vecshuttle.c).  They exist because a mutation in scan
 * code reachable only through the planner can be answered by a bitmap heap scan's
 * own recheck and survive the entire suite (AGENTS.md, 2026-09-16), so the scan
 * machinery needs an entry point with no executor recheck behind it.
 */
extern Datum weave_vec_scan(PG_FUNCTION_ARGS);
extern Datum weave_vec_scan_stats(PG_FUNCTION_ARGS);

/* ---------------------------------------------------------------------------
 * SQL-callable surface (defined in src/vector/wvec.c)
 * ------------------------------------------------------------------------- */

extern Datum wvec_in(PG_FUNCTION_ARGS);
extern Datum wvec_out(PG_FUNCTION_ARGS);
extern Datum wvec_recv(PG_FUNCTION_ARGS);
extern Datum wvec_send(PG_FUNCTION_ARGS);
extern Datum wvec_dims(PG_FUNCTION_ARGS);
extern Datum wvec_norm(PG_FUNCTION_ARGS);
extern Datum wvec_l2_distance(PG_FUNCTION_ARGS);
extern Datum wvec_ip_distance(PG_FUNCTION_ARGS);
extern Datum wvec_cosine_distance(PG_FUNCTION_ARGS);
extern Datum wvec_l1_distance(PG_FUNCTION_ARGS);

/* Vector-weft introspection (defined in src/vector/vecwrite.c).  See the block
 * comment there for why these are not in src/am/amcheck.c and why two of V7's
 * properties are not assertable from SQL without them. */
extern Datum weave_vec_meta(PG_FUNCTION_ARGS);
extern Datum weave_vec_lanes(PG_FUNCTION_ARGS);
extern Datum weave_vec_blocks(PG_FUNCTION_ARGS);
extern Datum weave_vec_strips(PG_FUNCTION_ARGS);

/*
 * Open an index by OID for one of the vector channel's SQL-callable functions:
 * refuse anything that is not a weave index, and read the metapage through the
 * version-aware reader.  Non-static so the scan SRF (src/vector/vecshuttle.c)
 * enters by the same door the introspection SRFs do; a second opener would be a
 * second definition of which relations these functions accept.
 */
extern Relation weave_vec_introspect_open(Oid indexoid, WeaveMetaPageData *meta);

#endif							/* WEAVE_VECTOR_H */
