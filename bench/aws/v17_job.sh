#!/usr/bin/env bash
#
# bench/aws/v17_job.sh -- the EC2 driver for bench/v17_crossover.sh (doc/PHASES.md
# V17, measurement only).  Run as the `script` job, which has already built,
# installed and smoke-tested the extension on this host:
#
#   SCRIPT=bench/aws/v17_job.sh bench/aws/run.sh c7i.4xlarge script
#
# Scale 1 is two real BEIR corpora (scifact 5.2k, fiqa 57.6k; MiniLM 384-d) loaded
# through bench/fuse.sh's own loader, so the corpus is the one bench/gatesweep.sh
# measured.  Scale 2 is a synthetic 1M-row table (random 384-d unit vectors, short
# Zipf-ish wdocs) built inside v17_crossover.sh.  Everything lands in /tmp/out, which
# the harness pulls every SCRIPT_PULL seconds.  SCALES/DATASETS/SYNTH_N can be edited
# here; the harness does not forward the caller's environment.

set -uo pipefail
SCALES=${SCALES:-"beir synth"}
DATASETS=${DATASETS:-"scifact fiqa"}
SYNTH_N=${SYNTH_N:-1000000}
LATN=${LATN:-25}
REPS=${REPS:-6}
BENCH=/scratch/bench
mkdir -p /tmp/out "$BENCH"
say() { printf '%s v17job: %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { say "FAIL: $*"; exit 1; }

say "host: $(nproc) vCPU, $(awk '/MemTotal/{print int($2/1048576)}' /proc/meminfo) GiB, $(psql -X -At -c 'select version()' -d postgres)"
psql -X -At -d postgres -c "select name || '=' || setting from pg_settings where name in ('shared_buffers','work_mem','jit','max_parallel_workers_per_gather')" | tee /tmp/out/gucs.txt

for S in $SCALES; do
    case $S in
    beir)
        # The embedder, exactly as bench/aws/run.sh's ensure_embedder installs it
        # (CPU torch first, so sentence-transformers does not pull CUDA wheels).
        if [ ! -x /scratch/venv/bin/python3 ] || ! /scratch/venv/bin/python3 -c 'import sentence_transformers' 2>/dev/null; then
            say "installing the embedder (several minutes, no output)"
            sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y python3-venv >/dev/null 2>&1
            python3 -m venv /scratch/venv || die "venv"
            /scratch/venv/bin/pip install --quiet --upgrade pip
            /scratch/venv/bin/pip install --quiet torch --index-url https://download.pytorch.org/whl/cpu || die "torch"
            /scratch/venv/bin/pip install --quiet sentence-transformers || die "sentence-transformers"
            /scratch/venv/bin/python3 -c 'import sentence_transformers' || die "embedder does not import"
        fi
        for D in $DATASETS; do
            say "$D: prepdata (minilm)"
            /scratch/venv/bin/python3 bench/prepdata.py --dataset "$D" --out "$BENCH" --embed minilm \
                > "/tmp/out/prep-$D.log" 2>&1 || die "prepdata $D (see prep-$D.log)"
            say "$D: load (fuse.sh FUSE_LOAD_ONLY)"
            PGDATABASE=v17_$D FUSE_LOAD_ONLY=1 bash bench/fuse.sh "$BENCH" "$D" > "/tmp/out/load-$D.log" 2>&1 \
                || die "fuse.sh load $D (see load-$D.log)"
            grep -q 'LOAD ONLY' "/tmp/out/load-$D.log" || die "$D: fuse.sh did not take the load-only path"
            say "$D: crossover"
            PREP=beir DB=v17_$D LATN=$LATN REPS=$REPS OUT=/tmp/out TAG=$D \
                bash bench/v17_crossover.sh > "/tmp/out/run-$D.log" 2>&1
            rc=$?
            tail -14 "/tmp/out/run-$D.log"
            [ "$rc" = 0 ] || die "v17_crossover $D exited $rc (see run-$D.log)"
            grep -q V17_CROSSOVER_DONE "/tmp/out/run-$D.log" || die "$D: no completion marker"
        done
        ;;
    synth)
        T=synth$((SYNTH_N / 1000))k
        say "$T: crossover (builds its own table)"
        PREP=synth DB=v17_$T N=$SYNTH_N DIM=384 MATNONE=0 LATN=$LATN REPS=$REPS OUT=/tmp/out TAG=$T \
            bash bench/v17_crossover.sh > "/tmp/out/run-$T.log" 2>&1
        rc=$?
        tail -14 "/tmp/out/run-$T.log"
        [ "$rc" = 0 ] || die "v17_crossover $T exited $rc (see run-$T.log)"
        grep -q V17_CROSSOVER_DONE "/tmp/out/run-$T.log" || die "$T: no completion marker"
        ;;
    *) die "unknown scale $S" ;;
    esac
done
say "ALL DONE"
