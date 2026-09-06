/*-------------------------------------------------------------------------
 *
 * vector.c
 *		The wvec SQL type, distance functions, and the code-scan shuttle.
 *
 * Tasks V1, V7, V8 in doc/PHASES.md.  Currently stubs with the contracts
 * written out; the codec they will use is implemented and property-tested in
 * src/vector/quantize.c.
 *
 * wvec is a distinct type from pgvector's `vector` deliberately.  Defining a
 * second type named `vector` would make pg_weave and pgvector mutually exclusive
 * in one database, which forecloses incremental migration -- the only kind
 * anyone actually performs.  See doc/MIGRATION.md.  The struct layout mirrors
 * pgvector's so the cast is a header rewrite rather than an element loop.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vector.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/vector.h"

int			weave_vec_oversample = 4;
int			weave_vec_recall = WEAVE_RECALL_GRAPH;

#define WEAVE_VEC_TODO(task) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("the weave vector channel is not implemented yet"), \
			 errdetail("Task %s in doc/PHASES.md.", task), \
			 errhint("The quantizer core is implemented and tested; storage, " \
					 "kernels, and the graph are not.")))

/*
 * Begin a code-scan shuttle over one bolt's vector weft.
 *
 * Contract obligations, from weave/channel.h:
 *
 *	 (C1) seek() walks the block directory in ascending warp order.
 *	 (C2) block_max() returns weave_block_bound_ip/l2() computed from the
 *		  WeaveVecBlockHdr the shuttle is sitting on.  Per
 *		  bench/RESULTS_BOUND_PRUNING.md this MUST be the centroid+radius
 *		  formulation; the per-coordinate LUT bound prunes 0.0% of blocks.
 *	 (C3) no page reads in block_max() -- the header is already held.
 *	 (C6) do not consult livedocs; the fused scorer does that.
 *
 * When `allow` is non-NULL, a block whose livemask and allow-bitmap do not
 * intersect is skipped without touching any code bytes.  That is one AND and a
 * branch, and it is why a selective predicate makes this channel faster.
 */
WeaveShuttle *
weave_vec_shuttle_begin(Relation index, int segno,
						const float *query, WeaveMetric metric,
						const uint64 *allow, WeaveWarp nwarp,
						float4 weight)
{
	WEAVE_VEC_TODO("V8");
	return NULL;
}

PG_FUNCTION_INFO_V1(wvec_in);
PG_FUNCTION_INFO_V1(wvec_out);
PG_FUNCTION_INFO_V1(wvec_recv);
PG_FUNCTION_INFO_V1(wvec_send);
PG_FUNCTION_INFO_V1(wvec_dims);
PG_FUNCTION_INFO_V1(wvec_norm);
PG_FUNCTION_INFO_V1(wvec_l2_distance);
PG_FUNCTION_INFO_V1(wvec_ip_distance);
PG_FUNCTION_INFO_V1(wvec_cosine_distance);
PG_FUNCTION_INFO_V1(wvec_l1_distance);

Datum
wvec_in(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_out(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_recv(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_send(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_dims(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_norm(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_l2_distance(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_ip_distance(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

Datum
wvec_cosine_distance(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}

/*
 * L1 is accepted but exact-only: the quantizer is built around inner products
 * and L1 admits no useful compressed-domain bound, so the graph is unused and
 * the planner costs it as a full scan.  Documented in doc/MIGRATION.md rather
 * than left for someone to discover from a slow query.
 */
Datum
wvec_l1_distance(PG_FUNCTION_ARGS)
{
	WEAVE_VEC_TODO("V1");
	PG_RETURN_NULL();
}
