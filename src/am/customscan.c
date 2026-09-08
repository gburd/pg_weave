/*-------------------------------------------------------------------------
 *
 * pg_weave_customscan.c
 *	  CustomScan providers for pg_weave:
 *	    1. COUNT pushdown -- answer  SELECT count(*) ... WHERE col @@@ q
 *	       from the weave index (VM-based bulk count) instead of a bitmap heap
 *	       scan, ~3x faster on a common term.
 *	    (later stages add a parallel ranked top-k CustomScan)
 *
 * The providers are installed by _PG_init via create_upper_paths_hook (count)
 * and set_rel_pathlist_hook (ranked).  They are strictly additive: a candidate
 * CustomPath is only *added* alongside the normal paths, so if anything about
 * the shape is unsupported we simply add nothing and the planner uses the
 * ordinary plan.  Nothing here changes results -- only the mechanism.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "commands/defrem.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "utils/guc.h"
#include "executor/tuptable.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "weave/weave.h"
#include "weave/am.h"			/* weave_init_reloptions */

/* engine entry point implemented in pg_weave_am_scan.c (via pg_weave_am.c) */
extern int64 weave_count_visible_oid(Oid indexoid, WeaveQuery q);

PG_FUNCTION_INFO_V1(pg_weave_customscan_dummy);	/* keeps the file non-empty for old toolchains */
Datum
pg_weave_customscan_dummy(PG_FUNCTION_ARGS)
{
	PG_RETURN_NULL();
}

/* ---- saved previous hooks (chain, do not clobber) ---- */
static create_upper_paths_hook_type prev_upper_paths_hook = NULL;

/* cached OID of the @@@ (wdoc, wquery) operator; resolved lazily */
static Oid	weave_match_op = InvalidOid;

/* ===== count-pushdown CustomScan: path/plan/exec ===== */

typedef struct WeaveCountScanState
{
	CustomScanState css;
	Oid			indexoid;
	WeaveQuery	query;
	bool		done;
} WeaveCountScanState;

static Plan *WeaveCountPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
									struct CustomPath *best_path, List *tlist,
									List *clauses, List *custom_plans);
static Node *WeaveCountCreateScanState(CustomScan *cscan);
static void WeaveCountBeginScan(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *WeaveCountExecScan(CustomScanState *node);
static void WeaveCountEndScan(CustomScanState *node);
static void WeaveCountReScan(CustomScanState *node);

static const CustomPathMethods weave_count_path_methods = {
	.CustomName = "WeaveCount",
	.PlanCustomPath = WeaveCountPlanCustomPath,
};

static const CustomScanMethods weave_count_scan_methods = {
	.CustomName = "WeaveCount",
	.CreateCustomScanState = WeaveCountCreateScanState,
};

static const CustomExecMethods weave_count_exec_methods = {
	.CustomName = "WeaveCount",
	.BeginCustomScan = WeaveCountBeginScan,
	.ExecCustomScan = WeaveCountExecScan,
	.EndCustomScan = WeaveCountEndScan,
	.ReScanCustomScan = WeaveCountReScan,
};

/*
 * Resolve the @@@ operator OID in the extension's schema.  Returns InvalidOid
 * if pg_weave's SQL objects are not installed in this database's search path
 * (then the pushdown simply never triggers).
 */
static Oid
weave_lookup_match_op(void)
{
	if (OidIsValid(weave_match_op))
		return weave_match_op;
	/* @@@ (wdoc, wquery) */
	weave_match_op = OpernameGetOprid(list_make1(makeString("@@@")),
									TypenameGetTypid("wdoc"),
									TypenameGetTypid("wquery"));
	return weave_match_op;
}

/*
 * If the RestrictInfo list contains exactly one clause of the form
 *   <indexable expr> @@@ <WeaveQuery Const>
 * covered by a weave index on `rel`, return the index OID and the query Const;
 * else InvalidOid.
 */
static Oid
weave_find_pushdown_index(PlannerInfo *root, RelOptInfo *rel,
						List *baserestrictinfo, WeaveQuery *query_out)
{
	Oid			matchop = weave_lookup_match_op();
	RangeTblEntry *rte;
	Relation	heap;
	ListCell   *lc;
	OpExpr	   *matchclause = NULL;
	int			nquals = 0;

	if (!OidIsValid(matchop))
		return InvalidOid;
	if (rel->reloptkind != RELOPT_BASEREL || rel->rtekind != RTE_RELATION)
		return InvalidOid;

	/* need exactly one qual, and it must be the @@@ operator */
	foreach(lc, baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		OpExpr	   *op;

		nquals++;
		if (!IsA(ri->clause, OpExpr))
			return InvalidOid;
		op = (OpExpr *) ri->clause;
		if (op->opno != matchop || list_length(op->args) != 2)
			return InvalidOid;
		matchclause = op;
	}
	if (nquals != 1 || matchclause == NULL)
		return InvalidOid;

	/* the right-hand side must be a plan-time constant WeaveQuery */
	{
		Node	   *rhs = (Node *) lsecond(matchclause->args);

		if (!IsA(rhs, Const) || ((Const *) rhs)->constisnull)
			return InvalidOid;
		*query_out = (WeaveQuery) DatumGetPointer(((Const *) rhs)->constvalue);
	}

	/* find a weave index on this rel whose expression matches the LHS */
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION)
		return InvalidOid;
	heap = table_open(rte->relid, AccessShareLock);
	{
		List	   *indexoidlist = RelationGetIndexList(heap);
		ListCell   *ic;
		Oid			found = InvalidOid;
		Node	   *lhs = (Node *) linitial(matchclause->args);

		foreach(ic, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(ic);
			Relation	ind = index_open(indexoid, AccessShareLock);

			if (ind->rd_rel->relam == get_index_am_oid("weave", true))
			{
				if (ind->rd_indexprs != NIL)
				{
					/* expression index (e.g. USING weave (to_wdoc(body))):
					 * the LHS must equal the index expression. */
					if (equal(linitial(ind->rd_indexprs), lhs))
						found = indexoid;
				}
				else if (ind->rd_index->indnatts == 1 &&
						 IsA(lhs, Var) &&
						 ((Var *) lhs)->varno == rel->relid &&
						 ((Var *) lhs)->varattno == ind->rd_index->indkey.values[0])
				{
					/* plain-column index (USING weave (d)): the LHS must be the
					 * Var for that single indexed column.  This is the stored-
					 * wdoc-column form the docs recommend; without this the
					 * count pushdown only fired for expression indexes and a
					 * stored-column count(*) fell back to a slow bitmap scan. */
					found = indexoid;
				}
			}
			index_close(ind, AccessShareLock);
			if (OidIsValid(found))
				break;
		}
		list_free(indexoidlist);
		table_close(heap, AccessShareLock);
		return found;
	}
}

/*
 * create_upper_paths_hook: at the GROUP/AGG stage, if the query is a bare
 * COUNT(*) over a single base rel whose only qual is `col @@@ q` with a weave
 * index, add a CustomScan path that answers the count from the index.
 */
static void
weave_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel,
					   void *extra)
{
	Query	   *parse = root->parse;
	RelOptInfo *baserel;
	Oid			indexoid;
	WeaveQuery	query;
	CustomPath *cpath;

	if (prev_upper_paths_hook)
		prev_upper_paths_hook(root, stage, input_rel, output_rel, extra);

	if (stage != UPPERREL_GROUP_AGG)
		return;
	/* bare aggregate: exactly one COUNT(*), no GROUP BY / HAVING / DISTINCT / window / set-op */
	if (parse->groupClause || parse->groupingSets || parse->havingQual ||
		parse->distinctClause || parse->hasWindowFuncs || parse->setOperations ||
		parse->hasDistinctOn || list_length(parse->targetList) != 1)
		return;
	if (list_length(parse->rtable) != 1)
		return;
	{
		TargetEntry *te = (TargetEntry *) linitial(parse->targetList);
		Aggref	   *agg;

		if (!IsA(te->expr, Aggref))
			return;
		agg = (Aggref *) te->expr;
		/* count(*) : COUNT with no args, no FILTER, no DISTINCT, no ORDER BY */
		if (agg->aggfnoid != F_COUNT_ ||
			agg->args != NIL || agg->aggfilter != NULL ||
			agg->aggdistinct != NIL || agg->aggorder != NIL)
			return;
	}

	/* the single base rel */
	if (bms_num_members(input_rel->relids) != 1)
		return;
	baserel = find_base_rel(root, bms_singleton_member(input_rel->relids));
	indexoid = weave_find_pushdown_index(root, baserel, baserel->baserestrictinfo,
									   &query);
	if (!OidIsValid(indexoid))
		return;

	/* build the CustomPath -- rows=1.
	 *
	 * Cost model: the count is answered from the weave index + the visibility
	 * map (weave_count_visible_oid), visiting NO heap tuples -- unlike the
	 * Bitmap Index Scan + Aggregate alternative, whose cost scales with the
	 * number of matching heap tuples it must fetch/recheck.  The old estimate
	 * (baserel->pages) priced this at the whole heap and always lost to the
	 * bitmap path even though the VM-based count is measurably faster on
	 * common terms.  Price it as a small dictionary/posting walk (a handful of
	 * index pages, VM-only) so the planner chooses the pushdown when it applies;
	 * this stays an underestimate of the true cost only relative to a full heap
	 * scan, and the pushdown is exact (index-native, no seq fallback). */
	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = output_rel;
	cpath->path.pathtarget = output_rel->reltarget;
	cpath->path.param_info = NULL;
	cpath->path.rows = 1;
	{
		/* index-only walk: a few index pages + the VM, no heap-tuple visits.
		 * Price it at a small fraction of the heap so it beats the Bitmap Index
		 * Scan + Aggregate alternative (whose cost scales with matching-tuple
		 * fetches) yet still scales mildly with table size.  The pushdown is
		 * exact and index-native; this only changes the planner's choice
		 * between two correct count paths. */
		double		c = (double) baserel->pages * 0.01 + 1.0;

		cpath->path.startup_cost = 0.0;
		cpath->path.total_cost = c;
	}
	cpath->flags = 0;
	cpath->custom_paths = NIL;
	{
		/*
		 * Carry the query into the plan as a proper VARLENA Const, not a bare
		 * INTERNALOID pointer.  WeaveQuery is a varlena blob; an INTERNALOID Const
		 * (pass-by-value, typlen 8) makes copyObject/the plan cache copy only the
		 * 8-byte POINTER, so a cached or re-executed plan (e.g. a count(*) inside
		 * a plpgsql loop) dereferences the query after its planning context is
		 * freed -- reading garbage (a bogus nitems), underflowing the RPN eval
		 * stack, and crashing (SIGSEGV) or returning wrong counts.  Storing it as
		 * a varlena Const (typlen -1, byval false) of the wquery type makes
		 * datumCopy() deep-copy the whole blob with the plan, so it lives exactly
		 * as long as the plan that references it.  Copy into the current (planner)
		 * context up front so the Const owns its own copy.
		 */
		Oid			wqueryoid = TypenameGetTypid("wquery");
		Datum		qcopy = datumCopy(PointerGetDatum(query), false, -1);

		cpath->custom_private =
			list_make2(makeInteger((int) indexoid),
					   makeConst(OidIsValid(wqueryoid) ? wqueryoid : BYTEAOID,
								 -1, InvalidOid, -1, qcopy, false, false));
	}
	cpath->methods = &weave_count_path_methods;
	add_path(output_rel, (Path *) cpath);
}

static Plan *
WeaveCountPlanCustomPath(PlannerInfo *root, RelOptInfo *rel,
					   struct CustomPath *best_path, List *tlist,
					   List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.scanrelid = 0;	/* no base rel scanned at exec time */
	cscan->custom_scan_tlist = tlist;
	cscan->custom_private = best_path->custom_private;
	cscan->methods = &weave_count_scan_methods;
	return &cscan->scan.plan;
}

static Node *
WeaveCountCreateScanState(CustomScan *cscan)
{
	WeaveCountScanState *st = (WeaveCountScanState *) newNode(sizeof(WeaveCountScanState),
														 T_CustomScanState);
	Const	   *qc;

	st->css.methods = &weave_count_exec_methods;
	st->indexoid = (Oid) intVal(linitial(cscan->custom_private));
	qc = (Const *) lsecond(cscan->custom_private);
	st->query = (WeaveQuery) DatumGetPointer(qc->constvalue);
	st->done = false;
	return (Node *) st;
}

static void
WeaveCountBeginScan(CustomScanState *node, EState *estate, int eflags)
{
	/* nothing to set up beyond the tuple slot the executor made */
}

static TupleTableSlot *
WeaveCountExecScan(CustomScanState *node)
{
	WeaveCountScanState *st = (WeaveCountScanState *) node;
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
	int64		c;

	if (st->done)
		return NULL;
	st->done = true;

	c = weave_count_visible_oid(st->indexoid, st->query);

	ExecClearTuple(slot);
	slot->tts_values[0] = Int64GetDatum(c);
	slot->tts_isnull[0] = false;
	ExecStoreVirtualTuple(slot);
	return slot;
}

static void
WeaveCountEndScan(CustomScanState *node)
{
}

static void
WeaveCountReScan(CustomScanState *node)
{
	((WeaveCountScanState *) node)->done = false;
}

/* ===== module init ===== */

void		_PG_init(void);

#ifdef WEAVE_TEST_HOOKS
/*
 * Test-only: a scan pauses on this advisory key right after snapshotting the
 * metapage in weave_collect_matches, so a concurrent session can free + recycle
 * the snapshotted segment's pages in exactly the vulnerable window (the A1
 * scan-vs-merge race).  0 = off (default, and the only value in production).
 * Guarded by -DWEAVE_TEST_HOOKS so the hook does not exist in a normal build.
 */
int			pg_weave_test_pause_advisory_key = 0;
#endif

void
_PG_init(void)
{
	weave_init_reloptions();
	RegisterCustomScanMethods(&weave_count_scan_methods);

	/* Fuzzy/regex channel GUCs (src/query/fuzzy_guc.c).  Registered here because
	 * this is the module's single documented entry point; see that file's note. */
	pg_weave_init_fuzzy_guc();

	/*
	 * Cap (in MB) on the total index size for which an index BUILD finalizes to
	 * a single optimal segment.  Above this, the build stops at a bounded,
	 * size-tiered set of segments (LSM-style) so it always converges instead of
	 * doing one giant single-backend collapse that can run for hours on a huge,
	 * high-vocabulary corpus.  Ranked scans then traverse a bounded handful of
	 * segments (a small, fixed cost); run weave_merge() in a maintenance window to
	 * collapse to one when desired.  0 = always collapse (the historical behavior).
	 */
	/*
	 * Initial k for the ranked (block-max WAND) scan.  See the comment at the
	 * so->curk assignment in amscan.c: PostgreSQL cannot tell an access method the
	 * query's LIMIT, so the scan starts here and grows x4 on demand.  Too high and
	 * every LIMIT 10 query pays for a top-100 pass; too low and a LIMIT 100 query
	 * pays for repeated passes.  Swept by bench/compete.
	 */
	DefineCustomIntVariable("pg_weave.wand_initial_k",
							"Initial top-k width for a ranked WAND scan before growing on demand.",
							"PostgreSQL does not expose the query LIMIT to an index access method, so a ranked scan starts at this k and grows 4x when the executor asks for more. Lower favours a first page of results; higher favours deep pagination in one pass.",
							&pg_weave_wand_initial_k,
							32, 1, 100000,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pg_weave.build_collapse_max_mb",
							"Max total index size (MB) for which a build finalizes to a single segment; larger builds stop at a bounded tiered set.",
							"Above this, an index build leaves a bounded, size-tiered set of segments so it always converges; run weave_merge() to collapse to one. 0 = always collapse.",
							&pg_weave_build_collapse_max_mb,
							4096, 0, INT_MAX,
							PGC_USERSET, GUC_UNIT_MB, NULL, NULL, NULL);

	DefineCustomIntVariable("pg_weave.build_mem_ceiling_mb",
							"Per-participant build flush-budget growth ceiling (MB); 0 = 2*maintenance_work_mem.",
							"Raise to trade RAM for fewer, larger segments on a very large build so its segment count stays under the cap. Peak build memory is about (max_parallel_maintenance_workers + 1) * this. 0 keeps the memory-safe default ceiling.",
							&pg_weave_build_mem_ceiling_mb,
							0, 0, INT_MAX,
							PGC_USERSET, GUC_UNIT_MB, NULL, NULL, NULL);

#ifdef WEAVE_TEST_HOOKS
	/*
	 * TEST-ONLY build.  This GUC only exists when compiled with
	 * -DWEAVE_TEST_HOOKS (never in a release build recipe -- Makefile, meson,
	 * and flake all omit it).  Announce loudly at load so a test-hook build can
	 * never be mistaken for, or silently shipped as, a production build.
	 */
	ereport(WARNING,
			(errmsg("pg_weave was built with WEAVE_TEST_HOOKS: this is a TEST build, not for production")));
	DefineCustomIntVariable("pg_weave.test_pause_advisory_key",
							"TEST-ONLY: advisory key a scan waits on mid-collect (0=off).",
							NULL,
							&pg_weave_test_pause_advisory_key,
							0, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
#endif

	prev_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = weave_create_upper_paths;
}
