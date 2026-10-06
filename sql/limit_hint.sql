-- doc/GAPS.md G87: the planner's LIMIT hint.  An index access method is not told
-- the LIMIT, so an ordering scan's first pass ran at pg_weave.wand_initial_k's
-- width (128) whatever the LIMIT was, and block-max WAND pruned against the
-- 128th-best score.  A planner_hook (src/am/customscan.c weave_planner) now copies
-- the ORDER BY wquery Const and writes LIMIT + OFFSET into its header `flags`,
-- which the scan uses as its first pass width only.
--
-- What each block guards, and the mutant that proves it can fail:
--   1. the GUC exists (an absent GUC reads like an OFF one: AGENTS.md, 12th member)
--   2. the hint REACHES the scan: less BM25 work at LIMIT 10       (mutant: ignore flags)
--   3. answers are exact with the hint on, including when the executor pulls far
--      past it through a filter the index does not apply     (mutant: no widening past k)
--   4. the flagship `WHERE d @@@ q ORDER BY d <=> q` still needs no recheck: the
--      hinted ORDER BY copy differs from the WHERE Const only in `flags`
--                                               (mutant: compare flags as well)
--   5. the hint is invisible: EXPLAIN VERBOSE prints the query unchanged
CREATE EXTENSION IF NOT EXISTS pg_weave;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE lim (id int, d wdoc) WITH (autovacuum_enabled = off);
INSERT INTO lim SELECT g, to_wdoc('simple',
   CASE WHEN g % 10 < 3 THEN 'a ' ELSE '' END || CASE WHEN g % 10 >= 7 THEN 'b ' ELSE '' END ||
   CASE WHEN g % 10 = 5 THEN 'c ' ELSE '' END || repeat('x' || (g % 101) || ' ', 1 + g % 23))
  FROM generate_series(1, 40000) g;
CREATE INDEX lim_w ON lim USING weave (d);
VACUUM ANALYZE lim;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;

-- 1.  Touch the index first: pg_settings only lists the GUC once the library is loaded.
SELECT count(*) FROM lim WHERE d @@@ 'zzz';
SELECT name, setting, short_desc IS NOT NULL AS registered
  FROM pg_settings WHERE name = 'pg_weave.limit_hint';

-- the exact ranking, from the AM's own top-k entry point (not the ordering scan)
CREATE TEMP TABLE ex AS
  SELECT l.id, s.score FROM weave_search('lim_w', 'a | b | c', 40000) s JOIN lim l ON l.ctid = s.ctid;
SELECT count(*) AS matched FROM ex;

-- 2.  The hint reaches the scan.
CREATE TEMP TABLE work (arm text, contribs bigint);
SET pg_weave.limit_hint = off;
SELECT weave_work_stats_reset();
SELECT count(*) FROM (SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 10) s;
INSERT INTO work SELECT 'off', lex_contribs FROM weave_work_stats();
SET pg_weave.limit_hint = on;
SELECT weave_work_stats_reset();
SELECT count(*) FROM (SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 10) s;
INSERT INTO work SELECT 'on', lex_contribs FROM weave_work_stats();
SELECT (SELECT contribs FROM work WHERE arm = 'on') < (SELECT contribs FROM work WHERE arm = 'off')
       AS hint_reduces_bm25_work;

-- 3.  Exact answers.  A returned sequence is right when it is non-increasing in the
-- exact score and its score multiset equals the exact top-n of the rows the query
-- admits.  (Ties are broad here, so ids are not compared: any tied member is right.)
CREATE FUNCTION lim_chk(q text, n int, filt text, off int DEFAULT 0) RETURNS text LANGUAGE plpgsql AS $$
DECLARE got float8[]; want float8[]; ordered bool;
BEGIN
  EXECUTE format('SELECT array_agg(e.score ORDER BY r.rn) FROM (SELECT row_number() OVER () rn, id FROM (%s) x) r JOIN ex e USING (id)', q) INTO got;
  EXECUTE format('SELECT array_agg(score ORDER BY score DESC) FROM (SELECT score FROM ex WHERE %s ORDER BY score DESC LIMIT %s OFFSET %s) z', filt, n, off) INTO want;
  SELECT bool_and(got[i] >= got[i + 1]) INTO ordered FROM generate_series(1, array_length(got, 1) - 1) i;
  RETURN format('n=%s ordered=%s exact=%s', array_length(got, 1), coalesce(ordered, true),
     (SELECT array_agg(v ORDER BY v DESC) FROM unnest(got) v) = want);
END $$;
SELECT 'LIMIT 10' AS shape, lim_chk($q$SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 10$q$, 10, 'true');
SELECT 'LIMIT 3 OFFSET 7' AS shape, lim_chk($q$SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 3 OFFSET 7$q$, 3, 'true', 7);
SELECT 'LIMIT 300' AS shape, lim_chk($q$SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 300$q$, 300, 'true');
-- the filter is not an index key, so the executor pulls ~97x past the hint
SELECT 'LIMIT 10, filtered past the hint' AS shape,
       lim_chk($q$SELECT id FROM lim WHERE d @@@ 'a | b | c' AND id % 97 = 0 ORDER BY d <=> 'a | b | c' LIMIT 10$q$, 10, 'id % 97 = 0');
SELECT 'LIMIT 40, rare term c' AS shape, lim_chk($q$SELECT id FROM lim WHERE d @@@ 'a | b | c' AND d @@@ 'c' ORDER BY d <=> 'a | b | c' LIMIT 40$q$, 40,
       'id IN (SELECT id FROM lim WHERE d @@@ ''c'')');

-- 4.  No recheck on the flagship.  The scan reports xs_recheck = false only when the
-- WHERE and ORDER BY queries are the same; the executor's recheck calls weave_match.
-- The control (different queries) proves the counter counts.
SET track_functions = 'all';
SET stats_fetch_consistency = none;
CREATE FUNCTION lim_wm() RETURNS bigint LANGUAGE sql AS
  $$ SELECT coalesce(sum(calls), 0) FROM pg_stat_user_functions WHERE funcname = 'weave_match' $$;
SELECT pg_stat_force_next_flush();
SELECT lim_wm() AS wm0 \gset
SELECT count(*) FROM (SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT lim_wm() - :wm0 AS flagship_recheck_calls;
SELECT lim_wm() AS wm1 \gset
SELECT count(*) FROM (SELECT id FROM lim WHERE d @@@ 'a' ORDER BY d <=> 'b' LIMIT 10) s;
SELECT pg_stat_force_next_flush();
SELECT lim_wm() - :wm1 > 0 AS control_rechecks;
RESET track_functions;

-- 5.  Invisible in EXPLAIN.
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT id FROM lim WHERE d @@@ 'a | b | c' ORDER BY d <=> 'a | b | c' LIMIT 10;

RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_sort;
DROP TABLE lim;
DROP FUNCTION lim_chk(text, int, text, int);
DROP FUNCTION lim_wm();
