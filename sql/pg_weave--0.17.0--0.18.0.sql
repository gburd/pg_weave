/* pg_weave 0.17.0 -> 0.18.0 */

-- The instrument FUSED_TOPK.md sect. 8's gate row needs, and the reason it did not
-- exist until now.
--
-- sect. 8 makes the ratio of channel score() calls against RRF's the row that
-- decides whether the whole fused design means anything: "if the score() call ratio
-- is not dramatically lower, stop and fix the bounds before optimizing anything
-- else -- a loose bound makes the entire design pointless and no amount of SIMD
-- recovers it."
--
-- The core has been counting since F1.  WeaveFuseChan carries nseek and nscore,
-- WeaveFuseState carries npivot, nblkskip, nrqskip, nlivedrop, nveto and nabandon,
-- and src/am/fuse.c increments all of them -- but the channels and the state are
-- per-bolt scratch that dies with the scan's memory context, and nothing carried a
-- single number out.  The property test in test/hegel/ could read them because it
-- owns the structs; no query could, and a benchmark is a query.  So the gate was
-- not "blocked on a benchmark harness", it was blocked on a counter nobody could
-- read -- the same shape as the allocator counters in 0.8.0, which were added for
-- the same reason and settled three wrong mechanisms in one afternoon.
--
-- ONE COLUMN IS NEW RATHER THAN CARRIED OUT: `bounds`, the block_max() call count
-- (WeaveFuseChan.nbmax).  Without it, a change that halves score() calls by asking
-- block_max() twice as often reads as a win, and on the vector channel block_max()
-- reads the block's stored bound rather than computing a constant.  Hard rule 8.
--
-- WHY A SEPARATE FUNCTION and not more columns on weave_channel_stats():  that one
-- answers "which mechanism served this leaf" and counts once per leaf or segment;
-- these count the scorer's inner loop and run to millions.  One record holding both
-- invites exactly one mistake, dividing one by the other.
--
-- READ include/weave/weave.h BEFORE QUOTING `scores`.  The widening ladder re-runs
-- the whole fused pass per rung and the merge-race retry re-runs it too, so `scores`
-- is the total over every pass, not the cost of the answer.  `passes` and `runs` are
-- the denominators that make it mean something, which is why they are columns and
-- not something a reader is left to reconstruct.

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
        OUT abandon bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_fuse_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_fuse_stats() IS
    'how much work the fused scorer did in this backend, and how much the bounds removed';

CREATE FUNCTION weave_fuse_stats_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'weave_fuse_stats_reset'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_fuse_stats_reset() IS
    'zero this backend''s fused-scorer counters so a measurement can bracket one query';
