/*-------------------------------------------------------------------------
 *
 * pg_weave_trgm.c
 *		Trigram pre-filter for fuzzy/regex term matching at scale.
 *
 * Cribbed in spirit from pg_tre (the approximate-regex access method): a
 * query term is reduced to a set of trigrams, and only dictionary terms
 * sharing a trigram with it are candidates for the expensive exact test.  This
 * turns the naive "test every term" scan into "test only trigram-overlapping
 * terms", which is the pruning that makes fuzzy viable on a large vocabulary.
 *
 * THIS FILE IS THE BYTE-TRIGRAM KEY SPACE, and nothing else.  weave_trigrams()
 * hashes 3 BYTES of a server-encoded term with hash_bytes(); that is the key the
 * trigram weft (src/pages/trgm_page.c) is written and probed with, on both the
 * fuzzy funnel and the regex route.  The regex route's REQUIRED trigrams no
 * longer come from here: weave_regex_trigrams(), a literal-run scanner over the
 * raw pattern text, used to live in this file and read `\d` as the literal `d`
 * (G32), which made the funnel demand a trigram no matching term
 * contains -- a false negative the heap recheck cannot undo.  They now come
 * from the pg_tre AST extractor (src/query/extract.c), whose codepoint triples
 * weave_regex_terms() (src/am/amscan.c) re-encodes and hashes through
 * weave_trigrams() so the two key spaces meet in exactly one place.
 *
 * For fuzzy matching with edit distance k, a term within k edits of the query
 * shares a trigram with it only when k edits cannot destroy them all.  One edit
 * at byte position p destroys the trigrams starting at p-2, p-1 and p -- THREE
 * of them -- so at least (t - 3k) of the query's t trigrams survive and the
 * "shares >= 1 trigram" filter is sound exactly when t > 3k.  Callers that
 * cannot meet that bound must fall back to a full scan; weave_trgm_candidates()
 * takes the required trigram count as an argument and refuses below it, so the
 * choice is the caller's and the failure mode is a slower scan rather than a
 * missing row.  (The bound used to be stated as "k below the trigram count",
 * which admitted the filter for a 5-byte term at k=1 and lost rows: see the
 * comment at its call site in src/am/amscan.c.)
 *
 * This module implements the trigram extraction and the overlap test the
 * heap-side fuzzy prefilter uses (src/query/doc.c); the persistent on-disk
 * trigram weft the weave AM probes at query time is in src/pages/trgm_page.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_trgm.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "common/hashfn.h"

/*
 * Extract the set of byte-trigrams from a term into caller-provided storage.
 * A term of length n < 3 yields a single padded trigram so short terms still
 * participate.  Returns the number of trigrams written (deduplicated).
 */
int
weave_trigrams(const char *s, int len, uint32 *out, int maxout)
{
	int			n = 0;
	int			i;

	if (len <= 0)
		return 0;

	if (len < 3)
	{
		char		pad[3] = {' ', ' ', ' '};
		int			j;

		for (j = 0; j < len; j++)
			pad[j] = s[j];
		if (n < maxout)
			out[n++] = hash_bytes((const unsigned char *) pad, 3);
		return n;
	}

	for (i = 0; i + 3 <= len; i++)
	{
		uint32		h = hash_bytes((const unsigned char *) (s + i), 3);
		int			k;
		bool		dup = false;

		for (k = 0; k < n; k++)
			if (out[k] == h)
			{
				dup = true;
				break;
			}
		if (!dup && n < maxout)
			out[n++] = h;
	}
	return n;
}

/*
 * Do two trigram sets share at least one trigram?  Both are small (bounded by
 * term length), so a nested scan is fine.
 */
bool
weave_trigrams_overlap(const uint32 *a, int na, const uint32 *b, int nb)
{
	int			i,
				j;

	for (i = 0; i < na; i++)
		for (j = 0; j < nb; j++)
			if (a[i] == b[j])
				return true;
	return false;
}
