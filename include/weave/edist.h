/*-------------------------------------------------------------------------
 *
 * edist.h -- the `<@>` edit-distance shuttle: a SCORED channel with a real
 *			  numeric lower bound on Levenshtein distance
 *
 * Task Z9, specified in doc/specs/FUZZY_CHANNEL.md sect. 5.  Ordering by
 * `wdoc <@> pattern` returns documents by ascending edit distance, so under
 * include/weave/channel.h
 *
 *		score()     = -edit_distance(pattern, term at this position)
 *		block_max() = -ed_lower(block)
 *
 * and (C2) -- block_max() >= score() everywhere in [cur, blkend] -- is exactly
 * the statement that ed_lower() is a true LOWER bound on the distance from the
 * pattern to every term in the block.  A bound one edit too large silently drops
 * rows, and per AGENTS.md hard rule 1 no fixed-output regression test can see
 * that; test/hegel/test_edist.c is the property test that can.
 *
 * WHAT A POSITION IS, AND WHAT A BLOCK IS.  Unlike every other channel so far,
 * this one's warp position is a DICTIONARY TERM ORDINAL, not a docid.  The
 * distance `<@>` orders by is a property of a term; a document's distance is the
 * MINIMUM over its terms (see the operator's comment in the install SQL), and
 * that minimum is formed downstream, by the scan, from the postings of the terms
 * the shuttle hands it in ascending distance.  Putting the shuttle on the term
 * axis is what makes the bound possible at all: a docid-keyed block has no term
 * length and no trigram count to bound anything with.
 *
 * A BLOCK IS ONE DICTIONARY PAGE, and the statistics that bound it are computed
 * ONCE, when the shuttle first enters that page, off the page it is already
 * holding pinned and share-locked for the walk.  This is option (ii) of the two
 * the task considered, and the reasoning is worth recording because option (i) is
 * superficially more attractive:
 *
 *	 (i)  a new per-dictionary-page header field carrying min/max term length and
 *		  max trigram count.  That is an on-disk FORMAT CHANGE: a version bump, a
 *		  page-kind decision, a read path for pages written by the old writer, and
 *		  a TAP page-surgery upgrade test.  All of it in support of a bound whose
 *		  pruning rate was unmeasured at the time the choice had to be made --
 *		  which is precisely what AGENTS.md hard rule 9 warns about.
 *	 (ii) compute them on entry to the page.  No format change, nothing to
 *		  upgrade, and the numbers are exact rather than whatever the writer
 *		  happened to record.
 *
 * DOES (ii) SATISFY (C3)?  (C3) says "block_max() must not read a buffer ... it
 * reads state the shuttle already has from its current block header."  The
 * function below reads nothing but integers in WeaveEdistStats, so the letter of
 * it holds.  The intent of (C3) is "no I/O inside the bound", and that also
 * holds: the page the statistics are derived from is the page the walk is already
 * on, pinned for the duration; entering it is the walk's own I/O, not the
 * bound's, and it is paid once per page rather than once per position.  What (C3)
 * forbids is a bound that makes the SKIP cost as much as the work it skips -- a
 * bound that reads a *different* page, or re-reads this one per position.  This
 * does neither: after the one pass, skipping the page costs three comparisons.
 * The honest residual is that a pruned page is still READ (and its terms' lengths
 * and trigram counts counted) before it is pruned, so the saving is the exact
 * Levenshtein computation per term, not the page I/O.  A format change is what
 * would buy the I/O back, and bench/RESULTS_EDIST_BOUND.md is the measurement
 * that says whether that is worth a format change.
 *
 * WHAT score() IS FOR, AND WHAT A MUTATION RUN SAID ABOUT IT.  The ordering scan
 * takes an admitted term's distance through weave_shuttle_score() -- not through
 * the bounded twin it used to reject cheaply -- specifically so channel.h's sign
 * convention is on the query path.  It still is not observable from SQL: the value
 * reaches the executor as xs_orderbyvals, and an access method that does not set
 * xs_recheck_orderby has that array IGNORED (nodeIndexscan.c trusts the index's
 * own order and only re-evaluates the order-by expressions when the AM asks it
 * to).  So a mutation that inverts score()'s sign changes no answer on a
 * non-assert build; it is caught by the Assert next to the call, and it would be
 * caught by any consumer that compares two channels' scores -- i.e. by Phase F's
 * scorer, which does not exist yet (AGENTS.md hard rule 7).  Recorded rather than
 * papered over: an uncaught mutation is either a missing test or a redundant
 * check, and this one is a missing CALLER.
 *
 * TWO HALVES, ONE FILE, in the pattern of include/weave/uleven.h and
 * include/weave/gate.h: the bound and the cursor are header-only inline with no
 * backend dependency, so the property test compiles them with nothing but
 * `-I include` and runs millions of checks; the WeaveShuttle face, which needs
 * buffers, varstr_levenshtein() and ereport(), is declared under POSTGRES_H and
 * defined in src/query/edist.c.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/edist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_EDIST_H
#define WEAVE_EDIST_H

#include <stddef.h>
#include <stdint.h>

/*
 * PostgreSQL's c.h defines uint32; only supply the weave_ed_* aliases from
 * <stdint.h> when compiled outside the backend.  Mirrors weave/gate.h.
 */
#ifndef POSTGRES_H
typedef uint32_t weave_ed_uint32;
#else
typedef uint32 weave_ed_uint32;
#endif

/* The core's end-of-vocabulary sentinel.  MUST equal WEAVE_WARP_END in
 * channel.h; src/query/edist.c asserts it at compile time.  Named separately so
 * the standalone test does not need channel.h. */
#define WEAVE_EDIST_END			((weave_ed_uint32) 0xFFFFFFFF)

/*
 * The trigram divisor, and why it is not always 3.
 *
 * The deficit argument (FUZZY_CHANNEL.md sect. 5, and the same pigeonhole the
 * heap-side prefilter in src/query/doc.c uses) is: transform the pattern into the
 * term one edit at a time.  A single edit splices out a run of `a` bytes and
 * splices in a run of `b` bytes at one place; every trigram of the old string
 * whose three bytes do not overlap the removed run still occurs, as the same
 * three bytes, in the new one.  The trigrams that CAN be lost are those starting
 * at the a+2 positions covering the removed run, so one edit removes at most
 * a+2 distinct trigrams from the set.  Composing ed edits,
 *
 *		T(pattern) - shared  <=  (a_max + 2) * ed
 *
 * where a_max is the largest byte length of any character the edit script
 * deletes or substitutes away.  An optimal script only ever deletes characters
 * of the pattern and inserts characters of the term, so a_max is bounded by the
 * longest character occurring in either string.
 *
 * THE UNIT IS THE CHARACTER AND THE TRIGRAM IS A BYTE TRIGRAM, and that mismatch
 * is exactly the bug G30 was: with both strings ASCII, a_max = 1 and the divisor
 * is 3 as the spec writes it; with a four-byte character anywhere in either
 * string, a_max = 4 and the divisor is 6.  Using 3 there would make the bound up
 * to twice too large -- unsound in the row-dropping direction.  Hence the block
 * carries an all-ASCII flag, the pattern carries one, and the divisor is 3 only
 * when both say so.
 */
#define WEAVE_EDIST_TRGDIV_ASCII	3

/*
 * Statistics over one block (= one dictionary page), sufficient for the bound.
 *
 * minchars/maxchars are CHARACTER lengths.  The dictionary stores only
 * WeaveDictEntry.termlen, a BYTE length, so a character length has to come from
 * somewhere; the two available inequalities are
 *
 *		chars >= ceil(bytes / maxcharlen)		and		chars <= bytes
 *
 * and the SOUND DIRECTION is not symmetric.  The bound needs a LOWER bound on
 * the block's minimum character length (to claim "every term here is at least
 * this much longer than the pattern") and an UPPER bound on its maximum (to
 * claim "every term here is at least this much shorter").  So a byte-derived
 * block would have to use ceil(minbytes/maxcharlen) for the first and maxbytes
 * for the second -- never minbytes, which is >= the true minchars and would
 * inflate the deficit.  That is the shape of G30: a byte length substituted for a
 * character length in a filter, sound-looking, and wrong in the direction that
 * loses rows.
 *
 * This implementation does not need the weaker byte-derived floor: entering the
 * page is already a pass over its terms, so the exact character count is taken
 * there (pg_mbstrlen_with_len, the same call weave_doc_has_fuzzy() makes, and a
 * no-op on a single-byte server encoding where a byte IS a character).  The
 * inequality is recorded here because a future per-page-header format change
 * would store bytes, and would then have to pick the right side of it.
 *
 * mintrg/maxtrg are counts of DISTINCT byte trigrams, i.e. what
 * weave_trigrams() returns -- a deduplicating extractor, so T(term) is not a
 * function of the term's length and the trigram deficit is not merely the length
 * deficit divided by three.
 *
 * WHERE THE SPEC IS WRONG, recorded rather than quietly fixed (AGENTS.md hard
 * rule 13).  FUZZY_CHANNEL.md sect. 5 writes the bound as
 *
 *		max( length_deficit, ceil((max_T_block - T_pattern)/3),
 *							 ceil((T_pattern - max_T_block)/3) )
 *
 * The second term is UNSOUND.  max_T_block is attained by ONE term of the block;
 * a bound over the block must hold for every term in it, and a block containing
 * both a term with T = 40 and a term with T = T_pattern would be credited a
 * deficit of (40 - T_pattern)/3 while containing a term at deficit 0.  The
 * statistic that direction needs is min_T_block, and the page pass computes it
 * for the same price.  The third term is sound as written (every term has
 * T(t) <= max_T_block, so T_pattern - max_T_block <= T_pattern - T(t)).
 */
typedef struct WeaveEdistStats
{
	weave_ed_uint32 nterms;
	weave_ed_uint32 minchars;
	weave_ed_uint32 maxchars;
	weave_ed_uint32 minbytes;	/* reported by the shuttle; not used by the
								 * bound -- see the note above */
	weave_ed_uint32 maxbytes;
	weave_ed_uint32 mintrg;
	weave_ed_uint32 maxtrg;
	int			all_ascii;		/* every term on this page is all-ASCII */
} WeaveEdistStats;

/* The pattern, reduced to the three numbers the bound needs. */
typedef struct WeaveEdistPattern
{
	weave_ed_uint32 chars;
	weave_ed_uint32 bytes;
	weave_ed_uint32 ntrg;
	int			all_ascii;
	weave_ed_uint32 trgdiv;		/* divisor to use when the block is NOT all
								 * ASCII: 2 + the encoding's max char length */
} WeaveEdistPattern;

typedef enum WeaveEdistError
{
	WEAVE_EDIST_OK = 0,
	WEAVE_EDIST_BACKWARD,		/* seek target below the last position returned */
	WEAVE_EDIST_NEEDBLOCK,		/* target is past the installed block: the
								 * driver must install the next one (or call
								 * weave_edist_finish) */
	WEAVE_EDIST_BADBLOCK		/* a block that does not begin after the
								 * previous one ended */
} WeaveEdistError;

/* An empty block makes no claim: no term is in it, so no distance is bounded. */
static inline void
weave_edist_stats_init(WeaveEdistStats *st)
{
	st->nterms = 0;
	st->minchars = 0;
	st->maxchars = 0;
	st->minbytes = 0;
	st->maxbytes = 0;
	st->mintrg = 0;
	st->maxtrg = 0;
	st->all_ascii = 1;
}

/*
 * Fold one term into a block's statistics.  `chars` is the term's CHARACTER
 * length, `ntrg` its distinct-byte-trigram count, `ascii` whether every byte is
 * below 0x80 (always true on a single-byte server encoding, where a byte is a
 * character and a byte trigram cannot straddle two of them).
 */
static inline void
weave_edist_stats_add(WeaveEdistStats *st, weave_ed_uint32 bytes,
					  weave_ed_uint32 chars, weave_ed_uint32 ntrg, int ascii)
{
	if (st->nterms == 0)
	{
		st->minchars = st->maxchars = chars;
		st->minbytes = st->maxbytes = bytes;
		st->mintrg = st->maxtrg = ntrg;
	}
	else
	{
		if (chars < st->minchars)
			st->minchars = chars;
		if (chars > st->maxchars)
			st->maxchars = chars;
		if (bytes < st->minbytes)
			st->minbytes = bytes;
		if (bytes > st->maxbytes)
			st->maxbytes = bytes;
		if (ntrg < st->mintrg)
			st->mintrg = ntrg;
		if (ntrg > st->maxtrg)
			st->maxtrg = ntrg;
	}
	if (!ascii)
		st->all_ascii = 0;
	st->nterms++;
}

/*
 * The pattern side.  `maxcharlen` is pg_database_encoding_max_length(): 1 makes
 * a byte a character, so the ASCII divisor applies unconditionally there.
 */
static inline void
weave_edist_pattern_init(WeaveEdistPattern *p, weave_ed_uint32 bytes,
						 weave_ed_uint32 chars, weave_ed_uint32 ntrg,
						 int ascii, weave_ed_uint32 maxcharlen)
{
	p->bytes = bytes;
	p->chars = chars;
	p->ntrg = ntrg;
	p->all_ascii = (maxcharlen <= 1) ? 1 : ascii;
	p->trgdiv = (maxcharlen <= 1) ? WEAVE_EDIST_TRGDIV_ASCII : maxcharlen + 2;
	if (p->trgdiv < WEAVE_EDIST_TRGDIV_ASCII)
		p->trgdiv = WEAVE_EDIST_TRGDIV_ASCII;
}

/*
 * ed_lower(block): the largest value the two deficits can justify, i.e. a value
 * L with L <= edit_distance(pattern, t) for EVERY term t in the block.  This is
 * the whole of (C2); everything else in this header is bookkeeping.
 *
 * Length deficit.  ed(a, b) >= | chars(a) - chars(b) | because each edit changes
 * the length by at most one.  Over a block, every term has
 * minchars <= chars(t) <= maxchars, so a pattern shorter than every term is at
 * least minchars - chars(p) edits from all of them, and a pattern longer than
 * every term at least chars(p) - maxchars.  Otherwise some term may have exactly
 * the pattern's length and the deficit is 0.
 *
 * Trigram deficit.  T(p) - shared <= div * ed and shared <= T(t), hence
 * ed >= (T(p) - T(t)) / div; and symmetrically ed >= (T(t) - T(p)) / div.  Over a
 * block, T(t) in [mintrg, maxtrg], so ed >= (T(p) - maxtrg)/div and
 * ed >= (mintrg - T(p))/div.  Integer distances let the quotient be rounded UP.
 *
 * Both are pure arithmetic on state the cursor already holds: (C3).
 */
static inline weave_ed_uint32
weave_edist_lower(const WeaveEdistPattern *p, const WeaveEdistStats *st)
{
	weave_ed_uint32 ld = 0;
	weave_ed_uint32 td = 0;
	weave_ed_uint32 div;

	if (st->nterms == 0)
		return 0;

	if (p->chars < st->minchars)
		ld = st->minchars - p->chars;
	else if (p->chars > st->maxchars)
		ld = p->chars - st->maxchars;

	div = (p->all_ascii && st->all_ascii) ? WEAVE_EDIST_TRGDIV_ASCII : p->trgdiv;
	if (p->ntrg < st->mintrg)
		td = st->mintrg - p->ntrg;
	else if (p->ntrg > st->maxtrg)
		td = p->ntrg - st->maxtrg;
	td = (td + div - 1) / div;

	return ld > td ? ld : td;
}

/*
 * A monotone cursor over dictionary term ordinals, one block at a time.
 *
 * The driver (src/query/edist.c, walking the dictionary chain) installs a block
 * with weave_edist_enter_block() when it opens a page and calls
 * weave_edist_finish() when the chain ends.  The cursor itself reads nothing and
 * owns nothing, which is what lets the property test play the driver with an
 * in-memory array of pages.
 *
 * `ret` is the position the last seek RETURNED, and a target below it is REFUSED
 * rather than clamped -- the V8 rationale recorded in channel.h under "NOT
 * IDEMPOTENT" and implemented identically in gate.h: answering a stale target
 * with the current position turns a fused-loop bug into a wrong answer instead
 * of an error.
 */
typedef struct WeaveEdistCursor
{
	WeaveEdistPattern pat;
	WeaveEdistStats st;			/* the installed block's statistics */
	weave_ed_uint32 first;		/* first term ordinal of the installed block */
	weave_ed_uint32 last;		/* last term ordinal of it, inclusive */
	weave_ed_uint32 pos;		/* current position, or WEAVE_EDIST_END */
	weave_ed_uint32 ret;		/* last position returned by seek */
	int			haveblk;
	int			done;			/* vocabulary exhausted */
	int			seeked;
} WeaveEdistCursor;

static inline void
weave_edist_init(WeaveEdistCursor *c, const WeaveEdistPattern *pat)
{
	c->pat = *pat;
	weave_edist_stats_init(&c->st);
	c->first = 0;
	c->last = 0;
	c->pos = 0;
	c->ret = 0;
	c->haveblk = 0;
	c->done = 0;
	c->seeked = 0;
}

/*
 * Install the block covering term ordinals [first, last].  Refused unless it
 * begins after the previous block ended and is non-empty in the same sense the
 * statistics are: a block with nterms == 0 covers no ordinal and must not be
 * installed, because [first, last] would then bound positions no term occupies
 * and (C2)'s "every position in [cur, blkend]" would be a claim about nothing.
 */
static inline WeaveEdistError
weave_edist_enter_block(WeaveEdistCursor *c, weave_ed_uint32 first,
						weave_ed_uint32 last, const WeaveEdistStats *st)
{
	if (c->done || last < first || st->nterms == 0 ||
		last - first + 1 != st->nterms ||
		last >= WEAVE_EDIST_END)
		return WEAVE_EDIST_BADBLOCK;
	if (c->haveblk && first <= c->last)
		return WEAVE_EDIST_BADBLOCK;
	if (c->seeked && first < c->ret)
		return WEAVE_EDIST_BADBLOCK;
	c->first = first;
	c->last = last;
	c->st = *st;
	c->haveblk = 1;
	return WEAVE_EDIST_OK;
}

/* No more blocks: every later seek answers WEAVE_EDIST_END. */
static inline void
weave_edist_finish(WeaveEdistCursor *c)
{
	c->done = 1;
	c->haveblk = 0;
	weave_edist_stats_init(&c->st);
}

/*
 * (C1): the smallest position >= target this channel could contribute at.
 *
 * Every dictionary term has a distance, so within the installed block that is
 * simply max(target, first); the interesting answers are the two refusals and
 * the end sentinel.  A target past the installed block is NEEDBLOCK rather than
 * a silent end: the driver holds the chain and must decide whether another page
 * exists, and inventing WEAVE_EDIST_END here would truncate the vocabulary.
 */
static inline WeaveEdistError
weave_edist_seek(WeaveEdistCursor *c, weave_ed_uint32 target,
				 weave_ed_uint32 *out)
{
	if (c->seeked && target < c->ret)
		return WEAVE_EDIST_BACKWARD;
	if (c->done)
	{
		c->seeked = 1;
		c->pos = WEAVE_EDIST_END;
		c->ret = WEAVE_EDIST_END;
		*out = WEAVE_EDIST_END;
		return WEAVE_EDIST_OK;
	}
	if (!c->haveblk || target > c->last)
		return WEAVE_EDIST_NEEDBLOCK;
	c->seeked = 1;
	c->pos = target < c->first ? c->first : target;
	c->ret = c->pos;
	*out = c->pos;
	return WEAVE_EDIST_OK;
}

/* The last position block_max() speaks for: channel.h's s->blkend. */
static inline weave_ed_uint32
weave_edist_blkend(const WeaveEdistCursor *c)
{
	return (c->done || !c->haveblk) ? c->pos : c->last;
}

/*
 * (C2) and (C4) as the two numbers the glue turns into float4s, so the decision
 * is HERE, under the property test, and src/query/edist.c only translates:
 * block_max() = -(float) weave_edist_block_lower(), score() = -(float) dist.
 */
static inline weave_ed_uint32
weave_edist_block_lower(const WeaveEdistCursor *c)
{
	if (c->done || !c->haveblk)
		return 0;
	return weave_edist_lower(&c->pat, &c->st);
}

/* ---------------------------------------------------------------------------
 * The WeaveShuttle face -- backend only (src/query/edist.c)
 * ------------------------------------------------------------------------- */
#ifdef POSTGRES_H

#include "utils/memutils.h"

#include "weave/am.h"
#include "weave/channel.h"

/*
 * WDOC_LEX_OPS's strategy numbers.  1 is `@@@` (the boolean search operator), 2 is
 * `<=>` (BM25 distance), and 3 is `<@>`.  amrescan is handed ScanKeyData whose
 * sk_strategy is the only thing distinguishing a wquery argument from a text one,
 * and DatumGetWQuery() on a text datum is a silent misread, so the number is named
 * here rather than written as a literal at the one place that reads it.
 *
 * THESE ARE NOT THE ONLY TWO.  Strategy numbers are scoped to an operator FAMILY,
 * so the same three integers are reused by gram_ops (include/weave/cgram.h) and by
 * wvec_weave_ops (WEAVE_STRAT_VEC_DISTANCE in include/weave/am.h, which is why that
 * one is 1 and not the next free number).  Adding a member to any family means
 * re-reading all three lists, because weave_rescan() dispatches order-by keys on
 * sk_strategy and only the attribute tells two families' numbers apart.
 */
#define WEAVE_STRAT_DISTANCE	2
#define WEAVE_STRAT_EDIST		3

/*
 * Per-pass work counters, the numbers bench/RESULTS_EDIST_BOUND.md reports and
 * the scan uses to decide nothing at all.  They exist because a bound whose
 * pruning rate is not counted is a bound nobody knows the value of (hard rule 9).
 */
typedef struct WeaveEdistCounters
{
	int64		npage;			/* dictionary pages entered */
	int64		npage_pruned;	/* ... skipped whole on the bound */
	int64		nterm;			/* terms on entered pages */
	int64		nterm_scored;	/* ... whose exact distance was computed */
} WeaveEdistCounters;

/*
 * Open a shuttle over one segment's dictionary chain for `pat`.  The pattern is
 * copied into the shuttle's own context, a child of `cxt`, so an ERROR between
 * begin() and end() reclaims everything at abort without end() running.  The
 * shuttle holds one dictionary buffer pinned and share-locked while positioned,
 * exactly as the fuzzy walk's iterator does (weave_fuzzy_terms in
 * src/am/amscan.c), so a caller must wrap its use in PG_TRY/PG_FINALLY and call
 * weave_edist_shuttle_end() from the cleanup arm.
 *
 * maxscore is 0.0: a distance is never negative, so no position can score above
 * zero, and zero is attained exactly when the pattern is itself a vocabulary
 * term.  That is the tight static ceiling, so there is nothing cleverer to
 * compute up front the way the vector shuttle computes its LUT bound.
 */
extern WeaveShuttle *weave_edist_shuttle_begin(Relation index,
											   const WeaveSegMeta *seg,
											   const char *pat, int patlen,
											   MemoryContext cxt);

/* The dictionary entry the shuttle is positioned on, or NULL past the end.
 * Valid only until the next seek: it points into a share-locked buffer.  The
 * scan needs it for the posting locator (firstposting/firstoffset/df), which is
 * three fields a warp position cannot carry -- the same reach-back
 * weave_fuzzy_hit() documents. */
extern const WeaveDictEntry *weave_edist_shuttle_entry(WeaveShuttle *s);

/* The exact distance at the current position, cut off above `maxd`: the return
 * is the distance when it is <= maxd and some value > maxd otherwise, which is
 * core's varstr_levenshtein_less_equal() contract and the cheap path the scan
 * uses.  score() is the same number unbounded, negated. */
extern int	weave_edist_shuttle_dist_le(WeaveShuttle *s, int maxd);

/* Skip the rest of the block the shuttle is on because block_max() ruled it
 * out: seek(blkend + 1), counted as a pruned page.  Returns the new position. */
extern WeaveWarp weave_edist_shuttle_skip_block(WeaveShuttle *s);

/* The bound for the block the shuttle is on, as an integer distance -- what
 * block_max() negates.  Exposed so the scan can compare it against an integer
 * threshold without a float round trip. */
extern int	weave_edist_shuttle_block_lower(WeaveShuttle *s);

extern void weave_edist_shuttle_counters(WeaveShuttle *s,
										 WeaveEdistCounters *out);
extern void weave_edist_shuttle_end(WeaveShuttle *s);

/* The heap-side half, src/query/doc.c: the minimum character-level Levenshtein
 * distance from `pat` to any term of `doc`, or -1 for a term-free document.
 * This is what the `<@>` operator evaluates and what the scan uses for
 * pending-list documents, so the two agree by construction. */
extern int	weave_doc_min_edist(WeaveDoc doc, const char *pat, int patlen);

#endif							/* POSTGRES_H */

#endif							/* WEAVE_EDIST_H */
