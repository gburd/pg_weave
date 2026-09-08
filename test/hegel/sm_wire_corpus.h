/*-------------------------------------------------------------------------
 *
 * sm_wire_corpus.h
 *		The shared corpus for the cross-version sparsemap wire check.
 *
 * Included by BOTH the writer (old library) and the reader (new library), so the
 * two cannot disagree about what was written.  Deterministic: the corpus is part
 * of the result.
 *
 * Shapes are chosen to hit the structures sparsemap 5.5.0's fixes touched -- dense
 * ranges become RLE chunks, sparse singletons stay sparse, and multiples of 64 are
 * where the sm_select off-by-one lived -- plus a randomized mixed-density sweep,
 * because a mix of sparse and RLE chunks in one map is where the both-RLE fallback
 * bug hid.
 *
 * Bits are always ASCENDING and UNIQUE, because that is how pg_weave writes them:
 * a segment's tombstones and a trigram's term ordinals are both emitted in order.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#ifndef SM_WIRE_CORPUS_H
#define SM_WIRE_CORPUS_H

#include <stdint.h>
#include <stdio.h>

#define SMW_BUFSZ    (1 << 21)
#define SMW_MAXBITS  60000
#define SMW_NFIXED   7
#define SMW_NRANDOM  120
#define SMW_NCASES   (SMW_NFIXED + SMW_NRANDOM)

/* xoshiro256**, reseeded per case so cases are independent of each other and of
 * evaluation order. */
static void
smw_seed(uint64_t *s, uint64_t k)
{
	int			i;

	s[0] = 0x9e3779b97f4a7c15ULL ^ k;
	s[1] = 0xbf58476d1ce4e5b9ULL + k;
	s[2] = 0x94d049bb133111ebULL ^ (k << 17);
	s[3] = 0x2545f4914f6cdd1dULL + (k << 31);
	for (i = 0; i < 8; i++)
	{
		uint64_t	t = s[1] << 17;

		s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
		s[2] ^= t;
		s[3] = (s[3] << 45) | (s[3] >> 19);
	}
}

static uint64_t
smw_rnd(uint64_t *s)
{
	uint64_t	r = ((s[1] * 5) << 7 | (s[1] * 5) >> 57) * 9;
	uint64_t	t = s[1] << 17;

	s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
	s[2] ^= t;
	s[3] = (s[3] << 45) | (s[3] >> 19);
	return r;
}

static const char *
smw_name(int c)
{
	static char buf[64];

	switch (c)
	{
		case 0: return "dense [0,16384) -- RLE chunks";
		case 1: return "dense [0,16357) -- the sm_difference case";
		case 2: return "[0,128)+[500,510) -- the sm_select case";
		case 3: return "{42,1024} -- the big-endian case";
		case 4: return "every 64th bit -- slot boundaries";
		case 5: return "single bit 0";
		case 6: return "single high bit";
		default:
			snprintf(buf, sizeof(buf), "random mixed-density #%d", c - SMW_NFIXED);
			return buf;
	}
}

/* Fill `bits` with case `c` and return the count. */
static int
smw_case(int c, uint64_t *bits)
{
	int			n = 0;
	uint64_t	i;

	switch (c)
	{
		case 0:
			for (i = 0; i < 16384; i++) bits[n++] = i;
			return n;
		case 1:
			for (i = 0; i < 16357; i++) bits[n++] = i;
			return n;
		case 2:
			for (i = 0; i < 128; i++) bits[n++] = i;
			for (i = 500; i < 510; i++) bits[n++] = i;
			return n;
		case 3:
			bits[n++] = 42; bits[n++] = 1024;
			return n;
		case 4:
			for (i = 0; i < 4096; i += 64) bits[n++] = i;
			return n;
		case 5:
			bits[n++] = 0;
			return n;
		case 6:
			bits[n++] = 50000;
			return n;
		default:
		{
			uint64_t	s[4];
			uint64_t	cur = 0;
			int			stride_max;

			smw_seed(s, (uint64_t) c);
			stride_max = 1 + (int) (smw_rnd(s) % 200);
			while (n < SMW_MAXBITS && cur < 200000)
			{
				bits[n++] = cur;
				cur += 1 + (smw_rnd(s) % (uint64_t) stride_max);
			}
			return n;
		}
	}
}

#endif							/* SM_WIRE_CORPUS_H */
