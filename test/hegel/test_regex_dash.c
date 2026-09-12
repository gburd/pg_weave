/*
 * test/hegel/test_regex_dash.c -- pg_weave regex tokenizer: a literal '-'
 * first or last in a bracket expression must not become a range operator.
 *
 * Ported from pg_tre 3.2.5 commit 2be8dbf ("fix: a literal '-' first or
 * last in a bracket expression was rejected").  pg_weave imported the
 * pre-fix tokenizer (src/query/regex_tokens.c, from pg_tre e03d6a8) and
 * inherited the bug: '-' inside `[...]` always tokenized as TOK_DASH (the
 * range operator), even when POSIX requires it to be a literal because it
 * is the first or last member of the bracket expression (or immediately
 * after a negating '^').  A bare TOK_DASH where the grammar expects a
 * LITERAL cannot be reduced by any `classitem` production in
 * src/query/regex_grammar.y, so patterns like `[-_.]` or `[abc-]` could
 * never parse.
 *
 * NOTE on why this drives the tokenizer directly rather than going through
 * SQL: weave_parse_regex() / weave_tokenize_next() currently have no
 * caller anywhere in the extension (grep confirms it -- the `@@@` regex
 * recheck path uses PostgreSQL core's own regex engine via
 * weave_doc_has_regex()/RE_compile_and_execute(), and the trigram-pruning
 * extractor in src/query/trgm.c is a separate hand-rolled scanner that
 * never calls into this Lime-grammar tokenizer/parser either).  This
 * component is staged ahead of the phase that wires it up (see the
 * "Contract file" note atop include/weave/regex_ast.h).  There is
 * therefore no SQL-visible symptom to regress-test yet; the tokenizer
 * itself is the right and only layer at which this bug is observable.
 *
 * This links src/query/regex_tokens.c UNMODIFIED for the test -- the file
 * under test is the real one that ships in the extension, not a copy.  It
 * needs a backend to build only for a handful of typedefs and palloc0();
 * see test/hegel/pgshim_regex/ for the substitute, and the ASCII-only
 * codepoint-stream stub below standing in for src/util/utf8.c (irrelevant
 * to this bug: '-', '^', '[', ']' are always single ASCII bytes in UTF-8).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/regex_ast.h"
#include "weave/utf8.h"
#include "regex_grammar.h"		/* token IDs: LITERAL, DASH, CARET, ... */

/*
 * ---- ASCII-only stand-in for src/util/utf8.c's UTF-8 decoder ----
 *
 * Every pattern below is pure ASCII, so this only needs to reproduce the
 * single-byte codepoint == byte-value behavior; multi-byte decoding is out
 * of scope for a tokenizer test about bracket-expression dash handling.
 */
void
pg_weave_cpstream_init(PgWeaveCpStream *s, const char *text, int len)
{
	s->src = (const unsigned char *) text;
	s->src_len = len;
	s->src_pos = 0;
}

int32
pg_weave_cpstream_next(PgWeaveCpStream *s)
{
	unsigned char c;

	if (s->src_pos >= s->src_len)
		return -1;
	c = s->src[s->src_pos];
	if (c >= 0x80)
	{
		fprintf(stderr,
				"test_regex_dash: non-ASCII input, stub decoder can't handle it\n");
		exit(2);
	}
	s->src_pos += 1;
	return (int32) c;
}

/* ---- Expected-token-stream table ---- */

typedef struct
{
	int			kind;			/* TOK_* / regex_grammar.h token id, 0 = EOF */
	int32		cp;				/* codepoint, only meaningful for LITERAL */
} ExpTok;

typedef struct
{
	const char *name;
	const char *pattern;
	ExpTok		expect[8];
	int			nexpect;
} DashCase;

static const DashCase cases[] = {
	/* POSIX: '-' first in the bracket expression is a literal. */
	{"[-x] dash first", "[-x]",
	 {{LBRACKET, 0}, {LITERAL, '-'}, {LITERAL, 'x'}, {RBRACKET, 0}}, 4},

	/* POSIX: '-' last in the bracket expression is a literal. */
	{"[x-] dash last", "[x-]",
	 {{LBRACKET, 0}, {LITERAL, 'x'}, {LITERAL, '-'}, {RBRACKET, 0}}, 4},

	/* POSIX: '-' right after a negating '^' is still "first". */
	{"[^-x] negated, dash first", "[^-x]",
	 {{LBRACKET, 0}, {CARET, 0}, {LITERAL, '-'}, {LITERAL, 'x'}, {RBRACKET, 0}}, 5},

	/* A genuine range (a-z) immediately followed by a trailing literal '-'. */
	{"[a-z-] range then dash last", "[a-z-]",
	 {{LBRACKET, 0}, {LITERAL, 'a'}, {DASH, 0}, {LITERAL, 'z'},
	  {LITERAL, '-'}, {RBRACKET, 0}}, 6},

	/* Control: a genuine range must still tokenize as LITERAL DASH LITERAL,
	 * i.e. this fix must not turn EVERY '-' into a literal. */
	{"[a-z] genuine range, unaffected", "[a-z]",
	 {{LBRACKET, 0}, {LITERAL, 'a'}, {DASH, 0}, {LITERAL, 'z'}, {RBRACKET, 0}}, 5},

	/* '-' is simultaneously first and last: still a literal. */
	{"[-] dash-only class", "[-]",
	 {{LBRACKET, 0}, {LITERAL, '-'}, {RBRACKET, 0}}, 3},

	/* Bonus (see task investigation step 1): '-' as a range ENDPOINT,
	 * `[--/]` = range from '-' to '/'.  The first '-' is "first" so it
	 * tokenizes as a literal; the second '-' is neither first nor last so
	 * it tokenizes as the range operator.  The resulting LITERAL('-') DASH
	 * LITERAL('/') stream is exactly what regex_grammar.y's
	 * "LITERAL DASH LITERAL -> range" production expects, so this case is
	 * handled correctly as a side effect of the position rule, with no
	 * extra logic. */
	{"[--/] dash as range endpoint", "[--/]",
	 {{LBRACKET, 0}, {LITERAL, '-'}, {DASH, 0}, {LITERAL, '/'}, {RBRACKET, 0}}, 5},
};

static bool
run_case(const DashCase *tc)
{
	WeaveParseCtx ctx;
	WeaveToken	tok;
	int			kind;
	int			i = 0;
	bool		ok = true;

	memset(&ctx, 0, sizeof(ctx));
	ctx.input = tc->pattern;
	ctx.input_len = (int) strlen(tc->pattern);
	ctx.tokenizer_state = NULL;

	while ((kind = weave_tokenize_next(&ctx, &tok)) > 0)
	{
		if (ctx.syntax_error)
		{
			printf("FAIL %-32s tokenizer error: %s\n", tc->name, ctx.errmsg);
			return false;
		}
		if (i >= tc->nexpect)
		{
			printf("FAIL %-32s more tokens than expected (extra kind=%d)\n",
				   tc->name, kind);
			return false;
		}
		if (kind != tc->expect[i].kind ||
			(kind == LITERAL && tok.cp != tc->expect[i].cp))
		{
			printf("FAIL %-32s token %d: got kind=%d cp=%d, want kind=%d cp=%d\n",
				   tc->name, i, kind, tok.cp,
				   tc->expect[i].kind, tc->expect[i].cp);
			ok = false;
		}
		i++;
	}

	if (kind < 0)
	{
		printf("FAIL %-32s tokenizer returned error (%s)\n", tc->name, ctx.errmsg);
		return false;
	}
	if (i != tc->nexpect)
	{
		printf("FAIL %-32s got %d tokens, want %d\n", tc->name, i, tc->nexpect);
		return false;
	}

	if (ok)
		printf("ok   %-32s '%s' -> %d tokens\n", tc->name, tc->pattern, i);
	return ok;
}

int
main(void)
{
	int			n = (int) (sizeof(cases) / sizeof(cases[0]));
	int			i;
	int			nfail = 0;

	for (i = 0; i < n; i++)
		if (!run_case(&cases[i]))
			nfail++;

	if (nfail > 0)
	{
		printf("test_regex_dash: %d/%d cases FAILED\n", nfail, n);
		return 1;
	}
	printf("test_regex_dash: all %d cases passed\n", n);
	return 0;
}
