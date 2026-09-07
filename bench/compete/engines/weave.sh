#!/usr/bin/env bash
#
# bench/compete/engines/weave.sh -- pg_weave, the subject under test.
#
# axis: lexical
#
# This file is also the reference implementation of the engine contract:
#   provision   install PostgreSQL and the engine
#   load <c>    fetch/generate the shared corpus, verify it, materialise `content`
#   index       create the index, record build seconds
#   gate        run correctness gates; emits gates.jsonl
#   measure     emit raw timing samples; emits raw.jsonl
#
# Every other engine script implements the same five verbs over the same corpus
# and the same `content` column.
#
set -euo pipefail
ENGINE=weave
source "$(dirname "$0")/common.sh"

do_provision() {
    setup_nvme; tune_os; install_pg_source; start_pg; record_hostinfo
    say "building pg_weave"
    export PATH="$NVME/pg/bin:$PATH"
    cd "$HOME/pg_weave"
    make -s PG_CONFIG="$NVME/pg/bin/pg_config" >/dev/null
    make -s install PG_CONFIG="$NVME/pg/bin/pg_config" >/dev/null
    psql -X -q -d "$PGDATABASE" -c "CREATE EXTENSION IF NOT EXISTS pg_weave"
    psql -X -q -t -A -d "$PGDATABASE" \
        -c "SELECT 'pg_weave ' || extversion FROM pg_extension WHERE extname='pg_weave'"
}

do_load() {
    load_corpus "$1"
    export PATH="$NVME/pg/bin:$PATH"
    # Materialise the analyzed column. An EXPRESSION index would make every query
    # re-analyze the text inside the ORDER BY; pg_fts profiling showed that was
    # 40-88% of measured latency, i.e. it measures the analyzer rather than the
    # index. Every engine here indexes a stored column for the same reason.
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" <<'SQL'
ALTER TABLE docs ADD COLUMN d wdoc;
UPDATE docs SET d = to_wdoc('english', content);
VACUUM (ANALYZE) docs;
SQL
    # Never build an index before heap churn is finished. pg_fts invalidated an
    # entire benchmark arm by materialising one column, building the index, then
    # UPDATEing a second column -- leaving every posting's TID dead and turning a
    # common-term ranked query into 24 s.
    say "heap settled before indexing"
}

do_index() {
    # Reloptions are REPORTED, not implied: pg_fts's phrase latency swung
    # 2.27 -> 128 -> 8500 ms purely on reloptions, so a number without its
    # configuration is meaningless.
    timed_index "CREATE INDEX docs_weave ON docs USING weave (d)"
    export PATH="$NVME/pg/bin:$PATH"
    echo "reloptions: positions=off trigrams=off doclen_sidecar=on (defaults)" >> "$OUTDIR/build.txt"
    # As of task L8 a fresh CREATE INDEX is already compacted; assert it so a
    # regression shows up here rather than as a mysterious size difference.
    psql -X -q -t -A -d "$PGDATABASE" -c "SELECT 'segments=' || weave_index_nsegments('docs_weave')"
    psql -X -q -d "$PGDATABASE" -c \
        "SELECT kind, npages, pg_size_pretty(bytes) FROM weave_index_size_detail('docs_weave') WHERE npages>0 ORDER BY bytes DESC" \
        >> "$OUTDIR/build.txt"
    prewarm docs docs_weave
}

do_gate() {
    local R M C; R=$(band rare); M=$(band mid); C=$(band common)
    cat > /tmp/gate.json <<JSON
{"engine":"weave","gates":[
 {"name":"rare_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') EXCEPT SELECT id FROM docs WHERE d @@@ '$R'::wquery) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE d @@@ '$R'::wquery EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M')) x"},
 {"name":"common_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$C\\\\M') EXCEPT SELECT id FROM docs WHERE d @@@ '$C'::wquery) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE d @@@ '$C'::wquery EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$C\\\\M')) x"},
 {"name":"and_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') AND content ~ ('\\\\m$M\\\\M') EXCEPT SELECT id FROM docs WHERE d @@@ '$R & $M'::wquery) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE d @@@ '$R & $M'::wquery EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') AND content ~ ('\\\\m$M\\\\M')) x"},
 {"name":"topk_parity_vs_bm25_oracle","type":"topk_parity",
  "violations_sql":"WITH k AS (SELECT id, (d <=> '$M'::wquery) AS s FROM docs WHERE d @@@ '$M'::wquery ORDER BY d <=> '$M'::wquery LIMIT 10), kth AS (SELECT max(s) AS w FROM k) SELECT count(*) FROM k, kth WHERE k.s > kth.w * 1.01"}
]}
JSON
    run_gate /tmp/gate.json
}

do_measure() {
    local samples=$1 warmup=$2 run=$3
    local R M C; R=$(band rare); M=$(band mid); C=$(band common)
    # expect_plan is asserted for every query. A latency benchmark that does not
    # verify the access path is measuring an unknown plan -- pg_fts published a
    # 24 s seq scan as a ranked-index number, and pg_weave's own first six
    # competitive runs did the same (task L7).
    python3 - "$R" "$M" "$C" > /tmp/spec.json <<'PY'
import json, sys
R, M, C = sys.argv[1:4]
IDX = r"Index Scan using docs_weave|Custom Scan \(WeaveCount\)|Bitmap Index Scan on docs_weave"
q = []
for band, T in (("rare", R), ("mid", M), ("common", C)):
    for k in (10, 100):
        q.append({"label": f"ranked_{band}_k{k}",
                  "sql": f"SELECT id FROM docs WHERE d @@@ '{T}'::wquery "
                         f"ORDER BY d <=> '{T}'::wquery LIMIT {k}",
                  "expect_plan": IDX,
                  "count_sql": f"SELECT count(*) FROM docs WHERE d @@@ '{T}'::wquery"})
    # The bare pgvector-idiom form, which task L7 made work. Kept as its own
    # measurement because it is the form users write first.
    q.append({"label": f"bare_orderby_{band}",
              "sql": f"SELECT id FROM docs ORDER BY d <=> '{T}'::wquery LIMIT 10",
              "expect_plan": IDX})
q += [
 {"label": "count_common", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{C}'::wquery",
  "expect_plan": IDX, "count_sql": f"SELECT count(*) FROM docs WHERE d @@@ '{C}'::wquery"},
 {"label": "count_and", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{R} & {M}'::wquery",
  "expect_plan": IDX, "count_sql": f"SELECT count(*) FROM docs WHERE d @@@ '{R} & {M}'::wquery"},
 {"label": "count_or2", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{R} | {M}'::wquery",
  "expect_plan": IDX},
 {"label": "count_not", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{C} & !{R}'::wquery",
  "expect_plan": IDX},
 {"label": "count_prefix", "sql": "SELECT count(*) FROM docs WHERE d @@@ 'word0001*'::wquery",
  "expect_plan": IDX},
 {"label": "count_rare_marker", "sql": "SELECT count(*) FROM docs WHERE d @@@ 'zzqrare'::wquery",
  "expect_plan": IDX, "count_sql": "SELECT count(*) FROM docs WHERE d @@@ 'zzqrare'::wquery"},
]
print(json.dumps({
  "engine": "weave",
  "version_sql": "SELECT 'pg_weave ' || extversion FROM pg_extension WHERE extname='pg_weave'",
  "fingerprint_sql": "SELECT md5(string_agg(content, E'\\n' ORDER BY id)) FROM docs",
  "index_size_sql": "SELECT pg_relation_size('docs_weave')",
  "queries": q}, indent=1))
PY
    python3 -c "
import json
s=json.load(open('/tmp/spec.json'))
s['build_seconds']=float(open('$OUTDIR/build.txt').read().split()[0])
json.dump(s,open('/tmp/spec.json','w'))"
    run_measure /tmp/spec.json "$samples" "$warmup" "$run"
}

case "${1:?usage: weave.sh <provision|load|index|gate|measure>}" in
    provision) do_provision ;;
    load)      do_load "${2:?corpus}" ;;
    index)     do_index ;;
    gate)      do_gate ;;
    measure)   do_measure "${2:-200}" "${3:-10}" "${4:-adhoc}" ;;
    *) echo "unknown verb $1" >&2; exit 1 ;;
esac
