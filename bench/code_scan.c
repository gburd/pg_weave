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
 *        code_scan selfcheck=<nblocks>
 *
 * The second form needs no corpus: it is the differential gate on the two
 * byte-LUT kernels this harness adds (lut-byte-ref and lut-byte, see the
 * "byte-LUT kernels" section), and it exits non-zero if the AVX2 kernel is not
 * bit-identical to its scalar reference.
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
 *
 * `filedim` is the file's dimensionality; `usedim <= filedim` is how many leading
 * coordinates to keep, and the norm is taken over THOSE, so a truncated corpus is
 * a proper unit-norm corpus of the lower dimensionality rather than a set of
 * partial vectors. Truncation is legitimate here and not a shortcut: the
 * quantizer's rotation makes coordinates exchangeable, which is the same property
 * V15's prefix stage rests on.
 *
 * One consequence to keep in view when comparing across `usedim`: the zero-norm
 * test now applies to the truncated vector, so a row whose leading coordinates are
 * all zero is dropped at small `usedim` and kept at large. The kept count is
 * therefore a function of usedim, which is why the caller reports it per point.
 */
static long
fvecs_read(const char *path, int filedim, long n, long skip, float *out,
		   int usedim)
{
	FILE	   *f = fopen(path, "rb");
	size_t		reclen = 4 + (size_t) filedim * 4;
	long		kept = 0,
				dropped = 0;
	float	   *rec;

	if (!f)
		die("cannot open fvecs");
	rec = xmalloc((size_t) filedim * sizeof(float));
	if (skip && fseek(f, (long) (skip * reclen), SEEK_SET) != 0)
		die("fseek");
	for (long i = 0; i < n; i++)
	{
		int32_t		d;
		float	   *v = out + (size_t) kept * usedim;
		double		ss = 0;

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
		{
			dropped++;
			continue;
		}
		double		inv = 1.0 / sqrt(ss);

		for (int j = 0; j < usedim; j++)
			v[j] = (float) (v[j] * inv);
		kept++;
	}
	free(rec);
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

/*
 * Stage 2 groups its survivors by block, which needs them ordered by block.
 * qsort's comparator cannot take the candidate array or the position table as
 * arguments, so they are handed over here.  File statics are acceptable in a
 * single-threaded harness and are set immediately before each qsort call.
 */
static const long *g_s2_vipos;
static const Hit *g_s2_cand;

/* orders candidate INDICES by the block their vector lives in */
static int
cand_by_block(const void *a, const void *b)
{
	long		pa = g_s2_vipos[g_s2_cand[*(const int *) a].id];
	long		pb = g_s2_vipos[g_s2_cand[*(const int *) b].id];

	return (pa > pb) - (pa < pb);
}

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

/*
 * Top-k as a binary MIN-HEAP: the root is the weakest survivor, so the running
 * threshold is h[0].score and an accepted insert costs O(log k).
 *
 * This was a sorted array with a qsort per accepted insert, which is fine at
 * k = 10 and catastrophic at the k = 8000-20000 a prefix scan's stage-1 window
 * needs: accepted inserts run to about k*ln(n/k), so at n = 1M and k = 8000 the
 * bookkeeping was ~4e9 operations and it reported 7.6 SECONDS for a stage that
 * should cost ~70 ms. The recall numbers were unaffected -- they do not depend on
 * how the set is maintained -- but every timing was the harness measuring itself.
 * Worth stating because the failure looked exactly like a slow algorithm.
 */
typedef struct
{
	Hit		   *h;
	int			k,
				n;
	float		theta;			/* weakest survivor, or -inf while n < k */
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
	int			i;

	if (t->n < t->k)
	{
		/* sift up toward the root, which holds the minimum */
		i = t->n++;
		t->h[i].score = s;
		t->h[i].id = id;
		while (i > 0)
		{
			int			par = (i - 1) / 2;

			if (t->h[par].score <= t->h[i].score)
				break;
			Hit			tmp = t->h[par];

			t->h[par] = t->h[i];
			t->h[i] = tmp;
			i = par;
		}
		if (t->n == t->k)
			t->theta = t->h[0].score;
		return;
	}
	if (s <= t->theta)
		return;
	t->h[0].score = s;
	t->h[0].id = id;
	for (i = 0;;)
	{
		int			l = 2 * i + 1,
					r = l + 1,
					m = i;

		if (l < t->n && t->h[l].score < t->h[m].score)
			m = l;
		if (r < t->n && t->h[r].score < t->h[m].score)
			m = r;
		if (m == i)
			break;
		Hit			tmp = t->h[m];

		t->h[m] = t->h[i];
		t->h[i] = tmp;
		i = m;
	}
	t->theta = t->h[0].score;
}

/* Heap order is not rank order.  Anything that reads h[] positionally -- an
 * elementwise comparison of two arms, or h[0] as the best hit -- must call this
 * first. */
static void
topk_finish(TopK *t)
{
	qsort(t->h, (size_t) t->n, sizeof(Hit), hitcmp);
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
/*
 * Kernel lookup, approximate-ness and listing for this harness.
 *
 * lut-byte-ref and lut-byte used to be a private prototype duplicated here
 * (byte-quantized query table, scalar reference and AVX2 vpshufb gather).
 * Task V16 ported both into src/vector/kernels.c against the shipped
 * WeaveQueryLut.lut8 (see weave/quantize.h), so this harness now reaches them
 * exactly the way the backend does: by name, through the real registry, with
 * `approximate` read off WeaveScoreKernel rather than compared against a
 * pointer to a kernel that no longer exists here.
 */
static const WeaveScoreKernel *
bench_kernel_lookup(const char *name)
{
	return weave_score_kernel_lookup(name);
}

/* True for the byte-LUT kernels (and any other kernel marked approximate):
 * their scores differ from the oracle's by the query table's quantization.
 * Two of this harness's assertions compare a score against a bound derived
 * from the EXACT float table, so they have to know. */
static int
bench_kernel_is_approx(const WeaveScoreKernel *k)
{
	return k->approximate;
}

/* The byte kernels are 4-bit-only; at other widths they refuse every block,
 * so listing them would just abort the run.  weave_score_kernel_list()
 * already includes them (gated on AVX2 for lut-byte), so this only has to
 * filter, not add. */
static int
bench_kernel_list(const WeaveScoreKernel **out, int max, int bits)
{
	int			n = weave_score_kernel_list(out, max);
	int			i,
				w = 0;

	for (i = 0; i < n; i++)
	{
		if (out[i]->approximate && bits != 4)
			continue;
		out[w++] = out[i];
	}
	return w;
}

/* --------------------------------------------------------------- self-check
 *
 * THE GATE ON THE TWO KERNELS ABOVE.  AGENTS.md rule 8: verify correctness before
 * recording a latency, because a benchmark of a broken fast path is worse than no
 * benchmark.  Two DIFFERENT questions, and conflating them is the mistake this
 * separates:
 *
 * 1. lut-byte vs lut-byte-ref must be BIT-IDENTICAL.  Same integer accumulator,
 *    same reconstruction expression, and integer addition is associative, so
 *    there is no tolerance to argue about.  Any difference is a SIMD bug -- a
 *    wrong nibble, a wrong 128-bit half, or a 16-bit overflow -- and it FAILS
 *    this harness with a non-zero exit.
 *
 * 2. lut-byte-ref vs the exact `scalar` oracle is REPORTED, never asserted.  That
 *    deviation IS the 8-bit query table's rounding error; it is the number this
 *    measurement exists to produce, so treating it as a failure would be
 *    measuring the tolerance instead of the kernel.
 *
 * The blocks are randomized over the things that go wrong: dim parity, the
 * 256-coordinate flush boundary, short blocks, dead lanes, an allowlist at a
 * non-zero and non-64-aligned firstwarp, and a scalestride above 1.  Codes are
 * written through weave_pack_lane(), so the layout the kernels assume is the
 * layout pack.c produces and not a restatement of it.
 *
 * WHAT THE RANDOM POPULATION DOES NOT REACH, and why there is a deterministic set
 * as well: a 16-bit accumulator only wraps if the byte values are large, and a
 * global step means most rows quantize to small numbers.  Raising
 * LUT8_FLUSH_PAIRS to 4096 -- i.e. removing the widening this kernel exists to
 * get right -- passed 600 random blocks.  The saturating set below fails it
 * immediately.  A gate that a known bug walks through is worse than no gate.
 * ------------------------------------------------------------------------- */

/* Dims that bracket every structural boundary in the AVX2 kernel: odd/even, the
 * flush period (256 coordinates = 128 pair-iterations), and dims past the point
 * where a 16-bit accumulator would overflow without the flush. */
static const int selfcheck_dims[] = {
	1, 2, 3, 4, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128,
	254, 255, 256, 257, 258, 511, 512, 513, 959, 960, 961, 1024
};

#define SELFCHECK_NDIMS ((int) (sizeof(selfcheck_dims) / sizeof(selfcheck_dims[0])))
#define SELFCHECK_MAXDIM 1024

/*
 * Self-check accumulators.
 *
 * Deviation is tracked in TWO populations, because one number covering both
 * would be the adversarial one and would then get quoted as if it were the real
 * one:
 *
 *	 [0] REAL: the table comes out of weave_query_lut_build() for a random unit
 *		 query and the codes out of weave_encode() on random unit vectors -- the
 *		 distribution a query actually produces, and the one the recall question
 *		 is about.
 *	 [1] STRESS: tables built to break a single global step (one row 500x wider
 *		 than the rest, so every other row quantizes to almost nothing), plus the
 *		 saturating set.  Not distributions this codec produces; they are here to
 *		 show what the failure mode looks like, and they are labelled.
 *
 * The bit-identity assertion does not care which population a block came from
 * and runs over both.
 */
static double sc_maxabs[2],
			sc_maxrel[2];
static int	sc_maxabs_dim[2],
			sc_maxrel_dim[2];
static long sc_ncmp_dev[2],
			sc_ncmp_bits,
			sc_nbitfail;

/* A random unit vector: what weave_encode() and weave_query_lut_build() are fed
 * everywhere else in this harness (fvecs_read L2-normalizes). */
static void
selfcheck_unit(float *v, int dim)
{
	double		ss;
	int			j;

	do
	{
		ss = 0.0;
		for (j = 0; j < dim; j++)
		{
			/* Box-Muller would be tidier; a sum of three uniforms is close
			 * enough to Gaussian for a direction and needs no logf. */
			double		u = (double) (r64() >> 11) / 9007199254740992.0
				+ (double) (r64() >> 11) / 9007199254740992.0
				+ (double) (r64() >> 11) / 9007199254740992.0 - 1.5;

			v[j] = (float) u;
			ss += u * u;
		}
	} while (ss <= 0.0);
	ss = 1.0 / sqrt(ss);
	for (j = 0; j < dim; j++)
		v[j] = (float) (v[j] * ss);
}

/*
 * Score one prepared block with all three kernels and record both comparisons.
 * `p` selects the deviation population; `tag` only appears in a failure message.
 */
static void
selfcheck_block(const WeaveScoreBlock *blk, int p, const char *tag)
{
	const WeaveScoreKernel *ref = bench_kernel_lookup("lut-byte-ref");
	const WeaveScoreKernel *simd = bench_kernel_lookup("lut-byte");
	float		o_exact[LANES],
				o_ref[LANES],
				o_simd[LANES];
	int			nlanes = blk->nlanes;
	int			s;

	if (!ref)
		die("self-check: lut-byte-ref is not registered on this host");

	for (s = 0; s < LANES; s++)
		o_exact[s] = o_ref[s] = o_simd[s] = 0.0f;

	if (weave_score_kernel_scalar.score_block(blk, o_exact) != nlanes ||
		ref->score_block(blk, o_ref) != nlanes)
		die("self-check: a kernel refused a well-formed block");

	if (simd)
	{
		if (simd->score_block(blk, o_simd) != nlanes)
			die("self-check: lut-byte refused a well-formed block");

		/* Bit-identical, so compare the BITS.  memcmp also settles the sentinel
		 * lanes and would catch a NaN one path produced and the other did not,
		 * which == would not. */
		for (s = 0; s < nlanes; s++)
		{
			sc_ncmp_bits++;
			if (memcmp(&o_ref[s], &o_simd[s], sizeof(float)) != 0)
			{
				if (sc_nbitfail < 8)
					fprintf(stderr,
							"  BIT MISMATCH %s dim=%d lane=%d nlanes=%d "
							"stride=%d allow=%d: ref=%.9g simd=%.9g\n",
							tag, blk->lut->dim, s, nlanes, blk->scalestride,
							blk->allow != NULL, (double) o_ref[s],
							(double) o_simd[s]);
				sc_nbitfail++;
			}
		}
	}

	for (s = 0; s < nlanes; s++)
	{
		double		a,
					e;

		if (o_ref[s] == WEAVE_KERNEL_NEVER || o_exact[s] == WEAVE_KERNEL_NEVER)
		{
			/* Both paths mask the same lanes, so one sentinel implies the other;
			 * assert that rather than skipping quietly. */
			if (o_ref[s] != o_exact[s])
				die("self-check: the byte path masked a different lane set than "
					"the oracle");
			continue;
		}
		sc_ncmp_dev[p]++;
		e = (double) o_exact[s];
		a = fabs((double) o_ref[s] - e);
		if (a > sc_maxabs[p])
		{
			sc_maxabs[p] = a;
			sc_maxabs_dim[p] = blk->lut->dim;
		}
		if (e != 0.0)
		{
			double		rel = a / fabs(e);

			if (rel > sc_maxrel[p])
			{
				sc_maxrel[p] = rel;
				sc_maxrel_dim[p] = blk->lut->dim;
			}
		}
	}
}

/*
 * The deterministic saturating set: lut[j][c] = c, so the global range is 15,
 * every row quantizes to c * 17, and a code of all-15 nibbles makes EVERY
 * coordinate contribute the maximum 255.  At dim = 1024 the true accumulator is
 * 261120, four times what a 16-bit lane holds, so a kernel that does not widen
 * inside the loop wraps and this fails.  (The reconstruction is exact here --
 * step * 255 == 15 == lut[j][15] -- so it contributes nothing to the deviation
 * figures, only to the bit-identity one.)
 */
static const int selfcheck_satdims[] = {258, 511, 512, 513, 959, 960, 961, 1024};

#define SELFCHECK_NSAT ((int) (sizeof(selfcheck_satdims) / sizeof(selfcheck_satdims[0])))

static int
bench_selfcheck(int nblocks)
{
	const WeaveScoreKernel *simd = bench_kernel_lookup("lut-byte");
	weave_uint8 *codes = xmalloc((size_t) weave_block_codebytes(SELFCHECK_MAXDIM, 4));
	weave_uint8 *code = xmalloc(((size_t) SELFCHECK_MAXDIM * 4 + 7) / 8);
	float	   *scales = xmalloc((size_t) LANES * 4 * sizeof(float));
	float	   *fvec = xmalloc((size_t) SELFCHECK_MAXDIM * sizeof(float));
	weave_uint64 allow[4];
	float		o_ref[LANES],
				o_simd[LANES];
	int			nfellback = 0;
	int			it;

	if (nblocks < 1)
		nblocks = 200;

	printf("# self-check: %d random blocks + %d saturating blocks, bits=4, "
		   "layout=WEAVE_PACK_LANE\n", nblocks, SELFCHECK_NSAT);
	printf("# lut-byte: %s\n", simd ? "AVX2, present" :
		   "ABSENT (not compiled with -mavx2; no scalar alias, by design)");

	/* ---- the saturating set --------------------------------------------
	 *
	 * lut8[j][c] = round((lut[j][c] - mn_j) / step), step set by the
	 * GLOBAL range of the table (weave/quantize.h).  lut[j][c] = x[j] *
	 * centroid[c] with x the ROTATED query and centroid the (shared, one
	 * per quantizer) codebook, so a query whose rotated coordinates are
	 * the same positive constant everywhere makes EVERY row of lut[]
	 * literally identical: each row's own range then equals the global
	 * range, and each row's top code (c = nlevels - 1, the codebook's
	 * largest centroid since centroid[] is ascending) quantizes to exactly
	 * 255.  A code of all-1111 nibbles then makes EVERY coordinate
	 * contribute the maximum 255, so at dim = 1024 the true accumulator is
	 * 261120 -- four times what a 16-bit lane holds -- and a kernel that
	 * does not widen inside the loop wraps and this fails.
	 *
	 * The rotated-constant query is built by running the INVERSE rotation
	 * on a constant vector: weave_query_lut_build() applies weave_rotate()
	 * to whatever query it is handed, and weave_rotate_inverse() is its
	 * exact left inverse (up to float rounding, nowhere near the 1/2-ulp
	 * margin lrintf needs to still land on 255 -- see selfcheck_block()'s
	 * bit-identity check, which is what would catch it if it didn't).
	 * Built through weave_query_lut_build(), not a hand-rolled table, so
	 * lut8 is the one the shipped kernels actually read.
	 */
	for (it = 0; it < SELFCHECK_NSAT; it++)
	{
		WeaveQuantizer qz;
		WeaveQueryLut lut;
		WeaveScoreBlock blk;
		int			dim = selfcheck_satdims[it];
		char		tag[32];
		int			j,
					s;

		memset(&qz, 0, sizeof(qz));
		if (weave_quantizer_init(&qz, dim, 4, NULL, malloc, free) != 0)
			die("self-check: quantizer_init refused a saturating dim");

		for (j = 0; j < dim; j++)
			fvec[j] = 1.0f;
		weave_rotate_inverse(&qz.rot, fvec);

		memset(&lut, 0, sizeof(lut));
		if (weave_query_lut_build(&lut, &qz, fvec, malloc) != 0)
			die("self-check: query_lut_build refused a saturating query");

		memset(codes, 0xFF, (size_t) weave_block_codebytes(dim, 4));
		for (s = 0; s < LANES; s++)
			scales[s] = 1.0f;

		memset(&blk, 0, sizeof(blk));
		blk.lut = &lut;
		blk.layout = WEAVE_PACK_LANE;
		blk.codes = codes;
		blk.scales = scales;
		blk.scalestride = 1;
		blk.nlanes = LANES;
		blk.livemask = 0xffffffffu;

		snprintf(tag, sizeof(tag), "sat[%d]", it);
		selfcheck_block(&blk, 1, tag);

		free(lut._alloc);
		weave_quantizer_free(&qz, free);
	}

	/* ---- the random population ----------------------------------------- */
	for (it = 0; it < nblocks; it++)
	{
		WeaveQuantizer qz;
		WeaveQueryLut lut;
		WeaveScoreBlock blk;
		int			dim = (it < SELFCHECK_NDIMS) ? selfcheck_dims[it]
			: selfcheck_dims[r64() % SELFCHECK_NDIMS];
		int			nlanes = 1 + (int) (r64() % LANES);
		int			stride = 1 + (int) (r64() % 3);
		int			shape = (int) (r64() % 3);
		int			useallow = (int) (r64() % 2);
		weave_uint32 livemask = (weave_uint32) r64();
		int			real = (it % 3) != 0;	/* two thirds real, one third stress */
		char		tag[32];
		int			s,
					j;

		memset(&qz, 0, sizeof(qz));
		memset(&lut, 0, sizeof(lut));
		for (s = 0; s < LANES * stride; s++)
		{
			double		u = (double) (r64() >> 11) / 9007199254740992.0;

			/* Occasionally 0: a lane whose scale is zero must still reconstruct
			 * identically in both kernels. */
			scales[s] = (r64() % 16 == 0) ? 0.0f : (float) (0.05 + 4.0 * u);
		}
		memset(codes, 0, (size_t) weave_block_codebytes(dim, 4));

		if (weave_quantizer_init(&qz, dim, 4, NULL, malloc, free) != 0)
		{
			/* weave_codebook_solve declines dim < 4.  There is no
			 * WeaveQueryLut to build for such a dim -- lut8 comes only from
			 * weave_query_lut_build(), which needs a quantizer -- so the
			 * iteration is skipped rather than answered with a hand-rolled
			 * table.  Counted and reported rather than skipped silently, so
			 * the block count in the summary is honest. */
			nfellback++;
			continue;
		}

		if (real)
		{
			/* --- the real distribution ------------------------------------ */
			selfcheck_unit(fvec, dim);
			if (weave_query_lut_build(&lut, &qz, fvec, malloc) != 0)
				die("self-check: query_lut_build refused a unit query");
			for (s = 0; s < LANES; s++)
			{
				float		norm,
							sc;

				selfcheck_unit(fvec, dim);
				if (weave_encode(&qz, fvec, code, &norm, &sc) != 0)
					die("self-check: encode refused a unit vector");
				weave_pack_lane(WEAVE_PACK_LANE, dim, 4, codes, s, code);
				scales[(size_t) s * stride] = sc;
			}
		}
		else
		{
			/*
			 * --- a stress QUERY, through the real builder, and uniformly
			 * random codes ---
			 *
			 * lut[j][c] = x[j] * centroid[c] (weave/quantize.h), x the
			 * ROTATED query, so the only lever available without a hand-
			 * built table is what x looks like -- and weave_rotate_inverse()
			 * gets to choose that directly: feed it the desired POST-
			 * rotation vector and it returns the query that reproduces it
			 * (up to float rounding), the same trick the saturating set uses.
			 */
			for (j = 0; j < dim; j++)
			{
				double		u = (double) (r64() >> 11) / 9007199254740992.0;

				switch (shape)
				{
					case 0:
						/* One rotated coordinate far wider than the rest, so
						 * the global step is set by an outlier and every
						 * other coordinate quantizes coarsely. */
						fvec[j] = (float) ((j == dim / 2 ? 500.0 : 1.0) *
											(2.0 * u - 1.0));
						break;
					case 1:
						/* The all-zero query: x rotates to all zero, so the
						 * table's global range is exactly 0, the step
						 * guard's case, which must come back bit-exact. */
						fvec[j] = 0.0f;
						break;
					default:
						fvec[j] = (float) (u * u * u * 4.0 - 2.0);
						break;
				}
			}
			weave_rotate_inverse(&qz.rot, fvec);
			if (weave_query_lut_build(&lut, &qz, fvec, malloc) != 0)
				die("self-check: query_lut_build refused a stress query");

			for (s = 0; s < LANES; s++)
			{
				size_t		nb = ((size_t) dim * 4 + 7) / 8;
				size_t		b;

				for (b = 0; b < nb; b++)
					code[b] = (weave_uint8) r64();
				weave_pack_lane(WEAVE_PACK_LANE, dim, 4, codes, s, code);
			}
		}


		for (s = 0; s < 4; s++)
			allow[s] = r64();

		memset(&blk, 0, sizeof(blk));
		blk.lut = &lut;
		blk.layout = WEAVE_PACK_LANE;
		blk.codes = codes;
		blk.scales = scales;
		blk.scalestride = stride;
		blk.nlanes = nlanes;
		blk.livemask = livemask;
		if (useallow)
		{
			/* A firstwarp that is neither zero nor 64-aligned, so the
			 * two-word allow extract in the shipped kernels is exercised. */
			blk.firstwarp = (weave_uint32) (r64() % 70);
			blk.nwarp = blk.firstwarp + (weave_uint32) nlanes +
				(weave_uint32) (r64() % 20);
			if (blk.nwarp > 4 * 64)
				blk.nwarp = 4 * 64;
			if (blk.firstwarp + (weave_uint32) nlanes > blk.nwarp)
				blk.firstwarp = blk.nwarp - (weave_uint32) nlanes;
			blk.allow = allow;
		}

		snprintf(tag, sizeof(tag), "it=%d", it);
		selfcheck_block(&blk, real ? 0 : 1, tag);

		free(lut._alloc);
		weave_quantizer_free(&qz, free);
	}

	/*
	 * Negative cases: the contract says an inconsistent block description
	 * returns -1 without writing anything.  Cheap to check, and the alternative
	 * is a kernel that indexes a number that came off a page.
	 */
	{
		WeaveQuantizer qz;
		WeaveQueryLut lut;
		WeaveScoreBlock blk;
		const WeaveScoreKernel *ref = bench_kernel_lookup("lut-byte-ref");
		int			bad = 0;
		int			j;

		if (!ref)
			die("self-check: lut-byte-ref is not registered on this host");

		/* A real WeaveQueryLut, so blk->lut->lut8 is the one the shipped
		 * kernels actually check for -- a hand-built table with no lut8 would
		 * be refused for the wrong reason and every case below would "pass"
		 * without exercising block_bits() at all. */
		memset(&qz, 0, sizeof(qz));
		if (weave_quantizer_init(&qz, 8, 4, NULL, malloc, free) != 0)
			die("self-check: quantizer_init refused dim=8");
		for (j = 0; j < 8; j++)
			fvec[j] = 0.25f;
		memset(&lut, 0, sizeof(lut));
		if (weave_query_lut_build(&lut, &qz, fvec, malloc) != 0)
			die("self-check: query_lut_build refused the contract-test query");

		memset(&blk, 0, sizeof(blk));
		blk.lut = &lut;
		blk.layout = WEAVE_PACK_LANE;
		blk.codes = codes;
		blk.scales = scales;
		blk.scalestride = 1;
		blk.nlanes = 8;
		blk.livemask = 0xffu;

		{
			WeaveScoreBlock b2 = blk;

			b2.nlanes = 0;
			if (ref->score_block(&b2, o_ref) != -1)
				bad++;
			if (simd && simd->score_block(&b2, o_simd) != -1)
				bad++;
		}
		{
			WeaveScoreBlock b2 = blk;

			b2.nlanes = WEAVE_VEC_BLOCK + 1;
			if (ref->score_block(&b2, o_ref) != -1)
				bad++;
			if (simd && simd->score_block(&b2, o_simd) != -1)
				bad++;
		}
		{
			WeaveScoreBlock b2 = blk;

			b2.layout = (WeavePackLayout) 7;
			if (ref->score_block(&b2, o_ref) != -1)
				bad++;
			if (simd && simd->score_block(&b2, o_simd) != -1)
				bad++;
		}
		{
			/* A firstwarp that would read `allow` past nwarp. */
			WeaveScoreBlock b2 = blk;

			b2.allow = allow;
			b2.nwarp = 8;
			b2.firstwarp = 4;
			if (ref->score_block(&b2, o_ref) != -1)
				bad++;
			if (simd && simd->score_block(&b2, o_simd) != -1)
				bad++;
		}
		{
			/* A 2-bit table: refused outright, NOT delegated to the oracle. */
			WeaveQueryLut l2 = lut;
			WeaveScoreBlock b2 = blk;

			l2.nlevels = 4;
			b2.lut = &l2;
			if (ref->score_block(&b2, o_ref) != -1)
				bad++;
			if (simd && simd->score_block(&b2, o_simd) != -1)
				bad++;
		}
		if (bad)
		{
			fprintf(stderr, "CONTRACT FAILURES: %d rejections not reported as -1\n",
					bad);
			sc_nbitfail += bad;
		}

		free(lut._alloc);
		weave_quantizer_free(&qz, free);
	}

	printf("\n");
	printf("lut-byte vs lut-byte-ref (must be bit-identical)\n");
	if (!simd)
		printf("  SKIPPED: lut-byte is not present in this build\n");
	else
	{
		printf("  lane comparisons           : %ld\n", sc_ncmp_bits);
		printf("  bit mismatches             : %ld   %s\n", sc_nbitfail,
			   sc_nbitfail ? "<-- FAIL" : "(pass)");
	}
	printf("\n");
	printf("lut-byte-ref vs scalar (REPORTED, not asserted -- this is the 8-bit\n");
	printf("query table's rounding error, the number this measurement is for).\n");
	printf("The relative figure is |dev| / |exact score| per lane, so it is\n");
	printf("dominated by lanes whose exact score is near zero; the absolute one is\n");
	printf("on the scale of a unit-vector score and is the one to read.\n");
	printf("  REAL tables (weave_query_lut_build on a unit query, weave_encode'd codes)\n");
	printf("    live-lane comparisons    : %ld\n", sc_ncmp_dev[0]);
	printf("    max |deviation|          : %.6g   (at dim=%d)\n",
		   sc_maxabs[0], sc_maxabs_dim[0]);
	printf("    max relative deviation   : %.6g   (at dim=%d)\n",
		   sc_maxrel[0], sc_maxrel_dim[0]);
	printf("  STRESS tables (one row 500x the rest; NOT a distribution this codec makes)\n");
	printf("    live-lane comparisons    : %ld\n", sc_ncmp_dev[1]);
	printf("    max |deviation|          : %.6g   (at dim=%d)\n",
		   sc_maxabs[1], sc_maxabs_dim[1]);
	printf("    max relative deviation   : %.6g   (at dim=%d)\n",
		   sc_maxrel[1], sc_maxrel_dim[1]);
	if (nfellback)
		printf("  (%d blocks skipped: dim < 4, which weave_codebook_solve declines,\n"
			   "   so there is no WeaveQueryLut to build for it)\n",
			   nfellback);
	printf("\n");

	free(codes);
	free(code);
	free(scales);
	free(fvec);

	if (sc_nbitfail)
	{
		fprintf(stderr, "SELF-CHECK FAILED: lut-byte is not bit-identical to "
				"lut-byte-ref, so no timing of it means anything\n");
		return 1;
	}
	printf("self-check: PASS\n");
	return 0;
}

/* ---------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
	/*
	 * selfcheck=<nblocks> needs no corpus, so it is answered before the corpus
	 * arguments are demanded.  It is the gate on the byte-LUT kernels above and
	 * it exits non-zero if lut-byte and lut-byte-ref disagree by one bit.
	 */
	for (int i = 1; i < argc; i++)
	{
		if (!strncmp(argv[i], "selfcheck=", 10))
			return bench_selfcheck(atoi(argv[i] + 10));
	}

	if (argc < 4)
		die("usage: code_scan <base.fvecs> <nbase> <nq> [bits=4 k=10 "
			"order=clustered|natural lists=1024 kernel=<name> queries=<path> "
			"dim=<d>]\n"
			"       code_scan selfcheck=<nblocks>");

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
	int			prefix[8],
				nprefix = 0;
	int			pwin[8],
				npwin = 0;
	int			usedim = 0;			/* 0 = whatever the file has */

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
		else if (!strncmp(a, "dim=", 4))
			usedim = atoi(a + 4);
		else if (!strncmp(a, "kernel=", 7))
			kernelname = a + 7;
		else if (!strncmp(a, "queries=", 8))
			qpath = a + 8;
		else if (!strncmp(a, "prefix=", 7))
		{
			char	   *t = a + 7;

			while (*t && nprefix < 8)
			{
				prefix[nprefix++] = atoi(t);
				while (*t && *t != ',')
					t++;
				if (*t == ',')
					t++;
			}
		}
		else if (!strncmp(a, "pwin=", 5))
		{
			char	   *t = a + 5;

			while (*t && npwin < 8)
			{
				pwin[npwin++] = atoi(t);
				while (*t && *t != ',')
					t++;
				if (*t == ',')
					t++;
			}
		}
		else if (!strncmp(a, "order=", 6))
			clustered = !strcmp(a + 6, "clustered");
		else
			die("unknown option");
	}
	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		die("bits out of range");

	int			filedim = fvecs_dim(basepath);
	int			dim = usedim > 0 ? usedim : filedim;

	if (dim > filedim)
		die("dim= exceeds the corpus dimensionality");
	if (dim < 1)
		die("dim= must be positive");

	if (dim != filedim)
		fprintf(stderr, "  dim=%d (truncated from the file's %d)\n", dim, filedim);
	fprintf(stderr, "  dim=%d, reading up to %ld rows\n", dim, nreq);
	float	   *base = xmalloc((size_t) nreq * dim * sizeof(float));
	long		n = fvecs_read(basepath, filedim, nreq, 0, base, dim);

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

	const int	layout_is_vecmajor = 0;	/* this harness packs WEAVE_PACK_LANE */
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
	/* Inverse of lanevec: where vector i lives, as block*LANES + lane.  Stage 2
	 * of the prefix scan needs to rescore a specific vector, and searching
	 * lanevec for it would make the harness O(n) per candidate. */
	long	   *vipos = xmalloc((size_t) n * sizeof(long));
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
			vipos[vi] = b * LANES + s;
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

		if (qd != filedim)
			die("query dim != base dim");
		qv = xmalloc((size_t) nq * dim * sizeof(float));
		nqv = fvecs_read(qpath, filedim, nq, 0, qv, dim);
	}
	else
	{
		/* Held out from the tail of the base file, which is why nbase must be
		 * below the file's row count for this path to be a true hold-out. */
		qv = xmalloc((size_t) nq * dim * sizeof(float));
		nqv = fvecs_read(basepath, filedim, nq, nreq, qv, dim);
	}
	if (nqv < nq)
		die("too few query vectors");

	/*
	 * kernel=all times every kernel this host has, on ONE prepared corpus.
	 *
	 * That is not a convenience.  The first EC2 run of this harness invoked it
	 * once per kernel, so it re-read the corpus, re-ran k-means and re-encoded
	 * for each -- and the clustering does not depend on the kernel at all.  At
	 * n = 1M with lists = n/32 the k-means alone is O(n * lists * dim) ~ 3e13
	 * flops per iteration, which would not have finished, times three kernels.
	 * Preparation is per-corpus; timing is per-kernel; the loop belongs inside.
	 */
	const WeaveScoreKernel *kall[16];
	int			nkern = 0;

	if (kernelname && strcmp(kernelname, "all") != 0)
	{
		kall[0] = bench_kernel_lookup(kernelname);
		if (!kall[0])
			die("no such kernel on this host");
		nkern = 1;
	}
	else if (kernelname)
	{
		const WeaveScoreKernel *list[16];

		nkern = bench_kernel_list(list, 16, bits);
		for (int i = 0; i < nkern; i++)
			kall[i] = list[i];
	}
	else
	{
		kall[0] = weave_score_kernel_best();
		nkern = 1;
	}
	for (int i = 0; i < nkern; i++)
	{
		if (bench_kernel_is_approx(kall[i]) && bits != 4)
			die("the byte-LUT kernels are 4-bit only; they refuse every block at "
				"other widths rather than falling back to the oracle");
	}

	/* ---- two-stage prefix scan ----------------------------------------
	 *
	 * The other lever on an O(dim * nvec) scan: score fewer COORDINATES.
	 *
	 * Stage 1 scores every vector against only the first `m` of `dim`
	 * coordinates, keeps the best `W`, and stage 2 rescores those W over all
	 * `dim` coordinates.  Stage 3 is the exact float32 rerank of the top 25 that
	 * the ratified shape already pays for, so the recall reported here is
	 * END-TO-END against brute force -- the number the gate actually cares about,
	 * not an intermediate agreement rate.
	 *
	 * WHY A PREFIX IS A VALID SUBSAMPLE, AND WHY IT IS FREE HERE.  Two facts line
	 * up, neither of them arranged for this purpose:
	 *
	 *  - The rotation (sect. 3) applies a global permutation and a Walsh-Hadamard
	 *    transform, so the coordinates of any input are exchangeable and carry
	 *    equal energy in expectation.  A prefix is therefore a uniform random
	 *    subsample of coordinates, and <q[0:m], r[0:m]> * (dim/m) is an unbiased
	 *    estimate of the full inner product.  Without the rotation, a prefix of an
	 *    energy-ordered embedding would be a biased and much better estimate --
	 *    and of a reversed one, far worse.  The rotation makes it predictable.
	 *  - In WEAVE_PACK_LANE the code index is `j * 32 + slot`, INDEPENDENT of dim
	 *    (src/vector/pack.c code_index()).  So coordinates 0..m-1 of all 32 lanes
	 *    are a contiguous PREFIX of the block, and the LUT is row-major by
	 *    coordinate, so its first m rows are a prefix too.  Stage 1 is therefore
	 *    the existing kernel called with a shallow-copied LUT whose dim is m: no
	 *    new kernel, no repacking, no extra bytes on disk.
	 *
	 * THIS TRICK IS LAYOUT-SPECIFIC.  In WEAVE_PACK_VECMAJOR the index is
	 * `slot * dim + j`, which DOES depend on dim, so truncating the LUT there
	 * would silently read the wrong bits rather than fewer of them.  Asserted
	 * below rather than commented.
	 */
	if (nprefix > 0)
	{
		if (layout_is_vecmajor)
			die("prefix scan requires WEAVE_PACK_LANE: in VECMAJOR the code index "
				"depends on dim, so a truncated LUT reads wrong bits, not fewer");
		if (npwin == 0)
		{
			pwin[0] = 100;
			pwin[1] = 500;
			pwin[2] = 2000;
			npwin = 3;
		}

		float	   *out2 = xmalloc(sizeof(float) * LANES);
		int			RW = 25;	/* the ratified shape's exact-rerank window */

		printf("# corpus %s, n=%ld dim=%d bits=%d k=%d nq=%d\n", basepath, n, dim, bits, K, nq);
		printf("# kernel: %s   two-stage prefix scan, exact rerank window %d\n",
			   kall[0]->name, RW);
		printf("# stage-1 coordinate work is m/dim of a full scan; stage 2 is W*dim\n");
		printf("#\n");
		printf("# s2_blocks is what stage 2 scored after grouping survivors by block;\n");
		printf("# s2_pred is nblocks * (1 - exp(-W/nblocks)), the count grouping should reach\n");
		printf("prefix_m\tm/dim\tW\trecall@10\tcoord_work_vs_full\ts1_ms\ts2_ms\ttotal_ms\ts2_blocks\ts2_pred\n");

		/* Exact top-K per query, by brute force over the original vectors: the
		 * only honest reference for an end-to-end recall number. */
		int		   *gt = xmalloc((size_t) nq * K * sizeof(int));

		for (int t = 0; t < nq; t++)
		{
			TopK		g;

			topk_init(&g, K);
			for (long i = 0; i < n; i++)
			{
				const float *v = base + (size_t) i * dim;
				const float *qq = qv + (size_t) t * dim;
				double		ip = 0;

				for (int j = 0; j < dim; j++)
					ip += (double) qq[j] * v[j];
				topk_push(&g, (float) ip, (int) i);
			}
			for (int i = 0; i < K; i++)
				gt[t * K + i] = g.h[i].id;
			free(g.h);
			fprintf(stderr, "\r  exact gt %d/%d   ", t + 1, nq);
		}
		fprintf(stderr, "\r  exact gt done        \n");

		for (int pi = 0; pi < nprefix; pi++)
		{
			int			m = prefix[pi];

			if (m < 1 || m > dim)
				die("prefix out of range");
			for (int wi = 0; wi < npwin; wi++)
			{
				int			W = pwin[wi];
				double		hits = 0;
				double		ts1 = 0,
							ts2 = 0;
				/* blocks stage 2 actually scored, summed over queries.  Reported
				 * so the grouping's predicted saving is checked rather than
				 * asserted: the prediction is
				 * nblocks * (1 - exp(-W / nblocks)) per query. */
				double		s2groups = 0;
				Hit		   *cand = xmalloc(sizeof(Hit) * W);
				int		   *ord = xmalloc(sizeof(int) * W);
				float	   *s2score = xmalloc(sizeof(float) * W);

				for (int t = 0; t < nq; t++)
				{
					WeaveQueryLut lut;

					if (weave_query_lut_build(&lut, &q, qv + (size_t) t * dim, malloc) != 0)
						die("lut_build");

					/* Stage 1: the same kernel, told the vector is m long. */
					WeaveQueryLut plut = lut;

					plut.dim = m;

					TopK		s1;

					topk_init(&s1, W);
					double		tt = now();

					for (long b = 0; b < nblk; b++)
					{
						WeaveScoreBlock blk = {0};

						blk.lut = &plut;
						blk.layout = WEAVE_PACK_LANE;
						blk.codes = codes + (size_t) b * blkbytes;
						blk.scales = scales + b * LANES;
						blk.scalestride = 1;
						blk.nlanes = nlanes[b];
						blk.livemask = nlanes[b] == 32 ? 0xffffffffu : ((1u << nlanes[b]) - 1);
						if (kall[0]->score_block(&blk, out2) < 0)
							die("score_block rejected a prefix block");
						for (int sl = 0; sl < nlanes[b]; sl++)
							topk_push(&s1, out2[sl], lanevec[b * LANES + sl]);
					}

					ts1 += now() - tt;

					/* Stage 2: full-dim rescore of the W survivors.
					 *
					 * GROUPED BY BLOCK, and the reason is worth stating because
					 * it bounds what this stage can ever cost.  A kernel scores a
					 * BLOCK; the smallest thing it can score is 32 lanes.  Worse,
					 * in WEAVE_PACK_LANE one vector's code is maximally
					 * scattered -- coordinate j of lane s is a single nibble at
					 * byte j * 16 + s / 2 -- so reading ONE lane's dim nibbles
					 * touches dim distinct 16-byte spans, which is every byte of
					 * the block.  Scoring one lane therefore costs the same
					 * memory traffic as scoring all 32, and since the scan is
					 * bandwidth-bound (bench/RESULTS_CODE_SCAN.md) there is no
					 * per-vector shortcut to be had in this layout.
					 *
					 * So the only lever is to touch each block ONCE however many
					 * survivors it holds, which is what the grouping below does.
					 * The earlier version scored one block per survivor with
					 * livemask = 1 << sl, paying for a whole block per candidate.
					 *
					 * What that is worth is small and predictable: at W survivors
					 * over nblocks blocks the distinct-block count is
					 * nblocks * (1 - exp(-W/nblocks)), so at W = 8000 over 31,250
					 * blocks it is ~7,057 -- about 12%, not the ~4x an earlier
					 * revision of doc/PHASES.md guessed.  s2_groups below reports
					 * the count that was actually scored so the prediction is
					 * checked rather than asserted.
					 *
					 * The stage that WOULD be cheap here is one over
					 * WEAVE_PACK_VECMAJOR, where a vector's coordinates are
					 * contiguous -- which is exactly what that layout exists for.
					 * Stage 1 wants LANE and stage 2 wants VECMAJOR, and no index
					 * can have both without storing the codes twice.  That
					 * tension is the real reason a two-stage scan is not free.
					 */
					int			ncand = s1.n;

					memcpy(cand, s1.h, sizeof(Hit) * ncand);

					/* Score in block order, but PUSH in the original order.
					 *
					 * Grouping is a statement about which blocks get touched, not
					 * about the order results are consumed in, and keeping those
					 * two things separate is what makes this provably identical to
					 * the ungrouped version rather than probably identical.  An
					 * earlier revision sorted `cand` itself and pushed in the
					 * sorted order; recall moved (0.9350 -> 0.9200 at n=200k with
					 * lut-wide), which is how a reordering that was assumed to be
					 * harmless announced that it was not.
					 *
					 * `ord` holds candidate indices sorted by block; scores land in
					 * `s2score` indexed by the ORIGINAL candidate position.
					 */
					for (int c = 0; c < ncand; c++)
						ord[c] = c;
					g_s2_vipos = vipos;
					g_s2_cand = cand;
					qsort(ord, (size_t) ncand, sizeof(int), cand_by_block);

					tt = now();
					for (int oi = 0; oi < ncand;)
					{
						long		b = vipos[cand[ord[oi]].id] / LANES;
						int		   *e = ord + oi;
						int			cnt = 0;
						weave_uint32 mask = 0;
						WeaveScoreBlock blk = {0};

						/* every survivor in this block, in one mask */
						while (oi + cnt < ncand &&
							   vipos[cand[ord[oi + cnt]].id] / LANES == b)
						{
							mask |= 1u << (int) (vipos[cand[ord[oi + cnt]].id] % LANES);
							cnt++;
						}

						blk.lut = &lut;
						blk.layout = WEAVE_PACK_LANE;
						blk.codes = codes + (size_t) b * blkbytes;
						blk.scales = scales + b * LANES;
						blk.scalestride = 1;
						blk.nlanes = nlanes[b];
						blk.livemask = mask;
						if (kall[0]->score_block(&blk, out2) < 0)
							die("score_block rejected a rescore block");
						for (int q = 0; q < cnt; q++)
						{
							int			ci = e[q];
							int			sl = (int) (vipos[cand[ci].id] % LANES);

							s2score[ci] = out2[sl];
						}
						s2groups++;
						oi += cnt;
					}
					ts2 += now() - tt;

					TopK		s2;

					topk_init(&s2, RW);
					for (int c = 0; c < ncand; c++)
						topk_push(&s2, s2score[c], cand[c].id);

					/* The top-RW selection is not stage-2 scan work and is not
					 * timed as such; ts2 was closed above, at the end of the
					 * scoring loop. */

					/* Stage 3: exact float32 rerank of the top RW, which is what
					 * the shipping shape does from the heap. */
					TopK		s3;

					topk_init(&s3, K);
					for (int c = 0; c < s2.n; c++)
					{
						const float *v = base + (size_t) s2.h[c].id * dim;
						const float *qq = qv + (size_t) t * dim;
						double		ip = 0;

						for (int j = 0; j < dim; j++)
							ip += (double) qq[j] * v[j];
						topk_push(&s3, (float) ip, s2.h[c].id);
					}
					for (int i = 0; i < s3.n; i++)
						for (int j = 0; j < K; j++)
							if (s3.h[i].id == gt[t * K + j])
							{
								hits++;
								break;
							}
					free(s1.h);
					free(s2.h);
					free(s3.h);
					free(lut._alloc);
				}
				double		work = (double) m / dim + (double) W * dim / ((double) n * dim);
				long		nblk = (n + LANES - 1) / LANES;
				double		predicted = (double) nblk *
					(1.0 - exp(-(double) W / (double) nblk));

				printf("%d\t%.4f\t%d\t%.4f\t%.4f\t%.2f\t%.2f\t%.2f\t%.0f\t%.0f\n",
					   m, (double) m / dim, W, hits / (nq * (double) K), work,
					   1000 * ts1 / nq, 1000 * ts2 / nq,
					   1000 * (ts1 + ts2) / nq,
					   s2groups / nq, predicted);
				fflush(stdout);
				free(cand);
				free(ord);
				free(s2score);
			}
		}
		return 0;
	}

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

  for (int ki = 0; ki < nkern; ki++)
  {
	const WeaveScoreKernel *kern = kall[ki];

	t_flat = t_prune = 0;
	sum_pruned_run = sum_pruned_oracle = sum_lanes = 0;
	sum_theta = sum_b1 = sum_b2 = sum_b3 = sum_bmin = sum_rad = sum_cens = sum_best = 0;
	violations = mismatches = 0;

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
		float		theta_final = a.theta;	/* k-th best: the heap root, before sorting */

		topk_finish(&a);
		memcpy(ref, a.h, sizeof(Hit) * a.n);

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
		topk_finish(&p);
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

	/*
	 * BOTH ASSERTIONS BELOW COMPARE A SCORE AGAINST A BOUND (or a top-k) DERIVED
	 * FROM THE EXACT FLOAT TABLE, so they can only be fatal for an EXACT kernel.
	 * The byte-LUT kernels reconstruct a score with a rounding residual of up to
	 * step/2 per coordinate, and nothing stops that residual pushing a lane a
	 * hair above weave_block_bound_ip()'s exact-table bound, or reordering two
	 * near-tied lanes between the flat and the pruned arm.  Killing the run there
	 * would blame the block bound for the query table's quantization, which is
	 * the wrong diagnosis and would hide the right one.
	 *
	 * So for an approximate kernel these are REPORTED with their real cause named
	 * and the run continues; for every exact kernel the gate is exactly as fatal
	 * as it was.  The correctness gate for the byte kernels is
	 * `selfcheck=<nblocks>`, which is bit-exact and does exit non-zero.
	 */
	if (violations)
	{
		if (bench_kernel_is_approx(kern))
			fprintf(stderr, "note: %ld lane scores exceeded the exact-table bound "
					"-- expected for the approximate kernel %s, and it is the "
					"8-bit table's rounding, not an unsound bound\n",
					violations, kern->name);
		else
		{
			fprintf(stderr, "BOUND VIOLATIONS: %ld -- the bound is UNSOUND, no timing "
					"below this line means anything\n", violations);
			return 1;
		}
	}
	if (mismatches)
	{
		if (bench_kernel_is_approx(kern))
			fprintf(stderr, "note: pruned top-k differs from flat top-k on %d of "
					"%d queries under the approximate kernel %s -- near-tied "
					"lanes reordered by the 8-bit table, see selfcheck=\n",
					(int) mismatches, nq, kern->name);
		else
		{
			fprintf(stderr, "TOP-K MISMATCHES: %d of %d queries -- the pruned arm "
					"returns different results from the flat arm, so it is faster and "
					"wrong\n", (int) mismatches, nq);
			return 1;
		}
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
	if (violations || mismatches)
		printf("# soundness: APPROXIMATE kernel -- %ld bound excursions, %d/%d "
			   "queries' top-k differ between arms (8-bit table rounding; the "
			   "bit-exact gate is selfcheck=)\n", violations, (int) mismatches, nq);
	else
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
	printf("per vector                   : %8.1f ns   (flat)\n",
		   1e9 * t_flat / nq / n);
	printf("\n");
  }
	return 0;
}
