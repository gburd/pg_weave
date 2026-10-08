-- doc/GAPS.md G86: score reuse.  The planner keeps an ordering scan's own
-- `d <=> q` in the scan's target list as a hidden (RESJUNK) sort key, so the
-- executor re-read and re-scored every returned row.  A planner_hook
-- (src/am/customscan.c weave_reuse_distance) replaces that hidden entry with
--     COALESCE(weave_current_distance(index, ctid, q), d <=> q)
-- and the scan publishes each returned row's value for it (src/am/amscan.c).
--
-- The scan's value is the CORPUS BM25 distance; the operator's is weave_distance()'s
-- N = 1 one.  They differ, so a selected `d <=> q` must keep the operator's value.
-- The corpus is built so they visibly differ: every 'alpha' document has tf = 1, so
-- the operator gives all of them ONE distance, while the index's length
-- normalization gives ten (five lengths, with or without 'gamma').  A reader of the
-- sort key (WITH TIES, an Incremental Sort) therefore sees which value it got.
--
-- What each block guards, and the mutant that proves it can fail:
--   1. the GUC exists (an absent GUC reads like an OFF one: AGENTS.md, 12th member)
--   2. EXPLAIN: substituted on the resjunk entry only, and only on a weave scan
--                       (mutants: substitute a visible entry; substitute any scan)
--   3. a selected d <=> q is the operator's value     (mutant: visible entry too)
--   4. the published value is the CURRENT row's      (mutant: previous row's value)
--   5. WITH TIES, a nested-loop rescan, a cursor: the key is the stream's value
--   6. the operator is no longer called per row, with a control that counts
--   7. a nested ordering scan between fetch and projection does not clobber it
--                                  (mutant: an unkeyed global, pg_fts's design)
--   8. no substitution when the function is not the library's C function
--   9. the FUSED route, `ORDER BY fuse(...)` (weave_current_fused_distance, 0.31.0):
--      9a plans (hidden key only; the visible fuse() column and fuse() over another
--         query untouched), 9b visible values unchanged, 9c the published value is
--         the current row's against weave_fuse_search(), 9d WITH TIES / cursor /
--         nested-loop rescan in both fuse_normalize modes, 9e padded rows, 9f the
--         per-row calls, 9g the clobber case, 9h only the library's C function
--                  (mutants: visible entry; previous row; wrong pad value; unkeyed)
CREATE EXTENSION IF NOT EXISTS pg_weave;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE sr (id int, d wdoc) WITH (autovacuum_enabled = off);
INSERT INTO sr SELECT g, to_wdoc('simple',
   CASE WHEN g % 3 = 0 THEN 'alpha ' ELSE 'beta ' END ||
   CASE WHEN g % 7 = 0 THEN 'gamma ' ELSE '' END ||
   repeat('x' || (g % 13) || ' ', 1 + 3 * (g % 5)))
  FROM generate_series(1, 3000) g;
CREATE INDEX sr_w ON sr USING weave (d);
VACUUM ANALYZE sr;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;

-- 1.  Touch the index first: pg_settings only lists the GUC once the library is loaded.
SELECT count(*) FROM sr WHERE d @@@ 'zzz';
SELECT name, setting, short_desc IS NOT NULL AS registered
  FROM pg_settings WHERE name = 'pg_weave.reuse_distance';
-- outside a scan the function answers NULL, never a stale value
SELECT weave_current_distance('sr_w', '(0,1)', 'alpha') IS NULL AS null_outside_a_scan;

-- the index's own ranking, from weave_search() (not the ordering scan)
CREATE TEMP TABLE ex AS
  SELECT l.id, 1.0 / (1.0 + s.score) AS dist
  FROM weave_search('sr_w', 'alpha', 3000) s JOIN sr l ON l.ctid = s.ctid;
SELECT count(*) AS matched, count(DISTINCT dist) AS index_distances,
       (SELECT count(DISTINCT d <=> 'alpha') FROM sr WHERE id % 3 = 0) AS operator_distances
  FROM ex;

-- 2.  The plans.
-- the hidden sort key is substituted
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 3;
-- a selected d <=> q is NOT
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id, d <=> 'alpha' AS dist FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 3;
-- an Incremental Sort reads the substituted key
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr ORDER BY d <=> 'alpha', id LIMIT 3;
-- off: the operator, as before
SET pg_weave.reuse_distance = off;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 3;
RESET pg_weave.reuse_distance;
-- not a weave scan: a GiST KNN scan has the same plan shape and is left alone
CREATE TABLE sr_pt (id int, p point);
INSERT INTO sr_pt SELECT g, point(g % 50, g / 50) FROM generate_series(1, 2000) g;
CREATE INDEX sr_pt_gist ON sr_pt USING gist (p);
ANALYZE sr_pt;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr_pt ORDER BY p <-> point(3, 3) LIMIT 3;
-- the other routes are not substituted: a vector scan's value is a quantized
-- score, and an edit-distance or fused sort key is not `d <=> q` (doc/GAPS.md G86)
CREATE TABLE sr_v (id int, d wdoc, v wvec(4));
INSERT INTO sr_v SELECT g, to_wdoc('simple', 'alpha w' || (g % 17)),
       ('[' || (g % 7) || ',' || (g % 5) || ',' || (g % 3) || ',1]')::wvec
  FROM generate_series(1, 500) g;
CREATE INDEX sr_v_w ON sr_v USING weave (d, v);
ANALYZE sr_v;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr_v ORDER BY v <-> '[1,1,1,1]' LIMIT 3;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr_v ORDER BY d <@> 'w7x' LIMIT 3;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr_v ORDER BY fuse(d <=> 'alpha', v <-> '[1,1,1,1]') LIMIT 3;
-- not an ordering scan of this index (a Sort over a seq scan): left alone
RESET enable_seqscan; RESET enable_sort;
SET enable_indexscan = off;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE id < 10 ORDER BY d <=> 'alpha' LIMIT 3;
RESET enable_indexscan;
SET enable_seqscan = off; SET enable_sort = off;

-- 3.  A selected d <=> q is the operator's value, row for row.
SELECT count(*) AS n,
       count(*) FILTER (WHERE dist IS DISTINCT FROM weave_distance(d, 'alpha')) AS visible_value_changed
  FROM (SELECT d, d <=> 'alpha' AS dist FROM sr WHERE d @@@ 'alpha'
        ORDER BY d <=> 'alpha' LIMIT 500) s;

-- 4.  The value published for the CURRENT row is the index's own distance for
-- that row.  Selected directly, so every row reads it; a value from the previous
-- row would be another group's distance at every group boundary.
SELECT count(*) AS n,
       count(*) FILTER (WHERE abs(s.cur - ex.dist) > 1e-9 * ex.dist) AS wrong_row_value,
       count(*) FILTER (WHERE s.cur IS NULL) AS no_value
  FROM (SELECT id, weave_current_distance('sr_w', ctid, 'alpha') AS cur
          FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 1000) s
  JOIN ex USING (id);

-- 5.  The sort key's readers.  FETCH ... WITH TIES keeps every row tied with the
-- last one ON THE KEY.  With the index's distances that is the rows at or before
-- the 5th row's distance; with the operator's single distance (reuse off) it is
-- every 'alpha' row.  The reference is weave_search()'s ranking.
SELECT count(*) AS reference_with_ties FROM ex
 WHERE dist <= (SELECT dist FROM ex ORDER BY dist LIMIT 1 OFFSET 4);
SELECT count(*) AS with_ties_on FROM
  (SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES) s;
SET pg_weave.reuse_distance = off;
SELECT count(*) AS with_ties_off FROM
  (SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES) s;
RESET pg_weave.reuse_distance;
-- and the rows are the right ones: exactly the reference's tie group and better
SELECT count(*) AS with_ties_not_in_reference FROM
  (SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES) s
 WHERE id NOT IN (SELECT id FROM ex WHERE dist <= (SELECT dist FROM ex ORDER BY dist LIMIT 1 OFFSET 4));
-- Incremental Sort: (index distance, id) order, against the reference
SELECT (SELECT array_agg(id) FROM (SELECT id FROM sr WHERE d @@@ 'alpha'
                                   ORDER BY d <=> 'alpha', id LIMIT 40) s)
     = (SELECT array_agg(id ORDER BY dist, id) FROM (SELECT * FROM ex ORDER BY dist, id LIMIT 40) e)
       AS incremental_sort_matches_reference;
-- a nested-loop inner side, rescanned per outer row, with a different query each time
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
CREATE TEMP TABLE exg AS
  SELECT o.t, l.id, 1.0 / (1.0 + s.score) AS dist
  FROM unnest(ARRAY['alpha', 'beta', 'gamma']) o(t)
       CROSS JOIN LATERAL weave_search('sr_w', o.t::wquery, 3000) s
       JOIN sr l ON l.ctid = s.ctid;
SELECT count(*) AS outer_rows,
       count(*) FILTER (WHERE x.n IS DISTINCT FROM y.n) AS rescan_mismatches
  FROM unnest(ARRAY['alpha', 'beta', 'gamma']) o(t),
  LATERAL (SELECT count(*) n FROM (SELECT id FROM sr WHERE d @@@ o.t::wquery
           ORDER BY d <=> o.t::wquery FETCH FIRST 5 ROWS WITH TIES) i) x,
  LATERAL (SELECT count(*) n FROM exg WHERE exg.t = o.t AND exg.dist <=
           (SELECT dist FROM exg e2 WHERE e2.t = o.t ORDER BY dist LIMIT 1 OFFSET 4)) y;
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;
-- a cursor over a substituted WITH TIES plan, fetched one row at a time
BEGIN;
DECLARE sr_c CURSOR FOR SELECT id FROM sr WHERE d @@@ 'alpha'
  ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES;
CREATE TEMP TABLE cur_rows (rn serial, id int);
DO $$ DECLARE v int; c refcursor := 'sr_c'; BEGIN
  LOOP FETCH c INTO v; EXIT WHEN NOT FOUND; INSERT INTO cur_rows (id) VALUES (v); END LOOP; END $$;
COMMIT;
SELECT count(*) = (SELECT count(*) FROM ex WHERE dist <= (SELECT dist FROM ex ORDER BY dist LIMIT 1 OFFSET 4))
       AS cursor_matches_reference
  FROM cur_rows;

-- 6.  weave_distance() is no longer called per returned row.  The control (reuse
-- off) shows the counter counts; the seq-scan arm shows it counts exactly.
SET track_functions = 'all';
SET stats_fetch_consistency = none;
CREATE FUNCTION sr_wd() RETURNS bigint LANGUAGE sql AS
  $$ SELECT coalesce(sum(calls), 0) FROM pg_stat_user_functions WHERE funcname = 'weave_distance' $$;
SELECT pg_stat_force_next_flush();
SELECT sr_wd() AS wd0 \gset
SELECT count(*) FROM (SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 400) s;
SELECT pg_stat_force_next_flush();
SELECT sr_wd() - :wd0 AS reused_calls;
SET pg_weave.reuse_distance = off;
SELECT sr_wd() AS wd1 \gset
SELECT count(*) FROM (SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 400) s;
SELECT pg_stat_force_next_flush();
SELECT sr_wd() - :wd1 >= 400 AS control_calls_per_row;
RESET pg_weave.reuse_distance;
RESET enable_seqscan;
SET enable_indexscan = off;
SELECT sr_wd() AS wd2 \gset
SELECT sum(weave_distance(d, 'alpha')) > 0 AS evaluated FROM sr WHERE id <= 50;
SELECT pg_stat_force_next_flush();
SELECT sr_wd() - :wd2 AS seqscan_control_calls;
RESET enable_indexscan;
SET enable_seqscan = off;
RESET track_functions;

-- 7.  A nested ordering scan between the outer fetch and its projection.  A
-- correlated subquery in the WHERE clause is the scan's Filter, so it runs a second
-- weave ordering scan after every outer fetch and before the hidden sort key is
-- evaluated.  (A volatile or expensive SELECT-list column would not do: the
-- planner postpones those above the Limit.)  The outer WITH TIES must still see
-- the outer scan's values, first with the inner scan on another index, then on the
-- SAME index with the same query, returning a different row.
CREATE TABLE sr2 (id int, d wdoc) WITH (autovacuum_enabled = off);
INSERT INTO sr2 SELECT g, to_wdoc('simple', 'beta ' || repeat('y ', 1 + g % 9)) FROM generate_series(1, 300) g;
CREATE INDEX sr2_w ON sr2 USING weave (d);
VACUUM ANALYZE sr2;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE d @@@ 'alpha'
     AND (SELECT i.id FROM sr2 i ORDER BY i.d <=> 'beta' LIMIT 1 OFFSET sr.id % 3) > 0
   ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES;
SELECT count(*) = (SELECT count(*) FROM ex WHERE dist <= (SELECT dist FROM ex ORDER BY dist LIMIT 1 OFFSET 4))
       AS other_index_clobber_matches_reference
  FROM (SELECT id FROM sr WHERE d @@@ 'alpha'
           AND (SELECT i.id FROM sr2 i ORDER BY i.d <=> 'beta' LIMIT 1 OFFSET sr.id % 3) > 0
         ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES) s;
SELECT count(*) = (SELECT count(*) FROM ex WHERE dist <= (SELECT dist FROM ex ORDER BY dist LIMIT 1 OFFSET 4))
       AS same_index_clobber_matches_reference
  FROM (SELECT id FROM sr WHERE d @@@ 'alpha'
           AND (SELECT i.id FROM sr i WHERE i.d @@@ 'alpha' ORDER BY i.d <=> 'alpha'
                LIMIT 1 OFFSET 900 + sr.id % 7) > 0
         ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES) s;

-- 8.  Only the library's own C function is planted.  A same-named SQL function
-- (a decoy, or a catalog that predates 0.29.0 with something else in its place)
-- is not, and the plan keeps the operator.  The extension's function is restored
-- by recreating it exactly as the upgrade script does.
ALTER EXTENSION pg_weave DROP FUNCTION weave_current_distance(regclass, tid, wquery);
DROP FUNCTION weave_current_distance(regclass, tid, wquery);
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 3;
CREATE FUNCTION weave_current_distance(regclass, tid, wquery) RETURNS float8
  LANGUAGE sql AS $$ SELECT 0.5::float8 $$;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 3;
DROP FUNCTION weave_current_distance(regclass, tid, wquery);
CREATE FUNCTION weave_current_distance(index regclass, row_ctid tid, query wquery)
  RETURNS float8 AS '$libdir/pg_weave', 'weave_current_distance'
  LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
ALTER EXTENSION pg_weave ADD FUNCTION weave_current_distance(regclass, tid, wquery);
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sr WHERE d @@@ 'alpha' ORDER BY d <=> 'alpha' LIMIT 3;

-- 9.  The FUSED route.  A fused scan orders by -S, the weighted sum of CORPUS
-- channel scores (normalized by each key's ceiling when pg_weave.fuse_normalize
-- is on), and its hidden `fuse(...)` sort key used to be re-evaluated per row with
-- N = 1 BM25s.  sf: tf of 'alpha' and 'beta' and the vector all vary with the
-- row, so the fused values are many and the operator's differ from them; every
-- 50th vector is NULL, so those rows are padded (fuse() is NULL for them); 1001..
-- 1100 match neither query, so with two lexical channels they pad at 0.
CREATE TABLE sf (id int, d wdoc, v wvec(4)) WITH (autovacuum_enabled = off);
INSERT INTO sf SELECT g, to_wdoc('simple',
   repeat('alpha ', 1 + g % 4) || repeat('beta ', 1 + g % 3) || repeat('x ', g % 5) || 'w' || (g % 11)),
   CASE WHEN g % 50 = 0 THEN NULL
        ELSE ('[' || (g % 7) * 0.1 || ',' || (g % 5) * 0.1 || ',' || (g % 3) * 0.1 || ',0.5]')::wvec END
  FROM generate_series(1, 1000) g;
INSERT INTO sf SELECT g, to_wdoc('simple', 'gamma w' || (g % 11)), '[0.1,0.2,0.3,0.4]'
  FROM generate_series(1001, 1100) g;
CREATE INDEX sf_w ON sf USING weave (d, v);
VACUUM ANALYZE sf;
-- Reference: weave_fuse_search() drives the same fused scan and returns -value as
-- its score, for the ranked rows.  dist = -score, the value the stream orders by.
CREATE FUNCTION sf_ref(norm bool) RETURNS TABLE (shape text, id int, dist float8)
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('pg_weave.fuse_normalize', norm::text, true);
  RETURN QUERY SELECT 'll'::text, l.id, -s.score
    FROM weave_fuse_search('sf_w', ARRAY['alpha'::wquery, 'beta'], NULL, '{1,2}', 5000) s
    JOIN sf l ON l.ctid = s.ctid;
  RETURN QUERY SELECT 'lv'::text, l.id, -s.score
    FROM weave_fuse_search('sf_w', ARRAY['alpha'::wquery], ARRAY['[0.3,0.2,0.1,0.5]'::wvec],
                           NULL, 5000) s
    JOIN sf l ON l.ctid = s.ctid;
END $$;
CREATE TEMP TABLE fex AS
  SELECT true AS norm, * FROM sf_ref(true) UNION ALL SELECT false, * FROM sf_ref(false);
SELECT norm, shape, count(*) AS rows, count(DISTINCT dist) AS index_values,
       count(*) FILTER (WHERE dist = 0) AS at_zero
  FROM fex GROUP BY 1, 2 ORDER BY 1, 2;

-- 9a. Plans.  The hidden key is substituted, with the scan's key operands.
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') LIMIT 3;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', v <-> '[0.3,0.2,0.1,0.5]') LIMIT 3;
-- the commutator spelling and a WHERE gate: still the scan's own key
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf WHERE d @@@ 'alpha'
   ORDER BY fuse('alpha' <=> d, v <-> '[0.3,0.2,0.1,0.5]') LIMIT 3;
-- a selected fuse(...) is NOT substituted, and neither is a selected fuse() over
-- other queries, though it sits in the same target list
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id, fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') AS f,
         fuse(d <=> 'beta', d <=> 'alpha') AS other
    FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') LIMIT 3;
-- off: the operator, as before
SET pg_weave.reuse_distance = off;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') LIMIT 3;
RESET pg_weave.reuse_distance;
-- outside a scan the function answers NULL
SELECT weave_current_fused_distance('sf_w', '(0,1)', 'alpha'::wquery, 'beta'::wquery,
                                    '{1,2}'::real[]) IS NULL AS fused_null_outside_a_scan;

-- 9b. A selected fuse(...) keeps the operator's value, row for row.
SELECT count(*) AS n,
       count(*) FILTER (WHERE f IS DISTINCT FROM
                        fuse(weave_lexscore(weave_distance(d, 'alpha')),
                             weave_lexscore(weave_distance(d, 'beta')), weights => '{1,2}'))
         AS visible_value_changed
  FROM (SELECT d, fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') AS f
          FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') LIMIT 500) s;

-- 9c. The value published for the CURRENT row is the scan's own -S for it, in
-- both normalizer modes and both shapes.  Selected directly so every row reads it.
CREATE FUNCTION sf_cur(norm bool) RETURNS TABLE (shape text, n bigint, wrong bigint, nulls bigint)
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('pg_weave.fuse_normalize', norm::text, true);
  RETURN QUERY SELECT 'll', count(*),
         count(*) FILTER (WHERE abs(s.cur - e.dist) > 1e-9 * abs(e.dist)),
         count(*) FILTER (WHERE s.cur IS NULL)
    FROM (SELECT id, weave_current_fused_distance('sf_w', ctid, 'alpha'::wquery, 'beta'::wquery,
                                                  '{1,2}'::real[]) AS cur
            FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') LIMIT 1000) s
    JOIN fex e ON e.id = s.id AND e.norm = sf_cur.norm AND e.shape = 'll';
  RETURN QUERY SELECT 'lv', count(*),
         count(*) FILTER (WHERE abs(s.cur - e.dist) > 1e-9 * abs(e.dist)),
         count(*) FILTER (WHERE s.cur IS NULL)
    FROM (SELECT id, weave_current_fused_distance('sf_w', ctid, 'alpha'::wquery,
                       '[0.3,0.2,0.1,0.5]'::wvec, '{1,1}'::real[]) AS cur
            FROM sf ORDER BY fuse(d <=> 'alpha', v <-> '[0.3,0.2,0.1,0.5]') LIMIT 900) s
    JOIN fex e ON e.id = s.id AND e.norm = sf_cur.norm AND e.shape = 'lv';
END $$;
SELECT * FROM sf_cur(true) UNION ALL SELECT * FROM sf_cur(false);

-- 9d. The sort key's readers, against the reference, in both modes.  WITH TIES
-- keeps every row tied with the k-th ON THE KEY: with the scan's values that is
-- the reference's tie group; with the operator's (reuse off) it is a different
-- set, because the N = 1 values tie differently.  A cursor fetched row by row and
-- a nested-loop rescan (a different query vector per outer row) read it too.
CREATE FUNCTION sf_ties(norm bool, k int) RETURNS TABLE (shape text, got bigint, ref bigint, outside bigint)
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('pg_weave.fuse_normalize', norm::text, true);
  RETURN QUERY
  WITH g AS (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}')
             FETCH FIRST k ROWS WITH TIES),
       r AS (SELECT e.id FROM fex e WHERE e.norm = sf_ties.norm AND e.shape = 'll'
               AND e.dist <= (SELECT x.dist FROM fex x WHERE x.norm = sf_ties.norm AND x.shape = 'll'
                              ORDER BY x.dist LIMIT 1 OFFSET k - 1))
  SELECT 'll', (SELECT count(*) FROM g), (SELECT count(*) FROM r),
         (SELECT count(*) FROM g WHERE g.id NOT IN (SELECT r.id FROM r));
  RETURN QUERY
  WITH g AS (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', v <-> '[0.3,0.2,0.1,0.5]')
             FETCH FIRST k ROWS WITH TIES),
       r AS (SELECT e.id FROM fex e WHERE e.norm = sf_ties.norm AND e.shape = 'lv'
               AND e.dist <= (SELECT x.dist FROM fex x WHERE x.norm = sf_ties.norm AND x.shape = 'lv'
                              ORDER BY x.dist LIMIT 1 OFFSET k - 1))
  SELECT 'lv', (SELECT count(*) FROM g), (SELECT count(*) FROM r),
         (SELECT count(*) FROM g WHERE g.id NOT IN (SELECT r.id FROM r));
END $$;
SELECT 'norm' AS mode, * FROM sf_ties(true, 5)
UNION ALL SELECT 'raw', * FROM sf_ties(false, 5)
UNION ALL SELECT 'raw k=40', * FROM sf_ties(false, 40);
-- the control, at top level (a plpgsql function would keep its cached plan
-- across a GUC change): with reuse off the key is the operator's N = 1 value,
-- which depends on tf alone, so it ties far more rows than the reference's 'll'
SET pg_weave.reuse_distance = off; SET pg_weave.fuse_normalize = off;
SELECT count(*) AS fused_with_ties_reuse_off
  FROM (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}')
        FETCH FIRST 5 ROWS WITH TIES) s;
RESET pg_weave.reuse_distance; RESET pg_weave.fuse_normalize;
-- a cursor over a substituted WITH TIES plan, one row at a time
BEGIN;
SET LOCAL pg_weave.fuse_normalize = off;
DECLARE sf_c CURSOR FOR SELECT id FROM sf
  ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') FETCH FIRST 5 ROWS WITH TIES;
CREATE TEMP TABLE fcur_rows (rn serial, id int);
DO $$ DECLARE v int; c refcursor := 'sf_c'; BEGIN
  LOOP FETCH c INTO v; EXIT WHEN NOT FOUND; INSERT INTO fcur_rows (id) VALUES (v); END LOOP; END $$;
COMMIT;
SELECT (SELECT count(*) FROM fcur_rows) = (SELECT ref FROM sf_ties(false, 5) WHERE shape = 'll')
       AS fused_cursor_matches_reference;
-- a nested-loop inner side, rescanned per outer row with a different query vector
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_material = off;
SET pg_weave.fuse_normalize = off;
CREATE TEMP TABLE fexv AS
  SELECT o.qv, l.id, -s.score AS dist
  FROM unnest(ARRAY['[0.3,0.2,0.1,0.5]', '[0.6,0.4,0.2,0.5]', '[0,0,0,0.5]']::wvec[]) o(qv)
       CROSS JOIN LATERAL weave_fuse_search('sf_w', ARRAY['alpha'::wquery], ARRAY[o.qv], NULL, 5000) s
       JOIN sf l ON l.ctid = s.ctid;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT o.qv, i.id
    FROM unnest(ARRAY['[0.3,0.2,0.1,0.5]', '[0.6,0.4,0.2,0.5]', '[0,0,0,0.5]']::wvec[]) o(qv),
    LATERAL (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', v <-> o.qv)
             FETCH FIRST 5 ROWS WITH TIES) i;
SELECT count(*) AS outer_rows,
       count(*) FILTER (WHERE x.n IS DISTINCT FROM y.n) AS fused_rescan_mismatches
  FROM unnest(ARRAY['[0.3,0.2,0.1,0.5]', '[0.6,0.4,0.2,0.5]', '[0,0,0,0.5]']::wvec[]) o(qv),
  LATERAL (SELECT count(*) n FROM (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', v <-> o.qv)
           FETCH FIRST 5 ROWS WITH TIES) i) x,
  LATERAL (SELECT count(*) n FROM fexv WHERE fexv.qv::text = o.qv::text AND fexv.dist <=
           (SELECT dist FROM fexv e2 WHERE e2.qv::text = o.qv::text ORDER BY dist LIMIT 1 OFFSET 4)) y;
RESET pg_weave.fuse_normalize;
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;

-- 9e. Padded rows.  Past the ranked rows the scan pads: with a vector key a NULL-
-- vector row's fuse() is NULL, so it pads at NULL and the function answers NULL
-- (the operator's NULL is then the key too); with two lexical channels a row that
-- matches neither pads at 0, which is what the function must answer, and what
-- fuse() gives it as well (-(0 + 0)).  Every row of the table comes out once.
CREATE TEMP TABLE fpad AS
  SELECT row_number() OVER () AS rn, id, cur
    FROM (SELECT id, weave_current_fused_distance('sf_w', ctid, 'alpha'::wquery, 'beta'::wquery,
                                                  '{1,1}'::real[]) AS cur
            FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta') LIMIT 5000) s;
SELECT count(*) AS rows_out, count(DISTINCT id) AS distinct_rows,
       count(*) FILTER (WHERE id > 1000) AS padded,
       count(*) FILTER (WHERE id > 1000 AND cur = 0) AS padded_at_zero,
       count(*) FILTER (WHERE id > 1000 AND cur IS NULL) AS padded_no_value,
       count(*) FILTER (WHERE id > 1000 AND rn <= 1000) AS padded_before_a_ranked_row
  FROM fpad;
CREATE TEMP TABLE fpadv AS
  SELECT row_number() OVER () AS rn, id, nullvec, cur
    FROM (SELECT id, v IS NULL AS nullvec,
                 weave_current_fused_distance('sf_w', ctid, 'alpha'::wquery,
                                              '[0.3,0.2,0.1,0.5]'::wvec, '{1,1}'::real[]) AS cur
            FROM sf ORDER BY fuse(d <=> 'alpha', v <-> '[0.3,0.2,0.1,0.5]') LIMIT 5000) s;
SELECT count(*) AS rows_out, count(*) FILTER (WHERE nullvec) AS null_vector_rows,
       count(*) FILTER (WHERE nullvec AND cur IS NOT NULL) AS null_vector_with_value,
       count(*) FILTER (WHERE nullvec AND rn <= (SELECT count(*) FROM fpadv WHERE NOT nullvec))
         AS null_vector_before_a_ranked_row
  FROM fpadv;
-- The vector route's +Infinity: a row the fused pass cannot rank although its
-- vector is present -- here a PENDING vector of the wrong dimension (an untyped
-- wvec column), which the flush would drop (G71).  It pads at +Infinity, last,
-- and that is the published value.  Before 0.31.0 the hidden fuse() re-evaluated
-- `v <-> q` for it and raised "different wvec dimensions": that is the control.
CREATE TABLE sfd (id int, d wdoc, v wvec) WITH (autovacuum_enabled = off);
INSERT INTO sfd SELECT g, to_wdoc('simple', 'alpha w' || g), ('[' || g * 0.1 || ',0,0,1]')::wvec
  FROM generate_series(1, 20) g;
CREATE INDEX sfd_w ON sfd USING weave (d, v);
INSERT INTO sfd VALUES (21, to_wdoc('simple', 'alpha w21'), '[1,2,3]');
SELECT array_agg(id ORDER BY rn) FILTER (WHERE rn > 18) AS last_rows,
       (array_agg(cur ORDER BY rn DESC))[1] AS last_value
  FROM (SELECT row_number() OVER () AS rn, id, cur
          FROM (SELECT id, weave_current_fused_distance('sfd_w', ctid, 'alpha'::wquery,
                                                        '[0,0,0,1]'::wvec, '{1,1}'::real[]) AS cur
                  FROM sfd ORDER BY fuse(d <=> 'alpha', v <-> '[0,0,0,1]') LIMIT 100) s) t;
SET pg_weave.reuse_distance = off;
SELECT count(*) FROM (SELECT id FROM sfd ORDER BY fuse(d <=> 'alpha', v <-> '[0,0,0,1]') LIMIT 100) s;
RESET pg_weave.reuse_distance;
-- WITH TIES reaching into the padding: k = every ranked row + 1, so the k-th row
-- is a padded one and the tie group is the whole padding at that value.  Through
-- the substituted key it is exactly every row (ranked ones and the 100 at 0).
SELECT count(*) AS with_ties_into_padding
  FROM (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta')
        FETCH FIRST 1001 ROWS WITH TIES) s;

-- 9f. fuse()'s channels are no longer re-evaluated per row, with the control.
SET track_functions = 'all';
CREATE FUNCTION sf_calls() RETURNS bigint LANGUAGE sql AS
  $$ SELECT coalesce(sum(calls), 0) FROM pg_stat_user_functions
      WHERE funcname IN ('weave_distance', 'weave_lexscore', 'fuse', 'wvec_l2_distance') $$;
SELECT pg_stat_force_next_flush();
SELECT sf_calls() AS fc0 \gset
SELECT count(*) FROM (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta') LIMIT 400) s;
SELECT count(*) FROM (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', v <-> '[0.3,0.2,0.1,0.5]') LIMIT 400) s;
SELECT pg_stat_force_next_flush();
SELECT sf_calls() - :fc0 AS fused_reused_calls;
SET pg_weave.reuse_distance = off;
SELECT sf_calls() AS fc1 \gset
SELECT count(*) FROM (SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta') LIMIT 400) s;
SELECT pg_stat_force_next_flush();
SELECT sf_calls() - :fc1 >= 400 * 5 AS fused_control_calls_per_row;
RESET pg_weave.reuse_distance;
RESET track_functions;

-- 9g. Clobbering.  A correlated subquery in the scan's Filter runs a second
-- ordering scan after every outer fetch and before the hidden key is evaluated.
-- `i.id = sf.id` makes the inner scan stop ON THE OUTER'S ROW, so index and TID
-- match and only the keys (or the route) tell the two publications apart:
-- (1) a fused scan on the same index with OTHER weights, on the same row;
-- (2) the same fused scan, same keys, on another row (only the TID differs);
-- (3) a lexical scan on the same index and row whose query is the outer's first
--     channel's; and (4) the reverse, a lexical outer with a fused inner on its
--     row, whose WHERE `@@@` query IS the outer's query -- a fused scan carries
--     that in so->query, so only the route check keeps the lexical lookup off it.
-- The outer WITH TIES set must stay the reference's in every case.
CREATE TEMP TABLE sfx AS
  SELECT l.id, 1.0 / (1.0 + s.score) AS dist
  FROM weave_search('sf_w', 'alpha', 5000) s JOIN sf l ON l.ctid = s.ctid;
SET pg_weave.fuse_normalize = off;
SELECT (SELECT count(*) FROM (SELECT id FROM sf
          WHERE (SELECT i.id FROM sf i WHERE i.id = sf.id
                 ORDER BY fuse(i.d <=> 'alpha', i.d <=> 'beta', weights => '{2,1}') LIMIT 1) > 0
          ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') FETCH FIRST 5 ROWS WITH TIES) s)
       = (SELECT ref FROM sf_ties(false, 5) WHERE shape = 'll') AS other_weights_clobber_ok,
       (SELECT count(*) FROM (SELECT id FROM sf
          WHERE (SELECT i.id FROM sf i ORDER BY fuse(i.d <=> 'alpha', i.d <=> 'beta', weights => '{1,2}')
                 LIMIT 1 OFFSET 900 + sf.id % 7) > 0
          ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') FETCH FIRST 5 ROWS WITH TIES) s)
       = (SELECT ref FROM sf_ties(false, 5) WHERE shape = 'll') AS same_keys_clobber_ok,
       (SELECT count(*) FROM (SELECT id FROM sf
          WHERE (SELECT i.id FROM sf i WHERE i.d @@@ 'alpha' AND i.id = sf.id
                 ORDER BY i.d <=> 'alpha' LIMIT 1) > 0
          ORDER BY fuse(d <=> 'alpha', d <=> 'beta', weights => '{1,2}') FETCH FIRST 5 ROWS WITH TIES) s)
       = (SELECT ref FROM sf_ties(false, 5) WHERE shape = 'll') AS lexical_inner_clobber_ok,
       (SELECT count(*) FROM (SELECT id FROM sf
          WHERE d @@@ 'alpha'
            AND (SELECT i.id FROM sf i WHERE i.d @@@ 'alpha' AND i.id = sf.id
                 ORDER BY fuse(i.d <=> 'alpha', i.d <=> 'beta') LIMIT 1) > 0
          ORDER BY d <=> 'alpha' FETCH FIRST 5 ROWS WITH TIES) s)
       = (SELECT count(*) FROM sfx WHERE dist <= (SELECT dist FROM sfx ORDER BY dist LIMIT 1 OFFSET 4))
         AS fused_inner_clobber_ok;
RESET pg_weave.fuse_normalize;

-- 9h. Only the library's C function is planted (as in 8).
ALTER EXTENSION pg_weave DROP FUNCTION weave_current_fused_distance(regclass, tid, "any");
DROP FUNCTION weave_current_fused_distance(regclass, tid, "any");
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta') LIMIT 3;
-- the decoy is a C function, since only C can take "any", with the WRONG symbol
CREATE FUNCTION weave_current_fused_distance(regclass, tid, VARIADIC "any") RETURNS float8
  AS '$libdir/pg_weave', 'weave_current_distance' LANGUAGE C STRICT;
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta') LIMIT 3;
DROP FUNCTION weave_current_fused_distance(regclass, tid, "any");
CREATE FUNCTION weave_current_fused_distance(index regclass, row_ctid tid, VARIADIC keys "any")
  RETURNS float8 AS '$libdir/pg_weave', 'weave_current_fused_distance'
  LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
ALTER EXTENSION pg_weave ADD FUNCTION weave_current_fused_distance(regclass, tid, "any");
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM sf ORDER BY fuse(d <=> 'alpha', d <=> 'beta') LIMIT 3;

RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_sort;
DROP FUNCTION sr_wd();
DROP FUNCTION sf_ref(bool);
DROP FUNCTION sf_cur(bool);
DROP FUNCTION sf_ties(bool, int);
DROP FUNCTION sf_calls();
DROP TABLE sr, sr2, sr_pt, sr_v, sf, sfd;
