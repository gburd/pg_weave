/*-------------------------------------------------------------------------
 *
 * sm_wire_reader.c
 *		Read OLD-version sparsemap blobs with the NEW library version.
 *
 * The other half of the cross-version wire-compatibility check; see
 * sm_wire_writer.c for why this is two programs and not one.
 *
 * Verifies three things about every blob the old version wrote:
 *
 *   1. membership: every bit the old version set is still found
 *   2. iteration:  sm_next_member yields exactly those bits, in order.  This is
 *      the operation pg_weave uses to walk tombstones and trigram term ordinals,
 *      and it is in the family sparsemap 5.5.0 fixed for big-endian hosts -- so if
 *      little-endian behaviour changed too, every existing index needs a REINDEX
 *      and the release notes must say so.
 *   3. reproduction: the new version, given the same bits, serializes to
 *      BYTE-IDENTICAL output.  That is the strongest form of "the wire format is
 *      unchanged": not merely readable, but reproduced.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Use the SAME prefix the vendored library is compiled with (weave/sparsemap.h
 * sets it), so this test exercises pg_weave's actual configuration rather than a
 * hypothetical unprefixed one.  The header macro-renames every public symbol, so
 * the plain sm_* names below resolve to __pg_weave_sm_* exactly as they do in
 * src/am/amscan.c.
 */
#define SPARSEMAP_PREFIX __pg_weave_
#define SM_EXPOSE_STRUCT
#include "weave/sparsemap_impl.h"

#include "sm_wire_corpus.h"

static int	failures = 0;
static long checks = 0;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			if (failures <= 25) { \
				printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); \
			} \
		} \
	} while (0)

int
main(int argc, char **argv)
{
	FILE	   *f;
	int			c;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <in-file>\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f)
	{
		perror("fopen");
		return 2;
	}

	printf("reader: sparsemap %s\n", SM_VERSION_STRING);

	for (c = 0; c < SMW_NCASES; c++)
	{
		static unsigned char blob[SMW_BUFSZ];
		static unsigned char rebuilt[SMW_BUFSZ];
		static uint64_t bits[SMW_MAXBITS];
		static uint64_t expect[SMW_MAXBITS];
		uint32_t	hdr[3];
		sm_t		map;
		sm_t		map2;
		int			n,
					i,
					steps;
		size_t		sz;
		uint64_t	idx;
		sm_cursor_t cur = SM_CURSOR_INIT;

		if (fread(hdr, sizeof(hdr), 1, f) != 1)
		{
			CHECK(0, "case %d: truncated header -- writer and reader disagree "
				  "about the corpus", c);
			break;
		}
		n = (int) hdr[1];
		sz = (size_t) hdr[2];
		CHECK((int) hdr[0] == c, "case %d: header says case %u", c, hdr[0]);
		CHECK(n <= SMW_MAXBITS && sz <= SMW_BUFSZ,
			  "case %d: implausible n=%d sz=%zu", c, n, sz);
		if (n > SMW_MAXBITS || sz > SMW_BUFSZ)
			break;
		if (fread(bits, sizeof(uint64_t), (size_t) n, f) != (size_t) n
			|| fread(blob, 1, sz, f) != sz)
		{
			CHECK(0, "case %d: truncated body", c);
			break;
		}

		/* The corpus generator must agree with what the writer recorded; if not,
		 * a failure below would be a test bug rather than a library bug. */
		{
			int			n2 = smw_case(c, expect);

			CHECK(n2 == n, "case %d: corpus says %d bits, writer recorded %d",
				  c, n2, n);
			if (n2 == n)
				CHECK(memcmp(expect, bits, sizeof(uint64_t) * (size_t) n) == 0,
					  "case %d: corpus bits differ from the writer's", c);
		}

		/* (1) membership, exactly as a scan reads a page blob */
		sm_open(&map, blob, sz);
		for (i = 0; i < n; i++)
			CHECK(sm_contains(&map, bits[i], NULL),
				  "case %d (%s): NEW lost set bit %llu", c, smw_name(c),
				  (unsigned long long) bits[i]);

		/* (2) iteration order and length */
		steps = 0;
		/*
		 * SM_IDX_MAX is the START sentinel, not 0.  sm_next_member takes a LOWER
		 * EXCLUSIVE bound, so passing 0 asks for the next member strictly after
		 * bit 0 and silently skips it -- which is what the first version of this
		 * test did, producing a uniform off-by-one that looked exactly like a wire
		 * incompatibility.  src/am/am.c and src/pages/trgm_page.c both use
		 * (uint64_t) -1 here; matching them is the point.
		 */
		idx = SM_IDX_MAX;
		for (;;)
		{
			idx = sm_next_member(&map, idx, &cur);
			if (idx == SM_IDX_MAX)
				break;
			if (steps < n)
				CHECK(idx == bits[steps],
					  "case %d (%s): iteration differs at step %d: got %llu "
					  "want %llu", c, smw_name(c), steps,
					  (unsigned long long) idx,
					  (unsigned long long) bits[steps]);
			if (++steps > n + 8)
				break;
		}
		CHECK(steps == n, "case %d (%s): iterated %d bits, expected %d",
			  c, smw_name(c), steps, n);

		/* (3) byte-identical reproduction from the same inputs */
		memset(rebuilt, 0, sizeof(rebuilt));
		sm_init(&map2, rebuilt, sizeof(rebuilt));
		for (i = 0; i < n; i++)
		{
			sm_t	   *mp = &map2;

			sm_add_grow(&mp, bits[i]);
		}
		{
			size_t		nsz = sm_get_size(&map2);

			CHECK(nsz == sz, "case %d (%s): NEW serializes %zu bytes, OLD %zu",
				  c, smw_name(c), nsz, sz);
			if (nsz == sz)
				CHECK(memcmp(sm_get_data(&map2), blob, sz) == 0,
					  "case %d (%s): NEW bytes differ from OLD for identical input",
					  c, smw_name(c));
		}
	}

	fclose(f);
	printf("\n%ld checks, %d failures\n", checks, failures);
	if (failures)
		printf("\nWIRE FORMAT IS NOT COMPATIBLE with the vendored version's "
			   "predecessor. Existing indexes would need a REINDEX; that must be "
			   "stated in the release notes and enforced by a format-version bump.\n");
	return failures == 0 ? 0 : 1;
}
