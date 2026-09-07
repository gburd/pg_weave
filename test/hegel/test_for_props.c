/*
 * Property-based SAFETY tests for the pg_weave FOR codec (weave/for.h),
 * complementing test_for.c.  test_for.c proves the clean round-trip / random
 * access / bytelen / bitwidth contracts for well-formed columns; this file
 * targets the failure the crashes actually came from: unpacking a column whose
 * width byte or length is hostile must stay strictly inside the caller's
 * output and input buffers.  We surround out[] and buf[] with canary sentinels
 * and, for the fuzz cases, allocate exact-size buffers so ASan aborts on any
 * over-read/over-write.  Run under ASan in CI.
 *
 * No round-trip assertions here -- that is test_for.c's job; this file only
 * adds bounds/safety properties, no duplication.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <cmocka.h>

#include <hegel/hegel.h>
#include <hegel/generators.h>

#include "weave/for.h"

#define MAXN 128
#define MAXBUF (1 + (MAXN * 64 + 7) / 8)

/* sentinel word patterns for canary checks. */
#define CANARY 0xA5A5A5A5A5A5A5A5ULL

static hegel_session *session;

/* Draw n in [0,128], drawn separately so full blocks (and n==0/1) are reached. */
static int
draw_n(hegel_test_case *tc)
{
	return (int) hegel_draw_int(tc, hegel_integers(0, MAXN));
}

/* Fill a buffer with random bytes; return length written. */
static void
draw_buf(hegel_test_case *tc, unsigned char *buf, int nbytes)
{
	int			i;

	for (i = 0; i < nbytes; i++)
		buf[i] = (unsigned char) hegel_draw_int(tc, hegel_integers(0, 255));
}

/*
 * ---- Property: unpack writes EXACTLY n values and reads only
 * weave_for_bytelen(buf,n) bytes. ----
 * We over-allocate out[] and put a canary just past out[n]; unpack must not
 * touch it.  buf is sized to the FULL worst case and padded with a canary
 * region past bytelen(buf,n); unpack must not read into it.  This is the exact
 * write/read overflow the crashes came from.
 */
static void
prop_unpack_bounds(hegel_test_case *tc, void *ctx)
{
	uint64		out[MAXN + 1];
	unsigned char buf[MAXBUF + 8];
	int			n = draw_n(tc);
	int			width = (int) hegel_draw_int(tc, hegel_integers(0, 64));
	int			bytelen;
	int			consumed;
	int			i;

	(void) ctx;
	/* width byte + random payload; then a canary region after bytelen. */
	buf[0] = (unsigned char) width;
	bytelen = weave_for_bytelen(buf, n);
	draw_buf(tc, buf + 1, bytelen - 1);
	for (i = bytelen; i < MAXBUF + 8; i++)
		buf[i] = 0xC3;			/* canary tail; unpack must not read here */

	out[n] = CANARY;			/* canary just past the n values written */

	consumed = weave_for_unpack(buf, n, out);

	/* reads exactly bytelen bytes... */
	assert_int_equal(consumed, bytelen);
	/* ...and writes exactly n values -- the (n+1)th slot is untouched. */
	assert_true(out[n] == CANARY);
}

/*
 * ---- Property: bytelen agreement across all widths 0..64. ----
 * weave_for_bytelen(buf,n) must equal the value weave_for_unpack returns, for
 * every width byte in [0,64] and every n.  A disagreement means a column-skip
 * (bytelen) and a column-decode (unpack) walk the stream by different amounts
 * -- the precise desync that corrupts the posting cursor.
 */
static void
prop_bytelen_agreement(hegel_test_case *tc, void *ctx)
{
	uint64		out[MAXN];
	unsigned char buf[MAXBUF];
	int			n = draw_n(tc);
	int			width = (int) hegel_draw_int(tc, hegel_integers(0, 64));
	int			bytelen;

	(void) ctx;
	buf[0] = (unsigned char) width;
	bytelen = weave_for_bytelen(buf, n);
	draw_buf(tc, buf + 1, bytelen - 1);

	assert_int_equal(weave_for_bytelen(buf, n), weave_for_unpack(buf, n, out));
}

/*
 * ---- Property: corrupt width byte (65..255) is safe. ----
 * A width > 64 can never be produced by weave_for_pack, but a corrupt on-disk
 * byte can carry it.  unpack must stay inside the caller's out[n] buffer and
 * inside the bytes it declares it consumed (weave_for_bytelen) -- garbage output
 * values are acceptable, an OOB read/write is not.  Exact-size buffers +
 * canaries + ASan enforce it.
 */
static void
prop_corrupt_width_safe(hegel_test_case *tc, void *ctx)
{
	int			n = draw_n(tc);
	int			width = (int) hegel_draw_int(tc, hegel_integers(65, 255));
	int			bytelen = 1 + (n * width + 7) / 8;	/* what unpack will read */
	unsigned char *buf;
	uint64	   *out;
	int			consumed;

	(void) ctx;
	/*
	 * Exact-size allocations: any read past buf[bytelen-1] or write past
	 * out[n-1] is an ASan-detected heap overflow.  We give buf exactly bytelen
	 * bytes and out exactly n slots (+1 canary slot to assert the write bound).
	 */
	buf = (unsigned char *) malloc((size_t) bytelen);
	out = (uint64 *) malloc((size_t) (n + 1) * sizeof(uint64));
	assert_true(buf != NULL && out != NULL);

	buf[0] = (unsigned char) width;
	draw_buf(tc, buf + 1, bytelen - 1);
	out[n] = CANARY;

	consumed = weave_for_unpack(buf, n, out);

	assert_int_equal(consumed, bytelen);	/* declared read stays as advertised */
	assert_true(out[n] == CANARY);			/* no write past out[n-1] */

	free(buf);
	free(out);
}

/* ---- cmocka wrappers ---- */
#define RUN(prop) do { \
	hegel_session *s = (hegel_session *) *state; \
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS; \
	hegel_results r; \
	settings.max_examples = 500; \
	r = hegel_run_test(s, prop, NULL, &settings); \
	assert_true(r.passed); \
	hegel_results_free(&r); \
} while (0)

static void test_unpack_bounds(void **state) { RUN(prop_unpack_bounds); }
static void test_bytelen_agreement(void **state) { RUN(prop_bytelen_agreement); }
static void test_corrupt_width_safe(void **state) { RUN(prop_corrupt_width_safe); }

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
		cmocka_unit_test(test_unpack_bounds),
		cmocka_unit_test(test_bytelen_agreement),
		cmocka_unit_test(test_corrupt_width_safe),
	};

	return cmocka_run_group_tests(tests, setup, teardown);
}
