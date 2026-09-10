/*
 * test/hegel/test_tre_intmax_crash.c -- pg_tre 1521662 regression: TRE
 * INT_MAX crash fix (upstream ad26b6d, "Avoid crashing on large inputs").
 *
 * lib/tre-match-{approx,backtrack,parallel}.c used plain `int` for `pos`
 * and `len` throughout.  lib/regexec.c's tre_match() cast a caller-supplied
 * size_t length to `int` before calling into those matchers.  A length
 * greater than INT_MAX (~2 GiB) overflows that cast to a negative value,
 * which the matchers' own `len < 0` check reads as the "unknown length,
 * scan for a NUL terminator" sentinel -- so a correctly-sized, real buffer
 * gets treated as unbounded and the matcher walks straight past its end.
 *
 * This is opt-in (see test/hegel/run_tre_bump.sh and `make
 * check-tre-bump-intmax`), not part of the default check-standalone gate:
 * demonstrating it for real needs an actual buffer bigger than INT_MAX
 * bytes (~2 GiB), which this allocates via mmap with a PROT_NONE guard
 * page immediately after it so the overread is a deterministic, immediate
 * SIGSEGV rather than a slow crawl through however much heap happens to
 * follow.  The buffer is filled entirely with 'a' (no NUL anywhere), and
 * the pattern ("z") cannot match, so a correct matcher must scan the whole
 * declared length and nothing more.
 *
 * Old TRE: the (int) cast makes the declared length negative, the low-level
 * matcher (lib/tre-match-parallel.c has no backrefs so tre_match() picks it)
 * treats that as "keep scanning for NUL", and it segfaults on the guard
 * page.  New TRE: `len` stays `ssize_t` end to end and gets clamped to the
 * new `TRE_MAX_STRING` (== INT_MAX, lib/tre-internal.h), which is still
 * inside the real buffer, so it scans exactly that many bytes and returns
 * REG_NOMATCH cleanly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>
#include "tre.h"

int
main(void)
{
	size_t pagesize = (size_t) sysconf(_SC_PAGESIZE);
	size_t buf_size = (size_t) INT_MAX + 4096; /* > INT_MAX: triggers the (int) cast */
	size_t map_size;
	unsigned char *base;
	regex_t preg;
	regmatch_t pmatch[1];
	int rc;

	/* Round up to a page boundary: mprotect() requires page alignment. */
	buf_size = ((buf_size + pagesize - 1) / pagesize) * pagesize;
	map_size = buf_size + pagesize;

	base = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
	{
		perror("mmap");
		return 3;
	}
	if (mprotect(base + buf_size, pagesize, PROT_NONE) != 0)
	{
		perror("mprotect");
		return 3;
	}
	memset(base, 'a', buf_size);	/* no NUL anywhere in the real buffer */

	rc = tre_regcomp(&preg, "z", REG_EXTENDED | REG_NOSUB);
	if (rc != REG_OK)
	{
		fprintf(stderr, "tre_regcomp failed: %d\n", rc);
		return 2;
	}
	/* Real, correct length -- old TRE mishandles this via (int)len. */
	rc = tre_regnexec(&preg, (const char *) base, buf_size, 0, pmatch, 0);
	tre_regfree(&preg);

	printf("rc=%d\n", rc);		/* if we get here at all, we did not crash */
	munmap(base, map_size);
	return 0;
}
