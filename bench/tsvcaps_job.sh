#!/usr/bin/env bash
#
# bench/tsvcaps_job.sh -- M7 step 1: what BM25 loses when tf and document length
# come from a tsvector instead of from the text.  doc/PHASES.md row M7; results in
# bench/RESULTS_TSVECTOR_CAPS.md.
#
# Run on EC2 as a SCRIPT job (it needs the extension installed, which smoke does):
#
#   SCRIPT=bench/tsvcaps_job.sh bench/aws/run.sh c7i.4xlarge script
#
# Every corpus is downloaded ON THE INSTANCE (BEIR through bench/prepdata.py,
# Wikipedia through bench/tsvcaps.py).  Nothing is cached in the repository.
#
# ARMS.  One table per (corpus, text-search config), one weave index per arm, and
# every arm queried with the same statement shape
#     SELECT id FROM t WHERE col @@@ q ORDER BY col <=> q LIMIT 100
# so the only variable is the document representation in `col`:
#
#   exact      a = to_wdoc(cfg, body)                exact tf; doclen = every token
#   exact2     the same index REINDEXed and re-run   determinism control: must be 1.0
#   sumtf      x = a || to_wdoc(''::tsvector)        exact tf; doclen = sum of tf
#                                                    (the definition to_wdoc(tsvector)
#                                                    uses, without its caps)
#   tsv        b = to_wdoc(to_tsvector(cfg, body))   what option 1 converts TODAY:
#                                                    tf = npos (capped), doclen = sum
#   tsvmaxpos  c = b padded so doclen = max(last position, sum npos)
#                                                    what an opclass could derive
#                                                    instead: positions count the
#                                                    stopword gaps, so the last
#                                                    position IS the token count below
#                                                    16,383
#   strip      s = to_wdoc(strip(to_tsvector(...)))  positive control: tf = 1 per
#                                                    lexeme.  Must lose measurably, or
#                                                    the harness cannot see a loss
#
# The `<=>` ordering is the index's block-max WAND path, so N and avgdl come from the
# index's own metapage for each arm (avgdl differs between arms, as it would in a
# real deployment) and doclen goes through the index's 1-byte quantized doclen
# sidecar, which is part of what the product would actually rank with.
#
# The WHERE form, not the bare ORDER BY: with `@@@` every arm returns exactly the
# matching documents and nothing else, so a difference between two arms is a
# RANKING difference and never an artefact of how non-matching rows fill a tail.
#
# Outputs under /tmp/out (pulled while the job runs):
#   probes.txt   the caps themselves, measured on constructed documents
#   caps.tsv     measure 1 per (corpus, cfg)
#   heap.tsv     measure 3 per (corpus, cfg): average pg_column_size, and
#   heaptab.tsv  pg_total_relation_size of a one-column table per representation
#   quality.tsv  nDCG@10 / Recall@100 / MRR@10 per arm (BEIR, which has qrels)
#   agree.tsv    top-10 / top-100 overlap of every arm against `exact` (all corpora)
#   plans.txt    the EXPLAIN of one query per arm, asserted to be the weave index
#   scancheck.tsv  queries run vs index scans counted (evidence the index answered)
#   runs/        every run file, qid<TAB>docid<TAB>rank
#
set -euo pipefail
OUT=/tmp/out
W=/scratch/tsvcaps
mkdir -p "$OUT/runs" "$W"
export PGDATABASE=tsvcaps
PSQL="psql -X -q -v ON_ERROR_STOP=1"
say() { printf '\033[1m--> %s\033[0m\n' "$*"; echo "$(date +%T) $*" >> "$OUT/timing.log"; }
die() { echo "FATAL: $*" >&2; exit 1; }
FAILED=0

# Wikipedia parts: dumps.wikimedia.org needs no account.  Part 1 holds the oldest
# page ids, which are long, heavily edited articles; part 6 is a later id range with
# more typical lengths.  They are the long-document corpus WHOLE (not chunked), and
# part 1 is also cut into 400-word windows so the chunking conjecture is tested on
# identical text.
WIKIDATE=${WIKIDATE:-20260901}
WBASE=https://dumps.wikimedia.org/enwiki/$WIKIDATE
# name|part file suffix|article limit (0 = whole part)|chunk words (0 = none)
WIKIS=${WIKIS-"wiki1|1.xml-p1p41242|0|400 wiki6|6.xml-p958046p1483661|60000|0 wiki3|3.xml-p151574p311329|30000|256"}
BEIR=${BEIR-scifact nfcorpus fiqa}
CFGS=${CFGS:-simple english}
# A tsvector over this many bytes of text MIGHT exceed the 1 MB limit, so those rows
# go through an exception-catching wrapper (serial).  Below it the limit cannot be
# reached: the limit is on the lexeme+position area (MAXSTRPOS, 1 MB - 1), which spends
# at most 3 bytes per byte of text (a 1-byte lexeme + its 1-byte separator costs 1
# lexeme byte, <=1 alignment byte, 2 count bytes, 2 position bytes = 6 per 2), and the
# margin covers case folding that lengthens a UTF-8 sequence.
BIGTXT=200000

# ---------------------------------------------------------------------------
# One (corpus, config): build the representations, measure, query every arm.
#   $1 corpus name (table docs_$1 must exist)   $2 regconfig   $3 beir|wiki
# ---------------------------------------------------------------------------
one() {
	local C=$1 G=$2 K=$3 tag="$1_$2"
	say "$tag: representations"
	$PSQL <<SQL
SET max_parallel_workers_per_gather = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
DROP TABLE IF EXISTS t0, t, q, st, h_text, h_tsv, h_wdoc, h_wdocnp, h_wdoctsv;
CREATE TABLE t0 AS
SELECT id, body,
       CASE WHEN octet_length(body) <= $BIGTXT THEN to_tsvector('$G', body) END AS tsv,
       octet_length(body) > $BIGTXT AS big,
       to_wdoc('$G'::regconfig, body) AS a
  FROM docs_$C;
UPDATE t0 SET tsv = safe_tsv('$G', body) WHERE big;
CREATE TABLE t AS
SELECT t0.id, t0.body, t0.tsv, t0.a,
       t0.a || to_wdoc(''::tsvector) AS x,
       to_wdoc(t0.tsv) AS b,
       CASE WHEN t0.tsv IS NULL THEN NULL
            WHEN k.maxpos > k.npos
            THEN to_wdoc(t0.tsv) || format('''zzpadzz'':%s', k.maxpos - k.npos)::wdoc
            ELSE to_wdoc(t0.tsv) END AS c,
       to_wdoc(strip(t0.tsv)) AS s,
       k.maxnp, k.maxpos, k.npos, k.nlex,
       wdoc_length(t0.a) AS len_a,
       wdoc_length(t0.a || to_wdoc(''::tsvector)) AS sumtf_a,
       wdoc_length(to_wdoc(t0.tsv)) AS len_b,
       greatest(k.maxpos, k.npos) AS len_c
  FROM t0, LATERAL tsv_caps(t0.tsv) k;
DROP TABLE t0;
VACUUM ANALYZE t;
SQL

	say "$tag: measure 1 (caps)"
	$PSQL -At -F $'\t' >> "$OUT/caps.tsv" <<SQL
SELECT '$C', '$G', count(*),
       count(*) FILTER (WHERE tsv IS NULL),
       count(*) FILTER (WHERE maxnp >= 255),
       count(*) FILTER (WHERE maxpos >= 16383),
       count(*) FILTER (WHERE tsv IS NOT NULL AND sumtf_a <> len_b),
       count(*) FILTER (WHERE tsv IS NOT NULL AND len_a <> len_b),
       count(*) FILTER (WHERE tsv IS NOT NULL AND len_a <> len_c),
       count(*) FILTER (WHERE len_a > 16383),
       count(*) FILTER (WHERE c IS NOT NULL AND wdoc_length(c) <> len_c),
       round(avg(len_a)),
       percentile_disc(0.5) WITHIN GROUP (ORDER BY len_a),
       percentile_disc(0.9) WITHIN GROUP (ORDER BY len_a),
       percentile_disc(0.99) WITHIN GROUP (ORDER BY len_a),
       max(len_a),
       sum(len_a), sum(sumtf_a), sum(len_b), sum(len_c),
       max(pg_column_size(tsv)),
       max(octet_length(body)),
       round(avg(len_b::numeric / len_a) FILTER (WHERE len_a > 16383), 4),
       round(avg(len_c::numeric / len_a) FILTER (WHERE len_a > 16383), 4),
       (SELECT count(*) FROM (SELECT a FROM t WHERE len_a > 16383 ORDER BY id LIMIT 200) z),
       (SELECT count(*) FROM (SELECT a FROM t WHERE len_a > 16383 ORDER BY id LIMIT 200) z WHERE NOT rt_ok(a))
  FROM t;
SQL
	tail -1 "$OUT/caps.tsv"

	say "$tag: measure 3 (heap bytes)"
	$PSQL -At -F $'\t' >> "$OUT/heap.tsv" <<SQL
SELECT '$C', '$G', count(*),
       round(avg(octet_length(body))),
       round(avg(pg_column_size(body))),
       round(avg(pg_column_size(tsv))),
       round(avg(pg_column_size(a))),
       round(avg(pg_column_size(x))),
       round(avg(pg_column_size(b))),
       round(avg(pg_column_size(s)))
  FROM t;
SQL
	$PSQL <<SQL
CREATE TABLE h_text AS SELECT id, body FROM t;
CREATE TABLE h_tsv AS SELECT id, tsv FROM t;
CREATE TABLE h_wdoc AS SELECT id, a FROM t;
CREATE TABLE h_wdocnp AS SELECT id, x FROM t;
CREATE TABLE h_wdoctsv AS SELECT id, b FROM t;
VACUUM h_text, h_tsv, h_wdoc, h_wdocnp, h_wdoctsv;
SQL
	$PSQL -At -F $'\t' >> "$OUT/heaptab.tsv" <<SQL
SELECT '$C', '$G',
       pg_total_relation_size('h_text'), pg_total_relation_size('h_tsv'),
       pg_total_relation_size('h_wdoc'), pg_total_relation_size('h_wdocnp'),
       pg_total_relation_size('h_wdoctsv');
DROP TABLE h_text, h_tsv, h_wdoc, h_wdocnp, h_wdoctsv;
SQL

	say "$tag: indexes"
	$PSQL <<SQL
CREATE INDEX ix_a ON t USING weave (a);
CREATE INDEX ix_x ON t USING weave (x);
CREATE INDEX ix_b ON t USING weave (b);
CREATE INDEX ix_c ON t USING weave (c);
CREATE INDEX ix_s ON t USING weave (s);
SQL

	$PSQL -At -F $'\t' -c "SELECT '$C', '$G', pg_relation_size('ix_a'), pg_relation_size('ix_x'),
	        pg_relation_size('ix_b'), pg_relation_size('ix_c'), pg_relation_size('ix_s')" >> "$OUT/idxsize.tsv"

	say "$tag: queries"
	if [ "$K" = beir ]; then
		# fuse.sh's rule: lower, split on non-alphanumerics, drop length <= 2 and
		# the three operator words, de-duplicate, first 20, OR-joined; then the
		# config normalizes each term.  A query that normalizes to nothing is
		# dropped (and counted) for every arm alike.
		$PSQL <<SQL
CREATE TABLE q AS
WITH tk AS (
    SELECT q.qid, s.tok, min(s.ord) AS ord
      FROM qtext_$C q,
           LATERAL unnest(regexp_split_to_array(lower(q.txt), '[^a-z0-9]+'))
                   WITH ORDINALITY AS s(tok, ord)
     WHERE length(s.tok) > 2 AND s.tok NOT IN ('and', 'or', 'not', 'near')
     GROUP BY q.qid, s.tok
), r AS (
    SELECT qid, tok, row_number() OVER (PARTITION BY qid ORDER BY ord) AS rn FROM tk
), j AS (
    SELECT qid, string_agg(tok, ' | ' ORDER BY rn) AS terms FROM r WHERE rn <= 20 GROUP BY qid
)
SELECT qid::text AS qid, 'beir'::text AS grp,
       format('to_wquery(%L::regconfig, %L)', '$G', terms) AS qexpr
  FROM j
 WHERE to_wquery('$G'::regconfig, terms)::text <> '';
SQL
		echo -e "$tag\tqueries_total\t$($PSQL -At -c "SELECT count(*) FROM qtext_$C")\tqueries_run\t$($PSQL -At -c "SELECT count(*) FROM q")" >> "$OUT/queries.tsv"
	else
		# No qrels: queries are drawn from the corpus vocabulary by document
		# frequency band, 25 queries each of 1, 2 and 3 OR-ed terms per band, in a
		# deterministic md5 order.  Plus `captf`: 50 single-term queries on lexemes
		# that reach the 255-position cap in at least one document, which is where
		# a tf-cap effect has to show if it shows anywhere.
		$PSQL <<SQL
CREATE TABLE st AS
SELECT word, ndoc FROM ts_stat('SELECT tsv FROM t WHERE tsv IS NOT NULL')
 WHERE word ~ '^[a-z][a-z0-9]{2,19}$' AND word NOT IN ('and', 'or', 'not', 'near');
CREATE TABLE q AS
WITH n AS (SELECT count(*)::float8 AS n FROM t),
b AS (
    SELECT word,
           CASE WHEN ndoc BETWEEN 2 AND 10 THEN 'rare'
                WHEN ndoc >= 0.001 * n AND ndoc < 0.01 * n THEN 'mid'
                WHEN ndoc >= 0.01 * n AND ndoc < 0.1 * n THEN 'common'
                WHEN ndoc >= 0.1 * n THEN 'vcommon' END AS band
      FROM st, n
), r AS (
    SELECT word, band, row_number() OVER (PARTITION BY band ORDER BY md5(word)) - 1 AS rn
      FROM b WHERE band IS NOT NULL
), s AS (
    SELECT bb.band, nt, k, (CASE nt WHEN 1 THEN 0 WHEN 2 THEN 25 ELSE 75 END) + k * nt AS lo
      FROM (SELECT DISTINCT band FROM r) bb, generate_series(1, 3) nt, generate_series(0, 24) k
)
SELECT format('%s_%s_%s', s.band, s.nt, s.k) AS qid, s.band || '_' || s.nt AS grp,
       quote_literal(string_agg(r.word, ' | ' ORDER BY r.rn)) || '::wquery' AS qexpr
  FROM s JOIN r ON r.band = s.band AND r.rn >= s.lo AND r.rn < s.lo + s.nt
 GROUP BY s.band, s.nt, s.k
HAVING count(*) = s.nt;
INSERT INTO q
SELECT 'captf_' || row_number() OVER (ORDER BY md5(word)), 'captf', quote_literal(word) || '::wquery'
  FROM (SELECT word FROM (SELECT DISTINCT u.lexeme AS word
                            FROM t, unnest(t.tsv) u
                           WHERE t.maxnp >= 255 AND cardinality(u.positions) >= 255) d
         WHERE word ~ '^[a-z][a-z0-9]{2,19}$' AND word NOT IN ('and', 'or', 'not', 'near')
         ORDER BY md5(word) LIMIT 50) z;
-- Known-item: an article's title, OR-ed, finds the article.  Built like a BEIR
-- query (same tokenizing rule), judged by construction.
CREATE TABLE tq AS
WITH pick AS (
    SELECT pageid, title FROM titles_$C
     WHERE array_length(regexp_split_to_array(trim(title), '\s+'), 1) BETWEEN 2 AND 6
     ORDER BY md5(pageid::text) LIMIT 200
), tk AS (
    SELECT p.pageid, s.tok, min(s.ord) AS ord
      FROM pick p, LATERAL unnest(regexp_split_to_array(lower(p.title), '[^a-z0-9]+'))
                   WITH ORDINALITY AS s(tok, ord)
     WHERE length(s.tok) > 2 AND s.tok NOT IN ('and', 'or', 'not', 'near')
     GROUP BY p.pageid, s.tok
)
SELECT pageid, string_agg(tok, ' | ' ORDER BY ord) AS terms FROM tk GROUP BY pageid;
INSERT INTO q
SELECT 'title_' || pageid, 'title', format('to_wquery(%L::regconfig, %L)', '$G', terms)
  FROM tq WHERE to_wquery('$G'::regconfig, terms)::text <> '';
SQL
		# A chunked corpus is named <part>c and its row ids are pageid * 10000 + k.
		local jcond="t.id = tq.pageid"
		case "$C" in *c) jcond="t.id / 10000 = tq.pageid" ;; esac
		$PSQL -c "\copy (SELECT 'title_' || tq.pageid, t.id, 1 FROM tq JOIN t ON $jcond WHERE 'title_' || tq.pageid IN (SELECT qid FROM q) ORDER BY 1, 2) TO '$W/qrels_title.tsv'"
		$PSQL -c "DROP TABLE tq" >/dev/null
		$PSQL -At -F $'\t' -c "SELECT '$tag', grp, count(*) FROM q GROUP BY grp ORDER BY grp" >> "$OUT/queries.tsv"
	fi
	$PSQL -c "\\copy (SELECT qid, grp FROM q ORDER BY qid) TO '$OUT/runs/${tag}_groups.tsv'"

	$PSQL -c "TRUNCATE runs" >/dev/null
	local arm col ix res nq ns qx
	qx=$($PSQL -At -c "SELECT qexpr FROM q ORDER BY qid LIMIT 1")
	for spec in exact:a:ix_a exact2:a:ix_a sumtf:x:ix_x tsv:b:ix_b tsvmaxpos:c:ix_c strip:s:ix_s; do
		IFS=: read -r arm col ix <<< "$spec"
		if [ "$arm" = exact2 ]; then
			$PSQL -c "REINDEX INDEX ix_a" >/dev/null
		fi
		# The plan of one query per arm, ASSERTED: the weave index answering with
		# an ordering scan.  A Seq Scan + Sort would give the same rows by a path
		# that measures nothing about the index.
		{
			echo "== $tag $arm"
			$PSQL -c "SET enable_seqscan = off; SET enable_bitmapscan = off;
			          SET max_parallel_workers_per_gather = 0;
			          EXPLAIN (COSTS OFF) SELECT id FROM t WHERE $col @@@ $qx ORDER BY $col <=> $qx LIMIT 100"
		} > "$W/plan.txt" 2>&1
		cat "$W/plan.txt" >> "$OUT/plans.txt"
		grep -q "Index Scan using $ix on t" "$W/plan.txt" && grep -q 'Order By:' "$W/plan.txt" \
			|| die "$tag $arm: plan is not an ordering scan of $ix (see plans.txt)"
		local t0 t1
		t0=$(date +%s.%N)
		res=$($PSQL -At -c "SET enable_seqscan = off; SET enable_bitmapscan = off;
		                    SET max_parallel_workers_per_gather = 0;
		                    SELECT tc_run('$arm', '$col', '$ix')" | tail -1)
		t1=$(date +%s.%N)
		nq=${res%%$'\t'*}
		ns=${res##*$'\t'}
		printf '%s\t%s\t%s\t%s\t%s\n' "$tag" "$arm" "$nq" "$ns" "$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')" >> "$OUT/scancheck.tsv"
		case "$nq$ns" in ''|*[!0-9]*) die "$tag $arm: tc_run returned '$res'" ;; esac
		if [ "$ns" -lt "$nq" ]; then
			echo "SCANCHECK FAIL: $tag $arm ran $nq queries but $ix counted $ns scans" | tee -a "$OUT/scancheck.tsv"
			FAILED=1
		fi
		$PSQL -c "\\copy (SELECT qid, id, o FROM runs, unnest(ids) WITH ORDINALITY u(id, o) WHERE arm = '$arm' ORDER BY qid, o) TO '$OUT/runs/${tag}_${arm}.tsv'"
	done

	say "$tag: scoring"
	local QR
	if [ "$K" = beir ]; then QR=$W/beir/$C/qrels.tsv; else QR=$W/qrels_title.tsv; fi
	cp "$QR" "$OUT/runs/${tag}_qrels.tsv"
	for arm in exact exact2 sumtf tsv tsvmaxpos strip; do
		# For the wiki corpora only the title queries are judged; ndcg.py scores
		# the qids in qrels, so the band queries in the same run file are ignored.
		python3 bench/ndcg.py --qrels "$QR" --run "$OUT/runs/${tag}_${arm}.tsv" \
			--k 10 --label "$tag/$arm" > "$W/ndcg.out" 2>> "$OUT/ndcg.stderr" \
			|| { echo "NDCG FAILED: $tag/$arm" | tee -a "$OUT/failed.txt"; FAILED=1; }
		grep -v '^label' "$W/ndcg.out" >> "$OUT/quality.tsv" || true
		python3 bench/tsvcaps.py paired "$QR" "$OUT/runs/${tag}_exact.tsv" "$OUT/runs/${tag}_${arm}.tsv" \
			--label "$tag/$arm" >> "$OUT/paired.tsv" \
			|| { echo "PAIRED FAILED: $tag/$arm" | tee -a "$OUT/failed.txt"; FAILED=1; }
		python3 bench/tsvcaps.py agree "$OUT/runs/${tag}_exact.tsv" "$OUT/runs/${tag}_${arm}.tsv" \
			--groups "$OUT/runs/${tag}_groups.tsv" --label "$tag/$arm" >> "$OUT/agree.tsv"
	done
	# The CAP effect alone: tsv against sumtf.  Both define doclen as the sum of tf,
	# so the only difference left between them is what the caps did to tf and length.
	python3 bench/tsvcaps.py agree "$OUT/runs/${tag}_sumtf.tsv" "$OUT/runs/${tag}_tsv.tsv" \
		--groups "$OUT/runs/${tag}_groups.tsv" --label "$tag/tsv_vs_sumtf" >> "$OUT/agree.tsv"
	grep "^$tag/" "$OUT/quality.tsv" 2>/dev/null || true
	grep "^$tag/" "$OUT/paired.tsv" 2>/dev/null || true
	grep "^$tag/" "$OUT/agree.tsv" | grep -P '\tall\t' || true
	$PSQL -c "DROP TABLE t, q; DROP TABLE IF EXISTS st;" >/dev/null
}

# Each (corpus, config) runs in its own bash process, so a failure in one is
# recorded and the rest still run, while `set -e` keeps its meaning inside it.
if [ "${1:-}" = unit ]; then
	shift
	one "$@"
	exit "$FAILED"
fi
unit() {
	bash "$0" unit "$@" || { echo "UNIT FAILED: $*" | tee -a "$OUT/failed.txt"; FAILED=1; }
}

# A marker left by an earlier run on the same host would let the main loop start on a
# part while this run's prep is still REWRITING it -- the titles file is written in
# place, not renamed -- and it did: M7 step 2's second run (pgweave-20261007-221700-55a5)
# read a half-written titles file and drew 35 of its 200 title queries from a
# different pool.  The download cache stays; only the markers go.
find "$W" -maxdepth 1 \( -name '*.done' -o -name '*.failed' \) -delete

# Start the Wikipedia fetch + strip now, in the background, so it overlaps BEIR.
# Sequential inside the subshell: dumps.wikimedia.org allows two connections per IP.
# Each part's files appear atomically (renamed into place), with a .done marker
# after the last one, so the main loop can start on a part while the next downloads.
(
	for spec in $WIKIS; do
		IFS='|' read -r name part limit chunk <<< "$spec"
		args=(--limit "$limit" --titles-out "$W/${name}_titles.tsv" --cache "$W/_cache")
		[ "$chunk" -gt 0 ] && args+=(--chunk "$chunk" --chunk-out "$W/${name}c.tsv")
		python3 bench/tsvcaps.py wiki "$WBASE/enwiki-$WIKIDATE-pages-articles-multistream$part.bz2" \
			"$W/$name.tsv" "${args[@]}" || { touch "$W/$name.failed"; continue; }
		touch "$W/$name.done"
	done
) > "$OUT/wikiprep.log" 2>&1 &
WIKIPID=$!

createdb tsvcaps 2>/dev/null || true
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_weave" >/dev/null
$PSQL -At -c "SELECT version()" > "$OUT/env.txt"
$PSQL -At -c "SELECT 'pg_weave ' || extversion FROM pg_extension WHERE extname = 'pg_weave'" >> "$OUT/env.txt"
$PSQL -At -c "SELECT 'default_toast_compression = ' || current_setting('default_toast_compression')" >> "$OUT/env.txt"
git rev-parse HEAD >> "$OUT/env.txt" 2>/dev/null || cat "$HOME/commit.txt" >> "$OUT/env.txt" 2>/dev/null || true

$PSQL <<'SQL'
-- to_tsvector, or NULL when the result would exceed the tsvector size limit.
CREATE OR REPLACE FUNCTION safe_tsv(c regconfig, t text) RETURNS tsvector
LANGUAGE plpgsql AS $$
BEGIN
	RETURN to_tsvector(c, t);
EXCEPTION WHEN program_limit_exceeded THEN
	RETURN NULL;
END $$;

-- Per-tsvector cap statistics.  positions are ascending, so the last one is the max.
CREATE OR REPLACE FUNCTION tsv_caps(v tsvector, OUT maxnp int, OUT maxpos int,
                                    OUT npos int, OUT nlex int)
LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
SELECT max(cardinality(positions))::int,
       max(positions[cardinality(positions)])::int,
       coalesce(sum(cardinality(positions)), 0)::int,
       count(*)::int
  FROM unnest(v) $$;

-- Does a wdoc survive its own text output + input?  (pg_dump's path.)
CREATE OR REPLACE FUNCTION rt_ok(d wdoc) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM d::text::wdoc;
	RETURN true;
EXCEPTION WHEN others THEN
	RETURN false;
END $$;

DROP TABLE IF EXISTS runs;
CREATE TABLE runs (arm text, qid text, ids bigint[]);

-- One literal statement per query, so the planner sees a constant wquery exactly as
-- an application's query would.  Returns "<queries run>\t<index scans counted>".
CREATE OR REPLACE FUNCTION tc_run(arm text, col text, ix text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r	record;
	ids	bigint[];
	n	int := 0;
	s0	bigint;
BEGIN
	s0 := pg_stat_get_xact_numscans(ix::regclass);
	FOR r IN SELECT qid, qexpr FROM q ORDER BY qid LOOP
		EXECUTE format('SELECT ARRAY(SELECT id FROM t WHERE %I @@@ %s ORDER BY %I <=> %s LIMIT 100)',
		               col, r.qexpr, col, r.qexpr) INTO ids;
		INSERT INTO runs VALUES (arm, r.qid, ids);
		n := n + 1;
	END LOOP;
	RETURN n || E'\t' || (pg_stat_get_xact_numscans(ix::regclass) - s0);
END $$;
SQL

# ---------------------------------------------------------------------------
# Probes: the caps on constructed documents, so the numbers in the results file are
# measured on this server and not quoted from a header.
# ---------------------------------------------------------------------------
say "probes"
psql -X -q -e > "$OUT/probes.txt" 2>&1 <<'SQL'
\echo P1 tf cap: one lexeme repeated 1000 times
SELECT cardinality(positions) AS tsv_npos,
       wdoc_length(to_wdoc(to_tsvector('simple', repeat('x ', 1000)))) AS from_tsv_len,
       wdoc_length(to_wdoc('simple'::regconfig, repeat('x ', 1000))) AS exact_len
  FROM unnest(to_tsvector('simple', repeat('x ', 1000)));
\echo P2 position cap: 1000 words round-robin x 30 (30000 tokens, every exact tf = 30)
WITH d AS (SELECT string_agg('w' || (i % 1000), ' ' ORDER BY i) AS body
             FROM generate_series(0, 29999) i)
SELECT wdoc_length(to_wdoc('simple'::regconfig, body)) AS exact_len,
       wdoc_length(to_wdoc(to_tsvector('simple', body))) AS from_tsv_len,
       (tsv_caps(to_tsvector('simple', body))).*
  FROM d;
\echo P3 1 MB limit: 40000 distinct 32-character tokens
DO $$
BEGIN
	PERFORM to_tsvector('simple', (SELECT string_agg(md5(i::text), ' ') FROM generate_series(1, 40000) i));
	RAISE NOTICE 'P3: no error';
EXCEPTION WHEN program_limit_exceeded THEN
	RAISE NOTICE 'P3: ERROR %', SQLERRM;
END $$;
SELECT safe_tsv('simple', (SELECT string_agg(md5(i::text), ' ') FROM generate_series(1, 40000) i)) IS NULL
       AS p3_safe_tsv_returns_null;
\echo P4 strip: tf becomes 1 per lexeme
SELECT to_wdoc(strip(to_tsvector('simple', 'a a a b')))::text AS stripped,
       wdoc_length(to_wdoc(strip(to_tsvector('simple', 'a a a b')))) AS stripped_len;
\echo P5 wdoc text and binary round trip past position 16383 (to_wdoc(regconfig,text))
DO $$
DECLARE
	d	wdoc;
	n	int;
BEGIN
	FOREACH n IN ARRAY ARRAY[5000, 10000] LOOP
		d := to_wdoc('simple'::regconfig, repeat('a b ', n));
		BEGIN
			PERFORM d::text::wdoc;
			RAISE NOTICE 'P5 % tokens: text round trip OK', 2 * n;
		EXCEPTION WHEN others THEN
			RAISE NOTICE 'P5 % tokens: text round trip ERROR %', 2 * n, SQLERRM;
		END;
	END LOOP;
	d := to_wdoc(repeat('a b ', 10000));
	BEGIN
		PERFORM d::text::wdoc;
		RAISE NOTICE 'P5 20000 tokens via to_wdoc(text): text round trip OK';
	EXCEPTION WHEN others THEN
		RAISE NOTICE 'P5 20000 tokens via to_wdoc(text): ERROR %', SQLERRM;
	END;
END $$;
CREATE TEMP TABLE p5 (d wdoc);
INSERT INTO p5 SELECT to_wdoc('simple'::regconfig, repeat('a b ', 10000));
COPY p5 TO '/tmp/tsvcaps_p5.bin' (FORMAT binary);
COPY p5 TO '/tmp/tsvcaps_p5.txt';
CREATE TEMP TABLE p5t (d wdoc);
DO $$
BEGIN
	COPY p5t FROM '/tmp/tsvcaps_p5.txt';
	RAISE NOTICE 'P5 text COPY round trip OK';
EXCEPTION WHEN others THEN
	RAISE NOTICE 'P5 text COPY round trip ERROR %', SQLERRM;
END $$;
CREATE TEMP TABLE p5b (d wdoc);
DO $$
BEGIN
	COPY p5b FROM '/tmp/tsvcaps_p5.bin' (FORMAT binary);
	RAISE NOTICE 'P5 binary round trip OK';
EXCEPTION WHEN others THEN
	RAISE NOTICE 'P5 binary round trip ERROR %', SQLERRM;
END $$;
\echo P6 the padding term used by arm tsvmaxpos, and the empty wdoc used by arm sumtf
SELECT wdoc_length('''zzpadzz'':5'::wdoc) AS pad5,
       wdoc_length(to_wdoc(to_tsvector('simple', 'a b c')) || '''zzpadzz'':5'::wdoc) AS three_plus5,
       wdoc_length(to_wdoc('simple'::regconfig, 'the the cat') || to_wdoc(''::tsvector)) AS simple_sumtf,
       wdoc_length(to_wdoc('english'::regconfig, 'the the cat') || to_wdoc(''::tsvector)) AS english_sumtf;
\echo P8 doclen through the wdoc text and binary I/O (the pg_dump / COPY path)
CREATE TEMP TABLE p8 (d wdoc);
INSERT INTO p8 VALUES (to_wdoc('english'::regconfig, 'the cat sat on the mat'));
COPY p8 TO '/tmp/tsvcaps_p8.bin' (FORMAT binary);
CREATE TEMP TABLE p8b (d wdoc);
COPY p8b FROM '/tmp/tsvcaps_p8.bin' (FORMAT binary);
SELECT wdoc_length(d) AS stored_len,
       wdoc_length(d::text::wdoc) AS after_text_io,
       (SELECT wdoc_length(d) FROM p8b) AS after_binary_io
  FROM p8;
\echo P7 doclen definitions under a stopword list
SELECT wdoc_length(to_wdoc('english'::regconfig, 'the cat sat on the mat')) AS exact_len,
       wdoc_length(to_wdoc(to_tsvector('english', 'the cat sat on the mat'))) AS from_tsv_len,
       to_tsvector('english', 'the cat sat on the mat')::text AS tsv;
SQL
cat "$OUT/probes.txt"
# The NOTICE, not the text: psql -e echoes the statement, which contains the same words.
grep -q 'NOTICE:  P5 binary round trip' "$OUT/probes.txt" \
	|| { echo "PROBES did not run to the end" | tee -a "$OUT/failed.txt"; FAILED=1; }


printf 'corpus\tcfg\tndocs\tn_tsv_error\tn_tf_cap_reached\tn_pos_cap_reached\tn_tf_wrong\tn_len_b_wrong\tn_len_c_wrong\tn_len_gt_16383\tn_len_c_selfcheck_fail\tavg_len\tp50_len\tp90_len\tp99_len\tmax_len\tsum_len_a\tsum_tf_a\tsum_len_b\tsum_len_c\tmax_tsv_bytes\tmax_text_octets\tlong_avg_len_b_over_a\tlong_avg_len_c_over_a\tn_rt_checked\tn_rt_fail\n' > "$OUT/caps.tsv"
printf 'corpus\tcfg\tndocs\tavg_text_octets\tavg_text\tavg_tsv\tavg_wdoc\tavg_wdoc_nopos\tavg_wdoc_from_tsv\tavg_wdoc_strip\n' > "$OUT/heap.tsv"
printf 'corpus\tcfg\ttext\ttsvector\twdoc\twdoc_nopos\twdoc_from_tsv\n' > "$OUT/heaptab.tsv"
printf 'label\tnqueries_scored\tndcg@10\trecall@100\tmrr@10\n' > "$OUT/quality.tsv"
printf 'label\tgroup\tnq\tov10\tov100\tsame10\n' > "$OUT/agree.tsv"
printf 'label\tnq\tnq_changed\tdelta_ndcg10_vs_exact\tci95_lo\tci95_hi\n' > "$OUT/paired.tsv"
printf 'tag\tarm\tqueries\tindex_scans\tseconds\n' > "$OUT/scancheck.tsv"
printf 'corpus\tcfg\tix_exact\tix_sumtf\tix_tsv\tix_tsvmaxpos\tix_strip\n' > "$OUT/idxsize.tsv"

# ---------------------------------------------------------------------------
# BEIR (chunked passages, with qrels).  --embed hash: the vectors are not used.
# ---------------------------------------------------------------------------
for D in $BEIR; do
	say "$D: prepdata"
	python3 bench/prepdata.py --dataset "$D" --out "$W/beir" --embed hash > "$OUT/prep-$D.log" 2>&1 \
		|| { cat "$OUT/prep-$D.log"; die "prepdata $D failed"; }
	$PSQL <<SQL
DROP TABLE IF EXISTS stage_c, stage_q, docs_$D, qtext_$D;
CREATE TABLE stage_c (docid bigint, txt text, vec text);
CREATE TABLE stage_q (qid bigint, txt text, vec text);
\copy stage_c FROM '$W/beir/$D/corpus.tsv' WITH (FORMAT csv, DELIMITER E'\t', QUOTE E'\b')
\copy stage_q FROM '$W/beir/$D/queries.tsv' WITH (FORMAT csv, DELIMITER E'\t', QUOTE E'\b')
CREATE TABLE docs_$D AS SELECT docid AS id, txt AS body FROM stage_c;
CREATE TABLE qtext_$D AS SELECT qid, txt FROM stage_q;
DROP TABLE stage_c, stage_q;
SQL
	for G in $CFGS; do unit "$D" "$G" beir; done
	$PSQL -c "DROP TABLE docs_$D, qtext_$D" >/dev/null
done

# ---------------------------------------------------------------------------
# Wikipedia (long whole documents, no qrels), then the same text chunked.
# ---------------------------------------------------------------------------
for spec in $WIKIS; do
	IFS='|' read -r name part limit chunk <<< "$spec"
	say "waiting for $name"
	while [ ! -e "$W/$name.done" ] && [ ! -e "$W/$name.failed" ]; do
		kill -0 "$WIKIPID" 2>/dev/null || break
		sleep 10
	done
	if [ ! -e "$W/$name.done" ]; then
		cat "$OUT/wikiprep.log"
		echo "WIKI PREP FAILED: $name" | tee -a "$OUT/failed.txt"; FAILED=1; continue
	fi
	tail -1 "$OUT/wikiprep.log"
	D_LIST=$name; [ "$chunk" -gt 0 ] && D_LIST="$name ${name}c"
	for D in $D_LIST; do
		# Known-item title queries: the article's own title must find the
		# article (whole corpus) or any of its chunks (chunked corpus, row id =
		# pageid * 10000 + window).  200 titles, md5 order, 2-6 words long.
		$PSQL <<SQL
DROP TABLE IF EXISTS docs_$D, titles_$D;
CREATE TABLE docs_$D (id bigint, body text);
\copy docs_$D FROM '$W/$D.tsv' WITH (FORMAT csv, DELIMITER E'\t', QUOTE E'\b')
CREATE TABLE titles_$D (pageid bigint, title text);
\copy titles_$D FROM '$W/${name}_titles.tsv' WITH (FORMAT csv, DELIMITER E'\t', QUOTE E'\b')
SQL
		for G in $CFGS; do unit "$D" "$G" wiki; done
		$PSQL -c "DROP TABLE docs_$D, titles_$D" >/dev/null
	done
done

say "done (FAILED=$FAILED)"
exit "$FAILED"
