/*-------------------------------------------------------------------------
 *
 * vecweft.c
 *		A vector weft's strip layout: the reference implementation.
 *
 * Backend-independent, like src/vector/vecpage.c and src/vector/pack.c.  The
 * header states what this file is for and why the centroid needed accessors of
 * its own; doc/specs/VECTOR_CHANNEL.md sect. 7.1 has the ratified format and the
 * arithmetic that forced it.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vecweft.c
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "weave/vecweft.h"

/* Byte offset of centroid coordinate j.  Integral only when j is a multiple of
 * weave_cen_gran(bits), which every caller here has already established -- so
 * this function does not re-check it, its callers do. */
static inline size_t
cen_coord_offset(int bits, int j)
{
	return ((size_t) j * (size_t) bits) / 8;
}

/* Bytes `n` centroid coordinates occupy.  The ceil matters only for the LAST
 * strip of a weft whose dim is not a multiple of the granularity. */
static inline size_t
cen_coord_bytes(int bits, int n)
{
	return ((size_t) n * (size_t) bits + 7) / 8;
}

int
weave_vecweft_geom(WeaveVecWeftGeom *g, int usable, int dim, int bits,
				   int layout, weave_uint32 nvec)
{
	int			avail;

	if (g == NULL)
		return -1;
	memset(g, 0, sizeof(*g));

	/*
	 * VECMAJOR is refused here as well as in weave_strip_build(), not merely
	 * relied upon to be refused there.  A caller that plans a weft it cannot
	 * write would allocate pages and fail on the first strip, leaving them
	 * leaked; failing before the first page is allocated is the difference
	 * between a clean error and a leak.
	 */
	if (layout != WEAVE_PACK_LANE)
		return -1;
	if (dim <= 0 || dim > WEAVE_MAX_DIM)
		return -1;
	if (bits < WEAVE_BITS_MIN || bits > WEAVE_BITS_MAX)
		return -1;
	if (nvec == 0)
		return -1;
	if (usable <= (int) sizeof(WeaveVecStripHdr))
		return -1;

	g->dim = dim;
	g->bits = bits;
	g->layout = layout;
	g->usable = usable;
	g->nvec = nvec;
	g->nblocks = (nvec + (weave_uint32) WEAVE_VEC_BLOCK - 1) /
		(weave_uint32) WEAVE_VEC_BLOCK;

	g->codebytes = (dim * bits + 7) / 8;
	g->blockbytes = weave_block_codebytes(dim, bits);

	g->lane_cpp = weave_strip_coords_per_page(usable, bits);
	if (g->lane_cpp <= 0)
		return -1;				/* not even one coordinate of 32 lanes fits */

	g->cen_gran = weave_cen_gran(bits);
	avail = usable - (int) sizeof(WeaveVecStripHdr);
	g->cen_cpp = (int) (((size_t) avail * 8) / (size_t) bits);
	g->cen_cpp -= g->cen_cpp % g->cen_gran;	/* every cut but the last is aligned */
	if (g->cen_cpp <= 0)
		return -1;

	g->lane_strips = weave_strip_count(dim, g->lane_cpp);
	g->cen_strips = weave_strip_count(dim, g->cen_cpp);
	g->strips_per_block = g->lane_strips + g->cen_strips;
	if (g->strips_per_block <= 0)
		return -1;

	g->rpp = weave_vecdir_recs_per_page(usable);
	if (g->rpp <= 0)
		return -1;
	g->ndirpages = (g->nblocks + (weave_uint32) g->rpp - 1) /
		(weave_uint32) g->rpp;
	g->nstrips = g->nblocks * (weave_uint32) g->strips_per_block;

	g->wpp = weave_vecwarp_per_page(usable);
	if (g->wpp <= 0)
		return -1;
	g->nwarppages = (g->nvec + (weave_uint32) g->wpp - 1) / (weave_uint32) g->wpp;
	return 0;
}

int
weave_vecweft_strip_plan(const WeaveVecWeftGeom *g, weave_uint32 i,
						 WeaveVecStripPlan *out)
{
	weave_uint32 blockno;
	int			k;

	if (g == NULL || out == NULL || g->strips_per_block <= 0)
		return -1;
	if (i >= g->nstrips)
		return -1;

	memset(out, 0, sizeof(*out));
	blockno = i / (weave_uint32) g->strips_per_block;
	k = (int) (i % (weave_uint32) g->strips_per_block);
	out->blockno = blockno;

	if (k < g->lane_strips)
	{
		int			j0 = k * g->lane_cpp;
		int			n = g->dim - j0;

		if (n > g->lane_cpp)
			n = g->lane_cpp;
		out->j0 = j0;
		out->ncoords = n;
		out->flags = 0;
		out->nbytes = n * weave_strip_coordbytes(g->bits);
		out->srcoff = j0 * weave_strip_coordbytes(g->bits);
	}
	else
	{
		int			c = k - g->lane_strips;
		int			j0 = c * g->cen_cpp;
		int			n = g->dim - j0;

		if (n > g->cen_cpp)
			n = g->cen_cpp;
		out->j0 = j0;
		out->ncoords = n;
		out->flags = WEAVE_VSTRIP_F_CENTROID;
		out->nbytes = (int) cen_coord_bytes(g->bits, n);
		out->srcoff = (int) cen_coord_offset(g->bits, j0);
	}
	return 0;
}

int
weave_vecweft_block_lanes(const WeaveVecWeftGeom *g, weave_uint32 blockno)
{
	weave_uint32 first;

	if (g == NULL || blockno >= g->nblocks)
		return -1;
	first = blockno * (weave_uint32) WEAVE_VEC_BLOCK;
	if (g->nvec - first >= (weave_uint32) WEAVE_VEC_BLOCK)
		return WEAVE_VEC_BLOCK;
	return (int) (g->nvec - first);
}

int
weave_censtrip_build(void *dst, size_t dstlen, const WeaveVecWeftGeom *g,
					 const WeaveVecStripPlan *p, const weave_uint8 *cencode)
{
	WeaveVecStripHdr *hdr;
	size_t		need;

	if (dst == NULL || g == NULL || p == NULL || cencode == NULL)
		return -1;
	if ((p->flags & WEAVE_VSTRIP_F_CENTROID) == 0)
		return -1;
	if (p->ncoords < 1 || p->j0 < 0 || p->j0 + p->ncoords > g->dim)
		return -1;
	if (p->j0 % g->cen_gran != 0)
		return -1;				/* an unaligned cut would split a code */
	if (p->nbytes != (int) cen_coord_bytes(g->bits, p->ncoords) ||
		p->srcoff != (int) cen_coord_offset(g->bits, p->j0))
		return -1;				/* the plan disagrees with the arithmetic */
	if ((size_t) p->srcoff + (size_t) p->nbytes > (size_t) g->codebytes)
		return -1;

	need = sizeof(WeaveVecStripHdr) + (size_t) p->nbytes;
	if (dstlen < need)
		return -1;

	/* Zero everything, including the header pad and the slack past the payload:
	 * the page image is WAL'd as a delta on rewrite and hashed by any
	 * cross-architecture fixture, and neither tolerates uninitialized bytes.
	 * Same requirement as weave_strip_build(). */
	memset(dst, 0, dstlen);
	hdr = (WeaveVecStripHdr *) dst;
	hdr->blockno = p->blockno;
	hdr->j0 = (weave_uint16) p->j0;
	hdr->ncoords = (weave_uint16) p->ncoords;
	hdr->flags = p->flags;
	hdr->pad = 0;
	memcpy((weave_uint8 *) dst + sizeof(WeaveVecStripHdr),
		   cencode + p->srcoff, (size_t) p->nbytes);
	return (int) need;
}

int
weave_censtrip_parse(const void *src, size_t srclen, const WeaveVecWeftGeom *g,
					 WeaveVecStripHdr *hdr, const weave_uint8 **bytes,
					 const char **why)
{
	const WeaveVecStripHdr *h;
	size_t		need;

	if (why != NULL)
		*why = NULL;
	if (src == NULL || g == NULL || hdr == NULL || bytes == NULL)
	{
		if (why)
			*why = "null argument";
		return -1;
	}
	if (srclen < sizeof(WeaveVecStripHdr))
	{
		if (why)
			*why = "page payload shorter than a strip header";
		return -1;
	}
	h = (const WeaveVecStripHdr *) src;
	if ((h->flags & WEAVE_VSTRIP_F_CENTROID) == 0)
	{
		if (why)
			*why = "strip is not a centroid strip";
		return -1;
	}
	if (h->pad != 0)
	{
		if (why)
			*why = "strip header pad is nonzero";
		return -1;
	}
	if (h->ncoords < 1)
	{
		if (why)
			*why = "strip carries no coordinates";
		return -1;
	}
	if ((int) h->j0 + (int) h->ncoords > g->dim)
	{
		if (why)
			*why = "strip coordinate range runs past dim";
		return -1;
	}
	if ((int) h->j0 % g->cen_gran != 0)
	{
		if (why)
			*why = "centroid strip does not start on a byte boundary";
		return -1;
	}
	need = sizeof(WeaveVecStripHdr) + cen_coord_bytes(g->bits, h->ncoords);
	if (srclen < need)
	{
		if (why)
			*why = "page payload shorter than the strip it declares";
		return -1;
	}

	*hdr = *h;
	*bytes = (const weave_uint8 *) src + sizeof(WeaveVecStripHdr);
	return (int) h->ncoords;
}

int
weave_censtrip_scatter(weave_uint8 *cencode, size_t cenlen,
					   const WeaveVecWeftGeom *g, const WeaveVecStripHdr *hdr,
					   const weave_uint8 *bytes)
{
	size_t		off,
				len;

	if (cencode == NULL || g == NULL || hdr == NULL || bytes == NULL)
		return -1;
	if (hdr->ncoords < 1 || (int) hdr->j0 + (int) hdr->ncoords > g->dim)
		return -1;
	if ((int) hdr->j0 % g->cen_gran != 0)
		return -1;
	off = cen_coord_offset(g->bits, hdr->j0);
	len = cen_coord_bytes(g->bits, hdr->ncoords);
	if (off + len > cenlen)
		return -1;
	memcpy(cencode + off, bytes, len);
	return 0;
}
