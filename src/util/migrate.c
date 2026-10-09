/*-------------------------------------------------------------------------
 *
 * pg_weave_migrate.c
 *		Migration helpers from the existing tsvector/tsquery stack to pg_weave.
 *
 * Stage 11 of pg_weave.  tsquery_to_wquery() mechanically converts a tsquery
 * into an wquery: & -> AND, | -> OR, ! -> NOT, <-> -> phrase, lexeme:*
 * -> prefix, lexeme:ABCD -> weight-restricted term.  The result answers
 * exactly what core's @@ answers on the same tsvector, or the cast raises
 * feature_not_supported (G93; see mig_walk for the shapes refused).
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

#include "miscadmin.h"
#include "weave/weave.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"

/* An emitted wquery item, collected before flattening. */
typedef struct MigItem
{
	uint8		type;
	uint8		op;
	uint16		flags;			/* WEAVE_QF_* for VAL items */
	uint32		distance;		/* phrase gap, or a VAL's weight mask */
	char	   *term;			/* lexeme text for VAL items */
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
mig_emit(MigState *st, uint8 type, uint8 op, uint16 flags, uint32 distance,
		 char *term, int termlen)
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
	st->items[st->nitems].flags = flags;
	st->items[st->nitems].distance = distance;
	st->items[st->nitems].term = term;
	st->items[st->nitems].termlen = termlen;
	st->nitems++;
}

/*
 * A tsquery shape wquery cannot answer exactly.  Refused rather than converted
 * to something close: a near miss here is a row silently added or dropped
 * (G93), and the caller still has core's @@ for these.
 */
static void
mig_refuse(const char *what, const char *detail)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot convert tsquery to wquery: %s", what),
			 errdetail("%s", detail)));
}

/*
 * Walk the tsquery item at `item`, emitting postfix.  `in_phrase` is true when
 * the item is an operand of a phrase operator, where positions matter.
 *
 * Returns the operand's phrase WIDTH in core's sense (TS_phrase_execute): 0
 * for a lexeme, N + width(left) + width(right) for `left <N> right`.  Core
 * matches `L <N> R` when end(L) + N + width(R) == end(R) -- an EXACT gap
 * between END positions, 0 meaning the same position.  That is emitted as a
 * WEAVE_QF_PHRASE_EXACT phrase with distance N + width(R), which both wquery
 * evaluators answer as end(R) - end(L) == distance, so every chain of lexemes
 * -- <0>, <N>, left- or right-nested, phrase of phrases -- maps exactly.
 * (Without the flag a wquery phrase is a gap of 1..distance, which agrees with
 * <-> only when N + width(R) == 1; the flag is set on every phrase anyway so
 * there is one rule.)
 *
 * Inside a phrase, wquery's &, | and ! drop positions and make the phrase
 * false, while core evaluates them positionally (negated position sets aligned
 * by width, TS_phrase_output); and a prefix lexeme carries no positions in
 * wquery, which makes the phrase permissive.  Both refused.
 */
static int64
mig_walk(MigState *st, QueryItem *item, bool in_phrase)
{
	check_stack_depth();

	if (item->type == QI_VAL)
	{
		QueryOperand *op = &item->qoperand;
		char	   *term = (char *) palloc(Max(op->length, 1));
		uint16		flags = 0;
		uint32		dist = 0;

		/* tsquery lexemes are already normalized: copy verbatim */
		memcpy(term, st->operands + op->distance, op->length);

		if (op->prefix)
		{
			/* wquery's prefix match is presence-only: no zones, no positions */
			if (op->weight != 0)
				mig_refuse("a prefix lexeme with a weight restriction",
						   "A wquery prefix match cannot be restricted to weights.");
			if (in_phrase)
				mig_refuse("a prefix lexeme inside a phrase",
						   "A wquery prefix match carries no positions, so the phrase would match non-adjacent words.");
			flags = WEAVE_QF_PREFIX;
		}
		else if (op->weight != 0 && op->weight != 0xF)
		{
			/*
			 * Same encoding as wquery's mask: bit 3 = A ... bit 0 = D, tested
			 * against the position's label.  All four labels is no restriction
			 * at all, so it stays a plain term.
			 */
			flags = WEAVE_QF_WEIGHTED;
			dist = op->weight;
		}
		mig_emit(st, WEAVE_QI_VAL, 0, flags, dist, term, op->length);
		return 0;
	}
	else if (item->type == QI_OPR)
	{
		QueryOperator *op = &item->qoperator;
		int64		wl,
					wr;

		if (op->oper != OP_PHRASE && in_phrase)
			mig_refuse("&, | or ! inside a phrase",
					   "wquery evaluates a phrase over &, | or ! as false; tsquery evaluates it positionally.");

		if (op->oper == OP_NOT)
		{
			/* NOT has a single (right) operand at item+1 */
			mig_walk(st, item + 1, false);
			mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_NOT, 0, 0, NULL, 0);
			return 0;
		}

		wl = mig_walk(st, item + op->left, op->oper == OP_PHRASE);
		wr = mig_walk(st, item + 1, op->oper == OP_PHRASE);

		switch (op->oper)
		{
			case OP_AND:
				mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_AND, 0, 0, NULL, 0);
				return 0;
			case OP_OR:
				mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_OR, 0, 0, NULL, 0);
				return 0;
			case OP_PHRASE:
				/* widths add up (int64: a long query cannot overflow);
				 * a gap past every position never matches, as in core */
				mig_emit(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE,
						 WEAVE_QF_PHRASE_EXACT,
						 (uint32) Min(op->distance + wr, (int64) PG_UINT32_MAX),
						 NULL, 0);
				return op->distance + wl + wr;
		}
		elog(ERROR, "unrecognized tsquery operator: %d", op->oper);
	}
	elog(ERROR, "unrecognized tsquery item type: %d", item->type);
	return 0;					/* keep compiler quiet */
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
		mig_walk(&st, GETQUERY(query), false);

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
		items[i].flags = st.items[i].flags;
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
