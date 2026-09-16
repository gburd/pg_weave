/*-------------------------------------------------------------------------
 *
 * vecpage.c
 *		Coordinate-sliced code strips: the reference implementation.
 *
 * Backend-independent, like src/vector/pack.c.  Everything is arithmetic over
 * caller-supplied buffers so test/hegel/test_vecpage.c can prove the round trip,
 * the refusals and the determinism property with no backend.  The design and the
 * arithmetic that forced it are in doc/specs/VECTOR_CHANNEL.md sect. 7.1; the
 * header states the three facts it rests on.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vecpage.c
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "weave/vecpage.h"

/*
 * Byte offset of coordinate j within a WEAVE_PACK_LANE block.
 *
 * Code (coordinate j, lane s) is at bit (j*32 + s)*bits, so coordinate j starts at
 * bit j*32*bits, which is byte j*4*bits.  The whole strip format depends on that
 * being an integer, which it is for every width because 32*bits/8 == 4*bits.  This
 * function exists so there is exactly one expression of it.
 */
static inline size_t
lane_coord_offset(int bits, int j)
{
	return (size_t) j * (size_t) weave_strip_coordbytes(bits);
}

int
weave_strip_build(void *dst, size_t dstlen, WeavePackLayout layout,
				  int dim, int bits, weave_uint32 blockno,
				  int j0, int ncoords, weave_uint16 flags,
				  const weave_uint8 *block)
{
	WeaveVecStripHdr *hdr;
	weave_uint8 *out;
	size_t		cb,
				need;

	/*
	 * VECMAJOR is refused rather than handled.  There, code (j, s) is at bit
	 * (s*dim + j)*bits, so a coordinate range is 32 discontiguous runs and a page
	 * break inside a vector splits it -- a reader would return wrong distances, not
	 * an error.  No scanning kernel uses VECMAJOR (doc/specs/VECTOR_CHANNEL.md
	 * sect. 8), so refusing costs nothing and prevents the silent case.
	 */
	if (layout != WEAVE_PACK_LANE)
		return -1;
	if (dim <= 0 || bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		return -1;
	if (ncoords < 1 || j0 < 0 || j0 + ncoords > dim)
		return -1;
	if (block == NULL || dst == NULL)
		return -1;

	cb = (size_t) weave_strip_coordbytes(bits);
	need = sizeof(WeaveVecStripHdr) + (size_t) ncoords * cb;
	if (dstlen < need)
		return -1;

	/*
	 * Zero the WHOLE destination, not just the payload.  Two things depend on it,
	 * both recorded against V7 in doc/specs/VECTOR_CHANNEL.md sect. 7: a
	 * GenericXLog delta over a rewritten page must not see uninitialized tail
	 * bytes change, and a cross-architecture fixture hash of a page image must
	 * reproduce.  The slack exists because weave_block_codebytes() rounds up per
	 * lane rather than once, so a block buffer carries 0-28 bytes no pack function
	 * ever writes.
	 */
	memset(dst, 0, dstlen);

	hdr = (WeaveVecStripHdr *) dst;
	hdr->blockno = blockno;
	hdr->j0 = (weave_uint16) j0;
	hdr->ncoords = (weave_uint16) ncoords;
	hdr->flags = flags;
	hdr->pad = 0;

	out = (weave_uint8 *) dst + sizeof(WeaveVecStripHdr);
	memcpy(out, block + lane_coord_offset(bits, j0), (size_t) ncoords * cb);
	return (int) need;
}

int
weave_strip_parse(const void *src, size_t srclen, int dim, int bits,
				  WeaveVecStripHdr *hdr, const weave_uint8 **codes,
				  const char **why)
{
	const WeaveVecStripHdr *h;
	size_t		cb,
				need;

	if (why != NULL)
		*why = NULL;
	if (src == NULL || hdr == NULL || codes == NULL)
	{
		if (why)
			*why = "null argument";
		return -1;
	}
	if (dim <= 0 || bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
	{
		if (why)
			*why = "geometry out of range";
		return -1;
	}
	if (srclen < sizeof(WeaveVecStripHdr))
	{
		if (why)
			*why = "page payload shorter than a strip header";
		return -1;
	}

	h = (const WeaveVecStripHdr *) src;
	if (h->ncoords < 1)
	{
		if (why)
			*why = "strip carries no coordinates";
		return -1;
	}
	if ((int) h->j0 + (int) h->ncoords > dim)
	{
		if (why)
			*why = "strip coordinate range runs past dim";
		return -1;
	}
	if (h->pad != 0)
	{
		/*
		 * A nonzero pad is not cosmetic.  It means the page was written by
		 * something that did not zero its buffer, which is the determinism bug
		 * sect. 7 warns about -- so the page image is not reproducible even if the
		 * codes on it happen to be right.  Fail rather than read it.
		 */
		if (why)
			*why = "strip header pad is nonzero";
		return -1;
	}

	cb = (size_t) weave_strip_coordbytes(bits);
	need = sizeof(WeaveVecStripHdr) + (size_t) h->ncoords * cb;
	if (srclen < need)
	{
		if (why)
			*why = "page payload shorter than the strip it declares";
		return -1;
	}

	*hdr = *h;
	*codes = (const weave_uint8 *) src + sizeof(WeaveVecStripHdr);
	return (int) h->ncoords;
}

int
weave_strip_scatter(weave_uint8 *block, size_t blocklen, int dim, int bits,
					const WeaveVecStripHdr *hdr, const weave_uint8 *codes)
{
	size_t		cb,
				off,
				len;

	if (block == NULL || hdr == NULL || codes == NULL)
		return -1;
	if (dim <= 0 || bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		return -1;
	if (hdr->ncoords < 1 || (int) hdr->j0 + (int) hdr->ncoords > dim)
		return -1;

	cb = (size_t) weave_strip_coordbytes(bits);
	off = lane_coord_offset(bits, hdr->j0);
	len = (size_t) hdr->ncoords * cb;
	if (off + len > blocklen)
		return -1;

	memcpy(block + off, codes, len);
	return 0;
}
