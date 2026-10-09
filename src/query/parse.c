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
 *
 *	  '-' is negation only in PREFIX position; between two word characters it is
 *	  part of the term ('pkg-config'), as are '.' and '/'.  See
 *	  is_term_infix_byte().
 *	  primary := '(' expr ')' | term
 *	  term    := run of token bytes (folded like the analyzer), which may contain
 *	             an intra-word '-', '.' or '/'
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
#include "mb/pg_wchar.h"
#include "miscadmin.h"
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
	TOK_COMMA,					/* , inside NEAR(...) */
	TOK_PHRASE					/* <->, <N> (exact gap) or <=N> (at most N) */
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
	bool		exact;			/* TOK_PHRASE: <-> / <N> rather than <=N> */
	uint32		gap;			/* TOK_PHRASE: N */
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

static int64 parse_or(ParseState *st);
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

/*
 * May this byte appear INSIDE a term, i.e. between two word characters?
 *
 * The set is not a guess: it is what the DOCUMENT analyzer joins.  Verified on
 * this tree against to_wdoc('simple', 'a-b c/d e.f g_h i+j'), which yields
 *	 'a':1@2 'a-b':1@1 'b':1@3 'c/d':1@4 'e.f':1@5 'g':1@6 'h':1@7 'i':1@8 'j':1@9
 * -- '-', '/' and '.' are kept inside a token (PostgreSQL's parser classifies
 * them asciihword / file / file) while '_' and '+' SPLIT.  The query lexer has
 * to agree with that or a token the user typed can never match the token we
 * stored: before this rule, to_wquery('simple', 'pkg-config') could not produce
 * the lexeme 'pkg-config' at all, no matter what the index contained.
 *
 * The built-in 1-arg to_wdoc(text) analyzer is a deliberate exception: it splits
 * on every non-alphanumeric byte, so on that path 'pkg-config' stores 'pkg' and
 * 'config' and a literal 'pkg-config' query matches nothing.  That is still the
 * right trade: the old parse returned a *wrong* row set (see lex_raw below),
 * while this one returns an empty one, and the configured analyzer -- the one an
 * index is built with -- matches exactly.
 *
 * A trailing separator is deliberately not covered by this rule, because it is
 * not between two word characters: `c++` and `notepad++` lex to 'c' /
 * 'notepad', which is what both PostgreSQL and our own document analyzer do.
 */
static inline bool
is_term_infix_byte(unsigned char c)
{
	return c == '-' || c == '.' || c == '/';
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
 * Read the decimal digits at buf[p...] into *val.  Returns the position after
 * them (p itself when there are none) and sets *overflow when the value
 * exceeds `max`.  Every number the lexer reads goes through here: the old
 * open-coded loops wrapped, so `term~4294967297` meant ~1.
 */
static int
lex_digits(const char *buf, int len, int p, uint32 max, uint32 *val,
		   bool *overflow)
{
	uint64		v = 0;

	*overflow = false;
	while (p < len && buf[p] >= '0' && buf[p] <= '9')
	{
		v = v * 10 + (buf[p] - '0');
		if (v > max)
		{
			*overflow = true;
			v = max;
		}
		p++;
	}
	*val = (uint32) v;
	return p;
}

/*
 * The suffixes a term may carry -- `*` (prefix), `~k` (fuzzy) and `:ABCD`
 * (weight labels) -- read after a bare term or after a quoted 'literal', so
 * wquery_out's 'fo'* / 'fo'~1 / 'fox':A parse back (doc/GAPS.md G96).
 */
static void
lex_suffix(ParseState *st, Token *tok)
{
	/* a trailing '*' marks a prefix term */
	if (st->pos < st->len && st->buf[st->pos] == '*')
	{
		tok->prefix = true;
		st->pos++;
	}
	/* a trailing '~k' marks a fuzzy term (k defaults to 2) */
	else if (st->pos < st->len && st->buf[st->pos] == '~')
	{
		uint32		k;
		bool		overflow;
		int			p = st->pos + 1;
		int			e = lex_digits(st->buf, st->len, p, PG_INT32_MAX, &k,
								   &overflow);

		if (overflow)
			st->error = true;
		st->pos = e;

		/*
		 * `term~0` IS AN EDIT BUDGET OF ZERO, i.e. the term itself, and it is
		 * now normalized to a PLAIN term rather than widened to ~1.  The old
		 * Max(k, 1) answered a stricter question with a looser one: a user who
		 * wrote ~0 also got every term at distance 1, which is a wrong answer
		 * in the false-POSITIVE direction and the only kind this parser
		 * produces on its own.  Zero is also what contrib/fuzzystrmatch's
		 * levenshtein() means by zero, and that agreement is the basis of the
		 * oracle in sql/fuzzyuleven.sql.
		 *
		 * Normalized rather than carried as a fuzzy item with k = 0 because
		 * `fuzzy_k == 0` is this lexer's sentinel for "not fuzzy" (see the
		 * field's comment and the flag assignment below), and the two mean the
		 * same rows: the exact-term route returns precisely the terms at
		 * distance 0, doing less work than an automaton with an empty budget.
		 * The visible consequence is that 'term~0'::wquery prints as 'term',
		 * which is the normalization stated rather than hidden.
		 *
		 * A BARE `term~` still means 2, unchanged: there is no digit to honour
		 * there, so the default is a convention rather than a value the user
		 * wrote.
		 */
		tok->fuzzy_k = (e > p) ? (int) k : 2;
	}

	/*
	 * A trailing ':' followed by a run of weight labels A/B/C/D (any case)
	 * restricts the term to those field zones (tsquery-style term:A).  Sets a
	 * 4-bit mask (bit L for label L in 0..3 = D,C,B,A).  Applies to plain,
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
			/*
			 * A weight label cannot combine with a prefix/fuzzy suffix in this
			 * release (those are presence-only, no per-occurrence positions to
			 * zone-filter).  Reject term:A* / term:A~k as a syntax error rather
			 * than silently dropping the * / ~k: the parser sees weightmask and
			 * a prefix/fuzzy flag together and errors.
			 */
			if (p < st->len && (st->buf[p] == '*' || st->buf[p] == '~'))
			{
				tok->prefix = (st->buf[p] == '*');
				tok->fuzzy_k = (st->buf[p] == '~') ? 2 : 0;
				p++;
			}
			tok->weightmask = mask;
			st->pos = p;
		}
	}
}

/*
 * Raw lexer.  Recognizes &, |, !, - and parentheses as punctuation; the
 * keywords AND/OR/NOT (case-insensitive) as operators; everything else is a
 * term.  A bare "and"/"or"/"not" is treated as an operator only when it stands
 * alone as a token, which is the standard, least-surprising behavior.  '-', '.'
 * and '/' are punctuation only where they are not part of a word; see
 * is_term_infix_byte().
 *
 * Callers use next_token()/peek() rather than calling this directly, so that a
 * peeked token is lexed (and its term palloc'd) exactly once.
 */
static Token
lex_raw(ParseState *st)
{
	Token		tok = {0};
	int			start;
	int			flen;
	char	   *folded;

	/*
	 * Skip whitespace and punctuation -- except that a '-', '.' or '/' sitting
	 * between two word characters belongs to the TERM, not to the operator set,
	 * so break out and hand it to the term scanner below.
	 *
	 * This is a wrong-answer fix, not a niceness: to_wquery('pkg-config') used
	 * to parse as ('pkg' & !'config'), and that NOT clause actively EXCLUDED the
	 * pkg-config documents being searched for -- upstream (the project this
	 * parser was forked from) measured `install-info` matching 1 row instead of
	 * 10.  A silently different answer
	 * is a worse failure mode than a visible parse error.  '/' was worse still:
	 * it opened a /regex/, so the rest of the query was swallowed and `foo/bar`
	 * became just 'foo'.
	 *
	 * The st->pos > 0 guard is what keeps the operators these bytes still are in
	 * prefix position, and it is the non-obvious half of the rule:
	 *
	 *	- without it, a leading '-' would start a term, so '-b' would lex as the
	 *	  term 'b' instead of !'b' -- prefix NOT would silently stop negating.
	 *	  (`a -b` is safe either way: the byte before '-' is a space, not a token
	 *	  byte.  Only the position-0 case needs the guard.)
	 *	- without it, a standalone '/regex/' would never reach the regex branch
	 *	  below, because its opening '/' is followed by a token byte; the query
	 *	  would become a term containing the regex text.  Every regex in
	 *	  expected/weave.out is of exactly that shape.
	 *
	 * The flanking test also means a separator next to a non-token byte stays an
	 * operator, so `a-(b)` is still ('a' & !'b'), and a TRAILING separator is
	 * dropped by construction: `c++` is 'c'.
	 */
	while (st->pos < st->len &&
		   !is_token_byte((unsigned char) st->buf[st->pos]))
	{
		char		c = st->buf[st->pos];

		if (is_term_infix_byte((unsigned char) c) && st->pos > 0 &&
			is_token_byte((unsigned char) st->buf[st->pos - 1]) &&
			st->pos + 1 < st->len &&
			is_token_byte((unsigned char) st->buf[st->pos + 1]))
			break;				/* intra-word separator: part of the term */

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
			case '<':
				{
					/*
					 * tsquery's phrase operators, which wquery_out prints
					 * (doc/GAPS.md G96): `<->` and `<N>` are an EXACT gap of N
					 * (WEAVE_QF_PHRASE_EXACT), `<=N>` is wquery's own "at most
					 * N" (the NEAR/"..." semantics).  Any other '<' is an
					 * ordinary separator, as it always was.
					 */
					int			p = st->pos + 1;
					bool		atmost = false;
					bool		overflow = false;
					uint32		n = 1;
					int			e;

					if (p + 1 < st->len && st->buf[p] == '-' &&
						st->buf[p + 1] == '>')
						e = p + 1;
					else
					{
						if (p < st->len && st->buf[p] == '=')
						{
							atmost = true;
							p++;
						}
						e = lex_digits(st->buf, st->len, p, PG_UINT32_MAX, &n,
									   &overflow);
						if (e == p || e >= st->len || st->buf[e] != '>')
						{
							st->pos++;	/* not an operator: a separator */
							break;
						}
					}
					if (overflow)
						st->error = true;
					st->pos = e + 1;
					tok.kind = TOK_PHRASE;
					tok.exact = !atmost;
					tok.gap = n;
					return tok;
				}
			case '\'':
				{
					/*
					 * A quoted literal, 'text', as wquery_out prints every
					 * term: the bytes between the quotes VERBATIM (no folding,
					 * no keywords -- 'and' is a term), with \x standing for x,
					 * and optionally followed by a suffix (`*`, `~k`, `:ABCD`).
					 * Like tsquery's 'Fox', which stays 'Fox'.  Verbatim is
					 * what makes the text round-trip: a stored term need not be
					 * folded (the tsquery cast copies lexemes, to_wquery(cfg)
					 * stores whatever the dictionary returned).
					 *
					 * It is a literal only at a token boundary and only when an
					 * unescaped closing quote follows with something between,
					 * so `don't` and a lone `'` still separate as before.
					 */
					int			j = st->pos + 1;
					int			k;
					char	   *lit;
					int			n = 0;

					if (st->pos > 0 &&
						is_token_byte((unsigned char) st->buf[st->pos - 1]))
					{
						st->pos++;
						break;
					}
					while (j < st->len && st->buf[j] != '\'')
						j += (st->buf[j] == '\\' && j + 1 < st->len) ? 2 : 1;
					if (j >= st->len || j == st->pos + 1 ||
						(j + 1 < st->len &&
						 is_token_byte((unsigned char) st->buf[j + 1])))
					{
						/* unterminated, empty, or closed inside a word
						 * (`'tis and 'twas`): a separator, as before */
						st->pos++;
						break;
					}
					lit = (char *) palloc(j - st->pos);
					for (k = st->pos + 1; k < j; k++)
					{
						if (st->buf[k] == '\\')
							k++;
						lit[n++] = st->buf[k];
					}
					st->pos = j + 1;
					tok.kind = TOK_TERM;
					tok.term = lit;
					tok.termlen = n;
					lex_suffix(st, &tok);
					return tok;
				}
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
						/*
						 * Unterminated: a TRAILING '/' is a dropped separator
						 * like any other (`foo/` is 'foo'), but one with a word
						 * after it is an error.  This returned EOF, so `a /foo`
						 * and `a / b` silently became just 'a' (doc/GAPS.md G96).
						 */
						int			j;

						for (j = rstart; j < st->len; j++)
							if (is_token_byte((unsigned char) st->buf[j]))
								st->error = true;
						tok.kind = TOK_EOF;
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

	/*
	 * A term: a run of token bytes, folded, which may contain '-', '.' or '/'
	 * provided each one is flanked by token bytes (see is_term_infix_byte).
	 * That keeps 'pkg-config', 'foo/bar' and 'python3.14' whole, exactly as the
	 * configured document analyzer stores them, while a leading or trailing
	 * separator still terminates the term.
	 */
	start = st->pos;
	while (st->pos < st->len)
	{
		unsigned char ch = (unsigned char) st->buf[st->pos];

		if (is_token_byte(ch))
		{
			st->pos++;
			continue;
		}
		if (is_term_infix_byte(ch) && st->pos + 1 < st->len &&
			is_token_byte((unsigned char) st->buf[st->pos + 1]))
		{
			st->pos++;			/* separator between word characters */
			continue;
		}
		break;
	}
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
		lex_suffix(st, &tok);
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

/*
 * primary := '(' expr ')' | '"' term+ '"' | NEAR '(' term+ [',' k] ')' | term
 *
 * Every parse_* function returns its operand's phrase WIDTH, the quantity an
 * exact gap is stored relative to (WEAVE_QF_PHRASE_EXACT): 0 for a term, the
 * larger operand's for AND / OR, the operand's for NOT, D + width(L) for an
 * exact phrase and D + width(L) + width(R) for an at-most one.  wquery_out
 * subtracts the same width to print N, and wquery_validate() checks it, so the
 * three must agree; migrate.c computes core's equivalent for the cast.
 */
static int64
parse_primary(ParseState *st)
{
	Token		tok;
	int64		w = 0;

	check_stack_depth();
	tok = next_token(st);
	if (tok.kind == TOK_LPAREN)
	{
		w = parse_or(st);
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
			/* a /regex/ is not a phrase word: it was taken as a plain term
			 * with the regex's text (doc/GAPS.md G96) */
			if (p.term == NULL || p.regex)
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
		w = Max(nterms - 1, 0);
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
			return 0;
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
				 p.kind != TOK_NOT && p.kind != TOK_NEAR) || p.term == NULL ||
				p.regex)
			{
				st->error = true;
				return 0;
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
			bool		overflow;

			/* k: digits only, and in range (this loop used to wrap) */
			if (kt.kind != TOK_TERM || kt.regex || kt.prefix || kt.fuzzy_k ||
				kt.weightmask ||
				lex_digits(kt.term, kt.termlen, 0, PG_UINT32_MAX, &dist,
						   &overflow) != kt.termlen || kt.termlen == 0 ||
				overflow)
			{
				st->error = true;
				return 0;
			}
			p = next_token(st);
		}
		else
			dist = 10;			/* NEAR default proximity */
		if (p.kind != TOK_RPAREN)
		{
			st->error = true;
			return 0;
		}
		if (nterms < 2 || dist < 1)
		{
			st->error = true;	/* NEAR needs >=2 terms and k>=1 */
			return 0;
		}
		/* join the nterms operands with PHRASE(dist): nterms-1 operators */
		{
			int			m;

			for (m = 1; m < nterms; m++)
				emit_dist(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, NULL, 0, 0, dist);
		}
		w = (int64) (nterms - 1) * dist;
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
				return 0;
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
	return w;
}

/* unary := NOT unary | primary */
static int64
parse_unary(ParseState *st)
{
	Token		tok;
	int64		w;

	check_stack_depth();
	tok = peek(st);
	if (tok.kind == TOK_NOT)
	{
		(void) next_token(st);
		w = parse_unary(st);
		emit(st, WEAVE_QI_OPR, WEAVE_OP_NOT, NULL, 0, 0);
	}
	else
		w = parse_primary(st);
	return w;
}

/*
 * phrase_expr := unary ( PHRASE unary )*, left-associative, binding tighter
 * than AND and looser than NOT -- tsquery's precedence, so the cast's text
 * (`'quick' <-> 'brown' <-> 'fox'` from core) reads as core reads it.
 *
 * `L <N> R` (and `<->`, N = 1) is an exact gap stored as D = N + width(R);
 * `L <=N> R` is "within N", stored as D = N, the NEAR / "..." semantics.
 */
static int64
parse_phrase(ParseState *st)
{
	int64		w = parse_unary(st);

	while (peek(st).kind == TOK_PHRASE)
	{
		Token		op = next_token(st);
		int64		wr = parse_unary(st);
		uint32		d;

		if (op.exact)
		{
			/* clamped like the cast: a gap past every position never matches */
			if (wr > PG_UINT32_MAX)
				st->error = true;
			d = (uint32) Min((int64) op.gap + wr, (int64) PG_UINT32_MAX);
			w = (int64) d + w;
		}
		else
		{
			d = op.gap;
			w = (int64) d + w + wr;
		}
		emit_dist(st, WEAVE_QI_OPR, WEAVE_OP_PHRASE, NULL, 0,
				  op.exact ? WEAVE_QF_PHRASE_EXACT : 0, d);
	}
	return w;
}

/* and_expr := phrase ( AND? phrase )*  (implicit AND between adjacent terms) */
static int64
parse_and(ParseState *st)
{
	int64		w = parse_phrase(st);

	for (;;)
	{
		Token		tok = peek(st);

		if (tok.kind == TOK_AND)
		{
			int64		wr;

			(void) next_token(st);
			wr = parse_phrase(st);	/* not inside Max(): it is a macro */
			w = Max(w, wr);
			emit(st, WEAVE_QI_OPR, WEAVE_OP_AND, NULL, 0, 0);
		}
		else if (tok.kind == TOK_TERM || tok.kind == TOK_NOT ||
				 tok.kind == TOK_LPAREN || tok.kind == TOK_QUOTE ||
				 tok.kind == TOK_NEAR)
		{
			/* implicit AND */
			int64		wr = parse_phrase(st);

			w = Max(w, wr);
			emit(st, WEAVE_QI_OPR, WEAVE_OP_AND, NULL, 0, 0);
		}
		else
			break;
	}
	return w;
}

/* or_expr := and_expr ( OR and_expr )* */
static int64
parse_or(ParseState *st)
{
	int64		w = parse_and(st);

	for (;;)
	{
		Token		tok = peek(st);

		if (tok.kind == TOK_OR)
		{
			int64		wr;

			(void) next_token(st);
			wr = parse_and(st);
			w = Max(w, wr);
			emit(st, WEAVE_QI_OPR, WEAVE_OP_OR, NULL, 0, 0);
		}
		else
			break;
	}
	return w;
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
	uint16		flags;			/* the operator's WEAVE_QF_* (PHRASE_EXACT) */
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
	check_stack_depth();
	n->op = items[*pos].op;
	n->flags = items[*pos].flags;
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
	check_stack_depth();
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
	out[*k].flags = n->flags;	/* was 0: an exact gap became "at most" */
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
 * The phrase WIDTH of an operand, as parse_primary() defines it and as
 * migrate.c computes core's: 0 for a term, the operand's for NOT, the larger
 * for AND / OR, D + width(L) for an exact phrase and D + width(L) + width(R)
 * for an at-most one.  wquery_out recovers an exact phrase's N as
 * D - width(R), and weave_query_validate() checks that it is not negative.
 */
static int64
query_op_width(const WeaveQueryItem *it, int64 wl, int64 wr)
{
	if (it->op != WEAVE_OP_PHRASE)
		return Max(wl, wr);
	if (it->flags & WEAVE_QF_PHRASE_EXACT)
		return (int64) it->distance + wl;
	return (int64) it->distance + wl + wr;
}

/*
 * Is `t` safe to print BARE inside "..." or NEAR(...)?  Only when the lexer
 * would read it back as the same bytes: ASCII lower-case letters and digits
 * (which fold to themselves).  Anything else is printed as a quoted literal.
 */
static bool
out_bare_word(const char *t, int len)
{
	int			j;

	if (len == 0)
		return false;
	for (j = 0; j < len; j++)
		if (!((t[j] >= 'a' && t[j] <= 'z') || (t[j] >= '0' && t[j] <= '9')))
			return false;
	/* a keyword is a word inside "...", but lexes without its suffix */
	if ((len == 3 && (memcmp(t, "and", 3) == 0 || memcmp(t, "not", 3) == 0)) ||
		(len == 2 && memcmp(t, "or", 2) == 0) ||
		(len == 4 && memcmp(t, "near", 4) == 0))
		return false;
	return true;
}

static void
out_quoted(StringInfo s, const char *t, int len)
{
	int			j;

	appendStringInfoChar(s, '\'');
	for (j = 0; j < len; j++)
	{
		if (t[j] == '\'' || t[j] == '\\')
			appendStringInfoChar(s, '\\');
		appendStringInfoChar(s, t[j]);
	}
	appendStringInfoChar(s, '\'');
}

/* One operand on wquery_out's stack. */
typedef struct OutEnt
{
	StringInfoData s;			/* the operand as a standalone expression */
	StringInfoData words;		/* word / chain: its words for "..." / NEAR() */
	bool		word;			/* a plain or prefix term (a phrase word) */
	bool		lchain;			/* left-deep at-most-1 chain: prints "a b c" */
	bool		rchain;			/* right-deep at-most-k chain: NEAR(a b c, k) */
	uint32		k;				/* rchain's gap */
	int64		width;
} OutEnt;

/*
 * wquery_out -- render a wquery as text that wquery_in parses back to the
 * SAME value, byte for byte (doc/GAPS.md G96; sql/wquery_roundtrip.sql).
 *
 * Terms print as quoted literals, 'text' with \' and \\ escaped, followed by
 * their suffix: `*` (prefix), `~k` (fuzzy) or `:ABCD` (weight).  A regex is
 * /text/.  AND, OR and NOT print as `&`, `|` and `!`, fully parenthesised.
 *
 * Phrases print in the native syntax that produces exactly their shape, and
 * only otherwise as an operator:
 *
 *	- a LEFT-deep chain of plain / prefix words at most 1 apart, as "..."
 *	  emits it: "quick brown fox";
 *	- a RIGHT-deep chain of plain / prefix words with one gap k, as NEAR
 *	  emits it: NEAR(quick brown fox, 3) (a two-word chain with k = 1 is both,
 *	  and prints as "quick brown");
 *	- any other at-most phrase, which only binary input can build (an operand
 *	  that is not a word, mixed gaps), as `(L <=N> R)`;
 *	- an exact gap (WEAVE_QF_PHRASE_EXACT, from the tsquery cast) in tsquery's
 *	  spelling, `<->` for N = 1 and `<N>` otherwise, N recovered from the stored
 *	  end-to-end gap by subtracting the right operand's width.
 *
 * Iterative over the postfix items with a string stack, so a long chain does
 * not recurse.  A value built by wquery_recv was checked by
 * weave_query_validate(), so the stack shape and every N here are sound.
 */
Datum
wquery_out(PG_FUNCTION_ARGS)
{
	WeaveQuery	q = PG_GETARG_WQUERY(0);
	WeaveQueryItem *items = q->items;
	OutEnt	   *stack;
	int			top = 0;
	uint32		i;
	char	   *result;

	if (q->nitems == 0)
	{
		PG_FREE_IF_COPY(q, 0);
		PG_RETURN_CSTRING(pstrdup(""));
	}

	stack = (OutEnt *) palloc0(q->nitems * sizeof(OutEnt));

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &items[i];
		OutEnt		e;

		memset(&e, 0, sizeof(e));
		initStringInfo(&e.s);
		if (it->type == WEAVE_QI_VAL)
		{
			const char *t = WEAVE_QUERY_ITEMTEXT(q, it);

			if (it->flags & WEAVE_QF_REGEX)
			{
				appendStringInfoChar(&e.s, '/');
				appendBinaryStringInfo(&e.s, t, it->termlen);
				appendStringInfoChar(&e.s, '/');
			}
			else
			{
				out_quoted(&e.s, t, it->termlen);
				if (it->flags & WEAVE_QF_PREFIX)
					appendStringInfoChar(&e.s, '*');
				else if (it->flags & WEAVE_QF_FUZZY)
					appendStringInfo(&e.s, "~%u", it->distance);
				else if (it->flags & WEAVE_QF_WEIGHTED)
				{
					/* the weight mask as :A/B/C/D (high labels first) */
					appendStringInfoChar(&e.s, ':');
					if (it->distance & (1u << 3))
						appendStringInfoChar(&e.s, 'A');
					if (it->distance & (1u << 2))
						appendStringInfoChar(&e.s, 'B');
					if (it->distance & (1u << 1))
						appendStringInfoChar(&e.s, 'C');
					if (it->distance & (1u << 0))
						appendStringInfoChar(&e.s, 'D');
				}
				if ((it->flags & ~WEAVE_QF_PREFIX) == 0)
				{
					/* a phrase word: bare if the lexer reads it back as is */
					e.word = true;
					initStringInfo(&e.words);
					if (out_bare_word(t, it->termlen))
						appendBinaryStringInfo(&e.words, t, it->termlen);
					else
						out_quoted(&e.words, t, it->termlen);
					if (it->flags & WEAVE_QF_PREFIX)
						appendStringInfoChar(&e.words, '*');
				}
			}
			stack[top++] = e;
			continue;
		}
		if (it->op == WEAVE_OP_NOT)
		{
			Assert(top >= 1);
			appendStringInfoChar(&e.s, '!');
			appendBinaryStringInfo(&e.s, stack[top - 1].s.data,
								   stack[top - 1].s.len);
			e.width = stack[top - 1].width;
			stack[top - 1] = e;
			continue;
		}

		Assert(top >= 2);
		{
			OutEnt	   *l = &stack[top - 2];
			OutEnt	   *r = &stack[top - 1];
			bool		atmost = (it->op == WEAVE_OP_PHRASE &&
								  !(it->flags & WEAVE_QF_PHRASE_EXACT));

			e.width = query_op_width(it, l->width, r->width);
			if (atmost)
			{
				e.lchain = it->distance == 1 && (l->word || l->lchain) && r->word;
				e.rchain = it->distance >= 1 && l->word &&
					(r->word || (r->rchain && r->k == it->distance));
				e.k = it->distance;
			}
			if (e.lchain || e.rchain)
			{
				/* the same words either way: L's, then R's */
				initStringInfo(&e.words);
				appendBinaryStringInfo(&e.words, l->words.data, l->words.len);
				appendStringInfoChar(&e.words, ' ');
				appendBinaryStringInfo(&e.words, r->words.data, r->words.len);
				if (e.lchain)
					appendStringInfo(&e.s, "\"%s\"", e.words.data);
				else
					appendStringInfo(&e.s, "NEAR(%s, %u)", e.words.data, e.k);
			}
			else
			{
				appendStringInfoChar(&e.s, '(');
				appendBinaryStringInfo(&e.s, l->s.data, l->s.len);
				switch (it->op)
				{
					case WEAVE_OP_AND:
						appendStringInfoString(&e.s, " & ");
						break;
					case WEAVE_OP_OR:
						appendStringInfoString(&e.s, " | ");
						break;
					default:
						if (atmost)
							appendStringInfo(&e.s, " <=%u> ", it->distance);
						else if ((int64) it->distance - r->width == 1)
							appendStringInfoString(&e.s, " <-> ");
						else
							appendStringInfo(&e.s, " <" INT64_FORMAT "> ",
											 (int64) it->distance - r->width);
						break;
				}
				appendBinaryStringInfo(&e.s, r->s.data, r->s.len);
				appendStringInfoChar(&e.s, ')');
			}
			top -= 2;
			stack[top++] = e;
		}
	}

	Assert(top == 1);
	result = stack[0].s.data;

	PG_FREE_IF_COPY(q, 0);
	PG_RETURN_CSTRING(result);
}

/*
 * weave_query_validate -- reject a wquery that no parser could have built.
 *
 * wquery_recv is a trust boundary, and before this check it accepted any item
 * sequence: a postfix list that underflows the stack (`AND` alone) indexed
 * stack[-1] in wquery_out and in both evaluators, unknown flags and gaps were
 * stored, and a term with a NUL, a '/' in a regex or bytes invalid in the
 * server encoding printed as text that does not read back.  What is accepted
 * here is exactly what wquery_out can print and wquery_in can read back to
 * the same bytes (doc/GAPS.md G96).
 */
static void
weave_query_validate(WeaveQuery q)
{
	int64	   *width = (int64 *) palloc(Max(q->nitems, 1) * sizeof(int64));
	int			top = 0;
	uint32		i;

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];
		const char *bad = NULL;

		if (it->type == WEAVE_QI_VAL)
		{
			const char *t = WEAVE_QUERY_ITEMTEXT(q, it);

			if (it->op != 0)
				bad = "operator on a term";
			else if (it->flags == WEAVE_QF_FUZZY)
			{
				if (it->distance < 1 || it->distance > PG_INT32_MAX)
					bad = "fuzzy edit budget out of range";
			}
			else if (it->flags == WEAVE_QF_WEIGHTED)
			{
				if (it->distance < 1 || it->distance > 0xF)
					bad = "weight mask out of range";
			}
			else if (it->flags != 0 && it->flags != WEAVE_QF_PREFIX &&
					 it->flags != WEAVE_QF_REGEX)
				bad = "term flags";
			else if (it->distance != 0)
				bad = "gap on a term";
			if (bad == NULL && it->termlen == 0 && it->flags != WEAVE_QF_REGEX)
				bad = "empty term";
			if (bad == NULL && (it->flags & WEAVE_QF_REGEX) &&
				memchr(t, '/', it->termlen) != NULL)
				bad = "'/' in a regex";
			if (bad == NULL)
				(void) pg_verifymbstr(t, it->termlen, false);
			width[top++] = 0;
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			if (top < 1)
				bad = "NOT without an operand";
			else if (it->flags != 0 || it->distance != 0)
				bad = "flags or gap on NOT";
		}
		else
		{
			if (top < 2)
				bad = "operator without two operands";
			else if (it->op != WEAVE_OP_PHRASE &&
					 (it->flags != 0 || it->distance != 0))
				bad = "flags or gap on AND / OR";
			else if (it->op == WEAVE_OP_PHRASE &&
					 (it->flags & ~WEAVE_QF_PHRASE_EXACT) != 0)
				bad = "phrase flags";
			else if (it->op == WEAVE_OP_PHRASE &&
					 (it->flags & WEAVE_QF_PHRASE_EXACT) &&
					 (int64) it->distance < width[top - 1])
				bad = "exact gap shorter than its right operand";
			if (bad == NULL)
			{
				width[top - 2] = query_op_width(it, width[top - 2],
												width[top - 1]);
				top--;
			}
		}
		if (bad != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid wquery: %s at item %u", bad, i + 1)));
	}
	if (q->nitems > 0 && top != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid wquery: %d operands left over", top)));
	pfree(width);
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

	weave_query_validate(q);
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
