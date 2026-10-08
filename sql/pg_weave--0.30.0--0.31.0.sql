/* pg_weave 0.30.0 -> 0.31.0 */

-- weave_current_fused_distance() (doc/GAPS.md G86, the fused route): score reuse
-- for `ORDER BY fuse(...)`.  The planner keeps the fuse(...) expression in the
-- fused index scan's target list as a hidden sort key, so the executor
-- re-evaluated it per returned row: one heap detoast and one N = 1 BM25 per
-- lexical channel, for a value the scan had already computed.  The planner hook
-- in src/am/customscan.c replaces that hidden copy (never a selected column) with
--
--     COALESCE(weave_current_fused_distance(index, ctid, q1, ..., qn, weights),
--              fuse(...))
--
-- q1..qn and weights being the right operands of the scan's ORDER BY keys.  It
-- returns the value the live fused scan on `index` with exactly those keys
-- ordered heap tuple `ctid` by, or NULL when there is none, which includes every
-- call made outside such a scan.  Not meant to be called directly.

CREATE FUNCTION weave_current_fused_distance(index regclass, row_ctid tid,
											 VARIADIC keys "any")
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_current_fused_distance'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION weave_current_fused_distance(regclass, tid, "any") IS
'Internal (G86): the fused value the live weave fused scan on the index stored for this '
'row and these ORDER BY key operands, or NULL. The planner substitutes it for the hidden '
'sort-key copy of ORDER BY fuse(...); not for direct use.';
