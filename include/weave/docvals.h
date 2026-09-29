/*-------------------------------------------------------------------------
 *
 * docvals.h
 *		Backend-independent scalar "docvalues" store for one facet column
 *		(int8-encoded scalars, or dictionary-encoded text).
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
 * and no zone-map, plus a SINGLE text column (store format v3): the same int64
 * value array then holds a DICTIONARY ORDINAL, and a dictionary region after the
 * docid array (and bitmap) carries the distinct values sorted by the column's
 * collation, so a text comparison becomes an ordinal-range comparison
 * (weave_dv_eval_ord) once the constant is resolved to two ordinal boundaries
 * (weave_dv_dict_lower_bound / weave_dv_dict_upper_bound).
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
 * On-disk header for a docvalues store (int8: v1, or v2 with an optional null
 * bitmap; text: v3, v2's shape plus a dictionary).  Fixed-width fields only, so
 * the byte layout is identical on every target: the total is 32 bytes with no
 * trailing padding (largest member is 4-byte aligned).  The store has up to four
 * regions after the header:
 *
 *	 values	 : ndocs int64 values, begins at values_off == WEAVE_DV_MAXALIGN(32)
 *			   == 32.  In a v3 (text) store each value is a dictionary ordinal.
 *	 docids	 : ndocs uint64 GLOBAL docids (weave_tid_to_docid), STRICTLY ascending,
 *			   begins at docids_off == values_off + ndocs*8.  docids[i] is the
 *			   global docid the dense index i denotes; the two arrays are parallel.
 *	 nulls	 : (v2/v3 only, present iff null_off != 0) a bitmap of ceil(ndocs/8)
 *			   bytes, one bit per dense docid, bit set == that docid's value is
 *			   NULL.  It begins at null_off == WEAVE_DV_MAXALIGN(docids_off +
 *			   ndocs*8), i.e. immediately after the docid array, 8-byte aligned.
 *	 dict	 : (v3 only) begins at dict_off == WEAVE_DV_MAXALIGN(end of the null
 *			   bitmap if null_off != 0, else end of the docid array):
 *			   uint32 ndict, uint32 offs[ndict + 1] (offs[0] == 0, non-decreasing),
 *			   then offs[ndict] bytes of packed entry payload (raw text bytes, no
 *			   varlena header).  Entry i is blob[offs[i] .. offs[i+1]).  The
 *			   entries are STRICTLY ascending under the column's collation, and a
 *			   non-NULL docid's value is an ordinal in [0, ndict).
 *
 * WHY dict_off FITS WITHOUT A LAYOUT CHANGE, AND WHY IT IS ONLY READ FOR v3.
 * The v1/v2 header was 28 bytes, but values_off was always WEAVE_DV_MAXALIGN(28)
 * == 32, so bytes 28..31 were alignment padding.  dict_off occupies exactly that
 * padding: every offset of a v1/v2 image is unchanged, so they are byte-identical
 * and still validate.  But the v1/v2 writer copied only the 28 named header
 * bytes into an unzeroed buffer, so bytes 28..31 of an EXISTING v1/v2 store may
 * hold garbage.  Hence the rule: dict_off is interpreted ONLY when version == 3,
 * by the validator and by every reader; a v1/v2 store's dict_off is ignored, not
 * required to be 0 (requiring it would reject valid stores already on disk).
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
 * typid/typlen/typbyval/collation, but the store knows only two value
 * representations (an int64, or a dictionary ordinal), so all of that type
 * identity collapses to a single typid_kind (WEAVE_DV_KIND_INT8 for v1/v2,
 * WEAVE_DV_KIND_TEXT for v3; the pure int64 encodings below map every other
 * scalar type onto int8).  The collation is NOT recorded: the dictionary
 * is sorted under the index column's collation, which the backend knows, and a
 * collation drift invalidates it exactly as it invalidates a btree (REINDEX).
 * zonemap_off is 0 and reserved for the per-block zone-map a later slice adds.
 */
typedef struct WeaveDocvalsHeader
{
	uint32		magic;			/* 0x57445631 == "WDV1"; wrong => not our store */
	uint16		version;		/* 1, 2 or 3; a reader refuses anything else */
	uint16		typid_kind;		/* WEAVE_DV_KIND_INT8 (v1/v2) or
								 * WEAVE_DV_KIND_TEXT (v3) */
	uint32		ndocs;			/* number of per-docid values; the id space size */
	uint32		null_off;		/* 0 == no null bitmap (all non-null); else the
								 * v2/v3 bitmap offset,
								 * MAXALIGN(docids_off+ndocs*8).  Must be 0 for
								 * version 1 (see the file header). */
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
	uint32		dict_off;		/* v3 only: offset of the dictionary region.
								 * Occupies the former alignment padding, so it is
								 * UNDEFINED (possibly garbage) in v1/v2 and must
								 * never be read unless version == 3.  Writers set
								 * it to 0 for v1/v2. */
} WeaveDocvalsHeader;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(WeaveDocvalsHeader) == 32,
			   "WeaveDocvalsHeader on-disk layout must be 32 bytes");
#endif

/* "WDV1" in little-endian byte order (0x31='1',0x56='V',0x44='D',0x57='W'). */
#define WEAVE_DOCVALS_MAGIC		0x57445631u

/*
 * typid_kind values.  The kind is tied to the version (v1/v2 int8, v3 text) so a
 * store whose version and kind disagree is refused rather than read as the wrong
 * value representation: an ordinal read as an int8 value, or an int8 value read
 * as an ordinal, is a confident wrong gate set.
 */
#define WEAVE_DV_KIND_INT8		1
#define WEAVE_DV_KIND_TEXT		2

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
	WEAVE_DV_T_FLOAT8,

	/*
	 * text / varchar (text_docval_ops).  NOT an int64 encoding: a text value has
	 * no order-preserving fixed-width image under a collation, so a text column
	 * stores a per-segment DICTIONARY ORDINAL (store v3) and the query constant
	 * is resolved to ordinal boundaries per segment (weave_dv_dict_lower_bound /
	 * weave_dv_dict_upper_bound).  Appended last so no existing value moves.
	 */
	WEAVE_DV_T_TEXT
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
 * Returns NULL if img (of byte length len) is a structurally valid store --
 * int8 (version 1, or version 2 with an optional null bitmap) or text (version
 * 3: v2's shape plus a dictionary region) -- for its stated ndocs, whose docid
 * array is strictly ascending; otherwise a STATIC reason string.  On-disk bytes
 * are not trusted, so every field is checked and nothing past len is ever read:
 * the size bounds (docids_off + ndocs*8, the bitmap end, the dictionary offsets
 * and blob) are computed in uint64 so a large ndocs or ndict cannot wrap a
 * size_t and admit a too-short image.
 *
 * What it CANNOT check for v3 is that the dictionary is strictly ascending under
 * the column's collation: that needs the backend's collation-aware comparator,
 * so it is weave_check's deep verification, not this pure function's.  What it
 * does check is everything a reader would otherwise index with: the region's
 * offsets, and that every non-NULL ordinal names an entry that exists.
 */
static inline const char *
weave_docvals_validate(const void *img, size_t len)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint64		docids_need;
	uint64		total_need;
	uint64		region_end;
	uint32		ndict = 0;
	uint32		i;
	uint64		prev = 0;

	if (len < sizeof(WeaveDocvalsHeader))
		return "image shorter than header";

	/*
	 * Copy the header out of img before reading any field: img need not be
	 * 8-aligned (the standalone test buffer is not), so a struct-pointer cast
	 * and field access would be UB.  The len >= sizeof(WeaveDocvalsHeader)
	 * check above guarantees this memcpy does not read past the image.  (A
	 * v1/v2 image is always >= values_off == 32 bytes long, so growing the
	 * header into its former padding does not make this check refuse one.)
	 */
	memcpy(&h, img, sizeof(h));

	if (h.magic != WEAVE_DOCVALS_MAGIC)
		return "bad magic (not a weave docvalues store)";
	if (h.version != 1 && h.version != 2 && h.version != 3)
		return "unsupported docvalues version";
	if (h.version == 3)
	{
		if (h.typid_kind != WEAVE_DV_KIND_TEXT)
			return "unsupported value kind (v3 is text only)";
	}
	else if (h.typid_kind != WEAVE_DV_KIND_INT8)
		return "unsupported value kind (v1/v2 are int8 only)";
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
	region_end = total_need;

	/*
	 * Null bitmap (v2 and v3).  null_off == 0 is the "no nulls" sentinel and is
	 * the only legal value for version 1; a nonzero null_off must be version 2
	 * or 3, must equal the single MAXALIGNed post-docids offset (any other value
	 * could point the reader off the image -- the bytes are not trusted), and
	 * its ceil(ndocs/8)-byte bitmap must fit within len.  All arithmetic is
	 * uint64 for the same overflow discipline as the docid/value bounds above.
	 */
	if (h.null_off != 0)
	{
		uint64		null_need = WEAVE_DV_MAXALIGN(total_need);
		uint64		bitmap_end;

		if (h.version == 1)
			return "null_off set on a version 1 store";
		if ((uint64) h.null_off != null_need)
			return "null_off is not the aligned post-docids offset";
		bitmap_end = (uint64) h.null_off + ((uint64) h.ndocs + 7u) / 8u;
		if ((uint64) len < bitmap_end)
			return "image too short for the null bitmap";
		region_end = bitmap_end;
	}

	/*
	 * Dictionary region (v3 only; dict_off is garbage-tolerant padding in v1/v2
	 * and is deliberately not looked at there -- see the struct comment).  Like
	 * null_off, the one legal dict_off is recomputed and any other refused, so a
	 * reader never trusts an offset from disk.  Each sub-region is bounded by
	 * len BEFORE it is read: the ndict word, then the ndict+1 offsets, then the
	 * blob the last offset claims.  ndict+1 is computed in uint64 so ndict ==
	 * UINT32_MAX cannot wrap to 0 and admit a zero-length offset array.
	 */
	if (h.version == 3)
	{
		uint64		dict_need = WEAVE_DV_MAXALIGN(region_end);
		uint64		offs_base;
		uint64		offs_end;
		uint64		j;
		uint32		o;
		uint32		prevo;

		if (h.dict_off == 0)
			return "v3 store has no dictionary (dict_off is 0)";
		if ((uint64) h.dict_off != dict_need)
			return "dict_off is not the aligned post-docids/bitmap offset";
		offs_base = (uint64) h.dict_off + 4u;
		if ((uint64) len < offs_base)
			return "image too short for the dictionary header";
		memcpy(&ndict, base + h.dict_off, sizeof(ndict));
		offs_end = offs_base + ((uint64) ndict + 1u) * 4u;
		if ((uint64) len < offs_end)
			return "image too short for the dictionary offsets";

		/*
		 * offs[0] == 0 and non-decreasing: entry i is blob[offs[i]..offs[i+1]),
		 * so a decreasing pair would hand a reader a negative (wrapped) length,
		 * and a nonzero offs[0] would leave unaccounted bytes that a writer
		 * never produces.  Every read is inside [offs_base, offs_end) <= len.
		 */
		memcpy(&prevo, base + offs_base, sizeof(prevo));
		if (prevo != 0)
			return "dictionary offs[0] is not 0";
		for (j = 1; j <= (uint64) ndict; j++)
		{
			memcpy(&o, base + offs_base + j * 4u, sizeof(o));
			if (o < prevo)
				return "dictionary offsets are not non-decreasing";
			prevo = o;
		}
		if ((uint64) len < offs_end + (uint64) prevo)
			return "image too short for the dictionary blob";
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

	/*
	 * v3: every non-NULL value is a dictionary ordinal and must name an entry
	 * that exists, [0, ndict).  weave_dv_eval_ord() compares ordinals without
	 * looking entries up, so an out-of-range ordinal would not crash there --
	 * it would silently fall on one side of every boundary, a confident wrong
	 * answer; and any caller that DOES look it up (merge re-dictionary) would
	 * read past the blob.  A NULL docid's slot is not an ordinal (the gate skips
	 * it before comparing), so it is not checked.  A v3 store with a non-NULL
	 * docid and ndict == 0 is therefore refused.  The bitmap read is inside the
	 * region bounded above.
	 */
	if (h.version == 3)
	{
		for (i = 0; i < h.ndocs; i++)
		{
			int64_t		v;

			if (h.null_off != 0 &&
				((base[h.null_off + (size_t) (i >> 3)] >> (i & 7u)) & 1))
				continue;
			memcpy(&v, base + h.values_off + (size_t) i * 8u, sizeof(v));
#ifndef WEAVE_DV_PLANT_NO_ORD_GUARD
			if (v < 0 || (uint64) v >= (uint64) ndict)
				return "dictionary ordinal out of range";
#else
			/* test/fuzz/run.sh teeth build only: the guard above removed */
			(void) v;
#endif
		}
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
	assert(h.version != 3);		/* a text store holds ordinals, not values */
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
 * ---------------------------------------------------------------------------
 * Text (v3) readers.  All of them assume a store weave_docvals_validate() has
 * accepted; they assert rather than re-check, exactly as the int8 readers do.
 * ---------------------------------------------------------------------------
 */

/*
 * Number of dictionary entries.  0 for a v1/v2 store: dict_off is undefined
 * padding there (see the struct comment) and must not be followed.
 */
static inline uint32_t
weave_docvals_ndict(const void *img)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	uint32_t	ndict;

	memcpy(&h, img, sizeof(h));
	if (h.version != 3)
		return 0;
	memcpy(&ndict, base + h.dict_off, sizeof(ndict));
	return ndict;
}

/*
 * Dictionary entry ord: returns a pointer to its first byte (into the image;
 * NOT NUL-terminated, may be the empty string) and sets *len to its byte length
 * offs[ord+1] - offs[ord].  Caller guarantees a v3 store and ord < ndict
 * (asserted); the validator has bounded every offset and the blob within the
 * image, and made offs non-decreasing, so the subtraction cannot wrap.
 */
static inline const unsigned char *
weave_docvals_dict_entry(const void *img, uint32_t ord, uint32_t *len)
{
	WeaveDocvalsHeader h;
	const unsigned char *base = (const unsigned char *) img;
	const unsigned char *offs;
	uint32_t	ndict;
	uint32_t	a;
	uint32_t	b;

	memcpy(&h, img, sizeof(h));
	assert(h.version == 3);
	memcpy(&ndict, base + h.dict_off, sizeof(ndict));
	assert(ord < ndict);
	offs = base + h.dict_off + 4u;
	memcpy(&a, offs + (size_t) ord * 4u, sizeof(a));
	memcpy(&b, offs + ((size_t) ord + 1u) * 4u, sizeof(b));
	*len = b - a;
	return offs + ((size_t) ndict + 1u) * 4u + a;
}

/*
 * Comparator injected into the boundary search: returns <0, 0 or >0 as the
 * byte string a (alen bytes) sorts before, equal to, or after b (blen bytes),
 * under whatever order the dictionary was built with.  The backend passes a
 * varstr_cmp() wrapper under the column collation (ctx carries it); the
 * property test passes a memcmp order.  Injected, rather than hard-wired, so the
 * search -- the off-by-one-prone part -- stays pure and property-testable.
 */
typedef int (*WeaveDvCmp) (void *ctx, const void *a, uint32_t alen,
						   const void *b, uint32_t blen);

/*
 * Resolve a constant k to its two ordinal boundaries over a dictionary D[0,n)
 * strictly ascending under cmp:
 *
 *	 lower_bound(k) = lo = #{ i : D[i] <  k }
 *	 upper_bound(k) = hi = #{ i : D[i] <= k }
 *
 * Because D is strictly ascending, at most one entry equals k, so hi - lo is 1
 * when k is present and 0 when it is absent (between entries, below all: lo ==
 * hi == 0, above all: lo == hi == n).  Both are the standard half-open binary
 * search over [0, n): the only difference is the predicate that moves the lower
 * end, cmp(D[mid], k) < 0 versus <= 0, and swapping the two is exactly the
 * off-by-one weave_dv_eval_ord() would then turn into dropped or admitted rows.
 * A v1/v2 store has ndict == 0 and so returns 0.
 */
static inline uint32_t
weave_dv_dict_lower_bound(const void *img, const void *key, uint32_t klen,
						  WeaveDvCmp cmp, void *ctx)
{
	uint32_t	lo = 0;
	uint32_t	hi = weave_docvals_ndict(img);

	while (lo < hi)
	{
		uint32_t	mid = lo + (hi - lo) / 2u;
		uint32_t	elen;
		const unsigned char *e = weave_docvals_dict_entry(img, mid, &elen);

		if (cmp(ctx, e, elen, key, klen) < 0)
			lo = mid + 1u;
		else
			hi = mid;
	}
	return lo;
}

static inline uint32_t
weave_dv_dict_upper_bound(const void *img, const void *key, uint32_t klen,
						  WeaveDvCmp cmp, void *ctx)
{
	uint32_t	lo = 0;
	uint32_t	hi = weave_docvals_ndict(img);

	while (lo < hi)
	{
		uint32_t	mid = lo + (hi - lo) / 2u;
		uint32_t	elen;
		const unsigned char *e = weave_docvals_dict_entry(img, mid, &elen);

		if (cmp(ctx, e, elen, key, klen) <= 0)
			lo = mid + 1u;
		else
			hi = mid;
	}
	return lo;
}

/*
 * The ordinal-range evaluator for a v3 (text) store: exactly the output
 * contract of weave_dv_eval_int8() -- GLOBAL docids in ascending order, NULL
 * docids skipped before any comparison (spec sect. 6), writes guarded by outcap,
 * the TRUE match count returned -- but the predicate is on the stored ordinal o,
 * given the constant's boundaries lo = #{D[i] < k}, hi = #{D[i] <= k}:
 *
 *	 value <  k	 <=>  o <  lo	(D[o] < k exactly for the first lo entries)
 *	 value <= k	 <=>  o <  hi	(D[o] <= k exactly for the first hi entries)
 *	 value =  k	 <=>  lo <= o < hi	(empty when k is absent: hi == lo)
 *	 value >= k	 <=>  o >= lo
 *	 value >  k	 <=>  o >= hi
 *
 * The mapping holds because ordinal order IS the dictionary order (D strictly
 * ascending, one ordinal per distinct value), so {o : D[o] < k} is the prefix
 * [0, lo) and {o : D[o] <= k} the prefix [0, hi).  THIS is the one place an
 * off-by-one silently drops or admits rows (doc/specs/DOCVALS_CHANNEL.md sect.
 * 7): using lo where hi belongs changes the answer only for the rows equal to
 * k, which every result still "looks" plausible without -- so it is checked by
 * the property test against a reference computed from the strings directly.
 * The comparison is done in int64 so a large uint32 boundary is not mis-signed.
 * It does NOT make a negative ordinal harmless (-1 would satisfy LT and LE):
 * only the validator's "every non-NULL ordinal in [0, ndict)" rule keeps one
 * out, which is why this reader is only ever handed a validated image.
 */
static inline int
weave_dv_eval_ord(const void *img, WeaveDvStrat op, uint32_t lo, uint32_t hi,
				  uint64_t *out, uint32_t outcap)
{
	WeaveDocvalsHeader h;
	int64_t		slo = (int64_t) lo;
	int64_t		shi = (int64_t) hi;
	uint32_t	n;
	uint32_t	i;
	int			count = 0;

	/* memcpy the header: img need not be aligned (see the struct comment). */
	memcpy(&h, img, sizeof(h));
	assert(h.version == 3);		/* ordinals mean nothing in an int8 store */
	n = h.ndocs;

	for (i = 0; i < n; i++)
	{
		int64_t		o;
		int			match;

		/* A NULL docid satisfies no comparison; skip before the op switch. */
		if (weave_docvals_isnull(img, i))
			continue;

		o = weave_docvals_int8(img, i);

		switch (op)
		{
			case WEAVE_DV_LT:
				match = (o < slo);
				break;
			case WEAVE_DV_LE:
				match = (o < shi);
				break;
			case WEAVE_DV_EQ:
				match = (o >= slo && o < shi);
				break;
			case WEAVE_DV_GE:
				match = (o >= slo);
				break;
			case WEAVE_DV_GT:
				match = (o >= shi);
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
	h->dict_off = 0;			/* former padding; zeroed on every write */

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
	h->dict_off = 0;			/* former padding; zeroed on every write */

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

/*
 * Bytes a v3 (text) store occupies: the v2 shape (header + ordinals + docids,
 * plus the MAXALIGNed null bitmap when has_nulls), then the MAXALIGNed
 * dictionary region -- ndict word, ndict+1 offsets, blob bytes.  Mirrors the
 * layout weave_docvals_build_text() produces and the validator recomputes.
 */
static inline size_t
weave_docvals_store_len_text(uint32_t ndocs, int has_nulls, uint32_t ndict,
							 size_t blob)
{
	size_t		body = weave_docvals_store_len_nulls(ndocs, has_nulls);

	return (size_t) WEAVE_DV_MAXALIGN(body)
		+ 4u					/* ndict */
		+ ((size_t) ndict + 1u) * 4u	/* offs[ndict + 1] */
		+ blob;
}

/*
 * Fill a version-3 (text) store into buf, which must be at least
 * weave_docvals_store_len_text(ndocs, nullbits != NULL, ndict, offs[ndict])
 * bytes.  ords[] are the per-doc dictionary ordinals (a NULL doc's slot is
 * written as given; the validator ignores it), nullbits is as for
 * weave_docvals_build_nulls(), offs has ndict + 1 entries (offs[0] == 0), and
 * blob holds offs[ndict] bytes.  Every padding byte between regions is zeroed,
 * as the backend writer must, so the image is fully determined by its inputs.
 * The header is assembled locally and memcpy'd, so buf need not be aligned.
 */
static inline void
weave_docvals_build_text(void *buf, const int64_t *ords, const uint64_t *docids,
						 const uint8_t *nullbits, uint32_t ndocs,
						 const uint32_t *offs, uint32_t ndict,
						 const unsigned char *blob)
{
	WeaveDocvalsHeader h;
	unsigned char *base = (unsigned char *) buf;
	size_t		voff = WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	size_t		doff = voff + (size_t) ndocs * 8u;
	size_t		dend = doff + (size_t) ndocs * 8u;
	size_t		noff = WEAVE_DV_MAXALIGN(dend);
	size_t		bend = (nullbits != NULL) ? noff + (size_t) ((ndocs + 7u) / 8u) : dend;
	size_t		dictoff = WEAVE_DV_MAXALIGN(bend);
	size_t		offsbase = dictoff + 4u;
	size_t		blobbase = offsbase + ((size_t) ndict + 1u) * 4u;
	uint32_t	i;

	memset(&h, 0, sizeof(h));
	h.magic = WEAVE_DOCVALS_MAGIC;
	h.version = 3;
	h.typid_kind = WEAVE_DV_KIND_TEXT;
	h.ndocs = ndocs;
	h.null_off = (nullbits != NULL) ? (uint32) noff : 0;
	h.zonemap_off = 0;
	h.values_off = (uint32) voff;
	h.docids_off = (uint32) doff;
	h.dict_off = (uint32) dictoff;
	memcpy(base, &h, sizeof(h));

	for (i = 0; i < ndocs; i++)
		memcpy(base + voff + (size_t) i * 8u, &ords[i], sizeof(int64_t));
	for (i = 0; i < ndocs; i++)
		memcpy(base + doff + (size_t) i * 8u, &docids[i], sizeof(uint64_t));

	/* Zero everything from the end of the docids to the dictionary region. */
	memset(base + dend, 0, dictoff - dend);
	if (nullbits != NULL)
	{
		for (i = 0; i < ndocs; i++)
			if (nullbits[i])
				base[noff + (i >> 3)] |= (unsigned char) (1u << (i & 7u));
	}

	memcpy(base + dictoff, &ndict, sizeof(uint32_t));
	for (i = 0; i <= ndict; i++)
		memcpy(base + offsbase + (size_t) i * 4u, &offs[i], sizeof(uint32_t));
	if (offs[ndict] > 0)
		memcpy(base + blobbase, blob, offs[ndict]);
}

#endif							/* WEAVE_DOCVALS_TEST_HELPERS */

#endif							/* WEAVE_DOCVALS_H */
