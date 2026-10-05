/*-------------------------------------------------------------------------
 *
 * doclist.h
 *		The per-bolt DOCUMENT LIST (format v12): every docid a bolt holds,
 *		posting or not, plus the subset whose lexical column was NULL.
 *
 * doc/specs/SEGMENT_FORMAT.md sect. 6 "The document list" is the spec.  Before
 * v12 the only enumeration of a bolt's documents was its posting lists, so a
 * document with no posting -- a zero-term wdoc, and since v12 a NULL-document
 * row -- was invisible to VACUUM (doc/GAPS.md G80), to the NOT universe (G78)
 * and, for the NULL-document case, never indexed at all (G77).
 *
 * BACKEND-INDEPENDENT on purpose, for the reason include/weave/docvals.h is:
 * the validator must be fuzzable (test/fuzz/fuzz_doclist.c) and property-tested
 * (test/hegel/test_doclist.c) without a server.  It depends on the vendored
 * sparsemap only, and in the backend that is src/util/sparsemap.c.
 *
 * Image layout (one contiguous byte string on a WEAVE_PK_DOCLIST page chain):
 *
 *	 [0, 40)                        WeaveDocListHeader
 *	 [40, 40 + alllen)              ALL: the raw sparsemap buffer (sm_get_data /
 *	                                sm_get_size -- the livedocs blob's encoding)
 *	 [40 + MAXALIGN8(alllen), +nulllen)  NULL: the same, for NULL documents;
 *	                                absent (nulllen == 0) when nnull == 0
 *
 * total length == 40 + MAXALIGN8(alllen) + nulllen, EXACTLY: a chain whose
 * payload length disagrees is torn or foreign and is refused, which is how a
 * torn chain is caught with no second length field (the SuRF reasoning).
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_DOCLIST_H
#define WEAVE_DOCLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "weave/sparsemap.h"

#define WEAVE_DOCLIST_MAGIC		0x57444c31u	/* "WDL1" little-endian */
#define WEAVE_DOCLIST_VERSION	1

/*
 * COMPLETE: the list was produced by a v12 writer from the heap or from
 * v12 pending items, so ALL is every row the bolt indexes, NULL-document rows
 * included.  A merge output carries it only if every input did.  Unknown flag
 * bits are an error, not ignored (SEGMENT_FORMAT.md sect. 8 item 2).
 */
#define WEAVE_DOCLIST_F_COMPLETE	0x0001u
#define WEAVE_DOCLIST_F_ALL			0x0001u

typedef struct WeaveDocListHeader
{
	uint32_t	magic;
	uint16_t	version;
	uint16_t	flags;
	uint64_t	ndocs;			/* cardinality of ALL; >= 1 */
	uint64_t	nnull;			/* cardinality of NULL; <= ndocs */
	uint32_t	alllen;			/* raw sparsemap bytes of ALL */
	uint32_t	nulllen;		/* raw sparsemap bytes of NULL; 0 iff nnull == 0 */
	uint32_t	reserved;		/* must read as zero */
	uint32_t	reserved2;		/* must read as zero */
} WeaveDocListHeader;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(WeaveDocListHeader) == 40,
			   "WeaveDocListHeader on-disk layout must be 40 bytes");
#endif

#define WEAVE_DOCLIST_HDRSIZE	40u
#define WEAVE_DOCLIST_ALIGN8(x)	(((x) + (size_t) 7) & ~(size_t) 7)

static inline size_t
weave_doclist_null_off(uint32_t alllen)
{
	return WEAVE_DOCLIST_HDRSIZE + WEAVE_DOCLIST_ALIGN8((size_t) alllen);
}

static inline size_t
weave_doclist_image_len(uint32_t alllen, uint32_t nulllen)
{
	return weave_doclist_null_off(alllen) + (size_t) nulllen;
}

/*
 * Walk one raw sparsemap of `len` bytes at `src` with sm_next_member(),
 * calling `emit` per member (may be NULL).  Returns the member count, or
 * UINT64_MAX when `limit` members have already been seen and another arrives.
 *
 * ONE WALKER FOR THE VALIDATOR AND THE DECODER, and that is the point: the
 * vendored sparsemap's sm_cardinality() and its sm_next_member() iteration can
 * DISAGREE on a buffer sm_validate() accepts (test/hegel/test_doclist.c found a
 * single flipped byte giving cardinality 1973 and a walk of 1909).  A validator
 * that counted with one and a decoder that iterated with the other would accept
 * an image the decoder then cannot honour.  Counting with the iteration the
 * decoder uses makes "validated" and "decodable" the same predicate.
 *
 * The map is opened over a SCRATCH COPY: sm_open() of a structurally invalid
 * buffer rewrites its first word to make the map empty, and a validator must
 * not write to the bytes it validates.
 */
typedef struct WeaveDocListWalk
{
	uint8_t    *buf;
	sm_t		m;
	sm_cursor_t cur;
	uint64_t	last;
	bool		started;
} WeaveDocListWalk;

static inline bool
weave_doclist_walk_open(WeaveDocListWalk *w, const uint8_t *src, size_t len)
{
	w->buf = (uint8_t *) malloc(len > 0 ? len : 1);
	if (w->buf == NULL)
		return false;
	memcpy(w->buf, src, len);
	sm_open(&w->m, w->buf, len);
	w->cur = (sm_cursor_t) SM_CURSOR_INIT;
	w->started = false;
	w->last = 0;
	return true;
}

/* Next member, strictly ascending, or false at the end (or on a non-ascending
 * member, which a sane iterator never yields but a hostile buffer might). */
static inline bool
weave_doclist_walk_next(WeaveDocListWalk *w, uint64_t *out, bool *bad)
{
	uint64_t	v = sm_next_member(&w->m, w->started ? w->last : (uint64_t) -1,
								   &w->cur);

	if (v == SM_IDX_MAX)
		return false;
	if (w->started && v <= w->last)
	{
		*bad = true;
		return false;
	}
	w->started = true;
	w->last = v;
	*out = v;
	return true;
}

static inline void
weave_doclist_walk_close(WeaveDocListWalk *w)
{
	free(w->buf);
	w->buf = NULL;
}

/*
 * Validate an image of exactly `len` bytes.  Returns NULL when valid, else a
 * constant string naming the first rule it breaks.
 *
 * Checks: magic, version, unknown flags, reserved words, exact length, each
 * set at least a sparsemap header long, ALL's walked membership == ndocs >= 1,
 * NULL's walked membership == nnull <= ndocs, nulllen == 0 iff nnull == 0, and
 * NULL a subset of ALL (one merge of the two ascending walks, O(ndocs)).
 */
static inline const char *
weave_doclist_check(const void *img, size_t len)
{
	WeaveDocListHeader h;
	WeaveDocListWalk wa;
	WeaveDocListWalk wn;
	uint64_t	a = 0,
				x = 0,
				na = 0,
				nn = 0;
	bool		bad = false;
	bool		havea;
	const char *why = NULL;

	if (img == NULL || len < WEAVE_DOCLIST_HDRSIZE)
		return "document list shorter than its header";
	memcpy(&h, img, sizeof(h));
	if (h.magic != WEAVE_DOCLIST_MAGIC)
		return "document list magic is wrong";
	if (h.version != WEAVE_DOCLIST_VERSION)
		return "document list version is not recognized";
	if ((h.flags & ~WEAVE_DOCLIST_F_ALL) != 0)
		return "document list has unknown flag bits";
	if (h.reserved != 0 || h.reserved2 != 0)
		return "document list reserved words are not zero";
	if (h.ndocs == 0)
		return "document list is empty";
	if (h.nnull > h.ndocs)
		return "document list has more NULL documents than documents";
	if ((h.nnull == 0) != (h.nulllen == 0))
		return "document list NULL set length disagrees with its count";
	/* a raw sparsemap is at least its 8-byte chunk-count word; shorter would
	 * reach sm_open() below the size at which it validates anything */
	if (h.alllen < 8 || (h.nulllen != 0 && h.nulllen < 8))
		return "document list set is shorter than a sparsemap header";
	if (weave_doclist_image_len(h.alllen, h.nulllen) != len)
		return "document list length disagrees with its header";

	if (!weave_doclist_walk_open(&wa, (const uint8_t *) img + WEAVE_DOCLIST_HDRSIZE,
								 h.alllen))
		return "out of memory validating a document list";
	if (h.nnull == 0)
	{
		while (weave_doclist_walk_next(&wa, &a, &bad))
			na++;
		weave_doclist_walk_close(&wa);
		if (bad)
			return "document list ALL set does not ascend";
		return na == h.ndocs ? NULL :
			"document list ALL set is malformed or disagrees with ndocs";
	}
	if (!weave_doclist_walk_open(&wn, (const uint8_t *) img +
								 weave_doclist_null_off(h.alllen), h.nulllen))
	{
		weave_doclist_walk_close(&wa);
		return "out of memory validating a document list";
	}
	havea = weave_doclist_walk_next(&wa, &a, &bad);
	if (havea)
		na++;
	while (why == NULL && weave_doclist_walk_next(&wn, &x, &bad))
	{
		nn++;
		while (havea && a < x)
		{
			havea = weave_doclist_walk_next(&wa, &a, &bad);
			if (havea)
				na++;
		}
		if (!havea || a != x)
			why = "document list NULL set is not a subset of ALL";
	}
	while (why == NULL && havea)
	{
		havea = weave_doclist_walk_next(&wa, &a, &bad);
		if (havea)
			na++;
	}
	weave_doclist_walk_close(&wa);
	weave_doclist_walk_close(&wn);
	if (why != NULL)
		return why;
	if (bad)
		return "document list set does not ascend";
	if (na != h.ndocs)
		return "document list ALL set is malformed or disagrees with ndocs";
	if (nn != h.nnull)
		return "document list NULL set is malformed or disagrees with nnull";
	return NULL;
}

/*
 * Decode a VALIDATED image into two caller-provided ascending arrays: `all`
 * of h.ndocs entries and `nul` of h.nnull entries (may be NULL when nnull==0).
 * Returns false on any inconsistency (it re-checks the cardinalities while
 * walking, so a caller that skipped validation still cannot overrun).
 */
static inline bool
weave_doclist_decode(const void *img, size_t len, uint64_t *all, uint64_t *nul)
{
	WeaveDocListHeader h;
	WeaveDocListWalk w;
	uint64_t	v;
	uint64_t	n = 0;
	bool		bad = false;

	if (len < WEAVE_DOCLIST_HDRSIZE)
		return false;
	memcpy(&h, img, sizeof(h));
	if (weave_doclist_image_len(h.alllen, h.nulllen) != len)
		return false;

	if (!weave_doclist_walk_open(&w, (const uint8_t *) img + WEAVE_DOCLIST_HDRSIZE,
								 h.alllen))
		return false;
	while (weave_doclist_walk_next(&w, &v, &bad))
	{
		if (n >= h.ndocs)
		{
			bad = true;
			break;
		}
		all[n++] = v;
	}
	weave_doclist_walk_close(&w);
	if (bad || n != h.ndocs)
		return false;
	if (h.nnull == 0)
		return true;

	if (!weave_doclist_walk_open(&w, (const uint8_t *) img +
								 weave_doclist_null_off(h.alllen), h.nulllen))
		return false;
	n = 0;
	while (weave_doclist_walk_next(&w, &v, &bad))
	{
		if (n >= h.nnull)
		{
			bad = true;
			break;
		}
		nul[n++] = v;
	}
	weave_doclist_walk_close(&w);
	return !bad && n == h.nnull;
}

/*
 * NOT sm_create_from_array(): the vendored implementation calls the
 * NON-growing sm_add_many() on a map created at 1 KiB, and when that has to
 * grow it reallocs the buffer and then sm_free()s the pre-realloc pointer -- a
 * use-after-free that ASan reports on the first set over ~1 KiB
 * (test/hegel/test_doclist.c found it).  sm_create + sm_add_many_grow is the
 * idiom the rest of the tree uses (amvacuum.c).
 */
static inline sm_t *
weave_doclist_sm_from(const uint64_t *v, size_t n)
{
	sm_t	   *m = sm_create(256);

	if (m == NULL)
		return NULL;
	if (n > 0 && !sm_add_many_grow(&m, v, n))
	{
		sm_free(m);
		return NULL;
	}
	return m;
}

static inline uint8_t *
weave_doclist_encode(const uint64_t *all, size_t n, const uint64_t *nul,
					 size_t nnull, uint16_t flags, size_t *len_out)
{
	sm_t	   *ma;
	sm_t	   *mn = NULL;
	size_t		alllen;
	size_t		nulllen = 0;
	uint8_t    *img;
	WeaveDocListHeader h;

	*len_out = 0;
	if (n == 0)
		return NULL;
	ma = weave_doclist_sm_from(all, n);
	if (ma == NULL)
		return NULL;
	alllen = sm_get_size(ma);
	if (nnull > 0)
	{
		mn = weave_doclist_sm_from(nul, nnull);
		if (mn == NULL)
		{
			sm_free(ma);
			return NULL;
		}
		nulllen = sm_get_size(mn);
	}
	if (alllen > UINT32_MAX || nulllen > UINT32_MAX)
	{
		sm_free(ma);
		sm_free(mn);
		return NULL;
	}
	img = (uint8_t *) calloc(1, weave_doclist_image_len((uint32_t) alllen,
														(uint32_t) nulllen));
	if (img == NULL)
	{
		sm_free(ma);
		sm_free(mn);
		return NULL;
	}
	memset(&h, 0, sizeof(h));
	h.magic = WEAVE_DOCLIST_MAGIC;
	h.version = WEAVE_DOCLIST_VERSION;
	h.flags = flags;
	h.ndocs = (uint64_t) n;
	h.nnull = (uint64_t) nnull;
	h.alllen = (uint32_t) alllen;
	h.nulllen = (uint32_t) nulllen;
	memcpy(img, &h, sizeof(h));
	memcpy(img + WEAVE_DOCLIST_HDRSIZE, sm_get_data(ma), alllen);
	if (nulllen > 0)
		memcpy(img + weave_doclist_null_off((uint32_t) alllen), sm_get_data(mn),
			   nulllen);
	*len_out = weave_doclist_image_len((uint32_t) alllen, (uint32_t) nulllen);
	sm_free(ma);
	sm_free(mn);
	return img;
}

/*
 * Sort + dedupe a docid array in place; returns the new length.  The build
 * accumulates in heap-scan order, which is NOT docid order (a synchronized scan
 * starts mid-relation, t/017), so every writer goes through this.
 */
static inline int
weave_doclist_cmp_u64(const void *a, const void *b)
{
	uint64_t	x = *(const uint64_t *) a;
	uint64_t	y = *(const uint64_t *) b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

static inline size_t
weave_doclist_sort_uniq(uint64_t *v, size_t n)
{
	size_t		i,
				o = 0;

	if (n == 0)
		return 0;
	qsort(v, n, sizeof(uint64_t), weave_doclist_cmp_u64);
	for (i = 0; i < n; i++)
		if (o == 0 || v[o - 1] != v[i])
			v[o++] = v[i];
	return o;
}

#endif							/* WEAVE_DOCLIST_H */
