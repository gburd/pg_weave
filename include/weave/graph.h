/*-------------------------------------------------------------------------
 *
 * graph.h
 *		Graph navigation over CENTROIDS for the weave vector weft.
 *
 * PLAN CHANGE, 2026-09-07.  This header was written to specify a Vamana graph over
 * the quantized VECTORS.  That plan is withdrawn.  pg_turbovec added exactly that
 * structure in v1.23.0 for exactly the reason given below, and DEPRECATED it in
 * v2.5.0 after measuring, at matched recall on GIST-10M/960-d with R@10 >= 0.98:
 * IVF 28.4 ms, flat 34.2 ms, and the graph unable to reach 0.98 at ANY latency
 * (ceiling 0.873 at 181 ms), while being 57-90x slower to build with no
 * out-of-core path.  Its apparent sublinearity held only at iso-beam -- p50
 * improved 1.11x for a 10x corpus while recall fell 0.605 to 0.472.
 *
 * The corrected plan is an IVF coarse quantizer; see doc/specs/VECTOR_CHANNEL.md
 * sect. 8.  This file is retained because deprecating the graph KIND is not
 * deprecating graph TECHNIQUES: a graph that navigates CENTROIDS is small, fits in
 * memory, shortens the nprobe search, and pg_turbovec kept theirs precisely
 * because IVF's win partly rests on it.  The interfaces below are being repointed
 * at that structure -- nodes are centroids, not documents -- and the volume of
 * text about out-of-core partitioned builds no longer applies at centroid scale.
 *
 * The graph exists to fix one measured failure.  pg_turbovec shipped a flat
 * quantized scan and measured, on 1M x 1024-d Cohere-wiki, a warm p50 of
 * 2552 ms against pgvector HNSW's 5.2 ms -- a 490x loss -- in exchange for 10x
 * less storage and recall 1.000 versus HNSW's 0.96 (pg_turbovec
 * docs/PARITY_GAPS.md).  SIMD cannot fix that: a flat scan is O(n * dim) and a
 * graph traversal is sublinear.  So we build the graph, and we build it over the
 * QUANTIZED codes rather than over float32, which is what preserves the storage
 * win while buying the latency.
 *
 * Vamana (DiskANN) rather than HNSW, for three reasons that matter here:
 *
 *	 1. Single layer.  HNSW's hierarchy costs pages and complicates the
 *		out-of-core build; Vamana gets the same effect from a long-range-edge
 *		pruning rule (alpha) on one layer.
 *	 2. The robust-prune rule tolerates approximate distances well, and every
 *		distance we compute during both build and search is approximate by
 *		construction because it is computed from codes.
 *	 3. It was designed for a partitioned, out-of-core build, which is what
 *		lets us respect maintenance_work_mem instead of requiring the graph to
 *		fit in memory.
 *
 * THE FILTER IS THE POINT.  A traversal that is handed a warp-indexed allowlist
 * before it starts can steer its greedy descent into the surviving region.  A
 * traversal that is post-filtered cannot, and that is the recall collapse every
 * pgvector-plus-WHERE-clause deployment runs into.  set_visit_filter in
 * weave/channel.h is the mechanism; weave_graph_search honours it during
 * expansion, not after.  See doc/specs/VECTOR_CHANNEL.md sect. 8-9.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/graph.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_GRAPH_H
#define WEAVE_GRAPH_H

#include "postgres.h"

#include "storage/buffile.h"
#include "weave/channel.h"
#include "weave/quantize.h"
#include "weave/vector.h"

/* ---------------------------------------------------------------------------
 * On-page adjacency
 *
 * Compressed sparse row: a directory maps warp position -> (page, offset), and
 * each node's neighbour list is a run of uint32 warp positions.  Out-degree is
 * capped at R, so a node's list is at most R * 4 bytes and never spans pages --
 * that cap is what makes a neighbour fetch exactly one buffer read.
 *
 * Neighbour lists are stored SORTED ASCENDING.  Not for search (which visits in
 * best-first order) but so that a merge can rewrite adjacency by a merge-join
 * against the warp remapping table instead of a hash lookup per edge, and so
 * weave_check() can validate ordering as a cheap corruption signal.
 * ------------------------------------------------------------------------- */

#define WEAVE_GRAPH_MAX_DEGREE		128
#define WEAVE_GRAPH_MIN_DEGREE		8

typedef struct WeaveGraphPageHdr
{
	uint32		magic;			/* WEAVE_GRAPH_MAGIC */
	uint16		version;
	uint16		degree;			/* R for this segment */
	WeaveWarp	firstwarp;		/* first node whose list lives on this page */
	uint32		nnodes;			/* nodes on this page */
	BlockNumber next;			/* next graph page, or Invalid */
} WeaveGraphPageHdr;

#define WEAVE_GRAPH_MAGIC		0x57475231	/* "WGR1" */
#define WEAVE_GRAPH_VERSION		1

/*
 * Graph-wide state, read once per scan from the WEAVE_VMETA page.
 */
typedef struct WeaveGraph
{
	Relation	index;
	int			segno;
	BlockNumber start;			/* first WEAVE_VGRAPH page */
	BlockNumber dirstart;		/* node -> location directory */
	WeaveWarp	entry;			/* the medoid, the descent start point */
	WeaveWarp	nnodes;
	int			degree;			/* R */
} WeaveGraph;

/* ---------------------------------------------------------------------------
 * Search
 *
 * Beam search with a visit filter.  `beam` is L, the candidate-list width;
 * larger L costs distance computations and buys recall.  The caller supplies the
 * prepared query table so the traversal scores from codes, exactly as the flat
 * scan does -- one scoring implementation, one set of bounds, no second code
 * path that can disagree with the first.
 *
 * `allow` may be NULL.  When non-NULL:
 *
 *	 - a node whose warp bit is clear is never ADMITTED to the result set, but
 *	   it IS still expanded, because an excluded node may be the only bridge to
 *	   an included region.  Refusing to expand excluded nodes is the classic
 *	   mistake and it is what makes filtered ANN recall fall off a cliff.
 *	 - the traversal budget is scaled by the filter's selectivity, so a 1%
 *	   filter does not silently get 100x fewer admitted candidates than
 *	   requested.
 *
 * Returns the number of warp positions written to out_warp/out_score, ordered
 * by ascending warp position (NOT by score) so the result can be consumed
 * directly as a monotone cursor by the fused scorer.  See
 * doc/specs/FUSED_TOPK.md sect. 6, option 2: the graph is a bound source and a
 * candidate generator, and the code-scan shuttle is the cursor.
 */
extern int	weave_graph_search(WeaveGraph *g,
							   const WeaveQueryLut *lut,
							   int k, int beam,
							   const uint64 *allow, WeaveWarp nwarp,
							   WeaveWarp *out_warp, float4 *out_score);

/* ---------------------------------------------------------------------------
 * Build
 *
 * Two passes over a partitioned corpus, per DiskANN:
 *
 *	 1. Partition the warp into P clusters by a cheap k-means over a sample,
 *	   with each vector assigned to its two nearest centroids so partition
 *	   boundaries get edges.
 *	 2. Build a Vamana graph within each partition in memory, then union the
 *	   adjacency lists and robust-prune the union back down to R.
 *
 * Partition count is chosen so one partition's working set fits in
 * maintenance_work_mem.  Spill goes through BufFile, so the build respects the
 * memory limit rather than the corpus size -- this is the lesson from
 * pg_tre/LIMITATIONS.md, where an in-memory build was the documented wall.
 * ------------------------------------------------------------------------- */

typedef struct WeaveGraphBuildState
{
	Relation	index;
	int			segno;
	WeaveQuantizer *quant;

	int			degree;			/* R */
	int			beam;			/* L during build */
	double		alpha;			/* robust-prune slack, 1.0 = plain nearest;
								 * 1.2 is the DiskANN default and adds the
								 * long-range edges that make the descent
								 * sublinear */

	WeaveWarp	nnodes;
	int			npartitions;
	BufFile   **spill;			/* one per partition */

	MemoryContext ctx;
} WeaveGraphBuildState;

extern void weave_graph_build_begin(WeaveGraphBuildState *bs, Relation index,
									int segno, WeaveQuantizer *quant,
									int degree, int beam, double alpha);
extern void weave_graph_build_add(WeaveGraphBuildState *bs, WeaveWarp warp,
								  const uint8 *code, float4 scale, float4 norm);
extern void weave_graph_build_finish(WeaveGraphBuildState *bs);

/*
 * Robust prune: from a candidate set sorted by ascending distance, keep at most
 * R neighbours, admitting a candidate only if no already-kept neighbour is
 * alpha times closer to it than the query is.  This is the single rule that
 * distinguishes Vamana from a plain kNN graph and the reason the descent is
 * sublinear rather than a random walk.
 *
 * Exposed because it is short, pure, and the highest-value thing in this file to
 * property-test in isolation (test/hegel/test_graph_prune.c).
 */
extern int	weave_graph_robust_prune(const WeaveWarp *cand, const float4 *dist,
									 int ncand, int degree, double alpha,
									 float4 (*distfn) (void *ctx, WeaveWarp a, WeaveWarp b),
									 void *distctx,
									 WeaveWarp *out);

/* ---------------------------------------------------------------------------
 * Maintenance
 *
 * Insert into a built graph is the honest weak spot.  pg_turbovec's graph kind
 * documented an O(n) whole-relfile rewrite per insert, which is unusable.  Our
 * answer is the one the segment architecture already gives us for free: new rows
 * go to the PENDING list and are searched exhaustively (there are few of them),
 * and the graph is only ever built by a flush or a merge, never incrementally
 * patched.  That trades a small always-scanned tail for never having to solve
 * incremental graph maintenance, and it is why the segment engine had to come
 * first.
 * ------------------------------------------------------------------------- */

/* Validate every invariant in doc/specs/SEGMENT_FORMAT.md that concerns the
 * graph: degree bound, sorted neighbour lists, no dangling warp references, no
 * edges to tombstoned nodes, entry point live, and reachability of every live
 * node from the entry point.  The last one is the expensive check and the one
 * that actually catches a bad build. */
extern void weave_graph_check(Relation index, int segno, bool deep);

#endif							/* WEAVE_GRAPH_H */
