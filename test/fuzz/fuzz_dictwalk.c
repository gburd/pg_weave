/*
 * fuzz_dictwalk.c -- corruption/fuzz harness for the DICTIONARY PAGE WALK: the
 * loop that steps over variable-length WeaveDictEntry records on an index page,
 * taking its end bound from the page header's pd_lower and its stride from the
 * on-page de->termlen.
 *
 * WHY THIS TARGET EXISTS.  Eight of these walks shipped with no bounds check at
 * all.  The class is not hypothetical: the same defect in this project's
 * ancestor presented at field scale as `invalid memory alloc request size
 * 3406063183` raised from the streaming merge's page loader -- ~142M entries
 * counted where an 8 kB page holds a few hundred -- and because that loader sits
 * under merge, autovacuum cleanup AND explicit vacuum, the affected index could
 * never be vacuumed or reclaimed again.  A wrong `n` here is therefore not a
 * wrong answer, it is a permanently unmaintainable index.  See doc/GAPS.md G15.
 *
 * TWO DISTINCT HAZARDS, and the harness has teeth for both:
 *   (a) OVERRUN.  An unvalidated de->termlen oversteps the page; the walk then
 *       counts garbage entries, and that count sizes a heap allocation and
 *       bounds a second walk that WRITES.
 *   (b) UB AT POINTER FORMATION.  `page + pd_lower` for an out-of-range
 *       pd_lower is undefined behaviour *when the pointer is formed*, before
 *       any dereference.  A guard written as the pointer comparison
 *       `ptr < (char *) page + pd_lower` is itself the bug it means to prevent.
 *       Upstream's first version of this fix tripped exactly this, and it was a
 *       fuzz target under UBSan that said so.
 *
 * MODELING CHOICE, stated honestly (same as fuzz_block.c): the real guards are
 * `static inline` in include/weave/am.h, which needs postgres.h and so cannot be
 * compiled standalone.  weave_page_entry_end() and weave_dict_entry_fits() below
 * are transcriptions.  They are kept to three lines each, and the WeaveDictEntry
 * field order is transcribed from include/weave/am.h:247, to keep drift small.
 * If am.h's guards change, change these too -- a transcription that drifts
 * proves nothing.
 *
 * PROPERTY (default build, ASan+UBSan): for ANY page bytes and ANY pd_lower,
 * the guarded walk never reads past the 8192-byte page, never counts more
 * entries than a page can physically hold, and the second (writing) walk never
 * writes past the array the first walk sized.
 *
 * TEETH #1 (-DFUZZ_NO_FITS_GUARD=1): drop weave_dict_entry_fits.  A corrupt
 * termlen oversteps the page; ASan aborts.  This is the shipped-for-months bug.
 * TEETH #2 (-DFUZZ_RAW_PDLOWER=1): form the end pointer from an unvalidated
 * pd_lower.  UBSan aborts on the pointer-index overflow, with no dereference
 * required -- hazard (b).
 *
 * A THIRD TEETH BUILD WAS WRITTEN AND DID NOT BITE, and the reason is worth more
 * than the build was.  -DFUZZ_NO_SECOND_WALK_BOUND removed the `written < cap`
 * test from the writing pass, on the stated rationale that "both passes read a
 * buffer held under only BUFFER_LOCK_SHARE, so the two passes seeing the same
 * bytes is an assumption, not a fact".  That rationale is FALSE:
 * BUFFER_LOCK_SHARE excludes writers, and merge_source_load_page holds it across
 * both passes, so the two passes provably see identical bytes and the bound is
 * unreachable.  The build exited 0 -- i.e. the harness correctly reported that
 * there was no bug to find.  The `written < cap` test is kept in the real code
 * because it makes the loop safe to read in isolation, but it is
 * belt-and-braces, NOT load-bearing, and the code comment says so.
 *
 * The general lesson, which this project has now paid for twice: a planted-bug
 * build that exits 0 is either a toothless harness or a wrong claim about the
 * code.  Establish which before changing either.
 *
 * No hegel/cmocka: deterministic PRNG, fixed seed, reproducible, zero deps.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGESZ 8192				/* BLCKSZ */
#define MAXALIGN(x) (((x) + 7) & ~((uintptr_t) 7))
#define SIZEOF_PAGE_HEADER 24	/* MAXALIGN(SizeOfPageHeaderData) */

/* Transcribed from include/weave/am.h:247 (WeaveDictEntry). */
typedef struct DictEntry
{
	uint32_t	termlen;
	uint32_t	df;
	uint32_t	max_tf;
	uint32_t	firstoffset;
	uint32_t	firstposting;
	char		term[];			/* FLEXIBLE_ARRAY_MEMBER */
}			DictEntry;

#define ENTRY_HDR ((size_t) offsetof(DictEntry, term))

/* The largest number of entries an 8 kB page can physically hold: every entry
 * costs at least the aligned header.  Used as the harness's independent
 * postcondition -- it is NOT derived from the code under test. */
#define MAX_ENTRIES_PER_PAGE ((PAGESZ - SIZEOF_PAGE_HEADER) / MAXALIGN(ENTRY_HDR) + 1)

/* --- transcriptions of the two guards in include/weave/am.h -------------- */

static char *
page_entry_end(char *page, uint32_t pd_lower)
{
#if FUZZ_RAW_PDLOWER
	/* TEETH #2: the pointer is formed from an untrusted integer.  This is UB
	 * even with no dereference, and UBSan's pointer-overflow check fires. */
	return page + pd_lower;
#else
	if ((size_t) pd_lower > (size_t) PAGESZ ||
		(size_t) pd_lower < (size_t) SIZEOF_PAGE_HEADER)
		return page + SIZEOF_PAGE_HEADER;	/* implausible: page is empty */
	return page + pd_lower;
#endif
}

static int
dict_entry_fits(const DictEntry *de, const char *end)
{
#if FUZZ_NO_FITS_GUARD
	(void) de;
	(void) end;
	return 1;					/* TEETH #1: the shipped-for-months behaviour */
#else
	const char *t = (const char *) de + ENTRY_HDR;

	return t <= end && t + de->termlen <= end;
#endif
}

/* --- the site under test ------------------------------------------------- */

/*
 * Faithful transcription of merge_source_load_page()'s two passes
 * (src/am/ambuild.c): pass 1 counts entries and term bytes and SIZES the arrays
 * from that count; pass 2 fills them.  Returns the entry count.
 */
static int
dict_walk_two_pass(char *page, uint32_t pd_lower, int *out_maxwrite)
{
	char	   *ptr = page + SIZEOF_PAGE_HEADER;
	char	   *end = page_entry_end(page, pd_lower);
	size_t		used = 0;
	int			n = 0;
	int			cap;
	size_t		bytescap;
	char	   *terms;
	uint32_t   *lens;
	int			written = 0;

	/* pass 1: count */
	while (ptr < end)
	{
		DictEntry  *de = (DictEntry *) ptr;

		if (!dict_entry_fits(de, end))
			break;
		n++;
		used += de->termlen;
		ptr += MAXALIGN(ENTRY_HDR + de->termlen);
	}

	/* The independent postcondition: a page cannot hold more entries than
	 * this, whatever its bytes say.  A count above it means the walk left the
	 * page -- which is the failure this target exists to catch, and which ASan
	 * may or may not see depending on where the page sits in memory. */
	assert(n >= 0 && (size_t) n <= MAX_ENTRIES_PER_PAGE);
	assert(used <= (size_t) PAGESZ);

	cap = n > 0 ? n : 1;
	bytescap = used > 0 ? used : 1;
	terms = malloc(bytescap);
	lens = malloc((size_t) cap * sizeof(uint32_t));
	assert(terms != NULL && lens != NULL);

	/* pass 2: fill.  Applies the identical entry-fits bound.  The `written < cap`
	 * and byte-capacity tests are belt-and-braces: the share lock is held across
	 * both passes, so they see the same bytes and these cannot fire.  Kept so the
	 * loop is safe to read in isolation; see the header comment for the planted-bug
	 * build that proved they are not load-bearing. */
	ptr = page + SIZEOF_PAGE_HEADER;
	used = 0;
	while (ptr < end && written < cap)
	{
		DictEntry  *de = (DictEntry *) ptr;

		if (!dict_entry_fits(de, end))
			break;
		if (used + de->termlen > bytescap)
			break;
		memcpy(terms + used, de->term, de->termlen);
		lens[written++] = de->termlen;
		used += de->termlen;
		ptr += MAXALIGN(ENTRY_HDR + de->termlen);
	}

	free(terms);
	free(lens);
	*out_maxwrite = written;
	return n;
}

/* --- corpus generation --------------------------------------------------- */

static uint64_t rngstate = 0x9e3779b97f4a7c15ULL;

static uint32_t
rnd(void)
{
	rngstate ^= rngstate << 13;
	rngstate ^= rngstate >> 7;
	rngstate ^= rngstate << 17;
	return (uint32_t) (rngstate >> 11);
}

/* Lay down a WELL-FORMED run of entries, then corrupt one field.  A purely
 * random page is a weak test: it almost never produces a plausible-looking walk,
 * which is precisely the case that shipped. */
static uint32_t
build_page(char *page, int mode)
{
	char	   *ptr = page + SIZEOF_PAGE_HEADER;
	char	   *limit = page + PAGESZ;
	int			nwritten = 0;

	memset(page, 0, PAGESZ);
	while (nwritten < 40)
	{
		uint32_t	termlen = 1 + (rnd() % 24);
		size_t		step = MAXALIGN(ENTRY_HDR + termlen);
		DictEntry  *de;

		if (ptr + step > limit)
			break;
		de = (DictEntry *) ptr;
		de->termlen = termlen;
		de->df = 1 + (rnd() % 1000);
		de->max_tf = 1 + (rnd() % 8);
		de->firstoffset = rnd() % 8192;
		de->firstposting = rnd() % 1000;
		memset(de->term, 'a' + (int) (rnd() % 26), termlen);
		ptr += step;
		nwritten++;
	}

	{
		uint32_t	honest_lower = (uint32_t) (ptr - page);

		switch (mode)
		{
			case 0:				/* honest page */
				return honest_lower;
			case 1:				/* corrupt ONE termlen to a huge value */
				if (nwritten > 0)
				{
					DictEntry  *de = (DictEntry *) (page + SIZEOF_PAGE_HEADER);

					de->termlen = 0x40000000u + rnd();
				}
				return honest_lower;
			case 2:				/* pd_lower past the page */
				return PAGESZ + 1 + (rnd() % 100000);
			case 3:				/* pd_lower below the page header */
				return rnd() % SIZEOF_PAGE_HEADER;
			case 4:				/* pd_lower absurd (the 3.4 GB shape) */
				return 0xC2FFFFFFu;
			case 5:				/* every termlen corrupt */
				{
					char	   *p = page + SIZEOF_PAGE_HEADER;
					int			i;

					for (i = 0; i < nwritten && p + ENTRY_HDR <= limit; i++)
					{
						DictEntry  *de = (DictEntry *) p;
						uint32_t	honest = de->termlen;

						de->termlen = rnd();
						p += MAXALIGN(ENTRY_HDR + honest);
					}
					return honest_lower;
				}
			case 6:				/* termlen 0 everywhere (stride must not be 0) */
				{
					char	   *p = page + SIZEOF_PAGE_HEADER;
					int			i;

					for (i = 0; i < nwritten && p + ENTRY_HDR <= limit; i++)
					{
						DictEntry  *de = (DictEntry *) p;
						uint32_t	honest = de->termlen;

						de->termlen = 0;
						p += MAXALIGN(ENTRY_HDR + honest);
					}
					return honest_lower;
				}
			default:			/* fully random page bytes */
				{
					size_t		i;

					for (i = SIZEOF_PAGE_HEADER; i < PAGESZ; i++)
						page[i] = (char) rnd();
					return rnd();
				}
		}
	}
}

int
main(void)
{
	int			iter;
	long		total_entries = 0;

	for (iter = 0; iter < 200000; iter++)
	{
		/* malloc, not a stack array: ASan redzones a heap allocation, so a read
		 * one byte past 8192 is a hard failure rather than a read into an
		 * adjacent stack slot. */
		char	   *page = malloc(PAGESZ);
		uint32_t	pd_lower;
		int			n,
					written;

		assert(page != NULL);
		pd_lower = build_page(page, iter % 8);
		n = dict_walk_two_pass(page, pd_lower, &written);
		assert(written <= n);
		total_entries += n;
		free(page);
	}

	printf("fuzz_dictwalk: 200000 pages walked, %ld entries accepted\n",
		   total_entries);
	return 0;
}
