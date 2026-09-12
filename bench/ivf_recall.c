/*-------------------------------------------------------------------------
 *
 * ivf_recall.c
 *		Is Phase V's recall@10 >= 0.99 gate reachable by an IVF coarse
 *		quantizer over pg_weave's own codes?
 *
 * doc/PHASES.md task V9 picks an IVF coarse quantizer as the route to a
 * sublinear point on the recall x latency frontier, and Phase V's gate is
 * recall@10 >= 0.99.  doc/specs/VECTOR_CHANNEL.md sect. 8a records the risk in
 * so many words: pg_turbovec measured that an IVF's PROBE COUNT sets a hard
 * recall ceiling that no widening of the exact-rerank window can break, because
 * the two knobs fix different failure modes -- a neighbour whose cell was never
 * probed is not recoverable by reranking a wider retrieved set.  That mechanism
 * is about which cells get visited, not about how vectors inside a visited cell
 * are scored, so it is bit-width agnostic and applies here.  What does NOT
 * transfer is their numbers: their measurement is a 1-bit corpus-mean-centered
 * sign code, and this file scores with the 2-4 bit rotated Lloyd-Max codebook of
 * src/vector/quantize.c.  Hence this harness, per AGENTS.md rule 9: measure the
 * thing the design rests on before building the disk format on it.
 *
 * The measurement is deliberately shaped like bench/bound_pruning.c: standalone
 * C, no backend, the shipping codec linked in unmodified, and a work proxy
 * reported as a COUNT (candidate vectors scored) rather than as wall-clock, so
 * the numbers reproduce on any host.  Results: bench/RESULTS_IVF_RECALL.md.
 *
 *		gcc -O2 -Wall -Wextra -I include -o /scratch/ivf_recall \
 *			bench/ivf_recall.c src/vector/quantize.c src/vector/pack.c -lm
 *		/scratch/ivf_recall glove /scratch/pgw-v9-corpus/glove.6B.200d.txt \
 *			200000 200
 *		/scratch/ivf_recall fvecs /scratch/pgw-v9-corpus/gist/gist_base.fvecs \
 *			200000 100
 *		/scratch/ivf_recall synth 1024 2048 0.30 200000 100
 *
 * THREE ARMS, BECAUSE ONE NUMBER CANNOT ATTRIBUTE THE LOSS.  Every row reports
 * recall@10 three ways over the SAME probed candidate set:
 *
 *	 R_exact	candidates rescored with exact float inner products.  Its only
 *				error source is probe-miss, so it is the IVF ceiling for that
 *				nprobe -- what a perfect scorer inside probed cells would get.
 *	 R_w<W>		candidates scored with weave_lut_score_code() (the compressed
 *				domain, no dequantization), top-W kept, then those W rescored
 *				exactly and the best 10 returned.  W = 10 means no rerank at
 *				all.  Error = probe-miss + quantization.
 *	 cellrec	fraction of the true top-10 whose cluster was probed.  An upper
 *				bound on R_exact that involves no scoring at all, so it
 *				separates "the cell was never visited" from everything else.
 *
 * Quantization-only error is then read off the nprobe == lists row: probing
 * every cell has no probe-miss, so 1 - R_w10 there is pure codebook loss, and
 * R_exact there must be exactly 1.000.  THAT IS THE HARNESS SELF-CHECK
 * (AGENTS.md rule 8: a benchmark of a broken fast path is worse than no
 * benchmark).  If probing every cluster with exact rescoring does not reproduce
 * brute force, the partition, the probe order or the candidate walk is broken,
 * and every recall in the table is meaningless.  The program exits non-zero.
 *
 * Recall is computed tie-tolerantly: a retrieved vector counts as a hit if its
 * exact score is >= the K-th best exact score in the ground truth, which is the
 * ann-benchmarks convention.  Plain id-set recall would report < 1.000 for a
 * correct full scan whenever two corpus vectors are equidistant from the query
 * (real corpora contain duplicate rows), and that would turn the self-check
 * above into a false alarm.
 *
 * NESTED PROBES, WHICH IS WHY THE WHOLE SWEEP IS AFFORDABLE.  Candidate sets
 * grow monotonically with nprobe, so the harness walks the clusters once in
 * probe order per (query, lists, bits) cell and snapshots recall as it crosses
 * each nprobe of interest.  One pass yields the whole probe curve instead of one
 * pass per probe count.
 *
 * CORPUS GEOMETRY IS THE WHOLE BALL GAME.  Uniformly random high-dimensional
 * vectors have no cluster structure, so an IVF over them measures nothing about
 * an IVF over embeddings -- in either direction.  The `glove` and `fvecs`
 * loaders read real corpora.  The `synth` mode exists for the sanitizer run and
 * for contrast, generates EXPLICIT cluster structure (nclust isotropic Gaussian
 * blobs, stated sigma), and labels itself synthetic-geometry in its output so a
 * reader cannot mistake one for the other.  Nothing here slices a
 * high-dimensional embedding down to fake a lower dimension: sect. 11 item 3 of
 * doc/specs/VECTOR_CHANNEL.md caveats pg_turbovec's dimension sweep exactly that
 * way, and importing the flaw would make the dimension arm an upper bound rather
 * than a number.  Every corpus is read at its native width.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  bench/ivf_recall.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/quantize.h"

#define MAXK			32
#define MAXPROBES		64
/* Raised from 8 when parse_list() stopped truncating: a window sweep wants
 * 10..200 in one pass, and windows are nested prefixes so each extra one is a
 * single accumulator slot, not another scoring pass. */
#define MAXWIN			16
#define MAXLISTS_CFG	8
/* All widths WEAVE_BITS_MIN..WEAVE_BITS_MAX must fit in one sweep, so that
 * `bits=2,3,4,5,6,7,8` is a single run rather than silently truncated by
 * parse_list().  Sized from the header so it tracks the header. */
#define MAXBITS_CFG		(WEAVE_BITS_MAX - WEAVE_BITS_MIN + 1)

/* xoshiro256**, same generator bench/bound_pruning.c uses, so the two harnesses
 * draw the same streams for the same seed. */
static unsigned long long rs[4] = {0x9e3779b97f4a7c15ULL, 0xbf58476d1ce4e5b9ULL,
	0x94d049bb133111ebULL, 7};

static unsigned long long
r64(void)
{
	unsigned long long r = ((rs[1] * 5) << 7 | (rs[1] * 5) >> 57) * 9;
	unsigned long long t = rs[1] << 17;

	rs[2] ^= rs[0];
	rs[3] ^= rs[1];
	rs[1] ^= rs[2];
	rs[0] ^= rs[3];
	rs[2] ^= t;
	rs[3] = (rs[3] << 45) | (rs[3] >> 19);
	return r;
}

static double
ru(void)
{
	return (double) (r64() >> 11) * (1.0 / 9007199254740992.0);
}

static double
rnorm(void)
{
	double		a = ru();

	if (a < 1e-300)
		a = 1e-300;
	return sqrt(-2 * log(a)) * cos(2 * M_PI * ru());
}

static void
die(const char *msg)
{
	fprintf(stderr, "ivf_recall: %s\n", msg);
	exit(2);
}

static void *
xmalloc(size_t n)
{
	void	   *p = malloc(n);

	if (p == NULL)
		die("out of memory");
	return p;
}

/* ---------------------------------------------------------------------------
 * Corpus
 *
 * Rows are L2-normalized and the metric is inner product, i.e. cosine.  On
 * normalized vectors that ranks identically to L2 (||q-v||^2 = 2 - 2<q,v>), so
 * one code path covers both metrics the channel supports, and the quantizer's
 * compressed-domain estimator is an inner-product estimator to begin with.
 * Ground truth is always recomputed here rather than taken from a dataset's
 * shipped neighbour file, which would be tied to that dataset's own metric and
 * normalization convention.
 * ------------------------------------------------------------------------- */

typedef struct Corpus
{
	int			n;
	int			dim;
	float	   *v;				/* n * dim, row-major, unit norm */
	const char *label;
	int			synthetic;
} Corpus;

static void
normalize_rows(Corpus *c)
{
	int			i,
				j,
				w = 0;

	for (i = 0; i < c->n; i++)
	{
		float	   *p = c->v + (size_t) i * c->dim;
		double		s = 0;

		for (j = 0; j < c->dim; j++)
			s += (double) p[j] * (double) p[j];
		s = sqrt(s);

		/*
		 * weave_encode() refuses a zero vector rather than encoding it, so a
		 * zero row cannot be carried through the sweep.  Drop it and say how
		 * many were dropped; silently substituting anything would create a row
		 * that scores equally against every query.
		 */
		if (!(s > 1e-20))
			continue;
		if (w != i)
			memmove(c->v + (size_t) w * c->dim, p, sizeof(float) * (size_t) c->dim);
		p = c->v + (size_t) w * c->dim;
		for (j = 0; j < c->dim; j++)
			p[j] = (float) ((double) p[j] / s);
		w++;
	}
	if (w != c->n)
		fprintf(stderr, "  dropped %d zero-norm rows\n", c->n - w);
	c->n = w;
}

/* GloVe-style text: one row per line, "token f1 f2 ... fd".  Read at the file's
 * native width; no slicing. */
static void
load_glove(Corpus *c, const char *path, int want)
{
	FILE	   *f = fopen(path, "r");
	size_t		cap = 1 << 20;
	char	   *line = xmalloc(cap);
	int			dim = -1;
	int			n = 0;

	if (f == NULL)
		die("cannot open corpus file");
	c->v = NULL;
	while (n < want)
	{
		size_t		len = 0;
		int			ch;
		char	   *p;
		int			j;

		for (;;)
		{
			ch = fgetc(f);
			if (ch == EOF || ch == '\n')
				break;
			if (len + 2 >= cap)
			{
				cap *= 2;
				line = realloc(line, cap);
				if (line == NULL)
					die("out of memory");
			}
			line[len++] = (char) ch;
		}
		if (len == 0 && ch == EOF)
			break;
		line[len] = '\0';

		p = line;
		while (*p && *p != ' ')
			p++;					/* skip the token */

		if (dim < 0)
		{
			char	   *q = p;

			dim = 0;
			for (;;)
			{
				char	   *end;
				double		d = strtod(q, &end);

				(void) d;
				if (end == q)
					break;
				dim++;
				q = end;
			}
			if (dim < 8 || dim > WEAVE_MAX_DIM)
				die("implausible dimension in text corpus");
			c->dim = dim;
			c->v = xmalloc(sizeof(float) * (size_t) want * (size_t) dim);
		}

		for (j = 0; j < dim; j++)
		{
			char	   *end;
			double		d = strtod(p, &end);

			if (end == p)
				break;
			c->v[(size_t) n * dim + j] = (float) d;
			p = end;
		}
		if (j != dim)
			continue;			/* short line, skip */
		n++;
	}
	fclose(f);
	free(line);
	if (c->v == NULL || n == 0)
		die("empty corpus");
	c->n = n;
}

/* .fvecs (TEXMEX SIFT/GIST): int32 dim, then dim float32, repeated. */
static void
load_fvecs(Corpus *c, const char *path, int want)
{
	FILE	   *f = fopen(path, "rb");
	int			dim = -1;
	int			n = 0;

	if (f == NULL)
		die("cannot open corpus file");
	c->v = NULL;
	while (n < want)
	{
		int			d;

		if (fread(&d, sizeof(int), 1, f) != 1)
			break;
		if (dim < 0)
		{
			if (d < 8 || d > WEAVE_MAX_DIM)
				die("implausible dimension in fvecs header");
			dim = d;
			c->dim = d;
			c->v = xmalloc(sizeof(float) * (size_t) want * (size_t) d);
		}
		if (d != dim)
			die("ragged fvecs file");
		if (fread(c->v + (size_t) n * dim, sizeof(float), (size_t) dim, f) !=
			(size_t) dim)
			break;
		n++;
	}
	fclose(f);
	if (c->v == NULL || n == 0)
		die("empty corpus");
	c->n = n;
}

/*
 * Synthetic corpus WITH cluster structure, parameters stated in the output.
 * nclust isotropic Gaussian blobs: a unit-Gaussian centre per cluster, members
 * at centre + sigma * N(0, I), then normalized.  This is not a stand-in for an
 * embedding corpus -- it is the sanitizer fixture and a contrast arm, and every
 * number derived from it is labelled synthetic-geometry.
 */
static void
gen_synth(Corpus *c, int n, int dim, int nclust, double sigma)
{
	float	   *ctr = xmalloc(sizeof(float) * (size_t) nclust * (size_t) dim);
	int			i,
				j;

	if (dim < 8 || dim > WEAVE_MAX_DIM || nclust < 1)
		die("bad synth parameters");
	c->n = n;
	c->dim = dim;
	c->v = xmalloc(sizeof(float) * (size_t) n * (size_t) dim);
	for (i = 0; i < nclust; i++)
		for (j = 0; j < dim; j++)
			ctr[(size_t) i * dim + j] = (float) rnorm();
	for (i = 0; i < n; i++)
	{
		const float *b = ctr + (size_t) (r64() % (unsigned long long) nclust) * dim;
		float	   *p = c->v + (size_t) i * dim;

		for (j = 0; j < dim; j++)
			p[j] = (float) (b[j] + sigma * rnorm());
	}
	free(ctr);
}

/* ---------------------------------------------------------------------------
 * Bounded min-heap of (score, id), used for top-K and for the rerank window.
 * ------------------------------------------------------------------------- */

typedef struct HeapEnt
{
	double		score;
	int			id;
} HeapEnt;

typedef struct Heap
{
	HeapEnt    *e;
	int			n;
	int			cap;
} Heap;

static void
heap_reset(Heap *h)
{
	h->n = 0;
}

static void
heap_push(Heap *h, double score, int id)
{
	int			i;

	if (h->n == h->cap)
	{
		if (score <= h->e[0].score)
			return;
		h->e[0].score = score;
		h->e[0].id = id;
		/* sift down */
		i = 0;
		for (;;)
		{
			int			l = 2 * i + 1,
						r = l + 1,
						m = i;

			if (l < h->n && h->e[l].score < h->e[m].score)
				m = l;
			if (r < h->n && h->e[r].score < h->e[m].score)
				m = r;
			if (m == i)
				break;
			{
				HeapEnt		t = h->e[i];

				h->e[i] = h->e[m];
				h->e[m] = t;
			}
			i = m;
		}
		return;
	}
	i = h->n++;
	h->e[i].score = score;
	h->e[i].id = id;
	while (i > 0)
	{
		int			p = (i - 1) / 2;

		if (h->e[p].score <= h->e[i].score)
			break;
		{
			HeapEnt		t = h->e[i];

			h->e[i] = h->e[p];
			h->e[p] = t;
		}
		i = p;
	}
}

/* (distance, cluster) for ordering probes.  Probe order is by L2 distance from
 * the query to the centroid, which is the rule that matches an L2 k-means
 * partition; inner-product ordering would be a different, wrong partition rule
 * because the centroids are not unit norm. */
typedef struct CDist
{
	float		d;
	int			id;
} CDist;

static int
cmp_cdist_asc(const void *a, const void *b)
{
	float		x = ((const CDist *) a)->d;
	float		y = ((const CDist *) b)->d;

	if (x < y)
		return -1;
	if (x > y)
		return 1;
	/* Ties broken by cluster id so the probe order is deterministic. */
	return ((const CDist *) a)->id - ((const CDist *) b)->id;
}

static int
cmp_ent_desc(const void *a, const void *b)
{
	double		x = ((const HeapEnt *) a)->score;
	double		y = ((const HeapEnt *) b)->score;

	return x < y ? 1 : (x > y ? -1 : 0);
}

/* ---------------------------------------------------------------------------
 * Exact scoring and ground truth
 * ------------------------------------------------------------------------- */

static double
exact_ip(const float *a, const float *b, int dim)
{
	double		s = 0;
	int			j;

	for (j = 0; j < dim; j++)
		s += (double) a[j] * (double) b[j];
	return s;
}

/* ---------------------------------------------------------------------------
 * IVF build: k-means over a sample, then one assignment pass.  Simple and
 * correct on purpose -- this is a measurement harness, not the shipping build.
 * Distances here use float accumulation because a partition is a partition; the
 * scored arms and the ground truth use double.
 * ------------------------------------------------------------------------- */

static float
l2sq(const float *a, const float *b, int dim)
{
	float		s = 0;
	int			j;

	for (j = 0; j < dim; j++)
	{
		float		d = a[j] - b[j];

		s += d * d;
	}
	return s;
}

static int
nearest(const float *v, const float *cen, int lists, int dim, float *outd)
{
	int			best = 0;
	float		bd = l2sq(v, cen, dim);
	int			i;

	for (i = 1; i < lists; i++)
	{
		float		d = l2sq(v, cen + (size_t) i * dim, dim);

		if (d < bd)
		{
			bd = d;
			best = i;
		}
	}
	if (outd != NULL)
		*outd = bd;
	return best;
}

static void
kmeans(const Corpus *c, int lists, int iters, int sample, float *cen)
{
	int			dim = c->dim;
	int		   *sid = xmalloc(sizeof(int) * (size_t) sample);
	int		   *asg = xmalloc(sizeof(int) * (size_t) sample);
	double	   *acc = xmalloc(sizeof(double) * (size_t) lists * (size_t) dim);
	int		   *cnt = xmalloc(sizeof(int) * (size_t) lists);
	int			i,
				j,
				it;

	for (i = 0; i < sample; i++)
		sid[i] = (int) (r64() % (unsigned long long) c->n);
	for (i = 0; i < lists; i++)
		memcpy(cen + (size_t) i * dim, c->v + (size_t) sid[i % sample] * dim,
			   sizeof(float) * (size_t) dim);

	for (it = 0; it < iters; it++)
	{
		memset(acc, 0, sizeof(double) * (size_t) lists * (size_t) dim);
		memset(cnt, 0, sizeof(int) * (size_t) lists);
		for (i = 0; i < sample; i++)
		{
			const float *v = c->v + (size_t) sid[i] * dim;

			asg[i] = nearest(v, cen, lists, dim, NULL);
			cnt[asg[i]]++;
			for (j = 0; j < dim; j++)
				acc[(size_t) asg[i] * dim + j] += v[j];
		}
		for (i = 0; i < lists; i++)
		{
			if (cnt[i] == 0)
			{
				/* Re-seed an empty cluster onto a random sample point rather
				 * than leaving it stranded: a dead cluster wastes a probe. */
				int			s = sid[(int) (r64() % (unsigned long long) sample)];

				memcpy(cen + (size_t) i * dim, c->v + (size_t) s * dim,
					   sizeof(float) * (size_t) dim);
				continue;
			}
			for (j = 0; j < dim; j++)
				cen[(size_t) i * dim + j] = (float) (acc[(size_t) i * dim + j] / cnt[i]);
		}
		fprintf(stderr, "\r  kmeans lists=%d iter %d/%d   ", lists, it + 1, iters);
	}
	fprintf(stderr, "\r  kmeans lists=%d done            \n", lists);
	free(sid);
	free(asg);
	free(acc);
	free(cnt);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

/*
 * Parse a comma-separated int list.  FATAL on more items than fit, never
 * truncating: this function used to stop at maxn and return, so
 * `windows=10,...,200` quietly became the first eight windows and a sweep could
 * report that no window reached a recall target while never having tested the
 * windows that would have.  MAXBITS_CFG above is sized from the header
 * specifically to dodge that for `bits=`, which is a fix at one call site for a
 * hazard that lives in this function -- so it is fixed here instead, for all
 * four lists.
 */
static int
parse_list(const char *s, int *out, int maxn, const char *what)
{
	int			n = 0;

	while (*s)
	{
		char	   *end;
		long		v = strtol(s, &end, 10);

		if (end == s)
			break;
		if (n == maxn)
		{
			fprintf(stderr, "ivf_recall: %s= has more than %d values; "
					"raise its MAX and rebuild rather than measuring a "
					"silently shortened sweep\n", what, maxn);
			exit(2);
		}
		out[n++] = (int) v;
		s = end;
		if (*s == ',')
			s++;
	}
	return n;
}

static void
usage(void)
{
	fprintf(stderr,
			"usage: ivf_recall glove <path> <nbase> <nq> [k=v ...]\n"
			"       ivf_recall fvecs <path> <nbase> <nq> [k=v ...]\n"
			"       ivf_recall synth <dim> <nclust> <sigma> <nbase> <nq> [k=v ...]\n"
			"options: lists=256,512,1024 bits=2,3,4,5,6,7,8 probes=1,2,4,...\n"
			"         windows=10,100,1000\n"
			"         k=10 iters=12 sampleper=32 seed=7 calib=0\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	Corpus		c;
	const char *mode;
	int			nbase,
				nq;
	int			K = 10;
	int			iters = 12;
	int			sampleper = 32;
	int			listcfg[MAXLISTS_CFG] = {256, 512, 1024};
	int			nlistcfg = 3;
	int			bitcfg[MAXBITS_CFG] = {2, 3, 4};
	int			nbitcfg = 3;
	int			probecfg[MAXPROBES] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024,
	2048, 4096};
	int			nprobecfg = 13;
	int			wincfg[MAXWIN] = {10, 100, 1000};
	int			nwincfg = 3;
	int			ai;
	int			li,
				bi,
				pi,
				wi,
				i,
				j;
	int		   *perm;
	float	   *base,
			   *qry;
	int		   *gtid;
	double	   *gtheta;
	int			wmax;
	int			docalib = 0;
	int			sanity_fail = 0;

	memset(&c, 0, sizeof(c));
	if (argc < 2)
		usage();
	mode = argv[1];

	if (strcmp(mode, "synth") == 0)
	{
		if (argc < 7)
			usage();
		ai = 7;
	}
	else if (strcmp(mode, "glove") == 0 || strcmp(mode, "fvecs") == 0)
	{
		if (argc < 5)
			usage();
		ai = 5;
	}
	else
	{
		usage();
		return 2;
	}

	for (; ai < argc; ai++)
	{
		const char *a = argv[ai];

		if (strncmp(a, "lists=", 6) == 0)
			nlistcfg = parse_list(a + 6, listcfg, MAXLISTS_CFG, "lists");
		else if (strncmp(a, "bits=", 5) == 0)
			nbitcfg = parse_list(a + 5, bitcfg, MAXBITS_CFG, "bits");
		else if (strncmp(a, "probes=", 7) == 0)
			nprobecfg = parse_list(a + 7, probecfg, MAXPROBES, "probes");
		else if (strncmp(a, "windows=", 8) == 0)
			nwincfg = parse_list(a + 8, wincfg, MAXWIN, "windows");
		else if (strncmp(a, "k=", 2) == 0)
			K = atoi(a + 2);
		else if (strncmp(a, "iters=", 6) == 0)
			iters = atoi(a + 6);
		else if (strncmp(a, "sampleper=", 10) == 0)
			sampleper = atoi(a + 10);
		else if (strncmp(a, "calib=", 6) == 0)
			docalib = atoi(a + 6);
		else if (strncmp(a, "seed=", 5) == 0)
			rs[3] = (unsigned long long) strtoull(a + 5, NULL, 10);
		else
			usage();
	}
	if (K < 1 || K > MAXK || nlistcfg < 1 || nbitcfg < 1 || nprobecfg < 1 ||
		nwincfg < 1)
		usage();

	/* Windows and probe counts are read as prefixes of one sorted walk, so they
	 * must be ascending, and the narrowest window cannot be narrower than k. */
	wmax = 0;
	for (wi = 0; wi < nwincfg; wi++)
	{
		if (wincfg[wi] < K || wincfg[wi] <= wmax)
			die("windows must be ascending and >= k");
		wmax = wincfg[wi];
	}
	for (pi = 1; pi < nprobecfg; pi++)
		if (probecfg[pi] <= probecfg[pi - 1])
			die("probes must be ascending");

	/* Reject an out-of-range width here rather than at weave_quantizer_init(),
	 * which only fires after the corpus has been read and clustered. */
	for (bi = 0; bi < nbitcfg; bi++)
		if (bitcfg[bi] < WEAVE_BITS_MIN || bitcfg[bi] > WEAVE_BITS_MAX)
			die("each bits= value must be within [WEAVE_BITS_MIN, WEAVE_BITS_MAX]");

	if (strcmp(mode, "synth") == 0)
	{
		int			dim = atoi(argv[2]);
		int			nclust = atoi(argv[3]);
		double		sigma = atof(argv[4]);

		nbase = atoi(argv[5]);
		nq = atoi(argv[6]);
		if (nbase < 1 || nq < 1)
			usage();
		gen_synth(&c, nbase + nq, dim, nclust, sigma);
		c.label = "synth";
		c.synthetic = 1;
		printf("# corpus: SYNTHETIC-GEOMETRY (not an embedding corpus): %d "
			   "isotropic Gaussian blobs, sigma=%.3f, dim=%d, native width\n",
			   nclust, sigma, dim);
	}
	else
	{
		nbase = atoi(argv[3]);
		nq = atoi(argv[4]);
		if (nbase < 1 || nq < 1)
			usage();
		if (strcmp(mode, "glove") == 0)
			load_glove(&c, argv[2], nbase + nq);
		else
			load_fvecs(&c, argv[2], nbase + nq);
		c.label = mode;
		c.synthetic = 0;
		printf("# corpus: REAL, %s, file %s, native dim %d, first %d rows\n",
			   mode, argv[2], c.dim, c.n);
	}
	normalize_rows(&c);
	if (c.n < nbase + nq)
	{
		if (c.n < nq + 100)
			die("corpus too small for the requested split");
		nbase = c.n - nq;
		fprintf(stderr, "  corpus short; nbase reduced to %d\n", nbase);
	}

	/*
	 * Split base from queries by a deterministic shuffle rather than by taking
	 * the tail: GloVe rows are frequency-ordered, so the tail is all rare words
	 * and a tail-held-out query set would be drawn from a different
	 * distribution than the base it searches.
	 */
	perm = xmalloc(sizeof(int) * (size_t) c.n);
	for (i = 0; i < c.n; i++)
		perm[i] = i;
	for (i = c.n - 1; i > 0; i--)
	{
		int			s = (int) (r64() % (unsigned long long) (i + 1));
		int			t = perm[i];

		perm[i] = perm[s];
		perm[s] = t;
	}
	base = xmalloc(sizeof(float) * (size_t) nbase * (size_t) c.dim);
	qry = xmalloc(sizeof(float) * (size_t) nq * (size_t) c.dim);
	for (i = 0; i < nbase; i++)
		memcpy(base + (size_t) i * c.dim, c.v + (size_t) perm[i] * c.dim,
			   sizeof(float) * (size_t) c.dim);
	for (i = 0; i < nq; i++)
		memcpy(qry + (size_t) i * c.dim, c.v + (size_t) perm[nbase + i] * c.dim,
			   sizeof(float) * (size_t) c.dim);
	free(c.v);
	c.v = NULL;
	free(perm);

	printf("# split: nbase=%d nq=%d (held out, excluded from base) dim=%d k=%d\n",
		   nbase, nq, c.dim, K);
	printf("# metric: inner product on L2-normalized rows (= cosine; ranks as L2 does)\n");
	printf("# ground truth: exact brute force over all %d base rows, double accumulation\n",
		   nbase);
	printf("# work proxy: candidates = vectors scored, summed over queries / nq\n");
	printf("# TQ+ calibration (sect. 5): %s\n", docalib ? "ON, fitted over 8192 rows" : "off");

	/* Ground truth. */
	gtid = xmalloc(sizeof(int) * (size_t) nq * (size_t) K);
	gtheta = xmalloc(sizeof(double) * (size_t) nq);
	{
		Heap		h;

		h.cap = K;
		h.e = xmalloc(sizeof(HeapEnt) * (size_t) K);
		for (i = 0; i < nq; i++)
		{
			heap_reset(&h);
			for (j = 0; j < nbase; j++)
				heap_push(&h, exact_ip(qry + (size_t) i * c.dim,
									   base + (size_t) j * c.dim, c.dim), j);
			qsort(h.e, (size_t) h.n, sizeof(HeapEnt), cmp_ent_desc);
			for (j = 0; j < K; j++)
				gtid[(size_t) i * K + j] = h.e[j].id;
			gtheta[i] = h.e[K - 1].score;
			if ((i % 16) == 0)
				fprintf(stderr, "\r  ground truth %d/%d   ", i, nq);
		}
		fprintf(stderr, "\r  ground truth done       \n");
		free(h.e);
	}

	printf("#\n# corpus\tdim\tnbase\tlists\tbits\tnprobe\tcand\tcandfrac"
		   "\tcellrec\tR_exact");
	for (wi = 0; wi < nwincfg; wi++)
		printf("\tR_w%d", wincfg[wi]);
	printf("\n");

	for (li = 0; li < nlistcfg; li++)
	{
		int			lists = listcfg[li];
		int			sample = sampleper * lists;
		Corpus		bc;
		float	   *cen;
		int		   *asg;
		int		   *cstart;
		int		   *cids;
		CDist	   *ord;
		int		   *invrank;
		int			nsnap = 0;
		int			snap[MAXPROBES + 1];

		if (lists < 1 || lists > nbase)
			die("lists out of range for this corpus");
		if (sample > nbase)
			sample = nbase;

		/*
		 * Snapshot list: the requested probe counts that fit, then lists
		 * itself.  nprobe == lists is not optional -- it is the row the harness
		 * self-check reads.
		 */
		for (pi = 0; pi < nprobecfg; pi++)
			if (probecfg[pi] >= 1 && probecfg[pi] < lists)
				snap[nsnap++] = probecfg[pi];
		snap[nsnap++] = lists;

		cen = xmalloc(sizeof(float) * (size_t) lists * (size_t) c.dim);
		bc.n = nbase;
		bc.dim = c.dim;
		bc.v = base;
		bc.label = c.label;
		bc.synthetic = c.synthetic;
		kmeans(&bc, lists, iters, sample, cen);

		/* Assignment pass over the whole base, then a CSR cluster directory. */
		asg = xmalloc(sizeof(int) * (size_t) nbase);
		cstart = xmalloc(sizeof(int) * (size_t) (lists + 1));
		cids = xmalloc(sizeof(int) * (size_t) nbase);
		for (i = 0; i <= lists; i++)
			cstart[i] = 0;
		for (i = 0; i < nbase; i++)
		{
			asg[i] = nearest(base + (size_t) i * c.dim, cen, lists, c.dim, NULL);
			cstart[asg[i] + 1]++;
			if ((i % 4096) == 0)
				fprintf(stderr, "\r  assign %d/%d   ", i, nbase);
		}
		fprintf(stderr, "\r  assign done          \n");
		for (i = 0; i < lists; i++)
			cstart[i + 1] += cstart[i];
		{
			int		   *fill = xmalloc(sizeof(int) * (size_t) lists);

			for (i = 0; i < lists; i++)
				fill[i] = cstart[i];
			for (i = 0; i < nbase; i++)
				cids[fill[asg[i]]++] = i;
			for (i = 0; i < lists; i++)
				if (fill[i] != cstart[i + 1])
					die("cluster directory inconsistent");
			free(fill);
		}

		ord = xmalloc(sizeof(CDist) * (size_t) lists);
		invrank = xmalloc(sizeof(int) * (size_t) lists);

		for (bi = 0; bi < nbitcfg; bi++)
		{
			int			bits = bitcfg[bi];
			WeaveQuantizer q;
			weave_uint8 *codes;
			float	   *scales;
			int			cb;
			double		acc_cand[MAXPROBES + 1];
			double		acc_cell[MAXPROBES + 1];
			double		acc_exact[MAXPROBES + 1];
			double		acc_win[MAXPROBES + 1][MAXWIN];
			Heap		hw,
						he,
						hr;
			HeapEnt    *sorted;
			int			si;

			WeaveCalibration cal;

			if (weave_quantizer_init(&q, c.dim, bits, NULL, malloc, free) != 0)
				die("weave_quantizer_init failed (bits or dim out of range)");

			/*
			 * Optional TQ+ calibration arm (sect. 5).  The fit wants ALREADY
			 * ROTATED unit vectors, so the uncalibrated quantizer above is built
			 * first purely to borrow its rotation, then the quantizer is rebuilt
			 * with the calibration attached.  Without this arm the quantization
			 * numbers below are the uncalibrated codebook's, and sect. 5 says
			 * calibration is expected to matter most exactly where the
			 * asymptotic-Beta assumption is weakest -- low dimension -- so
			 * reporting only the uncalibrated arm would leave the biggest
			 * available mitigation unmeasured.
			 */
			memset(&cal, 0, sizeof(cal));
			if (docalib)
			{
				int			ns = nbase < 8192 ? nbase : 8192;
				float	   *samp = xmalloc(sizeof(float) * (size_t) ns * (size_t) c.dim);

				if (ns < WEAVE_CALIB_MIN_ROWS)
					die("corpus too small to fit a calibration");
				for (i = 0; i < ns; i++)
				{
					/* rows are already unit norm; the fit needs them rotated */
					memcpy(samp + (size_t) i * c.dim,
						   base + (size_t) ((size_t) i * (size_t) nbase / (size_t) ns) * c.dim,
						   sizeof(float) * (size_t) c.dim);
					weave_rotate(&q.rot, samp + (size_t) i * c.dim);
				}
				if (weave_calibration_fit(&cal, &q.cb, samp, ns, c.dim, malloc) != 0)
					die("weave_calibration_fit failed");
				free(samp);
				weave_quantizer_free(&q, free);
				if (weave_quantizer_init(&q, c.dim, bits, &cal, malloc, free) != 0)
					die("weave_quantizer_init failed with calibration");
			}
			cb = q.codebytes;
			codes = xmalloc((size_t) nbase * (size_t) cb);
			scales = xmalloc(sizeof(float) * (size_t) nbase);
			for (i = 0; i < nbase; i++)
			{
				float		nrmv;

				if (weave_encode(&q, base + (size_t) i * c.dim,
								 codes + (size_t) i * cb, &nrmv, &scales[i]) != 0)
					die("weave_encode refused a row (zero norm should be gone)");
			}

			for (pi = 0; pi <= MAXPROBES; pi++)
			{
				acc_cand[pi] = acc_cell[pi] = acc_exact[pi] = 0;
				for (wi = 0; wi < MAXWIN; wi++)
					acc_win[pi][wi] = 0;
			}

			hw.cap = wmax;
			hw.e = xmalloc(sizeof(HeapEnt) * (size_t) wmax);
			he.cap = K;
			he.e = xmalloc(sizeof(HeapEnt) * (size_t) K);
			hr.cap = K;
			hr.e = xmalloc(sizeof(HeapEnt) * (size_t) K);
			sorted = xmalloc(sizeof(HeapEnt) * (size_t) wmax);

			for (i = 0; i < nq; i++)
			{
				const float *qv = qry + (size_t) i * c.dim;
				WeaveQueryLut lut;
				double		theta = gtheta[i];
				double		eps = 1e-9 * fabs(theta) + 1e-12;
				long		cand = 0;
				int			r;

				if (weave_query_lut_build(&lut, &q, qv, malloc) != 0)
					die("weave_query_lut_build failed");

				for (j = 0; j < lists; j++)
				{
					ord[j].d = l2sq(qv, cen + (size_t) j * c.dim, c.dim);
					ord[j].id = j;
				}
				qsort(ord, (size_t) lists, sizeof(CDist), cmp_cdist_asc);
				for (j = 0; j < lists; j++)
					invrank[ord[j].id] = j;

				heap_reset(&hw);
				heap_reset(&he);
				si = 0;
				for (r = 0; r < lists; r++)
				{
					int			cl = ord[r].id;
					int			m;

					for (m = cstart[cl]; m < cstart[cl + 1]; m++)
					{
						int			id = cids[m];

						cand++;
						heap_push(&hw, (double) weave_lut_score_code(&lut, bits,
																	 codes + (size_t) id * cb,
																	 scales[id]), id);
						heap_push(&he, exact_ip(qv, base + (size_t) id * c.dim,
												c.dim), id);
					}

					while (si < nsnap && snap[si] == r + 1)
					{
						int			hits = 0;
						int			cellhits = 0;
						int			nsort;
						int			w;

						acc_cand[si] += (double) cand;

						for (j = 0; j < K; j++)
							if (invrank[asg[gtid[(size_t) i * K + j]]] <= r)
								cellhits++;
						acc_cell[si] += (double) cellhits / K;

						for (j = 0; j < he.n; j++)
							if (he.e[j].score >= theta - eps)
								hits++;
						acc_exact[si] += (double) hits / K;

						/*
						 * The rerank arm.  Sort the compressed-domain window
						 * once, then walk it pushing exact scores into a K-heap
						 * and read off each requested window as a prefix --
						 * windows are nested exactly as probe counts are.
						 */
						nsort = hw.n;
						memcpy(sorted, hw.e, sizeof(HeapEnt) * (size_t) nsort);
						qsort(sorted, (size_t) nsort, sizeof(HeapEnt), cmp_ent_desc);
						heap_reset(&hr);
						wi = 0;
						for (w = 0; w < nsort && wi < nwincfg; w++)
						{
							heap_push(&hr, exact_ip(qv, base + (size_t) sorted[w].id * c.dim,
													c.dim), sorted[w].id);
							while (wi < nwincfg && wincfg[wi] == w + 1)
							{
								int			h2 = 0;

								for (j = 0; j < hr.n; j++)
									if (hr.e[j].score >= theta - eps)
										h2++;
								acc_win[si][wi] += (double) h2 / K;
								wi++;
							}
						}
						/* A window wider than the candidate set is the whole
						 * candidate set. */
						for (; wi < nwincfg; wi++)
						{
							int			h2 = 0;

							for (j = 0; j < hr.n; j++)
								if (hr.e[j].score >= theta - eps)
									h2++;
							acc_win[si][wi] += (double) h2 / K;
						}

						/*
						 * SELF-CHECK (AGENTS.md rule 8).  Probing every cluster
						 * and rescoring exactly is brute force by another route,
						 * so it must reproduce the ground truth for every single
						 * query.  If it does not, the partition, the probe order
						 * or the candidate walk is broken and no recall below is
						 * worth reading.
						 */
						if (snap[si] == lists)
						{
							if (hits != K || cellhits != K || cand != nbase)
							{
								fprintf(stderr,
										"SANITY FAILURE q=%d lists=%d bits=%d: "
										"exact hits=%d/%d cellhits=%d/%d cand=%ld/%d\n",
										i, lists, bits, hits, K, cellhits, K,
										cand, nbase);
								sanity_fail++;
							}
						}
						si++;
					}
				}
				if (si != nsnap)
					die("snapshot list not exhausted");
				free(lut._alloc);
				if ((i % 8) == 0)
					fprintf(stderr, "\r  sweep lists=%d bits=%d query %d/%d   ",
							lists, bits, i, nq);
			}
			fprintf(stderr, "\r  sweep lists=%d bits=%d done            \n",
					lists, bits);

			for (pi = 0; pi < nsnap; pi++)
			{
				printf("%s%s%s\t%d\t%d\t%d\t%d\t%d\t%.0f\t%.4f\t%.4f\t%.4f",
					   c.label, c.synthetic ? "(synthetic)" : "",
					   docalib ? "+tq+" : "", c.dim, nbase,
					   lists, bits, snap[pi], acc_cand[pi] / nq,
					   acc_cand[pi] / nq / nbase, acc_cell[pi] / nq,
					   acc_exact[pi] / nq);
				for (wi = 0; wi < nwincfg; wi++)
					printf("\t%.4f", acc_win[pi][wi] / nq);
				printf("\n");
			}
			fflush(stdout);

			free(hw.e);
			free(he.e);
			free(hr.e);
			free(sorted);
			free(codes);
			free(scales);
			weave_quantizer_free(&q, free);
			free(cal._alloc);
		}

		free(cen);
		free(asg);
		free(cstart);
		free(cids);
		free(ord);
		free(invrank);
	}

	printf("#\n# nprobe==lists self-check: %s\n",
		   sanity_fail == 0 ? "PASS (exact rescoring at full probe reproduces "
		   "brute force for every query)" : "FAIL");

	free(base);
	free(qry);
	free(gtid);
	free(gtheta);
	return sanity_fail == 0 ? 0 : 1;
}
