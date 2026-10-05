#!/bin/bash
mkdir -p /tmp/out
cat > /tmp/out/mutants.sh <<'MUTEOF'
#!/bin/bash
# Apply mutant $1 to the tree in the cwd.  Each substitution must match exactly
# once, or the mutant is refused (a mutant that silently did not apply is the
# harness member AGENTS.md warns about).
set -e
m=$1
sub() {	# file, from (literal), to (literal)
	FROM="$2" TO="$3" perl -0pi -e '
		my $f = quotemeta($ENV{FROM}); my $n = () = /$f/g;
		die "mutant: pattern matched $n times in $ARGV\n" unless $n == 1;
		my $t = $ENV{TO}; s/$f/$t/;' "$1"
}
case $m in
M1_bulkdelete_postings)
	sub src/am/amvacuum.c '		weave_segment_docset(index, sg, &ds);' \
		'		memset(&ds, 0, sizeof(ds)); weave_segment_posting_docids(index, sg, &ds.ids, &ds.n);' ;;
M2_universe_postings)
	sub src/am/amscan.c '	if (root == InvalidBlockNumber)
		return weave_universe_bounded(index, seg->dictstart, seg->ndocs,' \
		'	root = InvalidBlockNumber;
	if (root == InvalidBlockNumber)
		return weave_universe_bounded(index, seg->dictstart, seg->ndocs,' ;;
M3_build_skips_nulldocs)
	sub src/am/ambuild.c '	nulldoc = isnull[lexidx];' \
		'	nulldoc = isnull[lexidx];
	if (nulldoc)
		return;' ;;
M4_universe_ignores_null_bit)
	sub src/am/amscan.c '			continue;			/* a NULL document matches no `@@@` */' \
		'			;			/* MUTANT: NULL bit ignored */' ;;
*) echo "unknown mutant $m"; exit 2 ;;
esac
echo "applied $m"
MUTEOF
chmod +x /tmp/out/mutants.sh
# Mutation run for the doclist work.  For each mutant: fresh copy of the shipped
# tree, apply, make clean + build, install, run the doclist regression test, and
# record the evidence that each step happened -- the build's exit status, the
# installed .so's checksum differing from the clean one, the results file
# existing -- before counting a diff as a catch.  A CONTROL (the clean tree,
# same commands, same filter) must show no diff, else the filter is wrong.
set -u
OUT=/tmp/out
mkdir -p $OUT
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
MUTS="${MUTS:-M1_bulkdelete_postings M2_universe_postings M3_build_skips_nulldocs M4_universe_ignores_null_bit}"
NOTICE='NOTICE:  extension "pg_weave" already exists, skipping'
log() { echo "$(date +%T) $*" | tee -a $OUT/mutants.log; }

install_tree() {	# dir -> 0 on success
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $($PGC --pkglibdir)/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}
run_doclist() {	# dir, tag -> prints the number of differing lines
	(cd "$1" && rm -f results/doclist.out &&
	 make installcheck PG_CONFIG=$PGC REGRESS=doclist > $OUT/ic-$2.log 2>&1)
	if [ ! -s "$1/results/doclist.out" ]; then echo RAN_NOTHING; return; fi
	cp "$1/results/doclist.out" $OUT/doclist-$2.out
	diff <(grep -vF "$NOTICE" "$1/expected/doclist.out") \
	     <(grep -vF "$NOTICE" "$1/results/doclist.out") > $OUT/doclist-$2.diff
	wc -l < $OUT/doclist-$2.diff
}

# control: the clean tree
install_tree "$SRC" clean || { log "CONTROL build/install FAILED"; exit 1; }
CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
c=$(run_doclist "$SRC" clean)
log "CONTROL clean tree: so=$CLEAN_MD5 diff_lines=$c (must be 0)"
[ "$c" = 0 ] || { log "CONTROL FAILED: the filter or the expected file is wrong"; exit 1; }

caught=0; total=0
for m in $MUTS; do
	total=$((total+1))
	D=/tmp/mut-$m
	rm -rf $D; cp -a $SRC $D
	if ! (cd $D && bash /tmp/out/mutants.sh $m > $OUT/apply-$m.log 2>&1); then
		log "$m: DID NOT APPLY ($(cat $OUT/apply-$m.log))"; continue
	fi
	if ! install_tree $D $m; then
		log "$m: DID NOT BUILD/INSTALL -- not counted ($(grep -m3 'error' $OUT/build-$m.log))"; continue
	fi
	md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	if [ "$md5" = "$CLEAN_MD5" ]; then log "$m: installed .so is the CLEAN one -- not counted"; continue; fi
	n=$(run_doclist $D $m)
	if [ "$n" = RAN_NOTHING ]; then log "$m: the test produced no results -- not counted"; continue; fi
	if [ "$n" -gt 0 ]; then caught=$((caught+1)); log "$m: CAUGHT (so=$md5, $n diff lines)"
	else log "$m: SURVIVED (so=$md5)"; fi
	rm -rf $D
done
install_tree "$SRC" clean-restore >/dev/null 2>&1
log "DONE caught=$caught of $total"

# G75 on this branch: t/029 alone, N times, keeping the diag of every failure
G75N=${G75N:-20}; g75fail=0; g75ran=0
for r in $(seq 1 $G75N); do
	(cd $SRC && make installcheck PG_CONFIG=$PGC REGRESS= ISOLATION= \
	    PROVE_TESTS=t/029_flush_atomic.pl > $OUT/g75-run$r.log 2>&1)
	rc=$?
	if grep -q 'ok 7 - ndocs still equals the heap' $SRC/tmp_check/log/regress_log_029_flush_atomic 2>/dev/null; then
		g75ran=$((g75ran+1))
	fi
	if [ $rc != 0 ]; then
		g75fail=$((g75fail+1))
		cp $SRC/tmp_check/log/regress_log_029_flush_atomic $OUT/g75-fail$r.regress_log 2>/dev/null
	fi
done
log "G75 t/029 x$G75N: ran_to_completion=$g75ran failed=$g75fail"
[ $caught = $total ]
