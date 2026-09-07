/*
 * Property-based tests for the pg_weave Levenshtein automaton (pg_weave_lev.c),
 * the pure byte-wise bounded-DP DFA used for `term~k` fuzzy matching.
 *
 * pg_weave_lev.c has no #includes of its own; it only needs int16/bool/Min in
 * scope (normally supplied by postgres.h when the extension #includes it).  We
 * provide those shims and include the .c directly -- single source of truth,
 * no duplication.  The automaton operates on raw bytes, so the oracle below is
 * byte-wise Levenshtein, computed independently of the code under test.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <cmocka.h>

#include <hegel/hegel.h>
#include <hegel/generators.h>

/* Backend-type shims pg_weave_lev.c expects from postgres.h / c.h. */
typedef int16_t int16;
#ifndef Min
#define Min(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "pg_weave_lev.c"

/* WEAVE_LEV_MAXQ (255) is the max query length the automaton handles; the
 * extension falls back for longer queries, so it is a genuine domain bound. */
#define MAXQ WEAVE_LEV_MAXQ
#define MAXCAND 300

static hegel_session *session;

/* Independent byte-wise Levenshtein edit distance (the oracle). */
static int
naive_edit(const unsigned char *a, int la, const unsigned char *b, int lb)
{
	int			prev[MAXCAND + 2];
	int			cur[MAXCAND + 2];
	int			i,
				j;

	for (j = 0; j <= lb; j++)
		prev[j] = j;
	for (i = 1; i <= la; i++)
	{
		cur[0] = i;
		for (j = 1; j <= lb; j++)
		{
			int			sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
			int			ins = cur[j - 1] + 1;
			int			del = prev[j] + 1;

			cur[j] = Min(sub, Min(ins, del));
		}
		memcpy(prev, cur, sizeof(int) * (lb + 1));
	}
	return prev[lb];
}

/* Draw a byte buffer of the given length via hegel int draws (full 0..255). */
static int
draw_bytes(hegel_test_case *tc, unsigned char *out, int minlen, int maxlen)
{
	int			n = (int) hegel_draw_int(tc, hegel_integers(minlen, maxlen));
	int			i;

	for (i = 0; i < n; i++)
		out[i] = (unsigned char) hegel_draw_int(tc, hegel_integers(0, 255));
	return n;
}

/*
 * ---- Property: the DFA accepts iff naive edit distance <= k. ----
 * Oracle equivalence, the strongest correctness check.
 */
static void
prop_equivalence(hegel_test_case *tc, void *ctx)
{
	unsigned char q[MAXQ + 1];
	unsigned char cand[MAXCAND + 1];
	WeaveLevAut	aut;
	int			deadlen;
	int			qlen;
	int			clen;
	int			k;
	int			dist;
	bool		got;

	(void) ctx;
	qlen = draw_bytes(tc, q, 0, MAXQ);
	clen = draw_bytes(tc, cand, 0, MAXCAND);
	k = (int) hegel_draw_int(tc, hegel_integers(0, 8));

	aut.q = q;
	aut.m = qlen;
	aut.k = k;

	got = weave_lev_match_prefix(&aut, cand, clen, &deadlen);
	dist = naive_edit(q, qlen, cand, clen);

	assert_true(got == (dist <= k));
	/* deadlen is a valid index into cand (or clen when never dead). */
	assert_true(deadlen >= 0 && deadlen <= clen);
}

/*
 * ---- Property: robustness -- arbitrary query + candidate bytes and any k in
 * range never crash, and the reported dead prefix is well-formed.
 */
static void
prop_no_crash(hegel_test_case *tc, void *ctx)
{
	unsigned char q[MAXQ + 1];
	unsigned char cand[MAXCAND + 1];
	WeaveLevAut	aut;
	int			deadlen = -1;
	int			qlen;
	int			clen;

	(void) ctx;
	qlen = draw_bytes(tc, q, 0, MAXQ);
	clen = draw_bytes(tc, cand, 0, MAXCAND);

	aut.q = q;
	aut.m = qlen;
	aut.k = (int) hegel_draw_int(tc, hegel_integers(0, 64));

	(void) weave_lev_match_prefix(&aut, cand, clen, &deadlen);
	assert_true(deadlen >= 0 && deadlen <= clen);
}

#define RUN(prop) do { \
	hegel_session *s = (hegel_session *) *state; \
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS; \
	hegel_results r; \
	settings.max_examples = 500; \
	r = hegel_run_test(s, prop, NULL, &settings); \
	assert_true(r.passed); \
	hegel_results_free(&r); \
} while (0)

static void test_equivalence(void **state) { RUN(prop_equivalence); }
static void test_no_crash(void **state) { RUN(prop_no_crash); }

static int
setup(void **state)
{
	session = hegel_session_new();
	*state = session;
	return 0;
}

static int
teardown(void **state)
{
	hegel_session_free((hegel_session *) *state);
	return 0;
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_equivalence),
		cmocka_unit_test(test_no_crash),
	};

	return cmocka_run_group_tests(tests, setup, teardown);
}
