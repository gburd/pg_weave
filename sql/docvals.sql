-- Docvals channel: the scalar/docvalues gate, end to end (spec
-- doc/specs/DOCVALS_CHANNEL.md sect. 5, 7, 8).
--
-- WHAT A GREEN RUN PROVES:
--   1. PLAN SHAPE.  A comparison on a docvalues column is an Index Cond, not an
--      executor Filter -- on a plain scan AND inside a fused ORDER BY fuse().
--      A Filter would be the pre-gate behaviour (doc/GAPS.md G49 / the prize
--      spike): correct answer, no skipping.  Threat 2 of bench/gatesweep.sh.
--   2. INDEX == HEAP, as a SET, for every btree strategy (< <= = >= >).  The
--      docvalues gate is EXACT, so the index-forced answer must equal the
--      seqscan answer exactly.  This is self-checking: the boolean is TRUE only
--      if the two plans agree, so a wrong gate flips it to FALSE rather than
--      being re-pinned when expected output is regenerated.
--   3. THE BARE (uncast) int4 literal pushes down -- the cross-type operators
--      (0.24.0 -> 0.25.0) -- and so does an int2 constant.
--   4. A FUSED query narrowed by a docvalues qual returns only rows satisfying
--      it, and is a subset of the unnarrowed answer.
--   5. A docvalues restriction NO LONGER CRASHES the scan (G49): every arm here
--      that once segfaulted now returns rows.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

SET max_parallel_workers_per_gather = 0;

CREATE TABLE dv (id int, body wdoc, emb wvec(4), price bigint);
-- price = g % 100 over 500 rows: each value 0..99 appears 5 times, so the
-- expected counts are deterministic (price < 50 -> 250, price = 50 -> 5, ...).
INSERT INTO dv
SELECT g,
       to_wdoc('common doc ' || (g % 5)),
       ('[' || g || ',' || (g % 4) || ',' || (g % 7) || ',1]')::wvec,
       (g % 100)::bigint
FROM generate_series(1, 500) g;
CREATE INDEX dv_w ON dv USING weave (body, emb, price int8_docval_ops)
    WITH (metric = ip);
ANALYZE dv;

-- ---------------------------------------------------------------------------
-- (1) PLAN SHAPE: Index Cond, not Filter -- plain and fused.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- bare int4 literal (cross-type operator): an Index Cond, not a Filter
EXPLAIN (COSTS OFF) SELECT id FROM dv WHERE price < 50;

-- inside a fused ORDER BY: the qual becomes a gate on the fused scan
EXPLAIN (COSTS OFF)
SELECT id FROM dv
 WHERE price < 50
 ORDER BY fuse(body <=> 'common'::wquery, emb <#> '[1,1,1,1]'::wvec) LIMIT 5;

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ---------------------------------------------------------------------------
-- (2) INDEX == HEAP, as a set, for every strategy.  The index arm forces a
-- plain Index Scan honouring the docvalues key; the seq arm forces a Seq Scan.
-- `agree` is TRUE only if the two return the identical id set.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION dv_agree(pred text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
    n_bad bigint;
BEGIN
    EXECUTE 'SET LOCAL enable_seqscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_indexscan = on';
    EXECUTE format('CREATE TEMP TABLE dv_idx AS SELECT id FROM dv WHERE %s', pred);
    EXECUTE 'SET LOCAL enable_indexscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_seqscan = on';
    EXECUTE format('CREATE TEMP TABLE dv_seq AS SELECT id FROM dv WHERE %s', pred);
    SELECT count(*) INTO n_bad FROM (
        SELECT id FROM dv_idx EXCEPT SELECT id FROM dv_seq
        UNION ALL
        SELECT id FROM dv_seq EXCEPT SELECT id FROM dv_idx
    ) d;
    DROP TABLE dv_idx;
    DROP TABLE dv_seq;
    RETURN n_bad = 0;
END;
$$;

SELECT dv_agree('price < 50')      AS lt_agrees,
       dv_agree('price <= 50')     AS le_agrees,
       dv_agree('price = 50')      AS eq_agrees,
       dv_agree('price >= 50')     AS ge_agrees,
       dv_agree('price > 50')      AS gt_agrees;

-- cross-type: an int2 constant, and a same-type int8 constant, both agree
SELECT dv_agree('price < 50::int2')    AS int2_agrees,
       dv_agree('price < 50::bigint')  AS int8_agrees;

-- conjunction with the lexical channel agrees too
SELECT dv_agree($$body @@@ 'common'::wquery AND price < 50$$) AS lex_and_dv_agrees;

-- ---------------------------------------------------------------------------
-- (3) DETERMINISTIC COUNTS (price = g % 100 over 500 rows).  Forced through the
-- index; a wrong gate would change these, and they are also checked against the
-- heap by (2) above.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS lt50 FROM dv WHERE price < 50;
SELECT count(*) AS eq50 FROM dv WHERE price = 50;
SELECT count(*) AS ge90 FROM dv WHERE price >= 90;
SELECT count(*) AS gt99 FROM dv WHERE price > 99;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- ---------------------------------------------------------------------------
-- (4) A FUSED query narrowed by a docvalues qual: every returned row satisfies
-- it, and the narrowed answer is a subset of the unnarrowed one.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;

CREATE TEMP TABLE dv_gated AS
SELECT id FROM dv
 WHERE price < 20
 ORDER BY fuse(body <=> 'common'::wquery, emb <#> '[1,1,1,1]'::wvec) LIMIT 10;

CREATE TEMP TABLE dv_ungated AS
SELECT id FROM dv
 ORDER BY fuse(body <=> 'common'::wquery, emb <#> '[1,1,1,1]'::wvec) LIMIT 500;

RESET enable_seqscan;
RESET enable_bitmapscan;

SELECT count(*) AS gated_rows_failing_qual
  FROM dv_gated JOIN dv USING (id)
 WHERE NOT (dv.price < 20);

SELECT NOT EXISTS (SELECT 1 FROM dv_gated
                    WHERE id NOT IN (SELECT id FROM dv_ungated))
       AS gated_is_subset_of_ungated;

DROP TABLE dv_gated, dv_ungated;
DROP FUNCTION dv_agree(text);
DROP TABLE dv;
