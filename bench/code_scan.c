/*-------------------------------------------------------------------------
 *
 * bench/code_scan.c
 *		What does the compressed-domain code scan cost, and does the block
 *		bound make it affordable?
 *
 * THE QUESTION THIS ANSWERS
 *
 * doc/PHASES.md's restated Phase V gate is met on recall and storage and
 * unmeasured on latency, and bench/RESULTS_PHASE_V_COLD.md names the reason: the
 * rerank half is affordable (86 ms cold for 20 candidates, 0.598 ms warm) but a
 * real query also scans every code, and V7/V8 do not exist, so no pg_weave vector
 * query can be timed end to end. At the ratified 4 bits, one million 1024-d
 * vectors is 512 MB of codes. Reading that per query at a few GB/s is tens of
 * milliseconds against pgvector HNSW's 3.548 ms warm, so a flat scan loses the
 * latency gate by an order of magnitude unless the block bound prunes hard.
 *
 * bench/RESULTS_BOUND_PRUNING.md says the bound prunes 99.6% -- but at dim=256,
 * over 8,192 SYNTHETIC vectors, where a "coherent" block is 32 perturbations of a
 * shared direction with sigma=0.35. That is an idealized cluster, not what
 * k-means on a real corpus produces, and the pruning rate is exactly the quantity
 * that would be flattered by it. This harness re-asks the question with real
 * vectors, a real clustering, real held-out queries, and a wall clock.
 *
 * TWO THINGS THE EARLIER HARNESS DID NOT MEASURE
 *
 * 1. THETA IS A RUNNING THRESHOLD, NOT AN ORACLE. The old harness compared every
 *    block's bound against the FINAL k-th best score. A real scan does not know
 *    that value until it finishes: it starts at -infinity and tightens as it goes,
 *    so a block that would have been pruned by the final threshold is scored
 *    anyway if it comes early. The oracle figure is an upper bound on what any
 *    block ordering could achieve; the running figure is what a scan in stored
 *    order actually gets. Both are reported, because the gap between them is
 *    precisely the value of block ordering, and quoting only the oracle overstates
 *    the bound by however large that gap is.
 *
 * 2. BYTES, NOT BLOCKS. "99.6% of blocks pruned" is not "99.6% of the time
 *    saved". A pruned block still costs its header -- the centroid code plus
 *    radius the bound is computed from -- and at 4 bits a centroid code is the
 *    same size as a lane's code. What decides latency is bytes touched, so that is
 *    reported alongside, for both arms.
 *
 * SOUNDNESS IS CHECKED BEFORE LATENCY IS REPORTED
 *
 * .agent/skills/weave-bench: a benchmark of a broken fast path is worse than no
 * benchmark, because it gets quoted -- pg_turbovec published a 2.3x win that was a
 * scalar-fallback bug and had to retract it. So the pruned arm's top-k must equal
 * the unpruned arm's top-k EXACTLY, per query, or this harness exits non-zero
 * before printing a single timing. A bound that drops a true top-k member makes
 * the scan faster and the answer wrong, which is the one failure mode that would
 * look like success.
 *
 * usage: code_scan <base.fvecs> <nbase> <nq> [k=v ...]
 *          bits=4 k=10 order=clustered|natural lists=1024 kernel=<name>
 *          queries=<path.fvecs>
 *
 * Memory: the float base is held to build the clustering and the centroids, so
 * roughly 4*dim*nbase bytes plus the codes. 1M x 960-d is ~4.4 GB.
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "weave/kernels.h"
#include "weave/quantize.h"

#define LANES WEAVE_VEC_BLOCK

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
	rs[3] = rs[3] << 45 | rs[3] >> 19;
	return r;
}

static void
die(const char *m)
{
	fprintf(stderr, "code_scan: %s\n", m);
	exit(2);
}

static void *
xmalloc(size_t n)
{
	void	   *p = malloc(n);

	if (!p)
		die("out of memory");
	return p;
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

/* ---------------------------------------------------------------- fvecs */

static int
fvecs_dim(const char *path)
{
	FILE	   *f = fopen(path, "rb");
	int32_t		d;

	if (!f)
		die("cannot open fvecs");
	if (fread(&d, 4, 1, f) != 1 || d <= 0 || d > 65536)
		die("bad fvecs header");
	fclose(f);
	return d;
}

/*
 * Read up to `n` rows, L2-normalizing and DROPPING zero-norm rows, which is what
 * bench/ivf_recall.c does -- weave_encode() refuses them, and GIST contains a
 * handful. Returns the number actually kept, so every downstream count is the
 * kept count and not the requested one.
 */
static long
fvecs_read(const char *path, int dim, long n, long skip, float *out)
{
	FILE	   *f = fopen(path, "rb");
	size_t		reclen = 4 + (size_t) dim * 4;
	long		kept = 0,
				dropped = 0;

	if (!f)
		die("cannot open fvecs");
	if (skip && fseek(f, (long) (skip * reclen), SEEK_SET) != 0)
		die("fseek");
	for (long i = 0; i < n; i++)
	{
		int32_t		d;
		float	   *v = out + (size_t) kept * dim;
		double		ss = 0;

		if (fread(&d, 4, 1, f) != 1)
			break;
		if (d != dim)
			die("ragged fvecs");
		if (fread(v, 4, (size_t) dim, f) != (size_t) dim)
			break;
		for (int j = 0; j < dim; j++)
			ss += (double) v[j] * v[j];
		if (ss <= 0)
		{
			dropped++;
			continue;
		}
		double		inv = 1.0 / sqrt(ss);

		for (int j = 0; j < dim; j++)
			v[j] = (float) (v[j] * inv);
		kept++;
	}
	fclose(f);
	if (dropped)
		fprintf(stderr, "  dropped %ld zero-norm rows\n", dropped);
	return kept;
}

/* ---------------------------------------------------------------- top-k */

typedef struct
{
	float		score;
	int			id;
} Hit;

static int
hitcmp(const void *a, const void *b)
{
	float		x = ((const Hit *) a)->score,
				y = ((const Hit *) b)->score;

	if (x < y)
		return 1;
	if (x > y)
		return -1;
	/* id breaks ties so the two arms' outputs are comparable elementwise even
	 * when several vectors share a score, which quantized codes make common. */
	return ((const Hit *) a)->id - ((const Hit *) b)->id;
}

/* Insert into a k-sized min-at-top list kept as an unsorted array + running
 * threshold. Simple and honest: the scan's cost is the block loop, not this. */
typedef struct
{
	Hit		   *h;
	int			k,
				n;
	float		theta;			/* k-th best so far, or -inf while n < k */
} TopK;

static void
topk_init(TopK *t, int k)
{
	t->h = xmalloc(sizeof(Hit) * k);
	t->k = k;
	t->n = 0;
	t->theta = -INFINITY;
}

static void
topk_push(TopK *t, float s, int id)
{
	if (t->n < t->k)
	{
		t->h[t->n].score = s;
		t->h[t->n].id = id;
		t->n++;
		if (t->n == t->k)
		{
			qsort(t->h, t->n, sizeof(Hit), hitcmp);
			t->theta = t->h[t->k - 1].score;
		}
		return;
	}
	if (s <= t->theta)
		return;
	t->h[t->k - 1].score = s;
	t->h[t->k - 1].id = id;
	qsort(t->h, t->k, sizeof(Hit), hitcmp);
	t->theta = t->h[t->k - 1].score;
}

/* ---------------------------------------------------------------- k-means */

/*
 * Crude Lloyd over a sample, then a full assignment pass. Deliberately the same
 * shape as bench/ivf_recall.c's: the point is a REALISTIC clustering, not a good
 * one. A better clustering can only raise the pruning rate, so the figure this
 * produces is a floor, and a floor is the useful direction for a gate.
 */
static void
kmeans_assign(const float *base, long n, int dim, int lists, int iters,
			  int *assign)
{
	float	   *cent = xmalloc((size_t) lists * dim * sizeof(float));
	double	   *acc = xmalloc((size_t) lists * dim * sizeof(double));
	long	   *cnt = xmalloc((size_t) lists * sizeof(long));

	for (int c = 0; c < lists; c++)
		memcpy(cent + (size_t) c * dim, base + (r64() % (unsigned long long) n) * dim,
			   (size_t) dim * sizeof(float));

	for (int it = 0; it < iters; it++)
	{
		memset(acc, 0, (size_t) lists * dim * sizeof(double));
		memset(cnt, 0, (size_t) lists * sizeof(long));
		for (long i = 0; i < n; i++)
		{
			const float *v = base + (size_t) i * dim;
			int			bestc = 0;
			double		best = -INFINITY;

			for (int c = 0; c < lists; c++)
			{
				const float *cv = cent + (size_t) c * dim;
				double		ip = 0;

				for (int j = 0; j < dim; j++)
					ip += (double) v[j] * cv[j];
				if (ip > best)
				{
					best = ip;
					bestc = c;
				}
			}
			assign[i] = bestc;
			cnt[bestc]++;
			for (int j = 0; j < dim; j++)
				acc[(size_t) bestc * dim + j] += v[j];
		}
		for (int c = 0; c < lists; c++)
		{
			if (!cnt[c])
				continue;
			double		ss = 0;

			for (int j = 0; j < dim; j++)
			{
				double		m = acc[(size_t) c * dim + j] / cnt[c];

				cent[(size_t) c * dim + j] = (float) m;
				ss += m * m;
			}
			if (ss > 0)
			{
				double		inv = 1.0 / sqrt(ss);

				for (int j = 0; j < dim; j++)
					cent[(size_t) c * dim + j] *= inv;
			}
		}
		fprintf(stderr, "\r  kmeans lists=%d iter %d/%d   ", lists, it + 1, iters);
	}
	fprintf(stderr, "\r  kmeans lists=%d done          \n", lists);
	free(cent);
	free(acc);
	free(cnt);
}

static int  *g_assign;
static int
bycluster(const void *a, const void *b)
{
	int			x = *(const int *) a,
				y = *(const int *) b;

	if (g_assign[x] != g_assign[y])
		return g_assign[x] - g_assign[y];
	return x - y;
}

/* ---------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
	if (argc < 4)
		die("usage: code_scan <base.fvecs> <nbase> <nq> [bits=4 k=10 "
			"order=clustered|natural lists=1024 kernel=<name> queries=<path>]");

	const char *basepath = argv[1];
	long		nreq = atol(argv[2]);
	int			nq = atoi(argv[3]);
	int			bits = 4,
				K = 10,
				lists = 1024,
				iters = 8,
				clustered = 1;
	const char *kernelname = NULL,
			   *qpath = NULL;

	for (int i = 4; i < argc; i++)
	{
		char	   *a = argv[i];

		if (!strncmp(a, "bits=", 5))
			bits = atoi(a + 5);
		else if (!strncmp(a, "k=", 2))
			K = atoi(a + 2);
		else if (!strncmp(a, "lists=", 6))
			lists = atoi(a + 6);
		else if (!strncmp(a, "iters=", 6))
			iters = atoi(a + 6);
		else if (!strncmp(a, "kernel=", 7))
			kernelname = a + 7;
		else if (!strncmp(a, "queries=", 8))
			qpath = a + 8;
		else if (!strncmp(a, "order=", 6))
			clustered = !strcmp(a + 6, "clustered");
		else
			die("unknown option");
	}
	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		die("bits out of range");

	int			dim = fvecs_dim(basepath);

	fprintf(stderr, "  dim=%d, reading up to %ld rows\n", dim, nreq);
	float	   *base = xmalloc((size_t) nreq * dim * sizeof(float));
	long		n = fvecs_read(basepath, dim, nreq, 0, base);

	if (n < LANES)
		die("corpus too small");
	fprintf(stderr, "  kept %ld rows\n", n);

	/* ---- order the warp ------------------------------------------------ */
	int		   *perm = xmalloc((size_t) n * sizeof(int));

	for (long i = 0; i < n; i++)
		perm[i] = (int) i;
	if (clustered)
	{
		int		   *assign = xmalloc((size_t) n * sizeof(int));

		kmeans_assign(base, n, dim, lists, iters, assign);
		g_assign = assign;
		qsort(perm, (size_t) n, sizeof(int), bycluster);
		/* assign is not freed: g_assign aliases it and nothing after this
		 * point sorts again, but leaking it deliberately is cheaper than a
		 * dangling pointer if that ever changes. */
	}

	/* ---- encode -------------------------------------------------------- */
	WeaveQuantizer q;

	if (weave_quantizer_init(&q, dim, bits, NULL, malloc, free) != 0)
		die("quantizer_init");

	long		nblk = (n + LANES - 1) / LANES;
	size_t		blkbytes = (size_t) weave_block_codebytes(dim, bits);
	size_t		lanebytes = ((size_t) dim * bits + 7) / 8;

	fprintf(stderr, "  encoding %ld vectors -> %ld blocks, %.1f MB of codes\n",
			n, nblk, (double) nblk * blkbytes / 1048576.0);

	weave_uint8 *codes = xmalloc((size_t) nblk * blkbytes);
	float	   *scales = xmalloc((size_t) nblk * LANES * sizeof(float));
	/* The slack tail of each block is never written by weave_pack_lane (see
	 * weave_block_codebytes' comment), and a kernel never reads it, but leaving
	 * it uninitialized would make this harness's timings depend on malloc's
	 * history under a tool like valgrind.  Zero it once. */
	memset(codes, 0, (size_t) nblk * blkbytes);

	weave_uint8 *code = xmalloc(q.codebytes);
	float	   *rec = xmalloc((size_t) LANES * dim * sizeof(float));
	float	   *cen = xmalloc((size_t) nblk * dim * sizeof(float));
	float	   *rad = xmalloc((size_t) nblk * sizeof(float));
	float	   *smax = xmalloc((size_t) nblk * sizeof(float));
	float	   *mrec = xmalloc((size_t) nblk * sizeof(float));
	int		   *lanevec = xmalloc((size_t) nblk * LANES * sizeof(int));
	int		   *nlanes = xmalloc((size_t) nblk * sizeof(int));

	for (long b = 0; b < nblk; b++)
	{
		int			m = (int) (n - b * LANES);

		if (m > LANES)
			m = LANES;
		nlanes[b] = m;
		smax[b] = 0;
		mrec[b] = 0;

		for (int s = 0; s < m; s++)
		{
			int			vi = perm[b * LANES + s];
			float		norm,
						scale;

			if (weave_encode(&q, base + (size_t) vi * dim, code, &norm, &scale) != 0)
				die("encode refused a vector");
			weave_pack_lane(WEAVE_PACK_LANE, dim, bits, codes + (size_t) b * blkbytes,
							s, code);
			scales[b * LANES + s] = scale;
			lanevec[b * LANES + s] = vi;
			weave_decode(&q, code, scale, rec + (size_t) s * dim);

			if (scale > smax[b])
				smax[b] = scale;
			double		rn = 0;

			for (int j = 0; j < dim; j++)
				rn += (double) rec[(size_t) s * dim + j] * rec[(size_t) s * dim + j];
			rn = sqrt(rn);
			if (rn > mrec[b])
				mrec[b] = (float) rn;
		}
		/* Centroid and radius over the block's RECONSTRUCTIONS, because the
		 * bound is asserted about reconstructed scores, not about the original
		 * vectors.  Using the originals would give a bound that is tighter and
		 * unsound. */
		for (int j = 0; j < dim; j++)
		{
			double		a = 0;

			for (int s = 0; s < m; s++)
				a += rec[(size_t) s * dim + j];
			cen[(size_t) b * dim + j] = (float) (a / m);
		}
		rad[b] = 0;
		for (int s = 0; s < m; s++)
		{
			double		d = 0;

			for (int j = 0; j < dim; j++)
			{
				double		t = (double) rec[(size_t) s * dim + j] - cen[(size_t) b * dim + j];

				d += t * t;
			}
			d = sqrt(d);
			if (d > rad[b])
				rad[b] = (float) d;
		}
		if ((b & 1023) == 0)
			fprintf(stderr, "\r  encode %ld/%ld   ", b, nblk);
	}
	fprintf(stderr, "\r  encode done            \n");

	/* The centroid is stored as a quantized code in the real format (contract
	 * C3: the bound reads only header-resident data), so quantize it here too --
	 * scoring a float centroid would make the bound tighter than the shipping
	 * one and the pruning rate optimistic. */
	weave_uint8 *cencode = xmalloc((size_t) nblk * q.codebytes);
	float	   *censcale = xmalloc((size_t) nblk * sizeof(float));

	for (long b = 0; b < nblk; b++)
	{
		float		norm,
					scale;

		if (weave_encode(&q, cen + (size_t) b * dim, cencode + (size_t) b * q.codebytes,
						 &norm, &scale) != 0)
		{
			/* A degenerate all-zero centroid cannot be encoded.  Zero the code
			 * and give the bound a scale of 0, which makes <q,c> zero and leaves
			 * (B2)/(B1) to carry the block -- looser, never unsound. */
			memset(cencode + (size_t) b * q.codebytes, 0, q.codebytes);
			scale = 0;
		}
		censcale[b] = scale;
	}

	/* ---- queries ------------------------------------------------------- */
	float	   *qv;
	long		nqv;

	if (qpath)
	{
		int			qd = fvecs_dim(qpath);

		if (qd != dim)
			die("query dim != base dim");
		qv = xmalloc((size_t) nq * dim * sizeof(float));
		nqv = fvecs_read(qpath, dim, nq, 0, qv);
	}
	else
	{
		/* Held out from the tail of the base file, which is why nbase must be
		 * below the file's row count for this path to be a true hold-out. */
		qv = xmalloc((size_t) nq * dim * sizeof(float));
		nqv = fvecs_read(basepath, dim, nq, nreq, qv);
	}
	if (nqv < nq)
		die("too few query vectors");

	const WeaveScoreKernel *kern = kernelname ? weave_score_kernel_lookup(kernelname)
		: weave_score_kernel_best();

	if (!kern)
		die("no such kernel on this host");

	/* ---- scan ---------------------------------------------------------- */
	float	   *out = xmalloc(sizeof(float) * LANES);
	double		t_flat = 0,
				t_prune = 0;
	double		sum_pruned_run = 0,
				sum_pruned_oracle = 0;
	double		sum_lanes = 0;
	long		violations = 0,
				mismatches = 0;
	/* Diagnostics.  A pruning rate of zero is indistinguishable by inspection
	 * from a harness that computes the bound wrongly, so the components are
	 * reported and can be checked against physics: for unit vectors every score
	 * and every bound must lie in roughly [-1, 1], and a bound must never be
	 * below the block's best lane. */
	double		sum_theta = 0,
				sum_b1 = 0,
				sum_b2 = 0,
				sum_b3 = 0,
				sum_bmin = 0,
				sum_rad = 0,
				sum_cens = 0,
				sum_best = 0;
	Hit		   *ref = xmalloc(sizeof(Hit) * K);

	for (int t = 0; t < nq; t++)
	{
		WeaveQueryLut lut;

		if (weave_query_lut_build(&lut, &q, qv + (size_t) t * dim, malloc) != 0)
			die("lut_build");

		/* --- arm A: flat, every block, every lane --------------------- */
		TopK		a;

		topk_init(&a, K);
		double		t0 = now();

		for (long b = 0; b < nblk; b++)
		{
			WeaveScoreBlock blk = {0};

			blk.lut = &lut;
			blk.layout = WEAVE_PACK_LANE;
			blk.codes = codes + (size_t) b * blkbytes;
			blk.scales = scales + b * LANES;
			blk.scalestride = 1;
			blk.nlanes = nlanes[b];
			blk.livemask = nlanes[b] == 32 ? 0xffffffffu : ((1u << nlanes[b]) - 1);
			blk.firstwarp = 0;
			blk.allow = NULL;
			blk.nwarp = 0;
			if (kern->score_block(&blk, out) < 0)
				die("score_block rejected a block");
			for (int s = 0; s < nlanes[b]; s++)
				topk_push(&a, out[s], lanevec[b * LANES + s]);
		}
		t_flat += now() - t0;
		memcpy(ref, a.h, sizeof(Hit) * a.n);
		float		theta_final = a.theta;

		/* --- arm B: bound-pruned, running theta ----------------------- */
		TopK		p;

		topk_init(&p, K);
		long		pruned = 0,
					scored_lanes = 0;

		t0 = now();
		for (long b = 0; b < nblk; b++)
		{
			float		censcore = weave_lut_score_code(&lut, bits,
														cencode + (size_t) b * q.codebytes,
														censcale[b]);
			float		bound = weave_block_bound_ip(&lut, smax[b], mrec[b],
													 censcore, rad[b]);

			if (bound <= p.theta)
			{
				pruned++;
				continue;
			}
			WeaveScoreBlock blk = {0};

			blk.lut = &lut;
			blk.layout = WEAVE_PACK_LANE;
			blk.codes = codes + (size_t) b * blkbytes;
			blk.scales = scales + b * LANES;
			blk.scalestride = 1;
			blk.nlanes = nlanes[b];
			blk.livemask = nlanes[b] == 32 ? 0xffffffffu : ((1u << nlanes[b]) - 1);
			if (kern->score_block(&blk, out) < 0)
				die("score_block rejected a block");
			scored_lanes += nlanes[b];
			for (int s = 0; s < nlanes[b]; s++)
				topk_push(&p, out[s], lanevec[b * LANES + s]);
		}
		t_prune += now() - t0;

		/* --- soundness, before any number is believed ----------------- */
		if (p.n != a.n)
			mismatches++;
		else
			for (int i = 0; i < K; i++)
				if (ref[i].id != p.h[i].id || ref[i].score != p.h[i].score)
				{
					mismatches++;
					break;
				}

		/* Oracle pruning: what the bound would achieve against the FINAL
		 * threshold, i.e. the best any block ordering could do.  Also the
		 * per-block soundness assertion, which is about the bound and not about
		 * the traversal. */
		long		oracle = 0;

		for (long b = 0; b < nblk; b++)
		{
			float		censcore = weave_lut_score_code(&lut, bits,
														cencode + (size_t) b * q.codebytes,
														censcale[b]);
			float		bound = weave_block_bound_ip(&lut, smax[b], mrec[b],
													 censcore, rad[b]);

			if (bound <= theta_final)
				oracle++;
			/* The bound must dominate every lane in the block. */
			WeaveScoreBlock blk = {0};

			blk.lut = &lut;
			blk.layout = WEAVE_PACK_LANE;
			blk.codes = codes + (size_t) b * blkbytes;
			blk.scales = scales + b * LANES;
			blk.scalestride = 1;
			blk.nlanes = nlanes[b];
			blk.livemask = nlanes[b] == 32 ? 0xffffffffu : ((1u << nlanes[b]) - 1);
			if (kern->score_block(&blk, out) < 0)
				die("score_block rejected a block");
			for (int s = 0; s < nlanes[b]; s++)
				if (out[s] > bound + 1e-5f * fabsf(bound))
					violations++;
		}

		/* Bound components, averaged over blocks, for this query. */
		{
			double		ab1 = 0, ab2 = 0, ab3 = 0, abm = 0, acs = 0;

			for (long b = 0; b < nblk; b++)
			{
				float		cs = weave_lut_score_code(&lut, bits,
													  cencode + (size_t) b * q.codebytes,
													  censcale[b]);
				double		b3 = cs + lut.qnorm * rad[b];
				double		b2 = mrec[b] * lut.qnorm;
				double		b1 = smax[b] * lut.lutbound;
				double		bm = b3 < b2 ? b3 : b2;

				if (b1 < bm)
					bm = b1;
				ab1 += b1;
				ab2 += b2;
				ab3 += b3;
				abm += bm;
				acs += cs;
			}
			sum_b1 += ab1 / nblk;
			sum_b2 += ab2 / nblk;
			sum_b3 += ab3 / nblk;
			sum_bmin += abm / nblk;
			sum_cens += acs / nblk;
		}
		sum_theta += theta_final;
		sum_best += ref[0].score;
		{
			double		ar = 0;

			for (long b = 0; b < nblk; b++)
				ar += rad[b];
			sum_rad += ar / nblk;
		}
		sum_pruned_run += (double) pruned / nblk;
		sum_pruned_oracle += (double) oracle / nblk;
		sum_lanes += (double) scored_lanes / n;
		free(lut._alloc);
		fprintf(stderr, "\r  query %d/%d   ", t + 1, nq);
	}
	fprintf(stderr, "\r  scan done            \n");

	if (violations)
	{
		fprintf(stderr, "BOUND VIOLATIONS: %ld -- the bound is UNSOUND, no timing "
				"below this line means anything\n", violations);
		return 1;
	}
	if (mismatches)
	{
		fprintf(stderr, "TOP-K MISMATCHES: %d of %d queries -- the pruned arm "
				"returns different results from the flat arm, so it is faster and "
				"wrong\n", (int) mismatches, nq);
		return 1;
	}

	/* Bytes: the flat arm touches every block's codes.  The pruned arm touches
	 * every block's HEADER (a quantized centroid code plus a radius and two
	 * scalars) and only the scored blocks' codes.  Header bytes are what makes
	 * "99% of blocks pruned" less than a 99% saving. */
	double		hdr = (double) q.codebytes + 4 * sizeof(float);
	double		flat_mb = (double) nblk * blkbytes / 1048576.0;
	double		prune_mb = ((double) nblk * hdr
							+ (1.0 - sum_pruned_run / nq) * nblk * blkbytes) / 1048576.0;

	printf("# corpus %s, n=%ld dim=%d bits=%d k=%d nq=%d\n", basepath, n, dim, bits, K, nq);
	printf("# warp order: %s%s\n", clustered ? "CLUSTERED (k-means, lists=" : "NATURAL (file order)",
		   clustered ? "" : "");
	if (clustered)
		printf("#   lists=%d iters=%d\n", lists, iters);
	printf("# kernel: %s\n", kern->name);
	printf("# blocks=%ld lanes/block=%d block bytes=%zu lane bytes=%zu\n",
		   nblk, LANES, blkbytes, lanebytes);
	printf("# soundness: PASS (no bound violation, pruned top-k == flat top-k on all %d queries)\n", nq);
	printf("\n");
	printf("why: mean over blocks and queries -- a bound prunes only if it is BELOW theta\n");
	printf("  best score (top-1)         : %8.4f\n", sum_best / nq);
	printf("  theta (k-th best)          : %8.4f   <-- the bar a bound must get under\n", sum_theta / nq);
	printf("  bound B1 (LUT)             : %8.4f\n", sum_b1 / nq);
	printf("  bound B2 (Cauchy-Schwarz)  : %8.4f\n", sum_b2 / nq);
	printf("  bound B3 (centroid+radius) : %8.4f   = <q,c> %.4f + |q|*R, mean R = %.4f\n",
		   sum_b3 / nq, sum_cens / nq, sum_rad / nq);
	printf("  bound min of three         : %8.4f\n", sum_bmin / nq);
	printf("\n");
	printf("blocks pruned, running theta : %6.2f%%\n", 100 * sum_pruned_run / nq);
	printf("blocks pruned, oracle theta  : %6.2f%%   (upper bound over all block orderings)\n",
		   100 * sum_pruned_oracle / nq);
	printf("lanes scored / n             : %6.2f%%\n", 100 * sum_lanes / nq);
	printf("\n");
	printf("flat scan   p50-ish mean     : %8.3f ms   (%.1f MB touched)\n",
		   1000 * t_flat / nq, flat_mb);
	printf("pruned scan p50-ish mean     : %8.3f ms   (%.1f MB touched)\n",
		   1000 * t_prune / nq, prune_mb);
	printf("speedup                      : %8.2fx\n", t_flat / t_prune);
	return 0;
}
