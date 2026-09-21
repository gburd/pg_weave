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
 * WHAT THIS FILE DELIBERATELY DOES NOT DO.  There is no IndexPath here, no
 * set_rel_pathlist_hook, and no scan key: the pushdown is F2.2, which needs the
 * AM side, and sect. 7a (3) establishes that no channel this example uses is an
 * ORDER BY operand yet (tasks F6 and F7).  A path that nothing can serve is
 * worse than no path.  What F2.1 owes F2.2 instead is a SHAPE: after the
 * rewrite, a recognized channel argument is exactly
 *
 *	  FuncExpr(weave_lexscore | weave_cosscore, args = [ the original OpExpr ])
 *
 * with the OpExpr preserved intact, because that OpExpr is what F2.2 will lift
 * into `indexorderbys`.  This file is the only producer of that shape and F2.2's
 * matcher is its only intended consumer; if a third party starts to depend on
 * it, it needs a name and a header, not a grep.
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

#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/supportnodes.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

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
	Oid			lexscore_fn;	/* weave_lexscore(float8) */
	Oid			cosscore_fn;	/* weave_cosscore(float8) */
} WeaveFuseOids;

static WeaveFuseOids weave_fuse_oids = {InvalidOid, InvalidOid, InvalidOid,
	InvalidOid, InvalidOid, InvalidOid
};

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
	n.lexscore_fn = LookupFuncName(list_make2(makeString(nspname),
											  makeString("weave_lexscore")),
								   1, argtypes, true);
	n.cosscore_fn = LookupFuncName(list_make2(makeString(nspname),
											  makeString("weave_cosscore")),
								   1, argtypes, true);

	if (!OidIsValid(n.lex_op) || !OidIsValid(n.lex_commop) ||
		!OidIsValid(n.vec_op) || !OidIsValid(n.lexscore_fn) ||
		!OidIsValid(n.cosscore_fn))
		return false;

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
