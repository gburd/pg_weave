/*-------------------------------------------------------------------------
 *
 * test_bounds.c
 *		Contracts (C1), (C2) and (C5) for the boolean-gate shuttle's cursor
 *		core (include/weave/gate.h), task Z7.
 *
 * THIS IS THE TEST AGENTS.md HARD RULE 1 REQUIRES for the gate channel, and
 * FUZZY_CHANNEL.md sect. 8 names it as the Z7 gate.  There is no scored bound
 * to be 1 % too low here; the analogous silent failure is a seek that lands
 * one key late, or that answers a stale target with the current position, or a
 * score() that says "match" at a position no key occupies.  Each of those keeps
 * every answer plausible and drops or admits rows, and no fixed-output
 * regression test sees it -- so the cursor is compared against a linear-scan
 * oracle on random input.
 *
 * Properties, over random strictly-ascending key sets (empty, singleton, dense,
 * sparse across the full 32-bit range, clustered near UINT32_MAX) and random
 * NON-DECREASING target sequences (jumps, exact keys, key +/- 1, repeats of the
 * last return, 0, the sentinel):
 *
 *	G1	(C1) every return is >= its target, and >= the previous return
 *	G2	(C1) every return EQUALS the oracle's smallest key >= target, and END
 *		exactly when the oracle finds none
 *	G3	(C2) block_max >= score at every position in [cur, blkend]; the block is
 *		one position, so that is one comparison, made anyway rather than assumed
 *	G4	(C5) block_max is +INF at a key and -INF exactly when no key remains;
 *		score is 0.0 at the returned key and -INF at every other probe
 *	G5	a backward seek is REFUSED (WEAVE_GATE_BACKWARD) and leaves the cursor
 *		unchanged; seek(target) twice is refused when the first returned more
 *		than target (the channel.h "NOT IDEMPOTENT" note made concrete); after
 *		END every target below END is refused and END itself is not
 *	G6	weave_gate_check_keys() accepts every array the generator built
 *		ascending, and refuses an injected duplicate / descent / over-wide key
 *		at exactly the index it was injected, with the right code
 *
 * The infinities: the standalone test maps the core's two predicates to
 * +/-INFINITY the same way src/query/gate.c maps them to WEAVE_SCORE_ALWAYS /
 * WEAVE_SCORE_NEVER, so (C2) is asserted on the values a scorer would see.
 *
 * Build and run:
 *		cc -O2 -Wall -Wextra -I include -o /tmp/tb test/hegel/test_bounds.c && /tmp/tb
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_bounds.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/gate.h"

static long failures = 0;
static long checks = 0;
static long prop_checks[8];

#define CHECK(prop, cond, ...) \
	do { \
		checks++; \
		prop_checks[(prop)]++; \
		if (!(cond)) \
		{ \
			failures++; \
			if (failures <= 20) \
			{ \
				printf("FAIL G%d %s:%d: ", (prop), __FILE__, __LINE__); \
				printf(__VA_ARGS__); \
				printf("\n"); \
			} \
		} \
	} while (0)

/* xorshift64*, seeded fixed so a failure reproduces. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t
rnd64(void)
{
	uint64_t	x = rng_state;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static uint32_t
rnd_below(uint32_t n)
{
	return n == 0 ? 0 : (uint32_t) (rnd64() % n);
}

/* ---------------------------------------------------------------------------
 * The (C5) mapping, as the glue does it
 * ------------------------------------------------------------------------- */

static float
block_max_of(const WeaveGateCursor *c, uint32_t cur)
{
	if (cur == WEAVE_GATE_END || !weave_gate_block_may_match(c))
		return -INFINITY;
	return INFINITY;
}

static float
score_of(const WeaveGateCursor *c, uint32_t cur)
{
	if (cur == WEAVE_GATE_END || !weave_gate_matches(c, cur))
		return -INFINITY;
	return 0.0f;
}

/* ---------------------------------------------------------------------------
 * The oracle: smallest key >= t by linear scan, or END
 * ------------------------------------------------------------------------- */

static uint32_t
oracle_seek(const uint64_t *keys, uint32_t n, uint32_t t)
{
	uint32_t	i;

	for (i = 0; i < n; i++)
		if (keys[i] >= (uint64_t) t)
			return (uint32_t) keys[i];
	return WEAVE_GATE_END;
}

/* ---------------------------------------------------------------------------
 * Generators
 * ------------------------------------------------------------------------- */

typedef enum
{
	GEN_DENSE,					/* consecutive-ish from a random base */
	GEN_SPARSE,					/* uniform over [0, END) */
	GEN_TOP,					/* clustered just below END */
	GEN_MIXED,					/* runs of dense with sparse gaps */
	GEN_NKINDS
} GenKind;

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t	x = *(const uint64_t *) a;
	uint64_t	y = *(const uint64_t *) b;

	return x < y ? -1 : (x > y ? 1 : 0);
}

/*
 * Fill keys[] with n strictly ascending values below WEAVE_GATE_END.  Returns
 * the count actually produced (the top cluster can run out of room).
 */
static uint32_t
gen_keys(uint64_t *keys, uint32_t n, GenKind kind)
{
	uint64_t	limit = (uint64_t) WEAVE_GATE_END;	/* exclusive */
	uint64_t	v;
	uint32_t	i;

	switch (kind)
	{
		case GEN_DENSE:
			v = rnd_below(1 << 20);
			for (i = 0; i < n; i++)
			{
				v += 1 + rnd_below(3);
				if (v >= limit)
					return i;
				keys[i] = v;
			}
			return n;

		case GEN_SPARSE:
			/* sort a uniform sample, then dedupe by bumping */
			for (i = 0; i < n; i++)
				keys[i] = rnd64() % limit;
			qsort(keys, n, sizeof(uint64_t), cmp_u64);
			for (i = 1; i < n; i++)
				if (keys[i] <= keys[i - 1])
					keys[i] = keys[i - 1] + 1;
			while (n > 0 && keys[n - 1] >= limit)
				n--;
			return n;

		case GEN_TOP:
			v = limit - 1 - (uint64_t) n * 2 - rnd_below(8);
			for (i = 0; i < n; i++)
			{
				v += 1 + rnd_below(2);
				if (v >= limit)
					return i;
				keys[i] = v;
			}
			return n;

		case GEN_MIXED:
		default:
			v = rnd64() % (limit / 2);
			for (i = 0; i < n; i++)
			{
				if (rnd_below(16) == 0)
					v += rnd_below(1u << 24);
				else
					v += 1 + rnd_below(2);
				if (v >= limit)
					return i;
				keys[i] = v;
			}
			return n;
	}
}

/* A next target >= floor, drawn from the shapes a fused loop produces. */
static uint32_t
gen_target(const uint64_t *keys, uint32_t n, uint32_t floor)
{
	uint32_t	t;
	uint64_t	k;

	switch (rnd_below(8))
	{
		case 0:					/* stay */
			return floor;
		case 1:					/* small step */
			t = floor + 1 + rnd_below(4);
			return t < floor ? WEAVE_GATE_END : t;
		case 2:					/* exact key at or past floor */
		case 3:
			if (n == 0)
				return floor;
			k = keys[rnd_below(n)];
			return k >= floor ? (uint32_t) k : floor;
		case 4:					/* key - 1 */
			if (n == 0)
				return floor;
			k = keys[rnd_below(n)];
			if (k > 0)
				k--;
			return k >= floor ? (uint32_t) k : floor;
		case 5:					/* key + 1 */
			if (n == 0)
				return floor;
			k = keys[rnd_below(n)] + 1;
			return k >= floor ? (uint32_t) k : floor;
		case 6:					/* random jump */
			t = (uint32_t) rnd64();
			return t >= floor ? t : floor;
		default:				/* big jump within range */
			t = floor + rnd_below(1u << 28);
			return t < floor ? WEAVE_GATE_END : t;
	}
}

/* ---------------------------------------------------------------------------
 * One trial: build a key set, validate it, seek across it
 * ------------------------------------------------------------------------- */

static void
one_trial(uint64_t *keys, uint32_t nmax, int nseeks)
{
	uint32_t	n = rnd_below(nmax + 1);
	GenKind		kind = (GenKind) rnd_below(GEN_NKINDS);
	WeaveGateCursor c;
	WeaveGateError rc;
	uint32_t	bad = 12345;
	uint32_t	floor = 0;
	uint32_t	prev = 0;
	int			have_prev = 0;
	int			i;

	if (rnd_below(4) == 0)
		n = rnd_below(3);		/* empty and singleton often */
	n = gen_keys(keys, n, kind);

	/* G6: the generator's output is accepted. */
	rc = weave_gate_check_keys(keys, n, &bad);
	CHECK(6, rc == WEAVE_GATE_OK, "kind=%d n=%u refused with %d at %u",
		  (int) kind, n, (int) rc, bad);
	if (rc != WEAVE_GATE_OK)
		return;

	/* G6: a single injected defect is refused at its index. */
	if (n >= 2)
	{
		uint32_t	at = 1 + rnd_below(n - 1);
		uint64_t	save = keys[at];
		int			which = (int) rnd_below(3);

		if (which == 0)
			keys[at] = keys[at - 1];	/* duplicate */
		else if (which == 1)
			keys[at] = keys[at - 1] - (keys[at - 1] > 0 ? 1 : 0);	/* descent (or dup at 0) */
		else
		{
			at = n - 1;
			save = keys[at];
			keys[at] = (uint64_t) WEAVE_GATE_END + rnd_below(2);	/* too wide */
		}
		bad = 12345;
		rc = weave_gate_check_keys(keys, n, &bad);
		if (which == 2)
			CHECK(6, rc == WEAVE_GATE_TOO_WIDE && bad == at,
				  "wide key at %u: rc=%d bad=%u", at, (int) rc, bad);
		else if (keys[at] == keys[at - 1])
			CHECK(6, rc == WEAVE_GATE_DUPLICATE && bad == at,
				  "dup at %u: rc=%d bad=%u", at, (int) rc, bad);
		else
			CHECK(6, rc == WEAVE_GATE_UNSORTED && bad == at,
				  "descent at %u: rc=%d bad=%u", at, (int) rc, bad);
		keys[at] = save;
	}

	weave_gate_init(&c, keys, n);

	/* Before any seek: on the first key iff there is one. */
	CHECK(4, weave_gate_block_may_match(&c) == (n > 0),
		  "n=%u: fresh cursor block_may_match=%d", n, weave_gate_block_may_match(&c));

	for (i = 0; i < nseeks; i++)
	{
		uint32_t	target = gen_target(keys, n, floor);
		uint32_t	got = 0xDEADBEEF;
		uint32_t	want = oracle_seek(keys, n, target);
		float		bm,
					sc;

		rc = weave_gate_seek(&c, target, &got);
		CHECK(1, rc == WEAVE_GATE_OK, "kind=%d n=%u t=%u: forward seek refused (%d)",
			  (int) kind, n, target, (int) rc);
		if (rc != WEAVE_GATE_OK)
			break;

		/* G1 */
		CHECK(1, got >= target, "kind=%d n=%u t=%u: returned %u < target",
			  (int) kind, n, target, got);
		if (have_prev)
			CHECK(1, got >= prev, "kind=%d n=%u t=%u: returned %u < previous %u",
				  (int) kind, n, target, got, prev);

		/* G2 */
		CHECK(2, got == want, "kind=%d n=%u t=%u: returned %u, oracle %u",
			  (int) kind, n, target, got, want);

		/* G3: block is [cur, blkend] == [got, got]. */
		bm = block_max_of(&c, got);
		sc = score_of(&c, got);
		CHECK(3, bm >= sc, "kind=%d n=%u t=%u at %u: block_max %f < score %f",
			  (int) kind, n, target, got, (double) bm, (double) sc);

		/* G4 */
		if (want == WEAVE_GATE_END)
		{
			CHECK(4, bm == -INFINITY, "n=%u t=%u: exhausted but block_max=%f",
				  n, target, (double) bm);
			CHECK(4, sc == -INFINITY, "n=%u t=%u: exhausted but score=%f",
				  n, target, (double) sc);
		}
		else
		{
			CHECK(4, bm == INFINITY, "n=%u t=%u at %u: on key but block_max=%f",
				  n, target, got, (double) bm);
			CHECK(4, sc == 0.0f, "n=%u t=%u at %u: on key but score=%f",
				  n, target, got, (double) sc);
			/* Off-key probes around the key are non-matches. */
			if (got > 0 && got - 1 >= target)
				CHECK(4, score_of(&c, got - 1) == -INFINITY,
					  "n=%u t=%u: score at %u (below key %u) is not -INF",
					  n, target, got - 1, got);
			if (got + 1 < WEAVE_GATE_END)
				CHECK(4, score_of(&c, got + 1) == -INFINITY,
					  "n=%u t=%u: score at %u (above key %u) is not -INF",
					  n, target, got + 1, got);
			if (target < got)
				CHECK(4, score_of(&c, target) == -INFINITY,
					  "n=%u t=%u: score at the skipped target is not -INF",
					  n, target);
		}

		/* G5: a backward target is refused and the cursor is untouched. */
		if (have_prev && prev > 0 && rnd_below(2) == 0)
		{
			WeaveGateCursor before = c;
			uint32_t	back = rnd_below(got == WEAVE_GATE_END ? prev : got);
			uint32_t	dummy = 0xDEADBEEF;

			if (back < got)
			{
				rc = weave_gate_seek(&c, back, &dummy);
				CHECK(5, rc == WEAVE_GATE_BACKWARD,
					  "n=%u: backward seek %u after %u accepted (%d)",
					  n, back, got, (int) rc);
				CHECK(5, memcmp(&before, &c, sizeof(c)) == 0,
					  "n=%u: refused seek changed the cursor", n);
			}
		}
		/* G5: the NOT IDEMPOTENT note -- seek(target) twice is illegal when
		 * the first returned more than target. */
		if (got != target && rnd_below(2) == 0)
		{
			uint32_t	dummy = 0xDEADBEEF;

			rc = weave_gate_seek(&c, target, &dummy);
			CHECK(5, rc == WEAVE_GATE_BACKWARD,
				  "n=%u: seek(%u) twice accepted after it returned %u",
				  n, target, got);
		}
		/* G5: seek(returned) is legal and answers the same. */
		if (got != WEAVE_GATE_END && rnd_below(2) == 0)
		{
			uint32_t	again = 0xDEADBEEF;

			rc = weave_gate_seek(&c, got, &again);
			CHECK(5, rc == WEAVE_GATE_OK && again == got,
				  "n=%u: seek(%u) at the current key: rc=%d got %u",
				  n, got, (int) rc, again);
		}

		prev = got;
		have_prev = 1;
		if (got == WEAVE_GATE_END)
		{
			uint32_t	dummy = 0xDEADBEEF;

			/* G5: past the end, everything below END is backward... */
			rc = weave_gate_seek(&c, target, &dummy);
			CHECK(5, target == WEAVE_GATE_END || rc == WEAVE_GATE_BACKWARD,
				  "n=%u: seek(%u) after END accepted", n, target);
			/* ...and END itself is not. */
			rc = weave_gate_seek(&c, WEAVE_GATE_END, &dummy);
			CHECK(5, rc == WEAVE_GATE_OK && dummy == WEAVE_GATE_END,
				  "n=%u: seek(END) after END: rc=%d got %u", n, (int) rc, dummy);
			CHECK(4, block_max_of(&c, dummy) == -INFINITY,
				  "n=%u: block_max after END is not -INF", n);
			break;
		}
		floor = got;
	}
}

/*
 * A large array with few seeks: the gallop's long-jump path, checked against
 * the oracle where the oracle is affordable.
 */
static void
big_trial(uint64_t *keys, uint32_t n)
{
	WeaveGateCursor c;
	uint32_t	floor = 0;
	uint32_t	prev = 0;
	int			i;

	n = gen_keys(keys, n, (GenKind) (rnd_below(2) ? GEN_SPARSE : GEN_MIXED));
	if (weave_gate_check_keys(keys, n, NULL) != WEAVE_GATE_OK)
	{
		CHECK(6, 0, "big n=%u refused", n);
		return;
	}
	weave_gate_init(&c, keys, n);
	for (i = 0; i < 24; i++)
	{
		uint32_t	target = gen_target(keys, n, floor);
		uint32_t	got = 0;
		uint32_t	want = oracle_seek(keys, n, target);
		WeaveGateError rc = weave_gate_seek(&c, target, &got);

		CHECK(1, rc == WEAVE_GATE_OK && got >= target && (i == 0 || got >= prev),
			  "big n=%u t=%u: rc=%d got=%u prev=%u", n, target, (int) rc, got, prev);
		CHECK(2, got == want, "big n=%u t=%u: got %u oracle %u", n, target, got, want);
		CHECK(3, block_max_of(&c, got) >= score_of(&c, got),
			  "big n=%u at %u: bound below score", n, got);
		if (got == WEAVE_GATE_END)
			break;
		floor = got;
		prev = got;
	}
}

int
main(void)
{
	enum
	{
		NMAX = 4096,
		NBIG = 1u << 20
	};
	uint64_t   *keys = malloc(sizeof(uint64_t) * NBIG);
	long		trials = 0;
	int			i;

	if (keys == NULL)
	{
		printf("out of memory\n");
		return 2;
	}

	printf("== Z7 gate shuttle: (C1) seek == oracle, (C2)/(C5) infinities, backward refused ==\n");

	/* Mostly small and medium sets with long seek sequences. */
	for (i = 0; i < 30000; i++)
	{
		uint32_t	r = rnd_below(20);
		uint32_t	nmax = r < 5 ? 48 : (r < 17 ? 512 : NMAX);

		one_trial(keys, nmax, 64);
		trials++;
	}
	/* Some large sets with few seeks. */
	for (i = 0; i < 40; i++)
	{
		big_trial(keys, NBIG - 8);
		trials++;
	}
	free(keys);

	printf("trials: %ld\n", trials);
	printf("G1 monotone >= target    : %8ld checks\n", prop_checks[1]);
	printf("G2 == linear oracle      : %8ld checks\n", prop_checks[2]);
	printf("G3 block_max >= score    : %8ld checks\n", prop_checks[3]);
	printf("G4 +/-INF placement      : %8ld checks\n", prop_checks[4]);
	printf("G5 backward refused      : %8ld checks\n", prop_checks[5]);
	printf("G6 key-set validation    : %8ld checks\n", prop_checks[6]);
	if (failures > 0)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
