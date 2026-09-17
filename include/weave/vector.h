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
 * ------------------------------------------------------------------------- */

typedef enum WeaveMetric
{
	WEAVE_METRIC_L2 = 1,
	WEAVE_METRIC_IP = 2,
	WEAVE_METRIC_COSINE = 3,
	WEAVE_METRIC_L1 = 4
} WeaveMetric;

/* Does this metric admit a compressed-domain block bound?  If not, the fused
 * scorer must treat the channel as exact-only. */
#define WEAVE_METRIC_HAS_BOUND(m) \
	((m) == WEAVE_METRIC_L2 || (m) == WEAVE_METRIC_IP || (m) == WEAVE_METRIC_COSINE)

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
#define WEAVE_VMETA_VERSION		1

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
 *		graph			bool				build the Vamana weft	default true
 *		graph_degree	int					R, out-degree		default 32
 *		graph_beam		int					L, build beam width	default 64
 *		calibrate		int					TQ+ sample rows, 0=off	default 0
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
 * Producer 1's accumulator: one lane slot per document the build indexed, in warp
 * order, holding the CODE rather than the float vector.
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
 * It also makes warp -> docid derivable rather than lost: warp i is the i-th
 * smallest docid in the bolt, and the bolt's docids are all in its lexical weft.
 * That derivation is expensive and V8 will want it recorded instead -- see
 * doc/GAPS.md G24 -- but "expensive" and "impossible" are different problems.
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
								 bool active, int bits);
extern void weave_vec_accum_reset(WeaveVecAccum *acc);
extern void weave_vec_accum_add(WeaveVecAccum *acc, Relation index,
								ItemPointer tid, Datum value, bool isnull);

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
 * trip) walk the whole weft anyway; V8's rerank window cannot afford this and is
 * the task that has to fix the format.
 */
extern bool weave_vec_block_read(const WeaveVecWeft *w, uint32 blockno,
								 uint8 *block, uint8 *cencode,
								 const char **why);

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
 * ------------------------------------------------------------------------- */

extern WeaveShuttle *weave_vec_shuttle_begin(Relation index, int segno,
											 const float *query, WeaveMetric metric,
											 const uint64 *allow, WeaveWarp nwarp,
											 float4 weight);

/* ---------------------------------------------------------------------------
 * SQL-callable surface (defined in src/vector/vector.c)
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
extern Datum weave_vec_blocks(PG_FUNCTION_ARGS);
extern Datum weave_vec_strips(PG_FUNCTION_ARGS);

#endif							/* WEAVE_VECTOR_H */
