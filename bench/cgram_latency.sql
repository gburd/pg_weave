-- Z8 BENCH, part 2: warm latency, three arms, six patterns, TWO passes.
--
-- METHOD, stated because AGENTS.md hard rule 10 says a between-arm delta smaller
-- than the within-arm spread is not a result, and rule 11 says a harness can make
-- a number up.
--
--  * MEDIAN OF 7, FIRST DROPPED.  Each cell runs the query 8 times and reports
--    the median of runs 2..8.  The first is dropped because it pays for buffer
--    warming and, on the pg_weave arm, for the first dictionary/posting page
--    reads; reporting it would measure the cache, not the channel.
--  * TWO PASSES over the whole table of cells, printed separately and NOT
--    averaged.  Pass-to-pass disagreement within one arm IS the within-arm
--    spread, and it is the only thing that says whether a between-arm difference
--    means anything.
--  * clock_timestamp() around the query, in plpgsql, not \timing.  \timing
--    includes psql round-trip and cannot be aggregated; and a count(*) is used as
--    the projection so the number is the SCAN, not the transfer of a result set.
--  * EACH ARM IS PINNED BY GUC, and the plan is printed once per arm so the
--    pinning is evidence rather than an intention.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE IF NOT EXISTS z8pat (n int primary key, pat text, note text);
TRUNCATE z8pat;
INSERT INTO z8pat VALUES
 (1, '%7 t1%',     'cross-token, ~311k matches'),
 (2, '%9 t20%',    'cross-token, ~31k matches'),
 (3, '%0 t123%',   'cross-token, ~3.1k matches'),
 (4, '%t12345%',   'in-token, ~352 matches'),
 (5, '%t20471%',   'in-token, few matches'),
 (6, '%zzq qzz%',  'no match, and no trigram of it exists');

-- One timing cell.  `arm` selects which GUCs are set; the SQL text differs per arm
-- only in the OPERATOR, because the pg_trgm and seq-scan arms must use LIKE (that
-- is what pg_trgm indexes) and the pg_weave arm must use @~ (its own operator).
-- Same predicate by construction: weave_cgram_like() IS core's textlike().
CREATE OR REPLACE FUNCTION z8time(arm text, pat text, reps int DEFAULT 8)
RETURNS TABLE (ms numeric, nrows bigint)
LANGUAGE plpgsql AS $$
DECLARE
  t0 timestamptz; t1 timestamptz; i int; c bigint;
  samples numeric[] := '{}';
BEGIN
  IF arm = 'trgm' THEN
    SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = on;
    SET LOCAL enable_bitmapscan = on;
  ELSIF arm = 'weave' THEN
    SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = on;
    SET LOCAL enable_bitmapscan = on;
  ELSE
    SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off;
    SET LOCAL enable_bitmapscan = off;
  END IF;
  FOR i IN 1..reps LOOP
    t0 := clock_timestamp();
    IF arm = 'weave' THEN
      EXECUTE 'SELECT count(*) FROM cgb WHERE body @~ $1' INTO c USING pat;
    ELSE
      EXECUTE 'SELECT count(*) FROM cgb WHERE body LIKE $1' INTO c USING pat;
    END IF;
    t1 := clock_timestamp();
    IF i > 1 THEN   -- FIRST RUN DROPPED
      samples := samples || (extract(epoch from (t1 - t0)) * 1000)::numeric;
    END IF;
  END LOOP;
  SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY s) INTO ms
    FROM unnest(samples) s;
  nrows := c;
  RETURN NEXT;
END $$;

\echo === plans, once per arm, so the GUC pinning is evidence ===
SET enable_seqscan = off;
DROP INDEX IF EXISTS cgb_w;      -- so the pg_weave arm cannot pick the no-cgram index
EXPLAIN (COSTS OFF) SELECT count(*) FROM cgb WHERE body @~ '%7 t1%';
EXPLAIN (COSTS OFF) SELECT count(*) FROM cgb WHERE body LIKE '%7 t1%';
SET enable_seqscan = on; SET enable_indexscan = off; SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM cgb WHERE body LIKE '%7 t1%';
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;

\echo === PASS 1 ===
SELECT p.n, p.pat, p.note,
       (SELECT ms FROM z8time('trgm',  p.pat)) AS trgm_ms,
       (SELECT ms FROM z8time('weave', p.pat)) AS weave_ms,
       (SELECT ms FROM z8time('seq',   p.pat)) AS seq_ms,
       (SELECT nrows FROM z8time('seq', p.pat, 2)) AS matches
  FROM z8pat p ORDER BY p.n;

\echo === PASS 2 ===
SELECT p.n, p.pat,
       (SELECT ms FROM z8time('trgm',  p.pat)) AS trgm_ms,
       (SELECT ms FROM z8time('weave', p.pat)) AS weave_ms,
       (SELECT ms FROM z8time('seq',   p.pat)) AS seq_ms
  FROM z8pat p ORDER BY p.n;

\echo === the route served every pattern it should have ===
SELECT weave_channel_stats_reset();
SET enable_seqscan = off;
SELECT count(*) FROM cgb WHERE body @~ '%0 t123%';
SELECT cgram_scan FROM weave_channel_stats();
