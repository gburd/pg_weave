#!/usr/bin/env bash
#
# bench/aws/run.sh -- launch, build, test, benchmark, TERMINATE.
#
# Task P2 in doc/PHASES.md.  One command reproduces a recorded result on known
# hardware, and the instance dies afterwards even when something fails.
#
# Usage:
#	 bench/aws/run.sh [instance-type] [job]
#
#	 instance-type  default c7i.4xlarge.  See .agent/skills/weave-bench for the
#					types the source projects used: r6id.4xlarge for the lexical
#					5-way, i4i.8xlarge for at-scale stress.
#	 job			 default `smoke`.  One of:
#					  smoke	   build + regression + isolation + TAP + codec test
#					  bound	   the block-bound pruning sweep (bench/bound_pruning.c)
#					  lexical  smoke, then pg_weave vs tsvector+GIN on a real corpus
#					  fuzzy	   smoke, then the Z5 (fuzzy term~1/term~2) and Z6
#							     (character-class regex) latency gates on a 1M-row
#							     corpus, both index arms (trigrams off/on), each
#							     run twice (bench/fuzzy.sh)
#					  all	   all of the above
#					  winsweep   rerank window at n=1M, the Phase V frontier's
#							     fragile number (CPU only, no server)
#					  codescan   flat code-scan throughput at n up to 1M, three
#							     kernels, 4 and 3 bits (CPU only, no server).
#							     Needs an UNCONTENDED host to mean anything.
#					  csrecall   ONLY the recall half, at n=1M with CSNQ=100
#							     queries.  Exists because `codescan` spends ~40
#							     min running the two SCALAR reference kernels
#							     100x at n=1M to produce latencies that are not
#							     wanted, while the recall gate needs the query
#							     COUNT raised and nothing else.  Recall is
#							     deterministic, so this arm does not need an
#							     uncontended host -- but it is run here anyway
#							     because the corpus is here.
#					  csdim	   ns/vector for every kernel across dim (V17),
#							     two arms: fixed n and fixed code bytes.  Sets
#							     weave_score_kernel_best()'s registry order, which
#							     until V17 rested on the 960-d point alone.  CPU
#							     only, and it needs an UNCONTENDED host for the
#							     same reason codescan does.
#					  rerankcold winsweep, then cold p50 of a heap rerank with
#							     the real wvec type (bench/rerank_cold.sh)
#					  hnswbase   the pgvector HNSW baseline: bytes/vector,
#							     recall@10 vs ef, warm+cold p50. SEPARATE HOST
#							     from rerankcold -- one engine per host.
#					  fuse	   the fused-retrieval benchmark (doc/specs/FUSED_TOPK.md):
#							     CPU-embeds each dataset with sentence-transformers
#							     (bench/prepdata.py), then bench/fuse.sh over it.
#							     Wants a BIGGER instance than the default -- pass
#							     it as this script's first positional argument,
#							     e.g. `bench/aws/run.sh c7i.8xlarge fuse`.
#
#	 NDOCS / VOCAB environment variables size the lexical corpus (default 1M /
#	 200k).  A 1M-document run takes a few minutes to generate.  NDOCS / REPS size
#	 the fuzzy corpus and rep count (default 1M / 7); see bench/fuzzy.sh.
#
# The AWS profile is a BURNER and it changes -- `hotdog` as of 2026-09-18,
# `lava` before it, `bene` before that.  It is a default here and nowhere else,
# and no account id or AMI id is hardcoded anywhere in this harness, which is
# the only reason each swap has cost minutes instead of a rewrite.  Override
# with AWS_PROFILE.  Everything this script creates is tagged Project=pg_weave
# and named with the run id, so a stray is identifiable.
#
# TERMINATION IS NOT OPTIONAL.  The trap fires on EXIT, which covers success,
# failure, and Ctrl-C.  A forgotten bare-metal instance costs more than any
# benchmark is worth.  The script verifies the shutdown actually happened rather
# than assuming the API call worked.
#
set -uo pipefail

PROFILE=${AWS_PROFILE:-hotdog}
ITYPE=${1:-c7i.4xlarge}
JOB=${2:-smoke}
# Root volume size in GiB.  80 is fine for the code-only jobs; the corpus jobs
# need far more (GloVe unzips to ~2 GB, GIST's base set to ~4 GB, and the P0
# job's 2M-row index plus its heap does not fit alongside them in 80).  gp3
# IOPS and throughput are set explicitly because the defaults throttle, which
# would turn a merge measurement into a measurement of EBS.
VOLGB=${VOLGB:-80}
case "$JOB" in
	bitsweep|p0merge|vall) VOLGB=${VOLGB_OVERRIDE:-250} ;;
	# The cold jobs are the disk-hungry ones: GIST's base set is ~4 GB of fvecs,
	# a 1M x 960-d wvec table is ~4 GB of toast plus heap, and an HNSW index over
	# the same corpus is ~5 GB again.  400 leaves room to hold all of it at once
	# without the load failing halfway through a two-hour run.
	winsweep|rerankcold|hnswbase) VOLGB=${VOLGB_OVERRIDE:-400} ;;
	codescan|csrecall|csdim) VOLGB=${VOLGB_OVERRIDE:-250} ;;
	# fuse CPU-embeds a corpus with sentence-transformers.  The small BEIR sets
	# (scifact/nfcorpus/fiqa) it defaults to are tiny, but FUSE_DATASETS can be
	# pointed at MS MARCO, whose collection is ~3 GB compressed, plus the
	# sentence-transformer model cache at ~500 MB -- 250 leaves headroom instead
	# of prepdata.py dying partway through a download.
	fuse) VOLGB=${VOLGB_OVERRIDE:-250} ;;
	# vecmerge loads the same 1M x 960-d GIST corpus the cold jobs use (~4 GB of
	# fvecs, ~4 GB of heap plus toast) and then builds a weave index over it AND
	# merges it, so at peak it holds the index, its merge output, and the staging
	# table at once.  Same 400 as the other GIST jobs for the same reason.
	vecmerge) VOLGB=${VOLGB_OVERRIDE:-400} ;;
	# gatesweep loads the same BEIR corpora as fuse through the same loader, plus
	# the sentence-transformer model cache.  Same 250 for the same reason.
	gatesweep) VOLGB=${VOLGB_OVERRIDE:-250} ;;
esac
REGION=$(aws configure get region --profile "$PROFILE")
RUN=pgweave-$(date -u +%Y%m%d-%H%M%S)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/bench/aws/out/$RUN

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf '\033[31mFATAL: %s\033[0m\n' "$*" >&2; exit 1; }

mkdir -p "$OUT"
aws sts get-caller-identity --profile "$PROFILE" >"$OUT/identity.json" \
	|| die "profile $PROFILE cannot authenticate"

IID=""
KEYNAME=""
SGID=""
KEYFILE=$OUT/key.pem

cleanup() {
	local rc=$?
	say "cleanup (exit $rc)"

	if [ -n "$IID" ]; then
		say "terminating $IID"
		aws ec2 terminate-instances --profile "$PROFILE" \
			--instance-ids "$IID" --output text >/dev/null 2>&1
		# Verify, do not assume.  An API call that silently failed leaves a
		# running instance and a clean-looking log.
		for _ in $(seq 1 30); do
			st=$(aws ec2 describe-instances --profile "$PROFILE" \
				--instance-ids "$IID" \
				--query 'Reservations[].Instances[].State.Name' \
				--output text 2>/dev/null)
			say "  state: ${st:-unknown}"
			case "$st" in
				terminated|shutting-down) break ;;
			esac
			sleep 10
		done
		[ "$st" = terminated ] || [ "$st" = shutting-down ] \
			|| printf '\033[31mWARNING: %s may still be running -- CHECK MANUALLY\033[0m\n' "$IID" >&2
	fi

	# The security group cannot be deleted until the instance is gone.
	if [ -n "$SGID" ]; then
		for _ in $(seq 1 30); do
			aws ec2 delete-security-group --profile "$PROFILE" \
				--group-id "$SGID" >/dev/null 2>&1 && { say "deleted sg $SGID"; break; }
			sleep 10
		done
	fi
	[ -n "$KEYNAME" ] && aws ec2 delete-key-pair --profile "$PROFILE" \
		--key-name "$KEYNAME" >/dev/null 2>&1 && say "deleted key $KEYNAME"

	say "artifacts in $OUT"
	exit $rc
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
say "run $RUN  type=$ITYPE  job=$JOB  region=$REGION"

KEYNAME=$RUN-key
aws ec2 create-key-pair --profile "$PROFILE" --key-name "$KEYNAME" \
	--key-type ed25519 \
	--query KeyMaterial --output text >"$KEYFILE" || die "create-key-pair"
chmod 600 "$KEYFILE"

MYIP=$(curl -s --max-time 10 https://checkip.amazonaws.com | tr -d '[:space:]')
[ -n "$MYIP" ] || die "could not determine this host's public IP"

VPC=$(aws ec2 describe-vpcs --profile "$PROFILE" --filters Name=isDefault,Values=true \
	--query 'Vpcs[0].VpcId' --output text)
SGID=$(aws ec2 create-security-group --profile "$PROFILE" \
	--group-name "$RUN-sg" --description "pg_weave bench $RUN" \
	--vpc-id "$VPC" --query GroupId --output text) || die "create-security-group"
# SSH from this host only.  Never 0.0.0.0/0: a benchmark box with an open port is
# a liability, and the run is short enough that a single-IP rule is no burden.
aws ec2 authorize-security-group-ingress --profile "$PROFILE" --group-id "$SGID" \
	--protocol tcp --port 22 --cidr "$MYIP/32" >/dev/null || die "authorize-ingress"

# Ubuntu 24.04 LTS amd64, resolved from SSM so the AMI id is never hardcoded.
AMI=$(aws ssm get-parameters --profile "$PROFILE" \
	--names /aws/service/canonical/ubuntu/server/24.04/stable/current/amd64/hvm/ebs-gp3/ami-id \
	--query 'Parameters[0].Value' --output text)
[ "$AMI" != None ] || die "could not resolve Ubuntu 24.04 AMI"
say "ami $AMI"

IID=$(aws ec2 run-instances --profile "$PROFILE" \
	--image-id "$AMI" --instance-type "$ITYPE" --key-name "$KEYNAME" \
	--security-group-ids "$SGID" --count 1 \
	--block-device-mappings "DeviceName=/dev/sda1,Ebs={VolumeSize=$VOLGB,VolumeType=gp3,Iops=8000,Throughput=500,DeleteOnTermination=true}" \
	--instance-initiated-shutdown-behavior terminate \
	--tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$RUN},{Key=Project,Value=pg_weave}]" \
	--query 'Instances[0].InstanceId' --output text) || die "run-instances"
say "launched $IID"

aws ec2 wait instance-running --profile "$PROFILE" --instance-ids "$IID" || die "instance never ran"
HOST=$(aws ec2 describe-instances --profile "$PROFILE" --instance-ids "$IID" \
	--query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
say "host $HOST"

#
# -F /dev/null is load-bearing, not defensive.  A developer's ~/.ssh/config
# commonly has a `Host *` block, and this one sets ControlMaster auto with a
# shared ControlPath, an explicit IdentityFile, and a 5-second ConnectTimeout.
# Any of those breaks a fresh-instance connection: the mux socket can collide,
# and the global IdentityFile is offered ahead of ours so authentication fails
# before the launch key is ever tried.  The first version of this script did not
# pass -F and failed with a bare "ssh never came up" that looked like a
# networking problem and was not.
#
# IdentitiesOnly=yes stops the agent from offering unrelated keys.
# ControlMaster=no / ControlPath=none belt-and-braces in case -F is ever dropped.
#
SSH="ssh -F /dev/null -i $KEYFILE -o IdentitiesOnly=yes \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o ControlMaster=no -o ControlPath=none \
	-o ConnectTimeout=15 -o LogLevel=ERROR ubuntu@$HOST"
for i in $(seq 1 40); do
	$SSH true 2>/dev/null && break
	if [ "$i" = 40 ]; then
		# Capture what the instance itself thinks happened before giving up; a
		# bare "ssh never came up" sent the first debugging session down a
		# networking rabbit hole when the cause was client-side ssh config.
		aws ec2 get-console-output --profile "$PROFILE" --instance-id "$IID" \
			--output text >"$OUT/console.txt" 2>&1
		die "ssh never came up (console output in $OUT/console.txt)"
	fi
	sleep 10
done
say "ssh up"

# ---------------------------------------------------------------------------
say "provisioning"
#
# Ubuntu 24.04 ships PostgreSQL 16; pg_weave needs 17+.  So the PGDG apt
# repository is mandatory, not a convenience.
#
# Errors are NOT swallowed here.  The first version redirected apt output to
# /dev/null and used a `;` before the version check, so a failed install produced
# a silent no-op and the run died 30 seconds later with "make: command not found"
# -- a symptom three steps removed from the cause.  `set -e` inside the remote
# shell plus a checked exit status makes provisioning fail where it fails.
#
$SSH 'set -e
	export DEBIAN_FRONTEND=noninteractive
	sudo apt-get -qq update
	sudo apt-get -qq install -y curl ca-certificates gnupg lsb-release >/dev/null
	sudo install -d /usr/share/postgresql-common/pgdg
	sudo curl -fsSL -o /usr/share/postgresql-common/pgdg/apt.postgresql.org.asc \
		https://www.postgresql.org/media/keys/ACCC4CF8.asc
	echo "deb [signed-by=/usr/share/postgresql-common/pgdg/apt.postgresql.org.asc] \
https://apt.postgresql.org/pub/repos/apt $(lsb_release -cs)-pgdg main" \
		| sudo tee /etc/apt/sources.list.d/pgdg.list >/dev/null
	sudo apt-get -qq update
	sudo apt-get -qq install -y build-essential git clang lld pkg-config \
		postgresql-17 postgresql-server-dev-17 libipc-run-perl \
		postgresql-17-pgtap >/dev/null 2>&1 || \
	sudo apt-get -qq install -y build-essential git clang lld pkg-config \
		postgresql-17 postgresql-server-dev-17 libipc-run-perl >/dev/null
	echo "gcc:      $(gcc --version | head -1)"
	echo "pg_config: $(/usr/lib/postgresql/17/bin/pg_config --version)"
' 2>&1 | tee "$OUT/provision.log" || die "provisioning failed (see $OUT/provision.log)"

grep -q 'pg_config: PostgreSQL 17' "$OUT/provision.log" \
	|| die "PostgreSQL 17 not installed (see $OUT/provision.log)"

# Tune, and RECORD the tuning.  An untuned number and a tuned number differ by
# more than most of the changes being measured, so an unrecorded setting makes a
# result unusable (.agent/skills/weave-bench).
say "tuning postgresql"
MEMKB=$($SSH "awk '/MemTotal/{print \$2}' /proc/meminfo")
# 40% of RAM, in MB.  MEMKB is KILOBYTES, so ONE division by 1024 gives MB.
# This line used to divide twice and then label the result MB, so every tuned
# run this harness ever produced had shared_buffers set to 24 MB on a 61 GB box
# -- a 1000x under-allocation, in the step that exists specifically to stop an
# untuned number being mistaken for a tuned one.  It went unnoticed because the
# readback that would have shown it was itself broken (it ran as -U postgres and
# always failed), which is why the plausibility check below is not optional:
# a guard that cannot fail is not a guard.
SB=$(( MEMKB / 1024 * 40 / 100 ))
$SSH "sudo mkdir -p /etc/postgresql/17/main/conf.d
sudo tee -a /etc/postgresql/17/main/conf.d/bench.conf >/dev/null <<EOF
shared_buffers = ${SB}MB
maintenance_work_mem = 2GB
work_mem = 256MB
max_parallel_maintenance_workers = 8
max_parallel_workers_per_gather = 4
effective_cache_size = $(( MEMKB / 1024 / 1024 * 75 / 100 ))GB
checkpoint_timeout = 30min
max_wal_size = 8GB
random_page_cost = 1.1
jit = off
EOF
sudo pg_ctlcluster 17 main restart || sudo pg_ctlcluster 17 main start"

# Deliberately a separate, single-quoted step: nothing here needs local
# expansion, and folding it into the double-quoted heredoc above cost a
# debugging round on escaping.
#
# The superuser role exists because PGDG's default pg_hba uses peer auth for
# local connections, so running psql as -U postgres from the ubuntu account
# fails with "Peer authentication failed".  Every psql in this script is
# therefore unqualified.  The readback below used to run as -U postgres and so
# never worked at all: the harness recorded no tuning, and an unrecorded setting
# makes a result unusable, which is the entire reason this step exists.
#
# /scratch does not exist on the instance and an unprivileged mkdir cannot
# create one.
$SSH 'sudo -u postgres createuser -s $(whoami) 2>/dev/null || true
	# ...and a database of the same name, because psql with no -d connects to a
	# database named after the user.  Without this every unqualified psql fails
	# with `database "ubuntu" does not exist`.
	sudo -u postgres createdb -O $(whoami) $(whoami) 2>/dev/null || true
	sudo install -d -o $(whoami) -g $(whoami) /scratch
	# SHOW rather than a select over pg_settings: quoting SQL string literals
	# through ssh + two shells cost two debugging rounds ($$-quoting was expanded
	# to the remote shell PID, and escaped double quotes closed the outer string).
	# There is nothing to quote this way.
	for s in shared_buffers maintenance_work_mem work_mem effective_cache_size jit; do
		printf "%s = %s\n" "$s" "$(psql -tAc "show $s")"
	done' \
	2>&1 | tee "$OUT/tuning.log"

grep -q 'shared_buffers = ' "$OUT/tuning.log" \
	|| die "tuning could not be read back (see $OUT/tuning.log)"

# Plausibility, not just presence.  shared_buffers is meant to be ~40% of RAM,
# so anything reported in MB below four figures means the arithmetic or the unit
# suffix is wrong again and the run would measure an untuned server.
SBSEEN=$(sed -n 's/^shared_buffers = \([0-9]*\).*/\1/p' "$OUT/tuning.log")
SBUNIT=$(sed -n 's/^shared_buffers = [0-9]*\([A-Za-z]*\).*/\1/p' "$OUT/tuning.log")
case "$SBUNIT" in
	GB) : ;;
	MB) [ "${SBSEEN:-0}" -ge 1024 ] \
			|| die "shared_buffers came back as ${SBSEEN}${SBUNIT} -- untuned; refusing to measure" ;;
	*)  die "shared_buffers came back as '${SBSEEN}${SBUNIT}', which is not a size this check understands" ;;
esac
say "shared_buffers = ${SBSEEN}${SBUNIT} (of $(( MEMKB / 1024 / 1024 )) GB RAM)"

say "uploading source"
# git archive of HEAD: only committed state is measured, so a result can always
# be tied to a commit.
git -C "$ROOT" archive --format=tar --prefix=pg_weave/ HEAD \
	| $SSH 'cat > /tmp/src.tar && rm -rf ~/pg_weave && tar -xf /tmp/src.tar -C ~' \
	|| die "source upload"
$SSH 'cd pg_weave && git init -q 2>/dev/null; true'
echo "$(git -C "$ROOT" rev-parse HEAD)" > "$OUT/commit.txt"
say "commit $(cat "$OUT/commit.txt")"

$SSH 'cd pg_weave && nproc && free -g | head -2 && lscpu | grep -E "^Model name|^Flags" \
	| sed "s/Flags.*\(avx512[a-z0-9_]*\).*/Flags: has \1/" | head -3' \
	2>&1 | tee "$OUT/hostinfo.txt"

# ---------------------------------------------------------------------------
run_smoke() {
	say "build"
	$SSH 'cd pg_weave && make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config 2>&1 \
			| grep -viE "^(gcc|clang) " | tail -25; test -f pg_weave.so' \
		| tee "$OUT/build.log" || die "build failed (see $OUT/build.log)"
	say "build produced pg_weave.so"

	say "lint gates"
	$SSH 'cd pg_weave && for t in check-ascii check-alloc check-rename; do
			printf "%-14s " "$t"
			make -s $t PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1 \
				&& echo PASS || echo FAIL
		  done' 2>&1 | tee "$OUT/lint.log"

	say "standalone codec property test (17741 checks)"
	$SSH 'cd pg_weave && gcc -O2 -I include -o /tmp/tq test/hegel/test_quantize.c \
			src/vector/quantize.c src/vector/pack.c -lm && /tmp/tq' \
		2>&1 | tee "$OUT/codec.log"

	say "installcheck (regression + isolation)"
	# THE STATUS IS TAKEN, NOT PIPED PAST.  Until 2026-09-23 the remote command
	# ended in `| tail -30` and the local one in `| tee`, so BOTH exit statuses
	# belonged to the last command in a pipeline and `make installcheck` could fail
	# while the job carried on and took numbers off the host.  It did: a gatesweep
	# run recorded `1 of 19 tests failed` in this very log and then measured three
	# corpora.  AGENTS.md's eleventh member, sitting in the harness that fronts every
	# EC2 measurement this project has published.
	#
	# The status has to be rescued on BOTH sides of the ssh: the remote shell's own
	# exit status is `tail`'s, so the log goes to a file, the status is saved, the
	# tail is printed, and the status is re-raised.
	$SSH 'cd pg_weave && sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1
		  sudo -u postgres pg_ctlcluster 17 main start 2>/dev/null || true
		  sudo -u postgres createuser -s ubuntu 2>/dev/null || true
		  make installcheck PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/tmp/ic.log 2>&1
		  rc=$?; tail -30 /tmp/ic.log; exit $rc' \
		| tee "$OUT/installcheck.log"
	icrc=${PIPESTATUS[0]}
	$SSH 'cd pg_weave && cat regression.diffs 2>/dev/null | head -60' > "$OUT/regression.diffs" 2>/dev/null

	# No separate TAP step: TAP_TESTS = 1 in the Makefile means `make
	# installcheck` already ran t/*.pl above, and PGXS exposes no
	# prove_installcheck target to invoke them again.  The installcheck log holds
	# the TAP results.
	grep -E '^(t/|All tests|Result:|Files=)' "$OUT/installcheck.log" \
		> "$OUT/tap.log" 2>/dev/null || true

	# FATAL, and the dispatch comment at the bottom of this file already said why:
	# "the host has passed regression + isolation + TAP before any number is taken
	# off it, which is this project's own rule about correctness preceding latency
	# applied to the machine rather than to the code."  That was a claim the code did
	# not enforce.  SMOKE_TOLERATE_RED=1 exists for the one legitimate case -- a run
	# whose PURPOSE is to measure a host with a known-red test -- and it has to be
	# asked for, so the number it produces is labelled by its own invocation.
	if [ "$icrc" != 0 ]; then
		say "installcheck FAILED; the diff is:"
		cat "$OUT/regression.diffs" >&2
		[ "${SMOKE_TOLERATE_RED:-0}" = 1 ] \
			|| die "installcheck failed on this host -- no number taken from it is
			        trustworthy until you know whether the red test touches what you
			        are measuring.  Re-run with SMOKE_TOLERATE_RED=1 if you have
			        decided it does not."
		say "SMOKE_TOLERATE_RED=1: continuing over a red installcheck BY REQUEST"
	fi
}

# Ship and install the UPSTREAM fork, pg_fts, so bench/lexical.sh gets its third
# arm.  Optional by design and never fatal: a host without it produces a two-arm
# table with a banner, which is the honest degradation.  Returns non-zero when the
# arm will not run, and the caller records that.
#
# Source comes from `git archive HEAD` of a local checkout, the same rule the
# pg_weave upload follows and for the same reason -- only committed state is
# measured, so a number can always be tied to a commit in BOTH projects.  Nothing
# is cloned on the host: that would need credentials there, and the sibling
# repository is not public.
upload_pgfts() {
	local root=${PGFTS_ROOT:-$HOME/ws/pg_fts}

	if [ ! -d "$root/.git" ]; then
		say "no pg_fts checkout at $root -- the fork-vs-fork arm will be SKIPPED"
		return 1
	fi
	say "uploading pg_fts from $root"
	git -C "$root" archive --format=tar --prefix=pg_fts/ HEAD \
		| $SSH 'cat > /tmp/fts.tar && rm -rf ~/pg_fts && tar -xf /tmp/fts.tar -C ~' \
		|| { say "pg_fts upload failed -- arm SKIPPED"; return 1; }
	git -C "$root" rev-parse HEAD > "$OUT/pgfts_commit.txt"
	git -C "$root" describe --tags --always > "$OUT/pgfts_version.txt" 2>/dev/null || true
	say "pg_fts $(cat "$OUT/pgfts_version.txt" 2>/dev/null) $(cat "$OUT/pgfts_commit.txt")"

	# The build is checked by its OWN exit status and by the artifact, in that
	# order.  AGENTS.md's ninth verification-error member is a stale .so making a
	# failed build look like a pass, so the tree is cleaned first.
	$SSH 'cd pg_fts && make -s clean >/dev/null 2>&1; \
		  make -s PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/tmp/fts.build.log 2>&1 \
		  && test -f pg_fts.so \
		  && sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >>/tmp/fts.build.log 2>&1 \
		  && echo PGFTS_INSTALL_OK || { echo PGFTS_INSTALL_FAILED; tail -20 /tmp/fts.build.log; }' \
		2>&1 | tee "$OUT/pgfts_build.log" | grep -q PGFTS_INSTALL_OK \
		|| { say "pg_fts build/install FAILED (see $OUT/pgfts_build.log) -- arm SKIPPED"; return 1; }
	say "pg_fts installed"
	return 0
}

run_lexical() {
	# The comparison that decides adoption: pg_weave against the tsvector+GIN
	# baseline every PostgreSQL user already has, on the same host and the same
	# stored analyzed column -- plus, since 2026-09-21, against pg_fts, the
	# project pg_weave was forked from, which is the only arm that separates an
	# inherited number from an earned one.  Correctness is gated before any timing.
	upload_pgfts || say "continuing with two arms; the results file must say so"
	say "lexical benchmark vs tsvector+GIN and pg_fts"
	$SSH "cd pg_weave && sudo -u postgres createuser -s ubuntu 2>/dev/null; \
		  export PATH=/usr/lib/postgresql/17/bin:\$PATH PGDATABASE=weavebench; \
		  bash bench/lexical.sh ${NDOCS:-1000000} ${VOCAB:-200000} 7" \
		2>&1 | tee "$OUT/lexical.log"
}

# The CPU embedder, factored out of run_fuse when the gatesweep job needed the same
# corpus.  Idempotent: pip no-ops on a second install, so two jobs in one session
# pay for it once.  Everything it guards against is in the comments below, each of
# which corresponds to a run that died after paying for provisioning.
ensure_embedder() {
	say "installing sentence-transformers into a venv (pulls torch CPU -- several minutes with NO output; this is EXPECTED, not a hang)"
	$SSH 'set -e
		export DEBIAN_FRONTEND=noninteractive
		sudo apt-get -qq install -y python3-venv >/dev/null
		python3 -m venv /scratch/venv
		/scratch/venv/bin/pip install --quiet --upgrade pip
		/scratch/venv/bin/pip install --quiet torch \
			--index-url https://download.pytorch.org/whl/cpu
		/scratch/venv/bin/pip install --quiet sentence-transformers
		/scratch/venv/bin/python3 -c "import sentence_transformers, torch; \
print(\"EMBEDDER_OK st\", sentence_transformers.__version__, \"torch\", torch.__version__)"' \
		2>&1 | tee "$OUT/fuse-pip-install.log" \
		|| die "sentence-transformers install FAILED -- tail of $OUT/fuse-pip-install.log:
$(tail -30 "$OUT/fuse-pip-install.log")"
	# Assert the IMPORT succeeded, not merely that pip exited 0: a resolver that
	# installs a broken combination still exits 0, and the failure would then
	# surface as prepdata.py dying after the corpus download.
	grep -q EMBEDDER_OK "$OUT/fuse-pip-install.log" \
		|| die "sentence-transformers installed but does not import (see $OUT/fuse-pip-install.log)"
	say "embedder ready: $(grep EMBEDDER_OK "$OUT/fuse-pip-install.log")"
}

run_fuse() {
	# The fused-retrieval benchmark (doc/specs/FUSED_TOPK.md): each dataset gets
	# CPU-embedded with a sentence-transformer (bench/prepdata.py), then scored
	# through bench/fuse.sh.  Neither file is fatal to expect missing here --
	# both are written by a parallel task -- but the job itself is real.
	#
	# The embedder needs sentence-transformers, which is not on the base image.
	#
	# THREE THINGS THIS IMAGE MAKES NECESSARY, each of which killed a run:
	#  1. `pip3` DOES NOT EXIST on Ubuntu 24.04's base image.  The first attempt
	#     died on `pip3: command not found` after provisioning, building and
	#     passing the whole test suite -- about twelve minutes of paid instance.
	#  2. Even with pip installed, 24.04 marks the system Python
	#     EXTERNALLY-MANAGED (PEP 668), so a system-wide `pip install` refuses.
	#     Hence a venv, which is also cleaner: nothing this benchmark installs
	#     can perturb the system Python the server tooling uses.
	#  3. Plain `pip install sentence-transformers` resolves torch to the default
	#     wheel, which carries the CUDA runtime -- gigabytes, on a CPU instance
	#     that cannot use one.  torch comes from the explicit CPU index first, so
	#     the dependency is already satisfied when sentence-transformers is
	#     resolved.
	#
	# Installed ONCE, before any dataset, not per-dataset: pip no-ops on a second
	# install, and running it inside the loop would just make the "is this hung?"
	# moment happen four times instead of once.
	ensure_embedder

	local datasets=${FUSE_DATASETS:-"scifact nfcorpus fiqa"}
	local limit=${FUSE_LIMIT:-0}
	# /scratch, not /mnt/data.  /mnt/data is not mounted and not writable on this
	# image -- `mkdir -p /mnt/data/bench` fails for an unprivileged user -- while
	# /scratch is created with the right ownership during provisioning and is what
	# every other job in this file uses.
	local bench=/scratch/bench

	for D in $datasets; do
		say "fuse: preparing $D (embed=minilm, limit=$limit)"
		$SSH "cd pg_weave && mkdir -p $bench && \
			  /scratch/venv/bin/python3 bench/prepdata.py --dataset $D \
				--out $bench --embed minilm --limit \"$limit\"" \
			2>&1 | tee "$OUT/fuse-$D-prep.log" \
			|| die "prepdata.py failed for $D (see $OUT/fuse-$D-prep.log)"

		say "fuse: running $D (${REPS:-7} reps)"
		# LATN and CHECKN are forwarded rather than left to fuse.sh's
		# smoke-sized defaults.  CHECKN drives the RECALL-VS-EXHAUSTIVE row of
		# FUSED_TOPK.md sect. 8, which that table calls the most valuable row in
		# it -- a single miss is a (C2) violation -- so it should cover as many
		# queries as the clock allows, not the ten a smoke run wants.  Each one
		# costs two exhaustive per-channel scans, so it is linear in
		# queries x documents and worth watching on the larger sets.
		#
		# fuse.sh's own python is only bench/ndcg.py, which is stdlib-only, so
		# the system python3 is correct there -- the venv is the EMBEDDER's, not
		# the harness's.
		$SSH "cd pg_weave && LATN=\"${LATN:-50}\" CHECKN=\"${CHECKN:-100}\" \
			  bash bench/fuse.sh $bench $D \"${REPS:-7}\"" \
			2>&1 | tee "$OUT/fuse-$D.log" \
			|| die "fuse.sh failed for $D (see $OUT/fuse-$D.log)"

		# Pulled back HERE, per dataset, not once at the end.  AGENTS.md hard
		# rule 14: pull artefacts incrementally, never in one final scp -- a
		# burner expiring mid-run has already cost a sibling project an
		# instance and every byte of its data.
		say "fuse: pulling back $D artifacts"
		mkdir -p "$OUT/fuse-$D"
		for f in $($SSH "find $bench/$D -maxdepth 2 \
				\( -name manifest.json -o -name '*.tsv' \) 2>/dev/null"); do
			$SSH "cat '$f'" > "$OUT/fuse-$D/$(basename "$f")" \
				|| say "fuse: could not pull back $f for $D -- continuing, host may still be terminated on schedule"
		done
	done
}

run_fuzzy() {
	# The Z5 (fuzzy term~1/term~2) and Z6 (character-class regex) latency gates,
	# for the record.  Both routes are implemented (weave_fuzzy_terms(),
	# weave_regex_terms(), src/am/amscan.c) and both were previously measured
	# only on a dev box -- no bench/aws/run.sh run behind them, so no
	# commit-tied number.  Correctness is gated before any timing.
	say "fuzzy/regex benchmark: Z5 and Z6 gates"
	$SSH "cd pg_weave && sudo -u postgres createuser -s ubuntu 2>/dev/null; \
		  export PATH=/usr/lib/postgresql/17/bin:\$PATH PGDATABASE=weavebench; \
		  bash bench/fuzzy.sh ${NDOCS:-1000000} ${REPS:-7}" \
		2>&1 | tee "$OUT/fuzzy.log"
}

run_bitsweep() {
	# THE measurement the Phase V gate turns on: the smallest code width whose
	# full-probe compressed-domain recall@10 reaches 0.99.  See doc/PHASES.md's
	# Phase V gate block and doc/specs/VECTOR_CHANNEL.md sect. 2.1.1 for why the
	# answer decides whether the `size <= 0.15x pgvector HNSW` claim survives.
	#
	# Widths 2, 3 and 4 are re-measured, not assumed: the codebook solver was
	# found to have never converged (a 200-sweep cap against the ~700 sweeps a
	# 16-level solve needs), so the previously published 0.9205 / 0.8780 figures
	# came from an unconverged 4-bit codebook.  This run replaces them.
	#
	# FULL PROBE is the point.  probes == lists means zero probe-miss error, so
	# the recall reported on that row is the ceiling over every nprobe -- a
	# property of the codebook and the estimator alone.  A better partition
	# cannot raise it, which is what makes it a valid gate.
	say "fetching corpora"
	# Two independent fetches, deliberately not one `set -e` block: GIST comes
	# over FTP from ftp.irisa.fr and is ~2.6 GB, so it is the likely failure, and
	# losing it must not also lose the GloVe half of the sweep.
	$SSH 'set -e
		sudo apt-get install -y unzip bc >/dev/null 2>&1 || true
		mkdir -p /scratch/corpus && cd /scratch/corpus
		if [ ! -f glove.6B.200d.txt ]; then
			curl -sSLO https://nlp.stanford.edu/data/glove.6B.zip
			unzip -o -q glove.6B.zip glove.6B.200d.txt && rm -f glove.6B.zip
		fi
		ls -l glove.6B.200d.txt' \
		2>&1 | tee "$OUT/corpus.log" || die "GloVe fetch failed"
	GIST_OK=yes
	$SSH 'set -e
		cd /scratch/corpus
		if [ ! -f gist/gist_base.fvecs ]; then
			curl -sS --connect-timeout 30 -O \
				ftp://ftp.irisa.fr/local/texmex/corpus/gist.tar.gz
			tar xzf gist.tar.gz && rm -f gist.tar.gz
		fi
		ls -l gist/gist_base.fvecs' \
		2>&1 | tee -a "$OUT/corpus.log" || GIST_OK=no
	[ "$GIST_OK" = yes ] || say "WARNING: GIST unavailable; GloVe half only"

	say "building ivf_recall"
	$SSH 'cd pg_weave && gcc -O2 -march=native -std=gnu99 -I include \
			-o /scratch/ivf_recall bench/ivf_recall.c \
			src/vector/quantize.c src/vector/pack.c -lm && echo built' \
		2>&1 | tee -a "$OUT/corpus.log" || die "ivf_recall build failed"

	# One invocation per corpus walks every (lists, bits, probes) cell, so the
	# whole width sweep is two commands rather than fourteen.
	say "GloVe-200d: bits 2..8, full probe"
	$SSH '/scratch/ivf_recall glove /scratch/corpus/glove.6B.200d.txt 200000 200 \
			lists=512 bits=2,3,4,5,6,7,8 probes=1,8,32,128,512 windows=10,100 k=10' \
		2>&1 | tee "$OUT/bitsweep_glove.log"

	say "GIST-960d: bits 2..8, full probe"
	if [ "$GIST_OK" = yes ]; then
		$SSH '/scratch/ivf_recall fvecs /scratch/corpus/gist/gist_base.fvecs 100000 100 \
				lists=512 bits=2,3,4,5,6,7,8 probes=1,8,32,128,512 windows=10,100 k=10' \
			2>&1 | tee "$OUT/bitsweep_gist.log"
	else
		say "skipped: GIST corpus unavailable"
	fi
}

# ---------------------------------------------------------------------------
# Corpus fetch shared by the three vector-gate jobs.  GloVe is skipped: all three
# concern the 1M x ~1024-d regime the Phase V gate is written against, and GIST is
# the only corpus at hand with a million vectors at that width.
fetch_gist() {
	say "fetching GIST-1M"
	$SSH 'set -e
		sudo apt-get -qq install -y bc >/dev/null 2>&1 || true
		mkdir -p /scratch/corpus && cd /scratch/corpus
		if [ ! -f gist/gist_base.fvecs ]; then
			curl -sS --connect-timeout 30 -O \
				ftp://ftp.irisa.fr/local/texmex/corpus/gist.tar.gz
			tar xzf gist.tar.gz && rm -f gist.tar.gz
		fi
		ls -l gist/gist_base.fvecs gist/gist_query.fvecs' \
		2>&1 | tee "$OUT/corpus.log" || die "GIST fetch failed"
}

run_winsweep() {
	# The fragile half of the Phase V frontier, re-measured at the gate's own
	# corpus size.
	#
	# bench/RESULTS_BITWIDTH_SWEEP.md establishes the window a b-bit code needs
	# for recall@10 >= 0.99 -- 20 at 4 bits -- but it does so at n = 100k-200k,
	# and the whole rerank cost is linear in that window.  A window has to be wide
	# enough to still contain the true top-10 after quantization perturbs the
	# ordering, and nothing makes that requirement invariant in n: ten times the
	# vectors put roughly ten times more near-neighbours in range to displace
	# them.  So the published 20 is a LOWER BOUND for the 1M gate corpus, and
	# every page-read figure derived from it inherits that.
	#
	# Full probe (probes == lists) throughout, which makes probe-miss error zero
	# and the recall a property of the codebook and the estimator alone.  The
	# harness self-check must PASS or the numbers are not a ceiling.
	fetch_gist
	say "building ivf_recall"
	$SSH 'cd pg_weave && gcc -O2 -march=native -std=gnu99 -I include \
			-o /scratch/ivf_recall bench/ivf_recall.c \
			src/vector/quantize.c src/vector/pack.c -lm && echo built' \
		2>&1 | tee -a "$OUT/corpus.log" || die "ivf_recall build failed"

	# n = 1M, the gate corpus size, against the same widths and windows the
	# published frontier was drawn from so the two are directly comparable.
	say "GIST-1M 960-d, n=1M: bits 3..5 x windows 10..200, full probe"
	$SSH 'cd /scratch && ./ivf_recall fvecs corpus/gist/gist_base.fvecs 1000000 100 \
			lists=1024 bits=3,4,5 probes=1024 windows=10,15,20,25,30,40,50,75,100,150,200 k=10' \
		2>&1 | tee "$OUT/winsweep_1m.log"

	# The n = 100k row from the published frontier, re-run on THIS host with THIS
	# binary.  Without it, an n=1M window that differs from the published one
	# cannot be attributed to n rather than to the machine or the build.
	say "control: same binary at n=100k, the published operating point"
	$SSH 'cd /scratch && ./ivf_recall fvecs corpus/gist/gist_base.fvecs 100000 100 \
			lists=512 bits=3,4,5 probes=512 windows=10,15,20,25,30,40,50,75,100 k=10' \
		2>&1 | tee "$OUT/winsweep_100k.log"
}

run_rerankcold() {
	# Cold p50 of a heap rerank with the real wvec type.  See bench/rerank_cold.sh
	# for the method and its guards.  pgvector's half runs on a SEPARATE instance
	# (job hnswbase): one engine per host.
	fetch_gist
	say "build + install pg_weave"
	$SSH 'cd pg_weave && make -s PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1 \
		&& sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null \
		&& echo installed' 2>&1 | tee "$OUT/build.log" || die "build/install failed"

	say "cold rerank latency"
	# `|| die`, because run.sh runs without `set -e`: the first version of this
	# job died on its first SQL statement and the driver still printed
	# "done -- artifacts in ...", which is exactly the class of failure this
	# harness keeps producing.  Artifacts are collected BEFORE the check so a
	# failed run still yields its logs.
	$SSH "cd pg_weave && OUT=\$HOME/out NROWS=${NROWS:-1000000} NSAMP=${NSAMP:-25} \
			bash bench/rerank_cold.sh" 2>&1 | tee "$OUT/rerankcold.log"
	rc=${PIPESTATUS[0]}
	$SSH 'cd ~/out && tar cf - .' | tar xf - -C "$OUT" 2>/dev/null || true
	[ "$rc" = 0 ] || die "rerank_cold.sh failed (see $OUT/rerankcold.log)"
}

run_hnswbase() {
	# The baseline, measured rather than estimated: HNSW bytes/vector (the
	# denominator of every "x HNSW" figure in Phase V), whether it reaches
	# recall@10 0.99 at any ef, and its warm and cold p50.
	fetch_gist
	say "installing pgvector"
	# Presence is checked by looking for the module file, not by asking the
	# server: a SQL string literal here would have to survive a single-quoted ssh
	# argument and two shells, and that quoting has already cost this harness two
	# debugging rounds (see the tuning step).
	$SSH 'set -e
		sudo apt-get -qq install -y postgresql-17-pgvector >/dev/null
		ls -l /usr/lib/postgresql/17/lib/vector.so' \
		2>&1 | tee "$OUT/pgvector.log" || die "pgvector install failed"

	say "HNSW baseline"
	$SSH "cd pg_weave && OUT=\$HOME/out NROWS=${NROWS:-1000000} NSAMP=${NSAMP:-25} \
			bash bench/hnsw_base.sh" 2>&1 | tee "$OUT/hnswbase.log"
	rc=${PIPESTATUS[0]}
	$SSH 'cd ~/out && tar cf - .' | tar xf - -C "$OUT" 2>/dev/null || true
	[ "$rc" = 0 ] || die "hnsw_base.sh failed (see $OUT/hnswbase.log)"
}

run_codescan() {
	# The measurement the restated Phase V gate's latency term now turns on.
	#
	# bench/RESULTS_CODE_SCAN.md settled the STRUCTURAL half locally: the block
	# bound prunes 0.00% on GIST-960d and 0.01% on GloVe-200d, so a query scores
	# every code in the index.  What that costs in milliseconds could not be
	# measured there -- the workstation was at load average 26 on 8 cores and
	# identical work swung 2.2x.  A latency number taken under contention is not a
	# latency number, so it is taken here.
	#
	# TIMING RUNS USE order=natural, DELIBERATELY.  Nothing prunes, so every arm
	# scores every lane and the warp ordering cannot change the time; and
	# clustering at these sizes is the most expensive thing in the job by far.
	# The first version of this job asked for order=clustered with lists = n/32,
	# which at n = 1M is a k-means over 31,250 centroids: O(n * lists * dim) is
	# ~3e13 flops PER ITERATION, times six iterations, times three kernels,
	# because it also re-clustered for every kernel.  It would not have finished.
	# Preparation is per-corpus and timing is per-kernel, which is what
	# `kernel=all` now expresses.
	#
	# One clustered arm is kept at 200k to confirm the 0% pruning result on an
	# uncontended host at a size larger than the local run, since that is the
	# finding the rest of the phase now rests on.
	fetch_gist
	say "building code_scan"
	$SSH 'cd pg_weave && gcc -O2 -march=native -std=gnu99 -I include \
			-o /scratch/code_scan bench/code_scan.c src/vector/quantize.c \
			src/vector/pack.c src/vector/kernels.c -lm && echo built' \
		2>&1 | tee "$OUT/build.log" || die "code_scan build failed"

	# THE DIFFERENTIAL SELF-CHECK IS A GATE, AND IT RUNS ON THIS HOST, NOT ONLY ON
	# THE WORKSTATION.  `lut-byte` is an approximate SIMD kernel: it quantizes the
	# query table to 8 bits and accumulates in integers, so it cannot be compared
	# against the exact oracle for equality, only against its own scalar reference
	# `lut-byte-ref`.  -march=native here is not -mavx2 there, so the binary being
	# timed is not the binary that was checked locally.  Timing a kernel that is
	# wrong on THIS host would produce exactly the fast-but-wrong headline
	# AGENTS.md rule 8 exists to forbid, so if this exits non-zero the job dies
	# before any number is taken.
	say "byte-LUT differential self-check on this host (gate)"
	$SSH 'cd /scratch && ./code_scan selfcheck=2000' \
		2>&1 | tee "$OUT/selfcheck.log" || die "byte-LUT self-check FAILED on the bench host -- no timing below would mean anything"

	# THE SINGLE-CORE BANDWIDTH FLOOR, on this host, for this reason: the local
	# analysis says the flat scan is compute-bound with about an order of magnitude
	# of headroom, since 480 B/vector in 291 ns is only ~1.6 GB/s.  That headroom
	# is what makes a faster kernel worth writing at all, and it is an estimate
	# until something measures the wall it is headroom to.  A sequential read-sum
	# over a buffer far larger than L3, single-threaded, is the right shape: the
	# code scan reads its codes exactly once, sequentially, and never revisits
	# them.  This bounds any kernel we could ever write on this hardware.
	say "single-core sequential read bandwidth (the wall the kernel cannot pass)"
	$SSH 'cat > /scratch/bw.c <<EOF
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(void)
{
	size_t n = (size_t) 2048 * 1024 * 1024;	/* 2 GiB, far past any L3 */
	unsigned char *b = malloc(n);
	unsigned long long s = 0;
	struct timespec t0, t1;
	if (!b) return 1;
	for (size_t i = 0; i < n; i++) b[i] = (unsigned char) i;
	for (int rep = 0; rep < 3; rep++)
	{
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (size_t i = 0; i < n; i += 64) s += b[i];
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double sec = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
		printf("touch-every-cacheline  %.2f GB/s\n", (double) n / sec / 1e9);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (size_t i = 0; i < n; i++) s += b[i];
		clock_gettime(CLOCK_MONOTONIC, &t1);
		sec = (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec);
		printf("read-every-byte        %.2f GB/s\n", (double) n / sec / 1e9);
	}
	return (int) (s & 1);
}
EOF
		gcc -O2 -march=native -o /scratch/bw /scratch/bw.c && /scratch/bw' \
		2>&1 | tee "$OUT/bandwidth.log" || say "bandwidth probe failed (not fatal)"

	# n is swept so the per-vector cost is measured at three sizes rather than
	# extrapolated from one, and so the L3-resident case (50k = 23 MB of codes) is
	# distinguishable from the DRAM-bound case (1M = 458 MB).
	for N in 50000 200000 1000000; do
		say "n=$N, all kernels, 4 bits, natural order"
		$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs $N \
				${CSNQ:-10} bits=4 k=10 order=natural kernel=all \
				queries=corpus/gist/gist_query.fvecs" \
			2>&1 | tee -a "$OUT/codescan.log" || die "code_scan n=$N failed"
	done

	# 3 bits at n=1M, because the ratified shape's first named revision trigger is
	# exactly this trade: 3 bits scans 25% fewer bytes and needs a rerank window of
	# 50 instead of 25.  Without the scan cost at both widths there is nothing to
	# weigh.
	say "n=1000000, 3 bits -- the revision-trigger comparison"
	$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs 1000000 \
			${CSNQ:-10} bits=3 k=10 order=natural kernel=all \
			queries=corpus/gist/gist_query.fvecs" \
		2>&1 | tee -a "$OUT/codescan.log" || die "code_scan 3-bit failed"

	# The two-stage prefix scan, timed on THIS host.  It is the candidate answer to
	# the scan cost, and the number it has to be compared against -- pgvector HNSW
	# warm p50 at matched recall -- comes from bench/hnsw_base.sh on a different
	# instance.  Same CPU model and a single-threaded scan make that defensible,
	# but a favourable result on a cross-host comparison is exactly what a skeptic
	# should attack, so pass ITYPE=r7i.2xlarge to put both sides on the same type.
	say "two-stage prefix scan, n=1M, timed"
	$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs 1000000 \
			${CSNQ:-10} bits=4 k=10 order=natural kernel=lut-wide \
			prefix=960,480,240,120 pwin=8000,20000 \
			queries=corpus/gist/gist_query.fvecs" \
		2>&1 | tee -a "$OUT/prefix.log" || die "prefix scan failed"

	# DO V15 AND A FASTER KERNEL COMPOSE, OR DOES ONE MAKE THE OTHER POINTLESS?
	# The prefix scan (V15) scores fewer coordinates; a byte-LUT kernel scores each
	# coordinate more cheaply.  The two levers are independent in principle, so the
	# same prefix/window grid runs through `lut-byte` to see whether the product
	# holds -- and, more to the point, whether the flat byte-LUT scan is already
	# fast enough that spending V15's recall on top of it buys nothing.  Same grid
	# as the arm above so the two tables can be read side by side.
	say "two-stage prefix scan through the byte-LUT kernel, n=1M"
	$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs 1000000 \
			${CSNQ:-10} bits=4 k=10 order=natural kernel=lut-byte \
			prefix=960,480,240,120 pwin=8000,20000 \
			queries=corpus/gist/gist_query.fvecs" \
		2>&1 | tee -a "$OUT/prefix_byte.log" || die "byte-LUT prefix scan failed"

	# The clustered arm is OPT-IN (CSCLUSTER=1) because it re-derives an answer we
	# already have from two clean-host runs -- 0.00% pruning at n=200k, lists=6250 --
	# and a k-means over 6,250 centroids at 960-d costs ~11 minutes PER ITERATION,
	# six of them, which is most of an instance-hour to confirm a number twice
	# confirmed.  Set CSCLUSTER=1 when the bound itself is what changed.
	if [ "${CSCLUSTER:-0}" != "1" ]; then
		say "skipping the clustered arm (CSCLUSTER=1 to run it); 0.00% pruning already confirmed twice on clean hosts"
		return
	fi

	say "n=200000 clustered -- does the bound prune at scale on a clean host?"
	$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs 200000 \
			${CSNQ:-10} bits=4 k=10 order=clustered lists=6250 iters=6 \
			kernel=lut-wide queries=corpus/gist/gist_query.fvecs" \
		2>&1 | tee -a "$OUT/codescan.log" || die "code_scan clustered failed"
}

run_csrecall() {
	# THE RECALL HALF OF PHASE V's GATE, at the query count the gate actually needs.
	#
	# Every n=1M recall figure on record was taken with nq=10 -- 100 ground-truth
	# slots -- which cannot separate 0.99 from 1.00.  The gate asks for recall@10
	# >= 0.99 at n >= 1M, so it was not evidenced at that size no matter which way
	# the number came out.  CSNQ defaults to 100 here (1,000 slots, 0.1%
	# resolution) rather than to 10.
	#
	# Only the prefix grids run.  `codescan` would additionally spend ~40 minutes
	# putting the two scalar reference kernels through 100 queries at n=1M, whose
	# latencies are already known and whose recall is identical to the fast paths
	# by construction -- lut-byte is gated as bit-identical to lut-byte-ref, and
	# lut-wide is bit-identical to the scalar oracle.
	#
	# prefix=960 is the full-dim point: stage 1 becomes the whole flat scan, so
	# that row IS the flat scan's end-to-end recall, which is the specific number
	# doc/COMPETITIVE.md's gate table currently carries a caveat about.
	fetch_gist
	say "building code_scan"
	$SSH 'cd pg_weave && gcc -O2 -march=native -std=gnu99 -I include \
			-o /scratch/code_scan bench/code_scan.c src/vector/quantize.c \
			src/vector/pack.c src/vector/kernels.c -lm && echo built' \
		2>&1 | tee "$OUT/build.log" || die "code_scan build failed"

	# Same gate as the codescan job, same reason: -march=native here is not
	# -mavx2 on the workstation, so the binary about to produce recall numbers is
	# not the binary that was checked there.
	say "byte-LUT differential self-check on this host (gate)"
	$SSH 'cd /scratch && ./code_scan selfcheck=2000' \
		2>&1 | tee "$OUT/selfcheck.log" \
		|| die "byte-LUT self-check FAILED on the bench host -- no recall below would mean anything"

	for K in lut-wide lut-byte; do
		say "recall grid, n=1M, nq=${CSNQ:-100}, kernel=$K"
		$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs 1000000 \
				${CSNQ:-100} bits=4 k=10 order=natural kernel=$K \
				prefix=960,480,240,120 pwin=8000,20000 \
				queries=corpus/gist/gist_query.fvecs" \
			2>&1 | tee -a "$OUT/recall_$K.log" || die "recall grid failed for $K"
	done
}

run_csdim() {
	# V17: WHICH KERNEL WINS AT WHICH DIMENSIONALITY.
	#
	# Every kernel latency on record is at 960 dimensions, and
	# weave_score_kernel_best() picks by registry order for every dim.  V16
	# reordered that list on the strength of the 960-d point alone, which is the
	# same unmeasured-default mistake it was fixing, one position along.
	#
	# TWO ARMS, because "faster at dim d" has two answers depending on what is
	# held constant:
	#   fixed n      -- bytes scanned scale with dim; the realistic shape of an
	#                   index that happens to carry short vectors.
	#   fixed bytes  -- n chosen so the code array is about the same size at every
	#                   dim, holding the memory regime constant so the comparison
	#                   is per-coordinate compute rather than cache residency.
	# At 960-d/1M the scan is ~69 % bandwidth-bound, so one arm alone would
	# confound the two.
	#
	# dim= truncates each loaded vector to its leading d coordinates and
	# renormalizes.  Legitimate rather than a shortcut: the quantizer's rotation
	# makes coordinates exchangeable, the same property V15's prefix stage rests
	# on.  It also means the zero-norm drop applies to the truncated vector, so
	# the kept count is a function of dim -- which is why every row reports its
	# own n.
	#
	# A LIMITATION OF THE CORPUS, NOT THE HOST: GIST has 1M base vectors, so at
	# 64-d the largest possible code array is ~32 MB.  No low-dim point can be
	# made bandwidth-bound with this corpus on any instance, and the fixed-bytes
	# arm cannot reach the low dims at all.  Recorded rather than papered over.
	fetch_gist
	say "building code_scan"
	$SSH 'cd pg_weave && gcc -O2 -march=native -std=gnu99 -I include \
			-o /scratch/code_scan bench/code_scan.c src/vector/quantize.c \
			src/vector/pack.c src/vector/kernels.c -lm && echo built' \
		2>&1 | tee "$OUT/build.log" || die "code_scan build failed"

	# Same gate as the other code_scan jobs, same reason: -march=native here is
	# not -mavx2 on the workstation, so the binary about to produce latencies is
	# not the binary that was checked there.
	say "byte-LUT differential self-check on this host (gate)"
	$SSH 'cd /scratch && ./code_scan selfcheck=2000' \
		2>&1 | tee "$OUT/selfcheck.log" \
		|| die "byte-LUT self-check FAILED on the bench host -- no latency below would mean anything"

	# nq is small on purpose: this job measures ns/vector and the scan is timed
	# per query.  Recall at nq=100 is the csrecall job's business, and mixing the
	# two would make this job an hour longer for numbers already recorded.
	# queries= is NOT optional at n=1M.  Without it the harness holds the query
	# set out of the TAIL of the base file, and GIST has exactly 1,000,000 base
	# vectors, so there is nothing left to hold out and the run dies with "too few
	# query vectors" AFTER paying for the encode.  Learned by doing it.
	CSDIMS=${CSDIMS:-"64 128 240 480 768 960"}
	for dim in $CSDIMS; do
		say "fixed n=${CSDIMN:-1000000}, dim=$dim"
		$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs \
				${CSDIMN:-1000000} ${CSDIMNQ:-10} bits=4 k=10 order=natural \
				kernel=all dim=$dim queries=corpus/gist/gist_query.fvecs" \
			2>&1 | tee -a "$OUT/csdim_fixedn.log" || die "csdim fixed-n dim=$dim failed"
	done
	for dim in $CSDIMS; do
		# 4 bits => dim/2 bytes per vector.  THE DEFAULT IS 30 MB, NOT THE 480 MB
		# of the 960-d/1M point, and the reason is the corpus: 480 MB needs
		# 1,048,576 vectors at 960-d and more at every smaller dim, while GIST has
		# exactly 1,000,000 -- so a 480 MB arm skips EVERY point, which is what the
		# first run of this job did.  30 MB is 1M x 32 B, the largest array
		# reachable at 64-d, hence the only size a sweep spanning 64-960 can hold
		# constant here.  A bandwidth-bound low-dim point is not obtainable from
		# this corpus on any instance.
		n=$(( ${CSDIMMB:-30} * 1048576 / (dim / 2) ))
		if [ "$n" -gt 1000000 ]; then
			say "skipping fixed-bytes dim=$dim: it needs n=$n and the corpus has 1M"
			continue
		fi
		say "fixed ~${CSDIMMB:-480} MB, dim=$dim, n=$n"
		$SSH "cd /scratch && ./code_scan corpus/gist/gist_base.fvecs $n \
				${CSDIMNQ:-10} bits=4 k=10 order=natural kernel=all dim=$dim \
				queries=corpus/gist/gist_query.fvecs" \
			2>&1 | tee -a "$OUT/csdim_fixedbytes.log" || die "csdim fixed-bytes dim=$dim failed"
	done
}

run_p0merge() {
	# A/B for the merge tombstone P0 (bench/RESULTS_P0_MERGE_TOMBSTONE.md).
	#
	# A hang cannot be waited out, so this measures a SCALING CURVE rather than a
	# single number: VACUUM wall-clock at increasing corpus size, which should
	# grow superlinearly before the fix and near-linearly after.  Exposure needs
	# a long sparsemap chunk chain AND millions of term boundaries at the same
	# time, so the corpus is generated with a deliberately huge vocabulary --
	# ~10 tokens per row drawn from a 5M-token space -- rather than natural text,
	# whose Zipf distribution would give far fewer distinct terms per row.
	#
	# The "before" build is produced by reverting the fix commit rather than by
	# checking out an older tree, so the two binaries differ in exactly that
	# commit and nothing else.
	say "p0 merge A/B: preparing both builds"
	# The remote tree comes from `git archive HEAD` followed by a bare `git init`,
	# so it has NO HISTORY: `git revert`/`git log --grep` cannot work there, and an
	# earlier version of this job that used them would have silently measured the
	# same binary twice.  Instead the fix's diff is extracted HERE, where the
	# history exists, uploaded once, and applied in reverse to produce the
	# "before" arm.  The two binaries then differ in exactly that diff.
	P0SHA=$(git -C "$ROOT" log --format=%H --grep='^P0: VACUUM' -1)
	[ -n "$P0SHA" ] || die "cannot find the P0 commit to A/B against"
	git -C "$ROOT" show "$P0SHA" -- src/am/ambuild.c src/am/amvacuum.c \
		> "$OUT/p0.patch" || die "could not extract the P0 diff"
	say "A/B against $(git -C "$ROOT" log --oneline -1 "$P0SHA")"
	$SSH 'cat > /tmp/p0.patch' < "$OUT/p0.patch" || die "patch upload failed"
	# Prove the patch reverses cleanly before spending an hour of instance time.
	$SSH 'cd pg_weave && git apply --reverse --check /tmp/p0.patch && echo "patch reverses cleanly"' \
		2>&1 | tee "$OUT/p0_setup.log" || die "the P0 patch does not reverse against the uploaded tree"

	# Install once up front.  The corpus generation below calls to_wdoc(), which
	# needs the extension present -- an earlier version generated the corpus first
	# and every scale point died on "Could not open extension control file".
	say "p0 merge: installing the extension so the corpus can be built"
	$SSH 'cd pg_weave && make -s PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1
		sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1
		sudo -u postgres pg_ctlcluster 17 main restart 2>/dev/null || true
		psql -q -v ON_ERROR_STOP=1 -c "CREATE EXTENSION IF NOT EXISTS pg_weave"
		echo "extension present:"; psql -tAc "\dx pg_weave" ' \
		2>&1 | tee -a "$OUT/p0_setup.log" || die "extension install failed"

	# Alternate variants at each scale point (skill rule: A/B alternate, never
	# all-A then all-B) so thermal and neighbour drift cannot bias one arm.  One
	# run per arm per point rather than three: the expected effect is a
	# superlinear blow-up, not a few percent, so run-to-run variance is not the
	# limiting factor here -- and an operation that does not terminate has no
	# variance to average in the first place.  If the curves come out close, that
	# is itself the finding and it gets repeated properly.
	for n in 250000 750000 2000000; do
		# Generate the random text ONCE per scale point.  It is the expensive
		# part and it is identical for both arms; only the index build, the
		# delete and the VACUUM have to be repeated per variant.
		say "p0 merge: generating source corpus n=$n"
		$SSH "psql -q -v ON_ERROR_STOP=1 <<SQL
DROP TABLE IF EXISTS p0src;
-- The indexed column must be wdoc, not text: the weave AM has no default
-- operator class for text, and CREATE INDEX ... USING weave (body) on a text
-- column fails with 'data type text has no default operator class'.  Note: no
-- backticks anywhere in these heredocs -- they are inside a double-quoted \$SSH
-- string, so the REMOTE shell treats them as command substitution and emits
-- 'bash: command substitution: ...' into the middle of the SQL.
--
-- High distinct-term count is the point: ~10 tokens/row from a 5M-token space
-- gives millions of term boundaries in the merge, which is one of the two
-- conditions the pathology needs.  Natural text would not: its Zipf
-- distribution yields far fewer distinct terms for the same row count.
--
-- The ten tokens are spelled out rather than built with
--   (SELECT string_agg(...) FROM generate_series(1,10))
-- because that sublink is UNCORRELATED, so PostgreSQL hoists it to an InitPlan
-- and evaluates it ONCE for the whole statement -- volatility of random() does
-- not prevent it.  The first run of this job produced 2,000,000 identical rows
-- and nterms=10, and reported a 3.2 s VACUUM as though it meant something.
-- CREATE TABLE AS, never INSERT ... SELECT -- the latter silently loses the
-- parallel plan (measured 12x upstream in pg_turbovec b34f22c).
CREATE TABLE p0src AS
  SELECT id, body, to_wdoc(body) AS d
    FROM (SELECT i AS id,
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) || ' ' ||
                 't' || ((random() * 5000000)::int) AS body
            FROM generate_series(1, $n) i) s;
SQL" 2>&1 | tail -2 | tee -a "$OUT/p0_merge.log"

		# Assert the corpus is the shape the pathology needs, BEFORE spending a
		# build and a VACUUM on it.  Without this the degenerate corpus produced by
		# the first attempt gave a fast VACUUM that looked like a passing
		# measurement.  The distinct count is taken on the TEXT column, which is why
		# `body` is kept alongside `d`: wdoc has no equality operator, so
		# `count(distinct d)` fails outright.
		NDISTINCT=$($SSH "psql -tAc \"select count(distinct body) from p0src\"")
		say "n=$n distinct documents: $NDISTINCT"
		[ "${NDISTINCT:-0}" -ge $(( n / 2 )) ] \
			|| die "corpus is degenerate ($NDISTINCT distinct rows of $n) -- refusing to measure"

		for variant in fixed before; do
			say "p0 merge: n=$n variant=$variant"
			$SSH "set -e
				cd pg_weave
				if [ '$variant' = before ]; then
					git apply --reverse /tmp/p0.patch
				fi
				make -s PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1
				sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1
				if [ '$variant' = before ]; then
					git apply /tmp/p0.patch   # restore the tree for the next arm
				fi
				sudo -u postgres pg_ctlcluster 17 main restart 2>/dev/null || true
				psql -q -v ON_ERROR_STOP=1 <<SQL
DROP TABLE IF EXISTS p0doc;
CREATE TABLE p0doc AS SELECT * FROM p0src;
CREATE INDEX p0doc_weave ON p0doc USING weave (d);
-- Delete ~15% spread across the whole docid space, so the tombstone sparsemap
-- has a long chunk chain rather than a few dense chunks.
DELETE FROM p0doc WHERE id % 7 = 0;
SQL
				echo \"--- n=$n variant=$variant ---\"
				psql -tAc \"SELECT 'nterms=' || nterms FROM weave_index_stats('p0doc_weave')\" || true
				psql -tAc \"SELECT 'idxsize=' || pg_size_pretty(pg_relation_size('p0doc_weave'))\" || true
				psql -tAc \"SELECT 'tombstones=' || count(*) FROM p0src WHERE id % 7 = 0\" || true
				# A timeout so genuine non-termination reports as a bound instead
				# of hanging the whole run.
				start=\$(date +%s.%N)
				if timeout 3600 psql -q -c 'VACUUM p0doc'; then
					end=\$(date +%s.%N)
					echo \"VACUUM_SECONDS \$(echo \"\$end - \$start\" | bc)\"
				else
					echo 'VACUUM_SECONDS >3600 (timed out)'
				fi" \
				2>&1 | tee -a "$OUT/p0_merge.log"
		done
	done
	say "p0 merge A/B done -- see $OUT/p0_merge.log"
}

run_vecmerge() {
	# Hard rule 12's gate for the VECTOR weft's merge and vacuum paths, which until
	# 2026-09-23 did not exist: no job in this file built a vector-carrying weave
	# index at scale, and no job called weave_check() at all.  See bench/vecmerge.sh
	# for what is asserted and why; this function is the EC2 wrapper plus the
	# mutation control, which has to live here because the host has no git history.
	fetch_gist

	say "vecmerge: clean arm, ${NROWS:-1000000} x 960-d, build + $((${NBATCH:-4})) merges + 3 vacuums"
	# Artifacts are pulled BEFORE the status check, so a failure still yields the
	# per-stage weave_check output that says which invariant broke.
	$SSH "cd pg_weave && OUT=\$HOME/out NROWS=${NROWS:-1000000} \
			BATCH=${BATCH:-50000} NBATCH=${NBATCH:-4} DELFRAC=${DELFRAC:-10} \
			bash bench/vecmerge.sh" 2>&1 | tee "$OUT/vecmerge.log"
	rc=${PIPESTATUS[0]}
	$SSH 'cd ~/out && tar cf - .' | tar xf - -C "$OUT" 2>/dev/null || true
	$SSH 'cd /scratch/vecmerge && tar cf - check.*.tsv' | tar xf - -C "$OUT" 2>/dev/null || true
	# NOT `|| die` HERE, and that is a correction.  The first run of this job failed
	# the clean arm's page ratchet, died at this line, and so never ran the mutation
	# control below -- which is the only thing that makes the eight CLEAN
	# weave_check() results that run did produce mean anything.  A control gated
	# behind the success of the arm it validates is not a control.  The status is
	# carried to the end of the function instead.
	if [ "$rc" != 0 ]; then
		say "vecmerge.sh FAILED (see $OUT/vecmerge.log) -- running the control anyway,
		     because a red clean arm is exactly when you need to know whether the
		     invariant can fire"
	fi

	# ---------------------------------------------------------------- control
	#
	# THE CLEAN ARM ABOVE PROVES NOTHING ON ITS OWN.  weave_check() reporting no
	# violations is exactly what an invariant that cannot fire also reports, and this
	# project has now shipped three gates that reported on something other than the
	# thing under test (AGENTS.md, eleventh and twelfth members).  So: break the
	# writer by one page, rebuild, rebuild the index, and require weave_check() to
	# NAME the violation.
	#
	# `+ 2` rather than `+ 1` deliberately.  A one-page error can land on the
	# block's own second strip page, whose header claims the same block, and the
	# invariant would correctly pass; two pages clears a two-page block.  This is the
	# same mutation the local control uses, so the two are comparable.
	#
	# A SMALLER CORPUS, and that is not a weakening: the mutation is in a per-block
	# store, so every block in the index carries it and the first one found fails the
	# check.  What the small arm cannot tell you is whether the invariant scales,
	# which the clean arm above already answered.
	say "control: mutating the code-page pointer by +2 pages"
	$SSH 'set -e
		cd pg_weave
		sed -i "s/rec\.firstpage = (weave_uint32) BufferGetBlockNumber(codes\.buf);/rec.firstpage = (weave_uint32) BufferGetBlockNumber(codes.buf) + 2;/" \
			src/vector/vecwrite.c
		grep -q "BufferGetBlockNumber(codes.buf) + 2;" src/vector/vecwrite.c \
			|| { echo "MUTATION DID NOT APPLY -- the sed pattern no longer matches"; exit 1; }
		echo "mutation applied:"; grep -n "codes.buf) + 2" src/vector/vecwrite.c' \
		2>&1 | tee "$OUT/mutant_setup.log" || die "could not apply the mutation"

	# ASSERT THE MUTANT BUILT.  A mutation harness that treats "the check did not
	# succeed" as "the mutation was caught" reports a compile error as a pass; that
	# happened here for real on a V7 leg (AGENTS.md).  `with_llvm=no` because PGXS's
	# bitcode step on this image aborts and leaves a half-written bitcode directory
	# that later kills backends in unrelated tests.
	$SSH 'set -e
		cd pg_weave
		make -s PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config with_llvm=no 2>&1 | tail -20
		test -f pg_weave.so || { echo "MUTANT DID NOT BUILD"; exit 1; }
		sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config with_llvm=no >/dev/null
		sudo -u postgres pg_ctlcluster 17 main restart
		echo "mutant built and installed"' \
		2>&1 | tee -a "$OUT/mutant_setup.log" || die "the mutant did not build"

	say "control: the mutated writer must be caught by weave_check()"
	# THE SQL GOES OVER AS A FILE.  Quoting a string literal through a single-quoted
	# ssh argument, a remote shell and psql has already cost this harness two
	# debugging rounds (see the tuning step and run_p0merge's backtick note), and the
	# failure mode here is the worst kind: `"mut doc"` is a valid IDENTIFIER, so a
	# mis-quoted literal is a column-does-not-exist error at best and a silently
	# different test at worst.
	$SSH 'cat > /tmp/mut.sql' <<'MUTSQL'
DROP TABLE IF EXISTS vmut;
CREATE TABLE vmut (id int, d wdoc, v wvec(960));
INSERT INTO vmut SELECT id, to_wdoc('mut doc ' || id), v FROM vmsrc WHERE id <= 200000;
CREATE INDEX vmut_weave ON vmut USING weave (d, v) WITH (metric = 'ip');
\echo === weave_check on the mutant ===
SELECT invariant, ok, detail FROM weave_check('vmut_weave', true) WHERE NOT ok;
MUTSQL
	$SSH 'psql -X -q -v ON_ERROR_STOP=1 -f /tmp/mut.sql' \
		2>&1 | tee "$OUT/mutant_check.log" || true

	# The control passes only if the check FAILED, and only if it failed for the
	# right reason.  `grep firstpage` rather than `grep -c '^f'`: an unrelated
	# violation would satisfy "not clean" while proving nothing about this field.
	if grep -q 'firstpage' "$OUT/mutant_check.log"; then
		say "control PASSED: weave_check() named the firstpage violation"
	else
		cat "$OUT/mutant_check.log" >&2
		die "control FAILED: the mutated writer was NOT caught -- every clean result
		     in this run is therefore uninformative about firstpage"
	fi

	# Now the clean arm's status, after the control has had its say.  Both results are
	# reported so a reader can tell the two apart: "the invariant works and something
	# else is wrong" is a different state from "the invariant is blind".
	[ "$rc" = 0 ] || die "the control passed but the clean arm FAILED -- the firstpage
	                      invariant is demonstrably able to fire, so the failure is in
	                      something else this job measures.  See $OUT/vecmerge.log and
	                      $OUT/vecmerge.tsv"
}

run_gatesweep() {
	# CLAIM 3 OF doc/ARCHITECTURE.md sect. 9, the half that has never been measured:
	# does a fused query get FASTER as the predicate gets more selective?
	#
	# The work half was measured locally on 2026-09-23 (bench/RESULTS_GATE_SWEEP.md)
	# and split in two: CPU work tracks selectivity to three digits, while page
	# traffic was FLAT until G27 stored the code-page pointer, after which it falls
	# 2.04x on scifact and 4.79x on fiqa at 0.1 %.  Counts are host-independent so
	# that needed no instance.  Latency is not, and latency is what the claim is
	# about -- a user does not feel a pivot count.
	#
	# WHAT WOULD MAKE THIS RUN WORTHLESS, and what stops it: a point whose plan has
	# no `Index Cond:` measures the executor filtering rows and draws a beautiful
	# flat curve (gatesweep.sh dies on it); a single drifting point read as the curve
	# (every point is measured twice, in non-adjacent slots, and the A/A spread is
	# reported beside the ratio); and a corpus built differently from the one the work
	# pass used (both go through fuse.sh's loader, now reachable as FUSE_LOAD_ONLY).
	ensure_embedder

	local datasets=${FUSE_DATASETS:-"scifact nfcorpus fiqa"}
	local limit=${FUSE_LIMIT:-0}
	local bench=/scratch/bench

	for D in $datasets; do
		say "gatesweep: preparing $D (embed=minilm, limit=$limit)"
		$SSH "cd pg_weave && mkdir -p $bench && \
			  /scratch/venv/bin/python3 bench/prepdata.py --dataset $D \
				--out $bench --embed minilm --limit \"$limit\"" \
			2>&1 | tee "$OUT/gs-$D-prep.log" \
			|| die "prepdata.py failed for $D (see $OUT/gs-$D-prep.log)"

		# One database per dataset, so the sweep can be re-run against any of them
		# afterwards without reloading -- and so a failure on the third dataset does
		# not take the first two down with it.
		say "gatesweep: loading $D"
		$SSH "cd pg_weave && PGDATABASE=gs_$D FUSE_LOAD_ONLY=1 \
			  bash bench/fuse.sh $bench $D" \
			2>&1 | tee "$OUT/gs-$D-load.log" \
			|| die "fuse.sh load failed for $D (see $OUT/gs-$D-load.log)"
		grep -q 'LOAD ONLY' "$OUT/gs-$D-load.log" \
			|| die "$D: fuse.sh did not take the load-only path -- it may have run the
			        whole benchmark, or the corpus is not loaded"

		say "gatesweep: sweeping $D (LAT=1, ${LATN:-25} queries x ${REPS:-7} reps x 2 slots)"
		$SSH "cd pg_weave && DBS=gs_$D K=${K:-10} NQ=${GSNQ:-0} LAT=1 \
			  LATN=${LATN:-25} REPS=${REPS:-7} OUT=/scratch/gatesweep TAG=$D \
			  bash bench/gatesweep.sh" \
			2>&1 | tee "$OUT/gs-$D-sweep.log"
		rc=${PIPESTATUS[0]}
		# Pulled per dataset (hard rule 14), and BEFORE the status check so a point
		# that died still leaves the points that succeeded on local disk.
		for f in gatesweep-$D.tsv gatesweep-lat-$D.tsv; do
			$SSH "cat /scratch/gatesweep/$f" > "$OUT/$f" 2>/dev/null \
				|| say "gatesweep: could not pull $f for $D"
		done
		[ "$rc" = 0 ] || die "gatesweep.sh failed for $D (see $OUT/gs-$D-sweep.log)"
	done
}

run_bound() {
	# The measurement from bench/RESULTS_BOUND_PRUNING.md, on real hardware and
	# over more dimensions than a laptop run covers.  This is the number the
	# fused-scorer design depends on (doc/specs/FUSED_TOPK.md sect. 8).
	say "block-bound pruning sweep"
	$SSH 'cd pg_weave && gcc -O2 -march=native -I include -o /tmp/bp \
			bench/bound_pruning.c src/vector/quantize.c src/vector/pack.c -lm
		  echo "=== coherent warp order ==="; /tmp/bp 1
		  echo "=== random warp order ===";   /tmp/bp 0' \
		2>&1 | tee "$OUT/bound_pruning.log"
}

case "$JOB" in
	smoke)   run_smoke ;;
	bound)   run_bound ;;
	lexical) run_smoke; run_lexical ;;
	fuzzy)   run_smoke; run_fuzzy ;;
	bitsweep) run_bitsweep ;;
	p0merge) run_p0merge ;;
	vall)    run_bitsweep; run_p0merge ;;
	winsweep)   run_winsweep ;;
	codescan)   run_codescan ;;
	csrecall)   run_csrecall ;;
	csdim)      run_csdim ;;
	rerankcold) run_winsweep; run_rerankcold ;;
	hnswbase)   run_hnswbase ;;
	# run_smoke FIRST, exactly as the lexical and fuzzy jobs do, and it is not
	# optional: run_smoke is where `sudo make install` happens, so without it
	# `CREATE EXTENSION pg_weave` fails on the instance and the whole run dies
	# after paying for the launch, the torch install and the corpus download.
	# It also means the host has passed regression + isolation + TAP before any
	# number is taken off it, which is this project's own rule about correctness
	# preceding latency applied to the machine rather than to the code.
	fuse)       run_smoke; run_fuse ;;
	# run_smoke first for the same reason: it is where `sudo make install` happens,
	# and vecmerge's whole output is weave_check(), which needs the extension.
	vecmerge)   run_smoke; run_vecmerge ;;
	# gatesweep needs the extension installed (run_smoke) and, unlike the work pass
	# that runs on the workstation for free, a quiet machine -- it is the only
	# latency measurement claim 3 has.
	gatesweep)  run_smoke; run_gatesweep ;;
	all)     run_smoke; run_bound; run_lexical ;;
	*)     die "unknown job: $JOB" ;;
esac

say "done -- artifacts in $OUT"
# cleanup() runs from the EXIT trap.
