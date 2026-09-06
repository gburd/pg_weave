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
#					  smoke	  build + regression + isolation + TAP + codec test
#					  bound	  the block-bound pruning sweep (bench/bound_pruning.c)
#					  all	   both
#
# The AWS profile is `bene`.  Everything this script creates is tagged
# Project=pg_weave and named with the run id, so a stray is identifiable.
#
# TERMINATION IS NOT OPTIONAL.  The trap fires on EXIT, which covers success,
# failure, and Ctrl-C.  A forgotten bare-metal instance costs more than any
# benchmark is worth.  The script verifies the shutdown actually happened rather
# than assuming the API call worked.
#
set -uo pipefail

PROFILE=${AWS_PROFILE:-bene}
ITYPE=${1:-c7i.4xlarge}
JOB=${2:-smoke}
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
	--block-device-mappings 'DeviceName=/dev/sda1,Ebs={VolumeSize=80,VolumeType=gp3,DeleteOnTermination=true}' \
	--instance-initiated-shutdown-behavior terminate \
	--tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$RUN},{Key=Project,Value=pg_weave}]" \
	--query 'Instances[0].InstanceId' --output text) || die "run-instances"
say "launched $IID"

aws ec2 wait instance-running --profile "$PROFILE" --instance-ids "$IID" || die "instance never ran"
HOST=$(aws ec2 describe-instances --profile "$PROFILE" --instance-ids "$IID" \
	--query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
say "host $HOST"

SSH="ssh -i $KEYFILE -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
	-o ConnectTimeout=10 -o LogLevel=ERROR ubuntu@$HOST"
for i in $(seq 1 40); do
	$SSH true 2>/dev/null && break
	[ "$i" = 40 ] && die "ssh never came up"
	sleep 10
done
say "ssh up"

# ---------------------------------------------------------------------------
say "provisioning"
$SSH 'sudo DEBIAN_FRONTEND=noninteractive apt-get -qq update && \
	sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y \
		build-essential git postgresql-17 postgresql-server-dev-17 \
		libipc-run-perl clang lld pkg-config >/dev/null 2>&1; \
	pg_config --version' 2>&1 | tail -3 | tee "$OUT/provision.log"

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
	$SSH 'cd pg_weave && make -s PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config 2>&1 | tail -20' \
		| tee "$OUT/build.log"
	$SSH 'test -f pg_weave/pg_weave.so' || die "build produced no shared library"

	say "lint gates"
	$SSH 'cd pg_weave && for t in check-ascii check-alloc check-unity check-rename; do
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

	say "TAP"
	$SSH 'cd pg_weave && PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config \
			make prove_installcheck 2>&1 | tail -20' | tee "$OUT/tap.log" || true
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
	smoke) run_smoke ;;
	bound) run_bound ;;
	all)   run_smoke; run_bound ;;
	*)     die "unknown job: $JOB" ;;
esac

say "done -- artifacts in $OUT"
# cleanup() runs from the EXIT trap.
