/*-------------------------------------------------------------------------
 *
 * pg_weave_doc.c
 *		Input/output and support functions for the wdoc type.
 *
 * wdoc input accepts either a canonical rendering (round-trips wdoc_out
 * exactly, including positions) or -- for the ergonomic 'raw text'::wdoc
 * cast -- an arbitrary string that is analyzed by the stage-1 tokenizer.
 *
 * Output renders the distinct terms with their term frequencies, and, when the
 * document carries token positions, each term's positions, in a stable,
 * human-readable form.  The canonical grammar is:
 *
 *	  doc     := token ( ' ' token )*
 *	  token   := qterm ':' tf [ '@' pos ( ',' pos )* ]
 *	  qterm   := '\'' ( any char, with '\'' and '\\' backslash-escaped )* '\''
 *	  tf, pos := unsigned decimal integer
 *
 * Examples:
 *
 *	  position-free:  'brown':1 'fox':2 'quick':1
 *	  with positions: 'brown':1@3 'fox':2@2,5 'quick':1@1
 *
 * The '@positions' suffix appears only when the document has stored positions;
 * a term's position count equals its tf.  This mirrors tsvector's rendering
 * closely enough to be familiar while making the (BM25-relevant) term frequency
 * explicit, which tsvector's output hides.
 *
 * wdoc_in re-parses this grammar back to a byte-identical WeaveDoc, so
 * wdoc_in(wdoc_out(x)) = x for every x.  Any input that is not a complete
 * sequence of canonical tokens is treated as raw text and analyzed instead, so
 * 'the quick brown fox'::wdoc keeps working.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_doc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "weave/docvalid.h"
#include "catalog/pg_collation.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "regex/regex.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(wdoc_in);
PG_FUNCTION_INFO_V1(wdoc_out);
PG_FUNCTION_INFO_V1(wdoc_recv);
PG_FUNCTION_INFO_V1(wdoc_send);
PG_FUNCTION_INFO_V1(wdoc_length);

/*
 * weave_doc_is_valid -- structural self-consistency check for an WeaveDoc whose
 * bytes came from an untrusted-at-read-time source (a pending index page, a
 * detoasted stored column).  The readers (pending-list flush and scan) cast
 * raw page bytes to WeaveDoc and then walk entries[].len / .off / .tf / .posoff;
 * a torn page, a stale-format image, or any producing bug would otherwise turn
 * a bad length into a wild memcpy in add_posting (a _FORTIFY_SOURCE abort) or a
 * bad offset into an out-of-bounds positions/lexeme read in weave_doc_matches.
 * This confirms every derived offset stays within the varlena's own VARSIZE
 * before any of them is trusted.  `sz` is the total byte length available at
 * `doc` (VARSIZE for a detoasted datum, or the pending item's doclen).
 *
 * Returns true iff the header, the entries[] array, every term's lexeme slice,
 * and (when positions are present) the whole positions[] region and every
 * term's tf/posoff run fit inside `sz`, and the per-term tf counts sum to the
 * number of positions the layout has room for.  Cheap: one pass over entries.
 */
bool
weave_doc_is_valid(const WeaveDocData *doc, Size sz)
{
	/*
	 * The validator body lives in pg_weave_docvalid.h as pure standalone C so the
	 * fuzz/corruption harness (test/fuzz/) exercises this exact logic -- single
	 * source of truth.  Reading VARSIZE stays here, in its proper backend
	 * context; weave_doc_check() takes the declared size as a parameter.  A NULL
	 * doc has no readable VARSIZE, so short-circuit it (weave_doc_check also
	 * rejects NULL, but must not dereference it for VARSIZE first).
	 */
	if (doc == NULL || sz < WEAVE_DOC_HDRSIZE)
		return false;

	/* These asserts guard the WeaveDvDocData/WeaveDvTermEntry mirror in
	 * pg_weave_docvalid.h against drifting from the real pg_weave.h structs. */
	StaticAssertStmt(sizeof(WeaveDvDocData) == WEAVE_DOC_HDRSIZE,
					 "WeaveDvDocData layout drifted from WeaveDocData header");
	StaticAssertStmt(sizeof(WeaveDvTermEntry) == sizeof(WeaveTermEntry),
					 "WeaveDvTermEntry layout drifted from WeaveTermEntry");
	StaticAssertStmt(WEAVE_DV_HDRSIZE == WEAVE_DOC_HDRSIZE,
					 "WEAVE_DV_HDRSIZE drifted from WEAVE_DOC_HDRSIZE");
	StaticAssertStmt(WEAVE_DV_VERSION == WEAVE_DOC_VERSION,
					 "WEAVE_DV_VERSION drifted from WEAVE_DOC_VERSION");
	StaticAssertStmt(WEAVE_DV_FLAG_POSITIONS == WEAVE_DOCF_POSITIONS,
					 "WEAVE_DV_FLAG_POSITIONS drifted from WEAVE_DOCF_POSITIONS");

	return weave_doc_check(doc, sz, (uint32) VARSIZE(doc)) != 0;
}


/*
 * weave_doc_build -- assemble an WeaveDoc from parallel term arrays.
 *
 * terms[i]/lens[i] are the (already case-folded) term texts; tfs[i] the term
 * frequencies; when has_pos is true, positions[] holds the concatenated per-term
 * positions (tfs[i] of them for term i, in the order the terms appear) and the
 * result carries WEAVE_DOCF_POSITIONS.  Validates the on-disk invariants at this
 * trust boundary: terms strictly ascending and distinct, tf >= 1, and (with
 * positions) each term's positions strictly ascending.  errctx names the caller
 * for error messages ("wdoc" for text input, "binary wdoc" for recv).
 *
 * Mirrors weave_analyze_text's second pass and wdoc_recv's old assembly so the
 * layout is produced in exactly one style.
 */
WeaveDoc
weave_doc_build(uint32 nterms, char **terms, const int *lens, const uint32 *tfs,
			  bool has_pos, const uint32 *positions, const char *errctx)
{
	Size		lexbytes = 0;
	uint64		doclen = 0;
	uint64		npos = 0;
	Size		posbase;
	Size		total;
	WeaveDoc		doc;
	WeaveTermEntry *entries;
	char	   *lexemes;
	uint32		off = 0;
	uint32		pidx = 0;
	uint32		i;

	for (i = 0; i < nterms; i++)
	{
		if (lens[i] < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid %s: negative term length", errctx)));
		if (tfs[i] < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid %s: term frequency must be at least 1", errctx)));
		if (i > 0)
		{
			int			min = Min(lens[i - 1], lens[i]);
			int			c = memcmp(terms[i - 1], terms[i], min);

			if (c > 0 || (c == 0 && lens[i - 1] >= lens[i]))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid %s: terms must be sorted and distinct", errctx)));
		}
		lexbytes += lens[i];
		doclen += tfs[i];
		npos += tfs[i];
	}

	/* validate positions strictly ascending within each term */
	if (has_pos)
	{
		uint64		p = 0;

		for (i = 0; i < nterms; i++)
		{
			uint32		k;

			for (k = 0; k < tfs[i]; k++, p++)
			{
				if (k > 0 && WEAVE_POS_ORD(positions[p]) <= WEAVE_POS_ORD(positions[p - 1]))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("invalid %s: positions must be ascending within a term", errctx)));
			}
		}
	}

	posbase = MAXALIGN(WEAVE_DOC_HDRSIZE +
					   (Size) nterms * sizeof(WeaveTermEntry) + lexbytes);
	total = has_pos ? posbase + (Size) npos * sizeof(uint32) : posbase;
	/* An wdoc is a varlena (max 1GB via VARSIZE); a single document whose
	 * assembled form would exceed that cannot be represented.  Fail with a clear
	 * error rather than letting palloc0 throw the opaque "invalid memory alloc
	 * request size" (or SET_VARSIZE silently truncate). */
	if (total > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("wdoc document is too large"),
				 errdetail("An wdoc value is limited to %zu bytes; this document needs %zu.",
						   (Size) MaxAllocSize, total)));
	doc = (WeaveDoc) palloc0(total);
	SET_VARSIZE(doc, total);
	doc->version = WEAVE_DOC_VERSION;
	doc->flags = has_pos ? WEAVE_DOCF_POSITIONS : 0;
	doc->nterms = nterms;
	doc->doclen = (uint32) doclen;
	doc->lexbytes = lexbytes;

	entries = WEAVE_DOC_ENTRIES(doc);
	lexemes = WEAVE_DOC_LEXEMES(doc);
	for (i = 0; i < nterms; i++)
	{
		entries[i].off = off;
		entries[i].len = lens[i];
		entries[i].tf = tfs[i];
		entries[i].posoff = pidx;
		memcpy(lexemes + off, terms[i], lens[i]);
		off += lens[i];
		pidx += tfs[i];
	}
	if (has_pos)
	{
		uint32	   *dst = WEAVE_DOC_POSITIONS(doc);
		uint32		j;

		memcpy(dst, positions, (Size) npos * sizeof(uint32));
		/* record whether any position carries a non-D weight label (v4) */
		for (j = 0; j < (uint32) npos; j++)
			if (WEAVE_POS_LABEL(dst[j]) != 0)
			{
				doc->flags |= WEAVE_DOCF_WEIGHTS;
				break;
			}
	}
	return doc;
}

/*
 * Try to parse `in` as the canonical grammar (see the file header).  Returns
 * the reconstructed WeaveDoc on success, or NULL if `in` is not a complete
 * sequence of canonical tokens (caller then treats it as raw text).  A string
 * that clearly *looks* canonical (starts with a quoted term followed by ':')
 * but is malformed raises an error rather than silently falling back, so a
 * corrupt dump is rejected loudly at this trust boundary.
 */
static WeaveDoc
weave_doc_parse_canonical(const char *in)
{
	const char *p = in;
	uint32		cap = 4;
	uint32		nterms = 0;
	char	  **terms = (char **) palloc(cap * sizeof(char *));
	int		   *lens = (int *) palloc(cap * sizeof(int));
	uint32	   *tfs = (uint32 *) palloc(cap * sizeof(uint32));
	StringInfoData term;
	uint32		poscap = 8;
	uint32		npos = 0;
	uint32	   *positions = (uint32 *) palloc(poscap * sizeof(uint32));
	bool		has_pos = false;
	bool		seen_any = false;
	WeaveDoc		result;

	initStringInfo(&term);

	/* leading whitespace: an all-blank/empty string is the empty doc only if it
	 * is truly empty; otherwise let raw analysis handle it. */
	if (*p == '\0')
		return NULL;			/* empty string -> raw (analyzes to 0 terms) */

	for (;;)
	{
		uint32		tf;
		const char *save;

		/* skip a single separating space between tokens (and any run) */
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;

		/* a token must begin with a quote to be canonical */
		if (*p != '\'')
		{
			if (!seen_any)
				return NULL;	/* not canonical at all -> raw text */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed wdoc literal"),
					 errdetail("expected a quoted term at \"%s\".", p)));
		}

		/* parse quoted term, unescaping \' and \\ */
		save = p;
		p++;					/* opening quote */
		resetStringInfo(&term);
		for (;;)
		{
			if (*p == '\0')
			{
				if (!seen_any)
					return NULL;	/* unterminated -> treat whole as raw */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed wdoc literal"),
						 errdetail("unterminated quoted term at \"%s\".", save)));
			}
			if (*p == '\\')
			{
				if (p[1] == '\0')
				{
					if (!seen_any)
						return NULL;
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed wdoc literal"),
							 errdetail("trailing backslash in quoted term.")));
				}
				appendStringInfoChar(&term, p[1]);
				p += 2;
				continue;
			}
			if (*p == '\'')
			{
				p++;			/* closing quote */
				break;
			}
			appendStringInfoChar(&term, *p);
			p++;
		}

		/* a bare quoted lexeme with no ':' tf is not canonical: only bail to raw
		 * before the first token; after that it is a hard error. */
		if (*p != ':')
		{
			if (!seen_any)
				return NULL;
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed wdoc literal"),
					 errdetail("expected ':' after term at \"%s\".", p)));
		}
		p++;					/* colon */

		/* term frequency: one or more digits, no sign */
		if (*p < '0' || *p > '9')
		{
			if (!seen_any)
				return NULL;
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed wdoc literal"),
					 errdetail("expected term frequency after ':' at \"%s\".", p)));
		}
		tf = 0;
		while (*p >= '0' && *p <= '9')
		{
			uint64		next = (uint64) tf * 10 + (*p - '0');

			if (next > 0x7fffffff)	/* keep well below uint32 overflow / sane cap */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("wdoc term frequency out of range")));
			tf = (uint32) next;
			p++;
		}

		/* grow term arrays if needed */
		if (nterms == cap)
		{
			cap *= 2;
			terms = (char **) repalloc(terms, cap * sizeof(char *));
			lens = (int *) repalloc(lens, cap * sizeof(int));
			tfs = (uint32 *) repalloc(tfs, cap * sizeof(uint32));
		}
		terms[nterms] = (char *) palloc(Max(term.len, 1));	/* alloc-ok: one term's bytes */
		memcpy(terms[nterms], term.data, term.len);
		lens[nterms] = term.len;
		tfs[nterms] = tf;

		/* optional '@' positions, must supply exactly tf of them */
		if (*p == '@')
		{
			uint32		k;

			has_pos = true;
			p++;
			for (k = 0; k < tf; k++)
			{
				uint32		v;

				if (k > 0)
				{
					if (*p != ',')
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed wdoc literal"),
								 errdetail("expected %u positions for tf=%u.", tf, tf)));
					p++;
				}
				if (*p < '0' || *p > '9')
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed wdoc literal"),
							 errdetail("expected a position at \"%s\".", p)));
				v = 0;
				while (*p >= '0' && *p <= '9')
				{
					uint64		next = (uint64) v * 10 + (*p - '0');

					if (next > 0x3fffffff)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("wdoc position out of range")));
					v = (uint32) next;
					p++;
				}
				/* optional weight-label char A/B/C/D after the position (v4);
				 * absent = D (unlabeled), matching v3 rendering */
				if (*p == 'A' || *p == 'B' || *p == 'C' || *p == 'D')
				{
					v = WEAVE_POS_MAKE(v, WEAVE_WEIGHT_LABEL(*p));
					p++;
				}
				if (npos == poscap)
				{
					poscap *= 2;
					positions = (uint32 *) repalloc(positions, poscap * sizeof(uint32));
				}
				positions[npos++] = v;
			}
			/* a trailing ',' or extra digits means more than tf positions */
			if (*p == ',')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed wdoc literal"),
						 errdetail("more than tf=%u positions for a term.", tf)));
		}
		else if (has_pos)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed wdoc literal"),
					 errdetail("positions must be given for every term or none.")));

		nterms++;
		seen_any = true;

		/* after a token, only a space or end-of-string is legal */
		if (*p != ' ' && *p != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed wdoc literal"),
					 errdetail("unexpected trailing text at \"%s\".", p)));
	}

	if (!seen_any)
		return NULL;

	result = weave_doc_build(nterms, terms, lens, tfs, has_pos, positions, "wdoc");
	pfree(term.data);
	return result;
}

Datum
wdoc_in(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);
	WeaveDoc		doc = weave_doc_parse_canonical(in);

	/* not canonical -> the ergonomic 'raw text'::wdoc cast: analyze it */
	if (doc == NULL)
		doc = weave_analyze_text(in, strlen(in));

	PG_RETURN_WDOC(doc);
}

static void
append_quoted_term(StringInfo buf, const char *term, int len)
{
	int			i;

	appendStringInfoChar(buf, '\'');
	for (i = 0; i < len; i++)
	{
		char		c = term[i];

		if (c == '\'' || c == '\\')
			appendStringInfoChar(buf, '\\');
		appendStringInfoChar(buf, c);
	}
	appendStringInfoChar(buf, '\'');
}

Datum
wdoc_out(PG_FUNCTION_ARGS)
{
	WeaveDoc		doc = PG_GETARG_WDOC(0);
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	StringInfoData buf;
	uint32		i;

	initStringInfo(&buf);
	for (i = 0; i < doc->nterms; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ' ');
		append_quoted_term(&buf, WEAVE_DOC_TERMTEXT(doc, &entries[i]),
						   entries[i].len);
		appendStringInfo(&buf, ":%u", entries[i].tf);
		if (WEAVE_DOC_HAS_POS(doc))
		{
			const uint32 *pos = WEAVE_DOC_TERMPOS(doc, &entries[i]);
			uint32		k;
			static const char lblch[4] = {'D', 'C', 'B', 'A'};

			for (k = 0; k < entries[i].tf; k++)
			{
				uint8		lbl = WEAVE_POS_LABEL(pos[k]);

				appendStringInfo(&buf, k == 0 ? "@%u" : ",%u", WEAVE_POS_ORD(pos[k]));
				/* append the weight-label char (A/B/C) when present; label D
				 * (unlabeled) renders as before so v3 output is byte-identical */
				if (lbl != 0)
					appendStringInfoChar(&buf, lblch[lbl]);
			}
		}
	}

	PG_FREE_IF_COPY(doc, 0);
	PG_RETURN_CSTRING(buf.data);
}

/*
 * Binary receive/send.  The wire format is version-tagged and
 * architecture-neutral (fixed-width big-endian integers via pq_*), so it is
 * safe across replication and pg_dump -Fc.
 */
Datum
wdoc_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint16		version;
	uint32		nterms;
	uint32		doclen;
	uint8		has_pos;
	WeaveDoc		doc;
	uint32		i;
	char	  **terms;
	int		   *lens;
	uint32	   *tfs;
	uint32	   *positions = NULL;
	uint64		npos = 0;
	uint32		pidx = 0;

	version = (uint16) pq_getmsgint(buf, 2);
	/*
	 * Accept the current wire version (3, carries positions) and the previous
	 * one (2, position-free) so a binary dump (pg_dump -Fc) taken under an
	 * older pg_weave restores into this version.  A v2 message has no has_pos
	 * byte and no positions region.
	 */
	/*
	 * Accept the current wire version (4: positions + weight labels), v3
	 * (positions, no labels -- byte-identical to a v4 all-D document), and v2
	 * (position-free) so a binary dump (pg_dump -Fc) taken under an older pg_weave
	 * restores into this version.  A v2 message has no has_pos byte and no
	 * positions region; a v3 message's positions have no label bits (all D).
	 */
	if (version != WEAVE_DOC_VERSION && version != 3 && version != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("unsupported wdoc version number %u", version)));

	nterms = (uint32) pq_getmsgint(buf, 4);
	doclen = (uint32) pq_getmsgint(buf, 4);
	has_pos = (version >= 3) ? (uint8) pq_getmsgint(buf, 1) : 0;
	(void) doclen;				/* recomputed from tf in weave_doc_build */

	/*
	 * Guard against a hostile/corrupt binary message: each term contributes at
	 * least a 4-byte length + 4-byte tf, so nterms cannot exceed the remaining
	 * bytes / 8.  Rejects absurd counts before they reach palloc (overflow /
	 * OOM at a trust boundary).
	 */
	if (nterms > (uint32) (buf->len - buf->cursor) / 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid wdoc: term count %u exceeds message size", nterms)));

	terms = (char **) palloc(Max(nterms, 1) * sizeof(char *));	/* alloc-ok: one document's term count, capped above by nterms > (buf->len-cursor)/8 reject */
	lens = (int *) palloc(Max(nterms, 1) * sizeof(int));	/* alloc-ok: see terms[] above */
	tfs = (uint32 *) palloc(Max(nterms, 1) * sizeof(uint32));	/* alloc-ok: see terms[] above */

	for (i = 0; i < nterms; i++)
	{
		const char *t;

		lens[i] = pq_getmsgint(buf, 4);
		tfs[i] = (uint32) pq_getmsgint(buf, 4);
		if (lens[i] < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid wdoc term length")));
		t = pq_getmsgbytes(buf, lens[i]);
		terms[i] = (char *) palloc(Max(lens[i], 1));
		memcpy(terms[i], t, lens[i]);
		npos += tfs[i];
	}

	/*
	 * Positions region, when present.  Bound the total count the same way
	 * (each position is 4 bytes on the wire) before palloc, and read exactly
	 * sum(tf) of them, so a corrupt message can neither OOB-read nor OOM.
	 */
	if (has_pos)
	{
		if (npos > (uint64) (buf->len - buf->cursor) / 4)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid wdoc: position count exceeds message size")));
		positions = (uint32 *) palloc(Max((Size) npos, 1) * sizeof(uint32));	/* alloc-ok: one document's positions, bounded by the wdoc size */
		for (i = 0; i < nterms; i++)
		{
			uint32		k;

			for (k = 0; k < tfs[i]; k++)
				positions[pidx++] = (uint32) pq_getmsgint(buf, 4);
		}
	}

	/* weave_doc_build enforces sorted/distinct terms, tf >= 1 and ascending
	 * positions -- the same guards as the text input path. */
	doc = weave_doc_build(nterms, terms, lens, tfs, has_pos != 0, positions,
						"binary wdoc");

	PG_RETURN_WDOC(doc);
}

Datum
wdoc_send(PG_FUNCTION_ARGS)
{
	WeaveDoc		doc = PG_GETARG_WDOC(0);
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	StringInfoData buf;
	bool		has_pos = WEAVE_DOC_HAS_POS(doc);
	uint32		i;

	pq_begintypsend(&buf);
	pq_sendint16(&buf, doc->version);
	pq_sendint32(&buf, doc->nterms);
	pq_sendint32(&buf, doc->doclen);
	pq_sendint8(&buf, has_pos ? 1 : 0);
	for (i = 0; i < doc->nterms; i++)
	{
		pq_sendint32(&buf, entries[i].len);
		pq_sendint32(&buf, entries[i].tf);
		pq_sendbytes(&buf, WEAVE_DOC_TERMTEXT(doc, &entries[i]), entries[i].len);
	}
	if (has_pos)
	{
		for (i = 0; i < doc->nterms; i++)
		{
			const uint32 *pos = WEAVE_DOC_TERMPOS(doc, &entries[i]);
			uint32		k;

			for (k = 0; k < entries[i].tf; k++)
				pq_sendint32(&buf, pos[k]);
		}
	}

	PG_FREE_IF_COPY(doc, 0);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* wdoc_length(wdoc) -> int : total token count (doclen). Useful for BM25
 * length normalization later and handy for testing now. */
Datum
wdoc_length(PG_FUNCTION_ARGS)
{
	WeaveDoc		doc = PG_GETARG_WDOC(0);
	uint32		doclen = doc->doclen;

	PG_FREE_IF_COPY(doc, 0);
	PG_RETURN_INT32((int32) doclen);
}

/*
 * weave_doc_lookup -- binary search for a term in a doc.
 * Returns the matching entry, or NULL.  Shared by the match evaluator.
 */
WeaveTermEntry *
weave_doc_lookup(WeaveDoc doc, const char *term, int termlen)
{
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	int			lo = 0;
	int			hi = (int) doc->nterms - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		const char *mterm = WEAVE_DOC_TERMTEXT(doc, &entries[mid]);
		int			mlen = entries[mid].len;
		int			min = Min(mlen, termlen);
		int			c = memcmp(mterm, term, min);

		if (c == 0)
			c = mlen - termlen;

		if (c == 0)
			return &entries[mid];
		else if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/*
 * weave_doc_has_prefix -- does any term in the doc start with the given prefix?
 * Terms are sorted, so binary-search the lower bound for the prefix, then
 * check whether the term there begins with it.
 */
bool
weave_doc_has_prefix(WeaveDoc doc, const char *prefix, int prefixlen)
{
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	int			lo = 0;
	int			hi = (int) doc->nterms;

	if (prefixlen == 0)
		return doc->nterms > 0;

	/* lower_bound: first entry whose term >= prefix */
	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;
		const char *mterm = WEAVE_DOC_TERMTEXT(doc, &entries[mid]);
		int			mlen = entries[mid].len;
		int			min = Min(mlen, prefixlen);
		int			c = memcmp(mterm, prefix, min);

		if (c == 0)
			c = mlen - prefixlen;	/* shorter sorts before */
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}

	if (lo < (int) doc->nterms)
	{
		const char *mterm = WEAVE_DOC_TERMTEXT(doc, &entries[lo]);
		int			mlen = entries[lo].len;

		if (mlen >= prefixlen && memcmp(mterm, prefix, prefixlen) == 0)
			return true;
	}
	return false;
}

/*
 * weave_doc_has_fuzzy -- does any doc term lie within edit distance k of `term`?
 * Uses core's varstr_levenshtein_less_equal (bounded, so cheap for small k),
 * with two pre-filters to avoid the distance computation on most candidates:
 * a length filter (||cand|-|q|| <= k) and, when the query has more than k
 * trigrams (pigeonhole), a trigram-overlap filter.
 */
bool
weave_doc_has_fuzzy(WeaveDoc doc, const char *term, int termlen, int k)
{
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	uint32		i;
	uint32		qtrg[WEAVE_MAX_TRIGRAMS];
	int			nqtrg;
	bool		use_trgm;

	/*
	 * Trigram pre-filter: a term within k edits of the query must share a
	 * trigram with it, provided the query has more than k trigrams (pigeonhole).
	 * When it does not, the filter is unsound, so we skip it and scan fully --
	 * results stay correct, only speed varies.
	 */
	nqtrg = weave_trigrams(term, termlen, qtrg, WEAVE_MAX_TRIGRAMS);
	use_trgm = (nqtrg > k);

	for (i = 0; i < doc->nterms; i++)
	{
		const char *cand = WEAVE_DOC_TERMTEXT(doc, &entries[i]);
		int			candlen = entries[i].len;
		int			d;

		/* length difference alone can exceed k -> skip without computing */
		if (abs(candlen - termlen) > k)
			continue;

		/* trigram pre-filter: skip candidates that share no trigram */
		if (use_trgm)
		{
			uint32		ctrg[WEAVE_MAX_TRIGRAMS];
			int			nctrg = weave_trigrams(cand, candlen, ctrg, WEAVE_MAX_TRIGRAMS);

			if (!weave_trigrams_overlap(qtrg, nqtrg, ctrg, nctrg))
				continue;
		}

		d = varstr_levenshtein_less_equal(term, termlen, cand, candlen,
										  1, 1, 1, k, true);
		if (d <= k)
			return true;
	}
	return false;
}

/*
 * weave_doc_has_regex -- does any doc term match the regular expression?
 * Uses core's cached regex engine (RE_compile_and_execute).  The regex is
 * matched against each stored (folded) term.
 */
bool
weave_doc_has_regex(WeaveDoc doc, const char *re, int relen)
{
	WeaveTermEntry *entries = WEAVE_DOC_ENTRIES(doc);
	text	   *repat = cstring_to_text_with_len(re, relen);
	uint32		i;
	bool		found = false;

	for (i = 0; i < doc->nterms; i++)
	{
		const char *cand = WEAVE_DOC_TERMTEXT(doc, &entries[i]);
		int			candlen = entries[i].len;

		if (RE_compile_and_execute(repat, (char *) cand, candlen,
								   REG_ADVANCED, C_COLLATION_OID,
								   0, NULL))
		{
			found = true;
			break;
		}
	}
	pfree(repat);
	return found;
}

PG_FUNCTION_INFO_V1(setwdocweight);

/*
 * setwdocweight(wdoc, "char") -> wdoc
 * Relabel every token position of the document with the weight label
 * A/B/C/D (mirrors setweight(tsvector,"char")).  A position's ordinal is
 * unchanged; only its label bits are replaced.  A position-free document is
 * returned unchanged (nothing to label; matches tsvector, where weights live
 * on positions).
 */
Datum
setwdocweight(PG_FUNCTION_ARGS)
{
	WeaveDoc		in = PG_GETARG_WDOC(0);
	char		w = PG_GETARG_CHAR(1);
	uint8		lbl;
	Size		sz;
	WeaveDoc		out;

	if (w != 'A' && w != 'B' && w != 'C' && w != 'D' &&
		w != 'a' && w != 'b' && w != 'c' && w != 'd')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("weight must be one of A, B, C, D")));
	lbl = WEAVE_WEIGHT_LABEL(w);

	sz = VARSIZE(in);
	out = (WeaveDoc) palloc(sz);
	memcpy(out, in, sz);
	if (WEAVE_DOC_HAS_POS(out))
	{
		uint32	   *pos = WEAVE_DOC_POSITIONS(out);
		uint32		total = 0;
		uint32		i;

		for (i = 0; i < out->nterms; i++)
			total += WEAVE_DOC_ENTRIES(out)[i].tf;
		for (i = 0; i < total; i++)
			pos[i] = WEAVE_POS_MAKE(WEAVE_POS_ORD(pos[i]), lbl);
		if (lbl != 0)
			out->flags |= WEAVE_DOCF_WEIGHTS;
		else
			out->flags &= ~WEAVE_DOCF_WEIGHTS;
	}
	out->version = WEAVE_DOC_VERSION;
	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_WDOC(out);
}

PG_FUNCTION_INFO_V1(wdoc_concat);

/*
 * wdoc || wdoc -> wdoc
 * Concatenate two documents into one, as though their source texts were joined
 * (right after left).  The right operand's token ordinals are re-based by the
 * left operand's doclen so positions stay globally ascending; each side keeps
 * its own weight labels.  Terms are merged (a term in both sides sums its tf
 * and interleaves positions).  Used to build a multi-field document, e.g.
 *   to_wdoc('english', subject, 'A') || to_wdoc('english', body, 'C')
 * so a query term can restrict to a field zone (term:A).  If either side lacks
 * positions the result is position-free (labels require positions).
 */
Datum
wdoc_concat(PG_FUNCTION_ARGS)
{
	WeaveDoc		a = PG_GETARG_WDOC(0);
	WeaveDoc		b = PG_GETARG_WDOC(1);
	bool		has_pos = WEAVE_DOC_HAS_POS(a) && WEAVE_DOC_HAS_POS(b);
	uint32		abase = a->doclen;		/* right ordinals shift past the left doc */
	uint32		na = a->nterms,
				nb = b->nterms;
	uint32		ntot = na + nb;
	char	  **terms;
	int		   *lens;
	uint32	   *tfs;
	uint32	   *positions = NULL;
	uint32		pc = 0;
	WeaveDoc		out;

	/* Both operands' entries are already sorted+distinct by term text.  Merge
	 * them (sum tf and interleave positions for a shared term) so the arrays fed
	 * to weave_doc_build are strictly ascending + distinct, as it requires. */
	terms = (char **) palloc(sizeof(char *) * Max(ntot, 1));
	lens = (int *) palloc(sizeof(int) * Max(ntot, 1));
	tfs = (uint32 *) palloc(sizeof(uint32) * Max(ntot, 1));
	if (has_pos)
		positions = (uint32 *) palloc(sizeof(uint32) *
									  Max(a->doclen + b->doclen, 1u));
	{
		uint32		ia = 0,
					ib = 0,
					nout = 0;
		WeaveTermEntry *ea = WEAVE_DOC_ENTRIES(a);
		WeaveTermEntry *eb = WEAVE_DOC_ENTRIES(b);

#define ATERM(x) WEAVE_DOC_TERMTEXT(a, &ea[x])
#define BTERM(x) WEAVE_DOC_TERMTEXT(b, &eb[x])
		while (ia < na || ib < nb)
		{
			int			cmp;

			if (ia >= na)
				cmp = 1;
			else if (ib >= nb)
				cmp = -1;
			else
			{
				int			mn = Min(ea[ia].len, eb[ib].len);

				cmp = memcmp(ATERM(ia), BTERM(ib), mn);
				if (cmp == 0)
					cmp = (int) ea[ia].len - (int) eb[ib].len;
			}
			if (cmp < 0)			/* term only in A */
			{
				const uint32 *ap = WEAVE_DOC_TERMPOS(a, &ea[ia]);
				uint32		k;

				terms[nout] = ATERM(ia); lens[nout] = (int) ea[ia].len;
				tfs[nout] = ea[ia].tf;
				if (has_pos)
					for (k = 0; k < ea[ia].tf; k++)
						positions[pc++] = ap[k];
				nout++; ia++;
			}
			else if (cmp > 0)		/* term only in B (re-based ordinal) */
			{
				const uint32 *bp = WEAVE_DOC_TERMPOS(b, &eb[ib]);
				uint32		k;

				terms[nout] = BTERM(ib); lens[nout] = (int) eb[ib].len;
				tfs[nout] = eb[ib].tf;
				if (has_pos)
					for (k = 0; k < eb[ib].tf; k++)
						positions[pc++] = WEAVE_POS_MAKE(WEAVE_POS_ORD(bp[k]) + abase,
													   WEAVE_POS_LABEL(bp[k]));
				nout++; ib++;
			}
			else					/* term in BOTH: sum tf, positions A then re-based B */
			{
				const uint32 *ap = WEAVE_DOC_TERMPOS(a, &ea[ia]);
				const uint32 *bp = WEAVE_DOC_TERMPOS(b, &eb[ib]);
				uint32		k;

				terms[nout] = ATERM(ia); lens[nout] = (int) ea[ia].len;
				tfs[nout] = ea[ia].tf + eb[ib].tf;
				if (has_pos)
				{
					for (k = 0; k < ea[ia].tf; k++)
						positions[pc++] = ap[k];
					for (k = 0; k < eb[ib].tf; k++)
						positions[pc++] = WEAVE_POS_MAKE(WEAVE_POS_ORD(bp[k]) + abase,
													   WEAVE_POS_LABEL(bp[k]));
				}
				nout++; ia++; ib++;
			}
		}
#undef ATERM
#undef BTERM
		ntot = nout;
	}

	out = weave_doc_build(ntot, terms, lens, tfs, has_pos, positions, "wdoc ||");
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_WDOC(out);
}
