/*-------------------------------------------------------------------------
 *
 * quantize.c
 *		TurboQuant-style scalar quantization core -- scalar reference.
 *
 * This file is the SOURCE OF TRUTH for the encoding.  Every SIMD kernel in
 * src/vector/kernels.c must reproduce these results bit-for-bit; see the
 * determinism warning in include/weave/quantize.h and the fixture test in
 * test/hegel/test_rotation.c.
 *
 * Deliberately free of PostgreSQL dependencies apart from the optional
 * palloc/pfree passed in by the caller, so test/hegel/ and test/fuzz/ can link
 * it directly.  Same arrangement as weave/for.h.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/quantize.c
 *
 *-------------------------------------------------------------------------
 */
#include "weave/quantize.h"

#include <math.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * ChaCha8, used only as a deterministic byte source
 *
 * Not cryptography: it is here because we need a permutation and a sign vector
 * that are (a) identical on every platform forever and (b) not correlated with
 * coordinate index.  A libc PRNG satisfies neither.  8 rounds is ample for a
 * non-adversarial use and is what the reference implementation froze into the
 * wire format.
 * ------------------------------------------------------------------------- */

/* The frozen seed.  Changing any byte invalidates every index ever built. */
static const weave_uint8 weave_rot_seed[WEAVE_ROT_SEED_BYTES] = {
	0x77, 0x65, 0x61, 0x76, 0x65, 0x2d, 0x72, 0x6f,		/* "weave-ro" */
	0x74, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x2d, 0x76,		/* "tation-v" */
	0x31, 0x00, 0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a,
	0x7c, 0x15, 0xf3, 0x9c, 0xc0, 0x60, 0x5c, 0xed
};

#define ROTL32(v, n)	(((v) << (n)) | ((v) >> (32 - (n))))

#define QROUND(a, b, c, d) \
	do { \
		a += b; d ^= a; d = ROTL32(d, 16); \
		c += d; b ^= c; b = ROTL32(b, 12); \
		a += b; d ^= a; d = ROTL32(d, 8); \
		c += d; b ^= c; b = ROTL32(b, 7); \
	} while (0)

typedef struct ChaChaState
{
	weave_uint32 in[16];
	weave_uint8 buf[64];
	int			pos;			/* next unread byte in buf, 64 = exhausted */
} ChaChaState;

static void
chacha_init(ChaChaState *st, weave_uint64 stream)
{
	static const weave_uint32 sigma[4] = {
		0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
	};
	int			i;

	for (i = 0; i < 4; i++)
		st->in[i] = sigma[i];
	for (i = 0; i < 8; i++)
		st->in[4 + i] =
			(weave_uint32) weave_rot_seed[i * 4] |
			((weave_uint32) weave_rot_seed[i * 4 + 1] << 8) |
			((weave_uint32) weave_rot_seed[i * 4 + 2] << 16) |
			((weave_uint32) weave_rot_seed[i * 4 + 3] << 24);
	st->in[12] = 0;				/* block counter */
	st->in[13] = 0;
	st->in[14] = (weave_uint32) (stream & 0xFFFFFFFFu);
	st->in[15] = (weave_uint32) (stream >> 32);
	st->pos = 64;
}

static void
chacha_block(ChaChaState *st)
{
	weave_uint32 x[16];
	int			i;

	for (i = 0; i < 16; i++)
		x[i] = st->in[i];

	/* 8 rounds = 4 double-rounds */
	for (i = 0; i < 4; i++)
	{
		QROUND(x[0], x[4], x[8], x[12]);
		QROUND(x[1], x[5], x[9], x[13]);
		QROUND(x[2], x[6], x[10], x[14]);
		QROUND(x[3], x[7], x[11], x[15]);
		QROUND(x[0], x[5], x[10], x[15]);
		QROUND(x[1], x[6], x[11], x[12]);
		QROUND(x[2], x[7], x[8], x[13]);
		QROUND(x[3], x[4], x[9], x[14]);
	}

	for (i = 0; i < 16; i++)
	{
		weave_uint32 v = x[i] + st->in[i];

		st->buf[i * 4 + 0] = (weave_uint8) (v & 0xFF);
		st->buf[i * 4 + 1] = (weave_uint8) ((v >> 8) & 0xFF);
		st->buf[i * 4 + 2] = (weave_uint8) ((v >> 16) & 0xFF);
		st->buf[i * 4 + 3] = (weave_uint8) ((v >> 24) & 0xFF);
	}

	if (++st->in[12] == 0)
		st->in[13]++;
	st->pos = 0;
}

static weave_uint32
chacha_next32(ChaChaState *st)
{
	weave_uint32 v;

	if (st->pos > 60)
		chacha_block(st);
	v = (weave_uint32) st->buf[st->pos] |
		((weave_uint32) st->buf[st->pos + 1] << 8) |
		((weave_uint32) st->buf[st->pos + 2] << 16) |
		((weave_uint32) st->buf[st->pos + 3] << 24);
	st->pos += 4;
	return v;
}

/*
 * Uniform in [0, bound) without modulo bias.  Rejection sampling, because a
 * biased permutation would be a silent, dimension-dependent quality loss that
 * no test would notice.
 */
static weave_uint32
chacha_below(ChaChaState *st, weave_uint32 bound)
{
	weave_uint64 limit;
	weave_uint32 v;

	if (bound <= 1)
		return 0;

	/*
	 * Rejection threshold, computed and compared in 64 bits.  Doing this in
	 * uint32 is a trap: when bound divides 2^32 exactly the threshold IS 2^32,
	 * which truncates to 0 and turns the rejection loop below into an infinite
	 * loop.  Every power-of-two bound hits it, and Fisher-Yates over a
	 * power-of-two dimension hits it on the first step.
	 */
	limit = 0x100000000ULL - (0x100000000ULL % (weave_uint64) bound);
	do
	{
		v = chacha_next32(st);
	} while ((weave_uint64) v >= limit);
	return v % bound;
}

/* ---------------------------------------------------------------------------
 * Rotation
 * ------------------------------------------------------------------------- */

/*
 * B = largest power of two dividing dim, clamped into
 * [WEAVE_ROT_MIN_BLOCK, WEAVE_ROT_MAX_BLOCK].  When the clamp makes B stop
 * dividing dim, the remainder coordinates are left untransformed for that
 * round; the permutation still mixes them, so they are not permanently
 * excluded.
 */
static int
rot_block_for_dim(int dim)
{
	int			b = 1;

	while ((dim % (b * 2)) == 0 && b * 2 <= WEAVE_ROT_MAX_BLOCK)
		b *= 2;
	if (b < WEAVE_ROT_MIN_BLOCK)
		b = WEAVE_ROT_MIN_BLOCK;
	if (b > dim)
		b = 1;
	return b;
}

int
weave_rotation_init(WeaveRotation *rot, int dim,
					void *(*alloc) (size_t), void (*dealloc) (void *))
{
	size_t		permbytes,
				signbytes,
				total;
	weave_uint8 *p;
	int			r,
				i;

	if (dim <= 0 || dim > WEAVE_MAX_DIM)
		return -1;

	memset(rot, 0, sizeof(*rot));
	rot->dim = dim;
	rot->block = rot_block_for_dim(dim);
	rot->nblocks = dim / rot->block;
	rot->tail = dim - rot->nblocks * rot->block;
	rot->invsqrtb = (float) (1.0 / sqrt((double) rot->block));

	permbytes = sizeof(weave_uint16) * (size_t) dim;
	signbytes = sizeof(weave_uint64) * (size_t) ((dim + 63) / 64);
	total = (permbytes + signbytes) * WEAVE_ROT_ROUNDS;

	p = (weave_uint8 *) alloc(total);
	if (p == NULL)
		return -1;
	memset(p, 0, total);
	rot->_alloc = p;

	for (r = 0; r < WEAVE_ROT_ROUNDS; r++)
	{
		rot->perm[r] = (weave_uint16 *) p;
		p += permbytes;
		rot->sign[r] = (weave_uint64 *) p;
		p += signbytes;
	}

	/*
	 * One ChaCha stream per round, keyed by (round, dim) so two dimensions never
	 * share a permutation and a change of dim cannot alias.
	 */
	for (r = 0; r < WEAVE_ROT_ROUNDS; r++)
	{
		ChaChaState st;

		chacha_init(&st, ((weave_uint64) r << 32) | (weave_uint32) dim);

		/* identity, then Fisher-Yates downward -- fixed order, so the result is
		 * a pure function of the stream */
		for (i = 0; i < dim; i++)
			rot->perm[r][i] = (weave_uint16) i;
		for (i = dim - 1; i > 0; i--)
		{
			weave_uint32 j = chacha_below(&st, (weave_uint32) (i + 1));
			weave_uint16 t = rot->perm[r][i];

			rot->perm[r][i] = rot->perm[r][j];
			rot->perm[r][j] = t;
		}

		for (i = 0; i < dim; i++)
		{
			if (chacha_next32(&st) & 1u)
				rot->sign[r][i >> 6] |= (weave_uint64) 1 << (i & 63);
		}
	}

	(void) dealloc;				/* freeing is weave_rotation_free's job */
	return 0;
}

void
weave_rotation_free(WeaveRotation *rot, void (*dealloc) (void *))
{
	if (rot->_alloc != NULL)
	{
		dealloc(rot->_alloc);
		rot->_alloc = NULL;
	}
}

/*
 * Normalized Walsh-Hadamard transform on one contiguous block of length n
 * (a power of two).  Self-inverse because of the 1/sqrt(n) scaling.
 *
 * The loop order and the single trailing multiply are part of the wire format.
 * Do not reassociate, do not fuse the multiply into the butterfly, do not
 * vectorize in a way that changes the addition order.
 */
static void
wht_norm(float *v, int n, float invsqrtn)
{
	int			len,
				i,
				j;

	for (len = 1; len < n; len <<= 1)
	{
		for (i = 0; i < n; i += (len << 1))
		{
			for (j = i; j < i + len; j++)
			{
				float		a = v[j];
				float		b = v[j + len];

				v[j] = a + b;
				v[j + len] = a - b;
			}
		}
	}
	for (i = 0; i < n; i++)
		v[i] = v[i] * invsqrtn;
}

void
weave_rotate(const WeaveRotation *rot, float *x)
{
	float		tmp[WEAVE_MAX_DIM];
	int			dim = rot->dim;
	int			r,
				i,
				b;

	for (r = 0; r < WEAVE_ROT_ROUNDS; r++)
	{
		const weave_uint16 *perm = rot->perm[r];
		const weave_uint64 *sign = rot->sign[r];

		for (i = 0; i < dim; i++)
			tmp[i] = x[perm[i]];
		for (i = 0; i < dim; i++)
		{
			if (sign[i >> 6] & ((weave_uint64) 1 << (i & 63)))
				tmp[i] = -tmp[i];
		}
		for (b = 0; b < rot->nblocks; b++)
			wht_norm(tmp + (size_t) b * rot->block, rot->block, rot->invsqrtb);
		/* rot->tail coordinates pass through untouched this round */
		memcpy(x, tmp, sizeof(float) * (size_t) dim);
	}
}

void
weave_rotate_inverse(const WeaveRotation *rot, float *x)
{
	float		tmp[WEAVE_MAX_DIM];
	int			dim = rot->dim;
	int			r,
				i,
				b;

	for (r = WEAVE_ROT_ROUNDS - 1; r >= 0; r--)
	{
		const weave_uint16 *perm = rot->perm[r];
		const weave_uint64 *sign = rot->sign[r];

		/* WHT_norm is self-inverse */
		for (b = 0; b < rot->nblocks; b++)
			wht_norm(x + (size_t) b * rot->block, rot->block, rot->invsqrtb);
		for (i = 0; i < dim; i++)
		{
			if (sign[i >> 6] & ((weave_uint64) 1 << (i & 63)))
				x[i] = -x[i];
		}
		for (i = 0; i < dim; i++)
			tmp[perm[i]] = x[i];
		memcpy(x, tmp, sizeof(float) * (size_t) dim);
	}
}

/* ---------------------------------------------------------------------------
 * Lloyd-Max codebook for the sphere marginal
 *
 * A coordinate of a uniformly random unit vector in R^d has density
 * proportional to (1 - x^2)^((d-3)/2) on [-1, 1].  Evaluated in log space
 * because the exponent reaches ~8000 at d = 16384.
 * ------------------------------------------------------------------------- */

typedef struct BetaParams
{
	double		shape;			/* (d - 3) / 2 */
	int			moment;			/* 0 for the mass integral, 1 for x * f(x) */
} BetaParams;

static double
beta_f(double x, const BetaParams *bp)
{
	double		t = 1.0 - x * x;
	double		lg;

	if (t <= 0.0)
		return 0.0;
	lg = bp->shape * log(t);
	if (lg < -700.0)
		return 0.0;
	return (bp->moment ? x : 1.0) * exp(lg);
}

/*
 * The density is concentrated in |x| <~ 1/sqrt(d).  Integrating over the full
 * [-1, 1] with adaptive Simpson from a 3-point seed misses that spike entirely
 * for large d -- f(-1) = f(1) = 0 and the seed looks smooth.  So clip the domain
 * to where the log-density is above -80 and treat the rest as exactly zero.
 * This is the difference between a codebook that is right and one that is
 * plausible.
 */
static double
beta_domain(double shape)
{
	double		t;

	if (shape <= 0.0)
		return 1.0;
	t = 1.0 - exp(-80.0 / shape);
	if (t <= 0.0)
		return 1e-6;
	if (t >= 1.0)
		return 1.0;
	return sqrt(t);
}

static double
simpson_rec(double a, double b, double fa, double fm, double fb,
			const BetaParams *bp, double whole, int depth)
{
	double		m = 0.5 * (a + b);
	double		lm = 0.5 * (a + m);
	double		rm = 0.5 * (m + b);
	double		flm = beta_f(lm, bp);
	double		frm = beta_f(rm, bp);
	double		left = (m - a) / 6.0 * (fa + 4.0 * flm + fm);
	double		right = (b - m) / 6.0 * (fm + 4.0 * frm + fb);
	double		delta = left + right - whole;

	/*
	 * Depth cap of 14 bounds the work at 2^14 subintervals per call.  This is
	 * not a nicety: with a depth-40 cap and a tolerance tight enough to resolve
	 * the spike, the recursion is exponential and the solve never returns.  The
	 * cap plus the domain clipping in beta_domain() together are what make this
	 * terminate in microseconds instead of never.
	 *
	 * The tolerance is RELATIVE to the local estimate.  It used to be
	 * `1e-11 * (1.0 + fabs(left + right))`, and the `1.0 +` made it effectively
	 * an ABSOLUTE 1e-11 -- fine for the central cells, whose integrals are of
	 * order 1e-2, and useless for the outermost cell at 8 bits, whose integral is
	 * of order 1e-6 and so was resolved to only 1e-5 relative.  The outermost
	 * cell's conditional mean IS absmax, which the block bound (B1) and the TQ+
	 * calibration anchor both read, so that was the least acceptable place to be
	 * imprecise.  test_quantize.c's centroid-condition assertion fails at 7 and 8
	 * bits with the old form; there is no cancellation risk in making it purely
	 * relative because a cell never straddles zero (zero is a boundary, by
	 * symmetry and even n), so the moment-1 integrand has one sign throughout.
	 *
	 * 1e-10 rather than 1e-12: measured over all 49 (bits, dim) pairs the test
	 * covers, 1e-10 reproduces the 1e-12 answer to 1.2e-11 sd -- far below what a
	 * float32 centroid can hold -- for a fifth of the cost.  1e-9 was rejected
	 * despite also passing: it wobbles by 2e-7 sd, which is enough to flip
	 * float32 last bits and so to make the stored codebook depend on a host
	 * libm's ULPs.
	 */
	if (depth >= 14 || fabs(delta) <= 1e-10 * fabs(left + right) + 1e-300)
		return left + right + delta / 15.0;
	return simpson_rec(a, m, fa, flm, fm, bp, left, depth + 1) +
		simpson_rec(m, b, fm, frm, fb, bp, right, depth + 1);
}

static double
beta_integrate(double a, double b, const BetaParams *bp)
{
	double		m,
				fa,
				fm,
				fb,
				whole;

	if (b <= a)
		return 0.0;
	m = 0.5 * (a + b);
	fa = beta_f(a, bp);
	fm = beta_f(m, bp);
	fb = beta_f(b, bp);
	whole = (b - a) / 6.0 * (fa + 4.0 * fm + fb);
	return simpson_rec(a, b, fa, fm, fb, bp, whole, 0);
}

/*
 * Panels used to tabulate the CDF for the equiprobable seed below.  4096 is not
 * a tuning knob to shrug at: the seed's job is that every initial cell carries
 * mass ~1/n, and the narrowest equiprobable cell at n = 256 is about 0.01 sd
 * wide, so the panel grid has to be finer than that or the tabulated CDF cannot
 * resolve adjacent quantiles.  4096 panels over a domain of ~12 sd is ~0.003 sd
 * per panel, comfortably finer.  Cost is 4096 tiny quadratures, microseconds,
 * against a Lloyd loop that does 100k of them.
 */
#define CODEBOOK_CDF_PANELS		4096

/*
 * Seed the n centroids at the EQUIPROBABLE points of the target Beta -- the
 * quantiles at (i + 0.5) / n -- rather than uniformly across [lo, hi].
 *
 * WHY THIS AND NOT THE OBVIOUS UNIFORM SEED.  A uniform seed spreads centroids
 * evenly over the *domain* returned by beta_domain(), and beta_domain() depends
 * on the distribution shape but NOT on the level count n.  At d = 1536 that
 * domain is 12.3 sd wide, because it is the point where the log-density falls
 * below -80 and nothing more.  With n = 4..16 levels every uniform cell still
 * catches mass, so Lloyd moves every centroid and the answer is a true
 * Lloyd-Max codebook.  With n = 64..256 the OUTER uniform cells land in the
 * dead tail, their mass underflows, the empty-cell guard below (which exists to
 * keep the ladder sorted) pins those centroids exactly where they were seeded,
 * and they stay there forever.
 *
 * Counting a level as UNREACHABLE when its cell holds under 1e-6 of the fair
 * share 1/n -- so under one coordinate in 1e6/n would ever select it -- the old
 * seed produced, over dims 64..1536:
 *
 *		bits  levels  unreachable      also: under 1% of fair share
 *		----  ------  --------------   ----------------------------
 *		   5      32  0                0
 *		   6      64  0-2              14-20
 *		   7     128  20-42 (16-33 %)  58-72
 *		   8     256  74-122 (29-48 %) 134-168 (52-66 %)
 *
 * and absmax reported 10.6 sd at bits = 8, d = 1536 where the true Lloyd-Max
 * outermost centroid is 4.59 sd.  A level in dead space is a level no coordinate
 * ever maps to, so an "8-bit" code carried under 6.5 bits of real resolution and
 * any recall measured against it would have understated what 8 bits can do.
 * After this change the unreachable count is 0 at every (bits, dim); the levels
 * still under 1 % of fair share at 7 and 8 bits are 2 and 4, and those are the
 * legitimate outermost/overload cells of a correct Lloyd-Max quantizer, whose
 * share the companding law puts at 1.6e-3 of fair share.
 *
 * An equiprobable seed gives every initial cell mass exactly ~1/n by
 * construction, at every n and every d, which removes the dependence on
 * beta_domain() being level-count-aware.  Lloyd then descends from a feasible
 * point instead of an infeasible one.  The alternative fixes -- shrinking hi
 * until the tail beyond it holds less than 1/(2n), or splitting the
 * highest-distortion cell whenever one comes up empty -- both work, but both
 * leave a solver that is correct only because a heuristic keeps firing.  This
 * one is correct because the initial condition is already feasible.
 *
 * DETERMINISM.  Fixed panel count, fixed panel order, monotone single pass over
 * the tabulated CDF, and the upper half is mirrored onto the lower half so the
 * seed is EXACTLY symmetric about zero rather than symmetric to within
 * rounding.  No randomness, no data, no iteration count that depends on
 * anything but (bits, dim).  Exact symmetry of the seed is also what makes
 * exact symmetry of the *result* structural: for a symmetric c[], the midpoint
 * boundaries are exactly antisymmetric (negation is exact in IEEE), beta_f is
 * exactly even for moment 0 and exactly odd for moment 1, and the adaptive
 * Simpson recursion on a mirrored interval visits mirrored points and makes the
 * same subdivision decisions, so each Lloyd update reproduces the mirror
 * exactly.
 */
static void
codebook_seed_equiprobable(double lo, double hi, const BetaParams *mass,
						   int n, double *c)
{
	double		cum[CODEBOOK_CDF_PANELS + 1];
	double		w = (hi - lo) / (double) CODEBOOK_CDF_PANELS;
	double		total;
	int			half = n / 2;
	int			i,
				k;

	cum[0] = 0.0;
	for (k = 0; k < CODEBOOK_CDF_PANELS; k++)
		cum[k + 1] = cum[k] + beta_integrate(lo + (double) k * w,
											 lo + (double) (k + 1) * w, mass);
	total = cum[CODEBOOK_CDF_PANELS];

	/*
	 * Only the upper half is solved; the lower half is its exact negation.  n is
	 * 1 << bits with bits >= 2, so n is even and there is no middle level.
	 */
	k = CODEBOOK_CDF_PANELS / 2;
	for (i = half; i < n; i++)
	{
		double		p = total * ((double) i + 0.5) / (double) n;
		double		pm;

		/*
		 * Advance to the panel that straddles p.  Monotone in i, so the whole
		 * loop is one pass.  Stopping when cum[k + 1] > p guarantees the panel
		 * has positive mass, so the interpolation below cannot divide by zero.
		 */
		while (k < CODEBOOK_CDF_PANELS - 1 && cum[k + 1] <= p)
			k++;
		pm = cum[k + 1] - cum[k];
		if (pm <= 0.0)
			c[i] = lo + ((double) k + 0.5) * w;
		else
			c[i] = lo + ((double) k + (p - cum[k]) / pm) * w;
		c[n - 1 - i] = -c[i];
	}
}

int
weave_codebook_solve(int bits, int dim, WeaveCodebook *out)
{
	BetaParams	mass,
				mom;
	double		lo,
				hi,
				c[WEAVE_MAX_LEVELS],
				bnd[WEAVE_MAX_LEVELS - 1];
	int			n,
				i,
				iter;

	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		return -1;
	if (dim < 4 || dim > WEAVE_MAX_DIM)
		return -1;

	memset(out, 0, sizeof(*out));
	n = 1 << bits;
	mass.shape = 0.5 * ((double) dim - 3.0);
	mass.moment = 0;
	mom = mass;
	mom.moment = 1;

	hi = beta_domain(mass.shape);
	lo = -hi;

	/* Equiprobable seed: every initial cell carries mass ~1/n, at every n.  See
	 * the long comment on codebook_seed_equiprobable() for why a uniform seed
	 * silently produces dead levels above 4 bits.  With a symmetric log-concave
	 * density, Lloyd converges to the symmetric optimum. */
	codebook_seed_equiprobable(lo, hi, &mass, n, c);

	/*
	 * Solve the Lloyd-Max stationarity conditions by NEWTON, not by Lloyd's
	 * alternating iteration.
	 *
	 * WHY, because this used to be Lloyd's iteration capped at 200 and the cap
	 * was silently doing the deciding.  Lloyd's map on this system propagates a
	 * correction one cell per sweep along a chain of n cells, so its convergence
	 * is O(n^2) sweeps: measured to reach a 1e-14 fixed point it needs 54 sweeps
	 * at n = 4, 700 at n = 16, 10k at n = 64 and 130k at n = 256.  So the old
	 * loop never converged at ANY width -- it always exited on the iteration cap
	 * -- and the error that leaves behind grows with n.  At bits = 8, d = 1536 the
	 * 200-sweep answer put the outermost centroid at 3.86 sd where the true
	 * stationary point is 4.59 sd, 19 % short.  Over-relaxation (omega up to 1.9)
	 * buys 1.4x and Gauss-Seidel is 3x WORSE; neither touches the O(n^2).
	 *
	 * The conditions are F_i(c) = E[X | cell_i(c)] - c_i = 0, and cell_i depends
	 * only on c_{i-1}, c_i, c_{i+1} through its midpoint boundaries -- so the
	 * Jacobian is TRIDIAGONAL and one Newton step costs a Thomas solve, O(n).
	 * With G_i = E[X | cell_i], a_i and b_i the cell's ends and f the density,
	 *
	 *		dG_i/da_i = -f(a_i) (a_i - G_i) / M0_i
	 *		dG_i/db_i =  f(b_i) (b_i - G_i) / M0_i
	 *
	 * and da_i/dc_{i-1} = da_i/dc_i = db_i/dc_i = db_i/dc_{i+1} = 1/2.  Measured:
	 * 3 to 7 iterations at every (bits, dim) here, reaching the same fixed point
	 * as 130k Lloyd sweeps to all the digits a float can hold.
	 *
	 * Determinism is unaffected: fixed sweep order, fixed Thomas elimination
	 * order, a termination test on a fixed relative threshold, and a fixed cap.
	 * Newton also preserves the exact mirror symmetry of the seed, because the
	 * residual is exactly antisymmetric and the Thomas recurrence on an exactly
	 * mirror-symmetric tridiagonal system produces an exactly antisymmetric step.
	 */
	for (iter = 0; iter < 100; iter++)
	{
		double		gc[WEAVE_MAX_LEVELS],
					jlo[WEAVE_MAX_LEVELS],
					jdg[WEAVE_MAX_LEVELS],
					jup[WEAVE_MAX_LEVELS],
					rhs[WEAVE_MAX_LEVELS],
					tc[WEAVE_MAX_LEVELS],
					td[WEAVE_MAX_LEVELS];
		double		maxmove = 0.0;
		double		prev;
		int			bad = 0;

		for (i = 0; i < n - 1; i++)
			bnd[i] = 0.5 * (c[i] + c[i + 1]);

		for (i = 0; i < n; i++)
		{
			double		a = (i == 0) ? lo : bnd[i - 1];
			double		b = (i == n - 1) ? hi : bnd[i];
			double		m0 = beta_integrate(a, b, &mass);
			double		m1 = beta_integrate(a, b, &mom);
			double		dda,
						ddb;

			/*
			 * An empty cell has no conditional mean, so it has no Newton row
			 * either; pin it (identity row, zero residual) so the ladder stays
			 * sorted, which the branchless quantizer depends on.
			 *
			 * This branch used to be load-bearing and wrong to rely on: with the
			 * old uniform seed it fired for most of the levels above 4 bits and
			 * pinned them in the dead tail where they had been seeded (see
			 * codebook_seed_equiprobable()).  With an equiprobable seed no cell
			 * starts empty and no cell becomes empty, so it is now unreachable in
			 * practice -- test_quantize.c's no-dead-levels assertion is what
			 * holds that claim honest.  It stays as a defensive floor because
			 * dividing by an underflowed mass would give a NaN centroid and an
			 * unsorted ladder, which is a far worse failure than a level that did
			 * not move.
			 */
			if (m0 <= 1e-300)
			{
				gc[i] = c[i];
				jlo[i] = 0.0;
				jup[i] = 0.0;
				jdg[i] = -1.0;
				rhs[i] = 0.0;
				continue;
			}

			gc[i] = m1 / m0;
			dda = -beta_f(a, &mass) * (a - gc[i]) / m0;
			ddb = beta_f(b, &mass) * (b - gc[i]) / m0;
			jlo[i] = (i == 0) ? 0.0 : 0.5 * dda;
			jup[i] = (i == n - 1) ? 0.0 : 0.5 * ddb;
			jdg[i] = jlo[i] + jup[i] - 1.0;
			rhs[i] = c[i] - gc[i];
		}

		/*
		 * Thomas elimination.  jdg[i] is 0.5*(dda+ddb) - 1 and the off-diagonals
		 * sum to 0.5*(dda+ddb), so the system is only WEAKLY diagonally dominant
		 * and gets closer to singular as n grows -- that near-null direction is
		 * exactly the slow Lloyd mode, and resolving it is the point of using
		 * Newton at all.  Bail out to the Lloyd fallback below rather than
		 * dividing by something that has underflowed.
		 */
		td[0] = jdg[0];
		if (fabs(td[0]) < 1e-300)
			bad = 1;
		else
		{
			tc[0] = jup[0] / td[0];
			rhs[0] = rhs[0] / td[0];
			for (i = 1; i < n; i++)
			{
				td[i] = jdg[i] - jlo[i] * tc[i - 1];
				if (fabs(td[i]) < 1e-300)
				{
					bad = 1;
					break;
				}
				tc[i] = jup[i] / td[i];
				rhs[i] = (rhs[i] - jlo[i] * rhs[i - 1]) / td[i];
			}
		}

		if (!bad)
		{
			prev = rhs[n - 1];
			c[n - 1] += prev;
			maxmove = fabs(prev);
			for (i = n - 2; i >= 0; i--)
			{
				double		delta = rhs[i] - tc[i] * prev;

				c[i] += delta;
				prev = delta;
				if (fabs(delta) > maxmove)
					maxmove = fabs(delta);
			}

			/*
			 * A Newton step is only accepted if it left the ladder strictly
			 * sorted and finite.  It never failed at any (bits, dim) tested, but
			 * an unsorted ladder would break the branchless quantizer silently,
			 * so the check is not optional.
			 */
			for (i = 0; i < n; i++)
			{
				if (!(c[i] > lo && c[i] < hi))
					bad = 1;
				if (i > 0 && !(c[i] > c[i - 1]))
					bad = 1;
			}
		}

		if (bad)
		{
			/*
			 * Fallback: one plain Lloyd sweep, which is unconditionally
			 * monotonicity-preserving (a conditional mean lies strictly inside
			 * its own cell, and the cells are ordered).  Slow, but it can only
			 * descend, so a solve that Newton cannot handle still terminates at
			 * the iteration cap with a valid, if less exact, codebook.
			 */
			maxmove = 0.0;
			for (i = 0; i < n; i++)
			{
				if (fabs(gc[i] - c[i]) > maxmove)
					maxmove = fabs(gc[i] - c[i]);
				c[i] = gc[i];
			}
		}

		/*
		 * Relative to the integration domain, so the test is scale-free across
		 * dimensions: hi shrinks like 1/sqrt(dim) and an absolute 1e-12 would be
		 * a far tighter demand at dim = 16384 than at dim = 64.  1e-11 * hi is
		 * three orders below what a float32 centroid can represent, and Newton's
		 * quadratic convergence means the step goes from 1e-4 to below this in
		 * one or two iterations, so the exact threshold cannot change the stored
		 * floats.
		 */
		if (maxmove < 1e-11 * hi)
			break;
	}

	out->bits = bits;
	out->dim = dim;
	out->nlevels = n;
	for (i = 0; i < n; i++)
		out->centroid[i] = (float) c[i];
	for (i = 0; i < n - 1; i++)
		out->boundary[i] = (float) (0.5 * (c[i] + c[i + 1]));
	out->absmax = (float) fmax(fabs(c[0]), fabs(c[n - 1]));
	return 0;
}

/*
 * Process-local memo.  The solve is a few ms at 2 bits and ~20 ms at 8, and the
 * result is a pure function of the key, so a lossy fixed-size cache with no
 * locking is correct: a racing writer can only ever store the same bytes another
 * would have.  Nothing here is on disk (see the note on weave_codebook_get() in
 * the header), so the slot count is free to change at any time.
 *
 * SIZED, not guessed.  A backend touches one (bits, dim) per index it queries,
 * so the slot count only needs to cover the distinct indexes one session uses;
 * 8 was chosen when there were three widths and it is still ample -- a session
 * would have to query nine differently-shaped vector indexes to start thrashing,
 * and even then the cost is a re-solve, not a wrong answer.
 *
 * The reason to think about it at all is that WEAVE_MAX_LEVELS went from 16 to
 * 256 when WEAVE_BITS_MAX went from 4 to 8, so centroid[] + boundary[] grew 16x
 * and this static table grew with them: 8 slots x (256 + 255) floats + 3 ints is
 * ~16 KB of BSS per backend, up from ~1 KB.  Raising the slot count to cover
 * "widths 2..8 x several dims" would mean 40+ slots and ~80 KB of BSS in every
 * backend to serve a cache that a single session almost never needs, which is
 * the wrong trade -- so 8 STAYS, deliberately, and this comment exists so the
 * next person does not have to re-derive that the growth is in the struct rather
 * than in the demand for slots.
 */
#define CODEBOOK_MEMO_SLOTS		8

static struct
{
	int			bits;
	int			dim;
	WeaveCodebook cb;
}			codebook_memo[CODEBOOK_MEMO_SLOTS];

static int	codebook_memo_next = 0;

int
weave_codebook_get(int bits, int dim, WeaveCodebook *out)
{
	int			i;

	for (i = 0; i < CODEBOOK_MEMO_SLOTS; i++)
	{
		if (codebook_memo[i].bits == bits && codebook_memo[i].dim == dim)
		{
			*out = codebook_memo[i].cb;
			return 0;
		}
	}

	if (weave_codebook_solve(bits, dim, out) != 0)
		return -1;

	i = codebook_memo_next;
	codebook_memo_next = (codebook_memo_next + 1) % CODEBOOK_MEMO_SLOTS;
	codebook_memo[i].cb = *out;
	codebook_memo[i].dim = dim;
	codebook_memo[i].bits = bits;	/* last, so a reader never sees a half-
									 * populated slot as valid */
	return 0;
}

/* ---------------------------------------------------------------------------
 * Encode / decode
 * ------------------------------------------------------------------------- */

/* Branchless comparison ladder over the sorted boundaries. */
static inline int
quantize_one(const WeaveCodebook *cb, float x)
{
	int			code = 0;
	int			i;

	for (i = 0; i < cb->nlevels - 1; i++)
		code += (x > cb->boundary[i]) ? 1 : 0;
	return code;
}

static inline void
bits_put(weave_uint8 *dst, int index, int bits, weave_uint32 value)
{
	size_t		bit = (size_t) index * bits;
	int			k;

	for (k = 0; k < bits; k++)
	{
		size_t		b = bit + k;

		if (value & (1u << k))
			dst[b >> 3] |= (weave_uint8) (1u << (b & 7));
		else
			dst[b >> 3] &= (weave_uint8) ~(1u << (b & 7));
	}
}

static inline weave_uint32
bits_get(const weave_uint8 *src, int index, int bits)
{
	size_t		bit = (size_t) index * bits;
	weave_uint32 v = 0;
	int			k;

	for (k = 0; k < bits; k++)
	{
		size_t		b = bit + k;

		if (src[b >> 3] & (weave_uint8) (1u << (b & 7)))
			v |= (1u << k);
	}
	return v;
}

int
weave_quantizer_init(WeaveQuantizer *q, int dim, int bits,
					 const WeaveCalibration *cal,
					 void *(*alloc) (size_t), void (*dealloc) (void *))
{
	memset(q, 0, sizeof(*q));
	if (weave_codebook_get(bits, dim, &q->cb) != 0)
		return -1;
	if (weave_rotation_init(&q->rot, dim, alloc, dealloc) != 0)
		return -1;
	q->dim = dim;
	q->bits = bits;
	q->codebytes = (dim * bits + 7) / 8;
	q->cal = cal;
	return 0;
}

void
weave_quantizer_free(WeaveQuantizer *q, void (*dealloc) (void *))
{
	weave_rotation_free(&q->rot, dealloc);
}

int
weave_encode(const WeaveQuantizer *q, const float *v,
			 weave_uint8 *code, float *out_norm, float *out_scale)
{
	float		x[WEAVE_MAX_DIM];
	double		norm2 = 0.0;
	double		norm;
	double		dot = 0.0;
	int			dim = q->dim;
	int			j;

	for (j = 0; j < dim; j++)
		norm2 += (double) v[j] * (double) v[j];
	norm = sqrt(norm2);

	/*
	 * A zero vector has no direction and no defined renormalization scale.
	 * Refuse it here rather than substituting zeros, which would produce a row
	 * that scores identically against every query -- a wrong answer that looks
	 * like a working index.
	 */
	if (!(norm > 1e-30))
		return -1;

	for (j = 0; j < dim; j++)
		x[j] = (float) ((double) v[j] / norm);

	weave_rotate(&q->rot, x);

	if (q->cal != NULL && q->cal->nsample > 0)
	{
		for (j = 0; j < dim; j++)
			x[j] = (x[j] - q->cal->shift[j]) * q->cal->cscale[j];
	}

	memset(code, 0, (size_t) q->codebytes);
	for (j = 0; j < dim; j++)
	{
		int			cix = quantize_one(&q->cb, x[j]);

		bits_put(code, j, q->bits, (weave_uint32) cix);
		dot += (double) x[j] * (double) q->cb.centroid[cix];
	}

	/*
	 * scale = norm / <x, xhat>.  <x, xhat> is the projection of the
	 * reconstruction onto the true unit direction, and it is always < 1 because
	 * quantization shrinks toward the origin.  Dividing it out is what makes
	 * <q, scale * xhat> an unbiased estimator of <q, v>, which in turn is what
	 * lets us skip a float32 rerank pass at moderate k.  See
	 * doc/specs/VECTOR_CHANNEL.md sect. 2.
	 *
	 * A non-positive dot means the reconstruction points away from the input --
	 * only reachable with a pathological calibration.  Clamp rather than emit a
	 * negative scale, which would invert the sign of every score for this row.
	 */
	if (dot < 1e-12)
		dot = 1e-12;

	*out_norm = (float) norm;
	*out_scale = (float) (norm / dot);
	return 0;
}

void
weave_decode(const WeaveQuantizer *q, const weave_uint8 *code,
			 float scale, float *out)
{
	int			j;

	for (j = 0; j < q->dim; j++)
	{
		weave_uint32 cix = bits_get(code, j, q->bits);

		out[j] = q->cb.centroid[cix] * scale;
	}

	if (q->cal != NULL && q->cal->nsample > 0)
	{
		for (j = 0; j < q->dim; j++)
			out[j] = out[j] / q->cal->cscale[j] + q->cal->shift[j] * scale;
	}

	weave_rotate_inverse(&q->rot, out);
}

/* ---------------------------------------------------------------------------
 * Query lookup table and the block bound
 * ------------------------------------------------------------------------- */

int
weave_query_lut_build(WeaveQueryLut *out, const WeaveQuantizer *q,
					  const float *query, void *(*alloc) (size_t))
{
	float		x[WEAVE_MAX_DIM];
	int			dim = q->dim;
	int			n = q->cb.nlevels;
	double		bound = 0.0;
	double		qn2 = 0.0;
	int			j,
				c;

	memset(out, 0, sizeof(*out));
	out->lut = (float *) alloc(sizeof(float) * (size_t) dim * (size_t) n);
	if (out->lut == NULL)
		return -1;
	out->_alloc = out->lut;
	out->dim = dim;
	out->nlevels = n;

	for (j = 0; j < dim; j++)
	{
		qn2 += (double) query[j] * (double) query[j];
		x[j] = query[j];
	}

	/*
	 * The query goes through the SAME rotation as the data.  It is NOT
	 * normalized and NOT quantized: keeping the query in full precision is free
	 * (one vector) and removes a whole error term.
	 */
	weave_rotate(&q->rot, x);

	if (q->cal != NULL && q->cal->nsample > 0)
	{
		for (j = 0; j < dim; j++)
			x[j] = (x[j] - q->cal->shift[j]) * q->cal->cscale[j];
	}

	for (j = 0; j < dim; j++)
	{
		double		best = -1e300;

		for (c = 0; c < n; c++)
		{
			double		p = (double) x[j] * (double) q->cb.centroid[c];

			out->lut[(size_t) j * n + c] = (float) p;
			if (p > best)
				best = p;
		}
		/* The codebook is symmetric about zero, so best >= 0 always; the
		 * assertion that L(q) >= 0 in weave/quantize.h rests on that. */
		bound += best;
	}

	out->lutbound = (float) bound;
	out->qnorm2 = (float) qn2;
	out->qnorm = (float) sqrt(qn2);
	return 0;
}

/*
 * <q, c> for a centroid stored as a quantized code -- the (B3) bound's leading
 * term.  Same gather-and-add the scan kernels do, just for one vector.
 */
float
weave_lut_score_code(const WeaveQueryLut *lut, int bits,
					 const weave_uint8 *code, float scale)
{
	double		acc = 0.0;
	int			j;

	for (j = 0; j < lut->dim; j++)
	{
		weave_uint32 cix = bits_get(code, j, bits);

		acc += lut->lut[(size_t) j * lut->nlevels + cix];
	}
	return (float) (acc * (double) scale);
}

/* ---------------------------------------------------------------------------
 * TQ+ calibration
 * ------------------------------------------------------------------------- */

static int
cmp_float(const void *a, const void *b)
{
	float		x = *(const float *) a;
	float		y = *(const float *) b;

	return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

int
weave_calibration_fit(WeaveCalibration *cal, const WeaveCodebook *cb,
					  const float *sample, int nsample, int dim,
					  void *(*alloc) (size_t))
{
	float	   *col;
	weave_uint8 *p;
	double		anchor;
	int			j,
				i,
				qidx;

	memset(cal, 0, sizeof(*cal));
	if (nsample < WEAVE_CALIB_MIN_ROWS || dim <= 0 || dim > WEAVE_MAX_DIM)
		return -1;

	p = (weave_uint8 *) alloc(sizeof(float) * (size_t) dim * 2 +
							  sizeof(float) * (size_t) nsample);
	if (p == NULL)
		return -1;
	cal->_alloc = p;
	cal->shift = (float *) p;
	cal->cscale = (float *) (p + sizeof(float) * (size_t) dim);
	col = (float *) (p + sizeof(float) * (size_t) dim * 2);
	cal->dim = dim;
	cal->nsample = nsample;

	/*
	 * The anchor probability is read off the codebook rather than hardcoded: it
	 * is the mass the outermost cell should hold, which varies with bit width
	 * (~0.93 at 2 bits, ~0.996 at 4 bits).  Hardcoding one value silently
	 * mis-calibrates the other widths.
	 */
	anchor = 1.0 - 0.5 / (double) cb->nlevels;
	qidx = (int) (anchor * (double) (nsample - 1));
	if (qidx < 0)
		qidx = 0;
	if (qidx > nsample - 1)
		qidx = nsample - 1;

	for (j = 0; j < dim; j++)
	{
		double		med,
					hiq,
					sc;

		for (i = 0; i < nsample; i++)
			col[i] = sample[(size_t) i * dim + j];
		qsort(col, (size_t) nsample, sizeof(float), cmp_float);

		med = col[(nsample - 1) / 2];
		hiq = col[qidx];

		/* Map the empirical high quantile onto the outermost centroid. */
		if (hiq - med > 1e-12)
			sc = (double) cb->absmax / (hiq - med);
		else
			sc = 1.0;

		/* Clamp so the query-side inverse transform can never blow up.  An
		 * uncalibrated index beats one whose dequantization overflows. */
		if (sc < 0.05)
			sc = 0.05;
		if (sc > 20.0)
			sc = 20.0;

		cal->shift[j] = (float) med;
		cal->cscale[j] = (float) sc;
	}

	return weave_calibration_validate(cal);
}

int
weave_calibration_validate(const WeaveCalibration *cal)
{
	int			j;

	if (cal->nsample == 0)
		return 0;				/* identity */
	if (cal->dim <= 0 || cal->dim > WEAVE_MAX_DIM)
		return -1;
	if (cal->shift == NULL || cal->cscale == NULL)
		return -1;

	for (j = 0; j < cal->dim; j++)
	{
		if (!(cal->cscale[j] >= 0.05f && cal->cscale[j] <= 20.0f))
			return -1;
		if (!(cal->shift[j] > -10.0f && cal->shift[j] < 10.0f))
			return -1;
	}
	return 0;
}
