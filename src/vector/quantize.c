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
	 */
	if (depth >= 14 || fabs(delta) <= 1e-11 * (1.0 + fabs(left + right)))
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

	/* Symmetric uniform seed inside the effective domain.  With a symmetric
	 * log-concave density, Lloyd converges to the symmetric optimum. */
	for (i = 0; i < n; i++)
		c[i] = lo + (hi - lo) * ((double) i + 0.5) / (double) n;

	for (iter = 0; iter < 200; iter++)
	{
		double		maxmove = 0.0;

		for (i = 0; i < n - 1; i++)
			bnd[i] = 0.5 * (c[i] + c[i + 1]);

		for (i = 0; i < n; i++)
		{
			double		a = (i == 0) ? lo : bnd[i - 1];
			double		b = (i == n - 1) ? hi : bnd[i];
			double		m0 = beta_integrate(a, b, &mass);
			double		m1 = beta_integrate(a, b, &mom);
			double		nc;

			/*
			 * An empty cell has no conditional mean.  Leaving the centroid where
			 * it is (rather than collapsing it to zero) keeps the ladder sorted,
			 * which the branchless quantizer depends on.
			 */
			if (m0 <= 1e-300)
				continue;
			nc = m1 / m0;
			if (fabs(nc - c[i]) > maxmove)
				maxmove = fabs(nc - c[i]);
			c[i] = nc;
		}

		if (maxmove < 1e-12)
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
 * Process-local memo.  The solve is 25-100 ms and the result is a pure function
 * of the key, so a lossy fixed-size cache with no locking is correct: a racing
 * writer can only ever store the same bytes another would have.
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
