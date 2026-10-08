#!/bin/bash
# G86 fused follow-up, MEASURE BEFORE BUILDING (hard rule 9).  Run as
#   SCRIPT=bench/aws/g86f_prize.sh bench/aws/run.sh c7i.4xlarge script
# What the fused hidden sort key costs today, per returned row, and how much of a
# fused query's latency that is.  Three arms per cell:
#   q    `SELECT id FROM lh ORDER BY fuse(...) LIMIT k`, the query as users write it
#   eval the hidden key alone: sum(fuse(...)) minus count(id) over the SAME k rows,
#        fetched by a TID scan (both arms deform the heap tuple; only the first
#        detoasts and rescores) -- an upper bound on what reuse can remove
#   srf  weave_fuse_search(..., k): the same fused scan with no fuse() re-evaluation
# plus the per-row function calls from track_functions.  Two runs per arm (rule 10),
# two scales per document kind (rule 11).  Results under /tmp/out.
set -u
OUT=/tmp/out
mkdir -p $OUT
P="psql -X -q -At -d postgres -v ON_ERROR_STOP=1"
$P -c "CREATE EXTENSION IF NOT EXISTS pg_weave" || exit 1
$P -c "ALTER EXTENSION pg_weave UPDATE" || exit 1
QV="'[0.3,0.6,0.2,0.9,0.1,0.5,0.7,0.4]'"
corpus() {	# $1 kind (short|long), $2 n
	local body
	if [ "$1" = short ]; then
		body="repeat('x' || (g % 101) || ' ', 1 + g % 23)"
	else
		body="(SELECT string_agg('w' || ((g * 7919 + i * 104729) % 60000), ' ') FROM generate_series(1, 700) i)"
	fi
	$P <<SQL || exit 1
DROP TABLE IF EXISTS lh;
CREATE TABLE lh (id int, d wdoc, v wvec(8)) WITH (autovacuum_enabled = off);
INSERT INTO lh SELECT g, to_wdoc('simple',
   CASE WHEN g % 10 < 3 THEN 'a ' ELSE '' END || CASE WHEN g % 10 >= 7 THEN 'b ' ELSE '' END ||
   CASE WHEN g % 10 = 5 THEN 'c ' ELSE '' END || CASE WHEN g % 500 = 7 THEN 'rare ' ELSE '' END || $body),
   CASE WHEN g % 997 = 0 THEN NULL
        ELSE ('[' || (g % 97) / 97.0 || ',' || (g % 89) / 89.0 || ',' || (g % 83) / 83.0 || ',' ||
              (g % 79) / 79.0 || ',' || (g % 73) / 73.0 || ',' || (g % 71) / 71.0 || ',' ||
              (g % 67) / 67.0 || ',' || (g % 61) / 61.0 || ']')::wvec END
  FROM generate_series(1, $2) g;
CREATE INDEX lh_w ON lh USING weave (d, v);
VACUUM ANALYZE lh;
SQL
	$P -c "SELECT '$1 $2', avg(pg_column_size(d))::int AS avg_doc_bytes,
	         pg_size_pretty(pg_relation_size(reltoastrelid)) AS toast
	       FROM lh, pg_class WHERE relname = 'lh' GROUP BY reltoastrelid" | tee -a $OUT/corpus.txt
}
# name|fuse expression|weave_fuse_search lex array|vec array
QUERIES=(
"lexlex|fuse(d <=> 'c', d <=> 'rare')|ARRAY['c'::wquery, 'rare']|NULL"
"oror|fuse(d <=> 'a | b', d <=> 'c')|ARRAY['a | b'::wquery, 'c']|NULL"
"lexvec|fuse(d <=> 'c', v <-> $QV)|ARRAY['c'::wquery]|ARRAY[$QV::wvec]"
)
timeit() {	# $1 = SQL to PERFORM; prints the median ms of 25 after 5 discarded
	cat <<SQL
DROP TABLE IF EXISTS tm; CREATE TEMP TABLE tm(ms float8);
DO \$\$ DECLARE i int; t0 timestamptz; BEGIN
  FOR i IN 1..30 LOOP
    t0 := clock_timestamp();
    PERFORM $1;
    IF i > 5 THEN INSERT INTO tm VALUES (extract(epoch FROM clock_timestamp() - t0) * 1000); END IF;
  END LOOP; END \$\$;
SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY ms)::numeric(10,3) FROM tm;
SQL
}
for spec in "short 200000" "short 1000000" "long 50000" "long 200000"; do
	set -- $spec; kind=$1; n=$2
	corpus "$kind" "$n"
	for run in 1 2; do
		for qq in "${QUERIES[@]}"; do
			IFS='|' read -r qn fx lexa veca <<< "$qq"
			for lim in 10 400; do
				f=$OUT/c_${kind}_${n}_${qn}_${lim}_${run}
				fq="SELECT id FROM lh ORDER BY $fx LIMIT $lim"
				$P > $f.log 2>&1 <<SQL
SET jit = off; SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;
SELECT count(*) FROM lh WHERE d @@@ 'zzz';
CREATE TEMP TABLE pl(l text);
DO \$\$ DECLARE r record; BEGIN FOR r IN EXECUTE \$q\$EXPLAIN (VERBOSE, COSTS OFF) $fq\$q\$ LOOP INSERT INTO pl VALUES (r."QUERY PLAN"); END LOOP; END \$\$;
SELECT 'fused_plan', count(*) FROM pl WHERE l LIKE '%<~>%';
CREATE TEMP TABLE tids AS SELECT ctid AS t FROM ($fq) s0, lh WHERE lh.id = s0.id;
SELECT 'rows', count(*) FROM tids;
CREATE TEMP TABLE pl2(l text);
DO \$\$ DECLARE r record; BEGIN FOR r IN EXPLAIN (COSTS OFF) SELECT count(id) FROM lh WHERE ctid = ANY(ARRAY(SELECT t FROM tids)) LOOP INSERT INTO pl2 VALUES (r."QUERY PLAN"); END LOOP; END \$\$;
SELECT 'tid_plan', count(*) FROM pl2 WHERE l LIKE '%Tid Scan%';
SET track_functions = 'all'; SET stats_fetch_consistency = none;
SELECT pg_stat_force_next_flush();
CREATE TEMP TABLE c0 AS SELECT funcname, calls FROM pg_stat_user_functions;
SELECT 'qrows', count(*) FROM ($fq) s;
SELECT pg_stat_force_next_flush();
SELECT 'calls', string_agg(f.funcname || '=' || (f.calls - coalesce(c0.calls, 0)), ' ' ORDER BY f.funcname)
  FROM pg_stat_user_functions f LEFT JOIN c0 USING (funcname)
 WHERE f.calls - coalesce(c0.calls, 0) > 0;
RESET track_functions;
SQL
				for arm in "q|count(*) FROM ($fq) s" \
				           "evalsum|sum($fx) FROM lh WHERE ctid = ANY(ARRAY(SELECT t FROM tids))" \
				           "evalctl|count(id) FROM lh WHERE ctid = ANY(ARRAY(SELECT t FROM tids))" \
				           "srf|count(*) FROM weave_fuse_search('lh_w', $lexa, $veca, NULL, $lim)"; do
					an=${arm%%|*}; ap=${arm#*|}
					{
						echo "SET jit = off; SET max_parallel_workers_per_gather = 0;"
						echo "SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off;"
						echo "CREATE TEMP TABLE tids AS SELECT ctid AS t FROM ($fq) s0, lh WHERE lh.id = s0.id;"
						timeit "$ap"
					} | $P > $f.$an 2>&1
				done
				echo "$kind $n $qn L$lim run$run $(grep -h -E '^(fused_plan|rows|tid_plan|calls)' $f.log | tr '\n' ' ') q=$(tail -1 $f.q) evalsum=$(tail -1 $f.evalsum) evalctl=$(tail -1 $f.evalctl) srf=$(tail -1 $f.srf)" | tee -a $OUT/summary.txt
			done
		done
	done
done
echo "PRIZE DONE"
