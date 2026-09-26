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
 * values out of an image sized EXACTLY to len.  ASan's redzone sits immediately
 * past the last readable byte, so a validator that accepted a too-short image
 * turns into a hard overread rather than a read of adjacent bytes.
 *
 * TEETH (PLANT_BUG=1): builds a deliberately weakened copy of the validator,
 * weak_validate() below, with the "len >= values_off + ndocs*8" check removed --
 * the single most likely guard for someone to drop, since the magic/version
 * checks look like they already bound the image.  A truncated image with a large
 * ndocs is then accepted, the read-all-values postcondition walks past the exact
 * buffer, and ASan aborts, proving the harness detects the bug class instead of
 * passing vacuously.  See run.sh.
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

#ifdef PLANT_BUG
/*
 * The planted bug: weave_docvals_validate() with the image-length guard deleted.
 * Everything else is identical.  A too-short image with a large ndocs is then
 * accepted, and the read-all-values postcondition below MUST overrun.
 */
static const char *
weak_validate(const void *img, size_t len)
{
	WeaveDocvalsHeader h;

	if (len < sizeof(WeaveDocvalsHeader))
		return "image shorter than header";
	memcpy(&h, img, sizeof(h));
	if (h.magic != WEAVE_DOCVALS_MAGIC)
		return "bad magic";
	if (h.version != 1)
		return "unsupported version";
	if (h.typid_kind != 1)
		return "unsupported value kind";
	if (h.values_off != (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader)))
		return "values_off mismatch";
	if (h.null_off != 0)
		return "null_off must be 0";
	if (h.zonemap_off != 0)
		return "zonemap_off must be 0";
	if (h.reserved != 0)
		return "reserved must be 0";
	/* THE MISSING GUARD:
	 * need = values_off + ndocs*8; if (len < need) return "too short"; */
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

	assert(len >= sizeof(WeaveDocvalsHeader));
	memcpy(&h, img, sizeof(h));
	assert(h.magic == WEAVE_DOCVALS_MAGIC);
	assert(h.version == 1);
	assert(h.typid_kind == 1);
	assert(h.values_off == (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader)));
	assert(h.null_off == 0);
	assert(h.zonemap_off == 0);
	assert(h.reserved == 0);

	need = (uint64_t) h.values_off + (uint64_t) h.ndocs * 8u;
	assert((uint64_t) len >= need);

	/* Read every value: in the accepted case this is in-bounds by construction;
	 * under PLANT_BUG a too-short accepted image overruns here and ASan aborts. */
	for (d = 0; d < h.ndocs; d++)
		acc ^= weave_docvals_int8(img, d);
	(void) acc;
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
		uint32_t	i;
		const char *why;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) (((uint64_t) rnd() << 32) | rnd());
		weave_docvals_build(exact, vals, n);

		why = VALIDATE(exact, len);
		if (why != NULL)
		{
			fprintf(stderr, "well-formed image of %u value(s) rejected: %s\n",
					n, why);
			free(exact);
			free(vals);
			return 1;
		}
		verify_accepted(exact, len);
		for (i = 0; i < n; i++)
			assert(weave_docvals_int8(exact, i) == vals[i]);
		free(exact);
		free(vals);
		iters++;
	}

	/* 2. a well-formed image truncated to every length must be rejected or
	 * accepted consistently, never overread */
	for (trial = 0; trial <= 64; trial++)
	{
		uint32_t	n = (uint32_t) trial;
		size_t		len = weave_docvals_store_len(n);
		unsigned char *full = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		uint32_t	i;
		size_t		cut;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) rnd();
		weave_docvals_build(full, vals, n);

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
	}

	/* 3. random single- and multi-byte corruption of a well-formed image, plus a
	 * declared length torn independently of the buffer we hand over */
	for (trial = 0; trial < 200000; trial++)
	{
		uint32_t	n = rnd() % (MAXN + 1);
		size_t		len = weave_docvals_store_len(n);
		unsigned char *scratch = (unsigned char *) malloc(len);
		int64_t	   *vals = (int64_t *) malloc((n ? n : 1) * sizeof(int64_t));
		int			nsmash = 1 + (int) (rnd() % 8);
		unsigned char *exact;
		size_t		avail;
		uint32_t	i;
		const char *why;
		int			k;

		for (i = 0; i < n; i++)
			vals[i] = (int64_t) rnd();
		weave_docvals_build(scratch, vals, n);
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
			h.reserved = 0;
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

	printf("fuzz_docvals: %lu cases, %lu accepted, %lu rejected -- no overread, no UB\n",
		   iters, accepted, rejected);
	return 0;
}
