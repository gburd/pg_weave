-- F9 option (b), the maintainer's decision of 2026-10-04: fuzzy matching joins a
-- fused ranking as a GATE -- a WHERE restriction, `body @@@ 'term~k'` -- and not as a
-- scored channel inside fuse().  doc/specs/FUSED_TOPK.md sect. 7d.
--
-- WHAT A GREEN RUN HAS TO PROVE:
--
--   1. THE GATE FORM IS ONE FUSED INDEX SCAN.  For every gate kind the @@@ grammar
--      has (fuzzy ~1 and ~2, prefix, regex; src/query/parse.c), alone and beside a
--      docvalues predicate: one Index Scan, the gate in its `Index Cond`, the `<~>`
--      transport key in its `Order By` (the evidence the fused path was chosen),
--      and no Sort.
--   2. IT RETURNS THE HEAP'S ROWS.  Every row once, every row inside the gate, and
--      the same SET as a Seq Scan + Sort of the same query -- with the LIMIT inside
--      the match set and beyond it, and for a gate that matches nothing.
--   3. THE G71 RULE HOLDS UNDER A GATE.  A matching row with a NULL vector is not a
--      ranked candidate: it comes after every ranked row, as a Sort over fuse()
--      (NULL for it) puts it.
--   4. `fuse(..., body <@> p)` STAYS UNFUSED.  Edit distance as a SCORED channel is
--      refused by src/am/fusepath.c (req->servable = false) and planned as a Sort
--      over the fallback.  It must still run, raise nothing, and return the heap's
--      rows.
--
-- WHY THE CORPUS CAN COMPARE A TOP-k ACROSS ARMS AT ALL.  sect. 7a (1): the
-- fallback's `<=>` has no corpus (df = 1, avgdl = |D|), so in general the index and
-- a Sort over fuse() rank differently.  Here both channels prefer the same rows:
-- `alpha` occurs 61 - g times, so BM25 rises as g falls under either formula, and
-- `emb` is [g,0,0,0] against the query [0,0,0,0], so the l2 score rises as g falls
-- too, quantized or not (every vector has the same direction).  Any positive
-- weighting of the two ranks by ascending g, so the two arms agree on the ORDER of
-- the matching rows and a top-k set comparison is exact rather than hopeful.
--
-- THE CHECKS ARE SELF-CHECKING BOOLEANS where they can be: an expected file
-- regenerated over a wrong answer flips a `t` to an `f` rather than quietly pinning
-- a new row list.  This file was mutation-tested: with the fused pass's gate shuttle
-- removed (src/am/amscan.c, weave_fuse_pass(), the `if (so->plainInit)` that adds
-- weave_gate_shuttle_from_tidset()), section (2)'s within_gate and same_set go
-- false.  doc/PHASES.md F9 records the run.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
RESET client_min_messages;

-- Plan text stability only.
SET max_parallel_workers_per_gather = 0;

-- g % 7 picks the word; its Levenshtein distance from 'protien' is in brackets:
--   0 protien [0]  1 protiens [1]  2 protein [2]  3 protean [2]
--   4 protons [3]  5 zeta [6]      6 quux [7]
-- so 'protien~1' admits g % 7 in {0,1}, 'protien~2' {0,1,2,3}, 'prot*' {0..4},
-- and the anchored '/^prot[a-z]*n$/' {0,2,3}.  Rows 1001 and 1002 match every one
-- of those gates (1002 not ~1) and have NO VECTOR: section (3)'s subjects.
CREATE TABLE fg (id int, body wdoc, emb wvec(4), price bigint)
    WITH (autovacuum_enabled = off);
INSERT INTO fg
SELECT g,
       to_wdoc('simple', repeat('alpha ', 61 - g) ||
               (ARRAY['protien', 'protiens', 'protein', 'protean',
                      'protons', 'zeta', 'quux'])[g % 7 + 1]),
       ('[' || g || ',0,0,0]')::wvec,
       (g % 4)::bigint
  FROM generate_series(1, 60) g;
INSERT INTO fg VALUES (1001, to_wdoc('simple', 'alpha protien'), NULL, 0),
                      (1002, to_wdoc('simple', 'alpha protein'), NULL, 1);
CREATE INDEX fg_w ON fg USING weave (body, emb, price int8_docval_ops);
ANALYZE fg;

-- ---------------------------------------------------------------------------
-- The harness.  Each arm is a set of planner GUCs, applied transaction-locally
-- inside the function that plans the query, so it cannot leak into the next
-- statement:
--   index    seq scan and bitmap scan off: the plan the fused path must win
--   heap     index scan and bitmap scan off: a Seq Scan + Sort, the reference
--   default  nothing off
-- PostgreSQL 18 prints `Disabled: true` on a disabled node it still chose, and
-- 17 does not; no arm below makes the planner choose a disabled node, and the
-- plan checks are regexes over the text, so neither major's annotation matters.
-- ---------------------------------------------------------------------------
CREATE FUNCTION pg_temp.fg_arm(arm text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  PERFORM set_config('enable_seqscan',
                     CASE WHEN arm = 'index' THEN 'off' ELSE 'on' END, true);
  PERFORM set_config('enable_indexscan',
                     CASE WHEN arm = 'heap' THEN 'off' ELSE 'on' END, true);
  PERFORM set_config('enable_bitmapscan',
                     CASE WHEN arm = 'default' THEN 'on' ELSE 'off' END, true);
END $$;

-- The query, in the arm's plan, numbered in the order the plan returned it.  The
-- query is executed AS WRITTEN -- not wrapped in a subquery to number it -- so the
-- plan that ran is the plan fg_plan() shows.
CREATE FUNCTION pg_temp.fg_ids(q text, arm text) RETURNS TABLE (rn bigint, id int)
LANGUAGE plpgsql AS $$
DECLARE
  r record;
BEGIN
  PERFORM pg_temp.fg_arm(arm);
  rn := 0;
  FOR r IN EXECUTE q LOOP
    rn := rn + 1;
    id := r.id;
    RETURN NEXT;
  END LOOP;
END $$;

CREATE FUNCTION pg_temp.fg_plan(q text, arm text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  l text;
  p text := '';
BEGIN
  PERFORM pg_temp.fg_arm(arm);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    p := p || l || E'\n';
  END LOOP;
  RETURN p;
END $$;

-- The fused query this file is about, for a gate and a LIMIT.
CREATE FUNCTION pg_temp.fg_q(gate text, lim int) RETURNS text
LANGUAGE sql AS $$
  SELECT format('SELECT id FROM fg WHERE %s'
                ' ORDER BY fuse(body <=> %L::wquery, emb <-> %L::wvec) LIMIT %s',
                gate, 'alpha', '[0,0,0,0]', lim)
$$;

-- Everything section (1) and (2) assert about one gate at one LIMIT:
--   fused_plan  exactly one scan node, an Index Scan on fg_w, the gate in its
--               Index Cond, the transport key in its Order By, no Sort, and no
--               Filter (so a docvalues predicate is a gate too, not a filter)
--   heap_plan   the reference really is a Seq Scan + Sort and touches no index
--   n_match     the heap's match set for the gate alone, no LIMIT
--   once        no row twice from the fused scan
--   within_gate every fused row satisfies the gate (by the heap's evaluation)
--   same_set    the fused rows are the Seq Scan + Sort's rows
CREATE FUNCTION pg_temp.fg_check(gate text, lim int,
    OUT fused_plan bool, OUT heap_plan bool,
    OUT n_index int, OUT n_heap int, OUT n_match int,
    OUT once bool, OUT within_gate bool, OUT same_set bool)
LANGUAGE plpgsql AS $$
DECLARE
  q text := pg_temp.fg_q(gate, lim);
  p text;
  a_idx int[];
  a_heap int[];
  a_match int[];
BEGIN
  p := pg_temp.fg_plan(q, 'index');
  fused_plan := p ~ 'Index Scan using fg_w on fg'
            AND p ~ 'Index Cond: \(.*body @@@'
            AND p ~ '<~>'
            AND p !~ 'Sort'
            AND p !~ 'Filter'
            AND (SELECT count(*) FROM regexp_matches(p, 'Scan', 'g')) = 1;
  p := pg_temp.fg_plan(q, 'heap');
  heap_plan := p ~ 'Seq Scan on fg' AND p ~ 'Sort' AND p !~ 'Index';

  SELECT COALESCE(array_agg(i.id ORDER BY i.rn), '{}') INTO a_idx
    FROM pg_temp.fg_ids(q, 'index') i;
  SELECT COALESCE(array_agg(h.id ORDER BY h.rn), '{}') INTO a_heap
    FROM pg_temp.fg_ids(q, 'heap') h;
  SELECT COALESCE(array_agg(m.id), '{}') INTO a_match
    FROM pg_temp.fg_ids('SELECT id FROM fg WHERE ' || gate, 'heap') m;

  n_index := cardinality(a_idx);
  n_heap := cardinality(a_heap);
  n_match := cardinality(a_match);
  once := n_index = (SELECT count(DISTINCT x) FROM unnest(a_idx) x);
  within_gate := a_idx <@ a_match;
  same_set := (SELECT COALESCE(array_agg(x ORDER BY x), '{}') FROM unnest(a_idx) x)
            = (SELECT COALESCE(array_agg(x ORDER BY x), '{}') FROM unnest(a_heap) x);
END $$;

-- ---------------------------------------------------------------------------
-- (1) THE PLAN, shown in full for the two shapes users will write: a fuzzy gate,
-- and a fuzzy gate beside a docvalues predicate.  Both quals are Index Conds; the
-- third Order By key is the `<~>` transport key, which only the fused path emits.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;

EXPLAIN (COSTS OFF)
SELECT id FROM fg WHERE body @@@ 'protien~2'::wquery
 ORDER BY fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec) LIMIT 5;

EXPLAIN (COSTS OFF)
SELECT id FROM fg WHERE body @@@ 'protien~1'::wquery AND price < 2
 ORDER BY fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec) LIMIT 5;

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ---------------------------------------------------------------------------
-- (2) EVERY GATE KIND, against the heap.  LIMIT 5 cuts inside every non-empty
-- match set; LIMIT 100 is beyond every one, and beyond the table (62 rows), so the
-- fused scan has to finish its ranked phase and pad (G71) without emitting a row
-- outside the gate or a row twice.  Every row of this output must read t t ... t t t;
-- n_index = n_heap = least(LIMIT, n_match).
-- ---------------------------------------------------------------------------
CREATE TEMP TABLE fgq (ord int, name text, gate text, lim int);
INSERT INTO fgq VALUES
  ( 1, 'fuzzy ~1',              'body @@@ ''protien~1''::wquery',              5),
  ( 2, 'fuzzy ~1',              'body @@@ ''protien~1''::wquery',            100),
  ( 3, 'fuzzy ~2',              'body @@@ ''protien~2''::wquery',              5),
  ( 4, 'fuzzy ~2',              'body @@@ ''protien~2''::wquery',            100),
  ( 5, 'prefix',                'body @@@ ''prot*''::wquery',                  5),
  ( 6, 'prefix',                'body @@@ ''prot*''::wquery',                100),
  ( 7, 'regex',                 'body @@@ ''/^prot[a-z]*n$/''::wquery',        5),
  ( 8, 'regex',                 'body @@@ ''/^prot[a-z]*n$/''::wquery',      100),
  ( 9, 'fuzzy ~1 + docvalues',  'body @@@ ''protien~1''::wquery AND price < 2', 5),
  (10, 'fuzzy ~1 + docvalues',  'body @@@ ''protien~1''::wquery AND price < 2', 100),
  (11, 'fuzzy ~2 + docvalues',  'body @@@ ''protien~2''::wquery AND price < 2', 5),
  (12, 'fuzzy ~2 + docvalues',  'body @@@ ''protien~2''::wquery AND price < 2', 100),
  (13, 'fuzzy, matches nothing', 'body @@@ ''xylophone~1''::wquery',           5),
  (14, 'fuzzy, matches nothing', 'body @@@ ''xylophone~1''::wquery',         100);

SELECT q.name, q.lim, c.*
  FROM fgq q, LATERAL pg_temp.fg_check(q.gate, q.lim) c
 ORDER BY q.ord;

-- The same fourteen rows as one verdict, so a regenerated expected file cannot hide a
-- single `f` in the table above.
SELECT bool_and(c.fused_plan AND c.heap_plan AND c.once AND c.within_gate
                AND c.same_set AND c.n_index = c.n_heap
                AND c.n_index = least(q.lim, c.n_match)) AS every_gate_matches_the_heap
  FROM fgq q, LATERAL pg_temp.fg_check(q.gate, q.lim) c;

-- ---------------------------------------------------------------------------
-- (3) G71 UNDER A GATE.  Rows 1001 and 1002 match 'protien~2' and have a NULL
-- vector, so fuse() is NULL for them and a Sort puts them last.  The fused scan
-- must not rank them (doc/GAPS.md G71: an unreached channel contributes 0, which
-- for l2 is the BEST score) -- it pads them after every ranked row.  So: the
-- fused scan's tail, past the rows that have a vector, is exactly the heap's
-- NULL-fuse() rows, and a LIMIT inside the match set returns neither.
-- ---------------------------------------------------------------------------
CREATE TEMP TABLE fg_g71 AS
  SELECT * FROM pg_temp.fg_ids(pg_temp.fg_q('body @@@ ''protien~2''::wquery', 100),
                               'index');

SELECT (SELECT array_agg(id ORDER BY id) FROM fg_g71
         WHERE rn > (SELECT count(*) FROM fg
                      WHERE body @@@ 'protien~2'::wquery AND emb IS NOT NULL))
         AS fused_tail,
       (SELECT array_agg(id ORDER BY id) FROM fg
         WHERE body @@@ 'protien~2'::wquery
           AND fuse(body <=> 'alpha'::wquery, emb <-> '[0,0,0,0]'::wvec) IS NULL)
         AS heap_null_fuse,
       (SELECT count(*) FROM pg_temp.fg_ids(
                 pg_temp.fg_q('body @@@ ''protien~2''::wquery', 5), 'index')
         WHERE id IN (1001, 1002)) AS null_vector_rows_in_a_top5;
DROP TABLE fg_g71;

-- ---------------------------------------------------------------------------
-- (4) `fuse(..., body <@> p)` STAYS UNFUSED, and that is decision (b), not a gap:
-- edit distance is a gate, not a scored channel.  src/am/fusepath.c sets
-- req->servable = false for `<@>`, so no fused path is offered (no `<~>` key) and
-- the plan is a Sort over the fallback -- over a Seq Scan bare, over a plain Index
-- Scan carrying the @@@ qual when there is one.  Both must run, raise nothing, and
-- return the heap's rows.
-- ---------------------------------------------------------------------------
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM fg WHERE body @@@ 'protien~2'::wquery
 ORDER BY fuse(body <=> 'alpha'::wquery, body <@> 'protien') LIMIT 5;
RESET enable_seqscan;
RESET enable_bitmapscan;

EXPLAIN (COSTS OFF)
SELECT id FROM fg
 ORDER BY fuse(body <=> 'alpha'::wquery, body <@> 'protien') LIMIT 5;

CREATE TEMP TABLE fge (name text, q text, arm text);
INSERT INTO fge VALUES
  ('gated <@>, index arm',
   'SELECT id FROM fg WHERE body @@@ ''protien~2''::wquery'
   ' ORDER BY fuse(body <=> ''alpha''::wquery, body <@> ''protien'') LIMIT 5', 'index'),
  ('gated <@>, index arm, beyond',
   'SELECT id FROM fg WHERE body @@@ ''protien~2''::wquery'
   ' ORDER BY fuse(body <=> ''alpha''::wquery, body <@> ''protien'') LIMIT 100', 'index'),
  ('bare <@>, default arm',
   'SELECT id FROM fg'
   ' ORDER BY fuse(body <=> ''alpha''::wquery, body <@> ''protien'') LIMIT 5', 'default');

SELECT e.name,
       p ~ 'Sort' AND p !~ '<~>' AS sort_over_the_fallback,
       (e.arm = 'index') = (p ~ 'Index Cond: \(.*body @@@') AS gate_is_index_cond_iff_gated,
       (SELECT count(*) FROM pg_temp.fg_ids(e.q, e.arm)) AS n_rows,
       (SELECT count(DISTINCT id) FROM pg_temp.fg_ids(e.q, e.arm))
         = (SELECT count(*) FROM pg_temp.fg_ids(e.q, e.arm)) AS once,
       (SELECT array_agg(id ORDER BY id) FROM pg_temp.fg_ids(e.q, e.arm))
         = (SELECT array_agg(id ORDER BY id) FROM pg_temp.fg_ids(e.q, 'heap'))
         AS same_set_as_the_heap
  FROM fge e, LATERAL pg_temp.fg_plan(e.q, e.arm) p
 ORDER BY e.name;

DROP TABLE fge;
DROP TABLE fgq;
DROP TABLE fg;
RESET max_parallel_workers_per_gather;
