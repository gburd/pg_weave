/*-------------------------------------------------------------------------
 *
 * regex.h -- fuzzy/regex/prefix channel: GUCs and TRE deadline plumbing
 *
 * The declarations the pg_tre import needs from its host module.  pg_tre
 * kept these in pg_tre.h and defined them in src/module.c; neither file was
 * imported (see doc/specs/IMPORT_pg_tre.md, "Wiring TODO"), so they live
 * here and are defined in src/query/fuzzy_guc.c.
 *
 * Included from weave/weave.h, because uleven.c, tiling.c, extract.c,
 * pattern_cache.c and trgm_similarity.c reach these symbols through
 * #include "weave/weave.h".
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_REGEX_H
#define WEAVE_REGEX_H

/* ---- GUCs, defined and registered in src/query/fuzzy_guc.c ---- */

/*
 * pg_weave.max_extraction_fanout -- ceiling on the number of trigram
 * disjuncts a single query may expand into.  Read by extract.c and
 * tiling.c; hitting it degrades the plan to "always true" (a full recheck)
 * rather than raising, so the cap is a cost bound, never a correctness one.
 */
extern int	pg_weave_max_extraction_fanout;

/*
 * pg_weave.max_nfa_states -- reject a pattern whose compiled TRE automaton
 * has more states than this.  Read by pattern_cache.c.  Per-string match
 * cost is O(num_states * strlen * max_cost); this bounds the first factor.
 */
extern int	pg_weave_max_nfa_states;

/*
 * pg_weave.compile_timeout_ms / pg_weave.match_timeout_ms -- wall-clock
 * budgets enforced from inside the vendored TRE loops via the two weak
 * hooks (see weave/re_match.h).  TRE's compile and match paths have no
 * CHECK_FOR_INTERRUPTS reach, so without these a pathological pattern
 * spins the backend uninterruptibly.
 */
extern int	pg_weave_compile_timeout_ms;
extern int	pg_weave_match_timeout_ms;

/*
 * pg_weave.similarity_threshold -- the threshold the `%` operator uses in
 * trgm_similarity.c.  Default 0.3, matching pg_trgm so queries port.
 */
extern double pg_weave_similarity_threshold;

/*
 * Register all of the above.  Idempotent: safe to call from _PG_init even
 * though the module-load constructor in fuzzy_guc.c already called it.
 */
extern void pg_weave_init_fuzzy_guc(void);

/* ---- compile-deadline triad (used by src/query/pattern_cache.c) ---- */

/*
 * Arm a wall-clock deadline for a following weave_compile_pattern() call and
 * install the TRE compile progress hook.  timeout_ms <= 0 means "use
 * pg_weave.compile_timeout_ms".  Re-entrant: nested arms tighten the
 * deadline and only the outermost disarm uninstalls the hook.  Every arm
 * must be paired with a disarm, preferably via PG_TRY/PG_FINALLY.
 */
extern void pg_weave_arm_compile_deadline(int timeout_ms);
extern void pg_weave_disarm_compile_deadline(void);

/*
 * Raise ERRCODE_QUERY_CANCELED if the most recent armed compile aborted on
 * the deadline.  Call right after weave_compile_pattern() returns NULL: it
 * is what distinguishes a timeout from a syntax error, since TRE reports
 * both as a failed compile.
 */
extern void pg_weave_check_compile_timeout(void);

/* ---- match-deadline triad (for whoever calls weave_do_match()) ---- */

/*
 * Same shape as the compile triad, driving the separate match hook.
 * pg_weave_check_match_timeout() takes the result of the weave_do_match()
 * call so a timed-out match is never mistaken for a no-match -- which would
 * silently drop rows.  struct WeaveMatchResult is only forward-declared, so
 * weave/weave.h does not have to pull in the TRE-facing weave/re_match.h.
 */
struct WeaveMatchResult;

extern void pg_weave_arm_match_deadline(int timeout_ms);
extern void pg_weave_disarm_match_deadline(void);
extern void pg_weave_check_match_timeout(const struct WeaveMatchResult *r);

#endif							/* WEAVE_REGEX_H */
