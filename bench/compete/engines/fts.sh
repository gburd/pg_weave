#!/usr/bin/env bash
#
# bench/compete/engines/fts.sh -- pg_fts, pg_weave's direct ancestor.
#
# axis: lexical
#
# The most informative arm in the set.  pg_weave is a fork of pg_fts, so this
# comparison isolates what the fork actually changed: the segment format, the
# self-compacting build (task L8), and amoptionalkey=true (task L7).  Where
# pg_weave is not faster than pg_fts, the fork bought nothing on that axis, and
# that is a result to publish rather than to omit (AGENTS.md hard rule 8).
#
# Because it is the ancestor, its SQL surface is nearly identical -- ftsdoc /
# ftsquery / @@@ / <=> instead of wdoc / wquery -- which is why the query labels
# below map one-to-one onto weave.sh's.
#
set -euo pipefail
ENGINE=fts
source "$(dirname "$0")/common.sh"

do_provision() {
    setup_nvme; tune_os; install_pg_source

    # The extension is built BEFORE start_pg, out of the usual verb order, for a
    # hard reason: pg_fts's count(*) pushdown is a CustomScan registered in
    # _PG_init via create_upper_paths_hook, and a hook installed at first
    # function use does not exist at plan time for the statement that triggered
    # the load.  pg_fts's own 4-way recorded this: "pg_fts must be in
    # shared_preload_libraries for the count(*) pushdown CustomScan, but the KNN
    # scan itself does not require it" (pg_fts/bench/RESULTS_4WAY_2026-07-29.md:
    # 103-104).  Preloading a library that is not yet installed makes the server
    # refuse to start, so the build has to come first.
    say "building pg_fts"
    export PATH="$NVME/pg/bin:$PATH"
    # The orchestrator uploads THIS repo to ~/pg_weave; pg_fts is a separate
    # repository, so it is either shipped to ~/pg_fts by the orchestrator or
    # cloned here.  The clone is the fallback, not the default: a network clone
    # can resolve to a different commit than the workstation checkout, and
    # "which commit produced this number" must stay answerable -- the sha is
    # recorded below and lands in the spec's version string either way.
    if [ ! -d "$HOME/pg_fts" ]; then
        say "no ~/pg_fts uploaded; cloning upstream"
        git clone --quiet https://codeberg.org/gregburd/pg_fts.git "$HOME/pg_fts"
    fi
    cd "$HOME/pg_fts"
    git rev-parse --short HEAD > "$OUTDIR/engine_commit.txt" 2>/dev/null || \
        echo unknown > "$OUTDIR/engine_commit.txt"
    make -s PG_CONFIG="$NVME/pg/bin/pg_config" >/dev/null
    make -s install PG_CONFIG="$NVME/pg/bin/pg_config" >/dev/null

    PRELOAD=pg_fts
    export PRELOAD
    start_pg; record_hostinfo
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" -c "CREATE EXTENSION IF NOT EXISTS pg_fts"
    # Fail loudly if the preload did not take.  Without it the count_* rows
    # silently measure a bitmap heap scan instead of the pushdown, which is a
    # 3x difference attributed to the wrong mechanism.  A DO block, not
    # `CASE ... ELSE 1/0`: constant subexpressions are folded before CASE is
    # evaluated, so the divide-by-zero trick errors unconditionally.
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" <<'SQL'
DO $$ BEGIN
  IF current_setting('shared_preload_libraries') NOT LIKE '%pg_fts%' THEN
    RAISE EXCEPTION 'pg_fts is not preloaded; the count(*) CustomScan would be absent';
  END IF;
END $$;
SQL
    psql -X -q -t -A -d "$PGDATABASE" \
        -c "SELECT 'pg_fts ' || extversion || ' @' || '$(cat "$OUTDIR/engine_commit.txt")'
              FROM pg_extension WHERE extname='pg_fts'"
}

do_load() {
    load_corpus "$1"
    export PATH="$NVME/pg/bin:$PATH"
    # Stored ftsdoc, not an expression index: pg_fts's own profiling showed
    # re-analyzing text inside the query was 40-88% of measured latency, i.e. it
    # measures the analyzer, not the index.  Every arm indexes a stored column.
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" <<'SQL'
ALTER TABLE docs ADD COLUMN d ftsdoc;
UPDATE docs SET d = to_ftsdoc('english', content);
VACUUM (ANALYZE) docs;
SQL
    # This exact ordering mistake -- index first, UPDATE a second column after --
    # is what turned a pg_fts common-term ranked query into 24 s by leaving every
    # posting pointing at a dead TID.  Churn first, index second, always.
    say "heap settled before indexing"
}

do_index() {
    # Compaction is INSIDE the timed statement, deliberately.
    #
    # pg_weave compacts at the end of CREATE INDEX (task L8); pg_fts does not,
    # and leaves a fresh build spread over unmerged segments plus the dead pages
    # earlier merges left behind.  Timing pg_fts's bare CREATE INDEX and then
    # compacting afterwards would report a fast build next to an index ~3.4x
    # larger than its compacted form -- unfair to pg_fts on size.  Compacting
    # first and timing only the CREATE INDEX would be unfair to pg_weave on time.
    # Charging pg_fts for the compaction it needs to reach the state pg_weave
    # reaches automatically is the only comparison where both columns describe
    # the same physical index.  All three statements go through one psql -c, so
    # they are one implicit transaction and one measured interval.
    #
    # fts_merge folds the pending list into the main segments; fts_vacuum does
    # the on-demand full compaction plus truncation that actually shrinks the
    # file (pg_fts--1.5.9.sql:352-363).  Neither has a
    # PreventInTransactionBlock guard, so the single-transaction form is valid;
    # if a future version adds one, this statement fails visibly and the
    # fallback is to time the three separately and sum them -- never to drop the
    # compaction and publish the inflated size.
    timed_index "CREATE INDEX docs_fts ON docs USING fts (d); SELECT fts_merge('docs_fts'); SELECT fts_vacuum('docs_fts')"
    export PATH="$NVME/pg/bin:$PATH"
    echo "build includes fts_merge + fts_vacuum (pg_fts does not self-compact; pg_weave does, task L8)" \
        >> "$OUTDIR/build.txt"
    echo "reloptions: defaults (positions off)" >> "$OUTDIR/build.txt"
    prewarm docs docs_fts
}

do_gate() {
    local R M C; R=$(band rare); M=$(band mid); C=$(band common)
    # The same four gates as weave.sh, on the same reference, so ancestor and
    # fork are held to one standard.  pg_fts's 5-way admitted "pg_fts is the only
    # engine whose ranked output was verified exact; the others were checked only
    # for expected row counts" (RESULTS_5WAY_158:158-161) -- the asymmetry that
    # produced is exactly what running identical gates on every arm removes.
    #
    # The ::ftsquery cast, not to_ftsquery('english',...), matches weave.sh's
    # ::wquery cast.  On this corpus the choice is immaterial (the band terms are
    # `wordNNNNNN`, which Snowball leaves alone) but it must match weave.sh, not
    # merely be defensible, or the two arms are answering different queries.
    cat > /tmp/gate.json <<JSON
{"engine":"fts","gates":[
 {"name":"rare_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') EXCEPT SELECT id FROM docs WHERE d @@@ '$R'::ftsquery) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE d @@@ '$R'::ftsquery EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M')) x"},
 {"name":"common_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$C\\\\M') EXCEPT SELECT id FROM docs WHERE d @@@ '$C'::ftsquery) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE d @@@ '$C'::ftsquery EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$C\\\\M')) x"},
 {"name":"and_setequal_vs_seqscan","type":"set_equality",
  "missing_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') AND content ~ ('\\\\m$M\\\\M') EXCEPT SELECT id FROM docs WHERE d @@@ '$R & $M'::ftsquery) x",
  "extra_sql":"SELECT count(*) FROM (SELECT id FROM docs WHERE d @@@ '$R & $M'::ftsquery EXCEPT SELECT id FROM docs WHERE content ~ ('\\\\m$R\\\\M') AND content ~ ('\\\\m$M\\\\M')) x"},
 {"name":"topk_parity_vs_bm25_oracle","type":"topk_parity",
  "violations_sql":"WITH k AS (SELECT id, (d <=> '$M'::ftsquery) AS s FROM docs WHERE d @@@ '$M'::ftsquery ORDER BY d <=> '$M'::ftsquery LIMIT 10), kth AS (SELECT max(s) AS w FROM k) SELECT count(*) FROM k, kth WHERE k.s > kth.w * 1.01"}
]}
JSON
    run_gate /tmp/gate.json
}

do_measure() {
    local samples=$1 warmup=$2 run=$3
    local R M C; R=$(band rare); M=$(band mid); C=$(band common)
    local SHA; SHA=$(cat "$OUTDIR/engine_commit.txt" 2>/dev/null || echo unknown)
    python3 - "$R" "$M" "$C" "$SHA" > /tmp/spec.json <<'PY'
import json, sys
R, M, C, SHA = sys.argv[1:5]
# The CustomScan node name is "FtsCount", read from
# pg_fts/pg_fts_customscan.c:90-100 (.CustomName), not guessed: EXPLAIN prints
# the CustomName inside "Custom Scan (...)", and a wrong name here would record a
# PLAN FAIL for a working pushdown.
IDX = r"Index Scan using docs_fts|Custom Scan \(FtsCount\)|Bitmap Index Scan on docs_fts"
q = []
for band, T in (("rare", R), ("mid", M), ("common", C)):
    for k in (10, 100):
        q.append({"label": f"ranked_{band}_k{k}",
                  "sql": f"SELECT id FROM docs WHERE d @@@ '{T}'::ftsquery "
                         f"ORDER BY d <=> '{T}'::ftsquery LIMIT {k}",
                  "expect_plan": IDX,
                  "count_sql": f"SELECT count(*) FROM docs WHERE d @@@ '{T}'::ftsquery"})
    # No expect_plan, on purpose.  pg_fts sets amoptionalkey=false
    # (pg_fts_am.c:6255), so a pure ORDER BY with no @@@ predicate cannot use the
    # index at all: the planner produces a seq scan plus a top-N sort over the
    # whole table.  That IS the expected result for this engine, and it is the
    # 24 s "ranked latency" that pg_fts first published as an index number
    # (RESULTS_4WAY_2026-07-29.md:95-99).  pg_weave closed exactly this gap in
    # task L7, so the row is measured here to size the gap instead of asserting
    # it -- and asserting an index scan would flag correct behaviour as a
    # PLAN FAIL.
    q.append({"label": f"bare_orderby_{band}",
              "sql": f"SELECT id FROM docs ORDER BY d <=> '{T}'::ftsquery LIMIT 10"})
q += [
 {"label": "count_common", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{C}'::ftsquery",
  "expect_plan": IDX, "count_sql": f"SELECT count(*) FROM docs WHERE d @@@ '{C}'::ftsquery"},
 {"label": "count_and", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{R} & {M}'::ftsquery",
  "expect_plan": IDX, "count_sql": f"SELECT count(*) FROM docs WHERE d @@@ '{R} & {M}'::ftsquery"},
 {"label": "count_or2", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{R} | {M}'::ftsquery",
  "expect_plan": IDX},
 {"label": "count_not", "sql": f"SELECT count(*) FROM docs WHERE d @@@ '{C} & !{R}'::ftsquery",
  "expect_plan": IDX},
 # Prefix syntax is `term*` in ftsquery (pg_fts/sql/pg_fts.sql:208-218), not the
 # tsquery `term:*` -- same spelling as wquery, since wquery is its descendant.
 {"label": "count_prefix", "sql": "SELECT count(*) FROM docs WHERE d @@@ 'word0001*'::ftsquery",
  "expect_plan": IDX},
 {"label": "count_rare_marker", "sql": "SELECT count(*) FROM docs WHERE d @@@ 'zzqrare'::ftsquery",
  "expect_plan": IDX, "count_sql": "SELECT count(*) FROM docs WHERE d @@@ 'zzqrare'::ftsquery"},
]
print(json.dumps({
  "engine": "fts",
  # Extension version AND commit sha: pg_fts ships many point releases whose
  # performance differs, and "pg_fts 1.5.x" is not enough to reproduce a number.
  "version_sql": "SELECT 'pg_fts ' || extversion || ' @%s' FROM pg_extension WHERE extname='pg_fts'" % SHA,
  "fingerprint_sql": "SELECT md5(string_agg(content, E'\\n' ORDER BY id)) FROM docs",
  "index_size_sql": "SELECT pg_relation_size('docs_fts')",
  "queries": q}, indent=1))
PY
    python3 -c "
import json
s=json.load(open('/tmp/spec.json'))
s['build_seconds']=float(open('$OUTDIR/build.txt').read().split()[0])
json.dump(s,open('/tmp/spec.json','w'))"
    run_measure /tmp/spec.json "$samples" "$warmup" "$run"
}

case "${1:?usage: fts.sh <provision|load|index|gate|measure>}" in
    provision) do_provision ;;
    load)      do_load "${2:?corpus}" ;;
    index)     do_index ;;
    gate)      do_gate ;;
    measure)   do_measure "${2:-200}" "${3:-10}" "${4:-adhoc}" ;;
    *) echo "unknown verb $1" >&2; exit 1 ;;
esac
