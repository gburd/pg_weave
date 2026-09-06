-- wvec: the vector type for the weave vector channel.
--
-- Semantics are pgvector's on purpose (see doc/MIGRATION.md), so these tests
-- double as a compatibility contract: if a case here changes behaviour, an
-- application migrating from pgvector breaks.
CREATE EXTENSION IF NOT EXISTS pg_weave;

-- sql/weave.sql creates the extension pinned at 0.1.0 on purpose, so running the
-- upgrade here is not a workaround -- it is the only coverage the 0.1.0 -> 0.2.0
-- upgrade script has, and an unexercised upgrade script is a blocking gate in
-- doc/PRODUCTION_READINESS.md.
ALTER EXTENSION pg_weave UPDATE;   -- to the current default_version

-- ---- I/O ----------------------------------------------------------------
SELECT '[1,2,3]'::wvec;
SELECT '[1.5,-2.25,0]'::wvec;
SELECT ' [ 1 , 2 , 3 ] '::wvec;                 -- whitespace tolerated
SELECT '[1]'::wvec;
SELECT wvec_dims('[1,2,3,4]'::wvec);

-- out(in(x)) must round-trip exactly: float4out is shortest-round-trip
SELECT '[0.1,0.2,0.3]'::wvec::text = '[0.1,0.2,0.3]' AS text_roundtrip;

-- ---- rejected input -----------------------------------------------------
SELECT '1,2,3'::wvec;                           -- no brackets
SELECT '[1,2,3'::wvec;                          -- unterminated
SELECT '[1,2,]'::wvec;                          -- trailing comma
SELECT '[1,,2]'::wvec;                          -- empty element
SELECT '[]'::wvec;                              -- zero dimensions
SELECT '[1,2,3]x'::wvec;                        -- junk after
SELECT '[1,NaN,3]'::wvec;                       -- NaN corrupts every distance
SELECT '[1,Infinity,3]'::wvec;

-- ---- typmod -------------------------------------------------------------
CREATE TABLE wv (id int, v wvec(3));
INSERT INTO wv VALUES (1, '[1,0,0]'), (2, '[0,1,0]'), (3, '[1,1,0]');
INSERT INTO wv VALUES (4, '[1,0]');             -- wrong dimension, must fail
SELECT id, v FROM wv ORDER BY id;
SELECT '[1,2,3,4]'::wvec(3);                    -- explicit cast enforces too
CREATE TABLE wv_bad (v wvec(0));
CREATE TABLE wv_bad (v wvec(99999));

-- ---- casts --------------------------------------------------------------
SELECT ARRAY[1,2,3]::real[]::wvec;
SELECT '[1,2,3]'::wvec::real[];
SELECT ARRAY[1,NULL,3]::real[]::wvec;           -- NULL element must fail

-- ---- accessors ----------------------------------------------------------
SELECT wvec_norm('[3,4]'::wvec);                -- 5
SELECT round(wvec_norm(wvec_l2_normalize('[3,4]'::wvec))::numeric, 6) AS unit_norm;
SELECT wvec_l2_normalize('[0,0,0]'::wvec) IS NULL AS zero_normalizes_to_null;

-- ---- distances ----------------------------------------------------------
SELECT wvec_l2_distance('[0,0]'::wvec, '[3,4]'::wvec) AS l2;             -- 5
SELECT wvec_l2_squared_distance('[0,0]'::wvec, '[3,4]'::wvec) AS l2sq;   -- 25
SELECT wvec_inner_product('[1,2,3]'::wvec, '[4,5,6]'::wvec) AS ip;       -- 32
SELECT wvec_l1_distance('[1,2]'::wvec, '[4,6]'::wvec) AS l1;             -- 7

-- operator forms must agree with the function forms
SELECT '[0,0]'::wvec <-> '[3,4]'::wvec AS op_l2;
SELECT '[1,2,3]'::wvec <#> '[4,5,6]'::wvec AS op_negip;   -- -32, NOT 32
SELECT '[1,0]'::wvec <+> '[0,1]'::wvec AS op_l1;

-- cosine: identical, orthogonal, opposite
SELECT round(('[1,0]'::wvec <=> '[1,0]'::wvec)::numeric, 10) AS cos_same;
SELECT round(('[1,0]'::wvec <=> '[0,1]'::wvec)::numeric, 10) AS cos_orth;
SELECT round(('[1,0]'::wvec <=> '[-1,0]'::wvec)::numeric, 10) AS cos_opp;
-- scale invariance: cosine ignores magnitude
SELECT round(('[1,2,3]'::wvec <=> '[2,4,6]'::wvec)::numeric, 10) AS cos_scaled;
-- a zero operand has no direction; pgvector returns NaN and so do we
SELECT ('[0,0]'::wvec <=> '[1,1]'::wvec) AS cos_zero_is_nan;

-- dimension mismatch is an error, not a silent truncation
SELECT '[1,2]'::wvec <-> '[1,2,3]'::wvec;

-- ---- arithmetic ---------------------------------------------------------
SELECT '[1,2]'::wvec + '[3,4]'::wvec AS added;
SELECT '[1,2]'::wvec - '[3,4]'::wvec AS subbed;
SELECT '[1,2]'::wvec * '[3,4]'::wvec AS multiplied;
SELECT '[1,2]'::wvec + '[1,2,3]'::wvec;         -- mismatch must fail

-- ---- comparison, DISTINCT, ORDER BY, btree ------------------------------
SELECT '[1,2]'::wvec = '[1,2]'::wvec AS eq;
SELECT '[1,2]'::wvec = '[1,3]'::wvec AS neq;
SELECT count(DISTINCT v) FROM (VALUES ('[1,0]'::wvec), ('[1,0]'::wvec),
                                      ('[0,1]'::wvec)) t(v);
CREATE INDEX wv_btree ON wv USING btree (v);
DROP INDEX wv_btree;

-- ---- exact nearest neighbour by seq scan --------------------------------
-- No index yet (tasks V7-V9), so this is the exact reference the index will be
-- checked against once it exists.
SELECT id, round((v <-> '[1,0,0]'::wvec)::numeric, 6) AS dist
  FROM wv ORDER BY v <-> '[1,0,0]'::wvec, id;

-- ---- codec introspection ------------------------------------------------
-- The quantizer is implemented and property-tested; these expose it so
-- reconstruction error is measurable on a real corpus before the index exists.
SELECT wvec_dims(weave_quantize_roundtrip('[1,2,3,4,5,6,7,8]'::wvec, 4)) AS rt_dims;

-- The renormalization scale is defined so the reconstruction's projection onto
-- the true direction equals the true norm.  So the reconstruction's norm must be
-- at least the original's, and within a modest factor of it.  This is the SQL
-- reflection of property P6 in test/hegel/test_quantize.c, and it is the claim
-- that "no float32 rerank pass is needed" rests on.
WITH v AS (SELECT ('[' || string_agg((n * 7 % 23 - 11)::text, ',') || ']')::wvec AS x
             FROM generate_series(1, 64) n)
SELECT wvec_norm(weave_quantize_roundtrip(x, 4)) >= wvec_norm(x) * 0.99 AS scale_lifts,
       wvec_norm(weave_quantize_roundtrip(x, 4)) <= wvec_norm(x) * 2.0  AS scale_bounded
  FROM v;

-- More bits must not be worse than fewer, on average.  A monotonicity check
-- that catches a codebook indexed by the wrong width.
WITH v AS (SELECT ('[' || string_agg((n * 13 % 31 - 15)::text, ',') || ']')::wvec AS x
             FROM generate_series(1, 128) n)
SELECT wvec_l2_distance(x, weave_quantize_roundtrip(x, 4))
       <= wvec_l2_distance(x, weave_quantize_roundtrip(x, 2)) AS more_bits_not_worse
  FROM v;

SELECT weave_quantize_roundtrip('[0,0,0,0]'::wvec, 4);   -- zero vector must fail
SELECT weave_quantize_roundtrip('[1,2,3,4]'::wvec, 1);   -- bits out of range
SELECT weave_quantize_roundtrip('[1,2,3,4]'::wvec, 5);

SELECT weave_quantize_size(768, 4) AS bytes_768_4bit;     -- 384 codes + 8 lane
SELECT weave_quantize_size(768, 2) AS bytes_768_2bit;
-- storage claim, made checkable: 4-bit codes against float4
SELECT round((768 * 4)::numeric / weave_quantize_size(768, 4), 1) AS compression_vs_float4;

DROP TABLE wv;
