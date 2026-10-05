#!/bin/bash
# doclist scale gate (hard rule 12): 1M rows with zero-term and NULL documents,
# then DELETE / VACUUM / INSERT cycles that recycle ctids, comparing the index's
# answers with the heap's after every phase.  Run from ~/pg_weave by
# bench/aws/run.sh script; results under /tmp/out.
set -u
OUT=/tmp/out
mkdir -p $OUT
N=${DL_N:-1000000}
CYCLES=${DL_CYCLES:-4}
PSQL="psql -X -v ON_ERROR_STOP=1 -d postgres"
log() { echo "$(date +%T) $*" | tee -a $OUT/progress.log; }

$PSQL > $OUT/setup.log 2>&1 <<SQL
DROP TABLE IF EXISTS ds;
CREATE EXTENSION IF NOT EXISTS pg_weave;
CREATE TABLE ds (id bigint, body wdoc, emb wvec(4), price int8) WITH (autovacuum_enabled = off);
INSERT INTO ds SELECT g,
       CASE WHEN g % 97 = 0 THEN NULL
            WHEN g % 89 = 0 THEN to_wdoc('simple', '')
            ELSE to_wdoc('simple', 'common w' || (g % 5000) || ' x' || (g % 13)) END,
       CASE WHEN g % 7 = 0 THEN NULL
            ELSE ('[' || (g % 1000) || ',' || (g % 777) || ',' || (g % 555) || ',' || (g % 333) || ']')::wvec END,
       g % 1000
  FROM generate_series(1, $N) g;
SET maintenance_work_mem = '1GB';
CREATE INDEX ds_w ON ds USING weave (body, emb, price int8_docval_ops);
ANALYZE ds;
SQL
rc=$?; log "setup rc=$rc"; [ $rc = 0 ] || { cat $OUT/setup.log; exit 1; }

# One comparison: index arm and heap arm, as a hash of the sorted id set (and the
# count), so a 1M-row set is compared exactly without shipping it.
check() {
	local phase=$1 fail=0
	for q in "price < 3" "body @@@ '!common'" "body @@@ 'x3 & !w7'" "price < 3 AND body @@@ '!x5'"; do
		for arm in heap index bitmap; do
			case $arm in
			heap)   g="SET enable_seqscan=on; SET enable_indexscan=off; SET enable_bitmapscan=off;";;
			index)  g="SET enable_seqscan=off; SET enable_indexscan=on; SET enable_bitmapscan=off;";;
			bitmap) g="SET enable_seqscan=off; SET enable_indexscan=off; SET enable_bitmapscan=on;";;
			esac
			r=$($PSQL -Atc "$g SELECT count(*) || ':' || coalesce(md5(string_agg(id::text, ',' ORDER BY id)), '-') FROM ds WHERE $q" 2>&1)
			eval "res_$arm=\"\$r\""
		done
		if [ "$res_index" = "$res_heap" ] && [ "$res_bitmap" = "$res_heap" ]; then
			log "PASS $phase [$q] heap=$res_heap"
		else
			log "FAIL $phase [$q] heap=$res_heap index=$res_index bitmap=$res_bitmap"; fail=1
		fi
	done
	# vector ORDER BY LIMIT 50 under a docvalues restriction, and without
	for q in "ORDER BY emb <-> '[1,1,1,1]'::wvec LIMIT 50" "WHERE price < 5 ORDER BY emb <-> '[1,1,1,1]'::wvec LIMIT 50"; do
		h=$($PSQL -Atc "SET enable_seqscan=on; SET enable_indexscan=off; SET enable_bitmapscan=off; SELECT string_agg(round((emb <-> '[1,1,1,1]'::wvec)::numeric, 4)::text, ',') FROM (SELECT emb FROM ds $q) s" 2>&1)
		i=$($PSQL -Atc "SET enable_seqscan=off; SET enable_indexscan=on; SET enable_bitmapscan=off; SELECT string_agg(round((emb <-> '[1,1,1,1]'::wvec)::numeric, 4)::text, ',') FROM (SELECT emb FROM ds $q) s" 2>&1)
		if [ "$h" = "$i" ]; then log "PASS $phase [$q] distances agree"
		else log "FAIL $phase [$q] distances differ"; echo "heap=$h" >> $OUT/vecdiff.log; echo "index=$i" >> $OUT/vecdiff.log; fail=1; fi
	done
	v=$($PSQL -Atc "SELECT string_agg(invariant, ',') FROM weave_check('ds_w', true) WHERE NOT ok" 2>&1)
	if [ -z "$v" ]; then log "PASS $phase weave_check(deep) clean"; else log "FAIL $phase weave_check: $v"; fail=1; fi
	$PSQL -Atc "SELECT '$phase', weave_index_nsegments('ds_w'), ndocs, ndeleted FROM weave_index_stats('ds_w')" >> $OUT/stats.log 2>&1
	$PSQL -Atc "SELECT '$phase', detail FROM weave_check('ds_w') WHERE invariant = 'doclist_coverage'" >> $OUT/stats.log 2>&1
	return $fail
}

FAILS=0
check build || FAILS=$((FAILS+1))
for c in $(seq 1 $CYCLES); do
	# delete a slice that includes NULL, zero-term and ordinary rows, vacuum (tombstone),
	# insert rows that reuse the freed ctids with values that would be WRONG if
	# they inherited the dead rows' docvalue or lane
	$PSQL -c "DELETE FROM ds WHERE id % 50 = $c" > /dev/null 2>&1
	$PSQL -c "VACUUM ds" > /dev/null 2>&1
	$PSQL -c "INSERT INTO ds SELECT $N * 10 * $c + g,
	          CASE WHEN g % 3 = 0 THEN NULL WHEN g % 3 = 1 THEN to_wdoc('simple', '') ELSE to_wdoc('simple', 'fresh w' || g) END,
	          CASE WHEN g % 2 = 0 THEN NULL ELSE '[5000,5000,5000,5000]'::wvec END, 999
	          FROM generate_series(1, $N / 60) g" > /dev/null 2>&1
	check "cycle$c-pending" || FAILS=$((FAILS+1))
	$PSQL -c "VACUUM ds" > /dev/null 2>&1
	check "cycle$c-flushed" || FAILS=$((FAILS+1))
done
$PSQL -c "SELECT weave_merge('ds_w')" > /dev/null 2>&1
check "merged" || FAILS=$((FAILS+1))
log "DONE fails=$FAILS"
[ $FAILS = 0 ]
