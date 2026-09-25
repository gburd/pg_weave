/* pg_weave 0.20.0 -> 0.21.0 */

-- weave_work_stats() gains lex_pages_skip and lex_pages_load: the lexical channel's
-- page traffic, split by what the page was for.  doc/GAPS.md G48.
--
-- WHY THE LEXICAL CHANNEL NEEDED THIS AND THE VECTOR ONE DID NOT.  vec_blocks and
-- vec_blocks_bound_skipped have existed since 0.19.0, so the vector channel's pruning
-- is a measured ratio.  The lexical channel had nothing equivalent, so the figure the
-- gate sweep quotes for it -- "390 of 813 buffers at the tight gate"
-- (bench/RESULTS_GATE_SWEEP.md) -- is an inference from an EXPLAIN total rather than a
-- channel measurement, and the lever it points at could not be sized.
--
-- WHAT THE SPLIT IS FOR.  wand_skip_blocks() advances past whole 128-blocks reading only
-- block HEADERS, no FOR decode, which is what makes a seek over a high-df term cheap in
-- CPU.  It nonetheless visits every page it passes over, because the per-block
-- first_docid and block_max it consults live ON those pages.  So
-- lex_pages_skip / (lex_pages_skip + lex_pages_load) is the fraction of the channel's
-- page traffic spent proving blocks irrelevant -- the ceiling on what an out-of-chain
-- skip structure could remove.  Recorded as a ceiling and not as a plan: two earlier
-- levers asserted from a plausible mechanism measured at 6.7 % and within noise.
--
-- A PAGE VISIT IS NOT AN I/O.  A ReadBuffer() that hits shared_buffers costs a pin, a
-- share lock and a spinlock, not a read, and posting lists share pages so the same page
-- can be visited once per term.  These counters bound what could be removed from the
-- BUFFER ACCESS path; EXPLAIN (ANALYZE, BUFFERS) is what says how much of it was disk.
-- include/weave/weave.h states this next to the counters, because a number that gets
-- quoted as I/O when it is pins is a number that overstates its own lever.
--
-- DROP + CREATE rather than CREATE OR REPLACE, for the third time and for the reason the
-- 0.10->0.11 and 0.12->0.13 scripts both give: a function whose whole result is OUT
-- parameters has those parameters AS ITS RETURN TYPE, and PostgreSQL refuses to change an
-- existing function's return type, so a column cannot be added in place.  Nothing in the
-- extension depends on it (no view, no default, no other function), so the drop is safe;
-- a user who has built a view over it gets a dependency error here rather than a silently
-- changed result shape, which is the right failure.
DROP FUNCTION weave_work_stats();

CREATE FUNCTION weave_work_stats(
        OUT lex_contribs bigint,
        OUT vec_lanes bigint,
        OUT vec_blocks bigint,
        OUT vec_blocks_bound_skipped bigint,
        OUT vec_shuttles bigint,
        OUT lex_pages_skip bigint,
        OUT lex_pages_load bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_work_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_work_stats() IS
    'what the channels did, on whatever path asked them: the arm-comparable work counts';
