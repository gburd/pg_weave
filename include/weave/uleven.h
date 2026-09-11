/*-------------------------------------------------------------------------
 *
 * uleven.h -- Levenshtein neighbourhood expansion over a VOCABULARY, and the
 *		imported pg_tre trigram-neighbourhood generator.
 *
 * Two unrelated things live behind this one name, and the reason they are not
 * the same thing is the first thing to read.
 *
 * 1. weave_uleven_* (this header, static inline, task Z5) is pg_weave's own:
 *	  given a query term, an edit budget k and *any* way to enumerate the
 *	  vocabulary in ascending byte order, it produces EXACTLY the vocabulary
 *	  terms within edit distance k, and reports a dead prefix so the caller's
 *	  iterator can skip a whole subtree / a whole contiguous run of terms.
 *
 * 2. pg_weave_uleven_expand[_cp] (src/query/uleven.c, imported from pg_tre
 *	  e03d6a8, MIT, same author) enumerates the byte-alphabet neighbourhood of a
 *	  fixed THREE-BYTE TRIGRAM.  Its own file comment calls it a "Mihov-Schulz
 *	  universal Levenshtein automaton"; it is not one, there is no automaton in
 *	  it, and correcting that claim is deliberate -- see src/query/uleven.c.  It
 *	  stays because src/query/tiling.c widens a regex trigram spine with it,
 *	  which is a different job: a regex has no single query term to build an
 *	  automaton for.
 *
 * See doc/specs/IMPORT_pg_tre.md for the import mapping and
 * doc/specs/FUZZY_CHANNEL.md sect. 3.6 for the design of (1).
 *
 * ---------------------------------------------------------------------------
 * WHY A CORE WITH AN ITERATOR RATHER THAN A FUNCTION OVER A DICTIONARY PAGE
 * ---------------------------------------------------------------------------
 *
 * The vocabulary lives in three shapes already: the sorted dictionary page
 * chain, the SuRF trie over it (weave/surftrie.h, on disk as a WEAVE_PK_SURF
 * chain since Z3), and a plain sorted array in a test.  The automaton does not
 * care which: all it needs is "give me the next term in ascending unsigned-byte
 * order" plus "skip everything under this prefix".  Taking those two as a
 * WeaveUlevVocab means the same code is exercised by a standalone property test
 * today and runs against the trie unchanged later, and it is why this file has
 * no PostgreSQL dependency at all (include/weave/for.h is the house exemplar
 * and doc/TESTING.md is the reason: a core that cannot be linked into a plain
 * `gcc` invocation is a core whose property test people skip).
 *
 * PRUNING IS AN OPTIMIZATION AND MUST STAY ONE.  weave_uleven_match() returns
 * WEAVE_ULEVEN_DEAD with the byte length of a prefix that no within-k string can
 * extend, and weave_uleven_expand_vocab() feeds that to WeaveUlevVocab.skip.
 * `skip` may be NULL, and then the answer is *identical*, only more terms are
 * visited.  test/hegel/test_uleven.c asserts that equality on every generated
 * case, because a skip that is one byte too greedy is a false negative and no
 * fixed-expected-output regression test can catch it (AGENTS.md hard rule 1).
 *
 * ---------------------------------------------------------------------------
 * THE ERROR DIRECTION: THIS ONE IS EXACT, NOT ONE-SIDED
 * ---------------------------------------------------------------------------
 *
 * Unlike the SuRF trie (weave/surftrie.h), which answers false positives and
 * never false negatives, this enumeration is EXACT with respect to the terms
 * the iterator yields:
 *
 *		weave_uleven_expand_vocab() emits term t  <=>  dist(query, t) <= k
 *
 * Both inclusions are asserted by the property test, against a naive
 * full-matrix dynamic-programming Levenshtein written independently in the test
 * file.  Exactness is what removes the heap recheck for `term~k` -- an
 * over-generating funnel costs a recheck per candidate document, and a
 * *under*-generating one silently drops rows.
 *
 * The exactness claim is conditional on the iterator being complete: feed it a
 * truncating SuRF enumeration and the composition inherits SuRF's false
 * positives (never its absence of false negatives), which is the safe
 * direction.  Whoever wires the trie owes that recheck.
 *
 * ---------------------------------------------------------------------------
 * THE METRIC IS LEVENSHTEIN.  THE EDIT UNIT IS THE CHARACTER.
 * ---------------------------------------------------------------------------
 *
 * Insertion, deletion and substitution, each cost 1.  NOT
 * Damerau-Levenshtein: an adjacent transposition costs 2, so "hte" is at
 * distance 2 from "the" and a `~1` query does not match it.  The property test
 * asserts that, in that direction, rather than leaving it to be discovered.
 *
 * Why not Damerau, given that a transposition is the commonest human typo: the
 * SQL surface has to agree with something, and the thing it has to agree with is
 * `levenshtein()` from contrib/fuzzystrmatch, because doc/PHASES.md Z9 makes a
 * randomized differential test against a seq-scan `levenshtein()` the gate for
 * the `<@>` distance operator.  A channel whose k differs from the reference
 * function's k by one on transposed input fails that gate for a reason that has
 * nothing to do with the index.  (Adding Damerau later is a new unit mode, not a
 * change to this one: it changes which rows a query returns.)
 *
 * THE EDIT UNIT IS THE CHARACTER, NOT THE BYTE, and that is the part that
 * silently ships wrong.  `levenshtein()` counts characters under a multi-byte
 * server encoding.  Substituting one two-byte character for another is ONE
 * character edit but can be TWO byte edits, so a byte-unit automaton run at
 * k=1 does not return a row that `levenshtein(...) <= 1` selects: a false
 * negative, visible only to non-ASCII users, invisible to every
 * fixed-expected-output test.  Hence WEAVE_ULEVEN_UTF8.
 *
 * WEAVE_ULEVEN_BYTE exists and is not a fallback: on a single-byte server
 * encoding (LATIN1, SQL_ASCII, EUC_*, ...) a byte IS a character, and decoding
 * as UTF-8 would be the wrong answer there.  The caller selects the mode from
 * pg_database_encoding_max_length() == 1, and the mode is a property of the
 * query, not of the format.
 *
 * MALFORMED BYTES DO NOT GET AN OPINION.  A byte sequence that is not valid
 * UTF-8 (a stray continuation byte, an overlong form, an encoded surrogate, a
 * five-byte lead) decodes one byte at a time to the pseudo-unit 0xDC00 + byte,
 * which valid UTF-8 can never produce because encoded surrogates are rejected.
 * The decode therefore stays INJECTIVE over all byte strings, which is what
 * makes dist(t, t) == 0 and dist(a, b) > 0 for a != b hold unconditionally --
 * the alternative (fold every bad byte to one replacement character) makes two
 * different terms compare equal, and "two different terms are at distance 0" is
 * a wrong answer that arrives as a *missing* row once a caller de-duplicates.
 * Nothing here ever ereports and nothing reads past the declared length.
 *
 * ---------------------------------------------------------------------------
 * ALLOCATION -- READ THIS BEFORE ADDING ONE
 * ---------------------------------------------------------------------------
 *
 * A neighbourhood over a vocabulary is a vocabulary-scale quantity, which is the
 * exact class behind four real crashes in this extension's ancestor (`make
 * check-alloc`, AGENTS.md's lint table).  So this core allocates NOTHING and
 * never materializes the result set: hits are streamed to a callback, and a
 * caller that wants an array owns that array and owes it
 * WEAVE_ALLOC_MAYBE_HUGE.  A caller that wants a fanout cap (the shape of
 * pg_weave.max_extraction_fanout) returns nonzero from the callback and gets
 * WEAVE_ULEVEN_STOPPED.
 *
 * The automaton's own state is bounded by the QUERY, not by the corpus:
 * WEAVE_ULEVEN_MAX_UNITS + 1 units in WeaveUlevAut and two rows of that width
 * in weave_uleven_match()'s frame.  Candidate terms are streamed, so a term
 * longer than the query bound costs nothing extra and is not refused -- only an
 * over-long QUERY is refused, and refused rather than truncated, because a
 * truncated query accepts a different language and the difference lands in the
 * false-negative direction.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/uleven.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_ULEVEN_H
#define WEAVE_ULEVEN_H

#include <stddef.h>
#include <stdint.h>

/*
 * PostgreSQL's c.h defines uint8/uint32/int32; only supply the weave_ul_*
 * aliases from <stdint.h> when compiled outside the backend.  Mirrors
 * weave/surftrie.h, and it is what lets a standalone test include this header
 * with nothing but -I include.
 */
#ifndef POSTGRES_H
typedef uint8_t weave_ul_uint8;
typedef uint32_t weave_ul_uint32;
typedef int32_t weave_ul_int32;
#else
typedef uint8 weave_ul_uint8;
typedef uint32 weave_ul_uint32;
typedef int32 weave_ul_int32;
#endif

/*
 * Longest query, in edit units.  255 for three reasons that agree: it is
 * WEAVE_SURFTRIE_MAX_DEPTH, so a query longer than this cannot be matched
 * exactly by the trie either; it is WEAVE_LEV_MAXQ, the bound the existing
 * dictionary walk already advertises; and it keeps WeaveUlevAut and the two DP
 * rows small enough to be automatic variables, so no query path allocates.
 */
#define WEAVE_ULEVEN_MAX_UNITS	255

/*
 * Largest k.  Nothing in the algorithm needs a small k -- the row form has no
 * per-k table to generate, unlike a precomputed parametric-state automaton --
 * but the clamp value is k + 1 and a bound keeps every intermediate in `int`
 * with room to spare.  k >= the longer length accepts everything, which is
 * legal and tested, not an error.
 */
#define WEAVE_ULEVEN_MAX_K		255

/* The edit unit.  See the header comment: this is a property of the server
 * encoding, chosen by the caller, not a fallback. */
typedef enum WeaveUlevUnit
{
	WEAVE_ULEVEN_BYTE = 0,		/* one byte = one edit unit (single-byte encodings) */
	WEAVE_ULEVEN_UTF8 = 1		/* one UTF-8 character = one edit unit */
} WeaveUlevUnit;

typedef enum WeaveUlevError
{
	WEAVE_ULEVEN_OK = 0,
	WEAVE_ULEVEN_QUERY_TOO_LONG, /* over WEAVE_ULEVEN_MAX_UNITS units */
	WEAVE_ULEVEN_K_RANGE,		/* k negative or over WEAVE_ULEVEN_MAX_K */
	WEAVE_ULEVEN_UNIT_MODE,		/* not one of WeaveUlevUnit */
	WEAVE_ULEVEN_NO_ITER,		/* no vocabulary iterator supplied */
	WEAVE_ULEVEN_STOPPED		/* the hit callback asked to stop; not an error */
} WeaveUlevError;

static inline const char *
weave_uleven_errstr(WeaveUlevError e)
{
	switch (e)
	{
		case WEAVE_ULEVEN_OK:
			return "ok";
		case WEAVE_ULEVEN_QUERY_TOO_LONG:
			return "fuzzy query term is longer than the automaton bound";
		case WEAVE_ULEVEN_K_RANGE:
			return "fuzzy edit distance is negative or over the maximum";
		case WEAVE_ULEVEN_UNIT_MODE:
			return "unrecognized fuzzy edit-unit mode";
		case WEAVE_ULEVEN_NO_ITER:
			return "no vocabulary iterator supplied";
		case WEAVE_ULEVEN_STOPPED:
			return "enumeration stopped early by the caller";
	}
	return "unrecognized fuzzy expansion error";
}

/* weave_uleven_match() outcomes.  DEAD is not an error: it is the pruning
 * signal, and it is a strictly stronger statement than "no match". */
#define WEAVE_ULEVEN_MATCH	1	/* within k; *dist is the distance */
#define WEAVE_ULEVEN_ALIVE	0	/* not within k, but some extension might be */
#define WEAVE_ULEVEN_DEAD	(-1)	/* no extension of cand[0..*deadbytes) can match */

/*
 * The automaton for one (query, k, unit) triple.  Treat as opaque; build it with
 * weave_uleven_init().  Sized by the query, so it is fine on a stack frame.
 */
typedef struct WeaveUlevAut
{
	weave_ul_int32 q[WEAVE_ULEVEN_MAX_UNITS];	/* query in edit units */
	int			m;				/* number of units in q */
	int			k;				/* edit budget */
	int			unit;			/* a WeaveUlevUnit */
} WeaveUlevAut;

/*
 * The vocabulary, abstracted.  `next` yields terms in ASCENDING UNSIGNED-BYTE
 * order (the dictionary already is, and a lexicographic trie DFS is); `term` is
 * valid until the following call.  Returns 0 at end of vocabulary.  `ord` is
 * whatever the caller wants echoed back on a hit -- the dictionary term ordinal
 * in the AM, the array index in the test.
 *
 * `skip` is OPTIONAL and may be NULL.  When present it must advance the cursor
 * past every remaining term that has `prefix[0..plen)` as a prefix, and nothing
 * else.  Correctness must not depend on it: with skip == NULL the emitted set is
 * the same one.  A sorted array bisects; the SuRF trie prunes the subtree.
 *
 * Terms are NOT required to be distinct or non-empty -- that is the caller's
 * business (the trie refuses both, the dictionary guarantees both).  A duplicate
 * is emitted twice, an empty term matches iff m <= k.
 */
typedef struct WeaveUlevVocab
{
	int			(*next) (void *arg, const char **term, weave_ul_uint32 *len,
						 weave_ul_uint32 *ord);
	void		(*skip) (void *arg, const char *prefix, weave_ul_uint32 plen);
	void	   *arg;
} WeaveUlevVocab;

/*
 * One hit.  `dist` is the exact edit distance (0..k), which Z9's `<@>` ordering
 * needs and which costs nothing to report -- it is the DP row's last cell.
 * Return 0 to continue, nonzero to stop the walk (-> WEAVE_ULEVEN_STOPPED).
 */
typedef int (*WeaveUlevHitCb) (void *arg, const char *term,
							   weave_ul_uint32 len, weave_ul_uint32 ord,
							   int dist);

/* ---------------------------------------------------------------------------
 * Edit units
 * ------------------------------------------------------------------------- */

/*
 * Decode the unit starting at *pos, advance *pos past it, and return it.
 * Requires *pos < len.  Never reads at or past `len`, whatever the bytes say --
 * a truncated multi-byte sequence at the end of the buffer decodes as escaped
 * bytes, it does not read the next term's first byte.
 *
 * The escape (0xDC00 + byte, one byte at a time) is the "maximal subpart"
 * behaviour: a bad lead byte consumes only itself, so the bytes after it get
 * their own honest decode instead of being swallowed.  See the header comment on
 * why injectivity here is a correctness property and not fastidiousness.
 *
 * *clean (may be NULL) IS A SOUNDNESS OUTPUT, NOT A DIAGNOSTIC.  It is 1 when
 * the byte offset this unit ends at is a SELF-DELIMITING boundary: every unit in
 * s[0..*pos) was determined by bytes strictly inside s[0..*pos), so ANY longer
 * string sharing those bytes decodes to the same unit prefix.  It is 0 when this
 * unit's decode consulted a byte at or past the boundary -- a truncated
 * sequence, an invalid continuation, an overlong form -- because then a longer
 * string sharing the same bytes can decode them DIFFERENTLY.
 *
 * Why that matters, with the case that motivated it.  weave_uleven_match()
 * reports a dead prefix as a BYTE count so the caller can skip a whole range,
 * and that transfer from "this unit prefix is dead" to "this byte prefix is
 * dead" is only valid at a self-delimiting boundary.  Take query "a\xC3\xA9"
 * ("ae'" as two characters), k = 0, and the vocabulary { "a\xC3",
 * "a\xC3\xA9" }.  The first term is truncated UTF-8, so it decodes as
 * [a, escape(C3)] and the automaton dies after two bytes.  Skipping every term
 * beginning "a\xC3" would then skip "a\xC3\xA9" -- an EXACT match, dropped
 * silently.  The two terms share a byte prefix but not a unit prefix, because
 * the escape decision was made by looking past the boundary.  So that boundary
 * is not clean, the dead prefix is not reported, and the walk merely visits one
 * more term.  ASCII data and byte mode are clean at every boundary and lose
 * nothing.
 */
static inline weave_ul_int32
weave_uleven_unit_ex(const unsigned char *s, size_t len, size_t *pos, int unit,
					 int *clean)
{
	size_t		p = *pos;
	unsigned char b = s[p];
	weave_ul_int32 cp;
	int			need;
	int			i;

	if (clean)
		*clean = 1;

	if (unit == WEAVE_ULEVEN_BYTE || b < 0x80)
	{
		*pos = p + 1;
		return (weave_ul_int32) b;
	}

	if (b >= 0xC2 && b <= 0xDF)
	{
		need = 1;
		cp = b & 0x1F;
	}
	else if (b >= 0xE0 && b <= 0xEF)
	{
		need = 2;
		cp = b & 0x0F;
	}
	else if (b >= 0xF0 && b <= 0xF4)
	{
		need = 3;
		cp = b & 0x07;
	}
	else
	{
		/*
		 * 0x80..0xC1 (a continuation byte, or an overlong two-byte lead) and
		 * 0xF5..0xFF can never begin a valid sequence, and that verdict is
		 * reached from this byte alone -- which is inside the prefix.  So the
		 * boundary after it IS clean.
		 */
		*pos = p + 1;
		return (weave_ul_int32) (0xDC00 + b);
	}

	/*
	 * The sequence needs bytes p+1 .. p+need.  If the buffer ends first the
	 * lead byte is escaped on its own: reading s[len] to find out what the next
	 * term starts with would be both wrong and out of bounds.  The boundary is
	 * NOT clean -- a longer string supplies those bytes and may well decode a
	 * character here.
	 */
	if (p + (size_t) need >= len)
	{
		if (clean)
			*clean = 0;
		*pos = p + 1;
		return (weave_ul_int32) (0xDC00 + b);
	}

	for (i = 1; i <= need; i++)
	{
		unsigned char c = s[p + i];

		if ((c & 0xC0) != 0x80)
		{
			/* decided by s[p + i], which is at or past the boundary p + 1 */
			if (clean)
				*clean = 0;
			*pos = p + 1;
			return (weave_ul_int32) (0xDC00 + b);
		}
		cp = (cp << 6) | (c & 0x3F);
	}

	/*
	 * Reject the non-canonical encodings rather than accepting them: an
	 * overlong form or an encoded surrogate would decode to a codepoint that
	 * some other byte sequence also decodes to, and that breaks injectivity.
	 * Same boundary caveat as above: the rejection read bytes past p + 1.
	 */
	if ((need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
		(need == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
	{
		if (clean)
			*clean = 0;
		*pos = p + 1;
		return (weave_ul_int32) (0xDC00 + b);
	}

	*pos = p + (size_t) need + 1;
	return cp;
}

/* The common form, for callers that only want the unit. */
static inline weave_ul_int32
weave_uleven_unit(const unsigned char *s, size_t len, size_t *pos, int unit)
{
	return weave_uleven_unit_ex(s, len, pos, unit, NULL);
}
/* Number of edit units in s[0..len), without decoding into an array. */
static inline size_t
weave_uleven_nunits(const void *s, size_t len, int unit)
{
	const unsigned char *p = (const unsigned char *) s;
	size_t		pos = 0;
	size_t		n = 0;

	while (pos < len)
	{
		(void) weave_uleven_unit(p, len, &pos, unit);
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------------------
 * The automaton
 * ------------------------------------------------------------------------- */

/*
 * Build the automaton for (query, k, unit).  Refuses an over-long query rather
 * than truncating it (header comment: truncation is a false negative).
 */
static inline WeaveUlevError
weave_uleven_init(WeaveUlevAut *aut, const void *query, size_t qlen, int k,
				  int unit)
{
	const unsigned char *s = (const unsigned char *) query;
	size_t		pos = 0;
	int			m = 0;

	if (unit != WEAVE_ULEVEN_BYTE && unit != WEAVE_ULEVEN_UTF8)
		return WEAVE_ULEVEN_UNIT_MODE;
	if (k < 0 || k > WEAVE_ULEVEN_MAX_K)
		return WEAVE_ULEVEN_K_RANGE;

	while (pos < qlen)
	{
		if (m >= WEAVE_ULEVEN_MAX_UNITS)
			return WEAVE_ULEVEN_QUERY_TOO_LONG;
		aut->q[m++] = weave_uleven_unit(s, qlen, &pos, unit);
	}

	aut->m = m;
	aut->k = k;
	aut->unit = unit;
	return WEAVE_ULEVEN_OK;
}

/*
 * Match one candidate term.  Returns WEAVE_ULEVEN_MATCH (and sets *dist, if
 * given, to the exact distance), WEAVE_ULEVEN_ALIVE, or WEAVE_ULEVEN_DEAD (and
 * sets *deadbytes, if given, to the BYTE length of the shortest prefix of cand
 * that no within-k string extends).  `dist` and `deadbytes` may be NULL.
 *
 * THE ALGORITHM, and why it is this one.  State is the bounded DP row
 *
 *		row[j] = dist(query[0..j), consumed prefix of cand)  clamped to k+1
 *
 * and consuming a unit c advances it by the textbook recurrence.  Two facts make
 * it an automaton rather than a distance function:
 *
 *	 - min(row) never decreases as units are consumed, because every cell of the
 *	   next row is >= min of the previous one (next[0] = prev[0]+1, and
 *	   inductively next[j] >= min(prev[j-1], prev[j], next[j-1]) ).  So once
 *	   min(row) > k, NO extension can match: that is the DEAD verdict, and it is
 *	   what lets a sorted walk skip a contiguous run and a trie walk skip a
 *	   subtree.  Clamping at k+1 is safe for exactly the same reason.
 *	 - a candidate longer than m + k units is therefore dead by construction
 *	   (row[j] >= t - j >= t - m after t units), so streaming a long term costs
 *	   only until it dies.
 *
 * DEAD IS REPORTED IN BYTES AND THAT COSTS A SIDE CONDITION.  The unit prefix is
 * what died; the caller skips by BYTE prefix.  The two agree only at a
 * self-delimiting boundary, so when the killing unit's boundary is not clean
 * (weave_uleven_unit_ex()'s `clean`, which only ever happens on malformed UTF-8)
 * this returns ALIVE instead: still "no match", just no skip claim.  Reporting
 * the byte prefix anyway drops rows -- the worked example is on
 * weave_uleven_unit_ex().
 *
 * This is the Levenshtein automaton for (query, k) realized as a bounded row,
 * NOT Schulz-Mihov's precomputed parametric-state table.  The accepted language
 * is identical -- that is the theorem the construction rests on -- and the row
 * form is chosen because it needs no per-k table generation, so k is a runtime
 * value with no k-specific code to get wrong, and because the table's win is a
 * constant factor on a path whose cost this project has not yet measured
 * (AGENTS.md rule 9: do not build on an unmeasured performance premise).  If a
 * measurement later says the table is worth it, this file's property test is
 * exactly the harness that keeps the replacement honest.
 */
static inline int
weave_uleven_match(const WeaveUlevAut *aut, const void *cand, size_t candlen,
				   int *dist, size_t *deadbytes)
{
	const unsigned char *s = (const unsigned char *) cand;
	int			rowa[WEAVE_ULEVEN_MAX_UNITS + 1];
	int			rowb[WEAVE_ULEVEN_MAX_UNITS + 1];
	int		   *cur = rowa;
	int		   *nxt = rowb;
	const int	inf = aut->k + 1;
	size_t		pos = 0;
	int			clean = 1;
	int			j;

	for (j = 0; j <= aut->m; j++)
		cur[j] = (j < inf) ? j : inf;

	while (pos < candlen)
	{
		weave_ul_int32 c = weave_uleven_unit_ex(s, candlen, &pos, aut->unit,
											   &clean);
		int			rowmin;
		int		   *t;

		nxt[0] = (cur[0] < inf) ? cur[0] + 1 : inf;
		rowmin = nxt[0];
		for (j = 1; j <= aut->m; j++)
		{
			int			v = cur[j - 1] + (aut->q[j - 1] == c ? 0 : 1);

			if (nxt[j - 1] + 1 < v)
				v = nxt[j - 1] + 1;
			if (cur[j] + 1 < v)
				v = cur[j] + 1;
			if (v > inf)
				v = inf;
			nxt[j] = v;
			if (v < rowmin)
				rowmin = v;
		}

		t = cur;
		cur = nxt;
		nxt = t;

		if (rowmin > aut->k)
		{
			if (!clean)
				return WEAVE_ULEVEN_ALIVE;	/* dead, but not skippable by byte */
			if (deadbytes)
				*deadbytes = pos;
			return WEAVE_ULEVEN_DEAD;
		}
	}

	if (cur[aut->m] <= aut->k)
	{
		if (dist)
			*dist = cur[aut->m];
		return WEAVE_ULEVEN_MATCH;
	}
	return WEAVE_ULEVEN_ALIVE;
}

/*
 * THE Z5 CORE OPERATION: emit every vocabulary term within edit distance k of
 * the query, in the iterator's order, exactly once per term the iterator yields.
 *
 * *nhits and *nvisited (both optional) receive the number of emitted terms and
 * the number of terms pulled from the iterator.  `nvisited` is not a performance
 * counter dressed up as an output: the property test uses it to prove the skip
 * hook is actually exercised, because a `skip` that is silently never called
 * would leave a pruning bug undetectable while every answer stayed right.
 */
static inline WeaveUlevError
weave_uleven_expand_vocab(const WeaveUlevAut *aut, const WeaveUlevVocab *voc,
						  WeaveUlevHitCb cb, void *cbarg,
						  weave_ul_uint32 *nhits, weave_ul_uint32 *nvisited)
{
	weave_ul_uint32 hits = 0;
	weave_ul_uint32 visited = 0;
	WeaveUlevError rc = WEAVE_ULEVEN_OK;

	if (voc == NULL || voc->next == NULL)
		return WEAVE_ULEVEN_NO_ITER;

	for (;;)
	{
		const char *term = NULL;
		weave_ul_uint32 len = 0;
		weave_ul_uint32 ord = 0;
		size_t		dead = 0;
		int			dist = 0;
		int			r;

		if (!voc->next(voc->arg, &term, &len, &ord))
			break;
		visited++;

		r = weave_uleven_match(aut, term, (size_t) len, &dist, &dead);
		if (r == WEAVE_ULEVEN_MATCH)
		{
			hits++;
			if (cb != NULL && cb(cbarg, term, len, ord, dist) != 0)
			{
				rc = WEAVE_ULEVEN_STOPPED;
				break;
			}
		}
		else if (r == WEAVE_ULEVEN_DEAD && voc->skip != NULL)
		{
			/*
			 * cand[0..dead) is dead, so every remaining term with that prefix
			 * is too -- including the ones that are extensions of the whole
			 * term.  Terms equal to a strict prefix of the current term sorted
			 * BEFORE it and have already been visited, so skipping the prefix's
			 * whole range cannot skip an unexamined match.
			 */
			voc->skip(voc->arg, term, (weave_ul_uint32) dead);
		}
	}

	if (nhits)
		*nhits = hits;
	if (nvisited)
		*nvisited = visited;
	return rc;
}

/* ---------------------------------------------------------------------------
 * The imported pg_tre trigram-neighbourhood generator (src/query/uleven.c)
 *
 * Kept verbatim in behaviour and API because src/query/tiling.c widens a regex
 * trigram spine with it.  It is NOT the automaton above and it does not answer
 * `term~k`; read the file comment in src/query/uleven.c for what it actually
 * computes and where it is imprecise.
 * ------------------------------------------------------------------------- */

/*
 * Expand a trigram to include all trigrams within edit distance k.
 * Writes up to max_out distinct trigrams to the `out` array.
 * Returns the number of trigrams written, or -1 on overflow.
 *
 * - k=0: returns 1 (the original trigram only)
 * - k=1: returns ~hundreds (substitutions, insertions, deletions)
 * - k=2: returns ~thousands (nested expansion, deduped)
 * - k>2: returns -1 (not supported; fanout explosion)
 */
extern int pg_weave_uleven_expand(const weave_ul_uint8 tri[3], int k,
								  weave_ul_uint8 (*out)[3], int max_out);

/*
 * Codepoint-alphabet (UTF-8 aware) expansion.  For an all-ASCII trigram
 * this is identical to pg_weave_uleven_expand; for a trigram containing any
 * codepoint > 0x7F it returns only the exact trigram (bounded fanout,
 * soundness preserved by the authoritative heap recheck).  This is the
 * variant the k>0 tiling spine uses so that expanded trigrams hash the
 * same way the index built them (pg_weave_hash_trigram_cp).
 */
extern int pg_weave_uleven_expand_cp(const weave_ul_int32 tri[3], int k,
									 weave_ul_int32 (*out)[3], int max_out);

#endif							/* WEAVE_ULEVEN_H */
