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
-- weave_surf_stats() is the only SQL-reachable caller of the trie loader.  The
-- point of asserting it here is that surf_bytes is the per-call cost of consulting
-- the trie with no cache: weave_read_surf() reassembles the entire image into one
-- contiguous palloc, so any route that consults the trie pays this once per bolt
-- per query.  Z4's re-aim rests on that number being visible.
--
-- surf_cache_mb = 0 PINS THAT BEHAVIOUR HERE, which is the point of the setting
-- existing: the cache added by Z4 part 2 is on by default, so without this the
-- section below would be measuring a cache hit and calling it a load.  It also
-- leaves nothing resident, so the resident-trie section that follows starts cold.
SET pg_weave.surf_cache_mb = 0;
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS index_has_a_trie FROM weave_surf_stats('cs_idx');
SELECT surf_loads > 0 AS trie_image_was_loaded,
       surf_bytes > 0 AS and_its_size_is_reported
  FROM weave_channel_stats();

-- ---- fuzzy is served by the Levenshtein automaton over the dictionary ----
-- CORRECTING THE FIRST DRAFT OF THIS FILE, which asserted fuzzy_dict = 0 and
-- described the fuzzy channel as unwired.  It is not: weave_fuzzy_terms() walks the
-- sorted dictionary under a Levenshtein automaton, skipping every term that shares a
-- dead-end prefix, and the result is EXACT -- no heap recheck, unlike the funnel.
-- The zero was an artifact of the counter not existing, which is precisely the
-- failure these counters are supposed to prevent, so it is asserted the other way
-- round here.
SELECT weave_channel_stats_reset();
SELECT count(*) FROM cs WHERE d @@@ 'gamma~1'::wquery;
SELECT fuzzy_dict > 0 AS fuzzy_used_the_automaton_walk,
       fuzzy_trgm = 0 AS and_not_the_trigram_funnel,
       fuzzy_surf = 0 AS and_not_the_trie
  FROM weave_channel_stats();

-- ---- regex is served by core's engine over the dictionary (Z6) -----------
-- The pattern is compiled once and run over the segment's dictionary terms, so
-- regex_dict is the counter that says the leaf went through the index at all.  This
-- index has no trigram weft (the reloption defaults off), so regex_trgm = 0 here is
-- a statement about the RELOPTION and nothing more; sql/regexdict.sql builds the
-- weft and asserts the narrowing both ways, with a positive control.  The pattern is
-- a class shape on purpose: it has no literal run of three, which is the shape the
-- old funnel could not serve at all.
SELECT weave_channel_stats_reset();
SELECT count(*) FROM cs WHERE d @@@ '/gam[a-z]a/'::wquery;
SELECT regex_dict > 0 AS regex_used_the_dictionary_walk,
       regex_trgm = 0 AS no_weft_so_nothing_narrowed,
       regex_surf = 0 AS and_not_the_trie,
       terms_expanded AS vocabulary_terms_matched
  FROM weave_channel_stats();

-- ---- what is genuinely absent: a shuttle with a bound --------------------
-- These three columns ARE structurally zero, and each is pinned to the task that
-- will flip it, so routing a channel shows up here as a diff rather than as nothing:
--   prefix_surf  -- Z4 part 3, and only if the measurement says the trie beats the
--                   dictionary range walk
--   fuzzy_surf   -- Z5 plus Z4's resident trie
--   regex_surf   -- a regex route through the trie, if one is ever measured to win
--                   over the dictionary walk that Z6 wired
-- Note what these counters do NOT say.  A nonzero fuzzy_dict above means fuzzy
-- returns correct rows, not that the fuzzy CHANNEL exists: there is no shuttle
-- implementing include/weave/channel.h with a real bound, so fuzzy cannot yet join a
-- fused top-k.  That distinction is the whole of Z7.
SELECT weave_channel_stats_reset();
SELECT count(*) FROM cs WHERE d @@@ 'beta'::wquery;
SELECT count(*) FROM cs WHERE d @@@ 'alpha*'::wquery;
SELECT count(*) FROM cs WHERE d @@@ 'gamma~1'::wquery;
SELECT prefix_surf, fuzzy_surf, regex_surf
  FROM weave_channel_stats();

-- ==== Z4 part 2: the trie is RESIDENT ====================================
--
-- WHY THESE ASSERTIONS ARE THE MEASUREMENT.  A trie image is ~5.52 bytes per
-- vocabulary term -- about 11 MB per bolt at 2M terms -- and before this task every
-- consult reassembled the whole thing from its page chain.  Nothing in production
-- paid it per query (weave_surf_stats() was the only SQL-reachable caller), but
-- every remaining Z task consults the trie at QUERY time, and none of them is
-- measurable while one consult costs a whole image.  So the claim to pin down is
-- exactly: a repeat consult loads NOTHING.  surf_loads/surf_bytes still count
-- whole-image loads only; surf_cache_hits counts consults served from memory.  If a
-- hit ever incremented surf_loads the two numbers would say the same thing and the
-- measurement would be gone, which is why every assertion below pairs them.
--
-- EVERYTHING IS IN ONE SESSION because the counters are backend-local, and the
-- cache is too -- it lives in a MemoryContext under TopMemoryContext, keyed by
-- (relfilenode, weft root block, metapage generation).
--
-- The bolt count is taken while the cache is still OFF from the section above, on
-- purpose: counting the bolts is itself a consult, and taking it with the cache on
-- would leave the image resident and make the "first consult" below a hit.  That is
-- not a hypothetical -- the first draft of this section did exactly that and
-- asserted a miss that was a hit.
CREATE TEMP TABLE cs_bolts AS
  SELECT count(*)::bigint AS n FROM weave_surf_stats('cs_idx');
SELECT n AS bolts_with_a_trie FROM cs_bolts;
SET pg_weave.surf_cache_mb = 32;

-- ---- the first consult is a miss, and it pays for a whole image ----------
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS index_has_a_trie FROM weave_surf_stats('cs_idx');
CREATE TEMP TABLE cs_cold AS SELECT * FROM weave_channel_stats();
SELECT surf_cache_misses = (SELECT n FROM cs_bolts) AS one_miss_per_bolt,
       surf_cache_hits = 0 AS and_no_hit,
       surf_loads = surf_cache_misses AS one_whole_image_load_per_miss,
       surf_cache_bytes = surf_bytes AS the_loaded_image_is_now_resident
  FROM cs_cold;

-- ---- the second consult of the same bolt loads NOTHING -------------------
-- The bolt is immutable and the generation has not moved, so the key hits and the
-- page chain is never touched.  surf_loads = 0 here is the whole of Z4 part 2.
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS index_has_a_trie FROM weave_surf_stats('cs_idx');
SELECT surf_cache_hits = (SELECT n FROM cs_bolts) AS one_hit_per_bolt,
       surf_cache_misses = 0 AS and_no_miss,
       surf_loads = 0 AS and_no_whole_image_load,
       surf_bytes = 0 AS and_no_bytes_read,
       surf_cache_bytes = (SELECT surf_cache_bytes FROM cs_cold)
         AS resident_bytes_unchanged
  FROM weave_channel_stats();

-- ---- surf_cache_mb = 0 restores the old behaviour exactly ----------------
-- The control arm.  With the cache off there are no cache events at all -- not
-- hits, and not misses either, because there is no cache to miss -- and every
-- consult is a load.  Turning it off also hands the resident bytes back, which is
-- the only way this file can watch the gauge go DOWN: an eviction that does not
-- decrement it would leave a permanent overcount.
SET pg_weave.surf_cache_mb = 0;
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS index_has_a_trie FROM weave_surf_stats('cs_idx');
SELECT count(*) > 0 AS index_has_a_trie FROM weave_surf_stats('cs_idx');
SELECT surf_cache_hits = 0 AS no_hits_with_the_cache_off,
       surf_cache_misses = 0 AS and_no_misses_there_is_no_cache,
       surf_loads = 2 * (SELECT n FROM cs_bolts) AS every_consult_was_a_load,
       surf_cache_evicts > 0 AS the_resident_images_were_given_back,
       surf_cache_bytes = 0 AS and_the_resident_gauge_is_back_to_zero
  FROM weave_channel_stats();

-- ---- an image larger than the whole budget is not cached -----------------
-- 250k distinct vocabulary terms, which at 5.52 B/term is an image over 1 MB, so a
-- 1 MB budget cannot hold it.  Caching it would mean evicting everything else to
-- hold something that has to be dropped again on the next consult, so the loader
-- hands it straight to the caller instead.  From here that reads as misses climbing
-- while the resident gauge stays at zero -- and, load-bearing, as NOT AN ERROR: the
-- refusal must not wedge the consult.
CREATE TABLE csb (id serial, d wdoc);
INSERT INTO csb(d)
  SELECT to_wdoc('simple', (SELECT string_agg('w' || (g * 100 + i), ' ')
                              FROM generate_series(1, 100) i))
    FROM generate_series(1, 2500) g;
CREATE INDEX csb_idx ON csb USING weave (d);
SELECT count(*) = 1 AS one_bolt,
       max(bytes) > 1024 * 1024 AS its_image_exceeds_one_megabyte
  FROM weave_surf_stats('csb_idx');
SET pg_weave.surf_cache_mb = 1;
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS consulted FROM weave_surf_stats('csb_idx');
SELECT count(*) > 0 AS consulted_again FROM weave_surf_stats('csb_idx');
SELECT surf_cache_hits = 0 AS the_oversized_image_was_never_served_from_cache,
       surf_cache_misses = 2 AS both_consults_missed,
       surf_loads = 2 AS and_both_loaded_the_whole_image,
       surf_cache_bytes = 0 AS and_nothing_became_resident
  FROM weave_channel_stats();

-- The same image under a budget that fits IS cached, which is what makes the
-- refusal above a statement about the budget rather than about the image.
SET pg_weave.surf_cache_mb = 32;
SELECT weave_channel_stats_reset();
SELECT count(*) > 0 AS consulted FROM weave_surf_stats('csb_idx');
SELECT count(*) > 0 AS consulted_again FROM weave_surf_stats('csb_idx');
SELECT surf_cache_hits = 1 AS the_second_consult_hit,
       surf_loads = 1 AS and_only_the_first_one_loaded,
       surf_cache_bytes > 1024 * 1024 AS the_big_image_is_resident
  FROM weave_channel_stats();

-- ---- a directory change makes a resident image unusable ------------------
-- THE CORRECTNESS ASSERTION THAT MATTERS MOST.  A bolt is immutable once written,
-- so a cached image never goes stale in place -- but freed pages ARE recycled, and
-- a root block number can later belong to a DIFFERENT bolt.  This sequence
-- manufactures exactly that: build a bolt (503 terms), add documents carrying three
-- new terms, weave_merge() to fold them into a new bolt, then weave_vacuum(), whose
-- pack phase relocates the surviving bolt onto the low free blocks the merge
-- released -- putting the new trie on the SAME root block the first one occupied.
-- A cache keyed only by (relfilenode, root) would serve the pre-merge image here and
-- report 503 terms for a 506-term vocabulary: a wrong answer, silently.  The
-- metapage `generation` is the third key part precisely because every path that
-- frees a bolt's pages bumps it first, in the same commit.
CREATE TABLE csg (id serial, d wdoc);
INSERT INTO csg(d) SELECT to_wdoc('simple', 'alpha beta gamma doc' || g)
  FROM generate_series(1, 500) g;
CREATE INDEX csg_idx ON csg USING weave (d);
SELECT nterms AS terms_before FROM weave_surf_stats('csg_idx');
INSERT INTO csg(d) SELECT to_wdoc('simple', 'zeta eta theta doc' || g)
  FROM generate_series(1, 200) g;
SELECT weave_merge('csg_idx');
SELECT weave_vacuum('csg_idx');
SELECT weave_channel_stats_reset();
SELECT nterms AS terms_after FROM weave_surf_stats('csg_idx');
SELECT surf_cache_hits = 0 AS the_pre_merge_image_was_not_served,
       surf_loads > 0 AS the_new_bolt_was_loaded_from_its_pages
  FROM weave_channel_stats();

-- ---- reset really resets -------------------------------------------------
-- A counter surface whose reset does not work turns every later measurement into
-- "everything since connect", which is how a bracketed number becomes a wrong one.
--
-- surf_cache_bytes is EXCLUDED from that, deliberately, and asserted the other way
-- round: it is a gauge of memory this backend is still holding, so zeroing it on
-- reset would report zero resident bytes while the images are resident -- the same
-- false zero the first draft of this file shipped for fuzzy_dict.
SELECT weave_channel_stats_reset();
SELECT lex_term, prefix_dict, fuzzy_dict, fuzzy_trgm, regex_dict, regex_trgm,
       vector_scan, terms_expanded, dict_pages, surf_loads, surf_bytes,
       surf_cache_hits, surf_cache_misses, surf_cache_evicts
  FROM weave_channel_stats();
SELECT surf_cache_bytes > 0 AS the_resident_gauge_survives_a_reset
  FROM weave_channel_stats();

RESET pg_weave.surf_cache_mb;
DROP TABLE cs;
DROP TABLE csb;
DROP TABLE csg;

-- ---- G48: the lexical channel's page traffic, split by what the page was for ----
--
-- doc/GAPS.md G48.  wand_skip_blocks() advances past whole 128-blocks reading only
-- block HEADERS -- cheap in CPU, which is what its header claims and what makes a seek
-- over a high-df term fast -- but it VISITS every page it passes over, because the
-- per-block first_docid and block_max it consults live on those pages.  The vector
-- channel has had this split since 0.19.0 (vec_blocks vs vec_blocks_bound_skipped); the
-- lexical channel had nothing, so its share of a gated query's buffers was an inference
-- from an EXPLAIN total.
--
-- THIS SECTION IS THE COUNTERS' POSITIVE CONTROL, and it is the reason the fixture below
-- is 4,000 rows rather than reusing `cs`.  A counter that has never been observed to move
-- is indistinguishable from one wired to the wrong branch, and skipping only happens when
-- a seek jumps a cursor forward past a WHOLE block -- so it needs a term whose posting
-- list spans many 128-blocks (here: every row) alongside a rare one to seek toward.  With
-- `cs`'s 500 rows the busiest term spans two blocks on a single page and the control
-- would be asserting on a single visit.
CREATE TABLE csp (id serial, d wdoc);
INSERT INTO csp(d)
  SELECT to_wdoc('simple', 'common filler text doc' || g
                 || CASE WHEN g % 1000 = 0 THEN ' needle' ELSE '' END)
    FROM generate_series(1, 4000) g;
CREATE INDEX csp_idx ON csp USING weave (d);
ANALYZE csp;

-- THE QUERY SHAPE IS LOAD-BEARING, AND THE FIRST ONE TRIED HERE MEASURED NOTHING.
-- `SELECT count(*) ... WHERE d @@@ 'common & needle'` returns the right answer (4) with
-- BOTH counters at ZERO: that path never opens a WandCursor at all, so it cannot touch
-- either primitive.  Had the control been written as "the counters are non-negative" it
-- would have passed, and the counters would have shipped wired to a branch nothing in the
-- suite reaches -- which is the trap AGENTS.md records twice ("writing SQL that reaches a
-- specific C function is not the same as writing SQL that returns the right answer").
--
-- The RANKED path is the one that traverses postings through the cursor: it opens one
-- WandCursor per term and drives them with wand_seek(), which is where a pivot moves a
-- lagging cursor forward past whole blocks.  weave_search() enters that machinery
-- directly, which is also why it is the only thing that exercises the scan-side recheck.
SELECT weave_work_stats_reset();
SELECT count(*) FROM weave_search('csp_idx', 'common & needle'::wquery, 10);
SELECT lex_pages_load > 0 AS decoding_visits_pages
  FROM weave_work_stats();

-- lex_pages_skip IS ZERO HERE AND THAT IS RECORDED RATHER THAN ASSERTED AWAY.
--
-- wand_skip_blocks() -- the primitive lex_pages_skip counts -- is reached only from
-- wand_seek(), and only when the seek target lies beyond the cursor's whole current
-- 128-block.  Seven query shapes were tried against this fixture looking for one that
-- reaches it (conjunctions of a common term with a rare one, two rare terms at opposite
-- ends of the docid space, ranked disjunctions at k = 1, 2, 3 and 10, and an ORDER BY with
-- a WHERE): every one of them streamed the posting lists through wand_load_block and
-- NONE of them called wand_skip_blocks at all.  At 4,000 dense rows the busiest term's
-- 128th posting is at docid 128, so the traversals this corpus can produce never leave a
-- block behind unread.
--
-- SO THIS COUNTER HAS NO POSITIVE CONTROL YET, AND UNTIL IT HAS ONE ITS ZERO MEANS
-- NOTHING (AGENTS.md, twelfth member).  The evidence that the path is reachable at all is
-- doc/GAPS.md G43, a real BEIR corpus where a term with df = 211 was sought to docid
-- 184,499 -- three orders of magnitude past its own block.  bench/gatesweep.sh records
-- both counters on those corpora, and G48 is where the measurement lands.  A zero below
-- is therefore a statement about this fixture, not about the channel.
SELECT lex_pages_skip AS skip_not_reachable_at_this_scale FROM weave_work_stats();

-- And the ordinary bitmap path, recorded as a ZERO on purpose: a reader who sees zeros
-- after a plain `@@@` query needs to know that is the path not using the cursor rather
-- than the counters being broken.  If this ever becomes non-zero the bitmap path has
-- started traversing postings through the cursor, which would be a real change in what
-- these numbers mean.
SELECT weave_work_stats_reset();
SELECT count(*) FROM csp WHERE d @@@ 'common & needle'::wquery;
SELECT lex_pages_skip AS bitmap_path_skip, lex_pages_load AS bitmap_path_load
  FROM weave_work_stats();

-- The ratio is corpus-dependent and is NOT asserted here -- it is what
-- bench/gatesweep.sh measures on a real corpus, and pinning it in an expected file would
-- turn a measurement into a constant.  What is asserted is that both branches are
-- reachable and that the reset works, because everything downstream of these counters
-- depends on those two facts and on nothing else about their values.
SELECT weave_work_stats_reset();
SELECT lex_pages_skip AS skip_after_reset, lex_pages_load AS load_after_reset
  FROM weave_work_stats();

DROP TABLE csp;
