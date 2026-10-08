#!/bin/bash
# L23 EC2 job (doc/PHASES.md L23): after the PG17 smoke, t/035 and the merge/vacuum
# TAP files (t/014, t/029-t/033) twice each on the clean install, then mutants of
# the merge pre-flight, each of which must BUILD, install a different .so, and
# FAIL t/035 before it counts.  Then PG18: t/035.  Results under /tmp/out.
set -u
OUT=/tmp/out
mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
PHASES="${PHASES:-smokelogs control mutants pg18}"
MUTS="${MUTS:-noflag nocount publish noskip}"
log() { echo "$(date +%T) $*" | tee -a $OUT/l23.log; }
wipe() { [ -e "$1" ] && find "$1" -depth -delete; true; }

cat > $OUT/apply.sh <<'MUTEOF'
#!/bin/bash
set -e
m=$1
sub() {	# file, from (literal), to (literal): must match exactly once
	FROM="$2" TO="$3" perl -0pi -e '
		my $f = quotemeta($ENV{FROM}); my $n = () = /$f/g;
		die "mutant: pattern matched $n times in $ARGV\n" unless $n == 1;
		my $t = $ENV{TO}; s/$f/$t/;' "$1"
}
case $m in
noflag)		# the per-page FREED test in the chain walk (dict_freed, post_freed)
	sub src/am/amcheck.c '		if (WeavePageIsFreed(page))
		{
			UnlockReleaseBuffer(buf);' '		if (false && WeavePageIsFreed(page))	/* MUTANT noflag */
		{
			UnlockReleaseBuffer(buf);' ;;
nocount)	# the dictionary term count against nterms (dict_cut)
	sub src/am/amcheck.c '	if (nent != (uint64) seg->nterms)' '	if (false && nent != (uint64) seg->nterms)	/* MUTANT nocount */' ;;
publish)	# the merge ignores the pre-flight's verdict
	sub src/am/ambuild.c '		if (why == NULL)
			continue;' '		if (true)	/* MUTANT publish */
			continue;' ;;
noskip)		# a damaged input stops the level instead of being left out
	sub src/am/ambuild.c '	sk->seg[sk->n++] = meta->segs[sel[damaged]];
	return true;' '	sk->seg[sk->n++] = meta->segs[sel[damaged]];
	return false;	/* MUTANT noskip */' ;;
*) echo "unknown mutant $m"; exit 2 ;;
esac
grep -rc MUTANT src/am/amcheck.c src/am/ambuild.c
MUTEOF
chmod +x $OUT/apply.sh

install_tree() {	# dir tag [pgconfig] -> 0 on success
	local pgc=${3:-$PGC}
	(cd "$1" && make clean PG_CONFIG=$pgc >/dev/null 2>&1 &&
	 make PG_CONFIG=$pgc with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$pgc with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $($pgc --pkglibdir)/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}

# run_tap dir tag pgconfig tests... -> prove's own summary, or RAN_NOTHING
run_tap() {
	local d=$1 tag=$2 pgc=$3; shift 3
	(cd "$d" && make installcheck PG_CONFIG=$pgc REGRESS= ISOLATION= \
		PROVE_TESTS="$*" > $OUT/tap-$tag.log 2>&1)
	local rc=$?
	mkdir -p $OUT/taplog-$tag
	cp "$d"/tmp_check/log/regress_log_* $OUT/taplog-$tag/ 2>/dev/null
	if ! grep -q '^Result: ' $OUT/tap-$tag.log; then echo RAN_NOTHING; return; fi
	echo "rc=$rc $(grep -E '^Result: ' $OUT/tap-$tag.log) $(grep -E '^Files=' $OUT/tap-$tag.log | cut -d, -f1-2) $(cat $OUT/taplog-$tag/* 2>/dev/null | grep -cE '^(not )?ok ') asserts, $(cat $OUT/taplog-$tag/* 2>/dev/null | grep -cE '^not ok') not_ok"
}

ok=1
CLEAN_MD5=

if [[ " $PHASES " == *" smokelogs "* ]]; then
	mkdir -p $OUT/smoke-taplog
	cp $SRC/tmp_check/log/* $OUT/smoke-taplog/ 2>/dev/null
	log "SMOKELOGS: copied $(ls $OUT/smoke-taplog | wc -l) files"
fi

if [[ " $PHASES " == *" control "* ]]; then
	install_tree "$SRC" clean || { log "CONTROL build/install FAILED: $(grep -m5 error $OUT/build-clean.log)"; exit 1; }
	CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	for r in 1 2; do
		res=$(run_tap "$SRC" control$r $PGC t/035_merge_freed_page.pl t/014_merge_durability.pl \
			t/029_flush_atomic.pl t/030_bulkdelete_atomic.pl t/031_doclist_atomic.pl \
			t/032_reclaim_concurrent.pl t/033_reclaim_crash_loop.pl)
		log "CONTROL run $r (so=$CLEAN_MD5): $res"
		case "$res" in *"Result: PASS"*) ;; *) ok=0 ;; esac
	done
fi

if [[ " $PHASES " == *" mutants "* ]]; then
	[ -n "$CLEAN_MD5" ] || { install_tree "$SRC" clean; CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1); }
	for m in $MUTS; do
		D=/tmp/mut-$m
		wipe $D; cp -a $SRC $D
		if ! (cd $D && bash $OUT/apply.sh $m > $OUT/apply-$m.log 2>&1); then
			log "$m: DID NOT APPLY ($(cat $OUT/apply-$m.log)) -- not counted"; ok=0; continue
		fi
		if ! install_tree $D $m; then
			log "$m: DID NOT BUILD/INSTALL -- not counted ($(grep -m3 error $OUT/build-$m.log))"; ok=0; continue
		fi
		md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
		if [ "$md5" = "$CLEAN_MD5" ]; then log "$m: installed .so is the CLEAN one -- not counted"; ok=0; continue; fi
		res=$(run_tap $D mut-$m $PGC t/035_merge_freed_page.pl)
		fails=$(grep -hE '^not ok' $OUT/taplog-mut-$m/* 2>/dev/null | head -4 | tr '\n' ' ')
		case "$res" in
		RAN_NOTHING) log "$m: the TAP run printed no Result -- not counted"; ok=0 ;;
		*"Result: FAIL"*) log "$m: CAUGHT (so=$md5): $res :: $fails" ;;
		*) log "$m: SURVIVED (so=$md5): $res"; ok=0 ;;
		esac
		wipe $D
	done
	install_tree "$SRC" clean-restore > /dev/null 2>&1
	[ "$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)" = "$CLEAN_MD5" ] || log "WARNING: clean .so not restored"
fi

if [[ " $PHASES " == *" pg18 "* ]]; then
	sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q postgresql-18 postgresql-server-dev-18 \
		> $OUT/apt18.log 2>&1 || { log "PG18 apt FAILED: $(tail -3 $OUT/apt18.log)"; exit 1; }
	PGC18=/usr/lib/postgresql/18/bin/pg_config
	D=/tmp/pg18tree; wipe $D; cp -a $SRC $D
	install_tree $D pg18 $PGC18 || { log "PG18 build/install FAILED: $(grep -m5 error $OUT/build-pg18.log)"; exit 1; }
	res=$(run_tap $D pg18 $PGC18 t/035_merge_freed_page.pl)
	log "PG18 t/035: $res :: $($PGC18 --version)"
	case "$res" in *"Result: PASS"*) ;; *) ok=0 ;; esac
fi

log "DONE ok=$ok"
[ $ok = 1 ]
