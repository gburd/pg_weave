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
 * THE STORE IS SELF-CONTAINED IN THE GLOBAL DOCID SPACE.  The fused core drives
 * every channel in the GLOBAL docid space -- weave_tid_to_docid() = heap block *
 * MaxHeapTuplesPerPage + offset (include/weave/am.h, include/weave/gate.h) --
 * which is SPARSE, not the dense [0,ndocs) array index.  So a docvalues gate must
 * emit those global docids, and the only structure that maps a dense index to a
 * global docid is a per-bolt map.  The vector weft's warp map is one such map, but
 * it exists only when the bolt has a vector column, and claim 3 (a selective WHERE
 * makes the scan faster) must hold for a facet gate whether or not a vector column
 * is present.  So THIS store carries its OWN map: alongside the dense value array
 * it stores a parallel, STRICTLY ASCENDING array of the global docid each dense
 * index denotes (doc/specs/DOCVALS_CHANNEL.md sect. 3, "self-contained docid
 * array", decided 2026-09-26).  weave_dv_eval_int8() therefore emits GLOBAL
 * DOCIDS, ready to feed weave_gate_shuttle_from_tidset() with no external map.
 *
 * This slice covers a SINGLE int8 (int64) column with an OPTIONAL null bitmap
 * (store format v2; a NULL is recorded and excluded from every comparison gate)
 * and no zone-map.
 * It is extracted here as pure standalone C (no PostgreSQL includes) so it can
 * be exercised by standalone property tests (test/hegel/) while remaining the
 * single source of truth -- the backend page writer (src/pages/docvals_page.c)
 * #includes this header rather than carry its own copy, and asserts that its
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
 * On-disk header for an int8 docvalues store (v1, or v2 with an optional null
 * bitmap).  Fixed-width fields only, so the byte layout is identical on every
 * target: the total is 28 bytes with no trailing padding (largest member is
 * 4-byte aligned).  The store has up to four regions after the header:
 *
 *	 values	 : ndocs int64 values, begins at values_off == WEAVE_DV_MAXALIGN(28).
 *	 docids	 : ndocs uint64 GLOBAL docids (weave_tid_to_docid), STRICTLY ascending,
 *			   begins at docids_off == values_off + ndocs*8.  docids[i] is the
 *			   global docid the dense index i denotes; the two arrays are parallel.
 *	 nulls	 : (v2 only, present iff null_off != 0) a bitmap of ceil(ndocs/8)
 *			   bytes, one bit per dense docid, bit set == that docid's value is
 *			   NULL.  It begins at null_off == WEAVE_DV_MAXALIGN(docids_off +
 *			   ndocs*8), i.e. immediately after the docid array, 8-byte aligned.
 *
 * WHY null_off == 0 MEANS "NO NULLS", AND WHY THE OFFSET IS ALIGNED.  A segment
 * with no NULL facet values (the common case, and every NOT-NULL column) carries
 * no bitmap at all: null_off == 0 is the sentinel for "the whole dense space is
 * non-null", so a v2 store pays nothing for a facet that never nulls, and a v1
 * store (which predates the bitmap) reads unchanged.  When a bitmap IS present it
 * sits at the MAXALIGNed end of the docid array so the region keeps the same
 * 8-byte discipline as the two arrays before it; a reader that trusted an
 * arbitrary null_off could be walked off the image, so the validator recomputes
 * the one legal offset and refuses any other.
 *
 * These bytes come off disk and are NOT trusted; weave_docvals_validate() checks
 * every field, and the strict ascent of the docid array, before any value or
 * docid is read -- because a non-ascending docid array is a confident wrong gate
 * set, not an error, if it reaches the evaluator (the exact hazard
 * include/weave/vecdocmap.h spends its header on).
 *
 * Reconciling with doc/specs/DOCVALS_CHANNEL.md sect. 3: the spec's header lists
 * typid/typlen/typbyval/collation, but this int8 slice is int8-only, so all of
 * that type identity collapses to a single typid_kind (== 1 for int8).  The
 * float/date/text slices, whose type and collation metadata the spec anticipates,
 * arrive under a further-bumped version.  zonemap_off is 0 and reserved for the
 * per-block zone-map a later slice adds.
 */
typedef struct WeaveDocvalsHeader
{
	uint32		magic;			/* 0x57445631 == "WDV1"; wrong => not our store */
	uint16		version;		/* 1 or 2; a reader refuses anything else */
	uint16		typid_kind;		/* 1 == int8; the only value kind in this slice */
	uint32		ndocs;			/* number of per-docid values; the id space size */
	uint32		null_off;		/* 0 == no null bitmap (all non-null); else the
								 * v2 bitmap offset, MAXALIGN(docids_off+ndocs*8).
								 * Must be 0 for version 1 (see the file header). */
	uint32		zonemap_off;	/* 0 in v1: reserved for a later per-block min/max
								 * zone-map.  Adding it later only sets this to a
								 * nonzero offset and appends bytes, so it is a
								 * non-breaking addition a v1 reader rejects (it
								 * requires ==0) rather than silently misreads. */
	uint32		values_off;		/* MAXALIGN(sizeof header); start of int8 array */
	uint32		docids_off;		/* values_off + ndocs*8; start of the parallel
								 * uint64 global-docid array (see the file header:
								 * the store is self-contained in the global docid
								 * space and needs no external warp map). */
} WeaveDocvalsHeader;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(WeaveDocvalsHeader) == 28,
			   "WeaveDocvalsHeader on-disk layout must be 28 bytes");
#endif

/* "WDV1" in little-endian byte order (0x31='1',0x56='V',0x44='D',0x57='W'). */
#define WEAVE_DOCVALS_MAGIC		0x57445631u

/*
 * Local MAXALIGN.  The int8 values are 8-byte int64 and the docids are 8-byte
 * uint64, so both arrays are 8-byte aligned.  The mask is size_t-width
 * (~(size_t) 7, not ~7u) so a 64-bit argument's high bits are not truncated by a
 * 32-bit int mask.  This matches PostgreSQL's MAXALIGN for the 8-byte case
 * (MAXIMUM_ALIGNOF == 8 on every platform this project targets); the backend page
 * writer asserts that agreement rather than assume it.
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
 * Order-preserving type -> int64 encodings.  The on-disk store and
 * weave_dv_eval_int8() are int64-only; every supported facet type is mapped to an
 * int64 that sorts the SAME way the type's btree `<` does, at BOTH build/insert
 * (the stored value) and scan (the query constant), so the single signed-int64
 * comparison in the evaluator is exact for every type and the store never needs to
 * know the type.  Integer-like types (int2/int4/int8/date, bool as 0/1) are
 * order-preserving under sign-extension, so their encode is a widening cast at the
 * call sites (they need a PostgreSQL Datum accessor).  float8 is the one
 * non-trivial, PURE case and lives here with its property test
 * (test/hegel/test_docvals.c).
 */
typedef enum
{
	WEAVE_DV_T_INT8 = 0,
	WEAVE_DV_T_INT4,
	WEAVE_DV_T_INT2,
	WEAVE_DV_T_BOOL,
	WEAVE_DV_T_DATE,
	WEAVE_DV_T_FLOAT8
} WeaveDvType;

/*
 * Map a float8 to an int64 that sorts identically to PostgreSQL's float8 `<` under
 * SIGNED int64 comparison (the evaluator's comparison).
 *
 * The transform is the classic monotonic IEEE-754 one, in two moves:
 *   1. sign-magnitude -> UNSIGNED-orderable: for a negative double flip every bit,
 *      for a non-negative one set the sign bit.  Now -inf<..<-0<+0<..<+inf is
 *      ascending as an UNSIGNED 64-bit value.
 *   2. UNSIGNED-orderable -> SIGNED-orderable: XOR the sign bit, because signed
 *      int64 order equals the unsigned order of (x ^ 2^63).  Without this step
 *      negative floats would sort ABOVE positive ones (they were the ones whose
 *      encoded top bit is clear) -- the bug the property test is written to catch.
 *
 * TWO canonicalisations, because PostgreSQL's float8 order is NOT the raw IEEE one
 * and the gate must match the operator EXACTLY (a wrong gate is a silent wrong
 * answer, hard rule 1):
 *   - NaN -> INT64_MAX: PG orders NaN greater than every non-NaN and treats all NaN
 *     equal; trusting the bits would sort a sign-set NaN to the bottom.
 *   - signed zero: PG treats -0.0 == +0.0 but their bits differ by the sign bit, so
 *     force d == 0 to +0.0 so both zeros share one key.
 */
static inline int64_t
weave_dv_encode_f8(double d)
{
	uint64_t	bits;

	if (d != d)
		return INT64_MAX;			/* NaN: PG's largest, all-equal */
	if (d == 0.0)
		d = 0.0;					/* collapse -0.0 into +0.0 (PG: equal) */
	memcpy(&bits, &d, sizeof(bits));
	if (bits >> 63)
		bits = ~bits;				/* negative: flip all */
	else
		bits |= (uint64_t) 1 << 63; /* non-negative: set sign bit */
	bits ^= (uint64_t) 1 << 63;		/* unsigned-orderable -> signed-orderable */
	return (int64_t) bits;
}

/*
 * Returns NULL if img (of byte length len) is a structurally valid int8 store
 * (version 1, or version 2 with an optional null bitmap) for its stated ndocs,
 * whose docid array is strictly ascending; otherwise a STATIC reason string.
 * On-disk bytes are not trusted, so every field is checked and nothing past len
 * is ever read: the size bounds (docids_off + ndocs*8, and the bitmap end) are
 * computed in uint64 so a large ndocs cannot wrap a size_t and admit a too-short
 * image.
 */
static inline const char *
weave_docvals_validate(const void *img, size_t len)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint64		docids_need;
	uint64		total_need;
	uint32		i;
	uint64		prev = 0;

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
	if (h.version != 1 && h.version != 2)
		return "unsupported docvalues version";
	if (h.typid_kind != 1)
		return "unsupported value kind (v1 is int8 only)";
	if (h.values_off != voff)
		return "values_off does not match aligned header size";
	if (h.zonemap_off != 0)
		return "zonemap_off must be 0";

	/*
	 * Overflow-safe offsets and size check, all in uint64.  ndocs is uint32 so
	 * ndocs*8 <= ~3.4e10 cannot wrap uint64, and values_off + ndocs*8 stays well
	 * inside uint64, so docids_off is exact and comparisons never truncate.
	 */
	docids_need = (uint64) h.values_off + (uint64) h.ndocs * 8u;
	if ((uint64) h.docids_off != docids_need)
		return "docids_off does not follow the values array";
	total_need = (uint64) h.docids_off + (uint64) h.ndocs * 8u;
	if ((uint64) len < total_need)
		return "image too short for stated ndocs";

	/*
	 * Null bitmap (v2 only).  null_off == 0 is the "no nulls" sentinel and is
	 * the only legal value for version 1; a nonzero null_off must be version 2,
	 * must equal the single MAXALIGNed post-docids offset (any other value could
	 * point the reader off the image -- the bytes are not trusted), and its
	 * ceil(ndocs/8)-byte bitmap must fit within len.  All arithmetic is uint64
	 * for the same overflow discipline as the docid/value bounds above.
	 */
	if (h.null_off != 0)
	{
		uint64		null_need = WEAVE_DV_MAXALIGN(total_need);
		uint64		bitmap_end;

		if (h.version != 2)
			return "null_off set but version is not 2";
		if ((uint64) h.null_off != null_need)
			return "null_off is not the aligned post-docids offset";
		bitmap_end = (uint64) h.null_off + ((uint64) h.ndocs + 7u) / 8u;
		if ((uint64) len < bitmap_end)
			return "image too short for the null bitmap";
	}

	/*
	 * The docid array must be STRICTLY ascending: it is the dense-index ->
	 * global-docid map the gate emits through, and a non-monotone or duplicated
	 * entry would produce a wrong gate set (dropped or misordered rows), not an
	 * error, downstream.  Validate it here at the trust boundary.  Every read is
	 * inside [docids_off, docids_off + ndocs*8) which total_need bounds by len.
	 */
	for (i = 0; i < h.ndocs; i++)
	{
		uint64		d;

		memcpy(&d, base + h.docids_off + (size_t) i * 8u, sizeof(d));
		if (i > 0 && d <= prev)
			return "docid array is not strictly ascending";
		prev = d;
	}

	return NULL;
}

/*
 * Value at dense index.  Caller guarantees idx < ndocs (asserted).  Reads via
 * memcpy so an unaligned img or strict-aliasing does not invoke UB.
 */
static inline int64_t
weave_docvals_int8(const void *img, uint32_t idx)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	int64_t		v;

	/* memcpy the header: img need not be aligned (see the struct comment). */
	memcpy(&h, img, sizeof(h));
	assert(idx < h.ndocs);
	memcpy(&v, base + h.values_off + (size_t) idx * 8u, sizeof(v));
	return v;
}

/*
 * Global docid the dense index denotes.  Caller guarantees idx < ndocs
 * (asserted).  memcpy for the same alignment/aliasing reason as the value read.
 */
static inline uint64_t
weave_docvals_docid(const void *img, uint32_t idx)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	uint64_t	d;

	memcpy(&h, img, sizeof(h));
	assert(idx < h.ndocs);
	memcpy(&d, base + h.docids_off + (size_t) idx * 8u, sizeof(d));
	return d;
}

/*
 * Is the value at dense index idx NULL?  Caller guarantees idx < ndocs
 * (asserted).  A store with no bitmap (null_off == 0) has no NULLs, so this
 * returns 0 without touching any region past the header -- the "no nulls"
 * sentinel is why a NOT-NULL segment and every v1 store cost nothing here.  When
 * a bitmap is present, bit (idx & 7) of byte (idx >> 3) is set iff idx is NULL;
 * the validator has already bounded [null_off, null_off + ceil(ndocs/8)) inside
 * len, so this single byte read is in-image.
 */
static inline int
weave_docvals_isnull(const void *img, uint32_t idx)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;

	memcpy(&h, img, sizeof(h));
	assert(idx < h.ndocs);
	if (h.null_off == 0)
		return 0;
	return (base[h.null_off + (size_t) (idx >> 3)] >> (idx & 7u)) & 1;
}

/*
 * Write, in ASCENDING order, the GLOBAL DOCID of every dense index in [0,ndocs)
 * whose value satisfies (value op c) into out[] (capacity outcap, which callers
 * set to ndocs).  Returns the count of matches.  Pure, no allocation.  A write is
 * guarded by outcap so a mis-sized buffer truncates rather than overruns; the
 * returned count is always the true number of matches.
 *
 * The output is ascending because the docid array is strictly ascending in the
 * dense index and the pass visits indices in order (the validator has already
 * refused a store whose docid array is not).  A NULL docid (its null bit set) is
 * emitted for NO operator: NULL is not less than, equal to, or greater than any
 * constant, so it is skipped before the comparison rather than being compared as
 * whatever int64 happens to sit in its value slot.
 */
static inline int
weave_dv_eval_int8(const void *img, WeaveDvStrat op, int64_t c,
				   uint64_t *out, uint32_t outcap)
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
		int64_t		v;
		int			match;

		/* A NULL docid satisfies no comparison; skip before the op switch. */
		if (weave_docvals_isnull(img, i))
			continue;

		v = weave_docvals_int8(img, i);

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
				out[count] = weave_docvals_docid(img, i);
			count++;
		}
	}
	return count;
}

/*
 * Test-only helpers to build an in-memory store from parallel value and docid
 * arrays.  Guarded so they never enter a backend build; the property test defines
 * WEAVE_DOCVALS_TEST_HELPERS.
 */
#ifdef WEAVE_DOCVALS_TEST_HELPERS

/* Bytes a store of ndocs int8 values occupies: header + values + docids. */
static inline size_t
weave_docvals_store_len(uint32_t ndocs)
{
	return (size_t) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader))
		+ (size_t) ndocs * 8u	/* values */
		+ (size_t) ndocs * 8u;	/* docids */
}

/*
 * Fill header + values + docids into buf, which must be at least
 * weave_docvals_store_len(ndocs) bytes.  The caller supplies the docids
 * (strictly ascending for a well-formed store; the test also builds malformed
 * ones on purpose to check the validator refuses them).
 */
static inline void
weave_docvals_build(void *buf, const int64_t *vals, const uint64_t *docids,
					uint32_t ndocs)
{
	WeaveDocvalsHeader *h = (WeaveDocvalsHeader *) buf;
	unsigned char *base = (unsigned char *) buf;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint32		doff = voff + ndocs * 8u;
	uint32_t	i;

	h->magic = WEAVE_DOCVALS_MAGIC;
	h->version = 1;
	h->typid_kind = 1;
	h->ndocs = ndocs;
	h->null_off = 0;
	h->zonemap_off = 0;
	h->values_off = voff;
	h->docids_off = doff;

	for (i = 0; i < ndocs; i++)
		memcpy(base + voff + (size_t) i * 8u, &vals[i], sizeof(int64_t));
	for (i = 0; i < ndocs; i++)
		memcpy(base + doff + (size_t) i * 8u, &docids[i], sizeof(uint64_t));
}

/*
 * Bytes a v2 store occupies: header + values + docids, plus (when has_nulls) the
 * MAXALIGNed null bitmap of ceil(ndocs/8) bytes.  Mirrors the layout the writer
 * produces and the validator recomputes.
 */
static inline size_t
weave_docvals_store_len_nulls(uint32_t ndocs, int has_nulls)
{
	size_t		body = weave_docvals_store_len(ndocs);

	if (!has_nulls)
		return body;
	return (size_t) WEAVE_DV_MAXALIGN(body) + (size_t) ((ndocs + 7u) / 8u);
}

/*
 * Fill a version-2 store into buf, which must be at least
 * weave_docvals_store_len_nulls(ndocs, nullbits != NULL) bytes.  When nullbits is
 * non-NULL it is a per-doc byte array (nonzero == NULL) that is packed into the
 * bitmap and null_off is set to the aligned post-docids offset; when it is NULL
 * the store has no bitmap (null_off == 0, all non-null -- a NOT-NULL segment).
 */
static inline void
weave_docvals_build_nulls(void *buf, const int64_t *vals, const uint64_t *docids,
						  const uint8_t *nullbits, uint32_t ndocs)
{
	WeaveDocvalsHeader *h = (WeaveDocvalsHeader *) buf;
	unsigned char *base = (unsigned char *) buf;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint32		doff = voff + ndocs * 8u;
	uint32		noff = (uint32) WEAVE_DV_MAXALIGN((size_t) doff + (size_t) ndocs * 8u);
	uint32_t	i;

	h->magic = WEAVE_DOCVALS_MAGIC;
	h->version = 2;
	h->typid_kind = 1;
	h->ndocs = ndocs;
	h->null_off = (nullbits != NULL) ? noff : 0;
	h->zonemap_off = 0;
	h->values_off = voff;
	h->docids_off = doff;

	for (i = 0; i < ndocs; i++)
		memcpy(base + voff + (size_t) i * 8u, &vals[i], sizeof(int64_t));
	for (i = 0; i < ndocs; i++)
		memcpy(base + doff + (size_t) i * 8u, &docids[i], sizeof(uint64_t));

	if (nullbits != NULL)
	{
		uint32		nbytes = (ndocs + 7u) / 8u;

		for (i = 0; i < nbytes; i++)
			base[noff + i] = 0;
		for (i = 0; i < ndocs; i++)
			if (nullbits[i])
				base[noff + (i >> 3)] |= (unsigned char) (1u << (i & 7u));
	}
}

#endif							/* WEAVE_DOCVALS_TEST_HELPERS */

#endif							/* WEAVE_DOCVALS_H */
