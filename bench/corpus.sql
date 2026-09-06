-- bench/corpus.sql -- reproducible lexical corpus with meaningful IDF.
--
-- A uniform-random corpus makes BM25 meaningless: every term has the same
-- document frequency, so IDF is constant and ranked retrieval degenerates.  This
-- generates a heavy-tailed (Zipf-ish) vocabulary so rare terms carry signal and
-- common terms do not, which is what makes the rare / mid / common latency bands
-- below actually measure different code paths.
--
-- psql vars: :ndocs :vocab
-- Adapted from pg_fts/bench/gen_corpus.sql so the numbers stay comparable with
-- that project's recorded results.

\set ON_ERROR_STOP on
\timing off

DROP TABLE IF EXISTS docs CASCADE;
CREATE TABLE docs (id bigint PRIMARY KEY, body text);

-- Sampling weight ~ 1/rank via floor(vocab * u^3): word_00001 is common,
-- word_:vocab is rare.  8..15 words per document.
INSERT INTO docs
SELECT g,
       (SELECT string_agg('word_' || lpad(
                 (floor(:vocab * power(random(), 3))::int + 1)::text, 5, '0'), ' ')
        FROM generate_series(1, 8 + (g % 8)))
FROM generate_series(1, :ndocs) g;

-- A known rare marker in 0.1% of documents, so there is a high-IDF query with a
-- ground-truth match set for recall/nDCG checks.
UPDATE docs SET body = body || ' zzqrare' WHERE id % 1000 = 0;

-- Pick the actual df bands from the generated data rather than assuming them.
-- Hardcoding a term and hoping it lands in the intended band is how a "rare
-- term" benchmark silently becomes a mid-frequency one.
DROP TABLE IF EXISTS bands;
CREATE TABLE bands (band text PRIMARY KEY, term text, df bigint);

WITH freq AS (
    SELECT w AS term, count(*) AS df
      FROM docs, unnest(string_to_array(body, ' ')) w
     WHERE w LIKE 'word_%'
     GROUP BY w
), ranked AS (
    SELECT term, df,
           percent_rank() OVER (ORDER BY df) AS pr
      FROM freq
)
INSERT INTO bands
SELECT 'rare',   term, df FROM ranked WHERE pr BETWEEN 0.10 AND 0.12
  ORDER BY df LIMIT 1;
WITH freq AS (
    SELECT w AS term, count(*) AS df
      FROM docs, unnest(string_to_array(body, ' ')) w
     WHERE w LIKE 'word_%'
     GROUP BY w
), ranked AS (
    SELECT term, df, percent_rank() OVER (ORDER BY df) AS pr FROM freq
)
INSERT INTO bands
SELECT 'mid', term, df FROM ranked WHERE pr BETWEEN 0.55 AND 0.60
  ORDER BY df LIMIT 1;
WITH freq AS (
    SELECT w AS term, count(*) AS df
      FROM docs, unnest(string_to_array(body, ' ')) w
     WHERE w LIKE 'word_%'
     GROUP BY w
)
INSERT INTO bands
SELECT 'common', term, df FROM freq ORDER BY df DESC LIMIT 1;

SELECT count(*) AS ndocs, avg(length(body))::int AS avg_bytes,
       pg_size_pretty(pg_total_relation_size('docs')) AS heap
  FROM docs;
SELECT band, term, df FROM bands ORDER BY df;
