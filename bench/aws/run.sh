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
#					  all	   all of the above
#					  winsweep   rerank window at n=1M, the Phase V frontier's
#							     fragile number (CPU only, no server)
#					  rerankcold winsweep, then cold p50 of a heap rerank with
#							     the real wvec type (bench/rerank_cold.sh)
#					  hnswbase   the pgvector HNSW baseline: bytes/vector,
#							     recall@10 vs ef, warm+cold p50. SEPARATE HOST
#							     from rerankcold -- one engine per host.
#
#	 NDOCS / VOCAB environment variables size the lexical corpus (default 1M /
#	 200k).  A 1M-document run takes a few minutes to generate.
#
# The AWS profile is `lava`.  Everything this script creates is tagged
# Project=pg_weave and named with the run id, so a stray is identifiable.
#
# TERMINATION IS NOT OPTIONAL.  The trap fires on EXIT, which covers success,
# failure, and Ctrl-C.  A forgotten bare-metal instance costs more than any
# benchmark is worth.  The script verifies the shutdown actually happened rather
# than assuming the API call worked.
#
set -uo pipefail

PROFILE=${AWS_PROFILE:-lava}
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
	$SSH 'cd pg_weave && sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config >/dev/null 2>&1
		  sudo -u postgres pg_ctlcluster 17 main start 2>/dev/null || true
		  sudo -u postgres createuser -s ubuntu 2>/dev/null || true
		  make installcheck PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config 2>&1 | tail -30' \
		| tee "$OUT/installcheck.log"
	$SSH 'cd pg_weave && cat regression.diffs 2>/dev/null | head -60' > "$OUT/regression.diffs" 2>/dev/null

	# No separate TAP step: TAP_TESTS = 1 in the Makefile means `make
	# installcheck` already ran t/*.pl above, and PGXS exposes no
	# prove_installcheck target to invoke them again.  The installcheck log holds
	# the TAP results.
	grep -E '^(t/|All tests|Result:|Files=)' "$OUT/installcheck.log" \
		> "$OUT/tap.log" 2>/dev/null || true
}

run_lexical() {
	# The comparison that decides adoption: pg_weave against the tsvector+GIN
	# baseline every PostgreSQL user already has, on the same host and the same
	# stored analyzed column.  Correctness is gated before any timing.
	say "lexical benchmark vs tsvector+GIN"
	$SSH "cd pg_weave && sudo -u postgres createuser -s ubuntu 2>/dev/null; \
		  export PATH=/usr/lib/postgresql/17/bin:\$PATH PGDATABASE=weavebench; \
		  bash bench/lexical.sh ${NDOCS:-1000000} ${VOCAB:-200000} 7" \
		2>&1 | tee "$OUT/lexical.log"
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
	$SSH "cd pg_weave && OUT=\$HOME/out NROWS=${NROWS:-1000000} NSAMP=${NSAMP:-25} \
			bash bench/rerank_cold.sh" 2>&1 | tee "$OUT/rerankcold.log"
	$SSH 'cd ~/out && tar cf - .' | tar xf - -C "$OUT" 2>/dev/null || true
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
	$SSH 'cd ~/out && tar cf - .' | tar xf - -C "$OUT" 2>/dev/null || true
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
	bitsweep) run_bitsweep ;;
	p0merge) run_p0merge ;;
	vall)    run_bitsweep; run_p0merge ;;
	winsweep)   run_winsweep ;;
	rerankcold) run_winsweep; run_rerankcold ;;
	hnswbase)   run_hnswbase ;;
	all)     run_smoke; run_bound; run_lexical ;;
	*)     die "unknown job: $JOB" ;;
esac

say "done -- artifacts in $OUT"
# cleanup() runs from the EXIT trap.
