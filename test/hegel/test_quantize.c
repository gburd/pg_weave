/*-------------------------------------------------------------------------
 *
 * test_quantize.c
 *		Standalone property tests for the vector-channel codec.
 *
 * Links src/vector/quantize.c and src/vector/pack.c directly, with no backend.
 * That is the whole reason weave/quantize.h is written to be
 * backend-independent; see the design intent note at the top of weave/for.h,
 * which this file mirrors.
 *
 * Build and run:
 *		make -C test/hegel test_quantize && ./test/hegel/test_quantize
 *
 * The properties here are the ones that, if broken, produce an index that
 * returns plausible-but-wrong answers rather than an error:
 *
 *	 P1  rotation is orthogonal (preserves L2 norm)
 *	 P2  rotation is invertible (inverse . forward == identity)
 *	 P3  rotation is deterministic across repeated construction
 *	 P4  codebook centroids are sorted, symmetric, and inside the domain
 *	 P4b boundaries are the midpoints of neighbouring centroids (nearest
 *	     neighbour), and strictly interleave them
 *	 P4c every centroid is the conditional mean of its own cell (Lloyd-Max
 *	     stationarity -- catches a solver that stops iterating too early)
 *	 P4d no dead levels: every cell carries mass consistent with the
 *	     high-resolution companding law (catches levels stranded in the tail)
 *	 P4e absmax is bounded by a LEVEL-COUNT-AWARE ceiling, not a fixed sd
 *	     multiple, so the bound tightens as bits rises
 *	 P4f distortion strictly decreases as bits rises at fixed dim
 *	 P5  encode/decode round-trips to a bounded relative error
 *	 P6  the renormalization scale makes the IP estimator unbiased
 *	 P7  pack/unpack round-trips in both layouts
 *	 P8  the block bound is a TRUE upper bound -- channel.h contract (C2)
 *
 * P8 is the one that matters most.  A too-low bound silently drops rows and no
 * fixed-expected-output regression test can catch it, which is exactly why this
 * layer is mandatory rather than optional (doc/TESTING.md).
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_quantize.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/quantize.h"

static int	failures = 0;
static int	checks = 0;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

/* xoshiro256** -- a test-local PRNG, so the test corpus is reproducible and
 * independent of libc. */
static weave_uint64 rngs[4] = {0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL,
	0xa4093822299f31d0ULL, 0x082efa98ec4e6c89ULL};

static weave_uint64
rotl64(weave_uint64 x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static weave_uint64
rnd64(void)
{
	weave_uint64 r = rotl64(rngs[1] * 5, 7) * 9;
	weave_uint64 t = rngs[1] << 17;

	rngs[2] ^= rngs[0];
	rngs[3] ^= rngs[1];
	rngs[1] ^= rngs[2];
	rngs[0] ^= rngs[3];
	rngs[2] ^= t;
	rngs[3] = rotl64(rngs[3], 45);
	return r;
}

/* Uniform in [0,1). */
static double
rnd_unit(void)
{
	return (double) (rnd64() >> 11) * (1.0 / 9007199254740992.0);
}

/* Standard normal, Box-Muller.  Gives us isotropic test vectors, which is the
 * distribution the codebook is derived for -- and, separately, we also test
 * heavily anisotropic and energy-ordered inputs, because those are the ones
 * that break a rotation with the permutation in the wrong place. */
static double
rnd_normal(void)
{
	double		u1 = rnd_unit();
	double		u2 = rnd_unit();

	if (u1 < 1e-300)
		u1 = 1e-300;
	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static void
fill_isotropic(float *v, int dim)
{
	int			j;

	for (j = 0; j < dim; j++)
		v[j] = (float) rnd_normal();
}

/* Energy-ordered: variance decays geometrically with coordinate index, the way
 * a Matryoshka/MRL embedding is built.  This is the adversarial input for the
 * rotation. */
static void
fill_energy_ordered(float *v, int dim)
{
	int			j;

	for (j = 0; j < dim; j++)
		v[j] = (float) (rnd_normal() * exp(-4.0 * (double) j / (double) dim));
}

static double
l2norm(const float *v, int dim)
{
	double		s = 0.0;
	int			j;

	for (j = 0; j < dim; j++)
		s += (double) v[j] * (double) v[j];
	return sqrt(s);
}

static double
dot(const float *a, const float *b, int dim)
{
	double		s = 0.0;
	int			j;

	for (j = 0; j < dim; j++)
		s += (double) a[j] * (double) b[j];
	return s;
}

/* ------------------------------------------------------------------------- */

static void
test_rotation(int dim)
{
	WeaveRotation rot,
				rot2;
	float	   *x,
			   *y;
	double		n0,
				n1;
	int			j;

	CHECK(weave_rotation_init(&rot, dim, malloc, free) == 0,
		  "rotation_init failed for dim=%d", dim);
	CHECK(weave_rotation_init(&rot2, dim, malloc, free) == 0,
		  "rotation_init(2) failed for dim=%d", dim);

	/* P3: deterministic construction */
	for (j = 0; j < dim; j++)
		CHECK(rot.perm[0][j] == rot2.perm[0][j],
			  "dim=%d perm[0][%d] not deterministic", dim, j);

	x = malloc(sizeof(float) * dim);
	y = malloc(sizeof(float) * dim);

	fill_energy_ordered(x, dim);
	memcpy(y, x, sizeof(float) * dim);
	n0 = l2norm(x, dim);

	weave_rotate(&rot, x);
	n1 = l2norm(x, dim);

	/* P1: orthogonality.  Tolerance is float accumulation over log2(B) butterfly
	 * stages times ROUNDS, not an arbitrary epsilon. */
	CHECK(fabs(n1 - n0) <= 1e-4 * n0,
		  "dim=%d rotation not norm-preserving: %.9g -> %.9g", dim, n0, n1);

	/* P2: invertibility */
	weave_rotate_inverse(&rot, x);
	for (j = 0; j < dim; j++)
	{
		double		d = fabs((double) x[j] - (double) y[j]);

		CHECK(d <= 1e-3 * (1.0 + fabs((double) y[j])),
			  "dim=%d inverse rotation mismatch at %d: %.9g vs %.9g",
			  dim, j, (double) x[j], (double) y[j]);
		if (failures > 20)
			break;
	}

	/*
	 * The point of permuting before the Hadamard: after rotation, the energy
	 * must NOT still be concentrated in the low coordinates.  Compare the mass
	 * in the first eighth against the last eighth.
	 */
	fill_energy_ordered(x, dim);
	weave_rotate(&rot, x);
	{
		double		head = 0.0,
					tail = 0.0;
		int			eighth = dim / 8;

		for (j = 0; j < eighth; j++)
			head += (double) x[j] * (double) x[j];
		for (j = dim - eighth; j < dim; j++)
			tail += (double) x[j] * (double) x[j];
		/* Before rotation this ratio is ~e^8 = 2981.  After, it should be O(1).
		 * 10x is a loose gate that still fails hard if the permutation is
		 * missing or applied after the transform. */
		CHECK(head < 10.0 * tail && tail < 10.0 * head,
			  "dim=%d energy not spread by rotation: head=%.6g tail=%.6g",
			  dim, head, tail);
	}

	free(x);
	free(y);
	weave_rotation_free(&rot, free);
	weave_rotation_free(&rot2, free);
}

/* ---------------------------------------------------------------------------
 * Test-local quadrature against the sphere marginal.
 *
 * Deliberately NOT the solver's adaptive Simpson: an assertion computed with the
 * arithmetic under test cannot fail for the reason the assertion exists.  This
 * is fixed-panel composite Simpson evaluated cell-by-cell with EXACT cell
 * endpoints, which is the part that matters -- classifying samples from one
 * global grid into cells puts an O(h) error in each cell's mass, and at 8 bits a
 * central cell is only ~0.017 sd wide, so that error would swamp the centroid
 * condition below.
 *
 * Integrated over the true support [-1, 1] rather than the solver's clipped
 * domain, so these checks do not inherit beta_domain()'s -80 log-density cutoff.
 * That is safe rather than sloppy: by construction the density beyond the cutoff
 * is e^-80 ~ 2e-35 of its peak, which is 28 orders below the smallest cell share
 * any assertion here uses.
 *
 * The panel count is chosen from the interval width in units of the coordinate
 * standard deviation, not fixed.  That is not tidiness: the OUTERMOST cell runs
 * from its boundary out to 1, which at dim=1536 is 35 sd wide against a central
 * cell 0.017 sd wide, and a fixed 512 panels there gives a step of 0.07 sd and a
 * conditional mean wrong by 5e-5 sd -- enough to fail the centroid condition on
 * a solver that is actually correct.  A test whose own quadrature error exceeds
 * the tolerance it asserts is worse than no test.
 * ------------------------------------------------------------------------- */

#define CELLQ_MIN_PANELS		512
#define CELLQ_MAX_PANELS		32768
#define CELLQ_STEP_SD			0.004	/* target step, in units of sd */

/* moment 0: f(x).  1: x*f(x).  2: (x - ref)^2 * f(x). */
static double
sphere_integrate(double a, double b, double shape, double sd,
				 int moment, double ref)
{
	double		h,
				sum = 0.0;
	int			np,
				k;

	if (b <= a)
		return 0.0;

	np = (int) ((b - a) / (CELLQ_STEP_SD * sd)) + 1;
	if (np < CELLQ_MIN_PANELS)
		np = CELLQ_MIN_PANELS;
	if (np > CELLQ_MAX_PANELS)
		np = CELLQ_MAX_PANELS;
	np += (np & 1);				/* Simpson needs an even panel count */

	h = (b - a) / (double) np;
	for (k = 0; k <= np; k++)
	{
		double		x = (k == np) ? b : a + (double) k * h;
		double		t = 1.0 - x * x;
		double		f,
					lg,
					w;

		if (t <= 0.0)
			continue;
		lg = shape * log(t);
		if (lg < -700.0)
			continue;
		f = exp(lg);
		if (moment == 1)
			f *= x;
		else if (moment == 2)
			f *= (x - ref) * (x - ref);
		w = (k == 0 || k == np) ? 1.0 : ((k & 1) ? 4.0 : 2.0);
		sum += w * f;
	}
	return sum * h / 3.0;
}

/*
 * Mean squared quantization error of a codebook against the sphere marginal, in
 * units of the coordinate variance 1/dim.  Used for the monotone-distortion
 * property; returns -1 on a degenerate codebook.
 */
static double
codebook_distortion(const WeaveCodebook *cb)
{
	double		shape = 0.5 * ((double) cb->dim - 3.0);
	double		sd = 1.0 / sqrt((double) cb->dim);
	double		total = 0.0,
				dist = 0.0;
	int			i;

	for (i = 0; i < cb->nlevels; i++)
	{
		double		a = (i == 0) ? -1.0 : cb->boundary[i - 1];
		double		b = (i == cb->nlevels - 1) ? 1.0 : cb->boundary[i];

		total += sphere_integrate(a, b, shape, sd, 0, 0.0);
		dist += sphere_integrate(a, b, shape, sd, 2, cb->centroid[i]);
	}
	if (total <= 0.0)
		return -1.0;
	return (dist / total) * (double) cb->dim;
}

static void
test_codebook(int bits, int dim)
{
	WeaveCodebook cb;
	double		shape = 0.5 * ((double) dim - 3.0);
	double		sd = 1.0 / sqrt((double) dim);
	double		total;
	double		minshare = 1e300;
	int			n;
	int			i;

	CHECK(weave_codebook_solve(bits, dim, &cb) == 0,
		  "codebook_solve failed bits=%d dim=%d", bits, dim);
	n = cb.nlevels;

	/* P4: sorted */
	for (i = 0; i + 1 < n; i++)
		CHECK(cb.centroid[i] < cb.centroid[i + 1],
			  "bits=%d dim=%d centroids not sorted at %d: %.9g >= %.9g",
			  bits, dim, i, (double) cb.centroid[i], (double) cb.centroid[i + 1]);

	/* P4: symmetric about zero */
	for (i = 0; i < n / 2; i++)
	{
		double		lo = cb.centroid[i];
		double		hi = cb.centroid[n - 1 - i];

		CHECK(fabs(lo + hi) <= 1e-6 * (1.0 + fabs(hi)),
			  "bits=%d dim=%d codebook not symmetric at %d: %.9g vs %.9g",
			  bits, dim, i, lo, hi);
	}

	/* P4: inside [-1, 1], and non-degenerate */
	CHECK(cb.absmax > 0.0f && cb.absmax < 1.0f,
		  "bits=%d dim=%d absmax out of range: %.9g",
		  bits, dim, (double) cb.absmax);

	/*
	 * P4b: THE NEAREST-NEIGHBOUR CONDITION.  weave/quantize.h promises that
	 * boundary[i] is the midpoint of centroid[i] and centroid[i+1] -- that is
	 * what makes the branchless comparison ladder in quantize_one() equivalent
	 * to the distance search it replaces.  A boundary that is merely "between"
	 * its neighbours yields a quantizer that is sorted, plausible, and not
	 * nearest-neighbour, and nothing else in this suite would notice.
	 * Boundaries must also strictly interleave the centroids.
	 */
	for (i = 0; i + 1 < n; i++)
	{
		double		mid = 0.5 * ((double) cb.centroid[i] + (double) cb.centroid[i + 1]);

		CHECK(fabs((double) cb.boundary[i] - mid) <= 1e-6 * (sd + fabs(mid)),
			  "bits=%d dim=%d boundary %d is not the midpoint: %.9g vs %.9g",
			  bits, dim, i, (double) cb.boundary[i], mid);
		CHECK(cb.centroid[i] < cb.boundary[i] && cb.boundary[i] < cb.centroid[i + 1],
			  "bits=%d dim=%d boundary %d does not interleave: %.9g not in (%.9g, %.9g)",
			  bits, dim, i, (double) cb.boundary[i],
			  (double) cb.centroid[i], (double) cb.centroid[i + 1]);
	}

	/*
	 * P4c: THE CENTROID CONDITION.  Each centroid must equal the conditional
	 * mean of its own cell.  Together with P4b this is the full statement of
	 * Lloyd-Max stationarity, so between them they say "this is the optimal
	 * fixed-rate quantizer for this source", not merely "this is a sorted
	 * ladder".
	 *
	 * Tolerance 1e-5 sd.  Derivation: the solve runs in double and terminates on
	 * a step below 1e-11 of the integration domain, and the codebook is then
	 * STORED AS FLOAT32, whose 24-bit mantissa puts a 6e-8 relative floor on any
	 * centroid -- about 2e-8 sd at these magnitudes.  This quadrature adds its
	 * own error, empirically under 1e-8 sd.  1e-5 sd is therefore ~500x above the
	 * float32 representation floor and still 100x below the 1e-3 sd residual that
	 * the previous Lloyd-iteration solver left behind at 5 bits when it exited on
	 * its 200-sweep cap.  This is the assertion that catches a solver which stops
	 * iterating too early -- a failure mode that leaves every other property in
	 * this function satisfied.
	 */
	total = 0.0;
	for (i = 0; i < n; i++)
	{
		double		a = (i == 0) ? -1.0 : cb.boundary[i - 1];
		double		b = (i == n - 1) ? 1.0 : cb.boundary[i];
		double		m0 = sphere_integrate(a, b, shape, sd, 0, 0.0);
		double		m1 = sphere_integrate(a, b, shape, sd, 1, 0.0);

		total += m0;
		CHECK(m0 > 0.0, "bits=%d dim=%d cell %d has zero mass", bits, dim, i);
		if (m0 > 0.0)
			CHECK(fabs(m1 / m0 - (double) cb.centroid[i]) <= 1e-5 * sd,
				  "bits=%d dim=%d centroid %d is not its cell mean: %.9g vs %.9g "
				  "(residual %.3g sd -- solver not converged?)",
				  bits, dim, i, (double) cb.centroid[i], m1 / m0,
				  fabs(m1 / m0 - (double) cb.centroid[i]) / sd);
	}

	/*
	 * P4d: NO DEAD LEVELS.  This is the assertion that matters, and it replaced
	 * a hardcoded `absmax < 6.0 * sd` plausibility heuristic that was calibrated
	 * at 2-4 bits and hid a real defect at 6-8.
	 *
	 * A level whose cell carries no probability mass is a level no coordinate
	 * ever maps to, so it costs a code point and buys nothing: an "8-bit" code
	 * with 168 unreachable levels carries under 6.5 bits of real resolution.
	 * Measured with the previous uniform centroid seed, which spread levels over
	 * beta_domain() -- a domain that depends on the distribution shape but not on
	 * the level count -- at dim=1536, counting a level UNREACHABLE when its cell
	 * holds under 1e-6 of the fair share 1/n:
	 *
	 *		bits  unreachable  smallest cell share, x fair share 1/n
	 *		----  -----------  -------------------------------------
	 *		   4        0/16                              1.3e-01
	 *		   5        0/32                              1.9e-02
	 *		   6        2/64                              8.5e-09
	 *		   7       42/128                             4.1e-14
	 *		   8      122/256                             5.4e-25
	 *
	 * At 8 bits that is 122 of 256 levels that no coordinate would ever select,
	 * and 168 of 256 holding under 1 % of their fair share.  After the fix the
	 * unreachable count is 0 at every (bits, dim) and the smallest share is
	 * 1.4e-03 of fair share.
	 *
	 * THE BOUND IS DERIVED, not picked.  Under the high-resolution (Bennett /
	 * Panter-Dite) approximation the optimal level density is proportional to
	 * f(x)^(1/3), normalized to n levels, so a cell's width is 1/lambda(x) and
	 * its mass is f(x)/lambda(x) = K f(x)^(2/3) / n with K = integral of
	 * f^(1/3).  For a Gaussian of standard deviation s the constants cancel:
	 *
	 *		n * share(x) = sqrt(3) * exp(-x^2 / (3 s^2))
	 *
	 * and the sphere marginal is Gaussian to O(1/d).  The SMALLEST share is the
	 * outermost cell's, at x = absmax, so the floor below is that law evaluated
	 * at the codebook's own absmax with a 4x allowance for the approximation
	 * being asymptotic in n.  Measured ratio of actual to predicted across all
	 * (bits, dim) here: 0.72 to 0.93, so the 4x is ~4-5x of real margin -- while
	 * the broken codebook above misses it by nine orders of magnitude at 6 bits
	 * and thirteen at 8.  Unlike a fixed multiple of sd, this tightens
	 * automatically as bits rises.
	 */
	CHECK(total > 0.0, "bits=%d dim=%d total mass is zero", bits, dim);
	for (i = 0; i < n && total > 0.0; i++)
	{
		double		a = (i == 0) ? -1.0 : cb.boundary[i - 1];
		double		b = (i == n - 1) ? 1.0 : cb.boundary[i];
		double		share = sphere_integrate(a, b, shape, sd, 0, 0.0) / total;

		if (share * (double) n < minshare)
			minshare = share * (double) n;
	}
	{
		double		a = (double) cb.absmax / sd;
		double		predicted = sqrt(3.0) * exp(-a * a / 3.0);

		CHECK(minshare >= 0.25 * predicted,
			  "bits=%d dim=%d DEAD LEVEL: smallest cell share is %.4g x 1/n, "
			  "companding law predicts %.4g (absmax=%.4g sd)",
			  bits, dim, minshare, predicted, a);
	}

	/*
	 * P4e: absmax must track the DISTRIBUTION, not the integration domain.  The
	 * old form of this was `absmax < 6.0 * sd`, a constant calibrated when the
	 * ceiling was 4 bits; at 8 bits the true Lloyd-Max outermost centroid is
	 * 4.59 sd, so 6.0 was simultaneously too loose to catch a codebook whose
	 * levels had escaped into the dead tail at 6 bits (measured 7.46 sd at
	 * dim=1536, would have passed at 5.39 sd at dim=1024) and on course to become
	 * too tight had the true value kept climbing.  Both problems come from the
	 * bound not knowing n.
	 *
	 * Derived from the same companding law: the outermost cell's share obeys
	 * n * share = sqrt(3) exp(-a^2 / 3) with a = absmax / sd, and a level whose
	 * cell holds less than 1/n of its fair share 1/n -- i.e. less than 1/n^2 of
	 * the whole distribution -- is a level not worth its code point.  Requiring
	 * n * share >= 1/n gives a <= sqrt(3 ln(sqrt(3) n^2)).  That is 3.16 sd at
	 * n=4, 4.28 at n=16, 5.16 at n=64 and 5.91 at n=256, against measured 1.50,
	 * 2.73, 3.73 and 4.59 -- roughly 1.3x margin at the top end, and it rejects
	 * every broken point from 6 bits up.
	 */
	{
		double		ceil_sd = sqrt(3.0 * log(sqrt(3.0) * (double) n * (double) n));

		CHECK((double) cb.absmax < ceil_sd * sd,
			  "bits=%d dim=%d absmax=%.6g is %.4g sd, above the level-count "
			  "ceiling %.4g sd (levels in dead space, or domain tracking)",
			  bits, dim, (double) cb.absmax, (double) cb.absmax / sd, ceil_sd);
		CHECK((double) cb.absmax > 0.2 * sd,
			  "bits=%d dim=%d absmax=%.6g collapsed vs sd=%.6g",
			  bits, dim, (double) cb.absmax, sd);
	}
}

/*
 * P4f: MONOTONE DISTORTION.  Mean squared quantization error must strictly
 * decrease as bits rises at fixed dim.  This is the single best end-to-end check
 * that a wider code is genuinely wider: a codebook can be sorted, symmetric,
 * stationary and still be a waste of bits if a chunk of its levels sit where
 * nothing lands, and distortion is the one number that notices.  With the
 * previous solver, distortion at dim=1536 went 6.4e-2 (4 bits), 5.4e-2 (5),
 * 1.5e-1 (6) -- it got WORSE from 5 to 6 bits, i.e. a wider code that stored
 * less.  Nothing else in this suite failed on that.
 *
 * A ratio gate as well as strict decrease: high-resolution theory says
 * distortion falls by 4x per added bit (6.02 dB), so anything above 0.5x per bit
 * means levels are being wasted even if the sequence is technically decreasing.
 */
static void
test_distortion_monotone(int dim)
{
	double		prev = -1.0;
	int			bits;

	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		WeaveCodebook cb;
		double		d;

		CHECK(weave_codebook_solve(bits, dim, &cb) == 0,
			  "codebook_solve failed bits=%d dim=%d", bits, dim);
		d = codebook_distortion(&cb);
		CHECK(d > 0.0, "bits=%d dim=%d distortion not positive: %.6g", bits, dim, d);
		printf("  dim=%d bits=%d: MSE = %.6g of coordinate variance", dim, bits, d);
		if (prev > 0.0)
		{
			printf("  (%.3fx)", d / prev);
			CHECK(d < prev,
				  "dim=%d distortion NOT monotone in bits: %d bits gives %.6g, "
				  "%d bits gives %.6g -- a wider code that stores less",
				  dim, bits - 1, prev, bits, d);
			CHECK(d < 0.5 * prev,
				  "dim=%d distortion barely improved from %d to %d bits: "
				  "%.6g -> %.6g (%.3fx, theory says 0.25x) -- wasted levels",
				  dim, bits - 1, bits, prev, d, d / prev);
		}
		printf("\n");
		prev = d;
	}
}

static void
test_encode(int bits, int dim, int nvec)
{
	WeaveQuantizer q;
	float	   *v,
			   *rec,
			   *query;
	weave_uint8 *code;
	double		relerr_sum = 0.0;
	double		bias_sum = 0.0;
	double		absip_sum = 0.0;
	int			i;

	CHECK(weave_quantizer_init(&q, dim, bits, NULL, malloc, free) == 0,
		  "quantizer_init failed bits=%d dim=%d", bits, dim);

	v = malloc(sizeof(float) * dim);
	rec = malloc(sizeof(float) * dim);
	query = malloc(sizeof(float) * dim);
	code = malloc((size_t) q.codebytes);

	for (i = 0; i < nvec; i++)
	{
		float		norm,
					scale;
		double		nv,
					nr;

		fill_isotropic(v, dim);
		CHECK(weave_encode(&q, v, code, &norm, &scale) == 0,
			  "encode failed at i=%d", i);

		/* P5: the reported norm is the actual norm */
		nv = l2norm(v, dim);
		CHECK(fabs((double) norm - nv) <= 1e-3 * nv,
			  "norm mismatch: reported %.9g actual %.9g", (double) norm, nv);

		weave_decode(&q, code, scale, rec);
		nr = l2norm(rec, dim);

		/*
		 * P6: the renormalization scale is defined so that the reconstruction's
		 * projection onto the true direction equals the true norm.  So
		 * <v, rec> / ||v|| must be ~= ||v||, i.e. <v,rec> ~= ||v||^2.  This is
		 * the property the entire "no rerank pass needed" claim rests on; if it
		 * fails, doc/specs/VECTOR_CHANNEL.md sect. 2 is wrong.
		 */
		{
			double		proj = dot(v, rec, dim);

			CHECK(fabs(proj - nv * nv) <= 1e-2 * nv * nv,
				  "renormalization broken: <v,rec>=%.9g expected %.9g",
				  proj, nv * nv);
		}

		relerr_sum += fabs(nr - nv) / nv;

		/* Estimator bias over random query directions. */
		fill_isotropic(query, dim);
		{
			double		truth = dot(query, v, dim);
			double		est = dot(query, rec, dim);

			bias_sum += (est - truth);
			absip_sum += fabs(truth);
		}
	}

	/*
	 * Mean relative norm error.  At 4 bits over a d>=256 isotropic corpus this
	 * should be single-digit percent; the gate is deliberately loose because the
	 * point is to catch a broken codec, not to pin down a quality number (that
	 * belongs in bench/, measured against recall).
	 */
	CHECK(relerr_sum / nvec < 0.25,
		  "bits=%d dim=%d mean relative norm error %.4f too high",
		  bits, dim, relerr_sum / nvec);

	/* P6 in aggregate: the estimator must be unbiased, i.e. the mean signed
	 * error is small relative to the mean |true inner product|. */
	CHECK(fabs(bias_sum / nvec) < 0.15 * (absip_sum / nvec),
		  "bits=%d dim=%d estimator biased: mean signed err %.6g vs mean |ip| %.6g",
		  bits, dim, bias_sum / nvec, absip_sum / nvec);

	free(v);
	free(rec);
	free(query);
	free(code);
	weave_quantizer_free(&q, free);
}

static void
test_pack(int bits, int dim)
{
	weave_uint8 *block,
			   *code,
			   *back;
	int			codebytes = (dim * bits + 7) / 8;
	int			layout,
				slot,
				j;

	block = malloc((size_t) weave_block_codebytes(dim, bits));
	code = malloc((size_t) codebytes);
	back = malloc((size_t) codebytes);

	for (layout = 0; layout <= 1; layout++)
	{
		memset(block, 0, (size_t) weave_block_codebytes(dim, bits));

		for (slot = 0; slot < WEAVE_VEC_BLOCK; slot++)
		{
			for (j = 0; j < codebytes; j++)
				code[j] = (weave_uint8) (rnd64() & 0xFF);
			/* mask off bits beyond the last coordinate so the comparison is
			 * against what the layout can actually represent */
			if ((dim * bits) % 8 != 0)
				code[codebytes - 1] &=
					(weave_uint8) ((1u << ((dim * bits) % 8)) - 1);

			weave_pack_lane((WeavePackLayout) layout, dim, bits, block, slot, code);
			weave_unpack_lane((WeavePackLayout) layout, dim, bits, block, slot, back);

			/* P7 */
			CHECK(memcmp(code, back, (size_t) codebytes) == 0,
				  "layout=%d bits=%d dim=%d slot=%d pack round-trip failed",
				  layout, bits, dim, slot);
			if (failures > 20)
				goto out;
		}

		/* Zeroing one lane must not disturb its neighbours. */
		weave_pack_zero_lane((WeavePackLayout) layout, dim, bits, block, 5);
		weave_unpack_lane((WeavePackLayout) layout, dim, bits, block, 5, back);
		for (j = 0; j < codebytes; j++)
			CHECK(back[j] == 0, "layout=%d zero_lane left residue at byte %d",
				  layout, j);
	}

out:
	free(block);
	free(code);
	free(back);
}

/*
 * P8: the block bound must dominate every score in the block.
 *
 * This is contract (C2) of weave/channel.h.  We build a real 32-vector block,
 * compute the true inner-product score of every lane against a random query,
 * and assert the bound from weave_block_bound_ip() is >= all of them.  Repeated
 * over many random blocks and queries, because a bound that is right on average
 * and wrong on the tail is exactly the failure mode that loses rows.
 */
static void
test_block_bound(int bits, int dim, int ntrial)
{
	WeaveQuantizer q;
	WeaveQueryLut lut;
	float	   *v,
			   *query,
			   *cen,
			   *recs;
	weave_uint8 *code,
			   *cencode;
	float		scales[WEAVE_VEC_BLOCK];
	float		norms[WEAVE_VEC_BLOCK];
	weave_uint8 *codes[WEAVE_VEC_BLOCK];
	int			t,
				s,
				j;
	double		worst_ratio = 0.0;

	CHECK(weave_quantizer_init(&q, dim, bits, NULL, malloc, free) == 0,
		  "quantizer_init failed");

	v = malloc(sizeof(float) * dim);
	query = malloc(sizeof(float) * dim);
	cen = malloc(sizeof(float) * dim);
	recs = malloc(sizeof(float) * (size_t) dim * WEAVE_VEC_BLOCK);
	code = malloc((size_t) q.codebytes);
	cencode = malloc((size_t) q.codebytes);
	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
		codes[s] = malloc((size_t) q.codebytes);

	for (t = 0; t < ntrial; t++)
	{
		float		smax = 0.0f;
		float		minnorm = 1e30f;
		float		maxrec = 0.0f;
		float		radius = 0.0f;
		float		cennorm,
					censcale,
					censcore;
		float		base[WEAVE_MAX_DIM];

		/*
		 * Build a spatially COHERENT block: 32 perturbations of one direction.
		 * That is what a warp ordered by the graph's k-means partition looks
		 * like, and per bench/RESULTS_BOUND_PRUNING.md it is the only regime in
		 * which the (B3) bound prunes anything.  We assert (C2) here; the
		 * pruning RATE is measured by the benchmark, not by a unit test.
		 */
		for (j = 0; j < dim; j++)
			base[j] = (float) rnd_normal();

		for (s = 0; s < WEAVE_VEC_BLOCK; s++)
		{
			double		rn = 0.0;

			for (j = 0; j < dim; j++)
				v[j] = (float) ((double) base[j] + 0.35 * rnd_normal());
			if (weave_encode(&q, v, code, &norms[s], &scales[s]) != 0)
			{
				s--;
				continue;
			}
			memcpy(codes[s], code, (size_t) q.codebytes);
			weave_decode(&q, code, scales[s], recs + (size_t) s * dim);
			for (j = 0; j < dim; j++)
			{
				double		x = recs[(size_t) s * dim + j];

				rn += x * x;
			}
			rn = sqrt(rn);
			if (scales[s] > smax)
				smax = scales[s];
			if (norms[s] < minnorm)
				minnorm = norms[s];
			if ((float) rn > maxrec)
				maxrec = (float) rn;
		}

		/* centroid of the reconstructions, then re-encoded as a code so the
		 * bound needs no float centroid on the page */
		for (j = 0; j < dim; j++)
		{
			double		a = 0.0;

			for (s = 0; s < WEAVE_VEC_BLOCK; s++)
				a += recs[(size_t) s * dim + j];
			cen[j] = (float) (a / WEAVE_VEC_BLOCK);
		}
		if (weave_encode(&q, cen, cencode, &cennorm, &censcale) != 0)
			continue;

		/*
		 * R must be measured against the centroid AS RECONSTRUCTED FROM ITS
		 * CODE, not against the exact float centroid -- that is what the reader
		 * will use, and using the exact centroid here would make R too small and
		 * the bound unsound.  This is the subtle part of (B3).
		 */
		{
			float	   *cenrec = malloc(sizeof(float) * dim);

			weave_decode(&q, cencode, censcale, cenrec);
			for (s = 0; s < WEAVE_VEC_BLOCK; s++)
			{
				double		d = 0.0;

				for (j = 0; j < dim; j++)
				{
					double		x = (double) recs[(size_t) s * dim + j] - cenrec[j];

					d += x * x;
				}
				d = sqrt(d);
				if ((float) d > radius)
					radius = (float) d;
			}
			free(cenrec);
		}

		for (j = 0; j < dim; j++)
			query[j] = (float) rnd_normal();
		CHECK(weave_query_lut_build(&lut, &q, query, malloc) == 0,
			  "lut build failed");
		CHECK(lut.lutbound >= 0.0f, "L(q) negative: %.9g", (double) lut.lutbound);

		censcore = weave_lut_score_code(&lut, bits, cencode, censcale);

		{
			float		bound = weave_block_bound_ip(&lut, smax, maxrec,
													 censcore, radius);
			double		blockmax = -1e300;

			for (s = 0; s < WEAVE_VEC_BLOCK; s++)
			{
				double		sc = 0.0;

				for (j = 0; j < dim; j++)
				{
					weave_uint32 cix = 0;
					size_t		bit = (size_t) j * bits;
					int			k;

					for (k = 0; k < bits; k++)
						if (codes[s][(bit + k) >> 3] & (1u << ((bit + k) & 7)))
							cix |= (1u << k);
					sc += lut.lut[(size_t) j * lut.nlevels + cix];
				}
				sc *= scales[s];
				if (sc > blockmax)
					blockmax = sc;

				/* (C2): the bound must dominate every lane.  Float slack only. */
				CHECK(sc <= (double) bound + 1e-3 * (1.0 + fabs((double) bound)),
					  "(C2) VIOLATED bits=%d dim=%d: score %.9g > bound %.9g",
					  bits, dim, sc, (double) bound);
			}

			if (blockmax > 0.0 && (double) bound / blockmax > worst_ratio)
				worst_ratio = (double) bound / blockmax;
		}
		free(lut._alloc);
	}

	printf("  bits=%d dim=%d: worst bound/blockmax over %d coherent blocks = %.2fx\n",
		   bits, dim, ntrial, worst_ratio);

	for (s = 0; s < WEAVE_VEC_BLOCK; s++)
		free(codes[s]);
	free(v);
	free(query);
	free(cen);
	free(recs);
	free(code);
	free(cencode);
	weave_quantizer_free(&q, free);
}

int
main(void)
{
	int			dims[] = {64, 200, 256, 384, 768, 1024, 1536};
	int			ndims = (int) (sizeof(dims) / sizeof(dims[0]));
	int			bits,
				i;

	printf("rotation\n");
	for (i = 0; i < ndims; i++)
		test_rotation(dims[i]);

	printf("codebook\n");
	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
		for (i = 0; i < ndims; i++)
			test_codebook(bits, dims[i]);

	printf("codebook distortion vs bits (must strictly decrease)\n");
	for (i = 0; i < ndims; i++)
		test_distortion_monotone(dims[i]);

	printf("encode/decode\n");
	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		test_encode(bits, 256, 200);
		test_encode(bits, 768, 100);
	}

	printf("pack\n");
	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
	{
		test_pack(bits, 256);
		test_pack(bits, 200);
	}

	printf("block bound (channel.h contract C2)\n");
	for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
		test_block_bound(bits, 256, 40);
	test_block_bound(4, 768, 20);

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
