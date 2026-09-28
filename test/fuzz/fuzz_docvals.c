/*
 * fuzz_docvals.c -- corruption/fuzz harness for the int8 docvalues store
 * validator, weave_docvals_validate() in include/weave/docvals.h.
 *
 * A docvalues store comes off disk (weave_docvals_load() reassembles a page
 * chain into one contiguous image) and every field in its header is a length or
 * an offset that later code uses to index the value array: ndocs bounds the
 * per-docid reads weave_dv_eval_int8() makes, and values_off is where those
 * reads start.  On-disk bytes are not trusted (doc/CONVENTIONS.md decision 2),
 * and a store the validator wrongly ACCEPTS is not a crash but a WRONG GATE SET
 * -- a silently dropped or spurious row.  So the property below is not optional.
 *
 * A v2 store additionally carries an OPTIONAL null bitmap: null_off == 0 means
 * "no nulls" (the v1 shape, and every NOT-NULL column), and a nonzero null_off
 * points at a ceil(ndocs/8)-byte bitmap at the single MAXALIGNed post-docids
 * offset, one bit per dense docid (set == NULL, excluded from every gate).  That
 * bitmap is one more length/offset off untrusted disk, so it is fuzzed here too:
 * a validator that accepts a too-short bitmap is the same WRONG-GATE-SET hazard.
 *
 * WHAT IS FUZZED IS THE REAL VALIDATOR.  weave_docvals_validate() lives in
 * include/weave/docvals.h with no PostgreSQL dependency for exactly this reason
 * (docvalid.h / chandesc.h are the exemplars), so this is the same function
 * src/pages/docvals_page.c's weave_docvals_load() calls -- no transcription and
 * no modeling gap.
 *
 * PROPERTY (default build, under ASan+UBSan): for ANY byte string of ANY length,
 * weave_docvals_validate() reads only inside [img, img+len), returns NULL or a
 * static reason, and when it returns NULL the header's ndocs values provably fit
 * within len -- which this harness proves independently by READING all ndocs
 * values out of an image sized EXACTLY to len.  For a v2 store that acceptance
 * also implies the null bitmap's ceil(ndocs/8) bytes fit within len, which the
 * harness proves by READING every bit through weave_docvals_isnull() over the
 * whole dense space -- so a wrongly accepted too-short bitmap becomes an overread
 * too.  ASan's redzone sits immediately past the last readable byte, so a
 * validator that accepted a too-short image turns into a hard overread rather
 * than a read of adjacent bytes.
 *
 * TEETH (PLANT_BUG=1): builds a deliberately weakened copy of the validator,
 * weak_validate() below, with the length guards removed -- both the
 * "len >= values_off + ndocs*8" array check AND the v2 "null_off + ceil(ndocs/8)
 * <= len" bitmap-fits check, the two guards a careless refactor omits because the
 * magic/version/offset checks look like they already bound the image.  A
 * truncated image with a large ndocs, or one truncated between its docid array
 * and the end of its bitmap, is then accepted; the read-all-values and
 * read-all-bits postconditions walk past the exact buffer and ASan aborts,
 * proving the harness detects the bug class instead of passing vacuously.  See
 * run.sh.
 *
 * No hegel/cmocka: deterministic PRNG loop, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEAVE_DOCVALS_TEST_HELPERS	/* weave_docvals_build / _store_len */
#include "weave/docvals.h"

static uint64_t rngstate = 0x9E3779B97F4A7C15ULL;

static uint32_t
rnd(void)
{
	/* splitmix64, so the corpus is identical on every host and every rerun */
	uint64_t	z;

	rngstate += 0x9E3779B97F4A7C15ULL;
	z = rngstate;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return (uint32_t) ((z ^ (z >> 31)) >> 16);
}

/* Fill docids[0..n) strictly ascending and sparse, the weave_tid_to_docid()
 * shape, so a well-formed image passes the validator's strict-ascent check. */
static void
fill_docids(uint64_t *docids, uint32_t n)
{
	uint32_t	i;
	uint64_t	d = rnd() % 4u;

	for (i = 0; i < n; i++)
	{
		docids[i] = d;
		d += 1u + (rnd() % 8u);
	}
}

/*
 * Fill nullbits[0..n) (one byte per dense docid, nonzero == NULL) in one of a few
 * shapes so the v2 passes exercise a mix, an all-null bitmap, and a present but
 * all-zero bitmap.  mode selects the shape; a random mix is the common case.
 */
static void
fill_nullbits(uint8_t *nullbits, uint32_t n, int mode)
{
	uint32_t	i;

	switch (mode)
	{
		case 0:					/* every docid NULL (whole bitmap set) */
			for (i = 0; i < n; i++)
				nullbits[i] = 1;
			break;
		case 1:					/* no docid NULL (bitmap present, all zero) */
			for (i = 0; i < n; i++)
				nullbits[i] = 0;
			break;
		default:				/* a random mix, incl. the high trailing bits */
			for (i = 0; i < n; i++)
				nullbits[i] = (uint8_t) (rnd() & 1u);
			break;
	}
}

#ifdef PLANT_BUG
/*
 * The planted bug: weave_docvals_validate() with the image-length guards deleted
 * (and, since it can no longer be reached safely, the strict-ascent loop they
 * feed).  Everything else -- magic, version 1 OR 2, typid, the offset identities,
 * and the v2 null_off == aligned-post-docids rule -- is identical to the real
 * validator, so acceptance still lands on well-formed-looking headers; only the
 * two size checks are gone:
 *
 *   #1  len >= docids_off + ndocs*8       (values + docids fit)
 *   #2  len >= null_off + ceil(ndocs/8)   (v2 null bitmap fits)  <-- this task
 *
 * #2 is exactly the guard a refactor that adds the bitmap forgets, because the
 * null_off == MAXALIGN(...) identity check just above LOOKS like it bounds the
 * region.  A too-short image with a large ndocs, or a v2 image truncated between
 * its docid array and the end of its bitmap, is then accepted, and the
 * read-all-values / read-all-bits postcondition in verify_accepted() MUST
 * overrun the exact buffer.
 */
static const char *
weak_validate(const void *img, size_t len)
{
	WeaveDocvalsHeader h;
	uint32_t	voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint64_t	total_need;

	if (len < sizeof(WeaveDocvalsHeader))
		return "image shorter than header";
	memcpy(&h, img, sizeof(h));
	if (h.magic != WEAVE_DOCVALS_MAGIC)
		return "bad magic";
	if (h.version != 1 && h.version != 2)
		return "unsupported version";
	if (h.typid_kind != 1)
		return "unsupported value kind";
	if (h.values_off != voff)
		return "values_off mismatch";
	if (h.zonemap_off != 0)
		return "zonemap_off must be 0";
	if ((uint64_t) h.docids_off != (uint64_t) voff + (uint64_t) h.ndocs * 8u)
		return "docids_off mismatch";
	total_need = (uint64_t) h.docids_off + (uint64_t) h.ndocs * 8u;
	/* MISSING GUARD #1: if (len < total_need) return "too short"; */
	if (h.null_off != 0)
	{
		if (h.version != 2)
			return "null_off set but version is not 2";
		if ((uint64_t) h.null_off != WEAVE_DV_MAXALIGN(total_need))
			return "null_off is not the aligned post-docids offset";
		/* MISSING GUARD #2 (the one this task drops so the bitmap teeth fire):
		 * bitmap_end = null_off + ceil(ndocs/8);
		 * if (len < bitmap_end) return "image too short for the null bitmap"; */
	}
	/* strict-ascent loop also dropped: unreachable safely without guard #1 */
	return NULL;
}
#define VALIDATE(img, len)	weak_validate((img), (len))
#else
#define VALIDATE(img, len)	weave_docvals_validate((img), (len))
#endif

/*
 * Independently re-verify what acceptance CLAIMS, WITHOUT reusing the validator:
 * every header field is in its accepted domain, the image is long enough for its
 * stated ndocs, and -- the load-bearing part -- reading all ndocs values stays
 * inside the EXACT buffer.  A validator that returns NULL on an image it should
 * have rejected is a wrong answer the sanitizer cannot see on its own; this read
 * is what turns that into an overread ASan can.
 */
static void
verify_accepted(const unsigned char *img, size_t len)
{
	WeaveDocvalsHeader h;
	uint64_t	need;
	uint32_t	d;
	int64_t		acc = 0;
	uint64_t	dacc = 0;
	int			nacc = 0;

	assert(len >= sizeof(WeaveDocvalsHeader));
	memcpy(&h, img, sizeof(h));
	assert(h.magic == WEAVE_DOCVALS_MAGIC);
	assert(h.version == 1 || h.version == 2);
	assert(h.typid_kind == 1);
	assert(h.values_off == (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader)));
	assert(h.zonemap_off == 0);
	assert((uint64_t) h.docids_off ==
		   (uint64_t) h.values_off + (uint64_t) h.ndocs * 8u);

	need = (uint64_t) h.docids_off + (uint64_t) h.ndocs * 8u;
	assert((uint64_t) len >= need);

	/* Read every value AND every docid: in the accepted case this is in-bounds by
	 * construction; under PLANT_BUG a too-short accepted image overruns here (the
	 * docid array sits last of the two, so its read is the sharpest overrun) and
	 * ASan aborts, proving the harness detects the dropped guard instead of
	 * passing vacuously. */
	for (d = 0; d < h.ndocs; d++)
		acc ^= weave_docvals_int8(img, d);
	for (d = 0; d < h.ndocs; d++)
		dacc ^= weave_docvals_docid(img, d);

	/* The v2 null bitmap.  A v1 store, or a v2 store with no NULLs, carries no
	 * bitmap (null_off == 0) and behaves exactly as before -- nothing past the
	 * docid array is read.  When a bitmap IS present it must be version 2 and its
	 * ceil(ndocs/8) bytes must sit at the single MAXALIGNed post-docids offset
	 * inside len; READING every bit (weave_docvals_isnull touches byte idx>>3, so
	 * the full [null_off, null_off+ceil(ndocs/8)) range is swept) is what turns a
	 * validator that accepted a too-short bitmap -- PLANT_BUG's dropped guard #2 --
	 * into an overread ASan catches, the same discipline the array reads above
	 * apply.  The bitmap sits last of all regions, so this is the sharpest overrun
	 * of a v2 image truncated below its full length. */
	if (h.null_off != 0)
	{
		uint64_t	bitmap_end;

		assert(h.version == 2);
		assert((uint64_t) h.null_off == WEAVE_DV_MAXALIGN(need));
		bitmap_end = (uint64_t) h.null_off + ((uint64_t) h.ndocs + 7u) / 8u;
		assert((uint64_t) len >= bitmap_end);
		for (d = 0; d < h.ndocs; d++)
			nacc ^= weave_docvals_isnull(img, d);
	}
	(void) acc;
	(void) dacc;
	(void) nacc;
}

#define MAXN	512

int
main(void)
{
	unsigned long iters = 0;
	unsigned long accepted = 0;
	unsigned long rejected = 0;
	int			trial;

	/* 1. every well-formed image of a range of ndocs must be accepted, and its
	 * values must read back exactly what was written */
	for (trial = 0; trial <= MAXN; trial++)
	{
		uint32_t	n = (uint32_t) trial;
		size_t		len = weave_docvals_store_len(n);
		unsigned char *exact = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
		uint32_t	i;
		const char *why;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) (((uint64_t) rnd() << 32) | rnd());
		fill_docids(docids, n);
		weave_docvals_build(exact, vals, docids, n);

		why = VALIDATE(exact, len);
		if (why != NULL)
		{
			fprintf(stderr, "well-formed image of %u value(s) rejected: %s\n",
					n, why);
			free(exact);
			free(vals);
			free(docids);
			return 1;
		}
		verify_accepted(exact, len);
		for (i = 0; i < n; i++)
			assert(weave_docvals_int8(exact, i) == vals[i]);
		for (i = 0; i < n; i++)
			assert(weave_docvals_docid(exact, i) == docids[i]);
		free(exact);
		free(vals);
		free(docids);
		iters++;
	}

	/* 1b. every well-formed v2 store must be accepted, its values/docids read
	 * back exactly, and its null bits match what was written.  Cycle the bitmap
	 * shape: a NOT-NULL v2 store (no bitmap, null_off == 0), an all-null bitmap,
	 * an all-zero-but-present bitmap, and a random mix -- so both the "no bitmap"
	 * and "bitmap present" acceptance branches, and every trailing-bit pattern,
	 * are covered. */
	for (trial = 0; trial <= MAXN; trial++)
	{
		uint32_t	n = (uint32_t) trial;
		int			shape = trial % 4;		/* 0=no bitmap, 1..3 = a bitmap */
		int			has_nulls = (shape != 0);
		size_t		len = weave_docvals_store_len_nulls(n, has_nulls);
		unsigned char *exact = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
		uint8_t	   *nullbits = (uint8_t *) malloc(n ? n : 1);
		uint32_t	i;
		const char *why;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) (((uint64_t) rnd() << 32) | rnd());
		fill_docids(docids, n);
		fill_nullbits(nullbits, n, shape - 1);
		weave_docvals_build_nulls(exact, vals, docids,
								  has_nulls ? nullbits : NULL, n);

		why = VALIDATE(exact, len);
		if (why != NULL)
		{
			fprintf(stderr, "well-formed v2 image of %u value(s) rejected: %s\n",
					n, why);
			free(exact);
			free(vals);
			free(docids);
			free(nullbits);
			return 1;
		}
		verify_accepted(exact, len);
		for (i = 0; i < n; i++)
			assert(weave_docvals_int8(exact, i) == vals[i]);
		for (i = 0; i < n; i++)
			assert(weave_docvals_docid(exact, i) == docids[i]);
		/* A store with no bitmap reports every docid non-null; a store with a
		 * bitmap must report exactly the bits written. */
		for (i = 0; i < n; i++)
			assert(weave_docvals_isnull(exact, i) ==
				   (has_nulls ? (nullbits[i] ? 1 : 0) : 0));
		free(exact);
		free(vals);
		free(docids);
		free(nullbits);
		iters++;
	}
	for (trial = 0; trial <= 64; trial++)
	{
		uint32_t	n = (uint32_t) trial;
		size_t		len = weave_docvals_store_len(n);
		unsigned char *full = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
		uint32_t	i;
		size_t		cut;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) rnd();
		fill_docids(docids, n);
		weave_docvals_build(full, vals, docids, n);

		for (cut = 0; cut <= len; cut++)
		{
			unsigned char *exact = (unsigned char *) malloc(cut ? cut : 1);
			const char *why;

			memcpy(exact, full, cut);
			why = VALIDATE(exact, cut);
			if (why == NULL)
			{
				verify_accepted(exact, cut);
				accepted++;
			}
			else
				rejected++;
			free(exact);
			iters++;
		}
		free(full);
		free(vals);
		free(docids);
	}

	/* 2b. a well-formed v2 store WITH a bitmap, truncated to every length, must
	 * be rejected or accepted consistently and never overread.  This is the
	 * bitmap teeth: a cut in [docids_off+ndocs*8, null_off+ceil(ndocs/8)) leaves
	 * the values and docids intact but the bitmap short, so under PLANT_BUG (which
	 * drops the bitmap-fits-len guard) the image is accepted and verify_accepted's
	 * read-all-bits postcondition overruns -- while the real validator rejects it
	 * with "image too short for the null bitmap". */
	for (trial = 0; trial <= 64; trial++)
	{
		uint32_t	n = (uint32_t) trial;
		size_t		len = weave_docvals_store_len_nulls(n, 1);
		unsigned char *full = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
		uint8_t	   *nullbits = (uint8_t *) malloc(n ? n : 1);
		uint32_t	i;
		size_t		cut;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) rnd();
		fill_docids(docids, n);
		fill_nullbits(nullbits, n, 2);		/* a random mix */
		weave_docvals_build_nulls(full, vals, docids, nullbits, n);

		for (cut = 0; cut <= len; cut++)
		{
			unsigned char *exact = (unsigned char *) malloc(cut ? cut : 1);
			const char *why;

			memcpy(exact, full, cut);
			why = VALIDATE(exact, cut);
			if (why == NULL)
			{
				verify_accepted(exact, cut);
				accepted++;
			}
			else
				rejected++;
			free(exact);
			iters++;
		}
		free(full);
		free(vals);
		free(docids);
		free(nullbits);
	}

	/* 3. random single- and multi-byte corruption of a well-formed image, plus a
	 * declared length torn independently of the buffer we hand over */
	for (trial = 0; trial < 200000; trial++)
	{
		uint32_t	n = rnd() % (MAXN + 1);
		size_t		len = weave_docvals_store_len(n);
		unsigned char *scratch = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
		int			nsmash = 1 + (int) (rnd() % 8);
		unsigned char *exact;
		size_t		avail;
		uint32_t	i;
		const char *why;
		int			k;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) rnd();
		fill_docids(docids, n);
		weave_docvals_build(scratch, vals, docids, n);
		for (k = 0; k < nsmash; k++)
			scratch[rnd() % len] = (unsigned char) rnd();

		/* a torn pd_lower is the realistic case: give a buffer shorter or longer
		 * than the store thinks it is */
		avail = (rnd() % 3 == 0) ? (rnd() % (len + 1)) : len;
		exact = (unsigned char *) malloc(avail ? avail : 1);
		memcpy(exact, scratch, avail);
		why = VALIDATE(exact, avail);
		if (why == NULL)
		{
			verify_accepted(exact, avail);
			accepted++;
		}
		else
			rejected++;
		free(exact);
		free(scratch);
		free(vals);
		free(docids);
		iters++;
	}

	/* 4. fully random bytes at random lengths, including below the header size
	 * and above a plausible small store; half the time plant a correct header
	 * prefix so the deeper checks are actually reached */
	for (trial = 0; trial < 200000; trial++)
	{
		size_t		avail = rnd() % (weave_docvals_store_len(64) + 8);
		unsigned char *exact = (unsigned char *) malloc(avail ? avail : 1);
		size_t		i;
		const char *why;

		for (i = 0; i < avail; i++)
			exact[i] = (unsigned char) rnd();
		if (avail >= sizeof(WeaveDocvalsHeader) && (rnd() & 1))
		{
			WeaveDocvalsHeader h;

			h.magic = WEAVE_DOCVALS_MAGIC;
			h.version = 1;
			h.typid_kind = 1;
			h.ndocs = rnd() % (MAXN + 1);
			h.null_off = 0;
			h.zonemap_off = 0;
			h.values_off = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
			h.docids_off = h.values_off + h.ndocs * 8u;
			h.dict_off = 0;
			memcpy(exact, &h, sizeof(h));
		}
		why = VALIDATE(exact, avail);
		if (why == NULL)
		{
			verify_accepted(exact, avail);
			accepted++;
		}
		else
			rejected++;
		free(exact);
		iters++;
	}

	/* 5. header-field mutations that target the v2 null_off, built on a valid
	 * v2-with-bitmap base so only the field under test is wrong.  Each malformed
	 * variant must be REJECTED by the real validator; the two honest variants
	 * (version 2 with no bitmap, and the untouched base) must be ACCEPTED.  The
	 * one-byte-short variant is the bitmap teeth under PLANT_BUG. */
	for (trial = 1; trial <= 64; trial++)
	{
		uint32_t	n = (uint32_t) trial;
		size_t		len = weave_docvals_store_len_nulls(n, 1);
		size_t		len0 = weave_docvals_store_len_nulls(n, 0);
		unsigned char *base = (unsigned char *) malloc(len);
		unsigned char *nobmp = (unsigned char *) malloc(len0);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
		uint8_t	   *nullbits = (uint8_t *) malloc(n ? n : 1);
		uint32_t	valid_null_off;
		WeaveDocvalsHeader h;
		unsigned char *m;
		const char *why;
		uint32_t	i;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) rnd();
		fill_docids(docids, n);
		fill_nullbits(nullbits, n, 2);		/* a random mix */
		weave_docvals_build_nulls(base, vals, docids, nullbits, n);
		memcpy(&h, base, sizeof(h));
		valid_null_off = h.null_off;
		assert(valid_null_off != 0);

		/* the untouched base is well-formed: it must validate. */
		why = VALIDATE(base, len);
		assert(why == NULL);
		verify_accepted(base, len);
		accepted++;
		iters++;

		/* an honest v2 store with NO bitmap (version 2, null_off == 0) must be
		 * ACCEPTED -- the "v2 but not-null column" shape. */
		weave_docvals_build_nulls(nobmp, vals, docids, NULL, n);
		why = VALIDATE(nobmp, len0);
		assert(why == NULL);
		verify_accepted(nobmp, len0);
		accepted++;
		iters++;

		/* (a) null_off past the image end. */
		m = (unsigned char *) malloc(len);
		memcpy(m, base, len);
		memcpy(&h, m, sizeof(h));
		h.null_off = (uint32) (len + 8u);
		memcpy(m, &h, sizeof(h));
		why = VALIDATE(m, len);
		assert(why != NULL);
		rejected++;
		free(m);
		iters++;

		/* (b) null_off misaligned one byte past the single legal offset. */
		m = (unsigned char *) malloc(len);
		memcpy(m, base, len);
		memcpy(&h, m, sizeof(h));
		h.null_off = valid_null_off + 1u;
		memcpy(m, &h, sizeof(h));
		why = VALIDATE(m, len);
		assert(why != NULL);
		rejected++;
		free(m);
		iters++;

		/* (b') null_off pointing back inside the values array (a wrong offset
		 * that is well within len, so only the recompute-and-refuse rule stops
		 * the reader being walked into the wrong region). */
		m = (unsigned char *) malloc(len);
		memcpy(m, base, len);
		memcpy(&h, m, sizeof(h));
		h.null_off = h.values_off;
		memcpy(m, &h, sizeof(h));
		why = VALIDATE(m, len);
		assert(why != NULL);
		rejected++;
		free(m);
		iters++;

		/* (c) null_off nonzero but version == 1 (a v1 store may carry no bitmap). */
		m = (unsigned char *) malloc(len);
		memcpy(m, base, len);
		memcpy(&h, m, sizeof(h));
		h.version = 1;
		memcpy(m, &h, sizeof(h));
		why = VALIDATE(m, len);
		assert(why != NULL);
		rejected++;
		free(m);
		iters++;

		/* version == 3 is an unknown format and must be rejected outright. */
		m = (unsigned char *) malloc(len);
		memcpy(m, base, len);
		memcpy(&h, m, sizeof(h));
		h.version = 3;
		memcpy(m, &h, sizeof(h));
		why = VALIDATE(m, len);
		assert(why != NULL);
		rejected++;
		free(m);
		iters++;

		/* (d) the bitmap runs one byte past len: a valid header handed a buffer
		 * one byte too short.  The real validator refuses it ("image too short
		 * for the null bitmap"); under PLANT_BUG the dropped bitmap-fits-len guard
		 * accepts it and verify_accepted's read-all-bits postcondition overruns
		 * the (len-1)-sized buffer -- the bitmap teeth. */
		{
			size_t		short_len = len - 1u;
			unsigned char *t = (unsigned char *) malloc(short_len ? short_len : 1);

			memcpy(t, base, short_len);
			why = VALIDATE(t, short_len);
			if (why == NULL)
			{
				verify_accepted(t, short_len);
				accepted++;
			}
			else
				rejected++;
#ifndef PLANT_BUG
			assert(why != NULL);
#endif
			free(t);
			iters++;
		}

		free(base);
		free(nobmp);
		free(vals);
		free(docids);
		free(nullbits);
	}

	printf("fuzz_docvals: %lu cases, %lu accepted, %lu rejected -- no overread, no UB\n",
		   iters, accepted, rejected);
	return 0;
}
