/*-------------------------------------------------------------------------
 *
 * edist.c
 *		The `<@>` edit-distance shuttle's backend half: a WeaveShuttleOps skin
 *		over the bound and cursor in include/weave/edist.h, plus the SQL
 *		operator's function.
 *
 * Task Z9.  Everything that DECIDES anything -- the lower bound, the monotone
 * cursor, the backward refusal -- is in the header so that contracts (C1) and
 * (C2) can be property-tested at scale with a bare compiler
 * (test/hegel/test_edist.c).  This file walks a segment's dictionary chain,
 * computes each page's statistics once on entry, computes exact distances with
 * core's Levenshtein, and hands out float4s.
 *
 * THE VALUE IS contrib/fuzzystrmatch's levenshtein()'s, deliberately and by
 * construction: score() calls core's varstr_levenshtein(), which is the same
 * function levenshtein() itself calls, with the same unit -- CHARACTERS -- and
 * unit costs 1/1/1.  That agreement is the only reason sql/edist.sql can have an
 * oracle at all (doc/GAPS.md G30/G31, and doc/specs/FUZZY_CHANNEL.md sect. 8's
 * Z9 row: "randomized differential test vs seq-scan levenshtein()").
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/query/edist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"		/* pg_mbstrlen_with_len: the edit unit */
#include "miscadmin.h"			/* CHECK_FOR_INTERRUPTS in the page walk */
#include "storage/bufmgr.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/varlena.h"		/* varstr_levenshtein[_less_equal] */

#include "weave/weave.h"
#include "weave/am.h"
#include "weave/channel.h"
#include "weave/edist.h"

/* The core's sentinel must be the contract's, or seek() would return a value
 * the scorer does not recognise as the end. */
StaticAssertDecl(WEAVE_EDIST_END == WEAVE_WARP_END,
				 "WEAVE_EDIST_END must equal WEAVE_WARP_END");

/*
 * How many dictionary entries can fit on one page, exactly: every entry is at
 * least MAXALIGN(header + one byte of term) long.  A fixed array of this size is
 * a PAGE-scale allocation, not a vocabulary-scale one, which is why it is not a
 * doubling `cap` (see ci/check-alloc.sh and the note in AGENTS.md about what that
 * gate was widened for).
 */
#define EDIST_MAX_PAGE_TERMS \
	(BLCKSZ / MAXALIGN(offsetof(WeaveDictEntry, term) + 1))

typedef struct EdistShuttle
{
	WeaveShuttle sh;
	MemoryContext ctx;

	Relation	index;
	WeaveSegMeta seg;			/* copied: the caller's meta snapshot need not
								 * outlive the shuttle */

	char	   *pat;			/* our copy of the pattern */
	int			patlen;
	WeaveEdistCursor cur;
	bool		single_byte_enc;

	/* the open dictionary page */
	BlockNumber blk;			/* the next page to open */
	Buffer		buf;			/* pinned + share-locked while positioned */
	BlockNumber nextblk;
	uint32		pagefirst;		/* term ordinal of this page's first entry */
	int			npageterms;
	char	   *entry[EDIST_MAX_PAGE_TERMS];

	uint32		nyielded;		/* ordinals accounted for so far */
	WeaveEdistCounters ctr;
	bool		blkcounted;		/* has this page been counted as pruned? */
}			EdistShuttle;

/*
 * EOF-tolerant page read, the same guard weave_scan_readbuf() applies in
 * src/am/amscan.c and for the same reason: a concurrent weave_vacuum can
 * TRUNCATE the index tail, so a block number from a pre-truncation metapage
 * snapshot points past EOF and ReadBuffer would raise a hard error.  Out of
 * range is END OF CHAIN.  Duplicated rather than exported because it is four
 * lines and exporting it would put a scan-private policy in am.h.
 */
static inline Buffer
edist_readbuf(Relation index, BlockNumber blk)
{
	if (blk == InvalidBlockNumber || blk >= RelationGetNumberOfBlocks(index))
		return InvalidBuffer;
	return ReadBuffer(index, blk);
}

/* Drop the page we are sitting on, if any.  Idempotent. */
static void
edist_release(EdistShuttle *es)
{
	if (es->buf != InvalidBuffer)
		UnlockReleaseBuffer(es->buf);
	es->buf = InvalidBuffer;
	es->npageterms = 0;
}

/* All bytes below 0x80 -- the condition under which a byte trigram cannot
 * straddle two characters, so the divisor 3 applies (edist.h). */
static inline bool
edist_all_ascii(const char *s, int len)
{
	int			i;

	for (i = 0; i < len; i++)
		if ((unsigned char) s[i] >= 0x80)
			return false;
	return true;
}

/*
 * Open the next dictionary page that carries at least one usable entry, index
 * its entries, compute its statistics, and install it as the cursor's block.
 * Returns false at the end of the chain.
 *
 * EVERY GUARD THE FUZZY WALK HAS IS HERE AND EACH IS LOAD-BEARING, because a
 * dictionary page is read under BUFFER_LOCK_SHARE while a concurrent merge can
 * free it and an insert recycle it (weave_dict_entry_fits in weave/am.h):
 * edist_readbuf() returning InvalidBuffer is end of chain, not an error;
 * weave_dict_entry_fits() before termlen is trusted; weave_page_entry_end() as
 * the limit, never a raw pd_lower (make check-pdlower); CHECK_FOR_INTERRUPTS()
 * between pages with no buffer lock held.
 */
static bool
edist_open_next_page(EdistShuttle *es)
{
	for (;;)
	{
		Page		page;
		char	   *ptr;
		char	   *end;
		WeaveEdistStats st;

		edist_release(es);
		if (es->blk == InvalidBlockNumber)
			return false;
		CHECK_FOR_INTERRUPTS();	/* between pages, no buffer lock held */
		es->buf = edist_readbuf(es->index, es->blk);
		if (es->buf == InvalidBuffer)
		{
			/* truncated by a concurrent weave_vacuum: end of chain */
			es->blk = InvalidBlockNumber;
			return false;
		}
		LockBuffer(es->buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(es->buf);
		ptr = (char *) PageGetContents(page);
		end = weave_page_entry_end(page);
		es->nextblk = WeavePageGetOpaque(page)->nextblk;
		es->blk = es->nextblk;

		weave_edist_stats_init(&st);
		es->npageterms = 0;
		while (ptr < end && es->npageterms < (int) EDIST_MAX_PAGE_TERMS)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			int			chars;
			uint32		trg[WEAVE_MAX_TRIGRAMS];
			int			ntrg;
			bool		ascii;

			if (!weave_dict_entry_fits(de, end))
				break;			/* recycled/corrupt page: abandon the rest */
			ascii = es->single_byte_enc ||
				edist_all_ascii(de->term, (int) de->termlen);
			chars = es->single_byte_enc ? (int) de->termlen
				: pg_mbstrlen_with_len(de->term, (int) de->termlen);
			ntrg = weave_trigrams(de->term, (int) de->termlen, trg,
								  WEAVE_MAX_TRIGRAMS);
			weave_edist_stats_add(&st, de->termlen, (uint32) chars,
								  (uint32) ntrg, ascii ? 1 : 0);
			es->entry[es->npageterms++] = ptr;
			ptr += MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
		}

		if (es->npageterms == 0)
			continue;			/* empty or refused page: follow the chain */

		es->pagefirst = es->nyielded;
		es->nyielded += (uint32) es->npageterms;
		es->ctr.npage++;
		es->ctr.nterm += es->npageterms;
		es->blkcounted = false;
		if (weave_edist_enter_block(&es->cur, es->pagefirst,
									es->pagefirst + (uint32) es->npageterms - 1,
									&st) != WEAVE_EDIST_OK)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("weave edist shuttle refuses a dictionary block"),
					 errdetail("Block [%u, %u] of %d terms does not follow the previous one.",
							   es->pagefirst,
							   es->pagefirst + (uint32) es->npageterms - 1,
							   es->npageterms)));
		return true;
	}
}

/* The entry at the shuttle's current position, or NULL past the end. */
const WeaveDictEntry *
weave_edist_shuttle_entry(WeaveShuttle *s)
{
	EdistShuttle *es = (EdistShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END || es->npageterms == 0)
		return NULL;
	if (s->cur < es->pagefirst ||
		s->cur >= es->pagefirst + (uint32) es->npageterms)
		return NULL;
	return (const WeaveDictEntry *) es->entry[s->cur - es->pagefirst];
}

/*
 * (C1).  The core decides; this drives the page chain when the core says the
 * target is past the installed block, and raises V8's wording for a backward
 * seek so that a fused-loop bug reads the same whichever channel catches it.
 */
static WeaveWarp
edist_shuttle_seek(WeaveShuttle *s, WeaveWarp target)
{
	EdistShuttle *es = (EdistShuttle *) s->state;

	for (;;)
	{
		weave_ed_uint32 out = WEAVE_EDIST_END;
		WeaveEdistError rc = weave_edist_seek(&es->cur, (weave_ed_uint32) target,
											  &out);

		if (rc == WEAVE_EDIST_BACKWARD)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("weave edist shuttle cannot seek backwards"),
					 errdetail("Target warp %u is below the current position %u.",
							   (unsigned) target, (unsigned) s->cur)));
		if (rc == WEAVE_EDIST_NEEDBLOCK)
		{
			if (!edist_open_next_page(es))
			{
				edist_release(es);
				weave_edist_finish(&es->cur);
			}
			continue;
		}
		Assert(rc == WEAVE_EDIST_OK);
		s->cur = (WeaveWarp) out;
		s->blkend = (WeaveWarp) weave_edist_blkend(&es->cur);
		if (s->cur == WEAVE_WARP_END)
			s->blkend = WEAVE_WARP_END;
		return s->cur;
	}
}

/*
 * (C2)+(C3): minus the block's lower bound on the distance.  Reads the seven
 * integers the cursor already holds -- no buffer, no term, no I/O.
 */
static float4
edist_shuttle_block_max(WeaveShuttle *s)
{
	EdistShuttle *es = (EdistShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END)
		return WEAVE_SCORE_NEVER;
	return -(float4) weave_edist_block_lower(&es->cur);
}

/* (C4): the exact contribution at s->cur, which for this channel is minus the
 * exact character-level Levenshtein distance from the pattern to the term. */
static float4
edist_shuttle_score(WeaveShuttle *s)
{
	const WeaveDictEntry *de = weave_edist_shuttle_entry(s);

	if (de == NULL)
		return WEAVE_SCORE_NEVER;
	return -(float4) varstr_levenshtein(((EdistShuttle *) s->state)->pat,
									   ((EdistShuttle *) s->state)->patlen,
									   de->term, (int) de->termlen,
									   1, 1, 1, true);
}

int
weave_edist_shuttle_dist_le(WeaveShuttle *s, int maxd)
{
	EdistShuttle *es = (EdistShuttle *) s->state;
	const WeaveDictEntry *de = weave_edist_shuttle_entry(s);

	if (de == NULL)
		return INT_MAX;
	es->ctr.nterm_scored++;
	if (maxd < 0)
		maxd = 0;
	return varstr_levenshtein_less_equal(es->pat, es->patlen,
										 de->term, (int) de->termlen,
										 1, 1, 1, maxd, true);
}

int
weave_edist_shuttle_block_lower(WeaveShuttle *s)
{
	EdistShuttle *es = (EdistShuttle *) s->state;

	if (s->cur == WEAVE_WARP_END)
		return INT_MAX;
	return (int) weave_edist_block_lower(&es->cur);
}

WeaveWarp
weave_edist_shuttle_skip_block(WeaveShuttle *s)
{
	EdistShuttle *es = (EdistShuttle *) s->state;
	WeaveWarp	past;

	if (s->cur == WEAVE_WARP_END)
		return s->cur;
	if (!es->blkcounted)
	{
		es->ctr.npage_pruned++;
		es->blkcounted = true;
	}
	past = s->blkend;
	if (past == WEAVE_WARP_END)
		return edist_shuttle_seek(s, WEAVE_WARP_END - 1);
	return edist_shuttle_seek(s, past + 1);
}

void
weave_edist_shuttle_counters(WeaveShuttle *s, WeaveEdistCounters *out)
{
	*out = ((EdistShuttle *) s->state)->ctr;
}

/* One context, one delete -- and the buffer released first, because the shuttle
 * holds it share-locked between seeks (the fuzzy walk's PG_FINALLY rationale). */
static void
edist_shuttle_end(WeaveShuttle *s)
{
	EdistShuttle *es = (EdistShuttle *) s->state;
	MemoryContext ctx = es->ctx;

	edist_release(es);
	MemoryContextDelete(ctx);
}

void
weave_edist_shuttle_end(WeaveShuttle *s)
{
	edist_shuttle_end(s);
}

static const WeaveShuttleOps edist_shuttle_ops = {
	edist_shuttle_seek,
	edist_shuttle_block_max,
	edist_shuttle_score,
	NULL,						/* score_block: a block is a whole dictionary
								 * page and its scores are Levenshtein calls,
								 * so there is no cheaper bulk path */
	NULL,						/* set_visit_filter: graph-only */
	edist_shuttle_end
};

WeaveShuttle *
weave_edist_shuttle_begin(Relation index, const WeaveSegMeta *seg,
						  const char *pat, int patlen, MemoryContext cxt)
{
	MemoryContext ctx;
	EdistShuttle *es;
	WeaveEdistPattern pv;
	uint32		trg[WEAVE_MAX_TRIGRAMS];
	int			ntrg;
	int			maxcharlen = pg_database_encoding_max_length();
	int			chars;

	if (seg == NULL || patlen < 0 || (patlen > 0 && pat == NULL))
		elog(ERROR, "weave edist shuttle needs a segment and a pattern");

	ctx = AllocSetContextCreate(cxt, "weave edist shuttle",
								ALLOCSET_SMALL_SIZES);
	es = (EdistShuttle *) MemoryContextAllocZero(ctx, sizeof(EdistShuttle));
	es->ctx = ctx;
	es->index = index;
	es->seg = *seg;
	es->single_byte_enc = (maxcharlen == 1);
	es->pat = (char *) MemoryContextAlloc(ctx, (Size) patlen + 1);
	memcpy(es->pat, pat, (Size) patlen);
	es->pat[patlen] = '\0';
	es->patlen = patlen;

	chars = es->single_byte_enc ? patlen : pg_mbstrlen_with_len(pat, patlen);
	ntrg = weave_trigrams(pat, patlen, trg, WEAVE_MAX_TRIGRAMS);
	weave_edist_pattern_init(&pv, (uint32) patlen, (uint32) chars,
							 (uint32) ntrg,
							 edist_all_ascii(pat, patlen) ? 1 : 0,
							 (uint32) maxcharlen);
	weave_edist_init(&es->cur, &pv);

	es->blk = seg->dictstart;
	es->buf = InvalidBuffer;
	es->nextblk = InvalidBlockNumber;
	es->npageterms = 0;
	es->nyielded = 0;

	es->sh.ops = &edist_shuttle_ops;
	es->sh.kind = WEAVE_CH_FUZZY;
	es->sh.cur = 0;
	es->sh.blkend = 0;
	es->sh.weight = 1.0f;
	/* A distance is never negative, so no position scores above zero, and zero
	 * is attained exactly when the pattern IS a vocabulary term.  Tight. */
	es->sh.maxscore = 0.0f;
	es->sh.state = es;
	return &es->sh;
}

/* ---------------------------------------------------------------------------
 * The SQL surface: wdoc <@> text
 * ------------------------------------------------------------------------- */

PG_FUNCTION_INFO_V1(weave_edist);

/*
 * weave_edist(wdoc, text) -> float8: the minimum character-level Levenshtein
 * distance from the pattern to any term of the document.  See the install SQL
 * for why a document-level "distance" has to be a minimum over terms.
 *
 * A document with no terms has no distance; +Infinity sorts it last under
 * ascending ORDER BY, which is the only answer that keeps "closest first"
 * meaningful.  The index path never produces such a row, because a term-free
 * document appears in no posting list.
 */
Datum
weave_edist(PG_FUNCTION_ARGS)
{
	WeaveDoc	doc;
	text	   *pat;
	int			d;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	doc = PG_GETARG_WDOC(0);
	pat = PG_GETARG_TEXT_PP(1);

	d = weave_doc_min_edist(doc, VARDATA_ANY(pat), (int) VARSIZE_ANY_EXHDR(pat));

	PG_FREE_IF_COPY(doc, 0);
	PG_FREE_IF_COPY(pat, 1);
	if (d < 0)
		PG_RETURN_FLOAT8(get_float8_infinity());
	PG_RETURN_FLOAT8((float8) d);
}

PG_FUNCTION_INFO_V1(weave_edist_commutator);

/* text <@> wdoc (commutator): edit distance is symmetric, so this is the same
 * number with the arguments the other way round. */
Datum
weave_edist_commutator(PG_FUNCTION_ARGS)
{
	WeaveDoc	doc;
	text	   *pat;
	int			d;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	pat = PG_GETARG_TEXT_PP(0);
	doc = PG_GETARG_WDOC(1);

	d = weave_doc_min_edist(doc, VARDATA_ANY(pat), (int) VARSIZE_ANY_EXHDR(pat));

	PG_FREE_IF_COPY(pat, 0);
	PG_FREE_IF_COPY(doc, 1);
	if (d < 0)
		PG_RETURN_FLOAT8(get_float8_infinity());
	PG_RETURN_FLOAT8((float8) d);
}
