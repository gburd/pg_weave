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
 * v3 (TEXT) STORES, sections 6-9.  A v3 store adds a dictionary region --
 * dict_off, ndict, offs[ndict + 1], the entry blob -- and per-doc values that
 * are ORDINALS into it.  Every one of those comes off disk.  Sections 6-9 build
 * well-formed v3 images (with/without a null bitmap; ndict 0, 1, many, and many
 * with '' as an entry), truncate them to every length, apply targeted mutations
 * to dict_off / ndict / offs[] / the ordinals, and smash them at random; every
 * accepted image is handed to verify_accepted_text(), which re-derives the
 * region bounds independently and then drives EVERY reader (ndict, dict_entry
 * for every ordinal and every byte, both boundary searches under a memcmp
 * comparator -- exact against a linear recount when the dictionary is known
 * sorted -- and weave_dv_eval_ord for all five strategies).
 *
 * TEETH (PLANT_BUG_DICT=1): removes the "every non-NULL ordinal is in
 * [0, ndict)" guard from the REAL validator (WEAVE_DV_PLANT_NO_ORD_GUARD in
 * include/weave/docvals.h), so an out-of-range ordinal is accepted; the
 * harness's own ordinal check must abort.
 *
 * No hegel/cmocka: deterministic PRNG loop, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PLANT_BUG_DICT
/* The v3 teeth: remove the "every non-NULL ordinal is in [0, ndict)" guard from
 * the REAL validator (a compile-time removal in include/weave/docvals.h, the
 * surftrie/pagebound pattern -- a weakened transcription would drift).  An
 * out-of-range ordinal is then accepted, and verify_accepted_text()'s own
 * ordinal check aborts; without that check, the reader's dict_entry() lookup
 * reads past the dictionary. */
#define WEAVE_DV_PLANT_NO_ORD_GUARD 1
#endif
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

/* ---------------------------------------------------------------------------
 * v3 (text) stores: the dictionary region.
 * ---------------------------------------------------------------------------
 */
static void verify_accepted(const unsigned char *img, size_t len);

/* memcmp order, the comparator the property tests use in place of varstr_cmp
 * (include/weave/docvals.h WeaveDvCmp). */
static int
memcmp_cmp(void *ctx, const void *a, uint32_t alen, const void *b, uint32_t blen)
{
	uint32_t	m = alen < blen ? alen : blen;
	int			r = m ? memcmp(a, b, m) : 0;

	(void) ctx;
	if (r != 0)
		return r;
	return (alen > blen) - (alen < blen);
}

/*
 * Independently re-verify an accepted v3 image, then drive EVERY reader over
 * it: ndict, dict_entry for every ordinal (touching every byte), both boundary
 * searches for keys drawn from the dictionary and between its entries, and
 * weave_dv_eval_ord for all five strategies.  Each of those indexes the image
 * with a field that came off disk; under ASan an image the validator should
 * have refused turns into an overread here.
 *
 * `sorted` says the dictionary is KNOWN strictly ascending under memcmp (a
 * well-formed image this harness built): then the binary searches must equal a
 * linear count exactly.  A mutated image the validator accepted need not be
 * sorted -- order under the collation is weave_check(deep)'s invariant, not the
 * pure validator's -- so for those only the range of lo/hi is asserted.
 */
static void
verify_accepted_text(const unsigned char *img, size_t len, int sorted)
{
	WeaveDocvalsHeader h;
	uint64_t	docids_end;
	uint64_t	region_end;
	uint64_t	offs_base;
	uint32_t	ndict;
	uint32_t	i;
	uint32_t	sum = 0;
	uint64_t   *out;
	int			op;
	int			k;

	memcpy(&h, img, sizeof(h));
	assert(h.magic == WEAVE_DOCVALS_MAGIC);
	assert(h.version == 3);
	assert(h.typid_kind == WEAVE_DV_KIND_TEXT);
	assert(h.values_off == (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader)));
	docids_end = (uint64_t) h.docids_off + (uint64_t) h.ndocs * 8u;
	assert((uint64_t) len >= docids_end);
	region_end = docids_end;
	if (h.null_off != 0)
	{
		region_end = (uint64_t) h.null_off + ((uint64_t) h.ndocs + 7u) / 8u;
		assert((uint64_t) len >= region_end);
	}
	assert((uint64_t) h.dict_off == WEAVE_DV_MAXALIGN(region_end));
	offs_base = (uint64_t) h.dict_off + 4u;
	assert((uint64_t) len >= offs_base);

	ndict = weave_docvals_ndict(img);
	assert((uint64_t) len >= offs_base + ((uint64_t) ndict + 1u) * 4u);

	/* every entry, every byte: the blob bound is what dict_entry trusts */
	for (i = 0; i < ndict; i++)
	{
		uint32_t	elen;
		const unsigned char *e = weave_docvals_dict_entry(img, i, &elen);
		uint32_t	j;

		assert(e >= img && (uint64_t) (e - img) + elen <= (uint64_t) len);
		for (j = 0; j < elen; j++)
			sum += e[j];
	}

	/* every non-NULL ordinal names an entry that exists -- the claim the
	 * PLANT_BUG_DICT build breaks; checked HERE, independently, before any
	 * reader is handed the ordinal */
	for (i = 0; i < h.ndocs; i++)
	{
		int64_t		o;
		uint32_t	elen;

		(void) weave_docvals_docid(img, i);
		if (weave_docvals_isnull(img, i))
			continue;
		o = weave_docvals_int8(img, i);
		assert(o >= 0 && (uint64_t) o < (uint64_t) ndict);
		(void) weave_docvals_dict_entry(img, (uint32_t) o, &elen);
	}

	/* boundary searches: every entry as a key, plus keys between/around them */
	for (k = 0; k < (int) ndict + 3 && k < 48; k++)
	{
		unsigned char kbuf[16];
		const unsigned char *key;
		uint32_t	klen;
		uint32_t	lo;
		uint32_t	hi;

		if (k < (int) ndict)
		{
			uint32_t	ord = (uint32_t) ((uint64_t) k * ndict / (ndict < 45 ? ndict : 45));

			if (ord >= ndict)
				ord = ndict - 1;
			key = weave_docvals_dict_entry(img, ord, &klen);
			if (klen > 0 && (k & 1) && klen <= sizeof(kbuf) - 1)
			{
				/* just past an entry: the entry's bytes with a 0x00 appended */
				memcpy(kbuf, key, klen);
				kbuf[klen] = 0;
				key = kbuf;
				klen++;
			}
		}
		else
		{
			klen = (uint32_t) (rnd() % 5u);
			for (i = 0; i < klen; i++)
				kbuf[i] = (unsigned char) rnd();
			key = kbuf;
		}
		lo = weave_dv_dict_lower_bound(img, key, klen, memcmp_cmp, NULL);
		hi = weave_dv_dict_upper_bound(img, key, klen, memcmp_cmp, NULL);
		assert(lo <= ndict && hi <= ndict);
		if (sorted)
		{
			uint32_t	blo = 0;
			uint32_t	bhi = 0;

			for (i = 0; i < ndict; i++)
			{
				uint32_t	elen;
				const unsigned char *e = weave_docvals_dict_entry(img, i, &elen);
				int			c = memcmp_cmp(NULL, e, elen, key, klen);

				blo += (c < 0);
				bhi += (c <= 0);
			}
			assert(lo == blo && hi == bhi);
			assert(hi - lo <= 1u);
		}

		out = (uint64_t *) malloc(((size_t) h.ndocs + 1u) * sizeof(uint64_t));
		for (op = WEAVE_DV_LT; op <= WEAVE_DV_GT; op++)
		{
			int			cnt = weave_dv_eval_ord(img, (WeaveDvStrat) op, lo,
												 lo <= hi ? hi : lo, out,
												 h.ndocs);
			int			c;

			assert(cnt >= 0 && (uint32_t) cnt <= h.ndocs);
			for (c = 1; c < cnt; c++)
				assert(out[c] > out[c - 1]);
		}
		free(out);
	}
	(void) sum;
}

/*
 * Build a well-formed v3 image into a fresh exact-size buffer.  dshape picks
 * the dictionary: 0 = empty (every doc NULL), 1 = one entry, 2 = many, 3 = many
 * with the empty string as entry 0.  Entries are strictly ascending under
 * memcmp by construction: entry i (i >= the empty one) starts with the two
 * big-endian bytes of i, then a random tail of 0..6 bytes.
 */
static unsigned char *
build_text(uint32_t n, int has_nulls, int dshape, size_t *lenp)
{
	uint32_t	ndict;
	uint32_t   *offs;
	unsigned char *blob;
	size_t		bloblen = 0;
	int64_t    *ords = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
	uint64_t   *docids = (uint64_t *) malloc((n ? n : 1) * sizeof(uint64_t));
	uint8_t    *nullbits = (uint8_t *) malloc(n ? n : 1);
	unsigned char *img;
	uint32_t	i;
	uint32_t	first = 0;

	switch (dshape)
	{
		case 0:
			ndict = 0;
			break;
		case 1:
			ndict = 1;
			break;
		default:
			ndict = 2u + rnd() % 200u;
			break;
	}
	offs = (uint32_t *) malloc(((size_t) ndict + 1u) * sizeof(uint32_t));
	blob = (unsigned char *) malloc((size_t) ndict * 8u + 1u);
	offs[0] = 0;
	if (dshape == 3)
	{
		offs[1] = 0;			/* entry 0 is '' */
		first = 1;
	}
	for (i = first; i < ndict; i++)
	{
		uint32_t	tail = rnd() % 7u;
		uint32_t	t;

		blob[bloblen++] = (unsigned char) (i >> 8);
		blob[bloblen++] = (unsigned char) i;
		for (t = 0; t < tail && bloblen < (size_t) ndict * 8u; t++)
			blob[bloblen++] = (unsigned char) rnd();
		offs[i + 1] = (uint32_t) bloblen;
	}

	fill_docids(docids, n);
	for (i = 0; i < n; i++)
	{
		/* an empty dictionary admits only NULL docs */
		nullbits[i] = (ndict == 0) ? 1 : (has_nulls ? (uint8_t) ((rnd() % 5u) == 0) : 0);
		ords[i] = ndict ? (int64_t) (rnd() % ndict) : (int64_t) rnd();
	}
	if (ndict == 0)
		has_nulls = 1;
	*lenp = weave_docvals_store_len_text(n, has_nulls, ndict, bloblen);
	img = (unsigned char *) malloc(*lenp ? *lenp : 1);
	weave_docvals_build_text(img, ords, docids, has_nulls ? nullbits : NULL, n,
							 offs, ndict, blob);
	free(offs);
	free(blob);
	free(ords);
	free(docids);
	free(nullbits);
	return img;
}

/* offset of the dictionary's ndict word / offs[] / blob in a built image */
static void
text_regions(const unsigned char *img, uint64_t *dictoff, uint64_t *offsbase,
			 uint64_t *blobbase, uint32_t *ndict)
{
	WeaveDocvalsHeader h;

	memcpy(&h, img, sizeof(h));
	*dictoff = h.dict_off;
	memcpy(ndict, img + h.dict_off, sizeof(*ndict));
	*offsbase = (uint64_t) h.dict_off + 4u;
	*blobbase = *offsbase + ((uint64_t) *ndict + 1u) * 4u;
}

/* validate a copy of img[0..len) in an EXACT-size buffer; verify on accept */
static int
check_copy(const unsigned char *img, size_t len, int sorted,
		   unsigned long *accepted, unsigned long *rejected)
{
	unsigned char *exact = (unsigned char *) malloc(len ? len : 1);
	const char *why;

	memcpy(exact, img, len);
	why = VALIDATE(exact, len);
	if (why == NULL)
	{
		WeaveDocvalsHeader h;

		/* a smash can rewrite the version too, so dispatch on what is there */
		memcpy(&h, exact, sizeof(h));
		if (h.version == 3)
			verify_accepted_text(exact, len, sorted);
		else
			verify_accepted(exact, len);
		(*accepted)++;
	}
	else
		(*rejected)++;
	free(exact);
	return why == NULL;
}

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
	if (h.version == 3)
	{
		verify_accepted_text(img, len, 0);
		return;
	}
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

	/* 6. every well-formed v3 (text) store is accepted and every reader over it
	 * agrees with a linear recount: n from 0, with and without a null bitmap,
	 * ndict 0 / 1 / many / many-with-'' (empty entries are real entries). */
	for (trial = 0; trial < 1200; trial++)
	{
		uint32_t	n = (uint32_t) (trial % 150);
		int			dshape = (trial / 2) % 4;
		size_t		len;
		unsigned char *img = build_text(n, trial & 1, dshape, &len);
		const char *why = VALIDATE(img, len);

#ifndef PLANT_BUG
		if (why != NULL)
		{
			fprintf(stderr, "well-formed v3 image (n=%u, dict shape %d) rejected: %s\n",
					n, dshape, why);
			free(img);
			return 1;
		}
		verify_accepted_text(img, len, 1);
		accepted++;
#else
		(void) why;
#endif
		free(img);
		iters++;
	}

	/* 7. a well-formed v3 store truncated to every length: the dictionary blob
	 * ends the image, so EVERY proper prefix must be refused, and none may be
	 * read past (each cut is handed over in an exact-size buffer). */
	for (trial = 0; trial < 96; trial++)
	{
		uint32_t	n = (uint32_t) (trial % 24);
		size_t		len;
		unsigned char *img = build_text(n, trial & 1, (trial / 2) % 4, &len);
		size_t		cut;

		for (cut = 0; cut < len; cut++)
		{
			int			ok = check_copy(img, cut, 1, &accepted, &rejected);

#ifndef PLANT_BUG
			assert(!ok);
#else
			(void) ok;
#endif
			iters++;
		}
		free(img);
	}

	/* 8. targeted mutations of the dictionary region on a valid base: dict_off,
	 * ndict, offs[], and the per-doc ordinals.  Every variant below is a
	 * store no writer produces and must be REFUSED; the ordinal variants are the
	 * PLANT_BUG_DICT teeth (accepted there, and verify_accepted_text aborts). */
	for (trial = 0; trial < 256; trial++)
	{
		uint32_t	n = 1u + (uint32_t) (trial % 64);
		size_t		len;
		unsigned char *base = build_text(n, trial & 1, 2 + (trial / 2) % 2, &len);
		unsigned char *m = (unsigned char *) malloc(len);
		WeaveDocvalsHeader h;
		uint64_t	dictoff;
		uint64_t	offsbase;
		uint64_t	blobbase;
		uint32_t	ndict;
		uint32_t	u;
		uint32_t	v;
		uint32_t	j;
		int64_t		o;
		int			variant;

#ifndef PLANT_BUG
		assert(VALIDATE(base, len) == NULL);
#endif
		text_regions(base, &dictoff, &offsbase, &blobbase, &ndict);
		assert(ndict >= 2);

		for (variant = 0; variant < 12; variant++)
		{
			int			ordvariant = 0;
			int			ok;

			memcpy(m, base, len);
			memcpy(&h, m, sizeof(h));
			switch (variant)
			{
				case 0:		/* dict_off one slot late */
					h.dict_off += 8u;
					break;
				case 1:		/* dict_off one slot early (into the docids/bitmap) */
					h.dict_off -= 8u;
					break;
				case 2:		/* dict_off misaligned */
					h.dict_off += 1u;
					break;
				case 3:		/* v3 with no dictionary */
					h.dict_off = 0;
					break;
				case 4:		/* dict_off far past the image */
					h.dict_off = 0xFFFFFFF0u;
					break;
				case 5:		/* ndict huge: ndict + 1 must not wrap to 0 */
					u = 0xFFFFFFFFu;
					memcpy(m + dictoff, &u, 4);
					break;
				case 6:		/* offs[0] != 0 */
					u = 1;
					memcpy(m + offsbase, &u, 4);
					break;
				case 7:		/* a decreasing pair: offs[j] < offs[j-1] */
					j = 2u + rnd() % (ndict - 1u);
					memcpy(&u, m + offsbase + (size_t) (j - 1u) * 4u, 4);
					memcpy(&v, m + offsbase + (size_t) j * 4u, 4);
					if (u == 0)
					{
						/* make offs[j-1] positive first, then undercut it */
						u = v + 1u;
						memcpy(m + offsbase + (size_t) (j - 1u) * 4u, &u, 4);
					}
					else
					{
						v = u - 1u;
						memcpy(m + offsbase + (size_t) j * 4u, &v, 4);
					}
					break;
				case 8:		/* the blob claims one byte more than is there */
					memcpy(&u, m + offsbase + (size_t) ndict * 4u, 4);
					u += 1u;
					memcpy(m + offsbase + (size_t) ndict * 4u, &u, 4);
					break;
				case 9:		/* ordinal == ndict on a non-NULL doc */
				case 10:	/* ordinal == -1 */
				case 11:	/* ndict == 0 while a non-NULL doc exists */
					ordvariant = 1;
					for (j = 0; j < n; j++)
						if (!weave_docvals_isnull(base, j))
							break;
					if (j == n)
					{
						ordvariant = -1;	/* every doc NULL: nothing to aim at */
						break;
					}
					if (variant == 11)
					{
						/* shrink to ndict 0 with an intact (empty) offs/blob:
						 * offs[0] = 0 is already in place, and the image is
						 * cut to exactly the region an empty dictionary spans */
						u = 0;
						memcpy(m + dictoff, &u, 4);
						break;
					}
					o = (variant == 9) ? (int64_t) ndict : -1;
					memcpy(m + h.values_off + (size_t) j * 8u, &o, 8);
					break;
			}
			if (ordvariant < 0)
				continue;
			if (variant <= 4)
				memcpy(m, &h, sizeof(h));
			ok = check_copy(m, variant == 11 ? (size_t) offsbase + 4u : len, 0,
							&accepted, &rejected);
#ifdef PLANT_BUG_DICT
			if (!ordvariant)
				assert(!ok);
#elif !defined(PLANT_BUG)
			if (ok)
			{
				fprintf(stderr, "v3 mutation %d (n=%u) was ACCEPTED\n", variant, n);
				return 1;
			}
#else
			(void) ok;
#endif
			iters++;
		}
		free(m);
		free(base);
	}

	/* 9. random smashes of v3 images, half of them aimed at the dictionary
	 * region, plus a torn length; whatever is accepted must be readable by
	 * every reader without leaving the buffer */
	for (trial = 0; trial < 150000; trial++)
	{
		uint32_t	n = rnd() % 130u;
		size_t		len;
		unsigned char *img = build_text(n, (int) (rnd() & 1u), (int) (rnd() % 4u), &len);
		int			nsmash = 1 + (int) (rnd() % 6);
		uint64_t	dictoff;
		uint64_t	offsbase;
		uint64_t	blobbase;
		uint32_t	ndict;
		size_t		avail;
		int			k;

		text_regions(img, &dictoff, &offsbase, &blobbase, &ndict);
		for (k = 0; k < nsmash; k++)
		{
			size_t		at;

			if ((rnd() & 1u) && len > dictoff)
				at = (size_t) dictoff + rnd() % (len - (size_t) dictoff);
			else
				at = rnd() % len;
			img[at] = (unsigned char) rnd();
		}
		avail = (rnd() % 4 == 0) ? (rnd() % (len + 1)) : len;
		(void) check_copy(img, avail, 0, &accepted, &rejected);
		free(img);
		iters++;
	}

	printf("fuzz_docvals: %lu cases, %lu accepted, %lu rejected -- no overread, no UB\n",
		   iters, accepted, rejected);
	return 0;
}
