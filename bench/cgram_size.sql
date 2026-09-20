-- Z8 BENCH: the honest cgram comparison (bench/RESULTS_CGRAM.md).
--
-- Same corpus as the pre-measurement (/tmp/z8pre.sql): 1M rows, 8 tokens each
-- from a ~250k vocabulary, so cross-token substrings exist and no single
-- vocabulary entry contains one.  Byte-identical generator, so the two runs are
-- comparable.
\set ON_ERROR_STOP on
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '4GB';
CREATE EXTENSION IF NOT EXISTS pg_trgm;
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

DROP TABLE IF EXISTS cgb;
CREATE TABLE cgb (id bigserial, body text);
INSERT INTO cgb(body)
  SELECT (SELECT string_agg('t' || ((g::bigint * 7 + i * 104729) % 250000), ' ')
            FROM generate_series(1, 8) i)
    FROM generate_series(1, 1000000) g;
VACUUM (ANALYZE) cgb;

\echo === A. heap ===
SELECT count(*) AS rows, pg_relation_size('cgb') AS heap_bytes,
       pg_size_pretty(pg_relation_size('cgb')) AS heap,
       avg(length(body))::int AS avg_bytes FROM cgb;

\echo === B. pg_trgm GIN ===
\timing on
CREATE INDEX cgb_gin ON cgb USING gin (body gin_trgm_ops);
\timing off
SELECT pg_relation_size('cgb_gin') AS gin_bytes,
       pg_size_pretty(pg_relation_size('cgb_gin')) AS gin_size;

\echo === C. pg_weave WITHOUT cgram ===
\timing on
CREATE INDEX cgb_w ON cgb USING weave (to_wdoc(body));
\timing off
SELECT pg_relation_size('cgb_w') AS weave_nocgram_bytes,
       pg_size_pretty(pg_relation_size('cgb_w')) AS weave_nocgram;

\echo === D. pg_weave WITH cgram ===
\timing on
CREATE INDEX cgb_wg ON cgb USING weave (to_wdoc(body), body gram_ops);
\timing off
SELECT pg_relation_size('cgb_wg') AS weave_cgram_bytes,
       pg_size_pretty(pg_relation_size('cgb_wg')) AS weave_cgram;

\echo === E. where the cgram bytes went ===
SELECT kind, npages, bytes, round(pct::numeric, 2) AS pct
  FROM weave_index_size_detail('cgb_wg')
 WHERE npages > 0 ORDER BY bytes DESC;

\echo === F. how many bolts (a cgram-bearing bolt is not mergeable: Z8 has no cgram merge producer) ===
SELECT weave_index_nsegments('cgb_w')  AS bolts_without_cgram,
       weave_index_nsegments('cgb_wg') AS bolts_with_cgram;
