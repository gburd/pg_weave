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
#define WEAVE_VRERANK		WEAVE_PK_VRERANK	/* optional full-precision sidecar for
										 * the recall=exact rerank tail */

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
	BlockNumber codestart;		/* first WEAVE_VCODES page */
	BlockNumber graphstart;		/* first WEAVE_VGRAPH page, or Invalid */
	BlockNumber rerankstart;	/* first WEAVE_VRERANK page, or Invalid */
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
 *		bits			2 | 3 | 4			code width			default 4
 *		graph			bool				build the Vamana weft	default true
 *		graph_degree	int					R, out-degree		default 32
 *		graph_beam		int					L, build beam width	default 64
 *		rerank			bool				full-precision sidecar	default false
 *		calibrate		int					TQ+ sample rows, 0=off	default 0
 * ------------------------------------------------------------------------- */

#define WEAVE_VEC_DEFAULT_BITS			4
#define WEAVE_VEC_DEFAULT_GRAPH_DEGREE	32
#define WEAVE_VEC_DEFAULT_GRAPH_BEAM	64

/* ---------------------------------------------------------------------------
 * GUCs (query-time only; anything that changes stored bytes is a reloption)
 * ------------------------------------------------------------------------- */

/* Candidate multiplier for the graph traversal: visit oversample * k nodes. */
extern int	weave_vec_oversample;

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

	/* Score up to WEAVE_VEC_BLOCK lanes of one block against a prepared query
	 * table.  `allow` is a warp-indexed bitmap or NULL; lanes whose warp bit is
	 * clear must be skipped without being scored.  Returns the number of scores
	 * written, and writes WEAVE_SCORE_NEVER for skipped lanes so the caller's
	 * indexing stays positional. */
	int			(*score_block) (const WeaveQueryLut *lut,
								const WeaveVecBlockHdr *hdr,
								const uint8 *codes,
								const WeaveVecLane *lanes,
								const uint64 *allow,
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

#endif							/* WEAVE_VECTOR_H */
