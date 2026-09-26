/* pg_weave 0.22.0 -> 0.23.0 */

-- weave_work_stats() gains lex_reads_skip and lex_reads_load: the subset of the
-- lex_pages_skip / lex_pages_load VISITS that MISSED shared_buffers.  doc/GAPS.md G48.
--
-- WHY.  0.21.0 added lex_pages_skip / lex_pages_load and they answered "what fraction of
-- the lexical channel's page VISITS are spent proving blocks irrelevant" -- 19-50 %.  But a
-- visit to a resident page is a pin and a lock, not an I/O, and the number that decides
-- whether an out-of-chain skip structure (BlockMax-WAND) is worth a format change is the
-- fraction of DISK READS that are skip-only, not of visits.  These two counters are that:
-- taken as the delta of pgBufferUsage.shared_blks_read around each ReadBuffer at the skip
-- and load sites, they are the same quantity EXPLAIN (ANALYZE, BUFFERS) reports, attributed
-- to a site EXPLAIN cannot split -- a single index-scan node's reads divided into
-- skip-traffic and decode-traffic.
--
-- READ THIS BEFORE QUOTING THEM: they are STATE-DEPENDENT.  At a corpus that fits in
-- shared_buffers they are ~0 however much skipping happens, because nothing misses; the
-- lever is a large-scale/memory-pressure phenomenon and the number is only meaningful in
-- that regime.  A zero here at resident scale is a fact about the cache, not the lever.
--
-- DROP + CREATE, for the fifth time and for the same reason the 0.10->0.11, 0.12->0.13,
-- 0.20->0.21 and 0.21->0.22 scripts give: a function whose whole result is OUT parameters
-- has those parameters AS ITS RETURN TYPE, and PostgreSQL will not change an existing
-- function's return type.  weave_work_stats() takes no argument, so the signature to drop
-- is the empty one.
DROP FUNCTION weave_work_stats();

CREATE FUNCTION weave_work_stats(
        OUT lex_contribs bigint,
        OUT vec_lanes bigint,
        OUT vec_blocks bigint,
        OUT vec_blocks_bound_skipped bigint,
        OUT vec_shuttles bigint,
        OUT lex_pages_skip bigint,
        OUT lex_pages_load bigint,
        OUT lex_reads_skip bigint,
        OUT lex_reads_load bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_work_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_work_stats() IS
    'per-backend channel work counters: lexical contributions, vector lanes/blocks scored and bound-skipped, vector shuttles, lexical page visits (skip vs decode) and the subset of those visits that missed shared_buffers (skip vs decode reads)';
