/*-------------------------------------------------------------------------
 *
 * test_doclen_block.c
 *		Dependency-free oracle test for the doclen-sidecar block encoding
 *		AND the gated resident-block lookup that reads it.
 *
 * L17 changed the doclen sidecar's docid column from GAPS (each value is the
 * delta from its predecessor, so entry i's docid is a prefix sum) to ABSOLUTE
 * OFFSETS from the block's first_docid.  The point is random access: FOR packing
 * is fixed-width, so weave_for_get() addresses any entry in O(1), and an in-block
 * lookup becomes a ~7-step binary search instead of unpacking all 128 entries and
 * prefix-summing them.  That decode was ~72% of a ranked scan
 * (bench/RESULTS_SCAN_PROFILE.md).
 *
 * The L17 FOLLOW-UP (bench/RESULTS_L17.md's follow-ups) added a WINDOW GATE in
 * front of the ascending-resume-hint walk: one O(1) read at the hint decides
 * whether a short linear walk could plausibly reach the target before
 * committing to it, instead of always walking WEAVE_DOCLEN_WALK_WINDOW steps
 * and falling back to a bisect anyway.  That gate shipped with ZERO test
 * coverage of its own: this file's previous version hand-transcribed the
 * PRE-gate walk into a local v5_lookup() that never called the real gated
 * code, so it kept passing no matter what the real gate did.  That is
 * precisely the failure class doc/TESTING.md's "why the property layer is
 * mandatory" section describes -- a wrong gate yields a plausible wrong
 * doclen, not an error, and no fixed-expected-output test can see it.
 *
 * Fix: weave_doclen_walk_abs() / weave_doclen_walk_arr() (include/weave/for.h)
 * are the gated walk, extracted VERBATIM out of weave_doclen_cursor_lookup()'s
 * two branches so this test links and calls the actual backend code rather
 * than a copy of it -- the same reason weave_for_get()/weave_byte_to_doclen()
 * already lived there.  See that header's comment on the two functions for
 * the full rationale.
 *
 * Why this test exists, specifically:
 *
 * The reader now has TWO paths that must agree on what a block means -- v4's
 * gap-coded path (still needed: an index built by <= 0.5.0 keeps its v4 sidecar
 * pages after upgrade, and later merges write v5 ones, so both encodings coexist
 * in one relation) and v5's binary search over packed offsets.  A disagreement
 * between them does not produce an error.  It produces a WRONG DOCUMENT LENGTH,
 * which feeds BM25's length normalisation and yields a plausible-but-wrong
 * ranking -- the exact failure class AGENTS.md hard rule 1 exists for, and one
 * that no fixed-expected-output regression test can catch.  The gate is the
 * same shape of risk: too narrow a window (or an inverted comparison) does not
 * crash, it just occasionally returns the bisect's answer instead of the
 * walk's -- which is FINE, because the property below is "gated walk == plain
 * bisect", not "gated walk uses the walk path".  A gate that is simply
 * disabled always passes this property.  What the property DOES catch is a
 * gate, or a walk loop, that returns the WRONG byte or misses a present docid
 * -- which is what an off-by-one or an inverted overshoot check produces.  The
 * mutation testing done for this change (see the commit message) additionally
 * confirms the gate is doing something observable, which the property test
 * alone cannot: a property that only asserts "these two answers agree" cannot
 * distinguish "the gate is correct" from "the gate never fires", so this file
 * is deliberately paired with a manual mutation pass rather than claiming that
 * pairing back inside the test itself.
 *
 * So: for random docid sets, assert that
 *   (a) the gated walk finds every docid that is present, and returns its
 *       correct byte, for BOTH the v5 (packed, weave_for_get) and v4
 *       (decoded array) representations;
 *   (b) it reports absent for every docid that is not present, including ones
 *       inside the block's span (the sidecar is sparse -- a doc with quantized
 *       length 0 is deliberately not stored);
 *   (c) the absolute encoding and the gap encoding decode to the IDENTICAL docid
 *       sequence, which is the compatibility claim the dual reader rests on;
 *   (d) THE CENTRAL PROPERTY: for every (docid, starting-hint) pair, the gated
 *       walk-then-bisect returns EXACTLY what an ungated, independently-written
 *       full bisect returns -- for both encodings, and for every hint value
 *       (in range, out of range, cold/zero, exactly at the target, past it,
 *       nowhere near it);
 *   (e) directed scenarios the review called out by name, on top of (d)'s
 *       random coverage: target before/at/far-after the hint, a FRESH hint
 *       (index 0, value 0) whose target is far away and must never overshoot,
 *       a block boundary, a single-entry block, and the last entry of a
 *       full-size (128-entry) block.
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tdb test/hegel/test_doclen_block.c && /tmp/tdb
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_doclen_block.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* for.h is pure standalone C -- the same copy the backend compiles.  It
 * supplies weave_for_pack/unpack/get, weave_doclen_to_byte/byte_to_doclen,
 * and (as of this file's rev) weave_doclen_walk_abs/weave_doclen_walk_arr:
 * the ACTUAL gated resident-block lookup, not a transcription of it. */
#include "weave/for.h"

#define BLOCK_SIZE 128

static int		failures = 0;
static long		checks = 0;

#define CHECK(cond, fmt, ...)											\
	do {																\
		checks++;														\
		if (!(cond))													\
		{																\
			if (failures < 20)											\
				printf("FAIL %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
			failures++;													\
		}																\
	} while (0)

/* xorshift64*, so runs are reproducible without depending on libc rand() */
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

/*
 * The oracle: a PLAIN, UNGATED bisect over each representation, written
 * independently of weave_doclen_walk_abs/_arr (no shared helper, no shared
 * loop shape) so it cannot fail the same way the code under test fails.  This
 * is the "ungated full bisect" the property compares against.
 */
static int
bisect_abs(const unsigned char *raw, const uint8_t *rawbyte, uint64_t base,
		   int n, uint64_t docid)
{
	int			lo = 0,
				hi = n - 1;
	uint64_t	want;

	if (docid < base)
		return -1;
	want = docid - base;
	while (lo <= hi)
	{
		int			mid = (lo + hi) >> 1;
		uint64_t	v = weave_for_get(raw, mid);

		if (v < want)
			lo = mid + 1;
		else if (v > want)
			hi = mid - 1;
		else
			return rawbyte[mid];
	}
	return -1;
}

static int
bisect_arr(const uint64_t *docidarr, const uint8_t *bytearr, int n, uint64_t docid)
{
	int			lo = 0,
				hi = n - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) >> 1;

		if (docidarr[mid] < docid)
			lo = mid + 1;
		else if (docidarr[mid] > docid)
			hi = mid - 1;
		else
			return bytearr[mid];
	}
	return -1;
}

/*
 * Property (d): assert the gated walk agrees with the plain bisect for one
 * (docid, starting hint) pair, for BOTH representations.  `hint` is passed by
 * value and each call gets its own local copy, so callers can reuse the same
 * starting hint across many probes.
 */
static void
check_agrees_abs(const unsigned char *raw, const uint8_t *rawbyte, uint64_t base,
				  int n, uint64_t docid, int hint, const char *why)
{
	int			h = hint;
	int			got = weave_doclen_walk_abs(raw, rawbyte, base, n, docid, &h);
	int			want = bisect_abs(raw, rawbyte, base, n, docid);

	CHECK(got == want,
		  "%s: abs docid=%llu hint=%d got=%d want=%d",
		  why, (unsigned long long) docid, hint, got, want);
}

static void
check_agrees_arr(const uint64_t *docidarr, const uint8_t *bytearr, int n,
				  uint64_t docid, int hint, const char *why)
{
	int			h = hint;
	int			got = weave_doclen_walk_arr(docidarr, bytearr, n, docid, &h);
	int			want = bisect_arr(docidarr, bytearr, n, docid);

	CHECK(got == want,
		  "%s: arr docid=%llu hint=%d got=%d want=%d",
		  why, (unsigned long long) docid, hint, got, want);
}

/*
 * One random block: pick `n` strictly ascending docids with a given mean stride,
 * encode them BOTH ways, and check every property.
 */
static void
one_block(int n, uint64_t stride_mean)
{
	uint64_t	docid[BLOCK_SIZE];
	uint8_t		bytes[BLOCK_SIZE];
	uint64_t	offs[BLOCK_SIZE];
	uint64_t	gaps[BLOCK_SIZE];
	uint64_t	decoded[BLOCK_SIZE];
	uint64_t	docidarr[BLOCK_SIZE];	/* v4, as load_page's decode leaves it */
	unsigned char absbuf[1 + (BLOCK_SIZE * 64 + 7) / 8];
	unsigned char gapbuf[1 + (BLOCK_SIZE * 64 + 7) / 8];
	uint64_t	base;
	uint64_t	acc;
	int			i;
	int			hint;

	/* a docid is heapblock * MaxHeapTuplesPerPage + offset; the absolute values
	 * are large, which is exactly why the offsets-from-base trick keeps the coded
	 * width small */
	base = (rng() % 100000000ULL) + 1;
	docid[0] = base;
	bytes[0] = (uint8_t) (rng() % 255) + 1;		/* 0 means "absent", never stored */
	for (i = 1; i < n; i++)
	{
		uint64_t	step = 1 + (stride_mean ? rng() % (2 * stride_mean) : 0);

		docid[i] = docid[i - 1] + step;
		bytes[i] = (uint8_t) (rng() % 255) + 1;
	}

	for (i = 0; i < n; i++)
	{
		offs[i] = docid[i] - base;					/* v5 */
		gaps[i] = i == 0 ? 0 : docid[i] - docid[i - 1];	/* v4 */
	}
	weave_for_pack(offs, n, absbuf);
	weave_for_pack(gaps, n, gapbuf);

	/* (c) the two encodings must describe the identical docid sequence, and
	 * materialize the v4 array form exactly like weave_doclen_resident_load_block()
	 * does (prefix sum over the unpacked gaps) -- this array is what
	 * weave_doclen_walk_arr() searches. */
	(void) weave_for_unpack(gapbuf, n, decoded);
	acc = base;
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			acc += decoded[i];
		docidarr[i] = acc;
		CHECK(acc == docid[i],
			  "gap decode mismatch at %d: got %llu want %llu",
			  i, (unsigned long long) acc, (unsigned long long) docid[i]);
		CHECK(base + weave_for_get(absbuf, i) == docid[i],
			  "abs decode mismatch at %d: got %llu want %llu",
			  i, (unsigned long long) (base + weave_for_get(absbuf, i)),
			  (unsigned long long) docid[i]);
	}

	/* (a) every present docid is found, with the right byte, probing ASCENDING
	 * as the scan does so the resume hint is exercised the way it really runs --
	 * for BOTH representations, through the ACTUAL gated-walk functions. */
	hint = 0;
	{
		int			hint_arr = 0;

		for (i = 0; i < n; i++)
		{
			int			got = weave_doclen_walk_abs(absbuf, bytes, base, n,
													   docid[i], &hint);
			int			gota = weave_doclen_walk_arr(docidarr, bytes, n,
														docid[i], &hint_arr);

			CHECK(got == (int) bytes[i],
				  "present docid %llu (idx %d) abs: got %d want %d",
				  (unsigned long long) docid[i], i, got, (int) bytes[i]);
			CHECK(gota == (int) bytes[i],
				  "present docid %llu (idx %d) arr: got %d want %d",
				  (unsigned long long) docid[i], i, gota, (int) bytes[i]);
		}
	}

	/* (a') and again with a cold hint each time -- the bisect path alone */
	for (i = 0; i < n; i++)
	{
		int			h = n;			/* out of range: resets to 0 inside the walk */
		int			got;

		got = weave_doclen_walk_abs(absbuf, bytes, base, n, docid[i], &h);
		CHECK(got == (int) bytes[i],
			  "present docid %llu via cold-hint abs: got %d want %d",
			  (unsigned long long) docid[i], got, (int) bytes[i]);

		h = n;
		got = weave_doclen_walk_arr(docidarr, bytes, n, docid[i], &h);
		CHECK(got == (int) bytes[i],
			  "present docid %llu via cold-hint arr: got %d want %d",
			  (unsigned long long) docid[i], got, (int) bytes[i]);
	}

	/* (b) absent docids must report absent, including interior holes and the
	 * boundaries just outside the block */
	for (i = 0; i < n - 1; i++)
	{
		uint64_t	hole = docid[i] + 1;
		int			h = 0;

		if (hole >= docid[i + 1])
			continue;			/* consecutive: no hole between these two */
		CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, hole, &h) == -1,
			  "interior hole %llu reported present (abs)",
			  (unsigned long long) hole);
		h = 0;
		CHECK(weave_doclen_walk_arr(docidarr, bytes, n, hole, &h) == -1,
			  "interior hole %llu reported present (arr)",
			  (unsigned long long) hole);
	}
	{
		int			h = 0;

		if (base > 0)
		{
			CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, base - 1, &h) == -1,
				  "docid below base reported present (abs)");
			h = 0;
			CHECK(weave_doclen_walk_arr(docidarr, bytes, n, base - 1, &h) == -1,
				  "docid below base reported present (arr)");
		}
		h = 0;
		CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[n - 1] + 1, &h) == -1,
			  "docid above last reported present (abs)");
		h = 0;
		CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[n - 1] + 1, &h) == -1,
			  "docid above last reported present (arr)");
	}

	/*
	 * (d) THE CENTRAL PROPERTY: gated walk-then-bisect must agree with an
	 * independently-written plain bisect for the (docid, hint) pairs a real
	 * scan (and an adversarial one) can construct, not just the ascending
	 * ones -- a multi-term scan can seek backwards, and a corrupted or merely
	 * differently-shaped hint must never change the ANSWER, only which code
	 * path finds it.  Sweep probe docids across and beyond the block's span;
	 * for each, try both out-of-range hints (-1, n -- must reset safely) and
	 * a handful of random in-range hints, which is enough to hit "before",
	 * "at", and "far after" repeatedly across ~4500 blocks without the O(n^2)
	 * cost of trying literally every hint on every probe. */
	for (i = 0; i < 4 * n; i++)
	{
		uint64_t	probe = base + (rng() % ((docid[n - 1] - base) + 3));
		int			hw;
		int			ntries = n < 6 ? n + 1 : 6;

		check_agrees_abs(absbuf, bytes, base, n, probe, -1, "random-probe-oob-lo");
		check_agrees_arr(docidarr, bytes, n, probe, -1, "random-probe-oob-lo");
		check_agrees_abs(absbuf, bytes, base, n, probe, n, "random-probe-oob-hi");
		check_agrees_arr(docidarr, bytes, n, probe, n, "random-probe-oob-hi");
		for (hw = 0; hw < ntries; hw++)
		{
			int			h = (int) (rng() % (uint64_t) n);

			check_agrees_abs(absbuf, bytes, base, n, probe, h, "random-probe");
			check_agrees_arr(docidarr, bytes, n, probe, h, "random-probe");
		}
	}
}

/*
 * Directed scenarios named explicitly by the review, built by hand so each is
 * deterministic instead of relying on random coverage happening to land on
 * it.  Ten entries: three tight (stride 1-3, inside the walk window) so a hit
 * a few slots after the hint is reachable BY the walk, then a jump of 3900 (>>
 * WEAVE_DOCLEN_WALK_WINDOW) so "far after" both in index and in VALUE is
 * covered -- the gate keys on the value gap, not the index gap.
 */
static void
edge_case_scenarios(void)
{
	uint64_t	docid[10] = {1000, 1001, 1002, 1003, 1050, 1051, 1100, 5000, 5001, 5002};
	uint8_t		bytes[10] = {11, 22, 33, 44, 55, 66, 77, 88, 99, 111};
	int			n = 10;
	uint64_t	base = docid[0];
	uint64_t	offs[10],
				gaps[10],
				decoded[10],
				docidarr[10];
	unsigned char absbuf[64],
				gapbuf[64];
	uint64_t	acc;
	int			i;
	int			hint;

	for (i = 0; i < n; i++)
	{
		offs[i] = docid[i] - base;
		gaps[i] = i == 0 ? 0 : docid[i] - docid[i - 1];
	}
	weave_for_pack(offs, n, absbuf);
	weave_for_pack(gaps, n, gapbuf);
	(void) weave_for_unpack(gapbuf, n, decoded);
	acc = base;
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			acc += decoded[i];
		docidarr[i] = acc;
	}

	/* 1. target exactly AT the hint: hint points straight at docid[4]. */
	hint = 4;
	CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[4], &hint) == bytes[4],
		  "at-hint abs");
	hint = 4;
	CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[4], &hint) == bytes[4],
		  "at-hint arr");

	/* 2. target BEFORE the hint: hint points past it, so the walk's first
	 * read overshoots immediately (v > want) and falls straight to bisect. */
	hint = 6;
	CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[1], &hint) == bytes[1],
		  "before-hint abs");
	hint = 6;
	CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[1], &hint) == bytes[1],
		  "before-hint arr");

	/* 3. target FAR after the hint: docid[6]=1100 -> docid[7]=5000, a 3900
	 * gap -- the gate must refuse the walk and go straight to bisect rather
	 * than walking WEAVE_DOCLEN_WALK_WINDOW steps into empty space. */
	hint = 6;
	CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[7], &hint) == bytes[7],
		  "far-after-hint abs");
	hint = 6;
	CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[7], &hint) == bytes[7],
		  "far-after-hint arr");

	/*
	 * 4. FRESH hint (index 0, VALUE 0) whose target is far away and must
	 * NEVER OVERSHOOT.  This is the dominant real-world case -- a block
	 * change resets the resume hint to 0 -- and the one bench/RESULTS_L17.md's
	 * naive fix (widening the window) would have missed: a wider window still
	 * walks past a target this far away before falling back to bisect, so the
	 * ANSWER stays right but the win the gate exists for (bench/RESULTS_L17.md
	 * follow-up 1) disappears. The value at index 0 is 0 for abs (offset from
	 * its own base) and docid[0] for arr; either way `want/docid - v` is huge,
	 * so the gate must refuse the walk on its very first read.
	 */
	hint = 0;
	CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[n - 1], &hint) == bytes[n - 1],
		  "fresh-hint-far abs");
	hint = 0;
	CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[n - 1], &hint) == bytes[n - 1],
		  "fresh-hint-far arr");

	/* 5. block boundary: the first and last entries of the block. */
	hint = 0;
	CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[0], &hint) == bytes[0],
		  "first-entry abs");
	hint = 0;
	CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[0], &hint) == bytes[0],
		  "first-entry arr");
	hint = 0;
	CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[n - 1], &hint) == bytes[n - 1],
		  "last-entry abs");
	hint = 0;
	CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[n - 1], &hint) == bytes[n - 1],
		  "last-entry arr");

	/* 6. single-entry block: n == 1 is the degenerate case the window-gate
	 * arithmetic (`lim = i + WINDOW; if (lim > n) lim = n;`) has to clamp
	 * correctly, and the ONLY entry is simultaneously the block's first,
	 * last, and only entry. */
	{
		uint64_t	d1 = 77777;
		uint8_t		b1[1] = {42};
		uint64_t	o1[1] = {0};
		unsigned char ab1[8];
		uint64_t	da1[1];

		o1[0] = 0;
		weave_for_pack(o1, 1, ab1);
		da1[0] = d1;

		hint = 0;
		CHECK(weave_doclen_walk_abs(ab1, b1, d1, 1, d1, &hint) == b1[0],
			  "single-entry abs present, cold hint");
		hint = 5;				/* out of range: must reset safely, not crash */
		CHECK(weave_doclen_walk_abs(ab1, b1, d1, 1, d1, &hint) == b1[0],
			  "single-entry abs present, oob hint");
		hint = 0;
		CHECK(weave_doclen_walk_abs(ab1, b1, d1, 1, d1 + 1, &hint) == -1,
			  "single-entry abs absent above");
		hint = 0;
		CHECK(weave_doclen_walk_abs(ab1, b1, d1, 1, d1 - 1, &hint) == -1,
			  "single-entry abs absent below base");

		hint = 0;
		CHECK(weave_doclen_walk_arr(da1, b1, 1, d1, &hint) == b1[0],
			  "single-entry arr present, cold hint");
		hint = -3;				/* negative: must also reset safely */
		CHECK(weave_doclen_walk_arr(da1, b1, 1, d1, &hint) == b1[0],
			  "single-entry arr present, negative hint");
		hint = 0;
		CHECK(weave_doclen_walk_arr(da1, b1, 1, d1 + 1, &hint) == -1,
			  "single-entry arr absent above");
	}
}

/*
 * The last entry of a FULL-SIZE (128-entry) block -- 128 is the writer's own
 * WEAVE_BLOCK_SIZE, so this is the largest block shape that exists on disk,
 * and its last index (127) is where an off-by-one in the window-clamp
 * (`lim = i + WINDOW; if (lim > n) lim = n;`) or in the walk's own loop bound
 * would show up first.
 */
static void
full_block_last_entry(void)
{
	uint64_t	docid[BLOCK_SIZE];
	uint8_t		bytes[BLOCK_SIZE];
	uint64_t	offs[BLOCK_SIZE];
	uint64_t	gaps[BLOCK_SIZE];
	uint64_t	decoded[BLOCK_SIZE];
	uint64_t	docidarr[BLOCK_SIZE];
	unsigned char absbuf[1 + (BLOCK_SIZE * 64 + 7) / 8];
	unsigned char gapbuf[1 + (BLOCK_SIZE * 64 + 7) / 8];
	uint64_t	base = 42;
	uint64_t	acc;
	int			i;
	int			hint;
	int			n = BLOCK_SIZE;
	static const uint64_t strides[] = {1, 3, 50, 4096};
	int			s;

	for (s = 0; s < 4; s++)
	{
		uint64_t	stride = strides[s];

		docid[0] = base;
		bytes[0] = 1;
		for (i = 1; i < n; i++)
		{
			docid[i] = docid[i - 1] + 1 + (rng() % (2 * stride + 1));
			bytes[i] = (uint8_t) (1 + (i % 254));
		}
		for (i = 0; i < n; i++)
		{
			offs[i] = docid[i] - base;
			gaps[i] = i == 0 ? 0 : docid[i] - docid[i - 1];
		}
		weave_for_pack(offs, n, absbuf);
		weave_for_pack(gaps, n, gapbuf);
		(void) weave_for_unpack(gapbuf, n, decoded);
		acc = base;
		for (i = 0; i < n; i++)
		{
			if (i > 0)
				acc += decoded[i];
			docidarr[i] = acc;
		}

		/* last entry, cold hint (the block-change case) */
		hint = 0;
		CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[n - 1], &hint)
			  == bytes[n - 1],
			  "full-block last-entry abs, cold hint, stride %llu",
			  (unsigned long long) stride);
		hint = 0;
		CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[n - 1], &hint)
			  == bytes[n - 1],
			  "full-block last-entry arr, cold hint, stride %llu",
			  (unsigned long long) stride);

		/* last entry, hint already one before it (the walk's best case) */
		hint = n - 2;
		CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[n - 1], &hint)
			  == bytes[n - 1],
			  "full-block last-entry abs, warm hint, stride %llu",
			  (unsigned long long) stride);
		hint = n - 2;
		CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[n - 1], &hint)
			  == bytes[n - 1],
			  "full-block last-entry arr, warm hint, stride %llu",
			  (unsigned long long) stride);

		/* last entry, hint sitting exactly on the last valid index (n - 1) */
		hint = n - 1;
		CHECK(weave_doclen_walk_abs(absbuf, bytes, base, n, docid[n - 1], &hint)
			  == bytes[n - 1],
			  "full-block last-entry abs, hint==n-1, stride %llu",
			  (unsigned long long) stride);
		hint = n - 1;
		CHECK(weave_doclen_walk_arr(docidarr, bytes, n, docid[n - 1], &hint)
			  == bytes[n - 1],
			  "full-block last-entry arr, hint==n-1, stride %llu",
			  (unsigned long long) stride);
	}
}

int
main(void)
{
	int			trial;

	printf("doclen-sidecar block + gated resident lookup: v5 abs vs v4 gap\n");

	/*
	 * Stride matters and is the whole reason for both the v5 change and the
	 * walk gate: a rare term's candidates are far apart (large stride -> wide
	 * codes, and a gate that must refuse the walk), a common term's are
	 * adjacent (stride 1 -> narrow codes, and a gate that must ADMIT the
	 * walk).  Sweep both extremes plus the degenerate single-entry block.
	 */
	for (trial = 0; trial < 4000; trial++)
	{
		static const uint64_t strides[] = {0, 1, 2, 7, 50, 512, 65536, 1000000};
		int			n = 1 + (int) (rng() % BLOCK_SIZE);
		uint64_t	stride = strides[rng() % (sizeof(strides) / sizeof(strides[0]))];

		one_block(n, stride);
	}

	/* full blocks at every stride, since 128 is the writer's own block size */
	for (trial = 0; trial < 500; trial++)
	{
		static const uint64_t strides[] = {1, 3, 50, 4096};

		one_block(BLOCK_SIZE, strides[trial % 4]);
	}

	edge_case_scenarios();
	full_block_last_entry();

	/* the check count is the LAST line: `make check-standalone` reports each
	 * standalone test with `tail -1`, same as test_for_get.c and test_quantize.c.
	 * A nonzero exit is what actually fails the target (the recipe is set -e). */
	if (failures)
	{
		printf("FAILED -- %ld checks, %d failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %d failures\n", checks, failures);
	return 0;
}
