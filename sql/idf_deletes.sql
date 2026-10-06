-- idf_deletes: index-side IDF after deletes (doc/GAPS.md G83).
--
-- N is the LIVE corpus size (metapage ndocs, tombstones subtracted), but a term's
-- df is the dictionary df, which still counts a deleted document's postings until a
-- merge rewrites its segment.  For a term in nearly every document, deleting a
-- fraction makes df > N, and the unclamped ln(1 + (N - df + 0.5)/(df + 0.5)) goes
-- NEGATIVE: every contribution flips sign and block-max WAND -- whose bounds assume
-- non-negative contributions -- returns the WORST documents as the top-k.  Found in
-- the sibling pg_fts (its 1.9.0 dense_score test); pg_weave forked before that fix.
-- The shape is pg_fts's: 'com' in every document, lengths that vary so scores vary,
-- then delete 1 in 7.
--
-- THE ORACLE is the heap-side scorer weave_bm25(d, q, N, avgdl, dfs) fed the
-- index's OWN corpus statistics (weave_index_stats, weave_index_df).  weave_bm25
-- clamps df to [1, N] (src/query/rank.c weave_idf) and always did, so with the
-- index's N and df it computes exactly what the index-side scorer must.  The
-- operator `d <=> q` is not an oracle here: without an index it scores with N = 1
-- (rank.c weave_distance), a different number.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE idf (id int PRIMARY KEY, d wdoc) WITH (autovacuum_enabled = off);
INSERT INTO idf
SELECT g, to_wdoc('simple', 'com ' || repeat('com ', g % 3) || repeat('f' || (g % 5) || ' ', g % 17))
  FROM generate_series(1, 6000) g;
CREATE INDEX idf_w ON idf USING weave (d);
VACUUM ANALYZE idf;

-- The heap oracle's top-k BM25 scores for q, with the index's N, avgdl and df.
CREATE FUNCTION pg_temp.oracle(q wquery, k int) RETURNS float8[]
LANGUAGE sql AS $$
  SELECT array_agg(round(s::numeric, 9)::float8 ORDER BY s DESC)
    FROM (SELECT weave_bm25(d, q, st.ndocs, st.avgdl, weave_index_df('idf_w', q)) AS s
            FROM idf, weave_index_stats('idf_w') st
           WHERE weave_match(d, q)
           ORDER BY 1 DESC LIMIT k) x
$$;
-- The index's top-k scores, through weave_search (which enters the scan directly).
CREATE FUNCTION pg_temp.index_scores(q wquery, k int) RETURNS float8[]
LANGUAGE sql AS $$
  SELECT array_agg(round(score::numeric, 9)::float8 ORDER BY score DESC)
    FROM weave_search('idf_w', q, k)
$$;
-- The index's top-k ids through ORDER BY d <=> q (the WAND path a query plans),
-- mapped to the ORACLE's score for each id: equal arrays mean the index returned a
-- correct top-k (ties may swap ids, never scores).
CREATE FUNCTION pg_temp.orderby_scores(q wquery, k int) RETURNS float8[]
LANGUAGE plpgsql AS $$
DECLARE r float8[];
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  PERFORM set_config('enable_sort', 'off', true);
  SELECT array_agg(round(weave_bm25(i.d, q, st.ndocs, st.avgdl, weave_index_df('idf_w', q))::numeric, 9)::float8
                   ORDER BY i.rn)
    INTO r
    FROM (SELECT d, row_number() OVER () rn
            FROM (SELECT d FROM idf WHERE d @@@ q ORDER BY d <=> q LIMIT k) s) i,
         weave_index_stats('idf_w') st;
  RETURN r;
END $$;

-- (1) before any delete
SELECT pg_temp.index_scores('com', 10) = pg_temp.oracle('com', 10) AS weave_search_is_oracle,
       pg_temp.orderby_scores('com', 10) = pg_temp.oracle('com', 10) AS orderby_is_oracle,
       pg_temp.orderby_scores('com | f1', 10) = pg_temp.oracle('com | f1', 10) AS orderby_or_is_oracle;

-- (2) delete 1 in 7: N falls to 5143, the dictionary df of 'com' stays 6000.
-- Positive control, measured 2026-10-06 against the pre-fix build (main 1621f44):
-- weave_search_scores_nonnegative = f, weave_search_is_oracle = f and
-- orderby_is_oracle = f here, all t with the clamp.  orderby_or_is_oracle stays t on
-- both builds -- 'f1' has df 1200 < N, so its idf never goes negative and the OR's
-- top-k is carried by it; it is kept as a check that the clamp changes no answer
-- where df <= N.
DELETE FROM idf WHERE id % 7 = 0;
VACUUM idf;
SELECT ndocs AS live_n, (weave_index_df('idf_w', 'com'))[1] AS dict_df_com
  FROM weave_index_stats('idf_w');
SELECT bool_and(score >= 0) AS weave_search_scores_nonnegative
  FROM weave_search('idf_w', 'com', 100);
SELECT pg_temp.index_scores('com', 10) = pg_temp.oracle('com', 10) AS weave_search_is_oracle,
       pg_temp.orderby_scores('com', 10) = pg_temp.oracle('com', 10) AS orderby_is_oracle,
       pg_temp.orderby_scores('com | f1', 10) = pg_temp.oracle('com | f1', 10) AS orderby_or_is_oracle;

-- (3) the positive control's shape, stated: with the unclamped formula 'com' would
-- have this idf, and every BM25 contribution would be negative.
SELECT round(ln(1.0 + (st.ndocs - 6000 + 0.5) / (6000 + 0.5))::numeric, 6) AS unclamped_idf_would_be,
       round(ln(1.0 + 0.5 / (st.ndocs + 0.5))::numeric, 6) AS clamped_idf
  FROM weave_index_stats('idf_w') st;

DROP TABLE idf;
RESET max_parallel_workers_per_gather;
