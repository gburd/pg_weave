/*
 * fuzz_docvalid.c -- corruption/fuzz harness for the WeaveDoc structural
 * validator (weave_doc_check in weave/docvalid.h, the pure body of
 * weave_doc_is_valid).  This guards the v0.3.3 pending-doc-overflow class: the
 * readers cast untrusted page bytes to an WeaveDoc and walk entries[].len/off/
 * tf/posoff; the validator must reject any image whose derived offsets escape
 * the buffer, and it must do so WITHOUT itself reading out of bounds.
 *
 * Property (under ASan+UBSan): weave_doc_check(buf, sz, varsize) returns cleanly
 * (0 or 1) and NEVER reads outside [buf, buf+sz), for
 *   (a) fully random byte buffers of random length, and
 *   (b) structured-but-corrupted WeaveDoc images: a valid header then mutated
 *       nterms / entries[].len/off/tf / posoff / lexbytes / VARSIZE, plus
 *       truncated buffers and huge counts.
 *
 * TEETH: the buffer under test is a heap allocation of EXACTLY `sz` bytes (plus
 * we pass the true readable size as `sz`), so ASan's redzones turn any read
 * past the image into a hard failure.  We deliberately pass the REAL allocation
 * size as the `sz` argument -- so if the validator trusts a mutated VARSIZE and
 * reads past it, that read is caught.  We also run a pass where `sz` is set to
 * the mutated (possibly larger) VARSIZE to stress the "declared > actual" guard,
 * using a buffer sized to the declared value.
 *
 * No hegel/cmocka: deterministic PRNG loop, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/docvalid.h"

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
static uint32_t
rng_u32(void)
{
	return (uint32_t) rng_next();
}

/* Little-endian 4B varlena size, matching x86_64 VARSIZE_4B on a SET_VARSIZE'd
 * (uncompressed, 4-byte) datum: low 30 bits of the header word. */
static uint32_t
read_varsize(const void *base)
{
	uint32_t	w;

	memcpy(&w, base, sizeof(w));	/* first 4 bytes = vl_len_ */
	return w & 0x3FFFFFFFu;
}

/*
 * Run the validator once against a buffer whose TRUE readable length is `sz`.
 * weave_doc_check reads varsize itself via the caller; we mirror the backend by
 * reading it the same way the extension's SET_VARSIZE laid it down.  ASan
 * guarantees any OOB read aborts; the return value is just consumed (both 0 and
 * 1 are acceptable -- the property is "no crash / no OOB", not a fixed verdict).
 */
static int
check(const unsigned char *buf, size_t sz)
{
	uint32_t	varsize = (sz >= 4) ? read_varsize(buf) : 0;
	int			r = weave_doc_check(buf, sz, varsize);

	assert(r == 0 || r == 1);
	return r;
}

/* (a) fully random buffers of random length. */
static void
fuzz_random_bytes(void)
{
	int			iter;

	for (iter = 0; iter < 200000; iter++)
	{
		size_t		sz = (size_t) (rng_next() % 512);
		unsigned char *buf;
		size_t		k;

		buf = (unsigned char *) malloc(sz ? sz : 1);
		assert(buf != NULL);
		for (k = 0; k < sz; k++)
			buf[k] = (unsigned char) rng_next();
		check(buf, sz);			/* exact-size alloc: ASan catches any OOB read */
		free(buf);
	}
}

/*
 * Build a well-formed WeaveDoc image into a freshly-sized heap block, then hand
 * the caller a mutation hook.  Returns the malloc'd buffer and its size; caller
 * frees.  has_pos controls whether the positions region is laid out.
 */
static unsigned char *
build_valid(uint32_t nterms, uint32_t lexbytes, int has_pos,
			uint32_t *tfs, uint32_t *offs, uint32_t *lens, uint32_t *posoffs,
			size_t *out_sz)
{
	size_t		hdr = WEAVE_DV_HDRSIZE;
	size_t		entries_bytes = (size_t) nterms * sizeof(WeaveDvTermEntry);
	uint64_t	sumtf = 0;
	size_t		posbase;
	size_t		total;
	unsigned char *buf;
	WeaveDvDocData *doc;
	WeaveDvTermEntry *e;
	uint32_t	i;

	for (i = 0; i < nterms; i++)
		sumtf += tfs[i];

	posbase = WEAVE_DV_MAXALIGN(hdr + entries_bytes + lexbytes);
	total = has_pos ? posbase + (size_t) sumtf * sizeof(uint32_t)
		: hdr + entries_bytes + lexbytes;
	if (total < 4)
		total = 4;

	buf = (unsigned char *) calloc(1, total);
	assert(buf != NULL);
	doc = (WeaveDvDocData *) buf;
	doc->vl_len_ = (int32_t) total;	/* low-30-bit varlena size (top bits 0) */
	doc->version = WEAVE_DV_VERSION;
	doc->flags = has_pos ? WEAVE_DV_FLAG_POSITIONS : 0;
	doc->nterms = nterms;
	doc->doclen = (uint32_t) sumtf;
	doc->lexbytes = lexbytes;

	e = (WeaveDvTermEntry *) (buf + hdr);
	for (i = 0; i < nterms; i++)
	{
		e[i].off = offs[i];
		e[i].len = lens[i];
		e[i].tf = tfs[i];
		e[i].posoff = posoffs[i];
	}
	*out_sz = total;
	return buf;
}

/*
 * (b) structured-but-corrupted images.  Start from a consistent doc, then mutate
 * one or more header/entry fields to adversarial values.  We keep the buffer's
 * TRUE size == the honest layout size, but let VARSIZE / nterms / lens / offs /
 * tf / posoff / lexbytes be corrupted -- exactly the fields the readers trust.
 */
static void
fuzz_structured(void)
{
	int			iter;

	for (iter = 0; iter < 200000; iter++)
	{
		uint32_t	nterms = (uint32_t) (rng_next() % 6);	/* small, well-formed base */
		int			has_pos = (int) (rng_next() & 1);
		uint32_t	tfs[6],
					offs[6],
					lens[6],
					posoffs[6];
		uint32_t	lexbytes = 0;
		uint32_t	posidx = 0;
		uint32_t	i;
		size_t		sz;
		unsigned char *buf;
		WeaveDvDocData *doc;
		WeaveDvTermEntry *e;

		/* lay out a consistent base doc */
		for (i = 0; i < nterms; i++)
		{
			uint32_t	l = (uint32_t) (rng_next() % 8);
			uint32_t	tf = 1 + (uint32_t) (rng_next() % 4);

			lens[i] = l;
			offs[i] = lexbytes;
			lexbytes += l;
			tfs[i] = tf;
			posoffs[i] = posidx;
			posidx += tf;
		}

		buf = build_valid(nterms, lexbytes, has_pos, tfs, offs, lens, posoffs, &sz);
		doc = (WeaveDvDocData *) buf;
		e = (WeaveDvTermEntry *) (buf + WEAVE_DV_HDRSIZE);

		/* the honest image must validate (sanity: our generator is correct) */
		assert(check(buf, sz) == 1);

		/* now corrupt 1..3 fields to adversarial values and re-check. The
		 * TRUE readable size stays `sz`; ASan catches any OOB read the
		 * validator makes while trusting the corrupted header. */
		int			nmut = 1 + (int) (rng_next() % 3);

		for (int m = 0; m < nmut; m++)
		{
			switch (rng_next() % 9)
			{
				case 0:			/* huge / random nterms */
					doc->nterms = rng_u32();
					break;
				case 1:			/* mutate a VARSIZE bigger than the buffer */
					doc->vl_len_ = (int32_t) (rng_u32() & 0x3FFFFFFF);
					break;
				case 2:			/* huge lexbytes */
					doc->lexbytes = rng_u32();
					break;
				case 3:			/* corrupt an entry's len */
					if (nterms)
						e[rng_next() % nterms].len = rng_u32();
					break;
				case 4:			/* corrupt an entry's off */
					if (nterms)
						e[rng_next() % nterms].off = rng_u32();
					break;
				case 5:			/* corrupt an entry's tf (blows sumtf) */
					if (nterms)
						e[rng_next() % nterms].tf = rng_u32();
					break;
				case 6:			/* corrupt an entry's posoff */
					if (nterms)
						e[rng_next() % nterms].posoff = rng_u32();
					break;
				case 7:			/* flip version / flags */
					doc->version = (uint16_t) rng_next();
					doc->flags = (uint16_t) rng_next();
					break;
				case 8:			/* flip the positions flag on/off */
					doc->flags ^= WEAVE_DV_FLAG_POSITIONS;
					break;
			}
		}

		/* pass 1: honest readable size == the allocation. */
		check(buf, sz);

		/* pass 2: truncated buffer -- hand the validator a smaller `sz` than the
		 * image, so it must not read past the truncation point.  We re-alloc an
		 * exact-size copy so ASan redzones the truncation boundary. */
		{
			size_t		tsz = sz ? (size_t) (rng_next() % sz) : 0;
			unsigned char *tbuf = (unsigned char *) malloc(tsz ? tsz : 1);

			assert(tbuf != NULL);
			memcpy(tbuf, buf, tsz);
			check(tbuf, tsz);
			free(tbuf);
		}

		/* pass 3: declared VARSIZE says the image is LARGER than it is, and we
		 * pass that inflated size as `sz` -- but back it with an exact-size
		 * buffer of the DECLARED length, so a validator that walks to the
		 * declared end still stays inside a real allocation (property: it must
		 * not read past what it was told, and must reject when offsets escape). */
		{
			uint32_t	declared = 32 + (rng_u32() % 4096);
			unsigned char *dbuf = (unsigned char *) calloc(1, declared);

			assert(dbuf != NULL);
			memcpy(dbuf, buf, sz < declared ? sz : declared);
			/* force the varlena header to the declared size */
			{
				uint32_t	w = declared & 0x3FFFFFFFu;

				memcpy(dbuf, &w, sizeof(w));
			}
			check(dbuf, declared);
			free(dbuf);
		}

		free(buf);
	}
}

int
main(void)
{
	rng_seed(0xD0C5A11DFA57C0DEULL);	/* fixed seed: reproducible */
	fuzz_random_bytes();
	fuzz_structured();
	printf("fuzz_docvalid: OK (random + structured-corrupt WeaveDoc images)\n");
	return 0;
}
