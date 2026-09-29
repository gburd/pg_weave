-- L7: the keyless ordering scan.
--
-- `ORDER BY d <=> q LIMIT k` with NO `WHERE` clause is the pgvector idiom
-- (`ORDER BY embedding <=> $1 LIMIT 10`) and therefore the first form any user
-- writes.  Measured on 1M documents, pg_weave answered it with a Seq Scan plus a
-- top-N Sort evaluating <=> on every row: 83 ms with 4 parallel workers, 362 ms
-- serial, against 0.05 ms for the same intent through the WHERE-qualified form.
-- See bench/RESULTS_LEXICAL.md and doc/GAPS.md G1.
--
-- Correct results, a 7,000x cliff, and no diagnostic is the worst failure mode a
-- database feature can have.  This file asserts the plan, because a test that
-- only checks rows passes just as happily on the seq-scan path -- which is
-- exactly how the regression suite missed this for the whole life of the fork.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

CREATE TABLE ob (id serial, d wdoc);
-- A vocabulary with three frequency bands so ranking is meaningful and the
-- top-k is not an arbitrary tie-break.
INSERT INTO ob(d) SELECT to_wdoc('alpha rare' || g)        FROM generate_series(1, 5) g;
INSERT INTO ob(d) SELECT to_wdoc('alpha beta mid' || g)    FROM generate_series(1, 50) g;
INSERT INTO ob(d) SELECT to_wdoc('beta gamma common' || g) FROM generate_series(1, 500) g;
CREATE INDEX ob_weave ON ob USING weave (d);
ANALYZE ob;

-- enable_seqscan=off is how we test PATH GENERATION rather than cost: if the AM
-- can produce an ordering path the planner will take it, and if it cannot the
-- planner falls back to Seq Scan even at disable_cost.  So a Seq Scan here means
-- "no index path exists", not "the index looked expensive".
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = on;

-- (1) The supported form: WHERE alongside ORDER BY.  Must be an ordering Index
-- Scan with no Sort node.  This already worked.
EXPLAIN (COSTS OFF)
SELECT id FROM ob WHERE d @@@ 'alpha'::wquery
 ORDER BY d <=> 'alpha'::wquery LIMIT 5;

-- (2) THE TARGET: the bare form, no WHERE.  Must also be an ordering Index Scan
-- with no Sort.  Before L7 this was `Sort -> Seq Scan`.
EXPLAIN (COSTS OFF)
SELECT id FROM ob ORDER BY d <=> 'alpha'::wquery LIMIT 5;

-- (3) The bare form must return the SAME rows in the SAME order as the
-- WHERE-qualified form, for a query whose match set exceeds the limit.  If the
-- keyless path ranked a different candidate set, this would differ.
SELECT count(*) AS bare_matches_qualified
  FROM (SELECT id FROM ob ORDER BY d <=> 'alpha'::wquery LIMIT 5) a
  JOIN (SELECT id FROM ob WHERE d @@@ 'alpha'::wquery
         ORDER BY d <=> 'alpha'::wquery LIMIT 5) b USING (id);

-- (4) Every row a keyless ordering scan returns must satisfy @@@.  A ranked scan
-- that returns non-matching rows is the bug class the WHERE-qualified path
-- already guards against (sql/weave.sql, boolean-structure tests); the keyless
-- path must not reintroduce it.
SELECT count(*) AS bare_nonmatching
  FROM (SELECT d FROM ob ORDER BY d <=> 'alpha'::wquery LIMIT 20) s
 WHERE NOT (s.d @@@ 'alpha'::wquery);

-- (5) LIMIT larger than the match set.  RETRACTED CONTRACT (doc/GAPS.md G56,
-- 2026-09-29): this test used to assert that the scan returns exactly the 55
-- matches and does NOT pad.  But SQL's ORDER BY never filters -- a heap scan +
-- sort of the same query returns 500 rows, the non-matches at distance 1 --
-- so returning 55 was an incomplete answer, and a WHERE clause beside the
-- ordering (price < c ORDER BY d <=> q) inherited it.  The scan now emits its
-- ranked matches FIRST, then the rest of the table at distance 1.  All three
-- halves are asserted: the count, that the first 55 rows are the matches, and
-- that nothing after them matches.
CREATE TEMP TABLE ob5 AS
  SELECT row_number() OVER () AS pos, id, d
    FROM (SELECT id, d FROM ob ORDER BY d <=> 'alpha'::wquery LIMIT 500) s;
SELECT count(*) AS bare_rows,                                          -- 500
       count(*) FILTER (WHERE pos <= 55 AND NOT d @@@ 'alpha'::wquery)
         AS ranked_nonmatching,                                        -- 0
       count(*) FILTER (WHERE pos > 55 AND d @@@ 'alpha'::wquery)
         AS padded_matching                                            -- 0
  FROM ob5;
DROP TABLE ob5;

-- (6) Boolean structure must be honoured without a WHERE clause too: the
-- RANKED rows are exactly the query's matches.  'alpha & beta' has 50 matches,
-- so LIMIT 20 is all ranked; 'alpha & !beta' has 5, so the other 15 rows are
-- padding (G56) and must come after all 5.
SELECT count(*) AS bare_and_nonmatching
  FROM (SELECT d FROM ob ORDER BY d <=> 'alpha & beta'::wquery LIMIT 20) s
 WHERE NOT (s.d @@@ 'alpha & beta'::wquery);
SELECT count(*) AS bare_not_rows,                                      -- 20
       count(*) FILTER (WHERE pos <= 5 AND NOT d @@@ 'alpha & !beta'::wquery)
         AS bare_not_ranked_nonmatching,                               -- 0
       count(*) FILTER (WHERE pos > 5 AND d @@@ 'alpha & !beta'::wquery)
         AS bare_not_padded_matching                                   -- 0
  FROM (SELECT row_number() OVER () AS pos, d
          FROM (SELECT d FROM ob ORDER BY d <=> 'alpha & !beta'::wquery
                 LIMIT 20) s) t;

-- (7) A query matching nothing ranks nothing, so every row is padding.  This
-- used to assert 0 rows ("not the whole table ordered by an arbitrary score");
-- the whole table is exactly what a heap sort returns (G56).  None of the
-- rows may match.
SELECT count(*) AS bare_nomatch,                                       -- 10
       count(*) FILTER (WHERE d @@@ 'zzznotpresent'::wquery) AS bare_nomatch_matching
  FROM (SELECT d FROM ob ORDER BY d <=> 'zzznotpresent'::wquery LIMIT 10) s;

-- (8) Scores must be identical between the two forms.  A keyless scan that
-- computed BM25 against different corpus statistics would rank plausibly and
-- wrongly.
SELECT count(*) AS score_disagreements FROM (
  SELECT id, round((d <=> 'alpha'::wquery)::numeric, 9) AS s
    FROM ob WHERE d @@@ 'alpha'::wquery
   ORDER BY d <=> 'alpha'::wquery LIMIT 5) q
  JOIN (
  SELECT id, round((d <=> 'alpha'::wquery)::numeric, 9) AS s
    FROM ob ORDER BY d <=> 'alpha'::wquery LIMIT 5) b
  USING (id)
 WHERE q.s IS DISTINCT FROM b.s;

RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

-- (9) With the seq-scan path available, the planner must still CHOOSE the index
-- for the bare form on a table where the index is the better plan.  Path
-- generation alone is not enough: doc/GAPS.md G10 notes an uncalibrated cost
-- model silently loses the index, which is how this gap stayed hidden.
EXPLAIN (COSTS OFF)
SELECT id FROM ob ORDER BY d <=> 'alpha'::wquery LIMIT 5;

DROP TABLE ob;

-- ============================================================================
-- L14: incremental WAND growth (doc/GAPS.md G13, bench/RESULTS_WAND_K.md).
--
-- PostgreSQL gives an access method no way to learn the query's LIMIT, so the
-- ranked scan runs a block-max WAND pass at some candidate width and widens it
-- when the executor asks for more rows than the pass materialized.  L14 makes a
-- widening EXTEND the previous pass: the already-materialized visible rows are
-- kept and never re-emitted, and the new pass's candidates are filtered against
-- them.  The property that could break is exactly the one this section asserts:
-- a deeply paginated scan must return the SAME rows in the SAME order, with no
-- duplicate and no omission, as one wide pass.
--
-- pg_weave.wand_initial_k is a PGC_USERSET GUC, so the growth ladder is
-- reachable from SQL: a small value forces several widenings for a deep scan and
-- a large value serves the same scan in one pass.  Both must agree exactly.
CREATE TABLE wg (id int, d wdoc);
-- Unique tf per doc gives strictly distinct BM25 scores, so the ranking is a
-- total order: a duplicated or dropped row shows up as a difference rather than
-- disappearing into a tie shuffle.  Every 4th doc also carries beta, so the
-- boolean gates (AND / NOT) are exercised across a growth too.
INSERT INTO wg(id, d)
SELECT g, to_wdoc(repeat('alpha ', g) ||
       (CASE WHEN g % 4 = 0 THEN 'beta ' ELSE '' END))
FROM generate_series(1, 400) g;
CREATE INDEX wg_weave ON wg USING weave (d);
ANALYZE wg;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- The assertions below are worthless unless the ranked ordering INDEX path is
-- what runs them: a Sort over a bitmap scan would agree with itself no matter
-- what the AM did.
EXPLAIN (COSTS OFF)
SELECT id FROM wg WHERE d @@@ 'alpha'::wquery
 ORDER BY d <=> 'alpha'::wquery LIMIT 300;

-- (10) Deep pagination == one wide pass.  wand_initial_k=1 gives the minimum
-- candidate width, so LIMIT 300 cannot be served without widening; =400 serves
-- it in a single pass.
SET pg_weave.wand_initial_k = 1;
CREATE TEMP TABLE wg_grown AS
  SELECT row_number() OVER () AS pos, id,
         round((d <=> 'alpha'::wquery)::numeric, 9) AS dist
    FROM (SELECT id, d FROM wg WHERE d @@@ 'alpha'::wquery
           ORDER BY d <=> 'alpha'::wquery LIMIT 300) s;
SET pg_weave.wand_initial_k = 400;
CREATE TEMP TABLE wg_onepass AS
  SELECT row_number() OVER () AS pos, id,
         round((d <=> 'alpha'::wquery)::numeric, 9) AS dist
    FROM (SELECT id, d FROM wg WHERE d @@@ 'alpha'::wquery
           ORDER BY d <=> 'alpha'::wquery LIMIT 300) s;
SELECT count(*) AS grown_rows FROM wg_grown;                 -- 300
SELECT count(*) AS pagination_disagreements
  FROM wg_grown g FULL JOIN wg_onepass o USING (pos)
 WHERE g.id IS DISTINCT FROM o.id OR g.dist IS DISTINCT FROM o.dist;   -- 0
SELECT count(*) AS grown_duplicates
  FROM (SELECT id FROM wg_grown GROUP BY id HAVING count(*) > 1) x;    -- 0
-- The rows a deeply grown scan returns must be the TRUE top-300: exact BM25
-- (with the real corpus stats, the oracle form sql/weave.sql uses) may not put
-- any returned row more than 1% below the exact 300th-best score.  A widening
-- that lost or replaced a row shows up here; the 1% slack is the documented v4
-- doclen-quantization tolerance, not slack in the growth logic.
--
-- Note this cannot be done with the `d <=> q` in a target list: evaluated as a
-- plain expression it has no index behind it and therefore no ndocs/avgdl/df, so
-- it is NOT the index's ordering score and is not monotone in the index's order.
-- It is still a deterministic function of the row, which is what makes it a
-- valid column to compare BETWEEN the two executions above.
WITH st AS (SELECT ndocs, avgdl FROM weave_index_stats('wg_weave')),
     dfs AS (SELECT weave_index_df('wg_weave', 'alpha'::wquery) AS df),
     ix AS (SELECT weave_bm25(w.d, 'alpha'::wquery, st.ndocs, st.avgdl, dfs.df) AS sc
              FROM wg_grown g JOIN wg w USING (id), st, dfs),
     cut AS (SELECT min(sc) AS kth FROM (
              SELECT weave_bm25(w.d, 'alpha'::wquery, st.ndocs, st.avgdl, dfs.df) AS sc
                FROM wg w, st, dfs WHERE w.d @@@ 'alpha'::wquery
               ORDER BY sc DESC LIMIT 300) z)
SELECT count(*) AS grown_recall_misses
  FROM ix, cut WHERE ix.sc < cut.kth * 0.99 - 1e-9;                    -- 0

-- (11) No self-imposed ceiling.  An amcanorderbyop scan must be able to return
-- EVERY match in score order; the executor's LIMIT is what bounds it.  A
-- growth ladder that stops early silently truncates -- the bug the growth block
-- in amscan.c warns about.  All three forms must see all 400 alpha docs at the
-- smallest possible initial k.
SET pg_weave.wand_initial_k = 1;
SELECT count(*) AS all_matches_big_limit
  FROM (SELECT id FROM wg WHERE d @@@ 'alpha'::wquery
         ORDER BY d <=> 'alpha'::wquery LIMIT 5000) s;                 -- 400
SELECT count(*) AS all_matches_no_limit
  FROM (SELECT id FROM wg WHERE d @@@ 'alpha'::wquery
         ORDER BY d <=> 'alpha'::wquery) s;                            -- 400
SELECT count(*) AS all_matches_bare
  FROM (SELECT id FROM wg ORDER BY d <=> 'alpha'::wquery LIMIT 5000) s;-- 400

-- (12) Incremental demand through a CURSOR.  Fetching one row at a time makes
-- every exhaustion of the materialized depth a widening MID-SCAN, which is the
-- path a LIMIT cannot reach; the concatenated result must equal a single
-- execution of the same query, row for row, and cover all 400 matches.  (A
-- bare `MOVE FORWARD ALL` would not do: pg_regress runs psql quietly, so the
-- row count in the command tag never reaches the output.)
CREATE FUNCTION wg_cursor_walk() RETURNS TABLE (nrows int, mismatches int)
LANGUAGE plpgsql AS $$
DECLARE
  c CURSOR FOR SELECT id FROM wg WHERE d @@@ 'alpha'::wquery
                ORDER BY d <=> 'alpha'::wquery;
  got int[] := '{}';
  one int[];
  r record;
BEGIN
  OPEN c;
  LOOP
    FETCH c INTO r;
    EXIT WHEN NOT FOUND;
    got := got || r.id;
  END LOOP;
  CLOSE c;
  SELECT array_agg(id) INTO one FROM (
    SELECT id FROM wg WHERE d @@@ 'alpha'::wquery
     ORDER BY d <=> 'alpha'::wquery LIMIT 5000) s;
  nrows := coalesce(array_length(got, 1), 0);
  SELECT count(*) INTO mismatches
    FROM generate_series(1, greatest(coalesce(array_length(got, 1), 0),
                                     coalesce(array_length(one, 1), 0))) g
   WHERE got[g] IS DISTINCT FROM one[g];
  RETURN NEXT;
END $$;
SELECT * FROM wg_cursor_walk();                                        -- 400, 0
DROP FUNCTION wg_cursor_walk();

-- (13) Scores must not depend on how deep the scan went: the rows a LIMIT 10
-- and a LIMIT 300 execution share must carry identical distances and identical
-- positions.
SELECT count(*) AS shallow_deep_score_disagreements FROM (
  SELECT id, round((d <=> 'alpha'::wquery)::numeric, 9) AS s
    FROM wg WHERE d @@@ 'alpha'::wquery
   ORDER BY d <=> 'alpha'::wquery LIMIT 10) a
  JOIN wg_grown g USING (id)
 WHERE a.s IS DISTINCT FROM g.dist OR g.pos > 10;                      -- 0

-- (14) Boolean structure must survive a growth.  `alpha & beta` matches 100
-- docs and `alpha & !beta` 300; at the smallest initial k both need several
-- widenings to reach that depth, and neither may admit a row that @@@ rejects.
SELECT count(*) AS and_rows FROM (
  SELECT id FROM wg WHERE d @@@ 'alpha & beta'::wquery
   ORDER BY d <=> 'alpha & beta'::wquery LIMIT 5000) s;                -- 100
SELECT count(*) AS and_violations FROM (
  SELECT d FROM wg WHERE d @@@ 'alpha & beta'::wquery
   ORDER BY d <=> 'alpha & beta'::wquery LIMIT 5000) s
 WHERE NOT (s.d @@@ 'alpha & beta'::wquery);                           -- 0
SELECT count(*) AS not_rows FROM (
  SELECT id FROM wg WHERE d @@@ 'alpha & !beta'::wquery
   ORDER BY d <=> 'alpha & !beta'::wquery LIMIT 5000) s;               -- 300
SELECT count(*) AS not_violations FROM (
  SELECT d FROM wg WHERE d @@@ 'alpha & !beta'::wquery
   ORDER BY d <=> 'alpha & !beta'::wquery LIMIT 5000) s
 WHERE NOT (s.d @@@ 'alpha & !beta'::wquery);                          -- 0
-- A deep boolean scan must also agree with a one-pass one, row for row.
CREATE TEMP TABLE wg_and_grown AS
  SELECT row_number() OVER () AS pos, id FROM (
    SELECT id FROM wg WHERE d @@@ 'alpha & beta'::wquery
     ORDER BY d <=> 'alpha & beta'::wquery LIMIT 5000) s;
SET pg_weave.wand_initial_k = 400;
CREATE TEMP TABLE wg_and_onepass AS
  SELECT row_number() OVER () AS pos, id FROM (
    SELECT id FROM wg WHERE d @@@ 'alpha & beta'::wquery
     ORDER BY d <=> 'alpha & beta'::wquery LIMIT 5000) s;
SELECT count(*) AS and_pagination_disagreements
  FROM wg_and_grown g FULL JOIN wg_and_onepass o USING (pos)
 WHERE g.id IS DISTINCT FROM o.id;                                     -- 0

RESET pg_weave.wand_initial_k;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE wg;

-- ============================================================================
-- Phrase on a POSITIONLESS document must be FALSE, not a silent conjunction.
--
-- Ported from pg_fts 2deb38e. This was a live WRONG-ANSWER bug here, and worse
-- than upstream: pg_weave's `positions` reloption defaults to OFF, so every phrase
-- query against a default index was answered as a presence-only AND.
--
-- PostgreSQL documents the opposite behaviour. tsearch/ts_utils.h: without
-- TS_EXEC_PHRASE_NO_POS, "OP_PHRASE always returns false if lexeme position
-- information is not available". Verify against core directly below, so this test
-- pins OUR behaviour to CORE's rather than to a hand-written expectation.
--
-- false, not an error: a predicate that errors on a full scan but not under LIMIT
-- is order-dependent, and it would turn a few wrong rows into a whole-table
-- outage. false answers the correct rows.
-- ============================================================================
CREATE TABLE ph (id serial, d wdoc);
-- Adjacent in the text, so a positional index WOULD match the phrase.
INSERT INTO ph(d) SELECT to_wdoc('simple', 'quick brown fox');
-- Both words present but NOT adjacent: an AND matches, a phrase must not.
INSERT INTO ph(d) SELECT to_wdoc('simple', 'quick red slow brown');

-- CORRECTION, and worth recording because the first version of this test asserted
-- the wrong thing in two places and the run corrected me:
--
--  1. to_wdoc() does NOT strip positions. The `positions` reloption controls what
--     the INDEX stores, not what the datum stores, and a phrase is verified at heap
--     recheck against the datum. So a positional datum under a positionless index
--     answers the phrase CORRECTLY -- better than core's stripped tsvector, not
--     worse. The right core comparison is the UNSTRIPPED tsvector.
--  2. PostgreSQL treats an unlabelled lexeme as weight D, so `quick:D` matching an
--     unweighted document is correct, not a bug.
--
-- The genuinely positionless case is reachable only by building a wdoc FROM a
-- stripped tsvector, which is what the second block below does. That is the case
-- the fix is about.

-- (a) Positional datum, positionless index: phrase is verified at recheck and must
--     agree with core's UNSTRIPPED answer -- the adjacent row matches, the
--     non-adjacent one does not.
SELECT to_tsvector('simple','quick brown fox') @@ to_tsquery('simple','quick <-> brown')
         AS core_adjacent,
       to_tsvector('simple','quick red slow brown') @@ to_tsquery('simple','quick <-> brown')
         AS core_nonadjacent;
SELECT count(*) AS weave_phrase_positional_datum
  FROM ph WHERE d @@@ '"quick brown"'::wquery;

-- The AND is true for both rows, so the phrase result above is a real distinction.
SELECT count(*) AS weave_and_rows
  FROM ph WHERE d @@@ 'quick & brown'::wquery;

-- (b) THE ACTUAL BUG: a genuinely positionless wdoc, built from a stripped
--     tsvector. Core returns false even for the adjacent text; so must we. Before
--     the fix this degraded to a presence-only AND and returned true.
SELECT strip(to_tsvector('simple','quick brown fox')) @@ to_tsquery('simple','quick <-> brown')
         AS core_stripped_adjacent;
SELECT to_wdoc(strip(to_tsvector('simple','quick brown fox')))
         @@@ '"quick brown"'::wquery AS weave_stripped_adjacent;
SELECT to_wdoc(strip(to_tsvector('simple','quick red slow brown')))
         @@@ '"quick brown"'::wquery AS weave_stripped_nonadjacent;

-- (c) The fourth route, which no producer-side fix reaches: a boolean
--     sub-expression under a phrase loses positions from the QUERY SHAPE, on a
--     FULLY POSITIONED document. Reachable through the shipped tsquery cast.
SELECT to_tsvector('simple','fox brown zzz quick') @@ to_tsquery('simple','quick <-> (brown & fox)')
         AS core_bool_under_phrase;
SELECT to_wdoc('simple','fox brown zzz quick')
         @@@ (to_tsquery('simple','quick <-> (brown & fox)'))::wquery
         AS weave_bool_under_phrase;

-- (d) Phrase-with-prefix stays deliberately permissive: prefix expansion is not
--     tracked positionally, and that is shipped behaviour. The fix distinguishes
--     this from (b) and (c) with an explicit reason flag rather than a
--     document-level position check -- which cannot tell "no positions anywhere"
--     from "this operand lost them".
SELECT count(*) AS weave_phrase_prefix_permissive
  FROM ph WHERE d @@@ '"quick bro"*'::wquery;

-- (e) Zone/label filtering, same root cause: labels live in position high bits, so
--     a genuinely positionless document carries NO label information -- unknown,
--     not "D". An unlabelled but POSITIONAL document is weight D, matching core.
SELECT to_tsvector('simple','quick brown') @@ to_tsquery('simple','quick:D')
         AS core_unlabelled_is_D;
SELECT count(*) AS weave_zone_d_positional FROM ph WHERE d @@@ 'quick:D'::wquery;
SELECT to_wdoc(strip(to_tsvector('simple','quick brown')))
         @@@ 'quick:D'::wquery AS weave_zone_d_stripped;
SELECT to_wdoc(strip(to_tsvector('simple','quick brown')))
         @@@ 'quick:A'::wquery AS weave_zone_a_stripped;

DROP TABLE ph;

-- ---------------------------------------------------------------------------
-- G43: a term's LAST posting block must not be prove-skipped from the header
-- that follows it.
--
-- `wand_skip_blocks()` advances a cursor over whole 128-posting blocks by
-- reading block HEADERS only, and decides a block lies entirely below the seek
-- target when THE NEXT BLOCK'S first_docid <= target.  That inference is sound
-- only while the next block belongs to the same term.  Posting lists share
-- pages, so the header after a term's FINAL block belongs to another term, and
-- its first_docid is an unrelated -- typically small -- number.  Read as this
-- term's continuation it "proves" the final block is below almost any target,
-- so the block is skipped, `nread` reaches `df`, and the cursor reports itself
-- EXHAUSTED with its last block never decoded.
--
-- WHY IT NEEDS A FUSED SCAN TO SHOW UP, which is also why it survived to a real
-- corpus: the seek has to jump PAST the end of the cursor's current block while
-- postings remain, and only a second channel driving the pivot produces such a
-- target.  The single-channel ranked path advances with wand_next() and never
-- reaches the header inference at all.  Found on BEIR scifact (5,183 docs): a
-- term with df = 211 in blocks of 128 + 83 lost 83 postings, and the fused
-- top-10 that came back was a correct top-10 of the 128 that survived -- every
-- row plausible, six of ten wrong.  doc/GAPS.md G43.
--
-- The fixture needs all four conditions, and dropping any one of them hides the
-- bug (each was observed to):
--   1. a term with MORE THAN ONE posting block (df > 128),
--   2. which sorts EARLY in the dictionary, so another term's blocks follow its
--      last one on the page -- the term written last has no header behind it,
--   3. that is SPARSE relative to a second, DENSE term, so the dense term's
--      pivots land beyond the sparse term's block boundaries,
--   4. and varying document lengths, so BM25 scores are distinct and a top-k
--      oracle is well defined.  With every score tied, any top-k is "correct".
CREATE TABLE mblk (id serial, d wdoc);
INSERT INTO mblk(d) SELECT to_wdoc(
    CASE WHEN g % 10 = 1 THEN 'aaa bbb ' ELSE 'bbb ' END
    || repeat('pad' || (g % 7) || ' ', 1 + (g % 13)))
  FROM generate_series(1, 1300) g;
CREATE INDEX mblk_weave ON mblk USING weave (d);
ANALYZE mblk;

-- 'aaa' is in 130 documents: two blocks, 128 + 2.
SELECT count(*) AS aaa_df FROM mblk WHERE d @@@ 'aaa'::wquery;

-- 'aaa' has by far the higher idf, so every document containing it outranks
-- every document that does not.  The fused top-130 is therefore exactly the
-- 'aaa' documents -- as a SET, which is what makes the assertion immune to the
-- ordering among them.  Before the fix this returned 128 of 130, missing the two
-- postings in the skipped final block.
WITH f AS (SELECT id FROM mblk
            ORDER BY fuse(d <=> 'aaa'::wquery, d <=> 'bbb'::wquery,
                          weights => '{0.5,0.5}') LIMIT 130),
     m AS (SELECT id FROM mblk WHERE d @@@ 'aaa'::wquery)
SELECT (SELECT count(*) FROM m) = (SELECT count(*) FROM m JOIN f USING (id))
         AS fused_scored_every_posting,
       (SELECT count(*) FROM (SELECT id FROM m EXCEPT SELECT id FROM f) z)
         AS postings_dropped;

-- The same property stated on the counters rather than the ranking: the lexical
-- channels are scored once per (term, document) the pivot reaches, so a channel
-- that quits early shows up as a score count below its df.  'aaa' is essential
-- here and the scan is exhaustive, so its channel must be scored at all 130 of
-- its documents.
SELECT weave_fuse_stats_reset();
SET enable_seqscan = off;
SELECT count(*) > 0 AS ran FROM
  (SELECT id FROM mblk ORDER BY fuse(d <=> 'aaa'::wquery, d <=> 'bbb'::wquery,
                                     weights => '{0.5,0.5}') LIMIT 130) t;
SELECT scores - vec_scores - gate_scores >= 130 AS lexical_channels_not_truncated
  FROM weave_fuse_stats();

DROP TABLE mblk;

-- ============================================================================
-- G56: the padding phase, compared with the heap's own answer.
--
-- SQL's ORDER BY never filters, so `WHERE <anything> ORDER BY d <=> q` must
-- return every row the WHERE admits.  The ordering scan ranks q's matches and
-- then walks the heap for the rest (weave_pad_gettuple in src/am/amscan.c).
-- Each case below is checked against a query the index cannot answer, and the
-- ORDER is checked by class: q's matches, then rows with a document, then
-- NULL documents -- NULLs last, where ASC puts them.
--
-- The corpus deliberately holds what a posting list cannot see: NULL documents
-- (never indexed), empty documents (indexed, no postings), and rows moved by a
-- HOT update, whose physical TID is not the chain root an access method must
-- return -- a physical TID resolves to no tuple and the row silently vanishes
-- (AGENTS.md, the Z8 fallback's lesson).
CREATE TABLE pad (id int, d wdoc, tag text);
INSERT INTO pad
SELECT g,
       CASE WHEN g % 10 = 0 THEN NULL
            WHEN g % 10 = 1 THEN to_wdoc('')
            WHEN g % 2 = 0 THEN to_wdoc('alpha w' || g)
            ELSE to_wdoc('beta w' || g) END,
       CASE WHEN g % 3 = 0 THEN 'x' ELSE 'y' END
  FROM generate_series(1, 300) g;
CREATE INDEX pad_w ON pad USING weave (d);
UPDATE pad SET tag = tag WHERE id % 4 = 0;     -- HOT: tag is not indexed
ANALYZE pad;
-- the HOT case must actually exist, or the root-TID mapping is untested
SELECT pg_stat_force_next_flush();
SELECT n_tup_hot_upd > 0 AS some_updates_were_hot
  FROM pg_stat_user_tables WHERE relname = 'pad';
SELECT count(*) FILTER (WHERE d IS NULL) AS null_docs,
       count(*) FILTER (WHERE d @@@ 'alpha'::wquery) AS alpha_docs,
       count(*) AS all_rows
  FROM pad;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
-- a WHERE clause the index does not see: a Filter above the ordering scan
EXPLAIN (COSTS OFF)
SELECT id FROM pad WHERE tag = 'x' ORDER BY d <=> 'alpha'::wquery;
CREATE TEMP TABLE pad_ix AS
  SELECT row_number() OVER () AS pos, id,
         CASE WHEN d @@@ 'alpha'::wquery THEN 0 WHEN d IS NOT NULL THEN 1 ELSE 2 END AS cls
    FROM (SELECT id, d FROM pad WHERE tag = 'x' ORDER BY d <=> 'alpha'::wquery) s;
-- bare, and a LIMIT that stops inside the padding
CREATE TEMP TABLE pad_bare AS
  SELECT row_number() OVER () AS pos, id,
         CASE WHEN d @@@ 'alpha'::wquery THEN 0 WHEN d IS NOT NULL THEN 1 ELSE 2 END AS cls
    FROM (SELECT id, d FROM pad ORDER BY d <=> 'alpha'::wquery) s;
SELECT count(*) AS bare_limit_rows
  FROM (SELECT id FROM pad ORDER BY d <=> 'alpha'::wquery LIMIT 200) s;   -- 200
-- a nested loop rescans the ordering scan once per outer row; each rescan
-- must restart the ranked phase AND the padding walk.  Correlated on t.v so
-- the inner side cannot be materialized once; the plan is asserted.
EXPLAIN (COSTS OFF)
SELECT count(*)
  FROM (VALUES (1), (2), (3)) t(v),
       LATERAL (SELECT id FROM pad WHERE id > t.v
                 ORDER BY d <=> 'alpha'::wquery LIMIT 250) s;
SELECT count(*) AS rescanned_rows, count(DISTINCT (v, id)) AS rescanned_distinct
  FROM (VALUES (1), (2), (3)) t(v),
       LATERAL (SELECT id FROM pad WHERE id > t.v
                 ORDER BY d <=> 'alpha'::wquery LIMIT 250) s;
RESET enable_seqscan;
RESET enable_bitmapscan;

SELECT (SELECT count(*) FROM pad_ix) AS filtered_rows,
       (SELECT count(*) FROM pad WHERE tag = 'x') AS filtered_heap_rows,
       (SELECT count(*) FROM (SELECT id FROM pad_ix
                              EXCEPT SELECT id FROM pad WHERE tag = 'x') z) AS index_only,
       (SELECT count(*) FROM (SELECT id FROM pad WHERE tag = 'x'
                              EXCEPT SELECT id FROM pad_ix) z) AS heap_only,
       (SELECT count(*) - count(DISTINCT id) FROM pad_ix) AS duplicates,
       (SELECT count(*) FROM pad_ix a JOIN pad_ix b ON a.pos < b.pos
         WHERE a.cls > b.cls) AS class_inversions;
SELECT (SELECT count(*) FROM pad_bare) AS bare_rows,
       (SELECT count(DISTINCT id) FROM pad_bare) AS bare_distinct,
       (SELECT count(*) FROM pad_bare a JOIN pad_bare b ON a.pos < b.pos
         WHERE a.cls > b.cls) AS bare_class_inversions;

-- A PARTIAL index: the planner drops the WHERE clause its predicate implies,
-- so the padding walk must apply the predicate itself or it returns rows the
-- query excluded.
DROP INDEX pad_w;
CREATE INDEX pad_wp ON pad USING weave (d) WHERE tag = 'y';
ANALYZE pad;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM pad WHERE tag = 'y' ORDER BY d <=> 'alpha'::wquery;
CREATE TEMP TABLE pad_p AS
  SELECT id FROM pad WHERE tag = 'y' ORDER BY d <=> 'alpha'::wquery;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM pad_p) AS partial_rows,
       (SELECT count(*) FROM pad WHERE tag = 'y') AS partial_heap_rows,
       (SELECT count(*) FROM pad_p JOIN pad USING (id) WHERE pad.tag <> 'y') AS outside_predicate;

DROP TABLE pad_ix, pad_bare, pad_p;
DROP TABLE pad;

-- ============================================================================
-- G66: the ranked pass must rank the PENDING list too.  It used to cover the
-- merged segments only, so a row inserted or UPDATEd since the last flush was
-- missing from `WHERE d @@@ q ORDER BY d <=> q` altogether (60 of 120 in the
-- padding test above, before this fix).  Scores must be on ONE scale: odd ids
-- are in a segment, even ids are pending, and every document is 7 tokens long
-- -- a length the doclen quantizer stores exactly (include/weave/for.h), so
-- score is a strict function of k, the tf of 'alpha', with no quantization
-- tie-shuffling -- and the only correct order is k descending, the two
-- populations interleaved at every level.
CREATE TABLE pr66 (id int, k int, d wdoc);
INSERT INTO pr66 SELECT g, g % 6 + 1,
       to_wdoc(repeat('alpha ', g % 6 + 1) || repeat('filler ', 6 - g % 6))
  FROM generate_series(1, 239, 2) g;
CREATE INDEX pr66_w ON pr66 USING weave (d);
INSERT INTO pr66 SELECT g, g % 6 + 1,
       to_wdoc(repeat('alpha ', g % 6 + 1) || repeat('filler ', 6 - g % 6))
  FROM generate_series(2, 240, 2) g;
SELECT count(*) > 0 AS has_pending
  FROM weave_page_info('pr66_w') WHERE kind LIKE 'pending%' AND NOT freed;
SELECT min(wdoc_length(d)) AS min_len, max(wdoc_length(d)) AS max_len FROM pr66;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_weave.wand_initial_k = 1;    -- force the ladder to widen across pending
CREATE TEMP TABLE pr66_o AS
  SELECT row_number() OVER () AS pos, id, k
    FROM (SELECT id, k FROM pr66 WHERE d @@@ 'alpha'::wquery
           ORDER BY d <=> 'alpha'::wquery) s;
CREATE TEMP TABLE pr66_top AS
  SELECT row_number() OVER () AS pos, id, k
    FROM (SELECT id, k FROM pr66 ORDER BY d <=> 'alpha'::wquery LIMIT 20) s;
RESET pg_weave.wand_initial_k;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM pr66_o) AS ranked_rows,                          -- 240
       (SELECT count(*) FROM pr66_o WHERE id % 2 = 0) AS ranked_pending_rows, -- 120
       (SELECT count(*) FROM pr66_o a JOIN pr66_o b ON a.pos + 1 = b.pos
         WHERE a.k < b.k) AS order_violations,                               -- 0
       (SELECT count(*) FROM pr66_top WHERE k <> 6) AS first_page_not_best,  -- 0
       (SELECT count(*) FROM pr66 WHERE k = 6) AS best_rows;                -- 40
SELECT count(*) AS weave_search_rows
  FROM weave_search('pr66_w', 'alpha'::wquery, 1000);                        -- 240
DROP TABLE pr66_o, pr66_top;
DROP TABLE pr66;

-- The ladder's `curk >= maxhits` stop used a match bound computed from segment
-- df alone.  With 50 matches in a segment and 200 pending, the first rung (64)
-- fills from the union and already exceeds that bound of 50, so the scan
-- stopped at 64 rows.  maxhits now counts the pending list.
CREATE TABLE pr66b (id int, d wdoc);
INSERT INTO pr66b SELECT g, to_wdoc('alpha w' || g) FROM generate_series(1, 50) g;
CREATE INDEX pr66b_w ON pr66b USING weave (d);
INSERT INTO pr66b SELECT g, to_wdoc('alpha w' || g) FROM generate_series(51, 250) g;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET pg_weave.wand_initial_k = 1;
SELECT count(*) AS mostly_pending_rows                                       -- 250
  FROM (SELECT id FROM pr66b WHERE d @@@ 'alpha'::wquery
         ORDER BY d <=> 'alpha'::wquery) s;
RESET pg_weave.wand_initial_k;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE pr66b;
