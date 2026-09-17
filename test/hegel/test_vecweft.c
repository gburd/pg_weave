/*-------------------------------------------------------------------------
 *
 * test_vecweft.c
 *		Standalone property test for a WHOLE vector weft: encode, pack, plan,
 *		write, read back, recompute.  Task V7.
 *
 * Links src/vector/vecweft.c, vecpage.c, vecstats.c, quantize.c and pack.c with
 * no backend and no PostgreSQL header.  That is possible because the V7 writer was
 * split deliberately: everything about WHICH bytes go on WHICH page is arithmetic
 * over caller-supplied buffers, and only page allocation, GenericXLog and the chain
 * walk need a server (src/vector/vecwrite.c).  A format whose round trip can only
 * be tested through a running cluster is a format whose round trip gets tested by
 * reading a regression diff.
 *
 * WHAT THIS PROVES THAT test_vecpage.c DOES NOT.  That file proves ONE strip is
 * self-consistent.  This one proves the WEFT is: that the strip plan partitions
 * every block exactly once, that the plan and the page images agree, that a reader
 * who knows only the geometry can put every block back together byte-identically,
 * and -- the (C2) half -- that every directory record RECOMPUTES from the codes
 * that were stored next to it.  The last property is the one that catches a writer
 * which moved codes and kept stale statistics, which sect. 7.3 names as the trap
 * the merge producer will walk into and which no fixed-output test can see: a bound
 * that is 1 % too low returns plausible answers with rows missing (AGENTS.md hard
 * rule 1).
 *
 * The properties, swept over dim x bits x lane count (including counts that leave a
 * partial last block, and NULL/zero-vector lanes scattered through):
 *
 *	P1	the strip plan is a PARTITION: every coordinate of every block is covered
 *		exactly once by a lane strip and exactly once by a centroid strip, and no
 *		plan runs past dim.  A wrong j0 shows up here before any bytes move.
 *	P2	the round trip is byte-exact: read the weft back from its page images and
 *		every block equals the block that was packed, and every live lane unpacks to
 *		the code the encoder produced.  The lane check goes through
 *		weave_unpack_lane(), so a self-consistently wrong geometry cannot satisfy it.
 *	P3	every directory record recomputes.  weave_vecblock_stats() over the codes
 *		READ BACK FROM THE PAGES, compared to the record read back from its page,
 *		byte for byte -- including the centroid code, which must be the centroid of
 *		the reconstructions and not of anything else.
 *	P4	the page images are DETERMINISTIC: the same vectors written twice into
 *		buffers pre-filled with different garbage give identical pages.  This is the
 *		zero-the-block-buffer requirement (0-28 slack bytes per block that no pack
 *		function writes) tested where it actually bites, over a whole weft.
 *	P5	a directory record written to the WRONG SLOT is detectable: the reader that
 *		addresses record i by the O(1) formula gets a record whose firstwarp is not
 *		i*32.  This is the property the mutation table's "wrong slot" leg needs to
 *		have a home outside the SQL layer.
 *	P6	every refusal refuses: VECMAJOR, nvec 0, an unaligned centroid cut, a plan
 *		index past the weft, a short destination.
 *
 * Build and run:
 *		gcc -O2 -I include -o /tmp/tvw test/hegel/test_vecweft.c \
 *			src/vector/vecweft.c src/vector/vecpage.c src/vector/vecstats.c \
 *			src/vector/quantize.c src/vector/pack.c -lm && /tmp/tvw
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_vecweft.c
 *
 *-------------------------------------------------------------------------
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/vecweft.h"

/* The page payload the backend passes: BLCKSZ 8192 less the 24-byte page header
 * and the 8-byte weave opaque area.  Hard-coded rather than derived, because the
 * point is to test the geometry the shipped writer uses. */
#define PAYLOAD 8160

static long checks = 0;
static long failures = 0;

#define CHECK(cond, ...)											\
	do {															\
		checks++;													\
		if (!(cond))												\
		{															\
			failures++;												\
			printf("FAIL %s:%d: ", __FILE__, __LINE__);				\
			printf(__VA_ARGS__);									\
			printf("\n");											\
			if (failures > 20)										\
			{														\
				printf("too many failures\n");						\
				exit(1);											\
			}														\
		}															\
	} while (0)

static unsigned long long rngstate = 88172645463325252ull;

static unsigned long long
rnd(void)
{
	rngstate ^= rngstate << 13;
	rngstate ^= rngstate >> 7;
	rngstate ^= rngstate << 17;
	return rngstate;
}

static float
rndf(void)
{
	return (float) ((double) (rnd() & 0xFFFFFF) / 8388608.0 - 1.0);
}

/* One simulated weft: the page images, and everything the writer knew. */
typedef struct Weft
{
	WeaveVecWeftGeom g;
	WeaveQuantizer q;

	/* the accumulator, exactly as WeaveVecAccum holds it */
	unsigned int nlane;
	weave_uint8 *code;			/* nlane * codebytes */
	float	   *scale;
	float	   *norm;
	weave_uint8 *live;

	/* the pages */
	weave_uint8 *codepage;		/* nstrips * PAYLOAD */
	weave_uint8 *dirpage;		/* ndirpages * PAYLOAD */

	/* what the writer computed, kept for comparison */
	weave_uint8 *blocks;		/* nblocks * blockbytes */
	weave_uint8 *cencodes;		/* nblocks * codebytes */
	WeaveVecDirRec *recs;		/* nblocks */
} Weft;

static void
weft_free(Weft *w)
{
	free(w->code);
	free(w->scale);
	free(w->norm);
	free(w->live);
	free(w->codepage);
	free(w->dirpage);
	free(w->blocks);
	free(w->cencodes);
	free(w->recs);
	weave_quantizer_free(&w->q, free);
	memset(w, 0, sizeof(*w));
}

/*
 * Build a weft the way src/vector/vecwrite.c does, into memory.  `poison` is the
 * byte every page buffer is pre-filled with, so P4 can demand that two runs with
 * different poison produce identical pages -- which is only true if every writer
 * zeroes the whole payload it was given.
 */
static void
weft_build(Weft *w, int dim, int bits, unsigned int nlane, int livemod,
		   unsigned char poison)
{
	unsigned int i;
	unsigned int b;
	float	   *v;

	memset(w, 0, sizeof(*w));
	assert(weave_vecweft_geom(&w->g, PAYLOAD, dim, bits, WEAVE_PACK_LANE,
							  nlane) == 0);
	assert(weave_quantizer_init(&w->q, dim, bits, NULL, malloc, free) == 0);

	w->nlane = nlane;
	w->code = (weave_uint8 *) calloc((size_t) nlane, (size_t) w->g.codebytes);
	w->scale = (float *) calloc((size_t) nlane, sizeof(float));
	w->norm = (float *) calloc((size_t) nlane, sizeof(float));
	w->live = (weave_uint8 *) calloc((size_t) nlane, 1);
	w->codepage = (weave_uint8 *) malloc((size_t) w->g.nstrips * PAYLOAD);
	w->dirpage = (weave_uint8 *) malloc((size_t) w->g.ndirpages * PAYLOAD);
	w->blocks = (weave_uint8 *) calloc((size_t) w->g.nblocks,
									   (size_t) w->g.blockbytes);
	w->cencodes = (weave_uint8 *) calloc((size_t) w->g.nblocks,
										 (size_t) w->g.codebytes);
	w->recs = (WeaveVecDirRec *) calloc((size_t) w->g.nblocks,
										sizeof(WeaveVecDirRec));
	assert(w->code && w->scale && w->norm && w->live && w->codepage &&
		   w->dirpage && w->blocks && w->cencodes && w->recs);
	memset(w->codepage, poison, (size_t) w->g.nstrips * PAYLOAD);
	memset(w->dirpage, poison, (size_t) w->g.ndirpages * PAYLOAD);

	/* encode: every livemod'th lane is a dead one (a NULL or zero vector) */
	v = (float *) malloc((size_t) dim * sizeof(float));
	assert(v != NULL);
	for (i = 0; i < nlane; i++)
	{
		int			j;

		if (livemod > 0 && (i % (unsigned int) livemod) == 0)
			continue;			/* dead lane: code stays zero, live stays 0 */
		for (j = 0; j < dim; j++)
			v[j] = rndf();
		if (weave_encode(&w->q, v, w->code + (size_t) i * w->g.codebytes,
						 &w->norm[i], &w->scale[i]) != 0)
			continue;			/* a zero vector is a dead lane, as in the writer */
		w->live[i] = 1;
	}
	free(v);

	/* pack, stat, and write every page */
	{
		float	   *recon = (float *) malloc((size_t) WEAVE_VEC_BLOCK *
											 dim * sizeof(float));
		float	   *cen = (float *) malloc((size_t) dim * sizeof(float));
		weave_uint8 *tmp = (weave_uint8 *) malloc((size_t) w->g.codebytes);
		float		lane_scale[WEAVE_VEC_BLOCK];
		float		lane_norm[WEAVE_VEC_BLOCK];
		int			slot[WEAVE_VEC_BLOCK];

		assert(recon && cen && tmp);
		for (b = 0; b < w->g.nblocks; b++)
		{
			weave_uint8 *block = w->blocks + (size_t) b * w->g.blockbytes;
			weave_uint8 *cencode = w->cencodes + (size_t) b * w->g.codebytes;
			WeaveVecDirRec *rec = &w->recs[b];
			int			nlanes = weave_vecweft_block_lanes(&w->g, b);
			int			nlive = 0;
			int			s;
			int			k;
			int			dirslot = weave_vecdir_slot_index(b, w->g.rpp);
			int			dirpg = weave_vecdir_page_index(b, w->g.rpp);

			/* the block buffer is ZEROED before packing (sect. 7's determinism
			 * requirement); the memset here is the thing under test in P4 */
			memset(block, 0, (size_t) w->g.blockbytes);
			for (s = 0; s < nlanes; s++)
			{
				unsigned int lane = b * (unsigned int) WEAVE_VEC_BLOCK +
					(unsigned int) s;

				if (!w->live[lane])
					continue;
				weave_pack_lane(WEAVE_PACK_LANE, dim, bits, block, s,
								w->code + (size_t) lane * w->g.codebytes);
			}

			for (s = 0; s < nlanes; s++)
			{
				unsigned int lane = b * (unsigned int) WEAVE_VEC_BLOCK +
					(unsigned int) s;

				if (!w->live[lane])
					continue;
				weave_unpack_lane(WEAVE_PACK_LANE, dim, bits, block, s, tmp);
				weave_decode(&w->q, tmp, w->scale[lane],
							 recon + (size_t) nlive * dim);
				lane_scale[nlive] = w->scale[lane];
				lane_norm[nlive] = w->norm[lane];
				slot[nlive] = s;
				nlive++;
			}

			if (nlive == 0)
			{
				memset(rec, 0, sizeof(*rec));
				rec->firstwarp = b * (weave_uint32) WEAVE_VEC_BLOCK;
				memset(cencode, 0, (size_t) w->g.codebytes);
			}
			else
				assert(weave_vecblock_stats(rec, &w->q, recon, lane_scale,
											lane_norm, slot, nlive,
											b * (weave_uint32) WEAVE_VEC_BLOCK,
											cen, cencode) == 0);

			if (dirslot == 0)
			{
				unsigned int nrecs = w->g.nblocks - b;

				if (nrecs > (unsigned int) w->g.rpp)
					nrecs = (unsigned int) w->g.rpp;
				assert(weave_vecdir_page_init(w->dirpage + (size_t) dirpg * PAYLOAD,
											  PAYLOAD, PAYLOAD, b,
											  (int) nrecs) == 0);
			}
			assert(weave_vecdir_write(w->dirpage + (size_t) dirpg * PAYLOAD,
									  PAYLOAD, PAYLOAD, dirslot, rec) == 0);

			for (k = 0; k < w->g.strips_per_block; k++)
			{
				WeaveVecStripPlan p;
				unsigned int si = b * (unsigned int) w->g.strips_per_block +
					(unsigned int) k;
				weave_uint8 *dst = w->codepage + (size_t) si * PAYLOAD;
				int			n;

				assert(weave_vecweft_strip_plan(&w->g, si, &p) == 0);
				if ((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0)
					n = weave_censtrip_build(dst, PAYLOAD, &w->g, &p, cencode);
				else
					n = weave_strip_build(dst, PAYLOAD, WEAVE_PACK_LANE, dim,
										  bits, p.blockno, p.j0, p.ncoords,
										  p.flags, block);
				assert(n > 0);
			}
		}
		free(recon);
		free(cen);
		free(tmp);
	}
}

/* P1: the plan is a partition of every block's coordinates, twice over. */
static void
prop_partition(const Weft *w)
{
	unsigned int b;
	int		   *lanecov = (int *) calloc((size_t) w->g.dim, sizeof(int));
	int		   *cencov = (int *) calloc((size_t) w->g.dim, sizeof(int));

	assert(lanecov && cencov);
	for (b = 0; b < w->g.nblocks; b++)
	{
		int			k;
		int			j;

		memset(lanecov, 0, (size_t) w->g.dim * sizeof(int));
		memset(cencov, 0, (size_t) w->g.dim * sizeof(int));
		for (k = 0; k < w->g.strips_per_block; k++)
		{
			WeaveVecStripPlan p;
			unsigned int si = b * (unsigned int) w->g.strips_per_block +
				(unsigned int) k;

			CHECK(weave_vecweft_strip_plan(&w->g, si, &p) == 0,
				  "no plan for strip %u", si);
			CHECK(p.blockno == b, "strip %u belongs to block %u, not %u",
				  si, p.blockno, b);
			CHECK(p.j0 >= 0 && p.ncoords >= 1 && p.j0 + p.ncoords <= w->g.dim,
				  "strip %u range [%d,%d) is outside dim %d",
				  si, p.j0, p.j0 + p.ncoords, w->g.dim);
			for (j = p.j0; j < p.j0 + p.ncoords; j++)
			{
				if ((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0)
					cencov[j]++;
				else
					lanecov[j]++;
			}
			/* a centroid cut must land on a byte boundary or it splits a code */
			if ((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0)
				CHECK(p.j0 % w->g.cen_gran == 0,
					  "centroid strip %u starts at coordinate %d, not a multiple of %d",
					  si, p.j0, w->g.cen_gran);
			CHECK(p.nbytes > 0 && p.nbytes <= PAYLOAD - 12,
				  "strip %u payload %d does not fit a page", si, p.nbytes);
		}
		for (j = 0; j < w->g.dim; j++)
		{
			CHECK(lanecov[j] == 1, "block %u coordinate %d covered %d times by lane strips",
				  b, j, lanecov[j]);
			CHECK(cencov[j] == 1, "block %u coordinate %d covered %d times by centroid strips",
				  b, j, cencov[j]);
		}
	}
	free(lanecov);
	free(cencov);
}

/*
 * P2 + P3: read the weft back the way src/vector/vecwrite.c's reader does -- from
 * the page images and the geometry alone -- and check the codes and the statistics.
 */
static void
prop_roundtrip(const Weft *w)
{
	unsigned int b;
	weave_uint8 *block = (weave_uint8 *) malloc((size_t) w->g.blockbytes);
	weave_uint8 *cencode = (weave_uint8 *) malloc((size_t) w->g.codebytes);
	weave_uint8 *tmp = (weave_uint8 *) malloc((size_t) w->g.codebytes);
	weave_uint8 *recode = (weave_uint8 *) malloc((size_t) w->g.codebytes);
	float	   *recon = (float *) malloc((size_t) WEAVE_VEC_BLOCK *
										 w->g.dim * sizeof(float));
	float	   *cen = (float *) malloc((size_t) w->g.dim * sizeof(float));

	assert(block && cencode && tmp && recode && recon && cen);

	for (b = 0; b < w->g.nblocks; b++)
	{
		WeaveVecDirRec stored;
		WeaveVecDirRec fresh;
		const char *why = NULL;
		unsigned int si;
		int			nseen = 0;
		int			nlive = 0;
		int			i;
		float		lane_scale[WEAVE_VEC_BLOCK];
		float		lane_norm[WEAVE_VEC_BLOCK];
		int			slot[WEAVE_VEC_BLOCK];

		/* the reader's O(1) directory addressing */
		{
			int			dirpg = weave_vecdir_page_index(b, w->g.rpp);
			int			dirslot = weave_vecdir_slot_index(b, w->g.rpp);
			const weave_uint8 *pg = w->dirpage + (size_t) dirpg * PAYLOAD;
			const WeaveVecDirHdr *h = (const WeaveVecDirHdr *) pg;

			CHECK(h->first_blockno == (weave_uint32) dirpg * (weave_uint32) w->g.rpp,
				  "directory page %d says it starts at block %u", dirpg,
				  h->first_blockno);
			CHECK(weave_vecdir_read(pg, PAYLOAD, PAYLOAD, dirslot, &stored,
									&why) == 0,
				  "directory record %u unreadable: %s", b,
				  why != NULL ? why : "?");
			CHECK(stored.firstwarp == b * (weave_uint32) WEAVE_VEC_BLOCK,
				  "record for block %u carries firstwarp %u", b,
				  stored.firstwarp);
		}

		memset(block, 0, (size_t) w->g.blockbytes);
		memset(cencode, 0, (size_t) w->g.codebytes);

		/* scan every page for this block's strips, as the backend reader does */
		for (si = 0; si < w->g.nstrips; si++)
		{
			const weave_uint8 *pg = w->codepage + (size_t) si * PAYLOAD;
			const WeaveVecStripHdr *raw = (const WeaveVecStripHdr *) pg;
			WeaveVecStripHdr hdr;
			const weave_uint8 *bytes = NULL;
			int			n;

			if (raw->blockno != b)
				continue;
			if ((raw->flags & WEAVE_VSTRIP_F_CENTROID) != 0)
			{
				n = weave_censtrip_parse(pg, PAYLOAD, &w->g, &hdr, &bytes, &why);
				CHECK(n > 0, "centroid strip %u unparseable: %s", si,
					  why != NULL ? why : "?");
				if (n > 0)
					CHECK(weave_censtrip_scatter(cencode, w->g.codebytes, &w->g,
												 &hdr, bytes) == 0,
						  "centroid strip %u does not scatter", si);
			}
			else
			{
				n = weave_strip_parse(pg, PAYLOAD, w->g.dim, w->g.bits, &hdr,
									  &bytes, &why);
				CHECK(n > 0, "lane strip %u unparseable: %s", si,
					  why != NULL ? why : "?");
				if (n > 0)
					CHECK(weave_strip_scatter(block, w->g.blockbytes, w->g.dim,
											  w->g.bits, &hdr, bytes) == 0,
						  "lane strip %u does not scatter", si);
			}
			nseen++;
		}
		CHECK(nseen == w->g.strips_per_block,
			  "block %u was reassembled from %d strips, not %d", b, nseen,
			  w->g.strips_per_block);

		/* P2: byte-exact against what the writer packed */
		CHECK(memcmp(block, w->blocks + (size_t) b * w->g.blockbytes,
					 (size_t) w->g.blockbytes) == 0,
			  "block %u did not round-trip byte-identically", b);
		CHECK(memcmp(cencode, w->cencodes + (size_t) b * w->g.codebytes,
					 (size_t) w->g.codebytes) == 0,
			  "block %u centroid code did not round-trip", b);

		/* P2b: every live lane unpacks to the code the encoder produced.  Goes
		 * through weave_unpack_lane(), so this does not share an offset formula
		 * with the writer. */
		for (i = 0; i < WEAVE_VEC_BLOCK; i++)
		{
			unsigned int lane = b * (unsigned int) WEAVE_VEC_BLOCK +
				(unsigned int) i;

			if ((stored.livemask & (1u << i)) == 0)
				continue;
			CHECK(lane < w->nlane && w->live[lane],
				  "block %u lane %d is live on disk but not in the source", b, i);
			weave_unpack_lane(WEAVE_PACK_LANE, w->g.dim, w->g.bits, block, i,
							  tmp);
			CHECK(memcmp(tmp, w->code + (size_t) lane * w->g.codebytes,
						 (size_t) w->g.codebytes) == 0,
				  "block %u lane %d unpacked to a different code", b, i);

			weave_decode(&w->q, tmp, stored.lane[2 * i],
						 recon + (size_t) nlive * w->g.dim);
			lane_scale[nlive] = stored.lane[2 * i];
			lane_norm[nlive] = stored.lane[2 * i + 1];
			slot[nlive] = i;
			nlive++;
		}

		/* P3: the record recomputes from the codes that were read back */
		if (nlive == 0)
		{
			CHECK(stored.smax == 0.0f && stored.maxrecnorm == 0.0f &&
				  stored.minnorm == 0.0f && stored.censcale == 0.0f &&
				  stored.cenrad == 0.0f,
				  "block %u has no live lanes but states a bound", b);
			continue;
		}
		CHECK(weave_vecblock_stats(&fresh, &w->q, recon, lane_scale, lane_norm,
								   slot, nlive, stored.firstwarp, cen,
								   recode) == 0,
			  "block %u statistics could not be recomputed", b);
		CHECK(memcmp(&fresh, &stored, sizeof(WeaveVecDirRec)) == 0,
			  "block %u: the stored record is not what its codes compute (cenrad %g vs %g, smax %g vs %g)",
			  b, (double) stored.cenrad, (double) fresh.cenrad,
			  (double) stored.smax, (double) fresh.smax);
		CHECK(memcmp(recode, cencode, (size_t) w->g.codebytes) == 0,
			  "block %u: the stored centroid code is not the centroid of its codes",
			  b);
	}

	free(block);
	free(cencode);
	free(tmp);
	free(recode);
	free(recon);
	free(cen);
}

/* P5: a record in the wrong slot is detectable through the O(1) addressing. */
static void
prop_wrong_slot_detectable(const Weft *w)
{
	int			dirpg;
	weave_uint8 *pg;
	WeaveVecDirRec a;
	WeaveVecDirRec bb;
	const char *why = NULL;

	if (w->g.nblocks < 2 || w->g.rpp < 2)
		return;					/* needs two records on one page */

	pg = (weave_uint8 *) malloc(PAYLOAD);
	assert(pg != NULL);
	dirpg = weave_vecdir_page_index(0, w->g.rpp);
	memcpy(pg, w->dirpage + (size_t) dirpg * PAYLOAD, PAYLOAD);

	/* write block 1's record into block 0's slot -- the mutation, applied here so
	 * the detection is a property and not only a regression assertion */
	assert(weave_vecdir_read(pg, PAYLOAD, PAYLOAD, 1, &bb, &why) == 0);
	assert(weave_vecdir_write(pg, PAYLOAD, PAYLOAD, 0, &bb) == 0);
	assert(weave_vecdir_read(pg, PAYLOAD, PAYLOAD, 0, &a, &why) == 0);
	CHECK(a.firstwarp != 0,
		  "a record written to the wrong slot was indistinguishable from the right one");
	free(pg);
}

/* P6: every refusal refuses. */
static void
prop_refusals(void)
{
	WeaveVecWeftGeom g;
	WeaveVecStripPlan p;
	weave_uint8 buf[PAYLOAD];
	weave_uint8 code[64];

	memset(code, 0xA5, sizeof(code));

	CHECK(weave_vecweft_geom(&g, PAYLOAD, 64, 4, WEAVE_PACK_VECMAJOR, 100) == -1,
		  "VECMAJOR geometry was accepted");
	CHECK(weave_vecweft_geom(&g, PAYLOAD, 64, 4, WEAVE_PACK_LANE, 0) == -1,
		  "an empty weft was accepted");
	CHECK(weave_vecweft_geom(&g, PAYLOAD, 0, 4, WEAVE_PACK_LANE, 10) == -1,
		  "dim 0 was accepted");
	CHECK(weave_vecweft_geom(&g, PAYLOAD, 64, 1, WEAVE_PACK_LANE, 10) == -1,
		  "bits 1 was accepted");
	CHECK(weave_vecweft_geom(&g, PAYLOAD, WEAVE_MAX_DIM + 1, 4, WEAVE_PACK_LANE,
							 10) == -1, "dim past WEAVE_MAX_DIM was accepted");
	CHECK(weave_vecweft_geom(&g, 12, 64, 4, WEAVE_PACK_LANE, 10) == -1,
		  "a page with no room for a coordinate was accepted");

	assert(weave_vecweft_geom(&g, PAYLOAD, 64, 4, WEAVE_PACK_LANE, 100) == 0);
	CHECK(weave_vecweft_strip_plan(&g, g.nstrips, &p) == -1,
		  "a strip index past the weft was accepted");

	/* a centroid strip whose j0 is not on a code boundary */
	assert(weave_vecweft_strip_plan(&g, (weave_uint32) g.lane_strips, &p) == 0);
	CHECK((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0,
		  "strip %d of a block is not the first centroid strip", g.lane_strips);
	p.j0 = 1;					/* 1*4 bits is not a byte boundary at 4 bits */
	p.ncoords = 2;
	p.nbytes = 1;
	p.srcoff = 0;
	CHECK(weave_censtrip_build(buf, PAYLOAD, &g, &p, code) == -1,
		  "an unaligned centroid cut was accepted");

	/* a plan whose byte arithmetic disagrees with the geometry */
	assert(weave_vecweft_strip_plan(&g, (weave_uint32) g.lane_strips, &p) == 0);
	p.nbytes += 1;
	CHECK(weave_censtrip_build(buf, PAYLOAD, &g, &p, code) == -1,
		  "a centroid plan with the wrong byte count was accepted");

	/* a destination too small for the strip */
	assert(weave_vecweft_strip_plan(&g, (weave_uint32) g.lane_strips, &p) == 0);
	CHECK(weave_censtrip_build(buf, sizeof(WeaveVecStripHdr), &g, &p, code) == -1,
		  "a short destination was accepted");

	/* a lane strip is not a centroid strip and vice versa */
	{
		WeaveVecStripHdr hdr;
		const weave_uint8 *bytes = NULL;
		const char *why = NULL;
		int			n;

		assert(weave_vecweft_strip_plan(&g, 0, &p) == 0);
		assert((p.flags & WEAVE_VSTRIP_F_CENTROID) == 0);
		{
			weave_uint8 *block = (weave_uint8 *) calloc(1,
														(size_t) g.blockbytes);

			assert(block != NULL);
			assert(weave_strip_build(buf, PAYLOAD, WEAVE_PACK_LANE, g.dim,
									 g.bits, 0, p.j0, p.ncoords, p.flags,
									 block) > 0);
			free(block);
		}
		n = weave_censtrip_parse(buf, PAYLOAD, &g, &hdr, &bytes, &why);
		CHECK(n == -1, "a lane strip parsed as a centroid strip");
	}
}

static void
one_point(int dim, int bits, unsigned int nlane, int livemod)
{
	Weft		w;
	Weft		w2;

	weft_build(&w, dim, bits, nlane, livemod, 0x00);
	prop_partition(&w);
	prop_roundtrip(&w);
	prop_wrong_slot_detectable(&w);

	/*
	 * P4: determinism.  Same vectors -- the RNG is re-seeded so the second run
	 * encodes the identical inputs -- into buffers poisoned with a different byte.
	 * Any page byte the writer leaves untouched shows up here, which is the whole
	 * of sect. 7's block-buffer-zeroing requirement.
	 */
	{
		unsigned long long saved = rngstate;

		rngstate = 88172645463325252ull;
		weft_build(&w, dim, bits, nlane, livemod, 0x00);
		rngstate = 88172645463325252ull;
		weft_build(&w2, dim, bits, nlane, livemod, 0xFF);
		CHECK(memcmp(w.codepage, w2.codepage,
					 (size_t) w.g.nstrips * PAYLOAD) == 0,
			  "dim %d bits %d: the code pages are not deterministic", dim, bits);
		CHECK(memcmp(w.dirpage, w2.dirpage,
					 (size_t) w.g.ndirpages * PAYLOAD) == 0,
			  "dim %d bits %d: the directory pages are not deterministic",
			  dim, bits);
		weft_free(&w2);
		rngstate = saved;
	}
	weft_free(&w);
}

int
main(void)
{
	/*
	 * dim starts at 4, not 1: weave_codebook_solve() refuses dim < 4 (the Beta
	 * shape parameter it fits is (dim-3)/2), so a wvec(1..3) column cannot be
	 * indexed at all -- the writer reports that as an error rather than storing
	 * something it cannot score.  Recorded here because it is a real product limit
	 * that no document stated.
	 */
	static const int dims[] = {4, 15, 64, 96, 509, 510, 511, 960, 1536};
	static const unsigned int lanes[] = {1, 31, 32, 33, 100, 289};
	int			di;
	int			bits;
	int			li;

	for (di = 0; di < (int) (sizeof(dims) / sizeof(dims[0])); di++)
	{
		for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
		{
			for (li = 0; li < (int) (sizeof(lanes) / sizeof(lanes[0])); li++)
			{
				/* the big dims at every width and lane count is a lot of
				 * dequantization; keep the largest to the shipped width */
				if (dims[di] > 600 && bits != 4)
					continue;
				one_point(dims[di], bits, lanes[li], 0);	/* every lane live */
				one_point(dims[di], bits, lanes[li], 5);	/* every 5th dead */
				one_point(dims[di], bits, lanes[li], 1);	/* every lane dead */
			}
		}
	}
	prop_refusals();

	printf("checks: %ld, failures: %ld\n", checks, failures);
	if (failures > 0)
	{
		printf("== FAILED ==\n");
		return 1;
	}
	printf("== ALL PROPERTIES HOLD ==\n");
	return 0;
}
