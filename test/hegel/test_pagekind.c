/*
 * test_pagekind.c -- exhaustive property test for the v6 page-kind space
 * (include/weave/pagekind.h).  No PostgreSQL, no cmocka, no hegel: plain C,
 * plain asserts, so `make check-standalone` can gate a build on it.
 *
 * WHY THIS TEST EXISTS.  doc/specs/SEGMENT_FORMAT.md sect. 2's v6 decision rests
 * on a claim about code that is not in this tree: what a v5 binary does when it
 * reaches a page written with the extended kind encoding.  The claim is that it
 * FAILS CLOSED -- matches no page kind and treats the page as absent -- rather
 * than mistaking, say, WEAVE_PK_CHANDESC (id 16) for a posting page.  A claim
 * like that is worth nothing as a paragraph, so property 4 below restates the v5
 * reader's rule (first matching bit wins, exactly as src/am/amsize.c did before
 * v6) and checks it against every allocated kind.
 *
 * The flag word is 16 bits, so "for all inputs" is 65,536 cases per kind value
 * and the whole space is enumerable.  Nothing here is random: an exhaustive
 * check is strictly better than a sampled one when the domain is this small, and
 * it means a regression cannot hide in an unsampled corner.
 *
 * Build: cc -O2 -Wall -Wextra -I include -o t test/hegel/test_pagekind.c
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "weave/pagekind.h"

/* Every id that is a real kind, in enum order. */
static const WeavePageKind all_kinds[] = {
	WEAVE_PK_META, WEAVE_PK_DICT, WEAVE_PK_POSTING, WEAVE_PK_PENDING,
	WEAVE_PK_TRGM, WEAVE_PK_TRGM_DATA, WEAVE_PK_LIVEDOCS, WEAVE_PK_DICTINDEX,
	WEAVE_PK_DOCLEN,
	WEAVE_PK_CHANDESC, WEAVE_PK_VMETA, WEAVE_PK_VCODES, WEAVE_PK_VGRAPH,
	WEAVE_PK_VRERANK, WEAVE_PK_SURF, WEAVE_PK_ULEV, WEAVE_PK_REGEX,
	WEAVE_PK_FUZZY_SPARE, WEAVE_PK_DOCVALS, WEAVE_PK_CGRAM
};

#define NKINDS ((int) (sizeof(all_kinds) / sizeof(all_kinds[0])))

/*
 * The v4/v5 reader's rule, transcribed from src/am/amsize.c as it stood before
 * v6: walk the kind bits in a fixed order and take the first one that is set.
 * Returns 0 when nothing matches, which is what "unclassified" was.
 *
 * This is a deliberate duplicate of code that no longer exists.  It is the only
 * way to test a compatibility claim about a binary we cannot run.
 */
static uint16_t
v5_first_match_bit(uint16_t flags)
{
	static const uint16_t order[] = {
		WEAVE_FREED, WEAVE_META, WEAVE_DICT, WEAVE_DICTINDEX, WEAVE_POSTING,
		WEAVE_DOCLEN, WEAVE_LIVEDOCS, WEAVE_TRGM, WEAVE_TRGM_DATA, WEAVE_PENDING
	};
	size_t		i;

	for (i = 0; i < sizeof(order) / sizeof(order[0]); i++)
		if ((flags & order[i]) != 0)
			return order[i];
	return 0;
}

int
main(void)
{
	int			i;
	uint32_t	f;
	unsigned long checks = 0;

	/*
	 * 1. ENCODE/DECODE IS A BIJECTION over every allocated kind, with and without
	 * the WEAVE_FREED state ORed in.  Freed-ness must not disturb the kind: a
	 * freed posting page still has to report POSTING, which is what lets
	 * weave_free_page() set the flag while leaving the kind in place.
	 */
	for (i = 0; i < NKINDS; i++)
	{
		uint16_t	flags;
		uint16_t	kind;

		weave_page_kind_encode(all_kinds[i], &flags, &kind);
		assert(weave_page_kind_decode(flags, kind) == all_kinds[i]);
		assert(weave_page_kind_decode((uint16_t) (flags | WEAVE_FREED), kind)
			   == all_kinds[i]);
		assert((flags & WEAVE_PAGE_RESERVED_MASK) == 0);
		checks += 2;
	}

	/*
	 * 2. THE SHIPPED KINDS DID NOT MOVE.  A v6 encoder must produce exactly the
	 * byte pattern v5 produced for each of the ten legacy kinds: the one-hot bit
	 * in `flags`, zero in `kind`, escape bit clear.  This is the property that
	 * makes v6 a no-page-rewrite upgrade; if it ever breaks, every existing index
	 * needs a REINDEX.
	 */
	for (i = 0; i < NKINDS; i++)
	{
		uint16_t	bit = weave_page_kind_legacy_bit(all_kinds[i]);
		uint16_t	flags;
		uint16_t	kind;

		if (bit == 0)
			continue;			/* an extended kind, checked by property 3 */
		weave_page_kind_encode(all_kinds[i], &flags, &kind);
		assert(flags == bit);
		assert(kind == 0);
		assert((flags & WEAVE_PAGE_KIND_EXT) == 0);
		/* one-hot: exactly one bit, and it is inside the kind mask */
		assert((bit & (uint16_t) (bit - 1)) == 0);
		assert((bit & WEAVE_PAGE_KIND_MASK) == bit);
		checks++;
	}

	/*
	 * 3. EVERY EXTENDED KIND USES THE ESCAPE AND NOTHING ELSE.  flags must be
	 * exactly the escape bit -- no legacy kind bit, no reserved bit -- because
	 * property 4 depends on it.
	 */
	for (i = 0; i < NKINDS; i++)
	{
		uint16_t	flags;
		uint16_t	kind;

		if (weave_page_kind_legacy_bit(all_kinds[i]) != 0)
			continue;
		weave_page_kind_encode(all_kinds[i], &flags, &kind);
		assert(flags == WEAVE_PAGE_KIND_EXT);
		assert(kind == (uint16_t) all_kinds[i]);
		assert(kind >= (uint16_t) WEAVE_PK_EXT_FIRST);
		checks++;
	}

	/*
	 * 4. FAIL-CLOSED AGAINST A v5 READER.  For every extended kind, the v5
	 * first-match rule must find NO kind bit, so a v5 binary classifies the page
	 * as unclassified rather than as one of its own kinds.  This is the whole
	 * reason the kind integer lives in `opaque.kind` and not in the low bits of
	 * `flags`; the counter-example in the header (kind 20 reading as
	 * POSTING|TRGM) is checked here too.
	 */
	for (i = 0; i < NKINDS; i++)
	{
		uint16_t	flags;
		uint16_t	kind;

		if (weave_page_kind_legacy_bit(all_kinds[i]) != 0)
			continue;
		weave_page_kind_encode(all_kinds[i], &flags, &kind);
		assert(v5_first_match_bit(flags) == 0);
		/* and with the freed state set, a v5 reader still sees only "freed" --
		 * which is the correct answer for a freed page of any kind */
		assert(v5_first_match_bit((uint16_t) (flags | WEAVE_FREED)) == WEAVE_FREED);
		/* the counter-example: had the id gone in the low bits, this would fail */
		assert(((uint16_t) all_kinds[i] & WEAVE_PAGE_KIND_MASK) != 0);
		checks += 2;
	}

	/*
	 * 5. THE DECODER IS TOTAL AND NEVER LIES.  Exhaustive over all 65,536 flag
	 * words crossed with kind ids around the allocated range plus a few wild
	 * ones.  For every input the result must be a value in [0, WEAVE_PK_NKINDS),
	 * and when the escape bit is set the result must never be a LEGACY kind --
	 * the inverse of property 4, and the guard against a future edit that lets an
	 * extended id alias a legacy one.
	 */
	for (f = 0; f <= 0xFFFFu; f++)
	{
		static const uint16_t kinds[] = {
			0, 1, 9, 15, 16, 17, 26,
			(uint16_t) WEAVE_PK_NKINDS,
			(uint16_t) WEAVE_PK_NKINDS + 1,
			255, 256, 32767, 65535
		};
		size_t		k;

		for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
		{
			WeavePageKind got = weave_page_kind_decode((uint16_t) f, kinds[k]);

			assert(got >= WEAVE_PK_UNKNOWN && got < WEAVE_PK_NKINDS);

			if ((f & WEAVE_PAGE_KIND_EXT) != 0 && got != WEAVE_PK_UNKNOWN)
			{
				/* extended encoding accepted: it must be an extended id, and the
				 * page must have carried no legacy bit and no reserved bit */
				assert(got >= WEAVE_PK_EXT_FIRST);
				assert(weave_page_kind_legacy_bit(got) == 0);
				assert((f & WEAVE_PAGE_KIND_MASK) == 0);
				assert((f & WEAVE_PAGE_RESERVED_MASK) == 0);
				assert((uint16_t) got == kinds[k]);
			}
			if ((f & WEAVE_PAGE_KIND_EXT) == 0 && got != WEAVE_PK_UNKNOWN)
			{
				/* legacy encoding accepted: exactly one kind bit, and the decode
				 * agrees with the v5 first-match rule once FREED is masked off */
				uint16_t	bit = weave_page_kind_legacy_bit(got);

				assert(bit != 0);
				assert((f & WEAVE_PAGE_KIND_MASK) == bit);
				assert((f & WEAVE_PAGE_RESERVED_MASK) == 0);
				assert(v5_first_match_bit((uint16_t) (f & ~(uint32_t) WEAVE_FREED))
					   == bit);
			}
			/* a reserved bit anywhere is always a refusal, never a guess */
			if ((f & WEAVE_PAGE_RESERVED_MASK) != 0)
				assert(got == WEAVE_PK_UNKNOWN);
			checks++;
		}
	}

	/*
	 * 6. NO TWO KINDS SHARE AN ID, and every id is either legacy-bit-backed or in
	 * the extended range.  Cheap, and it is the check that catches a second
	 * channel quietly reusing a number the first one took -- the exact hazard
	 * doc/specs/SEGMENT_FORMAT.md sect. 2 says the single allocation table exists
	 * to prevent.
	 */
	for (i = 0; i < NKINDS; i++)
	{
		int			j;

		assert(all_kinds[i] != WEAVE_PK_UNKNOWN);
		assert(all_kinds[i] < WEAVE_PK_NKINDS);
		assert(weave_page_kind_legacy_bit(all_kinds[i]) != 0 ||
			   all_kinds[i] >= WEAVE_PK_EXT_FIRST);
		for (j = 0; j < i; j++)
		{
			assert(all_kinds[i] != all_kinds[j]);
			if (weave_page_kind_legacy_bit(all_kinds[i]) != 0)
				assert(weave_page_kind_legacy_bit(all_kinds[i]) !=
					   weave_page_kind_legacy_bit(all_kinds[j]));
		}
		checks++;
	}

	/* 7. Every allocated kind has a distinct enum id AND the enum covers exactly
	 * the ids we enumerate: WEAVE_PK_NKINDS must be one past the last. */
	assert(all_kinds[NKINDS - 1] == WEAVE_PK_NKINDS - 1);

	printf("test_pagekind: %lu exhaustive checks over all 2^16 flag words -- PASS\n",
		   checks);
	return 0;
}
