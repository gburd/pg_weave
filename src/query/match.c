/*-------------------------------------------------------------------------
 *
 * pg_weave_match.c
 *		Match evaluation: does an wdoc satisfy an wquery?
 *
 * The query is a postfix (RPN) item list, so evaluation is a stack machine: a
 * term operand pushes "does this doc contain the term", and each operator pops
 * its arguments and pushes the combined result.  Beyond boolean AND/OR/NOT it
 * evaluates phrase and NEAR (using per-term positions), prefix, fuzzy (bounded
 * Levenshtein) and regex operands.  It mirrors tsquery's TS_execute strategy;
 * O(nitems * log nterms) with the binary-search term lookup.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_match.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"

/*
 * Each stack entry is a boolean presence plus, for term and phrase operands, a
 * position list (ascending token positions where the operand ends).  Phrase
 * evaluation intersects a left operand's positions with a right term's
 * positions offset by 1..distance, chaining across a multi-word phrase.
 * Boolean operators (AND/OR/NOT) collapse to presence and drop positions.
 */
typedef struct MatchVal
{
	bool		present;
	uint32	   *pos;			/* NULL if positions unavailable/irrelevant */
	int			npos;
	/*
	 * Why `pos` is NULL, which decides what a phrase over this operand means.
	 * True only for a PREFIX leaf, whose positions are deliberately not tracked
	 * ("phrase-with-prefix is not tracked positionally") -- a shipped, lossy
	 * behaviour we keep.  False everywhere else, including a positionless
	 * document AND a boolean sub-expression (the AND/OR/NOT arms null `pos`),
	 * both of which make adjacency unknowable and so must yield FALSE rather
	 * than degrade to a conjunction.  Checking the document's flag alone is NOT
	 * sufficient: `quick <-> (brown & fox)` reaches phrase_step with positions
	 * nulled on a fully positioned doc.
	 */
	bool		pos_lossy_prefix;
}			MatchVal;

/*
 * Positions where a (possibly prefix) term occurs.  For an exact term this is
 * the stored position list; for a prefix term we merge the position lists of
 * all matching terms (rare, so a simple concat + sort).  If the doc carries no
 * positions, returns present-without-positions.
 */
static MatchVal
term_positions(WeaveDoc doc, const char *term, int termlen, uint16 flags,
			   uint32 distance)
{
	MatchVal	v;
	uint32		wmask = (flags & WEAVE_QF_WEIGHTED) ? distance : 0;

	v.present = false;
	v.pos = NULL;
	v.npos = 0;
	v.pos_lossy_prefix = false;

	if (flags & WEAVE_QF_REGEX)
	{
		v.present = weave_doc_has_regex(doc, term, termlen);
		return v;
	}
	if (flags & WEAVE_QF_FUZZY)
	{
		v.present = weave_doc_has_fuzzy(doc, term, termlen, (int) distance);
		return v;
	}
	if (flags & WEAVE_QF_PREFIX)
	{
		/* presence only; phrase-with-prefix is not tracked positionally.  Mark
		 * the absence as the deliberate prefix lossiness so a phrase over this
		 * operand stays permissive instead of becoming unverifiable-false. */
		v.present = weave_doc_has_prefix(doc, term, termlen);
		v.pos_lossy_prefix = true;
		return v;
	}
	else
	{
		WeaveTermEntry *e = weave_doc_lookup(doc, term, termlen);

		if (e == NULL)
			return v;
		v.present = true;
		if (WEAVE_DOC_HAS_POS(doc))
		{
			v.pos = WEAVE_DOC_TERMPOS(doc, e);
			v.npos = (int) e->tf;
		}
		/*
		 * Weight (field-zone) restriction: the term counts as present only if
		 * it occurs at >= 1 position whose label is in wmask.  A doc with no
		 * positions cannot be zone-filtered -- treat every position as label D
		 * (bit 0), i.e. matches iff the mask includes D.  When positions are
		 * present, narrow v.pos to the in-zone positions (kept as label-bearing
		 * words; phrase_step masks the ordinal) so a weighted phrase still
		 * enforces adjacency over only the in-zone occurrences.
		 */
		if (wmask != 0)
		{
			if (!WEAVE_DOC_HAS_POS(doc))
			{
				/*
				 * Zone labels are carried in each position's high bits
				 * (WEAVE_POS_LABEL), so a document without positions carries no
				 * label information at all -- it is unknown, not "D".  Treating
				 * unlabeled as D meant `term:D` matched EVERY positionless doc
				 * while `term:A` matched none, and a concatenation that dropped
				 * one side's labels (to_wdoc('simple','quick','A') ||
				 * $$'zz':1$$::wdoc) silently answered `term:D` = true.  A
				 * zone restriction we cannot evaluate must not match, for the
				 * same reason an unverifiable phrase must not (see
				 * phrase_step).
				 */
				v.present = false;
			}
			else
			{
				int			j,
							n = 0;
				bool		any = false;

				for (j = 0; j < v.npos; j++)
					if ((wmask & (1u << WEAVE_POS_LABEL(v.pos[j]))) != 0)
						any = true;
				v.present = any;
				/* compact in-zone positions in place (order preserved) */
				if (any)
				{
					uint32	   *keep = (uint32 *) palloc((Size) v.npos * sizeof(uint32));

					for (j = 0; j < v.npos; j++)
						if ((wmask & (1u << WEAVE_POS_LABEL(v.pos[j]))) != 0)
							keep[n++] = v.pos[j];
					v.pos = keep;
					v.npos = n;
				}
				else
				{
					v.pos = NULL;
					v.npos = 0;
					v.pos_lossy_prefix = false;
				}
			}
		}
		return v;
	}
}

/*
 * Phrase step over raw ascending position arrays: return, in out[0..*nout),
 * the right positions p such that some left position L satisfies
 * 0 < p - L <= distance.  out must have room for nright values.  This is the
 * single source of truth for phrase adjacency; both the in-memory matcher
 * (phrase_step) and the index posting-list phrase evaluator use it, so a
 * phrase answered from the postings is byte-identical to the heap recheck.
 */
void
weave_phrase_step_pos(const uint32 *left, int nleft,
					const uint32 *right, int nright,
					uint32 distance, uint32 *out, int *nout)
{
	int			li = 0,
				ri,
				k = 0;

	for (ri = 0; ri < nright; ri++)
	{
		uint32		p = WEAVE_POS_ORD(right[ri]);	/* ordinal only; ignore label bits */

		/* advance li to the first left position that could be in range */
		while (li < nleft && WEAVE_POS_ORD(left[li]) + distance < p)
			li++;
		/* any left position L with p-distance <= L < p works */
		if (li < nleft && WEAVE_POS_ORD(left[li]) < p && p - WEAVE_POS_ORD(left[li]) <= distance)
			out[k++] = right[ri];	/* keep the original (label-bearing) word */
	}
	*nout = k;
}

/*
 * Phrase step: given left positions (ends of the matched-so-far prefix) and
 * the right term's positions, return the right positions p such that some left
 * position L satisfies 0 < p - L <= distance.  Both inputs are ascending.
 */
static MatchVal
phrase_step(MatchVal left, MatchVal right, uint32 distance)
{
	MatchVal	r;

	r.present = false;
	r.pos = NULL;
	r.npos = 0;
	r.pos_lossy_prefix = false;

	/*
	 * Either side may lack positions, and the right answer depends on WHY.
	 *
	 * (a) The DOCUMENT carries no positions at all.  Adjacency is unknowable,
	 * so the phrase is FALSE.  This matches PostgreSQL: without
	 * TS_EXEC_PHRASE_NO_POS, "OP_PHRASE always returns false if lexeme
	 * position information is not available" (tsearch/ts_utils.h), and
	 * `strip(to_tsvector('simple','quick brown')) @@ 'quick <-> brown'` is
	 * false upstream even though the words ARE adjacent.  We claim to mirror
	 * TS_execute (see this file's header), so we must not diverge here.
	 *
	 * This branch is reached by more than a positionless document: the
	 * boolean arms below null out positions, so a boolean sub-expression
	 * under a phrase (`quick <-> (brown & fox)`, reachable via the tsquery
	 * cast) lands here even on a fully positioned doc.  Returning
	 * presence-only AND there answered `t` where PostgreSQL answers `f`.
	 *
	 * (b) The operand is a PREFIX leaf, whose positions are deliberately not
	 * tracked (see weave_doc_term_val: "phrase-with-prefix is not tracked
	 * positionally").  That is a shipped, documented lossiness -- `"quick bro*"`
	 * is over-permissive by design -- so keep presence-only there rather than
	 * silently narrowing a feature people may rely on.
	 *
	 * Only the operand's own `pos_lossy_prefix` distinguishes these.  The
	 * DOCUMENT's WEAVE_DOC_HAS_POS flag does NOT: the boolean case above arrives
	 * here with pos == NULL on a fully positioned document.
	 */
	if (left.pos == NULL || right.pos == NULL)
	{
		bool		lossy = (left.pos == NULL && left.pos_lossy_prefix) ||
			(right.pos == NULL && right.pos_lossy_prefix);
		bool		unknowable = (left.pos == NULL && !left.pos_lossy_prefix) ||
			(right.pos == NULL && !right.pos_lossy_prefix);

		if (unknowable)
			return r;			/* (a) -> false, as PostgreSQL does */
		if (lossy)
			r.present = left.present && right.present;	/* (b) prefix */
		return r;
	}

	r.pos = (uint32 *) palloc(right.npos * sizeof(uint32));
	weave_phrase_step_pos(left.pos, left.npos, right.pos, right.npos,
						distance, r.pos, &r.npos);
	r.present = (r.npos > 0);
	return r;
}

bool
weave_doc_matches(WeaveDoc doc, WeaveQuery query)
{
	WeaveQueryItem *items = query->items;
	MatchVal   *stack;
	int			top = 0;
	uint32		i;
	bool		result;

	/* An empty query matches nothing (there is no positive evidence). */
	if (query->nitems == 0)
		return false;

	stack = (MatchVal *) palloc(query->nitems * sizeof(MatchVal));

	for (i = 0; i < query->nitems; i++)
	{
		WeaveQueryItem *it = &items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			stack[top++] = term_positions(doc, WEAVE_QUERY_ITEMTEXT(query, it),
										  it->termlen, it->flags,
										  it->distance);
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			Assert(top >= 1);
			stack[top - 1].present = !stack[top - 1].present;
			stack[top - 1].pos = NULL;
			stack[top - 1].pos_lossy_prefix = false;
			stack[top - 1].npos = 0;
		}
		else if (it->op == WEAVE_OP_PHRASE)
		{
			Assert(top >= 2);
			stack[top - 2] = phrase_step(stack[top - 2], stack[top - 1],
										 it->distance);
			top--;
		}
		else if (it->op == WEAVE_OP_AND)
		{
			Assert(top >= 2);
			stack[top - 2].present = stack[top - 2].present && stack[top - 1].present;
			stack[top - 2].pos = NULL;
			stack[top - 2].pos_lossy_prefix = false;
			stack[top - 2].npos = 0;
			top--;
		}
		else					/* WEAVE_OP_OR */
		{
			Assert(top >= 2);
			stack[top - 2].present = stack[top - 2].present || stack[top - 1].present;
			stack[top - 2].pos = NULL;
			stack[top - 2].pos_lossy_prefix = false;
			stack[top - 2].npos = 0;
			top--;
		}
	}

	Assert(top == 1);
	result = stack[0].present;
	pfree(stack);
	return result;
}

PG_FUNCTION_INFO_V1(weave_match);

/* wdoc @@@ wquery -> bool */
Datum
weave_match(PG_FUNCTION_ARGS)
{
	WeaveDoc		doc = PG_GETARG_WDOC(0);
	WeaveQuery	query = PG_GETARG_WQUERY(1);
	bool		res;

	res = weave_doc_matches(doc, query);

	PG_FREE_IF_COPY(doc, 0);
	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}

PG_FUNCTION_INFO_V1(weave_match_commutator);

/* wquery @@@ wdoc -> bool (commutator) */
Datum
weave_match_commutator(PG_FUNCTION_ARGS)
{
	WeaveQuery	query = PG_GETARG_WQUERY(0);
	WeaveDoc		doc = PG_GETARG_WDOC(1);
	bool		res;

	res = weave_doc_matches(doc, query);

	PG_FREE_IF_COPY(query, 0);
	PG_FREE_IF_COPY(doc, 1);
	PG_RETURN_BOOL(res);
}
