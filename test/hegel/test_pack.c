/*-------------------------------------------------------------------------
 *
 * test_pack.c
 *		Standalone property tests for the 32-lane bit-packing codec (V5).
 *
 * Links src/vector/pack.c directly, with no backend, no other vector-channel
 * file, and no PostgreSQL header -- the same discipline test_quantize.c and
 * test_doclen_block.c apply, for the same reason: a codec that cannot be
 * linked into a plain `gcc` invocation is a codec whose property test people
 * skip.
 *
 * The failure mode this codec actually has, twice over:
 *
 *	 1. An in-bounds bug that corrupts a NEIGHBOUR lane.  Every operation here
 *		(`weave_pack_lane`, `weave_pack_zero_lane`, `weave_pack_move_lane`) is
 *		specified to touch exactly one lane's bits.  A bit-index arithmetic
 *		slip touches the lane next to it instead, and unless the test checks
 *		every OTHER lane after every mutation, that bug is invisible -- the
 *		lane under test still round-trips fine, and the corrupted neighbour
 *		gets checked later against a stale expectation that "coincidentally"
 *		still matches, or isn't checked again before being overwritten. So:
 *		every check below re-verifies the FULL 32-lane state, not just the
 *		lane just touched.
 *
 *	 2. An out-of-bounds bit write past `weave_block_codebytes(dim, bits)`.
 *		This codec does `dst[b >> 3] |= ...` with a byte index computed from a
 *		coordinate and a lane; get the block-size formula wrong by even one
 *		coordinate at one bit width and it corrupts whatever memory follows the
 *		block -- which, in the real block layout, is WeaveVecBlockHdr or the
 *		next block.  Guard bytes on both sides of every allocation catch this
 *		deterministically; ASan (run separately, see the Makefile target)
 *		catches it even when the guard bytes happen not to be touched.
 *
 * Properties, swept over a grid of dim values (including dim=1, non-multiples
 * of 8, and non-multiples of 32) x bits in [WEAVE_BITS_MIN, WEAVE_BITS_MAX] x
 * both WeavePackLayout values:
 *
 *	 P1  pack then unpack is the identity, for every lane
 *	 P2  packing lane s disturbs no other lane
 *	 P3  zero_lane zeroes exactly its lane
 *	 P4  move_lane(dst, src) makes dst == old src and disturbs nothing else,
 *		 including the dst == src no-op case
 *	 P5  no operation ever writes outside weave_block_codebytes(dim, bits) --
 *		 guard bytes on both sides of the allocation stay untouched
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tp test/hegel/test_pack.c \
 *			src/vector/pack.c && /tmp/tp
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_pack.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/quantize.h"

static long failures = 0;
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

/* xorshift64*, matching test_doclen_block.c: reproducible, no libc rand(). */
static weave_uint64 rng_state = 0x9E3779B97F4A7C15ULL;

static weave_uint64
rng(void)
{
	weave_uint64 x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/* Sentinel guard bytes never legitimately produced by any pack operation on
 * an all-zero-initialized block, so any change to them is unambiguously an
 * out-of-bounds write. */
#define GUARD		64
#define SENTINEL	0xA5

/* Mask off the bits beyond the last coordinate in a per-vector code buffer, so
 * comparisons are against what the layout can actually represent -- the same
 * technique test_quantize.c's test_pack() uses. */
static void
mask_tail(weave_uint8 *code, int dim, int bits, int codebytes)
{
	if ((dim * bits) % 8 != 0)
		code[codebytes - 1] &=
			(weave_uint8) ((1u << ((dim * bits) % 8)) - 1);
}

/*
 * Verify every lane of `block` still unpacks to `expected[slot]`, for all
 * WEAVE_VEC_BLOCK lanes.  This is the check that catches property P2/P4: a
 * mutation to one lane that leaks into its neighbour.
 */
static void
check_all_lanes(WeavePackLayout layout, int dim, int bits,
				const weave_uint8 *block, weave_uint8 **expected,
				int codebytes, const char *where)
{
	weave_uint8 *back = malloc((size_t) codebytes);
	int			slot;

	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
	{
		weave_unpack_lane(layout, dim, bits, block, slot, back);
		CHECK(memcmp(back, expected[slot], (size_t) codebytes) == 0,
			  "%s: layout=%d dim=%d bits=%d slot=%d lane mismatch",
			  where, layout, dim, bits, slot);
	}
	free(back);
}

static void
check_guards(const weave_uint8 *buf, int blockbytes, const char *where,
			 WeavePackLayout layout, int dim, int bits)
{
	int			i;

	for (i = 0; i < GUARD; i++)
	{
		CHECK(buf[i] == SENTINEL,
			  "%s: layout=%d dim=%d bits=%d leading guard byte %d clobbered (0x%02x)",
			  where, layout, dim, bits, i, buf[i]);
		CHECK(buf[GUARD + blockbytes + i] == SENTINEL,
			  "%s: layout=%d dim=%d bits=%d trailing guard byte %d clobbered (0x%02x)",
			  where, layout, dim, bits, i, buf[GUARD + blockbytes + i]);
	}
}

static void
test_layout(WeavePackLayout layout, int dim, int bits)
{
	int			codebytes = (dim * bits + 7) / 8;
	int			blockbytes = weave_block_codebytes(dim, bits);
	weave_uint8 *buf = malloc((size_t) (blockbytes + 2 * GUARD));
	weave_uint8 *block = buf + GUARD;
	weave_uint8 **expected = malloc(WEAVE_VEC_BLOCK * sizeof(weave_uint8 *));
	weave_uint8 *code = malloc((size_t) codebytes);
	int			slot,
				j;

	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
		expected[slot] = calloc((size_t) codebytes, 1);

	memset(buf, SENTINEL, (size_t) (blockbytes + 2 * GUARD));
	memset(block, 0, (size_t) blockbytes);

	/* P1 + P2: pack every lane with fresh random data, checking after EACH
	 * pack that every lane -- not just the one just written -- still matches
	 * what it should. */
	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
	{
		for (j = 0; j < codebytes; j++)
			code[j] = (weave_uint8) (rng() & 0xFF);
		mask_tail(code, dim, bits, codebytes);

		weave_pack_lane(layout, dim, bits, block, slot, code);
		memcpy(expected[slot], code, (size_t) codebytes);

		check_all_lanes(layout, dim, bits, block, expected, codebytes,
						 "after pack");
		check_guards(buf, blockbytes, "after pack", layout, dim, bits);
	}

	/* P3: zero a lane in the middle of the block and confirm only it changed. */
	{
		int			victim = WEAVE_VEC_BLOCK / 3;

		weave_pack_zero_lane(layout, dim, bits, block, victim);
		memset(expected[victim], 0, (size_t) codebytes);
		check_all_lanes(layout, dim, bits, block, expected, codebytes,
						 "after zero_lane");
		check_guards(buf, blockbytes, "after zero_lane", layout, dim, bits);
	}

	/* Re-fill every lane with distinct random data so move_lane has real
	 * content to move (a zeroed lane moving would not exercise much). */
	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
	{
		for (j = 0; j < codebytes; j++)
			code[j] = (weave_uint8) (rng() & 0xFF);
		mask_tail(code, dim, bits, codebytes);
		weave_pack_lane(layout, dim, bits, block, slot, code);
		memcpy(expected[slot], code, (size_t) codebytes);
	}
	check_all_lanes(layout, dim, bits, block, expected, codebytes, "after refill");
	check_guards(buf, blockbytes, "after refill", layout, dim, bits);

	/* P4: move_lane over a spread of (dst, src) pairs, including a no-op
	 * (dst == src) and both directions of a swap-remove (last lane into a
	 * hole, and a hole into what was the last lane). */
	{
		int			pairs[][2] = {
			{0, WEAVE_VEC_BLOCK - 1},		/* the real vacuum use: hole <- last */
			{WEAVE_VEC_BLOCK - 1, 0},		/* the reverse */
			{5, 5},							/* no-op */
			{3, 17},
			{31, 31},						/* no-op at the boundary */
			{0, 0}							/* no-op at the other boundary */
		};
		int			npairs = (int) (sizeof(pairs) / sizeof(pairs[0]));
		int			p;

		for (p = 0; p < npairs; p++)
		{
			int			dst = pairs[p][0];
			int			src = pairs[p][1];
			weave_uint8 *src_before = malloc((size_t) codebytes);

			memcpy(src_before, expected[src], (size_t) codebytes);

			weave_pack_move_lane(layout, dim, bits, block, dst, src);
			memcpy(expected[dst], src_before, (size_t) codebytes);
			/* src itself is untouched by a move -- only dst changes -- per
			 * weave/quantize.h's contract that livemask, not lane content,
			 * marks a slot dead. */

			check_all_lanes(layout, dim, bits, block, expected, codebytes,
							 "after move_lane");
			check_guards(buf, blockbytes, "after move_lane", layout, dim, bits);

			free(src_before);
		}
	}

	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
		free(expected[slot]);
	free(expected);
	free(code);
	free(buf);
}

/*
 * P5, isolated: a single all-ones pack into every lane in turn, on a buffer
 * sized EXACTLY to weave_block_codebytes -- the tightest case, since an
 * off-by-one in the size formula and an off-by-one in the write both have to
 * line up to escape this check, and this makes them line up on purpose.
 */
static void
test_bounds(WeavePackLayout layout, int dim, int bits)
{
	int			codebytes = (dim * bits + 7) / 8;
	int			blockbytes = weave_block_codebytes(dim, bits);
	weave_uint8 *buf = malloc((size_t) (blockbytes + 2 * GUARD));
	weave_uint8 *block = buf + GUARD;
	weave_uint8 *code = malloc((size_t) codebytes);
	weave_uint8 *back = malloc((size_t) codebytes);
	int			slot,
				j;

	memset(buf, SENTINEL, (size_t) (blockbytes + 2 * GUARD));
	memset(block, 0, (size_t) blockbytes);
	memset(code, 0xFF, (size_t) codebytes);
	mask_tail(code, dim, bits, codebytes);

	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
	{
		weave_pack_lane(layout, dim, bits, block, slot, code);
		check_guards(buf, blockbytes, "bounds/all-ones pack", layout, dim, bits);
	}
	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
	{
		weave_unpack_lane(layout, dim, bits, block, slot, back);
		CHECK(memcmp(back, code, (size_t) codebytes) == 0,
			  "bounds: layout=%d dim=%d bits=%d slot=%d all-ones round-trip failed",
			  layout, dim, bits, slot);
	}
	for (j = 0; j < WEAVE_VEC_BLOCK; j++)
	{
		weave_pack_zero_lane(layout, dim, bits, block, j);
		check_guards(buf, blockbytes, "bounds/zero_lane", layout, dim, bits);
	}
	for (j = 0; j < WEAVE_VEC_BLOCK; j++)
	{
		weave_pack_move_lane(layout, dim, bits, block, j, WEAVE_VEC_BLOCK - 1 - j);
		check_guards(buf, blockbytes, "bounds/move_lane", layout, dim, bits);
	}

	free(back);
	free(code);
	free(buf);
}

int
main(void)
{
	/* Deliberately includes dim=1 (degenerate), non-multiples of 8 (partial
	 * trailing byte per coordinate), and non-multiples of WEAVE_VEC_BLOCK=32
	 * (irrelevant to LANE indexing but worth ruling out), plus the dims the
	 * quantizer's own test uses so the two files' coverage overlaps at the
	 * boundary rather than leaving a gap between them. */
	int			dims[] = {1, 2, 3, 7, 8, 9, 16, 31, 32, 33, 63, 64, 100, 127,
		200, 256, 384};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	int			bits,
				i,
				layout;

	printf("pack/unpack round-trip, lane isolation, zero_lane, move_lane, bounds\n");

	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		for (i = 0; i < ndims; i++)
		{
			for (layout = 0; layout <= 1; layout++)
			{
				test_layout((WeavePackLayout) layout, dims[i], bits);
				test_bounds((WeavePackLayout) layout, dims[i], bits);
			}
		}
	}

	if (failures)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
