-- Format v7: the fuzzy weft -- a LOUDS-Sparse SuRF trie over each bolt's
-- vocabulary, on a WEAVE_PK_SURF page chain, registered as a WEAVE_WK_FUZZY
-- descriptor on the bolt's existing channel-descriptor page.  Task Z3;
-- doc/specs/FUZZY_CHANNEL.md section 3, doc/specs/SEGMENT_FORMAT.md sections 2, 6, 9.
--
-- What is NOT under test here: any query going through the trie.  Prefix (Z4),
-- fuzzy (Z5) and regex (Z6) are separate tasks.  What this file proves is that
-- the structure is on disk, is registered where a reader will look for it, is
-- verified in both directions against the dictionary, is rebuilt on merge, is
-- freed rather than leaked, and is loadable.
--
-- The invariant that matters most cannot be seen in a fixed expected output at
-- all: the trie is a filter with false positives and NO false negatives, so a
-- missing term is a silently dropped row.  That is what
-- weave_check()'s surf_trie_matches_dictionary row asserts, in both directions,
-- against the actual dictionary bytes -- and what t/013_surf_corruption.pl proves
-- has teeth by corrupting the image on disk.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

CREATE TABLE sf (id serial, d wdoc);
-- Terms that exercise the trie's shape rather than a degenerate one: a shared
-- prefix family (common/commonX), a mid band, and a per-row rare term, so the
-- vocabulary has real branching, interior terminals ("common" is both a term and
-- a prefix of others), and several thousand slots.
INSERT INTO sf(d)
  SELECT to_wdoc('common common' || (g % 13) || ' mid' || (g % 89) || ' rare' || g)
  FROM generate_series(1, 700) g;
CREATE INDEX sf_weave ON sf USING weave (d);
ANALYZE sf;

-- ---- the Z3 gate -----------------------------------------------------------
-- Trie membership is EXACTLY the dictionary term set.  Not a subset, not a
-- superset: a term the trie misses is a dropped row, a term it invents is a
-- wasted recheck, and the check merges the trie's lexicographic DFS against the
-- dictionary page walk to settle both directions in one pass.
SELECT ok, detail FROM weave_check('sf_weave')
 WHERE invariant = 'surf_trie_matches_dictionary';

-- Every bolt built by v7 carries one.
SELECT detail FROM weave_check('sf_weave') WHERE invariant = 'surf_coverage';

-- Nothing at all is violated, shallow or deep.  The deep pass is the one that
-- would catch the surf chain being leaked or double-claimed.
SELECT count(*) AS violations FROM weave_check('sf_weave', true) WHERE NOT ok;

-- The metapage reports the version this build writes.
SELECT detail FROM weave_check('sf_weave')
 WHERE invariant = 'metapage_version_recognized';

-- ---- the new page kind is accounted for, not "unclassified" ---------------
-- A nonzero unclassified count here is the L17 bug in its v6 form: a reader that
-- tested an extended page kind with a bitwise AND, which compiles and is always
-- false.
SELECT npages AS unclassified_pages FROM weave_index_size_detail('sf_weave')
 WHERE kind = 'unclassified';
SELECT npages > 0 AS surf_pages_exist
  FROM weave_index_size_detail('sf_weave') WHERE kind = 'surf_trie';

-- ---- the shape and the byte cost, from the reader path --------------------
-- weave_surf_stats() goes through weave_surf_load(), i.e. the same loader a
-- prefix scan will use, including its refusal to use an image that does not
-- validate.  nterms here must equal the bolt's dictionary size.
SELECT count(*) AS bolts_with_a_trie FROM weave_surf_stats('sf_weave');
SELECT bool_and(nterms > 0 AND nslots >= nterms AND nnodes >= 1
                AND nterminal >= 1 AND ntrunc = 0
                AND maxdepth BETWEEN 1 AND 255
                AND bytes > 32 AND npages >= 1) AS shape_sane
  FROM weave_surf_stats('sf_weave');

-- The image length is a pure function of the header counts, so this is an
-- identity rather than an estimate -- and it is the assertion that catches a
-- writer and a reader disagreeing about the section layout.
SELECT bool_and(bytes = 32 + nslots + 4 * (8 * ((nslots + 63) / 64))
                     + 4 * ((nslots + 511) / 512) * 2
                     + 4 * ((nnodes + 63) / 64)
                     + 4 * nterminal) AS bytes_match_the_geometry
  FROM weave_surf_stats('sf_weave');

-- ---- merge REBUILDS the trie; it does not concatenate two of them ---------
-- A merged bolt's vocabulary is the union of its inputs' minus fully tombstoned
-- terms, so its slot numbering, rank/select tables and every term ordinal
-- differ from both inputs.  After the merge the invariant must still hold, which
-- is the only mechanical statement of "rebuilt correctly".
INSERT INTO sf(d)
  SELECT to_wdoc('common delta' || (g % 7) || ' rare' || (10000 + g))
  FROM generate_series(1, 500) g;
SELECT weave_merge('sf_weave') IS NOT NULL AS merged;
SELECT count(*) AS violations_after_merge
  FROM weave_check('sf_weave', true) WHERE NOT ok;
SELECT count(*) = weave_index_nsegments('sf_weave') AS one_trie_per_bolt
  FROM weave_surf_stats('sf_weave');

-- Deletions plus vacuum compaction rewrite every bolt.  The freed tries must be
-- freed, not leaked -- weave_check(deep)'s pages_reachable_or_freed is the only
-- thing that would notice, and it is inside the violation count below.
DELETE FROM sf WHERE id % 4 = 0;
SELECT weave_vacuum('sf_weave') IS NOT NULL AS vacuumed;
SELECT count(*) AS violations_after_vacuum
  FROM weave_check('sf_weave', true) WHERE NOT ok;

-- REINDEX rebuilds from the heap.
REINDEX INDEX sf_weave;
SELECT count(*) AS violations_after_reindex
  FROM weave_check('sf_weave', true) WHERE NOT ok;

-- ---- answers are unaffected: the trie is not routed into any query yet ----
SET enable_seqscan = off;
SELECT count(*) AS idx_common FROM sf WHERE d @@@ 'common3'::wquery;
SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*) AS seq_common FROM sf WHERE d @@@ 'common3'::wquery;
RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_seqscan;

-- ---- the boundary cases the format defines --------------------------------
-- An empty bolt has no vocabulary, so it carries no fuzzy weft at all rather
-- than a header-only trie: absent is a state the descriptor can express, and it
-- costs zero bytes.  (weave_surftrie_open() does accept a 32-byte empty image;
-- the writer simply never has a reason to produce one.)
CREATE TABLE sfempty (id serial, d wdoc);
CREATE INDEX sfempty_weave ON sfempty USING weave (d);
SELECT count(*) AS tries_over_an_empty_index FROM weave_surf_stats('sfempty_weave');
SELECT ok FROM weave_check('sfempty_weave', true)
 WHERE invariant = 'surf_trie_matches_dictionary';

-- A term longer than WEAVE_SURFTRIE_MAX_DEPTH (255) is indexed TRUNCATED to that
-- depth with its slot marked `trunc`, which makes every query through it a MAYBE
-- the caller must recheck.  The two alternatives are worse: refusing the build
-- denies the channel to a vocabulary with one long token, and skipping the term
-- is a false negative wearing a build-time disguise.  Two terms sharing a
-- 260-byte prefix must therefore collapse onto ONE truncated terminal, and the
-- set-equality check must accept that one terminal covers both.
CREATE TABLE sflong (id serial, d wdoc);
INSERT INTO sflong(d) VALUES
  (to_wdoc(repeat('z', 260) || 'a')),
  (to_wdoc(repeat('z', 260) || 'b')),
  (to_wdoc('short'));
CREATE INDEX sflong_weave ON sflong USING weave (d);
SELECT ntrunc > 0 AS long_terms_truncated, maxdepth
  FROM weave_surf_stats('sflong_weave');
SELECT ok, detail FROM weave_check('sflong_weave', true)
 WHERE invariant = 'surf_trie_matches_dictionary';

-- ---- error surface --------------------------------------------------------
CREATE TABLE sfnb (id int);
CREATE INDEX sfnb_btree ON sfnb(id);
SELECT * FROM weave_surf_stats('sfnb_btree');

DROP TABLE sf;
DROP TABLE sfempty;
DROP TABLE sflong;
DROP TABLE sfnb;
