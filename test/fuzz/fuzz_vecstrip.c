/*-------------------------------------------------------------------------
 *
 * fuzz_vecstrip.c
 *		Corruption harness for a vector weft's CODE PAGES (WEAVE_PK_VCODES),
 *		the page layout of doc/specs/VECTOR_CHANNEL.md sect. 7.1 including v4,
 *		where a block's last lane page also carries its centroid strip.
 *
 * WHAT IS FUZZED IS THE REAL CODE.  weave_vecweft_page_take() in
 * src/vector/vecweft.c is the one reader of a code page -- the backend's
 * weave_vec_block_read() and the scan's code cursor both call it -- and it is
 * backend-independent, so the bytes it is handed here are treated exactly as a
 * page read off disk is.  A torn or recycled page is any byte string.
 *
 * PROPERTIES (under ASan+UBSan):
 *   (P1) every page weave_vecweft_page_build() writes, at every geometry in the
 *        sweep (2..8 bits, dims that pack and dims whose centroid spills, v3 and
 *        v4), is accepted by page_take and round-trips its bytes;
 *   (P2) GEOMETRY CANNOT OVERFILL A PAGE: for random (usable, dim, bits, nvec,
 *        version), when weave_vecweft_geom_v() accepts, every strip the plan
 *        places ends inside `usable` -- so the writer can never be told to put a
 *        packed centroid past the page;
 *   (P3) a valid page TRUNCATED to every length, in a buffer malloc'd to exactly
 *        that length, is read without touching a byte past it;
 *   (P4) random corruption anywhere in a page never crashes and never scatters
 *        out of bounds; and corruption of a header field the plan pins (blockno,
 *        j0, ncoords, the centroid flag, the pad) is REFUSED, never accepted as a
 *        different strip.
 *
 * TEETH.  -DWEAVE_VECWEFT_PLANT_NO_OFF_LEN compiles page_take with the strip's
 * remaining length computed as the whole payload rather than the payload past the
 * strip's offset -- the mistake a two-strip page invites, since the first strip is
 * at offset 0 and the bug is invisible there.  P3 then reads the packed centroid
 * past the end of a truncated buffer and ASan must abort.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/fuzz/fuzz_vecstrip.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/vecweft.h"

#define PAYLOAD 8160

static unsigned long long rs = 0x9E3779B97F4A7C15ull;

static unsigned long long
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}

static void
die(const char *msg, int dim, int bits, int ver)
{
	fprintf(stderr, "fuzz_vecstrip: %s (dim %d bits %d v%d)\n", msg, dim, bits, ver);
	exit(1);
}

/* One geometry's block 0 and every one of its pages, built by the real writer. */
typedef struct
{
	WeaveVecWeftGeom g;
	weave_uint8 *block;
	weave_uint8 *cen;
	weave_uint8 *pages;			/* pages_per_block * PAYLOAD */
	int		   *used;
} Fixture;

static int
fixture(Fixture *f, int dim, int bits, int ver)
{
	int			k;
	size_t		i;

	memset(f, 0, sizeof(*f));
	if (weave_vecweft_geom_v(&f->g, PAYLOAD, dim, bits, WEAVE_PACK_LANE, 40,
							 ver) != 0)
		return -1;
	f->block = (weave_uint8 *) malloc((size_t) f->g.blockbytes);
	f->cen = (weave_uint8 *) malloc((size_t) f->g.codebytes);
	f->pages = (weave_uint8 *) malloc((size_t) f->g.pages_per_block * PAYLOAD);
	f->used = (int *) malloc((size_t) f->g.pages_per_block * sizeof(int));
	if (!f->block || !f->cen || !f->pages || !f->used)
		die("out of memory", dim, bits, ver);
	for (i = 0; i < (size_t) f->g.blockbytes; i++)
		f->block[i] = (weave_uint8) rnd();
	for (i = 0; i < (size_t) f->g.codebytes; i++)
		f->cen[i] = (weave_uint8) rnd();
	for (k = 0; k < f->g.pages_per_block; k++)
	{
		f->used[k] = weave_vecweft_page_build(f->pages + (size_t) k * PAYLOAD,
											  PAYLOAD, &f->g, 0, k, f->block,
											  f->cen);
		if (f->used[k] <= 0 || f->used[k] > PAYLOAD)
			die("the writer could not build a page of its own layout", dim, bits, ver);
	}
	return 0;
}

static void
fixture_free(Fixture *f)
{
	free(f->block);
	free(f->cen);
	free(f->pages);
	free(f->used);
}

/* page_take into fresh, exactly-sized destinations: an out-of-bounds scatter
 * is an ASan abort, which is the point */
static int
take(const Fixture *f, const void *src, size_t len, weave_uint32 b, int pg)
{
	weave_uint8 *blk = (weave_uint8 *) malloc((size_t) f->g.blockbytes);
	weave_uint8 *cen = (weave_uint8 *) malloc((size_t) f->g.codebytes);
	const char *why = NULL;
	int			n;

	if (!blk || !cen)
		die("out of memory", f->g.dim, f->g.bits, f->g.version);
	n = weave_vecweft_page_take(&f->g, src, len, b, pg, blk, cen, &why);
	if (n < 0 && why == NULL)
		die("a refusal carried no reason", f->g.dim, f->g.bits, f->g.version);
	free(blk);
	free(cen);
	return n;
}

int
main(void)
{
	static const int dims[] = {4, 64, 128, 384, 500, 509, 768, 960, 1000, 1024,
	1536, 4096};
	unsigned long accepted = 0,
				refused = 0,
				geoms = 0;
	int			di,
				bits,
				ver,
				k;
	long		t;

	/* P2: random geometry never places a strip past the page */
	for (t = 0; t < 2000000; t++)
	{
		WeaveVecWeftGeom g;
		int			usable = 13 + (int) (rnd() % 9000);
		int			dim = 1 + (int) (rnd() % 17000);
		int			b = (int) (rnd() % 10);
		int			v = 2 + (int) (rnd() % 4);
		weave_uint32 nvec = 1 + (weave_uint32) (rnd() % 100);
		int			s;

		if (weave_vecweft_geom_v(&g, usable, dim, b, WEAVE_PACK_LANE, nvec, v) != 0)
			continue;
		geoms++;
		for (s = 0; s < g.strips_per_block; s++)
		{
			WeaveVecStripPlan p;

			if (weave_vecweft_strip_plan(&g, (weave_uint32) s, &p) != 0)
				die("no plan for a strip inside block 0", dim, b, v);
			if (p.page < 0 || p.page >= g.pages_per_block ||
				p.pageoff + (int) sizeof(WeaveVecStripHdr) + p.nbytes > usable)
				die("the plan placed a strip past the page", dim, b, v);
		}
	}

	for (ver = WEAVE_VECWEFT_V3; ver <= WEAVE_VECWEFT_V4; ver++)
		for (di = 0; di < (int) (sizeof(dims) / sizeof(dims[0])); di++)
			for (bits = WEAVE_BITS_MIN; bits <= WEAVE_BITS_MAX; bits++)
			{
				Fixture		f;

				if (fixture(&f, dims[di], bits, ver) != 0)
					continue;

				for (k = 0; k < f.g.pages_per_block; k++)
				{
					const weave_uint8 *pg = f.pages + (size_t) k * PAYLOAD;
					size_t		cut;
					int			trial;

					/* P1 */
					{
						weave_uint8 *blk = (weave_uint8 *) calloc(1, (size_t) f.g.blockbytes);
						weave_uint8 *cen = (weave_uint8 *) calloc(1, (size_t) f.g.codebytes);
						WeaveVecStripPlan p;
						const char *why = NULL;
						int			s;

						if (weave_vecweft_page_take(&f.g, pg, PAYLOAD, 0, k, blk,
													cen, &why) <= 0)
							die("a page the writer built was refused", f.g.dim,
								f.g.bits, ver);
						for (s = 0; s < f.g.strips_per_block; s++)
						{
							if (weave_vecweft_strip_plan(&f.g, (weave_uint32) s, &p) != 0)
								die("no plan", f.g.dim, f.g.bits, ver);
							if (p.page != k)
								continue;
							if ((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0
								? memcmp(cen + p.srcoff, f.cen + p.srcoff, (size_t) p.nbytes)
								: memcmp(blk + p.srcoff, f.block + p.srcoff, (size_t) p.nbytes))
								die("a strip did not round-trip", f.g.dim, f.g.bits, ver);
						}
						free(blk);
						free(cen);
					}

					/* P3: every truncation, exactly-sized buffer */
					for (cut = 0; cut <= (size_t) f.used[k] + 8 && cut <= PAYLOAD; cut++)
					{
						weave_uint8 *exact = (weave_uint8 *) malloc(cut ? cut : 1);

						memcpy(exact, pg, cut);
						if (take(&f, exact, cut, 0, k) >= 0)
						{
							accepted++;
							if (cut < (size_t) f.used[k])
								die("a truncated page was accepted", f.g.dim,
									f.g.bits, ver);
						}
						else
							refused++;
						free(exact);
					}

					/* P4: random corruption */
					for (trial = 0; trial < 3000; trial++)
					{
						weave_uint8 *img = (weave_uint8 *) malloc(PAYLOAD);
						int			nsmash = 1 + (int) (rnd() % 4);
						int			m;

						memcpy(img, pg, PAYLOAD);
						for (m = 0; m < nsmash; m++)
						{
							/* bias toward the two header sites */
							size_t		off;
							WeaveVecStripPlan p;

							switch (rnd() % 3)
							{
								case 0:
									off = rnd() % sizeof(WeaveVecStripHdr);
									break;
								case 1:
									if (weave_vecweft_strip_plan(&f.g,
																 (weave_uint32) f.g.lane_strips,
																 &p) == 0 &&
										p.page == k)
									{
										off = (size_t) p.pageoff +
											rnd() % sizeof(WeaveVecStripHdr);
										break;
									}
									/* FALLTHROUGH */
								default:
									off = rnd() % PAYLOAD;
									break;
							}
							img[off] = (weave_uint8) rnd();
						}
						if (take(&f, img, PAYLOAD, 0, k) >= 0)
						{
							/* accepted => every header the plan puts on this page
							 * says exactly what the plan says (the flag word's
							 * non-CENTROID bits are not part of the plan) */
							WeaveVecStripPlan q;
							int			s;

							accepted++;
							for (s = 0; s < f.g.strips_per_block; s++)
							{
								const WeaveVecStripHdr *h;

								if (weave_vecweft_strip_plan(&f.g, (weave_uint32) s, &q) != 0 ||
									q.page != k)
									continue;
								h = (const WeaveVecStripHdr *) (img + q.pageoff);
								if (h->blockno != 0 || h->j0 != (weave_uint16) q.j0 ||
									h->ncoords != (weave_uint16) q.ncoords ||
									((h->flags ^ q.flags) & WEAVE_VSTRIP_F_CENTROID) != 0 ||
									h->pad != 0)
									die("a corrupted strip header was accepted",
										f.g.dim, f.g.bits, ver);
							}
						}
						else
							refused++;
						free(img);
					}

					/* the page claimed as another block's, or another page */
					if (take(&f, pg, PAYLOAD, 1, k) >= 0)
						die("block 0's page was accepted as block 1's", f.g.dim, f.g.bits, ver);
					if (f.g.pages_per_block > 1 &&
						take(&f, pg, PAYLOAD, 0, (k + 1) % f.g.pages_per_block) >= 0)
						die("a page was accepted as another page of its block",
							f.g.dim, f.g.bits, ver);
				}
				fixture_free(&f);
			}

	printf("fuzz_vecstrip: %lu geometries checked, %lu pages accepted, %lu refused\n",
		   geoms, accepted, refused);
	return 0;
}
