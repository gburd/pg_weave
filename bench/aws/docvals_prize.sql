-- bench/aws/docvals_prize.sql -- the docvals PRIZE, re-confirmed at a SECOND scale
-- (rule 11: a number is provisional until a second scale; Task 8 measured fiqa
-- 57.6k / scifact 5.2k, this is 1M -- ~17x fiqa).
--
-- BEST-EFFORT (harness runs it with ON_ERROR_STOP off): the rule-12 deliverable is
-- docvals_scale.sql.  This builds its OWN vector-bearing table because the 10M
-- correctness run deliberately has no vector column (the 10M vector-weft build is
-- an unrelated bottleneck).  1M keeps the vector build to a few minutes.
--
-- PRIMARY METRIC: weave_work_stats().vec_blocks -- deterministic, host-independent.
-- Claim 3 holds iff vec_blocks FALLS as the docvals gate tightens (a selective
-- facet makes the vector scan do LESS work), not rises (over-fetch/collapse).

\set ON_ERROR_STOP off
\pset pager off
\timing on
SET maintenance_work_mem = '2GB';
SET max_parallel_maintenance_workers = 4;

\echo ==== building the 1M vector-bearing prize table ====
DROP TABLE IF EXISTS dvsv;
CREATE TABLE dvsv AS
  SELECT id,
         to_wdoc(body) AS d,
         ('[' || (id % 9) || ',' || (id % 7) || ',' || (id % 5) || ',' || (id % 3) || ']')::wvec AS emb,
         (((hashint8(id) % 1000) + 1000) % 1000)::bigint AS price
    FROM (SELECT i AS id, 'common doc ' || (i % 5) || ' t' || (i % 100000) AS body
            FROM generate_series(1, 1000000) i) s;
CREATE INDEX dvsv_w ON dvsv USING weave (d, emb, price int8_docval_ops) WITH (metric = ip);
ANALYZE dvsv;
SELECT weave_index_nsegments('dvsv_w') AS nsegments;

-- the docvals qual must push to an Index Cond inside the fused scan, else the
-- comparison is void.
SET enable_seqscan = off; SET enable_bitmapscan = off; SET jit = off;
EXPLAIN (COSTS OFF)
SELECT id FROM dvsv WHERE price < 100
 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[1,2,3,1]'::wvec, weights => '{0.5,0.5}') LIMIT 10;

\echo ==== prize @ 1M: vec_blocks vs facet selectivity (expect it to FALL) ====

-- 10% (price < 100)
SELECT weave_work_stats_reset();
SELECT id FROM dvsv WHERE price < 100 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[1,2,3,1]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 100 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[3,1,2,2]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 100 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[2,3,1,0]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 100 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[0,1,2,3]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 100 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[4,0,1,2]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT 'sel=0.10 (price<100)' AS arm, vec_lanes, vec_blocks FROM weave_work_stats();

-- 1% (price < 10)
SELECT weave_work_stats_reset();
SELECT id FROM dvsv WHERE price < 10 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[1,2,3,1]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 10 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[3,1,2,2]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 10 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[2,3,1,0]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 10 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[0,1,2,3]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 10 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[4,0,1,2]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT 'sel=0.01 (price<10)' AS arm, vec_lanes, vec_blocks FROM weave_work_stats();

-- 0.1% (price < 1)
SELECT weave_work_stats_reset();
SELECT id FROM dvsv WHERE price < 1 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[1,2,3,1]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 1 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[3,1,2,2]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 1 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[2,3,1,0]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 1 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[0,1,2,3]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT id FROM dvsv WHERE price < 1 ORDER BY fuse(d <=> 'common'::wquery, emb <#> '[4,0,1,2]'::wvec, weights => '{0.5,0.5}') LIMIT 10;
SELECT 'sel=0.001 (price<1)' AS arm, vec_lanes, vec_blocks FROM weave_work_stats();
