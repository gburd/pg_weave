#!/bin/bash
# doc/GAPS.md G73 EC2 job: bench/aws/g73_repro.pl, G73N clusters per C arm.
#   G73_CARMS  C arms (default "base"); each is a copy of the tree with ONE
#              exact-once substitution (apply() below), built and installed,
#              with a distinct .so md5 required
#   G73_ARMS   test-side arms cycled per round inside the reproducer
#   G73N       reproducer runs per C arm (default 4), G73_ROUNDS rounds each
# Logs: /tmp/out/<carm>-<n>/ (TAP log, regress log); summary /tmp/out/g73.log.
set -u
OUT=/tmp/out; mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
CARMS="${G73_CARMS:-base}"
N="${G73N:-4}"
log() { echo "$(date +%T) $*" | tee -a $OUT/g73.log; }

apply() {	# arm -> substitution in the current dir
	local f=src/am/am.c from to
	case $1 in
	base) return 0 ;;
	*)
		if [ -n "${ARM_FROM_FILE:-}" ] && [ "$1" = "${ARM_NAME:-}" ]; then
			f=${ARM_FILE:-src/am/am.c}; from=$(cat "$ARM_FROM_FILE"); to=$(cat "$ARM_TO_FILE")
		else
			echo "unknown arm $1"; return 2
		fi ;;
	esac
	FROM="$from" TO="$to" perl -0pi -e '
		my $f = quotemeta($ENV{FROM}); my $n = () = /$f/g;
		die "arm: pattern matched $n times in $ARGV\n" unless $n == 1;
		my $t = $ENV{TO}; s/$f/$t/;' $f || return 1
	echo "$f: $(grep -c 'ARM' $f) ARM line(s)"
}

install_tree() {
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}

ok=1
declare -A MD5
for arm in $CARMS; do
	D=/tmp/arm-$arm
	[ -d $D ] && find $D -depth -delete
	cp -a $SRC $D
	if ! (cd $D && apply $arm > $OUT/apply-$arm.log 2>&1); then
		log "$arm: DID NOT APPLY ($(cat $OUT/apply-$arm.log))"; ok=0; continue
	fi
	if ! install_tree $D $arm; then
		log "$arm: DID NOT BUILD ($(grep -m3 error $OUT/build-$arm.log))"; ok=0; continue
	fi
	md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	for a in "${!MD5[@]}"; do
		[ "${MD5[$a]}" = "$md5" ] && { log "$arm: .so identical to $a's -- arm not counted"; ok=0; }
	done
	MD5[$arm]=$md5
	for r in $(seq 1 $N); do
		tag=$arm-$r
		[ -d $D/tmp_check ] && find $D/tmp_check -depth -delete
		(cd $D && make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
			PROVE_TESTS=bench/aws/g73_repro.pl > $OUT/tap-$tag.log 2>&1)
		rc=$?
		mkdir -p $OUT/$tag
		cp $D/tmp_check/log/* $OUT/$tag/ 2>/dev/null
		nr=$(cat $OUT/$tag/regress_log_* 2>/dev/null | grep -c 'G73R ')
		log "$tag so=$md5 rc=$rc rounds=$nr $(cat $OUT/$tag/regress_log_* 2>/dev/null | grep 'G73T ' | sed 's/^# //' | tr '\n' ' ')"
		[ "$nr" -gt 0 ] || { log "$tag: RAN_NOTHING"; ok=0; }
	done
	find $D -depth -delete
done
log "TOTALS:"
cat $OUT/*/regress_log_* 2>/dev/null | grep 'G73R ' | sed 's/.*arm=\([a-z]*\).*result=\([A-Z]*\).*/\1 \2/' | sort | uniq -c | tee -a $OUT/g73.log
log "DONE ok=$ok"
[ $ok = 1 ]
