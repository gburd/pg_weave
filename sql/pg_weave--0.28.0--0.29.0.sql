/* pg_weave 0.28.0 -> 0.29.0 */

-- weave_current_distance() (doc/GAPS.md G86): score reuse.  An ordering scan
-- `ORDER BY d <=> q` has already computed each returned row's distance, but the
-- planner keeps `d <=> q` in the scan's target list as a hidden sort key, so the
-- executor re-read and re-scored every row.  The planner hook in
-- src/am/customscan.c replaces that hidden copy (never a selected column) with
--
--     COALESCE(weave_current_distance(index, ctid, q), d <=> q)
--
-- This returns the distance the live weave ordering scan on `index` stored for
-- heap tuple `ctid` under query `q`, or NULL when there is none, which includes
-- every call made outside such a scan.  Not meant to be called directly.

CREATE FUNCTION weave_current_distance(index regclass, row_ctid tid, query wquery)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_current_distance'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_current_distance(regclass, tid, wquery) IS
'Internal (G86): the <=> distance the live weave ordering scan on the index stored for this '
'row and query, or NULL. The planner substitutes it for the hidden sort-key copy of '
'ORDER BY d <=> q; not for direct use.';
