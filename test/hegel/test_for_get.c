/*-------------------------------------------------------------------------
 *
 * test_for_get.c
 *		Dependency-free oracle test for weave_for_get().
 *
 * weave_for_get() extracts one value from a FOR-bit-packed buffer.  It used to
 * loop bit by bit; it now does a word load, a shift and a mask (ported from
 * pg_fts 6448a70, which measured a 1.56x common-term ranked speedup from the
 * change).  It sits on the per-posting hot path twice over -- the WAND
 * contribution reads `tf` through it for every scored posting, and the v3
 * inline-doclen path reads |D| through it as well -- so it is exactly the kind of
 * code where a fast rewrite that is subtly wrong produces plausible rankings
 * rather than an error.
 *
 * The existing suite for this codec (test_for.c, test_for_props.c) needs cmocka
 * AND the author's `hegel` property-testing library, neither of which is
 * available here.  Worse, that suite had been silently ABSENT from this
 * repository since the fork -- pg_fts's .gitattributes marks `test` as
 * export-ignore for the PGXN release zip, and the fork used `git archive` -- so
 * the codec was modified twice with no property test present at all.
 *
 * This file is the answer to both problems: a self-contained oracle that needs
 * nothing but a C compiler, comparing the fast extractor against a bit-by-bit
 * reference and against the bulk unpacker over exhaustive widths and adversarial
 * values.  Same design intent as weave/for.h itself, and as
 * test/hegel/test_quantize.c.
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tfg test/hegel/test_for_get.c && /tmp/tfg
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_for_get.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/for.h"

static int	failures = 0;
static long checks = 0;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			if (failures <= 20) \
			{ \
				printf("FAIL %s:%d: ", __FILE__, __LINE__); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

/*
 * The reference: the bit-by-bit extractor weave_for_get() replaced.  Kept
 * verbatim in shape so the comparison is against the ORIGINAL semantics rather
 * than against a re-derivation of them.
 */
static uint64
for_get_reference(const unsigned char *buf, int i)
{
	int			width = buf[0];
	int			bitpos;
	uint64		v = 0;
	int			b;

	if (width == 0)
		return 0;
	bitpos = i * width;
	for (b = 0; b < width; b++)
	{
		int			abs = bitpos + b;

		if (buf[1 + (abs >> 3)] & (1 << (abs & 7)))
			v |= (uint64) 1 << b;
	}
	return v;
}

/* xoshiro256**, so the corpus is reproducible and independent of libc. */
static uint64 s[4] = {0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL,
	0xa4093822299f31d0ULL, 0x082efa98ec4e6c89ULL};

static uint64
rotl64(uint64 x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64
rnd(void)
{
	uint64		r = rotl64(s[1] * 5, 7) * 9;
	uint64		t = s[1] << 17;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = rotl64(s[3], 45);
	return r;
}

#define MAXN 512

static void
run_case(const uint64 *vals, int n, const char *what)
{
	unsigned char buf[1 + (MAXN * 64) / 8 + 16];
	uint64		out[MAXN];
	int			nbytes;
	int			i;
	int			width;

	memset(buf, 0xA5, sizeof(buf));		/* poison, so a short write shows up */
	nbytes = weave_for_pack(vals, n, buf);
	width = buf[0];

	CHECK(nbytes > 0, "%s: pack returned %d", what, nbytes);
	CHECK(nbytes == weave_for_bytelen(buf, n),
		  "%s: pack len %d != bytelen %d", what, nbytes,
		  weave_for_bytelen(buf, n));

	/* (1) the fast extractor must agree with the bit-by-bit reference, exactly */
	for (i = 0; i < n; i++)
	{
		uint64		a = weave_for_get(buf, i);
		uint64		b = for_get_reference(buf, i);

		CHECK(a == b, "%s: width=%d i=%d fast=%llu ref=%llu",
			  what, width, i, (unsigned long long) a, (unsigned long long) b);
	}

	/* (2) and with the original values */
	for (i = 0; i < n; i++)
		CHECK(weave_for_get(buf, i) == vals[i],
			  "%s: width=%d i=%d got=%llu want=%llu", what, width, i,
			  (unsigned long long) weave_for_get(buf, i),
			  (unsigned long long) vals[i]);

	/* (3) and with the bulk unpacker, which the scan uses on the other path.
	 * A divergence here would mean two decoders of the same bytes disagree,
	 * which is the worst possible shape for this bug. */
	weave_for_unpack(buf, n, out);
	for (i = 0; i < n; i++)
		CHECK(out[i] == vals[i], "%s: unpack width=%d i=%d got=%llu want=%llu",
			  what, width, i, (unsigned long long) out[i],
			  (unsigned long long) vals[i]);
}

int
main(void)
{
	uint64		vals[MAXN];
	int			n,
				i,
				w,
				trial;
	char		label[128];

	/*
	 * Exhaustive over width.  Width is derived from the maximum value, so drive
	 * it by planting a value that needs exactly w bits.  Every width 0..64 is
	 * covered, including the boundaries where the implementation branches:
	 * width 0 (all zeros), the shift+width <= 64 fast path, and the wide path
	 * that assembles across a 9-byte window.
	 */
	printf("exhaustive width sweep\n");
	for (w = 0; w <= 64; w++)
	{
		for (n = 1; n <= 40; n++)
		{
			for (i = 0; i < n; i++)
				vals[i] = (w == 0) ? 0 : (rnd() & (w >= 64 ? ~(uint64) 0
													: (((uint64) 1 << w) - 1)));
			/* force the width by planting the max representable value */
			if (w > 0)
				vals[n / 2] = (w >= 64) ? ~(uint64) 0 : (((uint64) 1 << w) - 1);
			snprintf(label, sizeof(label), "width=%d n=%d", w, n);
			run_case(vals, n, label);
		}
	}

	/*
	 * Adversarial patterns.  The shift/mask path is where an off-by-one in the
	 * byte window or a 64-bit shift (undefined behaviour) would hide, and those
	 * only surface at specific alignments.
	 */
	printf("adversarial patterns\n");
	{
		struct
		{
			const char *name;
			uint64		fill;
		}			pats[] = {
			{"all-zero", 0},
			{"all-ones", ~(uint64) 0},
			{"alternating", 0xAAAAAAAAAAAAAAAAULL},
			{"alternating-inv", 0x5555555555555555ULL},
			{"high-bit", (uint64) 1 << 63},
			{"low-bit", 1},
		};
		size_t		p;

		for (p = 0; p < sizeof(pats) / sizeof(pats[0]); p++)
		{
			for (n = 1; n <= 130; n++)
			{
				for (i = 0; i < n; i++)
					vals[i] = pats[p].fill;
				snprintf(label, sizeof(label), "%s n=%d", pats[p].name, n);
				run_case(vals, n, label);
			}
		}
	}

	/* Every value needing a different width, so no single width dominates and
	 * the packed stream has values at every bit offset within a byte. */
	printf("mixed-width randomized\n");
	for (trial = 0; trial < 4000; trial++)
	{
		n = 1 + (int) (rnd() % MAXN);
		for (i = 0; i < n; i++)
		{
			int			bits = (int) (rnd() % 65);

			vals[i] = (bits == 0) ? 0
				: (rnd() & (bits >= 64 ? ~(uint64) 0
							: (((uint64) 1 << bits) - 1)));
		}
		snprintf(label, sizeof(label), "mixed trial=%d n=%d", trial, n);
		run_case(vals, n, label);
	}

	printf("\n%ld checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
