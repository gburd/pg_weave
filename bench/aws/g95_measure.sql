\set ON_ERROR_STOP on
SET jit = off;
SET max_parallel_workers_per_gather = 0;
CREATE TEMP TABLE result (n int, arm text, leg text, route text, q text, k int,
                         ms float8, buffers bigint);
CREATE TEMP TABLE labels AS SELECT :n::int AS n, :'arm' AS arm, :'leg' AS leg;
-- Sequential membership oracle: independent from either index route. Keep
-- the full set and zero/ranked split so a LIMIT of wrong rows cannot pass.
SET enable_indexscan = off; SET enable_bitmapscan = off;
CREATE TEMP TABLE truth AS SELECT ctid AS tid, id, id % 10 <> 0 AS not_w1,
  id % 500 = 3 AS rare FROM g95_bench;
SET enable_seqscan = off; SET enable_indexscan = on; SET enable_sort = off;
DO $$
DECLARE spec record; route text; sql text; p json; i int; rows_ok bool; nr int;
BEGIN
 FOR spec IN SELECT * FROM (VALUES ('!w1', 10), ('!w1', 1000),
   ('rare | !w1', 10), ('rare | !w1', 5000), ('w2 & !w1', 10), ('w1', 10)) s(q,k) LOOP
  FOREACH route IN ARRAY ARRAY['scan', 'srf'] LOOP
   IF route = 'scan' THEN
    sql := format('SELECT ctid AS tid FROM g95_bench WHERE d @@@ %1$L::wquery '
                  'ORDER BY d <=> %1$L::wquery LIMIT %2$s', spec.q, spec.k);
   ELSE
    sql := format('SELECT ctid AS tid FROM weave_search(''g95_bench_w'', %L::wquery, %s)', spec.q, spec.k);
   END IF;
   -- Validate BEFORE timing: exact count, unique roots, membership and ranked
   -- prefix cardinality. These fixtures are INSERT-only so ctid is the root.
   EXECUTE format('WITH got AS (%s) SELECT count(*) = %s AND count(DISTINCT got.tid) = %s '
     'AND bool_and(t.id IS NOT NULL AND CASE %L '
     'WHEN ''!w1'' THEN t.not_w1 WHEN ''rare | !w1'' THEN t.rare OR t.not_w1 '
     'WHEN ''w2 & !w1'' THEN t.id %% 2 = 0 AND t.not_w1 ELSE NOT t.not_w1 END), '
     'count(*) FILTER (WHERE t.rare) FROM got LEFT JOIN truth t USING (tid)',
     sql, spec.k, spec.k, spec.q) INTO rows_ok, nr;
   IF rows_ok IS DISTINCT FROM true OR
      (spec.q = 'rare | !w1' AND nr <> least(spec.k, (SELECT count(*) FROM truth WHERE rare))) THEN
    RAISE EXCEPTION 'G95 correctness failure route=% q=% k=%', route, spec.q, spec.k;
   END IF;
   RAISE NOTICE 'G95 CORRECT route=% q=% k=%', route, spec.q, spec.k;
   FOR i IN 1..15 LOOP
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || sql INTO p;
    IF route = 'scan' AND p->0->'Plan'->'Plans'->0->>'Node Type' IS DISTINCT FROM 'Index Scan' THEN
     RAISE EXCEPTION 'G95 not an ordering scan: %', p;
    END IF;
    IF i > 3 THEN
     INSERT INTO result SELECT n, arm, leg, route, spec.q, spec.k,
       (p->0->>'Execution Time')::float8,
       (p->0->'Plan'->>'Shared Hit Blocks')::bigint +
       (p->0->'Plan'->>'Shared Read Blocks')::bigint FROM labels;
    END IF;
   END LOOP;
  END LOOP;
 END LOOP;
END $$;
COPY (SELECT n, arm, leg, route, q, k,
 percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) AS p50_ms,
 percentile_cont(0.99) WITHIN GROUP (ORDER BY ms) AS p99_ms,
 min(buffers) AS min_buffers, max(buffers) AS max_buffers
 FROM result GROUP BY n, arm, leg, route, q, k ORDER BY route, q, k)
 TO STDOUT WITH CSV HEADER;
