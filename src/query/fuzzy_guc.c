/*-------------------------------------------------------------------------
 *
 * fuzzy_guc.c -- GUCs and TRE compile/match deadline enforcement for the
 *				  fuzzy/regex/prefix channel
 *
 * pg_tre kept these in src/module.c, which was deliberately not imported
 * (pg_weave has its own module init in src/am/customscan.c).  Everything the
 * imported query-compilation front end needs from its host module is here:
 * the five GUCs listed in doc/specs/IMPORT_pg_tre.md "Wiring TODO", and the
 * two wall-clock deadline triads that drive the weak hooks the vendored TRE
 * library calls (vendor/tre/patches/tre-progress-hook.patch).
 *
 * Why the deadlines are not optional: TRE's compile path expands bounded
 * repetitions by copying the AST, so a{1000}{1000} is quadratic-to-worse
 * work inside tre_regncomp() with no CHECK_FOR_INTERRUPTS reach, and the
 * matcher's per-position NFA loop likewise never returns to PostgreSQL until
 * it is done.  A backend stuck in either is unkillable.  vendor/tre calls
 * tre_compile_progress_check() / tre_progress_check() (defined in
 * src/query/re_match.c, which must not include postgres.h); those forward to
 * the hooks installed here, which are the only place wall-clock time and
 * ereport() enter the picture.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/guc.h"
#include "utils/timestamp.h"

#include "weave/regex.h"
#include "weave/re_match.h"

/* ---- GUC storage.  Defaults duplicated in the Define...() calls below
 * because DefineCustomIntVariable assigns the boot value itself; keeping
 * both in sync is what pg_tre did and what PostgreSQL contrib does. ---- */

int			pg_weave_max_extraction_fanout = 4096;
int			pg_weave_max_nfa_states = 10000;
int			pg_weave_compile_timeout_ms = 1000;
int			pg_weave_match_timeout_ms = 1000;
double		pg_weave_similarity_threshold = 0.3;	/* pg_trgm-compatible */

void
pg_weave_init_fuzzy_guc(void)
{
	static bool done = false;

	/*
	 * Idempotent.  Today the only caller is the module-load constructor
	 * below, but _PG_init() (src/am/customscan.c) is where this belongs, and
	 * DefineCustomIntVariable() raises on a redefinition -- so adding the
	 * call there must not double-register.
	 */
	if (done)
		return;
	done = true;

	DefineCustomIntVariable("pg_weave.max_extraction_fanout",
							"Maximum number of trigram disjuncts a fuzzy or regex query may expand into.",
							"Reaching the cap does not fail the query: extraction gives up and the plan falls back to a full recheck, so this is a planning-cost bound and never a correctness one. Raise it to let a very alternation-heavy pattern keep its trigram pre-filter.",
							&pg_weave_max_extraction_fanout,
							4096, 1, 65536,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pg_weave.max_nfa_states",
							"Reject a regex whose compiled NFA exceeds this state count.",
							"Per-string match cost is roughly O(states * length * max_cost). This bounds the first factor, so a pattern that compiles into a huge automaton is refused up front instead of making every row expensive.",
							&pg_weave_max_nfa_states,
							10000, 32, 1000000,
							PGC_USERSET, 0, NULL, NULL, NULL);

	DefineCustomIntVariable("pg_weave.compile_timeout_ms",
							"Wall-clock budget for compiling one regular expression.",
							"Enforced from inside the vendored TRE compiler's AST-expansion loops, which have no CHECK_FOR_INTERRUPTS reach of their own. Without it a nested bounded repetition such as a{1000}{1000} makes the backend unresponsive until the compile finishes.",
							&pg_weave_compile_timeout_ms,
							1000, 1, 600000,
							PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomIntVariable("pg_weave.match_timeout_ms",
							"Wall-clock budget for matching one string against a compiled regular expression.",
							"Enforced once per input position from inside the TRE matcher loop. A match cut short raises ERRCODE_QUERY_CANCELED; it is never reported as a non-match, which would silently drop rows.",
							&pg_weave_match_timeout_ms,
							1000, 1, 600000,
							PGC_USERSET, GUC_UNIT_MS, NULL, NULL, NULL);

	DefineCustomRealVariable("pg_weave.similarity_threshold",
							 "Threshold the % operator uses for trigram-set similarity.",
							 "text % text is true when the trigram similarity of the two arguments is at least this value. Mirrors pg_trgm.similarity_threshold, including the default, so queries port unchanged.",
							 &pg_weave_similarity_threshold,
							 0.3, 0.0, 1.0,
							 PGC_USERSET, 0, NULL, NULL, NULL);
}

/*
 * Registration happens from _PG_init() in src/am/customscan.c, which is the
 * documented PostgreSQL extension entry point.
 *
 * An earlier revision registered from an ELF module-load constructor because
 * customscan.c was being edited concurrently.  That worked -- the constructor runs
 * inside the same dlopen() internal_load_library() performs just before calling
 * _PG_init(), and DefineCustom*Variable allocates with guc_malloc() rather than
 * palloc() so it does not depend on CurrentMemoryContext -- but it ran before the
 * PG_MODULE_MAGIC check and MSVC has no equivalent attribute, so the Windows build
 * would silently have had no fuzzy GUCs at all.
 */

/* ====================================================================
 * Compile-timeout enforcement (pg_weave.compile_timeout_ms).
 *
 * Per-backend and single-threaded: nested arms share one deadline, the
 * tightest one wins, and only the outermost disarm uninstalls the hook.
 * ==================================================================== */

static TimestampTz compile_deadline = 0;	/* 0 == not armed */
static int	compile_arm_depth = 0;
static bool compile_timed_out = false;

/*
 * Handed to TRE as a plain-C callback.  Must be cheap (it is polled from
 * inside the expansion loops) and must not throw -- it only sets a flag; the
 * ereport() happens in pg_weave_check_compile_timeout(), back on our side of
 * the ABI boundary.
 */
static int
compile_progress_hook(void)
{
	if (compile_deadline != 0 &&
		GetCurrentTimestamp() >= compile_deadline)
	{
		compile_timed_out = true;
		return 1;				/* deadline exceeded: abort the compile */
	}
	return 0;
}

void
pg_weave_arm_compile_deadline(int timeout_ms)
{
	int			ms = (timeout_ms > 0) ? timeout_ms : pg_weave_compile_timeout_ms;
	TimestampTz deadline;

	if (ms <= 0)
		return;					/* timeout disabled */

	deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), ms);

	if (compile_arm_depth == 0)
	{
		compile_deadline = deadline;
		compile_timed_out = false;
		(void) weave_set_compile_progress_hook(compile_progress_hook);
	}
	else if (deadline < compile_deadline)
		compile_deadline = deadline;	/* tighten */

	compile_arm_depth++;
}

void
pg_weave_disarm_compile_deadline(void)
{
	if (compile_arm_depth == 0)
		return;
	if (--compile_arm_depth == 0)
	{
		compile_deadline = 0;
		(void) weave_set_compile_progress_hook(NULL);
	}
}

void
pg_weave_check_compile_timeout(void)
{
	if (compile_timed_out)
	{
		compile_timed_out = false;
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("pg_weave: regex compilation exceeded pg_weave.compile_timeout_ms = %d ms",
						pg_weave_compile_timeout_ms),
				 errhint("Simplify the pattern (reduce nested bounded repetitions) or raise pg_weave.compile_timeout_ms for trusted callers.")));
	}
}

/* ====================================================================
 * Match-timeout enforcement (pg_weave.match_timeout_ms).
 *
 * Same shape as above, driving the separate match hook so arming one never
 * perturbs the other.  The channel code that eventually calls
 * weave_do_match() (task Z4) must arm/disarm around it and pass the result
 * to pg_weave_check_match_timeout().
 * ==================================================================== */

static TimestampTz match_deadline = 0;	/* 0 == not armed */
static int	match_arm_depth = 0;

static int
match_progress_hook(void)
{
	if (match_deadline != 0 &&
		GetCurrentTimestamp() >= match_deadline)
		return 1;				/* deadline exceeded: abort the match */
	return 0;
}

void
pg_weave_arm_match_deadline(int timeout_ms)
{
	int			ms = (timeout_ms > 0) ? timeout_ms : pg_weave_match_timeout_ms;
	TimestampTz deadline;

	if (ms <= 0)
		return;					/* timeout disabled */

	deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), ms);

	if (match_arm_depth == 0)
	{
		match_deadline = deadline;
		(void) weave_set_progress_hook(match_progress_hook);
	}
	else if (deadline < match_deadline)
		match_deadline = deadline;	/* tighten */

	match_arm_depth++;
}

void
pg_weave_disarm_match_deadline(void)
{
	if (match_arm_depth == 0)
		return;
	if (--match_arm_depth == 0)
	{
		match_deadline = 0;
		(void) weave_set_progress_hook(NULL);
	}
}

/*
 * A timed-out match is an error, not a non-match.  Reporting it as "no
 * match" would drop rows from a correct answer with no way to notice -- the
 * same failure mode the channel bound contracts guard against.
 */
void
pg_weave_check_match_timeout(const struct WeaveMatchResult *r)
{
	if (r != NULL && r->timed_out)
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("pg_weave: regex match exceeded pg_weave.match_timeout_ms = %d ms",
						pg_weave_match_timeout_ms),
				 errhint("Simplify the pattern, reduce the maximum edit cost, or raise pg_weave.match_timeout_ms for trusted callers.")));
}
