/*-------------------------------------------------------------------------
 *
 * pg_weave_analyze.c
 *		Stage-1 built-in tokenizer for pg_weave.
 *
 * Produces an wdoc from raw text.  This is the simple, self-contained default
 * analyzer -- to_wdoc(text): fold ASCII letters to lowercase, split on any
 * non-alphanumeric byte, and collect the distinct terms with their term
 * frequencies.
 *
 * The configuration-driven analyzer that reuses PostgreSQL's text-search parser
 * and dictionary pipeline (parsetext(), the snowball/ispell dictionaries) lives
 * in pg_weave_tsanalyze.c as to_wdoc(regconfig, text).  Tokenization is isolated
 * behind weave_analyze_text() so either analyzer can be used without touching the
 * type, the operator, or the on-disk format.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "common/unicode_case.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

/* One collected token before we fold duplicates into the wdoc. */
typedef struct RawTerm
{
	char	   *term;
	int			len;
	uint32		pos;			/* 1-based token position in the source */
} RawTerm;

/*
 * Case-fold a single ASCII byte.  This is the fast path used by fold_token()
 * for ASCII-only tokens; non-ASCII tokens go through Unicode lowercasing.
 */
static inline char
fold_ascii(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return (char) (c - 'A' + 'a');
	return (char) c;
}

static inline bool
is_token_byte(unsigned char c)
{
	/* ASCII alphanumerics start a/continue a token; so do all non-ASCII bytes
	 * (so UTF-8 words are kept whole rather than split at every byte). */
	if (c >= 0x80)
		return true;
	return (c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9');
}

/*
 * An ASCII-only token takes the length-preserving byte fast path.  In a UTF-8
 * database, a token containing any byte >= 0x80 is lowercased per Unicode code
 * point via unicode_lowercase_simple(), so 'E'+U+0301 style caseful pairs like
 * 'É'/'é' fold together and a query for one matches the other.
 *
 * *outlen - receives the folded byte length.
 */
char *
fold_token(const char *src, int len, int *outlen)
{
	char	   *dst = (char *) palloc(Max(len, 1));
	char	   *out;
	const unsigned char *srcp;
	const unsigned char *srcend;
	int			i = 0;

	/*
	 * Fast path: fold ASCII bytes in a single pass, stopping at the first
	 * non-ASCII byte.  ASCII case-folding equals Unicode lowercasing for those
	 * bytes, so a prefix folded here stays correct even if we fall through to
	 * the Unicode path.
	 */
	while (i < len && (unsigned char) src[i] < 0x80)
	{
		dst[i] = fold_ascii((unsigned char) src[i]);
		i++;
	}
	if (i == len)
	{
		*outlen = len;			/* pure ASCII: one pass, exact-sized buffer */
		return dst;
	}

	if (GetDatabaseEncoding() != PG_UTF8)
	{
		/*
		 * Non-UTF-8: a byte >= 0x80 is not a UTF-8 code point, so keep the
		 * ASCII-only behavior (such bytes pass through unchanged).  Folding is
		 * length-preserving, so the len-sized buffer still suffices.
		 */
		for (; i < len; i++)
			dst[i] = fold_ascii((unsigned char) src[i]);
		*outlen = len;
		return dst;
	}

	/*
	 * UTF-8: lowercase the remaining code points.  The folded length can now
	 * differ from len, so grow the buffer to the worst case (each code point's
	 * simple lowercase is a single code point, at most 4 UTF-8 bytes); the
	 * already-folded ASCII prefix is preserved by repalloc.
	 */
	dst = (char *) repalloc(dst, (Size) len * 4 + 1);
	out = dst + i;
	srcp = (const unsigned char *) src + i;
	srcend = (const unsigned char *) src + len;
	while (srcp < srcend)
	{
		int			clen = pg_utf_mblen(srcp);
		pg_wchar	lc = unicode_lowercase_simple(utf8_to_unicode(srcp));

		/* unicode_to_utf8() returns the START pointer, so advance out by the
		 * code point's UTF-8 length ourselves. */
		unicode_to_utf8(lc, (unsigned char *) out);
		out += unicode_utf8len(lc);
		srcp += clen;
	}
	*outlen = (int) (out - dst);
	return dst;
}

static int
cmp_rawterm(const void *a, const void *b)
{
	const RawTerm *ra = (const RawTerm *) a;
	const RawTerm *rb = (const RawTerm *) b;
	int			min = Min(ra->len, rb->len);
	int			c = memcmp(ra->term, rb->term, min);

	if (c != 0)
		return c;
	if (ra->len != rb->len)
		return ra->len - rb->len;
	/* same term: order by position so positions come out ascending */
	if (ra->pos < rb->pos)
		return -1;
	if (ra->pos > rb->pos)
		return 1;
	return 0;
}

/*
 * weave_analyze_text -- tokenize raw text into an wdoc.
 *
 * Returns a palloc'd, fully formed WeaveDoc varlena.  An empty input yields a
 * valid zero-term document (which matches nothing).
 */
WeaveDoc
weave_analyze_text(const char *str, int len)
{
	RawTerm    *raw;
	int			nraw = 0;
	int			maxraw;
	int			i;
	uint32		doclen = 0;

	/* Upper bound on tokens: every other byte could start a token.  Size with a
	 * huge-safe alloc: a very large single document (e.g. an inline patch series)
	 * can push maxraw*sizeof(RawTerm) past MaxAllocSize, and a plain palloc would
	 * throw "invalid memory alloc request size" mid-analyze. */
	maxraw = (len / 2) + 1;
	raw = (RawTerm *) (((Size) maxraw * sizeof(RawTerm)) > MaxAllocSize
					   ? MemoryContextAllocHuge(CurrentMemoryContext,
												(Size) maxraw * sizeof(RawTerm))
					   : palloc((Size) maxraw * sizeof(RawTerm)));	/* alloc-ok: huge branch of the > MaxAllocSize ternary above */

	i = 0;
	while (i < len)
	{
		int			start;

		/* skip separators */
		while (i < len && !is_token_byte((unsigned char) str[i]))
			i++;
		if (i >= len)
			break;

		start = i;
		while (i < len && is_token_byte((unsigned char) str[i]))
			i++;

		Assert(nraw < maxraw);
		raw[nraw].term = fold_token(str + start, i - start, &raw[nraw].len);
		raw[nraw].pos = doclen + 1;	/* 1-based token position */
		nraw++;
		doclen++;
	}

	/* Sort by (term, pos) so duplicates are adjacent and positions ascend. */
	if (nraw > 1)
		qsort(raw, nraw, sizeof(RawTerm), cmp_rawterm);

	/* Second pass: fold duplicates, recording tf and positions. */
	{
		int			ndistinct = 0;
		Size		lexbytes = 0;
		int			npos = nraw;	/* one position per token */
		WeaveDoc		doc;
		Size		posbase;
		Size		total;
		WeaveTermEntry *entries;
		char	   *lexemes;
		uint32	   *positions;
		uint32		off;
		uint32		pidx;
		int		   *runlen;
		int		   *runstart;

		/* identify distinct-term runs (term-only equality) */
		runlen = (int *) palloc(Max(nraw, 1) * sizeof(int));
		runstart = (int *) palloc(Max(nraw, 1) * sizeof(int));
		for (i = 0; i < nraw;)
		{
			int			run = 1;

			while (i + run < nraw)
			{
				int			min = Min(raw[i].len, raw[i + run].len);

				if (raw[i].len != raw[i + run].len ||
					memcmp(raw[i].term, raw[i + run].term, min) != 0)
					break;
				run++;
			}
			runstart[ndistinct] = i;
			runlen[ndistinct] = run;
			lexbytes += raw[i].len;
			ndistinct++;
			i += run;
		}

		posbase = MAXALIGN(WEAVE_DOC_HDRSIZE +
						   (Size) ndistinct * sizeof(WeaveTermEntry) + lexbytes);
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
		doc->doclen = doclen;
		doc->lexbytes = lexbytes;

		entries = WEAVE_DOC_ENTRIES(doc);
		lexemes = WEAVE_DOC_LEXEMES(doc);
		positions = WEAVE_DOC_POSITIONS(doc);
		off = 0;
		pidx = 0;
		for (i = 0; i < ndistinct; i++)
		{
			int			s = runstart[i];
			int			k;

			entries[i].off = off;
			entries[i].len = raw[s].len;
			entries[i].tf = runlen[i];
			entries[i].posoff = pidx;
			memcpy(lexemes + off, raw[s].term, raw[s].len);
			off += raw[s].len;
			for (k = 0; k < runlen[i]; k++)
				positions[pidx++] = raw[s + k].pos;
		}

		return doc;
	}
}

PG_FUNCTION_INFO_V1(to_wdoc);

Datum
to_wdoc(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_PP(0);
	WeaveDoc		doc;

	doc = weave_analyze_text(VARDATA_ANY(in), VARSIZE_ANY_EXHDR(in));

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_WDOC(doc);
}
