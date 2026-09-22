/* pg_weave 0.18.0 -> 0.19.0 */

-- The CONTROL arm's denominator, and the split that makes the two arms comparable.
--
-- 0.18.0 made the fused scorer's work readable (`doc/specs/FUSED_TOPK.md` sect. 8a).
-- Writing the sect. 8 harness against it exposed two ways the ratio it is supposed to
-- produce would have been wrong, and neither is fixable in the harness:
--
-- 1. THE CONTROL ARM WAS NOT COUNTED AT ALL.  sect. 8's gate is a RATIO against
--    RRF-with-over-fetch, and the two single-channel scans that make up that control
--    counted nothing.  The lexical WAND path had no score counter; the vector path
--    had per-scan counters that the ORDER BY path pfree'd unread.  A ratio with one
--    measured side is not a measurement.
--
-- 2. A FUSED HYBRID QUERY'S `scores` IS A SUM OF THREE DIFFERENT THINGS -- BM25
--    contributions, vector lane-asks, and one probe per required gate per pivot --
--    so comparing it against either arm of the control compares three quantities
--    with one.  `vec_scores` and `gate_scores` make the lexical part derivable
--    exactly, as `scores - vec_scores - gate_scores`.
--
-- AND THE VECTOR CHANNEL'S UNIT IS LANES, NOT score() CALLS.  The code-scan kernel
-- scores a 32-lane BLOCK at a time, so a fused scan asking for one lane and a top-k
-- scan asking for a whole block can register identical score() counts having done
-- 32x different work.  `weave_work_stats().vec_lanes` is what the kernel touched, and
-- it is harvested at `vec_shuttle_end()` -- the single site every vector shuttle
-- passes through on every path, so both arms are counted by one piece of code.
--
-- `lex_contribs` DELIBERATELY EXCLUDES THE FUSED PATH.  The fused lexical channel
-- scores through src/query/lexshuttle.c, which calls the same wand_contrib_cur() the
-- WAND loops do; an increment inside that function would count the fused arm twice --
-- once here and once in weave_fuse_stats().scores -- and a quantity double-counted in
-- one arm of a ratio is a made-up ratio. The increments are at the three WAND call
-- sites instead. include/weave/weave.h states all of this next to the counters.

DROP FUNCTION weave_fuse_stats();

CREATE FUNCTION weave_fuse_stats(
        OUT passes bigint,
        OUT runs bigint,
        OUT chans bigint,
        OUT seeks bigint,
        OUT scores bigint,
        OUT bounds bigint,
        OUT pivots bigint,
        OUT blkskip bigint,
        OUT rqskip bigint,
        OUT livedrop bigint,
        OUT veto bigint,
        OUT abandon bigint,
        OUT vec_scores bigint,
        OUT gate_scores bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_fuse_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_fuse_stats() IS
    'how much work the fused scorer did in this backend, and how much the bounds removed';

CREATE FUNCTION weave_work_stats(
        OUT lex_contribs bigint,
        OUT vec_lanes bigint,
        OUT vec_blocks bigint,
        OUT vec_blocks_bound_skipped bigint,
        OUT vec_shuttles bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_work_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_work_stats() IS
    'what the channels did, on whatever path asked them: the arm-comparable work counts';

CREATE FUNCTION weave_work_stats_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'weave_work_stats_reset'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_work_stats_reset() IS
    'zero this backend''s channel work counters so a measurement can bracket one query';
