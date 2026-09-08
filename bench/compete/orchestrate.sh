#!/usr/bin/env bash
#
# bench/compete/orchestrate.sh -- launch N engine hosts in parallel, measure,
# collect raw samples, terminate, analyze centrally.
#
# Usage:
#   bench/compete/orchestrate.sh <corpus> <engine> [engine ...]
#   bench/compete/orchestrate.sh wiki-2m weave fts gin textsearch
#
# Why parallel, one host per engine: pg_search, pg_textsearch, and
# VectorChord-bm25 all define an access method named `bm25` and cannot be
# co-installed (pg_fts/bench/RESULTS_4WAY_2026-07-29.md:22-25); pg_tre and pg_trgm
# both define `%`.  Sharing a host would also mean two engines' indexes competing
# for the same shared_buffers, which silently changes what is measured.
#
# Every engine host runs the SAME corpus artifact, verified by sha256 before any
# index is built.  pg_fts had to retract an entire 5-way comparison because two
# engines indexed title+body and two indexed body alone -- a 48% difference in
# postings scanned, noticed only when match counts diverged (commit 81532f4, the
# author's own "my error").  wbench's analyzer additionally asserts that every
# host reports the same corpus fingerprint and refuses to publish if not.
#
# TERMINATION IS ENFORCED, on every exit path, and VERIFIED rather than assumed.
#
set -uo pipefail

PROFILE=${AWS_PROFILE:-bene}
CORPUS=${1:?usage: orchestrate.sh <corpus> <engine> [engine ...]}
shift
ENGINES=("$@")
[ ${#ENGINES[@]} -gt 0 ] || { echo "no engines given" >&2; exit 1; }

RUN=compete-$(date -u +%Y%m%d-%H%M%S)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/bench/compete/results/$RUN
REGION=$(aws configure get region --profile "$PROFILE")

# Instance types per axis, matching the predecessor runs so numbers stay
# comparable: r6id.4xlarge is what every pg_fts 5-way used; i4i.8xlarge is what
# pg_turbovec's competitive vector runs used.
ITYPE_LEXICAL=${ITYPE_LEXICAL:-r6id.4xlarge}
ITYPE_VECTOR=${ITYPE_VECTOR:-i4i.8xlarge}
SAMPLES=${SAMPLES:-200}
WARMUP=${WARMUP:-10}

mkdir -p "$OUT"
say() { printf '\033[1m[%s] %s\033[0m\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { printf '\033[31mFATAL: %s\033[0m\n' "$*" >&2; exit 1; }

aws sts get-caller-identity --profile "$PROFILE" >"$OUT/identity.json" \
    || die "profile $PROFILE cannot authenticate"

# ---------------------------------------------------------------------------
# Shared infrastructure: one key pair and one security group for the whole run.
# ---------------------------------------------------------------------------
KEY=$RUN-key
KEYFILE=$OUT/key.pem
SGID=""
declare -A IID=()      # engine -> instance id
declare -A HOST=()     # engine -> public ip

cleanup() {
    local rc=$?
    say "cleanup (exit $rc) -- terminating everything this run created"

    local ids=()
    for e in "${!IID[@]}"; do [ -n "${IID[$e]}" ] && ids+=("${IID[$e]}"); done

    if [ ${#ids[@]} -gt 0 ]; then
        aws ec2 terminate-instances --profile "$PROFILE" \
            --instance-ids "${ids[@]}" --output text >/dev/null 2>&1
        for _ in $(seq 1 40); do
            local remaining
            remaining=$(aws ec2 describe-instances --profile "$PROFILE" \
                --instance-ids "${ids[@]}" \
                --query 'Reservations[].Instances[?State.Name!=`terminated`].InstanceId' \
                --output text 2>/dev/null)
            [ -z "$remaining" ] && { say "all instances terminated"; break; }
            say "  still shutting down: $remaining"
            sleep 15
        done
        # Verify, do not assume.  A silently failed terminate leaves running
        # instances and a clean-looking log.
        local alive
        alive=$(aws ec2 describe-instances --profile "$PROFILE" \
            --instance-ids "${ids[@]}" \
            --query 'Reservations[].Instances[?State.Name==`running`].InstanceId' \
            --output text 2>/dev/null)
        [ -n "$alive" ] && printf '\033[31mWARNING: STILL RUNNING: %s -- TERMINATE MANUALLY\033[0m\n' "$alive" >&2
    fi

    if [ -n "$SGID" ]; then
        for _ in $(seq 1 40); do
            aws ec2 delete-security-group --profile "$PROFILE" --group-id "$SGID" \
                >/dev/null 2>&1 && { say "deleted sg $SGID"; break; }
            sleep 15
        done
    fi
    aws ec2 delete-key-pair --profile "$PROFILE" --key-name "$KEY" >/dev/null 2>&1

    say "artifacts in $OUT"
    exit $rc
}
trap cleanup EXIT

say "run $RUN  corpus=$CORPUS  engines=${ENGINES[*]}"

aws ec2 create-key-pair --profile "$PROFILE" --key-name "$KEY" --key-type ed25519 \
    --query KeyMaterial --output text >"$KEYFILE" || die "create-key-pair"
chmod 600 "$KEYFILE"

# Dead-man's switch, in seconds.  The EXIT trap below is the primary cleanup, but
# it only fires when THIS shell exits normally: a `kill -9`, a timeout that takes
# out the process group, or a lost SSH session leaves instances running with no
# trap to release them.  That happened -- three r6id.4xlarge hosts were orphaned
# when a wrapper timeout killed the orchestrator -- so every instance now also
# arms `shutdown -h` on itself at boot.  Combined with
# instance-initiated-shutdown-behavior=terminate, an instance cannot outlive this
# budget no matter what happens to the coordinator.
DEADMAN_MIN=${DEADMAN_MIN:-240}

MYIP=$(curl -s --max-time 10 https://checkip.amazonaws.com | tr -d '[:space:]')
[ -n "$MYIP" ] || die "cannot determine this host's public IP"
VPC=$(aws ec2 describe-vpcs --profile "$PROFILE" --filters Name=isDefault,Values=true \
        --query 'Vpcs[0].VpcId' --output text)
SGID=$(aws ec2 create-security-group --profile "$PROFILE" --group-name "$RUN-sg" \
        --description "pg_weave compete $RUN" --vpc-id "$VPC" \
        --query GroupId --output text) || die "create-security-group"
aws ec2 authorize-security-group-ingress --profile "$PROFILE" --group-id "$SGID" \
    --protocol tcp --port 22 --cidr "$MYIP/32" >/dev/null || die "authorize ssh"
# Engine hosts must reach each other's postgres for the driver phase; scoped to
# this run's own group rather than a CIDR.
aws ec2 authorize-security-group-ingress --profile "$PROFILE" --group-id "$SGID" \
    --protocol tcp --port 5432 --source-group "$SGID" >/dev/null 2>&1 || true

AMI=$(aws ssm get-parameters --profile "$PROFILE" \
    --names /aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64 \
    --query 'Parameters[0].Value' --output text)
[ "$AMI" != None ] || die "cannot resolve AL2023 AMI"
say "ami $AMI"

# -F /dev/null is load-bearing: a `Host *` block in the invoking user's
# ~/.ssh/config with ControlMaster and an explicit IdentityFile breaks
# fresh-instance auth entirely, and the symptom is an opaque "ssh never came up".
SSHOPTS=(-F /dev/null -o IdentitiesOnly=yes -o StrictHostKeyChecking=no
         -o UserKnownHostsFile=/dev/null -o ControlMaster=no -o ControlPath=none
         -o ConnectTimeout=15 -o LogLevel=ERROR -i "$KEYFILE")

# ---------------------------------------------------------------------------
# Launch every engine host at once.
# ---------------------------------------------------------------------------
for e in "${ENGINES[@]}"; do
    [ -f "$ROOT/bench/compete/engines/$e.sh" ] || die "no engine script for '$e'"
done

for e in "${ENGINES[@]}"; do
    itype=$ITYPE_LEXICAL
    grep -q '^# axis: vector' "$ROOT/bench/compete/engines/$e.sh" && itype=$ITYPE_VECTOR
    id=$(aws ec2 run-instances --profile "$PROFILE" \
        --image-id "$AMI" --instance-type "$itype" --key-name "$KEY" \
        --security-group-ids "$SGID" --count 1 \
        --block-device-mappings 'DeviceName=/dev/xvda,Ebs={VolumeSize=120,VolumeType=gp3,DeleteOnTermination=true}' \
        --instance-initiated-shutdown-behavior terminate \
        --user-data "$(printf '#!/bin/bash\nshutdown -h +%d\n' "$DEADMAN_MIN")" \
        --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$RUN-$e},{Key=Project,Value=pg_weave},{Key=Run,Value=$RUN},{Key=Engine,Value=$e}]" \
        --query 'Instances[0].InstanceId' --output text) \
        || die "run-instances for $e"
    IID[$e]=$id
    say "launched $e -> $id ($itype)"
done

aws ec2 wait instance-running --profile "$PROFILE" --instance-ids "${IID[@]}" \
    || die "some instance never ran"
for e in "${ENGINES[@]}"; do
    HOST[$e]=$(aws ec2 describe-instances --profile "$PROFILE" \
        --instance-ids "${IID[$e]}" \
        --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
    say "$e at ${HOST[$e]}"
done

wait_ssh() {
    local h=$1 i
    for i in $(seq 1 60); do
        ssh "${SSHOPTS[@]}" "ec2-user@$h" true 2>/dev/null && return 0
        sleep 10
    done
    return 1
}
for e in "${ENGINES[@]}"; do
    wait_ssh "${HOST[$e]}" || die "$e: ssh never came up"
done
say "ssh up on all hosts"

# ---------------------------------------------------------------------------
# Per-engine pipeline, all engines concurrently.  Each writes its own log; the
# coordinator computes every statistic afterwards from the raw samples.
# ---------------------------------------------------------------------------
run_engine() {
    local e=$1 h=${HOST[$1]} log="$OUT/$1.log"
    local sh=(ssh "${SSHOPTS[@]}" "ec2-user@$h")

    {
        echo "=== $e on $h ==="
        # Ship the harness and this engine's script.
        tar -C "$ROOT/bench/compete" -cf - lib engines corpus 2>/dev/null \
            | "${sh[@]}" 'mkdir -p ~/compete && tar -xf - -C ~/compete'
        # Build artifacts MUST be excluded, and the host MUST clean before building.
        #
        # Without this the tar ships the workstation's *.o/*.so, `make` sees them
        # newer than their sources and does not rebuild, and the remote link can
        # combine objects from different revisions.  It happened: renaming the
        # vendored sparsemap symbol prefix produced
        # "undefined symbol: __pg_bm25_sm_contains" on a clean host while the
        # incremental local build passed, because the stale callers still
        # referenced the old name.  The benign outcome is a link error; the
        # dangerous one is a successful link that MEASURES THE WRONG CODE.
        tar -C "$ROOT" -cf - --exclude=.git --exclude=.forgejo --exclude=.github --exclude='bench/compete/results' \
            --exclude='*.o' --exclude='*.so' --exclude='*.bc' --exclude='results' . \
            | "${sh[@]}" 'rm -rf ~/pg_weave && mkdir -p ~/pg_weave && tar -xf - -C ~/pg_weave'
        # Ship pg_fts from the workstation when it is present.  Cloning it on the
        # host would benchmark whatever upstream HEAD happens to be, which is not
        # the version this comparison claims to have measured.
        if [ -d "$HOME/ws/pg_fts/.git" ]; then
            git -C "$HOME/ws/pg_fts" archive --format=tar HEAD \
                | "${sh[@]}" 'mkdir -p ~/pg_fts && tar -xf - -C ~/pg_fts'
            git -C "$HOME/ws/pg_fts" rev-parse --short HEAD \
                | "${sh[@]}" 'cat > ~/pg_fts/.shipped_sha'
        fi

        "${sh[@]}" "bash ~/compete/engines/$e.sh provision" 2>&1
        # WIKI_SHA256 is the cross-host corpus assertion for a DOWNLOADED corpus.
        # A generated corpus is deterministic from its seed; a fetched one is not --
        # HuggingFace can re-shard, and "first N in parquet scan order" can then
        # select different articles.  The first host to build records the checksum;
        # every other host asserts against it and aborts BEFORE building an index.
        # Without this passthrough the assertion silently never runs.
        "${sh[@]}" "WIKI_SHA256='${WIKI_SHA256:-}' bash ~/compete/engines/$e.sh load $CORPUS" 2>&1
        "${sh[@]}" "bash ~/compete/engines/$e.sh index" 2>&1

        # Correctness gates BEFORE timing.  A failing gate is recorded and the
        # engine's latency is still measured, because "fast and wrong" is itself
        # the result -- pg_turbovec published a 2.3x win that was a
        # fast-but-wrong scalar fallback, later corrected to a 490x loss.
        "${sh[@]}" "bash ~/compete/engines/$e.sh gate" 2>&1 || echo "GATE FAILURES (recorded)"
        "${sh[@]}" "bash ~/compete/engines/$e.sh measure $SAMPLES $WARMUP $RUN" 2>&1
    } >"$log" 2>&1
    local rc=$?

    for f in raw.jsonl gates.jsonl hostinfo.txt build.txt; do
        "${sh[@]}" "cat ~/compete/out/$f 2>/dev/null" > "$OUT/$e.$f" 2>/dev/null || true
    done
    [ -s "$OUT/$e.raw.jsonl" ] && say "$e: collected $(wc -l < "$OUT/$e.raw.jsonl") raw records" \
        || say "$e: NO RAW DATA (see $log)"
    return $rc
}

pids=()
for e in "${ENGINES[@]}"; do
    run_engine "$e" & pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
say "all engine pipelines finished (fail=$fail)"

# ---------------------------------------------------------------------------
# Central analysis.  Per-engine hosts emit samples; only this step computes a
# statistic, and it refuses to publish if the corpus fingerprints differ.
# ---------------------------------------------------------------------------
say "analyzing"
raws=("$OUT"/*.raw.jsonl)
if [ -e "${raws[0]}" ]; then
    python3 "$ROOT/bench/compete/lib/wbench.py" stats "${raws[@]}" \
        --out "$OUT/SUMMARY.md" || say "ANALYZER REPORTED A FAILED GATE"
    cat "$OUT/gates.summary" 2>/dev/null
    for g in "$OUT"/*.gates.jsonl; do
        [ -s "$g" ] && python3 -c "
import json,sys
d=json.loads(open('$g').read().strip().split('\n')[0])
print('gates', d.get('engine'), ':', ' '.join(
    ('PASS' if r['passed'] else 'FAIL')+':'+r['gate'] for r in d['results']))" 2>/dev/null
    done
else
    say "no raw data collected from any engine -- nothing to analyze"
    fail=1
fi

say "done"
exit $fail
