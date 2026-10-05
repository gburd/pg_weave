/*
 * fuzz_doclist.c -- the v12 document-list validator against hostile bytes
 *		(include/weave/doclist.h; doc/specs/SEGMENT_FORMAT.md sect. 6
 *		"The document list", sect. 8 item 3: every on-disk structure gets a
 *		fuzz target).
 *
 * Feeds weave_doclist_check() well-formed, truncated, single-field-corrupted,
 * random-byte and fully random images.  Each buffer is malloc'd at EXACTLY the
 * declared length so a one-byte overread is a hard ASan failure, and every
 * accepted image is independently re-verified: it decodes, ALL is strictly
 * ascending with ndocs members, NULL has nnull members each in ALL.
 *
 * Planted-bug control: -DFUZZ_NO_SUBSET_CHECK compiles a validator copy with
 * the subset rule removed, and this harness must then ABORT (exit nonzero) on
 * the "NULL outside ALL" family -- so a toothless harness fails instead of
 * passing vacuously (the fuzz_chandesc FUZZ_NO_ARRAY_GUARD pattern).
 *
 *	cc -O1 -g -fsanitize=address,undefined -I include \
 *	   -o /scratch/pg_weave/fdl test/fuzz/fuzz_doclist.c src/util/sparsemap.c
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/doclist.h"

static uint64_t rng = 0xD1B54A32D192ED03ull;

static uint64_t
rnd(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 2685821657736338717ull;
}

static long naccepted = 0,
			nrefused = 0;

static const char *
check(const uint8_t *img, size_t len)
{
#ifdef FUZZ_NO_SUBSET_CHECK
	/* the planted bug: accept as long as the header is sane and both sets
	 * have the declared cardinality, ignoring NULL subset-of ALL */
	WeaveDocListHeader h;
	const char *why = weave_doclist_check(img, len);

	if (why != NULL && len >= sizeof(h))
	{
		memcpy(&h, img, sizeof(h));
		if (strstr(why, "not a subset") != NULL)
			return NULL;
	}
	return why;
#else
	return weave_doclist_check(img, len);
#endif
}

/* Postcondition of an accepted image, re-derived independently. */
static void
verify_accepted(const uint8_t *img, size_t len)
{
	WeaveDocListHeader h;
	uint64_t   *a,
			   *n;
	uint64_t	i,
				j = 0;

	memcpy(&h, img, sizeof(h));
	if (h.ndocs == 0 || h.ndocs > (uint64_t) 1 << 24 || h.nnull > h.ndocs)
	{
		fprintf(stderr, "accepted an image with an impossible header\n");
		abort();
	}
	a = malloc(h.ndocs * sizeof(uint64_t));
	n = malloc((h.nnull ? h.nnull : 1) * sizeof(uint64_t));
	if (!weave_doclist_decode(img, len, a, n))
	{
		fprintf(stderr, "accepted an image that does not decode\n");
		abort();
	}
	for (i = 1; i < h.ndocs; i++)
		if (a[i - 1] >= a[i])
		{
			fprintf(stderr, "accepted ALL is not strictly ascending\n");
			abort();
		}
	for (i = 0; i < h.nnull; i++)
	{
		while (j < h.ndocs && a[j] < n[i])
			j++;
		if (j >= h.ndocs || a[j] != n[i])
		{
			fprintf(stderr, "accepted NULL set is not a subset of ALL\n");
			abort();
		}
	}
	free(a);
	free(n);
}

/* Run the validator over an EXACTLY-sized copy. */
static void
probe(const uint8_t *src, size_t len)
{
	uint8_t    *buf = malloc(len > 0 ? len : 1);

	memcpy(buf, src, len);
	if (check(buf, len) == NULL)
	{
		naccepted++;
		verify_accepted(buf, len);
	}
	else
		nrefused++;
	free(buf);
}

static size_t
gen_set(uint64_t *v, size_t cap)
{
	size_t		n = 1 + rnd() % cap;
	size_t		i;
	uint64_t	base = rnd() % 1000000;

	for (i = 0; i < n; i++)
		v[i] = (rnd() & 1) ? base + i * (1 + rnd() % 3) : rnd() % ((uint64_t) 1 << 36);
	return weave_doclist_sort_uniq(v, n);
}

#define CAP 1500

int
main(int argc, char **argv)
{
	int			iters = (argc > 1) ? atoi(argv[1]) : 2000;
	int			it;
	uint64_t   *all = malloc(CAP * sizeof(uint64_t));
	uint64_t   *nul = malloc(CAP * sizeof(uint64_t));

	for (it = 0; it < iters; it++)
	{
		size_t		n = gen_set(all, CAP);
		size_t		nn = 0,
					i,
					len;
		uint8_t    *img;

		for (i = 0; i < n; i++)
			if (rnd() % 7 == 0)
				nul[nn++] = all[i];
		img = weave_doclist_encode(all, n, nul, nn, (uint16_t) (it & 1), &len);
		assert(img != NULL);
		if (weave_doclist_check(img, len) != NULL)
		{
			fprintf(stderr, "a well-formed image was refused\n");
			abort();
		}
		probe(img, len);

		/* truncations and extensions */
		for (i = 0; i < 16; i++)
			probe(img, rnd() % len);
		{
			uint8_t    *ext = calloc(1, len + 8);

			memcpy(ext, img, len);
			probe(ext, len + 1 + rnd() % 8);
			free(ext);
		}

		/* single-field header corruption: every header byte */
		for (i = 0; i < WEAVE_DOCLIST_HDRSIZE; i++)
		{
			uint8_t    *bad = malloc(len);

			memcpy(bad, img, len);
			bad[i] ^= (uint8_t) (1 + rnd() % 255);
			probe(bad, len);
			free(bad);
		}

		/* random body bytes */
		for (i = 0; i < 32; i++)
		{
			uint8_t    *bad = malloc(len);
			int			k,
						nflip = 1 + (int) (rnd() % 4);

			memcpy(bad, img, len);
			for (k = 0; k < nflip; k++)
				bad[rnd() % len] = (uint8_t) rnd();
			probe(bad, len);
			free(bad);
		}

		/* the planted-bug family: a NULL set with a member outside ALL */
		{
			uint64_t	out = all[n - 1] + 1 + rnd() % 1000;
			uint8_t    *img2;
			size_t		len2;

			img2 = weave_doclist_encode(all, n, &out, 1, 0, &len2);
			assert(img2 != NULL);
			probe(img2, len2);
			free(img2);
		}
		free(img);

		/* fully random image with a plausible header */
		{
			size_t		rl = WEAVE_DOCLIST_HDRSIZE + rnd() % 512;
			uint8_t    *r = malloc(rl);
			WeaveDocListHeader h;

			for (i = 0; i < rl; i++)
				r[i] = (uint8_t) rnd();
			memset(&h, 0, sizeof(h));
			h.magic = WEAVE_DOCLIST_MAGIC;
			h.version = WEAVE_DOCLIST_VERSION;
			h.ndocs = 1 + rnd() % 100;
			h.alllen = (uint32_t) (rl - WEAVE_DOCLIST_HDRSIZE);
			memcpy(r, &h, sizeof(h));
			probe(r, rl);
			free(r);
		}
	}
	free(all);
	free(nul);
	printf("fuzz_doclist: %d iterations, %ld images accepted (each re-verified), %ld refused, 0 failures\n",
		   iters, naccepted, nrefused);
	return 0;
}
