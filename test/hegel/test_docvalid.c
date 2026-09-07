/*
 * Property-based tests for weave_doc_check() (weave/docvalid.h), the pure
 * structural validator for an on-disk WeaveDoc image.  This is the last line of
 * defense against a corrupt/hostile wdoc datum tricking the reader into an
 * out-of-bounds access, so the properties here are about SAFETY: a well-formed
 * image is accepted, and no input -- valid, mutated, or arbitrary garbage --
 * makes weave_doc_check itself read outside [base, base+sz) or accept an image
 * whose derived offsets escape the buffer.  Run under ASan in CI: an OOB read
 * inside weave_doc_check is a test failure, not a silent pass.
 *
 * Standalone: weave/docvalid.h is pure C (no PostgreSQL deps), so we include
 * it directly -- same source the extension's weave_doc_is_valid() wraps.
 *
 * The builder below (build_valid_doc) lays out bytes byte-for-byte like
 * weave_doc_build() in pg_weave_doc.c: header, entries[nterms], lexemes[lexbytes],
 * MAXALIGN pad, positions[sumtf].  Keeping it here (not in the header) avoids
 * bloating the shipped extension with test-only code.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <hegel/hegel.h>
#include <hegel/generators.h>

#include "weave/docvalid.h"

/* Layout constants, mirrored from pg_weave.h (see weave/docvalid.h header). */
#define ENTSZ		sizeof(WeaveDvTermEntry)
#define HDRSZ		WEAVE_DV_HDRSIZE
#define MA(x)		WEAVE_DV_MAXALIGN(x)

/* Generation bounds -- small enough to keep buffers on the stack, large enough
 * to exercise multi-entry / multi-position layouts and the MAXALIGN pad. */
#define MAXTERMS	8
#define MAXLEXPER	6			/* max lexeme bytes per term */
#define MAXTFPER	5			/* max tf (positions) per term */
#define MAXVARSIZE	(HDRSZ + MAXTERMS * ENTSZ + MAXTERMS * MAXLEXPER + 8 + \
					 MAXTERMS * MAXTFPER * sizeof(uint32_t))
/* headroom so mutations that GROW an offset still land inside the raw buffer
 * (weave_doc_check must reject them; it must never read past sz, and sz here is
 * the true buffer size, so ASan catches any overread). */
#define BUFCAP		(MAXVARSIZE + 256)

static hegel_session *session;

/* A generated logical doc, before serialization. */
typedef struct
{
	uint32_t	nterms;
	int			has_pos;
	uint32_t	len[MAXTERMS];	/* lexeme bytes for term i */
	uint32_t	tf[MAXTERMS];	/* term frequency (>=1) for term i */
} GenDoc;

/* Draw a well-formed logical doc: nterms in [0,MAXTERMS], each term len in
 * [0,MAXLEXPER], tf in [1,MAXTFPER]. */
static void
draw_doc(hegel_test_case *tc, GenDoc *g)
{
	uint32_t	i;

	g->nterms = (uint32_t) hegel_draw_int(tc, hegel_integers(0, MAXTERMS));
	g->has_pos = (int) hegel_draw_int(tc, hegel_integers(0, 1));
	for (i = 0; i < g->nterms; i++)
	{
		g->len[i] = (uint32_t) hegel_draw_int(tc, hegel_integers(0, MAXLEXPER));
		g->tf[i] = (uint32_t) hegel_draw_int(tc, hegel_integers(1, MAXTFPER));
	}
}

/*
 * Serialize a GenDoc into buf exactly as weave_doc_build() would.  Returns the
 * VARSIZE (== total image bytes).  off/posoff are laid out consecutively, so
 * the image is internally consistent and weave_doc_check() must accept it.
 */
static uint32_t
build_valid_doc(const GenDoc *g, unsigned char *buf)
{
	uint32_t	lexbytes = 0;
	uint32_t	sumtf = 0;
	uint32_t	i;
	size_t		posbase;
	uint32_t	total;
	WeaveDvDocData *doc;
	WeaveDvTermEntry *entries;
	uint32_t	off = 0;
	uint32_t	pidx = 0;

	for (i = 0; i < g->nterms; i++)
	{
		lexbytes += g->len[i];
		sumtf += g->tf[i];
	}

	posbase = MA(HDRSZ + (size_t) g->nterms * ENTSZ + lexbytes);
	total = (uint32_t) (g->has_pos
						? posbase + (size_t) sumtf * sizeof(uint32_t)
						: HDRSZ + (size_t) g->nterms * ENTSZ + lexbytes);

	memset(buf, 0, total);
	doc = (WeaveDvDocData *) buf;
	doc->vl_len_ = 0;			/* validator reads VARSIZE via the sz/varsize args */
	doc->version = WEAVE_DV_VERSION;
	doc->flags = g->has_pos ? WEAVE_DV_FLAG_POSITIONS : 0;
	doc->nterms = g->nterms;
	doc->doclen = sumtf;
	doc->lexbytes = lexbytes;

	entries = (WeaveDvTermEntry *) (buf + HDRSZ);
	for (i = 0; i < g->nterms; i++)
	{
		entries[i].off = off;
		entries[i].len = g->len[i];
		entries[i].tf = g->tf[i];
		entries[i].posoff = pidx;
		off += g->len[i];
		pidx += g->tf[i];
	}
	/* lexemes[] and positions[] left as zero fill -- content is irrelevant to
	 * the structural validator, only the sizes/offsets matter. */
	return total;
}

/* ---- Property: a well-formed image is accepted. ---- */
static void
prop_accepts_valid(hegel_test_case *tc, void *ctx)
{
	GenDoc		g;
	unsigned char buf[BUFCAP];
	uint32_t	vs;

	(void) ctx;
	draw_doc(tc, &g);
	vs = build_valid_doc(&g, buf);
	/* sz == vs: the caller hands the exact image. Must accept. */
	assert_true(weave_doc_check(buf, vs, vs) == 1);
}

/*
 * ---- Property: monotone truncation. A valid doc whose readable size (sz) OR
 * declared VARSIZE is cut below the true image size is rejected. ----
 * weave_doc_check trusts min(sz, varsize); any short read must fail closed.
 */
static void
prop_truncation_rejected(hegel_test_case *tc, void *ctx)
{
	GenDoc		g;
	unsigned char buf[BUFCAP];
	uint32_t	vs;
	uint32_t	cut;

	(void) ctx;
	draw_doc(tc, &g);
	vs = build_valid_doc(&g, buf);
	if (vs == 0)
		return;				/* HDRSZ>0 always, but guard anyway */
	/* cut to something strictly smaller than the real image */
	cut = (uint32_t) hegel_draw_int(tc, hegel_integers(0, (int) vs - 1));

	/* (a) declared VARSIZE shrunk below true size: reject. */
	assert_true(weave_doc_check(buf, vs, cut) == 0);
	/* (b) readable sz shrunk below VARSIZE: varsize>sz -> reject. */
	assert_true(weave_doc_check(buf, cut, vs) == 0);
}

/*
 * ---- Property: an out-of-bounds structural mutation is rejected. ----
 * Take a valid image, bump exactly one field so that a derived offset provably
 * escapes the buffer, and assert weave_doc_check returns 0.  This is the exact
 * class of corruption the validator exists to catch.  We only assert on
 * mutations we can PROVE push past sz -- the contrapositive of "accept implies
 * in-bounds".
 */
static void
prop_oob_mutation_rejected(hegel_test_case *tc, void *ctx)
{
	GenDoc		g;
	unsigned char buf[BUFCAP];
	uint32_t	vs;
	WeaveDvDocData *doc;
	WeaveDvTermEntry *entries;
	int			which;

	(void) ctx;
	draw_doc(tc, &g);
	vs = build_valid_doc(&g, buf);
	doc = (WeaveDvDocData *) buf;
	entries = (WeaveDvTermEntry *) (buf + HDRSZ);

	/*
	 * Pick a mutation. Each branch grows a size/offset field past what the
	 * image can hold, so the derived bound provably exceeds vs -- reject.
	 * VARSIZE stays == vs (the true buffer), so weave_doc_check reads only
	 * in-bounds bytes; ASan proves it.
	 */
	which = (int) hegel_draw_int(tc, hegel_integers(0, 4));
	switch (which)
	{
		case 0:
			/* nterms so large that header+entries[] alone overruns sz. */
			doc->nterms = (uint32_t) (vs / ENTSZ + 2);
			break;
		case 1:
			/* lexbytes larger than any room after header+entries. */
			doc->lexbytes = vs + 1;
			break;
		case 2:
			/* an entry's lexeme slice escapes lexbytes (off beyond lexbytes). */
			if (g.nterms == 0)
				return;			/* nothing to mutate; other cases cover it */
			entries[hegel_draw_int(tc, hegel_integers(0, (int) g.nterms - 1))].len =
				doc->lexbytes + 1;
			break;
		case 3:
			/* a term's tf run overshoots the positions region (only meaningful
			 * with positions present). posoff+tf > sumtf -> reject. */
			if (g.nterms == 0 || !g.has_pos)
				return;
			entries[hegel_draw_int(tc, hegel_integers(0, (int) g.nterms - 1))].tf =
				vs;				/* dwarfs sumtf */
			break;
		default:
			/* shrink VARSIZE below the header: reject (header must fit). */
			assert_true(weave_doc_check(buf, vs, (uint32_t) (HDRSZ - 1)) == 0);
			return;
	}
	assert_true(weave_doc_check(buf, vs, vs) == 0);
}

/*
 * ---- Property: robustness. Arbitrary bytes + arbitrary declared VARSIZE and
 * readable sz never make weave_doc_check crash or read out of bounds. ----
 * The buffer is exactly `sz` bytes (ASan redzone immediately after), so any
 * overread aborts. We assert nothing about the return value -- only that the
 * call returns at all without touching bytes it must not.
 */
static void
prop_fuzz_no_oob(hegel_test_case *tc, void *ctx)
{
	unsigned char *buf;
	int			sz;
	uint32_t	vs;
	int			i;
	int			r;

	(void) ctx;
	sz = (int) hegel_draw_int(tc, hegel_integers(0, (int) MAXVARSIZE));
	/* exact-size alloc so ASan flags any read at buf[sz] or beyond. */
	buf = (unsigned char *) malloc((size_t) sz);
	assert_true(sz == 0 || buf != NULL);
	for (i = 0; i < sz; i++)
		buf[i] = (unsigned char) hegel_draw_int(tc, hegel_integers(0, 255));
	/* declared VARSIZE independent of sz: exercises varsize>sz and varsize<sz. */
	vs = (uint32_t) hegel_draw_int(tc, hegel_integers(0, (int) MAXVARSIZE + 16));

	r = weave_doc_check(buf, (size_t) sz, vs);
	assert_true(r == 0 || r == 1);	/* well-defined boolean, no crash */
	free(buf);
}

/* ---- cmocka wrappers ---- */
#define RUN(prop) do { \
	hegel_session *s = (hegel_session *) *state; \
	hegel_settings settings = HEGEL_DEFAULT_SETTINGS; \
	hegel_results r; \
	settings.max_examples = 500; \
	r = hegel_run_test(s, prop, NULL, &settings); \
	assert_true(r.passed); \
	hegel_results_free(&r); \
} while (0)

static void test_accepts_valid(void **state) { RUN(prop_accepts_valid); }
static void test_truncation_rejected(void **state) { RUN(prop_truncation_rejected); }
static void test_oob_mutation_rejected(void **state) { RUN(prop_oob_mutation_rejected); }
static void test_fuzz_no_oob(void **state) { RUN(prop_fuzz_no_oob); }

static int
setup(void **state)
{
	session = hegel_session_new();
	*state = session;
	return 0;
}

static int
teardown(void **state)
{
	hegel_session_free((hegel_session *) *state);
	return 0;
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_accepts_valid),
		cmocka_unit_test(test_truncation_rejected),
		cmocka_unit_test(test_oob_mutation_rejected),
		cmocka_unit_test(test_fuzz_no_oob),
	};

	return cmocka_run_group_tests(tests, setup, teardown);
}
