/*-------------------------------------------------------------------------
 *
 * fusepath.c
 *	  The SQL surface of fuse(): its executable fallback, its planner support
 *	  function, and the two per-channel score-recovery functions the support
 *	  function emits.  Task F2.1 of doc/specs/FUSED_TOPK.md sect. 7 / 7a.
 *
 * WHAT THIS FILE IS.  `fuse()` is declared to take SCORES -- larger is more
 * relevant -- one per channel, plus a weights array, and to return the NEGATED
 * weighted sum, so that `ORDER BY fuse(...) LIMIT k` with its implicit ASC is
 * best-first.  But the spelling users write, and the only spelling the index
 * pushdown can ever recognize, is
 *
 *	  ORDER BY fuse(body <=> 'q'::wquery, emb <=> $1::wvec, weights => '{...}')
 *
 * whose arguments are DISTANCES.  The support function below rewrites each such
 * argument into the score it came from, by the inverse of that channel's own
 * distance map, and leaves every other argument alone.
 *
 * WHY THE VALUE IS -(sum w_i * s_i) AND NOT 1/(1 + sum w_i * s_i).  sect. 7a (2)
 * decided the 1/(1+S) form on the grounds that it is the map `<=>` already uses
 * (src/query/rank.c:460).  That map is only TOTAL because BM25 is >= 0.  A fused
 * sum is not: cosine similarity lives in [-1, 1], so a score recovered from
 * `wvec <=> wvec` is negative whenever the vectors point apart, and a weighted
 * sum containing it can pass through -1, where 1/(1+S) is a pole and either side
 * of which it is not monotone.  A non-total, non-monotone map does not fail --
 * it produces a WRONG ORDER, which is the one outcome this project treats as
 * worse than an error.  Negation is total, exactly order-reversing, and needs no
 * domain argument.  The cost is that `fuse()`'s value is not in (0,1] like a
 * `<=>` distance is, which costs nothing: it is an ORDER BY expression, and the
 * number that "means something" is F3's score(), which is -fuse().
 *
 * WHAT THIS FILE DELIBERATELY DID NOT DO UNTIL F2.2, AND NOW DOES.  F2.1 shipped
 * with no IndexPath, no set_rel_pathlist_hook and no scan key, on the stated
 * grounds that the pushdown needs the AM side and that sect. 7a (3) had just
 * established that no channel sect. 7's example uses was an ORDER BY operand
 * (tasks F6 and F7).  Both have landed, so F2.2 adds the second half below the
 * fallback: a chained set_rel_pathlist_hook that recognizes the shape the support
 * function emits and offers a hand-built IndexPath for it.
 *
 * What F2.1 owed F2.2 was a SHAPE, and the matcher below is its only intended
 * consumer: after the rewrite, a recognized channel argument is exactly
 *
 *	  FuncExpr(weave_lexscore | weave_cosscore, args = [ the original OpExpr ])
 *
 * with the OpExpr preserved intact, because that OpExpr is what gets lifted into
 * `indexorderbys`.  An argument that was never rewritten -- a bare `col <op> q`
 * whose operator the recovery step does not know, or a plain float -- is matched
 * directly, so the matcher reads through the wrapper rather than requiring it.
 *
 * THE HARD INVARIANT OF THE PLANNER HALF: NEVER OFFER A PATH THE AM CANNOT SERVE.
 * doc/GAPS.md G39 is what the alternative looks like -- an access method that
 * advertises a plan and then refuses it at run time, which is a query that fails
 * rather than a query that is slow.  Every refusal below is therefore a REFUSAL:
 * add no path, emit no message, and let the Sort over the fallback stand.
 * A path the AM would refuse is a bug in this file, not a user error.
 *
 * AND IT NEVER GUESSES.  An argument whose operator is not one of the three
 * recognized ones is left untouched and is therefore taken as a score, which is
 * what fuse()'s own declaration says it is.  That is not a silent fallback: a
 * float carries no evidence of which distance map produced it, so inferring one
 * would be inventing a channel.  sect. 7 requires that the query never simply
 * fail, and sect. 7a (1) records why the fallback's answers cannot match the
 * pushdown's anyway -- weave_distance() outside an index has no corpus, so it
 * uses df = 1 and avgdl = |D|.  Two rankings, both correct against their own
 * inputs.  sql/fuse_fallback.sql tests that divergence rather than hiding it.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/am/fusepath.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"		/* index_open: the metric is a reloption, so a
								 * plan-time metric test has to open the index */
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "weave/am.h"			/* WEAVE_STRAT_*, weave_fuse_install_pathlist_hook */
#include "weave/edist.h"		/* WEAVE_STRAT_DISTANCE, WEAVE_STRAT_EDIST */
#include "weave/fuse.h"			/* WEAVE_FUSE_MAX_CHAN */
#include "weave/quantize.h"		/* WEAVE_METRIC_L2, WEAVE_METRIC_IP (F8) */
#include "weave/weave.h"		/* WeaveQuery */

/*
 * Every catalog OID the rewrite needs, resolved together and cached for the
 * life of the backend.
 *
 * WHY A PER-BACKEND CACHE IS SAFE, AND WHAT IT ASSUMES.  These are OIDs of
 * objects owned by the pg_weave extension, and an OID is stable for the life of
 * the object: ALTER EXTENSION SET SCHEMA moves objects between namespaces
 * without renumbering them, so a relocation cannot invalidate the cache.  The
 * one event that can is DROP EXTENSION followed by CREATE EXTENSION in the same
 * backend, which recreates every object with fresh OIDs -- a stale cache would
 * then emit a FuncExpr naming a dropped function.  So the cache is KEYED ON
 * pg_weave's own extension OID and re-resolved when that changes, which costs
 * one pg_extension index lookup per planned fuse() call and removes the whole
 * failure mode.  src/am/customscan.c's weave_lookup_match_op() caches an
 * operator OID without that key; it is only ever COMPARED against an OID the
 * parsed query already carries, so a stale value there can misfire only into
 * "no pushdown".  Emitting an OID is the stricter case.
 */
typedef struct WeaveFuseOids
{
	Oid			extoid;			/* the pg_weave these were resolved from */
	Oid			lex_op;			/* <=> (wdoc, wquery) */
	Oid			lex_commop;		/* <=> (wquery, wdoc), the commutator form */
	Oid			vec_op;			/* <=> (wvec, wvec), cosine distance */
	Oid			vec_l2_op;		/* <-> (wvec, wvec), Euclidean distance */
	Oid			vec_ip_op;		/* <#> (wvec, wvec), negated inner product */
	Oid			edist_op;		/* <@> (wdoc, text), Levenshtein distance */
	Oid			lexscore_fn;	/* weave_lexscore(float8) */
	Oid			cosscore_fn;	/* weave_cosscore(float8) */
	Oid			l2score_fn;		/* weave_l2score(float8) */
	Oid			ipscore_fn;		/* weave_ipscore(float8) */
	Oid			edistscore_fn;	/* weave_edistscore(float8) */

	/* Whether the objects 0.17.0 added all exist.  NOT folded into
	 * the all-or-nothing test at the end of weave_fuse_resolve_oids(), and the
	 * reason is a real upgrade window rather than caution: the library is replaced
	 * before `ALTER EXTENSION pg_weave UPDATE` runs, so a 0.17.0 binary routinely
	 * plans queries against 0.16.0's catalog for a few seconds or a few days.  If a
	 * missing weave_l2score() failed the whole resolve, the LEXICAL rewrite would
	 * silently stop firing in that window -- turning a missing feature into a
	 * changed ranking on queries that have nothing to do with vectors. */
	bool		have_distscore;
} WeaveFuseOids;

static WeaveFuseOids weave_fuse_oids = {InvalidOid};

/*
 * Resolve, or confirm, every OID above.  Returns false if anything is missing,
 * and the caller must then DECLINE TO REWRITE rather than report a problem:
 * this runs inside the planner for any query mentioning fuse(), and an ereport()
 * from a failed catalog lookup would turn a missing object into a failure of
 * whatever query happened to be planned.  A decline leaves fuse()'s arguments as
 * scores, which is a defined answer.
 *
 * Resolution is by extension schema rather than by search_path.  pg_weave is
 * relocatable, so `<=>` under any other schema on the path is some other
 * extension's operator and must not be recognized as ours.
 */
static bool
weave_fuse_resolve_oids(void)
{
	WeaveFuseOids n;
	Oid			extoid;
	Oid			nspoid;
	char	   *nspname;
	Oid			wdoc;
	Oid			wquery;
	Oid			wvec;
	Oid			argtypes[1] = {FLOAT8OID};

	extoid = get_extension_oid("pg_weave", true);
	if (!OidIsValid(extoid))
		return false;
	if (weave_fuse_oids.extoid == extoid)
		return true;

	nspoid = get_extension_schema(extoid);
	if (!OidIsValid(nspoid))
		return false;
	nspname = get_namespace_name(nspoid);
	if (nspname == NULL)
		return false;

	wdoc = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						   PointerGetDatum("wdoc"),
						   ObjectIdGetDatum(nspoid));
	wquery = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 PointerGetDatum("wquery"),
							 ObjectIdGetDatum(nspoid));
	wvec = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						   PointerGetDatum("wvec"),
						   ObjectIdGetDatum(nspoid));
	if (!OidIsValid(wdoc) || !OidIsValid(wquery) || !OidIsValid(wvec))
		return false;

	n.lex_op = OpernameGetOprid(list_make2(makeString(nspname),
										   makeString("<=>")),
								wdoc, wquery);
	n.lex_commop = OpernameGetOprid(list_make2(makeString(nspname),
											   makeString("<=>")),
									wquery, wdoc);
	n.vec_op = OpernameGetOprid(list_make2(makeString(nspname),
										   makeString("<=>")),
								wvec, wvec);
	n.vec_l2_op = OpernameGetOprid(list_make2(makeString(nspname),
											  makeString("<->")),
								   wvec, wvec);
	n.vec_ip_op = OpernameGetOprid(list_make2(makeString(nspname),
											  makeString("<#>")),
								   wvec, wvec);
	n.lexscore_fn = LookupFuncName(list_make2(makeString(nspname),
											  makeString("weave_lexscore")),
								   1, argtypes, true);
	n.cosscore_fn = LookupFuncName(list_make2(makeString(nspname),
											  makeString("weave_cosscore")),
								   1, argtypes, true);
	n.edist_op = OpernameGetOprid(list_make2(makeString(nspname),
											 makeString("<@>")),
								  wdoc, TEXTOID);
	n.l2score_fn = LookupFuncName(list_make2(makeString(nspname),
											 makeString("weave_l2score")),
								  1, argtypes, true);
	n.ipscore_fn = LookupFuncName(list_make2(makeString(nspname),
											 makeString("weave_ipscore")),
								  1, argtypes, true);
	n.edistscore_fn = LookupFuncName(list_make2(makeString(nspname),
												makeString("weave_edistscore")),
									 1, argtypes, true);

	if (!OidIsValid(n.lex_op) || !OidIsValid(n.lex_commop) ||
		!OidIsValid(n.vec_op) || !OidIsValid(n.lexscore_fn) ||
		!OidIsValid(n.cosscore_fn))
		return false;

	n.have_distscore = (OidIsValid(n.vec_l2_op) && OidIsValid(n.vec_ip_op) &&
						OidIsValid(n.edist_op) && OidIsValid(n.l2score_fn) &&
						OidIsValid(n.ipscore_fn) &&
						OidIsValid(n.edistscore_fn));

	n.extoid = extoid;
	weave_fuse_oids = n;
	return true;
}

/*
 * Validate a weights array against the number of score arguments and return a
 * pointer to its elements.  Every rejection here is a wrong answer prevented
 * rather than a taste enforced, and include/weave/fuse.h note 3 is where the
 * reasons are written down: a NEGATIVE weight breaks contract (C2) under
 * weighting, because w * block_max() >= w * score() needs w > 0 and nothing
 * else, and a ZERO weight multiplies a gate's +INF ceiling into a NaN that makes
 * every prune silently evaluate false.  A user who wants a channel out of the
 * fusion leaves it out of the fuse() call.
 *
 * Shared by the fallback (per row) and by the support function (once, at plan
 * time, when the array is a Const), so the two cannot disagree about what a
 * legal weights array is.
 */
static float4 *
weave_fuse_weights(ArrayType *arr, int nscores)
{
	float4	   *w;
	int			nw;
	int			i;

	if (ARR_NDIM(arr) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fuse() weights must be a one-dimensional array, not %d-dimensional",
						ARR_NDIM(arr))));
	nw = (ARR_NDIM(arr) == 0) ? 0 : ARR_DIMS(arr)[0];

	if (nw != nscores)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fuse() was given %d weight(s) but has %d score argument(s)",
						nw, nscores),
				 errhint("Pass one weight per score argument, or omit weights for equal weights of 1.0.")));

	if (ARR_HASNULL(arr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fuse() weights must not contain NULLs")));

	w = (float4 *) ARR_DATA_PTR(arr);
	for (i = 0; i < nw; i++)
	{
		if (isnan(w[i]) || isinf(w[i]) || w[i] <= 0.0f)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("fuse() weight %d is %g: weights must be finite and greater than zero",
							i + 1, (double) w[i])));
	}

	return w;
}

/*
 * fuse(s1, ..., sn, weights) -> float8, the executable fallback.  One C symbol
 * serves every overload; the arity comes from PG_NARGS() and the last argument
 * is always the weights array.
 *
 * NOT STRICT, and it has to be: `weights` defaults to NULL, so a STRICT fuse()
 * would return NULL for every call that does not spell the weights out.  NULL
 * handling is therefore explicit -- a NULL score argument yields NULL, the
 * ordinary meaning of an unknown input.
 */
PG_FUNCTION_INFO_V1(weave_fuse);

Datum
weave_fuse(PG_FUNCTION_ARGS)
{
	int			nargs = PG_NARGS();
	int			nscores = nargs - 1;
	float4	   *w = NULL;
	double		sum = 0.0;
	int			i;

	if (nscores < 1)
		elog(ERROR, "fuse() requires at least one score argument");

	/*
	 * Weights are validated BEFORE any score is looked at, so that an illegal
	 * weights array is an error for every row rather than only for rows whose
	 * scores happen to be non-NULL.  An error that depends on the data is an
	 * error that appears and disappears with the LIMIT.
	 *
	 * NULL weights means equal weights of 1.0 each, and deliberately NOT 1/n:
	 * an unnormalized default means adding a channel to a fuse() call does not
	 * silently rescale the channels already in it.
	 */
	if (!PG_ARGISNULL(nscores))
		w = weave_fuse_weights(PG_GETARG_ARRAYTYPE_P(nscores), nscores);

	/*
	 * Summed LEFT TO RIGHT IN ARGUMENT ORDER, which is part of this function's
	 * contract and not an implementation detail.  float addition is not
	 * associative, so the sum depends on the order; the fused index scan
	 * (include/weave/fuse.h) sums required channels first and then scored
	 * channels by descending weighted ceiling, which is a DIFFERENT order.  The
	 * two results agree to within rounding and are not bit-identical -- F5
	 * measured 771 of 812,179 comparisons differing in the last ULP -- so
	 * nothing here or in the tests may claim they are.
	 */
	for (i = 0; i < nscores; i++)
	{
		if (PG_ARGISNULL(i))
			PG_RETURN_NULL();
		sum += (w != NULL ? (double) w[i] : 1.0) * PG_GETARG_FLOAT8(i);
	}

	/* Negated, so ascending order is best-first; see the header comment. */
	PG_RETURN_FLOAT8(-sum);
}

/*
 * weave_lexscore(float8) -> float8: recover a BM25 score from the lexical
 * `<=>` distance, which is 1/(1 + score) (src/query/rank.c:460), so the inverse
 * is 1/d - 1.
 *
 * TOTAL BY CONSTRUCTION.  The operator's range is (0, 1], but this function is
 * reachable from hand-written SQL with any float, and raising inside a sort key
 * would turn a user's arithmetic mistake into a failed query.  d <= 0 is outside
 * the range, and the limit of 1/d - 1 as d approaches 0 from above is +INF, so
 * that is what it returns; a NaN input propagates as a NaN, which the fused sum
 * then carries and the sort treats as an ordinary unordered value.
 */
PG_FUNCTION_INFO_V1(weave_lexscore);

Datum
weave_lexscore(PG_FUNCTION_ARGS)
{
	double		d = PG_GETARG_FLOAT8(0);

	if (d <= 0.0)
		PG_RETURN_FLOAT8(get_float8_infinity());

	PG_RETURN_FLOAT8(1.0 / d - 1.0);
}

/*
 * weave_cosscore(float8) -> float8: recover a cosine SIMILARITY from the cosine
 * DISTANCE `wvec <=> wvec` computes, which is 1 - similarity.  Total on every
 * float, and the recovered value is legitimately negative for vectors pointing
 * apart -- which is the fact that decided fuse()'s negation over 1/(1+S); see
 * the header comment.
 */
PG_FUNCTION_INFO_V1(weave_cosscore);

Datum
weave_cosscore(PG_FUNCTION_ARGS)
{
	double		d = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(1.0 - d);
}

/*
 * weave_l2score(float8) -> float8 and weave_ipscore(float8) -> float8, task F8:
 * recover the vector channel's score from what `<->` and `<#>` compute.
 *
 * WHY THESE HAD TO EXIST BEFORE THE VECTOR CHANNEL COULD BE FUSED AT ALL, and it
 * is not a missing convenience -- 0.16.0 answered `fuse(body <=> q, emb <-> v)`
 * WITH THE VECTOR CHANNEL INVERTED.  fuse() sums SCORES, higher being better, and
 * negates once at the end; `<->` is a DISTANCE, so passing it through unrecovered
 * made a far vector rank ahead of a near one, silently, in a query shape the
 * documentation invites.  doc/GAPS.md G41 records it as found-and-closed and
 * doc/specs/FUSED_TOPK.md sect. 7c has the reasoning; the reason nothing caught it
 * is that both arms -- fallback and (refused) pushdown -- were wrong the same way,
 * so no comparison between them could see it.
 *
 * THE DOMAIN IS THE CHANNEL'S, NOT THE OPERATOR'S.  The weave vector channel
 * scores in the metric's domain with higher better: -||q - v||^2 for l2 and the
 * inner product for ip (include/weave/vecscan.h "THE DOMAIN RULE").  So:
 *
 *	 l2: `<->` computes ||q - v||, hence the score is -d * d.  NOT -d, which is
 *		 monotone in the same direction and would still rank a single channel
 *		 correctly -- but a fused sum is not a ranking, it is arithmetic, and mixing
 *		 -d with the index's -d^2 would weight the vector channel differently in the
 *		 two arms at every distance except 1.
 *	 ip: `<#>` computes the NEGATED inner product (pgvector's convention, which
 *		 wvec follows so that smaller is nearer), hence the score is -d exactly.
 *
 * Total on every float, like their two siblings: reachable from hand-written SQL,
 * and raising inside a sort key would turn a user's arithmetic mistake into a
 * failed query.  -d * d overflows to -INF for |d| > ~1.3e154, which is the honest
 * limit of "as far away as possible"; a NaN propagates.
 */
PG_FUNCTION_INFO_V1(weave_l2score);

Datum
weave_l2score(PG_FUNCTION_ARGS)
{
	double		d = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(-(d * d));
}

PG_FUNCTION_INFO_V1(weave_ipscore);

Datum
weave_ipscore(PG_FUNCTION_ARGS)
{
	double		d = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(-d);
}

/*
 * weave_edistscore(float8) -> float8: recover a score from the `<@>` edit distance.
 *
 * SHIPPED WITH THE TWO ABOVE EVEN THOUGH `<@>` CANNOT BE FUSED YET (task F9), and
 * the reason is that the two halves of doc/GAPS.md G41 are independent.  The
 * pushdown needs a shuttle in the document space, which F9 owns; the FALLBACK's
 * arithmetic needs only this function, and without it `fuse(body <=> q, body <@> p)`
 * -- a shape sql/fuse_pushdown.sql has run since F2.2 -- sums a distance as a score
 * and ranks the WORST spelling match first.  Fixing two of three channels and
 * leaving the third inverted would be an arbitrary place to stop.
 *
 * A Levenshtein distance is a non-negative integer and smaller is better, so the
 * score is -d.  Nothing subtler is available or wanted: any monotone decreasing map
 * would rank one channel identically, and -d is the one that keeps a UNIT of
 * distance worth a unit of score, which is what a weighted sum needs to be
 * interpretable.
 */
PG_FUNCTION_INFO_V1(weave_edistscore);

Datum
weave_edistscore(PG_FUNCTION_ARGS)
{
	double		d = PG_GETARG_FLOAT8(0);

	PG_RETURN_FLOAT8(-d);
}

/*
 * If `arg` is an OpExpr of a channel distance operator we recognize, return a
 * FuncExpr that recovers that channel's score from it, with the OpExpr itself as
 * the single argument.  Otherwise NULL, meaning "leave this argument alone".
 */
static Expr *
weave_fuse_recover(Expr *arg)
{
	OpExpr	   *op;
	Oid			recover;

	if (arg == NULL || !IsA(arg, OpExpr))
		return NULL;
	op = (OpExpr *) arg;
	if (list_length(op->args) != 2)
		return NULL;

	if (op->opno == weave_fuse_oids.lex_op ||
		op->opno == weave_fuse_oids.lex_commop)
		recover = weave_fuse_oids.lexscore_fn;
	else if (op->opno == weave_fuse_oids.vec_op)
		recover = weave_fuse_oids.cosscore_fn;
	else if (weave_fuse_oids.have_distscore &&
			 op->opno == weave_fuse_oids.vec_l2_op)
		recover = weave_fuse_oids.l2score_fn;
	else if (weave_fuse_oids.have_distscore &&
			 op->opno == weave_fuse_oids.vec_ip_op)
		recover = weave_fuse_oids.ipscore_fn;
	else if (weave_fuse_oids.have_distscore &&
			 op->opno == weave_fuse_oids.edist_op)
		recover = weave_fuse_oids.edistscore_fn;
	else
		return NULL;

	return (Expr *) makeFuncExpr(recover, FLOAT8OID, list_make1(arg),
								 InvalidOid, InvalidOid,
								 COERCE_EXPLICIT_CALL);
}

/*
 * The planner support function.  Handles SupportRequestSimplify only.
 *
 * IT IS IDEMPOTENT, and that is a correctness property rather than tidiness:
 * eval_const_expressions() can reach the same expression more than once (query
 * rewriting, function inlining), and a rewrite that fired every time would nest
 * weave_lexscore() inside weave_lexscore().  It cannot, because a rewritten
 * argument is a FuncExpr and only an OpExpr is ever rewritten, and because this
 * returns NULL -- request unhandled -- unless it actually changed something.
 */
PG_FUNCTION_INFO_V1(weave_fuse_support);

Datum
weave_fuse_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *fcall = req->fcall;
		int			nargs = list_length(fcall->args);
		int			nscores = nargs - 1;
		Node	   *weights;
		List	   *newargs = NIL;
		bool		rewrote = false;
		ListCell   *lc;
		int			i = 0;

		if (nscores < 1)
			PG_RETURN_POINTER(NULL);

		/*
		 * A plan-time weights array is validated at plan time, so a wrong
		 * length or a negative weight is reported once, before the first row,
		 * instead of once per row from the fallback.  This is the one place
		 * where the support function is allowed to raise: it is the user's own
		 * constant that is wrong, not the catalog.
		 */
		weights = (Node *) list_nth(fcall->args, nargs - 1);
		if (IsA(weights, Const) && !((Const *) weights)->constisnull)
			(void) weave_fuse_weights(DatumGetArrayTypeP(((Const *) weights)->constvalue),
									  nscores);

		if (!weave_fuse_resolve_oids())
			PG_RETURN_POINTER(NULL);

		foreach(lc, fcall->args)
		{
			Expr	   *a = (Expr *) lfirst(lc);
			Expr	   *rec = (i < nscores) ? weave_fuse_recover(a) : NULL;

			if (rec != NULL)
			{
				newargs = lappend(newargs, rec);
				rewrote = true;
			}
			else
				newargs = lappend(newargs, a);
			i++;
		}

		if (rewrote)
		{
			FuncExpr   *newf = makeNode(FuncExpr);

			/*
			 * supportnodes.h forbids returning or modifying *fcall, since it is
			 * not separately allocated, but permits reusing parts of its args.
			 * So: a fresh node with the same call properties, a fresh list, and
			 * the untouched argument nodes shared with the original.
			 */
			*newf = *fcall;
			newf->args = newargs;
			ret = (Node *) newf;
		}
	}

	PG_RETURN_POINTER(ret);
}

/* ===========================================================================
 * F2.2 -- THE PUSHDOWN
 *
 * One chained set_rel_pathlist_hook.  It recognizes `ORDER BY fuse(...)` on a
 * base relation carrying a weave index, and adds an IndexPath whose
 * `indexorderbys` are the per-channel operators plus the weights transport key.
 * Everything it cannot serve, it declines to path -- see the header comment.
 * =========================================================================== */

/*
 * The PathKey direction test, which moved between majors.  PostgreSQL 18 replaced
 * PathKey.pk_strategy (a btree strategy number) with pk_cmptype (a CompareType);
 * both spell "ascending" and reading the wrong field would not compile rather than
 * silently invert, which is the only reason this is a macro and not a runtime
 * check.
 */
#if PG_VERSION_NUM >= 180000
#define WEAVE_PATHKEY_IS_ASC(pk)	((pk)->pk_cmptype == COMPARE_LT)
#else
#define WEAVE_PATHKEY_IS_ASC(pk)	((pk)->pk_strategy == BTLessStrategyNumber)
#endif

/*
 * The catalog OIDs the PUSHDOWN needs, cached separately from the rewrite's.
 *
 * SEPARATE ON PURPOSE.  weave_fuse_resolve_oids() above returns false if any one
 * of its OIDs is missing and its callers then decline to rewrite.  Folding the
 * pushdown's objects into that struct would mean that an extension not yet
 * updated to 0.16.0 -- no `<~>` operator, no member 5 -- made the whole resolve
 * fail, which would silently disable F2.1's argument rewrite as well.  A missing
 * transport operator must cost the pushdown and nothing else.
 */
typedef struct WeaveFusePathOids
{
	Oid			extoid;			/* the pg_weave these were resolved from */
	Oid			nspoid;			/* its schema, for the fuse() name test */
	Oid			amoid;			/* the weave access method */
	Oid			match_op;		/* @@@ (wdoc, wquery) -- the gate clause */
	Oid			lex_op;			/* <=> (wdoc, wquery) */
	Oid			lex_commop;		/* <=> (wquery, wdoc) */
	Oid			edist_op;		/* <@> (wdoc, text) */
	Oid			vec_l2_op;		/* <-> (wvec, wvec) */
	Oid			vec_ip_op;		/* <#> (wvec, wvec) */
	Oid			transport_op;	/* <~> (wdoc, float4[]) */
	Oid			lex_family;		/* wdoc_lex_ops */
	Oid			vec_family;		/* wvec_weave_ops */
} WeaveFusePathOids;

static WeaveFusePathOids weave_fuse_path_oids = {InvalidOid};

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/*
 * Resolve, or confirm, the pushdown's OIDs.  Keyed on pg_weave's extension OID
 * for the reason weave_fuse_resolve_oids() gives: DROP EXTENSION + CREATE
 * EXTENSION in one backend renumbers every object, and this cache EMITS an OID.
 *
 * Returns false if anything is missing, and the caller then adds no path.  Never
 * raises: this runs inside the planner for every base relation in every query.
 */
static bool
weave_fuse_path_resolve(void)
{
	WeaveFusePathOids n;
	char	   *nspname;
	Oid			wdoc;
	Oid			wquery;
	Oid			wvec;

	n.extoid = get_extension_oid("pg_weave", true);
	if (!OidIsValid(n.extoid))
		return false;
	if (weave_fuse_path_oids.extoid == n.extoid)
		return true;

	n.nspoid = get_extension_schema(n.extoid);
	if (!OidIsValid(n.nspoid))
		return false;
	nspname = get_namespace_name(n.nspoid);
	if (nspname == NULL)
		return false;

	n.amoid = get_index_am_oid("weave", true);
	if (!OidIsValid(n.amoid))
		return false;

	wdoc = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						   PointerGetDatum("wdoc"), ObjectIdGetDatum(n.nspoid));
	wquery = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 PointerGetDatum("wquery"), ObjectIdGetDatum(n.nspoid));
	wvec = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						   PointerGetDatum("wvec"), ObjectIdGetDatum(n.nspoid));
	if (!OidIsValid(wdoc) || !OidIsValid(wquery) || !OidIsValid(wvec))
		return false;

#define WEAVE_FUSE_OPER(name, l, r) \
	OpernameGetOprid(list_make2(makeString(nspname), makeString(name)), (l), (r))

	n.match_op = WEAVE_FUSE_OPER("@@@", wdoc, wquery);
	n.lex_op = WEAVE_FUSE_OPER("<=>", wdoc, wquery);
	n.lex_commop = WEAVE_FUSE_OPER("<=>", wquery, wdoc);
	n.edist_op = WEAVE_FUSE_OPER("<@>", wdoc, TEXTOID);
	n.vec_l2_op = WEAVE_FUSE_OPER("<->", wvec, wvec);
	n.vec_ip_op = WEAVE_FUSE_OPER("<#>", wvec, wvec);
	n.transport_op = WEAVE_FUSE_OPER("<~>", wdoc, FLOAT4ARRAYOID);

#undef WEAVE_FUSE_OPER

	n.lex_family = get_opfamily_oid(n.amoid,
									list_make2(makeString(nspname),
											   makeString("wdoc_lex_ops")),
									true);
	n.vec_family = get_opfamily_oid(n.amoid,
									list_make2(makeString(nspname),
											   makeString("wvec_weave_ops")),
									true);

	if (!OidIsValid(n.match_op) || !OidIsValid(n.lex_op) ||
		!OidIsValid(n.lex_commop) || !OidIsValid(n.edist_op) ||
		!OidIsValid(n.vec_l2_op) || !OidIsValid(n.vec_ip_op) ||
		!OidIsValid(n.transport_op) ||
		!OidIsValid(n.lex_family) || !OidIsValid(n.vec_family))
		return false;

	weave_fuse_path_oids = n;
	return true;
}

/* One recognized fuse() argument, resolved against one candidate index. */
typedef struct WeaveFuseChanReq
{
	Oid			opno;			/* the operator to EMIT (commutator normalized) */
	Expr	   *lhs;			/* the indexed operand */
	Expr	   *rhs;			/* the query operand */
	int			indexcol;		/* 0-based index column it matched */
	int			strategy;		/* the sk_strategy the AM will see */
	bool		servable;		/* the AM can actually drive this channel today */
	int			maxchan;		/* upper bound on shuttles this channel becomes */
} WeaveFuseChanReq;

/*
 * Is `f` a call to pg_weave's fuse()?
 *
 * BY NAME AND SCHEMA RATHER THAN BY OID, because fuse() is seven overloads (one
 * per arity from two score arguments to eight) and this test must accept all of
 * them without caching seven OIDs that would then have to be kept in step with
 * the SQL script.  The schema check is what stops some other extension's fuse()
 * being recognized; pg_weave is relocatable, so a bare name test would be wrong.
 */
static bool
weave_fuse_is_fuse_call(const FuncExpr *f)
{
	char	   *name;
	bool		match;

	if (get_func_namespace(f->funcid) != weave_fuse_path_oids.nspoid)
		return false;
	name = get_func_name(f->funcid);
	if (name == NULL)
		return false;
	match = (strcmp(name, "fuse") == 0);
	pfree(name);
	return match;
}

/*
 * The fuse() call behind a single ASC ORDER BY pathkey, or NULL.
 *
 * ONLY ASC MATTERS, and that is arithmetic rather than convention: fuse() returns
 * the NEGATED weighted sum (see this file's header), so ascending IS best-first
 * and the AM's ordering values are ascending distances.  A DESC pathkey asks for
 * the WORST documents first, which a top-k scan cannot produce at all -- it would
 * have to enumerate the whole match set -- so it is refused rather than served
 * backwards.  NULLS FIRST is refused for the same reason in miniature: the AM
 * never emits a NULL ordering value, so a plan that asked for nulls first would
 * be satisfied by accident today and wrong the day that changes.
 */
static FuncExpr *
weave_fuse_pathkey_call(PlannerInfo *root)
{
	PathKey    *pk;
	ListCell   *lc;

	if (list_length(root->query_pathkeys) != 1)
		return NULL;
	pk = (PathKey *) linitial(root->query_pathkeys);
	if (!WEAVE_PATHKEY_IS_ASC(pk) || pk->pk_nulls_first)
		return NULL;
	if (pk->pk_eclass == NULL || pk->pk_eclass->ec_has_volatile)
		return NULL;

	foreach(lc, pk->pk_eclass->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *e = em->em_expr;

		while (e != NULL && IsA(e, RelabelType))
			e = ((RelabelType *) e)->arg;
		if (e != NULL && IsA(e, FuncExpr) &&
			weave_fuse_is_fuse_call((FuncExpr *) e))
			return (FuncExpr *) e;
	}
	return NULL;
}

/*
 * An expression is usable as the right operand of an index ORDER BY key only if
 * its value does not depend on the row being scanned: the executor evaluates it
 * once per rescan, into the ScanKey.  So it must contain no Var of this relation
 * and nothing volatile.
 *
 * A Var of ANOTHER relation is fine and is deliberately allowed -- that is a
 * parameterized ordering scan under a nested loop, which is the `ORDER BY
 * fuse(body <=> o.q, ...)` correlated shape, and the path below asks for
 * rel->lateral_relids as its required_outer so the planner parameterizes it.
 */
static bool
weave_fuse_rhs_ok(PlannerInfo *root, RelOptInfo *rel, Expr *rhs)
{
	Relids		varnos;

	if (contain_volatile_functions((Node *) rhs))
		return false;
	varnos = pull_varnos(root, (Node *) rhs);
	if (bms_is_member(rel->relid, varnos))
		return false;
	return true;
}

/*
 * Attribute one fuse() score argument to a channel of `index`.
 *
 * Returns false when the argument cannot be attributed at all.  Returns true with
 * req->servable false when it names a channel this access method KNOWS but cannot
 * drive inside a fused scan yet; the caller then refuses the whole shape, because
 * a partially fused ordering is a different ordering, not a weaker one.
 *
 * WHY THERE IS A `servable` FLAG AT ALL.  The fused core drives every channel in
 * ONE warp space, and this index's channels do not all share one:
 *
 *	 - The lexical shuttle publishes weave_tid_to_docid() DOCIDS as its warp
 *	   positions (src/query/lexshuttle.c), and so does a gate shuttle built from a
 *	   TidSet (include/weave/gate.h).  Those two agree, which is why they were the
 *	   first shapes offered.
 *	 - The vector shuttle's warp is a SEGMENT-LOCAL DENSE LANE INDEX
 *	   (src/vector/vecshuttle.c), related to a docid only through the weft's warp
 *	   map.  **F8 closed this**: include/weave/vecdocmap.h relabels that channel
 *	   into the docid space through the map, which is monotone because the writer
 *	   emits lanes in docid order, so (C1) and (C2) both survive.  `<->` and `<#>`
 *	   are therefore servable now, subject to the metric test below.
 *	 - The `<@>` shuttle's warp is a position in the DICTIONARY, not in any
 *	   document space at all (src/query/edist.c; see weave_edist_pass()).  No
 *	   relabelling exists, because a document's `<@>` distance is a MINIMUM over
 *	   its terms; that needs a document-space shuttle, which is task F9.
 *
 * include/weave/gate.h states this as "reconciling them is Phase F's decision,
 * not this task's".  F2.2 decided the docid space and served what already lived
 * in it; doc/specs/FUSED_TOPK.md sect. 7b records the decision and sect. 7c what
 * F8 found carrying it out.  Offering a path for `<@>` today would be exactly the
 * G39 defect this file's header refuses.
 */

/*
 * Can a fused scan of `index` serve a vector channel scored in `want`?
 *
 * TWO CONDITIONS, AND BOTH ARE PLAN-TIME ON PURPOSE.
 *
 * 1. THE METRIC MUST MATCH, and this is the first place in the project that can
 *    check it before a plan exists.  A weave vector weft is scored in one metric,
 *    so answering `<->` out of an `ip` weft returns every row in an ordering the
 *    query did not ask for -- a wrong answer, not an approximation.  The plain
 *    `ORDER BY v <-> q` path can only refuse that at RUN time (see the long
 *    comment in weave_rescan(), and doc/specs/VECTOR_CHANNEL.md sect. 8b for the
 *    per-metric opclass split that would remove the need), because core matches a
 *    pathkey against an operator FAMILY and the metric is a reloption.  This path
 *    is our own code, so it can do better: DECLINING here means the fused plan is
 *    never offered and the Sort over the fallback stands, instead of an access
 *    method that advertises a plan and raises on the first tuple (doc/GAPS.md
 *    G39).  The reloption is read WITHOUT THROWING -- weave_index_vec_metric()
 *    raises on cosine and l1, and `ALTER INDEX ... SET (metric = 'cosine')` is
 *    accepted without a rewrite, so the throwing accessor would turn such an index
 *    into a query that cannot be PLANNED, never mind scanned.
 *
 * 2. THE SCORE-RECOVERY FUNCTIONS MUST EXIST.  Without weave_l2score(), the
 *    fallback sums a raw DISTANCE where the index sums a score, so the two arms
 *    would not merely differ in rounding, they would rank the vector channel in
 *    OPPOSITE directions -- and the pathkey this path claims to satisfy is the
 *    fallback's expression.  A binary newer than the catalog is an ordinary
 *    upgrade state (see WeaveFuseOids.have_vecscore), so this is reachable, and
 *    the honest answer in that window is no fused path.
 *
 * Opening the index relation is what plancat.c itself does downstream, and NoLock
 * is correct: get_relation_info() has already taken and retained a lock on every
 * index of this relation for the life of the transaction.
 */
static bool
weave_fuse_vec_servable(IndexOptInfo *index, int want)
{
	Relation	irel;
	int			have;

	if (!weave_fuse_oids.have_distscore)
		return false;

	irel = index_open(index->indexoid, NoLock);
	have = weave_index_vec_metric_raw(irel);
	index_close(irel, NoLock);

	return have == want;
}
static bool
weave_fuse_attribute(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *index,
					 Expr *arg, WeaveFuseChanReq *req)
{
	OpExpr	   *op;
	Expr	   *lhs;
	Expr	   *rhs;
	Oid			wantfamily;
	int			col;

	/* Read through the support function's recovery wrapper, if it is there. */
	if (IsA(arg, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) arg;

		if ((f->funcid == weave_fuse_oids.lexscore_fn ||
			 f->funcid == weave_fuse_oids.cosscore_fn ||
			 f->funcid == weave_fuse_oids.l2score_fn ||
			 f->funcid == weave_fuse_oids.ipscore_fn ||
			 f->funcid == weave_fuse_oids.edistscore_fn) &&
			list_length(f->args) == 1)
			arg = (Expr *) linitial(f->args);
	}

	if (!IsA(arg, OpExpr))
		return false;			/* a plain score: nothing to attribute */
	op = (OpExpr *) arg;
	if (list_length(op->args) != 2)
		return false;

	lhs = (Expr *) linitial(op->args);
	rhs = (Expr *) lsecond(op->args);
	req->opno = op->opno;
	req->maxchan = 1;

	if (op->opno == weave_fuse_path_oids.lex_op)
	{
		req->strategy = WEAVE_STRAT_DISTANCE;
		wantfamily = weave_fuse_path_oids.lex_family;
		req->servable = true;
	}
	else if (op->opno == weave_fuse_path_oids.lex_commop)
	{
		/*
		 * The commutator spelling `q <=> col`.  Normalized here rather than
		 * relied upon to have been normalized elsewhere: an index ORDER BY key
		 * must have the index column on the LEFT (fix_indexorderby_references
		 * rewrites the left operand into an INDEX_VAR reference and would fail on
		 * the query operand), and nothing in the planner commutes an ordering
		 * operator outside of core's own index matching, which this path bypasses.
		 */
		lhs = (Expr *) lsecond(op->args);
		rhs = (Expr *) linitial(op->args);
		req->opno = weave_fuse_path_oids.lex_op;
		req->strategy = WEAVE_STRAT_DISTANCE;
		wantfamily = weave_fuse_path_oids.lex_family;
		req->servable = true;
	}
	else if (op->opno == weave_fuse_path_oids.edist_op)
	{
		req->strategy = WEAVE_STRAT_EDIST;
		wantfamily = weave_fuse_path_oids.lex_family;
		req->servable = false;	/* dictionary-space warp; see above */
	}
	else if (op->opno == weave_fuse_path_oids.vec_l2_op)
	{
		req->strategy = WEAVE_STRAT_VEC_L2;
		wantfamily = weave_fuse_path_oids.vec_family;
		req->servable = weave_fuse_vec_servable(index, WEAVE_METRIC_L2);
	}
	else if (op->opno == weave_fuse_path_oids.vec_ip_op)
	{
		req->strategy = WEAVE_STRAT_VEC_IP;
		wantfamily = weave_fuse_path_oids.vec_family;
		req->servable = weave_fuse_vec_servable(index, WEAVE_METRIC_IP);
	}
	else
		return false;			/* not one of this AM's channel operators */

	/*
	 * WHICH COLUMN, and it must be a column of THIS index and of the right
	 * OPCLASS.  match_index_to_operand() is core's own test, so an expression
	 * index (USING weave (to_wdoc(body))) matches on the same terms a core index
	 * path would; the opfamily comparison then rejects a column of the right TYPE
	 * under the wrong opclass -- a text column under gram_ops is not a lexical
	 * column, which is the discriminator include/weave/am.h insists on.
	 */
	for (col = 0; col < index->nkeycolumns; col++)
	{
		if (index->opfamily[col] != wantfamily)
			continue;
		if (match_index_to_operand((Node *) lhs, col, index))
			break;
	}
	if (col >= index->nkeycolumns)
		return false;			/* the index does not carry this column */

	if (!weave_fuse_rhs_ok(root, rel, rhs))
		return false;			/* the query operand is not row-independent */

	/*
	 * HOW MANY SHUTTLES THIS CHANNEL BECOMES, needed at plan time because the
	 * fused core refuses more than WEAVE_FUSE_MAX_CHAN of them and an AM that
	 * refused a path at run time is G39.  A lexical key becomes ONE SHUTTLE PER
	 * QUERY TERM (src/query/lexshuttle.c explains why, and include/weave/fuse.h
	 * and channel.h both already assume it), and the query is a varlena the plan
	 * carries, so the count is readable here.  q->nitems is an RPN item count and
	 * is therefore an over-estimate of the term count -- operators are items too
	 * -- which is the safe direction: it can only refuse a shape that would have
	 * fitted, never admit one that would not.
	 */
	if (req->strategy == WEAVE_STRAT_DISTANCE && IsA(rhs, Const) &&
		!((Const *) rhs)->constisnull)
	{
		WeaveQuery	q = (WeaveQuery) DatumGetPointer(((Const *) rhs)->constvalue);

		req->maxchan = (int) q->nitems;
		if (req->maxchan < 1)
			return false;		/* an empty query matches nothing to fuse */
	}
	else if (req->strategy == WEAVE_STRAT_DISTANCE)
	{
		/*
		 * A parameterized wquery: the term count is unknowable until the scan
		 * runs.  Refuse, because the alternative is an AM that discovers at
		 * rescan time that it has more channels than the core accepts.  A user
		 * who wants a parameterized fused scan can spell the query as a literal
		 * or, once sect. 7b's warp-space work lands, revisit this bound.
		 */
		return false;
	}

	req->lhs = lhs;
	req->rhs = rhs;
	req->indexcol = col;
	return true;
}

/*
 * The weights array, as a plan-time expression to hang off the transport key.
 *
 * A Const array is validated here (one report, before the first row, rather than
 * one per row from the fallback) and passed through.  A NULL weights argument
 * means equal weights of 1.0 each -- NOT 1/n, so that adding a channel does not
 * rescale the ones already present -- and is materialized into a real array,
 * because the AM reads an array and has no way to be told "there wasn't one".
 *
 * Anything else -- a Param, an expression -- is REFUSED rather than transported.
 * It would work: the executor evaluates an ORDER BY key's right operand once per
 * rescan and the AM would see the value.  It is refused because the AM would then
 * be the first thing to learn that the array has the wrong length or a zero
 * weight, and its only recourse at that point is an ERROR from inside a scan.
 * Weights are a handful of literals in every shape sect. 7 describes.
 */
static Expr *
weave_fuse_weights_expr(FuncExpr *fcall, int nscores)
{
	Node	   *w = (Node *) list_nth(fcall->args, nscores);
	Datum	   *elems;
	ArrayType  *arr;
	int			i;

	if (!IsA(w, Const))
		return NULL;

	if (!((Const *) w)->constisnull)
	{
		(void) weave_fuse_weights(DatumGetArrayTypeP(((Const *) w)->constvalue),
								  nscores);
		return (Expr *) w;
	}

	elems = (Datum *) palloc(nscores * sizeof(Datum));
	for (i = 0; i < nscores; i++)
		elems[i] = Float4GetDatum(1.0f);
	arr = construct_array(elems, nscores, FLOAT4OID,
						  sizeof(float4), true, TYPALIGN_INT);
	pfree(elems);

	return (Expr *) makeConst(FLOAT4ARRAYOID, -1, InvalidOid, -1,
							  PointerGetDatum(arr), false, false);
}

/*
 * The `indexclauses` to reuse, so that a WHERE clause still becomes a REQUIRED
 * channel inside the fused scan (doc/specs/FUSED_TOPK.md sect. 3a (2): a gate is
 * conjunctive, and `required` comes from the clause having arrived as a QUAL and
 * never from the channel's kind).
 *
 * Taken from a core-generated IndexPath over the SAME index if there is one in
 * rel->pathlist, because building an IndexClause list by hand would be a second
 * implementation of match_clause_to_index().  NIL when there is none, and NIL is
 * SAFE rather than merely lossy: a qual that is not an index clause stays in
 * rel->baserestrictinfo, so create_indexscan_plan() puts it in the plan's filter
 * qual and the executor applies it.  The answer is the same; only the gate's
 * skipping is lost.
 *
 * ALL-OR-NOTHING, and only for `@@@` on a lexical column.  The fused pass honours
 * exactly one restriction key -- the boolean match set -- so a borrowed clause it
 * does not honour would be a pushed-down qual nobody evaluates: an Index Scan does
 * not re-check a pushed-down index qual, so rows failing it would come back with
 * plausible scores.  weave_rescan()'s cgram interaction is the same hazard one
 * level down and is why this refuses the whole list rather than filtering it.
 */
static List *
weave_fuse_borrow_indexclauses(RelOptInfo *rel, IndexOptInfo *index,
							   int lexcol)
{
	ListCell   *lc;

	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		IndexPath  *ipath;
		ListCell   *lc2;
		bool		allmatch = true;

		if (!IsA(path, IndexPath))
			continue;
		ipath = (IndexPath *) path;
		if (ipath->indexinfo != index || ipath->indexclauses == NIL)
			continue;

		foreach(lc2, ipath->indexclauses)
		{
			IndexClause *ic = (IndexClause *) lfirst(lc2);
			OpExpr	   *op;

			if (ic->indexcol != lexcol || ic->lossy ||
				list_length(ic->indexquals) != 1)
			{
				allmatch = false;
				break;
			}
			op = (OpExpr *) ((RestrictInfo *) linitial(ic->indexquals))->clause;
			if (!IsA(op, OpExpr) ||
				op->opno != weave_fuse_path_oids.match_op)
			{
				allmatch = false;
				break;
			}
		}
		if (allmatch)
			return ipath->indexclauses;
	}
	return NIL;
}

/*
 * set_rel_pathlist_hook.  Strictly additive: on any doubt it adds nothing and the
 * Sort over the fallback stands.
 */
static void
weave_fuse_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
							RangeTblEntry *rte)
{
	FuncExpr   *fcall;
	int			nscores;
	Expr	   *weights;
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION)
		return;
	if (rel->indexlist == NIL)
		return;
	if (!weave_fuse_resolve_oids() || !weave_fuse_path_resolve())
		return;

	fcall = weave_fuse_pathkey_call(root);
	if (fcall == NULL)
		return;
	nscores = list_length(fcall->args) - 1;
	if (nscores < 2)
		return;					/* one channel is not a fusion */

	weights = weave_fuse_weights_expr(fcall, nscores);
	if (weights == NULL)
		return;

	/* Try each weave index; the first that can serve every channel wins. */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		WeaveFuseChanReq *req;
		List	   *orderbys = NIL;
		List	   *orderbycols = NIL;
		List	   *indexclauses;
		int			lexcol = -1;
		int			transpchan = -1;
		int			totchan = 0;
		int			i;
		int			col;
		bool		ok = true;
		IndexPath  *ipath;

		if (index->relam != weave_fuse_path_oids.amoid)
			continue;

		/*
		 * The lexical column, which every weave index has exactly one of
		 * (weave_index_layout() throws otherwise).  It is where the transport key
		 * is hung, so a fused path cannot be built without it.
		 */
		for (col = 0; col < index->nkeycolumns; col++)
			if (index->opfamily[col] == weave_fuse_path_oids.lex_family)
			{
				lexcol = col;
				break;
			}
		if (lexcol < 0)
			continue;

		req = (WeaveFuseChanReq *) palloc0(nscores * sizeof(WeaveFuseChanReq));	/* alloc-ok: one per fuse() score argument, and fuse() has at most eight */
		for (i = 0; i < nscores && ok; i++)
		{
			Expr	   *arg = (Expr *) list_nth(fcall->args, i);

			if (!weave_fuse_attribute(root, rel, index, arg, &req[i]) ||
				!req[i].servable)
				ok = false;
			else
				totchan += req[i].maxchan;
		}

		/*
		 * One gate channel is reserved on top of the scored ones: a `@@@`
		 * restriction becomes one REQUIRED shuttle per segment in the fused pass,
		 * and the core's cap is per run, so one is the right reservation.  The cap
		 * is checked HERE because weave_fuse_init() refuses more than
		 * WEAVE_FUSE_MAX_CHAN channels, and an AM that refuses at rescan time a
		 * path the planner offered is doc/GAPS.md G39.
		 */
		if (ok && totchan + 1 > WEAVE_FUSE_MAX_CHAN)
			ok = false;
		if (!ok)
		{
			pfree(req);
			continue;
		}

		for (i = 0; i < nscores; i++)
		{
			orderbys = lappend(orderbys,
							   make_opclause(req[i].opno, FLOAT8OID, false,
											 req[i].lhs, req[i].rhs,
											 InvalidOid, InvalidOid));
			orderbycols = lappend_int(orderbycols, req[i].indexcol);
			if (transpchan < 0 && req[i].indexcol == lexcol)
				transpchan = i;
		}

		/*
		 * The transport key hangs off the LEXICAL column, so its left operand has
		 * to be an operand that matches THAT column -- not merely the first
		 * channel's, which would disagree with indexorderbycols the day a servable
		 * channel on another column exists, and fix_indexorderby_references() would
		 * then fail to rewrite the operand into an INDEX_VAR reference.  Every
		 * servable channel is lexical today, so this always finds one.
		 */
		if (transpchan < 0)
		{
			pfree(req);
			continue;
		}

		/*
		 * THE TRANSPORT KEY, last.  Its position in the list is not load-bearing
		 * -- weave_rescan() finds it by strategy number, not by index -- but the
		 * scored keys' RELATIVE order is: the AM assigns weights[j] to the j-th
		 * scored key it sees, and that order is the order of fuse()'s score
		 * arguments, which is the order of the weights array.  Appending the
		 * transport key after them keeps the two orders trivially aligned.
		 */
		orderbys = lappend(orderbys,
						   make_opclause(weave_fuse_path_oids.transport_op,
										 FLOAT8OID, false,
										 (Expr *) copyObject(req[transpchan].lhs),
										 weights, InvalidOid, InvalidOid));
		orderbycols = lappend_int(orderbycols, lexcol);

		indexclauses = weave_fuse_borrow_indexclauses(rel, index, lexcol);

		/*
		 * pathkeys = root->query_pathkeys is what elides the Sort, and it is
		 * honest: the AM returns rows in ascending fuse() value, which is what
		 * that pathkey asks for.  indexonly is false -- weave_canreturn() is
		 * false, the index stores postings and not the column.
		 */
		ipath = create_index_path(root, index,
								  indexclauses,
								  orderbys, orderbycols,
								  root->query_pathkeys,
								  ForwardScanDirection,
								  false,
								  rel->lateral_relids,
								  1.0,
								  false);
		add_path(rel, (Path *) ipath);
		pfree(req);
		return;					/* one fused path is enough */
	}
}

void
weave_fuse_install_pathlist_hook(void)
{
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = weave_fuse_set_rel_pathlist;
}

/*
 * `<~>`, the weights transport operator, which must never be evaluated.
 *
 * IT IS AN UNCONDITIONAL ERROR, and that is the whole design rather than
 * defensiveness.  sect. 7a rejected transporting the weights on a QUAL for a
 * specific reason: `indexqualorig` is re-evaluated by the executor during an EPQ
 * recheck, so a marker operator in a qual is eventually executed for real -- a
 * query that works until a concurrent UPDATE makes it not.  An ORDER BY key is
 * kept only as `indexorderbyorig`, for a reorder queue this AM does not request
 * (xs_recheckorderby is false), so nothing evaluates this.  Raising therefore
 * costs nothing and converts any future path that WOULD evaluate it from a silent
 * wrong ordering into a failure with a name.
 */
PG_FUNCTION_INFO_V1(weave_fuse_transport);

Datum
weave_fuse_transport(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("operator <~> cannot be evaluated outside a weave index scan"),
			 errdetail("It exists only to carry fuse() weights into the index access method as an ORDER BY scan key."),
			 errhint("Write \"ORDER BY fuse(col <=> query, ..., weights => '{...}')\" instead.")));
	PG_RETURN_NULL();			/* keep the compiler quiet */
}
