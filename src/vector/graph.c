/*-------------------------------------------------------------------------
 *
 * graph.c
 *		Vamana proximity graph over quantized codes.
 *
 * Task V9 in doc/PHASES.md, and the single highest-value unimplemented thing in
 * the project.  It exists to fix one measured failure: pg_turbovec's flat
 * quantized scan measured a warm p50 of 2552 ms against pgvector HNSW's 5.2 ms
 * on 1M x 1024-d Cohere-wiki -- a 490x loss -- in exchange for 10x less storage
 * and recall 1.000 versus 0.96 (pg_turbovec docs/PARITY_GAPS.md).  SIMD cannot
 * close that: a flat scan is O(n * dim) and a graph traversal is sublinear.
 *
 * Read include/weave/graph.h for the design, including why Vamana rather than
 * HNSW and why the filter must steer the traversal rather than post-filter it.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/graph.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/graph.h"

#define WEAVE_GRAPH_TODO(task) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("the weave vector graph is not implemented yet"), \
			 errdetail("Task %s in doc/PHASES.md.", task), \
			 errhint("Set weave.vec_recall = exact to use the flat code scan " \
					 "once task V8 lands.")))

int
weave_graph_search(WeaveGraph *g, const WeaveQueryLut *lut,
				   int k, int beam,
				   const uint64 *allow, WeaveWarp nwarp,
				   WeaveWarp *out_warp, float4 *out_score)
{
	WEAVE_GRAPH_TODO("V9");
	return 0;
}

void
weave_graph_build_begin(WeaveGraphBuildState *bs, Relation index,
						int segno, WeaveQuantizer *quant,
						int degree, int beam, double alpha)
{
	WEAVE_GRAPH_TODO("V9");
}

void
weave_graph_build_add(WeaveGraphBuildState *bs, WeaveWarp warp,
					  const uint8 *code, float4 scale, float4 norm)
{
	WEAVE_GRAPH_TODO("V9");
}

/*
 * IMPORTANT, and the reason this comment is here rather than in the spec alone:
 * the build's k-means partitioning step determines the order in which warp
 * positions are assigned, and that ordering is what makes the block bound
 * prunable at all.  bench/RESULTS_BOUND_PRUNING.md measures the centroid+radius
 * bound skipping 99.6% of blocks with a cluster-ordered warp and 0.0% with a
 * heap-ordered one.  Assigning warp positions in heap order here will pass every
 * correctness test and silently degrade the fused scorer to a full scan.  Task
 * V13.
 */
void
weave_graph_build_finish(WeaveGraphBuildState *bs)
{
	WEAVE_GRAPH_TODO("V9");
}

/*
 * Robust prune: from candidates sorted by ascending distance, keep at most
 * `degree` neighbours, admitting a candidate only if no already-kept neighbour
 * is `alpha` times closer to it than the query is.  This single rule is what
 * distinguishes Vamana from a plain kNN graph and the reason the greedy descent
 * is sublinear rather than a random walk.
 *
 * Short, pure, and the highest-value thing in this file to property-test in
 * isolation (test/hegel/test_graph_prune.c): the invariants are that the output
 * is a subset of the input, is at most `degree` long, preserves ascending
 * distance order, and always contains the nearest candidate.
 */
int
weave_graph_robust_prune(const WeaveWarp *cand, const float4 *dist,
						 int ncand, int degree, double alpha,
						 float4 (*distfn) (void *ctx, WeaveWarp a, WeaveWarp b),
						 void *distctx,
						 WeaveWarp *out)
{
	WEAVE_GRAPH_TODO("V9");
	return 0;
}

void
weave_graph_check(Relation index, int segno, bool deep)
{
	WEAVE_GRAPH_TODO("V9");
}
