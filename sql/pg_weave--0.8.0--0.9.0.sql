/* pg_weave 0.8.0 -> 0.9.0 */

-- Channel-mechanism counters, so "which channel served this query" is a number
-- rather than a claim.
--
-- WHAT THEY ARE FOR.  pg_weave puts several retrieval structures inside ONE index,
-- and a query leaf can be resolved by more than one of them: a prefix (`term*`) by
-- a dictionary range walk or by a SuRF trie enumeration, a fuzzy term by a
-- dictionary scan or by the trie.  Those have different costs, and every claim
-- about either is unfalsifiable while nothing reports which one ran.
-- pg_stat_user_indexes answers "was the index used"; this answers "which part of
-- it did the work".
--
-- WHY NOT EXPLAIN, which is what doc/PHASES.md Z4 originally asked for.  There is
-- no AM-level EXPLAIN callback on either supported major -- `amexplain` is absent
-- from PostgreSQL 17's and 18's IndexAmRoutine (checked against 18.4's
-- access/amapi.h).  A CustomScan can print into EXPLAIN, but pg_weave generates one
-- for a single plan shape (the count(*) pushdown), so EXPLAIN would name the
-- channel for a minority of queries and stay silent for the rest, including the
-- bitmap scan that is the common @@@ path.  A counter read from SQL covers every
-- plan shape.
--
-- WHAT "WHICH MECHANISM" MEANS HERE, because the first draft of these columns got it
-- wrong.  Prefix, fuzzy and regex all return CORRECT ROWS today: prefix through a
-- dictionary range walk, fuzzy through a Levenshtein automaton walked over the sorted
-- dictionary with dead-end prefix skipping, and regex -- plus any fuzzy term too long
-- for that automaton -- through the trigram funnel followed by an exact recheck.  What
-- none of them has is a shuttle with a real bound, which is what lets a channel join a
-- fused top-k, and that is the sense in which doc/PRODUCTION_READINESS.md counts them
-- as not answering.  A nonzero fuzzy_dict is NOT "Z5 is done".
--
-- The three _surf columns and prefix_surf are the structurally-zero ones: prefix_surf
-- waits on Z4's resident trie, fuzzy_surf on Z5 plus that trie, regex_surf on Z6's
-- trigram tiling.  Their zeros are asserted in sql/chanstats.sql so that routing a
-- channel shows up there as a diff.
--
-- A fuzzy or regex query that ran with BOTH its columns at zero fell back to a full
-- scan with recheck: the funnel refuses a pattern with too few usable trigrams.  That
-- is derivable from the pair of zeros, which is why it has no column of its own.
--
-- BACKEND-LOCAL, like weave_alloc_stats() and for the same reasons: the counters
-- are process state, not index state, so attributing them to an index would mean
-- shared memory and a stats collector for something whose only job is to say which
-- branch ran.  A zero therefore means "this mechanism served nothing in THIS
-- backend" and never "nothing happened" -- reset, query and read in ONE session.
CREATE FUNCTION weave_channel_stats(
        OUT lex_term bigint,
        OUT prefix_dict bigint,
        OUT prefix_surf bigint,
        OUT fuzzy_dict bigint,
        OUT fuzzy_trgm bigint,
        OUT fuzzy_surf bigint,
        OUT regex_trgm bigint,
        OUT regex_surf bigint,
        OUT vector_scan bigint,
        OUT terms_expanded bigint,
        OUT dict_pages bigint,
        OUT surf_loads bigint,
        OUT surf_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_channel_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_channel_stats() IS
    'which mechanism inside a weave index served this backend''s query leaves';

CREATE FUNCTION weave_channel_stats_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'weave_channel_stats_reset'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_channel_stats_reset() IS
    'zero this backend''s channel-mechanism counters';
