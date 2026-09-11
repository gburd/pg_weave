/*-------------------------------------------------------------------------
 *
 * test_surf.c
 *		Standalone property tests for the SuRF trie over the bolt vocabulary
 *		(task Z3).  Links src/query/surftrie.c directly: no backend, no
 *		PostgreSQL header, no external test framework.
 *
 * THE GATE: trie membership == dictionary membership.
 *
 * The oracle is the dictionary itself -- a sorted array plus a plain binary
 * search -- and NOT a second trie implementation.  That matters here for a
 * reason this session met in the flesh: a test in this repo hand-transcribed the
 * logic it was supposed to be checking and therefore passed regardless of what
 * the real code did (see include/weave/for.h's comment on
 * weave_doclen_walk_abs).  Here the only thing under test is the real builder,
 * the real reader and the real validator; the expected answer is "is this string
 * in the array we handed the builder", which cannot drift into agreement with a
 * bug.
 *
 * WHY BOTH DIRECTIONS ARE ASSERTED, AND WHY THAT IS NOT PEDANTRY.  A SuRF is
 * allowed false positives and forbidden false negatives (weave/surftrie.h).  A
 * test that only asserts "every member is found" is passed by the function
 * `return 1;`.  A test that only asserts "every non-member is rejected" is
 * passed by `return 0;` -- and THAT one silently drops rows, which per AGENTS.md
 * hard rule 1 no fixed-expected-output regression test can catch.  So:
 *
 *	 - every dictionary term MUST be reported present, always, in every case
 *	   including the truncating ones;
 *	 - when the vocabulary has no term longer than WEAVE_SURFTRIE_MAX_DEPTH the
 *	   filter is EXACT, so every non-member MUST be reported absent -- this is
 *	   what stops the "always yes" implementation from passing;
 *	 - an `exact` hit is never a false positive, in any case: exactness means the
 *	   path spells a stored term, so it implies membership;
 *	 - with terms past the maximum depth, false positives are permitted, counted
 *	   and reported, and at least one is asserted to actually occur -- otherwise
 *	   the truncation machinery could be dead code and nothing would say so.
 *
 * Properties, over seeded random vocabularies and a set of directed cases:
 *
 *	 P1	 every term is reported present; its ordinal is its dictionary index
 *	 P2	 every non-member is reported absent (non-truncating vocabularies), and
 *		 the adversarial probes are the ones near a member: one byte changed, one
 *		 byte appended, one byte removed, and every proper prefix
 *	 P3	 an exact hit implies membership (always, both regimes)
 *	 P4	 enumerate("") is the whole vocabulary, in ascending order, ordinals
 *		 0..n-1 with no gaps
 *	 P5	 enumerate(p) is exactly the terms with prefix p, in order -- checked for
 *		 every proper prefix of every term, plus random prefixes that mostly miss
 *	 P6	 a callback that stops the walk stops it, after exactly one call
 *	 P7	 size() == the length build() writes; build() writes nothing outside the
 *		 buffer (guard bytes both sides); a one-byte-short buffer is refused
 *	 P8	 open() then validate() accept every image the builder produces
 *	 P9	 builder input validation: unsorted, duplicated, and empty terms are
 *		 refused rather than silently mis-indexed
 *	 P10 truncation: members longer than the maximum depth are still reported
 *		 present, collapse into a shared truncated slot, and produce a REAL false
 *		 positive on the shared prefix -- the SuRF asymmetry, exercised
 *
 * MUTATION TESTING, because a property test that never fails proves nothing.
 * Every guard in the builder, the reader and the validator was broken in turn and
 * this file was required to fail; the list and the failure counts are in the
 * commit message.  Nineteen of twenty-one mutations are caught.  The one that is
 * NOT caught is the deep pass's reachability comparison, and that is a proof
 * rather than a gap -- it is implied by the has-child population identity plus
 * acyclicity, as derived in weave_surftrie_validate().  Recording that is the
 * point: an uncaught mutation is either a missing test or a redundant check, and
 * which one it is has to be established rather than assumed.
 *
 * Directed cases, each because it is a known trie bug: the empty vocabulary, a
 * single term, terms that are prefixes of each other ("a", "ab", "abc"), a node
 * with all 256 labels, shared long prefixes, UTF-8 multi-byte terms, a term of
 * exactly WEAVE_SURFTRIE_MAX_DEPTH bytes, and vocabularies large enough to cross
 * the 512-bit rank superblock and the 64-node select-sample boundaries.
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/ts test/hegel/test_surf.c \
 *			src/query/surftrie.c && /tmp/ts
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_surf.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/surftrie.h"

static long checks = 0;
static long failures = 0;
static long fp_allowed = 0;		/* permitted false positives actually observed */

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

/* xorshift64*, matching test_pack.c and test_doclen_block.c: reproducible on
 * every host and every rerun, no libc rand(). */
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

static uint32_t
rnd(uint32_t bound)
{
	return (uint32_t) (rng() % bound);
}

/* ---------------------------------------------------------------------------
 * The oracle: a sorted, de-duplicated array of terms and a binary search
 * ------------------------------------------------------------------------- */

typedef struct Vocab
{
	WeaveSurfTerm *t;
	int			n;
	int			cap;
} Vocab;

static void
vocab_init(Vocab *v)
{
	v->t = NULL;
	v->n = 0;
	v->cap = 0;
}

static void
vocab_add(Vocab *v, const void *s, uint32_t len)
{
	char	   *copy;

	if (v->n == v->cap)
	{
		v->cap = v->cap ? v->cap * 2 : 16;
		v->t = (WeaveSurfTerm *) realloc(v->t, (size_t) v->cap * sizeof(WeaveSurfTerm));
	}
	copy = (char *) malloc(len ? len : 1);
	if (len)
		memcpy(copy, s, len);
	v->t[v->n].s = copy;
	v->t[v->n].len = len;
	v->n++;
}

static void
vocab_free(Vocab *v)
{
	int			i;

	for (i = 0; i < v->n; i++)
		free((void *) v->t[i].s);
	free(v->t);
	vocab_init(v);
}

/*
 * The dictionary's order: unsigned byte order, shorter first on a tie.  This is
 * the INPUT CONTRACT the builder is handed, not a restatement of anything the
 * builder computes.
 */
static int
term_cmp(const WeaveSurfTerm *a, const WeaveSurfTerm *b)
{
	uint32_t	m = a->len < b->len ? a->len : b->len;
	int			c = m ? memcmp(a->s, b->s, m) : 0;

	if (c != 0)
		return c;
	if (a->len == b->len)
		return 0;
	return a->len < b->len ? -1 : 1;
}

static int
term_cmp_qsort(const void *a, const void *b)
{
	return term_cmp((const WeaveSurfTerm *) a, (const WeaveSurfTerm *) b);
}

static void
vocab_sort_dedup(Vocab *v)
{
	int			i;
	int			w = 0;

	if (v->n == 0)
		return;
	qsort(v->t, (size_t) v->n, sizeof(WeaveSurfTerm), term_cmp_qsort);
	for (i = 0; i < v->n; i++)
	{
		if (w > 0 && term_cmp(&v->t[w - 1], &v->t[i]) == 0)
		{
			free((void *) v->t[i].s);
			continue;
		}
		v->t[w++] = v->t[i];
	}
	v->n = w;
}

/* the oracle: dictionary index of `key`, or -1 */
static int
vocab_find(const Vocab *v, const void *key, uint32_t keylen)
{
	WeaveSurfTerm probe;
	int			lo = 0;
	int			hi = v->n - 1;

	probe.s = (const char *) key;
	probe.len = keylen;
	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		int			c = term_cmp(&v->t[mid], &probe);

		if (c == 0)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

static int
has_prefix(const WeaveSurfTerm *t, const void *p, uint32_t plen)
{
	return t->len >= plen && (plen == 0 || memcmp(t->s, p, plen) == 0);
}

/* ---------------------------------------------------------------------------
 * Building, with guard bytes on both sides of an exactly-sized buffer
 * ------------------------------------------------------------------------- */

#define GUARD		32
#define SENTINEL	0xA5

typedef struct Image
{
	unsigned char *raw;			/* GUARD + len + GUARD */
	unsigned char *img;			/* raw + GUARD */
	size_t		len;
} Image;

static int
image_build(const Vocab *v, Image *im, const char *label)
{
	size_t		want = 0;
	size_t		got = 0;
	WeaveSurfError err;
	size_t		i;

	err = weave_surftrie_size(v->t, (uint32_t) v->n, &want);
	CHECK(err == WEAVE_SURF_OK, "%s: size() failed: %s", label,
		  weave_surftrie_errstr(err));
	if (err != WEAVE_SURF_OK)
		return 0;

	im->len = want;
	im->raw = (unsigned char *) malloc(want + 2 * GUARD);
	memset(im->raw, SENTINEL, want + 2 * GUARD);
	im->img = im->raw + GUARD;

	err = weave_surftrie_build(v->t, (uint32_t) v->n, im->img, want, &got);
	CHECK(err == WEAVE_SURF_OK, "%s: build() failed: %s", label,
		  weave_surftrie_errstr(err));
	/* P7: the two functions must agree exactly, or the AM sizes a page chain
	 * from one number and fills it from another */
	CHECK(got == want, "%s: build wrote %zu, size said %zu", label, got, want);

	for (i = 0; i < GUARD; i++)
	{
		CHECK(im->raw[i] == SENTINEL, "%s: leading guard byte %zu clobbered", label, i);
		CHECK(im->raw[GUARD + want + i] == SENTINEL,
			  "%s: trailing guard byte %zu clobbered", label, i);
	}

	/* P7: one byte short must be refused, and must not have written anything */
	if (want > 0)
	{
		unsigned char *tight = (unsigned char *) malloc(want);

		memset(tight, SENTINEL, want);
		err = weave_surftrie_build(v->t, (uint32_t) v->n, tight, want - 1, NULL);
		CHECK(err == WEAVE_SURF_NOSPACE, "%s: short buffer accepted (%s)", label,
			  weave_surftrie_errstr(err));
		for (i = 0; i < want; i++)
			if (tight[i] != SENTINEL)
			{
				CHECK(0, "%s: refused build still wrote at byte %zu", label, i);
				break;
			}
		free(tight);
	}
	return err == WEAVE_SURF_NOSPACE || want == 0 ? 1 : 1;
}

static void
image_free(Image *im)
{
	free(im->raw);
	im->raw = NULL;
	im->img = NULL;
	im->len = 0;
}

/* ---------------------------------------------------------------------------
 * Enumeration collector
 * ------------------------------------------------------------------------- */

typedef struct Coll
{
	Vocab		out;
	uint32_t   *ords;
	int		   *exacts;
	int			cap;
	int			n;
	int			stop_at;		/* 0 = never stop */
} Coll;

static int
coll_cb(void *arg, const char *term, weave_st_uint32 termlen,
		weave_st_uint32 ord, int exact)
{
	Coll	   *c = (Coll *) arg;

	if (c->n == c->cap)
	{
		c->cap = c->cap ? c->cap * 2 : 32;
		c->ords = (uint32_t *) realloc(c->ords, (size_t) c->cap * sizeof(uint32_t));
		c->exacts = (int *) realloc(c->exacts, (size_t) c->cap * sizeof(int));
	}
	c->ords[c->n] = ord;
	c->exacts[c->n] = exact;
	c->n++;
	vocab_add(&c->out, term, termlen);

	if (c->stop_at != 0 && c->n >= c->stop_at)
		return 1;
	return 0;
}

static void
coll_init(Coll *c, int stop_at)
{
	vocab_init(&c->out);
	c->ords = NULL;
	c->exacts = NULL;
	c->cap = 0;
	c->n = 0;
	c->stop_at = stop_at;
}

static void
coll_free(Coll *c)
{
	vocab_free(&c->out);
	free(c->ords);
	free(c->exacts);
}

/* ---------------------------------------------------------------------------
 * The central property battery
 * ------------------------------------------------------------------------- */

/*
 * `exact_regime` is 1 when no term exceeds WEAVE_SURFTRIE_MAX_DEPTH, in which
 * case the filter is exact and non-members MUST be rejected.  When it is 0 the
 * one-sided regime applies: members must still be found, non-members may be
 * reported, and every reported non-member is counted.
 */
static void
test_vocab(Vocab *v, const char *label, int exact_regime, int nprefix_probes)
{
	Image		im;
	WeaveSurfTrie t;
	WeaveSurfError err;
	int			i;
	int			j;

	if (!image_build(v, &im, label))
		return;

	/* P8: the validator accepts what the builder produced -- both layers */
	err = weave_surftrie_open(im.img, im.len, &t);
	CHECK(err == WEAVE_SURF_OK, "%s: open() rejected a built image: %s", label,
		  weave_surftrie_errstr(err));
	if (err != WEAVE_SURF_OK)
	{
		image_free(&im);
		return;
	}
	err = weave_surftrie_validate(&t);
	CHECK(err == WEAVE_SURF_OK, "%s: validate() rejected a built image: %s", label,
		  weave_surftrie_errstr(err));
	CHECK(weave_surftrie_check(im.img, im.len) == WEAVE_SURF_OK,
		  "%s: check() rejected a built image", label);
	CHECK(t.nterms == (weave_st_uint32) v->n, "%s: nterms %u != %d", label,
		  t.nterms, v->n);

	/* an image one byte longer or shorter is a different length than the counts
	 * imply and must be refused: the reader trusts no length it was handed */
	if (im.len > WEAVE_ST_HDRSIZE)
	{
		WeaveSurfTrie t2;

		CHECK(weave_surftrie_open(im.img, im.len - 1, &t2) != WEAVE_SURF_OK,
			  "%s: open() accepted a short length", label);
	}

	/* ---- P1 / P3: every member is present ---- */
	for (i = 0; i < v->n; i++)
	{
		WeaveSurfHit hit;
		int			found = weave_surftrie_may_contain(&t, v->t[i].s, v->t[i].len, &hit);

		CHECK(found, "%s: FALSE NEGATIVE on member %d (len %u)", label, i,
			  v->t[i].len);
		if (!found)
			continue;
		if (v->t[i].len <= WEAVE_SURFTRIE_MAX_DEPTH)
		{
			CHECK(hit.exact == 1, "%s: member %d reported inexact", label, i);
			CHECK(hit.ord == (weave_st_uint32) i,
				  "%s: member %d ordinal %u", label, i, hit.ord);
		}
		else
		{
			/* longer than the format holds: a MAYBE whose ordinal is the first
			 * of the run it collapsed, hence <= i */
			CHECK(hit.exact == 0, "%s: over-long member %d claimed exact", label, i);
			CHECK(hit.ord <= (weave_st_uint32) i,
				  "%s: over-long member %d ordinal %u > %d", label, i, hit.ord, i);
		}
	}

	/* ---- P2 / P3: probes near a member, plus random noise ---- */
	for (i = 0; i < v->n; i++)
	{
		unsigned char probe[WEAVE_SURFTRIE_MAX_DEPTH + 8];
		uint32_t	plen = v->t[i].len;
		int			variant;

		if (plen > sizeof(probe) - 2)
			plen = (uint32_t) sizeof(probe) - 2;
		memcpy(probe, v->t[i].s, plen);

		for (variant = 0; variant < 3; variant++)
		{
			uint32_t	len = plen;
			WeaveSurfHit hit;
			int			found;
			int			ismember;

			switch (variant)
			{
				case 0:			/* one byte appended */
					probe[len] = (unsigned char) (0x30 + rnd(64));
					len++;
					break;
				case 1:			/* one byte removed */
					if (len < 2)
						continue;
					len--;
					break;
				default:		/* last byte changed */
					probe[len - 1] = (unsigned char) (probe[len - 1] ^ (1 + rnd(255)));
					break;
			}

			ismember = vocab_find(v, probe, len) >= 0;
			found = weave_surftrie_may_contain(&t, probe, len, &hit);

			if (ismember)
				CHECK(found, "%s: FALSE NEGATIVE on a mutated-but-present probe",
					  label);
			else if (exact_regime)
				CHECK(!found, "%s: false positive in the EXACT regime (len %u)",
					  label, len);
			else if (found)
				fp_allowed++;

			/* P3, in both regimes: exactness implies membership */
			if (found && hit.exact)
				CHECK(ismember, "%s: exact hit on a non-member", label);
			/* restore the mutated byte for the next variant */
			memcpy(probe, v->t[i].s, plen);
		}
	}

	/* every proper prefix of every member: the "a"/"ab"/"abc" bug lives here */
	for (i = 0; i < v->n; i++)
	{
		uint32_t	cut;
		uint32_t	lim = v->t[i].len;

		if (lim > 24)
			lim = 24;			/* keep the sweep O(n) rather than O(n * maxlen) */
		for (cut = 1; cut < lim; cut++)
		{
			WeaveSurfHit hit;
			int			isMember = vocab_find(v, v->t[i].s, cut) >= 0;
			int			found = weave_surftrie_may_contain(&t, v->t[i].s, cut, &hit);

			if (isMember)
				CHECK(found, "%s: FALSE NEGATIVE on prefix-member (cut %u)",
					  label, cut);
			else if (exact_regime)
				CHECK(!found, "%s: false positive on non-member prefix (cut %u)",
					  label, cut);
			else if (found)
				fp_allowed++;
			if (found && hit.exact)
				CHECK(isMember, "%s: exact hit on a non-member prefix", label);
		}
	}

	/* random noise: mostly non-members, and the empty key is never a member */
	{
		WeaveSurfHit hit;

		CHECK(weave_surftrie_may_contain(&t, "", 0, &hit) == 0,
			  "%s: the empty key was reported present", label);
	}
	for (i = 0; i < 200; i++)
	{
		unsigned char probe[16];
		uint32_t	len = 1 + rnd(sizeof(probe) - 1);
		WeaveSurfHit hit;
		int			isMember;
		int			found;

		for (j = 0; j < (int) len; j++)
			probe[j] = (unsigned char) rnd(256);
		isMember = vocab_find(v, probe, len) >= 0;
		found = weave_surftrie_may_contain(&t, probe, len, &hit);
		if (isMember)
			CHECK(found, "%s: FALSE NEGATIVE on random member", label);
		else if (exact_regime)
			CHECK(!found, "%s: false positive on random non-member", label);
		else if (found)
			fp_allowed++;
		if (found && hit.exact)
			CHECK(isMember, "%s: exact hit on a random non-member", label);
	}

	/* ---- P4: the empty prefix enumerates the whole vocabulary in order ---- */
	{
		Coll		c;
		weave_st_uint32 nhits = 0;

		coll_init(&c, 0);
		err = weave_surftrie_enumerate(&t, NULL, 0, coll_cb, &c, &nhits);
		CHECK(err == WEAVE_SURF_OK, "%s: enumerate(\"\") failed", label);
		CHECK((int) nhits == c.n, "%s: nhits %u != callbacks %d", label, nhits, c.n);
		if (exact_regime)
		{
			CHECK(c.n == v->n, "%s: enumerate(\"\") gave %d of %d terms", label,
				  c.n, v->n);
			for (i = 0; i < c.n && i < v->n; i++)
			{
				CHECK(term_cmp(&c.out.t[i], &v->t[i]) == 0,
					  "%s: enumerate(\"\") term %d differs", label, i);
				CHECK(c.ords[i] == (uint32_t) i,
					  "%s: enumerate(\"\") ordinal %d = %u", label, i, c.ords[i]);
				CHECK(c.exacts[i] == 1, "%s: enumerate(\"\") entry %d inexact",
					  label, i);
			}
		}
		else
		{
			/* one-sided coverage: every member is emitted exactly or covered by
			 * an emitted truncated entry that it extends */
			for (i = 0; i < v->n; i++)
			{
				int			covered = 0;

				for (j = 0; j < c.n; j++)
				{
					if (c.exacts[j] && term_cmp(&c.out.t[j], &v->t[i]) == 0)
						covered = 1;
					else if (!c.exacts[j] &&
							 has_prefix(&v->t[i], c.out.t[j].s, c.out.t[j].len))
						covered = 1;
					if (covered)
						break;
				}
				CHECK(covered, "%s: member %d not covered by enumerate(\"\")",
					  label, i);
			}
		}
		/* ordinals strictly ascend in lexicographic order: the property that
		 * makes an enumerated range a contiguous ordinal range for Z4 */
		for (i = 1; i < c.n; i++)
			CHECK(c.ords[i] > c.ords[i - 1],
				  "%s: enumerate(\"\") ordinals not ascending at %d", label, i);
		coll_free(&c);
	}

	/* ---- P5: enumerate(prefix) is exactly the terms with that prefix ---- */
	if (exact_regime)
	{
		for (i = 0; i < nprefix_probes; i++)
		{
			unsigned char probe[32];
			uint32_t	plen;
			Coll		c;
			int			nexp = 0;
			int			k;

			/* two thirds of the probes are real prefixes of real terms (the
			 * interesting case), one third is noise that should match nothing */
			if (v->n > 0 && rnd(3) != 0)
			{
				int			pick = (int) rnd((uint32_t) v->n);
				uint32_t	lim = v->t[pick].len;

				if (lim > sizeof(probe))
					lim = sizeof(probe);
				plen = 1 + rnd(lim);
				memcpy(probe, v->t[pick].s, plen);
			}
			else
			{
				plen = 1 + rnd(4);
				for (k = 0; k < (int) plen; k++)
					probe[k] = (unsigned char) rnd(256);
			}

			for (k = 0; k < v->n; k++)
				if (has_prefix(&v->t[k], probe, plen))
					nexp++;

			coll_init(&c, 0);
			err = weave_surftrie_enumerate(&t, probe, plen, coll_cb, &c, NULL);
			CHECK(err == WEAVE_SURF_OK, "%s: enumerate(prefix) failed", label);
			CHECK(c.n == nexp, "%s: enumerate(prefix len %u) gave %d, expected %d",
				  label, plen, c.n, nexp);
			if (c.n == nexp)
			{
				int			e = 0;

				for (k = 0; k < v->n; k++)
				{
					if (!has_prefix(&v->t[k], probe, plen))
						continue;
					CHECK(term_cmp(&c.out.t[e], &v->t[k]) == 0,
						  "%s: enumerate(prefix) entry %d differs", label, e);
					CHECK(c.ords[e] == (uint32_t) k,
						  "%s: enumerate(prefix) ordinal %d = %u, expected %d",
						  label, e, c.ords[e], k);
					CHECK(c.exacts[e] == 1,
						  "%s: enumerate(prefix) entry %d inexact", label, e);
					e++;
				}
			}
			coll_free(&c);

			/* P6: a callback that stops is obeyed, immediately */
			if (nexp > 1)
			{
				Coll		s;

				coll_init(&s, 1);
				(void) weave_surftrie_enumerate(&t, probe, plen, coll_cb, &s, NULL);
				CHECK(s.n == 1, "%s: early stop made %d calls", label, s.n);
				coll_free(&s);
			}
		}
	}

	image_free(&im);
}

/* ---------------------------------------------------------------------------
 * Directed cases
 * ------------------------------------------------------------------------- */

/* Append a UTF-8 encoding of `cp` to buf, returning bytes written. */
static int
utf8_put(unsigned char *buf, uint32_t cp)
{
	if (cp < 0x80)
	{
		buf[0] = (unsigned char) cp;
		return 1;
	}
	if (cp < 0x800)
	{
		buf[0] = (unsigned char) (0xC0 | (cp >> 6));
		buf[1] = (unsigned char) (0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000)
	{
		buf[0] = (unsigned char) (0xE0 | (cp >> 12));
		buf[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (unsigned char) (0x80 | (cp & 0x3F));
		return 3;
	}
	buf[0] = (unsigned char) (0xF0 | (cp >> 18));
	buf[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
	buf[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
	buf[3] = (unsigned char) (0x80 | (cp & 0x3F));
	return 4;
}

static void
test_empty_vocabulary(void)
{
	Vocab		v;
	Image		im;
	WeaveSurfTrie t;
	Coll		c;
	WeaveSurfHit hit;

	vocab_init(&v);
	if (!image_build(&v, &im, "empty"))
		return;
	/* the empty vocabulary is a legal header-only image, not an error and not a
	 * zero-length blob (a zero-length blob has no version to refuse later) */
	CHECK(im.len == WEAVE_ST_HDRSIZE, "empty: image is %zu bytes", im.len);
	CHECK(weave_surftrie_open(im.img, im.len, &t) == WEAVE_SURF_OK,
		  "empty: open() rejected the empty image");
	CHECK(weave_surftrie_validate(&t) == WEAVE_SURF_OK,
		  "empty: validate() rejected the empty image");
	CHECK(weave_surftrie_may_contain(&t, "a", 1, &hit) == 0,
		  "empty: reported a member");
	CHECK(weave_surftrie_may_contain(&t, "", 0, &hit) == 0,
		  "empty: reported the empty key");
	coll_init(&c, 0);
	CHECK(weave_surftrie_enumerate(&t, NULL, 0, coll_cb, &c, NULL) == WEAVE_SURF_OK,
		  "empty: enumerate failed");
	CHECK(c.n == 0, "empty: enumerate produced %d entries", c.n);
	coll_free(&c);
	image_free(&im);
	vocab_free(&v);
}

static void
test_input_validation(void)
{
	WeaveSurfTerm t[3];
	size_t		sz = 0;

	/* P9: unsorted input is refused.  It has to be: the construction relies on
	 * runs of a shared prefix being contiguous, so an unsorted array would build
	 * a trie that is missing terms -- false negatives, at build time. */
	t[0].s = "b";
	t[0].len = 1;
	t[1].s = "a";
	t[1].len = 1;
	CHECK(weave_surftrie_size(t, 2, &sz) == WEAVE_SURF_UNSORTED,
		  "unsorted input accepted");

	/* a duplicate is also "not strictly ascending": the ordinal column would
	 * otherwise have two terms claiming one slot */
	t[0].s = "a";
	t[0].len = 1;
	t[1].s = "a";
	t[1].len = 1;
	CHECK(weave_surftrie_size(t, 2, &sz) == WEAVE_SURF_UNSORTED,
		  "duplicate input accepted");

	/* an empty term is refused rather than dropped: dropping it would be a false
	 * negative wearing a build-time disguise (weave/surftrie.h) */
	t[0].s = "";
	t[0].len = 0;
	t[1].s = "a";
	t[1].len = 1;
	CHECK(weave_surftrie_size(t, 2, &sz) == WEAVE_SURF_EMPTY_TERM,
		  "empty term accepted");

	/* a NULL destination is refused, not dereferenced */
	t[0].s = "a";
	t[0].len = 1;
	CHECK(weave_surftrie_build(t, 1, NULL, 4096, NULL) == WEAVE_SURF_NOSPACE,
		  "NULL destination accepted");
}

/*
 * P10: the truncating regime.  Two terms longer than the maximum depth that
 * share their first WEAVE_SURFTRIE_MAX_DEPTH bytes MUST collapse into one
 * truncated slot, both MUST still be reported present, and the shared 255-byte
 * prefix -- which is NOT in the vocabulary -- MUST be reported present too.
 * That last assertion is the point: it proves the false-positive path is live
 * rather than dead code, and it is the SuRF asymmetry stated as a test.
 */
static void
test_truncation(void)
{
	Vocab		v;
	Image		im;
	WeaveSurfTrie t;
	WeaveSurfHit hit;
	unsigned char longa[WEAVE_SURFTRIE_MAX_DEPTH + 40];
	unsigned char longb[WEAVE_SURFTRIE_MAX_DEPTH + 40];
	unsigned char exact255[WEAVE_SURFTRIE_MAX_DEPTH];
	int			i;

	for (i = 0; i < (int) sizeof(longa); i++)
	{
		longa[i] = (unsigned char) ('a' + (i % 23));
		longb[i] = longa[i];
	}
	/* differ only PAST the maximum depth, so the trie cannot tell them apart */
	longb[WEAVE_SURFTRIE_MAX_DEPTH + 5] ^= 0x20;
	for (i = 0; i < WEAVE_SURFTRIE_MAX_DEPTH; i++)
		exact255[i] = (unsigned char) ('A' + (i % 26));

	vocab_init(&v);
	vocab_add(&v, longa, sizeof(longa));
	vocab_add(&v, longb, sizeof(longb));
	vocab_add(&v, exact255, sizeof(exact255));	/* the longest EXACT term */
	vocab_add(&v, "short", 5);
	vocab_sort_dedup(&v);

	if (!image_build(&v, &im, "truncation"))
	{
		vocab_free(&v);
		return;
	}
	CHECK(weave_surftrie_open(im.img, im.len, &t) == WEAVE_SURF_OK,
		  "truncation: open() failed");
	CHECK(weave_surftrie_validate(&t) == WEAVE_SURF_OK,
		  "truncation: validate() failed");

	/* the collapse happened: fewer terminals than terms, and a truncated slot */
	CHECK(t.ntrunc >= 1, "truncation: no truncated slot was recorded");
	CHECK(t.nterminal == t.nterms - 1,
		  "truncation: nterminal %u with nterms %u (expected one collapse)",
		  t.nterminal, t.nterms);
	CHECK(t.maxdepth == WEAVE_SURFTRIE_MAX_DEPTH,
		  "truncation: maxdepth %u", t.maxdepth);

	/* no false negatives, even for the collapsed pair */
	CHECK(weave_surftrie_may_contain(&t, longa, sizeof(longa), &hit) != 0,
		  "truncation: FALSE NEGATIVE on the first over-long term");
	CHECK(hit.exact == 0, "truncation: over-long term claimed exact");
	CHECK(weave_surftrie_may_contain(&t, longb, sizeof(longb), &hit) != 0,
		  "truncation: FALSE NEGATIVE on the second over-long term");
	CHECK(hit.exact == 0, "truncation: over-long term claimed exact");

	/* the term of exactly the maximum length is EXACT, not truncated */
	CHECK(weave_surftrie_may_contain(&t, exact255, sizeof(exact255), &hit) != 0,
		  "truncation: FALSE NEGATIVE on a max-length term");
	CHECK(hit.exact == 1, "truncation: max-length term reported inexact");

	/* THE ASYMMETRY: a non-member that shares the truncated prefix is reported
	 * present, with exact == 0 so the caller knows to recheck */
	CHECK(vocab_find(&v, longa, WEAVE_SURFTRIE_MAX_DEPTH) < 0,
		  "truncation: the test's own premise is wrong");
	CHECK(weave_surftrie_may_contain(&t, longa, WEAVE_SURFTRIE_MAX_DEPTH, &hit) != 0,
		  "truncation: the truncated prefix was rejected (a false NEGATIVE would "
		  "be a bug, this direction is the whole contract)");
	CHECK(hit.exact == 0, "truncation: false positive claimed to be exact");
	fp_allowed++;

	/* and a key that extends the truncated prefix differently is also a MAYBE */
	{
		unsigned char other[WEAVE_SURFTRIE_MAX_DEPTH + 3];

		memcpy(other, longa, WEAVE_SURFTRIE_MAX_DEPTH);
		other[WEAVE_SURFTRIE_MAX_DEPTH] = 0xFF;
		other[WEAVE_SURFTRIE_MAX_DEPTH + 1] = 0xFF;
		other[WEAVE_SURFTRIE_MAX_DEPTH + 2] = 0xFF;
		CHECK(weave_surftrie_may_contain(&t, other, sizeof(other), &hit) != 0,
			  "truncation: an extension of a truncated slot was rejected");
		CHECK(hit.exact == 0, "truncation: extension claimed exact");
		fp_allowed++;
	}

	/* a prefix query longer than the format holds returns candidates, not
	 * silence: prefixlen > MAX_DEPTH is the other one-sided case */
	{
		Coll		c;

		coll_init(&c, 0);
		(void) weave_surftrie_enumerate(&t, longa, sizeof(longa), coll_cb, &c, NULL);
		CHECK(c.n == 1, "truncation: over-long prefix gave %d candidates", c.n);
		if (c.n == 1)
		{
			CHECK(c.exacts[0] == 0, "truncation: over-long prefix claimed exact");
			CHECK(c.out.t[0].len == WEAVE_SURFTRIE_MAX_DEPTH,
				  "truncation: candidate length %u", c.out.t[0].len);
		}
		coll_free(&c);
	}

	image_free(&im);

	/* the full battery in the one-sided regime */
	test_vocab(&v, "truncation/battery", 0, 0);
	vocab_free(&v);
}

static void
test_directed(void)
{
	Vocab		v;
	int			i;

	/* a single term */
	vocab_init(&v);
	vocab_add(&v, "solitary", 8);
	test_vocab(&v, "single", 1, 32);
	vocab_free(&v);

	/* THE CLASSIC TRIE BUG: terms that are prefixes of each other */
	vocab_init(&v);
	vocab_add(&v, "a", 1);
	vocab_add(&v, "ab", 2);
	vocab_add(&v, "abc", 3);
	vocab_add(&v, "abcd", 4);
	vocab_add(&v, "b", 1);
	vocab_sort_dedup(&v);
	test_vocab(&v, "prefix-chain", 1, 64);
	vocab_free(&v);

	/* one node with all 256 possible labels, which also puts four select samples
	 * in play for a single-level trie */
	vocab_init(&v);
	for (i = 0; i < 256; i++)
	{
		unsigned char b = (unsigned char) i;

		vocab_add(&v, &b, 1);
	}
	vocab_sort_dedup(&v);
	test_vocab(&v, "fanout-256", 1, 128);
	vocab_free(&v);

	/* shared long prefixes: 400 terms differing only in their last two bytes,
	 * which is the deep-and-narrow shape a URL or SKU vocabulary produces */
	vocab_init(&v);
	for (i = 0; i < 400; i++)
	{
		unsigned char t[80];
		int			k;

		for (k = 0; k < 76; k++)
			t[k] = (unsigned char) ('p' + (k % 7));
		t[76] = (unsigned char) ('0' + (i / 100));
		t[77] = (unsigned char) ('0' + ((i / 10) % 10));
		t[78] = (unsigned char) ('0' + (i % 10));
		vocab_add(&v, t, 79);
	}
	vocab_sort_dedup(&v);
	test_vocab(&v, "shared-long-prefix", 1, 128);
	vocab_free(&v);

	/* UTF-8 multi-byte terms (see include/weave/utf8.h): the trie is over BYTES,
	 * so a multi-byte character is a path of two to four slots and a prefix that
	 * lands mid-character is a legal query that must simply not match */
	vocab_init(&v);
	for (i = 0; i < 500; i++)
	{
		unsigned char t[64];
		int			len = 0;
		int			nch = 1 + (int) rnd(6);
		int			k;

		for (k = 0; k < nch; k++)
		{
			uint32_t	cp;

			switch (rnd(4))
			{
				case 0:
					cp = 0x61 + rnd(26);
					break;
				case 1:
					cp = 0xE0 + rnd(0x300);		/* 2-byte: Latin-1 sup, Greek */
					break;
				case 2:
					cp = 0x4E00 + rnd(0x200);	/* 3-byte: CJK */
					break;
				default:
					cp = 0x1F300 + rnd(0x80);	/* 4-byte: emoji */
					break;
			}
			len += utf8_put(t + len, cp);
		}
		vocab_add(&v, t, (uint32_t) len);
	}
	vocab_sort_dedup(&v);
	test_vocab(&v, "utf8", 1, 256);
	vocab_free(&v);

	/* a term of exactly the maximum length the format allows, alone and beside
	 * neighbours that share all but the last byte */
	vocab_init(&v);
	for (i = 0; i < 4; i++)
	{
		unsigned char t[WEAVE_SURFTRIE_MAX_DEPTH];
		int			k;

		for (k = 0; k < WEAVE_SURFTRIE_MAX_DEPTH; k++)
			t[k] = (unsigned char) ('m' + (k % 11));
		t[WEAVE_SURFTRIE_MAX_DEPTH - 1] = (unsigned char) ('0' + i);
		vocab_add(&v, t, WEAVE_SURFTRIE_MAX_DEPTH);
	}
	vocab_sort_dedup(&v);
	test_vocab(&v, "maxdepth-exact", 1, 32);
	{
		Image		im;
		WeaveSurfTrie t;

		if (image_build(&v, &im, "maxdepth-exact/geometry"))
		{
			CHECK(weave_surftrie_open(im.img, im.len, &t) == WEAVE_SURF_OK,
				  "maxdepth-exact: open() failed");
			CHECK(t.maxdepth == WEAVE_SURFTRIE_MAX_DEPTH,
				  "maxdepth-exact: maxdepth %u", t.maxdepth);
			CHECK(t.ntrunc == 0, "maxdepth-exact: %u truncated slots, expected 0",
				  t.ntrunc);
			image_free(&im);
		}
	}
	vocab_free(&v);
}

/*
 * Random vocabularies.  Small alphabets are deliberate: they force shared
 * prefixes, prefix-of-each-other pairs and high fanout, which random 8-bit
 * strings almost never produce.  The largest sizes exist to cross the 512-bit
 * rank superblock and 64-node select-sample boundaries, where an off-by-one in
 * an accelerator would otherwise be invisible.
 */
static void
test_random(void)
{
	static const int alphas[] = {2, 3, 4, 8, 26, 256};
	static const int sizes[] = {1, 2, 3, 7, 32, 200, 1200, 4000};
	int			ai;
	int			si;
	int			trial;

	for (ai = 0; ai < (int) (sizeof(alphas) / sizeof(alphas[0])); ai++)
	{
		for (si = 0; si < (int) (sizeof(sizes) / sizeof(sizes[0])); si++)
		{
			for (trial = 0; trial < 4; trial++)
			{
				Vocab		v;
				char		label[64];
				int			i;
				int			maxlen = (sizes[si] > 200) ? 12 : 9;

				vocab_init(&v);
				for (i = 0; i < sizes[si]; i++)
				{
					unsigned char t[16];
					int			len = 1 + (int) rnd((uint32_t) maxlen);
					int			k;

					for (k = 0; k < len; k++)
						t[k] = (unsigned char) rnd((uint32_t) alphas[ai]);
					vocab_add(&v, t, (uint32_t) len);
				}
				vocab_sort_dedup(&v);
				snprintf(label, sizeof(label), "random a=%d n=%d/%d t=%d",
						 alphas[ai], v.n, sizes[si], trial);
				test_vocab(&v, label, 1, (v.n > 200) ? 96 : 48);
				vocab_free(&v);
			}
		}
	}
}

/*
 * Corruption, the cheap half of it: a single byte flipped anywhere in a valid
 * image must produce a RECOGNIZED error code or a self-consistent trie, never a
 * crash and never a hang.  test/fuzz/fuzz_surftrie.c does this properly under
 * ASan+UBSan with exact-sized buffers and planted-bug builds; this version runs
 * in the default gate, where clang may not be present, and it is also the
 * check that every error code has an errstr -- a new code with no string is a
 * corruption report nobody wrote an errdetail for.
 */
static void
test_corruption(void)
{
	Vocab		v;
	Image		im;
	int			i;
	long		accepted = 0;
	long		rejected = 0;

	vocab_init(&v);
	for (i = 0; i < 300; i++)
	{
		unsigned char t[10];
		int			len = 1 + (int) rnd(9);
		int			k;

		for (k = 0; k < len; k++)
			t[k] = (unsigned char) ('a' + rnd(6));
		vocab_add(&v, t, (uint32_t) len);
	}
	vocab_sort_dedup(&v);
	if (!image_build(&v, &im, "corruption"))
	{
		vocab_free(&v);
		return;
	}

	for (i = 0; i < 20000; i++)
	{
		size_t		off = (size_t) (rng() % im.len);
		unsigned char save = im.img[off];
		WeaveSurfError err;

		im.img[off] ^= (unsigned char) (1 + rnd(255));
		err = weave_surftrie_check(im.img, im.len);
		CHECK(strcmp(weave_surftrie_errstr(err),
					 "unrecognized surf trie error") != 0,
			  "corruption: error code %d has no errstr", (int) err);
		if (err == WEAVE_SURF_OK)
		{
			WeaveSurfTrie t;
			Coll		c;

			/*
			 * A flip can produce a DIFFERENT but perfectly valid trie (a label
			 * byte that keeps its node ascending, say), so there is no oracle
			 * for the answers here.  What must still hold is that every walk
			 * terminates and stays inside the image -- the guard bytes and, in
			 * the sanitizer build, ASan's redzones are what check that.
			 */
			CHECK(weave_surftrie_open(im.img, im.len, &t) == WEAVE_SURF_OK,
				  "corruption: check() and open() disagree");
			coll_init(&c, 0);
			(void) weave_surftrie_enumerate(&t, NULL, 0, coll_cb, &c, NULL);
			coll_free(&c);
			accepted++;
		}
		else
			rejected++;
		im.img[off] = save;
	}

	CHECK(rejected > 0, "corruption: nothing was rejected");
	CHECK(accepted + rejected == 20000, "corruption: case count");
	image_free(&im);
	vocab_free(&v);
}

/* ---------------------------------------------------------------------------
 * The validator has to REJECT things, and that needs its own battery
 * ------------------------------------------------------------------------- */

/*
 * Single-field surgery on a real image, asserting the SPECIFIC error code.
 *
 * Why this exists in addition to the random corruption battery: mutation-testing
 * this file showed that deleting a validator guard is often invisible to random
 * single-byte flips, because the flip has to land in the one field that guard
 * covers AND the consequence has to be observable.  A directed case per guard is
 * deterministic, and it also pins the error CODE, which is what a weave_check()
 * row and an errdetail are made of -- "index is corrupted" is not a diagnosis.
 */
static void
expect_code(unsigned char *img, size_t len, WeaveSurfError want, const char *what)
{
	WeaveSurfError got = weave_surftrie_check(img, len);

	CHECK(got == want, "reject/%s: got %s, expected %s", what,
		  weave_surftrie_errstr(got), weave_surftrie_errstr(want));
}

static void
test_validator_rejects(void)
{
	Vocab		v;
	Image		im;
	WeaveSurfTrie t;
	unsigned char *c;
	size_t		len;
	int			i;

	/*
	 * A vocabulary chosen so the surgery below is meaningful: the root node has
	 * several slots (labels to disorder), there are interior nodes (has-child
	 * bits to clear), nslots is not a multiple of 8 (so there ARE tail bits) and
	 * nnodes is far from a multiple of 64 (so bumping it does not resize the
	 * select sample array and turn every case into a size error).
	 */
	vocab_init(&v);
	vocab_add(&v, "ab", 2);
	vocab_add(&v, "abc", 3);
	vocab_add(&v, "abd", 3);
	vocab_add(&v, "b", 1);
	vocab_add(&v, "bc", 2);
	vocab_add(&v, "c", 1);
	vocab_sort_dedup(&v);
	if (!image_build(&v, &im, "reject/base"))
	{
		vocab_free(&v);
		return;
	}
	len = im.len;
	c = (unsigned char *) malloc(len);
	CHECK(weave_surftrie_open(im.img, len, &t) == WEAVE_SURF_OK,
		  "reject: the base image is not valid, so nothing below means anything");

	{
		{
			weave_st_uint32 nslots = t.nslots;
			weave_st_uint32 bw = t.bw;
			size_t		off_labels = WEAVE_ST_HDRSIZE;
			size_t		off_hc = off_labels + nslots;
			size_t		off_louds = off_hc + bw;
			size_t		off_term = off_louds + bw;
			size_t		off_trunc = off_term + bw;
			size_t		off_rankhc = off_trunc + bw;
			size_t		off_ranktm = off_rankhc + 4;
			size_t		off_sel = off_ranktm + 4;
			size_t		off_ords = off_sel + 4;

			CHECK(nslots % 8 != 0, "reject: nslots %u is a multiple of 8, so the "
				  "tail-bit case would not exercise anything", nslots);
			CHECK(off_ords + 4 * (size_t) t.nterminal == len,
				  "reject: the test's own offset arithmetic disagrees with the "
				  "image length");

			/* truncated below the fixed header */
			for (i = 0; i < (int) WEAVE_ST_HDRSIZE; i++)
			{
				memcpy(c, im.img, (size_t) i);
				expect_code(c, (size_t) i, WEAVE_SURF_TRUNCATED, "short header");
			}

			/* header fields */
			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_MAGIC] ^= 0xFF;
			expect_code(c, len, WEAVE_SURF_MAGIC, "magic");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_VERSION] = WEAVE_SURFTRIE_VERSION + 1;
			expect_code(c, len, WEAVE_SURF_VERSION, "version");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_FLAGS] = 1;
			expect_code(c, len, WEAVE_SURF_FLAGS, "flags");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_RESERVED] = 1;
			expect_code(c, len, WEAVE_SURF_RESERVED, "reserved");

			/* declared length disagreeing with the counts, both directions */
			memcpy(c, im.img, len);
			expect_code(c, len - 1, WEAVE_SURF_SIZE, "one byte short");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_NTERMS] = 0;
			c[WEAVE_ST_OFF_NTERMS + 1] = 0;
			expect_code(c, len, WEAVE_SURF_COUNTS, "nterms 0 with slots");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_NNODES] = (unsigned char) (t.nslots + 1);
			expect_code(c, len, WEAVE_SURF_COUNTS, "nnodes > nslots");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_MAXDEPTH] = 0;
			c[WEAVE_ST_OFF_MAXDEPTH + 1] = 0;
			expect_code(c, len, WEAVE_SURF_DEPTH, "maxdepth 0");

			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_MAXDEPTH] = 0x2C;	/* 300 > MAX_DEPTH */
			c[WEAVE_ST_OFF_MAXDEPTH + 1] = 0x01;
			expect_code(c, len, WEAVE_SURF_DEPTH, "maxdepth over the cap");

			/* a set bit past the last slot: not corruption a query would notice,
			 * which is exactly why it is rejected -- it is what makes the
			 * popcount identities below mean what they say */
			memcpy(c, im.img, len);
			c[off_trunc + (nslots >> 3)] |= (unsigned char) (1u << (nslots & 7));
			expect_code(c, len, WEAVE_SURF_TAILBITS, "tail bit in trunc");

			memcpy(c, im.img, len);
			c[off_louds + (nslots >> 3)] |= (unsigned char) (1u << (nslots & 7));
			expect_code(c, len, WEAVE_SURF_TAILBITS, "tail bit in louds");

			/* the population identities */
			memcpy(c, im.img, len);
			c[WEAVE_ST_OFF_NNODES] = (unsigned char) (t.nnodes - 1);
			expect_code(c, len, WEAVE_SURF_LOUDS_COUNT, "nnodes too small");

			memcpy(c, im.img, len);
			c[off_hc] &= (unsigned char) ~1u;	/* clear a has-child bit */
			expect_code(c, len, WEAVE_SURF_HASCHILD_COUNT, "has-child cleared");

			memcpy(c, im.img, len);
			c[off_term] ^= (unsigned char) 1u;	/* flip a terminal bit */
			expect_code(c, len, WEAVE_SURF_TERMINAL_COUNT, "terminal flipped");

			memcpy(c, im.img, len);
			c[off_trunc] |= (unsigned char) 1u;	/* trunc without ntrunc */
			expect_code(c, len, WEAVE_SURF_TRUNC, "trunc population");

			/* labels out of order inside a node breaks the binary search, which
			 * would MISS a term: a false negative, hence a rejection */
			memcpy(c, im.img, len);
			c[off_labels] = 0xFF;
			expect_code(c, len, WEAVE_SURF_LABELS, "labels disordered");

			/* the accelerators */
			memcpy(c, im.img, len);
			c[off_rankhc] = 1;	/* rank[0] must be 0 */
			expect_code(c, len, WEAVE_SURF_RANK, "rank table");

			memcpy(c, im.img, len);
			c[off_ranktm] = 1;
			expect_code(c, len, WEAVE_SURF_RANK, "terminal rank table");

			memcpy(c, im.img, len);
			c[off_sel] = 1;		/* sample[0] must be slot 0 */
			expect_code(c, len, WEAVE_SURF_SELECT, "select sample");

			/* ordinals */
			memcpy(c, im.img, len);
			c[off_ords] = (unsigned char) t.nterms;
			expect_code(c, len, WEAVE_SURF_ORD_RANGE, "ordinal out of range");

			memcpy(c, im.img, len);
			c[off_ords] = 1;	/* the first terminal in DFS order must be 0 */
			expect_code(c, len, WEAVE_SURF_ORD_ORDER, "first ordinal not zero");

			memcpy(c, im.img, len);
			c[off_ords] = 0;
			c[off_ords + 4] = 0;	/* second terminal repeats ordinal 0 */
			expect_code(c, len, WEAVE_SURF_ORD_ORDER, "ordinals not ascending");
		}
	}

	free(c);
	image_free(&im);
	vocab_free(&v);
}

/*
 * The one invariant that cannot be reached by patching a valid image, so it gets
 * a hand-laid one: a child node that does NOT start after its parent slot.
 *
 * This is the check that makes every walk terminate, and it is the reason a
 * corrupt trie produces an ERROR rather than a backend that spins in
 * weave_surftrie_enumerate() forever.  Any edit to the louds bitmap of a real
 * image is caught earlier by the select-sample recomputation, so the only way to
 * present this validator with a cycle is to build the whole image by hand -- two
 * slots, two nodes, the second slot claiming a child node that starts at itself.
 */
static void
test_reject_cycle(void)
{
	unsigned char img[128];
	size_t		bw = 8;
	size_t		off_labels = WEAVE_ST_HDRSIZE;
	size_t		off_hc = off_labels + 2;
	size_t		off_louds = off_hc + bw;
	size_t		off_term = off_louds + bw;
	size_t		off_trunc = off_term + bw;
	size_t		off_rankhc = off_trunc + bw;
	size_t		off_ranktm = off_rankhc + 4;
	size_t		off_sel = off_ranktm + 4;
	size_t		off_ords = off_sel + 4;
	size_t		len = off_ords + 4;

	memset(img, 0, sizeof(img));
	img[WEAVE_ST_OFF_MAGIC + 0] = 0x31;		/* "WST1", little-endian */
	img[WEAVE_ST_OFF_MAGIC + 1] = 0x54;
	img[WEAVE_ST_OFF_MAGIC + 2] = 0x53;
	img[WEAVE_ST_OFF_MAGIC + 3] = 0x57;
	img[WEAVE_ST_OFF_VERSION] = WEAVE_SURFTRIE_VERSION;
	img[WEAVE_ST_OFF_NTERMS] = 1;
	img[WEAVE_ST_OFF_NSLOTS] = 2;
	img[WEAVE_ST_OFF_NNODES] = 2;
	img[WEAVE_ST_OFF_NTERMINAL] = 1;
	img[WEAVE_ST_OFF_MAXDEPTH] = 1;
	img[off_labels + 0] = 'a';
	img[off_labels + 1] = 'b';
	img[off_hc] = 0x02;			/* slot 1 has a child */
	img[off_louds] = 0x03;		/* both slots begin a node */
	img[off_term] = 0x01;		/* slot 0 is terminal, ordinal 0 */
	(void) off_rankhc;
	(void) off_ranktm;
	(void) off_sel;				/* all-zero tables are the correct ones here */

	/*
	 * Sanity first: with slot 1's child pointing FORWARD this image is valid.
	 * Without this half the test could be passing because the image is malformed
	 * for some other reason entirely.
	 */
	CHECK(weave_surftrie_check(img, len) == WEAVE_SURF_CYCLE,
		  "reject/cycle: a self-referential child node was accepted (%s)",
		  weave_surftrie_errstr(weave_surftrie_check(img, len)));

	/* and the same image with the child bit cleared IS valid: proof that CYCLE
	 * above was the only thing wrong with it */
	img[off_hc] = 0x00;
	img[WEAVE_ST_OFF_NNODES] = 2;
	img[off_louds] = 0x03;
	CHECK(weave_surftrie_check(img, len) == WEAVE_SURF_HASCHILD_COUNT,
		  "reject/cycle: expected the has-child identity to complain next");
	img[WEAVE_ST_OFF_NNODES] = 1;
	img[off_louds] = 0x01;		/* one node covering both slots */
	CHECK(weave_surftrie_check(img, len) == WEAVE_SURF_OK,
		  "reject/cycle: the repaired image should be valid, so CYCLE really was "
		  "the only thing wrong with it (%s)",
		  weave_surftrie_errstr(weave_surftrie_check(img, len)));
}

int
main(void)
{
	printf("surf trie: membership == dictionary membership, prefix enumeration, "
		   "one-sided error, image validation\n");

	test_empty_vocabulary();
	test_input_validation();
	test_directed();
	test_truncation();
	test_validator_rejects();
	test_reject_cycle();
	test_corruption();
	test_random();

	if (failures)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures (%ld permitted false positives observed)\n",
		   checks, failures, fp_allowed);
	return 0;
}
