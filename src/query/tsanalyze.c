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

/* qsort_arg comparator: elements are indices into a ParsedWord array (arg).
 * Ties on (text, len) break by token position so a distinct-term run comes out
 * with its positions ascending -- phrase_step requires each term's position
 * list to be ascending (this mirrors cmp_rawterm in pg_weave_analyze.c). */
static int
cmp_word_idx(const void *a, const void *b, void *arg)
{
	ParsedWord *words = (ParsedWord *) arg;
	ParsedWord *wa = &words[*(const int *) a];
	ParsedWord *wb = &words[*(const int *) b];
	int			min = Min(wa->len, wb->len);
	int			c = memcmp(wa->word, wb->word, min);

	if (c != 0)
		return c;
	if (wa->len != wb->len)
		return wa->len - wb->len;
	/* same term: order by position so positions come out ascending.
	 * parsetext() fills pos.pos with a plain 1-based token ordinal via
	 * LIMITPOS() (no weight bits, capped at MAXENTRYPOS-1); alen==0 so the
	 * apos array form is never used on this path. */
	if (wa->pos.pos < wb->pos.pos)
		return -1;
	if (wa->pos.pos > wb->pos.pos)
		return 1;
	return 0;
}

/*
 * Build an wdoc from the words produced by parsetext().  The words are not
 * sorted and may contain duplicates and multiple variants per position, so we
 * sort, deduplicate and count term frequency, exactly like the simple
 * analyzer.  doclen is the number of token positions, which parsetext tracks
 * in prs->pos.  Per-token positions are stored (WEAVE_DOCF_POSITIONS) so phrase
 * and NEAR queries enforce adjacency on this path too; parsetext() fills each
 * ParsedWord.pos.pos with a 1-based token ordinal.
 */
static WeaveDoc
wdoc_from_parsed(ParsedText *prs, uint8 label)
{
	int			nw = prs->curwords;
	ParsedWord *words = prs->words;
	int		   *order;
	int			i;
	int			ndistinct = 0;
	Size		lexbytes = 0;
	WeaveDoc		doc;
	Size		posbase;
	Size		total;
	int			npos;
	WeaveTermEntry *entries;
	char	   *lexemes;
	uint32	   *positions;
	uint32		off;
	uint32		pidx;

	if (nw == 0)
	{
		total = WEAVE_DOC_HDRSIZE;
		doc = (WeaveDoc) palloc0(total);
		SET_VARSIZE(doc, total);
		doc->version = WEAVE_DOC_VERSION;
		doc->flags = 0;
		doc->nterms = 0;
		doc->doclen = 0;
		doc->lexbytes = 0;
		return doc;
	}

	/* index-sort words by (text, len, pos) without disturbing the array */
	order = (int *) palloc(nw * sizeof(int));
	for (i = 0; i < nw; i++)
		order[i] = i;
	qsort_arg(order, nw, sizeof(int), cmp_word_idx, words);

	for (i = 0; i < nw;)
	{
		int			run = 1;
		ParsedWord *w = &words[order[i]];

		while (i + run < nw)
		{
			ParsedWord *n = &words[order[i + run]];
			int			min = Min(w->len, n->len);
			int			c = memcmp(w->word, n->word, min);

			if (c == 0)
				c = w->len - n->len;
			if (c != 0)
				break;
			run++;
		}
		lexbytes += w->len;
		ndistinct++;
		i += run;
	}

	total = WEAVE_DOC_HDRSIZE +
		(Size) ndistinct * sizeof(WeaveTermEntry) + lexbytes;
	/* one stored position per token; npos == nw (sum of tf over all terms) */
	npos = nw;
	posbase = MAXALIGN(total);
	total = posbase + (Size) npos * sizeof(uint32);
	if (total > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("wdoc document is too large"),
				 errdetail("An wdoc value is limited to %zu bytes; this document needs %zu.",
						   (Size) MaxAllocSize, total)));
	doc = (WeaveDoc) palloc0(total);
	SET_VARSIZE(doc, total);
	doc->version = WEAVE_DOC_VERSION;
	doc->flags = WEAVE_DOCF_POSITIONS;
	doc->nterms = ndistinct;
	doc->doclen = (uint32) prs->pos;
	doc->lexbytes = lexbytes;

	entries = WEAVE_DOC_ENTRIES(doc);
	lexemes = WEAVE_DOC_LEXEMES(doc);
	positions = WEAVE_DOC_POSITIONS(doc);
	off = 0;
	pidx = 0;
	ndistinct = 0;
	for (i = 0; i < nw;)
	{
		int			run = 1;
		int			k;
		ParsedWord *w = &words[order[i]];

		while (i + run < nw)
		{
			ParsedWord *n = &words[order[i + run]];
			int			min = Min(w->len, n->len);
			int			c = memcmp(w->word, n->word, min);

			if (c == 0)
				c = w->len - n->len;
			if (c != 0)
				break;
			run++;
		}
		entries[ndistinct].off = off;
		entries[ndistinct].len = w->len;
		entries[ndistinct].tf = run;
		entries[ndistinct].posoff = pidx;
		memcpy(lexemes + off, w->word, w->len);
		off += w->len;
		/* cmp_word_idx broke ties by pos, so this run is already ascending */
		for (k = 0; k < run; k++)
			positions[pidx++] = WEAVE_POS_MAKE(words[order[i + k]].pos.pos, label);
		ndistinct++;
		i += run;
	}
	if (label != 0)
		doc->flags |= WEAVE_DOCF_WEIGHTS;

	return doc;
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
	prs.pos = 0;
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
 * strip()) or carry positions.  wdoc positions are all-or-nothing per doc, so
 * we keep positions ONLY if EVERY entry has them; if any entry is positionless
 * we build a positions-off wdoc (tf = max(npos,1)), matching how a stripped
 * tsvector degrades.  A tsvector position of 0 ("unknown") is treated as
 * positionless for that entry.  Positions are taken via WEP_GETPOS (the 14-bit
 * position, weight bits dropped) and are already ascending + distinct within an
 * entry per tsvector's own invariants; weave_doc_build re-validates at the trust
 * boundary.
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
	bool		has_pos = true;
	uint64		npos = 0;
	int			i;
	WeaveDoc		doc;

	if (n == 0)
	{
		doc = weave_doc_build(0, NULL, NULL, NULL, false, NULL, "wdoc");
		PG_FREE_IF_COPY(tsv, 0);
		PG_RETURN_WDOC(doc);
	}

	/* first pass: decide positions-on/off + total position count */
	for (i = 0; i < n; i++)
	{
		int			np = POSDATALEN(tsv, &we[i]);

		if (np <= 0)
			has_pos = false;
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
			}
		}
	}

	doc = weave_doc_build((uint32) n, terms, lens, tfs, has_pos, positions,
						"wdoc");
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
