/*-------------------------------------------------------------------------
 *
 * docvals.h
 *		Backend-independent scalar "docvalues" store for one int8 facet column.
 *
 * A docvalues store maps every docid in a segment's dense [0,ndocs) id space to
 * one scalar value, so a scalar predicate (a WHERE clause over a facet column)
 * can be evaluated as a straight pass over a value array sharing the segment's
 * docid space -- the same docid space the lexical and vector wefts use, which is
 * what lets a selective facet bound skip work in another channel.
 *
 * This slice covers a SINGLE int8 (int64) column with no nulls and no zone-map.
 * It is extracted here as pure standalone C (no PostgreSQL includes) so it can
 * be exercised by standalone property tests (test/hegel/) while remaining the
 * single source of truth -- the backend page writer (a later task) will
 * #include this header rather than carry its own copy, and will assert that its
 * MAXALIGN agrees with WEAVE_DV_MAXALIGN below.
 *
 * When included from a PostgreSQL backend TU, postgres.h has already defined
 * uint64/uint32/uint16/uint8; the guards below keep this header from redefining
 * them.  When included from a standalone test, it provides them from <stdint.h>.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  docvals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_DOCVALS_H
#define WEAVE_DOCVALS_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * PostgreSQL's c.h defines uint64/uint32/uint16/uint8 and UINT64CONST; only
 * supply them when we are compiled outside the backend (postgres.h not
 * included).  We key off UINT64CONST, which is only defined by c.h -- exactly
 * as weave/for.h does, so the two headers coexist in one translation unit.
 */
#ifndef UINT64CONST
typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;
#define UINT64CONST(x) ((uint64) x##ULL)
#endif

/*
 * On-disk header for a v1 int8 docvalues store.  Fixed-width fields only, so the
 * byte layout is identical on every target: the total is 28 bytes with no
 * trailing padding (largest member is 4-byte aligned), and the int8 value array
 * begins at values_off == WEAVE_DV_MAXALIGN(28) == 32.  These bytes come off
 * disk and are NOT trusted; weave_docvals_validate() checks every field before
 * any value is read.
 *
 * Reconciling with doc/specs/DOCVALS_CHANNEL.md §3: the spec's header lists
 * typid/typlen/typbyval/collation, but this v1 slice is int8-only, so all of
 * that type identity collapses to a single typid_kind (== 1 for int8) and no
 * width/byval/collation word is needed yet.  The float/date/text slices, whose
 * type and collation metadata the spec anticipates, arrive under a bumped
 * version consuming the currently-`reserved` word -- which is why both
 * typid_kind and reserved exist here rather than the full spec field set.
 */
typedef struct WeaveDocvalsHeader
{
	uint32		magic;			/* 0x57445631 == "WDV1"; wrong => not our store */
	uint16		version;		/* 1; a reader refuses anything it does not know */
	uint16		typid_kind;		/* 1 == int8; the only value kind in this slice */
	uint32		ndocs;			/* number of per-docid values; the id space size */
	uint32		null_off;		/* 0 in v1: no null bitmap yet (no nulls handled) */
	uint32		zonemap_off;	/* 0 in v1: reserved for a later per-block min/max
								 * zone-map.  Adding it later only sets this to a
								 * nonzero offset and appends bytes, so it is a
								 * non-breaking addition a v1 reader rejects (it
								 * requires ==0) rather than silently misreads. */
	uint32		values_off;		/* MAXALIGN(sizeof header); start of int8 array */
	uint32		reserved;		/* 0; kept for a future flags/pad word */
} WeaveDocvalsHeader;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(WeaveDocvalsHeader) == 28,
			   "WeaveDocvalsHeader on-disk layout must be 28 bytes");
#endif

/* "WDV1" in little-endian byte order (0x31='1',0x56='V',0x44='D',0x57='W'). */
#define WEAVE_DOCVALS_MAGIC		0x57445631u

/*
 * Local MAXALIGN.  The int8 values are 8-byte int64, so the value array is
 * 8-byte aligned.  The mask is size_t-width (~(size_t) 7, not ~7u) so a 64-bit
 * argument's high bits are not truncated by a 32-bit int mask.  This matches
 * PostgreSQL's MAXALIGN for the 8-byte case (MAXIMUM_ALIGNOF == 8 on every
 * platform this project targets); the backend page writer will assert that
 * agreement rather than assume it.
 */
#define WEAVE_DV_MAXALIGN(x)	(((x) + 7) & ~(size_t) 7)

/*
 * B-tree strategy numbering, so a committer recognises the operators: these are
 * the standard btree strategy numbers (BTLessStrategyNumber .. BTGreater
 * StrategyNumber) 1..5.
 */
typedef enum
{
	WEAVE_DV_LT = 1,
	WEAVE_DV_LE = 2,
	WEAVE_DV_EQ = 3,
	WEAVE_DV_GE = 4,
	WEAVE_DV_GT = 5
} WeaveDvStrat;

/*
 * Returns NULL if img (of byte length len) is a structurally valid v1 int8 store
 * for its stated ndocs; otherwise a STATIC reason string.  On-disk bytes are not
 * trusted, so every field is checked and nothing past len is ever read: the size
 * bound values_off + ndocs*8 is computed in uint64 so a large ndocs cannot wrap
 * a size_t and admit a too-short image.
 */
static inline const char *
weave_docvals_validate(const void *img, size_t len)
{
	WeaveDocvalsHeader h;
	uint64		need;

	if (len < sizeof(WeaveDocvalsHeader))
		return "image shorter than header";

	/*
	 * Copy the header out of img before reading any field: img need not be
	 * 8-aligned (the standalone test buffer is not), so a struct-pointer cast
	 * and field access would be UB.  The len >= sizeof(WeaveDocvalsHeader)
	 * check above guarantees this memcpy does not read past the image.
	 */
	memcpy(&h, img, sizeof(h));

	if (h.magic != WEAVE_DOCVALS_MAGIC)
		return "bad magic (not a weave docvalues store)";
	if (h.version != 1)
		return "unsupported docvalues version";
	if (h.typid_kind != 1)
		return "unsupported value kind (v1 is int8 only)";
	if (h.values_off != (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader)))
		return "values_off does not match aligned header size";
	if (h.null_off != 0)
		return "null_off must be 0 in v1";
	if (h.zonemap_off != 0)
		return "zonemap_off must be 0 in v1";
	if (h.reserved != 0)
		return "reserved must be 0 in v1";

	/*
	 * Overflow-safe size check: values_off + ndocs*8 in uint64.  ndocs is uint32
	 * so ndocs*8 <= ~3.4e10 cannot wrap uint64, and comparing the uint64 bound
	 * against len (widened to uint64) never truncates.
	 */
	need = (uint64) h.values_off + (uint64) h.ndocs * 8u;
	if ((uint64) len < need)
		return "image too short for stated ndocs";

	return NULL;
}

/*
 * Value at docid.  Caller guarantees docid < ndocs (asserted).  Reads via memcpy
 * so an unaligned img or strict-aliasing does not invoke UB.
 */
static inline int64_t
weave_docvals_int8(const void *img, uint32_t docid)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	int64_t		v;

	/* memcpy the header: img need not be aligned (see the struct comment). */
	memcpy(&h, img, sizeof(h));
	assert(docid < h.ndocs);
	memcpy(&v, base + h.values_off + (size_t) docid * 8u, sizeof(v));
	return v;
}

/*
 * Write, in ASCENDING docid order, every docid in [0,ndocs) whose value
 * satisfies (value op c) into out[] (capacity outcap, which callers set to
 * ndocs).  Returns the count of matches.  Pure, no allocation.  A write is
 * guarded by outcap so a mis-sized buffer truncates rather than overruns; the
 * returned count is always the true number of matches.  NULLs are not in this
 * slice (the store has none).
 */
static inline int
weave_dv_eval_int8(const void *img, WeaveDvStrat op, int64_t c,
				   uint32_t *out, uint32_t outcap)
{
	WeaveDocvalsHeader h;
	uint32_t	n;
	uint32_t	i;
	int			count = 0;

	/* memcpy the header: img need not be aligned (see the struct comment). */
	memcpy(&h, img, sizeof(h));
	n = h.ndocs;

	for (i = 0; i < n; i++)
	{
		int64_t		v = weave_docvals_int8(img, i);
		int			match;

		switch (op)
		{
			case WEAVE_DV_LT:
				match = (v < c);
				break;
			case WEAVE_DV_LE:
				match = (v <= c);
				break;
			case WEAVE_DV_EQ:
				match = (v == c);
				break;
			case WEAVE_DV_GE:
				match = (v >= c);
				break;
			case WEAVE_DV_GT:
				match = (v > c);
				break;
			default:
				match = 0;
				break;
		}

		if (match)
		{
			if ((uint32_t) count < outcap)
				out[count] = i;
			count++;
		}
	}
	return count;
}

/*
 * Test-only helpers to build an in-memory store from an int64 array.  Guarded so
 * they never enter a backend build; the property test defines
 * WEAVE_DOCVALS_TEST_HELPERS.
 */
#ifdef WEAVE_DOCVALS_TEST_HELPERS

/* Bytes a store of ndocs int8 values occupies: values_off + ndocs*8. */
static inline size_t
weave_docvals_store_len(uint32_t ndocs)
{
	return (size_t) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader))
		+ (size_t) ndocs * 8u;
}

/*
 * Fill header + values into buf, which must be at least
 * weave_docvals_store_len(ndocs) bytes.
 */
static inline void
weave_docvals_build(void *buf, const int64_t *vals, uint32_t ndocs)
{
	WeaveDocvalsHeader *h = (WeaveDocvalsHeader *) buf;
	unsigned char *base = (unsigned char *) buf;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint32_t	i;

	h->magic = WEAVE_DOCVALS_MAGIC;
	h->version = 1;
	h->typid_kind = 1;
	h->ndocs = ndocs;
	h->null_off = 0;
	h->zonemap_off = 0;
	h->values_off = voff;
	h->reserved = 0;

	for (i = 0; i < ndocs; i++)
		memcpy(base + voff + (size_t) i * 8u, &vals[i], sizeof(int64_t));
}

#endif							/* WEAVE_DOCVALS_TEST_HELPERS */

#endif							/* WEAVE_DOCVALS_H */
