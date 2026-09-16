/*-------------------------------------------------------------------------
 *
 * pagebound.h
 *		The integer-domain page-bound arithmetic every entry walk and every
 *		blob read in this access method depends on, as standalone C.
 *
 * WHY THIS IS A SEPARATE, PostgreSQL-FREE HEADER.  The value that bounds every
 * variable-length walk on a weave page is the page header's `pd_lower`, and it
 * comes off disk on a page held under only BUFFER_LOCK_SHARE: a recycled, torn
 * or corrupt page can report anything.  Two things then go wrong, and only the
 * first is obvious:
 *
 *   (1) `page + lower` for an out-of-range value is undefined behaviour *at the
 *       point the pointer is formed*, before anything is dereferenced.  So the
 *       validation has to happen on integers, not on pointers.
 *   (2) `avail = lower - contents_offset` is computed in an UNSIGNED type, so a
 *       page reporting `lower` BELOW the contents offset underflows `avail` to
 *       ~2^64.  A later `Min()` against a caller-supplied remaining length then
 *       clamps it to something plausible-looking that is not bounded by THIS
 *       page -- see doc/GAPS.md G22, where the clamp was the caller's remaining
 *       blob length, which spans a whole page chain, and the memcpy read past
 *       the page into adjacent shared buffers.  The destination was bounded,
 *       which is precisely why the site read as safe.
 *
 * Hazard (2) is arithmetic, not pointer arithmetic, and arithmetic is testable:
 * the functions below take (blcksz, contents_off, lower) as plain integers and
 * carry no PostgreSQL dependency, so test/fuzz/fuzz_pagebound.c drives THE SAME
 * CODE the backend runs.  include/weave/chandesc.h and include/weave/docvalid.h
 * are the exemplars for this split, and the reason is identical: a guard that
 * can only run inside a backend cannot be fuzzed, and a copy of it in a test
 * proves nothing about the copy that ships.
 *
 * `weave_page_entry_end()` and `weave_page_blob_chunk()` in include/weave/am.h
 * are thin wrappers over these -- they read `pd_lower` (the one place in the AM
 * that may; `make check-pdlower` enforces it) and hand it over as an integer.
 *
 * PLANTED-BUG BUILDS.  WEAVE_PAGEBOUND_PLANT_* compile the pre-G22 defect back
 * into these very functions so the fuzz target can be shown to catch it.  They
 * are compile-time removals in the real code rather than a weakened copy of it
 * in the test, for the reason recorded in test/fuzz/README.md: a transcribed
 * copy drifts out of step with what ships, and then the teeth check proves
 * something about a function nobody runs.  Never define them in a real build.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/pagebound.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_PAGEBOUND_H
#define WEAVE_PAGEBOUND_H

#include <stddef.h>
#include <stdint.h>

/*
 * Where does the writer's variable-length entry area on this page end, as a
 * BYTE OFFSET from the start of the page?
 *
 * `lower` is the raw on-disk item-area end (the page header's pd_lower, handed
 * over as an integer).  `contents_off` is where the readable contents begin
 * (what PageGetContents() implies).  An implausible `lower` yields
 * `contents_off` -- i.e. the page is treated as EMPTY and the walk terminates
 * immediately -- rather than an error: these walks run under VACUUM, cleanup
 * and merge, and an ereport there is how an index becomes permanently
 * unvacuumable (doc/GAPS.md G15).
 *
 * POSTCONDITION, which is what the fuzz target asserts: the result is always in
 * [min(contents_off, blcksz), blcksz].  It follows that the difference
 * `result - min(contents_off, blcksz)` cannot underflow, which is the whole
 * point of the function existing.
 *
 * `contents_off > blcksz` is nonsense geometry rather than corruption (no caller
 * can produce it: the contents offset is a compile-time property of the page
 * layout).  It is clamped rather than asserted so the postcondition above holds
 * for every input, including the ones only a fuzzer supplies.
 */
static inline size_t
weave_page_entry_end_off(size_t blcksz, size_t contents_off, uint32_t lower)
{
	size_t		low = contents_off > blcksz ? blcksz : contents_off;

#ifdef WEAVE_PAGEBOUND_PLANT_NO_LOW_GUARD
	/*
	 * PLANTED BUG: only the upper half of the guard, which is the half someone
	 * writes when they are thinking "the page is 8 kB, so bound it by 8 kB".
	 * It is not enough: a `lower` below the contents offset passes this test and
	 * underflows every subsequent `end - contents_off`.
	 */
	if ((size_t) lower > blcksz)
		return low;
	return (size_t) lower;
#else
	if ((size_t) lower > blcksz || (size_t) lower < low)
		return low;				/* implausible: treat the page as empty */
	return (size_t) lower;
#endif
}

/*
 * How many bytes of contents are readable on this page?
 *
 * POSTCONDITION: <= blcksz - min(contents_off, blcksz).  Never underflows.
 */
static inline size_t
weave_page_entry_avail(size_t blcksz, size_t contents_off, uint32_t lower)
{
	size_t		low = contents_off > blcksz ? blcksz : contents_off;

#ifdef WEAVE_PAGEBOUND_PLANT_RAW_SUB
	/*
	 * PLANTED BUG: the exact pre-G22 expression from src/pages/trgm_page.c,
	 *
	 *		avail = ((PageHeader) page)->pd_lower -
	 *			((char *) PageGetContents(page) - (char *) page);
	 *
	 * unsigned, unguarded, and ~2^64 for any page reporting a low pd_lower.
	 */
	return (size_t) lower - low;
#else
	return weave_page_entry_end_off(blcksz, contents_off, lower) - low;
#endif
}

/*
 * How many bytes may a blob reader copy out of THIS page, when it is `off`
 * bytes into a blob of `len` bytes spread over a page chain?
 *
 * The second clamp is what made G22 dangerous rather than merely wrong: `len`
 * bounds the DESTINATION over the whole chain, so on its own it says nothing
 * about how many bytes this page actually has.  Both bounds are needed, and
 * both are applied here so no caller can apply only one.
 *
 * POSTCONDITION: <= the page's readable bytes AND <= len - off (0 when
 * off >= len).
 */
static inline size_t
weave_page_blob_chunk_len(size_t blcksz, size_t contents_off, uint32_t lower,
						  size_t len, size_t off)
{
	size_t		avail = weave_page_entry_avail(blcksz, contents_off, lower);
	size_t		remain = off < len ? len - off : 0;

	return avail < remain ? avail : remain;
}

#endif							/* WEAVE_PAGEBOUND_H */
