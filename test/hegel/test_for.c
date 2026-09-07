/*
 * Property-based tests for the pg_weave Frame-of-Reference (FOR) integer codec
 * (weave/for.h), the compression core of the weave posting blocks.  Guards the
 * P1 doclen-sidecar work by exercising pack/unpack/random-access/bitwidth over
 * the full uint64 range and full 128-value blocks.
 *
 * Standalone: weave/for.h is pure C (no PostgreSQL deps), so we include it
 * directly -- same source the extension compiles.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hegel/hegel.h>
#include <hegel/generators.h>

#include "weave/for.h"

/* WEAVE_BLOCK_SIZE in the extension is 128; a column is at most that many. */
#define MAXN 128
/* worst-case column bytes: 1 width byte + n*64 bits. */
#define MAXBUF (1 + (MAXN * 64 + 7) / 8)

static hegel_session *session;

/*
 * Draw one full-range uint64.  hegel-c draws int64 in [min,max]; we take the
 * whole int64 range and reinterpret the bits, which covers all uint64 values.
 * We deliberately do NOT constrain -- 0, 1, powers of two and UINT64_MAX are
 * the interesting cases and must be reachable.
 */
static uint64
draw_u64(hegel_test_case *tc)
{
	int64_t		x = hegel_draw_int(tc, hegel_integers(INT64_MIN, INT64_MAX));

	return (uint64) x;
}

/* Draw a column: n in [0,128] (drawn separately so we reach full blocks) and n
 * full-range uint64 values. */
static int
draw_column(hegel_test_case *tc, uint64 *vals)
{
	int			n = (int) hegel_draw_int(tc, hegel_integers(0, MAXN));
	int			i;

	for (i = 0; i < n; i++)
		vals[i] = draw_u64(tc);
	return n;
}

/* ---- Property: pack then unpack is the identity. Core P1 guard. ---- */
static void
prop_roundtrip(hegel_test_case *tc, void *ctx)
{
	uint64		vals[MAXN];
	uint64		out[MAXN];
	unsigned char buf[MAXBUF];
	int			i;
	int			n = draw_column(tc, vals);

	(void) ctx;
	weave_for_pack(vals, n, buf);
	weave_for_unpack(buf, n, out);
	for (i = 0; i < n; i++)
		assert_true(out[i] == vals[i]);
}

/* ---- Property: weave_for_get(buf,i) == unpack(buf)[i] for every i. ---- */
static void
prop_random_access(hegel_test_case *tc, void *ctx)
{
	uint64		vals[MAXN];
	uint64		out[MAXN];
	unsigned char buf[MAXBUF];
	int			i;
	int			n = draw_column(tc, vals);

	(void) ctx;
	weave_for_pack(vals, n, buf);
	weave_for_unpack(buf, n, out);
	for (i = 0; i < n; i++)
		assert_true(weave_for_get(buf, i) == out[i]);
}

/*
 * ---- Property: byte-length contract. pack returns exactly the bytes unpack
 * consumes; and equals 1 for an all-zero column (width 0), else 1+(n*w+7)/8.
 */
static void
prop_bytelen(hegel_test_case *tc, void *ctx)
{
	uint64		vals[MAXN];
	uint64		out[MAXN];
	unsigned char buf[MAXBUF];
	uint64		maxv = 0;
	int			i;
	int			w;
	int			expect;
	int			packed;
	int			consumed;
	int			n = draw_column(tc, vals);

	(void) ctx;
	packed = weave_for_pack(vals, n, buf);
	consumed = weave_for_unpack(buf, n, out);
	assert_int_equal(packed, consumed);
	assert_int_equal(packed, weave_for_bytelen(buf, n));

	for (i = 0; i < n; i++)
		if (vals[i] > maxv)
			maxv = vals[i];
	w = weave_bitwidth(maxv);
	expect = (w == 0) ? 1 : 1 + (n * w + 7) / 8;
	assert_int_equal(packed, expect);
}

/*
 * ---- Property: weave_bitwidth(v) is the minimal number of bits to represent v.
 * i.e. v < (1<<w) and (w==0 or v >= (1<<(w-1))).  Boundaries 0, 1, powers of
 * two and UINT64_MAX are drawn by the full-range generator.
 */
static void
prop_bitwidth(hegel_test_case *tc, void *ctx)
{
	uint64		v = draw_u64(tc);
	int			w = weave_bitwidth(v);

	(void) ctx;
	assert_true(w >= 0 && w <= 64);
	if (v == 0)
	{
		assert_int_equal(w, 0);
		return;
	}
	/* v fits in w bits: v < 2^w (2^64 wraps to 0, treat width 64 as always ok) */
	if (w < 64)
		assert_true(v < ((uint64) 1 << w));
	/* w is minimal: top bit of v is bit (w-1), so v >= 2^(w-1) */
	assert_true(v >= ((uint64) 1 << (w - 1)));
}

/*
 * ---- Property: doclen quantization (weave_doclen_to_byte / weave_byte_to_doclen).
 * The v4 sidecar codec.  Three properties over the realistic length domain
 * [0, 16M tokens]:
 *  (a) MONOTONIC: len1 <= len2  =>  enc(len1) <= enc(len2).  BM25 length-norm
 *      must not invert two documents' relative lengths.
 *  (b) IDEMPOTENT: enc(dec(enc(len))) == enc(len).  A re-encode after decode
 *      (what a merge/vacuum rewrite does) does not drift the byte.
 *  (c) EXACT for small lengths 0..7 (no quantization loss where it matters most
 *      for short-doc IDF weighting).
 */
static void
prop_doclen_quant(hegel_test_case *tc, void *ctx)
{
	uint32		len = (uint32) hegel_draw_int(tc, hegel_integers(0, 16777216));
	uint32		len2 = (uint32) hegel_draw_int(tc, hegel_integers(0, 16777216));
	uint8		b = weave_doclen_to_byte(len);
	uint8		b2 = weave_doclen_to_byte(len2);

	(void) ctx;
	/* (a) monotonic */
	if (len <= len2)
		assert_true(b <= b2);
	else
		assert_true(b >= b2);
	/* (b) idempotent under decode->encode */
	assert_int_equal(weave_doclen_to_byte(weave_byte_to_doclen(b)), b);
	/* (c) exact for 0..7 */
	if (len <= 7)
	{
		assert_int_equal((uint32) b, len);
		assert_int_equal(weave_byte_to_doclen(b), len);
	}
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

static void test_roundtrip(void **state) { RUN(prop_roundtrip); }
static void test_random_access(void **state) { RUN(prop_random_access); }
static void test_bytelen(void **state) { RUN(prop_bytelen); }
static void test_bitwidth(void **state) { RUN(prop_bitwidth); }
static void test_doclen_quant(void **state) { RUN(prop_doclen_quant); }

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
		cmocka_unit_test(test_roundtrip),
		cmocka_unit_test(test_random_access),
		cmocka_unit_test(test_bytelen),
		cmocka_unit_test(test_bitwidth),
		cmocka_unit_test(test_doclen_quant),
	};

	return cmocka_run_group_tests(tests, setup, teardown);
}
