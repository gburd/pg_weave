/* pg_weave 0.4.0 -> 0.5.0
 *
 * Tasks Z1/Z2: the fuzzy/regex/prefix channel enters the build.
 *
 * This script is intentionally empty of DDL.  Everything Z1 and Z2 added is
 * C-side: the vendored TRE library (vendor/tre, laurikari/tre @ d0e0c99,
 * BSD-2), the query-compilation front end imported from pg_tre
 * (src/query/{surf,uleven,regex_ast,regex_grammar,regex_tokens,parser,extract,
 * tiling,like_translate,pattern_cache,trgm_similarity,re_match}.c), and the
 * five GUCs plus two deadline triads in src/query/fuzzy_guc.c.
 *
 * GUCs are registered at module load, not in the extension's catalog, so they
 * appear in pg_settings with no SQL object to create:
 *
 *     pg_weave.max_extraction_fanout
 *     pg_weave.max_nfa_states
 *     pg_weave.compile_timeout_ms
 *     pg_weave.match_timeout_ms
 *     pg_weave.similarity_threshold
 *
 * The channel is deliberately not reachable yet: no operator, opclass, or
 * planner support references it (tasks Z3 onward).  A compiling, linked, but
 * unreachable channel is the Z1/Z2 milestone -- see doc/PHASES.md.
 *
 * The script exists anyway because an extension whose default_version
 * advances without an upgrade script cannot be upgraded in place: ALTER
 * EXTENSION pg_weave UPDATE would fail with "extension \"pg_weave\" has no
 * update path from version 0.4.0 to version 0.5.0", stranding every existing
 * installation on 0.4.0.  sql/wvec.sql exercises exactly that path.
 */

\echo Use "ALTER EXTENSION pg_weave UPDATE TO '0.5.0'" to load this file. \quit

-- no catalog changes in 0.5.0
