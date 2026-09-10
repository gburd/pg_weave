/*-------------------------------------------------------------------------
 *
 * chandesc.h
 *		Pure structural validator for an on-disk WEAVE_CHANDESC page image.
 *
 * A v6 bolt SELF-DESCRIBES which wefts it carries: WeaveSegMeta.chandesc names
 * a WEAVE_CHANDESC page holding a header plus an array of weft descriptors.
 * That is what makes an index built without a vector column cost literally zero
 * vector bytes, and what stops a reader inferring a weft's geometry from a GUC
 * that may have changed since the build (doc/specs/SEGMENT_FORMAT.md sect. 6).
 *
 * This header carries the layout and the validator as standalone C with NO
 * PostgreSQL dependencies, so the fuzz/corruption harness (test/fuzz/) and the
 * property test (test/hegel/) can exercise the exact bytes the backend parses.
 * include/weave/docvalid.h is the exemplar for this split, and the reason is the
 * same: on-disk bytes are not trusted (doc/CONVENTIONS.md), and a validator that
 * can only run inside a backend cannot be fuzzed.
 *
 * The struct layouts here MUST stay byte-identical to the backend structs in
 * weave/am.h.  StaticAssertStmt()s in src/am/am.c (weave_chandesc_from_page)
 * fail the build on drift; they are the guard, not the comment.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/chandesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_CHANDESC_H
#define WEAVE_CHANDESC_H

#include <stddef.h>
#include <stdint.h>

#define WEAVE_CHANDESC_MAGIC	0x57434431	/* "WCD1" */
#define WEAVE_CHANDESC_VERSION	1

/*
 * Maximum wefts one bolt can describe.  Deliberately small: the descriptor
 * array must fit one page with room to spare, the fused scorer's essential /
 * non-essential split is over this set, and a bolt with 32 wefts is a design
 * error rather than a configuration.  Hitting the cap is an ERROR, not a silent
 * truncation -- the same stance WEAVE_MAX_SEGMENTS takes.
 */
#define WEAVE_MAX_WEFTS			32

/*
 * WEFT kinds -- what a bolt stores.  NOT the same enum as WeaveChannelKind in
 * weave/channel.h, which enumerates what a SHUTTLE does at scan time
 * (WEAVE_CH_VECTOR_SCAN and WEAVE_CH_VECTOR_GRAPH are two scan strategies over
 * ONE stored weft; WEAVE_CH_POSITION is a scan over the lexical weft's fourth
 * column, not a weft of its own).  Conflating them would put a scan strategy on
 * disk, and doc/specs/SEGMENT_FORMAT.md sect. 6 does not say which enum it meant.
 *
 * 0 is reserved and invalid on purpose: a zeroed or torn page must not validate
 * as "one weft of kind 0".
 */
typedef enum WeaveWeftKind
{
	WEAVE_WK_INVALID = 0,
	WEAVE_WK_LEXICAL = 1,		/* dictionary + postings; root = dictstart */
	WEAVE_WK_VECTOR = 2,		/* reserved (V): root = WEAVE_VMETA page */
	WEAVE_WK_FUZZY = 3,			/* reserved (Z): root = WEAVE_SURF page */
	WEAVE_WK_DOCVALS = 4,		/* reserved: root = WEAVE_DOCVALS page */
	WEAVE_WK_CGRAM = 5,			/* reserved: root = WEAVE_CGRAM page */
	WEAVE_WK_NKINDS				/* first unassigned id; not a kind */
} WeaveWeftKind;

/* WeaveChannelDesc.flags.  Unknown bits are an ERROR, not ignored: a reader that
 * ignores a flag it does not understand is doing a best-effort read of a format
 * it does not understand (doc/CONVENTIONS.md decision 3). */
#define WEAVE_WEFT_F_ALL		0x00000000u

/*
 * One weft descriptor.  Fixed-width, 12 bytes, no padding on any supported
 * platform (4-byte alignment throughout).
 */
typedef struct WeaveCdDesc
{
	uint16_t	kind;			/* WeaveWeftKind; never 0 */
	uint16_t	attnum;			/* 1-based index attribute, or 0 for a weft that
								 * is not tied to one attribute */
	uint32_t	flags;			/* WEAVE_WEFT_F_*; unknown bits rejected */
	uint32_t	root;			/* first page of the weft (BlockNumber) */
} WeaveCdDesc;

/*
 * WEAVE_CHANDESC page contents, at PageGetContents(page).
 *
 * `reserved` is not decoration: it keeps the descriptor array 8-byte aligned and
 * gives the next format generation a place to put a count without moving the
 * array.  It must read as zero, so a future writer that sets it is refused by
 * today's reader instead of silently misparsed.
 */
typedef struct WeaveCdPage
{
	uint32_t	magic;			/* WEAVE_CHANDESC_MAGIC */
	uint16_t	version;		/* WEAVE_CHANDESC_VERSION */
	uint16_t	nweft;			/* descriptors that follow; 1..WEAVE_MAX_WEFTS */
	uint32_t	reserved;		/* must be zero */
	/* WeaveCdDesc weft[nweft] follows */
} WeaveCdPage;

#define WEAVE_CD_HDRSIZE		(sizeof(WeaveCdPage))
#define WEAVE_CD_SIZE(n)		(WEAVE_CD_HDRSIZE + (size_t) (n) * sizeof(WeaveCdDesc))
#define WEAVE_CD_INVALID_BLK	((uint32_t) 0xFFFFFFFFu)	/* InvalidBlockNumber */

/*
 * Why the validator returns a code rather than a bool: the backend wrapper turns
 * it into an errdetail, and "which of the eleven ways this page is wrong" is the
 * difference between a diagnosable corruption report and "index is corrupted".
 * Keep weave_chandesc_errstr() in step.
 */
typedef enum WeaveCdError
{
	WEAVE_CD_OK = 0,
	WEAVE_CD_TRUNCATED,			/* header does not fit the readable bytes */
	WEAVE_CD_MAGIC,
	WEAVE_CD_VERSION,
	WEAVE_CD_RESERVED,			/* reserved word nonzero */
	WEAVE_CD_NWEFT,				/* nweft 0, or > WEAVE_MAX_WEFTS */
	WEAVE_CD_ARRAY,				/* descriptor array runs past the readable bytes */
	WEAVE_CD_KIND,				/* weft kind 0 or >= WEAVE_WK_NKINDS */
	WEAVE_CD_ORDER,				/* not strictly ascending by (kind, attnum) */
	WEAVE_CD_FLAGS,				/* unknown flag bits set */
	WEAVE_CD_ROOT,				/* root is Invalid, block 0, or past the relation */
	WEAVE_CD_OVERLAP,			/* two wefts claim the same root block */

	/*
	 * Raised by the backend wrapper, not by the pure body below, but part of the
	 * same enum because they are the same question ("can this descriptor page be
	 * trusted?") and one switch should answer it.
	 */
	WEAVE_CD_BLKRANGE,			/* chandesc block is Invalid or past the relation */
	WEAVE_CD_PAGENEW,			/* the block is uninitialized */
	WEAVE_CD_PAGEKIND			/* the block is not a WEAVE_CHANDESC page */
} WeaveCdError;

static inline const char *
weave_chandesc_errstr(WeaveCdError e)
{
	switch (e)
	{
		case WEAVE_CD_OK:
			return "ok";
		case WEAVE_CD_TRUNCATED:
			return "page too short for a channel-descriptor header";
		case WEAVE_CD_MAGIC:
			return "bad channel-descriptor magic";
		case WEAVE_CD_VERSION:
			return "unsupported channel-descriptor version";
		case WEAVE_CD_RESERVED:
			return "reserved word is not zero";
		case WEAVE_CD_NWEFT:
			return "weft count is zero or above the maximum";
		case WEAVE_CD_ARRAY:
			return "descriptor array runs past the end of the page";
		case WEAVE_CD_KIND:
			return "unknown weft kind";
		case WEAVE_CD_ORDER:
			return "descriptors are not strictly ascending by (kind, attnum)";
		case WEAVE_CD_FLAGS:
			return "unknown weft flag bits";
		case WEAVE_CD_ROOT:
			return "weft root block is invalid or out of relation bounds";
		case WEAVE_CD_OVERLAP:
			return "two wefts claim the same root block";
		case WEAVE_CD_BLKRANGE:
			return "channel-descriptor block is out of relation bounds";
		case WEAVE_CD_PAGENEW:
			return "channel-descriptor block is uninitialized";
		case WEAVE_CD_PAGEKIND:
			return "block is not a channel-descriptor page";
	}
	return "unrecognized channel-descriptor error";
}

/*
 * weave_chandesc_check -- the pure validator body.
 *
 * `base` points at the page contents; `avail` is the bytes readable there (the
 * backend passes pd_lower - contents offset, the fuzzer passes its buffer size);
 * `nblocks` is the relation's block count, or 0 to skip the bounds half of the
 * root check (the property test has no relation).  NEVER reads outside
 * [base, base + avail).
 *
 * Strictly-ascending (kind, attnum) is required rather than merely "no
 * duplicates" because it makes duplicate detection a single comparison against
 * the predecessor -- no O(n^2) scan and no scratch array in a validator that
 * must run on hostile bytes.  The ordering is also what lets a reader binary
 * search for a weft kind once the array grows.
 *
 * The O(n^2) root-overlap scan is deliberate and bounded by WEAVE_MAX_WEFTS: it
 * is the descriptor-level half of the "no two chains overlap" invariant that
 * doc/specs/SEGMENT_FORMAT.md sect. 8 item 5 demands, after the same bug bit a
 * sibling project four times.
 */
static inline WeaveCdError
weave_chandesc_check(const void *base, size_t avail, uint32_t nblocks,
					 uint16_t *nweft_out)
{
	const WeaveCdPage *hdr = (const WeaveCdPage *) base;
	const WeaveCdDesc *weft;
	uint32_t	prevkey = 0;
	uint16_t	i;
	uint16_t	j;

	if (nweft_out != NULL)
		*nweft_out = 0;
	if (base == NULL || avail < WEAVE_CD_HDRSIZE)
		return WEAVE_CD_TRUNCATED;
	if (hdr->magic != WEAVE_CHANDESC_MAGIC)
		return WEAVE_CD_MAGIC;
	if (hdr->version != WEAVE_CHANDESC_VERSION)
		return WEAVE_CD_VERSION;
	if (hdr->reserved != 0)
		return WEAVE_CD_RESERVED;
	if (hdr->nweft == 0 || hdr->nweft > WEAVE_MAX_WEFTS)
		return WEAVE_CD_NWEFT;
	if (WEAVE_CD_SIZE(hdr->nweft) > avail)
		return WEAVE_CD_ARRAY;

	weft = (const WeaveCdDesc *) ((const char *) base + WEAVE_CD_HDRSIZE);
	for (i = 0; i < hdr->nweft; i++)
	{
		uint32_t	key;

		if (weft[i].kind == WEAVE_WK_INVALID ||
			weft[i].kind >= (uint16_t) WEAVE_WK_NKINDS)
			return WEAVE_CD_KIND;
		if ((weft[i].flags & ~WEAVE_WEFT_F_ALL) != 0)
			return WEAVE_CD_FLAGS;

		/* A weft with no root has no descriptor: the whole point of v6 is that
		 * an absent weft costs zero bytes, so an Invalid root here means the
		 * writer emitted a descriptor it should have omitted. */
		if (weft[i].root == WEAVE_CD_INVALID_BLK || weft[i].root == 0)
			return WEAVE_CD_ROOT;
		if (nblocks != 0 && weft[i].root >= nblocks)
			return WEAVE_CD_ROOT;

		key = ((uint32_t) weft[i].kind << 16) | weft[i].attnum;
		if (i > 0 && key <= prevkey)
			return WEAVE_CD_ORDER;
		prevkey = key;

		for (j = 0; j < i; j++)
			if (weft[j].root == weft[i].root)
				return WEAVE_CD_OVERLAP;
	}

	if (nweft_out != NULL)
		*nweft_out = hdr->nweft;
	return WEAVE_CD_OK;
}

#endif							/* WEAVE_CHANDESC_H */
