#!/usr/bin/env bash
#
# bench/compete/engines/textsearch.sh -- Timescale pg_textsearch.
#
# axis: lexical
#
# The fairest like-for-like BM25 comparator in the set: same PostgreSQL
# `english` text configuration, same single `content` column, same Snowball
# stemmer, so match counts are directly comparable in a way they can never be
# with an engine that ships its own analyzer (pg_fts/bench/RESULTS_5WAY_159:
# 142-147).  pg_fts's own conclusion was that this is the engine to beat on
# ranked latency and index size, and the one it loses to on both.
#
# It is also the engine with the narrowest surface: ranking and nothing else.
# That shows up below as ABSENT query labels, which is deliberate -- see the
# comment on the count_* gap in do_measure.
#
set -euo pipefail
ENGINE=textsearch
source "$(dirname "$0")/common.sh"

do_provision() {
    setup_nvme; tune_os; install_pg_source

    # Built BEFORE start_pg because pg_textsearch REQUIRES preloading: without
    # it, CREATE EXTENSION itself fails with "pg_textsearch library not loaded.
    # Add pg_textsearch to shared_preload_libraries and restart"
    # (pg_fts/bench/data_5way_159/pgts_bench_result.txt:8-11).  And a server told
    # to preload a library that is not installed yet refuses to start, so the
    # order is: build, install, set PRELOAD, then start.
    say "building pg_textsearch"
    export PATH="$NVME/pg/bin:$PATH"
    if [ ! -d "$HOME/pg_textsearch" ]; then
        git clone --quiet https://github.com/timescale/pg_textsearch.git "$HOME/pg_textsearch"
    fi
    cd "$HOME/pg_textsearch"
    # The commit sha is part of the result.  pg_fts's 5-way pinned f940210
    # ("Dispatch compaction requests at commit (#477)") and that commit changes
    # WHEN the index compacts, which changes the size number -- a version string
    # without a sha cannot reproduce it.
    git rev-parse --short HEAD > "$OUTDIR/engine_commit.txt" 2>/dev/null || \
        echo unknown > "$OUTDIR/engine_commit.txt"
    make -s PG_CONFIG="$NVME/pg/bin/pg_config" >/dev/null
    make -s install PG_CONFIG="$NVME/pg/bin/pg_config" >/dev/null

    PRELOAD=pg_textsearch
    export PRELOAD
    start_pg; record_hostinfo
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" -c "CREATE EXTENSION IF NOT EXISTS pg_textsearch"
    psql -X -q -t -A -d "$PGDATABASE" \
        -c "SELECT 'pg_textsearch ' || extversion || ' @' || '$(cat "$OUTDIR/engine_commit.txt")'
              FROM pg_extension WHERE extname='pg_textsearch'"
}

do_load() {
    load_corpus "$1"
    export PATH="$NVME/pg/bin:$PATH"
    # No materialised column: the bm25 AM indexes `content` (text) directly and
    # analyzes at build time using text_config, so there is nothing to
    # pre-compute.  That means this arm has NO extra column and NO extra heap
    # churn -- which also means it does not pay the load-time analysis cost the
    # other arms pay.  Load time is therefore not comparable across arms; build
    # time and query latency are.  Recorded here so that asymmetry is not
    # rediscovered as an anomaly later.
    psql -X -q -v ON_ERROR_STOP=1 -d "$PGDATABASE" -c "VACUUM (ANALYZE) docs"
    say "content column is indexed directly; no derived column to materialise"
}

do_index() {
    # text_config='english' is what makes this engine's analyzer identical to
    # to_tsvector('english', ...) and therefore its match counts comparable.
    # It is a reloption and is reported, not implied.
    timed_index "CREATE INDEX docs_bm25 ON docs USING bm25 (content) WITH (text_config='english')"
    export PATH="$NVME/pg/bin:$PATH"
    echo "reloptions: text_config=english" >> "$OUTDIR/build.txt"
    # This engine dispatches compaction requests at COMMIT (upstream #477), so
    # the physical size right after CREATE INDEX can still be settling.  Record
    # the immediate size here for the record; the size the analyzer publishes
    # comes from index_size_sql, read at measure time, by which point compaction
    # has finished.  Two numbers, both labelled, rather than one ambiguous one.
    psql -X -q -v ON_ERROR_STOP=1 -t -A -d "$PGDATABASE" \
        -c "SELECT 'size_at_build_end=' || pg_size_pretty(pg_relation_size('docs_bm25'))" \
        >> "$OUTDIR/build.txt"
    prewarm docs docs_bm25
}

do_gate() {
    local M; M=$(band mid)
    # This engine CANNOT be set-equality gated.  It has no match predicate at
    # all: there is no `WHERE content <matches> term` operator, only the ordering
    # operator <@>, so there is no result set to EXCEPT against the seq-scan
    # reference.  Deriving one by exhausting the index (ORDER BY ... LIMIT
    # 3000000) is not an option: pg_fts measured 474 s for a single common term
    # that way (data_5way_159b/pgts_bench_result.txt:75).
    #
    # What can still be checked, and matters, is that the rows it ranks highest
    # actually contain the term.  This catches the failure mode that retracted
    # pg_turbovec's headline: fast and WRONG.  A ranking engine that pads its
    # top-k with non-matching documents (score 0 rows are observable in this
    # engine, see pg_fts/bench/RESULTS_ENCODING.md:36) would pass a latency
    # benchmark and fail this gate.
    cat > /tmp/gate.json <<JSON
{"engine":"textsearch","gates":[
 {"name":"topk_all_contain_term","type":"topk_parity",
  "violations_sql":"WITH k AS (SELECT id FROM docs ORDER BY content <@> '$M'::bm25query LIMIT 10) SELECT count(*) FROM k JOIN docs ON docs.id = k.id WHERE docs.content !~ ('\\\\m$M\\\\M')"}
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
# Query form and plan node taken from the recorded run, not guessed:
#   SELECT id, content <@> '<term>'::bm25query AS score FROM docs ORDER BY score LIMIT k
#   ->  Index Scan using docs_pgts on docs
#         Order By: (content <@> 'docs_pgts:slovakia'::bm25query)
# (pg_fts/bench/data_5way_159b/pgts_bench_result.txt:33-40).  The bare
# 'term'::bm25query literal is accepted and the index is resolved from the
# ordering operator; the printed 'index:term' form is how the planner renders it
# after resolution.  Getting a competitor's operator signature wrong is not
# hypothetical: a to_bm25query(regclass,text) guess for VectorChord produced a
# 0.083 ms "win" that was an erroring query (pg_fts/bench/data_5way/correctness.txt).
IDX = r"Index Scan using docs_bm25"
q = []
for band, T in (("rare", R), ("mid", M), ("common", C)):
    for k in (10, 100):
        # No count_sql.  This engine exposes no df or count function, and the
        # only way to derive a match count is index exhaustion, which cost 474 s
        # for the common term in the prior run -- that is a corpus property, not
        # a latency measurement, and it must not sit in a latency table.  The
        # match count for this corpus is reported by the gin and weave arms,
        # which index byte-identical content (asserted by fingerprint_sql), and
        # the analyzer's rows= column is simply empty here.
        q.append({"label": f"ranked_{band}_k{k}",
                  "sql": f"SELECT id, content <@> '{T}'::bm25query AS score "
                         f"FROM docs ORDER BY score LIMIT {k}",
                  "expect_plan": IDX})
# Deliberately ABSENT: bare_orderby_*, count_common, count_and, count_or2,
# count_not, count_prefix, count_rare_marker.
#
# bare_orderby_* is absent because for this engine the ranked_* form above IS
# the bare ORDER BY with no WHERE clause -- duplicating it under a second label
# would report the same measurement twice and make it look like this engine wins
# a shape the others lose, when in truth it is the only shape it has.  Read
# ranked_*_k10 as its bare-ORDER-BY row.
#
# The count_* labels are absent because they cannot be expressed at all: no
# match predicate, therefore no count(*) pushdown, no boolean AND/OR/NOT, and no
# prefix query.  An empty row in the comparison table is the intended output. A
# capability gap has to be visible next to the latency numbers, because the
# alternative -- an engine that looks competitive on the only four rows it can
# fill -- is how "fastest BM25 for PostgreSQL" gets claimed for something that
# cannot answer count(*).  engines/README.md tabulates this explicitly so the
# absence is never read as a harness bug.
print(json.dumps({
  "engine": "textsearch",
  "version_sql": "SELECT 'pg_textsearch ' || extversion || ' @%s' FROM pg_extension WHERE extname='pg_textsearch'" % SHA,
  # Same fingerprint expression as every other arm, byte for byte.  The analyzer
  # fails the whole run if the values differ, which is the mechanical form of the
  # check whose absence forced the 5-way retraction (title||body vs body alone).
  "fingerprint_sql": "SELECT md5(string_agg(h, '' ORDER BY id)) FROM (SELECT id, md5(content) AS h FROM docs) t",
  "index_size_sql": "SELECT pg_relation_size('docs_bm25')",
  "queries": q}, indent=1))
PY
    python3 -c "
import json
s=json.load(open('/tmp/spec.json'))
s['build_seconds']=float(open('$OUTDIR/build.txt').read().split()[0])
json.dump(s,open('/tmp/spec.json','w'))"
    run_measure /tmp/spec.json "$samples" "$warmup" "$run"
}

case "${1:?usage: textsearch.sh <provision|load|index|gate|measure>}" in
    provision) do_provision ;;
    load)      do_load "${2:?corpus}" ;;
    index)     do_index ;;
    gate)      do_gate ;;
    measure)   do_measure "${2:-200}" "${3:-10}" "${4:-adhoc}" ;;
    *) echo "unknown verb $1" >&2; exit 1 ;;
esac
