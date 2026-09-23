/* pg_weave 0.19.0 -> 0.20.0 */

-- ONE STATISTIC, SO THAT THE CORRECTNESS GATE CAN DESCRIBE THE OBJECTIVE THE SCAN
-- ACTUALLY COMPUTES.  doc/GAPS.md G46.
--
-- Since 0.19.0 the fused scorer divides each `fuse()` KEY by that key's pre-scan
-- ceiling before summing (`doc/specs/FUSED_TOPK.md` sect. 8d), which is what put the
-- fused ranking above an RRF control on three BEIR corpora.  For a lexical key the
-- ceiling is the sum over the key's terms of the BM25 term bound at max tf, taken over
-- every segment.
--
-- `bench/fuse.sh`'s gate -- the source of sect. 8's recall row, which that table calls
-- its most valuable -- compares the pushdown against an EXHAUSTIVE oracle assembled in
-- SQL from `weave_search()` and `weave_vec_scan()`.  The vector key's normalizer was
-- already reachable (the max over segments of `weave_vec_scan_stats().maxscore`, which
-- is the same (B2) fold the scan performs), but the lexical key's was not: nothing in
-- the SQL surface exposed max tf.  So the gate had to run with
-- `pg_weave.fuse_normalize = off`, and a gate that tests a non-default configuration is
-- one configuration change away from testing nothing.
--
-- WHY THE RAW STATISTIC AND NOT A `weave_fuse_key_norm()` THAT RETURNS THE CEILING.
-- The accessor would be shorter to call and would agree with the scan BY CONSTRUCTION,
-- because it would be the same C code -- and an oracle that shares code with the thing
-- it checks has stopped being an oracle.  Exposing max tf makes the oracle recompute
-- `idf * mtf * (k1 + 1) / (mtf + k1 * (1 - b))` independently, which is the one part of
-- the normalizer no fixed-output test can reach.
--
-- The result has one element per distinct query term, in query order -- the same shape
-- and order `weave_index_df()` returns -- so the two zip positionally.  A term absent
-- from every segment yields 0, whose term bound is 0, which is the contribution the
-- scan gives it as well.

CREATE FUNCTION weave_index_max_tf(regclass, wquery)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'weave_index_max_tf'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_index_max_tf(regclass, wquery) IS
    'maximum term frequency per query term, over every segment: the lexical half of a fuse() key''s pre-scan ceiling';
