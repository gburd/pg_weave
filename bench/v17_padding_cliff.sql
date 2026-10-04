-- run against a database loaded by: PGDATABASE=<db> FUSE_LOAD_ONLY=1 bash bench/fuse.sh <bench> scifact
SELECT wq, qv::text AS qv FROM fq ORDER BY qid LIMIT 1 \gset
SET jit = off; SET max_parallel_workers_per_gather = 0; SET pg_weave.fuse_normalize = off;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS abolish_rows FROM fd WHERE body @@@ 'abolish'::wquery;
-- (1) the cliff: 5 qualifying rows, LIMIT 10 -> the ranked phase runs dry and the padding walk reads the heap
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF)
SELECT id FROM fd WHERE body @@@ 'abolish'::wquery
 ORDER BY fuse(body <=> :'wq'::wquery, emb <#> :'qv'::wvec, weights => '{0.5,0.5}') LIMIT 10;
-- (2) control: same query, LIMIT 5 = the qualifying count -> no padding
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF)
SELECT id FROM fd WHERE body @@@ 'abolish'::wquery
 ORDER BY fuse(body <=> :'wq'::wquery, emb <#> :'qv'::wvec, weights => '{0.5,0.5}') LIMIT 5;
