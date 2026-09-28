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

-- ---------------------------------------------------------------------------
-- (6) THE DOCVALS WEFT SURVIVES A SEGMENT MERGE (doc/GAPS.md G51).  A build that
-- flushes more than one segment, or a post-build INSERT + VACUUM that flushes a
-- second segment, is merged by weave_merge() into one bolt.  The merge must carry
-- the docvals weft of every input that has one into the output bolt; before the
-- G51 fix it wrote an EMPTY store, so a `price <op> c` gate over the merged bolt
-- returned ZERO rows -- a silent wrong answer that every existing test missed
-- because they all build a single segment (hard rule 12 / the eleventh member: no
-- test had ever run a docvals weft through a merge).
--
-- POSITIVE CONTROL: the `> 1` assertion makes a merge that never happened (one
-- segment) impossible to pass off as a fix, and `pre_merge_gate = 150` shows the
-- gate was correct on the first segment BEFORE the merge, so a post-merge 0 is the
-- merge losing it, not the build never writing it.
-- ---------------------------------------------------------------------------
CREATE TABLE dvm (id bigint PRIMARY KEY, body wdoc, price bigint);
INSERT INTO dvm SELECT g, to_wdoc('common doc ' || (g % 5)), g
  FROM generate_series(1, 300) g;
CREATE INDEX dvm_w ON dvm USING weave (body, price int8_docval_ops);

SET enable_seqscan = off;
SET enable_bitmapscan = off;
-- gate correct on the single build segment (rows 1..300, price = id)
SELECT count(*) AS pre_merge_gate FROM dvm WHERE price <= 150;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- a post-build INSERT + VACUUM flushes a SECOND segment (weave.sql's fixture
-- shape); a run of two is below the tiered auto-merge threshold, so both persist
INSERT INTO dvm SELECT g, to_wdoc('common doc ' || (g % 5)), g
  FROM generate_series(301, 600) g;
VACUUM dvm;
SELECT weave_index_nsegments('dvm_w') > 1 AS more_than_one_segment;

SELECT weave_merge('dvm_w') IS NOT NULL AS merged;
SELECT weave_index_nsegments('dvm_w') = 1 AS one_segment_after_merge;

-- THE GATE STILL WORKS AFTER THE MERGE.  price <= 150 selects the 150 rows the
-- first (docvals-bearing) segment held; the merged bolt must still answer it.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS post_merge_gate FROM dvm WHERE price <= 150;
-- and it agrees with the heap, forced both ways
CREATE TEMP TABLE dvm_idx AS SELECT id FROM dvm WHERE price <= 150;
RESET enable_seqscan;
RESET enable_bitmapscan;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
CREATE TEMP TABLE dvm_seq AS SELECT id FROM dvm WHERE price <= 150;
RESET enable_indexscan;
RESET enable_bitmapscan;
SELECT count(*) AS merged_gate_disagreements FROM (
    SELECT id FROM dvm_idx EXCEPT SELECT id FROM dvm_seq
    UNION ALL
    SELECT id FROM dvm_seq EXCEPT SELECT id FROM dvm_idx
) d;

DROP TABLE dvm_idx, dvm_seq;
DROP TABLE dvm;

-- ---------------------------------------------------------------------------
-- (7) THE DOCVALS GATE SEES UN-FLUSHED INSERTS (doc/GAPS.md G52).  Rows inserted
-- after the build live in the PENDING buffer, in no bolt and no docvalues store.
-- The lexical channel matches them from the pending pages, so before this fix a
-- query `@@@ x AND facet <op> c` ANDed a lexical set that HAD the inserted row
-- against a docvalues set that did NOT, and silently dropped it -- and a bare
-- `facet <op> c` over a freshly inserted row returned nothing.  A v11 pending item
-- now carries the row's int8 value, so the gate evaluates it over pending too.
--
-- POSITIVE CONTROL: nsegments stays 1 after the post-build INSERTs (they are
-- pending, not a flushed second segment), and the inserted low-price row is
-- REQUIRED to be in the index answer while the high-price one is REQUIRED to be
-- absent -- pre-fix the first assertion returned false because the gate could not
-- see the pending buffer.
-- ---------------------------------------------------------------------------
CREATE TABLE dvp (id int, body wdoc, price bigint);
INSERT INTO dvp SELECT g, to_wdoc('common doc ' || (g % 5)), (g % 100)::bigint
  FROM generate_series(1, 300) g;
CREATE INDEX dvp_w ON dvp USING weave (body, price int8_docval_ops);

-- rows inserted AFTER the build; small docs, so they land in the pending buffer
INSERT INTO dvp VALUES
  (1001, to_wdoc('zebrafish special report'), 5),
  (1002, to_wdoc('zebrafish special report'), 95);

-- still one segment: the two rows are pending, NOT flushed to a second bolt
SELECT weave_index_nsegments('dvp_w') = 1 AS still_one_segment_pending;

CREATE OR REPLACE FUNCTION dvp_agree(pred text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
    n_bad bigint;
BEGIN
    EXECUTE 'SET LOCAL enable_seqscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_indexscan = on';
    EXECUTE format('CREATE TEMP TABLE dvp_idx AS SELECT id FROM dvp WHERE %s', pred);
    EXECUTE 'SET LOCAL enable_indexscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_seqscan = on';
    EXECUTE format('CREATE TEMP TABLE dvp_seq AS SELECT id FROM dvp WHERE %s', pred);
    SELECT count(*) INTO n_bad FROM (
        SELECT id FROM dvp_idx EXCEPT SELECT id FROM dvp_seq
        UNION ALL
        SELECT id FROM dvp_seq EXCEPT SELECT id FROM dvp_idx
    ) d;
    DROP TABLE dvp_idx;
    DROP TABLE dvp_seq;
    RETURN n_bad = 0;
END;
$$;

-- the gate over pending agrees with the heap for every strategy...
SELECT dvp_agree('price < 50')  AS lt_agrees,
       dvp_agree('price <= 5')  AS le_agrees,
       dvp_agree('price = 95')  AS eq_agrees,
       dvp_agree('price >= 90') AS ge_agrees,
       dvp_agree('price > 90')  AS gt_agrees;

-- ...and so does the conjunction with the lexical channel (the G52 repro shape)
SELECT dvp_agree($$body @@@ 'zebrafish'::wquery AND price < 50$$) AS lex_and_dv_agrees;

-- POSITIVE CONTROL: the inserted low-price pending row IS in the index answer,
-- and the high-price one is NOT.  Pre-fix the gate was blind to pending, so the
-- first was false (the row was dropped) regardless of the heap.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT EXISTS (SELECT 1 FROM dvp WHERE body @@@ 'zebrafish' AND price < 50 AND id = 1001)
       AS pending_low_row_found,
       NOT EXISTS (SELECT 1 FROM dvp WHERE body @@@ 'zebrafish' AND price < 50 AND id = 1002)
       AS pending_high_row_excluded;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- FLUSH (Task 4): a VACUUM drains the pending buffer into a new segment whose
-- docvalues store must carry the flushed rows' values, so the gate keeps finding
-- them once they are no longer pending -- and still does after a merge (with G51).
VACUUM dvp;
SELECT weave_index_nsegments('dvp_w') > 1 AS flushed_second_segment;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT dvp_agree('price < 50') AS lt_agrees_after_flush;
SELECT EXISTS (SELECT 1 FROM dvp WHERE price < 50 AND id = 1001) AS flushed_low_row_found;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT weave_merge('dvp_w') IS NOT NULL AS merged;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT dvp_agree('price < 50') AS lt_agrees_after_merge;
RESET enable_seqscan;
RESET enable_bitmapscan;

DROP FUNCTION dvp_agree(text);
DROP TABLE dvp;

-- ---------------------------------------------------------------------------
-- (8) TYPE SLICE 2: float8 / int4 / int2 / date / bool docvals opclasses
-- (doc/plans/2026-09-27-docvals-types-slice.md).  Each type is mapped to an
-- order-preserving int64 at build and the query constant by the same rule at
-- scan, so the SAME store and evaluator serve every type.  The proof is the same
-- one the int8 slice uses: for each type and each btree strategy the index-forced
-- answer equals the seqscan answer as a SET (dvt_agree), and a bare/cross-type
-- constant still pushes down.  float8 additionally carries the values that break a
-- naive encoding: +0/-0 (equal), NaN (largest), +/-Infinity.
-- ---------------------------------------------------------------------------
CREATE TABLE dvt (id int, body wdoc,
                  f8 float8, i4 int4, i2 int2, dt date, bl bool);
INSERT INTO dvt
SELECT g, to_wdoc('common doc ' || (g % 5)),
       (g % 100)::float8 / 4.0,          -- scattered floats 0..24.75
       (g % 100),                        -- int4 0..99
       (g % 100)::int2,                  -- int2 0..99
       DATE '2000-01-01' + (g % 100),    -- 100 distinct dates
       (g % 2 = 0)                       -- bool
FROM generate_series(1, 500) g;
-- float8 special values, in their own rows (NOT NULL, so no NULLs)
INSERT INTO dvt (id, body, f8, i4, i2, dt, bl) VALUES
  (1001, to_wdoc('special'),  'Infinity'::float8, 0, 0, DATE '2000-01-01', true),
  (1002, to_wdoc('special'), '-Infinity'::float8, 0, 0, DATE '2000-01-01', true),
  (1003, to_wdoc('special'),       'NaN'::float8, 0, 0, DATE '2000-01-01', true),
  (1004, to_wdoc('special'),             0.0::float8, 0, 0, DATE '2000-01-01', true),
  (1005, to_wdoc('special'),            -0.0::float8, 0, 0, DATE '2000-01-01', true);

CREATE INDEX dvt_f8 ON dvt USING weave (body, f8 float8_docval_ops);
CREATE INDEX dvt_i4 ON dvt USING weave (body, i4 int4_docval_ops);
CREATE INDEX dvt_i2 ON dvt USING weave (body, i2 int2_docval_ops);
CREATE INDEX dvt_dt ON dvt USING weave (body, dt date_docval_ops);
CREATE INDEX dvt_bl ON dvt USING weave (body, bl bool_docval_ops);
ANALYZE dvt;

CREATE OR REPLACE FUNCTION dvt_agree(pred text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
    n_bad bigint;
BEGIN
    EXECUTE 'SET LOCAL enable_seqscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_indexscan = on';
    EXECUTE format('CREATE TEMP TABLE dvt_i AS SELECT id FROM dvt WHERE %s', pred);
    EXECUTE 'SET LOCAL enable_indexscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_seqscan = on';
    EXECUTE format('CREATE TEMP TABLE dvt_s AS SELECT id FROM dvt WHERE %s', pred);
    SELECT count(*) INTO n_bad FROM (
        SELECT id FROM dvt_i EXCEPT SELECT id FROM dvt_s
        UNION ALL
        SELECT id FROM dvt_s EXCEPT SELECT id FROM dvt_i
    ) d;
    DROP TABLE dvt_i;
    DROP TABLE dvt_s;
    RETURN n_bad = 0;
END;
$$;

-- plan shape: each facet comparison is an Index Cond, not a Filter
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM dvt WHERE f8 < 5.0::float8;
EXPLAIN (COSTS OFF) SELECT id FROM dvt WHERE i4 < 50;
EXPLAIN (COSTS OFF) SELECT id FROM dvt WHERE dt < DATE '2000-02-01';
EXPLAIN (COSTS OFF) SELECT id FROM dvt WHERE bl = true;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- float8: every strategy agrees with the heap, including the special values
SELECT dvt_agree('f8 < 5.0::float8')   AS f8_lt,
       dvt_agree('f8 <= 5.0::float8')  AS f8_le,
       dvt_agree('f8 = 5.0::float8')   AS f8_eq,
       dvt_agree('f8 >= 5.0::float8')  AS f8_ge,
       dvt_agree('f8 > 5.0::float8')   AS f8_gt;
SELECT dvt_agree($$f8 < 'Infinity'::float8$$)   AS f8_lt_inf,   -- excludes +inf and NaN
       dvt_agree($$f8 > '-Infinity'::float8$$)  AS f8_gt_neginf,-- excludes -inf
       dvt_agree($$f8 = 0.0::float8$$)          AS f8_eq_zero,  -- +0 and -0 both match
       dvt_agree($$f8 < 4.5::float4$$)          AS f8_lt_f4;    -- cross-type float4 const

-- int4 / int2: same-type and cross-type integer constants
SELECT dvt_agree('i4 < 50')            AS i4_lt,
       dvt_agree('i4 = 50')            AS i4_eq,
       dvt_agree('i4 >= 50')           AS i4_ge,
       dvt_agree('i4 < 50::int8')      AS i4_lt_i8,   -- cross-type
       dvt_agree('i4 < 50::int2')      AS i4_lt_i2;   -- cross-type
SELECT dvt_agree('i2 < 50::int2')      AS i2_lt,
       dvt_agree('i2 = 50::int2')      AS i2_eq,
       dvt_agree('i2 < 50')            AS i2_lt_i4,   -- cross-type
       dvt_agree('i2 < 50::int8')      AS i2_lt_i8;   -- cross-type

-- date / bool
SELECT dvt_agree($$dt < DATE '2000-02-01'$$)  AS dt_lt,
       dvt_agree($$dt = DATE '2000-01-15'$$)  AS dt_eq,
       dvt_agree($$dt >= DATE '2000-03-01'$$) AS dt_ge;
SELECT dvt_agree('bl = true')          AS bl_eq_true,
       dvt_agree('bl = false')         AS bl_eq_false,
       dvt_agree('bl < true')          AS bl_lt_true;  -- false < true

DROP FUNCTION dvt_agree(text);
DROP TABLE dvt;


-- ---------------------------------------------------------------------------
-- (9) NULLs (doc/plans/2026-09-27-docvals-nulls-slice.md, step 3).  A NULL
-- facet value satisfies NO comparison operator (SQL three-valued logic), so the
-- pushed-down gate must EXCLUDE it -- the same index==heap set proof as the
-- other slices, on a NULLABLE column, held across build, a post-build INSERT
-- (the pending path), and a DELETE+VACUUM (the merge/tombstone rewrite).
-- IS NULL / IS NOT NULL pushdown is deferred (spec sect. 6): they fall back to
-- an executor Filter, which is correct but unaccelerated -- asserted by the plan
-- shape below (no Index Cond on the IS NULL qual).
-- ---------------------------------------------------------------------------
CREATE TABLE dvn (id int, body wdoc, price bigint);
-- price = g % 100, but NULL every 7th row: ~71 NULLs of 500, and 50 still
-- occurs (g%100=50 at g=50,150,250,450 with g%7<>0), so `= 50` is non-empty.
INSERT INTO dvn
SELECT g, to_wdoc('common doc ' || (g % 5)),
       CASE WHEN g % 7 = 0 THEN NULL ELSE (g % 100)::bigint END
FROM generate_series(1, 500) g;
CREATE INDEX dvn_w ON dvn USING weave (body, price int8_docval_ops);
ANALYZE dvn;

CREATE OR REPLACE FUNCTION dvn_agree(pred text) RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
    n_bad bigint;
BEGIN
    EXECUTE 'SET LOCAL enable_seqscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_indexscan = on';
    EXECUTE format('CREATE TEMP TABLE dvn_i AS SELECT id FROM dvn WHERE %s', pred);
    EXECUTE 'SET LOCAL enable_indexscan = off';
    EXECUTE 'SET LOCAL enable_bitmapscan = off';
    EXECUTE 'SET LOCAL enable_seqscan = on';
    EXECUTE format('CREATE TEMP TABLE dvn_s AS SELECT id FROM dvn WHERE %s', pred);
    SELECT count(*) INTO n_bad FROM (
        SELECT id FROM dvn_i EXCEPT SELECT id FROM dvn_s
        UNION ALL
        SELECT id FROM dvn_s EXCEPT SELECT id FROM dvn_i
    ) d;
    DROP TABLE dvn_i;
    DROP TABLE dvn_s;
    RETURN n_bad = 0;
END;
$$;

-- plan shape: a comparison is an Index Cond; IS NULL / IS NOT NULL are NOT
-- (they fall back to a Filter -- deferred pushdown, spec sect. 6).  The IS NULL
-- plans are shown with seqscan ENABLED so they are a natural (not disabled) Seq
-- Scan -- PG18 annotates a disabled-but-chosen node with "Disabled: true", which
-- PG17 does not, so forcing the node would diverge the two majors' expected out.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM dvn WHERE price < 50;
RESET enable_seqscan;
RESET enable_bitmapscan;
EXPLAIN (COSTS OFF) SELECT id FROM dvn WHERE price IS NULL;
EXPLAIN (COSTS OFF) SELECT id FROM dvn WHERE price IS NOT NULL;

-- every comparison strategy agrees with the heap, NULLs excluded from all
SELECT dvn_agree('price < 50')   AS lt,
       dvn_agree('price <= 50')  AS le,
       dvn_agree('price = 50')   AS eq,
       dvn_agree('price >= 50')  AS ge,
       dvn_agree('price > 50')   AS gt;
-- IS NULL / IS NOT NULL: correct via the Filter fallback
SELECT dvn_agree('price IS NULL')     AS is_null,
       dvn_agree('price IS NOT NULL') AS is_not_null;

-- post-build INSERT of NULL and non-NULL rows (the pending path): the gate must
-- see them before flush, and still exclude the NULL.
INSERT INTO dvn VALUES
  (10001, to_wdoc('common doc 3'), NULL),
  (10002, to_wdoc('common doc 3'), 7),
  (10003, to_wdoc('common doc 3'), 99),
  (10004, to_wdoc('common doc 3'), NULL);
SELECT dvn_agree('price < 50')   AS pend_lt,
       dvn_agree('price = 99')   AS pend_eq99,
       dvn_agree('price IS NULL') AS pend_is_null;

-- DELETE 40% then VACUUM (tombstone rewrite + merge): still index==heap.
DELETE FROM dvn WHERE id % 5 = 1;
VACUUM dvn;
SELECT dvn_agree('price < 50')   AS vac_lt,
       dvn_agree('price >= 50')  AS vac_ge,
       dvn_agree('price IS NULL') AS vac_is_null;

DROP FUNCTION dvn_agree(text);
DROP TABLE dvn;


-- ---------------------------------------------------------------------------
-- 10. text docvalues (store v3; doc/plans/2026-09-28-docvals-text-slice.md,
-- task 2).  A text_docval_ops column stores a per-segment DICTIONARY ORDINAL
-- per docid, the dictionary sorted under the column collation.  This step
-- proves the build writes a store that the structural check accepts, and (10b)
-- that a multi-segment build's collapse re-dictionaries every input; pushdown
-- (task 3) and pending INSERT/flush (task 4) follow.
-- cat has exactly 40 distinct non-NULL values, one of them '' (a real entry,
-- not NULL), and is NULL on every 19th row (~5%).
-- ---------------------------------------------------------------------------
CREATE TABLE dvt (id int, body wdoc, cat text COLLATE "C");
INSERT INTO dvt
SELECT g, to_wdoc('common doc ' || (g % 5)),
       CASE WHEN g % 19 = 0 THEN NULL
            WHEN g % 40 = 0 THEN ''
            ELSE 'c' || lpad((g % 40)::text, 2, '0') END
FROM generate_series(1, 2000) g;
SELECT count(*) AS n, count(cat) AS nonnull, count(DISTINCT cat) AS ndistinct,
       count(*) FILTER (WHERE cat = '') AS nempty
FROM dvt;
CREATE INDEX dvt_idx ON dvt USING weave (body wdoc_lex_ops, cat text_docval_ops);
SELECT invariant, ok FROM weave_check('dvt_idx', true) ORDER BY invariant;
SELECT count(*) AS violations FROM weave_check('dvt_idx', true) WHERE NOT ok;
DROP TABLE dvt;

-- 10b. A MULTI-SEGMENT text build.  A serial build that flushes more than once
-- ends by collapsing its segments through the MERGE (weave_build_finalize ->
-- weave_merge_segments / weave_merge_all), and a text store cannot be carried
-- through a merge by copying: its slots are ordinals into each input segment's
-- own dictionary.  The merge re-dictionaries (feeds each row back as bytes), so
-- the collapsed bolt must still carry a docvalues weft.
--
-- Corpus: sql/vecindex.sql's high-vocabulary shape (40 distinct terms per
-- document) under maintenance_work_mem = '1MB', which flushes several bolts.
-- cat mixes a value set that is DISJOINT per heap range ('k' || g/500, so each
-- flushed bolt's dictionary holds different strings and the same string gets
-- different ordinals in different bolts) with a shared one, '' and NULL.
--
-- POSITIVE CONTROL for "the collapse really carried every input": the build
-- flushes seven bolts here (the per-flush LOG lines say so under
-- client_min_messages = log) and the collapse merges them into one, so a merged
-- store covering all 12000 docs needs at least ceil(12000 * 16 / 8160) = 24
-- docvalues pages (an 8-byte ordinal + 8-byte docid per doc).  The interim guard
-- that dropped text wefts in the merge gives 0; a merge that carried only one
-- input bolt gives about 4.
-- ---------------------------------------------------------------------------
CREATE TABLE dvtm (id int, body wdoc, cat text COLLATE "C");
INSERT INTO dvtm
SELECT g, to_wdoc(array_to_string(ARRAY(SELECT 'id' || g || 'x' || k
                                          FROM generate_series(1, 40) k), ' ')),
       CASE WHEN g % 19 = 0 THEN NULL
            WHEN g % 40 = 0 THEN ''
            WHEN g % 3 = 0 THEN 'c' || lpad((g % 40)::text, 2, '0')
            ELSE 'k' || lpad((g / 500)::text, 4, '0') END
FROM generate_series(1, 12000) g;
SET maintenance_work_mem = '1MB';
SET max_parallel_maintenance_workers = 0;
CREATE INDEX dvtm_idx ON dvtm USING weave (body wdoc_lex_ops, cat text_docval_ops);
SELECT weave_index_nsegments('dvtm_idx') = 1 AS collapsed_to_one_segment;
SELECT count(*) >= 24 AS docvalues_weft_covers_all_inputs
  FROM weave_page_info('dvtm_idx')
 WHERE kind = 'docvalues' AND reachable AND NOT freed;
SELECT invariant, ok FROM weave_check('dvtm_idx', true) ORDER BY invariant;
SELECT count(*) AS violations FROM weave_check('dvtm_idx', true) WHERE NOT ok;
RESET maintenance_work_mem;
RESET max_parallel_maintenance_workers;
DROP TABLE dvtm;
