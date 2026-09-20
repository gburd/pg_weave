/*-------------------------------------------------------------------------
 *
 * cgram.h
 *		The corpus character-trigram weft (task Z8): on-disk root page, and
 *		the pattern -> required-trigram extractor the scan route uses.
 *
 * WHAT THIS CHANNEL IS, AND WHAT IT IS NOT.  A `cgram` weft is a SECOND,
 * LEXICAL-SHAPED weft whose vocabulary is character trigrams of a raw text
 * column: one dictionary entry per distinct trigram, whose postings are the
 * DOCIDS of the documents containing it, FOR-packed in exactly the lexical
 * posting format with the same sparse per-page block index.  Nothing about the
 * posting format is new; only the vocabulary is.  That is deliberate and it is
 * the only reason this task is tractable at all -- GenericXLog, the k-way merge
 * reader, vacuum, tombstones and the livedocs rule all come from reusing the
 * lexical shape rather than inventing a format.
 *
 * DO NOT CONFLATE IT WITH src/pages/trgm_page.c.  That is the VOCABULARY
 * trigram weft: trigram -> the TERM ORDINALS of dictionary terms containing it,
 * stored as a sparsemap blob per trigram, and its job is to prune which
 * dictionary TERMS a fuzzy/regex walk must test.  This one is trigram -> DOCIDS,
 * and its job is to prune which DOCUMENTS a cross-token substring search must
 * fetch.  One indexes the vocabulary, the other indexes the corpus; they share
 * a key space (weave_trigrams(), src/query/trgm.c) and nothing else.  The names
 * are close enough that a future reader will assume they are the same thing, so
 * this paragraph exists.
 *
 * THE UNIT IS THE BYTE.  weave_trigrams() hashes three consecutive BYTES of the
 * server-encoded value.  It does not know about character boundaries, so a
 * trigram may straddle two multi-byte characters.  That is sound for a
 * CANDIDATE filter -- both sides of the comparison (the indexed value and the
 * query pattern's literal runs) are hashed the same way, so a byte-identical
 * substring always produces byte-identical trigrams -- and it is exactly why
 * doc/GAPS.md G30 insists nothing here claims to count CHARACTERS.  The one
 * place it bites is case folding, see WEAVE_CGRAM_FOLD below.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/cgram.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_CGRAM_H
#define WEAVE_CGRAM_H

#include <stddef.h>
#include <stdint.h>

/*
 * The WEAVE_PK_CGRAM page at the weft root.  It exists because the channel
 * descriptor page (include/weave/chandesc.h) gives a weft exactly ONE root
 * block, and this weft is THREE chains -- dictionary, sparse block index, and
 * postings -- plus a count.  The vector weft hit the same wall and solved it the
 * same way with WEAVE_PK_VMETA; doing anything else (a fourth WeaveSegMeta
 * field, say) would break the v6 promise that a weft absent from a bolt costs
 * literally zero bytes.
 *
 * Every field is here because a reader cannot derive it:
 *
 *   magic/version   on-disk bytes are not trusted (doc/CONVENTIONS.md), and a
 *                   zeroed or recycled page must not validate as an empty weft;
 *                   version is what lets a future layout be REFUSED rather than
 *                   misparsed.
 *   dictstart       the trigram dictionary's first page.  Not derivable: the
 *                   root page is not the first dictionary page (it is written
 *                   last, after the chains it names).
 *   dictindexstart  the sparse block index over those dictionary pages, or
 *                   InvalidBlockNumber when the weft is small enough that the
 *                   writer emitted none.  Without it a probe would scan the
 *                   whole dictionary chain -- correct but linear in the
 *                   vocabulary, which for byte trigrams is up to 2^24 entries.
 *   postingstart    the head of the shared posting chain.  The dictionary
 *                   entries already name their own first posting block, so this
 *                   is NOT needed to READ the weft -- it is needed to FREE it.
 *                   weave_free_segment() recovers the lexical posting chain by
 *                   reading the first dictionary entry; doing that for a second
 *                   weft means trusting a second dictionary page's bytes on the
 *                   free path, and freeing pages from a corrupt read is the
 *                   worst outcome in that file.  Recording the head costs four
 *                   bytes once per bolt.
 *   nterms          distinct trigrams in this weft.  Diagnostic (weave_cgram_
 *                   stats(), the size report) and a cheap sanity bound: a weft
 *                   claiming more trigrams than 2^24 is corrupt.
 *   reserved        keeps the struct 8-byte aligned and must read as zero, so a
 *                   future writer that uses it is refused by today's reader.
 */
#define WEAVE_CGRAM_MAGIC		0x57434731	/* "WCG1" */
#define WEAVE_CGRAM_VERSION		1

/* A byte trigram is 3 bytes, so no corpus can hold more than 2^24 distinct
 * ones.  Used only as a corruption bound on nterms. */
#define WEAVE_CGRAM_MAX_TERMS	(1 << 24)

typedef struct WeaveCgramPageData
{
	uint32_t	magic;			/* WEAVE_CGRAM_MAGIC */
	uint16_t	version;		/* WEAVE_CGRAM_VERSION */
	uint16_t	reserved;		/* must be zero */
	uint32_t	dictstart;		/* first trigram-dictionary page */
	uint32_t	dictindexstart;	/* first sparse block-index page, or Invalid */
	uint32_t	postingstart;	/* head of the shared posting chain, or Invalid */
	uint32_t	nterms;			/* distinct trigrams */
} WeaveCgramPageData;

#define WEAVE_CGRAM_PAGEDATA_SIZE	(sizeof(WeaveCgramPageData))

/*
 * How many required trigrams one pattern may contribute.  A pattern longer than
 * this is not refused: the route takes the first WEAVE_CGRAM_MAX_REQ and drops
 * the rest, which only widens the candidate set (fewer AND terms), and the
 * mandatory recheck makes a wider candidate set slower, never wrong.  Dropping a
 * trigram is safe; ADDING one that the match does not imply is not.
 */
#define WEAVE_CGRAM_MAX_REQ		64

/*
 * WEAVE_CGRAM_FOLD: the indexed bytes and the pattern's literal runs are both
 * ASCII-lowercased, byte by byte, before hashing.
 *
 * WHY FOLD AT ALL: it is what lets ONE weft serve both the case-sensitive and
 * the case-insensitive operator.  An unfolded weft cannot serve ILIKE at all
 * (the pattern's trigrams are simply not the document's), and building two wefts
 * doubles the bytes this channel already admits it costs.
 *
 * WHY IT IS SOUND FOR THE CASE-SENSITIVE OPERATOR: folding is applied to BOTH
 * sides, so a byte-identical substring still yields identical trigrams.  Folding
 * can only make two different byte sequences collide, i.e. it can only ADD
 * candidates, and the recheck is exact.
 *
 * WHY IT IS NOT SOUND FOR ILIKE OVER NON-ASCII, which is a refusal and not a
 * bug: PostgreSQL's ILIKE folds case through lower(), which is encoding- and
 * locale-aware, so 'E' with an acute accent (0xC3 0x89 in UTF-8) matches its
 * lowercase form (0xC3 0xA9).  A byte-wise ASCII fold leaves both alone, so the
 * pattern would REQUIRE a trigram the matching document does not contain -- a
 * false negative, i.e. a silently dropped row, which no fixed-expected-output
 * test can catch (AGENTS.md hard rule 1).  So weave_cgram_required() REFUSES a
 * case-insensitive pattern whose literal runs contain any byte >= 0x80, and the
 * caller falls back to the universe path.  Case-SENSITIVE patterns have no such
 * restriction and are served over multi-byte text.
 */
static inline unsigned char
weave_cgram_fold_byte(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char) (c - 'A' + 'a') : c;
}

/*
 * The 4-byte DICTIONARY TERM a trigram hash is stored under, BIG-ENDIAN.
 *
 * Endianness is load-bearing and not a style choice.  The dictionary and its
 * sparse block index are ordered by memcmp-then-shorter-first (cmp_buildterm /
 * merge_cmp_term, and weave_dict_seek() binary-searches on exactly that), while
 * the writer groups pairs in NUMERIC hash order.  Big-endian is the encoding
 * under which those two orders are the SAME; little-endian would produce a
 * dictionary the block index mis-seeks, i.e. a probe that lands on the wrong
 * page and finds nothing -- a false negative, and therefore a silently dropped
 * row rather than an error.  One definition, used by the writer and the probe.
 */
static inline void
weave_cgram_key(uint32_t h, char key[4])
{
	key[0] = (char) ((h >> 24) & 0xFF);
	key[1] = (char) ((h >> 16) & 0xFF);
	key[2] = (char) ((h >> 8) & 0xFF);
	key[3] = (char) (h & 0xFF);
}

#define WEAVE_CGRAM_KEYLEN	4

/*
 * gram_ops strategy numbers.  1 and 2, and NOT the same operators as
 * wdoc_lex_ops's 1 (`@@@`) and 2 (`<=>`): sk_strategy is resolved against the
 * index COLUMN's operator family, so the two families reuse the numbers and the
 * scan disambiguates by attribute (weave_rescan).  They have to be inside
 * 1..amstrategies -- ALTER OPERATOR FAMILY validates the number against the
 * access method's amstrategies, which is 3 -- so "just use 11 and 12 and avoid
 * the question" is not available.
 */
#define WEAVE_STRAT_CGRAM_LIKE		1
#define WEAVE_STRAT_CGRAM_ILIKE		2

#endif							/* WEAVE_CGRAM_H */
