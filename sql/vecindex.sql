-- Task V7: the access method accepts a wvec attribute.
--
-- What is under test is the ROUTING, not the vector channel: no vector structure
-- is written yet (V7's page writers) and nothing queries one (V8).  What must hold
-- now is that a weave index can carry a wvec column at all, that every write and
-- recheck path finds the wdoc by ATTNUM rather than at values[0], and that the
-- combinations the access method cannot honour are refused with a message instead
-- of building an index that answers queries wrongly.
--
-- The four sites this exercises are the build callback and weave_insert
-- (src/am/ambuild.c), the scan-side exact recheck (src/am/amscan.c) and the
-- count-pushdown planner match (src/am/customscan.c).  Each is reached by a
-- different query below, and each is reached with the vector column listed FIRST,
-- because "first column" was the assumption they all shared.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

CREATE TABLE vi (id serial, d wdoc, v wvec(4), v2 wvec(4));
INSERT INTO vi(d, v, v2)
  SELECT to_wdoc('shared common' || (g % 7) || ' rare' || g),
         ('[' || g || ',' || (g + 1) || ',' || (g + 2) || ',' || (g + 3) || ']')::wvec,
         '[0,0,0,1]'::wvec
  FROM generate_series(1, 300) g;

-- ---- what the access method accepts -------------------------------------
CREATE INDEX vi_lex ON vi USING weave (d);
CREATE INDEX vi_both ON vi USING weave (d, v);
DROP INDEX vi_both;
DROP INDEX vi_lex;
-- The vector column FIRST, and from here on the ONLY weave index on the table, so
-- every query below is answered through it.  Nothing about a weave index's column
-- order is a capability question -- no channel's structure is shared with another's,
-- so there is no leading-column prefix rule -- and this is the order that breaks any
-- surviving values[0] assumption.
CREATE INDEX vi_vecfirst ON vi USING weave (v, d);
ANALYZE vi;

-- ---- what it refuses, and with what message ----------------------------
-- No lexical column: the docid space every channel indexes into is assigned by the
-- lexical build, so a vector-only index would answer every query with zero rows.
CREATE INDEX vi_bad ON vi USING weave (v);
-- Two of the same channel: which one is "the" document column would be arbitrary.
CREATE INDEX vi_bad ON vi USING weave (d, v, v2);
CREATE INDEX vi_bad ON vi USING weave (d, d);
-- An opclass on this access method that the registry does not know.  The routing is
-- keyed on the operator family, so an unrecognized one has no channel and must not
-- silently default to one.  amvalidate() is where that is reported; whether CREATE
-- OPERATOR CLASS itself reaches the validator is a core detail, so the create is
-- allowed to succeed here and the validator is called explicitly.
CREATE OPERATOR CLASS wi_bogus_ops FOR TYPE int4 USING weave AS STORAGE int4;
SELECT amvalidate(oid) FROM pg_opclass
  WHERE opcname = 'wi_bogus_ops' AND opcmethod = (SELECT oid FROM pg_am WHERE amname = 'weave');
-- ... and the two real ones validate.
SELECT opcname, amvalidate(oid) FROM pg_opclass
  WHERE opcmethod = (SELECT oid FROM pg_am WHERE amname = 'weave') AND opcname <> 'wi_bogus_ops'
  ORDER BY opcname;
-- An index built on it is refused too, by the same registry lookup.
CREATE INDEX vi_bad ON vi USING weave (d, id wi_bogus_ops);
DROP OPERATOR CLASS wi_bogus_ops USING weave;

-- ---- the build callback found the wdoc ---------------------------------
-- 300 documents indexed through a two-column index whose FIRST column is the
-- vector.  A build that read values[0] would have indexed a wvec as a wdoc.
SET enable_seqscan = off;
SELECT count(*) FROM vi WHERE d @@@ 'shared'::wquery;
SELECT count(*) FROM vi WHERE d @@@ 'rare42'::wquery;

-- ---- weave_insert found the wdoc --------------------------------------
INSERT INTO vi(d, v, v2)
  VALUES (to_wdoc('inserted afterwards rare9001'), '[9,9,9,9]', '[1,0,0,0]');
SELECT count(*) FROM vi WHERE d @@@ 'rare9001'::wquery;
SELECT weave_merge('vi_vecfirst') AS merged;
SELECT count(*) FROM vi WHERE d @@@ 'rare9001'::wquery;

-- ---- the exact recheck found the wdoc ---------------------------------
-- weave_recheck_exact() runs ONLY for query shapes the posting lists over-generate
-- (PHRASE/NEAR/fuzzy/regex), which is why the first version of this test missed the
-- site entirely: a plain two-term AND sets recheck=false and never calls it.  A
-- mutation that reverted this site to values[0] passed the whole suite.
--
-- These two documents differ only in adjacency, so the phrase answer (1) differs
-- from the AND answer (2).  That distinguishes three states rather than two: the
-- recheck not running at all, the recheck reading the wrong column, and the recheck
-- working.  Both callers are exercised -- weave_count_visible() below and the ranked
-- <=> scan, neither of which gets an executor recheck.
INSERT INTO vi(d, v, v2) VALUES
  (to_wdoc('alpha beta gamma'), '[1,1,1,1]', '[0,0,0,1]'),
  (to_wdoc('alpha gamma beta'), '[2,2,2,2]', '[0,0,0,1]');
SELECT count(*) AS and_set FROM vi WHERE d @@@ 'alpha & beta'::wquery;
SELECT count(*) AS phrase_set FROM vi WHERE d @@@ '"alpha beta"'::wquery;
SELECT id FROM vi WHERE d @@@ '"alpha beta"'::wquery
  ORDER BY d <=> '"alpha beta"'::wquery LIMIT 10;
-- The three queries above go through the PLANNER, which is free to answer a phrase
-- with a bitmap heap scan and the executor's own recheck of @@@ -- so they do not
-- prove our recheck ran.  A second mutation run showed exactly that: values[0] at
-- this site still passed them.  weave_count() and weave_search() enter the scan
-- machinery directly and have no executor recheck to fall back on, which is the
-- whole reason weave_recheck_exact() exists.
--
-- Merged first because the ranked scan does not score pending documents at all: the
-- WAND cursors come from segment dictionaries and a pending doc has none, which
-- src/am/amscan.c (above weave_gettuple's ranked path) records as an intentional
-- deferral -- "pending is transient and bounded".  So weave_search() returns 0 rows
-- for these two rows until they are merged, while weave_count() finds them in both
-- states.  Not what this test is about, but worth knowing before reading the numbers.
SELECT weave_merge('vi_vecfirst') AS merged_again;
SELECT weave_count('vi_vecfirst', 'alpha & beta'::wquery) AS and_set_direct;
SELECT weave_count('vi_vecfirst', '"alpha beta"'::wquery) AS phrase_set_direct;
SELECT count(*) AS phrase_ranked_direct
  FROM weave_search('vi_vecfirst', '"alpha beta"'::wquery, 10);

-- ---- the count pushdown matched the lexical column --------------------
-- Vector column first, so a match against indkey.values[0] would find a wvec.
EXPLAIN (COSTS OFF) SELECT count(*) FROM vi WHERE d @@@ 'shared'::wquery;
SELECT count(*) FROM vi WHERE d @@@ 'shared'::wquery;
RESET enable_seqscan;

-- ---- the index is structurally sound ----------------------------------
SELECT count(*) AS violations FROM weave_check('vi_vecfirst', true) WHERE NOT ok;
SELECT detail FROM weave_check('vi_vecfirst') WHERE invariant = 'chandesc_coverage';

DROP TABLE vi;

-- ========================================================================
-- V7's page writers: the weft is on disk, reachable, freed, and self-consistent
--
-- Everything below is about STORAGE.  Nothing queries a vector yet (V8), so the
-- assertions are the ones that can be made without a scan -- and they are chosen
-- to be the ones that FAIL if the weft is written wrongly rather than the ones
-- that merely pass when it is written at all:
--
--   * weave_check(deep) reports zero violations.  That is the LEAK test: a weft
--     that is written but not followed by the reachability walk shows up as
--     unreachable pages, and a weft written but not freed shows up the same way
--     after a merge.  Both are the good failure.
--   * weave_check()'s vector_block_stats_match_codes RECOMPUTES every directory
--     record from the codes stored beside it.  This is contract (C2) checked on
--     disk: a bound 1 % too low silently drops rows, so no fixed-output test can
--     catch it -- this one recomputes rather than compares to a constant.
--   * weave_index_size_detail() shows the new buckets AND still sums to
--     pg_relation_size().  An unbucketed page kind would land in "unclassified"
--     and the only report that says where the bytes go would start lying.
--   * a NULL vector must not shift any other document's answer.  The docid space
--     is shared, so a writer that skipped the NULL instead of leaving a dead lane
--     would associate every later vector with the wrong document -- and count(*)
--     would still be right, which is why the assertion has to be about the
--     livemask arithmetic and not about a row count.
--   * weave_merge() on a vector index returns WITHOUT merging and WITHOUT error,
--     and the index is still clean afterwards (doc/specs/VECTOR_CHANNEL.md
--     sect. 7.3: dropping the weft is undetectable data loss, and an ereport
--     reachable from VACUUM's cleanup is how an index becomes unvacuumable).
-- ========================================================================
CREATE TABLE vw (id serial, d wdoc, v wvec(96));
-- 300 documents, every 7th with a NULL vector.  96 dimensions at the default 4
-- bits is one lane strip and one centroid strip per block, so a block is two
-- pages and the strip chain is exercised in both flavours.
INSERT INTO vw(d, v)
  SELECT to_wdoc('vecweft common' || (g % 5) || ' tag' || g),
         CASE WHEN g % 7 = 0 THEN NULL
              ELSE (SELECT '[' || string_agg(((g * 7 + k * 13) % 101 - 50)::text, ',')
                              || ']' FROM generate_series(1, 96) k)::wvec
         END
    FROM generate_series(1, 300) g;
CREATE INDEX vw_weave ON vw USING weave (d, v);

-- The bolt self-describes a vector weft, and its root is a readable VMETA page
-- whose geometry is self-consistent (chandesc_roots_agree checks that half).
SELECT invariant, ok, detail FROM weave_check('vw_weave')
  WHERE invariant IN ('vector_coverage', 'chandesc_coverage');
SELECT invariant, ok FROM weave_check('vw_weave')
  WHERE invariant IN ('chandesc_roots_agree', 'vector_block_stats_match_codes');

-- Zero violations, deep: this is the leak test and the (C2)-on-disk test at once.
SELECT count(*) AS violations FROM weave_check('vw_weave', true) WHERE NOT ok;
SELECT invariant, ok, detail FROM weave_check('vw_weave', true)
  WHERE NOT ok ORDER BY invariant;

-- The new buckets exist, carry pages, and the report still sums to the relation.
SELECT kind, npages > 0 AS has_pages
  FROM weave_index_size_detail('vw_weave')
 WHERE kind IN ('vector_meta', 'vector_dir', 'vector_codes')
 ORDER BY kind;
SELECT sum(bytes) = pg_relation_size('vw_weave') AS sums_to_relation_size
  FROM weave_index_size_detail('vw_weave');
SELECT count(*) = 0 AS nothing_unclassified
  FROM weave_index_size_detail('vw_weave')
 WHERE kind = 'unclassified' AND npages > 0;

-- The weft's geometry is the one that was asked for, read off the page.
SELECT dim, bits, nvec, nblocks FROM weave_vec_meta('vw_weave');

-- A NULL vector leaves a DEAD LANE; it does not shift the warp.  And the warp is
-- the bolt's DENSE DOCID SPACE, so warp w is the w-th smallest ctid in the table --
-- not the w-th tuple the heap scan happened to reach.
--
-- 300 documents is 10 blocks of 32 lanes (the last partial) and 42 of the 300 have
-- g % 7 = 0, so the live count must be 300 - 42.  That number is the weak half: a
-- writer that SKIPPED the NULLs instead of leaving dead lanes would report 258 live
-- lanes too, and every count(*) in this file would still be right.  The strong half
-- is that the dead POSITIONS are exactly the ctid-ranks of the NULL-vector rows.
--
-- THIS ASSERTION FOUND A REAL BUG.  Lanes were originally assigned in callback
-- order, and a heap scan does not visit tuples in ascending ctid order: a row too
-- small for the current page goes on an earlier one (RelationGetBufferForTuple
-- consults the free space map), and the NULL-vector rows are exactly the small ones.
-- Comparing against `g` looked like the right expectation and was not -- ctid rank
-- is -- and the writer now sorts by docid, which is also what makes the page image
-- deterministic when a synchronized seqscan starts mid-relation.
SELECT sum(nlive) AS live_lanes, sum(nlanes) AS lane_slots, count(*) AS blocks
  FROM weave_vec_blocks('vw_weave');
CREATE TEMP VIEW dead_warps AS
  SELECT (b.firstwarp + i)::bigint AS w
    FROM weave_vec_blocks('vw_weave') b, generate_series(0, b.nlanes - 1) i
   WHERE (b.livemask >> i) & 1 = 0;
CREATE TEMP VIEW null_warps AS
  SELECT (row_number() OVER (ORDER BY ctid) - 1)::bigint AS w, v IS NULL AS isnull_v
    FROM vw;
SELECT (SELECT count(*) FROM dead_warps) AS dead_lanes,
       (SELECT count(*) FROM (SELECT w FROM dead_warps
                              EXCEPT SELECT w FROM null_warps WHERE isnull_v) x)
         AS dead_but_not_null,
       (SELECT count(*) FROM (SELECT w FROM null_warps WHERE isnull_v
                              EXCEPT SELECT w FROM dead_warps) x)
         AS null_but_not_dead;

-- weave_merge() SKIPS a vector-bearing group: no merge, no error, still clean.
SELECT weave_index_nsegments('vw_weave') AS segments_before;
SELECT weave_merge('vw_weave') AS merged_should_be_false;
SELECT weave_index_nsegments('vw_weave') AS segments_after;
SELECT count(*) AS violations_after_skipped_merge
  FROM weave_check('vw_weave', true) WHERE NOT ok;
-- ... and the lexical half still answers, so "skip" did not mean "break".
SET enable_seqscan = off;
SELECT count(*) FROM vw WHERE d @@@ 'vecweft'::wquery;
SELECT weave_count('vw_weave', 'tag42'::wquery) AS one_doc;
RESET enable_seqscan;

-- A second bolt, then a merge, then the deep check.  This is the FREE path: the
-- inserted rows flush into a bolt of their own (no vector weft -- a pending item
-- carries the wdoc alone, doc/GAPS.md G23), the merge must still refuse the group
-- that contains the vector-bearing bolt, and nothing may be left unreachable.
INSERT INTO vw(d, v) SELECT to_wdoc('later arrival tag' || g), NULL
  FROM generate_series(1001, 1010) g;
-- true here means the PENDING FLUSH did work, not that a merge happened:
-- weave_merge() flushes the pending buffer first and reports that.  The segment
-- count below is what says whether the merge itself ran.
SELECT weave_merge('vw_weave') AS merged_again;
SELECT weave_index_nsegments('vw_weave') AS segments_still_unmerged;
SELECT count(*) AS violations_after_second_merge
  FROM weave_check('vw_weave', true) WHERE NOT ok;

-- REINDEX rewrites the weft from scratch and frees the old one.  A free path that
-- forgot the strip chain leaves thousands of unreachable pages here, which is the
-- one place in this file where the leak is large enough to be unmissable.
REINDEX INDEX vw_weave;
SELECT count(*) AS violations_after_reindex
  FROM weave_check('vw_weave', true) WHERE NOT ok;
SELECT sum(bytes) = pg_relation_size('vw_weave') AS sums_to_relation_size_after_reindex
  FROM weave_index_size_detail('vw_weave');

-- An index built WITHOUT a vector column must cost literally zero vector bytes
-- (sect. 7.2), and its weave_check() must report no vector weft rather than an
-- empty one.
CREATE INDEX vw_lexonly ON vw USING weave (d);
SELECT count(*) = 0 AS no_vector_pages
  FROM weave_index_size_detail('vw_lexonly')
 WHERE kind IN ('vector_meta', 'vector_dir', 'vector_codes') AND npages > 0;
SELECT detail FROM weave_check('vw_lexonly') WHERE invariant = 'vector_coverage';

-- A wvec column of fewer than 4 dimensions cannot be indexed: the analytic
-- codebook is fit from a Beta shape of (dim-3)/2, so it does not exist below 4.
-- Refused with a message rather than stored unscoreably.
CREATE TABLE vwsmall (d wdoc, v wvec(2));
INSERT INTO vwsmall VALUES (to_wdoc('tiny'), '[1,2]');
CREATE INDEX vwsmall_weave ON vwsmall USING weave (d, v);
DROP TABLE vwsmall;

-- The `bits` reloption is the one knob that changes the stored bytes, and the
-- value used is recorded in the weft rather than re-read from the catalog.
CREATE INDEX vw_bits2 ON vw USING weave (d, v) WITH (bits = 2);
SELECT count(*) AS violations FROM weave_check('vw_bits2', true) WHERE NOT ok;
SELECT dim, bits, nvec, nblocks FROM weave_vec_meta('vw_bits2');
-- ... and a width outside 2..8 is refused by the reloption, not clamped silently.
CREATE INDEX vw_bits9 ON vw USING weave (d, v) WITH (bits = 9);

DROP VIEW dead_warps, null_warps;
DROP TABLE vw;
