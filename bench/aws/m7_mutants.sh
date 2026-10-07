#!/bin/bash
# M7 step 2 mutation run.  For each mutant: fresh copy of the shipped tree, apply
# (each substitution must match exactly once), make clean + build, install, check
# the installed .so differs from the clean one, run tsvector_input, and only then
# count a diff as a catch.  A CONTROL (clean tree, same commands) must show 0.
mkdir -p /tmp/out
cat > /tmp/out/mutants.sh <<'MUTEOF'
#!/bin/bash
set -e
m=$1
sub() {	# file, from (literal), to (literal)
	FROM="$2" TO="$3" perl -0pi -e '
		my $f = quotemeta($ENV{FROM}); my $n = () = /$f/g;
		die "mutant: pattern matched $n times in $ARGV\n" unless $n == 1;
		my $t = $ENV{TO}; s/$f/$t/;' "$1"
}
case $m in
M1_mixed_drops_positions)
	sub src/query/tsanalyze.c '		if (np > 0)
			has_pos = true;
		npos' '		if (np <= 0)
			has_pos = false;
		npos'
	sub src/query/tsanalyze.c '	/* first pass: positions-on unless every entry is positionless */
	has_pos = false;' '	has_pos = true;' ;;
M2_doclen_counts_stopwords)
	sub src/query/tsanalyze.c 'positions, nlexpos,' 'positions, ntok,' ;;
M3_warning_count_off_by_one)
	sub src/am/ambuild.c '						bs.tsv_positionless, RelationGetRelationName(index)),' \
		'						bs.tsv_positionless + 1, RelationGetRelationName(index)),' ;;
M4_stat_not_decremented)
	sub src/am/amscan.c '	scan = table_beginscan(heap, GetActiveSnapshot(), 0, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		if (pred == NULL || ExecQual(pred, econtext))' \
		'	scan = table_beginscan(heap, SnapshotAny, 0, NULL);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		if (pred == NULL || ExecQual(pred, econtext))' ;;
M5_parallel_sum_dropped)
	sub src/am/ambuild.c '	*tsv_positionless += weaveleader->weaveshared->tsv_positionless;' \
		'	(void) tsv_positionless;' ;;
M6_unknown_pos_adjacent)
	sub src/query/match.c '	while (li < nleft && WEAVE_POS_ORD(left[li]) == WEAVE_POS_UNKNOWN)
		li++;' '' ;;
*) echo "unknown mutant $m"; exit 2 ;;
esac
echo "applied $m"
MUTEOF
chmod +x /tmp/out/mutants.sh
set -u
OUT=/tmp/out
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
T=tsvector_input
MUTS="${MUTS:-M1_mixed_drops_positions M2_doclen_counts_stopwords M3_warning_count_off_by_one M4_stat_not_decremented M5_parallel_sum_dropped M6_unknown_pos_adjacent}"
NOTICE='NOTICE:  extension "pg_weave" already exists, skipping'
log() { echo "$(date +%T) $*" | tee -a $OUT/mutants.log; }
wipe() { [ -d "$1" ] && find "$1" -depth -delete; true; }
install_tree() {
	(cd "$1" && make clean >/dev/null 2>&1 &&
	 make PG_CONFIG=$PGC with_llvm=no > $OUT/build-$2.log 2>&1 &&
	 sudo make install PG_CONFIG=$PGC with_llvm=no > $OUT/install-$2.log 2>&1) || return 1
	sudo find $LIB/bitcode -maxdepth 1 -name 'pg_weave*' -exec find {} -depth -delete \; 2>/dev/null
	return 0
}
run_t() {
	(cd "$1" && find results -name "$T.out" -delete 2>/dev/null;
	 make installcheck PG_CONFIG=$PGC REGRESS=$T ISOLATION= TAP_TESTS= > $OUT/ic-$2.log 2>&1)
	if [ ! -s "$1/results/$T.out" ]; then echo RAN_NOTHING; return; fi
	cp "$1/results/$T.out" $OUT/$T-$2.out
	diff <(grep -vF "$NOTICE" "$1/expected/$T.out") <(grep -vF "$NOTICE" "$1/results/$T.out") > $OUT/$T-$2.diff
	wc -l < $OUT/$T-$2.diff
}
install_tree "$SRC" clean || { log "CONTROL build/install FAILED"; exit 1; }
CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
c=$(run_t "$SRC" clean)
log "CONTROL clean tree: so=$CLEAN_MD5 diff_lines=$c (must be 0)"
[ "$c" = 0 ] || { log "CONTROL FAILED"; exit 1; }
caught=0; total=0
for m in $MUTS; do
	total=$((total+1))
	D=/tmp/mut-$m
	wipe $D; cp -a $SRC $D
	if ! (cd $D && bash /tmp/out/mutants.sh $m > $OUT/apply-$m.log 2>&1); then
		log "$m: DID NOT APPLY ($(cat $OUT/apply-$m.log))"; continue
	fi
	if ! install_tree $D $m; then
		log "$m: DID NOT BUILD/INSTALL -- not counted ($(grep -m3 'error' $OUT/build-$m.log))"; continue
	fi
	md5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
	if [ "$md5" = "$CLEAN_MD5" ]; then log "$m: installed .so is the CLEAN one -- not counted"; continue; fi
	n=$(run_t $D $m)
	if [ "$n" = RAN_NOTHING ]; then log "$m: no results -- not counted"; continue; fi
	if [ "$n" -gt 0 ]; then caught=$((caught+1)); log "$m: BUILT (so=$md5) and CAUGHT ($n diff lines)"
	else log "$m: BUILT (so=$md5) and SURVIVED"; fi
	wipe $D
done
install_tree "$SRC" clean-restore >/dev/null 2>&1
log "DONE caught=$caught of $total"
[ $caught = $total ]
