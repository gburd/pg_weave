/*-------------------------------------------------------------------------
 *
 * vecpage.h
 *		The vector weft's on-page geometry: coordinate-sliced code strips.
 *
 * Backend-independent on purpose, like weave/for.h, weave/surftrie.h and
 * weave/uleven.h: everything here is arithmetic over caller-supplied buffers, so
 * test/hegel/test_vecpage.c can prove the round trip and the determinism property
 * without a running backend.  The AM side (allocating pages, GenericXLog, the
 * chain walk) is task V7 in src/vector/ and src/am/.
 *
 * THE SHAPE, and why it is this one.  A 32-lane block at the ratified 4 bits is
 * 16,124 bytes at 960 dimensions and does not fit an 8 kB page; at 1536-d it needs
 * four.  doc/specs/VECTOR_CHANNEL.md sect. 7.1 has the arithmetic and the rejected
 * alternative.  The ratified shape (2026-09-15) is that one page holds one
 * COORDINATE RANGE of one block's 32 lanes:
 *
 *	 - In WEAVE_PACK_LANE, code (coordinate j, lane s) is at bit (j*32 + s)*bits,
 *	   so coordinate j occupies exactly 4*bits BYTES -- integral at every supported
 *	   width.  A page break on a coordinate boundary splits nothing.
 *	 - The byte-LUT kernel walks coordinates in order and widens its accumulators
 *	   every <= 256 of them, so a strip boundary is a place it already pauses.  No
 *	   copy and no scratch buffer to reassemble a spanning block.
 *	 - WEAVE_PACK_VECMAJOR CANNOT be stored this way: a vector's coordinates are
 *	   contiguous there, so a coordinate cut splits vectors.  Every function here
 *	   refuses that layout rather than producing a page a reader would misread.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/vecpage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_VECPAGE_H
#define WEAVE_VECPAGE_H

#include "weave/quantize.h"

/*
 * Bytes one coordinate of a block occupies: 32 lanes at `bits` each.  Integral
 * for every supported width because 32*bits/8 == 4*bits, which is the property
 * the whole strip format rests on -- assert it rather than trusting the comment.
 */
static inline int
weave_strip_coordbytes(int bits)
{
	return 4 * bits;
}

/* The 12-byte header every strip page carries.
 *
 * Self-describing per PAGE, not per index, following L17's precedent: after an
 * upgrade one relation holds pages written by two generations, so a per-index
 * discriminator cannot describe it.  It also lets weave_check() validate a page in
 * isolation and stops a reader mistaking one block's strip for another's.
 *
 * Fixed layout, little-endian on the wire is NOT assumed: the AM writes this
 * struct into the page as a struct (same-architecture reads), and the
 * cross-architecture fixture in test/hegel/ hashes the CODE bytes, which are
 * produced by weave_pack_lane() and are architecture-independent by V2's gate.
 */
typedef struct WeaveVecStripHdr
{
	weave_uint32 blockno;		/* which block of this segment's vector weft */
	weave_uint16 j0;			/* first coordinate stored on this page */
	weave_uint16 ncoords;		/* coordinates stored on this page, >= 1 */
	weave_uint16 flags;			/* WEAVE_VSTRIP_F_* */
	weave_uint16 pad;			/* must be zero: it is part of the page image and a
								 * nonzero here would make two indexes holding the
								 * same vectors differ on disk */
} WeaveVecStripHdr;

#define WEAVE_VSTRIP_F_CENTROID 0x0001	/* this strip holds the block's centroid
										 * code (one lane's worth per coordinate),
										 * not the 32 lanes.  Read only by the (B3)
										 * bound, which measured 0.00% pruning on
										 * real corpora, so a non-pruning scan never
										 * touches these pages. */

/*
 * How many coordinates fit on a page with `usable` bytes of payload space.
 * Returns 0 when not even one does, which is a refusal the caller must honour:
 * at WEAVE_MAX_DIM (16,384) and 4 bits a centroid code alone is 8,192 bytes, so
 * "does it fit" is a real question and not a formality.
 */
static inline int
weave_strip_coords_per_page(int usable, int bits)
{
	int			avail = usable - (int) sizeof(WeaveVecStripHdr);
	int			cb = weave_strip_coordbytes(bits);

	if (avail <= 0 || cb <= 0)
		return 0;
	return avail / cb;
}

/* Strips needed to store `ncoord` coordinates at this page capacity. */
static inline int
weave_strip_count(int ncoord, int coords_per_page)
{
	if (coords_per_page <= 0)
		return 0;
	return (ncoord + coords_per_page - 1) / coords_per_page;
}

/*
 * The fixed per-block directory record: everything about a block that is NOT
 * dim-dependent, so its size is constant and record `i` is at a computable
 * offset.  That O(1) addressing is what "score block i" needs, and it is why the
 * dim-wide centroid code is not here (it would make the record variable AND
 * exceed a page at WEAVE_MAX_DIM).
 *
 * All five bound fields must be maintained by every path that mutates a block --
 * insert, vacuum's lane zero, merge -- or the bound stops being an upper bound and
 * the fused scorer silently drops rows.  That is contract (C2) in
 * include/weave/channel.h, and it is a correctness requirement rather than
 * bookkeeping: no fixed-output regression test can catch a bound that is 1 % too
 * low, because the answers stay plausible.
 */
typedef struct WeaveVecDirRec
{
	weave_uint32 firstwarp;		/* warp position of lane 0 */
	weave_uint32 livemask;		/* bit i set = lane i occupied */
	float		smax;			/* max renormalization scale over live lanes */
	float		maxrecnorm;		/* bound (B2) */
	float		minnorm;		/* min ||v|| over live lanes (L2 bound) */
	float		censcale;		/* the centroid's own renormalization scale */
	float		cenrad;			/* R, bound (B3) */
	float		lane[2 * WEAVE_VEC_BLOCK];	/* interleaved (scale, norm) per lane,
											 * the WeaveVecLane pair flattened so
											 * this header needs no backend type */
} WeaveVecDirRec;

/*
 * Header on each directory page.  A page whose records can only be located by
 * arithmetic done elsewhere cannot be validated in isolation, which is what
 * weave_check() needs and what L17's per-object-discriminator precedent asks for.
 * Eight bytes buys the cross-check that `first_blockno` is where the O(1) formula
 * says this page starts -- so a mislinked directory chain is caught instead of
 * silently answering for the wrong blocks.
 */
typedef struct WeaveVecDirHdr
{
	weave_uint32 first_blockno; /* block of record 0 on this page */
	weave_uint16 nrecs;			/* records present, >= 1 */
	weave_uint16 pad;			/* must be zero: part of the page image */
} WeaveVecDirHdr;

static inline int
weave_vecdir_recs_per_page(int usable)
{
	int			avail = usable - (int) sizeof(WeaveVecDirHdr);

	if (avail < (int) sizeof(WeaveVecDirRec))
		return 0;
	return avail / (int) sizeof(WeaveVecDirRec);
}

/* Which directory page holds block `blockno`, and which slot on it.  O(1), which
 * is the whole reason the record is fixed-size. */
static inline int
weave_vecdir_page_index(weave_uint32 blockno, int rpp)
{
	return rpp > 0 ? (int) (blockno / (weave_uint32) rpp) : -1;
}

static inline int
weave_vecdir_slot_index(weave_uint32 blockno, int rpp)
{
	return rpp > 0 ? (int) (blockno % (weave_uint32) rpp) : -1;
}

/*
 * Zero a directory page and write its header.  SEPARATE from
 * weave_vecdir_write() because a page holds many records and only the first
 * writer may zero it -- but the zeroing is not optional, for the same reason
 * weave_strip_build() zeroes: the slack past the last record (208 bytes at an
 * 8,160-byte page) is part of the page image, so a GenericXLog delta and any
 * fixture hash depend on it.  Returns 0 or -1.
 */
extern int weave_vecdir_page_init(void *dst, size_t dstlen, int usable,
								  weave_uint32 first_blockno, int nrecs);

/* Write one record into slot `slot` of an initialized page.  Refuses a slot past
 * the header's nrecs, and refuses a record whose floats are not finite -- a NaN
 * bound is not an upper bound. */
extern int weave_vecdir_write(void *dst, size_t dstlen, int usable, int slot,
							  const WeaveVecDirRec *rec);

/*
 * Read record `slot`.  Validates the page header, the slot, and every float,
 * because on-disk bytes are not trusted (doc/CONVENTIONS.md decision 2) and a
 * non-finite or negative bound field would make the (C2) comparison meaningless
 * rather than merely wrong.  Returns 0 or -1 with *why set.
 */
extern int weave_vecdir_read(const void *src, size_t srclen, int usable, int slot,
							 WeaveVecDirRec *out, const char **why);

/*
 * Build one strip's payload into `dst`, which must have room for
 * sizeof(WeaveVecStripHdr) + ncoords * weave_strip_coordbytes(bits) bytes.
 *
 * `block` is a full packed block as produced by weave_pack_lane() -- at least
 * weave_block_codebytes(dim, bits) bytes.  The copy is a straight byte range
 * because coordinate j starts at byte j * 4*bits in WEAVE_PACK_LANE.
 *
 * ZEROES THE WHOLE DESTINATION FIRST, including the header's pad and any bytes
 * after the payload the caller reserved.  That is not hygiene, it is the
 * determinism requirement doc/specs/VECTOR_CHANNEL.md sect. 7 records against V7:
 * weave_block_codebytes() rounds up per lane, so a block buffer has 0-28 slack
 * bytes no pack function ever writes, and a page image containing them is
 * nondeterministic -- which breaks both a GenericXLog delta over a rewritten page
 * and any cross-architecture fixture hash.
 *
 * Returns the bytes written, or -1 on a refusal: VECMAJOR layout (a coordinate cut
 * splits vectors there), a coordinate range outside the block, or ncoords < 1.
 */
extern int weave_strip_build(void *dst, size_t dstlen, WeavePackLayout layout,
							 int dim, int bits, weave_uint32 blockno,
							 int j0, int ncoords, weave_uint16 flags,
							 const weave_uint8 *block);

/*
 * Parse and validate a strip page payload.  On success returns the number of
 * coordinates and points `*codes` at the first code byte; on any inconsistency
 * returns -1 and sets *why to a static string.  Validation is not optional: on-disk
 * bytes are not trusted (doc/CONVENTIONS.md decision 2), and a strip whose j0 or
 * ncoords is wrong would silently score the wrong coordinates rather than fail.
 */
extern int weave_strip_parse(const void *src, size_t srclen, int dim, int bits,
							 WeaveVecStripHdr *hdr, const weave_uint8 **codes,
							 const char **why);

/*
 * Copy a parsed strip's codes back into the right byte range of a block buffer.
 * The inverse of weave_strip_build(), and what a reader that wants a whole block
 * contiguous (V10's rerank, vacuum's lane update) uses.  Returns 0 or -1.
 */
extern int weave_strip_scatter(weave_uint8 *block, size_t blocklen, int dim,
							   int bits, const WeaveVecStripHdr *hdr,
							   const weave_uint8 *codes);

#endif							/* WEAVE_VECPAGE_H */
