-- F7: the vector channel's ORDER BY operator.
--
-- WHAT WAS MISSING.  wvec_weave_ops was created `STORAGE wvec` and nothing else
-- (0.7.0--0.8.0), so the planner had no ordering operator to match a pathkey
-- against and `ORDER BY embedding <=> $1 LIMIT 10` -- the first query shape any
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
CREATE TEMP TABLE vomap AS
  SELECT t.id, l.docid
    FROM (SELECT id, row_number() OVER (ORDER BY ctid) AS rn FROM vo) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('vo_weave')) l USING (rn);
SELECT count(*) = (SELECT count(*) FROM vo) AS map_covers_every_row FROM vomap;

-- ============================================================================
-- (1) THE PLAN.  Index Scan with an Order By, no Sort.
-- ============================================================================
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5;

-- The LIMIT-LESS form too, because that is what the lexical channel supports: task
-- L7's sql/orderby.sql drains `ORDER BY d <=> q` with no LIMIT and with a cursor,
-- on the grounds that an amcanorderbyop scan must be able to return EVERY matching
-- tuple in order and the executor's LIMIT is what bounds it.  The vector ladder is
-- held to the same standard: PostgreSQL gives an access method no way to learn a
-- query's LIMIT, so correctness cannot depend on the planner's estimate of one.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec;

-- ...and a second query point, so the plan is not an artefact of one constant.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[90,80,70,60,50,40,30,20]'::wvec LIMIT 10;

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
    FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 1) i;
INSERT INTO voarm
  SELECT 'srf1', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 1) s
    JOIN vomap m USING (docid);

INSERT INTO voarm
  SELECT 'idx5', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5) i;
INSERT INTO voarm
  SELECT 'srf5', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 5) s
    JOIN vomap m USING (docid);

-- k = 100 is past the first rung of the ladder: the initial width is
-- weave_ord_width(pg_weave.wand_initial_k), which floors at 64, so serving 100 rows
-- REQUIRES a widening and the concatenation of two passes must equal one wide pass.
INSERT INTO voarm
  SELECT 'idx100', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 100) i;
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
    FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 1000) i;
INSERT INTO voarm
  SELECT 'srf1000', array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
    FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 1000) s
    JOIN vomap m USING (docid);

-- The bare LIMIT-less form has to agree with LIMIT 1000 row for row as well.
INSERT INTO voarm
  SELECT 'idxall', array_agg(id)
    FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) i;

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
-- (3) THE DIVERGENCE FROM AN EXACT FLOAT ORDERING, RECORDED AND NOT ASSERTED AWAY.
--
-- AGREEMENT IS NOT THE PROPERTY, and there are TWO independent reasons, both of
-- which are design, not defect:
--
--   1. The index scores QUANTIZED reconstructions -- 4 bits per dimension by
--      default -- while the heap `<=>` scores exact float4s.  At 8 dimensions that
--      reorders near-ties freely.  Asserting equality here would be asserting the
--      quantizer's recall, which test/hegel/test_quantize.c measures properly and
--      which no fixed expected output should pretend to pin.
--   2. `<=>` is NAMED for cosine distance, and the weft's metric is the `metric`
--      reloption, default l2.  include/weave/vecscan.h REFUSES cosine because a
--      sound compressed-domain bound for it has to switch on the sign of the
--      numerator and no maximum true norm is stored, and a channel with an unsound
--      bound violates contract (C2) SILENTLY.  For unit-normalized vectors --
--      what the embedding models this is aimed at emit -- cosine, l2 and inner
--      product induce the same ordering and the two agree; these vectors are not
--      normalized, so they do not.  doc/specs/VECTOR_CHANNEL.md sect. 8b holds the
--      eventual fix, one operator family per metric (wvec_l2_ops, wvec_ip_ops).
--
-- So the number of differing positions is RECORDED.  A diff here means the
-- quantizer, the bound or the metric changed -- worth failing over -- and not that
-- something is broken.  This is the discipline sql/fuse_fallback.sql uses for the
-- lexical fallback's divergence from the pushdown, and AGENTS.md hard rule 8's
-- "record losses as prominently as wins" applied to an approximation.
--
-- EACH ARM IS EXPLAINED, because a recorded divergence between two plans is
-- worthless if one of them did not run the plan it claims: an "exact" arm answered
-- by the index would record 0 and prove nothing.
-- ============================================================================

-- The EXACT arm: no index path at all, so the executor evaluates <=> per row.
-- enable_seqscan goes back ON for this arm rather than leaving the Seq Scan
-- disabled, because PostgreSQL 18 reports disabled nodes in EXPLAIN and 17 does
-- not -- a version-dependent expected file for no gain.
SET enable_seqscan = on;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 25;
INSERT INTO voarm
  SELECT 'exact25', array_agg(id)
    FROM (SELECT id FROM vo
           ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec, id LIMIT 25) e;

-- The INDEX arm, same query, same k.
SET enable_seqscan = off;
SET enable_indexscan = on;
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 25;
INSERT INTO voarm
  SELECT 'index25', array_agg(id)
    FROM (SELECT id FROM vo
           ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 25) i;

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
 ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5;

SELECT count(*) AS restricted_rows,
       bool_and(d @@@ 'tag17'::wquery) AS every_row_satisfies_the_qual
  FROM (SELECT id, d FROM vo WHERE d @@@ 'tag17'::wquery
         ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 5) s;

-- 30 rows through the same path, and the same two properties.
SELECT count(*) AS band3_rows,
       bool_and(d @@@ 'band3'::wquery) AS every_band3_row_satisfies_the_qual
  FROM (SELECT id, d FROM vo WHERE d @@@ 'band3'::wquery
         ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s;

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
EXPLAIN (COSTS OFF)
SELECT qid, (SELECT id FROM vo ORDER BY vo.v <=> voq.q LIMIT 1) AS nn
  FROM voq ORDER BY qid;

SELECT count(*) AS correlated_rescan_disagreements
  FROM voq
 WHERE (SELECT id FROM vo ORDER BY vo.v <=> voq.q LIMIT 1)
       IS DISTINCT FROM
       (SELECT m.id FROM weave_vec_scan('vo_weave', voq.q, 1) s
          JOIN vomap m USING (docid));

-- ... and a genuine nested loop over LATERAL, at k = 3 so the inner scan returns
-- several rows per rescan and an ordpos that was not reset would show up as a
-- short or a duplicated group.
EXPLAIN (COSTS OFF)
SELECT q.qid, n.id FROM voq q,
  LATERAL (SELECT id FROM vo ORDER BY vo.v <=> q.q LIMIT 3) n;

SELECT count(*) AS lateral_rows,
       count(DISTINCT (qid, id)) AS lateral_distinct_pairs
  FROM (SELECT q.qid, n.id FROM voq q,
          LATERAL (SELECT id FROM vo ORDER BY vo.v <=> q.q LIMIT 3) n) t;

SELECT count(*) AS lateral_rescan_disagreements
  FROM voq q
 WHERE (SELECT array_agg(n.id)
          FROM (SELECT id FROM vo ORDER BY vo.v <=> q.q LIMIT 3) n)
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
CREATE TEMP TABLE vomap AS
  SELECT t.id, l.docid
    FROM (SELECT id, row_number() OVER (ORDER BY ctid) AS rn FROM vo) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('vo_weave')) l USING (rn);
SELECT count(*) = (SELECT count(*) FROM vo) AS map_covers_every_row_2 FROM vomap;

-- Every lane of both bolts, in one ordering, each row once.
SELECT count(*) AS rows_over_two_bolts,
       count(DISTINCT id) AS distinct_rows_over_two_bolts
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s;

-- ... and it is the same ordering the SRF produces over the same two bolts.
SELECT (SELECT array_agg(id)
          FROM (SELECT id FROM vo
                 ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec LIMIT 10) i)
     = (SELECT array_agg(m.id ORDER BY s.score DESC, s.segno, s.warp)
          FROM weave_vec_scan('vo_weave', '[7,7,7,7,7,7,7,7]'::wvec, 10) s
          JOIN vomap m USING (docid))
       AS two_bolt_top10_index_equals_srf;

-- The oversized row is an exact match for its own vector and lives in the SECOND
-- bolt, so a query at that point must reach it -- an ordering that scanned only
-- bolt 0 would return 300 rows of the wrong ranking and no error.
SELECT (SELECT id FROM vo ORDER BY v <=> '[800,800,800,800,800,800,800,800]'::wvec
         LIMIT 1) = (SELECT max(id) FROM vo)
       AS second_bolt_row_is_reachable;

-- ============================================================================
-- (7) AN INSERT THAT LANDS IN THE PENDING BUFFER.  doc/GAPS.md G29.
--
-- WHAT THE BEHAVIOUR IS TODAY, stated rather than wished for: the vector channel
-- does NOT scan the pending buffer.  The shuttle is a per-bolt cursor over
-- VDIR/VCODES/VWARP and a pending document is in no bolt, so between an INSERT and
-- the next flush the row is in LEXICAL answers and absent from VECTOR ones.  G29
-- records why closing it is held for the fused scorer -- the pending vector would
-- need a scoring path that is not a bolt cursor, and its results would have to enter
-- the same top-k as the bolts', and building a second parallel top-k merge before
-- Phase F exists is how two scorers that disagree get written.
--
-- Asserted as a ROW COUNT rather than as a ranking, because a count is exact and
-- pins the mechanism: the vector ordering scan returns one row per live lane that
-- resolves to a visible tuple, so "the row is not in the weft" is directly
-- observable and does not bet on the quantizer placing an exact match first.
-- Closing G29 therefore shows up here as a diff rather than as nothing.
-- ============================================================================
INSERT INTO vo(d, v)
  VALUES (to_wdoc('order pendingtoken'), '[7,7,7,7,7,7,7,7]');

SELECT count(*) AS table_rows FROM vo;
SELECT sum(nvec) AS lanes_before_flush FROM weave_vec_meta('vo_weave');

-- The lexical channel sees it immediately: weave_collect_matches() walks the
-- pending page itself.  This asymmetry is the whole of G29.
SELECT count(*) AS lexical_sees_the_pending_row
  FROM vo WHERE d @@@ 'pendingtoken'::wquery;

-- The vector ordering scan does not: one fewer row than the table has, and the
-- inserted row is not among them even though its vector IS the query.  The EXPLAIN
-- is load-bearing here and not decoration: the COUNT is the discriminator, so a Seq
-- Scan answering this query would report 302 and the file would be asserting the
-- opposite of what it claims.
EXPLAIN (COSTS OFF)
SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec;
SELECT count(*) AS vec_rows_before_flush
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s;
SELECT count(*) AS pending_row_in_vec_answer_before_flush
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s
 WHERE s.id = (SELECT max(id) FROM vo);

-- After a flush the row is in a real weft (G23 is closed at the segment) and the
-- ordering covers it.
SELECT weave_merge('vo_weave');
SELECT sum(nvec) AS lanes_after_flush FROM weave_vec_meta('vo_weave');
SELECT count(*) AS vec_rows_after_flush
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s;
SELECT count(*) AS pending_row_in_vec_answer_after_flush
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s
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
SELECT count(*) AS hot_visible_rows FROM voh;
EXPLAIN (COSTS OFF)
SELECT id FROM voh ORDER BY v <=> '[5,5,5,5,5,5,5,5]'::wvec;
SELECT count(*) AS vec_rows_over_hot_chains
  FROM (SELECT id FROM voh ORDER BY v <=> '[5,5,5,5,5,5,5,5]'::wvec) s;
DROP TABLE voh;

-- A DELETE that is not yet vacuumed: the docid is still in the weft and the heap
-- probe is what must drop it.  The count is the whole assertion -- a scan that
-- skipped the probe would return the dead rows too.
DELETE FROM vo WHERE id IN (SELECT id FROM vo ORDER BY id LIMIT 10);
SELECT count(*) AS visible_rows_after_delete FROM vo;
SELECT sum(nvec) AS lanes_still_in_the_weft FROM weave_vec_meta('vo_weave');
SELECT count(*) AS vec_rows_after_delete
  FROM (SELECT id FROM vo ORDER BY v <=> '[7,7,7,7,7,7,7,7]'::wvec) s;

SELECT bool_and(ok) AS all_invariants_hold FROM weave_check('vo_weave');

-- ============================================================================
-- (9) AN INDEX WITH NO VECTOR CHANNEL falls through cleanly.  The operator is on
-- wvec, so it can only be matched to a wvec index column; what this asserts is the
-- other half -- a wvec COLUMN whose weft is not there.  A table whose every vector
-- is NULL has a weft with no live lane, and the ordering must return no rows rather
-- than erroring, because an ordering path over an empty channel is a legitimate
-- plan.
-- ============================================================================
CREATE TABLE voe (id serial, d wdoc, v wvec(8));
INSERT INTO voe(d, v) SELECT to_wdoc('empty tag' || g), NULL FROM generate_series(1, 20) g;
CREATE INDEX voe_weave ON voe USING weave (d, v);
SELECT count(*) AS wefts, sum(nvec) AS lanes,
       (SELECT count(*) FILTER (WHERE live) FROM weave_vec_lanes('voe_weave')) AS live_lanes
  FROM weave_vec_meta('voe_weave');
EXPLAIN (COSTS OFF)
SELECT id FROM voe ORDER BY v <=> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 5;
SELECT count(*) AS rows_from_an_all_null_vector_column
  FROM (SELECT id FROM voe ORDER BY v <=> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 5) s;

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
SELECT id FROM vol ORDER BY v <=> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3;
SELECT count(*) AS rows_without_a_vector_channel
  FROM (SELECT id FROM vol ORDER BY v <=> '[1,1,1,1,1,1,1,1]'::wvec LIMIT 3) s;
SET enable_seqscan = off;

DROP TABLE vol;
DROP TABLE voe;
DROP TABLE voq;
DROP TABLE vo;
