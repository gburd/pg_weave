/*-------------------------------------------------------------------------
 *
 * test_vecpage.c
 *		Standalone property tests for the coordinate-sliced code strips (V7).
 *
 * Links src/vector/vecpage.c and src/vector/pack.c with no backend and no
 * PostgreSQL header, the same discipline test_pack.c applies and for the same
 * reason: a format whose property test needs a running cluster is a format whose
 * property test people skip.
 *
 * WHAT CAN GO WRONG HERE, which is what the properties are aimed at.
 *
 *	1. A STRIP THAT SCORES THE WRONG COORDINATES.  The whole shape rests on
 *	   coordinate j of a WEAVE_PACK_LANE block living at byte j*4*bits.  Get that
 *	   off by one coordinate and every score is wrong by a small amount -- plausible
 *	   numbers, silently misranked rows, and no test that only checks "the page
 *	   parses" would notice.  So P1 reassembles the block from its strips and demands
 *	   BYTE EQUALITY with the original, and P2 checks each strip against the exact
 *	   byte range it should hold.
 *
 *	2. NONDETERMINISM IN THE PAGE IMAGE.  weave_block_codebytes() rounds up per
 *	   lane rather than once, so a block buffer carries 0-28 bytes no pack function
 *	   ever writes.  doc/specs/VECTOR_CHANNEL.md sect. 7 records that a page image
 *	   containing them breaks a GenericXLog delta over a rewritten page and any
 *	   cross-architecture fixture hash.  P3 builds the same strip twice into buffers
 *	   pre-filled with DIFFERENT garbage and demands identical images.
 *
 *	3. A REFUSAL THAT SILENTLY SUCCEEDS.  VECMAJOR cannot be sliced by coordinate
 *	   (a cut splits vectors), and a strip whose range runs past dim would read
 *	   another block's bytes.  P4 asserts every refusal actually refuses, because
 *	   a validator that returns 0 on bad input is worse than no validator.
 *
 * Properties, swept over dim in a grid including 1, non-multiples of 8 and of 32,
 * and up to WEAVE_MAX_DIM, x bits in [WEAVE_BITS_MIN, WEAVE_BITS_MAX] x several
 * page capacities (including a capacity of exactly one coordinate):
 *
 *	P1	strips cover the block exactly once and reassemble to it byte-identically,
 *		AND every lane unpacks from the reassembly to the code that was packed in.
 *		The second half matters because it is the only check that does not use this
 *		file's own offset formula: it goes through weave_unpack_lane(), so a
 *		self-consistently wrong geometry cannot satisfy it.  Mutation M5 (a wrong
 *		weave_strip_coordbytes) passed every other property precisely because they
 *		all computed their expectations from the formula under test.
 *	P2	each strip's payload equals the block's bytes for its coordinate range
 *	P3	the image is deterministic over the WHOLE destination, which is a PAGE and
 *		not an exact fit: the strip occupies part of it and the tail must be zeroed.
 *		The first version of this test passed `need` as the buffer length, so the
 *		header plus the payload covered every byte and the determinism requirement
 *		was never actually exercised -- mutation M2, which zeroed only the header,
 *		passed it.  The production caller always has slack.
 *	P4	every refusal refuses: VECMAJOR, ncoords < 1, range past dim by exactly one,
 *		short destination, short payload for the declared ncoords, nonzero pad
 *	P5	no build writes outside the buffer it was given -- guard bytes on both sides
 *	P6	the coordinate stride is 4*bits against a HARD-CODED table, not against the
 *		function that computes it
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tvp test/hegel/test_vecpage.c \
 *			src/vector/vecpage.c src/vector/pack.c && /tmp/tvp
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_vecpage.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/vecpage.h"

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

/* xorshift64*, matching test_pack.c: reproducible, no libc rand(). */
static weave_uint64 rng_state = 0x243F6A8885A308D3ULL;

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

#define GUARD		64
#define SENTINEL	0xC3

/*
 * Build a block by packing 32 random lanes through the real packer, so the bytes
 * under test are bytes a real writer would produce -- not a synthetic pattern that
 * could agree with a wrong offset formula by accident.
 */
static weave_uint8 *
make_block(int dim, int bits, size_t *blockbytes)
{
	int			cbytes = weave_block_codebytes(dim, bits);
	int			codebytes = (dim * bits + 7) / 8;
	weave_uint8 *block = calloc(1, (size_t) cbytes);
	weave_uint8 *code = malloc((size_t) codebytes);
	int			slot,
				j;

	if (!block || !code)
	{
		printf("out of memory\n");
		exit(2);
	}
	for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
	{
		for (j = 0; j < codebytes; j++)
			code[j] = (weave_uint8) (rng() & 0xFF);
		weave_pack_lane(WEAVE_PACK_LANE, dim, bits, block, slot, code);
	}
	free(code);
	*blockbytes = (size_t) cbytes;
	return block;
}

/* P1, P2, P3, P5 for one (dim, bits, capacity) point. */
static void
one_point(int dim, int bits, int usable)
{
	size_t		blockbytes;
	weave_uint8 *block = make_block(dim, bits, &blockbytes);
	int			cpp = weave_strip_coords_per_page(usable, bits);
	size_t		cb = (size_t) weave_strip_coordbytes(bits);
	weave_uint8 *rebuilt;
	int			nstrip,
				s,
				j0 = 0;

	if (cpp <= 0)
	{
		/* A page that cannot hold one coordinate must be reported as such, and
		 * every build against it must refuse rather than truncate. */
		weave_uint8 tiny[64];

		CHECK(weave_strip_build(tiny, (size_t) usable, WEAVE_PACK_LANE, dim, bits,
								0, 0, 1, 0, block) < 0,
			  "dim=%d bits=%d usable=%d: build succeeded on a page too small",
			  dim, bits, usable);
		free(block);
		return;
	}

	nstrip = weave_strip_count(dim, cpp);
	CHECK(nstrip >= 1, "dim=%d bits=%d cpp=%d: no strips", dim, bits, cpp);

	rebuilt = calloc(1, blockbytes);
	if (!rebuilt)
	{
		printf("out of memory\n");
		exit(2);
	}

	for (s = 0; s < nstrip; s++)
	{
		int			ncoords = dim - j0 < cpp ? dim - j0 : cpp;
		size_t		need = sizeof(WeaveVecStripHdr) + (size_t) ncoords * cb;
		/*
		 * The destination is the whole page, not the strip.  That is what the AM
		 * will pass, and it is the only shape in which the zeroing requirement is
		 * observable: with an exact-fit buffer the header plus the payload cover
		 * every byte, so a writer that zeroed nothing would still be
		 * deterministic.  Mutation M2 proved that hole was real.
		 */
		size_t		dstlen = (size_t) usable;
		weave_uint8 *buf = malloc(GUARD + dstlen + GUARD);
		weave_uint8 *img = buf + GUARD;
		int			n,
					i;
		WeaveVecStripHdr hdr;
		const weave_uint8 *codes;
		const char *why = NULL;

		if (!buf)
		{
			printf("out of memory\n");
			exit(2);
		}
		memset(buf, SENTINEL, GUARD + dstlen + GUARD);

		n = weave_strip_build(img, dstlen, WEAVE_PACK_LANE, dim, bits,
							  (weave_uint32) 7, j0, ncoords, 0, block);
		CHECK(n == (int) need, "dim=%d bits=%d: build returned %d, wanted %zu",
			  dim, bits, n, need);

		/* P5: nothing outside the buffer moved. */
		for (i = 0; i < GUARD; i++)
		{
			CHECK(buf[i] == SENTINEL, "dim=%d bits=%d: underflow at %d", dim, bits, i);
			CHECK(buf[GUARD + dstlen + i] == SENTINEL,
				  "dim=%d bits=%d: overflow at %d", dim, bits, i);
		}

		/* P3, first half: the page tail past the strip is zero, not whatever the
		 * buffer held.  This is the GenericXLog-delta and fixture-hash
		 * requirement from doc/specs/VECTOR_CHANNEL.md sect. 7. */
		for (i = (int) need; i < (int) dstlen; i++)
			CHECK(img[i] == 0,
				  "dim=%d bits=%d: page byte %d past the strip is 0x%02X, not zero",
				  dim, bits, i, img[i]);

		/* P3: determinism.  Same inputs, a buffer pre-filled with different
		 * garbage, and the image must be identical byte for byte. */
		{
			weave_uint8 *alt = malloc(dstlen);
			int			m;

			if (!alt)
			{
				printf("out of memory\n");
				exit(2);
			}
			for (i = 0; i < (int) dstlen; i++)
				alt[i] = (weave_uint8) (rng() & 0xFF);
			m = weave_strip_build(alt, dstlen, WEAVE_PACK_LANE, dim, bits,
								  (weave_uint32) 7, j0, ncoords, 0, block);
			CHECK(m == n, "dim=%d bits=%d: second build returned %d not %d",
				  dim, bits, m, n);
			CHECK(memcmp(alt, img, dstlen) == 0,
				  "dim=%d bits=%d j0=%d: page image is NOT deterministic over the"
				  " whole page", dim, bits, j0);
			free(alt);
		}

		/* Parse it back and check the header round-trips.  Parsing is given the
		 * whole page, as a reader would be. */
		n = weave_strip_parse(img, dstlen, dim, bits, &hdr, &codes, &why);
		CHECK(n == ncoords, "dim=%d bits=%d: parse returned %d (%s), wanted %d",
			  dim, bits, n, why ? why : "no reason", ncoords);
		CHECK(hdr.blockno == 7 && hdr.j0 == (weave_uint16) j0 &&
			  hdr.ncoords == (weave_uint16) ncoords && hdr.flags == 0 &&
			  hdr.pad == 0,
			  "dim=%d bits=%d: header did not round-trip", dim, bits);

		/* P2: the payload is exactly the block's bytes for this range. */
		CHECK(memcmp(codes, block + (size_t) j0 * cb, (size_t) ncoords * cb) == 0,
			  "dim=%d bits=%d j0=%d: strip payload is the wrong byte range",
			  dim, bits, j0);

		/* Scatter into the reassembly buffer for P1. */
		CHECK(weave_strip_scatter(rebuilt, blockbytes, dim, bits, &hdr, codes) == 0,
			  "dim=%d bits=%d j0=%d: scatter refused a strip it built",
			  dim, bits, j0);

		/* P4, on a real image: a nonzero pad must be rejected, because it means
		 * the writer did not zero its buffer and the image is not reproducible. */
		{
			weave_uint8 *bad = malloc(need);
			WeaveVecStripHdr bh;
			const weave_uint8 *bc;

			if (!bad)
			{
				printf("out of memory\n");
				exit(2);
			}
			memcpy(bad, img, need);
			((WeaveVecStripHdr *) bad)->pad = 1;
			CHECK(weave_strip_parse(bad, need, dim, bits, &bh, &bc, &why) < 0,
				  "dim=%d bits=%d: parse accepted a nonzero pad", dim, bits);
			free(bad);
		}

		free(buf);
		j0 += ncoords;
	}

	/* P1: the strips covered every coordinate exactly once, and the block they
	 * reassemble to is byte-identical over the tight region.  Compare only the
	 * tight 4*dim*bits/8 bytes: the slack past it is never written by the packer
	 * and is zero in both buffers by construction. */
	CHECK(j0 == dim, "dim=%d bits=%d: strips covered %d coordinates", dim, bits, j0);
	CHECK(memcmp(rebuilt, block, (size_t) dim * cb) == 0,
		  "dim=%d bits=%d: reassembled block differs from the original", dim, bits);

	/*
	 * P1, second half: every lane must UNPACK from the reassembly to the same
	 * bytes it unpacks to from the original.  This is the only check here that does
	 * not use this file's coordinate-offset formula -- it goes through
	 * weave_unpack_lane(), whose bit indexing is the packer's own.  A
	 * self-consistently wrong geometry satisfies every byte comparison above and
	 * fails this one, which is exactly what mutation M5 demonstrated.
	 */
	{
		int			codebytes = (dim * bits + 7) / 8;
		weave_uint8 *a = malloc((size_t) codebytes);
		weave_uint8 *b = malloc((size_t) codebytes);
		int			slot;

		if (!a || !b)
		{
			printf("out of memory\n");
			exit(2);
		}
		for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
		{
			weave_unpack_lane(WEAVE_PACK_LANE, dim, bits, block, slot, a);
			weave_unpack_lane(WEAVE_PACK_LANE, dim, bits, rebuilt, slot, b);
			CHECK(memcmp(a, b, (size_t) codebytes) == 0,
				  "dim=%d bits=%d: lane %d unpacks differently after reassembly",
				  dim, bits, slot);
		}
		free(a);
		free(b);
	}

	free(rebuilt);
	free(block);
}

/* P4: the refusals that do not need a whole point built. */
static void
refusals(void)
{
	weave_uint8 block[4096];
	weave_uint8 buf[4096];
	WeaveVecStripHdr hdr;
	const weave_uint8 *codes;
	const char *why;
	int			dim = 64,
				bits = 4;

	memset(block, 0x5A, sizeof(block));

	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_VECMAJOR, dim, bits,
							0, 0, 8, 0, block) < 0,
		  "VECMAJOR was accepted, but a coordinate cut splits vectors there");
	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_LANE, dim, bits,
							0, 0, 0, 0, block) < 0, "ncoords=0 was accepted");
	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_LANE, dim, bits,
							0, dim - 2, 4, 0, block) < 0,
		  "a range running past dim was accepted");
	/* Past dim by EXACTLY ONE.  The first version of this test only tried dim+2,
	 * so a check written as `> dim + 1` passed it (mutation M4).  Boundary cases
	 * are the only cases an off-by-one can be caught at. */
	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_LANE, dim, bits,
							0, dim - 1, 2, 0, block) < 0,
		  "a range running exactly one coordinate past dim was accepted");
	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_LANE, dim, 1,
							0, 0, 4, 0, block) < 0, "bits=1 was accepted");
	CHECK(weave_strip_build(buf, sizeof(WeaveVecStripHdr) + 4, WEAVE_PACK_LANE,
							dim, bits, 0, 0, 8, 0, block) < 0,
		  "a destination too short for the declared strip was accepted");

	/*
	 * A header declaring more coordinates than the payload holds -- and the
	 * declared count must stay WITHIN dim, or the range check catches it first and
	 * the length check is never the discriminator.  That is why mutation M7
	 * (removing the length check) originally survived: the test's ncoords was
	 * 4000 against dim 64, so it never reached the check it was aiming at.
	 */
	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_LANE, dim, bits,
							1, 0, dim, 0, block) > 0, "setup build failed");
	CHECK(weave_strip_parse(buf, sizeof(WeaveVecStripHdr) +
							(size_t) (dim / 2) * 16, dim, bits,
							&hdr, &codes, &why) < 0,
		  "parse accepted a strip whose payload is half the length it declares");
	CHECK(why != NULL, "parse refused without saying why");

	/* A range past dim on the read side too: on-disk bytes are not trusted. */
	CHECK(weave_strip_build(buf, sizeof(buf), WEAVE_PACK_LANE, dim, bits,
							1, 0, 8, 0, block) > 0, "setup build failed");
	((WeaveVecStripHdr *) buf)->j0 = (weave_uint16) (dim - 1);
	CHECK(weave_strip_parse(buf, sizeof(buf), dim, bits, &hdr, &codes, &why) < 0,
		  "parse accepted a strip whose range runs past dim");
}

int
main(void)
{
	static const int dims[] = {1, 2, 7, 8, 31, 32, 33, 64, 127, 128, 256, 384,
		480, 511, 512, 768, 960, 1536, 4096, WEAVE_MAX_DIM};
	/* Page capacities: the real one (8,160 usable bytes on an 8 kB page), one that
	 * holds exactly one coordinate at 4 bits, one absurdly small, and two in
	 * between -- the boundaries are where an off-by-one in the geometry lives. */
	static const int usables[] = {8160, 4096, 1024, 28, 12, 8};
	int			di,
				bits,
				ui;

	printf("== V7 strip format: coordinate-sliced code pages ==\n");
	refusals();

	/*
	 * P6: the coordinate stride against a hard-coded table rather than against the
	 * function that computes it.  Every other property derives its expectations
	 * from weave_strip_coordbytes(), so a wrong stride is self-consistent and
	 * invisible to them (mutation M5).  32 lanes at `bits` each is 4*bits bytes;
	 * these are the three widths that ship.
	 */
	CHECK(weave_strip_coordbytes(2) == 8, "2-bit coordinate stride is not 8 bytes");
	CHECK(weave_strip_coordbytes(3) == 12, "3-bit coordinate stride is not 12 bytes");
	CHECK(weave_strip_coordbytes(4) == 16, "4-bit coordinate stride is not 16 bytes");

	for (di = 0; di < (int) (sizeof(dims) / sizeof(dims[0])); di++)
	{
		for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
		{
			for (ui = 0; ui < (int) (sizeof(usables) / sizeof(usables[0])); ui++)
			{
				/* WEAVE_MAX_DIM at every width against every capacity is a lot of
				 * memcmp over 8 MB blocks; keep the largest dim to the real page
				 * size, which is the only capacity that ships. */
				if (dims[di] > 4096 && usables[ui] != 8160)
					continue;
				one_point(dims[di], bits, usables[ui]);
			}
		}
	}

	printf("checks: %ld, failures: %ld\n", checks, failures);
	if (failures > 0)
	{
		printf("== FAILED ==\n");
		return 1;
	}
	printf("== ALL PROPERTIES HOLD ==\n");
	return 0;
}
