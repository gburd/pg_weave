/*
 * test/hegel/test_regex_approx.c -- G88: the regex trigram prefilter never
 * drops a token TRE accepts, including approximate atoms `atom{~k}`.
 *
 * THE PROPERTY (AGENTS.md hard rule 1, applied to a prefilter).  For a pattern
 * P and a token T: if TRE (the matcher that decides `/P/` for a pattern with
 * `{~`, src/query/re_match.c weave_match_wide) accepts T, then every conjunct
 * of regex_extract_query(P, 0) has an alternative whose codepoint trigram
 * occurs in T.  The index narrows the dictionary to terms satisfying that CNF
 * before the matcher sees them, so a violation is a row the index silently
 * loses and no recheck can restore.
 *
 * WHAT IS REAL.  The pattern goes through the shipped parser (parser.c,
 * regex_tokens.c, regex_grammar.c, regex_ast.c) and extractor (extract.c), and
 * the matcher is the shipped wrapper over the vendored TRE.  Only the backend
 * runtime those files call is stubbed below (palloc, ereport, the trigram hash,
 * which the property does not read), and the codepoint decoder is ASCII-only
 * because every generated pattern is ASCII.
 *
 * THE GENERATOR stays inside the dialect weave_regex_narrowable() admits for
 * an approximate pattern (literals, `.`, bracket classes, groups, alternation,
 * ? * + {m,n}, edge anchors, `{~k}` on an atom), because outside it the
 * narrowing is refused and the property is vacuous.  Tokens are drawn by
 * sampling a string from the generated pattern and applying up to three random
 * edits, plus uniformly random short strings; TRE then says which match.
 *
 * Positive control: before G88 the extractor inlined a variable repetition's
 * child into the surrounding literal run, so `xa+y` required "xay" and
 * dropped "xaay"; and treating an APPROX atom as its child (the pre-G88 rule)
 * requires trigrams an edit removes.  This test fails on either (see
 * doc/GAPS.md G88).
 */
#include "postgres.h"

#include <setjmp.h>
#include <regex.h>
#include <stdarg.h>

#include "miscadmin.h"
#include "weave/regex_ast.h"
#include "weave/utf8.h"
#include "weave/hash.h"
#include "weave/re_match.h"

/* port.h redirects these to src/port's implementations; use libc's. */
#undef printf
#undef snprintf
#undef vsnprintf
#undef vfprintf

/* ---------------- backend runtime stand-ins ---------------- */

MemoryContext CurrentMemoryContext = NULL;
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;
int			pg_weave_max_extraction_fanout = 4096;

/* Leak-everything allocator: each case allocates a few KB; bounded by NCASES. */
void *
palloc(Size sz)
{
	void	   *p = malloc(sz ? sz : 1);

	if (!p)
		abort();
	return p;
}
void *
palloc0(Size sz)
{
	void	   *p = palloc(sz);

	memset(p, 0, sz);
	return p;
}
void *
repalloc(void *p, Size sz)
{
	void	   *q = realloc(p, sz);

	if (!q)
		abort();
	return q;
}
void
pfree(void *p)
{
	free(p);
}
void
check_stack_depth(void)
{
}

static int	n_ereports;
bool
errstart(int elevel, const char *domain)
{
	return elevel >= 20;
}
bool
errstart_cold(int elevel, const char *domain)
{
	return elevel >= 20;
}
int
errcode(int c)
{
	return 0;
}
int
errmsg(const char *fmt,...)
{
	return 0;
}
int
errmsg_internal(const char *fmt,...)
{
	return 0;
}
void
errfinish(const char *f, int l, const char *fn)
{
	n_ereports++;
	pg_re_throw();
}
void
pg_re_throw(void)
{
	if (PG_exception_stack)
		siglongjmp(*PG_exception_stack, 1);
	abort();
}
int
pg_snprintf(char *s, size_t n, const char *fmt,...)
{
	va_list		a;
	int			r;

	va_start(a, fmt);
	r = vsnprintf(s, n, fmt, a);
	va_end(a);
	return r;
}
int
pg_fprintf(FILE *f, const char *fmt,...)
{
	va_list		a;
	int			r;

	va_start(a, fmt);
	r = vfprintf(f, fmt, a);
	va_end(a);
	return r;
}
/* regex_ast_debug_dump() only */
void initStringInfo(StringInfo s) { abort(); }
void appendStringInfo(StringInfo s, const char *f,...) { abort(); }
void appendStringInfoChar(StringInfo s, char c) { abort(); }
void appendStringInfoString(StringInfo s, const char *c) { abort(); }

uint64
pg_weave_hash_trigram_cp(const int32 cp[3])
{
	return 0;					/* the property reads TrigramDisjunct.cp */
}

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
	if (s->src_pos >= s->src_len)
		return -1;
	if (s->src[s->src_pos] >= 0x80)
		abort();				/* generator is ASCII-only */
	return (int32) s->src[s->src_pos++];
}

/* ---------------- RNG (xorshift64*, as test_uleven.c) ---------------- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t
rng(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}
static int
rnd(int n)
{
	return (int) (rng() % (uint64_t) n);
}

/* ---------------- pattern generator (own tree, rendered + sampled) -------- */

#define ALPHA "abcd"

typedef struct G
{
	int			kind;			/* 0 lit, 1 any, 2 class, 3 cat, 4 alt, 5 rep, 6 approx */
	char		c;
	char		cls[5];			/* member set for class */
	bool		neg;
	int			m, n;			/* rep bounds, n=-1 unbounded */
	int			k;
	struct G   *l, *r;
} G;

static G *
gnew(int kind)
{
	G		   *g = calloc(1, sizeof(G));

	g->kind = kind;
	return g;
}

static bool gen_approx = true;	/* false: the exact dialect only (G91 leg) */

/* An atom: something `{~k}`, `?`, `*` may follow. */
static G *gen_seq(int depth, int len);
static G *
gen_atom(int depth)
{
	int			r = rnd(10);
	G		   *g;

	if (r < 6 || depth <= 0)
	{
		g = gnew(0);
		g->c = ALPHA[rnd(4)];
	}
	else if (r == 6)
		g = gnew(1);
	else if (r == 7)
	{
		int			i,
					n = 1 + rnd(2);

		g = gnew(2);
		g->neg = rnd(4) == 0;
		for (i = 0; i < n; i++)
			g->cls[i] = ALPHA[rnd(4)];
	}
	else if (r == 8)
	{
		G		   *a = gnew(4);

		a->l = gen_seq(depth - 1, 1 + rnd(3));
		a->r = gen_seq(depth - 1, 1 + rnd(3));
		g = a;					/* rendered parenthesized */
	}
	else
	{
		g = gen_seq(depth - 1, 2 + rnd(3));
		g = (g->kind == 3) ? g : g; /* group of a sequence */
		{
			G		   *grp = gnew(3);	/* a cat with r=NULL marks a group */

			grp->l = g;
			grp->r = NULL;
			g = grp;
		}
	}
	return g;
}

static G *
gen_piece(int depth)
{
	G		   *a = gen_atom(depth);
	int			r = rnd(12);

	if (r < 2 && gen_approx)
	{
		G		   *x = gnew(6);

		x->l = a;
		x->k = rnd(3);			/* 0, 1, 2 */
		return x;
	}
	if (r < 5)
	{
		G		   *x = gnew(5);
		static const int bounds[][2] = {{0, 1}, {0, -1}, {1, -1}, {2, 3}, {1, 2}, {2, 2}};
		int			b = rnd(6);

		x->l = a;
		x->m = bounds[b][0];
		x->n = bounds[b][1];
		return x;
	}
	return a;
}

static G *
gen_seq(int depth, int len)
{
	G		   *s = gen_piece(depth);
	int			i;

	for (i = 1; i < len; i++)
	{
		G		   *c = gnew(3);

		c->l = s;
		c->r = gen_piece(depth);
		s = c;
	}
	return s;
}

typedef struct Buf
{
	char		s[512];
	int			n;
} Buf;

static void
put(Buf *b, const char *t)
{
	while (*t && b->n < (int) sizeof(b->s) - 1)
		b->s[b->n++] = *t++;
	b->s[b->n] = 0;
}

static bool
needs_group(const G *g)
{
	return g->kind == 3 || g->kind == 4 || g->kind == 5 || g->kind == 6;
}

static void
render(Buf *b, const G *g)
{
	char		tmp[32];

	switch (g->kind)
	{
		case 0:
			tmp[0] = g->c;
			tmp[1] = 0;
			put(b, tmp);
			break;
		case 1:
			put(b, ".");
			break;
		case 2:
			put(b, g->neg ? "[^" : "[");
			put(b, g->cls);
			put(b, "]");
			break;
		case 3:
			if (g->r == NULL)
			{
				put(b, "(");
				render(b, g->l);
				put(b, ")");
			}
			else
			{
				render(b, g->l);
				render(b, g->r);
			}
			break;
		case 4:
			put(b, "(");
			render(b, g->l);
			put(b, "|");
			render(b, g->r);
			put(b, ")");
			break;
		case 5:
		case 6:
			if (needs_group(g->l))
			{
				put(b, "(");
				render(b, g->l);
				put(b, ")");
			}
			else
				render(b, g->l);
			if (g->kind == 6)
				snprintf(tmp, sizeof(tmp), "{~%d}", g->k);
			else if (g->m == 0 && g->n == 1)
				strcpy(tmp, "?");
			else if (g->m == 0 && g->n < 0)
				strcpy(tmp, "*");
			else if (g->m == 1 && g->n < 0)
				strcpy(tmp, "+");
			else if (g->m == g->n)
				snprintf(tmp, sizeof(tmp), "{%d}", g->m);
			else
				snprintf(tmp, sizeof(tmp), "{%d,%d}", g->m, g->n);
			put(b, tmp);
			break;
	}
}

/* Sample one string the pattern matches exactly (approx atoms at cost 0). */
static void
sample(Buf *b, const G *g)
{
	char		tmp[2] = {0, 0};
	int			i,
				n;

	switch (g->kind)
	{
		case 0:
			tmp[0] = g->c;
			put(b, tmp);
			break;
		case 1:
			tmp[0] = ALPHA[rnd(4)];
			put(b, tmp);
			break;
		case 2:
			if (!g->neg)
				tmp[0] = g->cls[rnd((int) strlen(g->cls))];
			else
			{
				do
					tmp[0] = ALPHA[rnd(4)];
				while (strchr(g->cls, tmp[0]));
			}
			put(b, tmp);
			break;
		case 3:
			sample(b, g->l);
			if (g->r)
				sample(b, g->r);
			break;
		case 4:
			sample(b, rnd(2) ? g->l : g->r);
			break;
		case 5:
			n = g->m + rnd((g->n < 0 ? 3 : g->n - g->m) + 1);
			for (i = 0; i < n; i++)
				sample(b, g->l);
			break;
		case 6:
			sample(b, g->l);
			break;
	}
}

static void
mutate(Buf *b)
{
	int			e = rnd(4),
				i;

	for (i = 0; i < e; i++)
	{
		int			op = rnd(3),
					p = b->n ? rnd(b->n + 1) : 0;

		if (op == 0 && b->n < 60)	/* insert */
		{
			memmove(b->s + p + 1, b->s + p, b->n - p + 1);
			b->s[p] = ALPHA[rnd(4)];
			b->n++;
		}
		else if (op == 1 && p < b->n)	/* delete */
		{
			memmove(b->s + p, b->s + p + 1, b->n - p);
			b->n--;
		}
		else if (p < b->n)		/* substitute */
			b->s[p] = ALPHA[rnd(4)];
	}
}

static int
count_approx(const char *p)
{
	int			n = 0;

	for (; (p = strstr(p, "{~")) != NULL; p += 2)
		n++;
	return n;
}

/* ---------------- the property ---------------- */

static bool
token_has(const char *t, int tn, const int32 cp[3])
{
	int			i;

	for (i = 0; i + 3 <= tn; i++)
		if (t[i] == cp[0] && t[i + 1] == cp[1] && t[i + 2] == cp[2])
			return true;
	return false;
}

static bool
cnf_admits(const TrigramQuery *q, const char *t, int tn)
{
	int			ci,
				ai;

	if (q->always_true)
		return true;
	for (ci = 0; ci < q->n; ci++)
	{
		bool		any = false;

		for (ai = 0; ai < q->conjuncts[ci].n && !any; ai++)
			any = token_has(t, tn, q->conjuncts[ci].alts[ai].cp);
		if (!any)
			return false;
	}
	return true;
}

static int
tre_accepts(void *h, const char *t, int tn)
{
	unsigned int w[512];
	int			i;

	for (i = 0; i < tn; i++)
		w[i] = (unsigned char) t[i];
	return weave_match_wide(h, w, tn);
}

static void *
compile(const char *p)
{
	unsigned int w[512];
	int			i,
				n = (int) strlen(p),
				err;

	for (i = 0; i < n; i++)
		w[i] = (unsigned char) p[i];
	return weave_compile_pattern(w, n, &err);
}

/* Fixed cases first: each is a pattern, a token TRE accepts, and why it matters. */
static const struct
{
	const char *pat,
			   *tok;
}			fixed[] = {
	{"xa+y", "xaay"},			/* variable repetition: "xay" is not required */
	{"xa{1,3}y", "xaaay"},
	{"(ab){1,2}c", "ababc"},
	{"(abc){~1}", "abd"},		/* an edit inside the approx atom */
	{"x(abc){~1}y", "xabdy"},
	{"(abcd){~2}", "axcd"},
	{"ab(cd){~1}", "abd"},		/* deletion at the atom boundary */
	{"(ab.cd){~0}", "abxcd"},
	{"ab.cd", "abxcd"},			/* the G88 tiling example, exact */
	{"^(abc){~1}d$", "xbcd"},
};

int
main(int argc, char **argv)
{
	int			ncases = (argc > 1) ? atoi(argv[1]) : 20000;
	bool		exact_only = (argc > 2 && strcmp(argv[2], "exact") == 0);
	long		checks = 0,
				positives = 0,
				narrowing = 0,
				fails = 0;
	int			i,
				j;

	for (i = 0; i < (int) (sizeof(fixed) / sizeof(fixed[0])) + ncases; i++)
	{
		Buf			pat = {{0}, 0};
		G		   *g = NULL;
		WeaveParseCtx ctx;
		TrigramQuery q;
		void	   *h;
		regex_t		posix;
		sigjmp_buf	jb;
		bool		ok;
		int			ntok = 0;

		gen_approx = !exact_only;
		if (i < (int) (sizeof(fixed) / sizeof(fixed[0])))
		{
			if (exact_only && strstr(fixed[i].pat, "{~"))
				continue;
			put(&pat, fixed[i].pat);
		}
		else
		{
			/* TRE refuses (and pattern_cache.c rejects) more than three approx atoms */
			do
			{
				pat.n = 0;
				pat.s[0] = 0;
				g = gen_seq(2, 1 + rnd(6));
				render(&pat, g);
			} while (count_approx(pat.s) > 3);
		}

		h = compile(pat.s);
		if (h == NULL)
		{
			printf("FAIL: TRE refused generated pattern /%s/\n", pat.s);
			fails++;
			continue;
		}

		if (exact_only && regcomp(&posix, pat.s, REG_EXTENDED | REG_NOSUB) != 0)
		{
			printf("FAIL: POSIX regcomp refused /%s/\n", pat.s);
			fails++;
			weave_free_pattern(h);
			continue;
		}

		PG_exception_stack = &jb;
		if (sigsetjmp(jb, 1) == 0)
			ok = weave_parse_regex(&ctx, pat.s, pat.n) &&
				regex_extract_query(&ctx, 0, &q);
		else
			ok = false;
		PG_exception_stack = NULL;
		if (!ok)
		{
			printf("FAIL: pg_tre parser refused /%s/ (%s)\n", pat.s, ctx.errmsg);
			fails++;
			weave_free_pattern(h);
			if (exact_only)
				regfree(&posix);
			continue;
		}
		if (!q.always_true && q.n > 0)
			narrowing++;

		for (j = 0; j < (g ? 60 : 1); j++)
		{
			Buf			t = {{0}, 0};
			int			m;

			if (!g)
				put(&t, fixed[i].tok);
			else if (j % 3 == 2)
			{
				int			n = rnd(9),
							c;

				for (c = 0; c < n; c++)
				{
					char		s[2] = {ALPHA[rnd(4)], 0};

					put(&t, s);
				}
			}
			else
			{
				sample(&t, g);
				if (t.n > 60)
					continue;
				mutate(&t);
			}

			m = tre_accepts(h, t.s, t.n);
			if (exact_only && m >= 0 && (regexec(&posix, t.s, 0, NULL, 0) == 0) != (m == 1))
			{
				printf("FAIL: TRE and POSIX regexec disagree on '%s' for /%s/\n", t.s, pat.s);
				fails++;
			}
			if (m < 0)
			{
				printf("FAIL: TRE error matching /%s/ against '%s'\n", pat.s, t.s);
				fails++;
				continue;
			}
			checks++;
			if (!g && m != 1)
			{
				printf("FAIL: fixed case: TRE does not accept '%s' for /%s/\n", t.s, pat.s);
				fails++;
				continue;
			}
			if (m == 1)
			{
				positives++;
				ntok++;
				if (!cnf_admits(&q, t.s, t.n))
				{
					if (fails < 20)
						printf("FAIL: prefilter drops '%s', which TRE accepts for /%s/\n",
							   t.s, pat.s);
					fails++;
				}
			}
		}
		weave_free_pattern(h);
		if (exact_only)
			regfree(&posix);
	}

	printf("%ld checks, %ld TRE-accepted tokens, %ld narrowing patterns, %ld failures\n",
		   checks, positives, narrowing, fails);
	if (positives < checks / 20 || narrowing < ncases / 10)
	{
		printf("FAIL: generator too weak to test anything\n");
		return 1;
	}
	return fails ? 1 : 0;
}
