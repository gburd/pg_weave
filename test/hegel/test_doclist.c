/*
 * test_doclist.c -- property test for the v12 per-bolt document list
 *		(include/weave/doclist.h; doc/specs/SEGMENT_FORMAT.md sect. 6
 *		"The document list"; doc/GAPS.md G77/G78/G80/G81).
 *
 * Properties, each over many random docid sets (sparse, dense, clustered, and
 * the tid-derived shape weave_tid_to_docid() produces: block*291 + offset):
 *
 *	P1  round trip: decode(encode(ALL, NULL)) == (ALL, NULL), exactly, and the
 *	    encoded image validates.
 *	P2  "list >= every posting docid": a list built as the union of a posting
 *	    population and a posting-less population (zero-term and NULL documents)
 *	    contains every posting docid -- the property bulkdelete and the NOT
 *	    universe rest on.  Checked by membership, not by count.
 *	P3  every single-byte corruption of a valid image is either REFUSED by the
 *	    validator or decodes to exactly the arrays the validator's own header
 *	    claims (no silent overrun, no out-of-header member).  Every truncation
 *	    is refused.
 *	P4  the validator refuses a NULL set that is not a subset of ALL.
 *
 * Fixed-seed xorshift64*, so a failure reproduces.  Build (no server needed):
 *	cc -O2 -Wall -Wextra -Wno-unused-parameter -I include \
 *	   -o /scratch/pg_weave/tdl test/hegel/test_doclist.c src/util/sparsemap.c
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/doclist.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t
rnd(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 2685821657736338717ull;
}

static long nchecks = 0;

#define CHECK(c) do { nchecks++; if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s (case %d)\n", __FILE__, __LINE__, #c, tc); \
	exit(1); } } while (0)

static size_t
gen(uint64_t *v, size_t cap, int shape)
{
	size_t		n = 1 + rnd() % cap;
	size_t		i;
	uint64_t	base = rnd() % 100000;

	for (i = 0; i < n; i++)
	{
		switch (shape)
		{
			case 0:				/* sparse, wide */
				v[i] = rnd() % ((uint64_t) 1 << 40);
				break;
			case 1:				/* dense run */
				v[i] = base + i;
				break;
			case 2:				/* clustered */
				v[i] = base + (rnd() % 4096);
				break;
			default:			/* tid-derived: block*291 + offset 1..60 */
				v[i] = (base + rnd() % 5000) * 291 + 1 + rnd() % 60;
				break;
		}
	}
	return weave_doclist_sort_uniq(v, n);
}

static bool
contains(const uint64_t *v, size_t n, uint64_t x)
{
	size_t		lo = 0,
				hi = n;

	while (lo < hi)
	{
		size_t		m = (lo + hi) / 2;

		if (v[m] < x)
			lo = m + 1;
		else
			hi = m;
	}
	return lo < n && v[lo] == x;
}

#define CAP 3000

int
main(int argc, char **argv)
{
	int			ncase = (argc > 1) ? atoi(argv[1]) : 3000;
	int			tc;
	uint64_t   *post = malloc(CAP * sizeof(uint64_t));
	uint64_t   *extra = malloc(CAP * sizeof(uint64_t));
	uint64_t   *all = malloc(2 * CAP * sizeof(uint64_t));
	uint64_t   *nul = malloc(2 * CAP * sizeof(uint64_t));
	uint64_t   *dall = malloc(2 * CAP * sizeof(uint64_t));
	uint64_t   *dnul = malloc(2 * CAP * sizeof(uint64_t));
	long		refused = 0,
				accepted = 0;

	for (tc = 0; tc < ncase; tc++)
	{
		size_t		np = gen(post, CAP, tc % 4);
		size_t		ne = (tc % 5 == 0) ? 0 : gen(extra, CAP / 4, (tc / 4) % 4);
		size_t		n,
					nn = 0,
					i,
					len;
		uint8_t    *img;
		WeaveDocListHeader h;

		/* ALL = postings U posting-less docs; NULL = a random subset of the
		 * posting-less ones (a NULL document never has a posting) */
		memcpy(all, post, np * sizeof(uint64_t));
		memcpy(all + np, extra, ne * sizeof(uint64_t));
		n = weave_doclist_sort_uniq(all, np + ne);
		for (i = 0; i < ne; i++)
			if (!contains(post, np, extra[i]) && (rnd() & 1))
				nul[nn++] = extra[i];
		nn = weave_doclist_sort_uniq(nul, nn);

		img = weave_doclist_encode(all, n, nul, nn,
								   (tc & 1) ? WEAVE_DOCLIST_F_COMPLETE : 0, &len);
		CHECK(img != NULL);
		CHECK(weave_doclist_check(img, len) == NULL);

		/* P1 */
		CHECK(weave_doclist_decode(img, len, dall, dnul));
		memcpy(&h, img, sizeof(h));
		CHECK(h.ndocs == n && h.nnull == nn);
		CHECK(memcmp(dall, all, n * sizeof(uint64_t)) == 0);
		CHECK(nn == 0 || memcmp(dnul, nul, nn * sizeof(uint64_t)) == 0);
		CHECK(((h.flags & WEAVE_DOCLIST_F_COMPLETE) != 0) == ((tc & 1) != 0));

		/* P2: every posting docid is in the decoded list */
		for (i = 0; i < np; i++)
			CHECK(contains(dall, n, post[i]));

		/* P3: truncations are refused */
		for (i = 0; i < len; i += 1 + len / 64)
			CHECK(weave_doclist_check(img, i) != NULL);

		/* P3: single-byte corruptions are refused or decode self-consistently */
		{
			int			k;

			for (k = 0; k < 24; k++)
			{
				uint8_t    *bad = malloc(len);
				size_t		off = rnd() % len;

				memcpy(bad, img, len);
				bad[off] ^= (uint8_t) (1 + rnd() % 255);
				if (weave_doclist_check(bad, len) != NULL)
					refused++;
				else
				{
					WeaveDocListHeader bh;
					uint64_t   *ba,
							   *bn;

					accepted++;
					memcpy(&bh, bad, sizeof(bh));
					CHECK(bh.ndocs >= 1 && bh.nnull <= bh.ndocs);
					ba = malloc(bh.ndocs * sizeof(uint64_t));
					bn = malloc((bh.nnull ? bh.nnull : 1) * sizeof(uint64_t));
					CHECK(weave_doclist_decode(bad, len, ba, bn));
					for (i = 1; i < bh.ndocs; i++)
						CHECK(ba[i - 1] < ba[i]);
					for (i = 0; i < bh.nnull; i++)
						CHECK(contains(ba, bh.ndocs, bn[i]));
					free(ba);
					free(bn);
				}
				free(bad);
			}
		}

		/* P4: a NULL set outside ALL is refused */
		if (n >= 1)
		{
			uint64_t	outside = all[n - 1] + 1;
			uint8_t    *img2;
			size_t		len2;

			img2 = weave_doclist_encode(all, n, &outside, 1, 0, &len2);
			CHECK(img2 != NULL);
			CHECK(weave_doclist_check(img2, len2) != NULL);
			free(img2);
		}
		free(img);
	}
	printf("test_doclist: %d cases, %ld checks, %ld corruptions refused, %ld accepted self-consistently, 0 failures\n",
		   ncase, nchecks, refused, accepted);
	free(post);
	free(extra);
	free(all);
	free(nul);
	free(dall);
	free(dnul);
	return 0;
}
