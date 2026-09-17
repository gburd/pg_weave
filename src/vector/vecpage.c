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

/* ---------------------------------------------------------------------------
 * The block directory
 *
 * Fixed-size records so record i is at a computable page and offset.  Everything
 * dim-dependent lives in the strips instead, which is what makes this possible --
 * see the header, and doc/specs/VECTOR_CHANNEL.md sect. 7.1 for why the split was
 * forced rather than chosen.
 * ------------------------------------------------------------------------- */

int
weave_vecdir_page_init(void *dst, size_t dstlen, int usable,
					   weave_uint32 first_blockno, int nrecs)
{
	WeaveVecDirHdr *h;
	int			rpp = weave_vecdir_recs_per_page(usable);

	if (dst == NULL || usable <= 0 || (size_t) usable > dstlen)
		return -1;
	if (rpp <= 0 || nrecs < 1 || nrecs > rpp)
		return -1;

	/* Zero the whole usable area, not just the header: the slack past the last
	 * record is part of the page image (208 bytes at an 8,160-byte page), and a
	 * page image that carries whatever the buffer held is nondeterministic. */
	memset(dst, 0, (size_t) usable);
	h = (WeaveVecDirHdr *) dst;
	h->first_blockno = first_blockno;
	h->nrecs = (weave_uint16) nrecs;
	h->pad = 0;
	return 0;
}

int
weave_vecdir_write(void *dst, size_t dstlen, int usable, int slot,
				   const WeaveVecDirRec *rec)
{
	WeaveVecDirHdr *h;
	int			rpp = weave_vecdir_recs_per_page(usable);
	size_t		off;

	if (dst == NULL || rec == NULL || usable <= 0 || (size_t) usable > dstlen)
		return -1;
	if (rpp <= 0 || slot < 0 || slot >= rpp)
		return -1;

	h = (WeaveVecDirHdr *) dst;
	if (h->pad != 0 || h->nrecs < 1 || (int) h->nrecs > rpp)
		return -1;				/* page was never initialized, or is corrupt */
	if (slot >= (int) h->nrecs)
		return -1;				/* past what this page declares it holds */
	if (!weave_vecdir_floats_ok(rec))
		return -1;				/* refuse to write a bound that is not a bound */

	off = sizeof(WeaveVecDirHdr) + (size_t) slot * sizeof(WeaveVecDirRec);
	/*
	 * Check the record FITS before writing it, even though `slot < rpp` should
	 * already guarantee it.  This is the same arithmetic tested from the other
	 * side, and mutation D6 is why it is here: a weave_vecdir_recs_per_page() that
	 * forgot the page header returns an rpp that is too large at some page sizes,
	 * and every slot check derived from that rpp agrees with it -- so the only
	 * thing between a wrong geometry and a heap overflow was a bound expressed in
	 * the same wrong terms.
	 */
	if (off + sizeof(WeaveVecDirRec) > (size_t) usable)
		return -1;
	memcpy((weave_uint8 *) dst + off, rec, sizeof(WeaveVecDirRec));
	return 0;
}

int
weave_vecdir_read(const void *src, size_t srclen, int usable, int slot,
				  WeaveVecDirRec *out, const char **why)
{
	const WeaveVecDirHdr *h;
	int			rpp = weave_vecdir_recs_per_page(usable);
	size_t		off;

	if (why != NULL)
		*why = NULL;
	if (src == NULL || out == NULL || usable <= 0 || (size_t) usable > srclen)
	{
		if (why)
			*why = "bad arguments";
		return -1;
	}
	if (rpp <= 0)
	{
		if (why)
			*why = "page too small for a directory record";
		return -1;
	}
	h = (const WeaveVecDirHdr *) src;
	if (h->pad != 0)
	{
		if (why)
			*why = "directory page header pad is nonzero";
		return -1;
	}
	if (h->nrecs < 1 || (int) h->nrecs > rpp)
	{
		if (why)
			*why = "directory page declares an impossible record count";
		return -1;
	}
	if (slot < 0 || slot >= (int) h->nrecs)
	{
		if (why)
			*why = "slot past the records this page holds";
		return -1;
	}

	off = sizeof(WeaveVecDirHdr) + (size_t) slot * sizeof(WeaveVecDirRec);
	if (off + sizeof(WeaveVecDirRec) > (size_t) usable)
	{
		if (why)
			*why = "record would extend past the page";
		return -1;
	}
	memcpy(out, (const weave_uint8 *) src + off, sizeof(WeaveVecDirRec));

	if (!weave_vecdir_floats_ok(out))
	{
		if (why)
			*why = "directory record carries a float that cannot be a bound";
		return -1;
	}
	if (out->livemask == 0)
	{
		/*
		 * Not a corruption: vacuum can empty a block and the space is reclaimed
		 * later.  But a caller that scores it would read 32 dead lanes, so say so
		 * rather than leaving every caller to remember.  Reported through the
		 * return value's absence, not an error: the record is returned and valid.
		 */
		if (why)
			*why = "block has no live lanes";
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * The warp map
 *
 * Dense uint64 docids, so warp w is at a computable page and offset.  See the
 * block comment above WeaveVecWarpHdr in the header for why the map exists and why
 * it is not four more fields in the directory record.
 * ------------------------------------------------------------------------- */

int
weave_vecwarp_page_init(void *dst, size_t dstlen, int usable,
						weave_uint32 firstwarp, int nwarps)
{
	WeaveVecWarpHdr *h;
	int			wpp = weave_vecwarp_per_page(usable);

	if (dst == NULL || usable <= 0 || (size_t) usable > dstlen)
		return -1;
	if (wpp <= 0 || nwarps < 1 || nwarps > wpp)
		return -1;

	/* The slack past the last entry is part of the page image; see
	 * weave_vecdir_page_init(). */
	memset(dst, 0, (size_t) usable);
	h = (WeaveVecWarpHdr *) dst;
	h->firstwarp = firstwarp;
	h->nwarps = (weave_uint16) nwarps;
	h->pad = 0;
	return 0;
}

int
weave_vecwarp_write(void *dst, size_t dstlen, int usable, int slot,
					weave_uint64 docid)
{
	WeaveVecWarpHdr *h;
	int			wpp = weave_vecwarp_per_page(usable);
	size_t		off;

	if (dst == NULL || usable <= 0 || (size_t) usable > dstlen)
		return -1;
	if (wpp <= 0 || slot < 0 || slot >= wpp)
		return -1;
	if (docid == 0)
		return -1;				/* the hole an unwritten slot leaves; see the header */

	h = (WeaveVecWarpHdr *) dst;
	if (h->pad != 0 || h->nwarps < 1 || (int) h->nwarps > wpp)
		return -1;
	if (slot >= (int) h->nwarps)
		return -1;

	off = sizeof(WeaveVecWarpHdr) + (size_t) slot * sizeof(weave_uint64);
	/* Checked against `usable` and not only against wpp, for the reason mutation
	 * D6 established in weave_vecdir_write(): a wrong per-page count makes every
	 * slot bound derived from it agree with itself. */
	if (off + sizeof(weave_uint64) > (size_t) usable)
		return -1;
	memcpy((weave_uint8 *) dst + off, &docid, sizeof(weave_uint64));
	return 0;
}

int
weave_vecwarp_read(const void *src, size_t srclen, int usable, int slot,
				   weave_uint64 *out, const char **why)
{
	const WeaveVecWarpHdr *h;
	int			wpp = weave_vecwarp_per_page(usable);
	size_t		off;

	if (why != NULL)
		*why = NULL;
	if (src == NULL || out == NULL || usable <= 0 || (size_t) usable > srclen)
	{
		if (why)
			*why = "bad arguments";
		return -1;
	}
	if (wpp <= 0)
	{
		if (why)
			*why = "page too small for a warp map entry";
		return -1;
	}
	h = (const WeaveVecWarpHdr *) src;
	if (h->pad != 0)
	{
		if (why)
			*why = "warp map page header pad is nonzero";
		return -1;
	}
	if (h->nwarps < 1 || (int) h->nwarps > wpp)
	{
		if (why)
			*why = "warp map page declares an impossible entry count";
		return -1;
	}
	if (slot < 0 || slot >= (int) h->nwarps)
	{
		if (why)
			*why = "slot past the entries this page holds";
		return -1;
	}

	off = sizeof(WeaveVecWarpHdr) + (size_t) slot * sizeof(weave_uint64);
	if (off + sizeof(weave_uint64) > (size_t) usable)
	{
		if (why)
			*why = "entry would extend past the page";
		return -1;
	}
	memcpy(out, (const weave_uint8 *) src + off, sizeof(weave_uint64));
	if (*out == 0)
	{
		if (why)
			*why = "warp map entry is zero, which is not a docid";
		return -1;
	}
	return 0;
}
