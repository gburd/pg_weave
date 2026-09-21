/*-------------------------------------------------------------------------
 *
 * fuse.c
 *		The fused-threshold top-k core: doc/specs/FUSED_TOPK.md sect. 3.
 *
 * Backend-free, like src/vector/vecscan.c, and for the reason
 * include/weave/fuse.h states at length: this file is where every silent wrong
 * answer in a fused scan would live, so it is driven by plain structs and linked
 * by test/hegel/test_fuse_props.c with a bare compiler against the brute-force
 * reference at the bottom of this file.  That arrangement is also the mitigation
 * for AGENTS.md hard rule 7, which Phase F is running under a scoped waiver of.
 *
 * THE FOUR THINGS THIS FILE IS CAREFUL ABOUT.  None of them is arithmetic, and
 * each is a hazard the spec's pseudocode walks into; fuse.h numbers them 1-4 and
 * explains why.  In code they are:
 *
 * 1. A channel is never seeked backwards.  fuse_advance() consults chan->cur --
 *	  the core's memory of what seek() last RETURNED -- and calls seek() only on a
 *	  channel standing below the target.  A (C1) violation by a channel is an
 *	  ERROR, not a clamp.
 *
 * 2. Required and scored channels are different algorithms sharing a loop.
 *	  Required channels intersect; scored channels sum.  Nothing required enters
 *	  ceil[], suffix[] or `ub`.
 *
 * 3. No infinity enters an additive expression.  `remaining` is a suffix sum,
 *	  never ub minus something; a -INF running sum is discarded before it is
 *	  compared to anything; a NaN from any channel is an ERROR.
 *
 * 4. Ties break one way, here, in fuse_worse(), and the reference at the bottom
 *	  of the file reproduces the same order by construction rather than by
 *	  agreement.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/am/fuse.c
 *
 *-------------------------------------------------------------------------
 */
#include "weave/fuse.h"

/*
 * The (C2) check's relative tolerance.  See fuse.h: a channel may fold the same
 * quantities in a different order than its bound does, so an exact comparison
 * fails on rounding, and a relative one does not quietly accept slack at large
 * magnitudes the way an absolute one would.
 */
#define FUSE_C2_RELTOL		1e-6f

static int
fuse_isnan(float x)
{
	return x != x;
}

static int
fuse_finite(float x)
{
	return !fuse_isnan(x) && x != INFINITY && x != -INFINITY;
}

/*
 * (C6).  A position at or past nwarp is dead, which is also how the scan
 * terminates on a bolt shorter than the key space -- a gate's keys are docids
 * and can outrun the warp (see gate.h's width note).
 */
static int
fuse_live(const WeaveFuseState *st, weave_ft_uint32 p)
{
	if (p >= st->nwarp)
		return 0;
	if (st->live == NULL)
		return 1;
	return (int) ((st->live[p >> 6] >> (p & 63)) & 1);
}

/* ---------------------------------------------------------------------------
 * The top-k heap
 *
 * A min-heap whose root is the WORST entry under (score ASC, warp DESC), so that
 * evicting the root removes the highest warp among tied scores.  Combined with
 * accepting a candidate only when its score STRICTLY beats theta, that yields
 * the (score DESC, warp ASC) order the reference produces.  fuse.h's header
 * comment carries the argument for why discarding a score equal to theta cannot
 * lose a row the reference keeps; the property test checks it rather than
 * trusting it.
 * ------------------------------------------------------------------------- */

static int
fuse_worse(const WeaveFuseHit *a, const WeaveFuseHit *b)
{
	if (a->score < b->score)
		return 1;
	if (a->score > b->score)
		return 0;
	return a->warp > b->warp;
}

static void
fuse_swap(WeaveFuseHit *a, WeaveFuseHit *b)
{
	WeaveFuseHit t = *a;

	*a = *b;
	*b = t;
}

static void
fuse_sift_down(WeaveFuseHit *h, int n, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			w = i;

		if (l < n && fuse_worse(&h[l], &h[w]))
			w = l;
		if (r < n && fuse_worse(&h[r], &h[w]))
			w = r;
		if (w == i)
			break;
		fuse_swap(&h[i], &h[w]);
		i = w;
	}
}

static void
fuse_push(WeaveFuseState *st, weave_ft_uint32 warp, float score)
{
	if (st->nheap < st->k)
	{
		int			i = st->nheap++;

		st->heap[i].warp = warp;
		st->heap[i].score = score;
		while (i > 0)
		{
			int			parent = (i - 1) / 2;

			if (!fuse_worse(&st->heap[i], &st->heap[parent]))
				break;
			fuse_swap(&st->heap[i], &st->heap[parent]);
			i = parent;
		}
	}
	else
	{
		/* Caller has already established score > theta == heap[0].score. */
		st->heap[0].warp = warp;
		st->heap[0].score = score;
		fuse_sift_down(st->heap, st->nheap, 0);
	}

	st->theta = (st->nheap == st->k) ? st->heap[0].score : -INFINITY;
}

/* ---------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------- */

/*
 * Insertion sort of the scored channels by descending weighted ceiling.  Not
 * qsort: this is a backend-free TU that must link with a bare compiler, nsc is a
 * few dozen (WEAVE_FUSE_MAX_CHAN is 64), and an insertion sort is stable, so
 * equal ceilings keep the caller's order and the partition is reproducible run
 * to run.  A comparison sort with an unspecified tie order would make `split`
 * depend on libc.
 */
static void
fuse_sort_scored(WeaveFuseState *st)
{
	int			i;

	for (i = 1; i < st->nsc; i++)
	{
		WeaveFuseChan *c = st->sc[i];
		float		v = st->ceil[i];
		int			j = i - 1;

		while (j >= 0 && st->ceil[j] < v)
		{
			st->sc[j + 1] = st->sc[j];
			st->ceil[j + 1] = st->ceil[j];
			j--;
		}
		st->sc[j + 1] = c;
		st->ceil[j + 1] = v;
	}
}

WeaveFuseError
weave_fuse_init(WeaveFuseState *st, WeaveFuseChan **chan, int nchan,
				WeaveFuseHit *heap, int k,
				const weave_ft_uint64 *live, weave_ft_uint32 nwarp)
{
	int			i;

	if (nchan < 1 || nchan > WEAVE_FUSE_MAX_CHAN)
		return WEAVE_FUSE_BAD_NCHAN;
	if (k < 1)
		return WEAVE_FUSE_BAD_K;

	/* Zero the whole state: every counter, and both scratch arrays. */
	for (i = 0; i < WEAVE_FUSE_MAX_CHAN; i++)
	{
		st->sc[i] = NULL;
		st->rq[i] = NULL;
		st->ceil[i] = 0.0f;
		st->suffix[i] = 0.0f;
		st->cbound[i] = 0.0f;
		st->csuffix[i] = 0.0f;
		st->contrib[i] = 0;
	}
	st->suffix[WEAVE_FUSE_MAX_CHAN] = 0.0f;
	st->csuffix[WEAVE_FUSE_MAX_CHAN] = 0.0f;
	st->nsc = 0;
	st->nrq = 0;
	st->ncontrib = 0;
	st->split = 0;
	st->heap = heap;
	st->k = k;
	st->nheap = 0;
	st->theta = -INFINITY;
	st->live = live;
	st->nwarp = nwarp;
	st->check_bounds = 0;
	st->npivot = 0;
	st->nblkskip = 0;
	st->nrqskip = 0;
	st->nlivedrop = 0;
	st->nveto = 0;
	st->nabandon = 0;
	st->badchan = NULL;

	for (i = 0; i < nchan; i++)
	{
		WeaveFuseChan *c = chan[i];

		c->cur = 0;
		c->blkend = 0;
		c->nseek = 0;
		c->nscore = 0;

		if (!fuse_finite(c->weight) || c->weight <= 0.0f)
		{
			st->badchan = c;
			return WEAVE_FUSE_BAD_WEIGHT;
		}

		if (c->required)
		{
			/*
			 * maxscore is ignored for a required channel, and deliberately not
			 * validated: (C5) makes it +INF, which is the one value the
			 * partition arithmetic must never see, and rejecting it here would
			 * refuse every conforming gate.
			 */
			st->rq[st->nrq++] = c;
		}
		else
		{
			if (!fuse_finite(c->maxscore))
			{
				st->badchan = c;
				return WEAVE_FUSE_BAD_MAXSCORE;
			}
			st->ceil[st->nsc] = c->weight * c->maxscore;
			st->sc[st->nsc] = c;
			st->nsc++;
		}
	}

	fuse_sort_scored(st);

	/* suffix[j] = sum of ceil[i] for i >= j; suffix[nsc] = 0. */
	st->suffix[st->nsc] = 0.0f;
	for (i = st->nsc - 1; i >= 0; i--)
		st->suffix[i] = st->suffix[i + 1] + st->ceil[i];

	/* theta is -INF, so every scored channel starts essential. */
	st->split = st->nsc;

	return WEAVE_FUSE_OK;
}

/* FUSED_TOPK.md sect. 5: split only ever moves left, as theta grows. */
static void
fuse_repartition(WeaveFuseState *st)
{
	while (st->split > 0 && st->suffix[st->split - 1] <= st->theta)
		st->split--;
}

/* ---------------------------------------------------------------------------
 * The loop
 * ------------------------------------------------------------------------- */

/*
 * Move one channel to >= target and record where it landed.  Returns 0 on a
 * (C1) violation.  This is the only place seek() is called, so hazard 1 is
 * structural rather than a rule to remember: a channel already at or past the
 * target is not asked.
 *
 * nseek == 0 doubles as "not yet positioned".  A channel starts with cur = 0,
 * and 0 is a legal warp position, so without this a channel would be scored at
 * position 0 having never been seeked -- reading whatever its state happened to
 * be initialized to.  gate.h carries the same distinction as its own `seeked`
 * flag; here the counter already records it exactly.
 */
static int
fuse_advance(WeaveFuseState *st, WeaveFuseChan *c, weave_ft_uint32 target)
{
	weave_ft_uint32 p;

	if (c->nseek > 0 && c->cur >= target)
		return 1;

	p = c->ops->seek(c, target);
	c->nseek++;
	if (p < target)
	{
		st->badchan = c;
		return 0;
	}
	c->cur = p;
	if (c->blkend < p)
		c->blkend = p;
	return 1;
}

/*
 * Advance every required channel to the intersection at or after *pp.  On
 * return either *pp is a position every required channel stands on, or *pp is
 * WEAVE_FUSE_END.  Counts a skip whenever the intersection moved the pivot
 * further than it was asked to -- that number is the mechanism behind claim 3
 * and it is measured, not assumed.
 */
static int
fuse_intersect(WeaveFuseState *st, weave_ft_uint32 *pp)
{
	weave_ft_uint32 p = *pp;

	for (;;)
	{
		int			i;
		weave_ft_uint32 hi = p;

		for (i = 0; i < st->nrq; i++)
		{
			WeaveFuseChan *c = st->rq[i];

			if (!fuse_advance(st, c, p))
				return 0;
			if (c->cur == WEAVE_FUSE_END)
			{
				*pp = WEAVE_FUSE_END;
				return 1;
			}
			if (c->cur > hi)
				hi = c->cur;
		}

		if (hi == p)
			break;
		st->nrqskip++;
		p = hi;
	}

	*pp = p;
	return 1;
}

/*
 * Pivot over the essential scored channels: the smallest position any of them
 * can reach.  A document no essential channel reaches cannot beat theta, by the
 * partition invariant, so it is not a candidate.  Returns WEAVE_FUSE_END when
 * they are all exhausted; clears *ok on a (C1) violation.
 */
static weave_ft_uint32
fuse_pivot(WeaveFuseState *st, weave_ft_uint32 p, int *ok)
{
	weave_ft_uint32 lo = WEAVE_FUSE_END;
	int			i;

	*ok = 1;
	for (i = 0; i < st->split; i++)
	{
		WeaveFuseChan *c = st->sc[i];

		if (!fuse_advance(st, c, p))
		{
			*ok = 0;
			return WEAVE_FUSE_END;
		}
		if (c->cur < lo)
			lo = c->cur;
	}
	return lo;
}

WeaveFuseError
weave_fuse_run(WeaveFuseState *st)
{
	weave_ft_uint32 p = 0;

	for (;;)
	{
		int			ok;
		int			j;
		int			i;
		float		ub;
		float		s;
		weave_ft_uint32 rend;

		if (p == WEAVE_FUSE_END || p >= st->nwarp)
			break;

		/*
		 * Global termination: once the heap is full and the sum of EVERY scored
		 * channel's ceiling cannot beat theta, no unvisited document can enter.
		 * fuse.h explains which three cases this one test subsumes.
		 */
		if (st->nheap == st->k && st->suffix[0] <= st->theta)
			break;

		/* 1. The required channels intersect, and may move the pivot forward. */
		if (st->nrq > 0)
		{
			if (!fuse_intersect(st, &p))
				return WEAVE_FUSE_C1_VIOLATION;
			if (p == WEAVE_FUSE_END || p >= st->nwarp)
				break;
		}

		/*
		 * 2. The essential scored channels pick the pivot.  With required
		 * channels present the candidate is already pinned to their
		 * intersection, so an essential channel landing past it means no scored
		 * channel reaches p: S(p) is bounded by the non-essential suffix, which
		 * is <= theta, so p is not a candidate and the scan resumes at the
		 * essential channels' own position.
		 */
		if (st->split > 0)
		{
			weave_ft_uint32 q = fuse_pivot(st, p, &ok);

			if (!ok)
				return WEAVE_FUSE_C1_VIOLATION;
			if (q > p)
			{
				p = q;
				continue;
			}
		}
		else if (st->nrq == 0)
			break;				/* nothing left that can produce a candidate */

		st->npivot++;

		/* 3. (C6), applied exactly once, here.  A channel must never look. */
		if (!fuse_live(st, p))
		{
			st->nlivedrop++;
			p++;
			continue;
		}

		/*
		 * 4. Bring EVERY scored channel to p, then bound.
		 *
		 * THE NON-ESSENTIAL CHANNELS MUST BE ADVANCED HERE, and sect. 3's
		 * pseudocode does not say so.  Its contributing test is
		 * "cur <= p <= blkend", which reads as though it were a filter over
		 * channels that happen to be positioned nearby; but a non-essential
		 * channel is never seeked by pivot selection, so it sits wherever it was
		 * left -- at the start of the scan, position 0 with blkend 0 -- and the
		 * test EXCLUDES it for every p > 0.  Its contribution is then silently
		 * dropped from the fused score, every document scores too low by that
		 * channel's amount, and the top-k is wrong in a way that still returns k
		 * plausible rows in a plausible order.  Found by the property test on its
		 * second run, at 40,574 disagreements out of 303,073 comparisons.
		 *
		 * What MaxScore actually saves is that a non-essential channel does not
		 * generate CANDIDATES -- the scan never visits a document only it
		 * matches -- and that incremental abandonment can stop before its
		 * score() is called.  It does not save the seek.  A lazier variant that
		 * defers even the seek is an optimization to measure, not a correctness
		 * question, and it is not attempted here.
		 *
		 * With every channel at or past p, the contributing set is exactly
		 * {cur == p} and the bound gets TIGHTER than sect. 3's: a channel with
		 * cur > p contributes 0 at p, so its bound is not summed at all.  The
		 * range the skip may cover shrinks to match -- see `rend` below -- since
		 * such a channel stops contributing 0 the moment the range reaches its
		 * cur.  A required channel is excluded from both: fuse.h note 2 explains
		 * that summing a gate's +INF bound would make ub infinite and kill the
		 * prune outright, and that excluding it is sound because a gate
		 * contributes 0.0 where it matches and -INF where it does not.
		 */
		for (i = 0; i < st->nsc; i++)
		{
			if (!fuse_advance(st, st->sc[i], p))
				return WEAVE_FUSE_C1_VIOLATION;
		}

		ub = 0.0f;
		st->ncontrib = 0;
		rend = WEAVE_FUSE_END - 1;
		for (i = 0; i < st->nsc; i++)
		{
			WeaveFuseChan *c = st->sc[i];

			if (c->cur == p)
			{
				float		b = c->weight * c->ops->block_max(c);

				if (fuse_isnan(b))
				{
					st->badchan = c;
					return WEAVE_FUSE_NAN_SCORE;
				}
				st->contrib[st->ncontrib] = i;
				st->cbound[st->ncontrib] = b;
				st->ncontrib++;
				ub += b;
				if (c->blkend < rend)
					rend = c->blkend;
			}
			else if (c->cur - 1 < rend)
				rend = c->cur - 1;	/* contributes 0 until it reaches cur */
		}

		if (ub <= st->theta)
		{
			/*
			 * Skip the whole range [p, rend] rather than the one document -- the
			 * step FUSED_TOPK.md sect. 2 says RRF cannot do.  ub bounds every
			 * document in that range by construction of rend above.  Required
			 * channels do not constrain it: where they match they add 0.0, and
			 * where they do not the document is vetoed anyway.
			 *
			 * rend >= p is an INVARIANT, not a hope: every scored channel was
			 * just advanced to at least p, so a contributing one has blkend >= p
			 * and a non-contributing one has cur > p.  It is checked rather than
			 * assumed because the failure is not a wrong answer, it is a HANG --
			 * rend < p sets p backwards and the scan cycles between two positions
			 * forever.  A mutation run found this by accident, with a leg that
			 * advanced only the essential channels: a non-essential channel left
			 * behind at 4 while the pivot was 7 produced rend = 3, and the loop
			 * ran until it was killed.  A hang is a worse test result than a
			 * failure, because it reports nothing at all, so the condition that
			 * can only arise from a bug is refused with a name.
			 */
			if (rend < p)
			{
				st->badchan = NULL;
				return WEAVE_FUSE_C1_VIOLATION;
			}
			st->nblkskip++;
			if (rend >= WEAVE_FUSE_END - 1)
				break;
			p = rend + 1;
			continue;
		}

		/*
		 * 5. Score, with incremental abandonment.  csuffix[j] is the sum of the
		 * bounds of the contributing channels NOT YET scored -- a suffix sum, so
		 * hazard 3's INF - INF never arises.
		 */
		st->csuffix[st->ncontrib] = 0.0f;
		for (j = st->ncontrib - 1; j >= 0; j--)
			st->csuffix[j] = st->csuffix[j + 1] + st->cbound[j];

		s = 0.0f;

		/*
		 * Required channels first, always: a predicate failure discards the
		 * document before any posting decode or SIMD block score runs.  They are
		 * standing on p already (step 1), so this is the confirmation (C5)
		 * permits a coarse gate to need.
		 */
		for (i = 0; i < st->nrq; i++)
		{
			WeaveFuseChan *c = st->rq[i];
			float		v;

			v = c->weight * c->ops->score(c);
			c->nscore++;
			if (fuse_isnan(v))
			{
				st->badchan = c;
				return WEAVE_FUSE_NAN_SCORE;
			}
			if (v == -INFINITY)
			{
				st->nveto++;
				goto next;
			}
			s += v;
		}

		for (j = 0; j < st->ncontrib; j++)
		{
			WeaveFuseChan *c = st->sc[st->contrib[j]];
			float		v;

			/* Standing on p already: step 4 advanced every scored channel and
			 * contrib holds exactly those that landed on it. */
			v = c->weight * c->ops->score(c);
			c->nscore++;
			if (fuse_isnan(v))
			{
				st->badchan = c;
				return WEAVE_FUSE_NAN_SCORE;
			}
			if (st->check_bounds && v > st->cbound[j] +
				FUSE_C2_RELTOL * (st->cbound[j] < 0.0f ? -st->cbound[j]
								  : st->cbound[j]))
			{
				st->badchan = c;
				return WEAVE_FUSE_C2_VIOLATION;
			}
			if (v == -INFINITY)
			{
				st->nveto++;
				goto next;
			}
			s += v;

			if (s + st->csuffix[j + 1] <= st->theta)
			{
				st->nabandon++;
				goto next;
			}
		}

		if (st->nheap < st->k || s > st->theta)
		{
			fuse_push(st, p, s);
			fuse_repartition(st);
		}

next:
		p++;
	}

	return WEAVE_FUSE_OK;
}

int
weave_fuse_drain(WeaveFuseState *st, WeaveFuseHit *out)
{
	int			n = st->nheap;
	int			i;

	/*
	 * Heapsort in place.  The root is the WORST entry, so each extraction places
	 * the worst remaining element at the end and the array is left BEST FIRST --
	 * (score DESC, warp ASC), which is already the final order.  No reversal.
	 *
	 * The first version reversed, on the reflex that heapsort ascends.  It does
	 * when the root is the minimum by the ORDER YOU WANT; here the root is the
	 * minimum by "worse", which is the opposite.  P1 could not see the mistake,
	 * because weave_fuse_reference() drains through this same function and the two
	 * agreed with each other while both were backwards.  That is what the
	 * explicit "output is in (score DESC, warp ASC) order" property in
	 * test/hegel/test_fuse_props.c is for, and it is the reason an oracle sharing
	 * ANY code with the thing it checks needs a second, absolute property.
	 *
	 * out and st->heap are legitimately the same array -- weave_fuse_reference()
	 * heaps directly into the caller's output buffer -- so the copy is last and
	 * conditional.
	 */
	for (i = n - 1; i > 0; i--)
	{
		fuse_swap(&st->heap[0], &st->heap[i]);
		fuse_sift_down(st->heap, i, 0);
	}

	if (out != st->heap)
	{
		for (i = 0; i < n; i++)
			out[i] = st->heap[i];
	}

	st->nheap = 0;
	st->theta = -INFINITY;
	return n;
}

/* ---------------------------------------------------------------------------
 * The brute-force reference
 *
 * Deliberately shares NOTHING with the loop above except the heap helpers and
 * the definition of "worse": it walks every live position, seeks every channel
 * to it, and never asks for a bound.  A (C2) violation therefore cannot move the
 * reference, which is what makes it a usable oracle for the one failure mode the
 * contract exists to catch.
 * ------------------------------------------------------------------------- */

int
weave_fuse_reference(WeaveFuseChan **chan, int nchan,
					 const weave_ft_uint64 *live, weave_ft_uint32 nwarp,
					 int k, WeaveFuseHit *out)
{
	WeaveFuseState st;
	weave_ft_uint32 p;
	int			i;
	int			nsc;

	/*
	 * Borrow the state only for its heap, its liveness test and its count of
	 * scored channels; the partition and the scratch arrays go unused.  A failed
	 * init means the caller handed the reference something the scorer would have
	 * refused, and the honest answer is zero rows rather than a different top-k.
	 */
	if (weave_fuse_init(&st, chan, nchan, out, k, live, nwarp) != WEAVE_FUSE_OK)
		return 0;
	st.heap = out;
	nsc = st.nsc;

	for (p = 0; p < nwarp; p++)
	{
		float		s = 0.0f;
		int			vetoed = 0;
		int			reached = 0;

		if (!fuse_live(&st, p))
			continue;

		for (i = 0; i < nchan; i++)
		{
			/*
			 * IN THE SCORER'S ORDER, NOT THE CALLER'S, and this is a statement
			 * about the product rather than about the test.  float32 addition is
			 * not associative, so a fused score is only well defined once the
			 * summation order is: required channels first, then scored channels by
			 * descending weighted ceiling.  Summing the caller's order instead
			 * produced last-ULP differences on 771 of 812,179 comparisons -- the
			 * same rows, with scores differing in the seventh digit -- the moment
			 * the generator was given a fine score lattice.  Nothing was wrong
			 * with either sum; there were two definitions of one number.
			 *
			 * The consequence to keep in mind for F2 and F3: anything that changes
			 * the order changes the last bits of score(), and two documents whose
			 * true scores differ by less than an ULP can then swap places.  The
			 * order must therefore come from the channel set, which is stable, and
			 * never from the plan or from which channels happened to be pruned.
			 */
			WeaveFuseChan *c = (i < st.nrq) ? st.rq[i] : st.sc[i - st.nrq];
			float		v;

			if (c->nseek == 0 || c->cur < p)
			{
				c->cur = c->ops->seek(c, p);
				c->nseek++;
				if (c->blkend < c->cur)
					c->blkend = c->cur;
			}

			if (c->cur != p)
			{
				/*
				 * A scored channel that does not reach p contributes nothing; a
				 * required one that does not reach p is not matched, and the
				 * document is out.  This asymmetry is the whole of fuse.h note
				 * 2, stated once more here because the reference is what proves
				 * the fast path implements it.
				 */
				if (c->required)
				{
					vetoed = 1;
					break;
				}
				continue;
			}

			if (!c->required)
				reached = 1;

			v = c->weight * c->ops->score(c);
			c->nscore++;
			if (v == -INFINITY)
			{
				vetoed = 1;
				break;
			}
			s += v;
		}

		if (vetoed)
			continue;

		/*
		 * THE CANDIDATE SET, which FUSED_TOPK.md sect. 3 never states and which
		 * the property test found the hard way.  A position no SCORED channel
		 * reaches is not a result: it matches nothing, its fused score is 0, and
		 * padding the top-k with such rows would answer "the ten best documents"
		 * with documents that contain none of the query.  So the candidate set is
		 * the UNION of the scored channels' positions, intersected with the
		 * required channels'; with no scored channel at all the required
		 * intersection is the candidate set on its own.
		 *
		 * The fast path gets this from pivot selection, which only ever proposes
		 * a position some essential scored channel reaches -- so the two agree,
		 * but they agree by a DEFINITION that had to be written down rather than
		 * by accident.  Note also that sect. 3's own proof does not cover it: the
		 * pivot-skip case argues S(d) <= sum of non-essential ceilings <= theta,
		 * and at the start of a scan every channel is essential, so that sum is 0
		 * and theta is -INF.  The skip is right; the justification for it is this
		 * definition, not that inequality.
		 */
		if (!reached && nsc > 0)
			continue;

		if (st.nheap < st.k || s > st.theta)
			fuse_push(&st, p, s);
	}

	return weave_fuse_drain(&st, out);
}
