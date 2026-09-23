#!/usr/bin/env bash
#
# gatesweep.sh -- does the fused scan get CHEAPER as the predicate gets more
# selective?  That is claim 3 of doc/ARCHITECTURE.md sect. 9, and until this script
# existed nothing in the tree measured it.
#
# WHY IT IS A SEPARATE SCRIPT FROM fuse.sh, AND WHY IT HAS NO CONTROL ARM.
# bench/fuse.sh compares the fused scan against an RRF over-fetch control on the
# UNFILTERED shape: no query in it has a WHERE clause (checked 2026-09-23).  Claim 3
# is a different proposition and a weaker one to state: it is a MONOTONICITY claim
# about ONE arm -- the same query, the same index, the same k, with the predicate
# tightened -- so it needs no denominator and no competitor.  That matters, because
# every ratio-against-RRF row has to argue about what the control should be allowed
# to do with the predicate, and this one does not.
#
# WORK BY DEFAULT; LATENCY ONLY WHEN ASKED, AND ONLY ON EC2.  This runs on the
# workstation, where every latency figure is worthless (AGENTS.md).  The counters it
# reads are deterministic and host-independent, which is the same reason nDCG is
# measured locally and p99 is not.  If the work curve is flat, no EC2 run can make
# the latency curve fall -- which is why the work pass came first and shipped alone.
#
# `LAT=1` adds the latency pass, and it exists because the work pass ALONE cannot
# support claim 3.  Claim 3 is a claim about queries, not about counters, and after
# G27 the two halves of the work curve disagree in an interesting way: CPU work
# tracks selectivity to three digits, while page traffic only started falling at all
# once the code-page pointer was stored (2.04x on scifact, 4.79x on fiqa at 0.1 %).
# A latency curve is the arbiter of which of those a user feels.  It needs a quiet
# machine, so `LAT=1` belongs to bench/aws/run.sh's `gatesweep` job and nowhere else.
#
# THE A/A LEG IS PER POINT, NOT PER ARM (hard rule 10).  There is no competitor here,
# so the noise floor has to come from measuring the SAME selectivity point twice in
# non-adjacent slots: every point is measured once in the first rotation and again in
# the second, after every other point has run.  A between-point delta smaller than a
# point's own slot1-slot2 spread is not a result.
#
# THREE THINGS THIS CANNOT SEE, stated here so a reader does not infer them:
#
#  1. **The predicate is a lexical term, not a scalar facet.**  `WEAVE_CH_DOCVALS`
#     is a declared channel kind whose page kind is still reserved
#     (include/weave/pagekind.h), so `WHERE category = 'x'` cannot be pushed into
#     the index at all today.  The only pushable predicate is `body @@@ term`, which
#     is what sql/fuse_pushdown.sql sect. 4 uses.  So "selectivity" here means a
#     term's document frequency, and the gate is correlated with the lexical channel
#     in a way an independent facet would not be.
#  2. **A missing index condition is silent.**  With bitmapscan on, core may answer
#     the qual with a bitmap path, the fused path's indexclause borrow finds nothing,
#     and the predicate becomes an EXECUTOR FILTER -- the same rows by a route that
#     does no gating inside the scan.  Both GUCs are off below and the plan is
#     asserted to carry an `Index Cond:`, because the failure mode of not checking is
#     a beautiful flat curve that measures the executor.
#  3. **Selectivity is measured, not assumed.**  The target is what we asked for; the
#     `sel` column is `count(*) / N` for the term actually chosen.  Quote the second.
#
# usage: [DBS="normsci normnf normfiqa"] [K=10] [NQ=0] bench/gatesweep.sh
#        NQ=0 means every query in fq.
#        LAT=1 adds the latency pass (LATN queries x REPS reps x 2 slots per point).
#
# Copyright (c) 2025-2026, Gregory Burd

set -uo pipefail

DBS=${DBS:-"normsci normnf normfiqa"}
K=${K:-10}
NQ=${NQ:-0}
LAT=${LAT:-0}
LATN=${LATN:-50}
REPS=${REPS:-7}
OUT=${OUT:-/scratch/pg_weave/gatesweep}
TAG=${TAG:-$(date +%Y%m%d-%H%M%S)}

mkdir -p "$OUT"
REPORT=$OUT/gatesweep-$TAG.tsv
LATREPORT=$OUT/gatesweep-lat-$TAG.tsv

say() { printf '%s\n' "$*" >&2; }
die() { printf 'gatesweep.sh: %s\n' "$*" >&2; exit 1; }

# Candidate gate terms.  A deliberately mixed list: function words (very common in
# any English corpus), general nouns, and vocabulary specific to the two subject
# areas these corpora cover (biomedical for scifact/nfcorpus, finance for fiqa), so
# that every corpus has candidates across four orders of magnitude of df.  A term
# with df = 0 in a given corpus is skipped, which is why the list is long.
TERMS=(
    the of and in to a is for with on that as by are from this be or at it
    have not which was were has been we their also more one two three
    study patients cells risk data results may between after high low
    cancer treatment associated increased protein gene expression disease
    vitamin diet obesity mortality tumor mice receptor insulin
    market tax money company stock loan bank credit card interest price
    business account invest mortgage dividend etf broker portfolio
    mitochondrial apoptosis phosphorylation carcinoma epidemiological
    amortization arbitrage escrow annuity
)

printf 'db\tndocs\tnq\ttarget\tterm\tgate_rows\tsel\tpivots\tlex_contribs\tvec_scores\tgate_scores\tvec_lanes\tvec_blocks\tblkskip\trqskip\tveto\tabandon\n' \
    > "$REPORT"
if [ "$LAT" = 1 ]; then
    printf 'db\tndocs\ttarget\tterm\tsel\tslot\tp50_ms\tp99_ms\tqueries\treps\n' > "$LATREPORT"
fi

# p50 and p99 from a file of one measurement per line.  Identical to bench/fuse.sh's,
# on purpose: two scripts measuring the same index with two percentile conventions
# would produce a difference that is neither script's subject.
pctl() {
    sort -g "$1" | awk '{v[NR]=$1}
        END { if (NR==0) { printf "0\t0"; exit }
              p=int(NR*0.99); if (p<1) p=NR;
              printf "%.3f\t%.3f", v[int((NR+1)/2)], v[p] }'
}

for DB in $DBS; do
    PSQL="psql -X -q -v ON_ERROR_STOP=1 -d $DB"
    $PSQL -c 'SELECT 1' >/dev/null 2>&1 || die "cannot reach database $DB"

    N=$($PSQL -t -A -c 'SELECT count(*) FROM fd')
    [ "$N" -gt 0 ] || die "$DB: fd is empty"

    # The query literals, exactly as bench/fuse.sh builds them (fuse.sh:273), so the
    # two scripts are measuring the same queries against the same index.
    QLIT=$(mktemp)
    $PSQL -t -A -F $'\t' \
        -c "SELECT qid, quote_literal(wq), quote_literal(qv::text) FROM fq ORDER BY qid;" \
        > "$QLIT"
    [ -s "$QLIT" ] || die "$DB: no queries in fq"
    if [ "$NQ" -gt 0 ]; then
        head -n "$NQ" "$QLIT" > "$QLIT.cut" && mv "$QLIT.cut" "$QLIT"
    fi
    NQUSED=$(wc -l < "$QLIT")

    say "$DB: $N docs, $NQUSED queries, k=$K -- measuring term document frequencies"

    # The resolved points, kept so the latency pass can replay exactly the shapes the
    # counter pass measured.  Re-deriving them there would let the two passes drift
    # onto different terms, and then the work curve could not explain the latency one.
    PTS=$(mktemp)

    # One count(*) per candidate, on the index.  The dictionary's df would be cheaper
    # but it is per segment and includes tombstoned documents; the gate's real
    # selectivity is the number of LIVE rows the qual admits, which is this.
    DFFILE=$(mktemp)
    for t in "${TERMS[@]}"; do
        c=$($PSQL -t -A -c "SELECT count(*) FROM fd WHERE body @@@ '$t'::wquery" 2>/dev/null)
        case $c in ''|*[!0-9]*) continue ;; esac
        [ "$c" -gt 0 ] && printf '%s\t%s\n' "$t" "$c" >> "$DFFILE"
    done
    [ -s "$DFFILE" ] || die "$DB: no candidate term matched anything"

    for TARGET in 1.0 0.1 0.01 0.001; do
        if [ "$TARGET" = "1.0" ]; then
            TERM=""
            GROWS=$N
            WHERE=""
        else
            # Nearest in LOG space: a term at 2x the target is as wrong as one at
            # half, and a linear nearest-match would always pick the biggest term
            # for the 0.001 point on a small corpus.
            read -r TERM GROWS < <(awk -v n="$N" -v tgt="$TARGET" -F'\t' '
                { s = $2 / n; if (s <= 0) next;
                  d = log(s / tgt); if (d < 0) d = -d;
                  if (best == "" || d < best) { best = d; bt = $1; bc = $2 } }
                END { if (bt != "") print bt "\t" bc }' "$DFFILE")
            [ -n "${TERM:-}" ] || { say "$DB: no term near $TARGET, skipping"; continue; }
            WHERE="WHERE body @@@ '$TERM'::wquery"
        fi
        SEL=$(awk -v a="$GROWS" -v b="$N" 'BEGIN{printf "%.5f", a/b}')

        # THE PLAN CHECK (threat 2 above).  A gated point whose plan has no
        # `Index Cond:` is not measuring the gate, so it is fatal rather than noted.
        if [ -n "$WHERE" ]; then
            IFS=$'\t' read -r qid1 wq1 qv1 < <(head -1 "$QLIT")
            PLAN=$($PSQL -t -A -c "SET enable_seqscan=off; SET enable_bitmapscan=off;
                EXPLAIN (COSTS OFF) SELECT id FROM fd $WHERE
                 ORDER BY fuse(body <=> $wq1::wquery, emb <#> $qv1::wvec,
                               weights => '{0.5,0.5}') LIMIT $K;")
            printf '%s' "$PLAN" | grep -q 'Index Cond' \
                || die "$DB target=$TARGET: the qual is not an index condition; the plan is: $PLAN"
            printf '%s' "$PLAN" | grep -q 'Order By' \
                || die "$DB target=$TARGET: the fused order-by did not reach the index"
        fi

        SQLF=$(mktemp)
        {
            echo "SET enable_seqscan=off; SET enable_bitmapscan=off;"
            echo "SELECT weave_fuse_stats_reset(); SELECT weave_work_stats_reset();"
            while IFS=$'\t' read -r qid wq qv; do
                printf "SELECT id FROM fd %s ORDER BY fuse(body <=> %s::wquery, emb <#> %s::wvec, weights => '{0.5,0.5}') LIMIT %s;\n" \
                    "$WHERE" "$wq" "$qv" "$K"
            done < "$QLIT"
            # THE LEXICAL UNIT IS DERIVED, NOT READ.  weave_work_stats().lex_contribs
            # deliberately excludes the fused path (src/am/am.c:893), so on this shape
            # it is 0 and reading it would report "the lexical channel did no work" for
            # every point of the sweep -- a flat line that looks like a finding.  The
            # decomposition is fuse.sh:517's: total score() calls minus the vector
            # adapters' share minus the gates' share.
            echo "SELECT f.pivots, f.scores - f.vec_scores - f.gate_scores, f.vec_scores, f.gate_scores, w.vec_lanes, w.vec_blocks, f.blkskip, f.rqskip, f.veto, f.abandon FROM weave_fuse_stats() f, weave_work_stats() w;"
        } > "$SQLF"
        ROW=$(psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -t -A -F $'\t' -f "$SQLF" | tail -1)
        rm -f "$SQLF"
        [ -n "$ROW" ] || die "$DB target=$TARGET: no counters came back"

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$DB" "$N" "$NQUSED" "$TARGET" "${TERM:--}" "$GROWS" "$SEL" "$ROW" \
            | tr -d '\r' >> "$REPORT"
        printf '%s\t%s\t%s\t%s\n' "$TARGET" "${TERM:--}" "$SEL" "$WHERE" >> "$PTS"
        say "$DB target=$TARGET term=${TERM:--} sel=$SEL -> $ROW"
    done

    # ------------------------------------------------------------------ latency
    #
    # Two rotations over the points, one query at a time.  Interleaving by QUERY
    # rather than running each point to completion is what keeps a drift in the
    # machine -- another tenant, a thermal step, a background autovacuum -- from
    # landing entirely on one point and being read as claim 3 succeeding or failing.
    #
    # `EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON)` and dropping the first rep, exactly
    # as bench/fuse.sh does: TIMING OFF removes per-node instrumentation overhead and
    # leaves the one number wanted, and the first rep of a fresh statement pays for
    # the plan and the cold buffers.
    if [ "$LAT" = 1 ]; then
        say "$DB: latency, $(wc -l < "$PTS") points x $LATN queries x $REPS reps x 2 slots"
        declare -A LATF
        while IFS=$'\t' read -r tgt term sel whr; do
            LATF["$tgt:1"]=$(mktemp); LATF["$tgt:2"]=$(mktemp)
        done < "$PTS"

        while IFS=$'\t' read -r qid wq qv; do
            for slot in 1 2; do
                while IFS=$'\t' read -r tgt term sel whr; do
                    {
                        echo "SET enable_seqscan=off; SET enable_bitmapscan=off;"
                        for _ in $(seq 1 "$REPS"); do
                            printf "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) SELECT id FROM fd %s ORDER BY fuse(body <=> %s::wquery, emb <#> %s::wvec, weights => '{0.5,0.5}') LIMIT %s;\n" \
                                "$whr" "$wq" "$qv" "$K"
                        done
                    } | psql -X -q -d "$DB" -t -A \
                      | sed -n 's/^Execution Time: \([0-9.]*\) ms$/\1/p' \
                      | tail -n +2 \
                      >> "${LATF["$tgt:$slot"]}"
                done < "$PTS"
            done
        done < <(head -n "$LATN" "$QLIT")

        while IFS=$'\t' read -r tgt term sel whr; do
            for slot in 1 2; do
                f=${LATF["$tgt:$slot"]}
                n=$(wc -l < "$f")
                [ "$n" -gt 0 ] || die "$DB target=$tgt slot=$slot: no timings came back"
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$DB" "$N" "$tgt" "$term" "$sel" "$slot" "$(pctl "$f")" \
                    "$LATN" "$REPS" >> "$LATREPORT"
                rm -f "$f"
            done
        done < "$PTS"
        unset LATF
    fi
    rm -f "$PTS"
    rm -f "$QLIT" "$DFFILE"
done

say ""
say "report: $REPORT"
say ""
# The one thing a reader wants: is the curve monotone?  Printed as ratios against
# the unfiltered point of the same database, because claim 3 is about the SHAPE.
awk -F'\t' 'NR==1{next}
    { key=$1; if ($4=="1.0") { base_p[key]=$8; base_l[key]=$9; base_v[key]=$10;
                               base_b[key]=$13 }
      rows[NR]=$0 }
    END {
      printf "%-9s %-7s %-8s %9s %9s %9s %9s\n", "db", "target", "sel",
             "pivots", "lex", "vec_sc", "vec_blk";
      for (i = 2; i <= NR; i++) {
        split(rows[i], f, "\t"); k = f[1];
        printf "%-9s %-7s %-8s %8.3fx %8.3fx %8.3fx %8.3fx\n", f[1], f[4], f[7],
               base_p[k] ? f[8]/base_p[k] : 0, base_l[k] ? f[9]/base_l[k] : 0,
               base_v[k] ? f[10]/base_v[k] : 0, base_b[k] ? f[13]/base_b[k] : 0;
      }
    }' "$REPORT" >&2

if [ "$LAT" = 1 ]; then
    say ""
    say "latency report: $LATREPORT"
    say ""
    # THE A/A COLUMN IS THE ONE TO READ FIRST.  `aa` is |slot1 - slot2| for the same
    # point; `vs_1.0` is the ratio of slot 1 against the unfiltered point of the same
    # database.  Claim 3 is supported at a point only if the ratio moved by more than
    # that point's own aa spread -- otherwise the machine moved, not the query.
    awk -F'\t' 'NR==1{next}
        { key=$1 "\t" $3; if ($6=="1") p50[key]=$7; else q50[key]=$7
          if ($3=="1.0") { if ($6=="1") base[$1]=$7 }
          if (!seen[key]++) order[++n]=key; sel[key]=$5; term[key]=$4 }
        END {
          printf "%-9s %-7s %-8s %10s %10s %9s %9s\n", "db", "target", "sel",
                 "p50_slot1", "p50_slot2", "aa_ms", "vs_1.0";
          for (i = 1; i <= n; i++) {
            k = order[i]; split(k, f, "\t"); d = p50[k] - q50[k]; if (d < 0) d = -d;
            printf "%-9s %-7s %-8s %10.3f %10.3f %9.3f %8.3fx\n", f[1], f[2], sel[k],
                   p50[k], q50[k], d, base[f[1]] ? p50[k]/base[f[1]] : 0;
          }
        }' "$LATREPORT" >&2
fi
