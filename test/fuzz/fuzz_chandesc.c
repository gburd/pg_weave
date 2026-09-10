/*
 * fuzz_chandesc.c -- corruption/fuzz harness for the WEAVE_CHANDESC page
 * decoder, the v6 format break's new on-disk structure.
 *
 * A bolt's channel-descriptor page is the first thing a v6 reader parses about a
 * bolt, and every field in it is a length or a block number that later code
 * dereferences: nweft indexes an array, and each weft's `root` becomes a
 * ReadBuffer() argument.  On-disk bytes are not trusted (doc/CONVENTIONS.md
 * decision 2), so the property below is not optional.
 *
 * WHAT IS FUZZED IS THE REAL DECODER.  weave_chandesc_check() lives in
 * include/weave/chandesc.h with no PostgreSQL dependency for exactly this reason
 * (docvalid.h is the exemplar), so unlike fuzz_block.c there is no transcription
 * and no modeling gap: this is the same function src/am/am.c's
 * weave_read_chandesc() calls.
 *
 * PROPERTY (default build, under ASan+UBSan): for ANY byte string of ANY length,
 * weave_chandesc_check() reads only inside [base, base+avail), returns a
 * recognized error code, and when it returns WEAVE_CD_OK the header's nweft
 * descriptors provably fit within avail, every kind is in range, every root is a
 * valid block, and the (kind, attnum) sequence is strictly ascending.  The last
 * clause matters: a caller that trusted a non-ascending array would fail to spot
 * a duplicate weft and could score one bolt's vector weft twice.
 *
 * The buffer handed to the validator is sized EXACTLY to `avail`, not to a whole
 * 8 KB page, so ASan's heap redzone sits immediately past the last readable byte.
 * A one-byte overread is a hard failure rather than a read of adjacent page
 * bytes -- this is stricter than the backend, where the page really is BLCKSZ,
 * and deliberately so.
 *
 * TEETH (FUZZ_NO_ARRAY_GUARD=1): builds a deliberately weakened copy of the
 * validator, weak_check() below, with the "descriptor array must fit avail"
 * guard removed -- the single most likely guard for someone to drop, since the
 * magic and version checks look like they already bound the page.  A corrupt
 * nweft then walks weft[i] straight past the buffer and ASan aborts, proving the
 * harness detects the bug class instead of passing vacuously.  See run.sh.
 *
 * No hegel/cmocka: deterministic PRNG loop, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/chandesc.h"

#define NBLOCKS 4096			/* modeled relation length for the root check */

static uint64_t rngstate = 0x9E3779B97F4A7C15ULL;

static uint32_t
rnd(void)
{
	/* splitmix64, so the corpus is identical on every host and every rerun */
	uint64_t	z;

	rngstate += 0x9E3779B97F4A7C15ULL;
	z = rngstate;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return (uint32_t) ((z ^ (z >> 31)) >> 16);
}

#ifdef FUZZ_NO_ARRAY_GUARD
/*
 * The planted bug: weave_chandesc_check() with the array-fits-avail guard
 * deleted.  Everything else is identical.  This MUST overrun.
 */
static WeaveCdError
weak_check(const void *base, size_t avail, uint32_t nblocks, uint16_t *nweft_out)
{
	const WeaveCdPage *hdr = (const WeaveCdPage *) base;
	const WeaveCdDesc *weft;
	uint32_t	prevkey = 0;
	uint16_t	i;

	if (nweft_out != NULL)
		*nweft_out = 0;
	if (base == NULL || avail < WEAVE_CD_HDRSIZE)
		return WEAVE_CD_TRUNCATED;
	if (hdr->magic != WEAVE_CHANDESC_MAGIC)
		return WEAVE_CD_MAGIC;
	if (hdr->version != WEAVE_CHANDESC_VERSION)
		return WEAVE_CD_VERSION;
	if (hdr->reserved != 0)
		return WEAVE_CD_RESERVED;
	if (hdr->nweft == 0 || hdr->nweft > WEAVE_MAX_WEFTS)
		return WEAVE_CD_NWEFT;
	/* THE MISSING GUARD:
	 * if (WEAVE_CD_SIZE(hdr->nweft) > avail) return WEAVE_CD_ARRAY; */

	weft = (const WeaveCdDesc *) ((const char *) base + WEAVE_CD_HDRSIZE);
	for (i = 0; i < hdr->nweft; i++)
	{
		uint32_t	key;

		if (weft[i].kind == WEAVE_WK_INVALID ||
			weft[i].kind >= (uint16_t) WEAVE_WK_NKINDS)
			return WEAVE_CD_KIND;
		if (weft[i].root == WEAVE_CD_INVALID_BLK || weft[i].root == 0)
			return WEAVE_CD_ROOT;
		if (nblocks != 0 && weft[i].root >= nblocks)
			return WEAVE_CD_ROOT;
		key = ((uint32_t) weft[i].kind << 16) | weft[i].attnum;
		if (i > 0 && key <= prevkey)
			return WEAVE_CD_ORDER;
		prevkey = key;
	}
	if (nweft_out != NULL)
		*nweft_out = hdr->nweft;
	return WEAVE_CD_OK;
}
#define CHECK(b, a, n, o)	weak_check((b), (a), (n), (o))
#else
#define CHECK(b, a, n, o)	weave_chandesc_check((b), (a), (n), (o))
#endif

/*
 * Independently re-verify the guarantees the validator claims, WITHOUT reusing
 * its code.  A validator that returns OK on a page it should have rejected is a
 * wrong answer, which the sanitizers cannot see; only an independent restatement
 * of the postcondition catches it.
 */
static void
verify_accepted(const unsigned char *buf, size_t avail, uint16_t nweft)
{
	const WeaveCdPage *hdr = (const WeaveCdPage *) buf;
	const WeaveCdDesc *weft = (const WeaveCdDesc *) (buf + WEAVE_CD_HDRSIZE);
	uint32_t	prevkey = 0;
	uint16_t	i;
	uint16_t	j;

	assert(nweft == hdr->nweft);
	assert(nweft >= 1 && nweft <= WEAVE_MAX_WEFTS);
	assert(WEAVE_CD_HDRSIZE <= avail);
	assert(WEAVE_CD_SIZE(nweft) <= avail);
	assert(hdr->magic == WEAVE_CHANDESC_MAGIC);
	assert(hdr->version == WEAVE_CHANDESC_VERSION);
	assert(hdr->reserved == 0);

	for (i = 0; i < nweft; i++)
	{
		uint32_t	key = ((uint32_t) weft[i].kind << 16) | weft[i].attnum;

		assert(weft[i].kind != WEAVE_WK_INVALID);
		assert(weft[i].kind < (uint16_t) WEAVE_WK_NKINDS);
		assert(weft[i].root != 0 && weft[i].root != WEAVE_CD_INVALID_BLK);
		assert(weft[i].root < NBLOCKS);
		assert(i == 0 || key > prevkey);
		prevkey = key;
		for (j = 0; j < i; j++)
			assert(weft[j].root != weft[i].root);
	}
}

/* Lay out a well-formed page image of `nweft` wefts into buf (>= 8192 bytes).
 * Returns the byte length the writer would have set pd_lower to. */
static size_t
build_valid(unsigned char *buf, int nweft)
{
	WeaveCdPage *hdr = (WeaveCdPage *) buf;
	WeaveCdDesc *weft = (WeaveCdDesc *) (buf + WEAVE_CD_HDRSIZE);
	int			i;

	memset(buf, 0, WEAVE_CD_SIZE(WEAVE_MAX_WEFTS));
	hdr->magic = WEAVE_CHANDESC_MAGIC;
	hdr->version = WEAVE_CHANDESC_VERSION;
	hdr->nweft = (uint16_t) nweft;
	hdr->reserved = 0;
	for (i = 0; i < nweft; i++)
	{
		/* strictly ascending (kind, attnum) with distinct roots */
		weft[i].kind = (uint16_t) (1 + (i % (WEAVE_WK_NKINDS - 1)));
		weft[i].attnum = (uint16_t) (1 + i);
		weft[i].flags = 0;
		weft[i].root = (uint32_t) (1 + i);
	}
	/* the ascending rule needs kind to be non-decreasing; with attnum strictly
	 * ascending inside a kind the key is strictly ascending only if kind is
	 * non-decreasing, so sort by rewriting kind monotonically */
	for (i = 0; i < nweft; i++)
		weft[i].kind = (uint16_t) (1 + (i * (WEAVE_WK_NKINDS - 1)) / (nweft > 0 ? nweft : 1));
	return WEAVE_CD_SIZE(nweft);
}

int
main(void)
{
	unsigned long iters = 0;
	unsigned long accepted = 0;
	unsigned long rejected = 0;
	int			trial;

	/* 1. every well-formed image of every legal weft count must be accepted */
	for (trial = 1; trial <= WEAVE_MAX_WEFTS; trial++)
	{
		unsigned char scratch[8192];
		size_t		len = build_valid(scratch, trial);
		unsigned char *exact = (unsigned char *) malloc(len);
		uint16_t	nweft = 0;
		WeaveCdError e;

		memcpy(exact, scratch, len);
		e = CHECK(exact, len, NBLOCKS, &nweft);
		if (e != WEAVE_CD_OK)
		{
			fprintf(stderr, "well-formed image of %d weft(s) rejected: %s\n",
					trial, weave_chandesc_errstr(e));
			free(exact);
			return 1;
		}
		verify_accepted(exact, len, nweft);
		free(exact);
		iters++;
	}

	/* 2. a well-formed image truncated to every length must be rejected or
	 * accepted consistently, never overread */
	for (trial = 1; trial <= WEAVE_MAX_WEFTS; trial++)
	{
		unsigned char scratch[8192];
		size_t		len = build_valid(scratch, trial);
		size_t		cut;

		for (cut = 0; cut <= len; cut++)
		{
			unsigned char *exact = (unsigned char *) malloc(cut ? cut : 1);
			uint16_t	nweft = 0;
			WeaveCdError e;

			memcpy(exact, scratch, cut);
			e = CHECK(exact, cut, NBLOCKS, &nweft);
			if (e == WEAVE_CD_OK)
				verify_accepted(exact, cut, nweft);
			free(exact);
			iters++;
		}
	}

	/* 3. random single-field and multi-byte corruption of a well-formed image */
	for (trial = 0; trial < 400000; trial++)
	{
		unsigned char scratch[8192];
		int			nweft_in = 1 + (int) (rnd() % WEAVE_MAX_WEFTS);
		size_t		len = build_valid(scratch, nweft_in);
		int			nsmash = 1 + (int) (rnd() % 6);
		unsigned char *exact;
		uint16_t	nweft = 0;
		WeaveCdError e;
		int			k;
		size_t		avail;

		for (k = 0; k < nsmash; k++)
		{
			size_t		off = rnd() % len;

			scratch[off] = (unsigned char) rnd();
		}
		/* also corrupt the DECLARED length independently of the buffer we give
		 * it: a torn pd_lower is the realistic case */
		avail = (rnd() % 3 == 0) ? (rnd() % (len + 1)) : len;
		exact = (unsigned char *) malloc(avail ? avail : 1);
		memcpy(exact, scratch, avail);
		e = CHECK(exact, avail, NBLOCKS, &nweft);
		if (e == WEAVE_CD_OK)
		{
			verify_accepted(exact, avail, nweft);
			accepted++;
		}
		else
		{
			/* the code must be one we recognize; an unrecognized code means a
			 * new failure mode nobody wrote an errdetail for */
			const char *s = weave_chandesc_errstr(e);

			assert(strcmp(s, "unrecognized channel-descriptor error") != 0);
			rejected++;
		}
		free(exact);
		iters++;
	}

	/* 4. fully random bytes at random lengths, including lengths below the
	 * header size and above a page */
	for (trial = 0; trial < 400000; trial++)
	{
		size_t		avail = rnd() % (WEAVE_CD_SIZE(WEAVE_MAX_WEFTS) + 8);
		unsigned char *exact = (unsigned char *) malloc(avail ? avail : 1);
		uint16_t	nweft = 0;
		size_t		i;
		WeaveCdError e;

		for (i = 0; i < avail; i++)
			exact[i] = (unsigned char) rnd();
		/* half the time make the magic and version right, so the deeper checks
		 * are actually reached instead of bailing at the first byte */
		if (avail >= WEAVE_CD_HDRSIZE && (rnd() & 1))
		{
			WeaveCdPage *h = (WeaveCdPage *) exact;

			h->magic = WEAVE_CHANDESC_MAGIC;
			h->version = WEAVE_CHANDESC_VERSION;
			h->reserved = 0;
		}
		e = CHECK(exact, avail, NBLOCKS, &nweft);
		if (e == WEAVE_CD_OK)
		{
			verify_accepted(exact, avail, nweft);
			accepted++;
		}
		else
			rejected++;
		free(exact);
		iters++;
	}

	/* 5. nblocks == 0 disables the bounds half of the root check (the property
	 * test has no relation); it must still never overread */
	for (trial = 0; trial < 50000; trial++)
	{
		size_t		avail = rnd() % (WEAVE_CD_SIZE(WEAVE_MAX_WEFTS) + 8);
		unsigned char *exact = (unsigned char *) malloc(avail ? avail : 1);
		uint16_t	nweft = 0;
		size_t		i;

		for (i = 0; i < avail; i++)
			exact[i] = (unsigned char) rnd();
		(void) CHECK(exact, avail, 0, &nweft);
		free(exact);
		iters++;
	}

	printf("fuzz_chandesc: %lu cases, %lu accepted, %lu rejected -- no overread, no UB\n",
		   iters, accepted, rejected);
	return 0;
}
