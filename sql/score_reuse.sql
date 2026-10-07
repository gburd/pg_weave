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

RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_sort;
DROP FUNCTION sr_wd();
DROP TABLE sr, sr2, sr_pt, sr_v;
