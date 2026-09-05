/*-------------------------------------------------------------------------
 *
 * pg_weave.h
 *		Full-text search with BM25 ranking for PostgreSQL.
 *
 * pg_weave provides the analyzed document type (wdoc) and the parsed query type
 * (wquery) with @@@ match evaluation, plus a dedicated weave index access
 * method (segmented inverted index, block-max WAND ranking) that answers @@@
 * and the <=> ordering operator; matching is also available by sequential scan
 * via @@@, exactly as tsvector/tsquery were first introduced.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_H
#define WEAVE_H

#include "storage/itemptr.h"

#include "postgres.h"

#include "fmgr.h"
#include "varatt.h"

/*
 * wdoc -- an analyzed document.
 *
 * A varlena holding a sorted, de-duplicated array of terms.  Each term entry
 * records its term frequency (tf) and, after the entry array, the term text.
 *
 * Format version 3 optionally stores per-term token positions (needed for
 * phrase and NEAR queries).  When the WEAVE_DOCF_POSITIONS flag is set, a
 * positions region of uint32 values follows the lexemes; each term entry's
 * posoff/tf delimit that term's positions (tf positions starting at posoff,
 * in units of uint32).  Without the flag, posoff is unused and the document is
 * position-free (smaller; phrase/NEAR then fall back to plain term presence).
 *
 * Layout:
 *	  WeaveDocData header
 *	  WeaveTermEntry entries[nterms]		(sorted by term text)
 *	  char lexemes[]					(term texts, in entry order)
 *	  uint32 positions[]				(only if WEAVE_DOCF_POSITIONS)
 */
typedef struct WeaveTermEntry
{
	uint32		off;			/* byte offset of term text within lexemes[] */
	uint32		len;			/* length of term text in bytes */
	uint32		tf;				/* term frequency (also # of positions) */
	uint32		posoff;			/* index of first position in positions[] */
} WeaveTermEntry;

typedef struct WeaveDocData
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint16		version;		/* format version, currently 3 */
	uint16		flags;			/* WEAVE_DOCF_* */
	uint32		nterms;			/* number of distinct terms */
	uint32		doclen;			/* total token count (sum of tf); needed by BM25 */
	uint32		lexbytes;		/* total bytes of lexemes[] (to find positions[]) */
	WeaveTermEntry entries[FLEXIBLE_ARRAY_MEMBER];
} WeaveDocData;

typedef WeaveDocData *WeaveDoc;

#define WEAVE_DOC_VERSION			4	/* wire format; v3 carries positions, v4 adds weight labels in position high bits */
#define WEAVE_DOCF_POSITIONS		0x0001	/* positions[] region is present */
#define WEAVE_DOCF_WEIGHTS		0x0002	/* some position carries a non-D weight label (v4) */
#define WEAVE_DOC_HAS_POS(d)		(((d)->flags & WEAVE_DOCF_POSITIONS) != 0)
#define WEAVE_DOC_HDRSIZE			offsetof(WeaveDocData, entries)
#define WEAVE_DOC_ENTRIES(d)		((d)->entries)
#define WEAVE_DOC_LEXEMES(d) \
	((char *) &(d)->entries[(d)->nterms])
#define WEAVE_DOC_TERMTEXT(d, e)	(WEAVE_DOC_LEXEMES(d) + (e)->off)

/* base of the positions[] region (valid only when WEAVE_DOC_HAS_POS).
 * Computed as doc_base + MAXALIGN(offset), matching how the analyzers lay the
 * region out (posbase = MAXALIGN(total)).  Must NOT be MAXALIGN() of the
 * absolute lexemes-end pointer: a detoasted/heap-read wdoc can sit at a
 * non-MAXALIGN'd address, and MAXALIGN(base+off) != base+MAXALIGN(off) there,
 * which pointed positions[] at garbage and silently degraded phrase/NEAR on
 * every stored (column-resident) wdoc. */
#define WEAVE_DOC_POSITIONS(d) \
	((uint32 *) ((char *) (d) + \
				 MAXALIGN(WEAVE_DOC_HDRSIZE + \
						  (Size) (d)->nterms * sizeof(WeaveTermEntry) + \
						  (d)->lexbytes)))
#define WEAVE_DOC_TERMPOS(d, e)	(WEAVE_DOC_POSITIONS(d) + (e)->posoff)

#define DatumGetWDoc(X)		((WeaveDoc) PG_DETOAST_DATUM(X))
#define PG_GETARG_WDOC(n)		DatumGetWDoc(PG_GETARG_DATUM(n))
#define PG_RETURN_WDOC(x)		PG_RETURN_POINTER(x)

/*
 * wquery -- a parsed boolean query.
 *
 * Stored as a varlena flattened postfix (RPN) list of items.  This mirrors the
 * proven tsquery representation: operands and operators in one array, term
 * text appended after.  Supports AND, OR, NOT, parenthesised grouping, phrase,
 * NEAR, prefix, fuzzy and regex items; field-scope and boosts can be added as
 * new item kinds without breaking v1 data (the version field guards the
 * on-disk format).
 */
typedef enum WeaveQueryItemType
{
	WEAVE_QI_VAL = 1,				/* a term operand */
	WEAVE_QI_OPR					/* a boolean operator */
} WeaveQueryItemType;

typedef enum WeaveQueryOp
{
	WEAVE_OP_NOT = 1,
	WEAVE_OP_AND,
	WEAVE_OP_OR,
	WEAVE_OP_PHRASE				/* two operands adjacent within `distance` */
} WeaveQueryOp;

typedef struct WeaveQueryItem
{
	uint8		type;			/* WeaveQueryItemType */
	uint8		op;				/* WeaveQueryOp, valid when type == WEAVE_QI_OPR */
	uint16		flags;			/* WEAVE_QF_* flags, valid for WEAVE_QI_VAL */
	uint32		distance;		/* max token gap for WEAVE_OP_PHRASE (1 = adjacent);
								 * on a WEAVE_QI_VAL with WEAVE_QF_WEIGHTED, instead holds
								 * the weight-label mask (bit L set => match label L,
								 * L in 0..3 for D,C,B,A) -- a VAL never uses the gap */
	/* for WEAVE_QI_VAL: */
	uint32		termoff;		/* offset of term text within the text region */
	uint32		termlen;		/* length of term text */
} WeaveQueryItem;

#define WEAVE_QF_PREFIX	0x0001	/* term is a prefix match (term*) */
#define WEAVE_QF_FUZZY	0x0002	/* term is a fuzzy match (term~k); k in distance */
#define WEAVE_QF_REGEX	0x0004	/* term text is a regular expression (/re/) */
#define WEAVE_QF_WEIGHTED	0x0008	/* term is weight-restricted (term:ABCD);
								 * the label mask is in `distance` (see above) */

/*
 * Weight labels (field zones), tsvector-compatible ordering D < C < B < A.
 * A label is stored in the TOP TWO BITS of each uint32 token position; the
 * low 30 bits are the 1-based token ordinal.  Label 0 = D (default/unlabeled),
 * so a v3 (label-free) position reads as D and behaves as "unlabeled".
 */
#define WEAVE_POS_LABEL_BITS	2
#define WEAVE_POS_LABEL_SHIFT	30
#define WEAVE_POS_ORD_MASK	0x3FFFFFFFu		/* low 30 bits: token ordinal */
#define WEAVE_POS_LABEL(p)	((uint8) ((p) >> WEAVE_POS_LABEL_SHIFT))	/* 0..3 */
#define WEAVE_POS_ORD(p)		((p) & WEAVE_POS_ORD_MASK)
#define WEAVE_POS_MAKE(ord, lbl)	(((uint32)(lbl) << WEAVE_POS_LABEL_SHIFT) | ((ord) & WEAVE_POS_ORD_MASK))
/* Map a weight char A/B/C/D (any case) to its 0..3 label; D/unknown -> 0. */
#define WEAVE_WEIGHT_LABEL(c) \
	(((c)=='A'||(c)=='a') ? 3 : ((c)=='B'||(c)=='b') ? 2 : ((c)=='C'||(c)=='c') ? 1 : 0)

typedef struct WeaveQueryData
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint16		version;		/* format version, currently 1 */
	uint16		flags;			/* reserved */
	uint32		nitems;			/* number of items in RPN list */
	WeaveQueryItem items[FLEXIBLE_ARRAY_MEMBER];
	/* term texts follow items[] */
} WeaveQueryData;

typedef WeaveQueryData *WeaveQuery;

#define WEAVE_QUERY_VERSION		2	/* v2: WEAVE_QI_VAL may carry a weight mask */
#define WEAVE_QUERY_HDRSIZE		offsetof(WeaveQueryData, items)
#define WEAVE_QUERY_TEXTBASE(q)	((char *) &(q)->items[(q)->nitems])
#define WEAVE_QUERY_ITEMTEXT(q, it) (WEAVE_QUERY_TEXTBASE(q) + (it)->termoff)

#define DatumGetWQuery(X)		((WeaveQuery) PG_DETOAST_DATUM(X))
#define PG_GETARG_WQUERY(n)	DatumGetWQuery(PG_GETARG_DATUM(n))
#define PG_RETURN_WQUERY(x)	PG_RETURN_POINTER(x)

/* pg_weave_analyze.c -- the built-in stage-1 tokenizer */
extern WeaveDoc weave_analyze_text(const char *str, int len);
extern char *fold_token(const char *src, int len, int *outlen);

/* pg_weave_tsanalyze.c -- analyzer reusing an installed TS configuration */
extern WeaveDoc weave_analyze_with_config(Oid cfgId, const char *str, int len, uint8 label);
#ifdef WEAVE_TEST_HOOKS
/* TEST-ONLY (see pg_weave_customscan.c _PG_init): advisory key a scan waits on
 * mid-collect to expose the scan-vs-merge recycle window; 0 = off. */
extern int pg_weave_test_pause_advisory_key;
#endif
extern WeaveDoc weave_doc_build(uint32 nterms, char **terms, const int *lens,
							const uint32 *tfs, bool has_pos,
							const uint32 *positions, const char *errctx);
extern char *weave_normalize_term(Oid cfgId, const char *term, int len, int *outlen);

/* pg_weave_query.c -- parse query text into an wquery */
extern WeaveQuery weave_parse_query(const char *str, int len);
extern WeaveQuery weave_parse_query_cfg(const char *str, int len, Oid cfgId);

/* pg_weave_match.c -- evaluate a parsed query against an analyzed doc */
extern bool weave_doc_matches(WeaveDoc doc, WeaveQuery query);
/* shared phrase adjacency over raw ascending position arrays (single source of
 * truth for the in-memory matcher and the index posting-list phrase eval) */
extern void weave_phrase_step_pos(const uint32 *left, int nleft,
								const uint32 *right, int nright,
								uint32 distance, uint32 *out, int *nout);
/* shared: binary-search a term in a doc; returns entry or NULL */
extern WeaveTermEntry *weave_doc_lookup(WeaveDoc doc, const char *term, int termlen);

/* shared: structural self-consistency check for an WeaveDoc read from an
 * untrusted source (pending page, detoasted column) before its offsets are
 * trusted; sz is the bytes available at doc.  See pg_weave_doc.c. */
extern bool weave_doc_is_valid(const WeaveDocData *doc, Size sz);

/* shared: does any term in the doc start with the given prefix? */
extern bool weave_doc_has_prefix(WeaveDoc doc, const char *prefix, int prefixlen);

/* shared: does any doc term match within edit distance k? (stage 13) */
extern bool weave_doc_has_fuzzy(WeaveDoc doc, const char *term, int termlen, int k);

/* shared: does any doc term match the regular expression? (stage 14) */
extern bool weave_doc_has_regex(WeaveDoc doc, const char *re, int relen);

/* pg_weave_rank.c -- collect distinct query term operands (shared) */
extern int	weave_query_terms(WeaveQuery q, const char ***terms_out, int **lens_out);

/* pg_weave_trgm.c -- trigram pre-filter for fuzzy/regex at scale */
#define WEAVE_MAX_TRIGRAMS 64
extern int	weave_trigrams(const char *s, int len, uint32 *out, int maxout);
extern int	weave_regex_trigrams(const char *re, int relen, uint32 *out, int maxout);
extern bool weave_trigrams_overlap(const uint32 *a, int na,
								 const uint32 *b, int nb);

/* pg_weave_am_scan.c -- count entry point reused by the COUNT-pushdown CustomScan */
extern int64 weave_count_visible_oid(Oid indexoid, WeaveQuery q);
extern int pg_weave_build_collapse_max_mb;
extern int pg_weave_build_mem_ceiling_mb;

#endif							/* WEAVE_H */
