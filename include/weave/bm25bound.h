/*-------------------------------------------------------------------------
 *
 * bm25bound.h -- one term's BM25 contribution and its per-block upper bound
 *
 * Task F6.  This is the arithmetic the lexical channel's shuttle needs to
 * satisfy contracts (C2), (C3) and (C4) of include/weave/channel.h, extracted
 * here as pure standalone C -- no PostgreSQL headers -- in the pattern of
 * include/weave/for.h and include/weave/gate.h, so that the property test
 * test/hegel/test_lexbound.c can link it with a bare compiler and run millions
 * of cases (AGENTS.md hard rule 1, doc/TESTING.md).
 *
 * THE POINT IS THAT THERE IS ONE COPY.  src/am/amscan.c's WAND used to carry
 * its own transcription of both formulas (wand_contrib_cur() and
 * wand_block_max_contrib()).  A property test over a second copy tests the copy
 * nobody runs, which is the exact failure include/weave/for.h's header comment
 * records for the doclen sidecar walk: a real change to the gate shipped with
 * zero coverage because the test had its own transcription and kept passing.
 * So amscan.c calls these functions; it does not paraphrase them.
 *
 * THE SATURATION FUNCTION.  For one query term t and one document D, Okapi
 * BM25's per-term contribution (Lucene IDF, src/query/rank.c sect. "Score") is
 *
 *		contrib(tf, |D|)  =  idf * tf * (k1 + 1)
 *						  / ( tf + k1*(1 - b) + (k1*b/avgdl) * |D| )
 *
 * which is the usual form with the length norm multiplied out, so that the two
 * terms that do not depend on the posting -- k1*(1-b) and k1*b/avgdl -- can be
 * computed once per (term, segment) and reused for every posting.  That is what
 * WeaveBm25Factors holds, and holding it is not premature: the WAND hot path
 * evaluates contrib() once per scored posting, and the alternative spends two
 * extra divisions there.
 *
 * (C2) THE BLOCK BOUND, AND WHY max_tf AND min_doclen ARE THE RIGHT EXTREMES.
 * A posting block's header (WeaveBlockHdr in include/weave/am.h) stores the
 * block's max tf and its min |D|.  Write A = k1*(1-b) + (k1*b/avgdl)*|D| >= 0,
 * so contrib = idf*(k1+1) * tf / (tf + A).  Then
 *
 *		d/d(tf)  contrib  =  idf*(k1+1) * A / (tf + A)^2	>= 0
 *		d/d|D|   contrib  = -idf*(k1+1) * tf * (k1*b/avgdl) / (tf + A)^2  <= 0
 *
 * for idf >= 0, k1 >= 0, 0 <= b <= 1, avgdl > 0.  The contribution is therefore
 * non-decreasing in tf and non-increasing in |D|, so over any set of postings
 * it is maximized at the LARGEST tf together with the SHORTEST document -- the
 * two values the block header already carries, one from each end.  Neither
 * extreme need belong to the same posting for the bound to be valid; that is
 * what makes it computable from two independent per-block statistics.
 *
 * AND THE BOUND IS EXACT AT THE EXTREME, NOT MERELY SOUND.  It is defined below
 * as contrib() itself evaluated at (max_tf, min_doclen) -- one forwarding call,
 * not a second expression.  Three consequences, and the first is the reason for
 * the shape:
 *
 *	 1. weave_bm25_block_bound() == weave_bm25_contrib() BITWISE whenever a
 *		posting sits at both extremes, because it is the same expression on the
 *		same inputs.  A bound that is attained is a bound that cannot be
 *		silently loose, and test/hegel/test_lexbound.c asserts the equality
 *		exactly rather than within a tolerance.
 *	 2. (C2) then reduces to the monotonicity above: no separate inequality
 *		needs to hold in floating point beyond "contrib does not increase when
 *		tf falls or |D| grows".  An independently-written bound expression would
 *		need its own last-bit argument, because two roundings of the same real
 *		product are not ordered -- see the note on the term-wide ceiling below,
 *		which is where this tree still has one.
 *	 3. It is the TIGHTEST bound derivable from (max_tf, min_doclen) alone.  A
 *		tighter one needs more per-block state, which is a format change, not an
 *		arithmetic change.
 *
 * WHAT IS NOT IN THE DOMAIN, stated because the functions do not check and a
 * caller that violates it gets a NaN rather than an error:
 *
 *	 - tf >= 1 for contrib(), max_tf >= 1 for the bound.  A posting exists only
 *	   because tf >= 1.  With tf = 0 AND k1 = 0 the denominator is 0 and the
 *	   quotient is NaN; every other combination is finite.
 *	 - idf >= 0.  The Lucene IDF is non-negative for df <= N, which the
 *	   dictionary guarantees.  A negative idf flips both derivatives above and
 *	   the bound stops being one.
 *	 - avgdl > 0.  weave_bm25_factors_init() CLAMPS a non-positive avgdl to 1.0
 *	   rather than propagating an infinity, which is what src/query/rank.c:130
 *	   already does at the heap-side scorer's entry, so the two copies of BM25
 *	   in this tree agree about the degenerate corpus.  In the index path the
 *	   clamp is unreachable -- avgdl is sumdoclen/ndocs, and a term with any
 *	   posting at all forces sumdoclen >= 1 -- so it changes no answer; it
 *	   exists so this header is TOTAL and can be property-tested at avgdl = 0
 *	   instead of having the test avoid the case.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/bm25bound.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_BM25BOUND_H
#define WEAVE_BM25BOUND_H

/*
 * The per-(term, segment) constants of the saturation function.  Field names
 * and initialization order are those of the WandCursor fields this replaced
 * (src/am/amscan.c), so the F6 refactor is a move a reviewer can read as one:
 * every expression below appeared verbatim in that file.
 */
typedef struct WeaveBm25Factors
{
	double		idf;			/* ln(1 + (N - df + 0.5)/(df + 0.5)) */
	double		k1;
	double		b;
	double		avgdl;			/* AFTER the non-positive clamp */

	/* Precomputed: the two parts of the norm that do not depend on the posting,
	 * and the numerator's constant factor. */
	double		k1b_inv_avgdl;	/* k1*b/avgdl */
	double		k1_1mb;			/* k1*(1-b) */
	double		idf_k1p1;		/* idf*(k1+1) */
} WeaveBm25Factors;

/*
 * Precompute the factors for one term in one segment.  Called once per cursor,
 * never per posting.  See the header comment for the avgdl clamp.
 */
static inline void
weave_bm25_factors_init(WeaveBm25Factors *f, double idf, double k1, double b,
						double avgdl)
{
	if (!(avgdl > 0.0))
		avgdl = 1.0;

	f->idf = idf;
	f->k1 = k1;
	f->b = b;
	f->avgdl = avgdl;
	f->k1b_inv_avgdl = k1 * b / avgdl;
	f->k1_1mb = k1 * (1.0 - b);
	f->idf_k1p1 = idf * (k1 + 1.0);
}

/*
 * (C4) The EXACT contribution of a posting with term frequency `tf` in a
 * document of length `doclen`.
 *
 * The operation order is load-bearing and is the one src/am/amscan.c's
 * wand_contrib_cur() has always used: the norm is summed left to right as
 * tf + k1_1mb + k1b_inv_avgdl*doclen, and the numerator is the single
 * precomputed product idf_k1p1 times tf.  Any other grouping changes the last
 * bit, and the last bit of a score is the order of two documents that tie.
 */
static inline double
weave_bm25_contrib(const WeaveBm25Factors *f, double tf, double doclen)
{
	double		norm = tf + f->k1_1mb + f->k1b_inv_avgdl * doclen;

	return f->idf_k1p1 * tf / norm;
}

/*
 * (C2)+(C3) The upper bound on weave_bm25_contrib() over every posting of a
 * block whose header says "no tf above max_tf, no document shorter than
 * min_doclen".  Cheap by construction: three multiplies, two adds, one divide,
 * no memory beyond the factors and the two header values (C3).
 *
 * ONE FORWARDING CALL, DELIBERATELY.  See consequence 1 in the header comment:
 * evaluating the contribution itself at the extremes is what makes the bound
 * exactly attained instead of approximately tight, and makes the (C2)
 * inequality a statement about monotonicity rather than about two independent
 * roundings of the same real number.  Do not "simplify" this into its own
 * expression -- that is the change test/hegel/test_lexbound.c's tightness
 * assertion exists to catch.
 */
static inline double
weave_bm25_block_bound(const WeaveBm25Factors *f, double max_tf,
					   double min_doclen)
{
	return weave_bm25_contrib(f, max_tf, min_doclen);
}

/*
 * The TERM-WIDE ceiling: the bound over every posting of the term in the
 * segment, from the dictionary's max tf alone.  This is WeaveShuttle.maxscore
 * (include/weave/channel.h) and the per-term impact the WAND pivot and
 * MaxScore's partition accumulate.
 *
 * |D| is taken to zero rather than to any observed length, because the
 * dictionary records no minimum document length -- so the length norm vanishes
 * entirely and the denominator is max_tf + k1*(1-b).
 *
 * WHY THIS ONE IS NOT ALSO A FORWARDING CALL, which is the one place this
 * header keeps two roundings of idf*(k1+1).  The expression is the verbatim
 * move of src/am/amscan.c's `idf * mtf * (k1 + 1.0) / (mtf + k1 * (1.0 - b))`,
 * and max_contrib feeds the pivot accumulation, the MaxScore ordering and its
 * suffix sums; changing its last bit changes which cursors sort into the
 * essential set, so F6 left it alone (see the F6 report).  channel.h requires
 * maxscore >= block_max() everywhere, and that survives the differing
 * rounding with room to spare rather than by a last-bit argument: this
 * denominator omits (k1*b/avgdl)*min_doclen, and a document containing the term
 * has |D| >= 1, so the block bound's denominator is larger by k1*b/avgdl --
 * about 0.9/avgdl at the k1 = 1.2, b = 0.75 defaults -- which is many orders of
 * magnitude more than the ~1 ULP the two numerator groupings can differ by.
 * The exception is k1 = 0 or b = 0, where the length norm is absent from BOTH
 * denominators and the two can then differ in the last bit; neither is
 * reachable from the scan, which hardcodes k1 = 1.2 and b = 0.75.
 */
static inline double
weave_bm25_term_bound(const WeaveBm25Factors *f, double max_tf)
{
	return f->idf * max_tf * (f->k1 + 1.0) / (max_tf + f->k1 * (1.0 - f->b));
}

/*
 * The same two functions for a caller holding no precomputed factors: the
 * property test, and any future single-shot scorer.  Spelled out with the full
 * parameter list the contract is stated in -- (idf, tf, doclen, k1, b, avgdl)
 * and (idf, max_tf, min_doclen, k1, b, avgdl) -- so the header can be read
 * without WeaveBm25Factors first.  NOT for the scan path: it recomputes a
 * division and two products per call.
 */
static inline double
weave_bm25_contrib_at(double idf, double tf, double doclen,
					  double k1, double b, double avgdl)
{
	WeaveBm25Factors f;

	weave_bm25_factors_init(&f, idf, k1, b, avgdl);
	return weave_bm25_contrib(&f, tf, doclen);
}

static inline double
weave_bm25_block_bound_at(double idf, double max_tf, double min_doclen,
						  double k1, double b, double avgdl)
{
	WeaveBm25Factors f;

	weave_bm25_factors_init(&f, idf, k1, b, avgdl);
	return weave_bm25_block_bound(&f, max_tf, min_doclen);
}

#endif							/* WEAVE_BM25BOUND_H */
