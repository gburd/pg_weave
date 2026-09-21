-- Task Z8: `cgram`, the CORPUS character-trigram channel, and its gate is
-- CORRECTNESS PARITY WITH A SEQ-SCAN `LIKE` REFERENCE over a randomized pattern
-- suite.
--
-- WHY THAT IS THE GATE and not a fixed list of expected ids.  `@~` is answered by
-- intersecting the docid posting lists of the trigrams a pattern REQUIRES and
-- then rechecking on the heap (src/am/amscan.c, the cgram route).  Every way this
-- can go wrong loses rows rather than producing an error: a required trigram
-- computed from a literal run that is not really required (a two-byte run, a run
-- straddling a `_`), a dictionary key whose byte order disagrees with the block
-- index's memcmp order so the probe lands on the wrong page, a bolt whose weft is
-- missing without the route noticing.  In every case the answer is a plausible,
-- smaller result set.  AGENTS.md hard rule 1 is about exactly this.  So each
-- assertion below asks the same question twice -- once through the index, once as
-- a plain `LIKE` on a table with no cgram index at all -- and asserts the id sets
-- are IDENTICAL.
--
-- THE REFERENCE IS A DIFFERENT TABLE, not a subquery over the same one, for the
-- reason sql/edist.sql records: everything here runs with enable_seqscan = off,
-- and a bare scan of the indexed table under that setting can be planned as an
-- Index Scan with no scan key, which the access method refuses.  `cgref` holds
-- the same rows with no index on it, so it is also the "control table with NO
-- cgram index" half of the gate: the same queries must return the same rows by
-- sequential scan.
--
-- WHAT `cgram_scan` ASSERTS.  It counts the times the trigram route SERVED a
-- scan, once per scan.  A pattern with no literal run of three or more bytes, or
-- a case-insensitive pattern over non-ASCII bytes, has no sound requirement to
-- intersect on; the route then FALLS BACK to a sequential pass over the heap and
-- the counter stays at zero.  That is a correct slow answer, so the test asserts
-- BOTH things about such a pattern: the right rows AND a zero counter.  A test
-- that only checked the rows would pass with the fallback deleted -- and deleting
-- the fallback loses rows, which is leg 3 of /scratch/pg_weave/z8-mut.sh.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
SET max_parallel_workers_per_gather = 0;

-- A corpus of multi-token documents.  The point of this channel is the CROSS-TOKEN
-- substring, so the bodies must have token boundaries that a pattern can span, and
-- the token vocabulary must be large enough that a two-token pattern is selective.
--
-- setseed() before the first random(), and the generate_series is ordered, so the
-- corpus is byte-identical on every run and on both majors.  A randomized corpus
-- with a fixed seed is the point: a hand-picked one would only contain the
-- substrings the author thought of.
CREATE TABLE cg (id int, body text);
SELECT setseed(0.42);
INSERT INTO cg
SELECT g,
       'tok' || (1 + floor(random() * 400))::int || ' ' ||
       'tok' || (1 + floor(random() * 400))::int || ' ' ||
       'tok' || (1 + floor(random() * 400))::int || ' ' ||
       'tok' || (1 + floor(random() * 400))::int
FROM generate_series(1, 2000) g;

-- A handful of hand-placed rows that the random corpus cannot be relied on to
-- contain: a multi-byte body, mixed case (which the ASCII fold makes a candidate
-- and the recheck then judges), and a body SHORTER THAN THREE BYTES, which the
-- writer stores under weave_trigrams()' space-padded key so that it still
-- occupies the weft.
INSERT INTO cg(id, body) VALUES
  (9001, 'connection refused by peer'),
  (9002, 'CONNECTION Refused by peer'),
  (9003, 'caf' || U&'\00E9' || ' society opened'),
  (9004, 'CAF' || U&'\00C9' || ' SOCIETY OPENED'),
  (9005, 'ab'),
  (9006, 'a'),
  (9007, ''),
  (9008, NULL),
  (9009, 'tion refutation of tok1');

CREATE TABLE cgref AS SELECT * FROM cg;

-- The wdoc column exists because a weave index REQUIRES a lexical column: the
-- docid space every other weft indexes into is assigned by the lexical build
-- (weave_index_layout() refuses an index without one, and says why).  So a
-- "cgram index" is always at least two columns, and this is the shape
-- include/weave/am.h's WeaveIndexLayout comment describes.
ALTER TABLE cg ADD COLUMN d wdoc;
UPDATE cg SET d = to_wdoc('simple', coalesce(body, ''));
CREATE INDEX cg_idx ON cg USING weave (d, body gram_ops);
ANALYZE cg;
ANALYZE cgref;

-- The channel is visible in the size report, and the report still sums to the
-- relation.  Four page kinds, counted separately from the lexical weft's -- which
-- is the only reason bench/RESULTS_CGRAM.md can state what the channel costs.
SELECT kind, npages > 0 AS present
FROM weave_index_size_detail('cg_idx')
WHERE kind LIKE 'cgram%'
ORDER BY kind;

-- No byte unattributed: the buckets must sum to pg_relation_size().
SELECT sum(bytes) = pg_relation_size('cg_idx') AS sums_to_relation
FROM weave_index_size_detail('cg_idx');

SET enable_seqscan = off;

-- The parity harness.  One function, so each pattern below is one line and the
-- comparison cannot be written two different ways by accident.
--
-- It returns the symmetric difference of the two id sets, so a passing pattern
-- prints nothing at all and a failing one prints exactly which ids each arm has
-- that the other does not.  Counting rows would say "they differ" without saying
-- how, which on a channel whose failure mode is a dropped row is the difference
-- between a diagnosable failure and a re-run.
-- The parentheses are load-bearing: EXCEPT and UNION have the SAME precedence
-- and associate left, so writing this without them gives
-- ((A EXCEPT B) UNION C) EXCEPT D -- which is not a symmetric difference and
-- would report an empty result for two sets that differ.
CREATE FUNCTION cg_diff(pat text) RETURNS TABLE (arm text, id int)
LANGUAGE sql AS $$
  (SELECT 'index-only'::text, id FROM cg    WHERE body @~ pat
   EXCEPT ALL
   SELECT 'index-only'::text, id FROM cgref WHERE body LIKE pat)
  UNION ALL
  (SELECT 'seqscan-only'::text, id FROM cgref WHERE body LIKE pat
   EXCEPT ALL
   SELECT 'seqscan-only'::text, id FROM cg    WHERE body @~ pat)
$$;

CREATE FUNCTION cg_idiff(pat text) RETURNS TABLE (arm text, id int)
LANGUAGE sql AS $$
  (SELECT 'index-only'::text, id FROM cg    WHERE body @~* pat
   EXCEPT ALL
   SELECT 'index-only'::text, id FROM cgref WHERE body ILIKE pat)
  UNION ALL
  (SELECT 'seqscan-only'::text, id FROM cgref WHERE body ILIKE pat
   EXCEPT ALL
   SELECT 'seqscan-only'::text, id FROM cg    WHERE body @~* pat)
$$;

-- NO `EXPLAIN` HERE, deliberately.  A plan shape is not the evidence this file
-- needs and it is not portable: the two majors pick Bitmap Heap Scan and Index
-- Scan differently for the same query, so an expected file with a plan in it
-- fails on one of them for a reason that is not a bug.  The evidence that the
-- INDEX ARM ran is the cgram_scan counter further down, which reports on the C
-- route itself rather than on the planner's opinion of it -- and which is what
-- AGENTS.md's "the suite running is not the SITE running" note asks for.

-- ---------------------------------------------------------------------------
-- The randomized suite.  Patterns are drawn from the corpus itself so they are
-- not all misses: a cross-token pattern taken from a real row's text is the shape
-- this channel exists for, and one invented by hand would mostly match nothing.
--
-- Twenty-four patterns, each asserted to produce an EMPTY symmetric difference.
-- The list deliberately includes every shape the route treats differently:
-- cross-token (a space inside the pattern), single-token, anchored, `_`
-- wildcards on both sides of the three-byte boundary, multi-byte, mixed case,
-- a pattern matching nothing, and a pattern shorter than three bytes.
-- ---------------------------------------------------------------------------
SELECT setseed(0.7);
CREATE TABLE cgpat (n int, pat text, why text);
INSERT INTO cgpat(n, pat, why) VALUES
  ( 1, '%tion refu%',          'cross-token: spans a space, the whole point'),
  ( 2, '%n refused b%',        'cross-token, longer'),
  ( 3, '%connection refused%', 'cross-token, two whole tokens'),
  ( 4, '%tok1 tok2%',          'cross-token over the random vocabulary'),
  ( 5, '%tok12%',              'single token, in-token substring'),
  ( 6, '%tok123%',             'single token, more selective'),
  ( 7, 'tok3%',                'anchored prefix: the run is still required'),
  ( 8, '%peer',                'anchored suffix'),
  ( 9, '%zzq qzz%',            'matches nothing, and no trigram of it exists'),
  (10, '%refused%by%',         'two runs, both usable: the AND of both'),
  (11, '%tion_refu%',          '_ between two usable runs'),
  (12, '%tok1_tok2%',          '_ between two usable runs, random vocabulary'),
  (13, '%ab%',                 'FALLBACK: no run of three bytes'),
  (14, '%a%',                  'FALLBACK: one-byte run'),
  (15, '%',                    'FALLBACK: all wildcards, matches every non-NULL'),
  (16, '%a_c%',                'FALLBACK: every run shorter than three bytes'),
  (17, '%' || U&'\00E9' || ' soc%', 'multi-byte: the trigram straddles two bytes of one character'),
  (18, '%caf' || U&'\00E9' || '%',  'multi-byte at the end of the run'),
  (19, '%CONNECTION%',         'uppercase: the fold makes both rows candidates, the recheck keeps one'),
  (20, '%Refused%',            'mixed case'),
  (21, '%ion refutation%',     'cross-token, matches the hand-placed row only'),
  (22, '%tok4%tok5%',          'two runs separated by %'),
  (23, '%100 tok2%',           'cross-token starting mid-token'),
  (24, '% %',                  'FALLBACK: a one-byte run that matches almost everything');

-- Every pattern, every arm: the symmetric difference must be empty.  One row of
-- output for the whole suite if it passes.
SELECT count(*) AS patterns_checked FROM cgpat;

SELECT p.n, p.why, d.arm, d.id
FROM cgpat p, cg_diff(p.pat) d
ORDER BY p.n, d.arm, d.id;

-- The same for ILIKE.  A separate list because the case-insensitive route refuses
-- a pattern whose literal runs contain a non-ASCII byte (WEAVE_CGRAM_FOLD in
-- include/weave/cgram.h: ILIKE's folding is encoding-aware and ours is not, so
-- requiring a folded non-ASCII trigram would be a FALSE NEGATIVE), and that
-- refusal has to be asserted as a fallback rather than as a served scan.
SELECT p.n, p.why, d.arm, d.id
FROM cgpat p, cg_idiff(p.pat) d
ORDER BY p.n, d.arm, d.id;

-- ---------------------------------------------------------------------------
-- The counter: which patterns the route SERVED, and which fell back.
--
-- One scan each, bracketed by a reset, so the number is exactly "did this one
-- query take the trigram route".
-- ---------------------------------------------------------------------------
CREATE FUNCTION cg_served(pat text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
  n bigint;
BEGIN
  PERFORM weave_channel_stats_reset();
  PERFORM count(*) FROM cg WHERE body @~ pat;
  SELECT cgram_scan INTO n FROM weave_channel_stats();
  RETURN n;
END $$;

CREATE FUNCTION cg_iserved(pat text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
  n bigint;
BEGIN
  PERFORM weave_channel_stats_reset();
  PERFORM count(*) FROM cg WHERE body @~* pat;
  SELECT cgram_scan INTO n FROM weave_channel_stats();
  RETURN n;
END $$;

-- Patterns 1..12 and 17..23 have a usable literal run and must be SERVED (1).
-- Patterns 13..16 and 24 have none and must FALL BACK (0).
SELECT n, why, cg_served(pat) AS served
FROM cgpat
ORDER BY n;

-- ILIKE: the same, except that the two multi-byte patterns (17, 18) must fall
-- back, because a byte-wise ASCII fold cannot be trusted against ILIKE's
-- encoding-aware one.  That difference between the two columns IS the refusal.
SELECT n, why, cg_iserved(pat) AS served
FROM cgpat
ORDER BY n;

-- ---------------------------------------------------------------------------
-- A row inserted AFTER the build.
--
-- It lands in the pending buffer, which carries the analyzed wdoc, the wvec and
-- -- since doc/GAPS.md G35 closed -- the RAW TEXT of the gram_ops column, so the
-- bolt a flush writes from it carries a real cgram weft.  While the row is still
-- pending it is in no bolt at all: the route adds every pending TID to the
-- candidates unconditionally and the mandatory recheck decides, so it is FOUND
-- either way.  Asserted because "found either way" is a claim, and an unasserted
-- claim about a dropped row is the failure mode this whole file exists for.
-- ---------------------------------------------------------------------------
INSERT INTO cg(id, body, d)
VALUES (9100, 'pending insertion refused later',
        to_wdoc('simple', 'pending insertion refused later'));
INSERT INTO cgref(id, body) VALUES (9100, 'pending insertion refused later');

SELECT p.n, p.why, d.arm, d.id
FROM cgpat p, cg_diff(p.pat) d
ORDER BY p.n, d.arm, d.id;

SELECT id FROM cg WHERE body @~ '%tion refused l%' ORDER BY id;

-- ---------------------------------------------------------------------------
-- THE FLUSH, AND THE TWO NUMBERS THAT DISCRIMINATE (doc/GAPS.md G35 and G34).
--
-- Read this before changing anything below it, because most of the obvious
-- assertions here pass whether the gaps are closed or not:
--
--   * THE ROW SET DOES NOT DISCRIMINATE.  Every parity assertion in this file
--     passes with both gaps open -- that is what "absent is safe" means: a bolt
--     with no cgram weft makes the route fall back to a sequential heap pass,
--     which returns the SAME rows, slower.  So the symmetric differences below
--     are a guard against the fix, not a test of it.
--   * NEITHER DOES THE COUNTER WHILE THE ROW IS STILL PENDING.  A pending
--     document is in no bolt, its TID is added as a candidate unconditionally,
--     and the route still serves the scan: `served_while_pending` is 1 before the
--     fix and 1 after.  It is asserted precisely so that the next number cannot
--     be mistaken for it.
--
-- What DOES discriminate is the counter AFTER the pending buffer is folded into a
-- bolt, and the segment count after a compaction:
--
--   served_after_flush     G35.  weave_cgram_collect() uses no bolt's weft unless
--                          EVERY live bolt has one, so the flushed bolt's missing
--                          weft de-accelerated the WHOLE index, not just the new
--                          row.  BEFORE the fix this is 0 (the route falls back to
--                          weave_cgram_heapscan and the counter deliberately does
--                          not move); AFTER it is 1.
--   segments_after_merge   G34.  weave_merge() flushes and then compacts.  BEFORE
--                          the fix a cgram-bearing bolt was refused by
--                          weave_seg_mergeable(), so the two bolts stayed two: 2.
--                          AFTER, the merge re-derives the output weft from its
--                          inputs' wefts and the index compacts to 1.
--
-- The 0 -> 1 and 2 -> 1 pair is also why the two gaps had to close together:
-- G35 alone would have made every flush add a bolt that no compaction could ever
-- consume, and a segment directory that only grows ends at WEAVE_MAX_SEGMENTS
-- with a FAILED INSERT (weave_add_segment_with_room).
--
-- WHAT CATCHES A MIS-MERGED POSTING LIST, which is the risk G34's refusal existed
-- to avoid: the sweeps further down, which now run against a MERGED cgram weft.
-- A merge that loses a (trigram, docid) posting makes the pattern's AND exclude a
-- document that LIKE keeps, and the symmetric difference is then non-empty and
-- names the ids.  No extra sweep is added here -- the ones after the DELETE and
-- after the VACUUM are those assertions, moved onto a merged weft by this block.
-- ---------------------------------------------------------------------------
SELECT cg_served('%tion refused l%') AS served_while_pending;
SELECT weave_merge('cg_idx');
SELECT cg_served('%tion refused l%') AS served_after_flush;
SELECT weave_index_nsegments('cg_idx') AS segments_after_merge;
SELECT id FROM cg WHERE body @~ '%tion refused l%' ORDER BY id;

-- The merged bolt's weft is visible in the size report, and the report still sums
-- to the relation: a merge that wrote cgram pages the report cannot attribute
-- would show up here rather than as a mystery in bench/RESULTS_CGRAM.md.
SELECT sum(bytes) = pg_relation_size('cg_idx') AS sums_to_relation_after_merge
FROM weave_index_size_detail('cg_idx');

-- ---------------------------------------------------------------------------
-- A DELETE, so the tombstone filter on this route is exercised.  A deleted row
-- must vanish from the index arm; the recheck would also drop it (it fetches the
-- live heap tuple), which is why the per-segment tombstone filter is belt and
-- braces here -- but the whole point of sharing the lexical shape is that the
-- livedocs rule applies unchanged, so it is asserted.
-- ---------------------------------------------------------------------------
DELETE FROM cg WHERE id = 9001;
DELETE FROM cgref WHERE id = 9001;
SELECT p.n, p.why, d.arm, d.id
FROM cgpat p, cg_diff(p.pat) d
ORDER BY p.n, d.arm, d.id;

-- ---------------------------------------------------------------------------
-- VACUUM, then the structural check.  weave_check(deep) asserts that every page
-- is reachable or freed, which is the invariant that catches a cgram weft the
-- free path cannot see (it is four chains behind one root; a descriptor-driven
-- free that released only the root would leak the dictionary and every posting
-- page, on every merge).
-- ---------------------------------------------------------------------------
VACUUM (ANALYZE) cg;
SELECT invariant, ok FROM weave_check('cg_idx', true)
WHERE invariant IN ('pages_reachable_or_freed',
                    'segment_roots_have_expected_kind',
                    'chains_do_not_overlap',
                    'chandesc_coverage',
                    'chandesc_reachable',
                    'chandesc_roots_agree',
                    'page_kinds_decodable')
ORDER BY invariant;

SELECT p.n, p.why, d.arm, d.id
FROM cgpat p, cg_diff(p.pat) d
ORDER BY p.n, d.arm, d.id;

-- A REINDEX rebuilds the weft from the heap.  Before doc/GAPS.md G35 closed it was
-- the ONLY path that could, which is what made a post-build INSERT de-accelerate
-- the whole index until someone ran one; the flush and the merge can both produce
-- a weft now, so this asserts a property rather than a workaround: a rebuild from
-- the heap and a weft carried through a flush and a merge agree about the row.
REINDEX INDEX cg_idx;
SELECT cg_served('%tion refused l%') AS served_after_reindex;
SELECT id FROM cg WHERE body @~ '%tion refused l%' ORDER BY id;

-- An oversized INSERT bypasses the pending buffer entirely and becomes its own
-- one-document bolt.  That path had to grow a cgram producer in the same change
-- for the same reason (doc/GAPS.md G35) -- and here the consequence of missing it
-- is larger than one row: a bolt with no weft de-accelerates every `@~` query in
-- the index, so this number is 0 with the pending-buffer half fixed alone and 1
-- with both.  sql/pendingvec.sql asserts the vector half of the same path for the
-- same reason.
INSERT INTO cg(id, body, d)
SELECT 9200, b, to_wdoc('simple', b)
FROM (SELECT 'oversized ' || string_agg('refutation' || g, ' ') AS b
        FROM generate_series(1, 3000) g) s;
INSERT INTO cgref(id, body)
SELECT 9200, 'oversized ' || string_agg('refutation' || g, ' ')
FROM generate_series(1, 3000) g;
SELECT cg_served('%tion refuta%') AS served_after_oversized;
SELECT count(*) AS oversized_row_found FROM cg WHERE body @~ '%refutation7 refutation8%';

-- An index with a gram_ops column and nothing else must be refused, and the
-- error must say why: the docid space comes from the lexical build.
CREATE TABLE cgbad (body text);
CREATE INDEX cgbad_idx ON cgbad USING weave (body gram_ops);

-- Two gram_ops columns is a design error, not a configuration.
CREATE TABLE cgtwo (a text, b text, d wdoc);
CREATE INDEX cgtwo_idx ON cgtwo USING weave (d, a gram_ops, b gram_ops);

RESET enable_seqscan;
DROP FUNCTION cg_served(text);
DROP FUNCTION cg_iserved(text);
DROP FUNCTION cg_diff(text);
DROP FUNCTION cg_idiff(text);
DROP TABLE cgpat;
DROP TABLE cgtwo;
DROP TABLE cgbad;
DROP TABLE cgref;
DROP TABLE cg;
