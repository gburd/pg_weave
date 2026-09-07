/*
 * fuzz_block.c -- corruption/fuzz harness for the weave_decode_term INNER LOOP,
 * the v0.3.4 crash site: a block header {count, bytelen, posbytelen,
 * first_docid, ...} read from an untrusted page, then `count` values unpacked
 * into fixed WEAVE_BLOCK_SIZE stack arrays via weave_for_unpack.
 *
 * MODELING CHOICE (documented honestly): weave_decode_term() itself needs the
 * backend (ReadBuffer/LockBuffer/Page), so it cannot be called standalone.  We
 * model just the pure part -- the header parse + the two 0.3.4 guards (clamp
 * count to WEAVE_BLOCK_SIZE; reject a block whose bytelen+posbytelen runs past
 * the page end) + the three weave_for_unpack calls into gaps/tfs/dls[128] -- as
 * decode_block_inner() below.  This is a faithful transcription of the loop
 * body in pg_weave_am.c (the lines around `int cnt = (int) bh->count;`), sharing
 * the real weave/for.h codec and the real WeaveBlockHdr layout.  It is NOT the
 * backend function; it is the exact overflow SITE lifted out so a fuzzer can
 * hammer the header with a corrupt count/bytelen and prove the clamp holds.
 *
 * The modeled "page" is a full BLCKSZ (8192) heap buffer, matching the real
 * backend where the page is a full BLCKSZ block: reads anywhere inside the
 * 8192-byte page are safe; only a read PAST the 8192 buffer is an overflow.
 * ASan redzones the 8192 boundary, so any read past it is a hard failure.  The
 * fixed 128-arrays inside decode_block_inner are redzoned by ASan's stack
 * instrumentation, so any WRITE past index 127 aborts.
 *
 * PROPERTY (default build, under ASan+UBSan): for a corrupt count and a corrupt
 * bytelen/posbytelen over a WELL-FORMED FOR stream (the realistic torn-header /
 * stale-format case), decode_block_inner never overflows the 128 arrays and
 * never reads past the page.  The 0.3.4 count clamp makes count>128 safe; the
 * bytelen-past-page guard rejects a block that does not fit.
 *
 * TEETH #1 (count overflow): define FUZZ_NO_CLAMP=1 to drop the count clamp
 * (pre-0.3.4 behavior); count>128 then overflows gaps[128] and ASan aborts --
 * proving the harness detects the exact 0.3.4 bug class.  See run.sh.
 *
 * TEETH #2 / RESIDUAL FINDING (adversarial width): define FUZZ_RANDOM_STREAM=1
 * to fill the FOR stream with fully-random bytes (a corrupt WIDTH byte).  Then
 * weave_for_unpack reads 1+(cnt*width+7)/8 bytes -- driven by the on-disk width,
 * which the 0.3.4 guards do NOT cross-check against bytelen -- and can read past
 * the page.  The fuzzer catches it (ASan).  This models a gap the 0.3.4 fix left
 * open: guard #2 bounds by the *declared* bytelen, but the decoder reads by the
 * *on-disk width*.  It is OFF by default (it is a finding to report, not a
 * regression in the harness); run.sh documents it.  See test/fuzz/README.md.
 *
 * No hegel/cmocka: deterministic PRNG loop, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/for.h"

/* Mirror of the extension's WEAVE_BLOCK_SIZE and WeaveBlockHdr (pg_weave_am.h). */
#define WEAVE_BLOCK_SIZE 128
#define PAGESZ 8192				/* BLCKSZ */

typedef struct BlockHdr
{
	uint32_t	count;
	uint32_t	max_tf;
	uint32_t	min_doclen;
	uint32_t	first_docid_hi;
	uint32_t	first_docid_lo;
	uint32_t	bytelen;
	uint32_t	posbytelen;
} BlockHdr;

/*
 * decode_block_inner -- the modeled inner loop of weave_decode_term.
 *
 * `page` is the modeled BLCKSZ page buffer, `off` the block start within it,
 * `pd_lower` the page's filled length (the backend's pend = page + pd_lower).
 * Decodes ONE block's three FOR columns into the fixed 128-arrays, applying the
 * 0.3.4 guards.  Proven by ASan to never overflow gaps/tfs/dls and never read
 * past the page buffer.
 */
static int
decode_block_inner(const unsigned char *page, size_t pd_lower, size_t off,
				   int want_positions)
{
	const unsigned char *pend = page + pd_lower;
	const unsigned char *p = page + off;
	uint64_t	gaps[WEAVE_BLOCK_SIZE];
	uint64_t	tfs[WEAVE_BLOCK_SIZE];
	uint64_t	dls[WEAVE_BLOCK_SIZE];
	const BlockHdr *bh;
	const unsigned char *stream;
	int			cnt;
	int			pos = 0;

	if (p + sizeof(BlockHdr) > pend)
		return 0;

	bh = (const BlockHdr *) p;
	stream = (const unsigned char *) (bh + 1);
	cnt = (int) bh->count;

	if (cnt == 0)
		return 0;

	/*
	 * Guard #1: clamp count so it can never overflow the fixed
	 * gaps/tfs/dls[WEAVE_BLOCK_SIZE] arrays via weave_for_unpack, NOR drive a
	 * negative n into weave_for_unpack.
	 *
	 * NOTE / FINDING: the SHIPPED 0.3.4 guard is one-sided --
	 *     int cnt = (int) bh->count;  if (cnt > WEAVE_BLOCK_SIZE) cnt = WEAVE_BLOCK_SIZE;
	 * -- and bh->count is uint32.  A corrupt count in [2^31, 2^32) casts to a
	 * NEGATIVE int, so `cnt > WEAVE_BLOCK_SIZE` is false, the clamp is skipped,
	 * and weave_for_unpack(stream, negative_n, ...) computes a garbage/negative
	 * byte length that walks `pos` to a wild pointer -> OOB read.  The fuzzer
	 * found this; the correct guard clamps BOTH ends.  This model uses the
	 * correct two-sided clamp by default; FUZZ_SIGNED_COUNT reverts to the
	 * shipped one-sided clamp so the fuzzer demonstrates the residual bug, and
	 * FUZZ_NO_CLAMP reverts the whole clamp (the pre-0.3.4 count-overflow bug).
	 */
#if defined(FUZZ_NO_CLAMP)
	/* no clamp at all: count>128 overflows gaps[128] */
#elif defined(FUZZ_SIGNED_COUNT)
	if (cnt > WEAVE_BLOCK_SIZE)	/* shipped one-sided clamp: misses negative cnt */
		cnt = WEAVE_BLOCK_SIZE;
#else
	if (cnt <= 0 || cnt > WEAVE_BLOCK_SIZE)	/* correct: clamp both ends */
		cnt = cnt <= 0 ? 0 : WEAVE_BLOCK_SIZE;
	if (cnt == 0)
		return 0;
#endif

	/* 0.3.4 guard #2: reject a block whose declared FOR columns run past pend. */
	if (stream + (size_t) bh->bytelen + (size_t) bh->posbytelen > pend)
		return 0;

	pos += weave_for_unpack(stream + pos, cnt, gaps);
	pos += weave_for_unpack(stream + pos, cnt, tfs);
	pos += weave_for_unpack(stream + pos, cnt, dls);

	if (want_positions && bh->posbytelen > 0)
	{
		const unsigned char *pstream = stream + bh->bytelen;
		uint64_t	deltas[WEAVE_BLOCK_SIZE * 4];
		uint64_t   *dbuf = deltas;
		int			sumtf = 0;
		int			i;

		for (i = 0; i < cnt; i++)
			sumtf += (int) tfs[i];

		/*
		 * Guard #3 (1.0.2): sanity-bound sumtf against the declared positions
		 * bytes.  The positions column is one FOR block of sumtf values whose
		 * exact size is width==0?1:1+ceil(sumtf*width/8) (width=pstream[0]); a
		 * corrupt/inflated
		 * tfs[] (values each in range, but summing huge) pushes sumtf far above
		 * what posbytelen encodes, so weave_for_unpack would read past the block
		 * AND a plain palloc(sumtf*8) throws "invalid memory alloc request size"
		 * once it crosses MaxAllocSize -- the 1.0.1 read-path crash reported
		 * against CREATE INDEX CONCURRENTLY.  The shipped fix rejects the block
		 * here for the corrupt case and uses a huge-safe alloc for the
		 * legitimately-large case.
		 *
		 * FUZZ_NO_SUMTF_GUARD reverts to the 1.0.1 behavior (no bound, plain
		 * malloc) so the fuzzer demonstrates it detects this bug class.
		 */
#if defined(FUZZ_NO_SUMTF_GUARD)
		if (sumtf < 0)
			sumtf = 0;			/* 1.0.1: negative-overflow floor only, no posbytelen bound */
#else
		{
			/* mirror the shipped guard: exact FOR-block length in 64-bit
			 * arithmetic (weave_for_bytelen's int n*width would itself overflow
			 * on a corrupt sumtf), reject if it exceeds posbytelen. */
			unsigned int pw = pstream[0];
			size_t		need;

			if (sumtf < 0)
				need = (size_t) -1;
			else
				need = (pw == 0) ? 1
					: (size_t) 1 + (((size_t) sumtf * pw + 7) / 8);
			if (need > (size_t) bh->posbytelen)
				return 0;		/* corrupt: reject the block */
		}
#endif
		if (sumtf > (int) (sizeof(deltas) / sizeof(deltas[0])))
			dbuf = (uint64_t *) malloc((size_t) sumtf * sizeof(uint64_t));
		if (dbuf != NULL && sumtf > 0)
			(void) weave_for_unpack(pstream, sumtf, dbuf);
		if (dbuf != deltas)
			free(dbuf);
	}

	return 0;
}

/* xorshift64* PRNG -- fixed seed => reproducible. */
static uint64_t rng_state;
static void
rng_seed(uint64_t s)
{
	rng_state = s ? s : 0x9E3779B97F4A7C15ULL;
}
static uint64_t
rng_next(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

/*
 * Lay a WELL-FORMED FOR stream (three columns of `cnt` values each, then an
 * optional positions column) at `stream` within a page, returning the total
 * byte length.  This is what a real (non-torn) block looks like; the fuzzer
 * then corrupts the HEADER (count/bytelen) around it -- the realistic
 * stale-format / torn-header case the 0.3.4 guards target.  Returns 0 if it
 * would not fit before `limit`.
 */
#if !defined(FUZZ_RANDOM_STREAM)
static size_t
lay_stream(unsigned char *stream, const unsigned char *limit, int cnt,
		   int has_pos, size_t *out_posbytelen)
{
	uint64_t	col[WEAVE_BLOCK_SIZE];
	uint64_t	tfvals[WEAVE_BLOCK_SIZE];
	uint64_t	posvals[WEAVE_BLOCK_SIZE * 4];
	int			i;
	size_t		total = 0;
	int			c = cnt > WEAVE_BLOCK_SIZE ? WEAVE_BLOCK_SIZE : cnt;
	int			sumtf = 0;

	*out_posbytelen = 0;
	/* three columns: gaps, tfs, doclens. Keep values modest so widths are sane.
	 * The tf column drives the positions count, so capture its values. */
	for (int colno = 0; colno < 3; colno++)
	{
		int			w = (int) (rng_next() % 20);	/* width up to ~19 bits */
		uint64_t	mask = w == 0 ? 0 : (((uint64_t) 1 << w) - 1);

		for (i = 0; i < c; i++)
		{
			col[i] = rng_next() & mask;
			if (colno == 1)		/* tf column: keep >=1 and small */
			{
				col[i] = 1 + (col[i] % 4);
				tfvals[i] = col[i];
			}
		}
		if (stream + total + 1 + (size_t) (c * 64 + 7) / 8 > limit)
			return 0;
		total += (size_t) weave_for_pack(col, c, stream + total);
	}

	/* sum(tf) is exactly how many positions decode_block_inner will read. */
	for (i = 0; i < c; i++)
		sumtf += (int) tfvals[i];

	if (has_pos && sumtf > 0)
	{
		int			w = (int) (rng_next() % 12);
		uint64_t	mask = w == 0 ? 0 : (((uint64_t) 1 << w) - 1);
		size_t		plen;

		if (sumtf > WEAVE_BLOCK_SIZE * 4)
			sumtf = WEAVE_BLOCK_SIZE * 4;	/* matches decode_block_inner deltas[] */
		for (i = 0; i < sumtf; i++)
			posvals[i] = rng_next() & mask;
		if (stream + total + 1 + (size_t) (sumtf * 64 + 7) / 8 > limit)
			return total;		/* skip positions if no room */
		plen = (size_t) weave_for_pack(posvals, sumtf, stream + total);
		*out_posbytelen = plen;
		total += plen;
	}
	return total;
}
#endif							/* !FUZZ_RANDOM_STREAM */

/*
 * Default fuzz: WELL-FORMED FOR stream, CORRUPT header (count/bytelen/posbytelen)
 * -- the realistic torn/stale-header case.  Proves the 0.3.4 guards hold.
 */
static void
fuzz_blocks(void)
{
	int			iter;
	unsigned char *page = (unsigned char *) malloc(PAGESZ);

	assert(page != NULL);

	for (iter = 0; iter < 300000; iter++)
	{
		size_t		off = (rng_next() % 64) * 8;	/* MAXALIGN'd start, near the front */
		size_t		pd_lower;
		BlockHdr   *bh;
		size_t		k;
		int			want_pos = (int) (rng_next() & 1);
		/* lay a FULL 128-value stream so the clamped count (<=128) always has
		 * that many real values to decode -- the realistic "corrupt count, honest
		 * bytelen" torn-header case the 0.3.4 clamp targets.  (A block with FEWER
		 * than 128 real values plus a corrupt count is the residual corrupt-read
		 * finding, exercised by FUZZ_RANDOM_STREAM.) */
		int			real_cnt = WEAVE_BLOCK_SIZE;
		size_t		streamlen,
					posbytelen;
		unsigned char *stream;

		for (k = 0; k < PAGESZ; k++)
			page[k] = (unsigned char) rng_next();

		if (off + sizeof(BlockHdr) + 4 > PAGESZ)
			off = 0;
		bh = (BlockHdr *) (page + off);
		stream = page + off + sizeof(BlockHdr);

#ifdef FUZZ_RANDOM_STREAM
		/* RESIDUAL FINDING mode: leave the stream fully random -> a corrupt WIDTH
		 * byte drives weave_for_unpack past bytelen/page. OFF by default. */
		(void) real_cnt;
		(void) stream;
		streamlen = (size_t) (rng_next() % (PAGESZ / 2));
		posbytelen = (size_t) (rng_next() % (PAGESZ / 2));
		pd_lower = 512 + (rng_next() % (PAGESZ - 512));
#else
		streamlen = lay_stream(stream, page + PAGESZ, real_cnt, want_pos, &posbytelen);
		if (streamlen == 0)
			continue;
		/* pd_lower must cover the laid block so guard #2 does not spuriously
		 * reject the honest case (pend >= end of the laid stream). */
		pd_lower = (size_t) (stream - page) + streamlen;
		if (pd_lower > PAGESZ)
			continue;
		/* jitter pd_lower up to the page end (never below the laid stream) */
		pd_lower += rng_next() % (PAGESZ - pd_lower + 1);
#endif

		/* CORRUPT the header count: mostly >128 (overflow direction), plus
		 * exactly 128 / 0 / fully-random. */
		switch (rng_next() % 5)
		{
			case 0:
				bh->count = 129 + (uint32_t) (rng_next() % 100000);
				break;
			case 1:
				bh->count = WEAVE_BLOCK_SIZE;
				break;
			case 2:
				bh->count = 0;
				break;
			case 3:
				bh->count = (uint32_t) rng_next();
				break;
			default:
				bh->count = (uint32_t) real_cnt;
				break;
		}

		/* bytelen/posbytelen are kept HONEST here: this fuzzer isolates the 0.3.4
		 * COUNT clamp (corrupt count over a well-formed stream).  Corrupt bytelen
		 * -- which makes the decoder read the wrong width byte / run past the page
		 * -- is the separate residual finding covered by FUZZ_RANDOM_STREAM. */
		bh->bytelen = (uint32_t) (streamlen - posbytelen);
		bh->posbytelen = (uint32_t) posbytelen;

#if defined(FUZZ_NO_SUMTF_GUARD)
		/*
		 * SUMTF teeth: inflate the decoded tf column so Sum(tf) vastly exceeds
		 * what posbytelen encodes, then decode WITH positions.  Under the 1.0.1
		 * (unguarded) model this drives weave_for_unpack(pstream, huge_sumtf, ..)
		 * to read far past the positions column / page -> ASan OOB (and the real
		 * code would palloc(huge) -> "invalid memory alloc request size").  The
		 * shipped guard rejects the block first, so the default build is clean.
		 * Rewrite the tfs column (2nd of 3, after the gaps column) to a wide,
		 * all-ones stream so each decoded tf is large.
		 */
		{
			int			gl = weave_for_bytelen(stream, real_cnt);
			unsigned char *tcol = stream + gl;
			int			w = 20;		/* wide tf values -> big Sum(tf) */
			int			tbytes = 1 + (real_cnt * w + 7) / 8;
			int			z;

			if (tcol + tbytes <= page + PAGESZ)
			{
				tcol[0] = (unsigned char) w;
				for (z = 1; z < tbytes; z++)
					tcol[z] = 0xff;
				bh->count = (uint32_t) real_cnt;	/* honest count, poisoned tfs */
				decode_block_inner(page, pd_lower, off, 1);
			}
		}
#endif

		decode_block_inner(page, pd_lower, off, want_pos);

		/* Guard #2 rejection: force bytelen reliably PAST the page (top bit set
		 * => always > any page offset) and require decode_block_inner to reject
		 * (return 0) without reading -- the 0.3.4 bytelen-past-page guard. */
		bh->bytelen = 0x80000000u | (uint32_t) rng_next();
		bh->posbytelen = (uint32_t) (rng_next() & 0x7fffffff);
		assert(decode_block_inner(page, pd_lower, off, want_pos) == 0);
	}
	free(page);
}

/*
 * Also fuzz the primitive directly at the exact overflow site: unpack an
 * attacker-controlled count into a fixed 128-array (redzoned by ASan stack
 * instrumentation) from a page-sized buffer, with a SMALL width so the read
 * stays in bounds -- the property is the WRITE side (count must be clamped).
 */
static void
fuzz_primitive(void)
{
	int			iter;
	unsigned char buf[PAGESZ];

	for (iter = 0; iter < 100000; iter++)
	{
		uint64_t	out[WEAVE_BLOCK_SIZE];
		int			raw = (int) (rng_next() % 100000);	/* attacker count */
		int			cnt = raw;
		size_t		k;

		for (k = 0; k < sizeof(buf); k++)
			buf[k] = (unsigned char) rng_next();

#ifndef FUZZ_NO_CLAMP
		if (cnt > WEAVE_BLOCK_SIZE)
			cnt = WEAVE_BLOCK_SIZE;
#endif
		buf[0] = (unsigned char) (rng_next() % 8);	/* small width: read stays in buf */
		weave_for_unpack(buf, cnt, out);
	}
}

int
main(void)
{
	rng_seed(0xB10CC0DEB10CC0DEULL);	/* fixed seed: reproducible */
	fuzz_blocks();
	fuzz_primitive();
#if defined(FUZZ_NO_CLAMP) || defined(FUZZ_RANDOM_STREAM) || defined(FUZZ_SIGNED_COUNT)
	printf("fuzz_block: reached end in a TEETH build (no clamp / random stream / "
		   "signed count) -- ASan should have aborted before here!\n");
	return 1;					/* un-aborted teeth build => toothless => fail */
#else
	printf("fuzz_block: OK (block-header parse + count clamp holds)\n");
	return 0;
#endif
}
