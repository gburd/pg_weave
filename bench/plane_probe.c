/*-------------------------------------------------------------------------
 * bench/plane_probe.c
 *		How large a shortlist does a SIGN-PLANE first stage need, on pg_weave's
 *		own 4-bit codec?  (doc/PHASES.md V18; turbovec's P1 probe, re-asked here)
 *
 * turbovec 1.1.0's biggest win (x1.72 over 32 cells, then x1.87 with tuning)
 * is a staged 4-bit search: scan only the SIGN bit of every coordinate for a
 * shortlist, rank the shortlist on the lower bit planes, rescore the best
 * exactly.  It reads a quarter of the code bytes in the first stage.  Its own
 * probe (turbovec benchmarks/hillclimb/LOG_search.md, round 2, P1) measured the
 * shortlist the sign stage needs for 99.9 % of queries to keep the exact top-k:
 * about 104 at k=10 out of 100K on OpenAI-1536, 180 on mpnet-768.
 *
 * pg_weave's codec is different -- its own rotation, its own Lloyd-Max codebook
 * (symmetric, 16 levels at 4 bits, sign = the top code bit), per-lane scale --
 * so turbovec's numbers do not transfer.  Hard rule 9: measure the thing the
 * design rests on before building on it.  This harness does turbovec's P1 for
 * our codes, with no index and no writer change:
 *
 *	 1. encode every base vector with pg_weave's quantizer (weave_encode)
 *	 2. per query: the exact 4-bit score of every vector (the score the shipped
 *	    kernels compute, via the float LUT) and its top-k
 *	 3. three coarser estimates of every vector's score:
 *	      sign    sum_j q'_j * s_j * m                (1 bit/coord, a quarter)
 *	      top2    sum_j q'_j * level(sign, msb)        (2 bits/coord, a half)
 *	      linear  turbovec's alpha*sgn + sum beta_j*rho_j fit (all 4 bits, linear)
 *	    where q' is the rotated query, s_j the sign bit, m the mean |level|
 *	 4. per query, the RANK the estimate assigns to the worst of the exact top-k:
 *	    that is the shortlist this query needs.  Report the 50 / 90 / 99 / 99.9
 *	    percentiles across queries, at k = 1, 10, 100.
 *
 * Usage: plane_probe <base.fvecs> <query.fvecs> <filedim> <n> <nq> [usedim]
 * GIST-1M is the corpus the vector gates are stated on (bench/aws/run.sh fetch_gist).
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/quantize.h"

static void
die(const char *m)
{
	fprintf(stderr, "plane_probe: %s\n", m);
	exit(2);
}

static void *
xmalloc(size_t n)
{
	void	   *p = malloc(n ? n : 1);

	if (!p)
		die("out of memory");
	return p;
}

/* fvecs reader, L2-normalizing, dropping all-zero rows (bench/code_scan.c's rule) */
static long
fvecs_read(const char *path, int filedim, long n, float *out, int usedim)
{
	FILE	   *f = fopen(path, "rb");
	float	   *rec = xmalloc((size_t) filedim * sizeof(float));
	long		kept = 0;

	if (!f)
		die("cannot open fvecs");
	for (long i = 0; i < n; i++)
	{
		int32_t		d;
		double		ss = 0;
		float	   *v = out + (size_t) kept * usedim;

		if (fread(&d, 4, 1, f) != 1)
			break;
		if (d != filedim)
			die("ragged fvecs");
		if (fread(rec, 4, (size_t) filedim, f) != (size_t) filedim)
			break;
		for (int j = 0; j < usedim; j++)
		{
			v[j] = rec[j];
			ss += (double) v[j] * v[j];
		}
		if (ss <= 0)
			continue;
		for (int j = 0; j < usedim; j++)
			v[j] = (float) (v[j] / sqrt(ss));
		kept++;
	}
	fclose(f);
	free(rec);
	return kept;
}

/* 4-bit code c of coordinate j: low nibble first, two per byte (pack-independent) */
static inline int
code_at(const uint8_t *code, int j)
{
	return (code[j >> 1] >> ((j & 1) * 4)) & 0xF;
}

typedef struct
{
	float		s;
	int			i;
} Scored;

static int
cmp_desc(const void *a, const void *b)
{
	float		x = ((const Scored *) a)->s,
				y = ((const Scored *) b)->s;

	return (x < y) - (x > y);
}

static int
cmp_int(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

/* percentile of an int array (sorted in place) */
static int
pct(int *v, int n, double p)
{
	int			idx;

	qsort(v, n, sizeof(int), cmp_int);
	idx = (int) ceil(p * n) - 1;
	if (idx < 0)
		idx = 0;
	if (idx >= n)
		idx = n - 1;
	return v[idx];
}

int
main(int argc, char **argv)
{
	int			filedim, usedim, dim;
	long		n, nq;
	float	   *base, *qv;
	WeaveQuantizer qz;
	uint8_t    *codes;
	float	   *scales;
	const int	K[3] = {1, 10, 100};

	if (argc < 6)
		die("usage: plane_probe base.fvecs query.fvecs filedim n nq [usedim]");
	filedim = atoi(argv[3]);
	n = atol(argv[4]);
	nq = atol(argv[5]);
	usedim = argc > 6 ? atoi(argv[6]) : filedim;
	dim = usedim;

	base = xmalloc((size_t) n * dim * sizeof(float));
	n = fvecs_read(argv[1], filedim, n, base, dim);
	qv = xmalloc((size_t) nq * dim * sizeof(float));
	nq = fvecs_read(argv[2], filedim, nq, qv, dim);
	fprintf(stderr, "loaded n=%ld nq=%ld dim=%d\n", n, nq, dim);

	if (weave_quantizer_init(&qz, dim, 4, NULL, malloc, free) != 0)
		die("quantizer init");

	codes = xmalloc((size_t) n * qz.codebytes);
	scales = xmalloc((size_t) n * sizeof(float));
	for (long i = 0; i < n; i++)
		if (weave_encode(&qz, base + (size_t) i * dim, codes + (size_t) i * qz.codebytes,
						 NULL, &scales[i]) != 0)
			die("encode");

	/*
	 * Turbovec's linear model, fitted the way pack::planes_stats fits it: a
	 * level as alpha*sgn + sum_j beta_j*rho_j with sgn, rho_j = +-1, weighted
	 * least squares over level frequencies (here: from the encoded corpus).
	 * And the sign stage's weight m = mean |level|; the two-plane model
	 * alpha1*sgn + beta1*rho_top for the "top2" stage.
	 */
	double		hist[16] = {0};
	double		m = 0, alpha = 0, beta[3] = {0}, alpha1 = 0, beta1 = 0;
	const float *C = qz.cb.centroid;

	for (long i = 0; i < n; i++)
		for (int j = 0; j < dim; j++)
			hist[code_at(codes + (size_t) i * qz.codebytes, j)] += 1;
	{
		double		tot = 0, ata[4][4] = {{0}}, atb[4] = {0};
		double		w_all = 0, w_sr = 0, b_s = 0, b_r = 0;

		for (int c = 0; c < 16; c++)
			tot += hist[c];
		for (int c = 0; c < 16; c++)
		{
			double		w = (hist[c] + 0.5) / tot;
			double		x[4];

			m += (hist[c] / tot) * fabs(C[c]);
			x[0] = (c >> 3) ? 1.0 : -1.0;
			for (int b = 0; b < 3; b++)
				x[1 + b] = ((c >> b) & 1) ? 1.0 : -1.0;
			w_all += w;
			w_sr += w * x[0] * x[3];
			b_s += w * x[0] * C[c];
			b_r += w * x[3] * C[c];
			for (int a = 0; a < 4; a++)
			{
				atb[a] += w * x[a] * C[c];
				for (int b = 0; b < 4; b++)
					ata[a][b] += w * x[a] * x[b];
			}
		}
		/* solve the 4x4 normal equations by Gauss-Jordan */
		for (int col = 0; col < 4; col++)
		{
			int			piv = col;

			for (int r = col + 1; r < 4; r++)
				if (fabs(ata[r][col]) > fabs(ata[piv][col]))
					piv = r;
			for (int c2 = 0; c2 < 4; c2++)
			{
				double		t = ata[col][c2];

				ata[col][c2] = ata[piv][c2];
				ata[piv][c2] = t;
			}
			{
				double		t = atb[col];

				atb[col] = atb[piv];
				atb[piv] = t;
			}
			for (int r = 0; r < 4; r++)
			{
				double		f;

				if (r == col)
					continue;
				f = ata[r][col] / ata[col][col];
				for (int c2 = 0; c2 < 4; c2++)
					ata[r][c2] -= f * ata[col][c2];
				atb[r] -= f * atb[col];
			}
		}
		alpha = atb[0] / ata[0][0];
		for (int b = 0; b < 3; b++)
			beta[b] = atb[1 + b] / ata[1 + b][1 + b];
		/* two-feature fit [sgn, rho_top]: 2x2 normal equations */
		{
			double		det = w_all * w_all - w_sr * w_sr;

			alpha1 = (b_s * w_all - b_r * w_sr) / det;
			beta1 = (b_r * w_all - b_s * w_sr) / det;
		}
		/* how well does the linear model reproduce each level? */
		double		maxerr = 0;

		for (int c = 0; c < 16; c++)
		{
			double		est = alpha * ((c >> 3) ? 1 : -1);

			for (int b = 0; b < 3; b++)
				est += beta[b] * (((c >> b) & 1) ? 1 : -1);
			if (fabs(est - C[c]) > maxerr)
				maxerr = fabs(est - C[c]);
		}
		printf("codebook: levels %.4f .. %.4f, mean|level| m = %.5f\n", C[0], C[15], m);
		printf("linear model: alpha %.5f beta %.5f %.5f %.5f  (max level error %.5f = %.1f%% of max level)\n",
			   alpha, beta[0], beta[1], beta[2], maxerr, 100.0 * maxerr / C[15]);
		printf("two-plane model: alpha1 %.5f beta1 %.5f\n", alpha1, beta1);
	}

	/* per query and per stage, the shortlist needed at each k */
	int		   *need[4][3];			/* [stage][k] -> per-query rank */

	for (int s = 0; s < 4; s++)
		for (int k = 0; k < 3; k++)
			need[s][k] = xmalloc((size_t) nq * sizeof(int));

	/*
	 * Queries are independent, so they run in parallel (OpenMP; -fopenmp).  Each
	 * thread owns its scratch arrays, and every per-query result lands in its own
	 * slot of need[][][qi], so the output is identical to a serial run's for any
	 * thread count -- the per-query numbers do not depend on scheduling, and the
	 * percentiles are taken after the loop.  weave_query_lut_build() reads the
	 * quantizer and writes only its own stack and its own allocation.  The 960-d,
	 * n = 1M arm took ~7 h serially (the reason this exists: that arm was lost to
	 * a host reboot on 2026-10-06 and had to be re-run).
	 */
#pragma omp parallel
	{
	Scored	   *ex = xmalloc((size_t) n * sizeof(Scored));
	Scored	   *est = xmalloc((size_t) n * sizeof(Scored));
	int		   *rank_of = xmalloc((size_t) n * sizeof(int));
	float	   *qr = xmalloc((size_t) dim * sizeof(float));

#pragma omp for schedule(dynamic, 1)
	for (long qi = 0; qi < nq; qi++)
	{
		WeaveQueryLut lut;

		if (weave_query_lut_build(&lut, &qz, qv + (size_t) qi * dim, malloc) != 0)
			die("lut");
		/* the rotated, calibrated query is lut[j][c] / C[c]; recover it from the
		 * table so the estimates use exactly the query the kernels use */
		for (int j = 0; j < dim; j++)
			qr[j] = lut.lut[j * 16 + 15] / C[15];

		/* exact 4-bit scores (the LUT gather the shipped kernels compute) */
		for (long i = 0; i < n; i++)
		{
			const uint8_t *code = codes + (size_t) i * qz.codebytes;
			double		acc = 0;

			for (int j = 0; j < dim; j++)
				acc += lut.lut[j * 16 + code_at(code, j)];
			ex[i].s = (float) (acc * scales[i]);
			ex[i].i = (int) i;
		}
		qsort(ex, n, sizeof(Scored), cmp_desc);

		for (int stage = 0; stage < 4; stage++)
		{
			for (long i = 0; i < n; i++)
			{
				const uint8_t *code = codes + (size_t) i * qz.codebytes;
				double		acc = 0;

				for (int j = 0; j < dim; j++)
				{
					int			c = code_at(code, j);
					double		sg = (c >> 3) ? 1.0 : -1.0;
					double		lv;

					if (stage == 0)
						lv = sg * m;
					else if (stage == 1)
						lv = alpha1 * sg + beta1 * (((c >> 2) & 1) ? 1.0 : -1.0);
					else if (stage == 2)
					{
						lv = alpha * sg;
						for (int b = 0; b < 3; b++)
							lv += beta[b] * (((c >> b) & 1) ? 1.0 : -1.0);
					}
					else
						lv = C[c];	/* HARNESS CONTROL: the exact level; must need exactly k */
					acc += qr[j] * lv;
				}
				est[i].s = (float) (acc * scales[i]);
				est[i].i = (int) i;
			}
			qsort(est, n, sizeof(Scored), cmp_desc);
			for (long r = 0; r < n; r++)
				rank_of[est[r].i] = (int) r + 1;
			for (int kk = 0; kk < 3; kk++)
			{
				int			worst = 0;

				for (int t = 0; t < K[kk] && t < n; t++)
					if (rank_of[ex[t].i] > worst)
						worst = rank_of[ex[t].i];
				need[stage][kk][qi] = worst;
			}
		}
		free(lut._alloc);
	}
	free(ex);
	free(est);
	free(rank_of);
	free(qr);
	}

	static const char *sname[4] = {"sign (1 bit, 1/4 bytes)", "top 2 bits (1/2)", "all 4, linear", "CONTROL exact (must = k)"};

	printf("\nshortlist needed so the estimate's top-L holds the exact 4-bit top-k, n=%ld nq=%ld dim=%d\n",
		   n, nq, dim);
	printf("%-26s %4s %8s %8s %8s %8s %8s\n", "first stage", "k", "p50", "p90", "p99", "p99.9", "max");
	for (int s = 0; s < 4; s++)
		for (int kk = 0; kk < 3; kk++)
		{
			int		   *v = need[s][kk];

			printf("%-26s %4d %8d %8d %8d %8d %8d\n", sname[s], K[kk],
				   pct(v, nq, 0.50), pct(v, nq, 0.90), pct(v, nq, 0.99),
				   pct(v, nq, 0.999), pct(v, nq, 1.0));
		}
	return 0;
}
