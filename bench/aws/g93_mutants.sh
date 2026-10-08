#!/bin/bash
# G93 mutation run, adapted from m7_mutants.sh.  First saves the smoke's own
# results/ and the FULL regression.diffs (smoke pulls only 60 lines).  Then, for
# each mutant: fresh copy of the shipped tree, apply (each substitution must
# match exactly once), make clean + build, install, check the installed .so
# differs from the clean one, run tsquery_cast, and only then count a diff
# against the CLEAN tree's output as a catch.  The CONTROL (clean tree, same
# commands, run twice) must differ from itself by 0 lines.
mkdir -p /tmp/out/smoke
cp -a $HOME/pg_weave/results /tmp/out/smoke/ 2>/dev/null
cp $HOME/pg_weave/regression.diffs /tmp/out/smoke/ 2>/dev/null
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
F=src/util/migrate.c
case $m in
M1_weight_dropped)
	sub $F '			flags = WEAVE_QF_WEIGHTED;' '			flags = 0;' ;;
M2_prefix_dropped)
	sub $F '			flags = WEAVE_QF_PREFIX;' '			flags = 0;' ;;
M3_phrase_distance_plus_one)
	sub $F 'mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, 0, 1, NULL, 0);' \
		'mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, 0, 2, NULL, 0);' ;;
M4_phrase_N_accepted_as_at_most_N)
	sub $F '				if (op->distance + wr != 1)' '				if (op->distance == 0)'
	sub $F 'mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, 0, 1, NULL, 0);' \
		'mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, 0, op->distance, NULL, 0);' ;;
M5_bool_in_phrase_accepted)
	sub $F '		if (op->oper != OP_PHRASE && in_phrase)' '		if (false)' ;;
M6_prefix_in_phrase_accepted)
	sub $F '			if (in_phrase)
				mig_refuse' '			if (false)
				mig_refuse' ;;
M8_index_not_inexact_reverted)
	sub src/am/amscan.c '			if (stack[top - 1].inexact)
			{' '			if (false)
			{' ;;
M7_pre_fix_converter)
	cp /tmp/out/migrate.c.prefix $F ;;
*) echo "unknown mutant $m"; exit 2 ;;
esac
echo "applied $m"
MUTEOF
chmod +x /tmp/out/mutants.sh
cat > /tmp/out/migrate.c.prefix <<'OLDEOF'
/*-------------------------------------------------------------------------
 *
 * pg_weave_migrate.c
 *		Migration helpers from the existing tsvector/tsquery stack to pg_weave.
 *
 * Stage 11 of pg_weave.  tsquery_to_wquery() mechanically converts a tsquery
 * into an wquery so existing queries port with minimal churn: & -> AND,
 * | -> OR, ! -> NOT, and the phrase operator <N> (OP_PHRASE) -> wquery
 * WEAVE_OP_PHRASE preserving the token gap, so adjacency is carried over
 * faithfully.
 *
 * tsquery is stored in prefix (Polish) order; wquery is postfix (RPN).  We
 * walk the tsquery tree recursively and emit postfix items.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_migrate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"

/* An emitted wquery item, collected before flattening. */
typedef struct MigItem
{
	uint8		type;
	uint8		op;
	uint32		distance;		/* max token gap for WEAVE_OP_PHRASE (else unused) */
	char	   *term;			/* folded term (lowercased) for VAL items */
	int			termlen;
}			MigItem;

typedef struct MigState
{
	TSQuery		query;
	char	   *operands;		/* base of operand text */
	MigItem    *items;
	int			nitems;
	int			maxitems;
}			MigState;

static void
mig_emit(MigState *st, uint8 type, uint8 op, uint32 distance, char *term, int termlen)
{
	if (st->nitems >= st->maxitems)
	{
		st->maxitems = st->maxitems ? st->maxitems * 2 : 16;
		if (st->items == NULL)
			st->items = (MigItem *) palloc(st->maxitems * sizeof(MigItem));
		else
			st->items = (MigItem *) repalloc(st->items,
											 st->maxitems * sizeof(MigItem));
	}
	st->items[st->nitems].type = type;
	st->items[st->nitems].op = op;
	st->items[st->nitems].distance = distance;
	st->items[st->nitems].term = term;
	st->items[st->nitems].termlen = termlen;
	st->nitems++;
}

/* Recursively walk the tsquery item at index `pos`, emitting postfix. */
static void
mig_walk(MigState *st, QueryItem *item)
{
	if (item->type == QI_VAL)
	{
		QueryOperand *op = &item->qoperand;
		char	   *src = st->operands + op->distance;
		char	   *folded = (char *) palloc(op->length);
		int			i;

		/* tsquery lexemes are already normalized; copy verbatim (they are the
		 * dictionary output, so no further folding is applied). */
		for (i = 0; i < (int) op->length; i++)
			folded[i] = src[i];
		mig_emit(st, WEAVE_QI_VAL, 0, 0, folded, op->length);
	}
	else						/* QI_OPR */
	{
		QueryOperator *op = &item->qoperator;

		if (op->oper == OP_NOT)
		{
			/* NOT has a single (right) operand at item+1 */
			mig_walk(st, item + 1);
			mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_NOT, 0, NULL, 0);
		}
		else
		{
			QueryItem  *left = item + op->left;
			QueryItem  *right = item + 1;
			uint8		ftop;
			uint32		dist = 1;

			mig_walk(st, left);
			mig_walk(st, right);

			switch (op->oper)
			{
				case OP_AND:
					ftop = WEAVE_OP_AND;
					break;
				case OP_OR:
					ftop = WEAVE_OP_OR;
					break;
				case OP_PHRASE:
					/* faithful: tsquery <N> -> wquery phrase with the same gap */
					ftop = WEAVE_OP_PHRASE;
					dist = op->distance;
					break;
				default:
					ftop = WEAVE_OP_AND;
					break;
			}
			mig_emit(st, WEAVE_QI_OPR, ftop, dist, NULL, 0);
		}
	}
}

PG_FUNCTION_INFO_V1(tsquery_to_wquery);

Datum
tsquery_to_wquery(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	MigState	st;
	WeaveQuery	q;
	WeaveQueryItem *items;
	char	   *textbase;
	Size		textbytes = 0;
	Size		total;
	uint32		off = 0;
	int			i;

	st.query = query;
	st.operands = GETOPERAND(query);
	st.items = NULL;
	st.nitems = 0;
	st.maxitems = 0;

	if (query->size > 0)
		mig_walk(&st, GETQUERY(query));

	for (i = 0; i < st.nitems; i++)
		if (st.items[i].type == WEAVE_QI_VAL)
			textbytes += st.items[i].termlen;

	total = WEAVE_QUERY_HDRSIZE +
		(Size) st.nitems * sizeof(WeaveQueryItem) + textbytes;
	q = (WeaveQuery) palloc0(total);
	SET_VARSIZE(q, total);
	q->version = WEAVE_QUERY_VERSION;
	q->flags = 0;
	q->nitems = st.nitems;

	items = q->items;
	textbase = WEAVE_QUERY_TEXTBASE(q);
	for (i = 0; i < st.nitems; i++)
	{
		items[i].type = st.items[i].type;
		items[i].op = st.items[i].op;
		items[i].flags = 0;
		items[i].distance = st.items[i].distance;
		if (st.items[i].type == WEAVE_QI_VAL)
		{
			items[i].termoff = off;
			items[i].termlen = st.items[i].termlen;
			memcpy(textbase + off, st.items[i].term, st.items[i].termlen);
			off += st.items[i].termlen;
		}
		else
		{
			items[i].termoff = 0;
			items[i].termlen = 0;
		}
	}

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_WQUERY(q);
}
OLDEOF
set -u
OUT=/tmp/out
PGC=/usr/lib/postgresql/17/bin/pg_config
LIB=$($PGC --pkglibdir)
SRC=$HOME/pg_weave
T=tsquery_cast
MUTS="${MUTS:-M1_weight_dropped M2_prefix_dropped M3_phrase_distance_plus_one M4_phrase_N_accepted_as_at_most_N M5_bool_in_phrase_accepted M6_prefix_in_phrase_accepted M7_pre_fix_converter M8_index_not_inexact_reverted}"
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
# run tsquery_cast solo; print the path of its output, or RAN_NOTHING
run_t() {
	(cd "$1" && find results -name "$T.out" -delete 2>/dev/null;
	 touch expected/$T.out;
	 make installcheck PG_CONFIG=$PGC REGRESS=$T ISOLATION= TAP_TESTS= > $OUT/ic-$2.log 2>&1)
	if [ ! -s "$1/results/$T.out" ]; then echo RAN_NOTHING; return; fi
	grep -vF "$NOTICE" "$1/results/$T.out" > $OUT/$T-$2.out
	echo $OUT/$T-$2.out
}
install_tree "$SRC" clean || { log "CONTROL build/install FAILED"; exit 1; }
# DIAGNOSTIC PROBE (not a gate): native wquery fuzzy/regex leaves inside boolean
# structure, heap evaluation vs weave_count.  A disagreement is a finding.
psql -X -d postgres -v ON_ERROR_STOP=1 > $OUT/probe_fuzzy_bool.out 2>&1 <<'PROBE'
DROP DATABASE IF EXISTS g93probe;
CREATE DATABASE g93probe;
\c g93probe
CREATE EXTENSION pg_weave;
SELECT setseed(0.5);
CREATE TABLE p (id int, d wdoc);
INSERT INTO p SELECT g, to_wdoc('simple', (SELECT string_agg((ARRAY['quick','brown','browne','fox','foxes','dog','lazy','jump'])[1 + floor(random()*8)::int], ' ') FROM generate_series(1, 1 + (g % 5)))) FROM generate_series(1, 2000) g;
CREATE INDEX p_ix ON p USING weave (d);
CREATE INDEX p_ix_trgm ON p USING weave (d) WITH (trigrams = on, positions = on);
SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT q, (SELECT count(*) FROM p WHERE d @@@ q::wquery) AS heap,
       weave_count('p_ix', q::wquery) AS ix, weave_count('p_ix_trgm', q::wquery) AS ix_trgm_pos
  FROM (VALUES ('brwn~1'), ('quick | brwn~1'), ('!brwn~1'), ('quick & !brwn~1'), ('!(quick & brwn~1)'),
               ('/fox.*/'), ('dog | /fox.*/'), ('!/fox.*/'), ('dog & !/fox.*/'), ('brwn~1 & /fox.*/'),
               ('brwn~1 | /fox.*/'), ('!(brwn~1 | /fox.*/)'), ('fox:A'), ('!fox:A'), ('"quick brown"'),
               ('!"quick brown"'), ('quick:A | !dog'), ('!(fox:D & dog)')) v(q);
PROBE
echo "probe exit $?" >> $OUT/probe_fuzzy_bool.out
CLEAN_MD5=$(md5sum $LIB/pg_weave.so | cut -d' ' -f1)
c1=$(run_t "$SRC" clean1); c2=$(run_t "$SRC" clean2)
[ "$c1" != RAN_NOTHING ] && [ "$c2" != RAN_NOTHING ] || { log "CONTROL ran nothing"; exit 1; }
n=$(diff $c1 $c2 | wc -l)
log "CONTROL clean tree twice: so=$CLEAN_MD5 diff_lines=$n (must be 0); DIFFERENT rows in clean: $(grep -c DIFFERENT $c1) (must be 0)"
[ "$n" = 0 ] || { log "CONTROL FAILED"; exit 1; }
grep -q 'queries' $c1 || { log "CONTROL output lacks the randomized summary"; exit 1; }
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
	o=$(run_t $D $m)
	if [ "$o" = RAN_NOTHING ]; then log "$m: no results -- not counted"; continue; fi
	diff $c1 $o > $OUT/$T-$m.diff; n=$(wc -l < $OUT/$T-$m.diff)
	if [ "$n" -gt 0 ]; then caught=$((caught+1)); log "$m: BUILT (so=$md5) and CAUGHT ($n diff lines, $(grep -c DIFFERENT $o) DIFFERENT rows)"
	else log "$m: BUILT (so=$md5) and SURVIVED"; fi
	wipe $D
done
install_tree "$SRC" clean-restore >/dev/null 2>&1
log "DONE caught=$caught of $total"
[ $caught = $total ]
