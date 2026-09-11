/*
 * fuzz_surftrie.c -- corruption/fuzz harness for the SuRF vocabulary trie image
 * (include/weave/surftrie.h, src/query/surftrie.c), the Z3 on-disk structure.
 *
 * Every on-disk structure owes a fuzz target (doc/CONVENTIONS.md decision 2,
 * gate 8) and this one owes it twice over, because the image is nothing BUT
 * lengths and indices: `nslots` sizes four bitmaps and a label array, `nnodes`
 * sizes a select-sample array, every rank superblock is a count later used to
 * index a node, and every select sample IS a slot index that the reader
 * dereferences.  A single wrong number walks off the page.
 *
 * WHAT IS FUZZED IS THE REAL DECODER.  weave_surftrie_open() /
 * weave_surftrie_validate() have no PostgreSQL dependency for exactly this
 * reason, so unlike fuzz_block.c there is no transcription and no modeling gap:
 * this is the same code the AM will call.
 *
 * PROPERTY (default build, under ASan+UBSan): for ANY byte string of ANY length,
 * weave_surftrie_open() reads only inside [img, img+len), returns a recognized
 * error code, and when it returns WEAVE_SURF_OK the image's own postcondition
 * holds -- re-derived here BY BRUTE FORCE, without using the rank tables, the
 * select samples, or any of the validator's code.  That last part is the point:
 * a validator that returns OK on an image it should have rejected is a WRONG
 * ANSWER, and no sanitizer can see a wrong answer.  Then, on every accepted
 * image, point queries and a full prefix enumeration are run to completion,
 * which is where "no query can overread or fail to terminate after open()
 * succeeds" gets tested rather than asserted.
 *
 * Buffers are sized EXACTLY to the declared length, never to a whole 8 KB page,
 * so ASan's heap redzone sits immediately past the last readable byte.  This is
 * stricter than the backend, where the page really is BLCKSZ, and deliberately
 * so.
 *
 * TEETH.  Two planted-bug builds, and they are compile-time removals in the REAL
 * validator (-DWEAVE_SURF_PLANT_*), not a weakened transcription of it the way
 * fuzz_chandesc.c's weak_check() is.  The reason is the failure mode that
 * pattern has: a transcribed copy drifts out of step with the original and then
 * the teeth check proves something about a function nobody ships.  Both builds
 * MUST abort under ASan; run.sh enforces that.
 *
 *	 WEAVE_SURF_PLANT_NO_SIZE_GUARD	  drops "len must equal the size the counts
 *									  imply", so a corrupt nslots/nnodes/
 *									  nterminal puts a whole section past the
 *									  buffer.  The most tempting guard to drop:
 *									  the magic and version checks LOOK like they
 *									  already bound the image.
 *	 WEAVE_SURF_PLANT_NO_SELECT_GUARD drops the select-sample recomputation, so a
 *									  corrupt sample becomes a wild slot index and
 *									  st_select_louds() reads the louds bitmap
 *									  outside the image.
 *
 * No hegel/cmocka: deterministic PRNG loop, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/surftrie.h"

static uint64_t rngstate = 0x5A5FF00DDEADBEEFULL;

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

/* ---------------------------------------------------------------------------
 * Brute-force restatement of what an accepted image must satisfy
 *
 * Deliberately written with naive loops and no accelerator lookups: if this
 * agreed with the validator by construction it would prove nothing.
 * ------------------------------------------------------------------------- */

static uint32_t
rd32(const unsigned char *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
		((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int
bit(const unsigned char *bm, uint32_t i)
{
	return (bm[i >> 3] >> (i & 7)) & 1;
}

static uint32_t
naive_popcount(const unsigned char *bm, uint32_t nbits)
{
	uint32_t	i;
	uint32_t	r = 0;

	for (i = 0; i < nbits; i++)
		r += (uint32_t) bit(bm, i);
	return r;
}

static void
verify_accepted(const unsigned char *img, size_t len)
{
	uint32_t	nterms = rd32(img + WEAVE_ST_OFF_NTERMS);
	uint32_t	nslots = rd32(img + WEAVE_ST_OFF_NSLOTS);
	uint32_t	nnodes = rd32(img + WEAVE_ST_OFF_NNODES);
	uint32_t	nterminal = rd32(img + WEAVE_ST_OFF_NTERMINAL);
	uint32_t	ntrunc = rd32(img + WEAVE_ST_OFF_NTRUNC);
	uint32_t	bw;
	uint32_t	nsb;
	uint32_t	nsel;
	const unsigned char *labels;
	const unsigned char *haschild;
	const unsigned char *louds;
	const unsigned char *terminal;
	const unsigned char *trunc;
	const unsigned char *rank_hc;
	const unsigned char *rank_tm;
	const unsigned char *sel;
	const unsigned char *ords;
	size_t		expect;
	uint32_t	i;
	uint32_t	runhc = 0;
	uint32_t	runtm = 0;
	uint32_t	cnt = 0;
	int			innode = 0;
	unsigned char prevlabel = 0;

	if (nterms == 0)
	{
		assert(len == WEAVE_ST_HDRSIZE);
		assert(nslots == 0 && nnodes == 0 && nterminal == 0 && ntrunc == 0);
		return;
	}

	bw = 8 * ((nslots + 63) / 64);
	nsb = (nslots + 511) / 512;
	nsel = (nnodes + 63) / 64;
	expect = (size_t) WEAVE_ST_HDRSIZE + nslots + (size_t) 4 * bw +
		(size_t) 8 * nsb + (size_t) 4 * nsel + (size_t) 4 * nterminal;
	assert(len == expect);

	labels = img + WEAVE_ST_HDRSIZE;
	haschild = labels + nslots;
	louds = haschild + bw;
	terminal = louds + bw;
	trunc = terminal + bw;
	rank_hc = trunc + bw;
	rank_tm = rank_hc + 4 * (size_t) nsb;
	sel = rank_tm + 4 * (size_t) nsb;
	ords = sel + 4 * (size_t) nsel;

	/* counts */
	assert(nslots >= 1 && nnodes >= 1 && nnodes <= nslots);
	assert(nterminal >= 1 && nterminal <= nslots && nterminal <= nterms);
	assert(ntrunc <= nterminal);

	/* populations, by brute force */
	assert(naive_popcount(louds, nslots) == nnodes);
	assert(naive_popcount(haschild, nslots) == nnodes - 1);
	assert(naive_popcount(terminal, nslots) == nterminal);
	assert(naive_popcount(trunc, nslots) == ntrunc);
	assert(bit(louds, 0) == 1);

	/* canonical tails */
	for (i = nslots; i < bw * 8; i++)
	{
		assert(bit(haschild, i) == 0);
		assert(bit(louds, i) == 0);
		assert(bit(terminal, i) == 0);
		assert(bit(trunc, i) == 0);
	}

	/* a truncated slot is terminal; labels ascend inside a node */
	for (i = 0; i < nslots; i++)
	{
		if (bit(trunc, i))
			assert(bit(terminal, i));
		if (bit(louds, i))
			innode = 0;
		if (innode)
			assert(labels[i] > prevlabel);
		prevlabel = labels[i];
		innode = 1;
	}

	/* the accelerators really do index their bitmaps */
	for (i = 0; i < nsb; i++)
	{
		assert(rd32(rank_hc + 4 * (size_t) i) == runhc);
		assert(rd32(rank_tm + 4 * (size_t) i) == runtm);
		runhc = naive_popcount(haschild, (i + 1) * 512 < nslots
							   ? (i + 1) * 512 : nslots);
		runtm = naive_popcount(terminal, (i + 1) * 512 < nslots
							   ? (i + 1) * 512 : nslots);
	}
	for (i = 0; i < nslots; i++)
	{
		if (bit(louds, i))
		{
			if ((cnt & 63) == 0)
				assert(rd32(sel + 4 * (size_t) (cnt >> 6)) == i);
			cnt++;
		}
	}

	/*
	 * THE TERMINATION GUARANTEE: every child node starts after its parent slot,
	 * so slot indices strictly increase along any path.  The node starts are
	 * recovered by scanning the louds bitmap, NOT by reading the select samples,
	 * which is what makes this an independent restatement rather than a
	 * re-execution of the validator.
	 */
	{
		uint32_t   *starts = (uint32_t *) malloc((size_t) 4 * nnodes);
		uint32_t	nfound = 0;
		uint32_t	childidx = 0;

		for (i = 0; i < nslots; i++)
			if (bit(louds, i))
			{
				assert(nfound < nnodes);
				starts[nfound++] = i;
			}
		assert(nfound == nnodes);
		assert(starts[0] == 0);
		for (i = 0; i < nslots; i++)
		{
			if (!bit(haschild, i))
				continue;
			childidx++;
			assert(childidx < nnodes);
			assert(starts[childidx] > i);
		}
		assert(childidx == nnodes - 1);
		free(starts);
	}

	/* every ordinal indexes the dictionary */
	for (i = 0; i < nterminal; i++)
		assert(rd32(ords + 4 * (size_t) i) < nterms);
}

/* ---------------------------------------------------------------------------
 * A deterministic well-formed corpus
 * ------------------------------------------------------------------------- */

#define MAXTERMS	600
#define TERMBUF		320

typedef struct Corpus
{
	WeaveSurfTerm t[MAXTERMS];
	unsigned char store[MAXTERMS][TERMBUF];
	int			n;
} Corpus;

static int
cmp_term(const void *pa, const void *pb)
{
	const WeaveSurfTerm *a = (const WeaveSurfTerm *) pa;
	const WeaveSurfTerm *b = (const WeaveSurfTerm *) pb;
	uint32_t	m = a->len < b->len ? a->len : b->len;
	int			c = m ? memcmp(a->s, b->s, m) : 0;

	if (c != 0)
		return c;
	if (a->len == b->len)
		return 0;
	return a->len < b->len ? -1 : 1;
}

/*
 * Shapes chosen to move the geometry around: alphabet size changes the fanout,
 * `kind` 3 emits terms past WEAVE_SURFTRIE_MAX_DEPTH so the truncated-slot path
 * is in the corpus rather than only in the property test.
 */
static void
corpus_make(Corpus *c, int nwanted, int alpha, int kind)
{
	int			i;
	int			w = 0;

	if (nwanted > MAXTERMS)
		nwanted = MAXTERMS;
	for (i = 0; i < nwanted; i++)
	{
		int			len;
		int			k;

		switch (kind)
		{
			case 0:
				len = 1 + (int) (rnd() % 8);
				break;
			case 1:
				len = 1 + (int) (rnd() % 40);
				break;
			case 2:
				len = (int) WEAVE_SURFTRIE_MAX_DEPTH - (int) (rnd() % 3);
				break;
			default:
				len = (int) WEAVE_SURFTRIE_MAX_DEPTH + 1 + (int) (rnd() % 20);
				break;
		}
		if (len > TERMBUF)
			len = TERMBUF;
		for (k = 0; k < len; k++)
			c->store[i][k] = (unsigned char) (rnd() % (uint32_t) alpha);
		c->t[i].s = (const char *) c->store[i];
		c->t[i].len = (uint32_t) len;
	}
	qsort(c->t, (size_t) nwanted, sizeof(WeaveSurfTerm), cmp_term);
	for (i = 0; i < nwanted; i++)
	{
		if (w > 0 && cmp_term(&c->t[w - 1], &c->t[i]) == 0)
			continue;
		c->t[w++] = c->t[i];
	}
	c->n = w;
}

/* Exercise every query path on an accepted image.  On a corrupt-but-accepted
 * image the ANSWERS are meaningless; what is being tested is that nothing reads
 * outside the buffer and that every walk terminates. */
static int
enum_cb(void *arg, const char *term, weave_st_uint32 termlen,
		weave_st_uint32 ord, int exact)
{
	unsigned long *n = (unsigned long *) arg;

	/* touch the bytes so a bad pointer or length is a real access */
	if (termlen > 0)
	{
		volatile unsigned char sink = 0;
		weave_st_uint32 i;

		for (i = 0; i < termlen; i++)
			sink = (unsigned char) (sink ^ (unsigned char) term[i]);
		(void) sink;
	}
	(void) ord;
	(void) exact;
	(*n)++;
	return 0;
}

static void
exercise(const WeaveSurfTrie *t)
{
	unsigned long nhit = 0;
	int			i;

	for (i = 0; i < 24; i++)
	{
		unsigned char key[40];
		uint32_t	len = 1 + (rnd() % (uint32_t) sizeof(key));
		uint32_t	k;
		WeaveSurfHit hit;

		for (k = 0; k < len; k++)
			key[k] = (unsigned char) (rnd() % 8);
		(void) weave_surftrie_may_contain(t, key, len, &hit);
		(void) weave_surftrie_enumerate(t, key, 1 + (rnd() % len), enum_cb,
										&nhit, NULL);
	}
	/* the whole-vocabulary walk: the deepest recursion and the most emissions */
	(void) weave_surftrie_enumerate(t, NULL, 0, enum_cb, &nhit, NULL);
	(void) weave_surftrie_validate(t);
}

int
main(void)
{
	unsigned long iters = 0;
	unsigned long accepted = 0;
	unsigned long rejected = 0;
	static Corpus corpus;
	int			trial;

	/* 1. every well-formed image must be accepted, and its postcondition must
	 * hold when re-derived by brute force */
	for (trial = 0; trial < 240; trial++)
	{
		static const int alphas[] = {2, 3, 7, 64, 256};
		int			nwanted = (int) (rnd() % (MAXTERMS + 1));
		int			alpha = alphas[trial % 5];
		int			kind = trial % 4;
		size_t		want = 0;
		unsigned char *img;
		WeaveSurfTrie t;
		WeaveSurfError e;

		corpus_make(&corpus, nwanted, alpha, kind);
		e = weave_surftrie_size(corpus.t, (uint32_t) corpus.n, &want);
		if (e != WEAVE_SURF_OK)
		{
			fprintf(stderr, "size() failed on a well-formed corpus: %s\n",
					weave_surftrie_errstr(e));
			return 1;
		}
		img = (unsigned char *) malloc(want);	/* EXACT size: ASan redzone */
		e = weave_surftrie_build(corpus.t, (uint32_t) corpus.n, img, want, NULL);
		if (e != WEAVE_SURF_OK)
		{
			fprintf(stderr, "build() failed on a well-formed corpus: %s\n",
					weave_surftrie_errstr(e));
			return 1;
		}
		e = weave_surftrie_open(img, want, &t);
		if (e != WEAVE_SURF_OK)
		{
			fprintf(stderr, "open() rejected a built image (%d terms): %s\n",
					corpus.n, weave_surftrie_errstr(e));
			return 1;
		}
		if (weave_surftrie_validate(&t) != WEAVE_SURF_OK)
		{
			fprintf(stderr, "validate() rejected a built image (%d terms)\n",
					corpus.n);
			return 1;
		}
		verify_accepted(img, want);
		exercise(&t);
		free(img);
		iters++;
	}

	/* 2. a well-formed image truncated to every length must be rejected or
	 * accepted consistently, and must never overread */
	for (trial = 0; trial < 6; trial++)
	{
		size_t		want = 0;
		unsigned char *full;
		size_t		cut;

		corpus_make(&corpus, 3 + trial * 5, 4, trial % 3);
		if (weave_surftrie_size(corpus.t, (uint32_t) corpus.n, &want) != WEAVE_SURF_OK)
			continue;
		full = (unsigned char *) malloc(want);
		(void) weave_surftrie_build(corpus.t, (uint32_t) corpus.n, full, want, NULL);
		for (cut = 0; cut <= want; cut++)
		{
			unsigned char *exact = (unsigned char *) malloc(cut ? cut : 1);
			WeaveSurfTrie t;

			memcpy(exact, full, cut);
			if (weave_surftrie_open(exact, cut, &t) == WEAVE_SURF_OK)
			{
				verify_accepted(exact, cut);
				exercise(&t);
				accepted++;
			}
			else
				rejected++;
			free(exact);
			iters++;
		}
		free(full);
	}

	/* 3. random smashes of a well-formed image, with the DECLARED length
	 * corrupted independently of the buffer handed over (a torn blob length),
	 * plus a targeted mode that always hits one chosen section so the deep
	 * checks are reached instead of bailing in the header */
	{
		size_t		want = 0;
		unsigned char *base;

		corpus_make(&corpus, 300, 5, 1);
		if (weave_surftrie_size(corpus.t, (uint32_t) corpus.n, &want) == WEAVE_SURF_OK)
		{
			base = (unsigned char *) malloc(want);
			(void) weave_surftrie_build(corpus.t, (uint32_t) corpus.n, base, want, NULL);

			for (trial = 0; trial < 120000; trial++)
			{
				int			nsmash = 1 + (int) (rnd() % 5);
				size_t		avail;
				unsigned char *img;
				WeaveSurfTrie t;
				WeaveSurfError e;
				int			k;

				avail = ((rnd() % 4) == 0) ? (rnd() % (uint32_t) (want + 1)) : want;
				img = (unsigned char *) malloc(avail ? avail : 1);
				memcpy(img, base, avail);
				for (k = 0; k < nsmash && avail > 0; k++)
				{
					size_t		off;

					if ((rnd() & 1) && avail > WEAVE_ST_HDRSIZE)
						/* targeted: past the header, i.e. in a section */
						off = WEAVE_ST_HDRSIZE +
							(rnd() % (uint32_t) (avail - WEAVE_ST_HDRSIZE));
					else
						off = rnd() % (uint32_t) avail;
					img[off] = (unsigned char) rnd();
				}
				e = weave_surftrie_open(img, avail, &t);
				if (e == WEAVE_SURF_OK)
				{
					verify_accepted(img, avail);
					exercise(&t);
					accepted++;
				}
				else
				{
					/* an unrecognized code is a failure mode nobody wrote an
					 * errdetail for */
					assert(strcmp(weave_surftrie_errstr(e),
								  "unrecognized surf trie error") != 0);
					rejected++;
				}
				free(img);
				iters++;
			}
			free(base);
		}
	}

	/* 4. fully random bytes at random lengths, half of them with a plausible
	 * header so the deeper checks are actually reached */
	for (trial = 0; trial < 120000; trial++)
	{
		size_t		avail = rnd() % 4096;
		unsigned char *img = (unsigned char *) malloc(avail ? avail : 1);
		WeaveSurfTrie t;
		size_t		i;

		for (i = 0; i < avail; i++)
			img[i] = (unsigned char) rnd();
		if (avail >= WEAVE_ST_HDRSIZE && (rnd() & 1))
		{
			img[WEAVE_ST_OFF_MAGIC + 0] = 0x31;
			img[WEAVE_ST_OFF_MAGIC + 1] = 0x54;
			img[WEAVE_ST_OFF_MAGIC + 2] = 0x53;
			img[WEAVE_ST_OFF_MAGIC + 3] = 0x57;
			img[WEAVE_ST_OFF_VERSION] = WEAVE_SURFTRIE_VERSION;
			img[WEAVE_ST_OFF_VERSION + 1] = 0;
			img[WEAVE_ST_OFF_FLAGS] = 0;
			img[WEAVE_ST_OFF_FLAGS + 1] = 0;
			img[WEAVE_ST_OFF_RESERVED] = 0;
			img[WEAVE_ST_OFF_RESERVED + 1] = 0;
			/* keep the counts small enough that the size check can plausibly
			 * pass, otherwise every case dies at the first hurdle */
			img[WEAVE_ST_OFF_NSLOTS + 2] = 0;
			img[WEAVE_ST_OFF_NSLOTS + 3] = 0;
			img[WEAVE_ST_OFF_NNODES + 2] = 0;
			img[WEAVE_ST_OFF_NNODES + 3] = 0;
			img[WEAVE_ST_OFF_NTERMINAL + 2] = 0;
			img[WEAVE_ST_OFF_NTERMINAL + 3] = 0;
			img[WEAVE_ST_OFF_NTRUNC + 2] = 0;
			img[WEAVE_ST_OFF_NTRUNC + 3] = 0;
			img[WEAVE_ST_OFF_MAXDEPTH + 1] = 0;
		}
		if (weave_surftrie_open(img, avail, &t) == WEAVE_SURF_OK)
		{
			verify_accepted(img, avail);
			exercise(&t);
			accepted++;
		}
		else
			rejected++;
		free(img);
		iters++;
	}

	/* 5. the builder on hostile INPUT rather than hostile bytes: unsorted,
	 * duplicated and empty terms must be refused, not mis-indexed */
	for (trial = 0; trial < 20000; trial++)
	{
		WeaveSurfTerm t[8];
		unsigned char store[8][8];
		int			n = 1 + (int) (rnd() % 8);
		int			i;
		size_t		sz = 0;
		WeaveSurfError e;

		for (i = 0; i < n; i++)
		{
			int			len = (int) (rnd() % 5);	/* deliberately includes 0 */
			int			k;

			for (k = 0; k < len; k++)
				store[i][k] = (unsigned char) (rnd() % 3);
			t[i].s = (const char *) store[i];
			t[i].len = (uint32_t) len;
		}
		e = weave_surftrie_size(t, (uint32_t) n, &sz);
		if (e == WEAVE_SURF_OK)
		{
			unsigned char *img = (unsigned char *) malloc(sz);
			WeaveSurfTrie tr;

			if (weave_surftrie_build(t, (uint32_t) n, img, sz, NULL) != WEAVE_SURF_OK)
			{
				fprintf(stderr, "build() refused input size() accepted\n");
				return 1;
			}
			if (weave_surftrie_open(img, sz, &tr) != WEAVE_SURF_OK ||
				weave_surftrie_validate(&tr) != WEAVE_SURF_OK)
			{
				fprintf(stderr, "a built image failed validation\n");
				return 1;
			}
			verify_accepted(img, sz);
			free(img);
			accepted++;
		}
		else
		{
			assert(e == WEAVE_SURF_UNSORTED || e == WEAVE_SURF_EMPTY_TERM);
			rejected++;
		}
		iters++;
	}

	printf("fuzz_surftrie: %lu cases, %lu accepted, %lu rejected -- no overread, "
		   "no UB, no hang\n", iters, accepted, rejected);
	return 0;
}
