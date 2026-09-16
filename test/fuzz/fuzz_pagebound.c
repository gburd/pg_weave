/*
 * fuzz_pagebound.c -- corruption/fuzz harness for the PAGE-BOUND ARITHMETIC:
 * `weave_page_entry_end_off()`, `weave_page_entry_avail()` and
 * `weave_page_blob_chunk_len()` in include/weave/pagebound.h, the integer-domain
 * guard that every variable-length walk and every blob read in this access
 * method takes its end bound from.
 *
 * WHY THIS TARGET EXISTS.  doc/GAPS.md G22: `src/pages/trgm_page.c` read a blob
 * spread over a page chain with
 *
 *		avail = ((PageHeader) page)->pd_lower -
 *			((char *) PageGetContents(page) - (char *) page);
 *		avail = Min(avail, len - off);
 *		memcpy(buf + off, PageGetContents(page), avail);
 *
 * `avail` is a `Size`, so a torn or recycled page reporting pd_lower BELOW the
 * contents offset underflows it to ~2^64, and the `Min()` then clamps it not to
 * this page but to the caller's remaining blob length -- which spans the whole
 * CHAIN.  The memcpy reads past the page into adjacent shared buffers.  It
 * cannot overrun `buf`, which is exactly why the site read as safe: the
 * destination is bounded, the source is not.  G22 was closed by routing the read
 * through the guard, and its last paragraph recorded the open work -- that no
 * test in the tree can reach the class, because neither `pg_regress` nor the TAP
 * suite can produce a torn page.  This is that test.
 *
 * WHAT IS FUZZED IS THE REAL CODE, NOT A MODEL.  This is the difference between
 * this target and fuzz_dictwalk.c, which transcribes the guard because the guard
 * used to be inseparable from `PageHeader`/`PageGetContents`.  The arithmetic now
 * lives in include/weave/pagebound.h with no PostgreSQL dependency (the
 * chandesc.h / docvalid.h precedent), and `weave_page_entry_end()` and
 * `weave_page_blob_chunk()` in include/weave/am.h are thin wrappers that read
 * pd_lower and hand it over as an integer.  So the functions exercised below are
 * byte-for-byte the ones the backend runs.
 *
 * PROPERTIES (default build, under ASan+UBSan):
 *   (P1) `weave_page_entry_end_off()` returns an offset in
 *        [min(contents_off, blcksz), blcksz] for EVERY (blcksz, contents_off,
 *        lower) -- so the pointer the backend forms from it is inside the page
 *        and the difference against the contents offset cannot underflow;
 *   (P2) `weave_page_entry_avail()` equals `end - min(contents_off, blcksz)` and
 *        never exceeds the bytes physically on the page.  This is the property
 *        G22 violated;
 *   (P3) `weave_page_blob_chunk_len()` never exceeds EITHER bound -- the page's
 *        readable bytes or the destination's remaining bytes.  G22 applied only
 *        the second;
 *   (P4) the guard does not cheat: for a PLAUSIBLE `lower` the end offset is
 *        exactly `lower`.  A guard that always answered "empty page" would
 *        satisfy P1-P3 and lose every trigram in the index, and no sanitizer can
 *        see a wrong answer;
 *   (P5) the blob-chain model -- a faithful transcription of weave_read_blob()'s
 *        loop, with each page malloc'd at EXACTLY BLCKSZ so ASan's redzone sits
 *        immediately past the last readable byte -- copies out of a chain of
 *        pages with arbitrary pd_lower without reading past any page, and
 *        without writing past a destination sized exactly to the declared blob
 *        length.
 *
 * TEETH.  Both planted-bug builds are compile-time removals in the REAL header
 * (test/fuzz/README.md explains why that is preferred to a weakened copy here):
 *   -DWEAVE_PAGEBOUND_PLANT_RAW_SUB: the pre-G22 expression restored verbatim,
 *        unguarded unsigned subtraction.  P2 fails and, in the chain model, ASan
 *        reports a heap-buffer-overflow READ of the source page.
 *   -DWEAVE_PAGEBOUND_PLANT_NO_LOW_GUARD: only the `lower > blcksz` half of the
 *        guard, which is the half someone writes when thinking "the page is 8 kB
 *        so bound it by 8 kB".  A low `lower` passes it, and every later
 *        `end - contents_off` underflows.  This is G22's actual mechanism.
 *
 * No hegel/cmocka: deterministic splitmix64, fixed seed, reproducible, zero
 * deps.  Also run without sanitizers by `make check-standalone`, where the
 * assertions above are the whole test.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/pagebound.h"

#define PAGESZ			8192	/* BLCKSZ */
#define CONTENTS_OFF	24		/* MAXALIGN(SizeOfPageHeaderData) */
#define PAYLOAD			((size_t) (PAGESZ - CONTENTS_OFF))

static uint64_t rngstate = 0x9E3779B97F4A7C15ULL;

static uint64_t
rnd64(void)
{
	/* splitmix64, so the corpus is identical on every host and every rerun */
	uint64_t	z;

	rngstate += 0x9E3779B97F4A7C15ULL;
	z = rngstate;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static uint32_t
rnd(void)
{
	return (uint32_t) (rnd64() >> 16);
}

/*
 * The independent restatement of the postcondition.  It deliberately recomputes
 * the low bound itself rather than calling anything in pagebound.h: a guard
 * checked with its own arithmetic proves nothing.
 */
static void
check_props(size_t blcksz, size_t contents_off, uint32_t lower,
			size_t len, size_t off)
{
	size_t		low = contents_off > blcksz ? blcksz : contents_off;
	size_t		end = weave_page_entry_end_off(blcksz, contents_off, lower);
	size_t		avail = weave_page_entry_avail(blcksz, contents_off, lower);
	size_t		chunk = weave_page_blob_chunk_len(blcksz, contents_off, lower,
												 len, off);
	size_t		remain = off < len ? len - off : 0;

	/* P1: the end offset is inside the page, at or after the contents */
	assert(end >= low);
	assert(end <= blcksz);

	/* P2: avail is the difference, and it fits the page.  A subtraction that
	 * underflowed would be astronomically larger than the page. */
	assert(avail == end - low);
	assert(avail <= blcksz - low);

	/* P3: the blob chunk respects BOTH bounds, not just the destination's */
	assert(chunk <= avail);
	assert(chunk <= remain);

	/* P4: no cheating -- a plausible lower is honoured exactly.  Without this a
	 * guard hard-wired to "empty page" would pass every check above. */
	if ((size_t) lower >= low && (size_t) lower <= blcksz)
	{
		assert(end == (size_t) lower);
		assert(avail == (size_t) lower - low);
	}
}

/* A pd_lower a torn, recycled or hostile page might report. */
static uint32_t
adversarial_lower(int mode, size_t honest)
{
	switch (mode)
	{
		case 0:
			return (uint32_t) honest;	/* honest */
		case 1:
			return 0;			/* zeroed header: BELOW contents -> G22 */
		case 2:
			return (uint32_t) (rnd() % CONTENTS_OFF);	/* torn, below contents */
		case 3:
			return (uint32_t) PAGESZ;	/* exactly full */
		case 4:
			return (uint32_t) (PAGESZ + 1 + rnd() % 4096);	/* past the page */
		case 5:
			return 0xFFFFFFFFu; /* all ones */
		case 6:
			return 0xC2FFFFFFu; /* the 3.4 GB shape from the field report */
		case 7:
			return (uint32_t) (CONTENTS_OFF + rnd() % (PAYLOAD + 1));	/* plausible */
		default:
			return rnd();		/* anything */
	}
}

/*
 * P5: a faithful transcription of weave_read_blob() (src/pages/trgm_page.c),
 * with `weave_page_blob_chunk(page, len, off)` -- the include/weave/am.h wrapper,
 * whose only backend content is reading pd_lower off the page header -- inlined
 * as its integer call.
 *
 * Every page is malloc'd at exactly BLCKSZ and the destination at exactly the
 * declared blob length, so ASan's redzones bound both sides: a source overread
 * (the G22 bug) and a destination overrun (the bug G22 did NOT have, kept in the
 * corpus so a "fix" that traded one for the other is caught).
 */
static size_t
blob_chain_read(size_t npages, size_t len, int lowermode)
{
	unsigned char **pages = (unsigned char **) malloc(npages * sizeof(*pages));
	unsigned char *buf = (unsigned char *) malloc(len ? len : 1);
	size_t		off = 0;
	size_t		i;

	assert(pages != NULL && buf != NULL);
	for (i = 0; i < npages; i++)
	{
		pages[i] = (unsigned char *) malloc(PAGESZ);
		assert(pages[i] != NULL);
		memset(pages[i], (int) ('a' + (i % 26)), PAGESZ);
	}

	for (i = 0; i < npages && off < len; i++)
	{
		uint32_t	lower = adversarial_lower(lowermode, PAGESZ);
		size_t		chunk = weave_page_blob_chunk_len(PAGESZ, CONTENTS_OFF,
													  lower, len, off);

		/* the destination is bounded even in the planted builds; it is the
		 * SOURCE that G22 walked off, and ASan is the judge of that */
		assert(off + chunk <= len);
		memcpy(buf + off, pages[i] + CONTENTS_OFF, chunk);
		off += chunk;
	}

	for (i = 0; i < npages; i++)
		free(pages[i]);
	free(pages);
	free(buf);
	return off;
}

int
main(void)
{
	static const size_t blkszs[] = {512, 1024, 4096, PAGESZ, 32768};
	unsigned long iters = 0;
	size_t		bi;
	int			trial;

	/*
	 * 1. P5 first, deliberately: the blob-chain read over ASan-redzoned pages.
	 * A planted-bug build then reports the G22 bug in its own terms -- a
	 * heap-buffer-overflow READ of the source page -- rather than tripping an
	 * assertion three passes later.  The declared length spans several pages,
	 * which is exactly what made the G22 clamp useless, and sometimes exceeds
	 * what the chain can hold.
	 */
	for (trial = 0; trial < 40000; trial++)
	{
		size_t		npages = 1 + (size_t) (rnd() % 6);
		int			lowermode = trial % 9;
		size_t		len;

		switch (trial % 4)
		{
			case 0:
				len = npages * PAYLOAD;	/* exactly the honest chain length */
				break;
			case 1:
				len = npages * PAYLOAD + 1 + rnd() % 4096;	/* torn: too long */
				break;
			case 2:
				len = (size_t) rnd64() % (npages * PAGESZ + 17);
				break;
			default:
				len = 1 + (size_t) rnd() % 64;	/* shorter than one page */
				break;
		}
		(void) blob_chain_read(npages, len, lowermode);
		iters++;
	}

	/* 2. the grid: every interesting (blcksz, contents_off, lower) triple,
	 * including the nonsense-geometry column no caller can produce */
	for (bi = 0; bi < sizeof(blkszs) / sizeof(blkszs[0]); bi++)
	{
		size_t		blcksz = blkszs[bi];
		size_t		conts[] = {0, 1, 8, 16, 24, 32, 64,
			blcksz - 1, blcksz, blcksz + 1, blcksz * 2, (size_t) -1
		};
		size_t		ci;

		for (ci = 0; ci < sizeof(conts) / sizeof(conts[0]); ci++)
		{
			uint32_t	lowers[] = {0, 1, 7, 8, 23, 24, 25, 63, 64,
				(uint32_t) (blcksz - 1), (uint32_t) blcksz,
				(uint32_t) (blcksz + 1), (uint32_t) (blcksz * 2),
				0x7FFFFFFFu, 0x80000000u, 0xC2FFFFFFu, 0xFFFFFFFFu
			};
			size_t		li;

			for (li = 0; li < sizeof(lowers) / sizeof(lowers[0]); li++)
			{
				size_t		lens[] = {0, 1, 17, PAYLOAD, PAGESZ, 65536,
					(size_t) 1 << 40
				};
				size_t		xi;

				for (xi = 0; xi < sizeof(lens) / sizeof(lens[0]); xi++)
				{
					/* off before, at and past the declared length */
					check_props(blcksz, conts[ci], lowers[li], lens[xi], 0);
					check_props(blcksz, conts[ci], lowers[li], lens[xi],
								lens[xi] / 2);
					check_props(blcksz, conts[ci], lowers[li], lens[xi],
								lens[xi]);
					check_props(blcksz, conts[ci], lowers[li], lens[xi],
								lens[xi] + 1);
					iters += 4;
				}
			}
		}
	}

	/* 3. fully random triples: nothing about the real page layout assumed */
	for (trial = 0; trial < 400000; trial++)
	{
		size_t		blcksz = (size_t) (rnd64() % 65537);
		size_t		contents_off = (rnd() & 3) == 0
			? (size_t) rnd64()	/* nonsense geometry */
			: (size_t) (rnd64() % (blcksz + 1));
		uint32_t	lower = rnd();
		size_t		len = (size_t) rnd64() % 1000003;
		size_t		off = (size_t) rnd64() % 1000003;

		check_props(blcksz, contents_off, lower, len, off);
		iters++;
	}

	/* 4. the realistic shape: a real BLCKSZ page whose pd_lower is adversarial.
	 * Note the real domain is narrower than this: pd_lower is a uint16 on disk,
	 * so a page can only report 0..65535.  am.h widens it to uint32 on the way
	 * in and this target fuzzes the whole widened domain, because a guard that is
	 * only correct for the values today's page header can hold is a guard that
	 * breaks silently if that ever changes. */
	for (trial = 0; trial < 400000; trial++)
	{
		uint32_t	lower = adversarial_lower(trial % 9, PAGESZ);
		size_t		len = (size_t) rnd64() % (8 * PAGESZ);
		size_t		off = (size_t) rnd64() % (8 * PAGESZ);

		check_props(PAGESZ, CONTENTS_OFF, lower, len, off);
		iters++;
	}

	printf("fuzz_pagebound: %lu cases -- end offset in range, no underflow, "
		   "no overread\n", iters);
	return 0;
}
