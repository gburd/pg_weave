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
CTL=${CTL:-1}
CTLN=${CTLN:-10}
OUT=${OUT:-/scratch/pg_weave/gatesweep}
TAG=${TAG:-$(date +%Y%m%d-%H%M%S)}

mkdir -p "$OUT"
REPORT=$OUT/gatesweep-$TAG.tsv
LATREPORT=$OUT/gatesweep-lat-$TAG.tsv
CTLREPORT=$OUT/gatesweep-ctl-$TAG.tsv

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

printf 'db\tndocs\tnq\ttarget\tterm\tgate_rows\tsel\tpivots\tlex_contribs\tvec_scores\tgate_scores\tvec_lanes\tvec_blocks\tblkskip\trqskip\tveto\tabandon\tlex_pages_skip\tlex_pages_load\tlex_reads_skip\tlex_reads_load\n' \
    > "$REPORT"
if [ "$LAT" = 1 ]; then
    printf 'db\tndocs\ttarget\tterm\tsel\tslot\tp50_ms\tp99_ms\tqueries\treps\n' > "$LATREPORT"
fi
if [ "$CTL" = 1 ]; then
    printf 'db\tndocs\ttarget\tterm\tsel\tarm\tbuffers\trecheck_removed\tqueries\n' > "$CTLREPORT"
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
            #
            # THE LEXICAL CHANNEL'S PAGE TRAFFIC, SPLIT (doc/GAPS.md G48), and unlike
            # lex_contribs these two ARE path-independent -- they sit in the cursor
            # primitives every lexical path goes through.  lex_pages_skip counts pages
            # visited only to prove blocks irrelevant, lex_pages_load pages visited to
            # decode one; the ratio is the ceiling on what an out-of-chain skip structure
            # could remove, and it is the only lever left after the vector side was
            # capped at ~15 %.  A page VISIT is a pin plus a share lock, not necessarily
            # an I/O -- see the note in include/weave/weave.h before quoting these.
            #
            # A ZERO IN lex_pages_skip IS A RESULT, NOT A HARNESS BUG: no query shape
            # reachable from the regression fixture (4,000 dense rows, seven shapes tried)
            # calls wand_skip_blocks() at all, because a seek there never leaves a whole
            # 128-block behind.  These corpora are the first place the path can be
            # observed; if it is zero here too, the lever is zero and G48 closes.
            # lex_reads_skip/load (G48's I/O half) are the shared_blks_read delta at the
            # skip vs decode ReadBuffer sites.  On THIS host at default shared_buffers the
            # index is resident, so they are ~0 (cold-start only) and the visit counters
            # above are the story; the I/O split only appears under a small pool, which is
            # how bench/RESULTS_GATE_SWEEP.md's G48 I/O section was measured (54-85% of
            # fiqa's lexical reads skip-only under pressure).  Recorded here so a pressured
            # run captures them without a code change.
            echo "SELECT f.pivots, f.scores - f.vec_scores - f.gate_scores, f.vec_scores, f.gate_scores, w.vec_lanes, w.vec_blocks, f.blkskip, f.rqskip, f.veto, f.abandon, w.lex_pages_skip, w.lex_pages_load, w.lex_reads_skip, w.lex_reads_load FROM weave_fuse_stats() f, weave_work_stats() w;"
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

    # ------------------------------------------------- the mechanism control
    #
    # WHAT THE LATENCY CURVE DOES NOT SHOW.  The work and latency passes measure that the
    # fused scan gets cheaper as the predicate tightens -- a MONOTONICITY claim about one
    # arm.  Claim 3 also asserts a MECHANISM: "the predicate is pushed into the SIMD block
    # mask instead of collapsing recall".  Nothing measured that, and it is the half a
    # reader is entitled to be sceptical about, because the obvious alternative -- fetch in
    # vector order and recheck the predicate afterwards -- is what every filtered-ANN
    # implementation without a shared docid space has to do.
    #
    # THE CONTROL IS OUR OWN INDEX, WHICH IS WHY IT IS WORTH ANYTHING.  Both arms below
    # carry the SAME `Index Cond:` on the SAME index over the SAME corpus, and both are
    # asserted to have one.  The only difference is where the predicate acts: inside the
    # scan for `fuse()`, or as a per-candidate recheck for a plain vector ORDER BY.  So a
    # difference cannot be attributed to an engine, a corpus, a build or a plan shape --
    # the three things that make a cross-engine filtered-ANN comparison unfalsifiable.
    #
    # `Rows Removed by Index Recheck` IS THE MEASUREMENT, not the buffer count.  It is the
    # over-fetch, counted by the executor rather than by us, and it is what "collapsing
    # recall" looks like when you keep recall instead: the vector-order arm must walk and
    # discard candidates to fill k, and the fused arm must not discard any.
    #
    # Buffers are deterministic and host-independent, so this pass runs anywhere -- which
    # is why it defaults ON while the latency pass defaults off.  CTLN is small because the
    # control arm reads ~100x more pages than the fused arm at a tight gate.
    if [ "$CTL" = 1 ]; then
        say "$DB: mechanism control, $CTLN queries x $(wc -l < "$PTS") points, fused vs vector-order"
        while IFS=$'\t' read -r tgt term sel whr; do
            for arm in fused vecorder; do
                tb=0; tr=0; nq=0
                while IFS=$'\t' read -r qid wq qv; do
                    case $arm in
                      fused) ob="fuse(body <=> $wq::wquery, emb <#> $qv::wvec, weights => '{0.5,0.5}')" ;;
                      vecorder) ob="emb <#> $qv::wvec" ;;
                    esac
                    plan=$(psql -X -q -d "$DB" -tA <<SQL
SET enable_seqscan=off; SET enable_bitmapscan=off;
EXPLAIN (ANALYZE, TIMING OFF, BUFFERS, COSTS OFF)
SELECT id FROM fd $whr ORDER BY $ob LIMIT $K;
SQL
)
                    # BOTH arms must carry an Index Cond, or the comparison is between a
                    # gated scan and an executor filter and says nothing about the mask.
                    # Only checked where there IS a predicate.
                    if [ -n "$whr" ] && ! printf '%s' "$plan" | grep -q 'Index Cond'; then
                        die "$DB $arm target=$tgt: no Index Cond; the control would be comparing the executor"
                    fi
                    b=$(printf '%s' "$plan" | sed -n 's/.*Buffers: shared hit=\([0-9]*\).*/\1/p' | head -1)
                    r=$(printf '%s' "$plan" | sed -n 's/.*Rows Removed by Index Recheck: \([0-9]*\).*/\1/p' | head -1)
                    tb=$((tb + ${b:-0})); tr=$((tr + ${r:-0})); nq=$((nq + 1))
                done < <(head -n "$CTLN" "$QLIT")
                printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                    "$DB" "$N" "$tgt" "$term" "$sel" "$arm" "$tb" "$tr" "$nq" >>"$CTLREPORT"
                say "  $arm target=$tgt: buffers=$tb recheck_removed=$tr over $nq queries"
            done
        done < "$PTS"
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

if [ "$CTL" = 1 ]; then
    say ""
    say "mechanism control: $CTLREPORT"
    say ""
    awk -F'\t' 'NR==1{next}
        { k=$1 "\t" $3; if ($6=="fused") { fb[k]=$7; fr[k]=$8 } else { vb[k]=$7; vr[k]=$8 }
          if (!seen[k]++) o[++n]=k; sel[k]=$5 }
        END {
          printf "%-9s %-7s %-8s %10s %10s %7s %9s %9s\n", "db", "target", "sel",
                 "fused_buf", "vecord_buf", "ratio", "fused_rm", "vecord_rm";
          for (i=1;i<=n;i++) { k=o[i]; split(k,f,"\t");
            printf "%-9s %-7s %-8s %10d %10d %6.1fx %9d %9d\n", f[1], f[2], sel[k],
                   fb[k], vb[k], fb[k] ? vb[k]/fb[k] : 0, fr[k], vr[k] } }' \
        "$CTLREPORT" >&2
fi

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
