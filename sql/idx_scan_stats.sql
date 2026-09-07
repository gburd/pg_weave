-- pg_stat_user_indexes visibility: every query path that reads the weave index
-- to answer a query must register an index scan (idx_scan) and the index entries
-- it produced (idx_tup_read).  For each path we reset stats, force the plan,
-- run one query, flush, and read the counters.  The dataset is fixed so the
-- counts are deterministic: 'quick & fox' matches 'quick brown fox' and
-- 'quick fox runs' -- exactly half of the 4000 rows = 2000.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ss (id int, body text);
INSERT INTO ss SELECT g, (ARRAY['quick brown fox','lazy dog','quick fox runs','brown bear'])[1+g%4]
  FROM generate_series(1, 4000) g;
CREATE INDEX ss_weave ON ss USING weave (to_wdoc(body));

-- 1) Bitmap Index Scan (the common @@@ path); idx_tup_read comes from index_getbitmap
SELECT pg_stat_reset();
SET enable_seqscan = off;
SET enable_bitmapscan = on;
SET enable_indexscan = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM (SELECT id FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox')) q;
SELECT count(*) FROM (SELECT id FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox')) q;
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';

-- 2) Plain Index Scan (@@@ with bitmap disabled); idx_tup_read from index_getnext_tid
SELECT pg_stat_reset();
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM (SELECT id FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox')) q;
SELECT count(*) FROM (SELECT id FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox')) q;
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';
RESET enable_bitmapscan;

-- 3) count(*) pushdown (Custom Scan WeaveCount)
SELECT pg_stat_reset();
EXPLAIN (COSTS OFF) SELECT count(*) FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox');
SELECT count(*) FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox');
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';

-- 4) weave_search() native top-k (k=10 -> 10 index entries returned)
SELECT pg_stat_reset();
SELECT count(*) FROM weave_search('ss_weave', to_wquery('quick & fox'), 10);
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';

-- 5) weave_count() native count
SELECT pg_stat_reset();
SELECT weave_count('ss_weave', to_wquery('quick & fox'));
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';

-- Restore planner settings for the ranked cases below (default costing lets the
-- ordered index scan win; enabling seq scan also avoids the version-specific
-- "Disabled:" EXPLAIN annotation on PG18+ for the bare-ORDER-BY case).
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

-- 6) Ranked index-ordering scan: a WHERE @@@ restricts to the match set and
--    ORDER BY <=> is served in score order straight from the index (the
--    weave_gettuple ranked path).  LIMIT 5 -> 5 index entries returned.
SELECT pg_stat_reset();
EXPLAIN (COSTS OFF)
  SELECT id FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox')
-- NOTE (task L7, 2026-09-06): this used to expect `Sort -> Seq Scan` and
-- idx_scan = 0, because a bare `ORDER BY <=> LIMIT` with no WHERE clause could
-- not produce an index path while amoptionalkey was false.  It now expects an
-- ordering `Index Scan` and idx_scan = 1.  That change is the whole point of L7:
-- the bare form was a silent seq scan costing 83 ms par4 / 362 ms serial on 1M
-- documents against 0.05 ms for the WHERE-qualified form.  See
-- bench/RESULTS_LEXICAL.md and doc/GAPS.md G1.
  ORDER BY to_wdoc(body) <=> to_wquery('quick & fox') LIMIT 5;
SELECT count(*) FROM (
  SELECT id FROM ss WHERE to_wdoc(body) @@@ to_wquery('quick & fox')
  ORDER BY to_wdoc(body) <=> to_wquery('quick & fox') LIMIT 5) q;
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';

-- 7) A bare ORDER BY <=> with no @@@ filter cannot use the index: ranking the
--    whole corpus would also need the non-matching documents (all at maximum
--    distance), which the posting lists do not carry -- so it is a Sort over a
--    Seq Scan and idx_scan correctly stays 0.
SELECT pg_stat_reset();
EXPLAIN (COSTS OFF) SELECT id FROM ss ORDER BY to_wdoc(body) <=> to_wquery('quick fox') LIMIT 5;
SELECT count(*) FROM (SELECT id FROM ss ORDER BY to_wdoc(body) <=> to_wquery('quick fox') LIMIT 5) q;
SELECT pg_stat_force_next_flush();
SELECT idx_scan FROM pg_stat_user_indexes WHERE indexrelname = 'ss_weave';

DROP TABLE ss;
