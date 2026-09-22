/*-------------------------------------------------------------------------
 *
 * vecdocmap.h -- the vector channel's adapter into the fused docid space
 *
 * Task F8.  doc/specs/FUSED_TOPK.md sect. 7b specified this and sect. 7c records
 * what building it found.
 *
 * THE PROBLEM, IN ONE SENTENCE.  The fused core (include/weave/fuse.h) drives
 * every channel in ONE position space, F2.2 chose the DOCID space because that is
 * where the lexical and gate channels already live, and the vector shuttle's
 * positions are SEGMENT-LOCAL DENSE LANE INDICES (src/vector/vecshuttle.c).  Two
 * channels that disagree about what an integer means do not produce a slow scan,
 * they produce a confident wrong answer: the core would intersect a docid against
 * a lane, prune on the comparison, and return k plausible rows.
 *
 * THE FIX IS A RELABELLING, AND THAT IS WHY IT IS SOUND.  Every lane of a weft
 * carries a docid, the warp map stores them, and the writer emits lanes in
 * DOCID-ASCENDING order (vec_docid_order() in src/vector/vecwrite.c; the ordering
 * guard t/017_vector_syncscan.pl exists to keep it that way).  So lane -> docid is
 * a strictly increasing map, and this file composes the lane-space channel
 * with it:
 *
 *	 seek(d)      = docid[ inner.seek( first lane whose docid >= d ) ]
 *	 block_max()  = inner.block_max()
 *	 score()      = inner.score() at the lane `cur` denotes
 *
 * WHY (C1) SURVIVES.  lower_bound is monotone in d, inner.seek is monotone in
 * lanes by (C1), and docid[] is monotone in lanes, so the composition of three
 * monotone maps is monotone.  The returned docid is also >= the target: the lane
 * found is the FIRST with docid >= d and inner.seek only moves forward from it.
 *
 * WHY (C2) SURVIVES, AND IS NOT EVEN WEAKENED.  The adapter publishes
 * blkend = docid[L] where L is the last lane of the inner block.  Take any docid
 * p in [cur, blkend] at which this channel can contribute: it is the docid of some
 * lane l, and because docid[] is STRICTLY ascending, cur = docid[lane] <= docid[l]
 * <= docid[L] forces lane <= l <= L -- so l lies inside the inner block and the
 * inner bound already covers it.  The docid interval also covers docids this bolt
 * does not carry at all, at which the channel contributes nothing, and the core
 * only ever sums the bound of a channel standing exactly on the pivot.
 *
 * WHY THE ORDER CHECK IS IN init() AND NOT AN ASSERTION.  The map comes off disk,
 * and doc/CONVENTIONS.md decision 2 is that on-disk bytes are not trusted.  A
 * descending pair would break the (C1) argument above silently -- a dropped row,
 * not an error -- so it is validated once, on the pass that materializes the
 * array, and refused with a reason the caller turns into ERRCODE_INDEX_CORRUPTED.
 *
 * WHY *STRICTLY* ASCENDING, WHICH THIS FILE FIRST GOT WRONG.  The first version
 * accepted equal neighbours and argued that a duplicate docid "breaks nothing
 * here, because both lanes lie inside any interval containing either".  Writing
 * the property test refuted it twice over, and the argument's error is worth
 * naming: it conflated the docid INTERVAL with the lane BLOCK the inner bound
 * actually describes.
 *
 *	 1. (C2).  With docid = [10, 10, 10, 50] and a block size of 1, seek(0) lands
 *		on lane 0 and publishes blkend = docid[0] = 10.  Lane 1 also has docid 10,
 *		so it is inside [cur, blkend] -- but the inner bound describes lane 0's
 *		block alone, and lane 1's score may be arbitrarily larger.  A bound that is
 *		too low drops rows silently, which is precisely what hard rule 1 is about.
 *	 2. (C4), which is worse and has no bound to blame.  score() is defined at ONE
 *		lane, so the other lanes sharing that docid contribute nothing no matter how
 *		honest the bound is.  A document's score would depend on which of its lanes
 *		the search happened to land on.
 *
 * Neither is fixable inside a relabelling: a docid with several lanes is not a
 * relabelling, it is an aggregation (a max over the lanes), which is a different
 * channel.  So duplicates are REFUSED.  Nothing legitimate writes one -- a bolt
 * carries each document once and vec_docid_order() sorts the lanes of a set of
 * distinct docids -- so this refuses corrupt bytes and nothing else.
 *
 * WHY THIS ADAPTS A WeaveFuseChan AND NOT A WeaveShuttle.  Two reasons, and the
 * second is the one that decided it:
 *
 *	 1. The relabelling is arithmetic over integers.  A WeaveFuseChan needs no
 *	    postgres.h (fuse.h supplies its own fixed-width aliases), so this core can
 *	    be linked by a bare compiler and driven at 10^6 random cases by
 *	    test/hegel/test_vecdocmap.c -- which is what hard rule 1 asks of a
 *	    channel, and what channel.h could not offer because it needs the backend.
 *	 2. src/am/fuseshuttle.c already owns the WeaveShuttle -> WeaveFuseChan seam,
 *	    including the two obligations a hand-written thunk forgets (copy blkend
 *	    after every seek; put the shuttle's cur back before scoring).  Adapting at
 *	    the shuttle level would mean a second copy of both.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  include/weave/vecdocmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_VECDOCMAP_H
#define WEAVE_VECDOCMAP_H

#include "weave/fuse.h"

/*
 * One adapted channel.  `chan` is the face the core drives and is deliberately
 * the FIRST member, so a caller can hand &a->chan to weave_fuse_init() and get
 * the adapter back from the ops without a container_of.
 */
typedef struct WeaveVecDocChan
{
	WeaveFuseChan chan;			/* docid space; give THIS to the core */
	WeaveFuseChan *inner;		/* lane space; borrowed, not owned */

	/* docid[l] is the document id of lane l, strictly ascending.  Borrowed: the caller
	 * materializes it from the weft's warp map and owns its storage for at least
	 * as long as the run.  8 bytes per lane per bolt per pass is the whole cost
	 * of F8 (doc/specs/FUSED_TOPK.md sect. 7b). */
	const weave_ft_uint64 *docid;
	weave_ft_uint32 nlane;

	/* The lane `chan.cur` denotes.  Maintained here rather than recomputed,
	 * because score() must put the INNER channel back on its lane and searching
	 * for it again would be both slower and a second place to get it wrong. */
	weave_ft_uint32 lane;
} WeaveVecDocChan;

/*
 * The first lane whose docid is >= target, or `n` when there is none.
 *
 * Exposed because it is the whole (C1) argument and the property test drives it
 * directly against a linear oracle.  Requires docid[] ascending; the caller
 * establishes that once, in weave_vecdoc_chan_init().  (Only non-decreasing order
 * is needed for the search itself to be correct; strictness is required for
 * reasons the header gives, and is checked there.)
 */
static inline weave_ft_uint32
weave_vecdoc_lower_bound(const weave_ft_uint64 *docid, weave_ft_uint32 n,
						 weave_ft_uint64 target)
{
	weave_ft_uint32 lo = 0;
	weave_ft_uint32 hi = n;

	/*
	 * Plain binary search on a half-open interval, written so that `mid` cannot
	 * overflow at nlane near 2^32 -- lo + (hi - lo) / 2 rather than (lo + hi) / 2.
	 * A bolt that large is unreachable today (WEAVE_MAX_SEGMENTS x lanes), but the
	 * form costs nothing and the alternative is a wrong answer at a size nobody
	 * will test at.
	 */
	while (lo < hi)
	{
		weave_ft_uint32 mid = lo + (hi - lo) / 2;

		if (docid[mid] < target)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Wrap `inner` (a lane-space channel, already wrapped by
 * weave_fuse_wrap_shuttles()) as a docid-space channel over `docid[0 .. nlane)`.
 *
 * Copies the inner channel's weight, maxscore and required flag, because those
 * are properties of the CHANNEL and a relabelling changes none of them -- and
 * because the core reads them off the struct it was handed, which is the adapter.
 *
 * Returns 0, or -1 with *why set and nothing initialized.  Three refusals, each a
 * silent wrong answer prevented:
 *
 *	 - nlane == 0: an empty weft has no position to publish.  The caller must not
 *	   create a channel that can never contribute; a scored channel that always
 *	   returns END would still enter the partition arithmetic with its ceiling.
 *	 - a pair that is not strictly increasing: a descent breaks (C1) and therefore
 *	   the pruning proof, and a duplicate breaks (C2) and (C4) -- see the header,
 *	   which records the argument this file first made for accepting duplicates and
 *	   the two counterexamples that refuted it.
 *	 - a docid >= WEAVE_FUSE_END: the fused position space is 32 bits (the
 *	   sentinel is 0xFFFFFFFF) and a docid is 64.  Checked at the LAST element,
 *	   which the ascending check has just proved is the maximum.  This is the same
 *	   ceiling lex_publish() enforces (src/query/lexshuttle.c) and it is a real
 *	   limit on heap size, not a formality -- see sect. 7c.
 */
extern int weave_vecdoc_chan_init(WeaveVecDocChan *a, WeaveFuseChan *inner,
								  const weave_ft_uint64 *docid,
								  weave_ft_uint32 nlane, const char **why);

#endif							/* WEAVE_VECDOCMAP_H */
