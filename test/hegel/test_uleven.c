/*-------------------------------------------------------------------------
 *
 * test_uleven.c
 *		Standalone property tests for the Z5 Levenshtein neighbourhood
 *		expansion core (include/weave/uleven.h).  No backend, no PostgreSQL
 *		header, no external test framework -- this file includes only the
 *		header under test and links nothing (weave_uleven_* is all
 *		`static inline`).  The imported pg_tre trigram expander at the bottom
 *		of that header (pg_weave_uleven_expand[_cp], src/query/uleven.c) is a
 *		different, unrelated thing -- see the header's own comment -- and is
 *		not exercised here because it needs postgres.h to build.
 *
 * THE ORACLE.  Every property below is checked against a full-matrix
 * dynamic-programming Levenshtein (naive_lev(), just below) written
 * independently of anything in uleven.h: it does not call
 * weave_uleven_unit_ex(), weave_uleven_match(), or any other function from the
 * header.  Where the ground-truth "what characters does this term decode to"
 * matters (UTF-8 mode), the test supplies it directly from the generator that
 * built the term's bytes, rather than re-deriving it with a second decoder --
 * so there is exactly one decoder in this file's dependency graph (the one
 * under test) plus one oracle that never touches it.  This is deliberate:
 * doc/TESTING.md and include/weave/for.h both warn about a property test that
 * hand-transcribes the logic it is supposed to be checking and therefore
 * cannot fail regardless of what the real code does.
 *
 * Properties, each with its own generator and its own check counter
 * (prop_checks[1..7], printed at the end):
 *
 *   P1  EXACTNESS, BOTH DIRECTIONS.  weave_uleven_match reports MATCH with the
 *       correct *dist* iff naive_lev(query, cand) <= k, for k in 0..4, over
 *       both near pairs (query plus 0..k+2 random edits) and independent
 *       random pairs, in both unit modes.
 *   P2  SKIP IS AN OPTIMIZATION, NOT A FILTER.  weave_uleven_expand_vocab with
 *       a real skip() and with skip==NULL emit IDENTICAL (ord,dist) sets over
 *       the same sorted vocabulary and query.  This is the false-negative
 *       guard AGENTS.md hard rule 1 requires: a dead-prefix that is one byte
 *       too greedy makes skip() over-skip and P2 catches the resulting
 *       divergence even though P1 (single-term) would not see it.
 *   P3  VOCABULARY EXPANSION AGREES WITH BRUTE FORCE.  The emitted set equals
 *       { t in vocab : naive_lev(query, t) <= k }, scanning the whole
 *       vocabulary with the independent oracle.
 *   P4  THE EDIT UNIT IS THE CHARACTER, NOT THE BYTE.  In WEAVE_ULEVEN_UTF8
 *       mode substituting one character for another (of a *different*
 *       UTF-8 byte-length, so the two modes cannot coincide by accident) is
 *       distance 1; the same byte buffer in WEAVE_ULEVEN_BYTE mode is not.
 *   P5  NOT DAMERAU.  An adjacent transposition of two distinct units costs
 *       2, over random generated transpositions in both unit modes, plus the
 *       header's own literal example ("the" vs "hte").
 *   P6  MALFORMED BYTES DECODE INJECTIVELY.  dist(t,t) == 0 and dist(a,b) > 0
 *       for every a != b, over a deduplicated pool that deliberately includes
 *       stray continuation bytes, overlong forms, encoded surrogates,
 *       five-byte leads and truncated sequences -- checked directly through
 *       weave_uleven_match() with k large enough that the real distance is
 *       always within budget, so a violation here is a real bug, not a
 *       starved k.
 *   P7  INPUT VALIDATION.  weave_uleven_init/expand_vocab return the
 *       documented WeaveUlevError for k out of range, a query over
 *       WEAVE_ULEVEN_MAX_UNITS *units* (not bytes), an unrecognized unit
 *       mode, and a missing iterator; a callback that asks to stop actually
 *       stops the walk.
 *
 * MUTATION TESTING.  Per doc/TESTING.md and AGENTS.md hard rule 1, a property
 * test that cannot fail proves nothing.  Each of P1-P6 was checked against a
 * deliberately broken header (dead-prefix off-by-one, byte units substituted
 * for character units, a transposition priced at 1, an inverted k
 * comparison, ...); the mutation table with which property caught each one
 * and how many checks in is recorded in the commit message, not here, because
 * the mutations were reverted before commit and this file must reflect only
 * the real, unmutated header.
 *
 * Build and run:
 *		gcc -O2 -std=c99 -Wall -Wextra -Werror -I include \
 *			-o /scratch/tu test/hegel/test_uleven.c -lm && /scratch/tu
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_uleven.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "weave/uleven.h"

/* ---------------------------------------------------------------------------
 * Check machinery
 * ------------------------------------------------------------------------- */

static long checks = 0;
static long failures = 0;
static long prop_checks[8];	/* [1..7] used */
static long first_fail_at = -1;
static long first_fail_prop_at = -1;
static int	first_fail_prop = -1;

#define CHECKP(p, cond, ...) \
	do { \
		prop_checks[p]++; \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			if (first_fail_prop < 0) \
			{ \
				first_fail_prop = (p); \
				first_fail_at = checks; \
				first_fail_prop_at = prop_checks[p]; \
			} \
			if (failures <= 20) \
			{ \
				printf("FAIL P%d @check %ld (P%d check %ld) %s:%d: ", \
					   (p), checks, (p), prop_checks[p], __FILE__, __LINE__); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

/* directed/fixed checks that do not belong to a generator-driven property */
#define CHECK(cond, ...) CHECKP(7, cond, __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * RNG: xorshift64*, matching test_surf.c / test_pack.c / test_doclen_block.c.
 * Seed from argv[1] if given, else a fixed constant -- reproducible either way.
 * ------------------------------------------------------------------------- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t
rng(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static uint32_t
rnd(uint32_t bound)
{
	return (uint32_t) (rng() % bound);
}

/* inclusive [lo,hi] */
static int
rnd_range(int lo, int hi)
{
	return lo + (int) rnd((uint32_t) (hi - lo + 1));
}

/* ---------------------------------------------------------------------------
 * The oracle: an independent full-matrix DP Levenshtein over unit arrays.
 * Never calls anything from weave/uleven.h.
 * ------------------------------------------------------------------------- */

#define ORACLE_MAXN 128

static int
naive_lev(const weave_ul_int32 *a, int na, const weave_ul_int32 *b, int nb)
{
	static int	d[ORACLE_MAXN + 1][ORACLE_MAXN + 1];
	int			i,
				j;

	if (na < 0 || nb < 0 || na > ORACLE_MAXN || nb > ORACLE_MAXN)
	{
		fprintf(stderr, "oracle: input out of range (%d,%d)\n", na, nb);
		exit(97);
	}

	for (i = 0; i <= na; i++)
		d[i][0] = i;
	for (j = 0; j <= nb; j++)
		d[0][j] = j;

	for (i = 1; i <= na; i++)
	{
		for (j = 1; j <= nb; j++)
		{
			int			cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			int			sub = d[i - 1][j - 1] + cost;
			int			del = d[i - 1][j] + 1;
			int			ins = d[i][j - 1] + 1;
			int			m = sub;

			if (del < m)
				m = del;
			if (ins < m)
				m = ins;
			d[i][j] = m;
		}
	}
	return d[na][nb];
}

/* ---------------------------------------------------------------------------
 * Independent UTF-8 encoder (never calls weave_uleven_unit_ex).  Only ever
 * fed well-formed codepoints (P1/P3/P4/P5's vocabulary and query generators),
 * so this always emits canonical, minimal-length UTF-8.
 * ------------------------------------------------------------------------- */

static int
cp_encode(uint32_t cp, unsigned char *buf)
{
	if (cp <= 0x7F)
	{
		buf[0] = (unsigned char) cp;
		return 1;
	}
	if (cp <= 0x7FF)
	{
		buf[0] = (unsigned char) (0xC0 | (cp >> 6));
		buf[1] = (unsigned char) (0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp <= 0xFFFF)
	{
		buf[0] = (unsigned char) (0xE0 | (cp >> 12));
		buf[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (unsigned char) (0x80 | (cp & 0x3F));
		return 3;
	}
	buf[0] = (unsigned char) (0xF0 | (cp >> 18));
	buf[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
	buf[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
	buf[3] = (unsigned char) (0x80 | (cp & 0x3F));
	return 4;
}

/* random codepoint of a given UTF-8 length class (1..4 bytes), never a
 * surrogate, never NUL, never beyond 0x10FFFF -- always well-formed */
static uint32_t
rnd_cp_class(int cls)
{
	switch (cls)
	{
		case 0:
			return (uint32_t) rnd_range(0x20, 0x7E);
		case 1:
			return (uint32_t) rnd_range(0x80, 0x7FF);
		case 2:
			{
				uint32_t	v;

				do
				{
					v = (uint32_t) rnd_range(0x800, 0xFFFF);
				} while (v >= 0xD800 && v <= 0xDFFF);
				return v;
			}
		default:
			return (uint32_t) rnd_range(0x10000, 0x10FFFF);
	}
}

/* biased towards ASCII, so most generated terms exercise the common case,
 * with enough multi-byte units mixed in to exercise the rest */
static uint32_t
rnd_cp_mixed(void)
{
	uint32_t	r = rnd(100);

	if (r < 55)
		return rnd_cp_class(0);
	if (r < 75)
		return rnd_cp_class(1);
	if (r < 92)
		return rnd_cp_class(2);
	return rnd_cp_class(3);
}

/* encode a unit array to bytes; returns byte length. buf must be >= 4*n */
static size_t
encode_units(const weave_ul_int32 *u, int n, unsigned char *buf)
{
	size_t		off = 0;
	int			i;

	for (i = 0; i < n; i++)
		off += (size_t) cp_encode((uint32_t) u[i], buf + off);
	return off;
}

/* ---------------------------------------------------------------------------
 * Random unit-array generators and editors, shared by P1/P4/P5.
 * ------------------------------------------------------------------------- */

#define QMAX 24

static int
gen_units(weave_ul_int32 *u, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		u[i] = (weave_ul_int32) rnd_cp_mixed();
	return n;
}

/* apply one random edit (insert/delete/substitute) to u[0..n), capped at
 * `cap` units; returns the new length */
static int
apply_random_edit(weave_ul_int32 *u, int n, int cap)
{
	int			op = (n == 0) ? 0 : (int) rnd(3);

	if (op == 0 && n < cap)
	{
		int			pos = rnd_range(0, n);
		int			i;

		for (i = n; i > pos; i--)
			u[i] = u[i - 1];
		u[pos] = (weave_ul_int32) rnd_cp_mixed();
		return n + 1;
	}
	else if (op == 1 && n > 0)
	{
		int			pos = rnd_range(0, n - 1);
		int			i;

		for (i = pos; i < n - 1; i++)
			u[i] = u[i + 1];
		return n - 1;
	}
	else if (n > 0)
	{
		int			pos = rnd_range(0, n - 1);
		weave_ul_int32 nv;

		do
		{
			nv = (weave_ul_int32) rnd_cp_mixed();
		} while (nv == u[pos]);
		u[pos] = nv;
		return n;
	}
	return n;
}

/* byte-mode analogue: units ARE raw bytes 0..255 */
static int
gen_bytes(weave_ul_int32 *u, int n)
{
	int			i;

	for (i = 0; i < n; i++)
	{
		/* biased towards printable ASCII, with the full byte range mixed in */
		if (rnd(100) < 70)
			u[i] = (weave_ul_int32) rnd_range(0x20, 0x7E);
		else
			u[i] = (weave_ul_int32) rnd_range(0x00, 0xFF);
	}
	return n;
}

static int
apply_random_edit_bytes(weave_ul_int32 *u, int n, int cap)
{
	int			op = (n == 0) ? 0 : (int) rnd(3);

	if (op == 0 && n < cap)
	{
		int			pos = rnd_range(0, n);
		int			i;

		for (i = n; i > pos; i--)
			u[i] = u[i - 1];
		u[pos] = (weave_ul_int32) rnd_range(0x00, 0xFF);
		return n + 1;
	}
	else if (op == 1 && n > 0)
	{
		int			pos = rnd_range(0, n - 1);
		int			i;

		for (i = pos; i < n - 1; i++)
			u[i] = u[i + 1];
		return n - 1;
	}
	else if (n > 0)
	{
		int			pos = rnd_range(0, n - 1);
		weave_ul_int32 nv;

		do
		{
			nv = (weave_ul_int32) rnd_range(0x00, 0xFF);
		} while (nv == u[pos]);
		u[pos] = nv;
		return n;
	}
	return n;
}

static void
bytes_from_units_byte_mode(const weave_ul_int32 *u, int n, unsigned char *buf)
{
	int			i;

	for (i = 0; i < n; i++)
		buf[i] = (unsigned char) u[i];
}

/* ---------------------------------------------------------------------------
 * P1: exactness, both directions
 * ------------------------------------------------------------------------- */

static void
test_exactness(long niter)
{
	long		i;

	for (i = 0; i < niter; i++)
	{
		int			mode = (int) rnd(2) ? WEAVE_ULEVEN_UTF8 : WEAVE_ULEVEN_BYTE;
		int			k = rnd_range(0, 4);
		int			qn = (rnd(100) < 5) ? 0 : rnd_range(1, QMAX);
		weave_ul_int32 qu[QMAX + 8];
		weave_ul_int32 cu[QMAX + 16];
		int			cn;
		int			near = (rnd(100) < 75);
		unsigned char qbuf[4 * (QMAX + 8) + 4];
		unsigned char cbuf[4 * (QMAX + 16) + 4];
		size_t		qlen,
					clen;
		int			naive,
					r,
					dist = -1;
		size_t		dead = (size_t) -1;
		WeaveUlevAut aut;
		WeaveUlevError err;

		if (mode == WEAVE_ULEVEN_UTF8)
			gen_units(qu, qn);
		else
			gen_bytes(qu, qn);

		if (near)
		{
			int			nedits = rnd_range(0, k + 2);
			int			e;

			memcpy(cu, qu, (size_t) qn * sizeof(weave_ul_int32));
			cn = qn;
			for (e = 0; e < nedits; e++)
				cn = (mode == WEAVE_ULEVEN_UTF8)
					? apply_random_edit(cu, cn, QMAX + 16)
					: apply_random_edit_bytes(cu, cn, QMAX + 16);
		}
		else
		{
			cn = (rnd(100) < 5) ? 0 : rnd_range(1, QMAX);
			if (mode == WEAVE_ULEVEN_UTF8)
				gen_units(cu, cn);
			else
				gen_bytes(cu, cn);
		}

		naive = naive_lev(qu, qn, cu, cn);

		if (mode == WEAVE_ULEVEN_UTF8)
		{
			qlen = encode_units(qu, qn, qbuf);
			clen = encode_units(cu, cn, cbuf);
		}
		else
		{
			bytes_from_units_byte_mode(qu, qn, qbuf);
			bytes_from_units_byte_mode(cu, cn, cbuf);
			qlen = (size_t) qn;
			clen = (size_t) cn;
		}

		err = weave_uleven_init(&aut, qbuf, qlen, k, mode);
		CHECKP(1, err == WEAVE_ULEVEN_OK,
			   "init failed unexpectedly: %s", weave_uleven_errstr(err));
		if (err != WEAVE_ULEVEN_OK)
			continue;

		r = weave_uleven_match(&aut, cbuf, clen, &dist, &dead);

		CHECKP(1, (naive <= k) == (r == WEAVE_ULEVEN_MATCH),
			   "mode=%d k=%d qn=%d cn=%d naive=%d r=%d (near=%d)",
			   mode, k, qn, cn, naive, r, near);

		if (r == WEAVE_ULEVEN_MATCH)
			CHECKP(1, dist == naive,
				   "mode=%d k=%d qn=%d cn=%d naive=%d reported dist=%d",
				   mode, k, qn, cn, naive, dist);

		if (r == WEAVE_ULEVEN_DEAD)
			CHECKP(1, dead <= clen,
				   "mode=%d dead prefix %zu exceeds candidate length %zu",
				   mode, dead, clen);
	}
}

/* ---------------------------------------------------------------------------
 * P2/P3: vocabulary expansion -- skip vs no-skip, and vs brute force
 * ------------------------------------------------------------------------- */

#define VOC_MAX_N	400
#define VOC_TERM_UNITS 12
#define VOC_TERM_BYTES (4 * VOC_TERM_UNITS + 4)

typedef struct VocTerm
{
	weave_ul_int32 units[VOC_TERM_UNITS];
	int			nunits;
	unsigned char bytes[VOC_TERM_BYTES];
	size_t		nbytes;
} VocTerm;

static int
term_bytecmp(const VocTerm *a, const VocTerm *b)
{
	size_t		m = a->nbytes < b->nbytes ? a->nbytes : b->nbytes;
	int			c = m ? memcmp(a->bytes, b->bytes, m) : 0;

	if (c != 0)
		return c;
	if (a->nbytes != b->nbytes)
		return (a->nbytes < b->nbytes) ? -1 : 1;
	return 0;
}

static int
term_qsort_cmp(const void *ap, const void *bp)
{
	return term_bytecmp((const VocTerm *) ap, (const VocTerm *) bp);
}

/* WeaveUlevVocab over a sorted array of VocTerm, with a real skip() that
 * bisects to the end of the dead byte-prefix's run. */
typedef struct ArrVoc
{
	VocTerm    *t;
	int			n;
	int			cursor;
	long		skip_calls;
	long		skipped_terms;
} ArrVoc;

static int
arrvoc_has_prefix(const VocTerm *t, const unsigned char *p, weave_ul_uint32 plen)
{
	if (t->nbytes < plen)
		return 0;
	return memcmp(t->bytes, p, plen) == 0;
}

static int
arrvoc_next(void *arg, const char **term, weave_ul_uint32 *len,
			weave_ul_uint32 *ord)
{
	ArrVoc	   *v = (ArrVoc *) arg;

	if (v->cursor >= v->n)
		return 0;
	*term = (const char *) v->t[v->cursor].bytes;
	*len = (weave_ul_uint32) v->t[v->cursor].nbytes;
	*ord = (weave_ul_uint32) v->cursor;
	v->cursor++;
	return 1;
}

/* bisects for the end of the run sharing prefix[0..plen); the vocabulary from
 * `cursor` onward is sorted, and the just-visited term (cursor-1) is known to
 * share this prefix, so the run starts at or before cursor. */
static void
arrvoc_skip(void *arg, const char *prefix, weave_ul_uint32 plen)
{
	ArrVoc	   *v = (ArrVoc *) arg;
	int			lo = v->cursor;
	int			hi = v->n;
	const unsigned char *p = (const unsigned char *) prefix;

	v->skip_calls++;
	/* binary search: first index in [lo,hi) that does NOT share the prefix */
	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (arrvoc_has_prefix(&v->t[mid], p, plen))
			lo = mid + 1;
		else
			hi = mid;
	}
	v->skipped_terms += (lo - v->cursor);
	v->cursor = lo;
}

typedef struct HitRec
{
	int			dist;			/* -1 if not a hit */
} HitRec;

typedef struct HitColl
{
	HitRec	   *rec;			/* indexed by ord, size n */
	weave_ul_uint32 nhits;
} HitColl;

static int
hitcoll_cb(void *arg, const char *term, weave_ul_uint32 len,
		   weave_ul_uint32 ord, int dist)
{
	HitColl    *c = (HitColl *) arg;

	(void) term;
	(void) len;
	c->rec[ord].dist = dist;
	c->nhits++;
	return 0;
}

static void
voc_gen_term(VocTerm *t, int mode)
{
	if (mode == WEAVE_ULEVEN_UTF8)
	{
		t->nunits = rnd_range(0, VOC_TERM_UNITS);
		gen_units(t->units, t->nunits);
		t->nbytes = encode_units(t->units, t->nunits, t->bytes);
	}
	else
	{
		t->nunits = rnd_range(0, VOC_TERM_UNITS);
		gen_bytes(t->units, t->nunits);
		bytes_from_units_byte_mode(t->units, t->nunits, t->bytes);
		t->nbytes = (size_t) t->nunits;
	}
}

static void
test_vocab_expansion(long ntrials)
{
	long		trial;
	int			any_skip_reduced = 0;

	for (trial = 0; trial < ntrials; trial++)
	{
		int			mode = (int) rnd(2) ? WEAVE_ULEVEN_UTF8 : WEAVE_ULEVEN_BYTE;
		int			vn = rnd_range(1, VOC_MAX_N);
		static VocTerm voc[VOC_MAX_N];
		int			i;
		int			k = rnd_range(0, 4);
		weave_ul_int32 qu[VOC_TERM_UNITS + 8];
		int			qn;
		unsigned char qbuf[4 * (VOC_TERM_UNITS + 8) + 4];
		size_t		qlen;
		WeaveUlevAut aut;
		WeaveUlevError err;
		HitRec	   *recs_skip = malloc((size_t) vn * sizeof(HitRec));
		HitRec	   *recs_noskip = malloc((size_t) vn * sizeof(HitRec));
		HitColl		cs,
					cn2;
		ArrVoc		av;
		WeaveUlevVocab wv;
		weave_ul_uint32 nhits_s = 0,
					nvis_s = 0,
					nhits_n = 0,
					nvis_n = 0;

		for (i = 0; i < vn; i++)
			voc_gen_term(&voc[i], mode);
		qsort(voc, (size_t) vn, sizeof(VocTerm), term_qsort_cmp);

		/* query: derive from a random vocab term plus edits half the time, to
		 * get a mix of trials with many hits and trials with none */
		if (rnd(2) && vn > 0)
		{
			int			pick = (int) rnd((uint32_t) vn);
			int			e,
						nedits = rnd_range(0, k + 2);

			qn = voc[pick].nunits;
			memcpy(qu, voc[pick].units, (size_t) qn * sizeof(weave_ul_int32));
			for (e = 0; e < nedits; e++)
				qn = (mode == WEAVE_ULEVEN_UTF8)
					? apply_random_edit(qu, qn, VOC_TERM_UNITS + 8)
					: apply_random_edit_bytes(qu, qn, VOC_TERM_UNITS + 8);
		}
		else
		{
			qn = rnd_range(0, VOC_TERM_UNITS);
			if (mode == WEAVE_ULEVEN_UTF8)
				gen_units(qu, qn);
			else
				gen_bytes(qu, qn);
		}

		if (mode == WEAVE_ULEVEN_UTF8)
			qlen = encode_units(qu, qn, qbuf);
		else
		{
			bytes_from_units_byte_mode(qu, qn, qbuf);
			qlen = (size_t) qn;
		}

		err = weave_uleven_init(&aut, qbuf, qlen, k, mode);
		CHECKP(2, err == WEAVE_ULEVEN_OK, "vocab trial init failed: %s",
			   weave_uleven_errstr(err));
		if (err != WEAVE_ULEVEN_OK)
		{
			free(recs_skip);
			free(recs_noskip);
			continue;
		}

		for (i = 0; i < vn; i++)
			recs_skip[i].dist = -1;
		for (i = 0; i < vn; i++)
			recs_noskip[i].dist = -1;

		/* run 1: real skip */
		av.t = voc;
		av.n = vn;
		av.cursor = 0;
		av.skip_calls = 0;
		av.skipped_terms = 0;
		wv.next = arrvoc_next;
		wv.skip = arrvoc_skip;
		wv.arg = &av;
		cs.rec = recs_skip;
		cs.nhits = 0;
		err = weave_uleven_expand_vocab(&aut, &wv, hitcoll_cb, &cs,
										 &nhits_s, &nvis_s);
		CHECKP(2, err == WEAVE_ULEVEN_OK, "expand (skip) returned %s",
			   weave_uleven_errstr(err));

		/* run 2: skip == NULL */
		av.cursor = 0;
		wv.skip = NULL;
		cn2.rec = recs_noskip;
		cn2.nhits = 0;
		err = weave_uleven_expand_vocab(&aut, &wv, hitcoll_cb, &cn2,
										 &nhits_n, &nvis_n);
		CHECKP(2, err == WEAVE_ULEVEN_OK, "expand (no skip) returned %s",
			   weave_uleven_errstr(err));

		CHECKP(2, nhits_s == cs.nhits && nhits_n == cn2.nhits,
			   "expand_vocab's *nhits disagrees with the callback count");
		CHECKP(2, nvis_s <= (weave_ul_uint32) vn && nvis_n == (weave_ul_uint32) vn,
			   "no-skip run must visit every term (vn=%d nvis_n=%u)",
			   vn, nvis_n);
		if (nvis_s < nvis_n)
			any_skip_reduced = 1;

		CHECKP(2, nhits_s == nhits_n,
			   "skip vs no-skip hit COUNT differs: %u vs %u (mode=%d vn=%d k=%d)",
			   nhits_s, nhits_n, mode, vn, k);

		for (i = 0; i < vn; i++)
		{
			CHECKP(2, recs_skip[i].dist == recs_noskip[i].dist,
				   "skip vs no-skip disagree on ord %d: dist %d vs %d "
				   "(mode=%d vn=%d k=%d) -- a dead-prefix that is too greedy "
				   "drops this term silently",
				   i, recs_skip[i].dist, recs_noskip[i].dist, mode, vn, k);

			/* P3: agree with the independent brute-force oracle */
			{
				int			naive = naive_lev(qu, qn, voc[i].units, voc[i].nunits);
				int			is_hit = (recs_noskip[i].dist >= 0);

				CHECKP(3, (naive <= k) == is_hit,
					   "vocab ord %d: naive=%d k=%d but expand reports hit=%d "
					   "(mode=%d vn=%d)", i, naive, k, is_hit, mode, vn);
				if (is_hit)
					CHECKP(3, recs_noskip[i].dist == naive,
						   "vocab ord %d: naive=%d but reported dist=%d",
						   i, naive, recs_noskip[i].dist);
			}
		}

		free(recs_skip);
		free(recs_noskip);
	}

	CHECKP(2, any_skip_reduced,
		   "skip() never reduced visited count over %ld trials -- "
		   "the skip hook was never actually exercised", ntrials);
}

/* ---------------------------------------------------------------------------
 * P4: the edit unit is the character, not the byte
 * ------------------------------------------------------------------------- */

static int
cp_byte_len(uint32_t cp)
{
	if (cp <= 0x7F)
		return 1;
	if (cp <= 0x7FF)
		return 2;
	if (cp <= 0xFFFF)
		return 3;
	return 4;
}

static void
bytes_to_units(const unsigned char *b, size_t n, weave_ul_int32 *out)
{
	size_t		i;

	for (i = 0; i < n; i++)
		out[i] = (weave_ul_int32) b[i];
}

#define CVB_N 14

static void
test_char_vs_byte(long niter)
{
	long		i;

	for (i = 0; i < niter; i++)
	{
		weave_ul_int32 base[CVB_N];
		weave_ul_int32 cand[CVB_N];
		int			n = rnd_range(3, CVB_N);
		int			pos = rnd_range(0, n - 1);
		int			oldcls,
					newcls;
		unsigned char qbuf[4 * CVB_N + 4];
		unsigned char cbuf[4 * CVB_N + 4];
		size_t		qlen,
					clen;
		weave_ul_int32 qbytes_as_units[4 * CVB_N];
		weave_ul_int32 cbytes_as_units[4 * CVB_N];
		int			naive_char,
					naive_byte;
		WeaveUlevAut aut;
		WeaveUlevError err;
		int			r,
					dist_utf8 = -1,
					dist_byte = -1;
		const int	k = 10;

		gen_units(base, n);
		oldcls = cp_byte_len((uint32_t) base[pos]) - 1;
		newcls = (oldcls + rnd_range(1, 3)) % 4;

		memcpy(cand, base, (size_t) n * sizeof(weave_ul_int32));
		cand[pos] = (weave_ul_int32) rnd_cp_class(newcls);

		qlen = encode_units(base, n, qbuf);
		clen = encode_units(cand, n, cbuf);

		naive_char = naive_lev(base, n, cand, n);
		CHECKP(4, naive_char == 1,
			   "sanity: a single-unit substitution must be Levenshtein "
			   "distance 1, got %d", naive_char);

		bytes_to_units(qbuf, qlen, qbytes_as_units);
		bytes_to_units(cbuf, clen, cbytes_as_units);
		naive_byte = naive_lev(qbytes_as_units, (int) qlen,
								cbytes_as_units, (int) clen);

		err = weave_uleven_init(&aut, qbuf, qlen, k, WEAVE_ULEVEN_UTF8);
		CHECKP(4, err == WEAVE_ULEVEN_OK, "utf8 init failed: %s",
			   weave_uleven_errstr(err));
		r = weave_uleven_match(&aut, cbuf, clen, &dist_utf8, NULL);
		CHECKP(4, r == WEAVE_ULEVEN_MATCH && dist_utf8 == naive_char,
			   "utf8 mode: expected dist %d, r=%d dist=%d", naive_char, r,
			   dist_utf8);

		err = weave_uleven_init(&aut, qbuf, qlen, k, WEAVE_ULEVEN_BYTE);
		CHECKP(4, err == WEAVE_ULEVEN_OK, "byte init failed: %s",
			   weave_uleven_errstr(err));
		r = weave_uleven_match(&aut, cbuf, clen, &dist_byte, NULL);
		CHECKP(4, r == WEAVE_ULEVEN_MATCH && dist_byte == naive_byte,
			   "byte mode: expected dist %d, r=%d dist=%d", naive_byte, r,
			   dist_byte);

		CHECKP(4, dist_byte != dist_utf8,
			   "byte and utf8 modes must genuinely differ on the same input: "
			   "byte=%d utf8=%d (oldcls=%d newcls=%d)", dist_byte, dist_utf8,
			   oldcls, newcls);
	}
}

/* ---------------------------------------------------------------------------
 * P5: not Damerau -- an adjacent transposition costs 2
 * ------------------------------------------------------------------------- */

#define TR_N 16

static void
test_transposition(long niter)
{
	long		i;

	for (i = 0; i < niter; i++)
	{
		int			mode = (int) rnd(2) ? WEAVE_ULEVEN_UTF8 : WEAVE_ULEVEN_BYTE;
		weave_ul_int32 u[TR_N];
		weave_ul_int32 c[TR_N];
		int			n = rnd_range(2, TR_N);
		int			pos;
		int			tries;
		unsigned char qbuf[4 * TR_N + 4];
		unsigned char cbuf[4 * TR_N + 4];
		size_t		qlen,
					clen;
		int			naive;
		WeaveUlevAut aut;
		WeaveUlevError err;
		int			r,
					dist = -1;

		if (mode == WEAVE_ULEVEN_UTF8)
			gen_units(u, n);
		else
			gen_bytes(u, n);

		/* pick an adjacent pair of DISTINCT units to transpose; regenerate
		 * the pair if the draw collides, bounded retries */
		pos = rnd_range(0, n - 2);
		for (tries = 0; tries < 50 && u[pos] == u[pos + 1]; tries++)
		{
			if (mode == WEAVE_ULEVEN_UTF8)
				u[pos + 1] = (weave_ul_int32) rnd_cp_mixed();
			else
				u[pos + 1] = (weave_ul_int32) rnd_range(0x00, 0xFF);
		}
		if (u[pos] == u[pos + 1])
			continue;			/* pathological draw; skip rather than lie */

		memcpy(c, u, (size_t) n * sizeof(weave_ul_int32));
		c[pos] = u[pos + 1];
		c[pos + 1] = u[pos];

		naive = naive_lev(u, n, c, n);
		CHECKP(5, naive == 2,
			   "sanity: adjacent transposition of distinct units must be "
			   "distance 2, oracle says %d", naive);

		if (mode == WEAVE_ULEVEN_UTF8)
		{
			qlen = encode_units(u, n, qbuf);
			clen = encode_units(c, n, cbuf);
		}
		else
		{
			bytes_from_units_byte_mode(u, n, qbuf);
			bytes_from_units_byte_mode(c, n, cbuf);
			qlen = (size_t) n;
			clen = (size_t) n;
		}

		err = weave_uleven_init(&aut, qbuf, qlen, 1, mode);
		CHECKP(5, err == WEAVE_ULEVEN_OK, "k=1 init failed: %s",
			   weave_uleven_errstr(err));
		r = weave_uleven_match(&aut, cbuf, clen, &dist, NULL);
		CHECKP(5, r != WEAVE_ULEVEN_MATCH,
			   "k=1 must NOT match a transposition (Damerau would): r=%d",
			   r);

		err = weave_uleven_init(&aut, qbuf, qlen, 2, mode);
		CHECKP(5, err == WEAVE_ULEVEN_OK, "k=2 init failed: %s",
			   weave_uleven_errstr(err));
		r = weave_uleven_match(&aut, cbuf, clen, &dist, NULL);
		CHECKP(5, r == WEAVE_ULEVEN_MATCH && dist == 2,
			   "k=2 must match the transposition at exactly distance 2: r=%d "
			   "dist=%d", r, dist);
	}
}

static void
test_transposition_directed(void)
{
	WeaveUlevAut aut;
	WeaveUlevError err;
	int			r,
				dist = -1;

	err = weave_uleven_init(&aut, "the", 3, 1, WEAVE_ULEVEN_BYTE);
	CHECKP(5, err == WEAVE_ULEVEN_OK, "directed: init(\"the\",k=1) failed: %s",
		   weave_uleven_errstr(err));
	r = weave_uleven_match(&aut, "hte", 3, &dist, NULL);
	CHECKP(5, r != WEAVE_ULEVEN_MATCH,
		   "directed: \"the\"~1 must not match \"hte\" (r=%d)", r);

	err = weave_uleven_init(&aut, "the", 3, 2, WEAVE_ULEVEN_BYTE);
	CHECKP(5, err == WEAVE_ULEVEN_OK, "directed: init(\"the\",k=2) failed: %s",
		   weave_uleven_errstr(err));
	r = weave_uleven_match(&aut, "hte", 3, &dist, NULL);
	CHECKP(5, r == WEAVE_ULEVEN_MATCH && dist == 2,
		   "directed: dist(\"the\",\"hte\") must be exactly 2, got r=%d dist=%d",
		   r, dist);
}

/* ---------------------------------------------------------------------------
 * P6: malformed bytes decode injectively
 * ------------------------------------------------------------------------- */

#define POOL_TERMLEN 48

typedef struct BStr
{
	unsigned char *b;
	size_t		n;
} BStr;

static size_t
gen_piece(unsigned char *buf)
{
	int			kind = rnd_range(0, 10);

	switch (kind)
	{
		case 0:
			buf[0] = (unsigned char) rnd_range(0x20, 0x7E);
			return 1;
		case 1:
			return (size_t) cp_encode(rnd_cp_class(1), buf);
		case 2:
			return (size_t) cp_encode(rnd_cp_class(2), buf);
		case 3:
			return (size_t) cp_encode(rnd_cp_class(3), buf);
		case 4:				/* stray continuation byte */
			buf[0] = (unsigned char) rnd_range(0x80, 0xBF);
			return 1;
		case 5:				/* overlong 2-byte lead (0xC0/0xC1) + cont */
			buf[0] = (unsigned char) (rnd(2) ? 0xC0 : 0xC1);
			buf[1] = (unsigned char) rnd_range(0x80, 0xBF);
			return 2;
		case 6:				/* encoded surrogate: ED A0-BF xx */
			buf[0] = 0xED;
			buf[1] = (unsigned char) rnd_range(0xA0, 0xBF);
			buf[2] = (unsigned char) rnd_range(0x80, 0xBF);
			return 3;
		case 7:				/* 5/6-byte lead (F5-FF), never valid */
			buf[0] = (unsigned char) rnd_range(0xF5, 0xFF);
			buf[1] = (unsigned char) rnd_range(0x00, 0xFF);
			return 2;
		case 8:				/* lone valid 2-byte lead, no continuation */
			buf[0] = (unsigned char) rnd_range(0xC2, 0xDF);
			return 1;
		case 9:				/* 3-byte lead + 1 continuation, missing 1 */
			buf[0] = (unsigned char) rnd_range(0xE0, 0xEF);
			buf[1] = (unsigned char) rnd_range(0x80, 0xBF);
			return 2;
		default:				/* valid lead followed by a non-continuation byte */
			buf[0] = (unsigned char) rnd_range(0xC2, 0xDF);
			buf[1] = (unsigned char) rnd_range(0x20, 0x7E);
			return 2;
	}
}

/* one term: 1-4 random pieces, occasionally ending in a lead byte truncated
 * by the end of the buffer (no bytes at all follow it) */
static size_t
gen_malformed_term(unsigned char *buf, size_t maxlen)
{
	size_t		off = 0;
	int			npieces = rnd_range(1, 4);
	int			i;
	unsigned char tmp[8];

	for (i = 0; i < npieces; i++)
	{
		size_t		plen = gen_piece(tmp);

		if (off + plen > maxlen)
			break;
		memcpy(buf + off, tmp, plen);
		off += plen;
	}

	if (rnd(3) == 0)
	{
		int			tk = rnd_range(0, 2);

		if (tk == 0 && off + 1 <= maxlen)
			buf[off++] = (unsigned char) rnd_range(0xC2, 0xDF);
		else if (tk == 1 && off + 2 <= maxlen)
		{
			buf[off++] = (unsigned char) rnd_range(0xE0, 0xEF);
			buf[off++] = (unsigned char) rnd_range(0x80, 0xBF);
		}
		else if (off + 3 <= maxlen)
		{
			buf[off++] = (unsigned char) rnd_range(0xF0, 0xF4);
			buf[off++] = (unsigned char) rnd_range(0x80, 0xBF);
			buf[off++] = (unsigned char) rnd_range(0x80, 0xBF);
		}
	}

	return off;
}

static int
bstr_eq(const BStr *a, const BStr *b)
{
	if (a->n != b->n)
		return 0;
	return a->n == 0 || memcmp(a->b, b->b, a->n) == 0;
}

static int
build_pool(BStr *pool, int maxpool, int rawcount)
{
	int			cnt = 0;
	int			i,
				j;

	for (i = 0; i < rawcount && cnt < maxpool; i++)
	{
		unsigned char tmp[POOL_TERMLEN];
		size_t		n = gen_malformed_term(tmp, sizeof tmp);
		BStr		cand;
		int			dup = 0;

		cand.b = tmp;
		cand.n = n;
		for (j = 0; j < cnt; j++)
		{
			if (bstr_eq(&pool[j], &cand))
			{
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;

		pool[cnt].b = (unsigned char *) malloc(n ? n : 1);
		if (n)
			memcpy(pool[cnt].b, tmp, n);
		pool[cnt].n = n;
		cnt++;
	}
	return cnt;
}

/*
 * Directed collision case: a lone truncated 2-byte lead 0xC2 escapes to the
 * pseudo-unit 0xDC00+0xC2 (per the header's stated rule).  0xC2 itself, as a
 * plain codepoint, is exactly the codepoint that the WELL-FORMED 2-byte
 * sequence {0xC3,0x82} decodes to.  If the 0xDC00 offset were ever dropped
 * (i.e. escaping to the raw byte value instead of the pseudo-unit) these two
 * completely different byte strings would decode to the same unit and
 * collide -- this pins that down deterministically rather than leaving it to
 * the random pool's luck.
 */
static void
test_injective_directed(void)
{
	static const unsigned char a[] = {0xC2};	/* truncated 2-byte lead */
	static const unsigned char b[] = {0xC3, 0x82}; /* valid encoding of U+00C2 */
	WeaveUlevAut aut;
	WeaveUlevError err;
	int			r,
				dist = -1;

	err = weave_uleven_init(&aut, a, sizeof a, 4, WEAVE_ULEVEN_UTF8);
	CHECKP(6, err == WEAVE_ULEVEN_OK, "directed collision: init failed: %s",
		   weave_uleven_errstr(err));
	r = weave_uleven_match(&aut, b, sizeof b, &dist, NULL);
	CHECKP(6, r == WEAVE_ULEVEN_MATCH,
		   "directed collision: expected a MATCH verdict (k=4 is generous), "
		   "got r=%d", r);
	CHECKP(6, dist > 0,
		   "directed collision: a truncated 2-byte lead (0xC2) and the "
		   "valid 2-byte encoding of U+00C2 ({0xC3,0x82}) are different "
		   "byte strings and must NOT decode to the same unit, but "
		   "dist=%d", dist);
}

static void
test_malformed_injective(int maxpool, int rawcount)
{
	BStr	   *pool = (BStr *) malloc((size_t) maxpool * sizeof(BStr));
	int			poolsize = build_pool(pool, maxpool, rawcount);
	int			i,
				j;

	printf("  malformed-decode pool: %d unique terms from %d raw draws\n",
		   poolsize, rawcount);

	for (i = 0; i < poolsize; i++)
	{
		WeaveUlevAut aut;
		WeaveUlevError err;
		int			k = 2 * POOL_TERMLEN;

		if (k > WEAVE_ULEVEN_MAX_K)
			k = WEAVE_ULEVEN_MAX_K;

		err = weave_uleven_init(&aut, pool[i].b, pool[i].n, k,
								 WEAVE_ULEVEN_UTF8);
		CHECKP(6, err == WEAVE_ULEVEN_OK, "pool[%d] (len %zu) init failed: %s",
			   i, pool[i].n, weave_uleven_errstr(err));
		if (err != WEAVE_ULEVEN_OK)
			continue;

		for (j = i; j < poolsize; j++)
		{
			int			dist = -1;
			int			r = weave_uleven_match(&aut, pool[j].b, pool[j].n,
												   &dist, NULL);

			CHECKP(6, r == WEAVE_ULEVEN_MATCH,
				   "pool[%d] vs pool[%d]: k=%d should be large enough for a "
				   "MATCH verdict but got r=%d", i, j, k, r);
			if (r != WEAVE_ULEVEN_MATCH)
				continue;

			if (i == j)
				CHECKP(6, dist == 0,
					   "dist(t,t) != 0 for pool[%d] (len %zu): dist=%d", i,
					   pool[i].n, dist);
			else
				CHECKP(6, dist > 0,
					   "decode is NOT injective: distinct byte strings "
					   "pool[%d] (len %zu) and pool[%d] (len %zu) compare "
					   "at distance %d", i, pool[i].n, j, pool[j].n, dist);
		}
	}

	for (i = 0; i < poolsize; i++)
		free(pool[i].b);
	free(pool);
}

/* ---------------------------------------------------------------------------
 * P7: input validation
 * ------------------------------------------------------------------------- */

static int
stop_after_first_cb(void *arg, const char *term, weave_ul_uint32 len,
					 weave_ul_uint32 ord, int dist)
{
	int		   *count = (int *) arg;

	(void) term;
	(void) len;
	(void) ord;
	(void) dist;
	(*count)++;
	return 1;					/* ask to stop */
}

static void
test_input_validation(void)
{
	WeaveUlevAut aut;
	WeaveUlevError err;
	unsigned char big[WEAVE_ULEVEN_MAX_UNITS + 8];
	unsigned char big_utf8[2 * (WEAVE_ULEVEN_MAX_UNITS + 8)];
	size_t		i;

	memset(big, 'x', sizeof big);

	/* k out of range */
	err = weave_uleven_init(&aut, "abc", 3, -1, WEAVE_ULEVEN_BYTE);
	CHECK(err == WEAVE_ULEVEN_K_RANGE, "k=-1 should be K_RANGE, got %s",
		  weave_uleven_errstr(err));

	err = weave_uleven_init(&aut, "abc", 3, WEAVE_ULEVEN_MAX_K + 1,
							 WEAVE_ULEVEN_BYTE);
	CHECK(err == WEAVE_ULEVEN_K_RANGE,
		  "k=MAX_K+1 should be K_RANGE, got %s", weave_uleven_errstr(err));

	err = weave_uleven_init(&aut, "abc", 3, 0, WEAVE_ULEVEN_BYTE);
	CHECK(err == WEAVE_ULEVEN_OK, "k=0 should be OK, got %s",
		  weave_uleven_errstr(err));

	err = weave_uleven_init(&aut, "abc", 3, WEAVE_ULEVEN_MAX_K,
							 WEAVE_ULEVEN_BYTE);
	CHECK(err == WEAVE_ULEVEN_OK, "k=MAX_K should be OK, got %s",
		  weave_uleven_errstr(err));

	/* unit mode out of range */
	err = weave_uleven_init(&aut, "abc", 3, 0, 2);
	CHECK(err == WEAVE_ULEVEN_UNIT_MODE, "unit=2 should be UNIT_MODE, got %s",
		  weave_uleven_errstr(err));
	err = weave_uleven_init(&aut, "abc", 3, 0, -1);
	CHECK(err == WEAVE_ULEVEN_UNIT_MODE, "unit=-1 should be UNIT_MODE, got %s",
		  weave_uleven_errstr(err));

	/* query length in BYTE mode: exactly at the boundary is fine, one over
	 * is refused -- and refused, not truncated (a truncated query would
	 * silently accept a different language, per the header's own comment) */
	err = weave_uleven_init(&aut, big, WEAVE_ULEVEN_MAX_UNITS, 0,
							 WEAVE_ULEVEN_BYTE);
	CHECK(err == WEAVE_ULEVEN_OK,
		  "query of exactly MAX_UNITS bytes should be OK, got %s",
		  weave_uleven_errstr(err));
	CHECK(aut.m == WEAVE_ULEVEN_MAX_UNITS,
		  "accepted query should record m == MAX_UNITS, got %d", aut.m);

	err = weave_uleven_init(&aut, big, WEAVE_ULEVEN_MAX_UNITS + 1, 0,
							 WEAVE_ULEVEN_BYTE);
	CHECK(err == WEAVE_ULEVEN_QUERY_TOO_LONG,
		  "query of MAX_UNITS+1 bytes should be QUERY_TOO_LONG, got %s",
		  weave_uleven_errstr(err));

	/* the bound is on UNITS, not bytes: build a UTF-8 query that has more
	 * than MAX_UNITS *bytes* but exactly MAX_UNITS *characters* (must be
	 * accepted), and one with MAX_UNITS+1 characters (must be refused) even
	 * though its byte length is unremarkable */
	for (i = 0; i < WEAVE_ULEVEN_MAX_UNITS; i++)
	{
		big_utf8[2 * i] = 0xC2;	/* U+00A2, cent sign, 2 bytes */
		big_utf8[2 * i + 1] = 0xA2;
	}
	err = weave_uleven_init(&aut, big_utf8, 2 * WEAVE_ULEVEN_MAX_UNITS, 0,
							 WEAVE_ULEVEN_UTF8);
	CHECK(err == WEAVE_ULEVEN_OK,
		  "MAX_UNITS 2-byte characters (2*MAX_UNITS bytes) should be OK, "
		  "got %s", weave_uleven_errstr(err));
	CHECK(aut.m == WEAVE_ULEVEN_MAX_UNITS,
		  "should decode to exactly MAX_UNITS units, got %d", aut.m);

	err = weave_uleven_init(&aut, big_utf8, 2 * (WEAVE_ULEVEN_MAX_UNITS + 1),
							 0, WEAVE_ULEVEN_UTF8);
	CHECK(err == WEAVE_ULEVEN_QUERY_TOO_LONG,
		  "MAX_UNITS+1 2-byte characters should be QUERY_TOO_LONG by unit "
		  "count even though byte length is unremarkable, got %s",
		  weave_uleven_errstr(err));

	/* no iterator */
	{
		WeaveUlevError r;

		err = weave_uleven_init(&aut, "abc", 3, 0, WEAVE_ULEVEN_BYTE);
		CHECK(err == WEAVE_ULEVEN_OK, "setup init failed: %s",
			  weave_uleven_errstr(err));

		r = weave_uleven_expand_vocab(&aut, NULL, NULL, NULL, NULL, NULL);
		CHECK(r == WEAVE_ULEVEN_NO_ITER, "voc==NULL should be NO_ITER, got %s",
			  weave_uleven_errstr(r));

		{
			WeaveUlevVocab wv;

			wv.next = NULL;
			wv.skip = NULL;
			wv.arg = NULL;
			r = weave_uleven_expand_vocab(&aut, &wv, NULL, NULL, NULL, NULL);
			CHECK(r == WEAVE_ULEVEN_NO_ITER,
				  "voc->next==NULL should be NO_ITER, got %s",
				  weave_uleven_errstr(r));
		}
	}

	/* a callback that asks to stop actually stops the walk */
	{
		VocTerm		voc[5];
		int			i2;
		ArrVoc		av;
		WeaveUlevVocab wv;
		int			cbcount = 0;
		weave_ul_uint32 nhits = 0,
					nvisited = 0;
		WeaveUlevError r;

		for (i2 = 0; i2 < 5; i2++)
		{
			voc[i2].nunits = 1;
			voc[i2].units[0] = 'a';
			voc[i2].bytes[0] = 'a';
			voc[i2].nbytes = 1;
		}
		av.t = voc;
		av.n = 5;
		av.cursor = 0;
		av.skip_calls = 0;
		av.skipped_terms = 0;
		wv.next = arrvoc_next;
		wv.skip = NULL;
		wv.arg = &av;

		err = weave_uleven_init(&aut, "a", 1, 0, WEAVE_ULEVEN_BYTE);
		CHECK(err == WEAVE_ULEVEN_OK, "setup init(\"a\") failed: %s",
			  weave_uleven_errstr(err));
		r = weave_uleven_expand_vocab(&aut, &wv, stop_after_first_cb, &cbcount,
									   &nhits, &nvisited);
		CHECK(r == WEAVE_ULEVEN_STOPPED,
			  "a callback returning nonzero should yield STOPPED, got %s",
			  weave_uleven_errstr(r));
		CHECK(cbcount == 1, "callback should have run exactly once, ran %d",
			  cbcount);
		CHECK(nhits == 1, "*nhits should count the one emitted hit, got %u",
			  nhits);
		CHECK(nvisited < 5,
			  "stopping should leave the rest of the vocabulary unvisited, "
			  "nvisited=%u", nvisited);
	}
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
	if (argc > 1)
		rng_state = (uint64_t) strtoull(argv[1], NULL, 0) | 1;	/* never 0 */

	printf("uleven: Z5 Levenshtein neighbourhood core -- exactness, skip "
		   "soundness, UTF-8 char units, non-Damerau, injective decode, "
		   "input validation\n");

	test_input_validation();
	test_transposition_directed();

	test_exactness(600000);
	test_vocab_expansion(3500);
	test_char_vs_byte(250000);
	test_transposition(150000);
	test_injective_directed();
	test_malformed_injective(1400, 4000);

	printf("P1 exactness            : %8ld checks\n", prop_checks[1]);
	printf("P2 skip == no-skip       : %8ld checks\n", prop_checks[2]);
	printf("P3 expand == brute force : %8ld checks\n", prop_checks[3]);
	printf("P4 char unit != byte unit: %8ld checks\n", prop_checks[4]);
	printf("P5 not Damerau           : %8ld checks\n", prop_checks[5]);
	printf("P6 injective decode      : %8ld checks\n", prop_checks[6]);
	printf("P7 input validation      : %8ld checks\n", prop_checks[7]);

	if (failures)
	{
		printf("FAILED -- %ld checks, %ld failures (first at check %ld, "
			   "property P%d check %ld)\n", checks, failures, first_fail_at,
			   first_fail_prop, first_fail_prop_at);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
