/*-------------------------------------------------------------------------
 *
 * pagekind.h
 *		The weave page-kind space: encoding, decoding, and the escape bit.
 *
 * Split out of weave/am.h with NO PostgreSQL dependency so it is a
 * property-testable core (include/weave/for.h is the exemplar).  The reason is
 * concrete: the v6 design argument for the escape bit rests on a claim about what
 * an OLDER binary does when it meets a new page kind, and a claim like that
 * belongs in a test rather than in a paragraph.
 * test/hegel/test_pagekind.c proves it exhaustively over all 2^16 flag words.
 *
 * HOW THE KIND SPACE WORKS, AND WHY IT IS NOT A FLAT BITMAP ANY MORE.
 *
 * `flags` is a uint16 and bits 0-9 are already spent on the ten shipped kinds.
 * The vector channel wants four more and the fuzzy channel wants four, plus
 * docvalues and corpus-trigrams: taken literally that is bits 10-19 of a 16-bit
 * field, so shipping either channel on the old scheme bakes in a collision
 * (doc/specs/SEGMENT_FORMAT.md sect. 2, blocking gate 5).
 *
 * v6 resolves it with a RESERVED ESCAPE BIT selecting an extended kind space,
 * not by widening `flags`:
 *
 *		flags bit 15	WEAVE_PAGE_KIND_EXT.  Clear => bits 0-7 and 9 are the
 *						LEGACY one-hot kind bitmap, exactly as v4/v5 wrote them.
 *						Set => the kind is the integer in WeavePageOpaqueData.kind
 *						and every legacy kind bit MUST be zero.
 *		flags bit 8		WEAVE_FREED.  A STATE, not a kind, and valid under both
 *						encodings -- a freed posting page is POSTING|FREED.
 *		flags 10-14		reserved, must be zero.  These are the bits the vector
 *						channel wanted; they are deliberately NOT allocated.
 *		opaque.kind		the extended kind (was the `unused` uint16).
 *
 * Widening `flags` to uint32 was the alternative and it is worse: it moves the
 * page opaque area on EVERY page of EVERY existing index, so it forces a REINDEX
 * on relations that contain no new page kind at all.
 *
 * The three properties that decided it, in order:
 *
 * 1. IT IS THE L17 PATTERN, WHICH ALREADY WORKED HERE.  L17 discriminated two
 *    doclen-sidecar encodings with a flag in the spare high bits of an existing
 *    count field, PER BLOCK, because an index upgraded from <= 0.5.0 keeps its
 *    old pages while later merges write new ones -- a per-index version cannot
 *    describe a mixed relation.  Page kinds have exactly that shape: after an
 *    upgrade one relation holds v5-written lexical pages and v6-written channel
 *    pages simultaneously, so the discriminator has to be PER PAGE.
 *
 * 2. IT FAILS CLOSED AGAINST AN OLDER .so.  In the extended encoding all legacy
 *    kind bits are zero, so a v5 binary that reaches such a page matches no kind
 *    and treats it as absent/unclassified.  Had the kind integer been laid into
 *    the LOW bits of `flags` instead, kind 20 (0b10100) would have read as
 *    POSTING|TRGM to that binary -- a wrong answer rather than a refusal.  (The
 *    metapage version gate refuses first; this is the second line of defence,
 *    the same two-layer argument as WEAVE_DOCLEN_ABS.)  This is the property
 *    test/hegel/test_pagekind.c asserts for every allocated kind.
 *
 * 3. THE SHIPPED KINDS DO NOT MOVE.  A v6 build keeps writing the legacy bitmap
 *    for all ten existing kinds, so a lexical page written by v6 is BYTE-
 *    IDENTICAL to one written by v5.  Only genuinely new kinds use the escape.
 *    That is why v6 needs no page rewrite and why `flags & WEAVE_DOCLEN` style
 *    reads in the hot path keep working.
 *
 * The cost, stated: the extended space does not read the kind out of `flags`, so
 * NOTHING may test a new kind with a bitwise AND.  Read every kind through
 * weave_page_kind_decode() (or WeavePageGetKind()/WeavePageHasKind() in
 * weave/am.h).  The reason this matters is on the record: the L17 change put a
 * flag in the high bits of WeaveDoclenBlockHdr.count and one validator kept
 * comparing the RAW count, which built an empty page directory -- and the only
 * symptom was slightly wrong BM25 scores and a pagination disagreement at row
 * 300.  Same field, same class of bug.
 *
 * Adding a kind means: editing the table in doc/specs/SEGMENT_FORMAT.md sect. 2,
 * adding the id below (channel headers alias it beside the struct they
 * describe), and extending weave_check() with an invariant for it.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/pagekind.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_PAGEKIND_H
#define WEAVE_PAGEKIND_H

#include <stdint.h>

/* Legacy one-hot kind bits (v4/v5 and still what v6 writes for these kinds). */
#define WEAVE_META			(1 << 0)
#define WEAVE_DICT			(1 << 1)
#define WEAVE_POSTING		(1 << 2)
#define WEAVE_PENDING		(1 << 3)
#define WEAVE_TRGM			(1 << 4)	/* trigram directory page */
#define WEAVE_TRGM_DATA		(1 << 5)	/* trigram sparsemap blob page */
#define WEAVE_LIVEDOCS		(1 << 6)	/* per-segment tombstone bitmap page.
										 * ALLOCATED BUT NEVER WRITTEN: the
										 * livedocs blob goes out through
										 * weave_write_blob(), which lays it on
										 * WEAVE_TRGM_DATA pages.  See
										 * doc/specs/SEGMENT_FORMAT.md sect. 2. */
#define WEAVE_DICTINDEX		(1 << 7)	/* sparse block index over dict pages */
#define WEAVE_FREED			(1 << 8)	/* page freed & pending recycle: nextblk
										 * holds the free-time TransactionId (see
										 * weave_free_page / the recycle gate in
										 * weave_new_buffer).  Reusing nextblk (dead
										 * on an off-chain freed page) keeps the page
										 * opaque layout unchanged -- no format
										 * change, so existing indexes need no
										 * REINDEX.  A page freed by an older version
										 * lacks this flag and is treated as
										 * immediately recyclable.
										 * A STATE, not a kind: it is set while
										 * LEAVING the page's kind in place, and it is
										 * the one flag valid under both encodings. */
#define WEAVE_DOCLEN		(1 << 9)	/* per-segment doclen sidecar page (v4):
										 * a chain of 128-doc blocks, each a
										 * FOR-packed docid column + one quantized
										 * length byte per doc, ordered by
										 * segment-local ascending docid.  Replaces
										 * the per-posting doclen column; scoring
										 * reads the byte and a precomputed
										 * 256-entry length-norm table. */

/* Every legacy kind bit, i.e. `flags` minus the state and reserved bits. */
#define WEAVE_PAGE_KIND_MASK \
	(WEAVE_META | WEAVE_DICT | WEAVE_POSTING | WEAVE_PENDING | \
	 WEAVE_TRGM | WEAVE_TRGM_DATA | WEAVE_LIVEDOCS | WEAVE_DICTINDEX | \
	 WEAVE_DOCLEN)

#define WEAVE_PAGE_KIND_EXT		(1 << 15)	/* escape: kind is in opaque.kind */
#define WEAVE_PAGE_STATE_MASK	WEAVE_FREED
/* Bits 10-14: unallocated, must read as zero under either encoding. */
#define WEAVE_PAGE_RESERVED_MASK \
	((uint16_t) ~(WEAVE_PAGE_KIND_MASK | WEAVE_PAGE_STATE_MASK | WEAVE_PAGE_KIND_EXT))

/*
 * The canonical page kind.  Ids 1-15 name the legacy kinds and are IN-MEMORY
 * ONLY (their on-disk representation is the one-hot bit above).  Ids >=
 * WEAVE_PK_EXT_FIRST are the extended space and the id IS the byte pattern
 * stored in WeavePageOpaqueData.kind, so these numbers are on-disk ABI: never
 * renumber one, only append.
 *
 * The reserved channel ids are here rather than in the channel headers so that
 * the whole space is visible in one place and a second channel cannot quietly
 * pick a number the first one took.  weave/vector.h and the fuzzy headers alias
 * them (WEAVE_VMETA and friends) beside the structs they describe.
 */
typedef enum WeavePageKind
{
	WEAVE_PK_UNKNOWN = 0,		/* unrecognized, inconsistent, or torn */

	WEAVE_PK_META = 1,
	WEAVE_PK_DICT = 2,
	WEAVE_PK_POSTING = 3,
	WEAVE_PK_PENDING = 4,
	WEAVE_PK_TRGM = 5,
	WEAVE_PK_TRGM_DATA = 6,
	WEAVE_PK_LIVEDOCS = 7,
	WEAVE_PK_DICTINDEX = 8,
	WEAVE_PK_DOCLEN = 9,

	WEAVE_PK_EXT_FIRST = 16,	/* first id stored in opaque.kind */

	WEAVE_PK_CHANDESC = 16,		/* v6 per-bolt weft descriptor page */
	WEAVE_PK_VMETA = 17,		/* reserved, vector: WeaveVecMeta */
	WEAVE_PK_VCODES = 18,		/* reserved, vector: packed quantized codes */
	WEAVE_PK_VGRAPH = 19,		/* reserved, vector: Vamana CSR adjacency */
	WEAVE_PK_VRERANK = 20,		/* reserved, vector: full-precision sidecar */
	WEAVE_PK_SURF = 21,			/* reserved, fuzzy: LOUDS-Sparse vocabulary trie */
	WEAVE_PK_ULEV = 22,			/* reserved, fuzzy: universal-Levenshtein aux */
	WEAVE_PK_REGEX = 23,		/* reserved, fuzzy: compiled-pattern cache */
	WEAVE_PK_FUZZY_SPARE = 24,	/* reserved, fuzzy */
	WEAVE_PK_DOCVALS = 25,		/* reserved: scalar/facet forward store */
	WEAVE_PK_CGRAM = 26,		/* reserved: opt-in corpus character trigrams */

	WEAVE_PK_NKINDS				/* first unassigned id; not a kind */
} WeavePageKind;

/*
 * Decode a page's kind from the two opaque words.  Split from the Page accessor
 * so a validator, a fuzz target, or weave_check() can decode bytes it read
 * itself.  Returns WEAVE_PK_UNKNOWN for anything inconsistent -- zero kind bits,
 * two kind bits, a reserved bit set, an out-of-range extended id -- because on
 * disk bytes are not trusted and "I do not recognize this" must be
 * representable.
 */
static inline WeavePageKind
weave_page_kind_decode(uint16_t flags, uint16_t kind)
{
	if ((flags & WEAVE_PAGE_RESERVED_MASK) != 0)
		return WEAVE_PK_UNKNOWN;

	if ((flags & WEAVE_PAGE_KIND_EXT) != 0)
	{
		/* extended encoding: no legacy kind bit may be set */
		if ((flags & WEAVE_PAGE_KIND_MASK) != 0)
			return WEAVE_PK_UNKNOWN;
		if (kind < (uint16_t) WEAVE_PK_EXT_FIRST ||
			kind >= (uint16_t) WEAVE_PK_NKINDS)
			return WEAVE_PK_UNKNOWN;
		return (WeavePageKind) kind;
	}

	/* legacy encoding: exactly one kind bit.  The switch rejects both zero bits
	 * and two-or-more bits without a popcount. */
	switch (flags & WEAVE_PAGE_KIND_MASK)
	{
		case WEAVE_META:
			return WEAVE_PK_META;
		case WEAVE_DICT:
			return WEAVE_PK_DICT;
		case WEAVE_POSTING:
			return WEAVE_PK_POSTING;
		case WEAVE_PENDING:
			return WEAVE_PK_PENDING;
		case WEAVE_TRGM:
			return WEAVE_PK_TRGM;
		case WEAVE_TRGM_DATA:
			return WEAVE_PK_TRGM_DATA;
		case WEAVE_LIVEDOCS:
			return WEAVE_PK_LIVEDOCS;
		case WEAVE_DICTINDEX:
			return WEAVE_PK_DICTINDEX;
		case WEAVE_DOCLEN:
			return WEAVE_PK_DOCLEN;
		default:
			return WEAVE_PK_UNKNOWN;
	}
}

/* The legacy one-hot bit for a kind, or 0 if the kind lives in the extended
 * space.  Single source of truth for the encoder; keep in step with
 * weave_page_kind_decode(). */
static inline uint16_t
weave_page_kind_legacy_bit(WeavePageKind k)
{
	switch (k)
	{
		case WEAVE_PK_META:
			return WEAVE_META;
		case WEAVE_PK_DICT:
			return WEAVE_DICT;
		case WEAVE_PK_POSTING:
			return WEAVE_POSTING;
		case WEAVE_PK_PENDING:
			return WEAVE_PENDING;
		case WEAVE_PK_TRGM:
			return WEAVE_TRGM;
		case WEAVE_PK_TRGM_DATA:
			return WEAVE_TRGM_DATA;
		case WEAVE_PK_LIVEDOCS:
			return WEAVE_LIVEDOCS;
		case WEAVE_PK_DICTINDEX:
			return WEAVE_DICTINDEX;
		case WEAVE_PK_DOCLEN:
			return WEAVE_DOCLEN;
		default:
			return 0;
	}
}

/*
 * Encode a kind into the two opaque words.  Inverse of the decoder for every
 * allocated kind; asserted exhaustively by test/hegel/test_pagekind.c.
 */
static inline void
weave_page_kind_encode(WeavePageKind k, uint16_t *flags, uint16_t *kind)
{
	uint16_t	bit = weave_page_kind_legacy_bit(k);

	if (bit != 0)
	{
		*flags = bit;
		*kind = 0;
	}
	else
	{
		*flags = WEAVE_PAGE_KIND_EXT;
		*kind = (uint16_t) k;
	}
}

#endif							/* WEAVE_PAGEKIND_H */
