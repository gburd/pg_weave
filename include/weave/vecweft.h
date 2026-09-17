/*-------------------------------------------------------------------------
 *
 * vecweft.h
 *		The layout of one segment's vector weft: how many strips, in what order,
 *		and which bytes each one carries.
 *
 * Backend-independent on purpose, exactly like weave/vecpage.h, and for the same
 * reason: everything here is arithmetic over caller-supplied buffers, so
 * test/hegel/test_vecweft.c can prove the whole weft round-trips -- encode, pack,
 * plan, write, read back, recompute the directory -- with no running backend.  The
 * page allocation, the GenericXLog cycles and the chain walk are the backend half
 * (src/vector/vecwrite.c, task V7).
 *
 * WHY A SEPARATE TRANSLATION UNIT FROM vecpage.c.  vecpage.c answers "what does
 * ONE strip page look like"; this answers "what strips does a weft consist of, and
 * in what order".  The second question is the one a writer and a reader must
 * answer IDENTICALLY -- a writer that emits block b's strips in one order and a
 * reader that assumes another returns another block's codes, silently.  So the
 * order is a function, and both sides call it.  Keeping it out of vecpage.c also
 * keeps vecpage.c's 5.1M-check property test valid as-is.
 *
 * TWO STRIP FLAVOURS, AND WHY THE CENTROID NEEDED ITS OWN.
 * doc/specs/VECTOR_CHANNEL.md sect. 7.1 says the block centroid "becomes strips of
 * its own, sliced by coordinate exactly like the lanes", and prices them at 1/32 of
 * the code bytes.  Those two statements are not simultaneously satisfiable by
 * weave_strip_build(): its stride is weave_strip_coordbytes(bits) = 4*bits bytes
 * per coordinate, which is 32 LANES' worth, so a centroid stored through it would
 * cost 32/32 of the code bytes, not 1/32.  A centroid coordinate is `bits` BITS,
 * so a centroid strip needs its own stride and its own cut rule:
 *
 *	 - a cut is only byte-aligned where j0*bits % 8 == 0, so the coordinate
 *	   granularity is WEAVE_CEN_GRAN(bits) = 8/gcd(bits,8) -- 2 coordinates at 4
 *	   bits, 8 at 3 bits, 1 at 8 bits.  A page break placed anywhere else would
 *	   split a code in half, which is exactly what shape (B) exists to avoid.
 *	 - the header is the SAME WeaveVecStripHdr, with WEAVE_VSTRIP_F_CENTROID set.
 *	   That is what the flag is for (sect. 7.1), so this is a reading of the
 *	   ratified format, not an extension of it -- but the accessors below are new,
 *	   and weave_strip_parse() must not be pointed at a centroid strip: it would
 *	   demand 32x the bytes and refuse.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/vecweft.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_VECWEFT_H
#define WEAVE_VECWEFT_H

#include "weave/vecpage.h"

/*
 * Coordinates a centroid strip may be cut on: the smallest run of `bits`-wide
 * codes that ends on a byte boundary.  gcd(bits,8) is a switch rather than a loop
 * because bits is 2..8 and a table is easier to check by eye than Euclid.
 */
static inline int
weave_cen_gran(int bits)
{
	switch (bits)
	{
		case 2:
			return 4;
		case 4:
			return 2;
		case 6:
			return 4;
		case 8:
			return 1;
		default:
			return 8;			/* odd widths: 8 codes = `bits` bytes */
	}
}

/*
 * Everything about a weft's shape, derived once from the four numbers that
 * describe it.  Held by the writer and rebuilt by the reader from the VMETA page,
 * so both sides compute every offset from the same expressions.
 */
typedef struct WeaveVecWeftGeom
{
	int			dim;
	int			bits;
	int			layout;			/* WeavePackLayout; only WEAVE_PACK_LANE is storable */
	int			usable;			/* payload bytes a page offers */

	weave_uint32 nvec;			/* lane slots, live and dead alike */
	weave_uint32 nblocks;		/* ceil(nvec / WEAVE_VEC_BLOCK) */

	int			codebytes;		/* one vector's code: ceil(dim*bits/8) */
	int			blockbytes;		/* weave_block_codebytes(): 0-28 bytes of slack */

	int			lane_cpp;		/* coordinates per lane strip */
	int			cen_cpp;		/* coordinates per centroid strip (multiple of gran) */
	int			cen_gran;
	int			lane_strips;	/* lane strips per block */
	int			cen_strips;		/* centroid strips per block */
	int			strips_per_block;

	int			rpp;			/* directory records per page */
	weave_uint32 ndirpages;
	weave_uint32 nstrips;		/* strips in the whole weft */
} WeaveVecWeftGeom;

/*
 * One strip, fully located: which block, which coordinates, which flavour, how
 * many bytes, and where those bytes start in the source buffer (the packed block
 * for a lane strip, the centroid code for a centroid strip).
 */
typedef struct WeaveVecStripPlan
{
	weave_uint32 blockno;
	int			j0;
	int			ncoords;
	weave_uint16 flags;
	int			nbytes;			/* payload bytes, header excluded */
	int			srcoff;			/* byte offset in the source buffer */
} WeaveVecStripPlan;

/*
 * Fill *g, or return -1 for a geometry that cannot be stored: a non-LANE layout
 * (a coordinate cut splits vectors there -- vecpage.h), dim or bits out of range,
 * a page too small for one coordinate of 32 lanes or for one directory record, or
 * nvec == 0 (an empty weft is written by not writing one at all).
 */
extern int	weave_vecweft_geom(WeaveVecWeftGeom *g, int usable, int dim, int bits,
							   int layout, weave_uint32 nvec);

/*
 * The i-th strip of the weft in WRITE ORDER, which is block-major: block 0's lane
 * strips, then block 0's centroid strips, then block 1's.  Sect. 7.1 rejected the
 * coordinate-major alternative because it makes single-block access pathological
 * (960 pages for 16 bytes each at n=1M/960-d), and both V10's rerank window and
 * vacuum's lane update do exactly that.
 *
 * O(1) so a reader can name the strip it wants rather than scanning for it.
 * Returns 0, or -1 if i is past the weft.
 */
extern int	weave_vecweft_strip_plan(const WeaveVecWeftGeom *g, weave_uint32 i,
									 WeaveVecStripPlan *out);

/* Lane slots block `blockno` covers: 32 for every block but possibly the last. */
extern int	weave_vecweft_block_lanes(const WeaveVecWeftGeom *g,
									  weave_uint32 blockno);

/*
 * Build a centroid strip's payload into `dst`.  Zeroes all `dstlen` bytes first,
 * for the reason weave_strip_build() does: the page image must be reproducible or
 * a GenericXLog delta covers bytes that change between rewrites.  Returns bytes
 * written (header included) or -1.
 */
extern int	weave_censtrip_build(void *dst, size_t dstlen,
								const WeaveVecWeftGeom *g,
								const WeaveVecStripPlan *p,
								const weave_uint8 *cencode);

/*
 * Parse and validate a centroid strip.  Returns the coordinate count and points
 * *bytes at the first code byte, or -1 with *why set.  Validates against the
 * geometry -- j0 on a granularity boundary, the range inside dim, the flag
 * actually set -- because on-disk bytes are not trusted (doc/CONVENTIONS.md
 * decision 2) and a centroid strip misread as a lane strip is a wrong bound, not
 * an error.
 */
extern int	weave_censtrip_parse(const void *src, size_t srclen,
								const WeaveVecWeftGeom *g,
								WeaveVecStripHdr *hdr,
								const weave_uint8 **bytes, const char **why);

/* Copy a parsed centroid strip back into the right byte range of a code buffer. */
extern int	weave_censtrip_scatter(weave_uint8 *cencode, size_t cenlen,
								  const WeaveVecWeftGeom *g,
								  const WeaveVecStripHdr *hdr,
								  const weave_uint8 *bytes);

#endif							/* WEAVE_VECWEFT_H */
