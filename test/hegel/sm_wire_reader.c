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
 *   3. reproduction: the new version, given the same bits, serializes to output
 *      that ROUND-TRIPS to the same set, and -- whenever it lands in the same
 *      encoding the old version used -- is BYTE-IDENTICAL.  Byte-identity is the
 *      strongest form of "the wire format is unchanged": not merely readable, but
 *      reproduced.  It is asserted only when the new and old serialized SIZES
 *      match; a size difference means the new version chose a different, smaller
 *      encoding for the same set, which is a re-encoding, not a wire break.
 *
 *      NAMED NOTE (sparsemap v5.7.0, 2026-09-27): v5.7.0 adds a self-describing
 *      "small-set" mode -- a set whose largest index is below a small cap is stored
 *      as a bare uint64 word array behind the same 8-byte header, selected by the
 *      header word's top bit (SM_SMALL_FLAG).  For near-zero sets ({0}, {0..63},
 *      ...) this is SMALLER than the old chunk form, so this reader now sees the new
 *      library re-serialize those cases to fewer bytes than the old writer emitted.
 *      That is expected and safe: (a) old on-disk bytes always have that top bit
 *      clear (a chunk count never approaches 2^63) so they still decode as chunk
 *      mode -- checks (1) and (2) prove it for every corpus case; (b) pg_weave only
 *      ever reads its own blobs with an EQUAL-OR-NEWER library, never an older one,
 *      so the smaller encoding is forward-safe; (c) pg_weave keeps no canonical
 *      byte encoding of a blob (no byte-wise dedup/compare), so a set changing
 *      representation across a bump is invisible to it.  We therefore assert
 *      round-trip on the re-encoded cases and byte-identity on the rest, and print
 *      the re-encoded count so the divergence is never silent.
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
static long reencoded = 0;

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

		/* (3) reproduction: round-trip always; byte-identity when the new
		 * version lands in the same-sized encoding the old one used.  A smaller
		 * size is the v5.7.0 small-set re-encoding (see the NAMED NOTE above),
		 * not a wire break -- so we require the new bytes to round-trip. */
		memset(rebuilt, 0, sizeof(rebuilt));
		sm_init(&map2, rebuilt, sizeof(rebuilt));
		for (i = 0; i < n; i++)
		{
			sm_t	   *mp = &map2;

			sm_add_grow(&mp, bits[i]);
		}
		{
			size_t		nsz = sm_get_size(&map2);

			if (nsz == sz)
				CHECK(memcmp(sm_get_data(&map2), blob, sz) == 0,
					  "case %d (%s): NEW bytes differ from OLD for identical input",
					  c, smw_name(c));
			else
			{
				/* Re-encoded (expected for near-zero sets under v5.7.0's
				 * small-set mode): the new bytes must round-trip to the same
				 * set.  Open the new serialization fresh and re-check membership
				 * and iteration against the corpus. */
				static unsigned char rt[SMW_BUFSZ];
				sm_t		map3;
				int			rsteps = 0;
				uint64_t	ridx;
				sm_cursor_t rcur = SM_CURSOR_INIT;

				reencoded++;
				memcpy(rt, sm_get_data(&map2), nsz);
				sm_open(&map3, rt, nsz);
				for (i = 0; i < n; i++)
					CHECK(sm_contains(&map3, bits[i], NULL),
						  "case %d (%s): re-encoded NEW blob lost bit %llu",
						  c, smw_name(c), (unsigned long long) bits[i]);
				ridx = SM_IDX_MAX;
				for (;;)
				{
					ridx = sm_next_member(&map3, ridx, &rcur);
					if (ridx == SM_IDX_MAX)
						break;
					if (rsteps < n)
						CHECK(ridx == bits[rsteps],
							  "case %d (%s): re-encoded iteration differs at "
							  "step %d", c, smw_name(c), rsteps);
					if (++rsteps > n + 8)
						break;
				}
				CHECK(rsteps == n, "case %d (%s): re-encoded iterated %d, "
					  "expected %d", c, smw_name(c), rsteps, n);
			}
		}
	}

	fclose(f);
	printf("\n%ld checks, %d failures, %ld case(s) re-encoded smaller by the new "
		   "version (round-trip verified)\n", checks, failures, reencoded);
	if (failures)
		printf("\nWIRE FORMAT IS NOT COMPATIBLE with the vendored version's "
			   "predecessor. Existing indexes would need a REINDEX; that must be "
			   "stated in the release notes and enforced by a format-version bump.\n");
	return failures == 0 ? 0 : 1;
}
