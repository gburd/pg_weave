-- F2.2: the fused ORDER BY pushdown -- the planner half (src/am/fusepath.c) and
-- the AM-side fused scan (src/am/amscan.c).  doc/specs/FUSED_TOPK.md sect. 3, 4,
-- 7, 7a and 7b.
--
-- WHAT A GREEN RUN HAS TO PROVE, in the order the file argues it:
--
--   1. THE PATH IS OFFERED AND THE SORT IS GONE.  An Index Scan whose `Order By`
--      carries one key per channel PLUS the `<~>` transport key.  The transport key
--      being visible is not cosmetic: it is the only evidence that the weights
--      reached the access method at all, and a plan without it would be a
--      single-channel ordering wearing a fused query's clothes.
--   2. THE ANSWER IS THE SAME SET the fallback picks, on a corpus where both arms
--      are exact.
--   3. EVERY REFUSAL FALLS BACK TO A SORT *WITHOUT ERROR*.  This is the half of
--      the file that matters most, because the alternative is doc/GAPS.md G39: an
--      access method that offers a path and then refuses it at run time, which is a
--      query that fails rather than a query that is slow.  src/am/fusepath.c's hard
--      invariant is that it never offers a path weave_rescan() refuses, and these
--      arms are how that invariant is checked from outside.
--   4. A WHERE CLAUSE NARROWS THE FUSED ANSWER -- the conjunctive-gate path, where
--      the qual becomes a REQUIRED channel inside the scan rather than a filter
--      above it (include/weave/fuse.h note 2, sect. 3a (2)).
--   5. A RESCAN does not carry state from the previous scan.
--
-- WHAT IT DOES NOT ASSERT: that the pushdown and the fallback produce the same
-- ORDER, or the same float.  sect. 7a (1) records why that is impossible rather
-- than unimplemented -- weave_distance() outside an index has no corpus, so it
-- scores with df = 1 and avgdl = |D| -- and sect. 3a records the second reason,
-- that float addition is not associative and the two sum in different orders.
-- Section (2) below is therefore built on a corpus where the two agree on the
-- candidate SET, and it compares sets.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;

-- Plan text stability only.
SET max_parallel_workers_per_gather = 0;

CREATE TABLE fp (id int, body wdoc, emb wvec(4));
INSERT INTO fp
SELECT g,
       to_wdoc(repeat('alpha ', g) || 'common' || (g % 3) ||
               (CASE WHEN g IN (7, 19) THEN ' zeta' ELSE '' END)),
       ('[' || g || ',' || (g % 4) || ',' || (g % 7) || ',1]')::wvec
FROM generate_series(1, 40) g;
CREATE INDEX fp_weave ON fp USING weave (body, emb);
ANALYZE fp;

-- ---------------------------------------------------------------------------
-- (1) THE PLAN.  An Index Scan, no Sort, and the transport key in `Order By`.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery,
               weights => '{0.25,0.75}') LIMIT 5;

-- Omitted weights are materialized into a real array by the planner, because the
-- access method reads an array and has no way to be told "there wasn't one".  The
-- plan must therefore show '{1,1}' even though the query wrote nothing.
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery, body <=> 'zeta'::wquery) LIMIT 5;

-- The commutator spelling `q <=> col` is normalized into `col <=> q` before it
-- becomes an index ORDER BY key: an index key must have the column on the LEFT,
-- and nothing in the planner commutes an ordering operator outside of core's own
-- index matching, which this path bypasses.  So this plan reads identically to
-- the first one.
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse('alpha'::wquery <=> body,
               body <=> 'zeta'::wquery,
               weights => '{0.25,0.75}') LIMIT 5;

-- ---------------------------------------------------------------------------
-- (2) THE ANSWER, against the fallback, as a SET.
--
-- Both arms are exact about the candidate set here: every document contains
-- 'alpha', so no document is missing from either ranking for want of a posting,
-- and the weights are exactly representable in float4 and float8 alike.  What may
-- differ between them is the ORDER, for the two reasons in this file's header, so
-- the comparison is array_agg(id ORDER BY id) over a k wide enough that the set is
-- determined: 20 of 40 documents, cut at a point where the two rankings cannot
-- disagree about membership because 'zeta' separates exactly two documents.
-- ---------------------------------------------------------------------------
CREATE TEMP TABLE p_pushdown AS
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery,
               weights => '{0.25,0.75}') LIMIT 20;

RESET enable_seqscan;

-- The fallback arm: no index path at all, so fuse() is evaluated per row over the
-- recovered scores and sorted.  The EXPLAIN is here because an arm that stopped
-- running the plan it claims to would make the comparison meaningless.
SET enable_indexscan = off;
SET enable_bitmapscan = off;

EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery,
               weights => '{0.25,0.75}') LIMIT 20;

CREATE TEMP TABLE p_fallback AS
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery,
               weights => '{0.25,0.75}') LIMIT 20;

RESET enable_indexscan;
RESET enable_bitmapscan;

-- RECORDED, NOT ASSERTED, and the retraction is the point.  An earlier version of
-- this file asserted that the pushdown and the fallback pick the same SET, on the
-- argument that 'zeta' separates exactly two of the 40 documents so a k = 20 cut
-- could not disagree about membership.  **That premise is false and the assertion
-- returned f.**  `doc/specs/FUSED_TOPK.md` sect. 7a (1): the fallback's `<=>` runs
-- through `weave_distance()` (`src/query/rank.c:459`), which has no corpus and
-- therefore scores with df = 1 and avgdl = |D|, while the index computes exact
-- BM25 -- so the two already rank a SINGLE lexical channel differently, and a
-- weighted sum of two of them can disagree about which documents are in the top 20
-- at all, not merely about their order.  Same discipline as
-- `sql/fuse_fallback.sql` sect. (5) and `sql/vecorderby.sql` sect. (3): both arms
-- keep their EXPLAIN, and the difference is a number.
SELECT count(*) AS pushdown_and_fallback_set_overlap
  FROM p_pushdown JOIN p_fallback USING (id);

-- THE ASSERTION THIS ARM ACTUALLY NEEDS, and it is stronger than agreement with an
-- approximate fallback: an EXACT ORACLE built from the index's own per-channel
-- scores.  `weave_search()` returns exact BM25 per (ctid, score) and enters the
-- scan machinery directly, so it carries none of `weave_distance()`'s df = 1 /
-- avgdl = |D| approximation.  One call per channel at k = 40 covers every match
-- (all 40 documents contain 'alpha', 2 contain 'zeta'); a document with no posting
-- in one channel contributes 0 to that side, hence the FULL JOIN and the coalesce.
-- What is left that could move the set is the fused scorer's own pruning and
-- ordering -- which is what F2.2 is under test for.
CREATE TEMP TABLE p_oracle_scores AS
SELECT COALESCE(a.ctid, z.ctid) AS rowtid,   -- not "ctid": a table may not have a column of that name
       0.25::float8 * COALESCE(a.score, 0::float8)
       + 0.75::float8 * COALESCE(z.score, 0::float8) AS score
  FROM weave_search('fp_weave', 'alpha'::wquery, 40) a
  FULL JOIN weave_search('fp_weave', 'zeta'::wquery, 40) z USING (ctid);

-- Tie-freeness first, because a tie straddling rank 20 makes "the" top-20 set
-- ambiguous no matter how the scorer behaves.  `sql/fuse_degenerate.sql` hit that
-- for real -- a seven-way tie there produced two different, both-correct answers.
SELECT count(*) = count(DISTINCT score) AS oracle_scores_are_tie_free
  FROM p_oracle_scores;

CREATE TEMP TABLE p_oracle AS
SELECT fp.id FROM p_oracle_scores s JOIN fp ON fp.ctid = s.rowtid
 ORDER BY s.score DESC LIMIT 20;

SELECT (SELECT array_agg(id ORDER BY id) FROM p_pushdown)
       = (SELECT array_agg(id ORDER BY id) FROM p_oracle)
       AS pushdown_matches_the_exact_oracle;

-- ---------------------------------------------------------------------------
-- (2b) THE FLAGSHIP SHAPE: one LEXICAL channel and one VECTOR channel, task F8.
--
-- This is the query doc/specs/FUSED_TOPK.md sect. 7 has advertised since the spec
-- was written and which nothing could answer from an index until F8:
--
--     ORDER BY fuse(body <=> 'alpha', emb <-> '[...]')
--
-- WHAT MADE IT POSSIBLE, in one sentence each, because two separate things were
-- missing and only one of them was the plumbing everyone expected:
--
--   * The vector shuttle publishes SEGMENT-LOCAL LANE INDICES and the fused core
--     drives docids, so the channel had to be relabelled -- include/weave/vecdocmap.h
--     does it through the bolt's docid-ascending warp map, and
--     test/hegel/test_vecdocmap.c is the (C1)/(C2) property test for it.
--   * fuse() sums SCORES, and `<->` is a DISTANCE.  0.16.0 had no recovery function
--     for it, so the raw distance went into the sum as if it were a score and the
--     final negation ranked the FARTHEST vector first -- silently, in this exact
--     query shape (doc/GAPS.md G41).  weave_l2score() and weave_ipscore() close it.
--
-- The second one is why this section asserts against an oracle rather than against
-- the fallback: both arms were wrong in the same direction before 0.17.0, so no
-- comparison between them could have found it.
-- ---------------------------------------------------------------------------

-- ROW <-> DOCID, so the vector SRF's output can be joined to rows.  Derived by
-- row_number() rather than from WEAVE_OFFSET_FACTOR, which keeps it independent of
-- BLCKSZ: docids ascend with ctid (include/weave/am.h -- weave_tid_to_docid() is
-- monotone in (block, offset)) and every row of `fp` has a non-NULL vector, so the
-- n-th row in ctid order owns the n-th docid in ascending order.  Borrowed verbatim
-- from sql/vecorderby.sql, including the count assertion that says the derivation
-- held.  The column is `rowtid` and not `ctid`, because a table may not have a
-- column of that name.
SET enable_seqscan = on;
CREATE TEMP TABLE fpmap AS
  SELECT t.id, t.rowtid, l.docid
    FROM (SELECT id, ctid AS rowtid, row_number() OVER (ORDER BY ctid) AS rn
            FROM fp) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('fp_weave')) l USING (rn);
SELECT count(*) = (SELECT count(*) FROM fp) AS map_covers_every_row FROM fpmap;

SET enable_seqscan = off;

-- THE PLAN.  Three `Order By` keys: the lexical channel, the vector channel, and the
-- `<~>` transport key carrying the weights.  The transport key hangs off the LEXICAL
-- column even though the second channel is on another one, which is deliberate --
-- src/am/fusepath.c explains why it must, and this arm is what would notice if the
-- per-key `indexorderbycols` and that choice ever disagreed.
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <-> '[1,0,0,1]'::wvec,
               weights => '{0.5,0.5}') LIMIT 10;

CREATE TEMP TABLE pv_pushdown AS
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <-> '[1,0,0,1]'::wvec,
               weights => '{0.5,0.5}') LIMIT 10;

RESET enable_seqscan;

-- THE EXACT ORACLE, one call per channel at k = 40 (every document).  Each SRF
-- enters the scan machinery directly and reports the INDEX's own per-channel score:
-- exact BM25 from weave_search(), and the QUANTIZED metric-domain score from
-- weave_vec_scan() -- which is the number the fused scan sums, not the exact
-- -||q-v||^2 the heap operator would give.  Using the exact one here would be
-- measuring the quantizer, not the scorer.
CREATE TEMP TABLE pv_oracle_scores AS
SELECT m.id,
       0.5::float8 * COALESCE(a.score, 0::float8)
       + 0.5::float8 * COALESCE(v.score::float8, 0::float8) AS score
  FROM fpmap m
  LEFT JOIN weave_search('fp_weave', 'alpha'::wquery, 40) a ON a.ctid = m.rowtid
  LEFT JOIN weave_vec_scan('fp_weave', '[1,0,0,1]'::wvec, 40) v
         ON v.docid = m.docid;

-- Tie-freeness first, for the reason section (2) gives: a tie straddling rank 10
-- makes "the" top-10 ambiguous however the scorer behaves.  It is a sharper question
-- here than there, because the vector term is QUANTIZED and two documents can be
-- assigned the same code-domain score even when their exact distances differ.
SELECT count(*) = count(DISTINCT score) AS oracle_scores_are_tie_free
  FROM pv_oracle_scores;

CREATE TEMP TABLE pv_oracle AS
SELECT id FROM pv_oracle_scores ORDER BY score DESC LIMIT 10;

SELECT (SELECT array_agg(id ORDER BY id) FROM pv_pushdown)
       = (SELECT array_agg(id ORDER BY id) FROM pv_oracle)
       AS fused_vector_pushdown_matches_the_exact_oracle;

-- AND THE DIRECTION, ON ITS OWN, because it is the half of G41 that an oracle
-- agreement could still hide: if the vector channel were summed as a distance
-- instead of a score, the top-10 would be the FARTHEST documents.  `emb` is
-- [g, g%4, g%7, 1] for g = 1..40 and the query is [1,0,0,1], so the near documents
-- are the small g -- and the lexical channel pulls the same way (tf of 'alpha' is g,
-- so LARGE g scores higher), which is what makes this arm informative: the two
-- channels disagree, and an inverted vector channel would move the answer.
SELECT min(id) AS nearest_id, max(id) AS farthest_id,
       (SELECT count(*) FROM pv_pushdown WHERE id <= 10) AS ids_in_the_near_third
  FROM pv_pushdown;

-- ip, on an index built for it, to show the second recovery function and the second
-- metric are wired the same way.  fp_ip is also section (3d)'s mismatch subject.
CREATE TABLE fp_ip (id int, body wdoc, emb wvec(4));
INSERT INTO fp_ip SELECT id, body, emb FROM fp;
CREATE INDEX fp_ip_weave ON fp_ip USING weave (body, emb) WITH (metric = 'ip');
ANALYZE fp_ip;

SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM fp_ip
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <#> '[1,0,0,1]'::wvec,
               weights => '{0.5,0.5}') LIMIT 5;
RESET enable_seqscan;

-- ---------------------------------------------------------------------------
-- (3) THE REFUSALS.  Each must be a Sort, and none may raise.
--
-- enable_seqscan stays ON through all of them, and the first version of this file
-- had it off with the argument that only a disabled seq scan forces the planner to
-- reach for an index path.  **PostgreSQL 18 prints `Disabled: true` on a disabled
-- node and PostgreSQL 17 does not**, so that choice made every refusal arm's
-- expected output major-dependent for no gain.  What these arms have to show is
-- that nothing was offered and nothing raised, and a Sort over a Seq Scan shows
-- both; section (1) is where the file proves a fused path IS offered and chosen,
-- and it is the arm that needs the seq scan out of the way.
-- ---------------------------------------------------------------------------
SET enable_seqscan = on;

-- (3a) DESC.  fuse() returns the NEGATED weighted sum, so ASCENDING is best-first
-- and a DESC pathkey asks for the WORST documents first -- which a top-k scan
-- cannot produce without enumerating the whole match set.  Refused, not served
-- backwards.
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery, weights => '{1,1}') DESC LIMIT 5;

-- (3b) An argument that names no channel.  `id::float8` is taken as a SCORE, which
-- is what fuse()'s own declaration says it is, and there is no index column behind
-- it -- so the shape has one attributable channel and a fusion needs two.  The
-- planner never guesses a distance map from a float (sect. 7a (2)).
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(id::float8,
               body <=> 'alpha'::wquery, weights => '{1,1}') LIMIT 5;

-- (3c) A column the index does not carry, ISOLATED from every other refusal: both
-- channels are the servable lexical one, both are spelled exactly as the offered
-- shape is, and the only thing wrong is that `title` is not in the index.  Using a
-- vector column here instead would have proved nothing, because the vector channel
-- is refused for its own reason (3d) and the arm would pass either way.
CREATE TABLE fp_nb (id int, body wdoc, title wdoc);
INSERT INTO fp_nb SELECT id, body, to_wdoc('beta gamma') FROM fp;
CREATE INDEX fp_nb_weave ON fp_nb USING weave (body);
ANALYZE fp_nb;

EXPLAIN (COSTS OFF)
SELECT id FROM fp_nb
 ORDER BY fuse(body <=> 'alpha'::wquery,
               title <=> 'beta'::wquery, weights => '{1,1}') LIMIT 5;

-- (3d) A METRIC THE INDEX WAS NOT BUILT WITH -- and since F8 this is a REAL
-- refusal rather than a refusal by subsumption.  `fp_weave` is l2 (the reloption
-- default) and `fp_ip_weave` is ip; section (2b) has already shown that each serves
-- ITS OWN operator.  The crossed pairs below must be a Sort.
--
-- WHY THE PLANNER CAN REFUSE THIS AND THE SINGLE-CHANNEL ORDER BY PATH CANNOT.  A
-- weave vector weft is scored in one metric, so answering `<->` out of an ip weft
-- returns every row in an ordering the query did not ask for -- a wrong answer, not
-- an approximation.  Core matches a pathkey against an operator FAMILY and the
-- metric is a reloption, invisible to path generation, so `ORDER BY emb <-> q` can
-- only be refused at RUN time (weave_rescan(), and VECTOR_CHANNEL.md sect. 8b for
-- the per-metric opclass split that would fix it properly).  The fused path is our
-- own code: src/am/fusepath.c opens the index, reads the metric with the
-- NON-THROWING accessor, and declines -- so the Sort below is chosen at plan time
-- and no query fails.  That is the difference between this and doc/GAPS.md G39.
CREATE TABLE fp_l2b (id int, body wdoc, emb wvec(4));
INSERT INTO fp_l2b SELECT id, body, emb FROM fp;
CREATE INDEX fp_l2b_weave ON fp_l2b USING weave (body, emb);
ANALYZE fp_l2b;

-- `<->` against the ip index: refused.
EXPLAIN (COSTS OFF)
SELECT id FROM fp_ip
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <-> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5;

-- `<#>` against the l2 index: refused, the same test in the other direction.
EXPLAIN (COSTS OFF)
SELECT id FROM fp_l2b
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <#> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5;

-- And `<=>` on a wvec -- COSINE -- which no weave index can carry at all
-- (weave_index_vec_metric() refuses the reloption at CREATE INDEX, because a cosine
-- bound must divide by a maximum true norm that is not stored).  It has a recovery
-- function, so fuse() computes it correctly row by row; what it does not have is an
-- opclass member, so there is no channel to attribute it to and no path to offer.
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <=> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5;

-- And the `<@>` channel, refused for the third warp space: its shuttle walks the
-- DICTIONARY, not any document space (src/query/edist.c).
EXPLAIN (COSTS OFF)
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <@> 'alpha', weights => '{1,1}') LIMIT 5;

-- enable_seqscan goes back ON for the three checks below, and the reason is at the
-- call site because it is not obvious: with it off, a bare count(*) or a
-- key-less aggregate over an indexed table fails on PostgreSQL 18 (doc/GAPS.md
-- G39 -- the access method is handed a scan with neither a restriction key nor an
-- ordering clause, and refuses it).  The aggregates below are over subqueries, not
-- over the table, but the cost of being wrong about that is a red test for a
-- reason unrelated to F2.2, so they run outside the off window.
RESET enable_seqscan;

-- Every refused shape must still RUN and return rows.  A refusal that produced a
-- correct plan and a failed query would be no better than G39.
SELECT count(*) > 0 AS desc_arm_runs FROM (
  SELECT id FROM fp
   ORDER BY fuse(body <=> 'alpha'::wquery,
                 body <=> 'zeta'::wquery, weights => '{1,1}') DESC LIMIT 5) s;

-- The refused metric-mismatch arm, and its SERVED counterpart beside it: since F8
-- the vector channel is not refused as a class, so this pair is what says the
-- refusal is about the metric and not about the channel.
SELECT count(*) > 0 AS mismatched_vector_arm_runs FROM (
  SELECT id FROM fp_ip
   ORDER BY fuse(body <=> 'alpha'::wquery,
                 emb <-> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5) s;

SELECT count(*) > 0 AS matched_vector_arm_runs FROM (
  SELECT id FROM fp_ip
   ORDER BY fuse(body <=> 'alpha'::wquery,
                 emb <#> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5) s;

SELECT count(*) > 0 AS edist_arm_runs FROM (
  SELECT id FROM fp
   ORDER BY fuse(body <=> 'alpha'::wquery,
                 body <@> 'alpha', weights => '{1,1}') LIMIT 5) s;

-- ---------------------------------------------------------------------------
-- (4) A WHERE CLAUSE NARROWS THE FUSED ANSWER: the conjunctive-gate path.
--
-- The qual becomes a REQUIRED channel INSIDE the fused run, so it steers pivot
-- selection rather than discarding rows that have already been scored -- which is
-- claim 3 of doc/ARCHITECTURE.md sect. 9 and the payoff include/weave/fuse.h note 2
-- describes.  What is assertable from SQL is the answer, in two halves: every row
-- returned satisfies the qual, and the narrowed answer is a SUBSET of the
-- unnarrowed one over a k wide enough to contain it.
--
-- bitmapscan is off so that core generates a plain IndexPath for the `@@@` clause,
-- which is the path whose indexclauses the fused path borrows.  Without it add_path
-- may discard the plain IndexPath in favour of a bitmap one, the borrow finds
-- nothing, and the qual becomes an executor filter -- the SAME ANSWER by a slower
-- route, which is why this is a GUC and not an assertion.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;

EXPLAIN (COSTS OFF)
SELECT id FROM fp
 WHERE body @@@ 'common0'::wquery
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery, weights => '{1,1}') LIMIT 5;

CREATE TEMP TABLE p_gated AS
SELECT id FROM fp
 WHERE body @@@ 'common0'::wquery
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery, weights => '{1,1}') LIMIT 5;

CREATE TEMP TABLE p_ungated AS
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery, weights => '{1,1}') LIMIT 40;

RESET enable_bitmapscan;
RESET enable_seqscan;

SELECT count(*) AS gated_rows_failing_the_qual
  FROM fp JOIN p_gated USING (id)
 WHERE NOT (fp.body @@@ 'common0'::wquery);

SELECT NOT EXISTS (SELECT 1 FROM p_gated
                    WHERE id NOT IN (SELECT id FROM p_ungated))
       AS gated_is_a_subset_of_ungated;

-- ---------------------------------------------------------------------------
-- (5) RESCAN.  The fused scan on the inner side of a nested loop, re-executed once
-- per outer row with a different pull depth.
--
-- `LIMIT o.n` is a lateral reference, so the subquery is rescanned rather than
-- materialized, and each arm drives the widening ladder to a different depth.  What
-- this watches for is a per-rescan field left over from the previous arm --
-- weave_rescan() resets fuseScan/fuseQ/fuseW/nfuse/fusek/fuseDone in the same place
-- it resets everything else, and a leaked `fuseDone` in particular would truncate
-- the deeper arms while every row returned still looked plausible.  That is the
-- failure sql/vecorderby.sql's correlated subquery was added for on the vector
-- side; the same hazard, the same shape of test.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;

SELECT o.n, count(*) AS rows_pulled
  FROM (VALUES (1), (3), (7)) o(n),
       LATERAL (SELECT id FROM fp
                 ORDER BY fuse(body <=> 'alpha'::wquery,
                               body <=> 'zeta'::wquery,
                               weights => '{0.5,0.5}') LIMIT o.n) l
 GROUP BY o.n ORDER BY o.n;

-- The deepest arm must agree, as a set, with the same query run on its own: a
-- rescan that returned a different top-7 than a fresh scan would be a leak.
CREATE TEMP TABLE p_lat AS
SELECT l.id
  FROM (VALUES (7)) o(n),
       LATERAL (SELECT id FROM fp
                 ORDER BY fuse(body <=> 'alpha'::wquery,
                               body <=> 'zeta'::wquery,
                               weights => '{0.5,0.5}') LIMIT o.n) l;

CREATE TEMP TABLE p_solo AS
SELECT id FROM fp
 ORDER BY fuse(body <=> 'alpha'::wquery,
               body <=> 'zeta'::wquery, weights => '{0.5,0.5}') LIMIT 7;

RESET enable_seqscan;

SELECT (SELECT array_agg(id ORDER BY id) FROM p_lat)
       = (SELECT array_agg(id ORDER BY id) FROM p_solo)
       AS rescan_matches_a_fresh_scan;

-- ---------------------------------------------------------------------------
-- (6) THE TRANSPORT OPERATOR IS NOT EVALUABLE, and that is the design rather than
-- defensiveness: sect. 7a rejected carrying the weights on a QUAL because
-- indexqualorig is re-evaluated during an EPQ recheck, so a marker in a qual is
-- eventually executed for real.  An ORDER BY key is not, so this can raise -- which
-- turns any future path that WOULD evaluate it from a silent wrong ordering into a
-- failure with a name.
-- ---------------------------------------------------------------------------
SELECT body <~> '{1,1}'::float4[] FROM fp LIMIT 1;

DROP TABLE p_pushdown, p_fallback, p_oracle_scores, p_oracle, p_gated, p_ungated, p_lat, p_solo;
DROP TABLE fpmap, pv_pushdown, pv_oracle_scores, pv_oracle;
DROP TABLE fp_nb;
DROP TABLE fp_l2b;
DROP TABLE fp_ip;
DROP TABLE fp;
RESET max_parallel_workers_per_gather;
