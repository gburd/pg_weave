/*-------------------------------------------------------------------------
 *
 * surftrie.h
 *		The SuRF trie over the bolt VOCABULARY -- layout, builder, reader,
 *		validator.  Task Z3.
 *
 * Backend-independent by construction: no postgres.h, no palloc, no elog, no
 * allocation of any kind.  include/weave/for.h is the exemplar and
 * doc/TESTING.md is the reason -- an algorithmic core that cannot be linked into
 * a plain `gcc` invocation is a core whose property test people skip, and this
 * one is a filter whose failure mode is invisible to every other test layer.
 *
 * WHY OVER THE VOCABULARY.  pg_tre indexes trigrams of the CORPUS; pg_weave
 * funnels through the DICTIONARY instead (doc/ARCHITECTURE.md sect. 7,
 * doc/specs/FUZZY_CHANNEL.md sect. 1).  By Heaps' law the vocabulary grows as
 * roughly n^0.5 and stops growing once the corpus is large, which is the whole
 * reason a trie over it can exist at a sane size.  The dictionary is already
 * sorted, so the build needs no sort.
 *
 * NOT TO BE CONFUSED WITH weave/surf.h.  That is the imported pg_tre SuRF over
 * uint64 trigram KEYS (pg_weave_surf_*, palloc, ereport).  Different key space,
 * different structure, and it stays.  This file is the trie over
 * variable-length terms.
 *
 * ---------------------------------------------------------------------------
 * THE ERROR IS ONE-SIDED, AND THAT IS A CORRECTNESS CONTRACT
 * ---------------------------------------------------------------------------
 *
 * A SuRF may report a non-member as present.  It must NEVER report a member as
 * absent.
 *
 *		weave_surftrie_may_contain() == 0	=>  DEFINITELY absent
 *		weave_surftrie_may_contain() != 0	=>  POSSIBLY present
 *
 * The direction is not a performance detail.  A false negative drops rows from
 * a query that should have returned them, silently, with plausible-looking
 * results -- and per AGENTS.md hard rule 1 no fixed-expected-output regression
 * test can catch that.  It also interacts with contract (C5) in
 * weave/channel.h: a gate shuttle that reports -INF for a block that could
 * contain a match violates (C2) through this function.  test/hegel/test_surf.c
 * asserts the direction explicitly: every dictionary term MUST be reported
 * present, and a non-member MAY be.
 *
 * Where the one-sidedness actually bites is TERM LENGTH.  The format represents
 * the first WEAVE_SURFTRIE_MAX_DEPTH bytes of a term.  A longer term is indexed
 * truncated to that depth with its slot marked `trunc`, so any key sharing those
 * bytes reports present and the caller must recheck.  The two alternatives are
 * both worse: refusing to build denies the channel to a vocabulary with one long
 * token, and skipping the long term IS a false negative wearing a build-time
 * disguise.  For the same reason a zero-length term is WEAVE_SURF_EMPTY_TERM
 * rather than a silent skip -- terminal marks live on slots, there is no slot at
 * depth 0 to hang one on, and dropping it would again be a false negative.
 *
 * ---------------------------------------------------------------------------
 * THE IMAGE
 * ---------------------------------------------------------------------------
 *
 * One contiguous little-endian byte image; doc/specs/FUZZY_CHANNEL.md sect. 3.3
 * is the authoritative table and derives every choice.  Summary, with
 * BW = 8*ceil(nslots/64), NSB = ceil(nslots/512), NSEL = ceil(nnodes/64):
 *
 *		[0]   32-byte header (magic, version, flags, five counts, maxdepth)
 *		[32]  labels[nslots]			one byte per slot, LEVEL ORDER (BFS)
 *		      haschild[BW]				bit/slot: slot has a child node
 *		      louds[BW]					bit/slot: slot is first of its node
 *		      terminal[BW]				bit/slot: the path here is a member
 *		      trunc[BW]					bit/slot: terminal by truncation (maybe)
 *		      rank_haschild[NSB]		u32 ones before each 512-bit superblock
 *		      rank_terminal[NSB]		u32, same, for terminal
 *		      select_louds[NSEL]		u32 slot of every 64th set louds bit
 *		      ords[nterminal]			u32 first ordinal each terminal covers
 *
 * A "slot" is one labelled edge: one distinct term prefix.  A "node" is the
 * maximal run of slots sharing a parent.  LOUDS-Sparse: the shape is carried by
 * two bits per slot and navigation is rank/select.
 *
 * Every multi-byte integer is read and written byte-wise little-endian and there
 * is NO struct overlay and NO alignment requirement.  That is deliberate: the
 * image is parsed straight out of a buffer page at whatever offset the writer
 * chose, so a struct overlay would make correctness depend on that offset and
 * UBSan would be right to complain.  It also makes the bytes identical on every
 * architecture.
 *
 * Total length is a pure function of the counts and the reader requires the
 * declared length to equal it EXACTLY.  There is deliberately no offset table:
 * an offset table is a second source of truth for where a section starts, and
 * then a corrupt image can be internally consistent and still disagree with the
 * counts.
 *
 * ---------------------------------------------------------------------------
 * VALIDATION IS TWO LAYERS, AND THE FIRST ONE IS A MEMORY-SAFETY CLAIM
 * ---------------------------------------------------------------------------
 *
 * On-disk bytes are not trusted (doc/CONVENTIONS.md decision 2).
 *
 *	weave_surftrie_open()	 header, exact size, zero tail bits, the three
 *							 popcount identities, trunc subset terminal, labels
 *							 strictly ascending within each node, rank tables and
 *							 select samples equal to a recomputation, and the
 *							 acyclicity inequality (a child node starts AFTER its
 *							 parent slot).  AFTER IT RETURNS WEAVE_SURF_OK, NO
 *							 QUERY CAN READ OUTSIDE THE IMAGE OR FAIL TO
 *							 TERMINATE.  test/fuzz/fuzz_surftrie.c asserts that.
 *
 *	weave_surftrie_validate() the semantic layer weave_check() calls: a full
 *							 lexicographic DFS proving every slot and node is
 *							 reachable, the terminal count is what the header
 *							 claims, the observed depth equals maxdepth, and the
 *							 ordinals are 0 = ord[0] < ord[1] < ... < nterms.
 *
 * Two of those checks exist because the failure they catch is a WRONG ANSWER
 * rather than a crash, and a wrong answer here is a false negative: a corrupt
 * rank superblock navigates to the wrong node, and a corrupt select sample walks
 * off the bitmap.  Recomputing both is one linear pass, which is cheap next to
 * "the fuzzer cannot see it and neither can a regression test".
 *
 * ---------------------------------------------------------------------------
 * ALLOCATION -- READ THIS BEFORE ADDING ONE
 * ---------------------------------------------------------------------------
 *
 * A trie over the vocabulary is a vocabulary-scale allocation, which is the
 * exact class behind four real crashes in this extension's ancestor (`make
 * check-alloc`, and AGENTS.md's lint table).  So this
 * core allocates NOTHING: weave_surftrie_size() returns the exact image length
 * and weave_surftrie_build() writes into a caller-supplied buffer, refusing with
 * WEAVE_SURF_NOSPACE rather than growing it.  The AM's writer is the one place
 * that allocates, and it must use WEAVE_ALLOC_MAYBE_HUGE.
 *
 * The builder needs no scratch memory either: at depth d the nodes are exactly
 * the maximal runs of terms sharing a d-byte prefix, and sortedness makes those
 * runs contiguous, so there is no BFS queue to size.  Cost is
 * O(nterms * maxdepth) byte comparisons, paid twice (measure, then emit) -- and
 * the two passes are the same function driven by two sinks, so a measure/emit
 * disagreement, which would be a buffer overrun, is not expressible.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/surftrie.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_SURFTRIE_H
#define WEAVE_SURFTRIE_H

#include <stddef.h>
#include <stdint.h>

#include "weave/pagekind.h"

/*
 * PostgreSQL's c.h defines uint64/uint32/...; only supply the weave_* aliases
 * from <stdint.h> when compiled outside the backend.  Mirrors weave/quantize.h.
 */
#ifndef POSTGRES_H
typedef uint64_t weave_st_uint64;
typedef uint32_t weave_st_uint32;
typedef uint16_t weave_st_uint16;
typedef uint8_t weave_st_uint8;
#else
typedef uint64 weave_st_uint64;
typedef uint32 weave_st_uint32;
typedef uint16 weave_st_uint16;
typedef uint8 weave_st_uint8;
#endif

/*
 * The page kind this image lands on.  An ALIAS of the id allocated in
 * weave/pagekind.h -- ids live in one place so a second channel cannot quietly
 * take a number this one used (doc/specs/SEGMENT_FORMAT.md sect. 2).  It is an
 * INTEGER in WeavePageOpaqueData.kind under the escape bit, NOT a bit: read it
 * with WeavePageHasKind(), never `flags & WEAVE_SURF_PAGE`, which compiles and
 * is always false.
 */
#define WEAVE_SURF_PAGE			WEAVE_PK_SURF

#define WEAVE_SURFTRIE_MAGIC	0x57535431	/* "WST1" */
#define WEAVE_SURFTRIE_VERSION	1

/*
 * Bytes of a term the trie represents.  255 rather than a page-sized limit for
 * two reasons: a depth counter fits a byte, and the DFS walkers can then keep
 * their stack and their term buffer as fixed-size automatic arrays, so no query
 * path allocates.  A longer term is indexed TRUNCATED to this depth -- see the
 * one-sided-error discussion at the top; that is not a limitation to be removed
 * later, it is the mechanism that keeps the guarantee one-sided.
 */
#define WEAVE_SURFTRIE_MAX_DEPTH	255

/*
 * Caps on the counts, checked before any size arithmetic.  They exist so the
 * total-size computation cannot overflow on a 32-bit size_t no matter what the
 * header says -- 2^28 slots is ~1.3 GB of image, far past anything a bolt holds,
 * and hitting the cap is an ERROR rather than a truncation (the stance
 * WEAVE_MAX_SEGMENTS and WEAVE_MAX_WEFTS already take).
 */
#define WEAVE_SURFTRIE_MAX_TERMS	0x0FFFFFFFu
#define WEAVE_SURFTRIE_MAX_SLOTS	0x0FFFFFFFu

/* Header field offsets.  Byte offsets, not a struct: see the top comment. */
#define WEAVE_ST_OFF_MAGIC			0
#define WEAVE_ST_OFF_VERSION		4
#define WEAVE_ST_OFF_FLAGS			6
#define WEAVE_ST_OFF_NTERMS			8
#define WEAVE_ST_OFF_NSLOTS			12
#define WEAVE_ST_OFF_NNODES			16
#define WEAVE_ST_OFF_NTERMINAL		20
#define WEAVE_ST_OFF_NTRUNC			24
#define WEAVE_ST_OFF_MAXDEPTH		28
#define WEAVE_ST_OFF_RESERVED		30
#define WEAVE_ST_HDRSIZE			32

/*
 * Why a code and not a bool: the backend wrapper turns it into an errdetail, and
 * "which of the twenty-odd ways this image is wrong" is the difference between a
 * diagnosable corruption report and "index is corrupted".  Keep
 * weave_surftrie_errstr() in step -- the fuzz target asserts every rejection
 * carries a recognized string, so a new code with no string fails the harness.
 */
typedef enum WeaveSurfError
{
	WEAVE_SURF_OK = 0,

	/* builder side */
	WEAVE_SURF_UNSORTED,		/* input not strictly ascending */
	WEAVE_SURF_EMPTY_TERM,		/* a zero-length term (see the top comment) */
	WEAVE_SURF_TOO_MANY,		/* a count above the format cap */
	WEAVE_SURF_NOSPACE,			/* destination smaller than the image */

	/* reader side: header and geometry */
	WEAVE_SURF_TRUNCATED,		/* shorter than the fixed header */
	WEAVE_SURF_MAGIC,
	WEAVE_SURF_VERSION,
	WEAVE_SURF_FLAGS,			/* unknown flag bits */
	WEAVE_SURF_RESERVED,		/* reserved word nonzero */
	WEAVE_SURF_SIZE,			/* length != the size the counts imply */
	WEAVE_SURF_COUNTS,			/* the counts contradict each other */
	WEAVE_SURF_DEPTH,			/* maxdepth zero, over the cap, or inconsistent */

	/* reader side: bitvectors and accelerators */
	WEAVE_SURF_TAILBITS,		/* a set bit past nslots in a bitmap's last word */
	WEAVE_SURF_LOUDS_ROOT,		/* slot 0 does not start a node */
	WEAVE_SURF_LOUDS_COUNT,		/* popcount(louds) != nnodes */
	WEAVE_SURF_HASCHILD_COUNT,	/* popcount(haschild) != nnodes - 1 */
	WEAVE_SURF_TERMINAL_COUNT,	/* popcount(terminal) != nterminal */
	WEAVE_SURF_TRUNC,			/* popcount(trunc) != ntrunc, or a trunc slot that
								 * is not also terminal */
	WEAVE_SURF_RANK,			/* a rank superblock != recomputation */
	WEAVE_SURF_SELECT,			/* a select sample != recomputation */
	WEAVE_SURF_CYCLE,			/* a child node does not start after its parent slot */
	WEAVE_SURF_LABELS,			/* labels not strictly ascending within a node */

	/* reader side: the deep semantic pass */
	WEAVE_SURF_ORD_RANGE,		/* an ordinal >= nterms */
	WEAVE_SURF_ORD_ORDER,		/* ordinals not 0 = ord[0] < ord[1] < ... */
	WEAVE_SURF_REACH			/* a slot or node not reachable from the root */
} WeaveSurfError;

static inline const char *
weave_surftrie_errstr(WeaveSurfError e)
{
	switch (e)
	{
		case WEAVE_SURF_OK:
			return "ok";
		case WEAVE_SURF_UNSORTED:
			return "vocabulary is not strictly ascending";
		case WEAVE_SURF_EMPTY_TERM:
			return "vocabulary contains a zero-length term";
		case WEAVE_SURF_TOO_MANY:
			return "vocabulary or trie exceeds the format's maximum size";
		case WEAVE_SURF_NOSPACE:
			return "destination buffer is smaller than the serialized trie";
		case WEAVE_SURF_TRUNCATED:
			return "image too short for a surf trie header";
		case WEAVE_SURF_MAGIC:
			return "bad surf trie magic";
		case WEAVE_SURF_VERSION:
			return "unsupported surf trie version";
		case WEAVE_SURF_FLAGS:
			return "unknown surf trie flag bits";
		case WEAVE_SURF_RESERVED:
			return "reserved word is not zero";
		case WEAVE_SURF_SIZE:
			return "image length disagrees with the header's counts";
		case WEAVE_SURF_COUNTS:
			return "surf trie counts are mutually inconsistent";
		case WEAVE_SURF_DEPTH:
			return "surf trie maximum depth is invalid";
		case WEAVE_SURF_TAILBITS:
			return "bitmap has set bits past the last slot";
		case WEAVE_SURF_LOUDS_ROOT:
			return "slot 0 does not begin the root node";
		case WEAVE_SURF_LOUDS_COUNT:
			return "louds bitmap population does not equal the node count";
		case WEAVE_SURF_HASCHILD_COUNT:
			return "has-child population does not equal the node count minus one";
		case WEAVE_SURF_TERMINAL_COUNT:
			return "terminal bitmap population does not equal the terminal count";
		case WEAVE_SURF_TRUNC:
			return "truncated-slot bitmap is inconsistent with the terminal bitmap";
		case WEAVE_SURF_RANK:
			return "a rank superblock disagrees with the bitmap it indexes";
		case WEAVE_SURF_SELECT:
			return "a select sample disagrees with the louds bitmap";
		case WEAVE_SURF_CYCLE:
			return "a child node does not start after its parent slot";
		case WEAVE_SURF_LABELS:
			return "labels are not strictly ascending within a node";
		case WEAVE_SURF_ORD_RANGE:
			return "a term ordinal is out of range";
		case WEAVE_SURF_ORD_ORDER:
			return "term ordinals are not ascending in lexicographic order";
		case WEAVE_SURF_REACH:
			return "a slot or node is not reachable from the root";
	}
	return "unrecognized surf trie error";
}

/* One vocabulary term.  `s` need not be NUL-terminated; `len` is authoritative. */
typedef struct WeaveSurfTerm
{
	const char *s;
	weave_st_uint32 len;
} WeaveSurfTerm;

/*
 * An opened image.  Pointers alias into the caller's buffer -- nothing is
 * copied, so the buffer must outlive the handle.  Fields are read-only; treat
 * the struct as opaque apart from the counts.
 */
typedef struct WeaveSurfTrie
{
	const weave_st_uint8 *img;
	size_t		len;

	weave_st_uint32 nterms;
	weave_st_uint32 nslots;
	weave_st_uint32 nnodes;
	weave_st_uint32 nterminal;
	weave_st_uint32 ntrunc;
	weave_st_uint32 maxdepth;
	weave_st_uint32 bw;			/* bytes per bitmap = 8 * ceil(nslots / 64) */

	const weave_st_uint8 *labels;
	const weave_st_uint8 *haschild;
	const weave_st_uint8 *louds;
	const weave_st_uint8 *terminal;
	const weave_st_uint8 *trunc;
	const weave_st_uint8 *rank_haschild;
	const weave_st_uint8 *rank_terminal;
	const weave_st_uint8 *select_louds;
	const weave_st_uint8 *ords;
} WeaveSurfTrie;

/*
 * The answer to a point query.  `ord` is the first vocabulary ordinal the
 * matching terminal slot covers, which for an exact hit is THE term's ordinal.
 * `exact` is 0 when the hit landed on a truncated slot, i.e. this is a MAYBE and
 * the caller owes a recheck against the real term.
 */
typedef struct WeaveSurfHit
{
	weave_st_uint32 ord;
	int			exact;
} WeaveSurfHit;

/*
 * Prefix-enumeration callback.  `term`/`termlen` are valid only for the duration
 * of the call.  `exact` is 0 for a truncated (maybe) entry, in which case
 * `termlen` is WEAVE_SURFTRIE_MAX_DEPTH and the real terms it covers start at
 * ordinal `ord`.  Return 0 to continue, nonzero to stop the walk early.
 */
typedef int (*WeaveSurfEnumCb) (void *arg, const char *term,
								weave_st_uint32 termlen,
								weave_st_uint32 ord, int exact);

/*
 * Exact byte length of the image weave_surftrie_build() would write for this
 * vocabulary, and the input validation (strictly ascending, no empty term, under
 * the caps) that the build relies on.  `terms` may be NULL when n == 0.
 */
extern WeaveSurfError weave_surftrie_size(const WeaveSurfTerm *terms,
										  weave_st_uint32 n, size_t *size_out);

/*
 * Serialize the trie for `terms` into `dst`.  `terms` must be strictly ascending
 * by unsigned byte order (the dictionary already is).  On success *written is the
 * image length, which always equals weave_surftrie_size()'s answer.  Writes
 * nothing outside [dst, dst + *written).
 */
extern WeaveSurfError weave_surftrie_build(const WeaveSurfTerm *terms,
										   weave_st_uint32 n,
										   void *dst, size_t dstlen,
										   size_t *written);

/*
 * Validate an image and populate `t`.  See the two-layer discussion above: this
 * is the layer whose success is a memory-safety and termination claim for every
 * query below it.  NEVER reads outside [img, img + len).
 */
extern WeaveSurfError weave_surftrie_open(const void *img, size_t len,
										  WeaveSurfTrie *t);

/*
 * Point query.  Returns nonzero if `key` MAY be a member and zero if it is
 * DEFINITELY not -- the asymmetry is the contract, see the top of this file.
 * `hit` may be NULL; it is written only when the answer is nonzero.
 */
extern int	weave_surftrie_may_contain(const WeaveSurfTrie *t,
									   const void *key,
									   weave_st_uint32 keylen,
									   WeaveSurfHit *hit);

/*
 * Enumerate every term with the given prefix, in ascending lexicographic order,
 * calling `cb` once per entry; *nhits (may be NULL) receives the number of calls
 * made.  A zero-length prefix enumerates the whole vocabulary.
 *
 * ONE-SIDED, in the same direction as the point query: every member with the
 * prefix is either emitted exactly or covered by an emitted truncated entry of
 * which it is an extension.  Extra entries are possible (a truncated entry, or
 * any entry at all when prefixlen > WEAVE_SURFTRIE_MAX_DEPTH, where only the
 * first MAX_DEPTH bytes could be matched); missing entries are not.
 */
extern WeaveSurfError weave_surftrie_enumerate(const WeaveSurfTrie *t,
											   const void *prefix,
											   weave_st_uint32 prefixlen,
											   WeaveSurfEnumCb cb, void *arg,
											   weave_st_uint32 *nhits);

/*
 * The deep semantic validator weave_check() calls, on an already-opened image.
 * Reachability, terminal count, observed depth, and the ordinal ordering that is
 * the machine-checkable form of "this trie is the sorted dictionary".
 */
extern WeaveSurfError weave_surftrie_validate(const WeaveSurfTrie *t);

/* open() + validate(), for a caller holding only bytes. */
extern WeaveSurfError weave_surftrie_check(const void *img, size_t len);

#endif							/* WEAVE_SURFTRIE_H */
