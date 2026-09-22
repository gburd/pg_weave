/*-------------------------------------------------------------------------
 *
 * test_fuse_props.c
 *		Task F5: the fused-threshold top-k core (include/weave/fuse.h,
 *		src/am/fuse.c) returns EXACTLY the brute-force top-k.
 *
 * THIS IS F1's GATE AS WELL AS F5's.  F1's stated gate, `sql/fuse_degenerate.sql`,
 * needs SQL surface to invoke the scorer, which is task F2 and is blocked under the
 * rule-7 waiver in doc/PHASES.md.  Three of the five degenerate shapes in
 * FUSED_TOPK.md sect. 4 are expressed here as generator modes instead, which is the
 * stronger form anyway: an expected-output file fixes one corpus, and the failure
 * mode of a fused scorer is a bound or a skip that is wrong on a DIFFERENT corpus.
 *
 * WHY A BRUTE-FORCE ORACLE AND NOT ASSERTIONS ABOUT THE PRUNES.  Every prune in
 * FUSED_TOPK.md sect. 3 is a claim that a set of documents CANNOT beat the
 * threshold.  When such a claim is wrong the query still returns k plausible rows;
 * it returns the wrong ones.  So the only useful oracle is the answer itself,
 * computed a way that shares nothing with the prunes -- weave_fuse_reference() uses
 * seek() and score() and never asks for a bound.
 *
 * Properties, over randomly generated channel sets (scored and boolean, dense and
 * sparse, blocked and degenerate, with and without tombstones):
 *
 *	P1	the fused top-k equals the reference top-k, position for position and
 *		score for score -- INCLUDING the tie-break, which is (score DESC, warp ASC)
 *	P2	the core never seeks a channel backwards.  Asserted by the synthetic
 *		channel itself, because this is the hazard FUSED_TOPK.md sect. 3's own
 *		pseudocode walks into (fuse.h note 1) and both real shuttles REFUSE it
 *	P3	score() is only ever called at the position the channel last returned,
 *		which is what contract (C4) means by "the exact contribution at s->cur"
 *	P4	the fused scan calls score() no more often than the reference does, and
 *		the ratio is reported.  NOT a gate: FUSED_TOPK.md sect. 8's ratio is
 *		against RRF on a real corpus, and synthetic data cannot stand in for it
 *	P5	no tombstoned position is ever returned, and no channel is ever asked
 *		about liveness -- (C6) is the scorer's job and the synthetic channels have
 *		entries at dead positions precisely so a leak would show
 *	P6	a required (boolean) channel is applied conjunctively: every returned
 *		position matches EVERY required channel.  This is fuse.h note 2, the
 *		silent-filter-loss (C5) invites, and it is checked directly rather than
 *		inferred from P1
 *	P7	the (C2) check catches an under-reported bound when check_bounds is set
 *	P8	THE POSITIVE CONTROL, and it is the reason to believe P1 is not vacuous.
 *		With check_bounds OFF and one channel's block_max deliberately scaled to
 *		0.99, the fused answer MUST sometimes differ from the reference.  The run
 *		fails if it never does -- a test that cannot see a 1 %-too-low bound is
 *		not testing hard rule 1, and AGENTS.md records what happens when a guard's
 *		output is identical under every input the harness can produce
 *
 * Build and run:
 *		cc -O2 -Wall -Wextra -I include -o /tmp/tf test/hegel/test_fuse_props.c \
 *			src/am/fuse.c -lm && /tmp/tf
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  test/hegel/test_fuse_props.c
 *
 *-------------------------------------------------------------------------
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weave/fuse.h"

static long failures = 0;
static long checks = 0;
static long prop_checks[10];

#define CHECK(prop, cond, ...)											\
	do {																\
		checks++;														\
		prop_checks[prop]++;											\
		if (!(cond))													\
		{																\
			if (failures < 12)											\
			{															\
				printf("P%d FAIL %s:%d: ", (prop), __FILE__, __LINE__);	\
				printf(__VA_ARGS__);									\
				printf("\n");											\
			}															\
			failures++;													\
		}																\
	} while (0)

/* xorshift64*; the generator must be reproducible without libc's rand(). */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t
rnd64(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint32_t
rnd_below(uint32_t n)
{
	return n == 0 ? 0 : (uint32_t) (rnd64() % n);
}

/*
 * TWO SCORE LATTICES, AND BOTH ARE NEEDED.
 *
 * Coarse (multiples of 0.25) makes TIES common, and ties are where a tie-break bug
 * hides -- a uniform float32 almost never produces one.
 *
 * Fine (a 20-bit fraction) makes NEAR-ties common, and near-ties are where a prune
 * that is slightly too aggressive hides.  The first mutation run had only the coarse
 * lattice and two legs SURVIVED because of it: shifting a prune's threshold by
 * 0.001 cannot change any outcome when every score sum is a multiple of 0.0625.
 * That is a generator limitation masquerading as a robust core, which is the
 * failure mode AGENTS.md's ninth verification-error member warns about -- so the
 * lattice is part of the generated case, not a constant.
 */
static float
rnd_score(int fine)
{
	if (fine)
		return (float) rnd_below(1u << 20) * (8.0f / (float) (1u << 20));
	return (float) rnd_below(33) * 0.25f;
}

/*
 * P8's legs: how often a bound scaled to this fraction of the truth changes the
 * ANSWER, with the (C2) check off.  Two magnitudes, because the rate is the point:
 * see the note where they are reported.
 */
static const float sab_scale[2] = {0.99f, 0.90f};
static long n_sabotage_trials[2];
static long n_sabotage_diffs[2];
static int	cur_sab_leg = 0;

/* ---------------------------------------------------------------------------
 * The synthetic channel
 *
 * An explicit entry list plus a block width.  The warp is partitioned into fixed
 * ranges of `bs` positions; block_max() returns the maximum entry score inside
 * the range holding `cur`, which is a TRUE and TIGHT bound, and blkend is the end
 * of that range.  bs == 1 gives the degenerate per-document bound a gate uses.
 *
 * Three flavours:
 *	 SCORED		additive; score() is the entry's value
 *	 GATE		required, exact: seek() lands on an entry, cur == blkend, and
 *				score() is 0.0 -- the include/weave/gate.h shape
 *	 COARSE		required, block-granular: seek() answers with the target itself
 *				whenever the target's block holds any entry, so cur may be a
 *				NON-match and score() returns -INF there.  (C5) permits this and
 *				a docvalues predicate over a block of values is exactly it; it is
 *				the flavour that exercises the veto path
 * ------------------------------------------------------------------------- */

typedef enum ChanFlavour
{
	CH_SCORED = 0,
	CH_GATE,
	CH_COARSE
} ChanFlavour;

#define MAXENT	4096

typedef struct SynthSpec
{
	ChanFlavour flavour;
	uint32_t	pos[MAXENT];
	float		sc[MAXENT];
	int			n;
	uint32_t	bs;
	float		weight;
	float		maxscore;
	float		bound_scale;	/* 1.0 sound; < 1.0 is the P8 sabotage */
} SynthSpec;

typedef struct SynthChan
{
	WeaveFuseChan ch;
	const SynthSpec *spec;
	int			idx;			/* entry index at/after cur, or n */
	uint32_t	last_ret;
	int			seeked;
	int			bad_backward;	/* the core seeked below a previous return */
	int			bad_offpos;		/* score() asked away from the last return */
} SynthChan;

static uint32_t
blk_start(const SynthSpec *sp, uint32_t p)
{
	return p - (p % sp->bs);
}

static uint32_t
blk_end(const SynthSpec *sp, uint32_t p)
{
	uint32_t	s = blk_start(sp, p);

	if (WEAVE_FUSE_END - sp->bs < s)
		return WEAVE_FUSE_END - 1;
	return s + sp->bs - 1;
}

/* First entry index with pos >= t, by linear scan from 0: the oracle's own
 * lookup, deliberately not the galloping one under test elsewhere. */
static int
first_at(const SynthSpec *sp, uint32_t t)
{
	int			i;

	for (i = 0; i < sp->n; i++)
	{
		if (sp->pos[i] >= t)
			return i;
	}
	return sp->n;
}

static weave_ft_uint32
synth_seek(WeaveFuseChan *c, weave_ft_uint32 target)
{
	SynthChan  *s = (SynthChan *) c->state;
	const SynthSpec *sp = s->spec;
	int			i;
	uint32_t	ret;

	if (s->seeked && target < s->last_ret)
		s->bad_backward = 1;
	s->seeked = 1;

	i = first_at(sp, target);
	s->idx = i;

	if (sp->flavour == CH_COARSE)
	{
		/*
		 * Block granularity: if the target's own block holds an entry, the block
		 * "may match" and the answer is the target itself.
		 */
		if (i < sp->n && blk_start(sp, sp->pos[i]) == blk_start(sp, target))
			ret = target;
		else if (i < sp->n)
			ret = blk_start(sp, sp->pos[i]);
		else
			ret = WEAVE_FUSE_END;
	}
	else
		ret = (i < sp->n) ? sp->pos[i] : WEAVE_FUSE_END;

	if (ret == WEAVE_FUSE_END)
		c->blkend = WEAVE_FUSE_END;
	else if (sp->flavour == CH_GATE)
		c->blkend = ret;		/* for a gate the skip IS the seek (gate.h) */
	else
		c->blkend = blk_end(sp, ret);

	s->last_ret = ret;
	return ret;
}

static float
synth_block_max(WeaveFuseChan *c)
{
	SynthChan  *s = (SynthChan *) c->state;
	const SynthSpec *sp = s->spec;
	uint32_t	lo;
	uint32_t	hi;
	float		m;
	int			i;

	if (sp->flavour != CH_SCORED)
		return INFINITY;		/* (C5) */

	if (c->cur == WEAVE_FUSE_END)
		return -INFINITY;

	lo = blk_start(sp, c->cur);
	hi = blk_end(sp, c->cur);
	m = -INFINITY;
	for (i = 0; i < sp->n; i++)
	{
		if (sp->pos[i] >= lo && sp->pos[i] <= hi && sp->sc[i] > m)
			m = sp->sc[i];
	}
	if (m == -INFINITY)
		return -INFINITY;
	return m * sp->bound_scale;
}

static float
synth_score(WeaveFuseChan *c)
{
	SynthChan  *s = (SynthChan *) c->state;
	const SynthSpec *sp = s->spec;
	int			i;

	/* (C4)/P3: the contribution is defined at cur, and cur must be where this
	 * channel last answered. */
	if (!s->seeked || c->cur != s->last_ret)
		s->bad_offpos = 1;

	if (sp->flavour == CH_GATE)
		return 0.0f;

	for (i = 0; i < sp->n; i++)
	{
		if (sp->pos[i] == c->cur)
			return sp->flavour == CH_COARSE ? 0.0f : sp->sc[i];
	}
	/* A coarse gate landing inside a may-match block on a non-match. */
	return -INFINITY;
}

static const WeaveFuseChanOps synth_ops = {
	synth_seek, synth_block_max, synth_score
};

static void
synth_bind(SynthChan *s, const SynthSpec *sp)
{
	memset(s, 0, sizeof(*s));
	s->spec = sp;
	s->idx = 0;
	s->last_ret = 0;
	s->seeked = 0;
	s->ch.ops = &synth_ops;
	s->ch.state = s;
	s->ch.cur = 0;
	s->ch.blkend = 0;
	s->ch.weight = sp->weight;
	s->ch.maxscore = sp->maxscore;
	s->ch.required = (sp->flavour != CH_SCORED);
	s->ch.nseek = 0;
	s->ch.nscore = 0;
}

/* ---------------------------------------------------------------------------
 * Generators
 * ------------------------------------------------------------------------- */

typedef enum TrialMode
{
	MODE_MIXED = 0,				/* anything */
	MODE_ONE_SCORED,			/* FUSED_TOPK sect. 4: reduces to block-max WAND */
	MODE_ALL_BOOLEAN,			/* reduces to a bitmap AND, no scoring */
	MODE_FLAT,					/* every score equal: theta never rises */
	MODE_NMODES
} TrialMode;

static void
gen_spec(SynthSpec *sp, uint32_t nwarp, TrialMode mode, int allow_sabotage)
{
	uint32_t	i;
	uint32_t	density;
	float		flat;
	int			fine = (int) rnd_below(2);

	memset(sp, 0, sizeof(*sp));

	switch (mode)
	{
		case MODE_ALL_BOOLEAN:
			sp->flavour = rnd_below(2) ? CH_GATE : CH_COARSE;
			break;
		case MODE_ONE_SCORED:
		case MODE_FLAT:
			sp->flavour = CH_SCORED;
			break;
		default:
			{
				uint32_t	r = rnd_below(10);

				sp->flavour = r < 6 ? CH_SCORED : (r < 8 ? CH_GATE : CH_COARSE);
				break;
			}
	}

	sp->bs = 1u << rnd_below(5);	/* 1, 2, 4, 8, 16 */
	if (sp->flavour == CH_GATE)
		sp->bs = 1;
	sp->weight = 0.25f + (float) rnd_below(8) * 0.5f;
	sp->bound_scale = 1.0f;
	if (allow_sabotage)
		sp->bound_scale = sab_scale[cur_sab_leg];

	/* Entry positions: ascending, distinct, covering roughly `density` percent. */
	density = 1 + rnd_below(100);
	flat = 1.5f;
	for (i = 0; i < nwarp && sp->n < MAXENT; i++)
	{
		if (rnd_below(100) < density)
		{
			sp->pos[sp->n] = i;
			sp->sc[sp->n] = (mode == MODE_FLAT) ? flat : rnd_score(fine);
			sp->n++;
		}
	}

	/* The ceiling the scorer is handed.  Sometimes exactly the maximum, sometimes
	 * slack -- a slack ceiling is legal (it is an upper bound) and must not change
	 * the answer, only the pruning. */
	if (sp->flavour == CH_SCORED)
	{
		float		m = 0.0f;
		int			j;

		for (j = 0; j < sp->n; j++)
		{
			if (sp->sc[j] > m)
				m = sp->sc[j];
		}
		sp->maxscore = rnd_below(4) == 0 ? m + (float) rnd_below(4) : m;
	}
	else
		sp->maxscore = INFINITY;	/* (C5); the core must not let this into a sum */
}

/* ---------------------------------------------------------------------------
 * One trial
 * ------------------------------------------------------------------------- */

#define MAXCHAN		6
#define MAXWARP		512
#define MAXK		16

/*
 * Aggregate counters, printed at the end.  AGENTS.md's ninth verification-error
 * member generalizes to "a result needs evidence that the specific thing you meant
 * to run, ran": a suite that never triggers the block prune is not testing the
 * block prune, however many comparisons it makes.  These are that evidence, and the
 * run FAILS if any prune path stayed at zero.
 */
static long long agg_pivot = 0;
static long long agg_blkskip = 0;
static long long agg_rqskip = 0;
static long long agg_veto = 0;
static long long agg_abandon = 0;
static long long agg_livedrop = 0;
static long long agg_split = 0;

/*
 * P4's MEASUREMENT, not just its assertion.  FUSED_TOPK.md sect. 8 makes the ratio
 * of score() calls against the control the row that decides whether the design
 * means anything -- "if the score() call ratio is not dramatically lower, stop and
 * fix the bounds".  The control there is RRF on a real corpus and this is neither,
 * so the number below is NOT that gate and must never be quoted as it.  What it is
 * worth: a ratio of 1.000 here would mean the prunes remove no scoring work at all
 * on ANY input, which sect. 8 says is the stop-everything signal, and finding that
 * out costs nothing and needs no corpus.  agg_bmax is beside it because a design
 * that trades score() calls for block_max() calls has moved work rather than
 * removed it (include/weave/fuse.h, WeaveFuseChan.nbmax).
 */
static long long agg_fscore = 0;
static long long agg_rscore = 0;
static long long agg_fseek = 0;
static long long agg_rseek = 0;
static long long agg_fbmax = 0;
static long long agg_score_win = 0;

static void
one_trial(TrialMode mode, int sabotage)
{
	SynthSpec	spec[MAXCHAN];
	SynthChan	fast[MAXCHAN];
	SynthChan	ref[MAXCHAN];
	WeaveFuseChan *fchan[MAXCHAN];
	WeaveFuseChan *rchan[MAXCHAN];
	WeaveFuseHit fhit[MAXK];
	WeaveFuseHit rhit[MAXK];
	WeaveFuseHit heap[MAXK];
	uint64_t	live[(MAXWARP / 64) + 1];
	const uint64_t *livep = NULL;
	WeaveFuseState st;
	WeaveFuseError err;
	uint32_t	nwarp;
	int			nchan;
	int			nsc = 0;
	int			k;
	int			i;
	int			nf;
	int			nr;

	nwarp = 8 + rnd_below(MAXWARP - 8);
	nchan = 1 + (int) rnd_below(mode == MODE_ONE_SCORED ? 1 : MAXCHAN);
	k = 1 + (int) rnd_below(MAXK);
	if (mode == MODE_FLAT && rnd_below(2))
		k = 1;

	for (i = 0; i < nchan; i++)
	{
		gen_spec(&spec[i], nwarp, mode, sabotage && i == 0);
		if (spec[i].flavour == CH_SCORED)
			nsc++;
	}

	/* Tombstones on a third of trials, and the channels keep their entries at the
	 * dead positions on purpose (P5). */
	if (rnd_below(3) == 0)
	{
		size_t		nw = (nwarp + 63) / 64;
		size_t		w;

		for (w = 0; w < nw; w++)
			live[w] = rnd64();
		livep = live;
	}

	for (i = 0; i < nchan; i++)
	{
		synth_bind(&fast[i], &spec[i]);
		synth_bind(&ref[i], &spec[i]);
		fchan[i] = &fast[i].ch;
		rchan[i] = &ref[i].ch;
	}

	err = weave_fuse_init(&st, fchan, nchan, heap, k, livep, nwarp);
	CHECK(1, err == WEAVE_FUSE_OK, "init returned %d", (int) err);
	if (err != WEAVE_FUSE_OK)
		return;
	st.check_bounds = sabotage ? 0 : 1;

	err = weave_fuse_run(&st);
	if (sabotage)
	{
		/* An under-reported bound may or may not change this particular answer;
		 * P8 only requires that it sometimes does. */
		if (err != WEAVE_FUSE_OK)
			return;
	}
	else
	{
		CHECK(1, err == WEAVE_FUSE_OK, "run returned %d", (int) err);
		if (err != WEAVE_FUSE_OK)
			return;
	}
	agg_pivot += (long long) st.npivot;
	agg_blkskip += (long long) st.nblkskip;
	agg_rqskip += (long long) st.nrqskip;
	agg_veto += (long long) st.nveto;
	agg_abandon += (long long) st.nabandon;
	agg_livedrop += (long long) st.nlivedrop;
	if (st.split < st.nsc)
		agg_split++;

	nf = weave_fuse_drain(&st, fhit);
	nr = weave_fuse_reference(rchan, nchan, livep, nwarp, k, rhit);

	if (sabotage)
	{
		int			diff = (nf != nr);

		for (i = 0; i < nf && !diff; i++)
		{
			if (fhit[i].warp != rhit[i].warp || fhit[i].score != rhit[i].score)
				diff = 1;
		}
		n_sabotage_trials[cur_sab_leg]++;
		if (diff)
			n_sabotage_diffs[cur_sab_leg]++;
		return;
	}

	/* P1 */
	CHECK(1, nf == nr, "row count %d != reference %d (nwarp=%u nchan=%d k=%d)",
		  nf, nr, nwarp, nchan, k);
	{
		static int	ndumped = 0;
		static long	last_failures = 0;

		if (failures != last_failures && ndumped < 3)
		{
			ndumped++;
			printf("  DUMP nwarp=%u k=%d nsc=%d nrq=%d npivot=%lld "
				   "nblkskip=%lld nrqskip=%lld nveto=%lld nlivedrop=%lld "
				   "nabandon=%lld live=%d\n",
				   nwarp, k, st.nsc, st.nrq, (long long) st.npivot,
				   (long long) st.nblkskip, (long long) st.nrqskip,
				   (long long) st.nveto, (long long) st.nlivedrop,
				   (long long) st.nabandon, livep != NULL);
			for (i = 0; i < nchan; i++)
				printf("    ch%d flavour=%d bs=%u n=%d w=%.3f max=%.3f\n",
					   i, (int) spec[i].flavour, spec[i].bs, spec[i].n,
					   (double) spec[i].weight, (double) spec[i].maxscore);
		}
		last_failures = failures;
	}
	for (i = 0; i < nf && i < nr; i++)
	{
		CHECK(1, fhit[i].warp == rhit[i].warp && fhit[i].score == rhit[i].score,
			  "row %d: fused (%u, %.6f) != reference (%u, %.6f) "
			  "(nwarp=%u nchan=%d k=%d mode=%d)",
			  i, fhit[i].warp, (double) fhit[i].score,
			  rhit[i].warp, (double) rhit[i].score, nwarp, nchan, k, (int) mode);
	}

	/* P1, the tie-break itself: the output is strictly ordered. */
	for (i = 1; i < nf; i++)
	{
		CHECK(1, fhit[i - 1].score > fhit[i].score ||
			  (fhit[i - 1].score == fhit[i].score &&
			   fhit[i - 1].warp < fhit[i].warp),
			  "row %d not in (score DESC, warp ASC) order", i);
	}

	/* P2, P3 */
	for (i = 0; i < nchan; i++)
	{
		CHECK(2, !fast[i].bad_backward, "channel %d was seeked backwards", i);
		CHECK(3, !fast[i].bad_offpos, "channel %d scored off-position", i);
	}

	/* P4 */
	{
		long		fs = 0;
		long		rs = 0;

		long		fb = 0;

		for (i = 0; i < nchan; i++)
		{
			fs += (long) fast[i].ch.nscore;
			rs += (long) ref[i].ch.nscore;
			fb += (long) fast[i].ch.nbmax;
			agg_fseek += (long long) fast[i].ch.nseek;
			agg_rseek += (long long) ref[i].ch.nseek;
		}
		CHECK(4, fs <= rs, "fused made %ld score() calls, reference %ld", fs, rs);

		/*
		 * AND THE COUNTERS THEMSELVES ARE CHECKED, because they are about to be
		 * carried out to SQL and quoted.  Every score() and every block_max() the
		 * core makes happens at a position it counted as a pivot, and at most once
		 * per channel there, so npivot * nchan bounds both.  An extra increment --
		 * the obvious way to get a flattering ratio by accident -- breaks this
		 * before it reaches a results table.
		 */
		CHECK(4, fs <= (long) st.npivot * nchan,
			  "%ld score() calls over %lld pivots x %d channels",
			  fs, (long long) st.npivot, nchan);
		CHECK(4, fb <= (long) st.npivot * nchan,
			  "%ld block_max() calls over %lld pivots x %d channels",
			  fb, (long long) st.npivot, nchan);

		agg_fscore += (long long) fs;
		agg_rscore += (long long) rs;
		agg_fbmax += (long long) fb;
		if (fs < rs)
			agg_score_win++;
	}

	/* P5, P6 */
	for (i = 0; i < nf; i++)
	{
		int			j;

		CHECK(5, fhit[i].warp < nwarp, "row %d warp %u >= nwarp %u",
			  i, fhit[i].warp, nwarp);
		if (livep != NULL)
			CHECK(5, (livep[fhit[i].warp >> 6] >> (fhit[i].warp & 63)) & 1,
				  "row %d warp %u is tombstoned", i, fhit[i].warp);

		for (j = 0; j < nchan; j++)
		{
			if (spec[j].flavour == CH_SCORED)
				continue;
			CHECK(6, first_at(&spec[j], fhit[i].warp) < spec[j].n &&
				  spec[j].pos[first_at(&spec[j], fhit[i].warp)] == fhit[i].warp,
				  "row %d warp %u fails required channel %d",
				  i, fhit[i].warp, j);
		}
	}
}

/* ---------------------------------------------------------------------------
 * P7: the (C2) check fires on an under-reported bound
 * ------------------------------------------------------------------------- */

static void
c2_detection_trial(void)
{
	SynthSpec	spec;
	SynthChan	ch;
	WeaveFuseChan *chan[1];
	WeaveFuseHit heap[4];
	WeaveFuseState st;
	WeaveFuseError err;

	/*
	 * One scored channel whose block_max is scaled well below its scores.  With
	 * check_bounds set the core must report the violation rather than return a
	 * plausible answer.  bs > 1 so the bound covers a range and score() is
	 * reached; scale far enough below 1 that float tolerance cannot excuse it.
	 */
	gen_spec(&spec, 128, MODE_ONE_SCORED, 0);
	if (spec.n < 4)
		return;
	spec.bs = 8;
	spec.bound_scale = 0.5f;

	synth_bind(&ch, &spec);
	chan[0] = &ch.ch;

	err = weave_fuse_init(&st, chan, 1, heap, 4, NULL, 128);
	if (err != WEAVE_FUSE_OK)
		return;
	st.check_bounds = 1;
	err = weave_fuse_run(&st);

	/*
	 * A zero score in a block of zeros is not a violation, so only assert when
	 * the channel actually has a positive score somewhere.
	 */
	{
		int			i;
		int			positive = 0;

		for (i = 0; i < spec.n; i++)
		{
			if (spec.sc[i] > 0.0f)
				positive = 1;
		}
		if (!positive)
			return;
		CHECK(7, err == WEAVE_FUSE_C2_VIOLATION,
			  "an under-reported bound was not caught (err=%d)", (int) err);
		CHECK(7, st.badchan == &ch.ch, "the violation named the wrong channel");
	}
}

/* ---------------------------------------------------------------------------
 * Refusals at init
 * ------------------------------------------------------------------------- */

static void
init_refusal_trials(void)
{
	SynthSpec	spec;
	SynthChan	ch;
	WeaveFuseChan *chan[1];
	WeaveFuseHit heap[4];
	WeaveFuseState st;

	gen_spec(&spec, 64, MODE_ONE_SCORED, 0);

	/* A zero weight on a gate's +INF ceiling is the 0 * INF trap (fuse.h note 3);
	 * on a scored channel it is merely useless.  Both are refused. */
	synth_bind(&ch, &spec);
	ch.ch.weight = 0.0f;
	chan[0] = &ch.ch;
	CHECK(7, weave_fuse_init(&st, chan, 1, heap, 4, NULL, 64) ==
		  WEAVE_FUSE_BAD_WEIGHT, "a zero weight was accepted");

	synth_bind(&ch, &spec);
	ch.ch.weight = -1.0f;
	CHECK(7, weave_fuse_init(&st, chan, 1, heap, 4, NULL, 64) ==
		  WEAVE_FUSE_BAD_WEIGHT, "a negative weight was accepted");

	synth_bind(&ch, &spec);
	ch.ch.weight = INFINITY;
	CHECK(7, weave_fuse_init(&st, chan, 1, heap, 4, NULL, 64) ==
		  WEAVE_FUSE_BAD_WEIGHT, "an infinite weight was accepted");

	/* A scored channel may not have an infinite ceiling: it would enter suffix[]
	 * and disable both the partition and the block prune. */
	synth_bind(&ch, &spec);
	ch.ch.required = 0;
	ch.ch.maxscore = INFINITY;
	CHECK(7, weave_fuse_init(&st, chan, 1, heap, 4, NULL, 64) ==
		  WEAVE_FUSE_BAD_MAXSCORE, "an infinite scored ceiling was accepted");

	/* ...but a REQUIRED channel's +INF ceiling is exactly what (C5) prescribes and
	 * must be accepted, because the core keeps it out of the arithmetic instead. */
	synth_bind(&ch, &spec);
	ch.ch.required = 1;
	ch.ch.maxscore = INFINITY;
	CHECK(7, weave_fuse_init(&st, chan, 1, heap, 4, NULL, 64) == WEAVE_FUSE_OK,
		  "a gate's +INF ceiling was refused");

	synth_bind(&ch, &spec);
	CHECK(7, weave_fuse_init(&st, chan, 1, heap, 0, NULL, 64) == WEAVE_FUSE_BAD_K,
		  "k = 0 was accepted");
	CHECK(7, weave_fuse_init(&st, chan, 0, heap, 4, NULL, 64) ==
		  WEAVE_FUSE_BAD_NCHAN, "zero channels was accepted");
}

int
main(int argc, char **argv)
{
	long		trials = 0;
	long		i;
	long		ntrial = 1000000;

	if (argc > 1)
		ntrial = atol(argv[1]);

	printf("== F1/F5 fused top-k: fused == brute force, no backward seek, "
		   "conjunctive gates ==\n");

	for (i = 0; i < ntrial; i++)
	{
		TrialMode	mode;
		uint32_t	r = rnd_below(100);

		if (r < 55)
			mode = MODE_MIXED;
		else if (r < 70)
			mode = MODE_ONE_SCORED;
		else if (r < 85)
			mode = MODE_ALL_BOOLEAN;
		else
			mode = MODE_FLAT;

		one_trial(mode, 0);
		trials++;
	}

	/* P8: the same generator with one channel's bound scaled below the truth and
	 * the (C2) check off, to prove the oracle can see what hard rule 1 is about. */
	for (cur_sab_leg = 0; cur_sab_leg < 2; cur_sab_leg++)
	{
		for (i = 0; i < ntrial / 20 + 20000; i++)
		{
			one_trial(MODE_MIXED, 1);
			trials++;
		}
	}

	for (i = 0; i < 4000; i++)
		c2_detection_trial();
	init_refusal_trials();

	printf("trials: %ld\n", trials);
	printf("P1 fused == brute force  : %8ld checks\n", prop_checks[1]);
	printf("P2 no backward seek      : %8ld checks\n", prop_checks[2]);
	printf("P3 score() at cur only   : %8ld checks\n", prop_checks[3]);
	printf("P4 score() calls <= ref  : %8ld checks\n", prop_checks[4]);
	printf("P5 no dead row returned  : %8ld checks\n", prop_checks[5]);
	printf("P6 gates are conjunctive : %8ld checks\n", prop_checks[6]);
	printf("P7 refusals and (C2)     : %8ld checks\n", prop_checks[7]);
	/*
	 * P8, and the RATE is the finding rather than the pass.  A bound 1 % too low
	 * changes the answer in a fraction of a percent of queries; a bound 10 % too
	 * low changes it in a few percent.  That is exactly why AGENTS.md hard rule 1
	 * says no fixed-expected-output regression test can catch a loose bound: on
	 * any one corpus it almost always returns the right rows, and the queries it
	 * ruins are not the ones anybody pinned.  A low rate here is not a weak test,
	 * it is the measurement of how silent the failure is -- but zero would mean
	 * the harness cannot see it at all, and that fails the run.
	 */
	for (i = 0; i < 2; i++)
		printf("P8 bound scaled to %.2f   : %8ld of %ld trials answered "
			   "differently (%.3f%%)\n", (double) sab_scale[i],
			   n_sabotage_diffs[i], n_sabotage_trials[i],
			   n_sabotage_trials[i] ? 100.0 * (double) n_sabotage_diffs[i] /
			   (double) n_sabotage_trials[i] : 0.0);
	printf("exercised: pivot=%lld blkskip=%lld rqskip=%lld veto=%lld "
		   "abandon=%lld livedrop=%lld trials-that-demoted-a-channel=%lld\n",
		   agg_pivot, agg_blkskip, agg_rqskip, agg_veto, agg_abandon,
		   agg_livedrop, agg_split);

	/* SYNTHETIC, and labelled so in the output itself: this is not sect. 8's row.
	 * See the comment on agg_fscore. */
	printf("P4 work vs reference (SYNTHETIC, not the sect. 8 gate): "
		   "score %lld/%lld = %.3f, seek %lld/%lld = %.3f, "
		   "fused block_max %lld; %lld of %ld trials pruned a score() call\n",
		   agg_fscore, agg_rscore,
		   agg_rscore ? (double) agg_fscore / (double) agg_rscore : 0.0,
		   agg_fseek, agg_rseek,
		   agg_rseek ? (double) agg_fseek / (double) agg_rseek : 0.0,
		   agg_fbmax, agg_score_win, trials);

	/* Every prune must have fired.  A zero here means the generator stopped
	 * reaching a path, not that the path is correct. */
	if (agg_score_win == 0)
	{
		printf("P4 FAIL: the fused scan never once made fewer score() calls than "
			   "the exhaustive reference, so the prunes remove no scoring work on "
			   "any input this generator can produce -- FUSED_TOPK.md sect. 8's "
			   "stop-everything signal\n");
		failures++;
	}

	if (agg_blkskip == 0 || agg_rqskip == 0 || agg_veto == 0 ||
		agg_abandon == 0 || agg_livedrop == 0 || agg_split == 0)
	{
		printf("COVERAGE FAIL: a prune path was never exercised, so its "
			   "correctness is untested rather than established\n");
		failures++;
	}

	for (i = 0; i < 2; i++)
	{
		if (n_sabotage_trials[i] > 0 && n_sabotage_diffs[i] == 0)
		{
			printf("P8 FAIL: a bound scaled to %.2f never changed the answer, so "
				   "this test cannot see the failure mode hard rule 1 exists "
				   "for\n", (double) sab_scale[i]);
			failures++;
		}
	}

	if (failures > 0)
	{
		printf("FAILED -- %ld checks, %ld failures\n", checks, failures);
		return 1;
	}
	printf("%ld checks, %ld failures\n", checks, failures);
	return 0;
}
