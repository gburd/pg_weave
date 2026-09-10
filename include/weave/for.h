/*-------------------------------------------------------------------------
 *
 * pg_weave_for.h
 *		Frame-of-reference (FOR) integer bit-packing codec for pg_weave.
 *
 * The compression core of the weave posting blocks, extracted here as pure
 * standalone C (no PostgreSQL includes) so it can be exercised by standalone
 * property tests (test/hegel/) while remaining the single source of truth --
 * pg_weave_am.c #includes this header rather than carrying its own copy.
 *
 * When included from a PostgreSQL backend TU, postgres.h has already defined
 * uint64/uint32; the guards below keep this header from redefining them.  When
 * included from a standalone test, it provides them from <stdint.h>.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_for.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_FOR_H
#define WEAVE_FOR_H

#include <stdint.h>
#include <string.h>

/*
 * PostgreSQL's c.h defines uint64/uint32 and UINT64CONST; only supply them when
 * we are compiled outside the backend (postgres.h not included).  PG guards its
 * own typedefs with HAVE_UINT64 etc., so we key off UINT64CONST, which is only
 * defined by c.h.
 */
#ifndef UINT64CONST
typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint8_t uint8;
#define UINT64CONST(x) ((uint64) x##ULL)
#endif

/*
 * FOR (frame-of-reference) bit-packing of a block's three columns (docid gaps,
 * tfs, doclens) as Structure-of-Arrays.  Each column is [u8 bitwidth][packed
 * little-endian bitstream of `n` values].  Small, uniform values (the common
 * case for delta-coded docids and tf/doclen) pack to a few bits each instead of
 * a whole varint byte -- this both shrinks the index and cuts decode cost.
 * Width 0 means every value is 0 (no bits stored).
 */
static inline int
weave_bitwidth(uint64 maxval)
{
	int			w = 0;

	while (maxval)
	{
		w++;
		maxval >>= 1;
	}
	return w;				/* 0 if maxval==0 */
}

/* pack n values at `width` bits each into buf starting at bit offset 0; returns
 * bytes written (including the leading width byte). */
static inline int
weave_for_pack(const uint64 *vals, int n, unsigned char *buf)
{
	uint64		maxv = 0;
	int			width;
	int			i;
	int			bitpos;
	int			nbytes;

	for (i = 0; i < n; i++)
		if (vals[i] > maxv)
			maxv = vals[i];
	width = weave_bitwidth(maxv);
	buf[0] = (unsigned char) width;
	if (width == 0)
		return 1;
	nbytes = 1 + (n * width + 7) / 8;
	memset(buf + 1, 0, nbytes - 1);
	bitpos = 0;
	for (i = 0; i < n; i++)
	{
		uint64		v = vals[i];
		int			b;

		for (b = 0; b < width; b++)
		{
			if (v & ((uint64) 1 << b))
			{
				int			abs = bitpos + b;

				buf[1 + (abs >> 3)] |= (unsigned char) (1 << (abs & 7));
			}
		}
		bitpos += width;
	}
	return nbytes;
}

/* unpack n values at the width stored in buf[0]; returns bytes consumed. */
static inline int
weave_for_unpack(const unsigned char *buf, int n, uint64 *out)
{
	int			width = buf[0];
	int			i;
	int			bitpos;
	const unsigned char *bits;
	uint64		mask;

	if (width == 0)
	{
		for (i = 0; i < n; i++)
			out[i] = 0;
		return 1;
	}

	bits = buf + 1;
	mask = (width >= 64) ? ~UINT64CONST(0) : (((uint64) 1 << width) - 1);
	bitpos = 0;
	for (i = 0; i < n; i++)
	{
		int			byte = bitpos >> 3;
		int			shift = bitpos & 7;
		uint64		v;

		/*
		 * A value spans at most width+7 bits, so a single unaligned load of
		 * the covering bytes at `byte` plus shift/mask extracts it when
		 * shift+width <= 64.  For the rare wide case (shift+width > 64)
		 * assemble across a 9-byte window.  Replaces the per-bit inner loop
		 * that dominated posting decode.
		 */
		if (shift + width <= 64)
		{
			uint64		w = 0;
			int			nb = (shift + width + 7) >> 3;
			int			k;

			for (k = 0; k < nb; k++)
				w |= (uint64) bits[byte + k] << (k * 8);
			v = (w >> shift) & mask;
		}
		else
		{
			uint64		lo = 0,
						hi;
			int			k;

			for (k = 0; k < 8; k++)
				lo |= (uint64) bits[byte + k] << (k * 8);
			hi = bits[byte + 8];
			/*
			 * shift is in [0,7]; (64 - shift) == 64 only when shift == 0, which
			 * reaches this wide branch solely for a corrupt width > 64 (a valid
			 * width <= 64 gives shift >= 1 here).  A 64-bit shift is undefined in
			 * C, so when shift == 0 the high word contributes nothing -- take lo
			 * directly.  Keeps every valid-width result identical while making a
			 * corrupt on-disk width byte safe rather than UB.
			 */
			if (shift == 0)
				v = lo & mask;
			else
				v = ((lo >> shift) | (hi << (64 - shift))) & mask;
		}
		out[i] = v;
		bitpos += width;
	}
	return 1 + (n * width + 7) / 8;
}

/* Byte length of a FOR column of n values, given its leading width byte -- used
 * to skip a column (e.g. tf/doclen) without unpacking any value. */
static inline int
weave_for_bytelen(const unsigned char *buf, int n)
{
	int			width = buf[0];

	return (width == 0) ? 1 : 1 + (n * width + 7) / 8;
}

/* Random-access one value at index i from a FOR column (buf[0] = width).
 * O(width), no full-column unpack -- lets the WAND hot path decode a posting's
 * tf/doclen only when it is actually scored, skipping pruned blocks entirely. */
static inline uint64
weave_for_get(const unsigned char *buf, int i)
{
	int			width = buf[0];
	const unsigned char *bits;
	int			bitpos;
	int			byte;
	int			shift;
	uint64		mask;

	if (width == 0)
		return 0;
	bits = buf + 1;
	bitpos = i * width;
	byte = bitpos >> 3;
	shift = bitpos & 7;
	mask = (width >= 64) ? ~UINT64CONST(0) : (((uint64) 1 << width) - 1);

	/*
	 * Same word-load/shift/mask extraction weave_for_unpack() uses, rather than
	 * the per-bit loop this function used to run.  A value spans at most
	 * width+7 bits, so the covering bytes give it in one pass when
	 * shift+width <= 64; the wide case assembles across a 9-byte window.
	 *
	 * This is on the per-posting hot path twice over: wand_contrib_cur() reads
	 * `tf` through it for EVERY scored posting, and the v3 inline-doclen path
	 * reads |D| through it too.  The per-bit version cost `width` branches per
	 * call (profiled 2026-09-06).
	 */
	if (shift + width <= 64)
	{
		uint64		w = 0;
		int			nb = (shift + width + 7) >> 3;
		int			k;

		for (k = 0; k < nb; k++)
			w |= (uint64) bits[byte + k] << (k * 8);
		return (w >> shift) & mask;
	}
	else
	{
		uint64		lo = 0;
		int			k;

		for (k = 0; k < 8; k++)
			lo |= (uint64) bits[byte + k] << (k * 8);
		/*
		 * shift == 0 here only for a corrupt width > 64 (a valid width <= 64
		 * gives shift >= 1 in this branch); a 64-bit shift is UB, and the high
		 * word contributes nothing in that case.  Mirrors weave_for_unpack().
		 */
		if (shift == 0)
			return lo & mask;
		return ((lo >> shift) | ((uint64) bits[byte + 8] << (64 - shift))) & mask;
	}
}

/*
 * doclen quantization (Lucene/Tantivy "fieldnorm"): map an integer document
 * length to a single byte and back.  Used by the per-segment doclen sidecar
 * (format v4) so BM25 length-normalization is a byte, not a per-posting uint32.
 *
 * Encoding is a mini 8-bit float: a 5-bit exponent and a 3-bit mantissa
 * (SmallFloat.intToByte4 / byte4ToInt).  It is monotonic (a longer doc never
 * encodes to a smaller byte after decode), exact for the smallest lengths, and
 * increasingly coarse for large lengths -- exactly right for BM25, where length
 * enters only through the normalization denominator, so a few-percent error on a
 * long document is invisible in the score ORDERING.  avgdl stays EXACT (from
 * WeaveSegMeta.sumdoclen/ndocs); only the per-doc denominator is quantized.
 *
 * Decode is total (every one of the 256 bytes maps to a length), so a scorer
 * can precompute a 256-entry length-norm factor table and index it by the byte.
 */
static inline uint8
weave_doclen_to_byte(uint32 len)
{
	uint32		exp;
	uint32		mant;
	uint32		e;
	uint32		hb;

	if (len == 0)
		return 0;
	if (len <= 7)
		return (uint8) len;		/* 0..7 exact (exponent field 0) */
	hb = 31 - (uint32) __builtin_clz(len);	/* highest set bit */
	exp = hb - 3;						/* keep 4 significant bits: 1 + 3 mantissa */
	mant = (len >> exp) & 0x07;
	e = exp + 1;						/* exponent field (>=1 for len>=8) */
	if (e > 31)
	{
		e = 31;
		mant = 7;
	}
	return (uint8) ((e << 3) | mant);
}

static inline uint32
weave_byte_to_doclen(uint8 b)
{
	uint32		mant = b & 0x07;
	uint32		e = (b >> 3) & 0x1F;

	if (e == 0)
		return mant;					/* 0..7 exact */
	return (0x08u | mant) << (e - 1);	/* implicit leading 1, shift back */
}

/*
 * Gated ascending-resume-hint walk over a doclen sidecar BLOCK, falling back
 * to a bisect, in the two representations a resident block can be in (see
 * WeaveDoclenResident in src/am/am.c):
 *
 *   weave_doclen_walk_abs() -- v5, absolute offsets from `base`, packed and
 *   read in place with weave_for_get(), never unpacked;
 *
 *   weave_doclen_walk_arr() -- v4, gap-coded on disk but already unpacked and
 *   prefix-summed into a plain ascending uint64 array by the caller.
 *
 * Both return the block's raw length BYTE (1..255; the caller applies
 * weave_byte_to_doclen()) for a present docid, or -1 if `docid` is absent from
 * the block (a gap, or outside its span). `*hint` is read as the index to try
 * first and rewritten to "one past the hit" on success, exactly like
 * WeaveDoclenResident.hint; a stale, out-of-range, or cold (0) hint is safe --
 * the walk gate below simply doesn't fire and the bisect finds the answer.
 *
 * This pair is extracted verbatim from weave_doclen_cursor_lookup()'s isabs
 * and !isabs branches so that src/am/am.c and the standalone property test in
 * test/hegel/test_doclen_block.c share the ACTUAL gated-walk code rather than
 * the test hand-transcribing it -- which is what let a real gate change
 * (WEAVE_DOCLEN_WALK_WINDOW below) go out with zero test coverage: the test's
 * old copy simply didn't have the gate, so it kept passing regardless of what
 * the gate did.  See doc/TESTING.md and include/weave/for.h's own top comment
 * for why a backend-independent core is the house pattern for exactly this
 * failure class.
 *
 * The walk only pays off when the target is within its own window of the
 * resume hint -- true for a common term whose consecutive docids land in
 * adjacent block positions.  A rare/mid term's sparse stride (~50-200 docids)
 * usually lands the target either in a different block (caught by the caller
 * before this function is even entered) or far past the hint WITHIN the
 * block, so an UNCONDITIONAL walk paid its whole window and then bisected
 * anyway: ~15 weave_for_get calls per lookup measured for ranked mid k=10,
 * against ~7 for a plain bisect (bench/RESULTS_L17.md's follow-up 1).  The
 * fix is one O(1) read at the hint deciding whether the walk could plausibly
 * reach the target ("would WEAVE_DOCLEN_WALK_WINDOW index steps be enough")
 * before committing to it, rather than always walking then bisecting anyway.
 *
 * A FRESH hint (index 0, value 0, i.e. right after a block change) with a far
 * target is the dominant case in practice and the one this gate exists for:
 * `v <= want` holds (0 <= anything) but `want - v < WEAVE_DOCLEN_WALK_WINDOW`
 * fails for anything far away, so the walk is skipped after exactly one read
 * instead of walking WEAVE_DOCLEN_WALK_WINDOW steps and overshooting into the
 * bisect anyway.  A fix that widened the window, or that gated on `want < W`
 * instead of `want - v < W`, would still pass every "does it find the right
 * answer" check while reintroducing the overshoot this exists to avoid --
 * which is exactly why the property test also mutates this constant and this
 * comparison and checks that it FAILS.
 */
#define WEAVE_DOCLEN_WALK_WINDOW	8

static inline int
weave_doclen_walk_abs(const unsigned char *raw, const uint8 *rawbyte,
					   uint64 base, int n, uint64 docid, int *hint)
{
	int			rlo = 0,
				rhi = n - 1;
	int			i = *hint;
	uint64		want;

	if (docid < base)
		return -1;
	want = docid - base;

	if (i < 0 || i >= n)
		i = 0;

	if (n > 0)
	{
		uint64		v = weave_for_get(raw, i);

		if (v <= want && want - v < WEAVE_DOCLEN_WALK_WINDOW)
		{
			int			lim = i + WEAVE_DOCLEN_WALK_WINDOW;

			if (lim > n)
				lim = n;
			for (;;)
			{
				if (v == want)
				{
					*hint = i + 1;
					return rawbyte[i];
				}
				if (v > want)
					break;		/* overshot: docid is absent (gap) or behind us */
				if (++i >= lim)
					break;
				v = weave_for_get(raw, i);
			}
		}
	}

	while (rlo <= rhi)
	{
		int			mid = (rlo + rhi) >> 1;
		uint64		v = weave_for_get(raw, mid);

		if (v < want)
			rlo = mid + 1;
		else if (v > want)
			rhi = mid - 1;
		else
		{
			*hint = mid + 1;
			return rawbyte[mid];
		}
	}
	return -1;
}

static inline int
weave_doclen_walk_arr(const uint64 *docidarr, const uint8 *bytearr, int n,
					   uint64 docid, int *hint)
{
	int			rlo = 0,
				rhi = n - 1;
	int			i = *hint;

	if (i < 0 || i >= n)
		i = 0;

	if (n > 0)
	{
		uint64		v = docidarr[i];

		if (v <= docid && docid - v < WEAVE_DOCLEN_WALK_WINDOW)
		{
			int			lim = i + WEAVE_DOCLEN_WALK_WINDOW;

			if (lim > n)
				lim = n;
			for (;;)
			{
				if (v == docid)
				{
					*hint = i + 1;
					return bytearr[i];
				}
				if (v > docid)
					break;		/* overshot: docid is absent (gap) or behind us */
				if (++i >= lim)
					break;
				v = docidarr[i];
			}
		}
	}

	while (rlo <= rhi)
	{
		int			mid = (rlo + rhi) >> 1;

		if (docidarr[mid] < docid)
			rlo = mid + 1;
		else if (docidarr[mid] > docid)
			rhi = mid - 1;
		else
		{
			*hint = mid + 1;
			return bytearr[mid];
		}
	}
	return -1;
}

#endif							/* WEAVE_FOR_H */
