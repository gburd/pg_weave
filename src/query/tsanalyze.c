/*-------------------------------------------------------------------------
 *
 * pg_weave_tsanalyze.c
 *		Analyzer that reuses PostgreSQL's existing text-search pipeline.
 *
 * Stage 2 of pg_weave.  Where pg_weave_analyze.c provides a minimal self-contained
 * tokenizer, this file makes the analyzer *pluggable* by binding an wdoc to
 * any installed text search configuration (pg_ts_config): the configured
 * parser and dictionary chain (snowball stemmers, ispell, synonyms, thesaurus,
 * stopwords) are run via parsetext(), and the resulting normalized lexemes are
 * folded into an wdoc.  No tokenizer or dictionary code is reimplemented --
 * this is the reuse the design calls for.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_tsanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "catalog/pg_type.h"
#include "tsearch/ts_cache.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

/*
 * Token positions without core's 16,383 clamp (doc/GAPS.md G89).
 *
 * parsetext() keeps an unclamped int32 token counter in prs->pos, but stores
 * each word's position as LIMITPOS(prs->pos) in a uint16, so every word past
 * token 16,383 used to get position 16,383: a term recurring there had
 * duplicate positions, which weave_doc_build() (wdoc_in, wdoc_recv) rejects, so
 * the value could not be dumped and restored.  The tokenizer and dictionary
 * loop that produces the true ordinal (LexizeInit/LexizeExec) is static in
 * ts_parse.c, so it cannot be re-run here without copying it.
 *
 * Instead the counter starts at PG_INT32_MIN.  It then stays negative for any
 * document a varlena can hold, so LIMITPOS() never clamps, and because
 * PG_INT32_MIN is a multiple of 65536 the stored uint16 is the true 1-based
 * ordinal t mod 65536.  The words come out in token order, so t is
 * non-decreasing along the array, and the builder unwraps it: u_0 is the
 * smallest value >= 1 congruent to r_0, and u_i the smallest >= u_(i-1)
 * congruent to r_i.
 *
 * Exactness.  By induction u_i <= t_i and d_i = t_i - u_i is a multiple of
 * 65536; and d_i - d_(i-1) = (t_i - t_(i-1)) - (u_i - u_(i-1)) > -65536, so d is
 * non-decreasing.  The true count N (prs->pos - PG_INT32_MIN) bounds t_last, so
 * N - u_last < 65536 forces d_last = 0 and with it every d_i = 0: every
 * position is exact.  Conversely the check fails exactly when the document has
 * 65,535 or more consecutive lexeme-less tokens (stopwords) between two
 * lexemes, or 65,536 or more at its start or end: there a gap of 65,536 cannot
 * be told from a gap of 0, so the document is refused rather than stored with
 * a wrong position.  sql/wdoc_roundtrip.sql tests both sides of the boundary.
 */
#define WEAVE_PRS_POS_BASE	PG_INT32_MIN

typedef struct TsWord
{
	const char *word;
	int			len;
	uint32		pos;			/* true 1-based token ordinal */
} TsWord;

/* sort by (text, len, pos) so a term's positions come out ascending */
static int
cmp_tsword(const void *a, const void *b)
{
	const TsWord *wa = (const TsWord *) a;
	const TsWord *wb = (const TsWord *) b;
	int			c = memcmp(wa->word, wb->word, Min(wa->len, wb->len));

	if (c != 0)
		return c;
	if (wa->len != wb->len)
		return wa->len - wb->len;
	if (wa->pos != wb->pos)
		return wa->pos < wb->pos ? -1 : 1;
	return 0;
}

/*
 * Build an wdoc from the words produced by parsetext() (called with
 * prs->pos = WEAVE_PRS_POS_BASE).  The words are not sorted and may contain
 * duplicates and several variants per position.  doclen is the number of
 * tokens that produced at least one lexeme (weave_doc_default_len: stopwords do
 * not count, a token with two lexemes counts once).  ntok, every token
 * including stopwords, is only the G89 bookkeeping check's bound.  A
 * dictionary can emit the same lexeme twice
 * for one token (ispell: 'footballklubber' -> ...klubber...klubber); like
 * to_tsvector, that counts once, so tf is the term's number of distinct
 * positions and positions stay strictly ascending.
 */
static WeaveDoc
wdoc_from_parsed(ParsedText *prs, uint8 label)
{
	int			nw = prs->curwords;
	int64		ntok = (int64) prs->pos - WEAVE_PRS_POS_BASE;
	TsWord	   *tw;
	char	  **terms;
	int		   *lens;
	uint32	   *tfs;
	uint32	   *positions;
	uint32		nterms = 0;
	uint32		npos = 0;
	uint32		last = 1;
	int64		nlexpos = 0;	/* tokens with >= 1 lexeme: the doclen */
	int			i;

	if (nw == 0)
		return weave_doc_build(0, NULL, NULL, NULL, true, NULL, 0, "wdoc");

	/*
	 * prs->words is one plain repalloc'd array of 24-byte entries, so nw is
	 * below MaxAllocSize / 24 and each array here fits a plain palloc.
	 */
	tw = (TsWord *) palloc(nw * sizeof(TsWord));	/* alloc-ok: nw < MaxAllocSize/sizeof(ParsedWord) */
	for (i = 0; i < nw; i++)
	{
		uint32		r = prs->words[i].pos.pos;	/* true ordinal mod 65536 */

		last += (r - last) & 0xFFFF;
		/* words come in token order, so a new ordinal is a new token */
		if (i == 0 || last != tw[i - 1].pos)
			nlexpos++;
		tw[i].word = prs->words[i].word;
		tw[i].len = prs->words[i].len;
		tw[i].pos = last;
	}
	/* last <= ntok is the invariant above; a 1 GB text has < 2^30 tokens */
	if (last > ntok || ntok > WEAVE_POS_ORD_MASK)
		elog(ERROR, "parsetext() position bookkeeping is not what to_wdoc() expects");
	if (ntok - last >= 65536)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("document has a run of 65535 or more consecutive tokens that produce no lexeme"),
				 errdetail("Token positions after such a run cannot be determined exactly.")));

	qsort(tw, nw, sizeof(TsWord), cmp_tsword);

	terms = (char **) palloc(nw * sizeof(char *));	/* alloc-ok: see tw */
	lens = (int *) palloc(nw * sizeof(int));	/* alloc-ok: see tw */
	tfs = (uint32 *) palloc(nw * sizeof(uint32));	/* alloc-ok: see tw */
	positions = (uint32 *) palloc(nw * sizeof(uint32));	/* alloc-ok: see tw */
	for (i = 0; i < nw; i++)
	{
		if (i > 0 && tw[i].len == tw[i - 1].len &&
			memcmp(tw[i].word, tw[i - 1].word, tw[i].len) == 0)
		{
			if (tw[i].pos == tw[i - 1].pos)
				continue;		/* the same lexeme twice for one token */
			tfs[nterms - 1]++;
		}
		else
		{
			terms[nterms] = (char *) tw[i].word;
			lens[nterms] = tw[i].len;
			tfs[nterms] = 1;
			nterms++;
		}
		positions[npos++] = WEAVE_POS_MAKE(tw[i].pos, label);
	}

	return weave_doc_build(nterms, terms, lens, tfs, true, positions, nlexpos,
						   "wdoc");
}

/*
 * weave_analyze_with_config -- analyze text using a specific TS configuration.
 */
WeaveDoc
weave_analyze_with_config(Oid cfgId, const char *str, int len, uint8 label)
{
	ParsedText	prs;
	WeaveDoc		doc;
	char	   *buf;

	prs.lenwords = Max(len / 6, 16);
	prs.curwords = 0;
	prs.pos = WEAVE_PRS_POS_BASE;	/* see wdoc_from_parsed */
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	/* parsetext wants a writable buffer */
	buf = (char *) palloc(len + 1);
	memcpy(buf, str, len);
	buf[len] = '\0';

	parsetext(cfgId, &prs, buf, len);

	doc = wdoc_from_parsed(&prs, label);

	if (prs.words)
		pfree(prs.words);
	pfree(buf);

	return doc;
}

PG_FUNCTION_INFO_V1(to_wdoc_byid);
PG_FUNCTION_INFO_V1(to_wdoc_from_tsvector);

/*
 * to_wdoc(tsvector) -- build an wdoc directly from an existing tsvector,
 * with no re-analysis of source text.  A tsvector is already the exact shape
 * an wdoc needs: lexemes sorted + distinct, each with an ascending position
 * list (1-based), so this is a straight structural map.  It is the adoption
 * on-ramp for a table that already materializes a tsvector column.
 *
 * Positions: a tsvector entry may be positionless (haspos=0, e.g. after
 * strip(), or `tsv || 'tag'::tsvector`) or carry positions.  A document is
 * positions-off only when NO entry has positions (a stripped tsvector: tf = 1
 * per lexeme, as core's ts_rank treats it).  A MIXED document keeps the
 * positions it has, and a positionless entry gets tf = 1 and the one position
 * ordinal 0 -- "occurs, position unknown", core's POSNULL convention
 * (tsrank.c).  Ordinal 0 never takes part in adjacency (weave_phrase_step_pos)
 * and is never in a weight zone (term_positions), so a phrase over such an
 * entry is false, as core's `@@` answers it (checkclass_str returns TS_MAYBE,
 * which TS_execute turns into false at the topmost phrase operator), while a
 * phrase over the positioned entries of the same document still matches.  It
 * used to drop the positions of the WHOLE document when any one entry lacked
 * them, which made every phrase on it false.  Positions are taken via
 * WEP_GETPOS (the 14-bit position, weight bits mapped to our label); tsvector
 * positions are >= 1, ascending and distinct within an entry, and
 * weave_doc_build re-validates at the trust boundary.
 */
Datum
to_wdoc_from_tsvector(PG_FUNCTION_ARGS)
{
	TSVector	tsv = PG_GETARG_TSVECTOR(0);
	int			n = tsv->size;
	WordEntry  *we = ARRPTR(tsv);
	char	   *lexbase = STRPTR(tsv);
	char	  **terms;
	int		   *lens;
	uint32	   *tfs;
	uint32	   *positions = NULL;
	bool		has_pos;
	uint64		npos = 0;
	int			i;
	WeaveDoc		doc;

	if (n == 0)
	{
		doc = weave_doc_build(0, NULL, NULL, NULL, false, NULL, -1, "wdoc");
		PG_FREE_IF_COPY(tsv, 0);
		PG_RETURN_WDOC(doc);
	}

	/* first pass: positions-on unless every entry is positionless */
	has_pos = false;
	for (i = 0; i < n; i++)
	{
		int			np = POSDATALEN(tsv, &we[i]);

		if (np > 0)
			has_pos = true;
		npos += (np > 0) ? (uint64) np : 1;
	}

	terms = (char **) palloc(n * sizeof(char *));
	lens = (int *) palloc(n * sizeof(int));
	tfs = (uint32 *) palloc(n * sizeof(uint32));
	if (has_pos)
		positions = (uint32 *) (npos * sizeof(uint32) > MaxAllocSize
							   ? MemoryContextAllocHuge(CurrentMemoryContext,
														npos * sizeof(uint32))
							   : palloc(npos * sizeof(uint32)));	/* alloc-ok: huge branch of the > MaxAllocSize ternary above */

	{
		uint64		p = 0;

		for (i = 0; i < n; i++)
		{
			int			np = POSDATALEN(tsv, &we[i]);

			terms[i] = lexbase + we[i].pos;
			lens[i] = we[i].len;
			tfs[i] = (np > 0) ? (uint32) np : 1;
			if (has_pos)
			{
				WordEntryPos *pv = POSDATAPTR(tsv, &we[i]);
				int			k;

				/* tsvector weight (0..3 = D,C,B,A) maps directly to our label */
				for (k = 0; k < np; k++)
					positions[p++] = WEAVE_POS_MAKE(WEP_GETPOS(pv[k]),
												  WEP_GETWEIGHT(pv[k]));
				if (np <= 0)
					positions[p++] = WEAVE_POS_UNKNOWN;	/* mixed doc: see above */
			}
		}
	}

	doc = weave_doc_build((uint32) n, terms, lens, tfs, has_pos, positions,
						-1, "wdoc");
	PG_FREE_IF_COPY(tsv, 0);
	PG_RETURN_WDOC(doc);
}

/* to_wdoc(regconfig, text) */
Datum
to_wdoc_byid(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *in = PG_GETARG_TEXT_PP(1);
	WeaveDoc		doc;

	doc = weave_analyze_with_config(cfgId,
								  VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in), 0);
	PG_FREE_IF_COPY(in, 1);
	PG_RETURN_WDOC(doc);
}

PG_FUNCTION_INFO_V1(to_wdoc_byid_weight);

/* to_wdoc(regconfig, text, weight "char") -- tag every token position with
 * the weight label A/B/C/D so a query term can restrict to this field (zone). */
Datum
to_wdoc_byid_weight(PG_FUNCTION_ARGS)
{
	Oid			cfgId = PG_GETARG_OID(0);
	text	   *in = PG_GETARG_TEXT_PP(1);
	char		w = PG_GETARG_CHAR(2);
	WeaveDoc		doc;

	if (w != 'A' && w != 'B' && w != 'C' && w != 'D' &&
		w != 'a' && w != 'b' && w != 'c' && w != 'd')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("weight must be one of A, B, C, D")));
	doc = weave_analyze_with_config(cfgId, VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in),
								  WEAVE_WEIGHT_LABEL(w));
	PG_FREE_IF_COPY(in, 1);
	PG_RETURN_WDOC(doc);
}

/*
 * weave_normalize_term -- run a single query term through a text-search config's
 * parser+dictionary pipeline and return its normalized lexeme (palloc'd), so a
 * query term matches the same stemmed/stopword-processed form the document
 * index stores.  Returns NULL and sets *outlen=0 if the term normalizes away
 * (e.g. it is a stopword), in which case the caller should drop it.  If the
 * term produces multiple lexemes only the first is used (query terms are single
 * words in stage-1 syntax).
 */
char *
weave_normalize_term(Oid cfgId, const char *term, int len, int *outlen)
{
	ParsedText	prs;
	char	   *buf;
	char	   *result = NULL;

	*outlen = 0;
	prs.lenwords = 4;
	prs.curwords = 0;
	prs.pos = 0;
	prs.words = (ParsedWord *) palloc(sizeof(ParsedWord) * prs.lenwords);

	buf = (char *) palloc(len + 1);
	memcpy(buf, term, len);
	buf[len] = '\0';
	parsetext(cfgId, &prs, buf, len);

	if (prs.curwords > 0)
	{
		result = (char *) palloc(prs.words[0].len);
		memcpy(result, prs.words[0].word, prs.words[0].len);
		*outlen = prs.words[0].len;
	}
	pfree(buf);
	if (prs.words)
		pfree(prs.words);
	return result;
}
