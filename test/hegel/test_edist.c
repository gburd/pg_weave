/*-------------------------------------------------------------------------
 *
 * test_edist.c
 *		Contracts (C1) and (C2) for the `<@>` edit-distance shuttle's bound and
 *		cursor core (include/weave/edist.h), task Z9.
 *
 * THIS IS THE TEST AGENTS.md HARD RULE 1 REQUIRES, and doc/specs/FUZZY_CHANNEL.md
 * sect. 8's Z9 row names it.  Unlike the gate channel, where +/-INF is exactly
 * tight, this channel has a REAL numeric bound, so the failure hard rule 1
 * describes is available in its purest form: a lower bound on edit distance that
 * is one edit too LARGE makes block_max() one too SMALL, the scan skips a
 * dictionary page that contained the nearest term, and the query returns
 * plausible rows that are simply not the closest ones.  No fixed-expected-output
 * test can see that.  A randomized comparison against an independent reference
 * Levenshtein can.
 *
 * THE REFERENCE IS IMPLEMENTED HERE, deliberately, and not shared with the
 * implementation: the shuttle's score() calls core's varstr_levenshtein(), which
 * this test cannot link, and a shared helper would make the oracle agree with the
 * thing under test by construction.  The full O(mn) dynamic program over decoded
 * CHARACTERS is 30 lines and is the definition every party is supposed to be
 * implementing (contrib/fuzzystrmatch's levenshtein(), unit costs).
 *
 * Properties, over random patterns and random term sets:
 *
 *	G1	(C1) every seek returns a position >= its target and >= the previous
 *		return; the sequence of returns over a whole vocabulary is the identity
 *		on term ordinals, and the end sentinel appears exactly once, at the end
 *	G2	(C2) block_max() >= score() for EVERY position in [cur, blkend] -- i.e.
 *		ed_lower(page) <= ed(pattern, term) for every term on the page, checked
 *		at every position, not sampled
 *	G3	the two deficits separately: each must be a lower bound on its own, so a
 *		regression in one is not masked by the other being loose
 *	G4	a backward seek is REFUSED and leaves the cursor unchanged; a block that
 *		does not follow its predecessor is REFUSED
 *	G5	monotonicity of the bound in the pattern: a page's bound never claims
 *		more than the page's own minimum distance (tightness, reported not
 *		asserted -- see the histogram at the end)
 *
 * Generators, shaped at the cases the bound's two halves are most likely to get
 * wrong: ASCII and multi-byte (2, 3 and 4-byte UTF-8) terms, the EMPTY pattern,
 * single-character patterns, terms far longer and far shorter than the pattern,
 * repeated-character terms (whose distinct-trigram count is far below their
 * length, which is the case that makes the trigram deficit more than the length
 * deficit divided by three), and pages deliberately made homogeneous so that
 * min/max statistics are tight.
 *
 * Build and run:
 *		cc -O2 -Wall -Wextra -I include -o /tmp/te test/hegel/test_edist.c && /tmp/te
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_edist.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/edist.h"

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
static uint64_t rng_state = 0x243F6A8885A308D3ULL;

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
 * The reference: character-level Levenshtein, unit costs, full DP.
 *
 * UTF-8 decode first, because the unit is the character.  A malformed byte
 * decodes to itself offset into a range no valid sequence produces, which is the
 * same injective escape include/weave/uleven.h uses and for the same reason: two
 * distinct byte strings must not compare at distance 0.
 * ------------------------------------------------------------------------- */

#define MAXCH 96

static int
decode_utf8(const char *s, int len, uint32_t *out)
{
	int			i = 0;
	int			n = 0;

	while (i < len && n < MAXCH)
	{
		unsigned char c = (unsigned char) s[i];
		int			need;
		uint32_t	cp;

		if (c < 0x80)
		{
			out[n++] = c;
			i++;
			continue;
		}
		if ((c & 0xE0) == 0xC0)
		{
			need = 1;
			cp = c & 0x1Fu;
		}
		else if ((c & 0xF0) == 0xE0)
		{
			need = 2;
			cp = c & 0x0Fu;
		}
		else if ((c & 0xF8) == 0xF0)
		{
			need = 3;
			cp = c & 0x07u;
		}
		else
		{
			out[n++] = 0xDC00u + c;
			i++;
			continue;
		}
		if (i + need >= len)
		{
			out[n++] = 0xDC00u + c;
			i++;
			continue;
		}
		{
			int			k;
			int			ok = 1;

			for (k = 1; k <= need; k++)
			{
				unsigned char cc = (unsigned char) s[i + k];

				if ((cc & 0xC0) != 0x80)
				{
					ok = 0;
					break;
				}
				cp = (cp << 6) | (cc & 0x3Fu);
			}
			if (!ok)
			{
				out[n++] = 0xDC00u + c;
				i++;
				continue;
			}
			out[n++] = cp;
			i += need + 1;
		}
	}
	return n;
}

static int
ref_levenshtein(const char *a, int alen, const char *b, int blen)
{
	uint32_t	ca[MAXCH];
	uint32_t	cb[MAXCH];
	int			m = decode_utf8(a, alen, ca);
	int			n = decode_utf8(b, blen, cb);
	int			prev[MAXCH + 1];
	int			cur[MAXCH + 1];
	int			i;
	int			j;

	for (j = 0; j <= n; j++)
		prev[j] = j;
	for (i = 1; i <= m; i++)
	{
		cur[0] = i;
		for (j = 1; j <= n; j++)
		{
			int			sub = prev[j - 1] + (ca[i - 1] == cb[j - 1] ? 0 : 1);
			int			del = prev[j] + 1;
			int			ins = cur[j - 1] + 1;
			int			best = sub < del ? sub : del;

			cur[j] = best < ins ? best : ins;
		}
		memcpy(prev, cur, sizeof(int) * (size_t) (n + 1));
	}
	return prev[n];
}

/* Character count, the pg_mbstrlen_with_len() the backend uses. */
static int
ref_chars(const char *s, int len)
{
	uint32_t	buf[MAXCH];

	return decode_utf8(s, len, buf);
}

/* Distinct byte trigrams, the weave_trigrams() the backend uses: a term shorter
 * than 3 bytes yields ONE space-padded trigram, and duplicates are collapsed. */
static int
ref_trigrams(const char *s, int len)
{
	uint32_t	seen[512];
	int			n = 0;
	int			i;

	if (len <= 0)
		return 0;
	if (len < 3)
		return 1;
	for (i = 0; i + 3 <= len; i++)
	{
		uint32_t	h = ((uint32_t) (unsigned char) s[i] << 16) |
			((uint32_t) (unsigned char) s[i + 1] << 8) |
			(uint32_t) (unsigned char) s[i + 2];
		int			k;
		int			dup = 0;

		for (k = 0; k < n; k++)
			if (seen[k] == h)
			{
				dup = 1;
				break;
			}
		if (!dup && n < (int) (sizeof(seen) / sizeof(seen[0])))
			seen[n++] = h;
	}
	return n;
}

static int
ref_ascii(const char *s, int len)
{
	int			i;

	for (i = 0; i < len; i++)
		if ((unsigned char) s[i] >= 0x80)
			return 0;
	return 1;
}

/* ---------------------------------------------------------------------------
 * Generators
 * ------------------------------------------------------------------------- */

#define MAXTERMLEN 48

typedef enum
{
	ALPH_ASCII_TINY,			/* 'a'..'d': dense neighbourhoods, many ties */
	ALPH_ASCII,					/* 'a'..'z' */
	ALPH_ASCII_REPEAT,			/* one letter repeated: T(term) << len */
	ALPH_LATIN1,				/* 2-byte UTF-8 */
	ALPH_CJK,					/* 3-byte UTF-8 */
	ALPH_ASTRAL,				/* 4-byte UTF-8 */
	ALPH_MIXED,					/* all of the above in one string */
	ALPH_N
} Alphabet;

/* Append one character of the given alphabet.  Returns bytes written. */
static int
gen_char(Alphabet a, char *out)
{
	uint32_t	cp;

	switch (a)
	{
		case ALPH_ASCII_TINY:
			out[0] = (char) ('a' + rnd_below(4));
			return 1;
		case ALPH_ASCII:
			out[0] = (char) ('a' + rnd_below(26));
			return 1;
		case ALPH_ASCII_REPEAT:
			out[0] = 'z';
			return 1;
		case ALPH_LATIN1:
			cp = 0xC0 + rnd_below(48);
			out[0] = (char) (0xC0 | (cp >> 6));
			out[1] = (char) (0x80 | (cp & 0x3F));
			return 2;
		case ALPH_CJK:
			cp = 0x4E00 + rnd_below(256);
			out[0] = (char) (0xE0 | (cp >> 12));
			out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
			out[2] = (char) (0x80 | (cp & 0x3F));
			return 3;
		case ALPH_ASTRAL:
			cp = 0x1F600 + rnd_below(64);
			out[0] = (char) (0xF0 | (cp >> 18));
			out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
			out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
			out[3] = (char) (0x80 | (cp & 0x3F));
			return 4;
		default:
			return gen_char((Alphabet) rnd_below(ALPH_ASTRAL + 1), out);
	}
}

/* A random string of `nchars` characters over `a`, NUL-free.  Returns bytes. */
static int
gen_string(Alphabet a, int nchars, char *out)
{
	int			len = 0;
	int			i;

	for (i = 0; i < nchars; i++)
	{
		if (len + 4 > MAXTERMLEN)
			break;
		len += gen_char(a, out + len);
	}
	return len;
}

/* ---------------------------------------------------------------------------
 * One trial: a pattern, and a "dictionary" of pages
 * ------------------------------------------------------------------------- */

#define MAXPAGETERMS 24
#define MAXPAGES 12

typedef struct
{
	char		term[MAXPAGETERMS][MAXTERMLEN];
	int			len[MAXPAGETERMS];
	int			n;
	WeaveEdistStats st;
}			TestPage;

/* Tightness histogram: how far below the true minimum distance of a page the
 * bound sits.  Reported, not asserted -- (C2) is the assertion; this is the
 * number hard rule 9 says to look at, and the pruning rate it implies is
 * measured for real (on a realistic vocabulary) by bench/edist_bound.c. */
static long tight_exact = 0;
static long tight_close = 0;		/* within 1 */
static long tight_loose = 0;		/* 2 or more below */
static long tight_zero = 0;			/* bound 0: no claim at all */
static long npages_seen = 0;

static void
one_trial(void)
{
	TestPage	pages[MAXPAGES];
	char		pat[MAXTERMLEN];
	int			patlen;
	WeaveEdistPattern pv;
	WeaveEdistCursor c;
	int			npages = 1 + (int) rnd_below(MAXPAGES);
	/* A quarter of the trials model a single-byte server encoding, where a byte
	 * IS a character and the trigram divisor is 3 unconditionally.  There every
	 * string must be ASCII -- a multi-byte term cannot exist in such a database --
	 * so the alphabet is restricted rather than the generated string rejected. */
	int			maxcharlen = rnd_below(4) == 0 ? 1 : 4;
	Alphabet	alphamax = maxcharlen == 1 ? ALPH_ASCII_REPEAT : (Alphabet) (ALPH_N - 1);
	Alphabet	patalpha = (Alphabet) rnd_below((uint32_t) alphamax + 1);
	int			p;
	uint32_t	ord = 0;
	uint32_t	got = 0;
	uint32_t	prev = 0;
	int			seeked = 0;

	/* the pattern: sometimes empty, sometimes one character, usually a word */
	{
		uint32_t	r = rnd_below(16);
		int			nch = r == 0 ? 0 : (r == 1 ? 1 : 1 + (int) rnd_below(12));

		patlen = gen_string(patalpha, nch, pat);
	}

	weave_edist_pattern_init(&pv, (weave_ed_uint32) patlen,
							 (weave_ed_uint32) (maxcharlen == 1 ? patlen : ref_chars(pat, patlen)),
							 (weave_ed_uint32) ref_trigrams(pat, patlen),
							 ref_ascii(pat, patlen),
							 (weave_ed_uint32) maxcharlen);

	/* the pages */
	for (p = 0; p < npages; p++)
	{
		/* Half the pages are homogeneous in alphabet and length, which is what
		 * makes their statistics tight and the bound actually bite; the other
		 * half are mixed, which is the realistic case. */
		Alphabet	pa = (Alphabet) rnd_below((uint32_t) alphamax + 1);
		int			homo = rnd_below(2);
		int			fixedlen = 1 + (int) rnd_below(14);
		int			i;

		pages[p].n = 1 + (int) rnd_below(MAXPAGETERMS);
		weave_edist_stats_init(&pages[p].st);
		for (i = 0; i < pages[p].n; i++)
		{
			Alphabet	ta = homo ? pa : (Alphabet) rnd_below((uint32_t) alphamax + 1);
			int			nch = homo ? fixedlen : 1 + (int) rnd_below(16);
			int			blen = gen_string(ta, nch, pages[p].term[i]);
			int			ascii = ref_ascii(pages[p].term[i], blen);

			pages[p].len[i] = blen;
			weave_edist_stats_add(&pages[p].st, (weave_ed_uint32) blen,
								  (weave_ed_uint32) (maxcharlen == 1 ? blen :
													 ref_chars(pages[p].term[i], blen)),
								  (weave_ed_uint32) ref_trigrams(pages[p].term[i], blen),
								  maxcharlen == 1 ? 1 : ascii);
		}
	}

	weave_edist_init(&c, &pv);

	for (p = 0; p < npages; p++)
	{
		uint32_t	first = ord;
		uint32_t	last = ord + (uint32_t) pages[p].n - 1;
		WeaveEdistError rc;
		int			i;
		int			truemin = -1;
		uint32_t	bound;

		rc = weave_edist_enter_block(&c, first, last, &pages[p].st);
		CHECK(4, rc == WEAVE_EDIST_OK, "page %d [%u,%u] refused rc=%d",
			  p, first, last, (int) rc);
		if (rc != WEAVE_EDIST_OK)
			return;

		/* A block that does not follow its predecessor must be refused. */
		CHECK(4, weave_edist_enter_block(&c, first, last, &pages[p].st) ==
			  WEAVE_EDIST_BADBLOCK, "page %d re-entered", p);

		/*
		 * (C1) and (C2) at every position of the block.  The seek sequence uses
		 * non-decreasing targets: mostly +1, sometimes a jump inside the block,
		 * sometimes a repeat of the position last RETURNED (which is legal --
		 * only a target below it is not).
		 */
		i = 0;
		while (i < pages[p].n)
		{
			uint32_t	target = first + (uint32_t) i;
			uint32_t	out = 0;
			float		blockmax;
			float		score;
			int			at;
			int			d;

			if (rnd_below(8) == 0 && seeked && prev >= first)
				target = prev;	/* repeat the position last RETURNED: legal */
			rc = weave_edist_seek(&c, target, &out);
			CHECK(1, rc == WEAVE_EDIST_OK, "page %d pos %d: seek rc=%d",
				  p, i, (int) rc);
			if (rc != WEAVE_EDIST_OK)
				return;
			CHECK(1, out >= target && (!seeked || out >= prev),
				  "page %d pos %d: out=%u target=%u prev=%u",
				  p, i, out, target, prev);
			CHECK(1, out == target, "page %d pos %d: out=%u != target=%u",
				  p, i, out, target);
			got = out;
			prev = out;
			seeked = 1;
			/* cur may be BEHIND i when the target was a repeat, so every check
			 * below is stated at the position the cursor actually reports. */
			at = (int) (got - first);

			CHECK(1, weave_edist_blkend(&c) == last,
				  "page %d pos %d: blkend=%u != %u", p, i,
				  weave_edist_blkend(&c), last);

			/*
			 * THE (C2) CHECK.  block_max() speaks for the whole closed interval
			 * [cur, blkend], so every remaining term of the page is compared
			 * against the ONE bound the cursor reports here -- not just the term
			 * at cur.  A bound that is right at cur and wrong three terms later
			 * is exactly the silent row-dropping hard rule 1 describes.
			 */
			bound = weave_edist_block_lower(&c);
			blockmax = -(float) bound;
			{
				int			q;

				for (q = at; q < pages[p].n; q++)
				{
					d = ref_levenshtein(pat, patlen, pages[p].term[q],
										pages[p].len[q]);
					score = -(float) d;
					CHECK(2, blockmax >= score,
						  "page %d cur %d term %d: bound %u > true distance %d",
						  p, i, q, bound, d);
					if (truemin < 0 || d < truemin)
						truemin = d;
				}
			}

			/* score() at cur, and (C2) restated as the integer inequality the
			 * header's comment proves. */
			d = ref_levenshtein(pat, patlen, pages[p].term[at], pages[p].len[at]);
			CHECK(2, (int) bound <= d,
				  "page %d pos %d: bound %u > distance %d", p, at, bound, d);

			/* G3: each deficit alone must be a lower bound. */
			{
				int			pc = (int) pv.chars;
				int			tc = maxcharlen == 1 ? pages[p].len[at]
					: ref_chars(pages[p].term[at], pages[p].len[at]);
				int			pt = (int) pv.ntrg;
				int			tt = ref_trigrams(pages[p].term[at], pages[p].len[at]);
				int			div = (pv.all_ascii &&
								   ref_ascii(pages[p].term[at], pages[p].len[at]))
					? 3 : (int) pv.trgdiv;
				int			ld = pc > tc ? pc - tc : tc - pc;
				int			td = (pt > tt ? pt - tt : tt - pt);

				td = (td + div - 1) / div;
				CHECK(3, ld <= d, "length deficit %d > distance %d (%d vs %d chars)",
					  ld, d, pc, tc);
				CHECK(3, td <= d, "trigram deficit %d > distance %d (%d vs %d trg, div %d)",
					  td, d, pt, tt, div);
			}

			/* G4: a backward seek is refused and changes nothing. */
			if (out > 0)
			{
				uint32_t	junk = 12345;
				WeaveEdistCursor save = c;

				CHECK(4, weave_edist_seek(&c, out - 1, &junk) ==
					  WEAVE_EDIST_BACKWARD, "backward seek to %u accepted",
					  out - 1);
				CHECK(4, memcmp(&save, &c, sizeof(c)) == 0,
					  "refused backward seek mutated the cursor");
			}

			if (at + 1 > i)
				i = at + 1;
			else
				i++;			/* the repeat case: make progress anyway */
			if (rnd_below(6) == 0)
				i += (int) rnd_below(3);	/* a forward jump inside the block */
		}

		/* Tightness bookkeeping over the whole page. */
		npages_seen++;
		{
			uint32_t	b = weave_edist_lower(&pv, &pages[p].st);

			if (b == 0)
				tight_zero++;
			else if ((int) b == truemin)
				tight_exact++;
			else if ((int) b + 1 >= truemin)
				tight_close++;
			else
				tight_loose++;
		}

		ord = last + 1;
	}

	/* The end of the vocabulary: after finish(), every seek answers END, and the
	 * bound there makes no claim. */
	weave_edist_finish(&c);
	{
		uint32_t	out = 0;

		CHECK(1, weave_edist_seek(&c, ord, &out) == WEAVE_EDIST_OK &&
			  out == WEAVE_EDIST_END, "after finish, seek did not answer END");
		CHECK(1, weave_edist_seek(&c, WEAVE_EDIST_END, &out) == WEAVE_EDIST_OK &&
			  out == WEAVE_EDIST_END, "seek(END) did not answer END");
	}
}

/*
 * Directed cases, because a random generator covers the space it is shaped like
 * and the interesting failures of THIS bound are needles: the empty pattern
 * against a long term, a pattern that IS a term (distance 0, where any positive
 * bound is a violation), a one-character difference spanning a byte-count change
 * (the G30 shape), and a repeated-character term whose trigram count is 1 while
 * its length is 20.
 */
static void
directed_cases(void)
{
	struct
	{
		const char *pat;
		const char *term;
		int			maxcharlen;
	}			cases[] = {
		{"", "abcdefghij", 1},
		{"", "", 1},
		{"a", "", 1},
		{"abcd", "abcd", 1},
		{"abcd", "abxd", 1},
		{"naive", "na\xC3\xAF" "ve", 4},	/* one CHARACTER, two bytes */
		{"naive", "na\xE6\xBC\xA2" "ve", 4},	/* one character, three bytes */
		{"caf\xC3\xA9", "cafe", 4},
		{"zzzzzzzzzzzzzzzzzzzz", "zzzzzzzzzzzzzzzzzzzy", 1},
		{"zzzzzzzzzzzzzzzzzzzz", "z", 1},
		{"\xF0\x9F\x98\x80", "a", 4},
		{"\xF0\x9F\x98\x80\xF0\x9F\x98\x81", "\xF0\x9F\x98\x80", 4},
		{"abc", "\xC3\xA9\xC3\xA9\xC3\xA9", 4},
		{"the quick brown", "the quick brwon", 1},
	};
	size_t		n = sizeof(cases) / sizeof(cases[0]);
	size_t		i;

	for (i = 0; i < n; i++)
	{
		const char *pat = cases[i].pat;
		const char *term = cases[i].term;
		int			patlen = (int) strlen(pat);
		int			termlen = (int) strlen(term);
		int			mcl = cases[i].maxcharlen;
		WeaveEdistPattern pv;
		WeaveEdistStats st;
		int			d = ref_levenshtein(pat, patlen, term, termlen);
		uint32_t	b;

		weave_edist_pattern_init(&pv, (weave_ed_uint32) patlen,
								 (weave_ed_uint32) (mcl == 1 ? patlen : ref_chars(pat, patlen)),
								 (weave_ed_uint32) ref_trigrams(pat, patlen),
								 ref_ascii(pat, patlen),
								 (weave_ed_uint32) mcl);
		weave_edist_stats_init(&st);
		weave_edist_stats_add(&st, (weave_ed_uint32) termlen,
							  (weave_ed_uint32) (mcl == 1 ? termlen : ref_chars(term, termlen)),
							  (weave_ed_uint32) ref_trigrams(term, termlen),
							  mcl == 1 ? 1 : ref_ascii(term, termlen));
		b = weave_edist_lower(&pv, &st);
		CHECK(2, (int) b <= d, "directed %zu: bound %u > distance %d (%s vs %s)",
			  i, b, d, pat, term);
	}

	/* An empty block makes no claim: its bound must be 0, because [first, last]
	 * covers no term and any positive value would be a statement about nothing. */
	{
		WeaveEdistPattern pv;
		WeaveEdistStats st;

		weave_edist_pattern_init(&pv, 5, 5, 3, 1, 1);
		weave_edist_stats_init(&st);
		CHECK(2, weave_edist_lower(&pv, &st) == 0, "empty block claims a bound");
	}

	/*
	 * THE SPEC'S FORMULA, kept as a directed case because it is the thing this
	 * task found wrong (doc/specs/FUZZY_CHANNEL.md sect. 5 wrote the second
	 * trigram term over max_T_block instead of min_T_block).  The page below
	 * holds one term with 18 distinct trigrams and one that IS the pattern, so
	 * max_T_block - T_pattern is large while the true minimum distance is 0.
	 * Asserting the CORRECT bound is 0 here is what a regression to the spec's
	 * text would trip over.
	 */
	{
		const char *pat = "abcdefgh";
		const char *far = "mnopqrstuvwxyzabcdefghij";
		WeaveEdistPattern pv;
		WeaveEdistStats st;

		weave_edist_pattern_init(&pv, 8, 8,
								 (weave_ed_uint32) ref_trigrams(pat, 8), 1, 1);
		weave_edist_stats_init(&st);
		weave_edist_stats_add(&st, 8, 8, (weave_ed_uint32) ref_trigrams(pat, 8), 1);
		weave_edist_stats_add(&st, 24, 24,
							  (weave_ed_uint32) ref_trigrams(far, 24), 1);
		CHECK(2, weave_edist_lower(&pv, &st) == 0,
			  "a block containing the pattern itself claims a positive bound");
	}
}

int
main(void)
{
	int			i;
	long		trials = 0;

	printf("== Z9 edist shuttle: (C1) monotone seek, (C2) bound <= every true distance ==\n");

	directed_cases();
	for (i = 0; i < 40000; i++)
	{
		one_trial();
		trials++;
		if (checks > 1200000 && i > 20000)
			break;
	}

	printf("trials: %ld\n", trials);
	printf("G1 monotone seek          : %8ld checks\n", prop_checks[1]);
	printf("G2 bound <= true distance : %8ld checks\n", prop_checks[2]);
	printf("G3 each deficit alone     : %8ld checks\n", prop_checks[3]);
	printf("G4 refusals               : %8ld checks\n", prop_checks[4]);
	printf("tightness over %ld pages: exact %.1f%%, within 1 %.1f%%, "
		   "looser %.1f%%, no claim (bound 0) %.1f%%\n",
		   npages_seen,
		   100.0 * (double) tight_exact / (double) npages_seen,
		   100.0 * (double) tight_close / (double) npages_seen,
		   100.0 * (double) tight_loose / (double) npages_seen,
		   100.0 * (double) tight_zero / (double) npages_seen);
	if (failures > 0)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
