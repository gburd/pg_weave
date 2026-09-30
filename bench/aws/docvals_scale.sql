-- bench/aws/docvals_scale.sql -- docvals rule-12 correctness AT SCALE (10M rows).
--
-- Discharges the hard-rule-12 debt for G51 (segment merge carries the docvals
-- weft, incl. the delete-heavy tombstone-drop path) and G52 (a post-build INSERT
-- is answerable via the pending buffer, and stays answerable after the flush folds
-- it into a segment).  Both are LOCAL-green already; rule 12 says a release touching
-- tombstones/merge/vacuum needs a run at scale before it counts.
--
-- The whole file is SELF-CHECKING: every gate assertion compares the index-forced
-- answer against the seqscan answer as a SET and RAISEs EXCEPTION on any
-- disagreement, so with ON_ERROR_STOP=on a wrong answer at scale FAILS the run
-- loudly rather than printing a number nobody reads.  The counts are host-
-- independent (deterministic), so this is valid evidence regardless of the shared
-- burner's noise; only the \timing lines are indicative.
--
-- price is a SCATTERED, docid-UNCORRELATED int8 facet (a hash rank), i.e. the
-- general "any scalar facet" case and the worst case for docid-contiguity pruning
-- -- the same choice Task 8 made (bench/RESULTS_DOCVALS_PRIZE.md).

\set ON_ERROR_STOP on
\timing on

-- Build headroom, recorded per bench/METHODOLOGY.md (an unrecorded setting makes
-- the number unusable).  Session-scoped, so no cluster restart is needed.  NOTE:
-- weave's own flush budget (32MB, weave_build_mem_budget) is hardcoded and
-- independent of maintenance_work_mem, so a 10M build is multi-segment + merge
-- REGARDLESS of these -- they only keep PostgreSQL's own sort/parallel infra from
-- thrashing, they do not weaken the G51 merge stress.
SET maintenance_work_mem = '4GB';
SET max_parallel_maintenance_workers = 4;
SET work_mem = '256MB';

CREATE EXTENSION IF NOT EXISTS pg_weave;

-- index-forced set == seqscan set, or RAISE.  A positive control is folded in:
-- the index arm runs with seqscan+bitmapscan OFF, so it CANNOT silently answer by
-- a seqscan fallback -- if the docvals gate were absent the plan would error or
-- return a different set, and the disagreement count would be nonzero.
CREATE OR REPLACE FUNCTION dvs_assert_agree(pred text) RETURNS void
LANGUAGE plpgsql AS $fn$
DECLARE
    nbad bigint;
    nidx bigint;
BEGIN
    EXECUTE 'SET LOCAL enable_seqscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_indexscan = on';
    EXECUTE format('CREATE TEMP TABLE _i AS SELECT id FROM dvs WHERE %s', pred);
    GET DIAGNOSTICS nidx = ROW_COUNT;
    EXECUTE 'SET LOCAL enable_indexscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_seqscan = on';
    EXECUTE format('CREATE TEMP TABLE _s AS SELECT id FROM dvs WHERE %s', pred);
    SELECT count(*) INTO nbad FROM (
        SELECT id FROM _i EXCEPT SELECT id FROM _s
        UNION ALL
        SELECT id FROM _s EXCEPT SELECT id FROM _i
    ) d;
    DROP TABLE _i;
    DROP TABLE _s;
    RAISE NOTICE 'AGREE  pred=[%]  index_rows=%  disagreements=%', pred, nidx, nbad;
    IF nbad <> 0 THEN
        RAISE EXCEPTION 'DOCVALS GATE MISMATCH AT SCALE: pred=[%] disagreements=%', pred, nbad;
    END IF;
END;
$fn$;

-- weave_check(deep) must report zero violations, incl. the v3 text invariant
-- docvals_dictionary_ascending, or RAISE.  Also RAISEs if that invariant is
-- absent from the report: a deep check that did not examine the dictionary is
-- not evidence the dictionary is sound.
CREATE OR REPLACE FUNCTION dvs_assert_deep(idx text) RETURNS void
LANGUAGE plpgsql AS $fn$
DECLARE
    nbad bigint;
    ndict bigint;
BEGIN
    SELECT count(*) FILTER (WHERE NOT ok),
           count(*) FILTER (WHERE invariant = 'docvals_dictionary_ascending')
      INTO nbad, ndict
      FROM weave_check(idx::regclass, true);
    RAISE NOTICE 'DEEP  idx=%  violations=%  dict_checks=%', idx, nbad, ndict;
    IF nbad <> 0 OR ndict = 0 THEN
        RAISE EXCEPTION 'DOCVALS DEEP CHECK FAILED AT SCALE: idx=% violations=% dict_checks=%',
            idx, nbad, ndict;
    END IF;
END;
$fn$;

-- ===========================================================================
-- PHASE 1 -- BUILD.  10M rows force the build over the 32MB flush budget into
-- many segments and then a MERGE, which is exactly the G51 path (a merge must
-- carry each input's docvals weft into the output bolt).
-- ===========================================================================
\echo ==== PHASE 1: build 10,000,000 rows ====
DROP TABLE IF EXISTS dvs;
-- CREATE TABLE AS (parallel), never INSERT ... SELECT (loses the parallel plan).
-- Bounded vocabulary so the build is tractable, but with a ~1% term (freqNN) for
-- the lexical-AND-facet intersection test and a shared term (common) in every row.
CREATE TABLE dvs AS
  SELECT id,
         body,
         to_wdoc(body) AS d,
         (((hashint8(id) % 1000) + 1000) % 1000)::bigint AS price,
         ((((hashint8(id) % 1000) + 1000) % 1000)::float8 / 7.0) AS fprice,
         -- NULLABLE facet: ~10% NULL (hash rank ending in 0), else the same
         -- scattered 0..999.  A NULL satisfies no comparison, so the seqscan
         -- oracle excludes it -- disagreements=0 on nprice proves the v2 null
         -- bitmap excludes it too, at 10M, through build/insert/vacuum/merge.
         CASE WHEN (((hashint8(id) % 10) + 10) % 10) = 0 THEN NULL
              ELSE (((hashint8(id) % 1000) + 1000) % 1000)::bigint END AS nprice,
         -- TEXT facet (docvals v3 dictionary), COLLATE "C", on an INDEPENDENT hash
         -- (id+17) so it is not correlated with price/nprice.  Shape: ~5% NULL,
         -- ~1% empty string, else 'catNNN' or 'catNNNx' -- 1000 distinct values
         -- where every 'catNNNx' shares its whole prefix with 'catNNN', the
         -- dictionary-order edge a byte-length or prefix bug gets wrong.
         (CASE WHEN r2 < 100 THEN NULL
               WHEN r2 < 120 THEN ''
               ELSE 'cat' || lpad((r2 % 500)::text, 3, '0')
                    || CASE WHEN r2 % 3 = 0 THEN 'x' ELSE '' END
          END) COLLATE "C" AS tcat
    FROM (SELECT i AS id,
                 'common doc ' || (i % 5) || ' t' || (i % 100000) || ' freq' || (i % 100) AS body,
                 (((hashint8(i + 17) % 2000) + 2000) % 2000)::int AS r2
            FROM generate_series(1, 10000000) i) s;

-- shape assertions BEFORE spending a build: the facet must be scattered (many
-- distinct values) and price<100 must be ~10%, else the test proves nothing.
-- pct_null confirms the nullable facet actually carries NULLs (~10%).
SELECT count(*) AS nrows, count(DISTINCT price) AS distinct_price,
       round(100.0 * count(*) FILTER (WHERE price < 100) / count(*), 2) AS pct_lt100,
       round(100.0 * count(*) FILTER (WHERE nprice IS NULL) / count(*), 2) AS pct_null,
       count(DISTINCT tcat) AS distinct_tcat,
       round(100.0 * count(*) FILTER (WHERE tcat IS NULL) / count(*), 2) AS pct_tcat_null,
       round(100.0 * count(*) FILTER (WHERE tcat = '') / count(*), 2) AS pct_tcat_empty
  FROM dvs;

-- NO vector column here: the 10M vector-weft build/merge is the bottleneck
-- (weave_vec_block_read dominated a 61-min profile at ITYPE=m7i.4xlarge) and is
-- IRRELEVANT to the G51/G52 docvals correctness this run exists to prove.  The
-- lexical + docvals wefts still flush over many segments and MERGE (the G51 path).
-- The prize (which DOES need a vector column) is re-confirmed in docvals_prize.sql
-- at a smaller vector-bearing scale.
CREATE INDEX dvs_w ON dvs USING weave (d, price int8_docval_ops);
-- a float8 facet on the SAME table: proves the type-slice-2 encode holds through a
-- real 10M build/merge/delete/pending, not just the 500-row regression.
CREATE INDEX dvs_f8 ON dvs USING weave (d, fprice float8_docval_ops);
-- a NULLABLE int8 facet on the SAME table: proves the v2 null bitmap excludes
-- NULLs from a comparison gate through a real 10M build/merge/delete/pending
-- (its own index because the layout resolves ONE docvals weft per index).
CREATE INDEX dvs_np ON dvs USING weave (d, nprice int8_docval_ops);
-- a TEXT facet (v3 sorted dictionary + ordinals): proves the dictionary merge
-- (union + ordinal remap) and pending-buffer text compare hold at 10M.
CREATE INDEX dvs_t ON dvs USING weave (d, tcat text_docval_ops);
ANALYZE dvs;
SELECT weave_index_nsegments('dvs_w') AS nsegments_after_build;

-- one EXPLAIN to the log: the docvals qual must be an Index Cond, not a Filter.
SET enable_seqscan = off; SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM dvs WHERE price < 100;
EXPLAIN (COSTS OFF) SELECT id FROM dvs WHERE tcat < 'cat050';
RESET enable_seqscan; RESET enable_bitmapscan;

\echo ==== PHASE 2: gate == heap after build (G51: merge carried the weft) ====
SELECT dvs_assert_agree('price < 100');                         -- ~10%
SELECT dvs_assert_agree('price < 10');                          -- ~1%
SELECT dvs_assert_agree('price < 1');                           -- ~0.1%
SELECT dvs_assert_agree('price = 500');                         -- point
SELECT dvs_assert_agree('price >= 990');                        -- ~1% high end
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND price < 100$p$);   -- lexical ~1% AND facet 10%
SELECT dvs_assert_agree('fprice < 10.0');                       -- float8 encode ~7%
SELECT dvs_assert_agree($p$fprice < 4.5::float4$p$);            -- float8 cross-type const
SELECT dvs_assert_agree('nprice < 100');                        -- NULLABLE facet, NULLs excluded
SELECT dvs_assert_agree('nprice IS NOT NULL AND nprice < 100'); -- same set (redundant IS NOT NULL)
SELECT dvs_assert_agree('nprice >= 990');                       -- nullable high end
SELECT dvs_assert_agree($p$tcat < 'cat050'$p$);                -- TEXT ~10%, includes ''
SELECT dvs_assert_agree($p$tcat = 'cat123'$p$);                 -- TEXT point, prefix of cat123x
SELECT dvs_assert_agree($p$tcat = 'cat123x'$p$);                -- TEXT point, extends cat123
SELECT dvs_assert_agree($p$tcat > 'cat123' AND tcat <= 'cat124'$p$); -- TEXT range across the prefix edge
SELECT dvs_assert_agree($p$tcat = ''$p$);                       -- TEXT empty string (not NULL)
SELECT dvs_assert_agree($p$tcat >= 'cat490'$p$);                -- TEXT high end
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND tcat < 'cat050'$p$); -- lexical AND text facet
SELECT dvs_assert_deep('dvs_t');

-- ===========================================================================
-- PHASE 3 -- DELETE-HEAVY + VACUUM.  Delete ~40% spread across the whole docid
-- space (id % 5 < 2), so the tombstone sparsemap has a long chunk chain, then
-- VACUUM -> the merge must DROP tombstoned docids while still carrying the
-- surviving docids' docvals (the G51 tombstone-drop path at scale).
-- ===========================================================================
\echo ==== PHASE 3: delete ~40% + VACUUM ====
DELETE FROM dvs WHERE id % 5 < 2;
VACUUM dvs;
SELECT weave_index_nsegments('dvs_w') AS nsegments_after_vacuum;
SELECT dvs_assert_agree('price < 100');
SELECT dvs_assert_agree('price < 10');
SELECT dvs_assert_agree('price < 1');
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND price < 100$p$);
SELECT dvs_assert_agree('fprice < 10.0');
SELECT dvs_assert_agree('nprice < 100');                        -- nullable, after tombstone-drop VACUUM
SELECT dvs_assert_agree($p$tcat < 'cat050'$p$);
SELECT dvs_assert_agree($p$tcat = 'cat123x'$p$);
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND tcat < 'cat050'$p$);
SELECT dvs_assert_deep('dvs_t');

\echo ==== PHASE 4: explicit full merge, gate still == heap ====
SELECT weave_merge('dvs_w') IS NOT NULL AS merged;
SELECT weave_merge('dvs_np') IS NOT NULL AS merged_np;
SELECT weave_merge('dvs_t') IS NOT NULL AS merged_t;
SELECT weave_index_nsegments('dvs_w') AS nsegments_after_merge;
SELECT weave_index_nsegments('dvs_t') AS nsegments_t_after_merge;
SELECT dvs_assert_agree('price < 100');
SELECT dvs_assert_agree('price < 10');
SELECT dvs_assert_agree('nprice < 100');                        -- nullable, after explicit merge
SELECT dvs_assert_agree($p$tcat < 'cat050'$p$);
SELECT dvs_assert_agree($p$tcat = 'cat123x'$p$);
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND tcat < 'cat050'$p$);
SELECT dvs_assert_deep('dvs_t');
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND price < 100$p$);

-- ===========================================================================
-- PHASE 5 -- POST-BUILD INSERT (G52).  200k NEW rows land in the pending buffer
-- (small docs); the docvals gate must see them BEFORE any flush, then a VACUUM
-- folds them into a segment (the G52 flush path) and the gate must stay correct.
-- ===========================================================================
\echo ==== PHASE 5: insert 200k rows (pending, G52), gate must see them ====
INSERT INTO dvs
  SELECT id,
         body,
         to_wdoc(body),
         (((hashint8(id) % 1000) + 1000) % 1000)::bigint,
         ((((hashint8(id) % 1000) + 1000) % 1000)::float8 / 7.0),
         CASE WHEN (((hashint8(id) % 10) + 10) % 10) = 0 THEN NULL
              ELSE (((hashint8(id) % 1000) + 1000) % 1000)::bigint END,
         -- pending text: ~1/3 are NEW values 'catNNNm' that sort BETWEEN the
         -- segment dictionary's 'catNNN' and 'catNNNx', so the flush/merge must
         -- interleave them into the union dictionary and remap every ordinal.
         CASE WHEN r2 < 100 THEN NULL
              WHEN r2 < 120 THEN ''
              ELSE 'cat' || lpad((r2 % 500)::text, 3, '0')
                   || CASE r2 % 3 WHEN 0 THEN 'x' WHEN 1 THEN 'm' ELSE '' END
         END
    FROM (SELECT i AS id,
                 'common doc ' || (i % 5) || ' t' || (i % 100000) || ' freq' || (i % 100) AS body,
                 (((hashint8(i + 17) % 2000) + 2000) % 2000)::int AS r2
            FROM generate_series(10000001, 10200000) i) s;
SELECT weave_index_nsegments('dvs_w') AS nsegments_after_insert;
SELECT dvs_assert_agree('price < 10');                          -- includes pending rows
SELECT dvs_assert_agree('nprice < 10');                         -- nullable, pending rows incl. NULLs
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND price < 100$p$);   -- AND over pending
-- the pending-only text values must actually exist, or the two asserts below
-- agree vacuously on an empty set.
DO $$
DECLARE n bigint;
BEGIN
    SELECT count(*) INTO n FROM dvs WHERE tcat > 'cat123' AND tcat < 'cat123x';
    RAISE NOTICE 'SHAPE  pending-only text rows=%', n;
    IF n = 0 THEN
        RAISE EXCEPTION 'SHAPE: no pending-only text values; the interleave asserts would be vacuous';
    END IF;
END $$;
SELECT dvs_assert_agree($p$tcat = 'cat123m'$p$);                -- TEXT value ONLY in pending
SELECT dvs_assert_agree($p$tcat > 'cat123' AND tcat < 'cat123x'$p$); -- TEXT: exactly the new pending values
SELECT dvs_assert_agree($p$tcat < 'cat050'$p$);                 -- TEXT over segments + pending

\echo ==== PHASE 6: flush pending (VACUUM) + merge, gate still == heap ====
VACUUM dvs;
SELECT weave_merge('dvs_w') IS NOT NULL AS merged2;
SELECT weave_merge('dvs_np') IS NOT NULL AS merged2_np;
SELECT weave_merge('dvs_t') IS NOT NULL AS merged2_t;
SELECT weave_index_nsegments('dvs_w') AS nsegments_final;
SELECT dvs_assert_agree('price < 10');
SELECT dvs_assert_agree('price < 100');
SELECT dvs_assert_agree('fprice < 10.0');
SELECT dvs_assert_agree('nprice < 10');                         -- nullable, after flush+merge
SELECT dvs_assert_agree('nprice < 100');
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND price < 100$p$);
SELECT dvs_assert_agree($p$tcat = 'cat123m'$p$);                -- TEXT: pending value now in the merged dictionary
SELECT dvs_assert_agree($p$tcat > 'cat123' AND tcat < 'cat123x'$p$);
SELECT dvs_assert_agree($p$tcat < 'cat050'$p$);
SELECT dvs_assert_agree($p$tcat = ''$p$);
SELECT dvs_assert_agree($p$d @@@ 'freq7' AND tcat < 'cat050'$p$);
SELECT dvs_assert_deep('dvs_t');

SELECT pg_size_pretty(pg_relation_size('dvs_w')) AS index_size,
       pg_size_pretty(pg_relation_size('dvs_t')) AS text_index_size,
       pg_size_pretty(pg_relation_size('dvs'))   AS heap_size;

\echo ==== ALL DOCVALS SCALE ASSERTIONS PASSED (index == heap at every phase) ====
