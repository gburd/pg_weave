-- Run against main BEFORE changing padding, then against G95. No wall-clock
-- assertion: shared buffer visits expose materialization on a 20k-match set.
-- The baseline must fail this generous (<512 visits) first-row budget.
\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_weave;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
DROP TABLE IF EXISTS g95_stream;
CREATE TABLE g95_stream (d wdoc) WITH (autovacuum_enabled = off);
INSERT INTO g95_stream SELECT to_wdoc('simple', 'other') FROM generate_series(1, 20000);
CREATE INDEX g95_stream_w ON g95_stream USING weave(d);
VACUUM ANALYZE g95_stream;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
DO $$
DECLARE p json; visits int; q text;
BEGIN
  FOREACH q IN ARRAY ARRAY[
    'SELECT * FROM g95_stream WHERE d @@@ ''!absent''::wquery ORDER BY d <=> ''!absent''::wquery LIMIT 10',
    'SELECT * FROM weave_search(''g95_stream_w'', ''!absent''::wquery, 10)'] LOOP
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || q INTO p;
    IF (p->0->'Plan'->>'Actual Rows')::int <> 10 THEN
      RAISE EXCEPTION 'G95 wrong count: %', p;
    END IF;
    visits := (p->0->'Plan'->>'Shared Hit Blocks')::int
            + (p->0->'Plan'->>'Shared Read Blocks')::int;
    RAISE NOTICE 'G95 first-row budget: % buffer visits: %', visits, q;
    IF visits >= 512 THEN
      RAISE EXCEPTION 'G95 padding materializes before LIMIT: % buffer visits', visits;
    END IF;
  END LOOP;
END $$;
DROP TABLE g95_stream;
