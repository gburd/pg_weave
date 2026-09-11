/*-------------------------------------------------------------------------
 *
 * surftrie.c
 *		Builder, reader and validator for the SuRF trie over the bolt
 *		vocabulary (LOUDS-Sparse).  Task Z3.
 *
 * Read include/weave/surftrie.h first: it carries the layout, the one-sided
 * error contract, and the reason this file has no PostgreSQL dependency and no
 * allocation.  doc/specs/FUZZY_CHANNEL.md sect. 3 derives the format.
 *
 * The four things in here that are easy to get wrong, and where they are:
 *
 *	 1. TERMS THAT ARE PREFIXES OF EACH OTHER ("a", "ab", "abc").  The classic
 *		trie bug.  Handled by st_walk()'s run detection: a slot is terminal iff
 *		the FIRST term of its run has length exactly d, which is exactly the
 *		"the prefix is itself a term" case, and it is independent of whether the
 *		slot also has children.
 *
 *	 2. THE MEASURE PASS AND THE EMIT PASS DISAGREEING, which is a buffer
 *		overrun.  Not expressible: both are st_walk() driven by one StSink, and
 *		the only difference is whether the sink's output pointers are NULL.
 *
 *	 3. A CORRUPT ACCELERATOR GIVING A WRONG ANSWER RATHER THAN A CRASH.
 *		st_check_structure() recomputes the rank superblocks and the select
 *		samples and rejects a mismatch, because navigating to the wrong node is
 *		a FALSE NEGATIVE and no sanitizer or regression test can see it.
 *
 *	 4. A WALK THAT NEVER TERMINATES on hostile bytes.  st_check_structure()
 *		requires every child node to start after its parent slot, so slot
 *		indices strictly increase along any path; the walkers additionally cap
 *		depth at WEAVE_SURFTRIE_MAX_DEPTH so their fixed-size stacks are enough.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/query/surftrie.c
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "weave/surftrie.h"

/* ---------------------------------------------------------------------------
 * Little-endian scalar access, and bits
 *
 * Byte-wise on purpose: the image is parsed at whatever offset a page writer
 * chose, so there is no alignment to rely on, and byte-wise assembly makes the
 * bytes identical on a big-endian host.
 * ------------------------------------------------------------------------- */

static weave_st_uint32
st_rd32(const weave_st_uint8 *p)
{
	return (weave_st_uint32) p[0]
		| ((weave_st_uint32) p[1] << 8)
		| ((weave_st_uint32) p[2] << 16)
		| ((weave_st_uint32) p[3] << 24);
}

static weave_st_uint16
st_rd16(const weave_st_uint8 *p)
{
	return (weave_st_uint16) ((weave_st_uint32) p[0] | ((weave_st_uint32) p[1] << 8));
}

static void
st_wr32(weave_st_uint8 *p, weave_st_uint32 v)
{
	p[0] = (weave_st_uint8) (v & 0xFF);
	p[1] = (weave_st_uint8) ((v >> 8) & 0xFF);
	p[2] = (weave_st_uint8) ((v >> 16) & 0xFF);
	p[3] = (weave_st_uint8) ((v >> 24) & 0xFF);
}

static void
st_wr16(weave_st_uint8 *p, weave_st_uint16 v)
{
	p[0] = (weave_st_uint8) (v & 0xFF);
	p[1] = (weave_st_uint8) ((v >> 8) & 0xFF);
}

/*
 * Byte popcount.  A 256-entry table rather than __builtin_popcount so this file
 * has no compiler-feature dependency at all; the ranges walked here are at most
 * 64 bytes (one 512-bit superblock), so the table is not the bottleneck it would
 * be in a whole-bitmap scan.
 */
static const weave_st_uint8 st_pc8[256] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
	4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

static int
st_getbit(const weave_st_uint8 *bm, weave_st_uint32 i)
{
	return (bm[i >> 3] >> (i & 7)) & 1;
}

static void
st_setbit(weave_st_uint8 *bm, weave_st_uint32 i)
{
	bm[i >> 3] |= (weave_st_uint8) (1u << (i & 7));
}

/* ---------------------------------------------------------------------------
 * Geometry: the section offsets, derived from the counts and nothing else
 * ------------------------------------------------------------------------- */

typedef struct StGeom
{
	weave_st_uint32 nterms;
	weave_st_uint32 nslots;
	weave_st_uint32 nnodes;
	weave_st_uint32 nterminal;
	weave_st_uint32 ntrunc;
	weave_st_uint32 maxdepth;

	weave_st_uint32 bw;			/* bytes per bitmap */
	weave_st_uint32 nsb;		/* rank superblocks */
	weave_st_uint32 nsel;		/* select samples */

	size_t		off_labels;
	size_t		off_haschild;
	size_t		off_louds;
	size_t		off_terminal;
	size_t		off_trunc;
	size_t		off_rank_haschild;
	size_t		off_rank_terminal;
	size_t		off_select_louds;
	size_t		off_ords;
	size_t		total;
} StGeom;

/*
 * Fill in the derived geometry.  Callers must have range-checked the counts
 * against the format caps first, which is what keeps this arithmetic from
 * overflowing a 32-bit size_t.
 */
static void
st_geom(StGeom *g)
{
	size_t		o;

	g->bw = 8 * ((g->nslots + 63) / 64);
	g->nsb = (g->nslots + 511) / 512;
	g->nsel = (g->nnodes + 63) / 64;

	o = WEAVE_ST_HDRSIZE;
	g->off_labels = o;
	o += g->nslots;
	g->off_haschild = o;
	o += g->bw;
	g->off_louds = o;
	o += g->bw;
	g->off_terminal = o;
	o += g->bw;
	g->off_trunc = o;
	o += g->bw;
	g->off_rank_haschild = o;
	o += (size_t) 4 * g->nsb;
	g->off_rank_terminal = o;
	o += (size_t) 4 * g->nsb;
	g->off_select_louds = o;
	o += (size_t) 4 * g->nsel;
	g->off_ords = o;
	o += (size_t) 4 * g->nterminal;
	g->total = o;
}

/* ---------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------- */

/*
 * The sink st_walk() feeds.  With the output pointers NULL it counts; with them
 * set it writes.  ONE walker, TWO modes -- the measure pass and the emit pass
 * cannot disagree about how many slots there are, and a disagreement there is a
 * buffer overrun.
 */
typedef struct StSink
{
	weave_st_uint32 nslots;
	weave_st_uint32 nnodes;
	weave_st_uint32 nterminal;
	weave_st_uint32 ntrunc;
	weave_st_uint32 maxdepth;

	weave_st_uint8 *labels;		/* NULL => counting only */
	weave_st_uint8 *haschild;
	weave_st_uint8 *louds;
	weave_st_uint8 *terminal;
	weave_st_uint8 *trunc;
	weave_st_uint8 *ords;
} StSink;

static void
st_sink_slot(StSink *sk, weave_st_uint32 depth, weave_st_uint8 label,
			 int is_node_start, int is_terminal, int is_trunc, int has_child,
			 weave_st_uint32 ord)
{
	if (sk->labels != NULL)
	{
		sk->labels[sk->nslots] = label;
		if (is_node_start)
			st_setbit(sk->louds, sk->nslots);
		if (has_child)
			st_setbit(sk->haschild, sk->nslots);
		if (is_terminal)
		{
			st_setbit(sk->terminal, sk->nslots);
			st_wr32(sk->ords + (size_t) 4 * sk->nterminal, ord);
		}
		if (is_trunc)
			st_setbit(sk->trunc, sk->nslots);
	}

	sk->nslots++;
	if (is_node_start)
		sk->nnodes++;
	if (is_terminal)
		sk->nterminal++;
	if (is_trunc)
		sk->ntrunc++;
	if (depth > sk->maxdepth)
		sk->maxdepth = depth;
}

/* Unsigned byte order, shorter-is-smaller on a tie: exactly the dictionary's
 * order (memcmp semantics), NOT the collation's. */
static int
st_cmp(const WeaveSurfTerm *a, const WeaveSurfTerm *b)
{
	weave_st_uint32 m = a->len < b->len ? a->len : b->len;
	int			c = (m == 0) ? 0 : memcmp(a->s, b->s, m);

	if (c != 0)
		return c;
	if (a->len == b->len)
		return 0;
	return a->len < b->len ? -1 : 1;
}

/*
 * Walk the vocabulary in level order, feeding one slot per distinct prefix.
 *
 * Depth by depth: at depth d the slots are the distinct d-byte prefixes of terms
 * with length >= d, and sortedness makes each such run CONTIGUOUS in the array.
 * That last claim is the whole reason no BFS queue is needed, so here is the
 * proof: suppose x < y < z, |x| >= d, |z| >= d, and x and z share the d-byte
 * prefix P.  If |y| >= d then y shares P too (the standard lexicographic
 * sandwich).  If |y| < d then y is either a prefix of P -- in which case y < x,
 * contradicting x < y -- or it differs from P at some position < |y| < d, in
 * which case y is on the same side of both x and z, contradicting x < y < z.  So
 * nothing can be interleaved into a run.
 *
 * Within a depth, distinct prefixes are produced in ascending order, which is
 * ascending by (parent prefix, label) -- exactly the level order LOUDS-Sparse
 * requires.
 */
static WeaveSurfError
st_walk(const WeaveSurfTerm *terms, weave_st_uint32 n, StSink *sk)
{
	weave_st_uint32 i;
	weave_st_uint32 d;
	weave_st_uint32 maxlen = 0;
	weave_st_uint32 dlimit;

	if (n > WEAVE_SURFTRIE_MAX_TERMS)
		return WEAVE_SURF_TOO_MANY;

	for (i = 0; i < n; i++)
	{
		if (terms[i].len == 0)
			return WEAVE_SURF_EMPTY_TERM;
		if (i > 0 && st_cmp(&terms[i - 1], &terms[i]) >= 0)
			return WEAVE_SURF_UNSORTED;
		if (terms[i].len > maxlen)
			maxlen = terms[i].len;
	}

	dlimit = (maxlen < WEAVE_SURFTRIE_MAX_DEPTH) ? maxlen : WEAVE_SURFTRIE_MAX_DEPTH;

	for (d = 1; d <= dlimit; d++)
	{
		weave_st_uint32 prevrun = 0;
		int			haveprev = 0;

		i = 0;
		while (i < n)
		{
			weave_st_uint32 j;
			int			is_terminal;
			int			is_trunc;
			int			has_child;
			int			is_node_start;
			int			longer;

			if (terms[i].len < d)
			{
				i++;
				continue;
			}

			/* the run of terms sharing this d-byte prefix */
			j = i + 1;
			while (j < n && terms[j].len >= d &&
				   memcmp(terms[j].s, terms[i].s, d) == 0)
				j++;

			/*
			 * Every term in the run other than the prefix itself is strictly
			 * longer than d, and the prefix itself -- if present -- sorts first
			 * in the run.  So one comparison decides both questions.
			 */
			is_terminal = (terms[i].len == d);
			longer = (j - i > 1) || (terms[i].len > d);

			if (d == WEAVE_SURFTRIE_MAX_DEPTH)
			{
				/*
				 * The deepest byte the format holds.  Longer terms collapse
				 * here: mark the slot terminal AND truncated, which turns every
				 * query through it into a MAYBE.  See the one-sided-error
				 * discussion in weave/surftrie.h -- the alternative to this is a
				 * false negative.
				 */
				is_trunc = longer;
				is_terminal = is_terminal || longer;
				has_child = 0;
			}
			else
			{
				is_trunc = 0;
				has_child = longer;
			}

			/* a new node begins where the (d-1)-byte parent prefix changes */
			if (!haveprev)
				is_node_start = 1;
			else if (d == 1)
				is_node_start = 0;	/* every depth-1 slot is in the root node */
			else
				is_node_start = (memcmp(terms[prevrun].s, terms[i].s, d - 1) != 0);

			st_sink_slot(sk, d, (weave_st_uint8) (unsigned char) terms[i].s[d - 1],
						 is_node_start, is_terminal, is_trunc, has_child, i);
			if (sk->nslots > WEAVE_SURFTRIE_MAX_SLOTS)
				return WEAVE_SURF_TOO_MANY;

			prevrun = i;
			haveprev = 1;
			i = j;
		}
	}

	return WEAVE_SURF_OK;
}

/* rank superblock table: ones in bm[0 .. 512*sb) for each superblock sb */
static void
st_fill_rank(const weave_st_uint8 *bm, weave_st_uint32 bw, weave_st_uint32 nsb,
			 weave_st_uint8 *out)
{
	weave_st_uint32 sb;
	weave_st_uint32 run = 0;

	for (sb = 0; sb < nsb; sb++)
	{
		weave_st_uint32 b;
		weave_st_uint32 bend = (sb + 1) * 64;

		st_wr32(out + (size_t) 4 * sb, run);
		if (bend > bw)
			bend = bw;
		for (b = sb * 64; b < bend; b++)
			run += st_pc8[bm[b]];
	}
}

/* select sample table: the slot index of every 64th set bit, first one first */
static void
st_fill_select(const weave_st_uint8 *louds, weave_st_uint32 nslots,
			   weave_st_uint8 *out)
{
	weave_st_uint32 i;
	weave_st_uint32 cnt = 0;

	for (i = 0; i < nslots; i++)
	{
		if (st_getbit(louds, i))
		{
			if ((cnt & 63) == 0)
				st_wr32(out + (size_t) 4 * (cnt >> 6), i);
			cnt++;
		}
	}
}

WeaveSurfError
weave_surftrie_size(const WeaveSurfTerm *terms, weave_st_uint32 n,
					size_t *size_out)
{
	StSink		sk;
	StGeom		g;
	WeaveSurfError err;

	memset(&sk, 0, sizeof(sk));
	err = st_walk(terms, n, &sk);
	if (err != WEAVE_SURF_OK)
		return err;

	memset(&g, 0, sizeof(g));
	g.nterms = n;
	g.nslots = sk.nslots;
	g.nnodes = sk.nnodes;
	g.nterminal = sk.nterminal;
	g.ntrunc = sk.ntrunc;
	g.maxdepth = sk.maxdepth;
	st_geom(&g);

	if (size_out != NULL)
		*size_out = g.total;
	return WEAVE_SURF_OK;
}

WeaveSurfError
weave_surftrie_build(const WeaveSurfTerm *terms, weave_st_uint32 n,
					 void *dst, size_t dstlen, size_t *written)
{
	StSink		sk;
	StGeom		g;
	WeaveSurfError err;
	weave_st_uint8 *img = (weave_st_uint8 *) dst;

	memset(&sk, 0, sizeof(sk));
	err = st_walk(terms, n, &sk);
	if (err != WEAVE_SURF_OK)
		return err;

	memset(&g, 0, sizeof(g));
	g.nterms = n;
	g.nslots = sk.nslots;
	g.nnodes = sk.nnodes;
	g.nterminal = sk.nterminal;
	g.ntrunc = sk.ntrunc;
	g.maxdepth = sk.maxdepth;
	st_geom(&g);

	if (img == NULL)
		return WEAVE_SURF_NOSPACE;
	if (dstlen < g.total)
		return WEAVE_SURF_NOSPACE;

	/*
	 * Zero first: the bitmaps are written by OR-ing single bits and their tail
	 * bits past nslots must read as zero (st_check_structure() enforces that,
	 * because a stray tail bit makes the popcount identities disagree).
	 */
	memset(img, 0, g.total);

	st_wr32(img + WEAVE_ST_OFF_MAGIC, WEAVE_SURFTRIE_MAGIC);
	st_wr16(img + WEAVE_ST_OFF_VERSION, WEAVE_SURFTRIE_VERSION);
	st_wr16(img + WEAVE_ST_OFF_FLAGS, 0);
	st_wr32(img + WEAVE_ST_OFF_NTERMS, n);
	st_wr32(img + WEAVE_ST_OFF_NSLOTS, g.nslots);
	st_wr32(img + WEAVE_ST_OFF_NNODES, g.nnodes);
	st_wr32(img + WEAVE_ST_OFF_NTERMINAL, g.nterminal);
	st_wr32(img + WEAVE_ST_OFF_NTRUNC, g.ntrunc);
	st_wr16(img + WEAVE_ST_OFF_MAXDEPTH, (weave_st_uint16) g.maxdepth);
	st_wr16(img + WEAVE_ST_OFF_RESERVED, 0);

	/* second pass: same walker, output pointers set */
	memset(&sk, 0, sizeof(sk));
	sk.labels = img + g.off_labels;
	sk.haschild = img + g.off_haschild;
	sk.louds = img + g.off_louds;
	sk.terminal = img + g.off_terminal;
	sk.trunc = img + g.off_trunc;
	sk.ords = img + g.off_ords;
	err = st_walk(terms, n, &sk);
	if (err != WEAVE_SURF_OK)
		return err;

	/*
	 * The two passes are the same function over the same input, so this cannot
	 * fire.  It is checked anyway because if it ever did, the emit pass would
	 * have written past a section boundary -- and a silent one of those is the
	 * bug class the whole file is arranged to make impossible.
	 */
	if (sk.nslots != g.nslots || sk.nnodes != g.nnodes ||
		sk.nterminal != g.nterminal || sk.ntrunc != g.ntrunc ||
		sk.maxdepth != g.maxdepth)
		return WEAVE_SURF_COUNTS;

	st_fill_rank(img + g.off_haschild, g.bw, g.nsb, img + g.off_rank_haschild);
	st_fill_rank(img + g.off_terminal, g.bw, g.nsb, img + g.off_rank_terminal);
	st_fill_select(img + g.off_louds, g.nslots, img + g.off_select_louds);

	if (written != NULL)
		*written = g.total;
	return WEAVE_SURF_OK;
}

/* ---------------------------------------------------------------------------
 * Rank / select over the validated bitvectors
 * ------------------------------------------------------------------------- */

/* ones in bm[0 .. pos], inclusive.  `pos` must be < nslots. */
static weave_st_uint32
st_rank_incl(const weave_st_uint8 *bm, const weave_st_uint8 *ranktbl,
			 weave_st_uint32 pos)
{
	weave_st_uint32 sb = pos >> 9;
	weave_st_uint32 r = st_rd32(ranktbl + (size_t) 4 * sb);
	weave_st_uint32 bend = pos >> 3;
	weave_st_uint32 b;

	for (b = sb * 64; b < bend; b++)
		r += st_pc8[bm[b]];
	r += st_pc8[bm[bend] & (weave_st_uint8) ((1u << ((pos & 7) + 1)) - 1)];
	return r;
}

/*
 * Slot index of the n-th set louds bit, 1-based.  Requires 1 <= n <= nnodes,
 * which every caller establishes from a validated count.
 *
 * The sample lands us on the (64k+1)-th one; at most 63 further ones are scanned
 * byte at a time.  The `byte < t->bw` bound is unreachable on an image that
 * passed st_check_structure() (the popcount identity guarantees the n-th one
 * exists) and is present because this function is also the first thing a
 * *corrupt* select sample would drag out of bounds -- see the planted-bug build
 * in test/fuzz/fuzz_surftrie.c.
 */
static weave_st_uint32
st_select_louds(const WeaveSurfTrie *t, weave_st_uint32 n)
{
	weave_st_uint32 k = (n - 1) >> 6;
	weave_st_uint32 pos = st_rd32(t->select_louds + (size_t) 4 * k);
	weave_st_uint32 remaining = (n - 1) - (k << 6);
	weave_st_uint32 byte;
	weave_st_uint8 cur;

	if (remaining == 0)
		return pos;

	byte = pos >> 3;
	cur = (weave_st_uint8) (t->louds[byte] &
						  ~(weave_st_uint8) ((1u << ((pos & 7) + 1)) - 1));
	for (;;)
	{
		weave_st_uint32 c = st_pc8[cur];

		if (c >= remaining)
		{
			int			bi;

			for (bi = 0; bi < 8; bi++)
			{
				if ((cur >> bi) & 1)
				{
					remaining--;
					if (remaining == 0)
						return (byte << 3) + (weave_st_uint32) bi;
				}
			}
		}
		remaining -= c;
		byte++;
		if (byte >= t->bw)
			return t->nslots;	/* unreachable when validated */
		cur = t->louds[byte];
	}
}

/* the slot range of node `node` (0-based node index) */
static void
st_node_bounds(const WeaveSurfTrie *t, weave_st_uint32 node,
			   weave_st_uint32 *start, weave_st_uint32 *end)
{
	*start = st_select_louds(t, node + 1);
	if (node + 1 < t->nnodes)
		*end = st_select_louds(t, node + 2) - 1;
	else
		*end = t->nslots - 1;
}

/* the node index a has-child slot points at */
static weave_st_uint32
st_child_node(const WeaveSurfTrie *t, weave_st_uint32 slot)
{
	return st_rank_incl(t->haschild, t->rank_haschild, slot);
}

/* the ordinal a terminal slot carries */
static weave_st_uint32
st_slot_ord(const WeaveSurfTrie *t, weave_st_uint32 slot)
{
	weave_st_uint32 r = st_rank_incl(t->terminal, t->rank_terminal, slot);

	return st_rd32(t->ords + (size_t) 4 * (r - 1));
}

/*
 * Find `label` among the slots of a node.  Binary search is legitimate only
 * because st_check_structure() proved labels are strictly ascending within every
 * node; on an unvalidated image it would silently miss (a false negative), which
 * is why that check is not optional.
 */
static int
st_find(const WeaveSurfTrie *t, weave_st_uint32 start, weave_st_uint32 end,
		weave_st_uint8 label, weave_st_uint32 *slot)
{
	weave_st_uint32 lo = start;
	weave_st_uint32 hi = end;

	while (lo <= hi)
	{
		weave_st_uint32 mid = lo + (hi - lo) / 2;
		weave_st_uint8 l = t->labels[mid];

		if (l == label)
		{
			*slot = mid;
			return 1;
		}
		if (l < label)
			lo = mid + 1;
		else
		{
			if (mid == start)
				return 0;
			hi = mid - 1;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Structural validation -- the memory-safety layer
 * ------------------------------------------------------------------------- */

/* ones in bm[0 .. nbits), whole bytes plus a masked tail */
static weave_st_uint32
st_popcount_bits(const weave_st_uint8 *bm, weave_st_uint32 nbits)
{
	weave_st_uint32 full = nbits >> 3;
	weave_st_uint32 i;
	weave_st_uint32 r = 0;

	for (i = 0; i < full; i++)
		r += st_pc8[bm[i]];
	if ((nbits & 7) != 0)
		r += st_pc8[bm[full] & (weave_st_uint8) ((1u << (nbits & 7)) - 1)];
	return r;
}

/* every bit at or past `nbits` must be zero, so the popcount identities mean
 * what they say */
static int
st_tail_clear(const weave_st_uint8 *bm, weave_st_uint32 nbits, weave_st_uint32 bw)
{
	weave_st_uint32 b = nbits >> 3;

	if ((nbits & 7) != 0)
	{
		if ((bm[b] & (weave_st_uint8) ~((1u << (nbits & 7)) - 1)) != 0)
			return 0;
		b++;
	}
	for (; b < bw; b++)
		if (bm[b] != 0)
			return 0;
	return 1;
}

static WeaveSurfError
st_check_structure(WeaveSurfTrie *t)
{
	weave_st_uint32 i;
	weave_st_uint32 cnt = 0;
	weave_st_uint32 prevlabel = 0;
	int			innode = 0;

	/* (a) the four bitmaps must be canonical: no bits past the last slot */
	if (!st_tail_clear(t->haschild, t->nslots, t->bw) ||
		!st_tail_clear(t->louds, t->nslots, t->bw) ||
		!st_tail_clear(t->terminal, t->nslots, t->bw) ||
		!st_tail_clear(t->trunc, t->nslots, t->bw))
		return WEAVE_SURF_TAILBITS;

	/*
	 * (b) the three population identities.  haschild == nnodes - 1 because every
	 * node except the root is the child of exactly one slot; that single
	 * equation is what makes st_child_node()'s rank a valid node index.
	 */
	if (st_popcount_bits(t->louds, t->nslots) != t->nnodes)
		return WEAVE_SURF_LOUDS_COUNT;
	if (st_popcount_bits(t->haschild, t->nslots) != t->nnodes - 1)
		return WEAVE_SURF_HASCHILD_COUNT;
	if (st_popcount_bits(t->terminal, t->nslots) != t->nterminal)
		return WEAVE_SURF_TERMINAL_COUNT;
	if (st_popcount_bits(t->trunc, t->nslots) != t->ntrunc)
		return WEAVE_SURF_TRUNC;

	/* (c) a truncated slot is always also terminal: a MAYBE is still a hit */
	for (i = 0; i < t->bw; i++)
		if ((t->trunc[i] & (weave_st_uint8) ~t->terminal[i]) != 0)
			return WEAVE_SURF_TRUNC;

	/* (d) slot 0 begins the root node */
	if (!st_getbit(t->louds, 0))
		return WEAVE_SURF_LOUDS_ROOT;

	/*
	 * (e) labels strictly ascending within each node.  Prevents a duplicate
	 * label (two children with the same byte: ambiguous) and licenses the binary
	 * search in st_find().
	 */
	for (i = 0; i < t->nslots; i++)
	{
		weave_st_uint8 l = t->labels[i];

		if (st_getbit(t->louds, i))
			innode = 0;
		if (innode && l <= prevlabel)
			return WEAVE_SURF_LABELS;
		prevlabel = l;
		innode = 1;
	}

	/*
	 * (f) the accelerators equal a recomputation.  A corrupt rank superblock
	 * navigates to the WRONG node and a corrupt select sample walks off the
	 * bitmap: the first is a false negative no sanitizer can see, the second is
	 * an out-of-bounds read.  Both are one linear pass to rule out.
	 */
#ifndef WEAVE_SURF_PLANT_NO_RANK_GUARD
	{
		weave_st_uint32 nsb = (t->nslots + 511) / 512;
		weave_st_uint32 sb;
		weave_st_uint32 runhc = 0;
		weave_st_uint32 runtm = 0;

		for (sb = 0; sb < nsb; sb++)
		{
			weave_st_uint32 b;
			weave_st_uint32 bend = (sb + 1) * 64;

			if (st_rd32(t->rank_haschild + (size_t) 4 * sb) != runhc ||
				st_rd32(t->rank_terminal + (size_t) 4 * sb) != runtm)
				return WEAVE_SURF_RANK;
			if (bend > t->bw)
				bend = t->bw;
			for (b = sb * 64; b < bend; b++)
			{
				runhc += st_pc8[t->haschild[b]];
				runtm += st_pc8[t->terminal[b]];
			}
		}
	}
#endif

#ifndef WEAVE_SURF_PLANT_NO_SELECT_GUARD
	cnt = 0;
	for (i = 0; i < t->nslots; i++)
	{
		if (st_getbit(t->louds, i))
		{
			if ((cnt & 63) == 0 &&
				st_rd32(t->select_louds + (size_t) 4 * (cnt >> 6)) != i)
				return WEAVE_SURF_SELECT;
			cnt++;
		}
	}
#endif

	/*
	 * (g) ACYCLICITY, and it is a validated field rather than an assumption:
	 * every child node must start AFTER its parent slot, so slot indices
	 * strictly increase along any path and every walk terminates.  Without this
	 * a corrupt image makes enumeration spin forever, which is the one failure
	 * mode a validator's caller cannot diagnose from the outside.
	 */
	for (i = 0; i < t->nslots; i++)
	{
		if (st_getbit(t->haschild, i))
		{
			weave_st_uint32 child = st_child_node(t, i);
			weave_st_uint32 cstart;

			if (child == 0 || child >= t->nnodes)
				return WEAVE_SURF_HASCHILD_COUNT;
			cstart = st_select_louds(t, child + 1);
			if (cstart <= i || cstart >= t->nslots)
				return WEAVE_SURF_CYCLE;
		}
	}

	/*
	 * (h) every stored ordinal is in range.  Checked here, not only in the deep
	 * pass, so that a reader that skips validate() still cannot hand a caller an
	 * ordinal that indexes past the dictionary.
	 */
	for (i = 0; i < t->nterminal; i++)
		if (st_rd32(t->ords + (size_t) 4 * i) >= t->nterms)
			return WEAVE_SURF_ORD_RANGE;

	(void) cnt;
	return WEAVE_SURF_OK;
}

WeaveSurfError
weave_surftrie_open(const void *img, size_t len, WeaveSurfTrie *t)
{
	const weave_st_uint8 *p = (const weave_st_uint8 *) img;
	StGeom		g;

	if (t != NULL)
		memset(t, 0, sizeof(*t));
	if (img == NULL || t == NULL || len < WEAVE_ST_HDRSIZE)
		return WEAVE_SURF_TRUNCATED;

	if (st_rd32(p + WEAVE_ST_OFF_MAGIC) != WEAVE_SURFTRIE_MAGIC)
		return WEAVE_SURF_MAGIC;
	if (st_rd16(p + WEAVE_ST_OFF_VERSION) != WEAVE_SURFTRIE_VERSION)
		return WEAVE_SURF_VERSION;
	if (st_rd16(p + WEAVE_ST_OFF_FLAGS) != 0)
		return WEAVE_SURF_FLAGS;
	if (st_rd16(p + WEAVE_ST_OFF_RESERVED) != 0)
		return WEAVE_SURF_RESERVED;

	memset(&g, 0, sizeof(g));
	g.nterms = st_rd32(p + WEAVE_ST_OFF_NTERMS);
	g.nslots = st_rd32(p + WEAVE_ST_OFF_NSLOTS);
	g.nnodes = st_rd32(p + WEAVE_ST_OFF_NNODES);
	g.nterminal = st_rd32(p + WEAVE_ST_OFF_NTERMINAL);
	g.ntrunc = st_rd32(p + WEAVE_ST_OFF_NTRUNC);
	g.maxdepth = st_rd16(p + WEAVE_ST_OFF_MAXDEPTH);

	/* caps first: everything below is size arithmetic on these numbers */
	if (g.nterms > WEAVE_SURFTRIE_MAX_TERMS || g.nslots > WEAVE_SURFTRIE_MAX_SLOTS)
		return WEAVE_SURF_COUNTS;

	if (g.nterms == 0)
	{
		/* the empty vocabulary is a legal, header-only image */
		if (g.nslots != 0 || g.nnodes != 0 || g.nterminal != 0 ||
			g.ntrunc != 0 || g.maxdepth != 0)
			return WEAVE_SURF_COUNTS;
		if (len != WEAVE_ST_HDRSIZE)
			return WEAVE_SURF_SIZE;
		t->img = p;
		t->len = len;
		return WEAVE_SURF_OK;
	}

	if (g.nslots == 0 || g.nnodes == 0 || g.nnodes > g.nslots ||
		g.nterminal == 0 || g.nterminal > g.nslots ||
		g.nterminal > g.nterms || g.ntrunc > g.nterminal)
		return WEAVE_SURF_COUNTS;
	if (g.maxdepth == 0 || g.maxdepth > WEAVE_SURFTRIE_MAX_DEPTH)
		return WEAVE_SURF_DEPTH;

	st_geom(&g);
#ifndef WEAVE_SURF_PLANT_NO_SIZE_GUARD
	if (len != g.total)
		return WEAVE_SURF_SIZE;
#endif

	t->img = p;
	t->len = len;
	t->nterms = g.nterms;
	t->nslots = g.nslots;
	t->nnodes = g.nnodes;
	t->nterminal = g.nterminal;
	t->ntrunc = g.ntrunc;
	t->maxdepth = g.maxdepth;
	t->bw = g.bw;
	t->labels = p + g.off_labels;
	t->haschild = p + g.off_haschild;
	t->louds = p + g.off_louds;
	t->terminal = p + g.off_terminal;
	t->trunc = p + g.off_trunc;
	t->rank_haschild = p + g.off_rank_haschild;
	t->rank_terminal = p + g.off_rank_terminal;
	t->select_louds = p + g.off_select_louds;
	t->ords = p + g.off_ords;

	return st_check_structure(t);
}

/* ---------------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------------- */

int
weave_surftrie_may_contain(const WeaveSurfTrie *t, const void *key,
						   weave_st_uint32 keylen, WeaveSurfHit *hit)
{
	const weave_st_uint8 *k = (const weave_st_uint8 *) key;
	weave_st_uint32 ns;
	weave_st_uint32 ne;
	weave_st_uint32 d;
	weave_st_uint32 slot = 0;

	if (t == NULL || t->nslots == 0 || k == NULL || keylen == 0)
		return 0;

	st_node_bounds(t, 0, &ns, &ne);
	for (d = 0; d < keylen; d++)
	{
		if (!st_find(t, ns, ne, k[d], &slot))
			return 0;			/* DEFINITELY absent: no path */

		if (d + 1 == keylen)
		{
			/* the key ends here: terminal decides, and trunc decides exactness */
			if (!st_getbit(t->terminal, slot))
				return 0;
			if (hit != NULL)
			{
				hit->ord = st_slot_ord(t, slot);
				hit->exact = !st_getbit(t->trunc, slot);
			}
			return 1;
		}

		/*
		 * The key continues.  A truncated slot means the trie stopped
		 * representing bytes here, so anything below it is a MAYBE -- this is
		 * the one place the filter's false positives come from, and returning 0
		 * here instead would be the false negative the contract forbids.
		 */
		if (st_getbit(t->trunc, slot))
		{
			if (hit != NULL)
			{
				hit->ord = st_slot_ord(t, slot);
				hit->exact = 0;
			}
			return 1;
		}
		if (!st_getbit(t->haschild, slot))
			return 0;			/* DEFINITELY absent: nothing extends this path */
		st_node_bounds(t, st_child_node(t, slot), &ns, &ne);
	}

	return 0;					/* unreachable: keylen > 0 returns inside */
}

/* One frame of an explicit DFS stack.  Explicit rather than recursive so a
 * hostile image cannot blow the C stack, and fixed-size so no query allocates. */
typedef struct StFrame
{
	weave_st_uint32 start;
	weave_st_uint32 end;
	weave_st_uint32 cur;
} StFrame;

WeaveSurfError
weave_surftrie_enumerate(const WeaveSurfTrie *t, const void *prefix,
						 weave_st_uint32 prefixlen, WeaveSurfEnumCb cb,
						 void *arg, weave_st_uint32 *nhits)
{
	const weave_st_uint8 *p = (const weave_st_uint8 *) prefix;
	char		buf[WEAVE_SURFTRIE_MAX_DEPTH];
	StFrame		stack[WEAVE_SURFTRIE_MAX_DEPTH + 1];
	weave_st_uint32 count = 0;
	weave_st_uint32 dlimit;
	weave_st_uint32 ns;
	weave_st_uint32 ne;
	weave_st_uint32 d;
	weave_st_uint32 slot = 0;
	weave_st_uint32 base;
	int			sp;
	int			stop = 0;

	if (nhits != NULL)
		*nhits = 0;
	if (t == NULL || cb == NULL)
		return WEAVE_SURF_OK;
	if (t->nslots == 0)
		return WEAVE_SURF_OK;
	if (prefixlen > 0 && p == NULL)
		return WEAVE_SURF_OK;

	dlimit = (prefixlen < WEAVE_SURFTRIE_MAX_DEPTH)
		? prefixlen : WEAVE_SURFTRIE_MAX_DEPTH;

	st_node_bounds(t, 0, &ns, &ne);
	for (d = 0; d < dlimit; d++)
	{
		if (!st_find(t, ns, ne, p[d], &slot))
			return WEAVE_SURF_OK;	/* nothing carries this prefix */
		buf[d] = (char) p[d];
		if (d + 1 < dlimit)
		{
			if (st_getbit(t->trunc, slot))
			{
				/* the prefix runs through a truncated slot: everything the slot
				 * collapsed is a candidate and the caller owes a recheck */
				count++;
				(void) cb(arg, buf, d + 1, st_slot_ord(t, slot), 0);
				if (nhits != NULL)
					*nhits = count;
				return WEAVE_SURF_OK;
			}
			if (!st_getbit(t->haschild, slot))
				return WEAVE_SURF_OK;
			st_node_bounds(t, st_child_node(t, slot), &ns, &ne);
		}
	}

	if (prefixlen > WEAVE_SURFTRIE_MAX_DEPTH)
	{
		/*
		 * The prefix is longer than the trie represents, so only a truncated
		 * slot can still cover a term that has it.  Reported as a candidate:
		 * exact == 0 says "these bytes matched as far as the format goes".
		 */
		if (st_getbit(t->trunc, slot))
		{
			count++;
			(void) cb(arg, buf, WEAVE_SURFTRIE_MAX_DEPTH, st_slot_ord(t, slot), 0);
		}
		if (nhits != NULL)
			*nhits = count;
		return WEAVE_SURF_OK;
	}

	if (prefixlen > 0)
	{
		/* the prefix itself may be a term */
		if (st_getbit(t->terminal, slot))
		{
			count++;
			stop = cb(arg, buf, prefixlen, st_slot_ord(t, slot),
					  !st_getbit(t->trunc, slot));
		}
		if (stop || !st_getbit(t->haschild, slot))
		{
			if (nhits != NULL)
				*nhits = count;
			return WEAVE_SURF_OK;
		}
		st_node_bounds(t, st_child_node(t, slot), &ns, &ne);
		base = prefixlen;
	}
	else
		base = 0;

	/*
	 * Pre-order DFS with label-ascending children, which is ascending
	 * lexicographic order over terms; the terminal on a slot is emitted BEFORE
	 * descending into it, which is what puts "ab" ahead of "abc".
	 */
	stack[0].start = ns;
	stack[0].end = ne;
	stack[0].cur = ns;
	sp = 1;

	while (sp > 0 && !stop)
	{
		StFrame    *f = &stack[sp - 1];

		if (f->cur > f->end)
		{
			sp--;
			continue;
		}
		slot = f->cur++;
		d = base + (weave_st_uint32) (sp - 1);
		if (d >= WEAVE_SURFTRIE_MAX_DEPTH)
		{
			/* cannot happen on an image this builder wrote (no path is deeper
			 * than MAX_DEPTH); on a corrupt-but-openable image it stops the walk
			 * rather than overrunning buf[] */
			sp--;
			continue;
		}
		buf[d] = (char) t->labels[slot];

		if (st_getbit(t->terminal, slot))
		{
			count++;
			stop = cb(arg, buf, d + 1, st_slot_ord(t, slot),
					  !st_getbit(t->trunc, slot));
			if (stop)
				break;
		}
		if (st_getbit(t->haschild, slot) && d + 1 < WEAVE_SURFTRIE_MAX_DEPTH)
		{
			weave_st_uint32 cs;
			weave_st_uint32 cetmp;

			st_node_bounds(t, st_child_node(t, slot), &cs, &cetmp);
			stack[sp].start = cs;
			stack[sp].end = cetmp;
			stack[sp].cur = cs;
			sp++;
		}
	}

	if (nhits != NULL)
		*nhits = count;
	return WEAVE_SURF_OK;
}

/* ---------------------------------------------------------------------------
 * The deep semantic pass -- what weave_check() calls
 * ------------------------------------------------------------------------- */

WeaveSurfError
weave_surftrie_validate(const WeaveSurfTrie *t)
{
	StFrame		stack[WEAVE_SURFTRIE_MAX_DEPTH + 1];
	weave_st_uint32 seen_slots = 0;
	weave_st_uint32 seen_nodes = 0;
	weave_st_uint32 seen_terminal = 0;
	weave_st_uint32 observed_depth = 0;
	weave_st_uint32 prev_ord = 0;
	weave_st_uint32 ns;
	weave_st_uint32 ne;
	int			sp;

	if (t == NULL)
		return WEAVE_SURF_TRUNCATED;
	if (t->nterms == 0)
		return (t->nslots == 0) ? WEAVE_SURF_OK : WEAVE_SURF_COUNTS;

	st_node_bounds(t, 0, &ns, &ne);
	stack[0].start = ns;
	stack[0].end = ne;
	stack[0].cur = ns;
	sp = 1;
	seen_nodes = 1;

	while (sp > 0)
	{
		StFrame    *f = &stack[sp - 1];
		weave_st_uint32 slot;
		weave_st_uint32 d;

		if (f->cur > f->end)
		{
			sp--;
			continue;
		}
		slot = f->cur++;
		d = (weave_st_uint32) sp;	/* depth of this slot, 1-based */
		if (d > WEAVE_SURFTRIE_MAX_DEPTH)
			return WEAVE_SURF_DEPTH;
		if (d > observed_depth)
			observed_depth = d;
		seen_slots++;
		if (seen_slots > t->nslots)
			return WEAVE_SURF_REACH;	/* a slot reached twice: not a tree */

		if (st_getbit(t->terminal, slot))
		{
			weave_st_uint32 ord = st_slot_ord(t, slot);

			/*
			 * THE INVARIANT THAT MAKES THIS "THE SORTED DICTIONARY".  A
			 * pre-order DFS with ascending labels visits terminals in ascending
			 * lexicographic order, so their ordinals must start at 0 and
			 * strictly increase.  A violation means the ordinal column and the
			 * trie shape describe different vocabularies, which would send a
			 * prefix scan to the wrong posting lists -- plausible wrong answers,
			 * the class doc/TESTING.md says no fixed-output test can catch.
			 */
			if (ord >= t->nterms)
				return WEAVE_SURF_ORD_RANGE;
			if (seen_terminal == 0)
			{
				if (ord != 0)
					return WEAVE_SURF_ORD_ORDER;
			}
			else if (ord <= prev_ord)
				return WEAVE_SURF_ORD_ORDER;
			prev_ord = ord;
			seen_terminal++;
		}

		if (st_getbit(t->haschild, slot))
		{
			weave_st_uint32 cs;
			weave_st_uint32 ce;

			if (sp > WEAVE_SURFTRIE_MAX_DEPTH)
				return WEAVE_SURF_DEPTH;
			st_node_bounds(t, st_child_node(t, slot), &cs, &ce);
			if (cs > ce || ce >= t->nslots)
				return WEAVE_SURF_REACH;
			stack[sp].start = cs;
			stack[sp].end = ce;
			stack[sp].cur = cs;
			sp++;
			seen_nodes++;
		}
	}

	/*
	 * REACHABILITY IS ALREADY IMPLIED, AND THIS IS RECORDED RATHER THAN REMOVED.
	 * Mutation-testing this file found that deleting the two comparisons below
	 * changes no test outcome, and the reason is a proof rather than a coverage
	 * gap: popcount(haschild) == nnodes - 1 makes the rank-derived child index a
	 * bijection from has-child slots onto nodes 1..nnodes-1, so every node has
	 * exactly one parent slot; acyclicity forces a node's start to exceed its
	 * parent slot, and the parent slot lies in a node that starts no later, so by
	 * induction every node is reachable from node 0.  Kept because
	 * doc/specs/SEGMENT_FORMAT.md sect. 9 asks for reachability by name, because it
	 * costs two comparisons on a pass that is already walking the trie, and
	 * because a future change to any of the three premises would silently take
	 * the property with it.
	 */
	if (seen_slots != t->nslots || seen_nodes != t->nnodes)
		return WEAVE_SURF_REACH;
	if (seen_terminal != t->nterminal)
		return WEAVE_SURF_TERMINAL_COUNT;
	if (observed_depth != t->maxdepth)
		return WEAVE_SURF_DEPTH;

	return WEAVE_SURF_OK;
}

WeaveSurfError
weave_surftrie_check(const void *img, size_t len)
{
	WeaveSurfTrie t;
	WeaveSurfError err = weave_surftrie_open(img, len, &t);

	if (err != WEAVE_SURF_OK)
		return err;
	return weave_surftrie_validate(&t);
}
