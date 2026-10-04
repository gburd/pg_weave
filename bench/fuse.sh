#!/usr/bin/env bash
#
# bench/fuse.sh -- doc/specs/FUSED_TOPK.md section 8: fused top-k against
# RRF-with-over-fetch, on a public dataset with relevance judgments.
#
# WHAT THIS IS THE FIRST MEASUREMENT OF.  Every other file in bench/ measures
# latency or size on queries whose answers are gated for equality.  This one
# measures RANKING QUALITY against human judgments, which no benchmark in this
# project or in either sibling project has ever done -- `pg_fts/bench/ndcg.py`
# existed and was never once run because the corpus had no qrels.  Section 8's
# gate is four rows and this produces all four.
#
# THE CONTROL IS THE SAME INDEX, QUERIED TWICE, and that is deliberate and is a
# STRONGER control than the two-index RRF stack section 8 describes.  Both arms read
# one weave index over (body, emb): the fused arm asks for one top-10 through
# fuse(), the control arm asks for a lexical top-100 and a vector top-100 and merges
# them with Reciprocal Rank Fusion.  Nothing differs but the scorer -- same BM25,
# same quantized vectors, same buffer cache, same code -- so a win cannot come from
# storage layout, and the storage cost of a real two-index stack (which the control
# does NOT pay here) would only widen it.  Any margin measured is a LOWER BOUND on
# the margin against a genuine two-index deployment.
#
# THREE HARNESS CONSTRAINTS THAT ARE PROPERTIES OF THE PRODUCT, not of this script,
# and all three must be reported next to the numbers or the numbers mislead:
#
#   1. A FUSED QUERY'S wquery MUST BE A LITERAL.  src/am/fusepath.c refuses a
#      parameterized wquery at plan time, because a lexical key becomes one shuttle
#      per query term and the term count is unknowable until the scan runs -- and an
#      access method that advertises a path then refuses it at run time is G39.  So
#      the correlated `LATERAL` shape (queries in a table, one plan for all of them)
#      CANNOT get the pushdown, and this script generates one statement per query
#      with the query inlined.  That is also the honest shape: it is what a user who
#      wants the fused path has to write today.
#   2. THE CHANNEL CAP IS 64 AND OR OPERATORS COUNT AGAINST IT.  fusepath.c estimates
#      the shuttle count from the query's RPN ITEM count, which includes operators, so
#      an n-term disjunction costs 2n-1 of the budget.  Query terms are therefore
#      capped (MAXTERMS below) and the number of queries that hit the cap is counted
#      and printed.  BOTH arms see the identical truncated query, so the comparison is
#      unaffected; the absolute quality numbers are affected and must say so.
#   3. MULTI-TERM wquery IS CONJUNCTIVE BY DEFAULT.  'a b' is a AND b, which for a
#      natural-language query returns almost nothing, so terms are joined with the
#      OR operator '|'.  A harness that skipped this would have measured a lexical
#      arm that returned no rows and blamed the ranker.
#
# Methodology, per .agent/skills/weave-bench and AGENTS.md hard rules 8/10/11:
#   - CORRECTNESS BEFORE LATENCY.  The fused pushdown's answer is compared against
#     the same fuse() expression evaluated by a sequential scan, per query, as a set.
#     A mismatch aborts the run rather than producing a fast wrong number.
#   - the PLAN of every measured shape is captured and asserted, because a latency
#     that looks flat is usually the planner having ignored the index
#   - warm, reps per query, first dropped, p50 AND p99
#   - A/B ALTERNATED per query rather than all-A-then-all-B, so thermal or
#     neighbour drift cannot land on one arm
#   - parallelism OFF for both arms: the work counters are PARALLEL RESTRICTED
#     backend-local globals, so a parallel worker's counts would be invisible and the
#     ratio would be silently wrong. Neither AM is parallel-capable anyway.
#
# Usage: bench/fuse.sh <workdir> <dataset> [reps]
#   <workdir>/<dataset>/{corpus.tsv,queries.tsv,qrels.tsv,manifest.json} as written
#   by bench/prepdata.py.
#
set -euo pipefail

WORK=${1:?usage: bench/fuse.sh <workdir> <dataset> [reps]}
DS=${2:?usage: bench/fuse.sh <workdir> <dataset> [reps]}
REPS=${3:-7}
DB=${PGDATABASE:-weavefuse}
DIR="$WORK/$DS"
OUT=${FUSE_OUT:-$DIR}

# The over-fetch depth of the control arm.  Section 8 names k'=100.
KP=${KP:-100}
# RRF's rank constant.  60 is Cormack et al. 2009's value and the one every
# implementation uses; it is NOT tuned here, because tuning the control's
# hyperparameter and not the fused arm's weights would bias the comparison.
RRFK=${RRFK:-60}
# Depth the quality pass retrieves.  10 for nDCG@10 and MRR@10, 100 for recall@100,
# so one pass at 100 serves all three.
QDEPTH=${QDEPTH:-100}
# See constraint 2 above.  20 distinct terms is 39 RPN items plus one vector channel,
# comfortably inside the cap; 33 terms would exceed it and be refused.
MAXTERMS=${MAXTERMS:-20}
# Queries used for the LATENCY pass.  Quality uses every query (it is deterministic
# and needs one execution each); latency needs repetitions, so it samples.
LATN=${LATN:-50}
# Queries used for the correctness gate.  Each one costs a sequential scan with a
# per-row fuse() evaluation, which is the most expensive thing here.
CHECKN=${CHECKN:-10}

PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mFAIL: %s\033[0m\n' "$*" >&2; exit 1; }

for f in corpus.tsv queries.tsv qrels.tsv manifest.json; do
    [ -s "$DIR/$f" ] || die "missing or empty $DIR/$f -- run bench/prepdata.py first"
done

# The embedding mode is load-bearing for every quality number, so it is read from
# the manifest and REFUSED if it is the stdlib fake.  bench/prepdata.py's hash mode
# exists to smoke-test the plumbing with no third-party packages; a quality figure
# from it is noise, and the way that mistake reaches a RESULTS file is exactly this
# script running happily and printing a number.
EMBED=$(sed -n 's/.*"embed_mode"[[:space:]]*:[[:space:]]*"\([a-z]*\)".*/\1/p' "$DIR/manifest.json" | head -1)
DIM=$(sed -n 's/.*"dim"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' "$DIR/manifest.json" | head -1)
[ -n "$DIM" ] || die "manifest.json has no dim"
if [ "$EMBED" = "hash" ]; then
    if [ "${FUSE_ALLOW_HASH:-0}" != "1" ]; then
        die "manifest says embed_mode=hash: quality numbers would be meaningless.
     Re-run prepdata.py with --embed minilm, or set FUSE_ALLOW_HASH=1 to smoke-test
     the plumbing only -- in which case the output is labelled and must not be
     recorded as a result."
    fi
    say "SMOKE MODE: embed_mode=hash, every quality number below is MEANINGLESS"
fi

say "$DS: loading (embed=$EMBED dim=$DIM)"
createdb "$DB" 2>/dev/null || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave;" >/dev/null
$PSQL -c "ALTER EXTENSION pg_weave UPDATE;" >/dev/null 2>&1 || true
WEAVEVER=$($PSQL -t -A -c "SELECT extversion FROM pg_extension WHERE extname='pg_weave';")

# ---------------------------------------------------------------------------
# Load.  Staging tables of text, then one typed table, because COPY cannot parse a
# wvec literal into a typed column without the cast being spelled out.
# ---------------------------------------------------------------------------
$PSQL <<SQL
DROP TABLE IF EXISTS fd, fq, fqrel, stage_c, stage_q CASCADE;
CREATE TABLE stage_c (docid bigint, txt text, vec text);
CREATE TABLE stage_q (qid bigint, txt text, vec text);
CREATE TABLE fqrel (qid bigint, docid bigint, rel int);
SQL
$PSQL -c "\\copy stage_c FROM '$DIR/corpus.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"
$PSQL -c "\\copy stage_q FROM '$DIR/queries.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"
$PSQL -c "\\copy fqrel FROM '$DIR/qrels.tsv' WITH (FORMAT csv, DELIMITER E'\\t', QUOTE E'\\b')"

# metric='ip' with L2-normalized vectors IS cosine similarity, and it is the only way
# to get cosine here: weave_index_vec_metric() raises on metric='cosine' (see
# VECTOR_CHANNEL.md section 8b), and after F8 a metric mismatch is refused at PLAN
# time. prepdata.py normalizes; if it had not, ip would silently rank by an
# unnormalized dot product and the vector arm would be measuring something else.
$PSQL <<SQL
CREATE TABLE fd (id bigint PRIMARY KEY, body wdoc, emb wvec($DIM));
INSERT INTO fd SELECT docid, to_wdoc(txt), vec::wvec FROM stage_c;

CREATE TABLE fq (qid bigint PRIMARY KEY, txt text, qv wvec($DIM), wq text, nterm int);
INSERT INTO fq SELECT qid, txt, vec::wvec, NULL, NULL FROM stage_q;
SQL

# ---------------------------------------------------------------------------
# Turn each natural-language query into a wquery, in SQL so the rule is one
# expression and both arms cannot diverge.
#
# lower, split on non-alphanumeric, drop length<=2, drop the three words the parser
# treats as OPERATORS when they stand alone ('and', 'or', 'not' -- src/query/parse.c
# line 330), de-duplicate, cap at MAXTERMS, join with ' | '.
#
# DE-DUPLICATION IS NOT COSMETIC: a repeated query term would become a SECOND
# lexical shuttle on the same term, which double-weights it and spends two of the 64
# channels on one word.
#
# The cap keeps the FIRST MAXTERMS distinct terms in order of appearance rather than
# the rarest, because selecting by document frequency would need a df lookup per
# term and would make the query set depend on the corpus -- so two datasets could
# not be compared, and neither could two runs after an ingest.  Order of appearance
# is stable, cheap, and identical for both arms.
# ---------------------------------------------------------------------------
$PSQL <<SQL
WITH t AS (
    SELECT q.qid, s.tok, min(s.ord) AS ord
      FROM fq q,
           LATERAL unnest(regexp_split_to_array(lower(q.txt), '[^a-z0-9]+'))
                   WITH ORDINALITY AS s(tok, ord)
     WHERE length(s.tok) > 2
       AND s.tok NOT IN ('and', 'or', 'not')
     GROUP BY q.qid, s.tok
), r AS (
    SELECT qid, tok, row_number() OVER (PARTITION BY qid ORDER BY ord) AS rn
      FROM t
)
UPDATE fq SET wq = a.wq, nterm = a.n
  FROM (SELECT qid, string_agg(tok, ' | ' ORDER BY rn) AS wq, count(*) AS n
          FROM r WHERE rn <= $MAXTERMS GROUP BY qid) a
 WHERE fq.qid = a.qid;
SQL

# A query whose every token was dropped has no lexical channel and cannot be fused.
# Deleted rather than silently scored as zero, and COUNTED, because a nonzero count
# here changes what the averages are over.
NOWQ=$($PSQL -t -A -c "SELECT count(*) FROM fq WHERE wq IS NULL OR wq = '';")
$PSQL -c "DELETE FROM fq WHERE wq IS NULL OR wq = '';" >/dev/null
TRUNC=$($PSQL -t -A -c "SELECT count(*) FROM fq WHERE nterm >= $MAXTERMS;")

NDOCS=$($PSQL -t -A -c "SELECT count(*) FROM fd;")
NQ=$($PSQL -t -A -c "SELECT count(*) FROM fq;")

say "$DS: building the index over (body, emb)"
BUILD_START=$(date +%s.%N)
$PSQL -c "CREATE INDEX fd_weave ON fd USING weave (body, emb) WITH (metric = 'ip');" >/dev/null
BUILD_END=$(date +%s.%N)
BUILD_S=$(awk -v a="$BUILD_START" -v b="$BUILD_END" 'BEGIN{printf "%.1f", b-a}')
$PSQL -c "ANALYZE fd;" >/dev/null
IDXMB=$($PSQL -t -A -c "SELECT round(pg_relation_size('fd_weave')/1048576.0, 1);")

# LOAD-ONLY EXIT.  Everything above is the corpus shape three scripts need and only
# this one knows how to build: `fd`/`fq`/`fqrel`, the wquery derivation, and the
# single (body, emb) index.  bench/normprod.sh's header already says "PGDATABASE must
# name a database bench/fuse.sh has already loaded", which meant running the whole
# 25-minute-per-arm measurement to get a loaded database -- and bench/gatesweep.sh
# needs the same thing.  Exiting here is not a mode of the benchmark; it is the
# benchmark's setup made reachable, so the two consumers cannot drift onto a
# differently-built corpus.
if [ "${FUSE_LOAD_ONLY:-0}" = 1 ]; then
    say "$DS: LOAD ONLY -- $NDOCS docs, $NQ queries, index ${IDXMB} MB in ${BUILD_S}s, db=$DB"
    printf 'dataset\tdb\tndocs\tnq\tdim\tembed\tindex_mb\tbuild_s\tweavever\n'
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$DS" "$DB" "$NDOCS" "$NQ" "$DIM" "$EMBED" "$IDXMB" "$BUILD_S" "$WEAVEVER"
    exit 0
fi

# Both arms, both passes, no parallelism: see the header.
SETUP="SET max_parallel_workers_per_gather = 0;"

# ---------------------------------------------------------------------------
# The two arms, as SQL text generators.  One statement per query, query inlined.
# ---------------------------------------------------------------------------
fused_sql() {                   # fused_sql <wq> <vec> <limit>
    printf "SELECT id FROM fd ORDER BY fuse(body <=> %s::wquery, emb <#> %s::wvec, weights => '{0.5,0.5}') LIMIT %s;" \
           "$1" "$2" "$3"
}

# RRF: two independent over-fetches merged by reciprocal rank.
#
# row_number() is taken OVER (ORDER BY the same distance) inside each branch rather
# than OVER () over an ordered subquery, because a bare OVER () window has no defined
# input order in the standard and would make the control's ranks depend on the plan.
# That is the sort of thing that silently hands the comparison to whichever arm the
# harness got right.
rrf_sql() {                     # rrf_sql <wq> <vec> <limit>
    printf "WITH l AS (SELECT id, row_number() OVER (ORDER BY body <=> %s::wquery) AS r FROM fd ORDER BY body <=> %s::wquery LIMIT %s), v AS (SELECT id, row_number() OVER (ORDER BY emb <#> %s::wvec) AS r FROM fd ORDER BY emb <#> %s::wvec LIMIT %s) SELECT id FROM (SELECT id, 1.0/(%s + r) AS s FROM l UNION ALL SELECT id, 1.0/(%s + r) AS s FROM v) u GROUP BY id ORDER BY sum(s) DESC, id LIMIT %s;" \
           "$1" "$1" "$KP" "$2" "$2" "$KP" "$RRFK" "$RRFK" "$3"
}

# ---------------------------------------------------------------------------
# THE NORMALIZATION STUDY (doc/GAPS.md G44), opt-in via FUSE_NORMSTUDY=1.
#
# The first real-corpus run measured the fused arm's nDCG BELOW RRF on all three
# BEIR datasets -- 0.687x on fiqa -- while its recall-vs-exhaustive row was 1.000.
# So the scan is exact and the OBJECTIVE is what loses: fuse() sums RAW channel
# scores, and BM25 (~10-20) against a quantized inner product (~[-1,1]) is a ~33x
# scale mismatch, which makes equal weights effectively lexical-only.  RRF wins by
# being scale-free, not by being cleverer.
#
# BEFORE WRITING ANY C, MEASURE WHETHER NORMALIZATION ACTUALLY FIXES IT.  That is
# hard rule 9, and this is cheap: the arms below compute candidate objectives in
# SQL from the index's OWN per-channel scores -- the exact numbers the fused scan
# sums -- so a scheme that wins here is a scheme worth implementing in the scorer,
# and one that loses here has cost nothing.  It is a QUALITY question, so it does
# not need EC2: nDCG is deterministic and host-independent.  Only latency is not.
#
# Two schemes, because a single one cannot distinguish "normalization helps" from
# "this particular normalization helps":
#
#   maxn  divide each channel by its realized per-query MAXIMUM.  Zero stays the
#         floor, which is right for BM25 (an absent term contributes exactly 0).
#   mmn   per-channel min-max over the corpus, which is the textbook choice and
#         the one that treats a negative inner product as the bottom of a range
#         rather than as a penalty.
#
# NULLIF guards a zero denominator; GREATEST guards a non-positive maximum, which
# for ip over L2-normalized vectors means a query anti-correlated with the entire
# corpus -- rare, but dividing by it would INVERT the channel rather than scale it,
# and an inverted channel would look like a normalization failure instead of a
# degenerate query.
# ---------------------------------------------------------------------------
normsum_sql() {                 # normsum_sql <wq> <vec> <limit> -- scheme maxn
    printf "WITH a AS (SELECT m.id, s.score FROM weave_search('fd_weave', %s::wquery, %s) s JOIN fdmap m ON m.rowtid = s.ctid), v AS (SELECT m.id, s.score FROM weave_vec_scan('fd_weave', %s::wvec, %s) s JOIN fdmap m USING (docid)), mx AS (SELECT GREATEST((SELECT max(score) FROM a), 1e-9) AS ml, GREATEST((SELECT max(score) FROM v), 1e-9) AS mv) SELECT f.id FROM fd f LEFT JOIN a ON a.id = f.id LEFT JOIN v ON v.id = f.id CROSS JOIN mx ORDER BY 0.5*COALESCE(a.score,0)/NULLIF(mx.ml,0) + 0.5*COALESCE(v.score,0)/NULLIF(mx.mv,0) DESC, f.id LIMIT %s;" \
           "$1" "$NDOCS" "$2" "$NDOCS" "$3"
}

minmaxsum_sql() {               # minmaxsum_sql <wq> <vec> <limit> -- scheme mmn
    printf "WITH a AS (SELECT m.id, s.score FROM weave_search('fd_weave', %s::wquery, %s) s JOIN fdmap m ON m.rowtid = s.ctid), v AS (SELECT m.id, s.score FROM weave_vec_scan('fd_weave', %s::wvec, %s) s JOIN fdmap m USING (docid)), mx AS (SELECT COALESCE((SELECT min(score) FROM a),0) AS nl, GREATEST((SELECT max(score) FROM a), 1e-9) AS ml, COALESCE((SELECT min(score) FROM v),0) AS nv, GREATEST((SELECT max(score) FROM v), 1e-9) AS mv) SELECT f.id FROM fd f LEFT JOIN a ON a.id = f.id LEFT JOIN v ON v.id = f.id CROSS JOIN mx ORDER BY 0.5*(COALESCE(a.score,mx.nl)-mx.nl)/NULLIF(mx.ml-mx.nl,0) + 0.5*(COALESCE(v.score,mx.nv)-mx.nv)/NULLIF(mx.mv-mx.nv,0) DESC, f.id LIMIT %s;" \
           "$1" "$NDOCS" "$2" "$NDOCS" "$3"
}

# psql literal quoting for a text value, done by the server so a quote or backslash
# in a query string cannot terminate the literal.
QLIT=$(mktemp); trap 'rm -f "$QLIT" "$VLIT"' EXIT
VLIT=$(mktemp)
$PSQL -t -A -F $'\t' -c "SELECT qid, quote_literal(wq), quote_literal(qv::text) FROM fq ORDER BY qid;" > "$QLIT"
[ -s "$QLIT" ] || die "no queries survived preparation"

# ---------------------------------------------------------------------------
# CORRECTNESS GATE, before any timing, AND NOT AGAINST THE fuse() FALLBACK.
#
# The obvious gate -- compare the pushdown against the same fuse() expression
# evaluated by a sequential scan -- is WRONG, and this harness had it wrong first.
# It disagreed on 5 of 5 queries, and neither channel was at fault:
#
#   * The bare `<=>` operator has no corpus.  weave_distance() (src/query/rank.c,
#     and it says so in its own comment) scores with df = 1 and avgdl = |D| because
#     a per-row operator call cannot see the dictionary, so the fallback is A
#     DIFFERENT RANKING FUNCTION from the index's exact BM25 -- not a wrong one.
#     FUSED_TOPK.md sect. 7a (1) records this, and sql/fuse_pushdown.sql already
#     abandoned that comparison after the naive assertion returned f.
#   * The vector side is QUANTIZED in the index and exact in the heap, so even a
#     perfect scorer ranks near-ties differently.  Measured here on one query: the
#     index and an exact sequential scan returned the SAME ten documents with two
#     adjacent pairs transposed.
#
# THE ORACLE THAT IS ACTUALLY EXACT, borrowed from sql/fuse_pushdown.sql sect. 2b:
# one SRF call per channel, both of which enter the scan machinery directly and
# therefore report the INDEX's own per-channel score -- exact BM25 from
# weave_search(), and the quantized metric-domain score from weave_vec_scan(), which
# is the number the fused scan actually sums.  k = ndocs, so it is exhaustive.  What
# is left that could move the answer is the fused scorer's pruning and summation,
# which is exactly what is under test.
#
# A TIE STRADDLING RANK 10 makes "the" top-10 ambiguous however the scorer behaves,
# and the quantized vector term makes ties likelier than in the regression fixture.
# Such queries are SKIPPED and COUNTED rather than failed, because an ambiguous
# oracle is not evidence either way.
# ---------------------------------------------------------------------------
say "$DS: docid map for the oracle"
$PSQL >/dev/null <<SQL
DROP TABLE IF EXISTS fdmap;
CREATE TABLE fdmap AS
  SELECT t.id, t.rowtid, l.docid
    FROM (SELECT id, ctid AS rowtid, row_number() OVER (ORDER BY ctid) AS rn
            FROM fd) t
    JOIN (SELECT docid, row_number() OVER (ORDER BY docid) AS rn
            FROM weave_vec_lanes('fd_weave')) l USING (rn);
SQL
MAPOK=$($PSQL -t -A -c "SELECT count(*) = (SELECT count(*) FROM fd) FROM fdmap;")
[ "$MAPOK" = "t" ] || die "the docid map does not cover every row: weave_vec_lanes() and ctid order disagree"

# THE GATE CHECKS BOTH OBJECTIVES, AND RECOMPUTES BOTH CEILINGS.  doc/GAPS.md G46.
#
# pg_weave.fuse_normalize (default on) divides each fuse() key's weight by that key's
# pre-scan ceiling N_key (src/am/amscan.c, "w_key / N_key").  Until 2026-09-22 this
# oracle computed only the raw sum, so the gate had to run with the normalizer OFF --
# a gate testing a non-default configuration.  681f9c4 moved it to the normalized
# objective and dropped the raw one; 2026-10-04 restores the raw check beside it, so
# a run now checks:
#
#   * NORMALIZED: the default-configuration pushdown against
#     0.5 * lex / N_L + 0.5 * vec / N_V;
#   * RAW: the same pushdown under `SET pg_weave.fuse_normalize = off` against
#     0.5 * lex + 0.5 * vec.
#
# BOTH CEILINGS ARE RECOMPUTED HERE, IN SQL, rather than asked for, because an
# accessor that returned the scan's own number would share code with the thing it
# checks:
#
#   * N_L = sum over terms of idf(df, ndocs) * mtf * (k1+1) / (mtf + k1*(1-b)), from
#     weave_index_df(), weave_index_max_tf() and weave_index_stats(); k1 = 1.2 and
#     b = 0.75 are the scan's hardcoded constants (weave_bm25_factors_init call sites)
#     and the expression is weave_bm25_term_bound() at the max tf.
#   * N_V = max over directory records of maxrecnorm, from weave_vec_blocks(), times
#     ||q||, from the query's own real[] -- bound (B2) in the ip domain.  Until
#     2026-10-04 this came from weave_vec_scan_stats().maxscore, which is produced by
#     the SAME fold (weave_vec_scan_maxscore()) the scan uses, so a defect in that
#     fold moved the oracle with the scan and was invisible.  The stats number is now
#     a CROSS-CHECK against the recompute, not the oracle's input.
#
# AND THE SCAN'S OWN N_key, under pg_weave.fuse_check_bounds: its per-channel NOTICE
# prints the effective weight w = w_key / N_key (amscan.c, "chan %d kind=%s ... w=%g"),
# so 0.5 / w must equal the recomputed N for every lexical and every vector channel.
# The NOTICE has to FIRE on every query before its agreement counts -- a GUC that does
# not exist is accepted as a placeholder and is silent (AGENTS.md, twelfth member).
#
# A TIE IS A TOLERANCE, NOT AN EQUALITY.  The scan sums in float4 and the oracle in
# float8, so "the top-10 set is unique" means the score gap between ranks 10 and 11
# exceeds FUSE_TIE_TOL (default 1e-5) times the largest |score|.  A query whose cut
# is inside the tolerance is reported as TIED for that objective -- neither a pass
# nor a fail.  Before 2026-10-04 the test was exact float equality, which counted a
# 1-ulp gap as a clean cut.
#
# GATE_PRE exists for ONE purpose: the positive control.  `SET pg_weave.fuse_normalize
# = off;` makes the default-configuration pushdown compute the raw objective, so the
# normalized comparison and the NOTICE check must both report mismatches -- measured on
# nfcorpus, 2026-09-23 on 25 queries: 25 of 25 AGREE with it unset, and 14 of 25
# MISMATCH with it set.  The raw arm SETs the GUC itself and is unaffected.  The
# mutants M1-M3 of doc/GAPS.md G46 are the stronger control: each perturbs one ceiling
# in the build, and each must fail this gate.
TOL=${FUSE_TIE_TOL:-1e-5}
MAXREC=$($PSQL -t -A -c "SELECT max(maxrecnorm)::float8 FROM weave_vec_blocks('fd_weave');")
[ -n "$MAXREC" ] || die "weave_vec_blocks('fd_weave') returned no directory records"
GATESET="$SETUP ${GATE_PRE:-}"
say "$DS: correctness gate on $CHECKN queries (pushdown vs exhaustive per-channel oracle, normalized AND raw objectives, N_key recomputed)"

# nkey_ok <expected N> <w values, one per line>: 0 agree, 1 disagree, 2 no values.
# A non-positive N leaves the weight alone in the scan, which is a divisor of 1.
nkey_ok() {
    printf '%s\n' "$2" | awk -v n="$1" 'BEGIN { d = (n > 0) ? n : 1 }
        NF { c++; g = 0.5 / $1; if (g - d > 1e-4 * d || d - g > 1e-4 * d) bad = 1 }
        END { exit (c == 0) ? 2 : bad }'
}

BAD=0; TIED=0; BADR=0; TIEDR=0; NKBAD=0; NVDIFF=0; FBDIFF=0
while IFS=$'\t' read -r qid wq qv; do
    got=$( { echo "$GATESET SET enable_seqscan = off;"; fused_sql "$wq" "$qv" 10; } \
           | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A | sort -n | tr '\n' ' ')
    gotr=$( { echo "$GATESET SET enable_seqscan = off; SET pg_weave.fuse_normalize = off;"; fused_sql "$wq" "$qv" 10; } \
           | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A | sort -n | tr '\n' ' ')
    # stderr only: the NOTICE lines.  A (C2) violation raises here too, and fails the run.
    nt=$( { echo "$GATESET SET enable_seqscan = off; SET pg_weave.fuse_check_bounds = on;"; fused_sql "$wq" "$qv" 10; } \
          | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A 2>&1 >/dev/null) \
        || die "qid=$qid: the fused scan raised under fuse_check_bounds: $nt"

    # The fallback, kept as a DIAGNOSTIC rather than a gate: the size of the
    # disagreement is sect. 7a (1) measured at corpus scale, which no test has done.
    fb=$( { echo "$GATESET SET enable_indexscan = off; SET enable_bitmapscan = off;"; fused_sql "$wq" "$qv" 10; } \
          | psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A | sort -n | tr '\n' ' ')
    [ "$got" = "$fb" ] || FBDIFF=$((FBDIFF + 1))

    IFS=$'\t' read -r nl nv nvs clean_n want clean_r wantr < <(psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F $'\t' <<SQL
WITH t AS (
    SELECT df, mtf
      FROM unnest(weave_index_df('fd_weave', $wq::wquery),
                  weave_index_max_tf('fd_weave', $wq::wquery)) AS u(df, mtf)
), k AS (
    SELECT COALESCE((SELECT sum(CASE WHEN t.mtf = 0 THEN 0::float8
                                     ELSE ln(1.0 + ((SELECT ndocs FROM weave_index_stats('fd_weave'))
                                                    - t.df + 0.5) / (t.df + 0.5))
                                          * t.mtf * 2.2 / (t.mtf + 1.2 * (1.0 - 0.75))
                                END) FROM t), 0) AS nl,
           $MAXREC::float8 * (SELECT sqrt(sum(x::float8 * x::float8))
                                FROM unnest($qv::wvec::real[]) x) AS nv,
           COALESCE((SELECT max(maxscore)::float8
                       FROM weave_vec_scan_stats('fd_weave', $qv::wvec, 1)), -1) AS nvs
), d AS (
    SELECT CASE WHEN nl > 0 THEN nl ELSE 1 END AS dl,
           CASE WHEN nv > 0 THEN nv ELSE 1 END AS dv FROM k
), s AS (
    SELECT m.id, COALESCE(a.score, 0::float8) AS l, COALESCE(v.score::float8, 0::float8) AS v
      FROM fdmap m
      LEFT JOIN weave_search('fd_weave', $wq::wquery, $NDOCS) a ON a.ctid = m.rowtid
      LEFT JOIN weave_vec_scan('fd_weave', $qv::wvec, $NDOCS) v ON v.docid = m.docid
), o AS (
    SELECT 'n'::text AS obj, s.id, 0.5 * s.l / d.dl + 0.5 * s.v / d.dv AS score FROM s, d
    UNION ALL
    SELECT 'r', s.id, 0.5 * s.l + 0.5 * s.v FROM s
), r AS (
    SELECT obj, id, score,
           row_number() OVER (PARTITION BY obj ORDER BY score DESC, id) AS rn,
           max(abs(score)) OVER (PARTITION BY obj) AS top
      FROM o
)
-- CLEAN means the top-10 SET is unique: only ranks 10 and 11 matter, ties inside the
-- top 10 cannot change a set (the 2026-09 lesson: a whole-corpus distinctness test
-- skipped 100 of 100 queries on two datasets).  No rank 11 is clean.
SELECT k.nl, k.nv, k.nvs,
       COALESCE((SELECT x.score - y.score > $TOL * x.top
                   FROM r x JOIN r y ON y.obj = x.obj AND y.rn = 11
                  WHERE x.obj = 'n' AND x.rn = 10), true),
       (SELECT string_agg(id::text, ' ' ORDER BY id) FROM r WHERE obj = 'n' AND rn <= 10),
       COALESCE((SELECT x.score - y.score > $TOL * x.top
                   FROM r x JOIN r y ON y.obj = x.obj AND y.rn = 11
                  WHERE x.obj = 'r' AND x.rn = 10), true),
       (SELECT string_agg(id::text, ' ' ORDER BY id) FROM r WHERE obj = 'r' AND rn <= 10)
  FROM k;
SQL
)
    [ -n "$want" ] || die "qid=$qid: the oracle returned nothing"

    # The vector ceiling, recomputed vs the shuttle's fold.
    if ! awk -v a="$nv" -v b="$nvs" 'BEGIN { e = a - b; if (e < 0) e = -e; exit !(e <= 1e-4 * (a < 0 ? -a : a)) }'; then
        printf 'NKEY qid=%s: vector ceiling recomputed %s, weave_vec_scan_stats %s\n' "$qid" "$nv" "$nvs" >&2
        NVDIFF=$((NVDIFF + 1))
    fi

    # The scan's own N_key, read off its NOTICE.
    wl=$(printf '%s\n' "$nt" | sed -n 's/.* kind=lexical .* w=\([^ ]*\).*/\1/p' | sort -u)
    wv=$(printf '%s\n' "$nt" | sed -n 's/.* kind=vector-scan .* w=\([^ ]*\).*/\1/p' | sort -u)
    [ -n "$wv" ] || die "qid=$qid: no vector-scan NOTICE under pg_weave.fuse_check_bounds -- \
the check did not run, so its silence is not evidence (is the GUC real in this build?)"
    rc=0; nkey_ok "$nv" "$wv" || rc=$?
    if [ "$rc" != 0 ]; then
        printf 'NKEY qid=%s: vector N from the scan %s, recomputed %s\n' "$qid" \
            "$(printf '%s ' $wv | awk '{for(i=1;i<=NF;i++) printf "%g ", 0.5/$i}')" "$nv" >&2
        NKBAD=$((NKBAD + 1))
    fi
    if awk -v n="$nl" 'BEGIN { exit !(n > 0) }'; then
        [ -n "$wl" ] || die "qid=$qid: N_L = $nl > 0 but no lexical NOTICE fired"
        rc=0; nkey_ok "$nl" "$wl" || rc=$?
        if [ "$rc" != 0 ]; then
            printf 'NKEY qid=%s: lexical N from the scan %s, recomputed %s\n' "$qid" \
                "$(printf '%s ' $wl | awk '{for(i=1;i<=NF;i++) printf "%g ", 0.5/$i}')" "$nl" >&2
            NKBAD=$((NKBAD + 1))
        fi
    fi

    if [ "$clean_n" != "t" ]; then
        TIED=$((TIED + 1))
    elif [ "$(echo $got)" != "$(echo $want)" ]; then
        printf 'MISMATCH qid=%s (normalized)\n  pushdown: %s\n  oracle  : %s\n' "$qid" "$got" "$want" >&2
        BAD=$((BAD + 1))
    fi
    if [ "$clean_r" != "t" ]; then
        TIEDR=$((TIEDR + 1))
    elif [ "$(echo $gotr)" != "$(echo $wantr)" ]; then
        printf 'MISMATCH qid=%s (raw)\n  pushdown: %s\n  oracle  : %s\n' "$qid" "$gotr" "$wantr" >&2
        BADR=$((BADR + 1))
    fi
done < <(head -n "$CHECKN" "$QLIT")
CHECKED=$((CHECKN - TIED))
CHECKEDR=$((CHECKN - TIEDR))
say "$DS: gate tally: normalized $BAD mismatched / $CHECKED compared / $TIED tied; raw $BADR / $CHECKEDR / $TIEDR; N_key NOTICE disagreed on $NKBAD, vector ceiling vs stats on $NVDIFF"
[ "$BAD" -eq 0 ] || die "$BAD of $CHECKN queries disagree with the exhaustive per-channel oracle (normalized objective)"
[ "$BADR" -eq 0 ] || die "$BADR of $CHECKN queries disagree with the exhaustive per-channel oracle (raw objective)"
[ "$NKBAD" -eq 0 ] || die "the scan's N_key disagrees with the SQL recompute on $NKBAD channel-queries"
[ "$NVDIFF" -eq 0 ] || die "weave_vec_scan_stats().maxscore disagrees with the recomputed vector ceiling on $NVDIFF queries"
# A GATE THAT COMPARED NOTHING DID NOT PASS.  `BAD` is 0 both when every query
# agreed and when every query was skipped, and those two states printed the same
# line until nfcorpus and fiqa skipped 100 of 100 and still said "gate passed".
[ "$CHECKED" -gt 0 ] || die "the normalized gate compared 0 of $CHECKN queries \
(all tied at the cut) -- it has proved nothing; fix the oracle before \
believing any number from this dataset"
[ "$CHECKEDR" -gt 0 ] || die "the raw gate compared 0 of $CHECKN queries (all tied at the cut)"
# And a mostly-skipped gate is weak evidence even when it is not vacuous, so the
# threshold is loud rather than silent.
if [ "$CHECKED" -lt $(( CHECKN / 2 )) ] || [ "$CHECKEDR" -lt $(( CHECKN / 2 )) ]; then
    printf 'WARNING: the correctness gate compared only %s (normalized) / %s (raw) of %s queries\n' \
        "$CHECKED" "$CHECKEDR" "$CHECKN" >&2
fi
say "$DS: gate passed (normalized: $CHECKED of $CHECKN COMPARED, $TIED tied; raw: $CHECKEDR COMPARED, $TIEDR tied; N_key checked on every query; the fuse() fallback differed on $FBDIFF)"
if [ "${FUSE_GATE_ONLY:-0}" = 1 ]; then
    say "FUSE_GATE_ONLY=1: stopping after the gate"
    exit 0
fi

# The plan is asserted, not hoped for: if the pushdown was not chosen, every latency
# number below is measuring the fallback and the comparison is meaningless.
# IFS=$'\t' IS LOAD-BEARING, exactly as in the gate loop above: a query literal
# contains SPACES (multi-term queries are OR-joined, `a | b | c`), so a default-IFS
# read splits one row into the wrong three fields and the EXPLAIN below is then
# handed a malformed statement.  It fails as "the fused arm is NOT using the index",
# which reads as a planner regression and is not one -- the statement never parsed.
IFS=$'\t' read -r qid1 wq1 qv1 < <(head -1 "$QLIT")
PLAN_FUSED=$( { echo "$SETUP"; printf 'EXPLAIN %s' "$(fused_sql "$wq1" "$qv1" 10)"; } \
              | psql -X -q -d "$DB" -t -A | tr '\n' ' ')
case "$PLAN_FUSED" in
    *"Index Scan using fd_weave"*) : ;;
    *) die "the fused arm is NOT using the index -- plan was: $PLAN_FUSED" ;;
esac
PLAN_RRF=$( { echo "$SETUP"; printf 'EXPLAIN %s' "$(rrf_sql "$wq1" "$qv1" 10)"; } \
            | psql -X -q -d "$DB" -t -A | tr '\n' ' ')
case "$PLAN_RRF" in
    *"Index Scan using fd_weave"*) : ;;
    *) die "the RRF arm is NOT using the index -- plan was: $PLAN_RRF" ;;
esac

# ---------------------------------------------------------------------------
# QUALITY PASS.  Every query, once, at depth QDEPTH, ranks taken from psql's output
# ORDER -- which is the row order of a single ORDER BY query and is therefore the
# ranking itself, with no window function to get wrong.
# ---------------------------------------------------------------------------
run_quality() {                 # run_quality <arm>
    local arm=$1 gen=$2 out="$OUT/run-$DS-$1.tsv"
    local sqlf; sqlf=$(mktemp)
    {
        echo "$SETUP ${FUSE_ARM_PRE:-}"
        while IFS=$'\t' read -r qid wq qv; do
            printf "\\\\echo QID %s\n" "$qid"
            $gen "$wq" "$qv" "$QDEPTH"
            printf '\n'
        done < "$QLIT"
    } > "$sqlf"
    psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -f "$sqlf" \
      | awk -v OFS='\t' '/^QID /{q=$2; r=0; next} NF{r++; print q, $1, r}' > "$out"
    rm -f "$sqlf"
    [ -s "$out" ] || die "$arm produced no rows"
    printf '%s\n' "$out"
}

say "$DS: quality pass, fused arm (normalizer ON, the shipping default)"
RUN_FUSED=$(run_quality fused fused_sql)
# THE SAME STATEMENT WITH THE NORMALIZER OFF, which is the arm every figure recorded
# before 2026-09-22 was measured on.  It is here so that one run carries both
# objectives: a re-run that cannot reproduce its own baseline is not an A/B (hard
# rule 10), and the baseline is a GUC away rather than a rebuild away.
say "$DS: quality pass, fused arm (normalizer OFF, the recorded baseline)"
RUN_RAW=$(FUSE_ARM_PRE="SET pg_weave.fuse_normalize = off;" run_quality raw fused_sql)
say "$DS: quality pass, RRF control arm"
RUN_RRF=$(run_quality rrf rrf_sql)
# The G44 normalization study, opt-in: two candidate objectives scored through the
# same run-file + ndcg.py path as the two real arms, so a scheme cannot win here by
# being measured differently.
RUN_MAXN=""; RUN_MMN=""
if [ "${FUSE_NORMSTUDY:-0}" = 1 ]; then
    say "$DS: quality pass, normalization study (maxn)"
    RUN_MAXN=$(run_quality maxn normsum_sql)
    say "$DS: quality pass, normalization study (mmn)"
    RUN_MMN=$(run_quality mmn minmaxsum_sql)
fi

# ---------------------------------------------------------------------------
# WORK COUNTERS.  One arm's whole quality pass, bracketed by resets.
#
# Read AFTER the quality pass rather than around a single query, because a per-query
# read would need a round trip per query and the totals are what the ratio is over.
# The counters are backend-local, so each arm gets its own psql session.
# ---------------------------------------------------------------------------
work_of() {                     # work_of <gen> -- prints one TSV row
    local gen=$1
    local sqlf; sqlf=$(mktemp)
    {
        echo "$SETUP ${FUSE_ARM_PRE:-}"
        echo "SELECT weave_fuse_stats_reset(); SELECT weave_work_stats_reset();"
        while IFS=$'\t' read -r qid wq qv; do
            $gen "$wq" "$qv" 10
            printf '\n'
        done < "$QLIT"
        # scores minus the vector adapters' share minus the gates' share IS the
        # lexical contribution count, which is the unit the control's lex_contribs
        # is in.  Derived here rather than assumed, per the 0.19.0 migration note.
        echo "SELECT f.scores - f.vec_scores - f.gate_scores, w.lex_contribs, w.vec_lanes, w.vec_blocks, w.vec_blocks_bound_skipped, f.scores, f.pivots, f.blkskip, f.rqskip, f.passes, f.runs, f.abandon, f.veto, f.bounds, f.seeks FROM weave_fuse_stats() f, weave_work_stats() w;"
    } > "$sqlf"
    psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F $'\t' -f "$sqlf" | tail -1
    rm -f "$sqlf"
}

say "$DS: work counters, fused arm (normalizer ON)"
W_FUSED=$(work_of fused_sql)
say "$DS: work counters, fused arm (normalizer OFF)"
W_RAW=$(FUSE_ARM_PRE="SET pg_weave.fuse_normalize = off;" work_of fused_sql)
say "$DS: work counters, RRF control arm"
W_RRF=$(work_of rrf_sql)

# ---------------------------------------------------------------------------
# LATENCY.  A/B ALTERNATED per query: fused, rrf, fused, rrf ... so drift over the
# pass cannot land on one arm (weave-bench, and the AWS skill's A/B rule).
#
# AND AN A/A LEG, which is AGENTS.md hard rule 10 and is not optional: the fused
# arm is measured TWICE per query, in the first and third slots, and the gap
# between those two measurements is the WITHIN-ARM SPREAD.  A between-arm delta
# smaller than that spread is not a result.  The sibling project's retracted
# 8.5 % "win" was a single baseline outlier, and the only thing that would have
# caught it is this leg.
#
# The second fused measurement sits AFTER the rrf one deliberately, rather than
# back-to-back with the first.  Back-to-back would measure the best case -- same
# cache, same everything -- and understate the spread.  Third slot means it
# carries whatever the rrf run did to the cache and to the clock, which is the
# conservative estimate and the one that can actually bound a claim.
# ---------------------------------------------------------------------------
say "$DS: latency, $LATN queries x $REPS reps, arms alternated, with an A/A leg"
# A FOURTH LEG, added 2026-09-22: the fused arm with the normalizer OFF.  The
# normalizer adds a pre-scan pass -- one LUT build and one directory fold per bolt
# per vector key -- and nothing had measured it.  Alternated in the same rotation as
# the others so drift cannot land on it, and reported as its own row: fused minus
# fused_raw IS the normalizer's cost, measured rather than argued.
LAT_F=$(mktemp); LAT_R=$(mktemp); LAT_F2=$(mktemp); LAT_RAW=$(mktemp)
while IFS=$'\t' read -r qid wq qv; do
    for leg in "fused_sql:$LAT_F:" "rrf_sql:$LAT_R:" "fused_sql:$LAT_F2:" "fused_sql:$LAT_RAW:SET pg_weave.fuse_normalize = off;"; do
        gen=$(echo "$leg" | cut -d: -f1)
        dest=$(echo "$leg" | cut -d: -f2)
        pre=$(echo "$leg" | cut -d: -f3-)
        {
            echo "$SETUP $pre"
            for _ in $(seq 1 "$REPS"); do
                printf 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) %s\n' "$($gen "$wq" "$qv" 10)"
            done
        } | psql -X -q -d "$DB" -t -A \
          | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p' \
          | tail -n +2 \
          >> "$dest"
    done
done < <(head -n "$LATN" "$QLIT")

pctl() {                        # pctl <file> -- p50 TAB p99
    sort -g "$1" | awk '{v[NR]=$1}
        END { if (NR==0) { printf "0\t0"; exit }
              p=int(NR*0.99); if (p<1) p=NR;
              printf "%.3f\t%.3f", v[int((NR+1)/2)], v[p] }'
}
P_FUSED=$(pctl "$LAT_F")
P_RRF=$(pctl "$LAT_R")
P_FUSED2=$(pctl "$LAT_F2")
P_RAW=$(pctl "$LAT_RAW")
rm -f "$LAT_F" "$LAT_R" "$LAT_F2" "$LAT_RAW"

# ---------------------------------------------------------------------------
# SCORE.  bench/ndcg.py excludes queries with no positive judgment and PENALIZES
# queries missing from a run; both counts are in its output.
# ---------------------------------------------------------------------------
Q_FUSED=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_FUSED" --k 10 --label fused | tail -1)
Q_RRF=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_RRF" --k 10 --label rrf | tail -1)
Q_RAW=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_RAW" --k 10 --label raw | tail -1)
Q_MAXN=""; Q_MMN=""
if [ -n "$RUN_MAXN" ]; then
    Q_MAXN=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_MAXN" --k 10 --label maxn | tail -1)
    Q_MMN=$(python3 bench/ndcg.py --qrels "$DIR/qrels.tsv" --run "$RUN_MMN" --k 10 --label mmn | tail -1)
fi

# ---------------------------------------------------------------------------
# Report.  TSV on stdout, one table per concern, for bench/aws/run.sh to tee.
# ---------------------------------------------------------------------------
printf '\n### fuse_setup\n'
printf 'dataset\tembed\tdim\tndocs\tnqueries\tdropped_no_terms\ttruncated_at_%s\tindex_mb\tbuild_s\tweave_version\tkprime\trrf_k\n' "$MAXTERMS"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
       "$DS" "$EMBED" "$DIM" "$NDOCS" "$NQ" "$NOWQ" "$TRUNC" "$IDXMB" "$BUILD_S" "$WEAVEVER" "$KP" "$RRFK"

printf '\n### fuse_correctness\n'
# `compared` is the load-bearing column, not `attempted`: the recall row of sect. 8
# is only as strong as the number of queries whose oracle was unambiguous, and
# reporting only the attempt count is what let a 0-of-100 gate read as a pass.
# The raw columns and the N_key columns since 2026-10-04 (doc/GAPS.md G46); the
# tolerance is the one the tie test used.
printf 'attempted\tcompared\tmismatched_vs_oracle\tskipped_tied_oracle\traw_compared\traw_mismatched\traw_tied\tnkey_notice_disagreed\tvec_ceiling_vs_stats_disagreed\ttie_tol\tfallback_differed\n'
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$CHECKN" "$CHECKED" "$BAD" "$TIED" "$CHECKEDR" "$BADR" "$TIEDR" "$NKBAD" "$NVDIFF" "$TOL" "$FBDIFF"

printf '\n### fuse_quality\n'
# Five columns, matching what bench/ndcg.py's data line actually emits.  The
# header used to promise excluded_no_positive and missing_from_run as well, but
# ndcg.py writes those to STDERR as `#` comments (they are diagnostics about the
# qrels, not per-arm measurements), so the table advertised two columns it never
# filled -- which reads, in a results file, as two zeros nobody measured.
printf 'label\tnqueries_scored\tndcg@10\trecall@100\tmrr@10\n'
printf '%s\n%s\n%s\n' "$Q_FUSED" "$Q_RAW" "$Q_RRF"
# `fused` and `maxn` differ ONLY in the objective -- same index, same query set, same
# per-channel scores, same scorer -- so the gap between those two rows is the whole
# value of normalization, isolated.  `fused` reproducing its recorded number is also
# the positive control for the study: if it does not, the study is measuring
# something else.
[ -n "$Q_MAXN" ] && printf '%s\n%s\n' "$Q_MAXN" "$Q_MMN"

printf '\n### fuse_latency\n'
printf 'arm\tp50_ms\tp99_ms\tqueries\treps\n'
printf 'fused\t%s\t%s\t%s\n' "$P_FUSED" "$LATN" "$REPS"
printf 'rrf\t%s\t%s\t%s\n' "$P_RRF" "$LATN" "$REPS"
# The A/A leg (hard rule 10).  `fused_aa` is the SAME arm measured again in the
# third slot; the fused-vs-fused_aa gap is the noise floor any fused-vs-rrf claim
# has to clear.  Reported as a row rather than folded into the fused numbers so
# that nobody can average the two and lose the only estimate of spread there is.
printf 'fused_aa\t%s\t%s\t%s\n' "$P_FUSED2" "$LATN" "$REPS"
# The normalizer's cost: same statement, same rotation, one GUC apart.
printf 'fused_raw\t%s\t%s\t%s\n' "$P_RAW" "$LATN" "$REPS"

printf '\n### fuse_work\n'
printf 'arm\tlex_contribs_fused_side\tlex_contribs_wand\tvec_lanes\tvec_blocks\tvec_blocks_bound_skipped\tfuse_scores_total\tpivots\tblkskip\trqskip\tpasses\truns\tabandon\tveto\tbounds\tseeks\n'
printf 'fused\t%s\n' "$W_FUSED"
printf 'fused_raw\t%s\n' "$W_RAW"
printf 'rrf\t%s\n' "$W_RRF"

# ---------------------------------------------------------------------------
# THE WORK ROW, IN THE UNITS SECT. 8 WAS RESTATED TO USE ON 2026-09-22.
#
# It used to be one ratio over "score() calls", which counted the vector channel in
# LANES.  Two measurements killed that unit.  First, in WEAVE_PACK_LANE a one-lane
# read touches every byte of its block, so a lane is not a unit of cost -- a block is,
# and the block ratio is what a pruning mechanism has to move.  Second, the lane ratio
# said normalization made the fused arm cheaper on all three datasets while the CLOCK
# said fiqa's p50 doubled; a work row that cannot predict the latency row is measuring
# the wrong thing.  What tracks the clock is the PIVOT COUNT, so it is reported per
# query beside the ratios -- as a diagnostic, not a gate, because the RRF control has
# no pivot loop and there is therefore nothing to take a ratio against.
#
# Computed here, in the script, so that nobody divides two table rows by hand and
# picks the wrong columns -- which is how the 0.20x row was quoted for a month.
# ---------------------------------------------------------------------------
printf '\n### fuse_work_ratios\n'
printf 'arm\tlex_contribs_vs_control\tvec_blocks_vs_control\tvec_lanes_vs_control\tpivots_per_query\tabandon\tblkskip\n'
{ printf 'fused\t%s\n' "$W_FUSED"; printf 'fused_raw\t%s\n' "$W_RAW"; printf 'rrf\t%s\n' "$W_RRF"; } \
  | awk -F'\t' -v nq="$NQ" '{ for (i=2;i<=NF;i++) c[$1"_"i]=$i }
      END {
        split("fused fused_raw rrf", a, " ");
        for (k = 1; k <= 3; k++) {
            n = a[k];
            lex = (n == "rrf") ? c["rrf_3"] : c[n"_2"];
            printf "%s\t%.3f\t%.3f\t%.3f\t%.0f\t%d\t%d\n", n,
                   (c["rrf_3"] ? lex / c["rrf_3"] : 0),
                   (c["rrf_5"] ? c[n"_5"] / c["rrf_5"] : 0),
                   (c["rrf_4"] ? c[n"_4"] / c["rrf_4"] : 0),
                   (nq ? c[n"_8"] / nq : 0), c[n"_13"], c[n"_9"];
        }
      }'

printf '\n### fuse_plans\n'
printf 'arm\tplan\n'
printf 'fused\t%s\n' "$PLAN_FUSED"
printf 'rrf\t%s\n' "$PLAN_RRF"

say "$DS: done ($NDOCS docs, $NQ queries, correctness gate passed on $CHECKN)"
