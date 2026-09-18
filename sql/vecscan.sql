-- Task V8: the vector code scan, reached DIRECTLY from SQL.
--
-- WHY THIS FILE EXISTS AT ALL, and it is not "coverage".  V8 wires no operator and
-- no ORDER BY path, so nothing the planner can produce enters the code scan.  On
-- 2026-09-16 a mutation that reintroduced a known scan-side bug survived the whole
-- suite twice -- the second time because the planner answered the query with a
-- bitmap heap scan whose executor recheck re-evaluated the operator itself, giving
-- the right answer by a path that never entered the mutated code (AGENTS.md).  So
-- the scan needs an entry point with no planner and no recheck behind it, and every
-- assertion below goes through weave_vec_scan() / weave_vec_scan_stats() for that
-- reason.
--
-- THE SCORING KERNEL IS PINNED TO scalar for the whole file.  `auto` picks the
-- widest SIMD path the host can run, and while every implemented path is proven
-- bit-identical to the scalar oracle by test/hegel/test_kernels.c, pinning removes
-- the host from the expected output entirely -- which is what lets the exact
-- rounded scores below be assertions rather than approximations.
CREATE EXTENSION IF NOT EXISTS pg_weave;
ALTER EXTENSION pg_weave UPDATE;
SET pg_weave.vec_kernel = 'scalar';
SET enable_seqscan = off;

-- 300 documents, 8 dimensions, every one of them with a vector, in one bolt: ten
-- 32-lane blocks with the last one short (300 = 9 * 32 + 12).  The vectors are a
-- deterministic spread rather than random so the brute-force comparison below is
-- reproducible.
CREATE TABLE vs (id serial, d wdoc, v wvec(8));
INSERT INTO vs(d, v)
  SELECT to_wdoc('scan tag' || g),
         ('[' || (g % 97) || ',' || (g % 89) || ',' || (g % 83) || ','
               || (g % 79) || ',' || (g % 73) || ',' || (g % 71) || ','
               || (g % 67) || ',' || (g % 61) || ']')::wvec
    FROM generate_series(1, 300) g;
CREATE INDEX vs_l2 ON vs USING weave (d, v);
ANALYZE vs;

-- The metric reloption's default: l2, which is WEAVE_METRIC_L2 == 1.  Every weft
-- already on disk before V8 says l2, so the default is the value that keeps them
-- meaning what they already meant.
SELECT dim, bits, metric, nvec, nblocks FROM weave_vec_meta('vs_l2');

-- WARP -> ROW, so a scan result can be compared with a brute-force SQL answer.
--
-- The weft is written in DOCID order and docids ascend with ctid (weave/am.h), the
-- lanes are dense, and no row here has a NULL vector -- so warp w is the (w+1)-th
-- row in ctid order.  Deriving the map with row_number() rather than hardcoding
-- WEAVE_OFFSET_FACTOR keeps it independent of BLCKSZ; the count assertion is what
-- says the derivation held.
CREATE TEMP TABLE vsmap AS
  SELECT (row_number() OVER (ORDER BY ctid)) - 1 AS warp, id, v FROM vs;
SELECT count(*) = (SELECT nvec FROM weave_vec_meta('vs_l2')) AS map_covers_the_weft
  FROM vsmap;

-- ---- a top-k that is sane ------------------------------------------------
-- Scores are in the metric's own domain, higher is better, so for l2 they are
-- -||q - v||^2 and therefore NEGATIVE.  They are quantized-domain scores: V8 does
-- no exact rerank (that is V10), so a score approximates the exact distance.
SELECT segno, warp, round(score::numeric, 2) AS score
  FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 5)
 ORDER BY score DESC, warp;

-- Descending score, and every score dominated by the shuttle's bolt-wide ceiling.
-- maxscore >= block_max() >= score everywhere is contract (C2) plus the MaxScore
-- partition's requirement; it is one of the few halves of (C2) that IS assertable
-- from SQL, and a ceiling that is too low is otherwise invisible.
SELECT bool_and(score <= m.maxscore) AS maxscore_dominates_every_score
  FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 25) s,
       weave_vec_scan_stats('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 25) m
 WHERE s.segno = m.segno;

-- ---- agreement with a brute-force exact answer ---------------------------
-- THE TOLERANCE, and why it is a set comparison rather than an equality.  A code
-- scan scores quantized reconstructions, so its ordering is the exact ordering
-- perturbed by quantization error -- at 4 bits and 8 dimensions that reorders
-- near-ties freely.  What must hold is that the exact nearest neighbour is still
-- IN a generously sized candidate set: that is what a rerank window consumes
-- (V10), and it is the property a broken bound or a mis-scattered strip destroys.
-- An exact top-1 = top-1 assertion would be testing the quantizer's recall, which
-- test/hegel/test_quantize.c measures properly and which no fixed expected output
-- should pretend to pin.
SELECT (SELECT warp FROM vsmap ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec, warp LIMIT 1)
         IN (SELECT warp FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 25))
       AS exact_top1_is_in_the_scanned_top25;
-- ... and again at a query that is not near the middle of the corpus.
SELECT (SELECT warp FROM vsmap ORDER BY v <-> '[90,80,70,60,50,40,30,20]'::wvec, warp LIMIT 1)
         IN (SELECT warp FROM weave_vec_scan('vs_l2', '[90,80,70,60,50,40,30,20]'::wvec, 25))
       AS exact_top1_is_in_the_scanned_top25_2;
-- The overlap between the scan's top-25 and the exact top-25, as a number rather
-- than a bound, so a regression in it is visible instead of merely passing.
SELECT count(*) AS overlap_of_top25_with_exact_top25
  FROM (SELECT warp FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 25)
        INTERSECT
        SELECT warp FROM (SELECT warp FROM vsmap
                           ORDER BY v <-> '[7,7,7,7,7,7,7,7]'::wvec, warp
                           LIMIT 25) e) x;

-- THE ANSWER MUST NOT DEPEND ON k, and this is the sharpest thing about the block
-- bound that a fixed expected output can say.  A larger k is a LOWER top-k floor,
-- so it prunes fewer blocks: the two scans below do measurably different amounts of
-- work (see blocks_scored under the counters) and must still agree about the best
-- five documents.  A bound that is ever too low drops rows silently -- the answers
-- stay plausible, just missing (AGENTS.md hard rule 1) -- and it drops them from
-- the SMALLER k first, which is exactly the disagreement this counts.
SELECT count(*) AS top5_is_the_first_5_of_top25
  FROM (SELECT warp FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 5)
        INTERSECT
        SELECT warp FROM (SELECT warp FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 25)
                           ORDER BY score DESC, warp LIMIT 5) t) x;

-- ---- the allowlist gate --------------------------------------------------
-- An EMPTY allowlist admits nothing and must return nothing.  It is NOT the same
-- as NULL, which means "no filter": conflating the two turns an empty candidate
-- set into a full scan, which is the failure this pair of queries pins.
SELECT count(*) AS rows_for_an_empty_allowlist
  FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10, '{}'::bigint[]);
SELECT count(*) AS rows_for_no_allowlist
  FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10, NULL);

-- ... and the counters say WHY the empty allowlist returned nothing: every block
-- was skipped on the mask, so not one strip was scattered and no kernel ran.  This
-- is the measurement task V8's gate is about, and it is only observable through a
-- function that returns rows when the scan returns none.
SELECT segno, blocks_seen, blocks_skipped_mask, blocks_scored, lanes_scored
  FROM weave_vec_scan_stats('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10, '{}'::bigint[]);
-- Unfiltered, nothing is skipped on the mask -- and the BOUND then skips whatever
-- the top-k floor lets it, which on this corpus is most of the weft: the vectors
-- are a monotone function of the generate_series, so warp order is nearly spatial
-- order and the centroid+radius bound is at its most effective (the property
-- bench/RESULTS_BOUND_PRUNING.md found to be load-bearing and doc/PHASES.md V13
-- makes a gate).  The number is therefore corpus-specific: a diff here means the
-- bound's arithmetic changed, which is worth failing over even though it is not by
-- itself a bug.
SELECT segno, blocks_seen, blocks_skipped_mask, blocks_skipped_bound,
       blocks_scored, lanes_scored
  FROM weave_vec_scan_stats('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10, NULL);
-- ... and at a larger k the floor is lower, so the same query prunes less.  This is
-- the work half of the k-independence assertion above: same answers, more blocks.
SELECT segno, blocks_skipped_bound, blocks_scored
  FROM weave_vec_scan_stats('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 25, NULL);

-- A SINGLE-DOCID allowlist returns exactly that document.  The docid comes from
-- the warp map through weave_vec_lanes(), so this also pins the docid -> warp
-- conversion the SRF does per bolt: an allowlist argument of warps would be
-- ambiguous across bolts (a warp is segment-local), and a conversion that picked
-- the wrong warp would answer with the wrong row rather than with no row.
SELECT warp AS asked_for_warp, docid > 0 AS docid_is_set
  FROM weave_vec_lanes('vs_l2') WHERE warp = 137;
SELECT s.segno, s.warp, s.docid = l.docid AS docid_matches
  FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10,
                      ARRAY[(SELECT docid FROM weave_vec_lanes('vs_l2') WHERE warp = 137)]) s,
       (SELECT docid FROM weave_vec_lanes('vs_l2') WHERE warp = 137) l;

-- A SELECTIVE allowlist: four documents, chosen to sit in four different blocks
-- (warps 5, 100, 200 and 290 are blocks 0, 3, 6 and 9), so six of the ten blocks
-- are skipped on the mask and four are scored.
--
-- blocks_skipped_mask counts saved SCORING and saved strip scatter, never saved
-- I/O: the code chain has no block -> page index, so a skipped block still costs
-- its page reads (doc/specs/VECTOR_CHANNEL.md sect. 8b).  Stating it here keeps
-- the number from being quoted as something it is not.
CREATE TEMP TABLE vspick AS
  SELECT docid FROM weave_vec_lanes('vs_l2') WHERE warp IN (5, 100, 200, 290);
SELECT segno, blocks_seen, blocks_skipped_mask, blocks_scored, lanes_scored
  FROM weave_vec_scan_stats('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10,
                            (SELECT array_agg(docid) FROM vspick));
SELECT segno, warp FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 10,
                                       (SELECT array_agg(docid) FROM vspick))
 ORDER BY warp;

-- ---- both metrics, which the reloption is what makes reachable ------------
-- metric = ip is WEAVE_METRIC_IP == 2, and it changes no stored byte: the codes
-- are metric-independent.  What it changes is the domain the score and the bound
-- are produced in, which is why it has to be recorded per weft rather than chosen
-- at query time (doc/specs/VECTOR_CHANNEL.md sect. 8b).
CREATE INDEX vs_ip ON vs USING weave (d, v) WITH (metric = 'ip');
SELECT dim, bits, metric, nvec, nblocks FROM weave_vec_meta('vs_ip');
-- An inner-product score is the inner product itself, so it is POSITIVE here and
-- the two indexes rank the same corpus differently -- which is the observable
-- consequence of the reloption and the thing a metric ignored at scan time would
-- get wrong.
SELECT segno, warp, round(score::numeric, 2) AS score
  FROM weave_vec_scan('vs_ip', '[7,7,7,7,7,7,7,7]'::wvec, 5)
 ORDER BY score DESC, warp;
-- Brute force again, in the inner-product domain: <#> is the NEGATIVE inner
-- product, so ascending <#> is descending inner product.
SELECT (SELECT warp FROM vsmap ORDER BY v <#> '[7,7,7,7,7,7,7,7]'::wvec, warp LIMIT 1)
         IN (SELECT warp FROM weave_vec_scan('vs_ip', '[7,7,7,7,7,7,7,7]'::wvec, 25))
       AS exact_ip_top1_is_in_the_scanned_top25;
-- The two metrics disagree about the corpus, which is the point of recording it.
SELECT (SELECT warp FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 1))
       <> (SELECT warp FROM weave_vec_scan('vs_ip', '[7,7,7,7,7,7,7,7]'::wvec, 1))
       AS l2_and_ip_rank_differently;

-- ... and the two metrics with no sound compressed-domain bound are refused at
-- BUILD time, with the reason.  They are members of the reloption enum on purpose:
-- leaving them out would fail with "invalid value", which says nothing about why.
CREATE INDEX vs_cos ON vs USING weave (d, v) WITH (metric = 'cosine');
CREATE INDEX vs_l1 ON vs USING weave (d, v) WITH (metric = 'l1');
-- A value that is not a metric at all is still the reloption parser's business.
CREATE INDEX vs_bogus ON vs USING weave (d, v) WITH (metric = 'hamming');

-- ---- argument handling ---------------------------------------------------
SELECT count(*) FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 0);
SELECT count(*) FROM weave_vec_scan('vs_l2', '[1,2,3]'::wvec, 5);
SELECT count(*) FROM weave_vec_scan('vs_l2', '[7,7,7,7,7,7,7,7]'::wvec, 5,
                                    ARRAY[1::bigint, NULL]);
-- A NULL index, query or k returns no rows, which is what STRICT would have done;
-- the fourth argument cannot be STRICT because its NULL is meaningful.
SELECT count(*) AS null_query_returns_nothing
  FROM weave_vec_scan('vs_l2', NULL, 5);

-- ---- a weft whose blocks are more than one strip -------------------------
-- Every weft above is ONE lane strip plus one centroid strip per block, so the
-- code cursor's lockstep advance never has to cross a strip boundary inside a
-- block.  1024 dimensions is three lane strips (j0 = 0, 509, 1018) plus one
-- centroid strip, which is four pages per block -- the case where a cursor that
-- miscounted pages would read one block's codes as another's and score them
-- against the wrong bounds.  40 rows is two blocks, the second short.
CREATE TABLE vsbig (id serial, d wdoc, v wvec(1024));
INSERT INTO vsbig(d, v)
  SELECT to_wdoc('slice tag' || g),
         (SELECT '[' || string_agg(((g * 31 + k * 17) % 199 - 99)::text, ',') || ']'
            FROM generate_series(1, 1024) k)::wvec
    FROM generate_series(1, 40) g;
CREATE INDEX vsbig_l2 ON vsbig USING weave (d, v);
SELECT dim, nvec, nblocks FROM weave_vec_meta('vsbig_l2');
SELECT segno, blocks_seen, blocks_scored, lanes_scored
  FROM weave_vec_scan_stats('vsbig_l2',
                            (SELECT v FROM vsbig WHERE id = 7), 5);
-- The query IS one of the indexed vectors, so the exact nearest neighbour is that
-- row itself at distance 0.  A block assembled from the wrong strips cannot put it
-- first, which makes this the one place in the file where the top-1 is exact
-- enough to assert.
CREATE TEMP TABLE vsbigmap AS
  SELECT (row_number() OVER (ORDER BY ctid)) - 1 AS warp, id FROM vsbig;
SELECT (SELECT warp FROM vsbigmap WHERE id = 7) =
       (SELECT warp FROM weave_vec_scan('vsbig_l2',
                                        (SELECT v FROM vsbig WHERE id = 7), 1))
       AS the_vector_itself_is_its_own_nearest_neighbour;

DROP TABLE vsbig;
DROP TABLE vs;
