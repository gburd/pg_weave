/*-------------------------------------------------------------------------
 *
 * edist_bound.c
 *		Does the `<@>` edit-distance block bound actually prune?
 *
 * doc/specs/FUZZY_CHANNEL.md sect. 5 ends with "Tightness is unmeasured and MUST
 * be measured before this is called done -- the lesson from the vector channel is
 * that a provably-correct bound can prune 0.0 % of blocks and that this is
 * invisible without measuring the pruning rate directly.  bench/bound_pruning.c
 * is the shape to copy."  This is that measurement, in that shape, and it
 * deliberately answers the same question with the same two numbers:
 *
 *	 - what fraction of dictionary PAGES can be skipped without computing a single
 *	   exact distance on them, and
 *	 - what fraction of TERMS therefore have their exact distance computed.
 *
 * Two thresholds are reported for the page fraction, because they bound the
 * answer from both sides and only one of them is achievable:
 *
 *	 RUNNING  theta is the k-th best distance found SO FAR, in dictionary order.
 *			  This is what the scan actually gets, and it is pessimistic at the
 *			  start of the walk when theta is still infinite.
 *	 FINAL	  theta is the true k-th best distance over the whole vocabulary.
 *			  Unachievable without knowing the answer in advance; it is the
 *			  ceiling a better dictionary ordering could approach, and it is the
 *			  number bench/bound_pruning.c reports for the vector channel, so it
 *			  is the one comparable to that file.
 *
 * It also asserts soundness (channel.h contract C2) on every page of every query
 * and exits non-zero on the first violation, so it doubles as a large randomized
 * correctness test of the bound over a realistic vocabulary rather than a
 * generated one.  No server is needed: the bound is in a header.
 *
 *		cc -O2 -I include -o /tmp/edist_bound bench/edist_bound.c -lm
 *		/tmp/edist_bound 0	# lexicographic pages: the real dictionary order
 *		/tmp/edist_bound 1	# length-clustered pages: a hypothetical ordering
 *
 * Results are recorded in bench/RESULTS_EDIST_BOUND.md.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  bench/edist_bound.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/edist.h"

/* ---------------------------------------------------------------------------
 * The vocabulary: 250k machine-generated terms plus a few thousand word-shaped
 * ones.  The first set is what a corpus of identifiers, SKUs or log tokens gives
 * a dictionary -- a huge run of terms sharing a prefix and differing by a few
 * digits -- and the second is what English gives it.  They are measured together
 * and separately, because the bound's two deficits behave completely differently
 * on them: 't'||n terms are nearly all the same length, so the length deficit has
 * almost nothing to say.
 * ------------------------------------------------------------------------- */
#define NNUM	250000
#define NWORD	4000
#define NTERMS	(NNUM + NWORD)
#define MAXLEN	24

static char *terms;				/* NTERMS * MAXLEN, NUL-padded */
static int	lens[NTERMS];

static uint64_t rs = 0x9E3779B97F4A7C15ULL;

static uint64_t
r64(void)
{
	uint64_t	x = rs;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rs = x;
	return x * 0x2545F4914F6CDD1DULL;
}

#define TERM(i) (terms + (size_t) (i) * MAXLEN)

/* ---------------------------------------------------------------------------
 * The reference: character-level Levenshtein, unit costs.  ASCII throughout in
 * this harness, so a byte is a character; the multi-byte behaviour of the bound
 * is covered by test/hegel/test_edist.c, which is the correctness gate.  Here the
 * question is how much work the bound saves, and mixing encodings into it would
 * only make the number harder to read.
 * ------------------------------------------------------------------------- */
static int
lev(const char *a, int m, const char *b, int n)
{
	int			prev[MAXLEN + 1];
	int			cur[MAXLEN + 1];
	int			i;
	int			j;

	for (j = 0; j <= n; j++)
		prev[j] = j;
	for (i = 1; i <= m; i++)
	{
		cur[0] = i;
		for (j = 1; j <= n; j++)
		{
			int			s = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
			int			d = prev[j] + 1;
			int			ins = cur[j - 1] + 1;
			int			best = s < d ? s : d;

			cur[j] = best < ins ? best : ins;
		}
		memcpy(prev, cur, sizeof(int) * (size_t) (n + 1));
	}
	return prev[n];
}

/* weave_trigrams(): distinct byte trigrams, one space-padded trigram below 3. */
static int
ntrg(const char *s, int len)
{
	uint32_t	seen[MAXLEN];
	int		n = 0;
	int		i;

	if (len <= 0)
		return 0;
	if (len < 3)
		return 1;
	for (i = 0; i + 3 <= len; i++)
	{
		uint32_t	h = ((uint32_t) (unsigned char) s[i] << 16) |
			((uint32_t) (unsigned char) s[i + 1] << 8) |
			(uint32_t) (unsigned char) s[i + 2];
		int			k;
		int			dup = 0;

		for (k = 0; k < n; k++)
			if (seen[k] == h)
			{
				dup = 1;
				break;
			}
		if (!dup && n < MAXLEN)
			seen[n++] = h;
	}
	return n;
}

static int
cmp_term(const void *x, const void *y)
{
	int			a = *(const int *) x;
	int			b = *(const int *) y;
	int			m = lens[a] < lens[b] ? lens[a] : lens[b];
	int			c = memcmp(TERM(a), TERM(b), (size_t) m);

	if (c != 0)
		return c;
	return lens[a] - lens[b];
}

/*
 * The SECOND arm, and the reason it exists.  A real dictionary is sorted
 * lexicographically, which is a byte order with nothing to do with term LENGTH or
 * trigram count -- so every page holds a near-full spread of both, its min/max
 * statistics are close to the vocabulary's global min/max, and the two deficits
 * are close to zero on every page.  That is a structural property of the ordering,
 * not of the bound, and it is the same shape as the finding in
 * bench/RESULTS_BOUND_PRUNING.md, where the vector bound needed a docid ordering
 * constraint nobody had written down.  This comparator clusters by (length,
 * term), which is the cheapest ordering that would make the length deficit
 * informative, so the two arms bracket what an ordering change could buy.  It is
 * HYPOTHETICAL: the lexical channel needs the dictionary in byte order for its
 * own point lookups and prefix scans, so adopting it is not a free change.
 */
static int
cmp_term_lenclustered(const void *x, const void *y)
{
	int			a = *(const int *) x;
	int			b = *(const int *) y;

	if (lens[a] != lens[b])
		return lens[a] - lens[b];
	return cmp_term(x, y);
}

/* ---------------------------------------------------------------------------
 * Paging.  A dictionary page is BLCKSZ with a page header, a special area and
 * MAXALIGN'd WeaveDictEntry records, so a term of L bytes occupies
 * MAXALIGN(20 + L) = 8*ceil((20+L)/8) bytes of about 8100 usable.  Reproducing
 * that arithmetic here rather than guessing a terms-per-page number is what makes
 * the page count comparable to a real index's.
 * ------------------------------------------------------------------------- */
#define PAGE_USABLE 8100
#define ENTRY_STRIDE(L) ((((20 + (L)) + 7) / 8) * 8)

#define MAXPAGES 4096

typedef struct
{
	int			first;			/* index into the sorted order */
	int			n;
	WeaveEdistStats st;
}			PageDesc;

static PageDesc pages[MAXPAGES];
static int	npages;
static int	order[NTERMS];

static void
build_pages(void)
{
	int			i = 0;

	npages = 0;
	while (i < NTERMS && npages < MAXPAGES)
	{
		PageDesc   *p = &pages[npages];
		int			used = 0;

		p->first = i;
		p->n = 0;
		weave_edist_stats_init(&p->st);
		while (i < NTERMS)
		{
			int			t = order[i];
			int			need = ENTRY_STRIDE(lens[t]);

			if (used + need > PAGE_USABLE && p->n > 0)
				break;
			weave_edist_stats_add(&p->st, (weave_ed_uint32) lens[t],
								  (weave_ed_uint32) lens[t],
								  (weave_ed_uint32) ntrg(TERM(t), lens[t]), 1);
			used += need;
			p->n++;
			i++;
		}
		npages++;
	}
}

/* ---------------------------------------------------------------------------
 * One query
 * ------------------------------------------------------------------------- */
typedef struct
{
	double		pr_running;		/* fraction of pages pruned, running theta */
	double		pr_final;		/* ... with the final (oracle) theta */
	double		term_ratio;		/* fraction of terms scored, running theta */
	int			bestk;			/* the true k-th best distance */
}			QResult;

static int
cmp_int(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

static QResult
run_query(const char *pat, int patlen, int k, int *violations)
{
	WeaveEdistPattern pv;
	QResult		r;
	int		   *all = malloc(sizeof(int) * NTERMS);
	int			theta;			/* the k-th best distance so far */
	int			topk[64];
	int			ntop = 0;
	int			p;
	long		pruned_run = 0;
	long		pruned_fin = 0;
	long		scored = 0;
	int			i;
	int			finaltheta;

	weave_edist_pattern_init(&pv, (weave_ed_uint32) patlen,
							 (weave_ed_uint32) patlen,
							 (weave_ed_uint32) ntrg(pat, patlen), 1, 1);

	/* the oracle: every distance, so the final theta is known */
	for (i = 0; i < NTERMS; i++)
		all[i] = lev(pat, patlen, TERM(order[i]), lens[order[i]]);
	{
		int		   *cp = malloc(sizeof(int) * NTERMS);

		memcpy(cp, all, sizeof(int) * NTERMS);
		qsort(cp, NTERMS, sizeof(int), cmp_int);
		finaltheta = cp[k - 1];
		free(cp);
	}

	theta = 1 << 30;
	for (p = 0; p < npages; p++)
	{
		int			lower = (int) weave_edist_lower(&pv, &pages[p].st);
		int			q;

		/* (C2): the bound must not exceed ANY distance on the page */
		for (q = 0; q < pages[p].n; q++)
			if (lower > all[pages[p].first + q])
			{
				printf("BOUND VIOLATION page=%d term=%d bound=%d distance=%d pattern=%.*s term=%.*s\n",
					   p, q, lower, all[pages[p].first + q], patlen, pat,
					   lens[order[pages[p].first + q]],
					   TERM(order[pages[p].first + q]));
				(*violations)++;
				if (*violations > 5)
					exit(1);
			}

		if (lower > finaltheta)
			pruned_fin++;
		if (lower > theta)
		{
			pruned_run++;
			continue;
		}
		for (q = 0; q < pages[p].n; q++)
		{
			int			d = all[pages[p].first + q];

			scored++;
			if (ntop < k)
			{
				topk[ntop++] = d;
				if (ntop == k)
				{
					qsort(topk, ntop, sizeof(int), cmp_int);
					theta = topk[k - 1];
				}
			}
			else if (d < theta)
			{
				topk[k - 1] = d;
				qsort(topk, k, sizeof(int), cmp_int);
				theta = topk[k - 1];
			}
		}
	}

	r.pr_running = (double) pruned_run / (double) npages;
	r.pr_final = (double) pruned_fin / (double) npages;
	r.term_ratio = (double) scored / (double) NTERMS;
	r.bestk = finaltheta;
	free(all);
	return r;
}

int
main(int argc, char **argv)
{
	int			i;
	int			violations = 0;
	int			lenclustered = (argc > 1 && atoi(argv[1]) != 0);
	const char *patterns[] = {
		"t129384",					/* a vocabulary term, verbatim: distance 0 */
		"t12938",					/* one deletion from one */
		"t1293845",					/* one insertion */
		"x129384",					/* one substitution, different first byte */
		"tttttttt",					/* in the numeric run's neighbourhood */
		"zqxjvk",					/* nothing like anything */
		"connection",				/* a word, 10 chars */
		"connectoin",				/* a word one transposition off */
		"internationalization",		/* very long */
		"ab",						/* very short */
		"q",						/* one character */
		""							/* empty */
	};
	int			npat = (int) (sizeof(patterns) / sizeof(patterns[0]));
	int			kk[3] = {1, 2, 3};

	terms = malloc((size_t) NTERMS * MAXLEN);
	memset(terms, 0, (size_t) NTERMS * MAXLEN);
	for (i = 0; i < NNUM; i++)
		lens[i] = snprintf(TERM(i), MAXLEN, "t%d", i);
	for (i = 0; i < NWORD; i++)
	{
		int			n = 3 + (int) (r64() % 10);
		int			j;

		for (j = 0; j < n; j++)
			TERM(NNUM + i)[j] = (char) ('a' + (r64() % 26));
		lens[NNUM + i] = n;
	}
	for (i = 0; i < NTERMS; i++)
		order[i] = i;
	qsort(order, NTERMS, sizeof(int),
		  lenclustered ? cmp_term_lenclustered : cmp_term);
	build_pages();

	printf("%s pages\n", lenclustered
		   ? "LENGTH-CLUSTERED (hypothetical ordering)"
		   : "LEXICOGRAPHIC (the real dictionary order)");
	printf("vocabulary: %d terms (%d numeric-suffix, %d word-shaped), "
		   "%d dictionary pages of %d usable bytes\n",
		   NTERMS, NNUM, NWORD, npages, PAGE_USABLE);
	printf("%-22s %2s %6s %9s %9s %9s\n",
		   "pattern", "k", "d(k)", "pages/run", "pages/fin", "terms");
	for (i = 0; i < npat; i++)
	{
		int			patlen = (int) strlen(patterns[i]);
		int			j;

		for (j = 0; j < 3; j++)
		{
			QResult		r = run_query(patterns[i], patlen, kk[j], &violations);

			printf("%-22s %2d %6d %8.1f%% %8.1f%% %8.2f%%\n",
				   patterns[i][0] ? patterns[i] : "(empty)", kk[j], r.bestk,
				   100.0 * r.pr_running, 100.0 * r.pr_final,
				   100.0 * r.term_ratio);
		}
	}
	printf("C2 violations: %d\n", violations);
	return violations == 0 ? 0 : 1;
}
