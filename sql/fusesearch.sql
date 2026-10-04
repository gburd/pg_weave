--
-- weave_fuse_search(): a fused ranking WITH its scores (doc/PHASES.md F3,
-- doc/specs/FUSED_TOPK.md sect. 7e).
--
-- The SRF is a workaround for a PostgreSQL core limitation (an index scan cannot
-- hand its ORDER BY value to the SELECT list; doc/upstream/ORDERBY_VALUES_TO_TLIST.md).
-- It drives the SAME fused index scan `ORDER BY fuse(...)` does, so this file holds
-- it to two things:
--
--   * ORDER: its rows come out in exactly the order of
--     `SELECT ctid ... ORDER BY fuse(...)` on the index path, in both
--     pg_weave.fuse_normalize modes.
--   * SCORE: with fuse_normalize off, each row's score equals an exact oracle
--     within 1e-4.  The oracle is the pattern of sql/fuse_pushdown.sql (G66): the
--     index's own BM25 from weave_search() for the lexical key, plus -||q - v||^2
--     for the vector key with q = 0, where the quantized lane score is exact.
--
-- And a row whose fuse() is NULL (a NULL vector, a NULL document) or whose heap
-- tuple is dead must not appear: the fused scan pads the first and the heap fetch
-- drops the second, and the SRF must honour both.
--
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

SET max_parallel_workers_per_gather = 0;

-- 30 flushed rows with tf of `alpha` = g % 5 + 1 and vector [0.1 g, 0, 0, 0], so the
-- two channels pull in different directions; three `beta` rows only the vector
-- channel reaches; 101 has a NULL vector, 301 a NULL document; 401 is PENDING (G66);
-- 501 matches neither lexical query, so only a vector channel ranks it;
-- 7 and 12 are deleted after the build and never vacuumed, so the index still
-- carries them.
CREATE TABLE fs (id int, body wdoc, emb wvec(4));
INSERT INTO fs SELECT g, to_wdoc('simple', repeat('alpha ', g % 5 + 1) || 'w' || g),
                      ('[' || (g * 0.1) || ',0,0,0]')::wvec
  FROM generate_series(1, 30) g;
INSERT INTO fs SELECT g, to_wdoc('simple', 'beta w' || g),
                      ('[' || ((g - 200) * 0.15) || ',0,0,0]')::wvec
  FROM generate_series(201, 203) g;
INSERT INTO fs VALUES (101, to_wdoc('simple', 'alpha alpha alpha w101'), NULL),
                      (301, NULL, '[0.05,0,0,0]'),
                      (501, to_wdoc('simple', 'gamma w501'), '[0.35,0,0,0]');
CREATE INDEX fs_weave ON fs USING weave (body, emb);
INSERT INTO fs VALUES (401, to_wdoc('simple', 'alpha alpha pendingrow'), '[0.25,0,0,0]');
CREATE TEMP TABLE fs_dead AS SELECT ctid AS rowtid FROM fs WHERE id IN (7, 12);
DELETE FROM fs WHERE id IN (7, 12);
-- 3 is HOT-updated: the index keeps the chain ROOT's TID, and the row's ctid is
-- now the new tuple's.  The SRF must return the latter (the heap fetch resolves the
-- chain), which is what `ORDER BY ... ctid` returns.
BEGIN;
UPDATE fs SET id = 3 WHERE id = 3;
SELECT pg_stat_get_xact_tuples_hot_updated('fs'::regclass) AS hot_updates;
COMMIT;
ANALYZE fs;

-- The arm the SRF is compared against IS the fused index path.
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT ctid FROM fs
 ORDER BY fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec,
               weights => '{0.5,2}') LIMIT 1000;
RESET enable_seqscan;

-- ---------------------------------------------------------------------------
-- (1) fuse_normalize OFF: order and score, for explicit and for NULL weights.
-- ---------------------------------------------------------------------------
SET pg_weave.fuse_normalize = off;

SET enable_seqscan = off;
CREATE TEMP TABLE fs_ord AS
  SELECT row_number() OVER () AS rn, rowtid
    FROM (SELECT ctid AS rowtid FROM fs
           ORDER BY fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec,
                         weights => '{0.5,2}') LIMIT 1000) s;
CREATE TEMP TABLE fs_ord1 AS
  SELECT row_number() OVER () AS rn, rowtid
    FROM (SELECT ctid AS rowtid FROM fs
           ORDER BY fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec)
           LIMIT 1000) s;
RESET enable_seqscan;

CREATE TEMP TABLE fs_srf AS
  SELECT row_number() OVER () AS rn, ctid AS rowtid, score, parts
    FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery],
                           ARRAY['[0,0,0,0]'::wvec], '{0.5,2}', 1000);
CREATE TEMP TABLE fs_srf1 AS
  SELECT row_number() OVER () AS rn, ctid AS rowtid, score, parts
    FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery],
                           ARRAY['[0,0,0,0]'::wvec]);

-- The exact oracle, over every live row whose fuse() is not NULL.
CREATE TEMP TABLE fs_orc AS
  SELECT f.ctid AS rowtid, f.id,
         0.5::float8 * COALESCE(s.score, 0::float8)
         - 2::float8 * (f.emb <-> '[0,0,0,0]'::wvec)::float8 ^ 2 AS score,
         COALESCE(s.score, 0::float8)
         - (f.emb <-> '[0,0,0,0]'::wvec)::float8 ^ 2 AS score1
    FROM fs f
    LEFT JOIN weave_search('fs_weave', 'alpha'::wquery, 1000) s ON s.ctid = f.ctid
   WHERE f.body IS NOT NULL AND f.emb IS NOT NULL;

-- Tie-freeness first: a tie would make "the" order ambiguous however the scan behaves.
SELECT count(*) = count(DISTINCT score) AS oracle_tie_free,
       count(*) = count(DISTINCT score1) AS oracle1_tie_free
  FROM fs_orc;

-- Every live, non-NULL-fused row once, and no other row.  The default k is 10.
SELECT (SELECT count(*) FROM fs_srf) = (SELECT count(*) FROM fs_orc) AS srf_every_ranked_row,
       (SELECT count(DISTINCT rowtid) FROM fs_srf) = (SELECT count(*) FROM fs_srf) AS srf_no_row_twice,
       (SELECT count(*) FROM fs_srf s JOIN fs_orc o USING (rowtid))
       = (SELECT count(*) FROM fs_srf) AS srf_rows_are_live_and_non_null,
       (SELECT count(*) FROM fs_srf1) AS default_k_rows;

-- The rows that must not appear, by name.
SELECT (SELECT count(*) FROM fs_srf s JOIN fs f ON f.ctid = s.rowtid
         WHERE f.id IN (101, 301)) AS null_fused_rows_returned,
       (SELECT count(*) FROM fs_srf s JOIN fs_dead d USING (rowtid)) AS dead_rows_returned,
       (SELECT count(*) FROM fs_srf s JOIN fs f ON f.ctid = s.rowtid
         WHERE f.id = 401) AS pending_row_returned,
       (SELECT count(*) FROM fs_srf s JOIN fs f ON f.ctid = s.rowtid
         WHERE f.id = 3) AS hot_row_returned_at_its_live_ctid;

-- ORDER: the SRF's rows are the index path's head, in its order.
SELECT (SELECT array_agg(rowtid ORDER BY rn) FROM fs_srf)
       = (SELECT array_agg(rowtid ORDER BY rn) FROM fs_ord
           WHERE rn <= (SELECT count(*) FROM fs_srf)) AS order_matches_index_path,
       (SELECT array_agg(rowtid ORDER BY rn) FROM fs_srf1)
       = (SELECT array_agg(rowtid ORDER BY rn) FROM fs_ord1 WHERE rn <= 10)
       AS null_weights_order_matches_index_path;

-- SCORE: per row against the oracle, and best first.
SELECT max(abs(s.score - o.score)) < 1e-4 AS score_matches_oracle,
       bool_and(s.score >= COALESCE(n.score, '-Infinity')) AS score_descends,
       bool_and(s.parts IS NULL) AS parts_owed_null
  FROM fs_srf s JOIN fs_orc o USING (rowtid)
  LEFT JOIN fs_srf n ON n.rn = s.rn + 1;
SELECT max(abs(s.score - o.score1)) < 1e-4 AS null_weights_score_matches_oracle
  FROM fs_srf1 s JOIN fs_orc o USING (rowtid);

-- k bounds the result and keeps the head.
SELECT (SELECT array_agg(ctid) FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery],
                                                     ARRAY['[0,0,0,0]'::wvec], '{0.5,2}', 3))
       = (SELECT array_agg(rowtid ORDER BY rn) FROM fs_srf WHERE rn <= 3) AS k3_is_the_head,
       (SELECT count(*) FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery],
                                               ARRAY['[0,0,0,0]'::wvec], NULL, 0)) AS k0_rows;

RESET pg_weave.fuse_normalize;

-- ---------------------------------------------------------------------------
-- (2) fuse_normalize ON (the default): order only.  The normalized score is the
-- fused pass's own number; the order is what a caller can hold it to.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
CREATE TEMP TABLE fs_ordn AS
  SELECT row_number() OVER () AS rn, rowtid
    FROM (SELECT ctid AS rowtid FROM fs
           ORDER BY fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec,
                         weights => '{0.5,2}') LIMIT 1000) s;
CREATE TEMP TABLE fs_ordl AS
  SELECT row_number() OVER () AS rn, rowtid
    FROM (SELECT ctid AS rowtid FROM fs
           ORDER BY fuse(body <=> 'alpha'::wquery, body <=> 'beta'::wquery)
           LIMIT 1000) s;
RESET enable_seqscan;
CREATE TEMP TABLE fs_srfn AS
  SELECT row_number() OVER () AS rn, ctid AS rowtid, score
    FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery],
                           ARRAY['[0,0,0,0]'::wvec], '{0.5,2}', 1000);
-- Two lexical channels and no vector one.
CREATE TEMP TABLE fs_srfl AS
  SELECT row_number() OVER () AS rn, ctid AS rowtid, score
    FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery, 'beta'::wquery], k => 1000);

SELECT (SELECT count(*) FROM fs_srfn) = (SELECT count(*) FROM fs_orc) AS norm_every_ranked_row,
       (SELECT array_agg(rowtid ORDER BY rn) FROM fs_srfn)
       = (SELECT array_agg(rowtid ORDER BY rn) FROM fs_ordn
           WHERE rn <= (SELECT count(*) FROM fs_srfn)) AS norm_order_matches_index_path,
       (SELECT count(*) FROM fs_srfn s JOIN fs_dead d USING (rowtid)) AS norm_dead_rows_returned,
       (SELECT bool_and(s.score >= COALESCE(n.score, '-Infinity'))
          FROM fs_srfn s LEFT JOIN fs_srfn n ON n.rn = s.rn + 1) AS norm_score_descends;
SELECT (SELECT array_agg(rowtid ORDER BY rn) FROM fs_srfl)
       = (SELECT array_agg(rowtid ORDER BY rn) FROM fs_ordl
           WHERE rn <= (SELECT count(*) FROM fs_srfl)) AS lex_only_order_matches_index_path,
       (SELECT count(*) FROM fs_srfl s JOIN fs_dead d USING (rowtid)) AS lex_only_dead_rows_returned;
-- With no vector key, a document neither lexical channel reaches is PADDED at the
-- value fuse() gives it, -0, after every ranked row (doc/specs/FUSED_TOPK.md 7e).
SELECT s.rn = (SELECT max(rn) FROM fs_srfl) AS lex_only_unreached_row_is_last,
       s.score = 0 AS lex_only_unreached_score_is_zero,
       (SELECT min(score) > 0 FROM fs_srfl WHERE rn < s.rn) AS lex_only_ranked_rows_positive
  FROM fs_srfl s JOIN fs f ON f.ctid = s.rowtid WHERE f.id = 501;

-- ---------------------------------------------------------------------------
-- (3) Refusals.
-- ---------------------------------------------------------------------------
SELECT * FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery]);
SELECT * FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery],
                                ARRAY['[0,0,0,0]'::wvec], '{1,1,1}');
SELECT * FROM weave_fuse_search('fs_weave', ARRAY['alpha'::wquery, NULL]);

DROP TABLE fs_ord, fs_ord1, fs_srf, fs_srf1, fs_orc, fs_ordn, fs_ordl, fs_srfn, fs_srfl, fs_dead;
DROP TABLE fs;
