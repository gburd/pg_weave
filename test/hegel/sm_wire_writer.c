/*-------------------------------------------------------------------------
 *
 * sm_wire_writer.c
 *		Write sparsemap blobs with the OLD library version.
 *
 * Half of the cross-version wire-compatibility check.  pg_weave stores sparsemap
 * blobs on disk -- per-segment livedocs tombstones and trigram term-ordinal
 * postings -- so re-vendoring the library carries a hazard an ordinary dependency
 * upgrade does not: existing indexes hold bytes written by the OLD version.  If
 * the new version reads them differently that is silent corruption of live data,
 * and it presents as a relevance bug rather than an error.
 *
 * This deliberately mirrors reality: the old version writes bytes to a file, and a
 * SEPARATE program built against the new version reads them back.  One process
 * including both headers fights the library's type namespacing for no benefit,
 * and would not model "bytes written months ago" as faithfully.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SM_EXPOSE_STRUCT
#include "sm_old.h"

#include "sm_wire_corpus.h"

int
main(int argc, char **argv)
{
	FILE	   *f;
	int			c;

	if (argc != 2)
	{
		fprintf(stderr, "usage: %s <out-file>\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "wb");
	if (!f)
	{
		perror("fopen");
		return 2;
	}

	printf("writer: sparsemap %s, %d cases\n", SM_VERSION_STRING, SMW_NCASES);

	for (c = 0; c < SMW_NCASES; c++)
	{
		static unsigned char buf[SMW_BUFSZ];
		static uint64_t bits[SMW_MAXBITS];
		sm_t		map;
		int			n,
					i;
		size_t		sz;
		uint32_t	hdr[3];

		n = smw_case(c, bits);
		memset(buf, 0, sizeof(buf));
		sm_init(&map, buf, sizeof(buf));

		for (i = 0; i < n; i++)
		{
			sm_t	   *mp = &map;

			if (sm_add_grow(&mp, bits[i]) != bits[i])
			{
				fprintf(stderr, "case %d: sm_add_grow(%llu) failed\n",
						c, (unsigned long long) bits[i]);
				return 1;
			}
			/* sm_add_grow may reallocate only for heap maps; ours is a fixed
			 * caller buffer, so the handle must not have moved. */
			if (mp != &map)
			{
				fprintf(stderr, "case %d: map handle moved\n", c);
				return 1;
			}
		}

		sz = sm_get_size(&map);

		/* case index, bit count, blob size -- then the bits, then the blob */
		hdr[0] = (uint32_t) c;
		hdr[1] = (uint32_t) n;
		hdr[2] = (uint32_t) sz;
		fwrite(hdr, sizeof(hdr), 1, f);
		fwrite(bits, sizeof(uint64_t), (size_t) n, f);
		fwrite(sm_get_data(&map), 1, sz, f);
	}

	fclose(f);
	printf("writer: wrote %s\n", argv[1]);
	return 0;
}
