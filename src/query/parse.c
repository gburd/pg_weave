/*-------------------------------------------------------------------------
 *
 * pg_weave_query.c
 *		Query-text parser and I/O for the wquery type.
 *
 * Stage-1 query grammar (recursive descent, no generator -- the grammar is
 * small and this keeps the extension self-contained):
 *
 *	  expr    := or_expr
 *	  or_expr := and_expr ( ('|' | 'OR') and_expr )*
 *	  and_expr:= unary ( ('&' | 'AND')? unary )*        -- implicit AND
 *	  unary   := ('!' | 'NOT' | '-') unary | primary
 *	  primary := '(' expr ')' | term
 *	  term    := run of token bytes (folded like the analyzer)
 *
 * The parser emits a postfix (RPN) item list, the same shape tsquery uses, so
 * evaluation is a simple stack machine.  Supported: AND, OR, NOT, parenthesised
 * grouping, phrase ("..."), NEAR, prefix (term*), fuzzy (term~k) and regex
 * (/re/); field scoping (field:term) and boosts remain future item kinds, which
 * the version field lets us add without breaking the on-disk format.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_query.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"

/* An operand collected during the parse, before flattening to a varlena. */
typedef struct ParsedItem
{
	uint8		type;			/* WeaveQueryItemType */
	uint8		op;				/* WeaveQueryOp when type == WEAVE_QI_OPR */
	uint16		flags;			/* WEAVE_QF_* for VAL items */
	uint32		distance;		/* WEAVE_OP_PHRASE gap */
	char	   *term;			/* palloc'd folded term when type == WEAVE_QI_VAL */
	int			termlen;
} ParsedItem;

/* Token kinds returned by the lexer. */
typedef enum
{
	TOK_EOF,
	TOK_TERM,
	TOK_AND,
	TOK_OR,
	TOK_NOT,
	TOK_LPAREN,
	TOK_RPAREN,
	TOK_QUOTE,					/* " -- starts/ends a phrase */
	TOK_NEAR,					/* NEAR keyword (proximity) */
	TOK_COMMA					/* , inside NEAR(...) */
} TokKind;

typedef struct Token
{
	TokKind		kind;
	char	   *term;			/* folded term text for TOK_TERM */
	int			termlen;
	bool		prefix;			/* TOK_TERM followed by '*' */
	int			fuzzy_k;		/* TOK_TERM followed by ~k (0 = not fuzzy) */
	bool		regex;			/* TOK_TERM holds a regex (from /.../ ) */
	uint32		weightmask;		/* TOK_TERM followed by :ABCD -> label mask (0 = none) */
} Token;

typedef struct ParseState
{
	const char *buf;
	int			len;
	int			pos;
	ParsedItem *items;
	int			nitems;
	int			maxitems;
	bool		error;
	bool		have_peeked;	/* is peeked valid? */
	Token		peeked;			/* one-token lookahead cache */
} ParseState;

static void parse_or(ParseState *st);
static void emit_dist(ParseState *st, uint8 type, uint8 op, char *term,
					  int termlen, uint16 flags, uint32 distance);

static inline bool
is_token_byte(unsigned char c)
{
	if (c >= 0x80)
		return true;
	return (c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9');
}

static void
emit(ParseState *st, uint8 type, uint8 op, char *term, int termlen,
	 uint16 flags)
{
	emit_dist(st, type, op, term, termlen, flags, 0);
}

static void
emit_dist(ParseState *st, uint8 type, uint8 op, char *term, int termlen,
		  uint16 flags, uint32 distance)
{
	if (st->nitems >= st->maxitems)
	{
		st->maxitems = st->maxitems ? st->maxitems * 2 : 16;
		if (st->items == NULL)
			st->items = (ParsedItem *) palloc(st->maxitems * sizeof(ParsedItem));
		else
			st->items = (ParsedItem *) repalloc(st->items,
												st->maxitems * sizeof(ParsedItem));
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
 * Raw lexer.  Recognizes &, |, !, - and parentheses as punctuation; the
 * keywords AND/OR/NOT (case-insensitive) as operators; everything else is a
 * term.  A bare "and"/"or"/"not" is treated as an operator only when it stands
 * alone as a token, which is the standard, least-surprising behavior.
 *
 * Callers use next_token()/peek() rather than calling this directly, so that a
 * peeked token is lexed (and its term palloc'd) exactly once.
 */
static Token
lex_raw(ParseState *st)
{
	Token		tok = {TOK_EOF, NULL, 0, false, 0, false};
	int			start;
	int			flen;
	char	   *folded;

	/* skip whitespace */
	while (st->pos < st->len &&
		   !is_token_byte((unsigned char) st->buf[st->pos]))
	{
		char		c = st->buf[st->pos];

		switch (c)
		{
			case '&':
				st->pos++;
				tok.kind = TOK_AND;
				return tok;
			case '|':
				st->pos++;
				tok.kind = TOK_OR;
				return tok;
			case '!':
			case '-':
				st->pos++;
				tok.kind = TOK_NOT;
				return tok;
			case '(':
				st->pos++;
				tok.kind = TOK_LPAREN;
				return tok;
			case ')':
				st->pos++;
				tok.kind = TOK_RPAREN;
				return tok;
			case ',':
				st->pos++;
				tok.kind = TOK_COMMA;
				return tok;
			case '"':
				st->pos++;
				tok.kind = TOK_QUOTE;
				return tok;
			case '/':
				{
					/* /regex/ : read until the closing slash (not folded) */
					int			rstart;
					int			rlen;
					char	   *rbuf;

					st->pos++;
					rstart = st->pos;
					while (st->pos < st->len && st->buf[st->pos] != '/')
						st->pos++;
					rlen = st->pos - rstart;
					if (st->pos < st->len)
						st->pos++;	/* consume closing slash */
					else
					{
						tok.kind = TOK_EOF;	/* unterminated regex */
						return tok;
					}
					rbuf = (char *) palloc(rlen);
					memcpy(rbuf, st->buf + rstart, rlen);
					tok.kind = TOK_TERM;
					tok.term = rbuf;
					tok.termlen = rlen;
					tok.regex = true;
					return tok;
				}
			default:
				st->pos++;		/* ordinary separator */
				break;
		}
	}
	if (st->pos >= st->len)
		return tok;				/* TOK_EOF */

	/* a term: run of token bytes, folded */
	start = st->pos;
	while (st->pos < st->len &&
		   is_token_byte((unsigned char) st->buf[st->pos]))
		st->pos++;
	flen = st->pos - start;

	/* fold identically to the document analyzer; folded length may differ from
	 * the raw run under Unicode lowercasing, so keyword checks use flen after. */
	folded = fold_token(st->buf + start, flen, &flen);

	/* keyword recognition (ASCII, case already folded).  Keyword tokens ALSO
	 * carry their folded text so a phrase/NEAR operand context can treat them as
	 * literal terms (a bare `and`/`or`/`not`/`near` inside "..." or NEAR(...) is a
	 * word, not an operator -- matching to_tsquery, which lexes them as lexemes). */
	if (flen == 3 && memcmp(folded, "and", 3) == 0)
	{
		tok.kind = TOK_AND;
		tok.term = folded;
		tok.termlen = flen;
	}
	else if (flen == 2 && memcmp(folded, "or", 2) == 0)
	{
		tok.kind = TOK_OR;
		tok.term = folded;
		tok.termlen = flen;
	}
	else if (flen == 3 && memcmp(folded, "not", 3) == 0)
	{
		tok.kind = TOK_NOT;
		tok.term = folded;
		tok.termlen = flen;
	}
	else if (flen == 4 && memcmp(folded, "near", 4) == 0)
	{
		tok.kind = TOK_NEAR;
		tok.term = folded;
		tok.termlen = flen;
	}
	else
	{
		tok.kind = TOK_TERM;
		tok.term = folded;
		tok.termlen = flen;
		/* a trailing '*' marks a prefix term */
		if (st->pos < st->len && st->buf[st->pos] == '*')
		{
			tok.prefix = true;
			st->pos++;
		}
		/* a trailing '~k' marks a fuzzy term (k defaults to 2) */
		else if (st->pos < st->len && st->buf[st->pos] == '~')
		{
			int			k = 0;
			bool		havedigit = false;

			st->pos++;
			while (st->pos < st->len &&
				   st->buf[st->pos] >= '0' && st->buf[st->pos] <= '9')
			{
				k = k * 10 + (st->buf[st->pos] - '0');
				havedigit = true;
				st->pos++;
			}
			tok.fuzzy_k = havedigit ? Max(k, 1) : 2;
		}
		/*
		 * A trailing ':' followed by a run of weight labels A/B/C/D (any case)
		 * restricts the term to those field zones (tsquery-style term:A).  Sets
		 * a 4-bit mask (bit L for label L in 0..3 = D,C,B,A).  Applies to plain,
		 * prefix, and fuzzy terms; a regex term already consumed its slashes.
		 */
		if (st->pos < st->len && st->buf[st->pos] == ':')
		{
			int			p = st->pos + 1;
			uint32		mask = 0;

			while (p < st->len)
			{
				char		c = st->buf[p];

				if (c == 'A' || c == 'a')
					mask |= 1u << 3;
				else if (c == 'B' || c == 'b')
					mask |= 1u << 2;
				else if (c == 'C' || c == 'c')
					mask |= 1u << 1;
				else if (c == 'D' || c == 'd')
					mask |= 1u << 0;
				else
					break;
				p++;
			}
			/* only consume the ':LABELS' if it was a valid non-empty label run */
			if (mask != 0)
			{
				/* A weight label cannot combine with a prefix/fuzzy suffix in this
				 * release (those are presence-only, no per-occurrence positions to
				 * zone-filter).  Reject term:A* / term:A~k as a syntax error rather
				 * than silently dropping the * / ~k. */
				if (p < st->len && (st->buf[p] == '*' || st->buf[p] == '~'))
				{
					tok.kind = TOK_TERM;
					tok.term = folded;
					tok.termlen = flen;
					tok.weightmask = mask;
					tok.prefix = (st->buf[p] == '*');
					tok.fuzzy_k = (st->buf[p] == '~') ? 2 : 0;
					st->pos = p + 1;
					/* parser sees weightmask + prefix/fuzzy flag together -> error */
					return tok;
				}
				tok.weightmask = mask;
				st->pos = p;
			}
		}
	}
	return tok;
}

/*
 * next_token -- consume and return the next token, using the one-token
 * lookahead cache if a peek() filled it.
 */
static Token
next_token(ParseState *st)
{
	if (st->have_peeked)
	{
		st->have_peeked = false;
		return st->peeked;
	}
	return lex_raw(st);
}

/* peek -- return the next token without consuming it (lexed at most once) */
static Token
peek(ParseState *st)
{
	if (!st->have_peeked)
	{
		st->peeked = lex_raw(st);
		st->have_peeked = true;
	}
	return st->peeked;
}

/* primary := '(' expr ')' | '"' term+ '"' | term */
static void
parse_primary(ParseState *st)
{
	Token		tok = next_token(st);

	if (tok.kind == TOK_LPAREN)
	{
		parse_or(st);
		tok = next_token(st);
		if (tok.kind != TOK_RPAREN)
			st->error = true;
	}
	else if (tok.kind == TOK_QUOTE)
	{
		/* phrase: emit the terms and join consecutive pairs with PHRASE(1) */
		int			nterms = 0;

		for (;;)
		{
			Token		p = next_token(st);

			if (p.kind == TOK_QUOTE)
				break;
			/* inside "...", a bare and/or/not/near is a literal word, not an
			 * operator: accept keyword tokens (they carry their folded text). */
			if (p.kind != TOK_TERM && p.kind != TOK_AND && p.kind != TOK_OR &&
				p.kind != TOK_NOT && p.kind != TOK_NEAR)
			{
				st->error = true;
				break;
			}
			if (p.term == NULL)	/* defensive: only real punctuation lacks text */
			{
				st->error = true;
				break;
			}
			emit(st, WEAVE_QI_VAL, 0, p.term, p.termlen,
				 p.prefix ? WEAVE_QF_PREFIX : 0);
			if (nterms > 0)
				emit_dist(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, NULL, 0, 0, 1);
			nterms++;
		}
		if (nterms == 0)
			st->error = true;	/* empty phrase "" */
	}
	else if (tok.kind == TOK_NEAR)
	{
		/* NEAR( term term ... , k ) : proximity within k tokens */
		int			nterms = 0;
		uint32		dist = 0;
		Token		p;

		p = next_token(st);
		if (p.kind != TOK_LPAREN)
		{
			st->error = true;
			return;
		}
		/* terms up to the comma */
		for (;;)
		{
			p = peek(st);
			if (p.kind == TOK_COMMA || p.kind == TOK_RPAREN ||
				p.kind == TOK_EOF)
				break;
			p = next_token(st);
			/* inside NEAR(...), and/or/not/near are literal words (they carry
			 * their folded text), not operators. */
			if ((p.kind != TOK_TERM && p.kind != TOK_AND && p.kind != TOK_OR &&
				 p.kind != TOK_NOT && p.kind != TOK_NEAR) || p.term == NULL)
			{
				st->error = true;
				return;
			}
			emit(st, WEAVE_QI_VAL, 0, p.term, p.termlen,
				 p.prefix ? WEAVE_QF_PREFIX : 0);
			nterms++;
		}
		/* optional ", k" (k defaults to 10 like FTS5 when omitted) */
		p = next_token(st);
		if (p.kind == TOK_COMMA)
		{
			Token		kt = next_token(st);
			int			j;

			if (kt.kind != TOK_TERM)
			{
				st->error = true;
				return;
			}
			for (j = 0; j < kt.termlen; j++)
			{
				if (kt.term[j] < '0' || kt.term[j] > '9')
				{
					st->error = true;
					return;
				}
				dist = dist * 10 + (kt.term[j] - '0');
			}
			p = next_token(st);
		}
		else
			dist = 10;			/* NEAR default proximity */
		if (p.kind != TOK_RPAREN)
		{
			st->error = true;
			return;
		}
		if (nterms < 2 || dist < 1)
		{
			st->error = true;	/* NEAR needs >=2 terms and k>=1 */
			return;
		}
		/* join the nterms operands with PHRASE(dist): nterms-1 operators */
		{
			int			m;

			for (m = 1; m < nterms; m++)
				emit_dist(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, NULL, 0, 0, dist);
		}
	}
	else if (tok.kind == TOK_TERM)
	{
		uint16		f = 0;
		uint32		dist;

		if (tok.regex)
			f = WEAVE_QF_REGEX;
		else if (tok.fuzzy_k > 0)
			f = WEAVE_QF_FUZZY;
		else if (tok.prefix)
			f = WEAVE_QF_PREFIX;
		dist = (tok.fuzzy_k > 0) ? (uint32) tok.fuzzy_k : 0;
		if (tok.weightmask != 0)
		{
			/* Weight (field-zone) restriction is supported on PLAIN terms only in
			 * this release: a plain VAL never uses `distance` for a gap, so the
			 * label mask rides there.  prefix/fuzzy/regex are presence-only in the
			 * matcher (no per-occurrence positions), so a weight on them cannot be
			 * enforced -- reject rather than silently ignore the label. */
			if (f != 0)
			{
				st->error = true;
				return;
			}
			f = WEAVE_QF_WEIGHTED;
			dist = tok.weightmask;
		}
		emit_dist(st, WEAVE_QI_VAL, 0, tok.term, tok.termlen, f, dist);
	}
	else
	{
		st->error = true;
	}
}

/* unary := NOT unary | primary */
static void
parse_unary(ParseState *st)
{
	Token		tok = peek(st);

	if (tok.kind == TOK_NOT)
	{
		(void) next_token(st);
		parse_unary(st);
		emit(st, WEAVE_QI_OPR, WEAVE_OP_NOT, NULL, 0, 0);
	}
	else
		parse_primary(st);
}

/* and_expr := unary ( AND? unary )*  (implicit AND between adjacent terms) */
static void
parse_and(ParseState *st)
{
	parse_unary(st);
	for (;;)
	{
		Token		tok = peek(st);

		if (tok.kind == TOK_AND)
		{
			(void) next_token(st);
			parse_unary(st);
			emit(st, WEAVE_QI_OPR, WEAVE_OP_AND, NULL, 0, 0);
		}
		else if (tok.kind == TOK_TERM || tok.kind == TOK_NOT ||
				 tok.kind == TOK_LPAREN || tok.kind == TOK_QUOTE ||
				 tok.kind == TOK_NEAR)
		{
			/* implicit AND */
			parse_unary(st);
			emit(st, WEAVE_QI_OPR, WEAVE_OP_AND, NULL, 0, 0);
		}
		else
			break;
	}
}

/* or_expr := and_expr ( OR and_expr )* */
static void
parse_or(ParseState *st)
{
	parse_and(st);
	for (;;)
	{
		Token		tok = peek(st);

		if (tok.kind == TOK_OR)
		{
			(void) next_token(st);
			parse_and(st);
			emit(st, WEAVE_QI_OPR, WEAVE_OP_OR, NULL, 0, 0);
		}
		else
			break;
	}
}

/*
 * Stopword-aware query normalization.
 *
 * to_wdoc() drops configuration stopwords from the document, so a query term
 * that is a stopword can never match a stored lexeme.  Standard PostgreSQL FTS
 * drops stopwords from BOTH sides (to_tsquery('english','the & x') -> 'x'), so
 * a stopword conjunct must be ELIDED from the query, not left as an
 * unsatisfiable term (which silently zeroes an AND).  We do that here by
 * building a small tree from the parsed RPN, marking each plain term that
 * normalizes away (weave_normalize_term returns NULL) as empty, and simplifying:
 *
 *   X & empty -> X      empty & X -> X       (AND drops the stopword side)
 *   X | empty -> X      empty | X -> X       (OR likewise; matches to_tsquery)
 *   !empty    -> empty                       (nothing to negate)
 *   X <-> empty / empty <-> X -> X           (phrase keeps the real operand;
 *                                             the adjacency gap is lost, but the
 *                                             query never becomes unsatisfiable)
 *   empty (op) empty -> empty
 *
 * A query that reduces entirely to empty yields a 0-item wquery, which
 * matches nothing -- consistent with to_tsquery('english','the') = '' @@ ... .
 * Prefix/fuzzy/regex terms are never stopwords (matched literally), so they are
 * never marked empty.
 */
typedef struct QNode
{
	bool		empty;			/* subtree elided (all-stopword) */
	int			item;			/* index into items[] for a VAL leaf, else -1 */
	uint8		op;				/* WEAVE_OP_* for an internal node */
	uint32		distance;		/* phrase gap */
	struct QNode *left;
	struct QNode *right;		/* NULL for NOT (unary, uses left) */
} QNode;

/* Pop the RPN in items[0..n) into a tree.  *pos walks from the end. */
static QNode *
qnode_build(ParsedItem *items, int *pos)
{
	QNode	   *n;

	if (*pos < 0)
		return NULL;
	n = (QNode *) palloc0(sizeof(QNode));
	n->item = -1;
	if (items[*pos].type == WEAVE_QI_VAL)
	{
		n->item = *pos;
		(*pos)--;
		return n;
	}
	n->op = items[*pos].op;
	n->distance = items[*pos].distance;
	(*pos)--;
	if (n->op == WEAVE_OP_NOT)
		n->left = qnode_build(items, pos);		/* unary */
	else
	{
		n->right = qnode_build(items, pos);		/* RPN top is the right operand */
		n->left = qnode_build(items, pos);
	}
	return n;
}

/* Simplify a tree in place, folding away empty (stopword) subtrees. */
static QNode *
qnode_simplify(QNode *n)
{
	if (n == NULL)
		return NULL;
	if (n->item >= 0)
		return n;					/* leaf: emptiness marked by the caller */
	n->left = qnode_simplify(n->left);
	n->right = qnode_simplify(n->right);
	if (n->op == WEAVE_OP_NOT)
	{
		if (n->left == NULL || n->left->empty)
			n->empty = true;
		return n;
	}
	{
		bool		le = (n->left == NULL || n->left->empty);
		bool		re = (n->right == NULL || n->right->empty);

		if (le && re)
		{
			n->empty = true;
			return n;
		}
		if (le)
			return n->right;
		if (re)
			return n->left;
		return n;
	}
}

/* Flatten a simplified tree back into RPN in out[]; advances *k. */
static void
qnode_flatten(QNode *n, ParsedItem *src, ParsedItem *out, int *k)
{
	if (n == NULL || n->empty)
		return;
	if (n->item >= 0)
	{
		out[*k] = src[n->item];
		(*k)++;
		return;
	}
	qnode_flatten(n->left, src, out, k);
	if (n->op != WEAVE_OP_NOT)
		qnode_flatten(n->right, src, out, k);
	out[*k].type = WEAVE_QI_OPR;
	out[*k].op = n->op;
	out[*k].flags = 0;
	out[*k].distance = n->distance;
	out[*k].term = NULL;
	out[*k].termlen = 0;
	(*k)++;
}

/*
 * weave_parse_query -- parse query text into an WeaveQuery varlena.
 * Raises an error on malformed input.  An input with no terms yields a valid
 * empty query (matches nothing).
 *
 * If cfgId is a valid text-search config, each plain term is normalized through
 * that config (stemming, case, stopwords) so it matches the same lexemes the
 * document index stores.  Prefix (term*), fuzzy (term~k) and regex (/re/) terms
 * are left literal -- they are matched against raw stored lexemes, not stemmed.
 * cfgId == InvalidOid keeps the raw folded term (the simple analyzer path).
 */
WeaveQuery
weave_parse_query_cfg(const char *str, int len, Oid cfgId)
{
	ParseState	st;
	WeaveQuery	q;
	WeaveQueryItem *items;
	char	   *textbase;
	Size		textbytes = 0;
	Size		total;
	uint32		off = 0;
	int			i;

	st.buf = str;
	st.len = len;
	st.pos = 0;
	st.items = NULL;
	st.nitems = 0;
	st.maxitems = 0;
	st.error = false;
	st.have_peeked = false;

	/* Only parse if there is at least one token; else empty query. */
	if (peek(&st).kind != TOK_EOF)
	{
		parse_or(&st);
		if (!st.error && peek(&st).kind != TOK_EOF)
			st.error = true;	/* trailing garbage */
	}

	if (st.error)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("syntax error in wquery: \"%.*s\"", len, str)));

	/*
	 * Normalize plain terms through the text-search config so they match the
	 * document index's stemmed lexemes.  Prefix/fuzzy/regex terms stay literal.
	 */
	if (OidIsValid(cfgId))
	{
		bool	   *stopword = (bool *) palloc0(sizeof(bool) * Max(st.nitems, 1));
		bool		any_stop = false;

		for (i = 0; i < st.nitems; i++)
		{
			if (st.items[i].type == WEAVE_QI_VAL &&
				!(st.items[i].flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX)))
			{
				int			nlen;
				char	   *norm = weave_normalize_term(cfgId, st.items[i].term,
														 st.items[i].termlen, &nlen);

				if (norm != NULL)
				{
					st.items[i].term = norm;
					st.items[i].termlen = nlen;
				}
				else
				{
					/* stopword: mark for elision so it does not zero an AND */
					stopword[i] = true;
					any_stop = true;
				}
			}
		}

		/*
		 * Elide stopword terms: build the RPN into a tree, mark stopword leaves
		 * empty, simplify (drop empty operands + their operators), flatten back.
		 */
		if (any_stop && st.nitems > 0)
		{
			int			pos = st.nitems - 1;
			QNode	   *root = qnode_build(st.items, &pos);
			QNode	  **stack = (QNode **) palloc(sizeof(QNode *) * st.nitems);
			int			sp = 0;

			if (root)
				stack[sp++] = root;
			while (sp > 0)
			{
				QNode	   *nd = stack[--sp];

				if (nd->item >= 0)
					nd->empty = stopword[nd->item];
				else
				{
					if (nd->left)
						stack[sp++] = nd->left;
					if (nd->right)
						stack[sp++] = nd->right;
				}
			}
			root = qnode_simplify(root);
			{
				ParsedItem *out = (ParsedItem *) palloc(sizeof(ParsedItem) * st.nitems);
				int			k = 0;

				qnode_flatten(root, st.items, out, &k);
				for (i = 0; i < k; i++)
					st.items[i] = out[i];
				st.nitems = k;
			}
		}
	}

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

	return q;
}

/* raw parse (no config normalization) -- the simple analyzer / wquery_in path */
WeaveQuery
weave_parse_query(const char *str, int len)
{
	return weave_parse_query_cfg(str, len, InvalidOid);
}

PG_FUNCTION_INFO_V1(wquery_in);
PG_FUNCTION_INFO_V1(wquery_out);
PG_FUNCTION_INFO_V1(wquery_recv);
PG_FUNCTION_INFO_V1(wquery_send);
PG_FUNCTION_INFO_V1(to_wquery);
PG_FUNCTION_INFO_V1(to_wquery_byid);

Datum
wquery_in(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);

	PG_RETURN_WQUERY(weave_parse_query(in, strlen(in)));
}

Datum
to_wquery(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	WeaveQuery	q;

	q = weave_parse_query(VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in));
	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_WQUERY(q);
}

/* to_wquery(regconfig, text): parse and normalize terms through the config */
Datum
to_wquery_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *in = PG_GETARG_TEXT_PP(1);
	WeaveQuery	q;

	q = weave_parse_query_cfg(VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in), cfgId);
	PG_FREE_IF_COPY(in, 1);
	PG_RETURN_WQUERY(q);
}

/*
 * Render an wquery as fully parenthesised infix for display/debugging.  (Note
 * the phrase operator prints as ` <-> `, which the query lexer does not accept
 * as input -- the rendering is human-readable, not a guaranteed round-trip.)
 * Postfix RPN is walked with a small string stack.
 */
Datum
wquery_out(PG_FUNCTION_ARGS)
{
	WeaveQuery	q = PG_GETARG_WQUERY(0);
	WeaveQueryItem *items = q->items;
	StringInfoData *stack;
	int			top = 0;
	uint32		i;
	StringInfoData result;

	if (q->nitems == 0)
	{
		PG_FREE_IF_COPY(q, 0);
		PG_RETURN_CSTRING(pstrdup(""));
	}

	stack = (StringInfoData *) palloc(q->nitems * sizeof(StringInfoData));

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			StringInfoData s;
			int			j;
			const char *t = WEAVE_QUERY_ITEMTEXT(q, it);

			initStringInfo(&s);
			if (it->flags & WEAVE_QF_REGEX)
			{
				appendStringInfoChar(&s, '/');
				appendBinaryStringInfo(&s, t, it->termlen);
				appendStringInfoChar(&s, '/');
				stack[top++] = s;
				continue;
			}
			appendStringInfoChar(&s, '\'');
			for (j = 0; j < (int) it->termlen; j++)
			{
				if (t[j] == '\'' || t[j] == '\\')
					appendStringInfoChar(&s, '\\');
				appendStringInfoChar(&s, t[j]);
			}
			appendStringInfoChar(&s, '\'');
			if (it->flags & WEAVE_QF_PREFIX)
				appendStringInfoChar(&s, '*');
			else if (it->flags & WEAVE_QF_FUZZY)
				appendStringInfo(&s, "~%u", it->distance);
			else if (it->flags & WEAVE_QF_WEIGHTED)
			{
				/* render the weight mask as :A/B/C/D (high labels first) */
				appendStringInfoChar(&s, ':');
				if (it->distance & (1u << 3)) appendStringInfoChar(&s, 'A');
				if (it->distance & (1u << 2)) appendStringInfoChar(&s, 'B');
				if (it->distance & (1u << 1)) appendStringInfoChar(&s, 'C');
				if (it->distance & (1u << 0)) appendStringInfoChar(&s, 'D');
			}
			stack[top++] = s;
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			StringInfoData s;

			Assert(top >= 1);
			initStringInfo(&s);
			appendStringInfoString(&s, "!");
			appendBinaryStringInfo(&s, stack[top - 1].data,
								   stack[top - 1].len);
			pfree(stack[top - 1].data);
			stack[top - 1] = s;
		}
		else
		{
			StringInfoData s;
			const char *opstr;

			switch (it->op)
			{
				case WEAVE_OP_AND:
					opstr = " & ";
					break;
				case WEAVE_OP_OR:
					opstr = " | ";
					break;
				case WEAVE_OP_PHRASE:
				default:
					opstr = " <-> ";
					break;
			}
			Assert(top >= 2);
			initStringInfo(&s);
			appendStringInfoChar(&s, '(');
			appendBinaryStringInfo(&s, stack[top - 2].data,
								   stack[top - 2].len);
			appendStringInfoString(&s, opstr);
			appendBinaryStringInfo(&s, stack[top - 1].data,
								   stack[top - 1].len);
			appendStringInfoChar(&s, ')');
			pfree(stack[top - 1].data);
			pfree(stack[top - 2].data);
			top -= 2;
			stack[top++] = s;
		}
	}

	Assert(top == 1);
	initStringInfo(&result);
	appendBinaryStringInfo(&result, stack[0].data, stack[0].len);
	pfree(stack[0].data);

	PG_FREE_IF_COPY(q, 0);
	PG_RETURN_CSTRING(result.data);
}

Datum
wquery_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint16		version;
	uint32		nitems;
	WeaveQuery	q;
	WeaveQueryItem *items;
	char	   *textbase;
	uint8	   *types;
	uint8	   *ops;
	uint16	   *flags;
	uint32	   *dists;
	char	  **terms;
	int		   *lens;
	Size		textbytes = 0;
	uint32		off = 0;
	uint32		i;

	version = (uint16) pq_getmsgint(buf, 2);
	if (version != WEAVE_QUERY_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("unsupported wquery version number %u", version)));

	nitems = (uint32) pq_getmsgint(buf, 4);

	/*
	 * Guard against a hostile/corrupt binary message: each item is at least a
	 * few fixed bytes (type+op+flags+distance), so nitems cannot exceed the
	 * remaining bytes / 8.  Rejects absurd counts before palloc (overflow /
	 * OOM at a trust boundary).
	 */
	if (nitems > (uint32) (buf->len - buf->cursor) / 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid wquery: item count %u exceeds message size", nitems)));

	types = (uint8 *) palloc(nitems * sizeof(uint8));
	ops = (uint8 *) palloc(nitems * sizeof(uint8));
	flags = (uint16 *) palloc(nitems * sizeof(uint16));
	dists = (uint32 *) palloc(nitems * sizeof(uint32));
	terms = (char **) palloc(nitems * sizeof(char *));
	lens = (int *) palloc(nitems * sizeof(int));

	for (i = 0; i < nitems; i++)
	{
		types[i] = (uint8) pq_getmsgint(buf, 1);
		ops[i] = (uint8) pq_getmsgint(buf, 1);
		flags[i] = (uint16) pq_getmsgint(buf, 2);
		dists[i] = (uint32) pq_getmsgint(buf, 4);
		if (types[i] == WEAVE_QI_VAL)
		{
			const char *t;

			lens[i] = pq_getmsgint(buf, 4);
			if (lens[i] < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("invalid wquery term length")));
			t = pq_getmsgbytes(buf, lens[i]);
			terms[i] = (char *) palloc(lens[i]);
			memcpy(terms[i], t, lens[i]);
			textbytes += lens[i];
		}
		else
		{
			if (types[i] != WEAVE_QI_OPR ||
				(ops[i] != WEAVE_OP_NOT && ops[i] != WEAVE_OP_AND &&
				 ops[i] != WEAVE_OP_OR && ops[i] != WEAVE_OP_PHRASE))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("invalid wquery item")));
			lens[i] = 0;
			terms[i] = NULL;
		}
	}

	{
		Size		total = WEAVE_QUERY_HDRSIZE +
			(Size) nitems * sizeof(WeaveQueryItem) + textbytes;

		q = (WeaveQuery) palloc0(total);
		SET_VARSIZE(q, total);
		q->version = WEAVE_QUERY_VERSION;
		q->flags = 0;
		q->nitems = nitems;

		items = q->items;
		textbase = WEAVE_QUERY_TEXTBASE(q);
		for (i = 0; i < nitems; i++)
		{
			items[i].type = types[i];
			items[i].op = ops[i];
			items[i].flags = flags[i];
			items[i].distance = dists[i];
			if (types[i] == WEAVE_QI_VAL)
			{
				items[i].termoff = off;
				items[i].termlen = lens[i];
				memcpy(textbase + off, terms[i], lens[i]);
				off += lens[i];
			}
		}
	}

	PG_RETURN_WQUERY(q);
}

Datum
wquery_send(PG_FUNCTION_ARGS)
{
	WeaveQuery	q = PG_GETARG_WQUERY(0);
	WeaveQueryItem *items = q->items;
	StringInfoData buf;
	uint32		i;

	pq_begintypsend(&buf);
	pq_sendint16(&buf, q->version);
	pq_sendint32(&buf, q->nitems);
	for (i = 0; i < q->nitems; i++)
	{
		pq_sendint8(&buf, items[i].type);
		pq_sendint8(&buf, items[i].op);
		pq_sendint16(&buf, items[i].flags);
		pq_sendint32(&buf, items[i].distance);
		if (items[i].type == WEAVE_QI_VAL)
		{
			pq_sendint32(&buf, items[i].termlen);
			pq_sendbytes(&buf, WEAVE_QUERY_ITEMTEXT(q, &items[i]),
						 items[i].termlen);
		}
	}

	PG_FREE_IF_COPY(q, 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}
