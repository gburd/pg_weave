-- F7: the vector channel's ORDER BY operators.
--
-- WHICH OPERATORS, because it is two and not one: `<->` (l2) and `<#>` (negative
-- inner product, so ascending is largest-IP-first), one per metric the scan core
-- serves (include/weave/vecscan.h).  `<=>` is cosine and is deliberately NOT a
-- member -- section (11) asserts the Sort it therefore gets -- because a cosine
-- index cannot exist (CREATE INDEX refuses metric = 'cosine') and a cosine member
-- could only ever be answered in l2 or ip order, which is a wrong answer.  Section
-- (10) asserts that a metric MISMATCH is refused rather than answered.
--
-- WHAT WAS MISSING.  wvec_weave_ops was created `STORAGE wvec` and nothing else
-- (0.7.0--0.8.0), so the planner had no ordering operator to match a pathkey
-- against and `ORDER BY embedding <-> $1 LIMIT 10` -- the first query shape any
-- pgvector user writes -- could not reach amrescan AT ALL.  It was answered by a
-- Seq Scan plus a top-N Sort evaluating the exact distance on every row, which is
-- task L7's recorded 7,000x cliff (doc/GAPS.md G1) with a different column type.
-- sql/vecscan.sql says at its top that "V8 wires no operator and no ORDER BY path,
-- so nothing the planner can produce enters the code scan"; this file is what makes
-- that sentence out of date.
--
-- SO THIS FILE ASSERTS PLANS, not only rows.  A test that checks rows alone passes
-- just as happily on the Seq Scan path, which is exactly how the suite missed the
-- lexical version of this cliff for the whole life of the fork.
--
-- THE SCORING KERNEL IS PINNED TO scalar for the whole file, for sql/vecscan.sql's
-- reason: `auto` picks the widest SIMD path the host can run, and while every
-- implemented path is proven bit-identical to the scalar oracle by
-- test/hegel/test_kernels.c, pinning removes the host from the expected output --
-- which is what lets the divergence COUNTS below be numbers instead of ranges.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
SET pg_weave.vec_kernel = 'scalar';

-- enable_seqscan=off is how we test PATH GENERATION rather than cost (see
-- sql/orderby.sql): if the AM can produce an ordering path the planner takes it, and
-- if it cannot the planner falls back to Seq Scan even at disable_cost.  So a Seq
-- Scan below means "no index path exists", not "the index looked expensive".
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = on;

-- 300 documents, 8 dimensions, one bolt.  The vectors are a deterministic spread
-- rather than random so every number in this file is reproducible.  `band` gives a
-- lexical token that selects 30 rows and `tag<g>` one that selects exactly one, both
-- needed further down.
CREATE TABLE vo (id serial, d wdoc, v wvec(8));
INSERT INTO vo(d, v)
  SELECT to_wdoc('order tag' || g || ' band' || (g % 10)),
         ('[' || (g % 97) || ',' || (g % 89) || ',' || (g % 83) || ','
               || (g % 79) || ',' || (g % 73) || ',' || (g % 71) || ','
               || (g % 67) || ',' || (g % 61) || ']')::wvec
    FROM generate_series(1, 300) g;
CREATE INDEX vo_weave ON vo USING weave (d, v);
ANALYZE vo;

-- One bolt, 300 lanes, metric l2 (WEAVE_METRIC_L2 == 1, the reloption default).
SELECT count(*) AS wefts, sum(nvec) AS lanes, min(metric) AS metric
  FROM weave_vec_meta('vo_weave');

-- ROW <-> DOCID, so a plan's rows can be compared with the scan SRF's output.
--
-- DOCID AND NOT WARP, and the difference is the reason this is a join rather than a
-- cast: a warp is SEGMENT-LOCAL (doc/ARCHITECTURE.md sect. 3), so warp 7 names a
-- different document in every bolt and the map would break the moment this index has
-- two -- which it does have by the end of this file.  A docid is the bolt-independent
-- name.  Both sides are derived by row_number() rather than from
-- WEAVE_OFFSET_FACTOR, which keeps the map independent of BLCKSZ: docids ascend with
-- ctid (include/weave/am.h, weave_tid_to_docid is monotone in (block, offset)) and
-- every row here has a non-NULL vector, so the n-th row in ctid order owns the n-th
-- docid in ascending order.  The count assertion is what says that derivation held.
-- enable_seqscan goes back ON for the map, and this is not cosmetic: `count(*)`
-- over `vo` is a scan with NEITHER a restriction nor an ordering clause, which
-- this index cannot serve -- rows whose indexed column is NULL have no entry, so a
-- full scan would undercount (src/am/am.c weave_costestimate prices that shape at
-- 1e12 and weave_gettuple refuses it).  With the seq scan DISABLED, PostgreSQL 18
-- compares disabled-node counts BEFORE costs, so it picks the priced-out Index Only
-- Scan anyway and the query hits the refusal.  PostgreSQL 17 does not.  The AM's
-- behaviour is deliberate and documented; what is wrong is asking a query-less scan
-- for a count while the alternative is disabled.
SET enable_seqscan = on;
CREATE TEMP TABLE vomap AS
  SELECT t.id, l.docid
    FROM (SELECT id, row_number() OVER (ORDER BY ctid) AS rn FROM vo) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('vo_weave')) l USING (rn);
SELECT count(*) = (SELECT count(*) FROM vo) AS map_covers_every_row FROM vomap;
SET enable_seqscan = off;

-- ============================================================================
-- (1) THE PLAN.  Index Scan with an Order By, no Sort.
-- ============================================================================
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5;

-- The LIMIT-LESS form too, because that is what the lexical channel supports: task
-- L7's sql/orderby.sql drains `ORDER BY d <=> q` with no LIMIT and with a cursor,
-- on the grounds that an amcanorderbyop scan must be able to return EVERY matching
-- tuple in order and the executor's LIMIT is what bounds it.  The vector ladder is
-- held to the same standard: PostgreSQL gives an access method no way to learn a
-- query's LIMIT, so correctness cannot depend on the planner's estimate of one.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec;

-- ...and a second query point, so the plan is not an artefact of one constant.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <-> '[90,80,70,60,50,40,30,20]'::wvec LIMIT 10;

-- ============================================================================
-- (2) THE ROWS EQUAL weave_vec_scan() AT THE SAME k.
--
-- weave_vec_scan() is the SRF that has been the only door into the vector channel
-- since V8, and F7 made the two share one scoring loop (weave_vec_topk_run,
-- include/weave/vector.h) precisely so that this comparison is not circular: it
-- cannot detect a scoring bug, because there is one scorer, and that is deliberate.
-- What it DOES pin is everything F7 added on top -- docid -> TID resolution, the
-- visibility probe, the widening ladder, and the order those three preserve.
--
-- THE SRF's ORDER, reproduced as `score DESC, segno, warp`: the top-k admits on
-- `s > incumbent` so an equal score does not displace one, and bolts are visited in
-- ascending segno and warps in ascending warp, so ties come out in arrival order,
-- which is exactly (segno, warp).
-- ============================================================================
CREATE TEMP TABLE voarm (arm text, ids int[]);

INSERT INTO voarm
  SELECT 'idx1', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 1) i;
INSERT INTO voarm
  SELECT 'srf1', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 1) s
    JOIN vomap m USING (docid);

INSERT INTO voarm
  SELECT 'idx5', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5) i;
INSERT INTO voarm
  SELECT 'srf5', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 5) s
    JOIN vomap m USING (docid);

-- k = 100 is past the first rung of the ladder: the initial width is
-- weave_ord_width(pg_weave.wand_initial_k), which floors at 64, so serving 100 rows
-- REQUIRES a widening and the concatenation of two passes must equal one wide pass.
INSERT INTO voarm
  SELECT 'idx100', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 100) i;
INSERT INTO voarm
  SELECT 'srf100', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 100) s
    JOIN vomap m USING (docid);

-- k LARGER THAN THE TABLE.  Both arms must terminate and return every row exactly
-- once -- the ladder's stop has to be a PROOF (the pass returned fewer hits than it
-- asked for, or it was as wide as the lane count), not a ceiling of the access
-- method's own, which is the bug weave_ord_grow() carries a paragraph about.
INSERT INTO voarm
  SELECT 'idx1000', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 1000) i;
INSERT INTO voarm
  SELECT 'srf1000', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 1000) s
    JOIN vomap m USING (docid);

-- The bare LIMIT-less form has to agree with LIMIT 1000 row for row as well.
INSERT INTO voarm
  SELECT 'idxall', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) i;

SELECT (SELECT ids FROM voarm WHERE arm = 'idx1')
         = (SELECT ids FROM voarm WHERE arm = 'srf1')    AS k1_index_equals_srf,
       (SELECT ids FROM voarm WHERE arm = 'idx5')
         = (SELECT ids FROM voarm WHERE arm = 'srf5')    AS k5_index_equals_srf,
       (SELECT ids FROM voarm WHERE arm = 'idx100')
         = (SELECT ids FROM voarm WHERE arm = 'srf100')  AS k100_index_equals_srf,
       (SELECT ids FROM voarm WHERE arm = 'idx1000')
         = (SELECT ids FROM voarm WHERE arm = 'srf1000') AS k1000_index_equals_srf,
       (SELECT ids FROM voarm WHERE arm = 'idxall')
         = (SELECT ids FROM voarm WHERE arm = 'idx1000') AS limitless_equals_limit1000;

-- Lengths, so an "equal" above that compared two NULLs would still fail here.
SELECT arm, coalesce(array_length(ids, 1), 0) AS n FROM voarm ORDER BY arm;

-- A narrow prefix is the prefix of a wide answer.  A widening EXTENDS the scan:
-- ordered[] survives, the rows already handed out are not re-probed and not
-- re-emitted, and nothing a wider pass newly finds may outrank them.
SELECT (SELECT ids FROM voarm WHERE arm = 'idx5')
         = (SELECT ids[1:5] FROM voarm WHERE arm = 'idx1000') AS top5_is_the_prefix_of_all;

-- ============================================================================
-- (3) THE QUANTIZER'S DIVERGENCE FROM AN EXACT FLOAT ORDERING, RECORDED AND NOT
--     ASSERTED AWAY.
--
-- AGREEMENT IS NOT THE PROPERTY, and there is now exactly ONE reason for that,
-- which is design and not defect: the index scores QUANTIZED reconstructions -- 4
-- bits per dimension by default -- while the heap `<->` scores exact float4s.  At 8
-- dimensions that reorders near-ties freely.  Asserting equality here would be
-- asserting the quantizer's recall, which test/hegel/test_quantize.c measures
-- properly and which no fixed expected output should pretend to pin.
--
-- THE PREVIOUS VERSION OF THIS SECTION WAS MEASURING SOMETHING ELSE AND CALLING IT
-- QUANTIZATION: it compared the index against `<=>`, a COSINE operator, while the
-- weft was ordered by l2 -- a metric mismatch, which is a wrong answer and not an
-- approximation, and which is why 24 of 25 positions differed.  Both arms now use
-- `<->`, the operator whose metric the index actually serves, so what is recorded
-- below is the quantizer alone.
--
-- So the number of differing positions is RECORDED.  A diff here means the
-- quantizer or the bound changed -- worth failing over -- and not that something is
-- broken.  This is the discipline sql/fuse_fallback.sql uses for the
-- lexical fallback's divergence from the pushdown, and AGENTS.md hard rule 8's
-- "record losses as prominently as wins" applied to an approximation.
--
-- EACH ARM IS EXPLAINED, because a recorded divergence between two plans is
-- worthless if one of them did not run the plan it claims: an "exact" arm answered
-- by the index would record 0 and prove nothing.
-- ============================================================================

-- The EXACT arm: no index path at all, so the executor evaluates <-> per row.
-- enable_seqscan goes back ON for this arm rather than leaving the Seq Scan
-- disabled, because PostgreSQL 18 reports disabled nodes in EXPLAIN and 17 does
-- not -- a version-dependent expected file for no gain.
SET enable_seqscan = on;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 25;
INSERT INTO voarm
  SELECT 'exact25', array_agg(id)
    FROM (SELECT id FROM vo
           ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec, id LIMIT 25) e;

-- The INDEX arm, same query, same k.
SET enable_seqscan = off;
SET enable_indexscan = on;
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 25;
INSERT INTO voarm
  SELECT 'index25', array_agg(id)
    FROM (SELECT id FROM vo
           ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 25) i;

SELECT count(*) AS positions_differing_index_vs_exact
  FROM voarm a, voarm b, generate_series(1, 25) g
 WHERE a.arm = 'index25' AND b.arm = 'exact25'
   AND a.ids[g] IS DISTINCT FROM b.ids[g];

-- The set overlap, as a number rather than a bound: positions may shuffle freely
-- while the candidate set stays right, and a rerank window (task V10) consumes
-- exactly that.  A collapse here is a bound or a scatter bug; a small wobble is
-- the quantizer.
SELECT count(*) AS overlap_of_index_top25_with_exact_top25
  FROM (SELECT unnest(ids) AS id FROM voarm WHERE arm = 'index25'
        INTERSECT
        SELECT unnest(ids) AS id FROM voarm WHERE arm = 'exact25') x;

-- ============================================================================
-- (4) A RESTRICTION CLAUSE ALONGSIDE THE VECTOR ORDER BY.
--
-- F7 does NOT fuse the two: folding a lexical restriction into the vector
-- channel's top-k IS the fused scorer, which is Phase F's own work and which
-- AGENTS.md hard rule 7 forbids building against a half-working channel.  So the
-- access method returns an admitted SUPERSET and sets xs_recheck, and the executor
-- re-evaluates the original qual against the heap tuple.  Without that flag an
-- Index Scan does not re-check a pushed-down qual and the query returns rows that
-- fail its WHERE clause.
--
-- `tag17` selects exactly ONE of the 300 rows, which makes this the strongest
-- available test of the ladder: the executor discards essentially everything, so
-- the scan is driven to PROVEN completeness rather than to a row count, and a
-- ladder that stopped at its first pass would return zero rows here.
-- ============================================================================
EXPLAIN (COSTS OFF)
SELECT id FROM vo WHERE d @@@ 'tag17'::wquery
 ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5;

SELECT count(*) AS restricted_rows,
       bool_and(d @@@ 'tag17'::wquery) AS every_row_satisfies_the_qual
  FROM (SELECT id, d FROM vo WHERE d @@@ 'tag17'::wquery
         ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5) s;

-- 30 rows through the same path, and the same two properties.
SELECT count(*) AS band3_rows,
       bool_and(d @@@ 'band3'::wquery) AS every_band3_row_satisfies_the_qual
  FROM (SELECT id, d FROM vo WHERE d @@@ 'band3'::wquery
         ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s;

-- ============================================================================
-- (5) RESCAN.  Every field weave_rescan() adds is reset there, and this is what
-- makes a leak visible: a nested loop calls amrescan once per outer row, so a
-- scan that kept the previous query vector, or the previous ladder width, or the
-- previous ordered[] array, answers every inner scan with the FIRST outer row's
-- neighbours -- plausible rows, a plausible row count, and a wrong answer that
-- exists only under a join.  Each arm is compared against weave_vec_scan(), which
-- starts from nothing on every call.
-- ============================================================================
CREATE TABLE voq (qid int, q wvec(8));
INSERT INTO voq VALUES
  (1, '[7,7,7,7,7,7,7,7]'),
  (2, '[90,80,70,60,50,40,30,20]'),
  (3, '[1,2,3,4,5,6,7,8]'),
  (4, '[0,0,0,0,0,0,0,0]');

-- A correlated subquery: the ordering operand is an outer reference, so the index
-- path is parameterized and is re-established per outer row.
--
-- enable_seqscan goes back ON for both plans in this section, for section (3)'s
-- reason: the OUTER relation here is `voq`, which has no index at all, so with the
-- seq scan disabled PostgreSQL 18 prints `Disabled: true` on it and PostgreSQL 17
-- does not -- a version-dependent expected file for no gain.  The INNER scan is the
-- one under test and it is chosen on cost, which is the stronger statement anyway.
SET enable_seqscan = on;
EXPLAIN (COSTS OFF)
SELECT qid, (SELECT id FROM vo ORDER BY vo.v <-> voq.q LIMIT 1) AS nn
  FROM voq ORDER BY qid;

SELECT count(*) AS correlated_rescan_disagreements
  FROM voq
 WHERE (SELECT id FROM vo ORDER BY vo.v <-> voq.q LIMIT 1)
       IS DISTINCT FROM
       (SELECT m.id FROM weave_vec_scan('vo_weave', voq.q, 1) s
          JOIN vomap m USING (docid));

-- ... and a genuine nested loop over LATERAL, at k = 3 so the inner scan returns
-- several rows per rescan and an ordpos that was not reset would show up as a
-- short or a duplicated group.
EXPLAIN (COSTS OFF)
SELECT q.qid, n.id FROM voq q,
  LATERAL (SELECT id FROM vo ORDER BY vo.v <-> q.q LIMIT 3) n;

SET enable_seqscan = off;
SELECT count(*) AS lateral_rows,
       count(DISTINCT (qid, id)) AS lateral_distinct_pairs
  FROM (SELECT q.qid, n.id FROM voq q,
          LATERAL (SELECT id FROM vo ORDER BY vo.v <-> q.q LIMIT 3) n) t;

SELECT count(*) AS lateral_rescan_disagreements
  FROM voq q
 WHERE (SELECT array_agg(n.id)
          FROM (SELECT id FROM vo ORDER BY vo.v <-> q.q LIMIT 3) n)
       IS DISTINCT FROM
       (SELECT array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
          FROM weave_vec_scan('vo_weave', q.q, 3) s JOIN vomap m USING (docid));

-- ============================================================================
-- (6) SEVERAL BOLTS, MERGED INTO ONE ORDERING.
--
-- An oversized document bypasses the pending buffer and becomes its own
-- one-document segment (sql/pendingvec.sql), which is how this file gets a second
-- bolt without a merge.  A warp is segment-local, so an ordering that forgot to
-- merge the bolts would return one bolt's ranking, or the same warp twice -- which
-- is why the map above is keyed on docid.
-- ============================================================================
INSERT INTO vo(d, v)
  SELECT to_wdoc(string_agg('oversized' || g, ' ')),
         '[800,800,800,800,800,800,800,800]'
    FROM generate_series(1, 3000) g;

SELECT count(*) AS wefts, sum(nvec) AS lanes FROM weave_vec_meta('vo_weave');

DROP TABLE vomap;
SET enable_seqscan = on;    -- the query-less count again; see the first vomap block
CREATE TEMP TABLE vomap AS
  SELECT t.id, l.docid
    FROM (SELECT id, row_number() OVER (ORDER BY ctid) AS rn FROM vo) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('vo_weave')) l USING (rn);
SELECT count(*) = (SELECT count(*) FROM vo) AS map_covers_every_row_2 FROM vomap;
SET enable_seqscan = off;

-- Every lane of both bolts, in one ordering, each row once.
SELECT count(*) AS rows_over_two_bolts,
       count(DISTINCT id) AS distinct_rows_over_two_bolts
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s;

-- ... and it is the same ordering the SRF produces over the same two bolts.
SELECT (SELECT array_agg(id)
          FROM (SELECT id FROM vo
                 ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 10) i)
     = (SELECT array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
          FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 10) s
          JOIN vomap m USING (docid))
       AS two_bolt_top10_index_equals_srf;

-- The oversized row is an exact match for its own vector and lives in the SECOND
-- bolt, so a query at that point must reach it -- an ordering that scanned only
-- bolt 0 would return 300 rows of the wrong ranking and no error.
SELECT (SELECT id FROM vo ORDER BY v <-> '[800,800,800,800,800,800,800,800]'::wvec
         LIMIT 1) = (SELECT max(id) FROM vo)
       AS second_bolt_row_is_reachable;

-- ============================================================================
-- (7) AN INSERT THAT LANDS IN THE PENDING BUFFER.  doc/GAPS.md G29, CLOSED
-- 2026-09-30.
--
-- The vector ordering scan now reads the pending list's vectors and scores them
-- exactly, inside the same generation bracket as the bolt pass, and merges them
-- into the lane ranking (weave_vec_collect_pending).  Until then a row was in
-- LEXICAL answers and absent from VECTOR ones between its INSERT and the next
-- flush, and this section asserted that absence as 301 rows / 0, "so that closing
-- it shows up here as a diff rather than as nothing".  It did: 302 / 1.
--
-- Asserted as a ROW COUNT rather than as a ranking, because a count is exact and
-- pins the mechanism: the vector ordering scan returns one row per live lane or
-- pending vector that resolves to a visible tuple.
-- ============================================================================
INSERT INTO vo(d, v)
  VALUES (to_wdoc('order pendingtoken'), '[7,7,7,7,7,7,7,7]');

-- seqscan back ON for this count: it is a query-less scan of an indexed table, which
-- this AM prices out and refuses, and PG18 picks the refused path anyway when the seq
-- scan is disabled.  See the first vomap block.
SET enable_seqscan = on;
SELECT count(*) AS table_rows FROM vo;
SET enable_seqscan = off;
SELECT sum(nvec) AS lanes_before_flush FROM weave_vec_meta('vo_weave');

-- The lexical channel sees it immediately: weave_collect_matches() walks the
-- pending page itself; until G29 closed, this was the only channel that did.
SELECT count(*) AS lexical_sees_the_pending_row
  FROM vo WHERE d @@@ 'pendingtoken'::wquery;

-- The vector ordering scan sees it too: every row of the table, the inserted one
-- among them.  The EXPLAIN is load-bearing and not decoration: a Seq Scan would
-- also report 302, so without it this would not show the INDEX answered.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec;
SELECT count(*) AS vec_rows_before_flush
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s;
SELECT count(*) AS pending_row_in_vec_answer_before_flush
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s
 WHERE s.id = (SELECT max(id) FROM vo);

-- After a flush the row is in a real weft (G23 is closed at the segment) and the
-- ordering covers it.
SELECT weave_merge('vo_weave');
SELECT sum(nvec) AS lanes_after_flush FROM weave_vec_meta('vo_weave');
SELECT count(*) AS vec_rows_after_flush
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s;
SELECT count(*) AS pending_row_in_vec_answer_after_flush
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s
 WHERE s.id = (SELECT max(id) FROM vo);

-- ============================================================================
-- (8) VISIBILITY, AND WHERE THE TIDs COME FROM.
--
-- AN ACCESS METHOD MAY ONLY RETURN HOT-CHAIN ROOT TIDs, AND RETURNING A PHYSICAL
-- ONE FAILS SILENTLY: heap_hot_search_buffer() -- what table_index_fetch_tuple()
-- and a bitmap heap scan both use to resolve a TID an index gave them -- REFUSES a
-- heap-only tuple as a chain start, so a physical TID resolves to NOTHING.  The
-- index reports the right row count and the heap access above it reports zero, with
-- no error and no warning, and only for rows that have been updated -- so a corpus
-- built with INSERT alone never sees it (AGENTS.md).
--
-- THE TIDs HERE ARE NOT MANUFACTURED FROM THE HEAP, which is why no
-- heap_get_root_tuples() pass appears in the vector path: weave_docid_to_tid()
-- inverts weave_tid_to_docid(), and the docid it inverts came off the weft's warp
-- map, written from the TID the BUILD CALLBACK or weave_insert() was handed.  Those
-- are already roots -- table_index_build_scan() reports the chain root for a
-- heap-only tuple -- so the mapping is root-preserving.  Contrast
-- weave_cgram_heapscan(), which reads the heap itself and therefore must call
-- heap_get_root_tuples(); that is the site whose omission cost a debugging round.
--
-- The shape below is sql/cgram.sql's, because it is the one that exposes the bug:
-- the UPDATE runs BEFORE the index exists, so live tuples are heap-only and the
-- build records their roots.  How many of the 40 updates are HOT depends on
-- free space and is not asserted; what is asserted is that EVERY visible row is
-- returned, which is 40 under correct behaviour and fewer under the bug for
-- however many chains are heap-only.
-- ============================================================================
CREATE TABLE voh (id serial, d wdoc, v wvec(8));
INSERT INTO voh(d) SELECT to_wdoc('hot tag' || g) FROM generate_series(1, 40) g;
UPDATE voh SET v = ('[' || (id % 13) || ',' || (id % 11) || ',' || (id % 7) || ','
                        || (id % 5) || ',' || (id % 3) || ',' || (id % 17) || ','
                        || (id % 19) || ',' || (id % 23) || ']')::wvec;
CREATE INDEX voh_weave ON voh USING weave (d, v);
ANALYZE voh;
SET enable_seqscan = on;    -- query-less count; see the first vomap block
SELECT count(*) AS hot_visible_rows FROM voh;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM voh ORDER BY v <-> '[5,5,5,5,5,5,5,5]'::wvec;
SELECT count(*) AS vec_rows_over_hot_chains
  FROM (SELECT id FROM voh ORDER BY v <-> '[5,5,5,5,5,5,5,5]'::wvec) s;
DROP TABLE voh;

-- A DELETE that is not yet vacuumed: the docid is still in the weft and the heap
-- probe is what must drop it.  The count is the whole assertion -- a scan that
-- skipped the probe would return the dead rows too.
DELETE FROM vo WHERE id IN (SELECT id FROM vo ORDER BY id LIMIT 10);
SET enable_seqscan = on;    -- query-less count; see the first vomap block
SELECT count(*) AS visible_rows_after_delete FROM vo;
SET enable_seqscan = off;
SELECT sum(nvec) AS lanes_still_in_the_weft FROM weave_vec_meta('vo_weave');
SELECT count(*) AS vec_rows_after_delete
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec) s;

SELECT bool_and(ok) AS all_invariants_hold FROM weave_check('vo_weave');

-- ============================================================================
-- (9) AN INDEX WITH NO VECTOR CHANNEL falls through cleanly.  The operator is on
-- wvec, so it can only be matched to a wvec index column; what this asserts is the
-- other half -- a wvec COLUMN whose weft is not there.  A table whose every vector
-- is NULL has a weft with no live lane, and the ordering must not error, because an
-- ordering path over an empty channel is a legitimate plan.  It returns the rows,
-- at a NULL distance, as the heap does: SQL's ORDER BY never filters, and NULLs
-- sort last (doc/GAPS.md G56).  It returned 0 before the vector route padded.
-- ============================================================================
CREATE TABLE voe (id serial, d wdoc, v wvec(8));
INSERT INTO voe(d, v) SELECT to_wdoc('empty tag' || g), NULL FROM generate_series(1, 20) g;
CREATE INDEX voe_weave ON voe USING weave (d, v);
SELECT count(*) AS wefts, sum(nvec) AS lanes,
       (SELECT count(*) FILTER (WHERE live) FROM weave_vec_lanes('voe_weave')) AS live_lanes
  FROM weave_vec_meta('voe_weave');
EXPLAIN (COSTS OFF)
SELECT id FROM voe ORDER BY v <-> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 5;
SELECT count(*) AS rows_from_an_all_null_vector_column
  FROM (SELECT id FROM voe ORDER BY v <-> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 5) s;

-- And an index with NO wvec column at all: the operator cannot be matched to any of
-- its columns, so there is no index path and the planner sorts.  That is the
-- pre-F7 behaviour, still correct, and it must be a plan rather than an error.
-- enable_seqscan goes back ON for it, because a DISABLED Seq Scan is reported by
-- PostgreSQL 18's EXPLAIN and not by 17's, which would make this expected file
-- depend on the major.
SET enable_seqscan = on;
CREATE TABLE vol (id serial, d wdoc, v wvec(8));
INSERT INTO vol(d, v) SELECT to_wdoc('lexonly tag' || g),
                             ('[' || g || ',1,2,3,4,5,6,7]')::wvec
                        FROM generate_series(1, 20) g;
CREATE INDEX vol_weave ON vol USING weave (d);
EXPLAIN (COSTS OFF)
SELECT id FROM vol ORDER BY v <-> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3;
SELECT count(*) AS rows_without_a_vector_channel
  FROM (SELECT id FROM vol ORDER BY v <-> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3) s;
SET enable_seqscan = off;

-- ============================================================================
-- (10) A METRIC MISMATCH IS REFUSED, NOT ANSWERED.
--
-- Each member names a metric and a weft is scored in exactly one of them, so `<->`
-- against an `ip` index -- and `<#>` against an `l2` one -- asks for an ordering
-- this index cannot produce.  Answering it in the metric the weft does carry is a
-- WRONG ANSWER for every row, silently, which is what F7 as first shipped did: it
-- made `<=>` the member while the weft ordered by l2, and section (3) above was
-- measuring that mismatch and calling it quantization.  So weave_rescan() throws,
-- naming the operator and the index's metric.
--
-- The metric is a RELOPTION, which path generation never looks at, so the planner
-- cannot decline to build the path and the scan is the last place that can refuse;
-- doc/specs/VECTOR_CHANNEL.md sect. 8b holds the per-metric opclass split that would
-- make it a planner decision (no member, no path, no error).
--
-- Bare statements, so the message itself lands in the expected output -- the
-- convention of expected/vecindex.out and expected/vecscan.out.  The EXPLAIN over
-- each is load-bearing rather than decoration: it is what says the error came from
-- an index ordering path, not from something the planner never built.
-- ============================================================================
CREATE TABLE voip (id serial, d wdoc, v wvec(8));
INSERT INTO voip(d, v)
  SELECT to_wdoc('ipmetric tag' || g),
         ('[' || g || ',1,2,3,4,5,6,7]')::wvec
    FROM generate_series(1, 20) g;
CREATE INDEX voip_weave ON voip USING weave (d, v) WITH (metric = 'ip');
ANALYZE voip;

EXPLAIN (COSTS OFF)
SELECT id FROM voip ORDER BY v <-> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3;
SELECT id FROM voip ORDER BY v <-> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3;

-- ... and the mirror image, on the default (l2) index of the main table.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <#> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 3;
SELECT id FROM vo ORDER BY v <#> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 3;

-- Each index serves its OWN operator, which is what makes the two refusals above a
-- statement about the MISMATCH rather than about either member being unusable.
SELECT count(*) AS ip_index_serves_ip
  FROM (SELECT id FROM voip ORDER BY v <#> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3) s;
SELECT count(*) AS l2_index_serves_l2
  FROM (SELECT id FROM vo ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 3) s;

-- ============================================================================
-- (11) `<=>` IS NOT A MEMBER, AND THE PLAN SAYS SO.
--
-- The index deliberately does not claim cosine.  The scan core serves ip and l2 and
-- refuses cosine (include/weave/vecscan.h), and a cosine index cannot exist at all
-- -- CREATE INDEX ... WITH (metric = 'cosine') errors (expected/vecscan.out) -- so a
-- `<=>` member could only ever be answered in some other metric.  With no member
-- there is no pathkey match, so the honest plan is a Sort over a Seq Scan, and this
-- is the one pgvector-shaped query this index does not accelerate.
--
-- enable_seqscan stays ON for this arm, for section (3)'s reason: PostgreSQL 18
-- reports disabled nodes in EXPLAIN and 17 does not, which would make this expected
-- file depend on the major.
-- ============================================================================
SET enable_seqscan = on;
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5;
SELECT count(*) AS rows_from_the_cosine_sort
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5) s;
SET enable_seqscan = off;

-- ============================================================================
-- (12) EVERY ROW, AS THE HEAP ORDERS IT -- doc/GAPS.md G56 on the vector and
-- edit-distance routes, and G29 for the pending rows.
--
-- SQL's ORDER BY never filters, so an ordering scan must return what a Seq Scan +
-- Sort would: rows whose vector is NULL come out LAST (at a NULL distance), and for
-- `<@>` a document with no term is +Infinity and a NULL document is NULL.  Until
-- these routes padded, the vector scan dropped NULL-vector rows and `<@>` dropped
-- both.  Rows inserted after the build sit in the pending list, and the vector scan
-- must find them too -- here they are the NEAREST rows, so they come first.
-- Compared against the heap as sets, plus the class order, plus the distinct-id
-- count (a row emitted twice would pass the set comparison).
-- ============================================================================
CREATE TABLE vg (id serial, d wdoc, v wvec(4));
INSERT INTO vg(d, v) SELECT to_wdoc('simple', 'tag' || g), ('[' || g || ',1,1,1]')::wvec
  FROM generate_series(1, 300) g;
INSERT INTO vg(d, v) SELECT to_wdoc('simple', 'nullvec' || g), NULL FROM generate_series(1, 20) g;
INSERT INTO vg(d, v) SELECT NULL, ('[' || g || ',2,2,2]')::wvec FROM generate_series(1, 10) g;
INSERT INTO vg(d, v) SELECT to_wdoc('simple', ''), '[5,5,5,5]'::wvec FROM generate_series(1, 5) g;
CREATE INDEX vg_weave ON vg USING weave (d, v);
INSERT INTO vg(d, v) SELECT to_wdoc('simple', 'pend' || g), ('[0,0,0,' || g || ']')::wvec
  FROM generate_series(1, 7) g;
INSERT INTO vg(d, v) VALUES (to_wdoc('simple', 'pendnull'), NULL);
SELECT npending > 0 AS rows_are_pending FROM (SELECT count(*) AS npending FROM vg WHERE id > 335) s;

EXPLAIN (COSTS OFF) SELECT id FROM vg ORDER BY v <-> '[0,0,0,0]'::wvec;
CREATE TEMP TABLE vgi AS
  SELECT row_number() OVER () AS rn, id FROM (SELECT id FROM vg ORDER BY v <-> '[0,0,0,0]'::wvec) s;
SET enable_seqscan = on;
SELECT (SELECT count(*) FROM vgi) = (SELECT count(*) FROM vg) AS vec_every_row,
       (SELECT count(DISTINCT id) FROM vgi) = (SELECT count(*) FROM vg) AS vec_no_row_twice,
       (SELECT array_agg(id ORDER BY rn) FROM vgi WHERE rn <= 3) AS vec_first3,
       (SELECT bool_and(rn > (SELECT count(*) FROM vg WHERE v IS NOT NULL))
          FROM vgi JOIN vg USING (id) WHERE vg.v IS NULL) AS vec_nulls_last,
       (SELECT count(*) FROM vgi WHERE id BETWEEN 336 AND 342) AS vec_pending_rows_answered;
SET enable_seqscan = off;
-- a restriction the ranked phase does not apply: every heap row that passes it
SELECT count(*) AS vec_filtered
  FROM (SELECT id FROM vg WHERE d @@@ 'tag5 | nullvec3 | pend2'
         ORDER BY v <-> '[0,0,0,0]'::wvec) s;
-- THE FRONTIER.  A narrow first pass (wand_initial_k = 1) sees only the nearest lanes,
-- so a pending row FARTHER than every lane must wait for the pass that has seen them
-- all: emitted with the first pass it would come out ahead of nearer lanes.  The row
-- below is the farthest vector in the table by orders of magnitude, so it is last
-- among every row that HAS A LANE OR A PENDING ITEM whatever the quantizer does.
-- Killed the mutant that drops the frontier (it put this row at rank 5).
--
-- The ten NULL-DOCUMENT rows are excluded from that rank, and they are a loss
-- recorded here rather than a pass: the build skips a row whose wdoc is NULL, so its
-- vector is in no weft, and it comes out of the padding at +Infinity -- AFTER this
-- row, where a heap sort would put it by its real distance.  Scoring padded rows
-- exactly would mean emitting a distance below ones already handed out, which the
-- executor refuses ("index returned tuples in wrong order").  doc/GAPS.md G56.
INSERT INTO vg(d, v) VALUES (to_wdoc('simple', 'farpend'), '[1000,1000,1000,1000]');
SET pg_weave.wand_initial_k = 1;
CREATE TEMP TABLE vgf AS
  SELECT row_number() OVER () AS rn, id FROM (SELECT id FROM vg ORDER BY v <-> '[0,0,0,0]'::wvec) s;
RESET pg_weave.wand_initial_k;
SET enable_seqscan = on;
SELECT (SELECT rn FROM vgf WHERE id = (SELECT max(id) FROM vg))
       = (SELECT count(*) FROM vg WHERE v IS NOT NULL AND d IS NOT NULL) AS far_pending_row_is_last_indexed,
       (SELECT min(rn) FROM vgf JOIN vg USING (id) WHERE vg.d IS NULL AND vg.v IS NOT NULL)
       > (SELECT rn FROM vgf WHERE id = (SELECT max(id) FROM vg)) AS null_doc_vectors_come_after,
       (SELECT count(*) FROM vgf) = (SELECT count(*) FROM vg) AS narrow_every_row;
SET enable_seqscan = off;
DROP TABLE vgf;
-- nested-loop rescans: three outer rows, each the whole table
SET enable_hashjoin = off;
SET enable_mergejoin = off;
-- the table size is taken FIRST, with the seq scan on: a query-less count is the
-- G39 shape PostgreSQL 18 sends to the refused index path when the seq scan is off
SET enable_seqscan = on;
SELECT count(*) AS vg_rows FROM vg \gset
SET enable_seqscan = off;
SELECT count(*) = 3 * :vg_rows AS vec_rescans_every_row
  FROM generate_series(1, 3) o,
       LATERAL (SELECT id FROM vg ORDER BY v <-> ('[' || o || ',0,0,0]')::wvec) s;
RESET enable_hashjoin;
RESET enable_mergejoin;

EXPLAIN (COSTS OFF) SELECT id FROM vg ORDER BY d <@> 'tag1';
CREATE TEMP TABLE vge AS
  SELECT row_number() OVER () AS rn, id, d <@> 'tag1' AS dist
    FROM (SELECT id, d FROM vg ORDER BY d <@> 'tag1') s;
SET enable_seqscan = on;
SELECT (SELECT count(*) FROM vge) = (SELECT count(*) FROM vg) AS edist_every_row,
       (SELECT count(DISTINCT id) FROM vge) = (SELECT count(*) FROM vg) AS edist_no_row_twice,
       (SELECT count(*) FROM vge WHERE dist = 'Infinity') AS edist_term_free_rows,
       (SELECT count(*) FROM vge WHERE dist IS NULL) AS edist_null_docs,
       (SELECT bool_and(a.dist <= b.dist OR b.dist IS NULL)
          FROM vge a JOIN vge b ON b.rn = a.rn + 1 WHERE a.dist IS NOT NULL) AS edist_ascending;
SET enable_seqscan = off;
DROP TABLE vgi;
DROP TABLE vge;
DROP TABLE vg;

DROP TABLE voip;
DROP TABLE vol;
DROP TABLE voe;
DROP TABLE voq;
DROP TABLE vo;

-- ============================================================================
-- (13) A RECYCLED CTID IS NOT THE DEAD ROW -- doc/GAPS.md G79.
--
-- After DELETE + VACUUM a row's docid is TOMBSTONED in its bolt, its lane is still
-- in the weft, and the heap is free to give its ctid to a new row.  The heap probe
-- of that ctid then finds the NEW row, visible, so a vector pass that skips the
-- tombstones ranks the new row at the DEAD row's distance: the reproducer's farthest
-- vector came out FIRST.  And because the new row is also in the pending list (or a
-- newer bolt) it was emitted a SECOND time at its own distance, which a LIMIT hid.
--
-- Rows 1 and 2 are deleted; row 100 (the farthest vector by orders of magnitude)
-- takes row 1's ctid and row 101 (a NULL vector, so a PADDING row) takes row 2's.
-- The ctids and the tombstone count are printed first, because the assertions below
-- prove nothing unless the reuse happened and VACUUM recorded it.  Each shape is
-- compared against the heap's own answer: the plain vector ORDER BY with and without
-- a LIMIT, the fused ORDER BY, `<@>` and the lexical channel (which already applied
-- tombstones, pinned here so they stay that way), and finally the same table after a
-- second VACUUM has flushed rows 100/101 into a NEWER bolt -- where the docid is
-- tombstoned in bolt 0 and LIVE in bolt 1, so a tombstone must suppress only its own
-- bolt's lane.
-- ============================================================================
CREATE FUNCTION pg_temp.wait_for_horizon() RETURNS void LANGUAGE plpgsql AS $$
DECLARE x xid := (txid_current() % 4294967296)::text::xid; i int;
BEGIN
  FOR i IN 1 .. 600 LOOP
    IF NOT EXISTS (SELECT 1 FROM pg_stat_activity
                    WHERE datname = current_database() AND pid <> pg_backend_pid()
                      AND (age(backend_xmin) > age(x) OR age(backend_xid) > age(x))) THEN
      RETURN;
    END IF;
    PERFORM pg_sleep(0.1);
    PERFORM pg_stat_clear_snapshot();  -- else every pass re-reads the first
  END LOOP;
  RAISE NOTICE 'wait_for_horizon: an older snapshot was still held after 60 s';
END $$;

-- The heap's order of a query, numbered, with the index paths off.
CREATE FUNCTION pg_temp.g79_heap(q text) RETURNS TABLE (rn bigint, id int)
LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
  PERFORM set_config('enable_seqscan', 'on', true);
  PERFORM set_config('enable_indexscan', 'off', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  rn := 0;
  FOR r IN EXECUTE q LOOP rn := rn + 1; id := r.id; RETURN NEXT; END LOOP;
END $$;
-- ...and the index's, with the seq scan off; the query runs AS WRITTEN.
CREATE FUNCTION pg_temp.g79_idx(q text) RETURNS TABLE (rn bigint, id int)
LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
  PERFORM set_config('enable_seqscan', 'off', true);
  PERFORM set_config('enable_indexscan', 'on', true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  rn := 0;
  FOR r IN EXECUTE q LOOP rn := rn + 1; id := r.id; RETURN NEXT; END LOOP;
END $$;
-- Index vs heap for one query: every row once, the farthest row and the NULL-vector
-- row at the heap's rank, and the first three as the heap has them.
CREATE FUNCTION pg_temp.g79_check(q text,
    OUT n_index bigint, OUT n_heap bigint, OUT once bool,
    OUT far_rank_as_heap bool, OUT nullvec_rank_as_heap bool,
    OUT index_top3 int[], OUT heap_top3 int[])
LANGUAGE plpgsql AS $$
BEGIN
  CREATE TEMP TABLE g79_i AS SELECT * FROM pg_temp.g79_idx(q);
  CREATE TEMP TABLE g79_h AS SELECT * FROM pg_temp.g79_heap(q);
  n_index := (SELECT count(*) FROM g79_i);
  n_heap := (SELECT count(*) FROM g79_h);
  once := n_index = (SELECT count(DISTINCT id) FROM g79_i);
  far_rank_as_heap := (SELECT array_agg(rn) FROM g79_i WHERE id = 100)
                    = (SELECT array_agg(rn) FROM g79_h WHERE id = 100);
  nullvec_rank_as_heap := (SELECT array_agg(rn) FROM g79_i WHERE id = 101)
                        = (SELECT array_agg(rn) FROM g79_h WHERE id = 101);
  index_top3 := (SELECT array_agg(id ORDER BY rn) FROM g79_i WHERE rn <= 3);
  heap_top3 := (SELECT array_agg(id ORDER BY rn) FROM g79_h WHERE rn <= 3);
  DROP TABLE g79_i;
  DROP TABLE g79_h;
END $$;

CREATE TABLE g79 (id int, body wdoc, emb wvec(4)) WITH (autovacuum_enabled = off);
INSERT INTO g79 SELECT g, to_wdoc('simple', 'common w' || g),
                       ('[' || g || ',' || g || ',' || g || ',' || g || ']')::wvec
  FROM generate_series(1, 20) g;
CREATE INDEX g79_w ON g79 USING weave (body, emb);
DELETE FROM g79 WHERE id IN (1, 2);
DO $$ BEGIN PERFORM pg_temp.wait_for_horizon(); END $$;   -- G60
VACUUM g79;                                               -- tombstones (0,1), (0,2)
INSERT INTO g79 VALUES (100, to_wdoc('simple', 'common new'), '[1000,1000,1000,1000]'),
                       (101, to_wdoc('simple', 'common nullvec'), NULL);
SET enable_seqscan = on;
SELECT id, ctid FROM g79 WHERE id >= 100 ORDER BY id;     -- the reuse happened
SELECT ndeleted AS g79_tombstones, weave_index_nsegments('g79_w') AS g79_bolts
  FROM weave_index_stats('g79_w');
SET enable_seqscan = off;

-- the reproducer, and the plan that answers it
EXPLAIN (COSTS OFF) SELECT id FROM g79 ORDER BY emb <-> '[0,0,0,0]'::wvec LIMIT 3;
SELECT array_agg(id) AS recycled_limit3
  FROM (SELECT id FROM g79 ORDER BY emb <-> '[0,0,0,0]'::wvec LIMIT 3) s;
-- no LIMIT: every row exactly once, row 100 19th, row 101 (padding) last
SELECT * FROM pg_temp.g79_check('SELECT id FROM g79 ORDER BY emb <-> ''[0,0,0,0]''::wvec');
-- the fused route, which applied tombstones before this fix and must keep doing so
EXPLAIN (COSTS OFF)
SELECT id FROM g79 ORDER BY fuse(body <=> 'common'::wquery, emb <-> '[0,0,0,0]'::wvec);
SELECT * FROM pg_temp.g79_check(
  'SELECT id FROM g79 ORDER BY fuse(body <=> ''common''::wquery, emb <-> ''[0,0,0,0]''::wvec)');
-- `<@>`: the dead row 1 held the term `w1` at distance 0, row 100 is at distance 3.
-- Compared as the sequence of the operator's own distances, which the ties in it do
-- not perturb (sql/edist.sql).
EXPLAIN (COSTS OFF) SELECT id FROM g79 ORDER BY body <@> 'w1';
SELECT (SELECT array_agg(g.body <@> 'w1' ORDER BY i.rn)
          FROM pg_temp.g79_idx('SELECT id FROM g79 ORDER BY body <@> ''w1''') i
          JOIN g79 g USING (id))
       = (SELECT array_agg(g.body <@> 'w1' ORDER BY h.rn)
            FROM pg_temp.g79_heap('SELECT id FROM g79 ORDER BY body <@> ''w1''') h
            JOIN g79 g USING (id)) AS edist_distances_as_heap,
       (SELECT count(*) FROM pg_temp.g79_idx('SELECT id FROM g79 ORDER BY body <@> ''w1''')) AS edist_rows;
-- lexical: weave_search() enters the scan machinery with no executor recheck behind it
SELECT (SELECT count(*) FROM weave_search('g79_w', 'w1', 10)) AS lex_dead_term_hits,
       (SELECT count(*) FROM weave_search('g79_w', 'new', 10)) AS lex_new_term_hits,
       (SELECT array_agg(id) FROM pg_temp.g79_idx(
          'SELECT id FROM g79 WHERE body @@@ ''w1 | w3'' ORDER BY body <=> ''w1 | w3''')) AS lex_ranked;

-- A NEWER BOLT: the second VACUUM folds rows 100/101 into bolt 1, under the docids
-- bolt 0 has tombstoned.  Row 100 must be found there -- a tombstone applied across
-- bolts would drop it to the padding at +Infinity and make it LAST for its own vector.
VACUUM g79;
SET enable_seqscan = on;
SELECT weave_index_nsegments('g79_w') AS g79_bolts_after_flush;
SET enable_seqscan = off;
SELECT array_agg(id) AS own_vector_limit2
  FROM (SELECT id FROM g79 ORDER BY emb <-> '[1000,1000,1000,1000]'::wvec LIMIT 2) s;
SELECT * FROM pg_temp.g79_check('SELECT id FROM g79 ORDER BY emb <-> ''[0,0,0,0]''::wvec');
SELECT * FROM pg_temp.g79_check(
  'SELECT id FROM g79 ORDER BY fuse(body <=> ''common''::wquery, emb <-> ''[0,0,0,0]''::wvec)');
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
DROP TABLE g79;
