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
       to_wdoc(repeat('alpha ', 1 + (g % 5)) || 'common' || (g % 3) ||
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

SELECT (SELECT array_agg(id ORDER BY id) FROM p_pushdown)
       = (SELECT array_agg(id ORDER BY id) FROM p_fallback)
       AS pushdown_and_fallback_pick_the_same_set;

-- ---------------------------------------------------------------------------
-- (3) THE REFUSALS.  Each must be a Sort, and none may raise.
--
-- enable_seqscan stays OFF through all of them on purpose: with it on, a Sort over
-- a Seq Scan would be the cheapest plan for some of these anyway and the arm would
-- prove nothing.  Off, the planner has to reach for an index path, and the fact
-- that it still produces a Sort is evidence that no fused path was offered.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;

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

-- (3d) A metric the index was not built with -- and the interesting part is that
-- the refusal does not depend on the metric at all.  fp_weave is l2 by default;
-- fp_ip is ip.  BOTH of these must be a Sort, because the vector channel is
-- refused OUTRIGHT by the fused path: its warp is a segment-local dense lane index
-- and the fused run advances everything through the docid space (sect. 7b).  So a
-- metric mismatch cannot reach the access method through a fused path, which is
-- how F2.2 discharges "refuse a metric the index was not built with" -- by
-- subsumption rather than by a check the planner could not make anyway (the metric
-- is a reloption, invisible to path generation; VECTOR_CHANNEL.md sect. 8b).
CREATE TABLE fp_ip (id int, body wdoc, emb wvec(4));
INSERT INTO fp_ip SELECT id, body, emb FROM fp;
CREATE INDEX fp_ip_weave ON fp_ip USING weave (body, emb) WITH (metric = 'ip');
ANALYZE fp_ip;

EXPLAIN (COSTS OFF)
SELECT id FROM fp_ip
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <-> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT id FROM fp_ip
 ORDER BY fuse(body <=> 'alpha'::wquery,
               emb <#> '[1,0,0,1]'::wvec, weights => '{1,1}') LIMIT 5;

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

SELECT count(*) > 0 AS vector_arm_runs FROM (
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

DROP TABLE p_pushdown, p_fallback, p_gated, p_ungated, p_lat, p_solo;
DROP TABLE fp_nb;
DROP TABLE fp_ip;
DROP TABLE fp;
RESET max_parallel_workers_per_gather;
