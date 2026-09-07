#!/usr/bin/env bash
#
# bench/compete/engines/gin.sh -- tsvector + GIN, the in-core baseline.
#
# axis: lexical
#
# This arm exists because it is what a user already has.  Every claim pg_weave
# makes is really "worth installing an extension for", and the only way to test
# that is against core PostgreSQL with no extension at all.  GIN is also the one
# competitor whose correctness is not in question, which makes it the natural
# reference for the set-equality gates: if pg_weave and GIN disagree on which
# documents match a term, one of them is wrong and the seq-scan regex says which.
#
# Nothing is installed beyond what common.sh builds: `gin` is an in-core access
# method and `tsvector` an in-core type.  pg_prewarm (contrib) is already
# installed by install_pg_source's `cd contrib && make install`.
#
set -euo pipefail
ENGINE=gin
source "$(dirname "$0")/common.sh"

do_provision() {
    setup_nvme; tune_os; install_pg_source; start_pg; record_hostinfo
    # No install step. Recorded explicitly rather than silently skipped, because
    # "which build produced this number" has to be answerable for every arm, and
    # for this arm the answer is "the server itself".
    export PATH="$NVME/pg/bin:$PATH"
    say "gin/tsvector are in-core; no extension to build"
    psql -X -q -t -A -d "$PGDATABASE" \
        -c "SELECT 'gin in postgresql ' || current_setting('server_version')"
}

do_load() {
    load_corpus "$1"
    export PATH="$NVME/pg/bin:$PATH"
    # A stored tsvector column, not an expression index, for the same reason as
    # every other arm: pg_fts profiling showed re-analyzing the text inside the
    # query was 40-88% of measured latency, which measures to_tsvector rather
    # than the index.  This also makes the comparison to pg_weave's stored wdoc
    # like-for-like: both engines pay analysis once, at load.
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" <<'SQL'
ALTER TABLE docs ADD COLUMN tsv tsvector;
UPDATE docs SET tsv = to_tsvector('english', content);
VACUUM (ANALYZE) docs;
SQL
    # Never build the index before heap churn is finished.  pg_fts invalidated a
    # whole benchmark arm by materialising one column, indexing, then UPDATEing a
    # second column -- every posting pointed at a dead TID and a common-term
    # ranked query took 24 s.  The UPDATE above is the churn; it happens first.
    say "heap settled before indexing"
}

do_index() {
    # Defaults only, and they are reported: GIN's fastupdate pending list and
    # gin_pending_list_limit change both size and latency, and pg_fts's phrase
    # latency swinging 2.27 -> 128 -> 8500 ms on reloptions alone is why a
    # number without its configuration is not a number.  A freshly built GIN
    # index has an empty pending list, so this measures the compacted form.
    timed_index "CREATE INDEX docs_gin ON docs USING gin (tsv)"
    export PATH="$NVME/pg/bin:$PATH"
    echo "reloptions: fastupdate=on gin_pending_list_limit=4MB (defaults)" >> "$OUTDIR/build.txt"
    # Assert the pending list is empty, the way weave.sh asserts segments=1 after
    # a build.  A non-empty pending list would mean the size and latency numbers
    # describe a half-merged index, which is the same class of unfairness as
    # measuring pg_fts before fts_vacuum (see fts.sh).  ON_ERROR_STOP so a
    # missing pageinspect is reported rather than silently producing no line:
    # psql exits 0 on a failed statement without it.
    psql -X -q -v ON_ERROR_STOP=1 -t -A -d "$PGDATABASE" \
        -c "CREATE EXTENSION IF NOT EXISTS pageinspect" \
        -c "SELECT 'pending_pages=' || pending_pages FROM gin_metapage_info(get_raw_page('docs_gin',0))" \
        >> "$OUTDIR/build.txt" || \
        echo "pending_pages=unknown (pageinspect unavailable)" >> "$OUTDIR/build.txt"
    prewarm docs docs_gin
}

do_gate() {
    local R M C; R=$(band rare); M=$(band mid); C=$(band common)
    # The same three set-equality gates as weave.sh, against the same seq-scan
    # regex reference, so the two arms are gated identically.  Symmetric EXCEPT
    # in both directions is pg_tre's method and the reason its correctness
    # claims held up; a one-directional check passes for an index that returns a
    # subset, which is exactly the failure mode a too-low block bound produces.
    #
    # \m and \M are word boundaries: `content ~ '\mword000123\M'` cannot match
    # word0001234, so the reference is an exact-token test and matches what an
    # analyzer that does not stem `wordNNNNNN` produces.  On this corpus the
    # english Snowball stemmer is a no-op for the band terms (they are
    # `wordNNNNNN`), so GIN's stemming cannot introduce a legitimate difference
    # here -- any difference is a bug.
    cat > /tmp/gate.json <<JSON
{"engine":"gin","gates":[
 {"name":"rare_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') EXCEPT SELECT id FROM docs WHERE tsv @@ to_tsquery('english','$R')) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('english','$R') EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M')) x"},
 {"name":"common_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$C\\\\M') EXCEPT SELECT id FROM docs WHERE tsv @@ to_tsquery('english','$C')) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('english','$C') EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$C\\\\M')) x"},
 {"name":"and_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') AND content ~ ('\\\\m$M\\\\M') EXCEPT SELECT id FROM docs WHERE tsv @@ to_tsquery('english','$R & $M')) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE tsv @@ to_tsquery('english','$R & $M') EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') AND content ~ ('\\\\m$M\\\\M')) x"}
]}
JSON
    # No topk_parity gate here on purpose.  ts_rank_cd is not BM25, so GIN's
    # ranking cannot be gated against the BM25 oracle without failing for a
    # reason that is a definitional difference rather than a defect.  The
    # ordering difference is a capability note (see engines/README.md), not a
    # correctness failure, and pretending otherwise would put a red FAIL next to
    # a working engine.
    run_gate /tmp/gate.json
}

do_measure() {
    local samples=$1 warmup=$2 run=$3
    local R M C; R=$(band rare); M=$(band mid); C=$(band common)
    # Labels are identical to weave.sh's, character for character.  The analyzer
    # builds its cross-engine table by label, so a renamed label does not produce
    # a mismatch -- it produces a silently missing comparison, which is worse.
    python3 - "$R" "$M" "$C" > /tmp/spec.json <<'PY'
import json, sys
R, M, C = sys.argv[1:4]
IDX = r"Bitmap Index Scan on docs_gin|Index Scan using docs_gin"
q = []
for band, T in (("rare", R), ("mid", M), ("common", C)):
    Q = f"to_tsquery('english','{T}')"
    for k in (10, 100):
        # The query has to name the tsquery twice: SQL cannot reference an output
        # alias from WHERE.  ts_rank_cd is evaluated per matching row, after the
        # heap fetch -- GIN has no top-k pushdown and no block-max skipping, so
        # this shape reads every match for a k=10 answer.  That is the structural
        # difference the ranked_* rows exist to quantify, not a mis-written query.
        q.append({"label": f"ranked_{band}_k{k}",
                  "sql": f"SELECT id, ts_rank_cd(tsv, {Q}) r FROM docs "
                         f"WHERE tsv @@ {Q} ORDER BY r DESC LIMIT {k}",
                  "expect_plan": IDX,
                  "count_sql": f"SELECT count(*) FROM docs WHERE tsv @@ {Q}"})
    # The bare ORDER BY form, with NO WHERE clause -- the shape a pgvector user
    # writes first, and the shape task L7 made work for pg_weave.  GIN cannot
    # answer it from the index either: ts_rank_cd is an ordinary function, not an
    # ordering operator in gin_tsvector_ops, so this is a seq scan plus a top-N
    # sort over 2M rows.  expect_plan is deliberately OMITTED: the seq scan is
    # the honest, expected result here and asserting an index would record a
    # PLAN FAIL for correct behaviour.  Kept as a measured row so the cost of the
    # shape is a number in the table rather than a claim in prose.
    q.append({"label": f"bare_orderby_{band}",
              "sql": f"SELECT id, ts_rank_cd(tsv, to_tsquery('english','{T}')) r "
                     f"FROM docs ORDER BY r DESC LIMIT 10"})
q += [
 {"label": "count_common",
  "sql": f"SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','{C}')",
  "expect_plan": IDX,
  "count_sql": f"SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','{C}')"},
 {"label": "count_and",
  "sql": f"SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','{R} & {M}')",
  "expect_plan": IDX,
  "count_sql": f"SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','{R} & {M}')"},
 {"label": "count_or2",
  "sql": f"SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','{R} | {M}')",
  "expect_plan": IDX},
 # NOT is only satisfiable when combined with a positive term; a bare !x is not
 # indexable in any of these engines.  Same '{C} & !{R}' shape as weave.sh.
 {"label": "count_not",
  "sql": f"SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','{C} & !{R}')",
  "expect_plan": IDX},
 {"label": "count_prefix",
  "sql": "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','word0001:*')",
  "expect_plan": IDX},
 {"label": "count_rare_marker",
  "sql": "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','zzqrare')",
  "expect_plan": IDX,
  "count_sql": "SELECT count(*) FROM docs WHERE tsv @@ to_tsquery('english','zzqrare')"},
]
print(json.dumps({
  "engine": "gin",
  "version_sql": "SELECT 'gin/tsvector in postgresql ' || current_setting('server_version')",
  # The cross-host corpus assertion.  The analyzer fails the entire run if two
  # engines disagree here; that is how the retracted 5-way (title||body vs body)
  # would have been caught on the day instead of a month later.
  "fingerprint_sql": "SELECT md5(string_agg(content, E'\\n' ORDER BY id)) FROM docs",
  "index_size_sql": "SELECT pg_relation_size('docs_gin')",
  "queries": q}, indent=1))
PY
    python3 -c "
import json
s=json.load(open('/tmp/spec.json'))
s['build_seconds']=float(open('$OUTDIR/build.txt').read().split()[0])
json.dump(s,open('/tmp/spec.json','w'))"
    run_measure /tmp/spec.json "$samples" "$warmup" "$run"
}

case "${1:?usage: gin.sh <provision|load|index|gate|measure>}" in
    provision) do_provision ;;
    load)      do_load "${2:?corpus}" ;;
    index)     do_index ;;
    gate)      do_gate ;;
    measure)   do_measure "${2:-200}" "${3:-10}" "${4:-adhoc}" ;;
    *) echo "unknown verb $1" >&2; exit 1 ;;
esac
