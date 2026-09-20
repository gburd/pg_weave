/*-------------------------------------------------------------------------
 *
 * cgram.c
 *		Task Z8: the pattern side of the corpus character-trigram channel, and
 *		the two SQL operator procedures that name it.
 *
 * The on-disk side lives in src/am/ambuild.c (writer), src/am/am.c (root page
 * and free path) and src/am/amscan.c (route).  This file is the part with no
 * PostgreSQL page in it: turning a LIKE pattern into the set of trigrams every
 * matching document MUST contain, and evaluating the exact predicate.
 *
 * See include/weave/cgram.h for what this channel is, why the unit is the BYTE,
 * and why both sides are ASCII-case-folded.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/query/cgram.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"	/* DEFAULT_COLLATION_OID for the recheck */
#include "fmgr.h"
#include "utils/builtins.h"	/* cstring_to_text_with_len */
#include "utils/fmgrprotos.h"	/* textlike / texticlike: the SAME C functions
								 * LIKE and ILIKE call */
#include "weave/cgram.h"
#include "weave/weave.h"

/*
 * Fold `len` bytes of `src` into `dst` (which must have room for len bytes).
 * ASCII-only, byte-wise: see WEAVE_CGRAM_FOLD in weave/cgram.h for why that is
 * sound for the case-sensitive operator and why the case-insensitive one refuses
 * non-ASCII literal runs rather than trusting it.
 */
void
weave_cgram_fold(char *dst, const char *src, int len)
{
	int			i;

	for (i = 0; i < len; i++)
		dst[i] = (char) weave_cgram_fold_byte((unsigned char) src[i]);
}

/*
 * The trigram hash of exactly three (already folded) bytes.
 *
 * Routed through weave_trigrams() rather than calling hash_bytes() here, and the
 * reason is the one src/query/trgm.c's header states: that function IS the byte
 * trigram key space, and a second caller computing "the same" hash its own way
 * is how two key spaces drift apart.  With len == 3 it takes the loop path and
 * emits exactly one value, so the O(n^2) dedup inside it never runs -- which
 * matters, because the build path calls this once per BYTE of the corpus and the
 * quadratic dedup weave_trigrams() does over a whole document would be
 * O(doclen^2) per document.  Dropping the dedup is free here: the build sorts
 * (trigram, docid) pairs globally and uniqs them there anyway.
 */
uint32
weave_cgram_hash3(const char *folded3)
{
	uint32		h;

	(void) weave_trigrams(folded3, 3, &h, 1);
	return h;
}

/*
 * The trigrams a LIKE/ILIKE pattern REQUIRES of every document it matches.
 *
 * Writes up to `maxout` hashes to `out` and returns the count, or -1 meaning
 * "this pattern has no usable requirement -- fall back to the universe path".
 * A -1 is not a failure: weave_trgm_candidates() and weave_regex_terms() have
 * the same shape, and the fallback is correct and slow rather than fast and
 * wrong.
 *
 * WHAT IS SOUND TO REQUIRE.  A LIKE pattern is a sequence of LITERAL RUNS
 * separated by `%` and `_`.  Any document the pattern matches contains every
 * literal run verbatim as a substring (that is what LIKE means), so it contains
 * every trigram of every run, so requiring their conjunction cannot lose a row.
 * The converse does not hold at all -- a document may contain all the trigrams
 * in the wrong order, in the wrong runs, or spanning a `%` boundary -- which is
 * why THE RECHECK IS MANDATORY on this route and not an optimization.
 *
 * WHAT IS NOT SOUND, and is therefore dropped rather than approximated:
 *
 *  - A run SHORTER than 3 bytes has no trigram.  weave_trigrams() would happily
 *    return a SPACE-PADDED one for it, and requiring that would demand a trigram
 *    matching documents do not contain -- exactly the G32 false negative the
 *    regex route already paid for.  Short runs contribute nothing.  A pattern
 *    whose every run is short therefore returns -1, which is the "pattern
 *    shorter than 3 bytes, or all wildcards" refusal.
 *  - `_` ENDS a run.  It matches one character, not one byte, and in a
 *    multi-byte encoding it does not even preserve length, so no trigram
 *    straddling it can be required.
 *  - For a case-insensitive pattern, a run containing any byte >= 0x80.  See
 *    WEAVE_CGRAM_FOLD: ILIKE's folding is encoding-aware and ours is not, so a
 *    non-ASCII run may be required in a form the document never stores.  One
 *    such byte anywhere in the pattern refuses the WHOLE pattern, not just that
 *    run: keeping the other runs would still be sound, but a pattern mixing
 *    ASCII and non-ASCII runs under ILIKE is precisely the case where a reader
 *    would later assume the non-ASCII half was handled.
 *
 * ESCAPES.  Backslash is LIKE's default escape: `\%`, `\_` and `\\` are literal
 * bytes and belong to the current run.  A trailing lone backslash is an error in
 * LIKE itself; here it simply ends the pattern, because this function must never
 * be the thing that raises on user input the recheck would have rejected anyway.
 */
int
weave_cgram_required(const char *pat, int patlen, bool caseinsens,
					 uint32 *out, int maxout)
{
	char	   *run;
	int			runlen = 0;
	int			nout = 0;
	int			i;

	if (pat == NULL || patlen <= 0 || maxout <= 0)
		return -1;

	/* one query pattern, bounded by the query text: a plain palloc is right */
	run = (char *) palloc(patlen);	/* alloc-ok: patlen is one query pattern's length */

	for (i = 0; i <= patlen; i++)
	{
		bool		boundary = (i == patlen);
		unsigned char c = 0;

		if (!boundary)
		{
			c = (unsigned char) pat[i];
			if (c == '%' || c == '_')
				boundary = true;
			else if (c == '\\' && i + 1 < patlen)
				c = (unsigned char) pat[++i];	/* escaped: literal byte */
			else if (c == '\\')
				boundary = true;	/* trailing lone backslash: end the pattern */
		}

		if (!boundary)
		{
			if (caseinsens && c >= 0x80)
			{
				pfree(run);
				return -1;		/* see the header: ILIKE + non-ASCII is refused */
			}
			run[runlen++] = (char) weave_cgram_fold_byte(c);
			continue;
		}

		/* flush the run that just ended */
		if (runlen >= 3)
		{
			int			p;

			for (p = 0; p + 3 <= runlen && nout < maxout; p++)
			{
				uint32		h = weave_cgram_hash3(run + p);
				int			k;
				bool		dup = false;

				/* O(n^2) over at most WEAVE_CGRAM_MAX_REQ values, i.e. bounded
				 * by a constant -- unlike the same loop over a document, which
				 * is why weave_cgram_hash3()'s comment says what it says. */
				for (k = 0; k < nout; k++)
					if (out[k] == h)
					{
						dup = true;
						break;
					}
				if (!dup)
					out[nout++] = h;
			}
		}
		runlen = 0;
	}

	pfree(run);
	return nout > 0 ? nout : -1;
}

/* ---------------------------------------------------------------------------
 * The two operator procedures.
 *
 * WHY DEDICATED OPERATORS AND NOT `~~` / `~~*` THEMSELVES.  Putting core's LIKE
 * operator in the family would make every LIKE on the column a candidate for
 * this index, including anchored ones the planner already has btree-oriented
 * special-case machinery for (match_special_index_operator's prefix extraction),
 * and it would route patterns whose cost this AM's estimator does not model.
 * The measurement in doc/specs/FUZZY_CHANNEL.md sect. 6 says the win is narrow
 * (selective cross-token patterns) and the loss is broad (index bytes), so the
 * surface is deliberately opt-in: a query asks for this channel by name.
 *
 * The VALUE is core's, by construction -- textlike()/texticlike() are the same
 * C functions `LIKE` and `ILIKE` call -- which is what makes the parity gate in
 * sql/cgram.sql meaningful.  A hand-written matcher here would be testing our
 * matcher against our matcher.
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(weave_cgram_like);
PG_FUNCTION_INFO_V1(weave_cgram_ilike);

Datum
weave_cgram_like(PG_FUNCTION_ARGS)
{
	return textlike(fcinfo);
}

Datum
weave_cgram_ilike(PG_FUNCTION_ARGS)
{
	return texticlike(fcinfo);
}

/*
 * The same predicate as a plain C call, for the scan route's own recheck.
 *
 * ONE implementation for the operator and for the recheck, on purpose.  If the
 * route rechecked with a second matcher, the parity gate would be comparing two
 * of our matchers and the executor's recheck on a lossy bitmap page (which
 * re-evaluates the OPERATOR) could disagree with the route that produced the
 * page.  Same function, same answer.
 */
bool
weave_cgram_match(const char *val, int vallen, const char *pat, int patlen,
				  bool caseinsens)
{
	text	   *v = cstring_to_text_with_len(val, vallen);
	text	   *p = cstring_to_text_with_len(pat, patlen);
	Datum		d;

	if (caseinsens)
		d = DirectFunctionCall2Coll(texticlike, DEFAULT_COLLATION_OID,
									PointerGetDatum(v), PointerGetDatum(p));
	else
		d = DirectFunctionCall2Coll(textlike, DEFAULT_COLLATION_OID,
									PointerGetDatum(v), PointerGetDatum(p));
	pfree(v);
	pfree(p);
	return DatumGetBool(d);
}
