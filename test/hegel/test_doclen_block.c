/*-------------------------------------------------------------------------
 *
 * test_doclen_block.c
 *		Dependency-free oracle test for the v5 doclen-sidecar block encoding.
 *
 * L17 changed the doclen sidecar's docid column from GAPS (each value is the
 * delta from its predecessor, so entry i's docid is a prefix sum) to ABSOLUTE
 * OFFSETS from the block's first_docid.  The point is random access: FOR packing
 * is fixed-width, so weave_for_get() addresses any entry in O(1), and an in-block
 * lookup becomes a ~7-step binary search instead of unpacking all 128 entries and
 * prefix-summing them.  That decode was ~72% of a ranked scan
 * (bench/RESULTS_SCAN_PROFILE.md).
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
 * that no fixed-expected-output regression test can catch.
 *
 * So: for random docid sets, assert that
 *   (a) binary search over absolute offsets finds every docid that is present,
 *       and returns its correct byte;
 *   (b) it reports absent for every docid that is not present, including ones
 *       inside the block's span (the sidecar is sparse -- a doc with quantized
 *       length 0 is deliberately not stored);
 *   (c) the absolute encoding and the gap encoding decode to the IDENTICAL docid
 *       sequence, which is the compatibility claim the dual reader rests on;
 *   (d) the walk-then-bisect shape the scan actually uses (an ascending resume
 *       hint, then a bisect on miss) returns the same answers as a plain bisect.
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

/* for.h is pure standalone C -- the same copy the backend compiles */
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
 * The v5 in-block lookup, transcribed from weave_doclen_cursor_lookup()'s
 * absolute branch: an ascending walk from a resume hint, then a bisect.  Returns
 * the byte, or -1 for absent.  `hint` is updated exactly as the cursor does.
 */
static int
v5_lookup(const unsigned char *raw, const uint8_t *bytes, int n,
		  uint64_t base, uint64_t docid, int *hint)
{
	int			rlo = 0,
				rhi = n - 1;
	int			i = *hint;
	int			lim;
	uint64_t	want;

	if (docid < base)
		return -1;
	want = docid - base;

	if (i < 0 || i >= n)
		i = 0;
	lim = i + 8;
	if (lim > n)
		lim = n;
	for (; i < lim; i++)
	{
		uint64_t	v = weave_for_get(raw, i);

		if (v == want)
		{
			*hint = i + 1;
			return bytes[i];
		}
		if (v > want)
			break;
	}
	while (rlo <= rhi)
	{
		int			mid = (rlo + rhi) >> 1;
		uint64_t	v = weave_for_get(raw, mid);

		if (v < want)
			rlo = mid + 1;
		else if (v > want)
			rhi = mid - 1;
		else
		{
			*hint = mid + 1;
			return bytes[mid];
		}
	}
	return -1;
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
	unsigned char absbuf[1 + (BLOCK_SIZE * 64 + 7) / 8];
	unsigned char gapbuf[1 + (BLOCK_SIZE * 64 + 7) / 8];
	uint64_t	base;
	uint64_t	acc;
	int			i;
	int			hint = 0;

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

	/* (c) the two encodings must describe the identical docid sequence */
	(void) weave_for_unpack(gapbuf, n, decoded);
	acc = base;
	for (i = 0; i < n; i++)
	{
		if (i > 0)
			acc += decoded[i];
		CHECK(acc == docid[i],
			  "gap decode mismatch at %d: got %llu want %llu",
			  i, (unsigned long long) acc, (unsigned long long) docid[i]);
		CHECK(base + weave_for_get(absbuf, i) == docid[i],
			  "abs decode mismatch at %d: got %llu want %llu",
			  i, (unsigned long long) (base + weave_for_get(absbuf, i)),
			  (unsigned long long) docid[i]);
	}

	/* (a) every present docid is found, with the right byte, probing ASCENDING
	 * as the scan does so the resume hint is exercised the way it really runs */
	hint = 0;
	for (i = 0; i < n; i++)
	{
		int			got = v5_lookup(absbuf, bytes, n, base, docid[i], &hint);

		CHECK(got == (int) bytes[i],
			  "present docid %llu (idx %d): got %d want %d",
			  (unsigned long long) docid[i], i, got, (int) bytes[i]);
	}

	/* (a') and again with a cold hint each time -- the bisect path alone */
	for (i = 0; i < n; i++)
	{
		int			h = 0;
		int			got;

		h = n;					/* force the walk to be skipped, then bisect */
		got = v5_lookup(absbuf, bytes, n, base, docid[i], &h);
		CHECK(got == (int) bytes[i],
			  "present docid %llu via bisect: got %d want %d",
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
		CHECK(v5_lookup(absbuf, bytes, n, base, hole, &h) == -1,
			  "interior hole %llu reported present",
			  (unsigned long long) hole);
	}
	{
		int			h = 0;

		if (base > 0)
			CHECK(v5_lookup(absbuf, bytes, n, base, base - 1, &h) == -1,
				  "docid below base reported present");
		h = 0;
		CHECK(v5_lookup(absbuf, bytes, n, base, docid[n - 1] + 1, &h) == -1,
			  "docid above last reported present");
	}

	/* (d) walk-then-bisect must agree with plain bisect on ARBITRARY order --
	 * a multi-term scan can revisit and seek backwards */
	for (i = 0; i < 4 * n; i++)
	{
		uint64_t	probe = base + (rng() % ((docid[n - 1] - base) + 3));
		int			hw = (int) (rng() % (uint64_t) (n + 1));
		int			hb = n;
		int			gw = v5_lookup(absbuf, bytes, n, base, probe, &hw);
		int			gb = v5_lookup(absbuf, bytes, n, base, probe, &hb);

		CHECK(gw == gb, "hint-dependent answer for %llu: walk %d bisect %d",
			  (unsigned long long) probe, gw, gb);
	}
}

int
main(void)
{
	int			trial;

	printf("v5 doclen-sidecar block: absolute offsets vs gaps\n");

	/*
	 * Stride matters and is the whole reason for the change: a rare term's
	 * candidates are far apart (large stride -> wide codes), a common term's are
	 * adjacent (stride 1 -> narrow codes).  Sweep both extremes plus the
	 * degenerate single-entry block.
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
