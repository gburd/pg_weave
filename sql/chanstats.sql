-- Task Z4: which mechanism inside the index served the query.
--
-- WHY THIS FILE EXISTS.  pg_weave's whole claim is that several retrieval
-- structures live in ONE index (doc/ARCHITECTURE.md sect. 9, claim 1).  The cost of
-- that design is that a query leaf can be resolved by more than one of them, and
-- for most of this project's life nothing reported which.  Z4's original gate asked
-- for it in EXPLAIN; there is no AM-level EXPLAIN callback on PostgreSQL 17 or 18
-- (`amexplain` is absent from both), so the surface is a counter read from SQL --
-- which has the advantage of covering every plan shape, including the bitmap scan
-- that is the common @@@ path and the only path a CustomScan would not see.
--
-- HOW TO READ A ZERO HERE.  The counters are BACKEND-LOCAL, so a zero means "this
-- mechanism served nothing in this backend", never "nothing happened".  Every
-- assertion below resets, queries and reads in one session for that reason; a shell
-- loop of `psql -c` would report zeros no matter what the index did.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
SET enable_seqscan = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE cs (id serial, body text, d wdoc, v wvec(4));
INSERT INTO cs(body)
  SELECT (ARRAY['alpha beta gamma','alphabet soup','alpine air',
                'beta carotene','gamma ray'])[1 + g % 5] || ' doc' || g
    FROM generate_series(1, 500) g;
UPDATE cs SET d = to_wdoc('simple', body),
              v = ('[' || (id % 7) || ',' || (id % 5) || ',' || (id % 3) || ','
                        || (id % 2) || ']')::wvec;
CREATE INDEX cs_idx ON cs USING weave (d, v);
ANALYZE cs;

-- ---- an exact term is served by the dictionary ---------------------------
SELECT weave_channel_stats_reset();
SELECT count(*) FROM cs WHERE d @@@ 'beta'::wquery;
SELECT lex_term > 0 AS exact_term_used_the_dictionary,
       prefix_dict AS prefix_walks,
       terms_expanded AS expanded
  FROM weave_channel_stats();

-- ---- a prefix is served by the dictionary RANGE WALK, not by the trie ----
-- This is the measurement Z4 turns on, stated as an assertion: today `alpha*` is
-- resolved by seeking the sparse dictionary index and scanning the matching range,
-- and the trie is not consulted at all.  `alpha*` matches `alpha` and `alphabet`
-- but not `alpine`, so the expansion count is a fact about the vocabulary rather
-- than about the corpus size.
SELECT weave_channel_stats_reset();
SELECT count(*) FROM cs WHERE d @@@ 'alpha*'::wquery;
SELECT prefix_dict > 0 AS prefix_used_the_dictionary_walk,
       prefix_surf = 0 AS trie_was_not_consulted,
       surf_loads = 0 AS and_no_trie_image_was_loaded,
       terms_expanded AS vocabulary_terms_matched,
       dict_pages > 0 AS dictionary_pages_were_read
  FROM weave_channel_stats();

-- ---- the vector channel counts its own mechanism -------------------------
-- weave_vec_scan() is the direct door to the shuttle (sql/vecscan.sql explains why
-- the vector channel has to be reached without the planner).  One increment per
-- bolt scanned, the same unit the lexical counters use.
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS scan_returned_rows
  FROM weave_vec_scan('cs_idx', '[1,2,3,4]'::wvec, 5);
SELECT vector_scan > 0 AS vector_shuttle_was_opened,
       lex_term = 0 AS and_no_lexical_leaf_ran
  FROM weave_channel_stats();

-- ---- the trie load is visible, and it is a WHOLE-IMAGE load --------------
-- weave_surf_stats() is the only SQL-reachable caller of weave_surf_load().  The
-- point of asserting it here is that surf_bytes is the per-call cost of consulting
-- the trie at query time: weave_read_surf() reassembles the entire image into one
-- contiguous palloc with no cache, so any route that consults the trie pays this
-- once per bolt per query.  Z4's re-aim rests on that number being visible.
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS index_has_a_trie FROM weave_surf_stats('cs_idx');
SELECT surf_loads > 0 AS trie_image_was_loaded,
       surf_bytes > 0 AS and_its_size_is_reported
  FROM weave_channel_stats();

-- ---- the channels that answer nothing say so -----------------------------
-- NOT filler.  Four of the six retrieval kinds are imported but unwired, and this
-- is where that stops being a sentence in doc/PRODUCTION_READINESS.md.  Each zero
-- is pinned to the task that will flip it, so routing a channel shows up here as a
-- diff rather than as nothing:
--   prefix_surf  -- Z4, and only if the measurement says the trie beats the walk
--   fuzzy_dict   -- Z5, routing uleven over the dictionary's vocabulary iterator
--   fuzzy_surf   -- Z5 plus a resident trie
--   regex_dict   -- Z6
--   regex_surf   -- Z6, via trigram tiling
SELECT weave_channel_stats_reset();
SELECT count(*) FROM cs WHERE d @@@ 'beta'::wquery;
SELECT count(*) FROM cs WHERE d @@@ 'alpha*'::wquery;
SELECT prefix_surf, fuzzy_dict, fuzzy_surf, regex_dict, regex_surf
  FROM weave_channel_stats();

-- ---- reset really resets -------------------------------------------------
-- A counter surface whose reset does not work turns every later measurement into
-- "everything since connect", which is how a bracketed number becomes a wrong one.
SELECT weave_channel_stats_reset();
SELECT lex_term, prefix_dict, vector_scan, terms_expanded, dict_pages,
       surf_loads, surf_bytes
  FROM weave_channel_stats();

DROP TABLE cs;
