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
 * Fuzzy/regex/prefix channel GUCs and the TRE compile/match deadline
 * helpers.  The files imported from pg_tre (uleven.c, tiling.c, extract.c,
 * pattern_cache.c, trgm_similarity.c) reach them through this header, which
 * is how pg_tre's own pg_tre.h was laid out.
 */
#include "weave/regex.h"

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

/* src/query/trgm.c -- the byte-trigram key space of the trigram weft (fuzzy
 * funnel; regex re-encodes the AST extractor's codepoint triples through it) */
#define WEAVE_MAX_TRIGRAMS 64
extern int	weave_trigrams(const char *s, int len, uint32 *out, int maxout);
extern bool weave_trigrams_overlap(const uint32 *a, int na,
								 const uint32 *b, int nb);

/* src/query/cgram.c -- the pattern side of the corpus trigram channel (Z8).
 * The SAME key space as weave_trigrams() above, deliberately and by
 * construction; see the comment on weave_cgram_hash3(). */
extern void weave_cgram_fold(char *dst, const char *src, int len);
extern uint32 weave_cgram_hash3(const char *folded3);
extern int	weave_cgram_required(const char *pat, int patlen, bool caseinsens,
								 uint32 *out, int maxout);
extern bool weave_cgram_match(const char *val, int vallen,
							  const char *pat, int patlen, bool caseinsens);

/* pg_weave_am_scan.c -- count entry point reused by the COUNT-pushdown CustomScan */
extern int64 weave_count_visible_oid(Oid indexoid, WeaveQuery q);
extern int pg_weave_wand_initial_k;
extern int pg_weave_build_collapse_max_mb;
extern int pg_weave_build_mem_ceiling_mb;
extern double pg_weave_vacuum_tombstone_frac;
extern bool pg_weave_vacuum_vacate;
extern int pg_weave_surf_cache_mb;

/* (C2) CHECKING INSIDE THE FUSED SCORER, reachable from SQL rather than only from a
 * cassert build.  The reason it had to become a GUC is doc/GAPS.md G43: a wrong answer
 * that reproduces on a RELEASE cluster, whose cause this check would name, and which
 * was therefore unreachable exactly where it was needed.  See the long comment at the
 * DefineCustomBoolVariable in src/am/customscan.c. */
extern bool pg_weave_fuse_check_bounds;

/* THE FUSED OBJECTIVE'S PER-KEY NORMALIZER, on by default.  Off restores the raw
 * weighted sum of channel scores, which is what shipped before 2026-09-22 and which
 * loses to an RRF control on every corpus measured because BM25 and a quantized inner
 * product differ by ~33x in scale (doc/GAPS.md G44, doc/specs/FUSED_TOPK.md sect. 8d).
 * It exists so the two objectives can be A/B'd in the product rather than in a study,
 * and so a corpus that the ceiling's looseness pushes the wrong way has a way out. */
extern bool pg_weave_fuse_normalize;

/* Allocator outcome counters (src/am/am.c).  Backend-local; read from SQL via
 * weave_alloc_stats().  See the block comment above weave_new_buffer() for why
 * they are always compiled in and why they are read from SQL rather than logged. */
extern uint64 weave_alloc_lowfree_reuse;
extern uint64 weave_alloc_lowfree_defer;
extern uint64 weave_alloc_lowfree_contended;
extern uint64 weave_alloc_fsm_reuse;
extern uint64 weave_alloc_fsm_defer;
extern uint64 weave_alloc_fsm_contended;
extern uint64 weave_alloc_extend;

/*
 * CHANNEL-MECHANISM COUNTERS (src/am/am.c).  Backend-local, always compiled in,
 * read from SQL via weave_channel_stats().  Same discipline and the same
 * limitations as the allocator counters above, for the same reasons.
 *
 * WHAT QUESTION THEY ANSWER: which MECHANISM served a query leaf.  Not "was the
 * index used" -- pg_stat_user_indexes answers that already and sql/idx_scan_stats.sql
 * asserts it -- but which of the several structures inside one index did the work.
 * A prefix leaf can be served by a dictionary range walk or by a trie enumeration,
 * and those have different costs; a claim about either is unfalsifiable while
 * nothing reports which one ran.  doc/PHASES.md Z4.
 *
 * WHY NOT EXPLAIN, which is what Z4's gate originally asked for.  There is no
 * AM-level EXPLAIN callback on either supported major: `amexplain` is not in
 * PostgreSQL 17's or 18's `IndexAmRoutine` (checked against 18.4's
 * access/amapi.h).  A CustomScan could print something, but only for the one plan
 * shape we generate a CustomScan for -- the count(*) pushdown -- so EXPLAIN would
 * report the channel for a minority of queries and say nothing for the rest.  A
 * counter read from SQL covers every plan shape, including the bitmap scan that is
 * the common @@@ path.
 *
 * WHAT "ANSWERS A QUERY" MEANS, because the first version of these counters got it
 * wrong and asserted the wrong zeros.  Prefix, fuzzy and regex all return CORRECT
 * ROWS today: prefix through a dictionary range walk, fuzzy through a Levenshtein
 * automaton walked over the sorted dictionary with dead-end prefix skipping
 * (weave_fuzzy_terms), regex through core's regex engine walked over the same
 * dictionary, narrowed by the trigram weft when there is one (weave_regex_terms),
 * and any fuzzy term too long for the automaton through the trigram funnel followed
 * by an exact heap recheck.  What they do NOT
 * have is a shuttle implementing include/weave/channel.h with a real bound, which is
 * what lets a channel participate in fused top-k, and that is the sense in which
 * doc/PRODUCTION_READINESS.md counts them as not answering.  These counters measure
 * the first thing, not the second; do not read a nonzero fuzzy_dict as "Z5 is done".
 *
 * THE _surf COLUMNS AND prefix_surf ARE STRUCTURALLY ZERO, with the task that makes
 * each nonzero named beside it above.  A zero from any column still means only "this
 * mechanism served nothing in THIS backend" -- never "nothing happened".
 *
 * A FUZZY QUERY THAT RAN WITH BOTH ITS COLUMNS AT ZERO fell back to a full scan
 * with recheck: weave_trgm_candidates() refuses a term with too few usable trigrams
 * and the caller then scans.  That case is deliberately derivable rather than given
 * its own column, because the funnel's refusal is a property of the term and the
 * pair of zeros says it exactly.
 *
 * REGEX IS DIFFERENT: regex_dict AND regex_trgm ARE NOT ALTERNATIVES.  A regex leaf
 * is always served by the dictionary walk (regex_dict), and the trigram weft, when
 * the index has one and the pattern yields required trigrams, NARROWS which terms
 * that walk asks the engine about (regex_trgm).  So one leaf on a trigrams=on index
 * with a narrowable pattern increments BOTH; the same leaf on a trigrams=off index,
 * or a pattern the narrowing refuses (`\d`, `(?i)`, [[:digit:]] -- see
 * weave_regex_narrowable in src/am/amscan.c), increments regex_dict alone.  A regex
 * query with regex_dict = 0 did not run through the index at all.
 */
extern uint64 weave_chan_lex_term;		/* exact-term leaf via the dictionary */
extern uint64 weave_chan_prefix_dict;	/* term* via the dictionary range walk */
extern uint64 weave_chan_prefix_surf;	/* term* via a SuRF enumeration (Z4) */
extern uint64 weave_chan_fuzzy_dict;	/* term~k via the Levenshtein automaton
										 * walked over the sorted dictionary */
extern uint64 weave_chan_fuzzy_trgm;	/* term~k too long for the automaton:
										 * trigram funnel + exact recheck */
extern uint64 weave_chan_fuzzy_surf;	/* term~k via the trie (Z5 + Z4's cache) */
extern uint64 weave_chan_regex_dict;	/* /re/ SERVED BY THE DICTIONARY WALK: the
										 * pattern compiled once and core's engine
										 * run over the segment's dictionary terms
										 * (all of them, or the weft's survivors);
										 * exact, no recheck.  Once per segment per
										 * leaf, like fuzzy_dict. */
extern uint64 weave_chan_regex_trgm;	/* /re/ whose dictionary walk the trigram
										 * weft NARROWED (the pg_tre CNF was applied
										 * to the candidate ordinals).  Never
										 * without regex_dict; see above. */
extern uint64 weave_chan_regex_surf;	/* /re/ via trigram tiling + trie (Z6) */
extern uint64 weave_chan_vector_scan;	/* a vector shuttle was opened (V8) */
extern uint64 weave_chan_cgram_scan;	/* Z8: times the CORPUS-TRIGRAM ROUTE
										 * ACTUALLY SERVED a `@~` / `@~*`
										 * restriction -- i.e. the pattern yielded
										 * at least one required trigram AND the
										 * route therefore intersected posting
										 * lists instead of returning false.  ONE per
										 * scan, not one per segment or per trigram,
										 * so a test can assert exactly 1.  A zero
										 * after a `@~` query means the pattern FELL
										 * BACK (no literal run of 3+ bytes, or ILIKE
										 * over non-ASCII), which is a correct slow
										 * answer and not a failure.  It does NOT
										 * count how many bolts had a cgram weft: a
										 * bolt without one contributes its whole live
										 * docid set to the candidates, and the route
										 * still ran. */
extern uint64 weave_chan_terms_expanded;	/* vocabulary terms a leaf expanded to */
extern uint64 weave_chan_dict_pages;	/* dictionary pages those expansions read */
extern uint64 weave_chan_surf_loads;	/* whole-image trie loads */
extern uint64 weave_chan_surf_bytes;	/* and their total size */

/*
 * THE FOUR RESIDENT-TRIE COLUMNS (Z4 part 2).  surf_loads/surf_bytes above count
 * WHOLE-IMAGE LOADS and must keep doing exactly that: a cache hit does not
 * increment them, and that separation is the entire measurement -- "the trie is
 * resident" is the claim that consults outnumber loads, which is unreadable if a
 * hit also counts as a load.
 *
 * surf_cache_bytes IS NOT A COUNTER, IT IS A GAUGE: bytes the cache is holding
 * right now.  weave_channel_stats_reset() deliberately leaves it alone, because
 * zeroing it would report 0 resident bytes while the backend still holds the
 * memory -- the same false zero the first draft of these counters shipped (see
 * the note above).  It returns to zero only when the entries are actually freed.
 */
extern uint64 weave_chan_surf_cache_hits;	/* consults served from a resident image */
extern uint64 weave_chan_surf_cache_misses; /* consults that had to load */
extern uint64 weave_chan_surf_cache_evicts; /* images dropped to stay in budget */
extern uint64 weave_chan_surf_cache_bytes;	/* resident image bytes RIGHT NOW */

/*
 * FUSED-SCORER WORK COUNTERS (accumulated in src/am/amscan.c, read from SQL via
 * weave_fuse_stats()).  Backend-local, always compiled in, same discipline and
 * the same "a zero is not evidence unless the query ran in this session" caveat
 * as the two blocks above.
 *
 * WHY THESE ARE A SEPARATE FUNCTION rather than more columns on
 * weave_channel_stats().  That one answers "which mechanism served a leaf" and
 * counts once per leaf or per segment; these count the scorer's inner loop and
 * run to millions.  Mixing a routing flag and a work total in one record invites
 * exactly one mistake -- dividing one by the other -- and the reset cadences
 * differ too: a benchmark brackets a single query with these.
 *
 * WHAT THEY ARE FOR, and it is not diagnostics.  doc/specs/FUSED_TOPK.md sect. 8
 * makes the `score()`-call ratio against RRF the row that decides whether this
 * whole design means anything: "if the score() call ratio is not dramatically
 * lower, stop and fix the bounds before optimizing anything else".  That row was
 * unmeasurable until these existed -- the core has counted into WeaveFuseChan and
 * WeaveFuseState since F1, and nothing carried the numbers out of the scan.
 *
 * TWO WAYS TO MISREAD `scores`, both of which would manufacture a wrong ratio, and
 * both of which is why `passes` and `runs` are reported beside it rather than left
 * for someone to reconstruct:
 *
 * 1. THE WIDENING LADDER RE-RUNS THE WHOLE FUSED PASS.  One SQL query climbs
 *	  candidate widths k, 4k, 16k ... and each rung scores from scratch, so `scores`
 *	  is the total over every rung and is NOT "the score() calls this query's answer
 *	  cost".  A ratio is only honest when it is taken over the same quantity on both
 *	  arms -- total work per query -- or when `passes` is 1 on both.  The merge-race
 *	  retry inside a single rung widens nothing and still re-runs; it increments
 *	  `passes` too, for the same reason.
 * 2. THE FUSED PASS RUNS ONCE PER BOLT.  `runs` is bolts summed over passes, so
 *	  `scores / runs` is per-bolt and `scores / passes` is per-query-pass.  Neither
 *	  is per document.
 *
 * `bounds` exists so the gate row cannot be passed dishonestly: halving score()
 * calls by asking block_max() twice as often moves work rather than removing it,
 * and on the vector channel block_max() reads the block's stored bound.  See the
 * comment on WeaveFuseChan.nbmax in include/weave/fuse.h.
 *
 * A VECTOR CHANNEL IS COUNTED ONCE, at the core's view of it.  Task F8 interposes
 * include/weave/vecdocmap.h between the core and the vector shuttle, and that
 * adapter forwards one score() per core score(); the forwarding calls are
 * deliberately not counted, because counting both would double every vector score
 * and the gate's quantity is what the CORE asked for.
 */
extern uint64 weave_fuse_passes;	/* fused passes started (ladder rungs plus
									 * merge-race retries); see misreading 1 */
extern uint64 weave_fuse_runs;	/* weave_fuse_run() calls: bolts x passes */
extern uint64 weave_fuse_chans; /* channels summed over runs, so a mean channel
								 * count is derivable and a one-channel "fused"
								 * query cannot masquerade as a fused one */
extern uint64 weave_fuse_seeks; /* channel seek() calls */
extern uint64 weave_fuse_scores;	/* channel score() calls -- THE sect. 8 row.
									 * INCLUDES the probe of every REQUIRED channel,
									 * which src/am/fuse.c scores once per pivot to
									 * obtain its veto.  So adding a selective gate
									 * can RAISE this while making the query faster,
									 * and claim 3 must be argued on `pivots`, not
									 * here.  Total work is the right quantity
									 * against RRF, which also evaluates the filter,
									 * and the wrong one for claim 3 --
									 * sql/fuse_pushdown.sql section 7 pins both
									 * halves of that. */
extern uint64 weave_fuse_bounds;	/* channel block_max() calls */
extern uint64 weave_fuse_pivots;	/* candidate positions the core considered */
extern uint64 weave_fuse_blkskip;	/* blocks skipped: bound could not beat theta */
extern uint64 weave_fuse_rqskip;	/* ranges a required channel's intersection
									 * jumped over */
extern uint64 weave_fuse_livedrop;	/* positions dropped as tombstoned, kept apart
									 * from pruning so a vacuum-heavy corpus cannot
									 * flatter the prune rate */
extern uint64 weave_fuse_veto;	/* documents a predicate channel rejected */
extern uint64 weave_fuse_abandon;	/* documents abandoned mid-sum once the
									 * remaining ceiling could not reach theta */

/*
 * THE SPLIT OF `weave_fuse_scores` BY CHANNEL KIND, and it exists because a total is
 * not comparable against an RRF control.  A hybrid fused query's `scores` is lexical
 * BM25 contributions PLUS vector lane-asks PLUS one probe per required gate per
 * pivot, and the control arm's two scans are counted in two different units by two
 * different counters.  Comparing a sum of three things against either one of them is
 * the mistake; these two make the lexical part derivable exactly as
 * `scores - vec_scores - gate_scores`, rather than by an assumption about which
 * channels a query had.  Attributed in src/am/amscan.c from the vector-slot list and
 * from WeaveFuseChan.required -- never from a channel KIND, for the reason (C5)'s
 * note gives.
 */
extern uint64 weave_fuse_vec_scores;	/* of `scores`, the vector adapters' share */
extern uint64 weave_fuse_gate_scores;	/* of `scores`, the required gates' share */

/*
 * CHANNEL WORK COUNTERS (read from SQL via weave_work_stats()).  Backend-local,
 * always compiled in, same caveats as every block above.
 *
 * WHY A SECOND FUNCTION AND NOT MORE COLUMNS ON weave_fuse_stats().  That one
 * measures THE FUSED SCORER'S LOOP.  These measure what a CHANNEL did, on whatever
 * path asked it -- fused, single-channel `ORDER BY`, or a diagnostic SQL function --
 * and the entire point of them is to be comparable ACROSS those paths.  FUSED_TOPK.md
 * sect. 8's gate is a ratio against an RRF control, and a ratio needs both arms
 * counted in one unit by one piece of code.  Putting the control's denominator inside
 * a function called "fuse" would be the wrong name on the right number.
 *
 * `weave_lex_contribs` COUNTS THE SINGLE-CHANNEL WAND PATH ONLY, and that is a
 * deliberate asymmetry with a reason.  The fused lexical channel scores through
 * src/query/lexshuttle.c, which calls weave_wand_cursor_contrib() -- the same
 * wand_contrib_cur() the WAND loops use.  An increment inside wand_contrib_cur()
 * would therefore count the FUSED arm here as well as in weave_fuse_scores, and a
 * quantity counted twice in one arm of a ratio is a made-up ratio in whichever
 * direction happens to flatter. So the increments sit at the three WAND call sites
 * (weave_search_bmw and weave_search_maxscore) and nowhere else.  Units match across
 * the two: one count is one (term, document) BM25 contribution on both arms.
 *
 * THE VECTOR COUNTERS ARE PATH-INDEPENDENT, from one site: vec_shuttle_end() in
 * src/vector/vecshuttle.c, which every vector shuttle passes through -- the fused
 * pass ends its shuttles at src/am/amscan.c, the ORDER BY pass at
 * weave_vec_bolt_pass(), and weave_vec_scan_stats() at its own call.  The per-scan
 * counters it harvests already existed in WeaveVecScanState and were being pfree'd
 * unread on the ORDER BY path.
 *
 * AND `vec_lanes` IS THE UNIT THAT MATTERS FOR A VECTOR CHANNEL, not score() calls.
 * The code-scan kernel scores a 32-lane BLOCK at a time, so a fused scan asking for
 * one lane and a top-k scan asking for a whole block can register the same number of
 * score() calls having done 32x different work.  Quoting a vector channel's score()
 * count as its cost would be the kind of number hard rule 11 says to interrogate
 * before publishing.  Lanes are what the kernel actually touched.
 */
extern uint64 weave_lex_contribs;	/* (term, doc) BM25 contributions computed by the
									 * SINGLE-CHANNEL WAND path; see above for why
									 * the fused path is excluded here */
extern uint64 weave_vecwork_lanes;	/* lanes the code-scan kernel scored, every path */
extern uint64 weave_vecwork_blocks; /* blocks it scored, every path */
extern uint64 weave_vecwork_blk_bound;	/* blocks the (C2) block bound pruned, every
									 * path -- the vector half of the pruning claim,
									 * and the counter that would have made
									 * bench/RESULTS_BOUND_PRUNING.md's 0.0 % visible
									 * from SQL rather than from a C harness */
extern uint64 weave_vecwork_shuttles;	/* vector shuttles ended: the denominator that
									 * says how many bolts the above is spread over */

#endif							/* WEAVE_H */
