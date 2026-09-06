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
-- NOTE: no underscore in the token.  pg_weave's analyzer splits on non-word
-- bytes, so 'word_00042' tokenizes as TWO terms ('word' and '00042') and a query
-- for it becomes an implicit AND.  The first version of this corpus used
-- underscores and every "single rare term" measurement was silently measuring a
-- two-term boolean query -- visible only in the EXPLAIN Sort Key, which is why
-- bench/lexical.sh now captures plans.
INSERT INTO docs
SELECT g,
       (SELECT string_agg('word' || lpad(
                 (floor(:vocab * power(random(), 3))::int + 1)::text, 6, '0'), ' ')
        FROM generate_series(1, 8 + (g % 8)))
FROM generate_series(1, :ndocs) g;

-- A known rare marker in 0.1% of documents, so there is a high-IDF query with a
-- ground-truth match set for recall/nDCG checks.
UPDATE docs SET body = body || ' zzqrare' WHERE id % 1000 = 0;

-- Pick the df bands from the generated data, by ABSOLUTE document frequency
-- rather than by percent_rank.
--
-- percent_rank is the wrong tool here: with a heavy-tailed vocabulary most terms
-- tie at a very low df, so the rank jumps and a window like
-- "pr BETWEEN 0.10 AND 0.12" can select nothing at all -- which it did, leaving
-- the rare band empty and every "rare term" measurement running against an empty
-- query that trivially returned 0 rows.  Absolute df targets are deterministic,
-- span four orders of magnitude, and cannot come up empty.
DROP TABLE IF EXISTS bands;
CREATE TABLE bands (band text PRIMARY KEY, term text, df bigint);

CREATE TEMP TABLE freq AS
SELECT w AS term, count(*) AS df
  FROM docs, unnest(string_to_array(body, ' ')) w
 WHERE w LIKE 'word%'
 GROUP BY w;

-- rare: nearest to 25 matching documents  (high IDF, one posting block)
INSERT INTO bands
SELECT 'rare', term, df FROM freq ORDER BY abs(df - 25), term LIMIT 1;
-- mid: nearest to 2500                     (tens of posting blocks)
INSERT INTO bands
SELECT 'mid', term, df FROM freq ORDER BY abs(df - 2500), term LIMIT 1;
-- common: the single most frequent term    (posting-scan bound; where block-max
--                                           WAND has to earn its keep)
INSERT INTO bands
SELECT 'common', term, df FROM freq ORDER BY df DESC, term LIMIT 1;

-- Fail loudly rather than benchmark an empty query.
DO $$
DECLARE n int;
BEGIN
    SELECT count(*) INTO n FROM bands WHERE term IS NOT NULL AND df > 0;
    IF n <> 3 THEN
        RAISE EXCEPTION 'band selection produced % usable bands, not 3', n;
    END IF;
END $$;

SELECT count(*) AS ndocs, avg(length(body))::int AS avg_bytes,
       pg_size_pretty(pg_total_relation_size('docs')) AS heap
  FROM docs;
SELECT band, term, df FROM bands ORDER BY df;
