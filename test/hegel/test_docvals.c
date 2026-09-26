/*-------------------------------------------------------------------------
 *
 * test_docvals.c
 *		Property test for the pg_weave int8 scalar docvalues store
 *		(include/weave/docvals.h), the backend-independent value array a scalar
 *		facet predicate is evaluated over.  No PostgreSQL, no cmocka, no hegel:
 *		plain C, its own PRNG and check counters, so `make check-standalone` --
 *		the dependency-free property gate -- can gate a build on it.
 *
 * WHY THIS TEST EXISTS -- the (C5) crux.  A scalar predicate that silently drops
 * one valid docid returns a plausible-but-wrong answer that NO fixed-output
 * regression test would catch: the rows returned are all real, one true row is
 * just missing.  So PRED below compares weave_dv_eval_int8() against a
 * straight-line reference loop over the full int64 range, for every one of the
 * five btree strategies and several boundary constants, and asserts the two
 * agree EXACTLY -- same count, same docids, strictly ascending.
 *
 * VALIDATE-REJECTS is the defence-in-depth half: the on-disk bytes are not
 * trusted, so every corrupted-header mutation and every truncated length must be
 * refused by weave_docvals_validate() (a non-NULL reason) without reading past
 * the buffer.
 *
 * COVERAGE IS ASSERTED, NOT HOPED FOR (AGENTS.md hard rule 11: a harness can make
 * a number up).  A generator that never produced a non-empty result, or never an
 * empty one, or never exercised one of the five operators, would make PRED pass
 * while testing far less than it claims -- so the run FAILS if any of those
 * coverage counters is zero.
 *
 * PRNG is xorshift64* seeded with a FIXED constant so any failure reproduces
 * exactly, the same shape test_lexbound.c and test_pagekind.c use.
 *
 * Build (the Makefile passes -DWEAVE_DOCVALS_TEST_HELPERS; do not define it here):
 *		cc -O2 -Wall -Wextra -Wno-unused-parameter -I include \
 *		   -DWEAVE_DOCVALS_TEST_HELPERS -o /tmp/td test/hegel/test_docvals.c -lm
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_docvals.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "weave/docvals.h"

/* A segment's dense id space is at most WEAVE_BLOCK_SIZE-ish; 128 here. */
#define MAXN 128
#define TRIALS 200000

/* xorshift64*, seeded fixed so a failure reproduces. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t
rng_next(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* A full-range int64: every bit pattern is reachable. */
static int64_t
draw_i64(void)
{
	return (int64_t) rng_next();
}

/*
 * A small-range int64 in [-4, 4].  Used for ~1/3 of trials so that duplicate
 * values -- and therefore EQ queries matching two or more docids -- actually
 * occur; a full-range draw makes every value distinct, so a stop-at-first-match
 * or drop-duplicate bug in weave_dv_eval_int8() would go unseen.
 */
static int64_t
draw_i64_small(void)
{
	return (int64_t) (rng_next() % 9u) - 4;	/* [-4, 4] */
}

/* Global counters. */
static long checks = 0;
static long failures = 0;

/* Per-property check counts. */
static long pred_checks = 0;
static long reject_checks = 0;
static long trunc_checks = 0;	/* outcap-truncation sub-check (FIX 5) */

/* Coverage counters, asserted non-zero at the end. */
static long cov_nonempty = 0;	/* some (op,c) produced a non-empty result set */
static long cov_empty = 0;		/* some (op,c) produced an empty result set */
static long cov_op[6] = {0};	/* each of the 5 operators (indexed 1..5) */
static long cov_present_eq = 0; /* EQ on a present value matched >= 1 docid */
static long cov_smallrange = 0; /* a trial drew values from the small range */
static long cov_eq_multi = 0;	/* an EQ query matched TWO OR MORE docids */
static long cov_dup_adjacent = 0;	/* a non-empty result had adjacent-equal
									 * values (ascending order with dups holds) */
static long cov_trunc = 0;		/* outcap-truncation branch actually fired */

static const WeaveDvStrat ops[5] = {
	WEAVE_DV_LT, WEAVE_DV_LE, WEAVE_DV_EQ, WEAVE_DV_GE, WEAVE_DV_GT
};

/* Straight-line reference: docids in [0,n) with (vals[i] op c), ascending. */
static int
ref_eval(const int64_t *vals, int n, WeaveDvStrat op, int64_t c, uint32_t *out)
{
	int			i;
	int			count = 0;

	for (i = 0; i < n; i++)
	{
		int			match;

		switch (op)
		{
			case WEAVE_DV_LT:
				match = (vals[i] < c);
				break;
			case WEAVE_DV_LE:
				match = (vals[i] <= c);
				break;
			case WEAVE_DV_EQ:
				match = (vals[i] == c);
				break;
			case WEAVE_DV_GE:
				match = (vals[i] >= c);
				break;
			case WEAVE_DV_GT:
				match = (vals[i] > c);
				break;
			default:
				match = 0;
				break;
		}
		if (match)
			out[count++] = (uint32_t) i;
	}
	return count;
}

/*
 * PRED: the (C5) exactness property.  Draw a column, build a store, assert the
 * validator accepts it, then for every operator crossed with several boundary
 * constants assert the evaluator emits exactly the reference docids in ascending
 * order.
 */
static void
prop_pred(void)
{
	int64_t		vals[MAXN];
	uint32_t	out[MAXN];
	uint32_t	ref[MAXN];
	int64_t		consts[5];
	/* One reusable store buffer at MAX size. */
	unsigned char buf[sizeof(WeaveDocvalsHeader) + 8 * MAXN + 16];
	int			n = (int) (rng_next() % (MAXN + 1));	/* [0,128] */
	int			i;
	int			oi;
	int			ci;
	int64_t		present;
	int64_t		absent;
	const char *why;
	size_t		len;
	int			small = ((rng_next() % 3u) == 0);	/* ~1/3 small-range */

	for (i = 0; i < n; i++)
		vals[i] = small ? draw_i64_small() : draw_i64();

	if (small)
		cov_smallrange++;

	len = weave_docvals_store_len((uint32_t) n);
	weave_docvals_build(buf, vals, (uint32_t) n);

	/* A well-formed store must validate. */
	why = weave_docvals_validate(buf, len);
	checks++;
	pred_checks++;
	if (why != NULL)
	{
		failures++;
		if (failures <= 20)
			printf("FAIL PRED %s:%d: valid store rejected (n=%d): %s\n",
				   __FILE__, __LINE__, n, why);
	}

	/* A value known present (vals[rng%n] if any), and one known absent. */
	present = (n > 0) ? vals[rng_next() % (uint32_t) n] : 0;
	absent = 0;
	for (;;)
	{
		int			seen = 0;

		for (i = 0; i < n; i++)
			if (vals[i] == absent)
			{
				seen = 1;
				break;
			}
		if (!seen)
			break;
		absent++;				/* terminates within n+1 tries */
	}

	consts[0] = INT64_MIN;
	consts[1] = INT64_MAX;
	consts[2] = 0;
	consts[3] = present;
	consts[4] = absent;

	for (oi = 0; oi < 5; oi++)
	{
		for (ci = 0; ci < 5; ci++)
		{
			int			gc = weave_dv_eval_int8(buf, ops[oi], consts[ci],
												out, (uint32_t) n);
			int			wc = ref_eval(vals, n, ops[oi], consts[ci], ref);
			int			k;
			int			ok = 1;

			checks++;
			pred_checks++;

			if (gc != wc)
				ok = 0;
			else
			{
				for (k = 0; k < wc; k++)
					if (out[k] != ref[k])
					{
						ok = 0;
						break;
					}
				for (k = 1; ok && k < gc; k++)
					if (!(out[k] > out[k - 1]))
					{
						ok = 0;
						break;
					}
			}

			if (!ok)
			{
				failures++;
				if (failures <= 20)
					printf("FAIL PRED %s:%d: n=%d op=%d c=%" PRId64
						   " expected %d got %d\n",
						   __FILE__, __LINE__, n, (int) ops[oi],
						   consts[ci], wc, gc);
			}

			/* Coverage. */
			if (gc > 0)
				cov_nonempty++;
			else
				cov_empty++;
			cov_op[(int) ops[oi]]++;
			if (ops[oi] == WEAVE_DV_EQ && ci == 3 && n > 0 && gc > 0)
				cov_present_eq++;

			/*
			 * FIX 4 coverage: an EQ query that matched two or more docids can
			 * only arise with duplicate values, which the small-range draws
			 * make happen.  And a non-empty result whose adjacent emitted
			 * docids carry EQUAL values proves the ascending-order guarantee
			 * still holds when duplicates are present (a stop-at-first-match or
			 * drop-duplicate bug would break one or both).
			 */
			if (ops[oi] == WEAVE_DV_EQ && gc >= 2)
				cov_eq_multi++;
			for (k = 1; k < gc; k++)
				if (vals[out[k]] == vals[out[k - 1]])
				{
					cov_dup_adjacent++;
					break;
				}
		}
	}
}

/*
 * OUTCAP-TRUNCATION (FIX 5).  weave_dv_eval_int8() always RETURNS the true total
 * match count, but only WRITES the first `outcap` ascending matches -- the write
 * is guarded by `count < outcap`, count is bumped unconditionally, and the
 * returned value is that final count.  The main property loop always passes
 * outcap == n, so that guard never truncates there.  Here we force it: build a
 * small-range store (so multiple docids match), pick GE INT64_MIN (every docid
 * matches, so the reference count is n and n >= 2 whenever it fires), call with a
 * deliberately small outcap, and assert:
 *   - the RETURN value is still the true total (== reference count, > outcap), and
 *   - exactly the first `outcap` ascending reference docids were written, and
 *   - nothing past outcap was written.
 */
static void
prop_trunc(void)
{
	int64_t		vals[MAXN];
	uint32_t	out[MAXN];
	uint32_t	ref[MAXN];
	unsigned char buf[sizeof(WeaveDocvalsHeader) + 8 * MAXN + 16];
	int			n = (int) (rng_next() % (MAXN + 1));
	int			i;
	int			refc;
	int			gc;
	uint32_t	outcap;
	int			k;
	int			ok = 1;

	/* Need at least two matches for a meaningful truncation; else skip. */
	if (n < 2)
		return;

	for (i = 0; i < n; i++)
		vals[i] = draw_i64_small();

	weave_docvals_build(buf, vals, (uint32_t) n);

	/* GE INT64_MIN matches every docid: reference is [0,n), count == n. */
	refc = ref_eval(vals, n, WEAVE_DV_GE, INT64_MIN, ref);

	/* A small cap strictly below the reference count exercises truncation. */
	outcap = 1u;

	/* Poison the tail so an over-write past outcap is detectable. */
	for (i = 0; i < n; i++)
		out[i] = 0xFFFFFFFFu;

	gc = weave_dv_eval_int8(buf, WEAVE_DV_GE, INT64_MIN, out, outcap);

	checks++;
	trunc_checks++;

	/* Return value is the TRUE total, not the truncated write count. */
	if (gc != refc)
		ok = 0;
	/* Only the first `outcap` ascending matches were written ... */
	for (k = 0; ok && k < (int) outcap; k++)
		if (out[k] != ref[k])
			ok = 0;
	/* ... and nothing past outcap was touched (poison survives). */
	for (k = (int) outcap; ok && k < n; k++)
		if (out[k] != 0xFFFFFFFFu)
			ok = 0;

	if (!ok)
	{
		failures++;
		if (failures <= 20)
			printf("FAIL TRUNC %s:%d: n=%d outcap=%u refc=%d got=%d "
				   "(expected return==refc, first outcap docids written)\n",
				   __FILE__, __LINE__, n, outcap, refc, gc);
	}
	else if ((uint32_t) gc > outcap)
		cov_trunc++;			/* the guard actually truncated a write */
}

/*
 * VALIDATE-REJECTS: build a valid store, then for each mutation on a COPY assert
 * the validator returns non-NULL (and does not read out of bounds).  Also two
 * truncated-length probes.
 */
static void
prop_reject(void)
{
	int64_t		vals[MAXN];
	unsigned char base[sizeof(WeaveDocvalsHeader) + 8 * MAXN + 16];
	unsigned char cpy[sizeof(WeaveDocvalsHeader) + 8 * MAXN + 16];
	int			n = (int) (rng_next() % (MAXN + 1));
	int			i;
	size_t		len;
	WeaveDocvalsHeader *h;

	for (i = 0; i < n; i++)
		vals[i] = draw_i64();

	len = weave_docvals_store_len((uint32_t) n);
	weave_docvals_build(base, vals, (uint32_t) n);

#define EXPECT_REJECT(desc) \
	do { \
		const char *r; \
		checks++; \
		reject_checks++; \
		r = weave_docvals_validate(cpy, len); \
		if (r == NULL) \
		{ \
			failures++; \
			if (failures <= 20) \
				printf("FAIL REJECT %s:%d: %s wrongly accepted (n=%d)\n", \
					   __FILE__, __LINE__, (desc), n); \
		} \
	} while (0)

	/* bad magic */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->magic ^= 0xFFu;
	EXPECT_REJECT("bad magic");

	/* version = 2 */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->version = 2;
	EXPECT_REJECT("version=2");

	/* typid_kind = 2 */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->typid_kind = 2;
	EXPECT_REJECT("typid_kind=2");

	/* values_off wrong (0) */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->values_off = 0;
	EXPECT_REJECT("values_off=0");

	/* null_off = 1 */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->null_off = 1;
	EXPECT_REJECT("null_off=1");

	/* zonemap_off = 1 */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->zonemap_off = 1;
	EXPECT_REJECT("zonemap_off=1");

	/* reserved = 1 */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->reserved = 1;
	EXPECT_REJECT("reserved=1");

	/* ndocs increased so stated length exceeds the buffer */
	memcpy(cpy, base, len);
	h = (WeaveDocvalsHeader *) cpy;
	h->ndocs = (uint32) (n + 1);
	EXPECT_REJECT("ndocs+1");

#undef EXPECT_REJECT

	/*
	 * Truncated-length probes: a valid header but a len that stops short of the
	 * values array must be refused, and the validator must not read past the len
	 * it was given.  Both use the pristine base image.
	 */
	{
		const WeaveDocvalsHeader *bh = (const WeaveDocvalsHeader *) base;
		uint32		voff = bh->values_off;
		const char *r;

		/* values_off - 1: one byte short of a complete header region */
		checks++;
		reject_checks++;
		r = weave_docvals_validate(base, (size_t) voff - 1);
		if (r == NULL)
		{
			failures++;
			if (failures <= 20)
				printf("FAIL REJECT %s:%d: truncated len=%u wrongly accepted (n=%d)\n",
					   __FILE__, __LINE__, (unsigned) (voff - 1), n);
		}

		/* values_off + n*8 - 1: one byte short of the full values array. Only
		 * meaningful when n > 0 (otherwise there is no values array). */
		if (n > 0)
		{
			size_t		tlen = (size_t) voff + (size_t) n * 8u - 1;

			checks++;
			reject_checks++;
			r = weave_docvals_validate(base, tlen);
			if (r == NULL)
			{
				failures++;
				if (failures <= 20)
					printf("FAIL REJECT %s:%d: truncated len=%zu wrongly accepted (n=%d)\n",
						   __FILE__, __LINE__, tlen, n);
			}
		}
	}
}

int
main(void)
{
	long		i;

	printf("== docvals int8 store (C5): eval == a straight-line reference loop; "
		   "validator rejects corruption ==\n");

	for (i = 0; i < TRIALS; i++)
	{
		prop_pred();
		prop_trunc();
		prop_reject();
	}

	printf("trials: %d\n", TRIALS);
	printf("PRED  eval == reference loop : %8ld checks\n", pred_checks);
	printf("REJECT validator refuses junk: %8ld checks\n", reject_checks);
	printf("TRUNC outcap truncation guard: %8ld checks\n", trunc_checks);
	printf("coverage: nonempty=%ld empty=%ld present-EQ-hit=%ld "
		   "ops LT=%ld LE=%ld EQ=%ld GE=%ld GT=%ld\n",
		   cov_nonempty, cov_empty, cov_present_eq,
		   cov_op[WEAVE_DV_LT], cov_op[WEAVE_DV_LE], cov_op[WEAVE_DV_EQ],
		   cov_op[WEAVE_DV_GE], cov_op[WEAVE_DV_GT]);
	printf("coverage: smallrange=%ld eq-multi-docid=%ld dup-adjacent=%ld "
		   "outcap-truncated=%ld\n",
		   cov_smallrange, cov_eq_multi, cov_dup_adjacent, cov_trunc);

	/*
	 * Assert the coverage rather than hoping for it (AGENTS.md hard rule 11).  A
	 * property untested is worse than one that passes: it is a green with no
	 * evidence behind it.
	 */
	if (cov_nonempty == 0 || cov_empty == 0 || cov_present_eq == 0 ||
		cov_op[WEAVE_DV_LT] == 0 || cov_op[WEAVE_DV_LE] == 0 ||
		cov_op[WEAVE_DV_EQ] == 0 || cov_op[WEAVE_DV_GE] == 0 ||
		cov_op[WEAVE_DV_GT] == 0)
	{
		printf("COVERAGE FAIL: nonempty=%ld empty=%ld present-EQ-hit=%ld "
			   "LT=%ld LE=%ld EQ=%ld GE=%ld GT=%ld -- a property with zero "
			   "cases is untested, not established\n",
			   cov_nonempty, cov_empty, cov_present_eq,
			   cov_op[WEAVE_DV_LT], cov_op[WEAVE_DV_LE], cov_op[WEAVE_DV_EQ],
			   cov_op[WEAVE_DV_GE], cov_op[WEAVE_DV_GT]);
		failures++;
	}

	/*
	 * FIX 4/FIX 5 coverage: the duplicate-value cases and the outcap-truncation
	 * branch must have actually fired, or the properties they defend (multi-docid
	 * EQ, ascending order with duplicates, the truncation guard) are untested.
	 */
	if (cov_smallrange == 0 || cov_eq_multi == 0 || cov_dup_adjacent == 0 ||
		cov_trunc == 0)
	{
		printf("COVERAGE FAIL: smallrange=%ld eq-multi-docid=%ld "
			   "dup-adjacent=%ld outcap-truncated=%ld -- a duplicate/EQ-multi "
			   "or outcap-truncation branch never fired, so it is untested\n",
			   cov_smallrange, cov_eq_multi, cov_dup_adjacent, cov_trunc);
		failures++;
	}

	if (failures > 0)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
