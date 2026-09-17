/*-------------------------------------------------------------------------
 *
 * vecwrite.c
 *		The vector weft on disk: writing it, and reading it back.
 *
 * Task V7.  doc/specs/VECTOR_CHANNEL.md sect. 7.3 is the brief; sect. 7.1 is the
 * ratified format, and include/weave/vecweft.h is the strip order that this writer
 * and its reader both compute from one place -- a writer that emits block b's
 * strips in one order and a reader that assumes another returns another block's
 * codes without failing.
 *
 * WHAT IS HERE AND WHAT IS DELIBERATELY NOT.  Everything in this file needs a
 * backend: page allocation, GenericXLog cycles, chain walks, the relcache.  All
 * the arithmetic -- which strip carries which coordinates, where a directory
 * record sits, what bytes a page image holds -- is in the two backend-independent
 * translation units (src/vector/vecpage.c, src/vector/vecweft.c) so
 * test/hegel/test_vecweft.c can prove the round trip with no server.
 *
 * 100% GenericXLog (AGENTS.md hard rule 2).  Every page written here is brand
 * new, so every registration is GENERIC_XLOG_FULL_IMAGE: a delta against a
 * never-written pre-image would be the whole page anyway.  One page per cycle, so
 * a weft of any size needs neither an oversized WAL record nor a page-count limit.
 * t/016_vector_durability.pl is the proof that it survives an immediate shutdown.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 * IDENTIFICATION
 *	  src/vector/vecwrite.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/genam.h"
#include "catalog/pg_am.h"
#include "commands/defrem.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "weave/am.h"
#include "weave/vector.h"

PG_FUNCTION_INFO_V1(weave_vec_meta);
PG_FUNCTION_INFO_V1(weave_vec_blocks);

/* ---------------------------------------------------------------------------
 * Producer 1's accumulator
 *
 * See WeaveVecAccum in include/weave/vector.h for why it holds codes rather than
 * float vectors, and why a NULL vector is a dead lane rather than a skipped
 * document.
 * ------------------------------------------------------------------------- */

void
weave_vec_accum_init(WeaveVecAccum *acc, MemoryContext ctx, bool active, int bits)
{
	MemSet(acc, 0, sizeof(*acc));
	acc->ctx = ctx;
	acc->active = active;
	acc->bits = bits;
	acc->layout = WEAVE_PACK_LANE;

	/*
	 * The metric is NOT known at build time, and recording that is better than
	 * pretending otherwise.  wvec_weave_ops declares no operator members on
	 * purpose (V7 is storage, V8 is the scan -- sect. 7.2), so nothing in the
	 * catalog selects L2 over inner product yet.  L2 is written because 0 is not a
	 * WeaveMetric and a zeroed field must not validate as one; it changes no
	 * stored byte, since the codes are metric-independent and the inputs are not
	 * unit-normalized (WEAVE_VMETA_F_NORMALIZED stays clear).  V8 decides whether
	 * the metric is an opclass or a reloption, and must revisit this.
	 */
	acc->metric = WEAVE_METRIC_L2;
}

/*
 * Drop everything the accumulator holds.
 *
 * Called after the segment write, because weave_build_flush_segment() then does
 * MemoryContextReset(bs->ctx) -- which frees the codes AND the quantizer's
 * rotation tables, so every pointer here would dangle if this did not clear them.
 * The quantizer is rebuilt on the next vector and is bit-identical when it is:
 * weave_rotation_init() and weave_codebook_get() are functions of (dim, bits)
 * alone, which is also what lets a reader reconstruct it from the VMETA page.
 */
void
weave_vec_accum_reset(WeaveVecAccum *acc)
{
	acc->ready = false;
	acc->dim = 0;
	acc->codebytes = 0;
	acc->code = NULL;
	acc->scale = NULL;
	acc->norm = NULL;
	acc->live = NULL;
	acc->docid = NULL;
	acc->nlane = 0;
	acc->lanecap = 0;
	acc->nlive = 0;
	MemSet(&acc->q, 0, sizeof(acc->q));
}

/*
 * Room for one more lane.  Doubling, and huge-safe rather than annotated bounded:
 * a code is dim*bits/8 bytes (480 at 960-d and 4 bits) and a build participant
 * accumulates until the memory budget flushes it, so this array is corpus-scale by
 * construction -- exactly the class ci/check-alloc.sh exists for.
 */
static void *
vec_grow_one(void *p, Size sz)
{
	return p != NULL ? WEAVE_REALLOC_MAYBE_HUGE(p, sz) : WEAVE_ALLOC_MAYBE_HUGE(sz);
}

static void
vec_accum_grow(WeaveVecAccum *acc)
{
	MemoryContext old;
	uint32		newcap;

	if (acc->nlane < acc->lanecap)
		return;

	old = MemoryContextSwitchTo(acc->ctx);
	newcap = acc->lanecap ? acc->lanecap * 2 : 1024;
	acc->scale = (float *) vec_grow_one(acc->scale, (Size) newcap * sizeof(float));
	acc->norm = (float *) vec_grow_one(acc->norm, (Size) newcap * sizeof(float));
	acc->live = (uint8 *) vec_grow_one(acc->live, (Size) newcap * sizeof(uint8));
	acc->docid = (uint64 *) vec_grow_one(acc->docid, (Size) newcap * sizeof(uint64));
	/* The code array waits for the dimension.  A column of NULLs can fill lanes
	 * before any vector arrives, and there is no code size to allocate until one
	 * does; weave_vec_accum_add() allocates and zeroes it at that point. */
	if (acc->codebytes > 0)
		acc->code = (uint8 *) vec_grow_one(acc->code,
										   (Size) newcap * (Size) acc->codebytes);
	acc->lanecap = newcap;
	MemoryContextSwitchTo(old);
}

/* A lane with no vector.  Its code bytes are zeroed so the page image does not
 * depend on what the buffer held -- livemask, not content, is what makes a lane
 * live, but a nondeterministic page image is a defect on its own (sect. 7). */
static void
vec_accum_add_dead(WeaveVecAccum *acc, ItemPointer tid)
{
	vec_accum_grow(acc);
	if (acc->code != NULL)
		MemSet(acc->code + (Size) acc->nlane * acc->codebytes, 0, acc->codebytes);
	acc->scale[acc->nlane] = 0.0f;
	acc->norm[acc->nlane] = 0.0f;
	acc->live[acc->nlane] = 0;
	acc->docid[acc->nlane] = weave_tid_to_docid(tid);
	acc->nlane++;
}

void
weave_vec_accum_add(WeaveVecAccum *acc, Relation index, ItemPointer tid,
					Datum value, bool isnull)
{
	WVec	   *v;
	MemoryContext old;
	float		norm = 0;
	float		scale = 0;

	if (!acc->active)
		return;

	/*
	 * A NULL vector occupies its lane and leaves it dead.  Skipping it instead
	 * would shift every later document's warp position by one, and since the
	 * docid space is shared with the lexical weft (doc/ARCHITECTURE.md sect. 3)
	 * that mis-associates every vector after the first NULL with somebody else's
	 * document -- a wrong answer that no count-based test can see.
	 */
	if (isnull)
	{
		vec_accum_add_dead(acc, tid);
		return;
	}

	v = DatumGetWVec(value);

	if (!acc->ready)
	{
		if (v->dim <= 0 || v->dim > WVEC_MAX_DIM)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("wvec dimension %d is out of range for index \"%s\"",
							v->dim, RelationGetRelationName(index))));
		old = MemoryContextSwitchTo(acc->ctx);
		if (weave_quantizer_init(&acc->q, v->dim, acc->bits, NULL,
								 palloc, pfree) != 0)
		{
			MemoryContextSwitchTo(old);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot index a %d-dimensional wvec at %d bits",
							v->dim, acc->bits),
					 errdetail("The analytic codebook is fit from a Beta shape parameter of (dim-3)/2, so it exists only for dim >= 4."),
					 errhint("Use at least 4 dimensions, or drop the vector column from the index.")));
		}
		MemoryContextSwitchTo(old);
		acc->dim = v->dim;
		acc->codebytes = acc->q.codebytes;
		acc->ready = true;

		/*
		 * Lanes that arrived NULL before the dimension was known have no code
		 * bytes yet.  Allocate the code array to the capacity already reached and
		 * zero ALL of it, so those earlier lanes hold zeros rather than whatever
		 * the allocator handed us -- their livemask bit is clear either way, but
		 * the page image must not depend on it.
		 */
		if (acc->lanecap > 0)
		{
			MemoryContext o2 = MemoryContextSwitchTo(acc->ctx);
			Size		sz = (Size) acc->lanecap * (Size) acc->codebytes;

			acc->code = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE(sz);
			MemSet(acc->code, 0, sz);
			MemoryContextSwitchTo(o2);
		}
	}
	else if (v->dim != acc->dim)
	{
		/*
		 * A wvec column with no typmod can hold vectors of different lengths, and
		 * one weft has one geometry.  Refuse rather than pad or truncate: either
		 * would produce distances that are wrong in a way nothing checks.
		 */
		int			got = v->dim;

		if ((Pointer) v != DatumGetPointer(value))
			pfree(v);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("wvec has %d dimensions but index \"%s\" indexes %d",
						got, RelationGetRelationName(index), acc->dim),
				 errhint("Declare the column as wvec(%d) so the mismatch is refused at INSERT time.",
						 acc->dim)));
	}

	vec_accum_grow(acc);

	if (weave_encode(&acc->q, v->x,
					 acc->code + (Size) acc->nlane * acc->codebytes,
					 &norm, &scale) != 0)
	{
		/*
		 * A zero (or denormal-norm) vector has no direction: its scale is
		 * undefined and encoding zeros would produce a row that matches every
		 * query equally well (weave_encode's contract in weave/quantize.h).  Dead
		 * lane, same as NULL -- the document keeps its warp position.
		 */
		if ((Pointer) v != DatumGetPointer(value))
			pfree(v);
		vec_accum_add_dead(acc, tid);
		return;
	}

	acc->scale[acc->nlane] = scale;
	acc->norm[acc->nlane] = norm;
	acc->live[acc->nlane] = 1;
	acc->docid[acc->nlane] = weave_tid_to_docid(tid);
	acc->nlane++;
	acc->nlive++;

	if ((Pointer) v != DatumGetPointer(value))
		pfree(v);
}

/* ---------------------------------------------------------------------------
 * The chain writer
 *
 * Both of a weft's chains are laid out block-major and therefore interleave: a
 * block's code strips are followed by its directory record, which lands on a page
 * shared with the next 27 blocks' records.  So two chains are open at once, each
 * holding its previous page pinned, locked and inside a GenericXLog cycle until
 * the next page of that chain exists (the pattern weave_write_surf() uses, for the
 * same reason: the link and the new page belong in one atomic cycle).
 *
 * WHY TWO HELD PAGES CANNOT DEADLOCK.  Every page here was just allocated by this
 * backend and is unreachable from the metapage until the VMETA page is written and
 * the bolt directory commits, so no other backend can be waiting on either buffer.
 * The only lock taken while they are held is the relation extension lock inside
 * weave_new_buffer(), which every other chain writer in this access method also
 * takes while holding a page.
 * ------------------------------------------------------------------------- */

typedef struct VecChain
{
	Relation	index;
	WeavePageKind kind;
	BlockNumber first;
	Buffer		buf;			/* the open page, or InvalidBuffer */
	Page		page;
	GenericXLogState *state;
} VecChain;

static void
vec_chain_init(VecChain *c, Relation index, WeavePageKind kind)
{
	c->index = index;
	c->kind = kind;
	c->first = InvalidBlockNumber;
	c->buf = InvalidBuffer;
	c->page = NULL;
	c->state = NULL;
}

/*
 * Append a page to the chain and return it, ready to be filled.  The previous
 * page's nextblk is set and its cycle closed in the process, so at most one page
 * per chain is ever open.
 */
static Page
vec_chain_append(VecChain *c)
{
	Buffer		buf = weave_new_buffer(c->index);
	BlockNumber blk = BufferGetBlockNumber(buf);
	GenericXLogState *state = GenericXLogStart(c->index);
	Page		page = GenericXLogRegisterBuffer(state, buf,
												 GENERIC_XLOG_FULL_IMAGE);

	weave_init_page(page, c->kind);

	if (c->buf != InvalidBuffer)
	{
		WeavePageGetOpaque(c->page)->nextblk = blk;
		GenericXLogFinish(c->state);
		UnlockReleaseBuffer(c->buf);
	}
	else
		c->first = blk;

	c->buf = buf;
	c->page = page;
	c->state = state;
	return page;
}

static void
vec_chain_close(VecChain *c)
{
	if (c->buf == InvalidBuffer)
		return;
	GenericXLogFinish(c->state);
	UnlockReleaseBuffer(c->buf);
	c->buf = InvalidBuffer;
	c->page = NULL;
	c->state = NULL;
}

/* Mark how much of a page the writer filled.  Readers do NOT consult this -- they
 * pass the fixed WEAVE_VECPAGE_PAYLOAD, because the strip and directory decoders
 * validate their own headers and the writer zeroed the whole payload.  It is set
 * so weave_index_size_detail()'s free-space column tells the truth about how well
 * these pages pack, which is the number that decides whether the low-dim waste
 * sect. 7.1 records is worth fixing. */
static void
vec_page_used(Page page, int nbytes)
{
	((PageHeader) page)->pd_lower =
		(char *) PageGetContents(page) - (char *) page + nbytes;
}

/* ---------------------------------------------------------------------------
 * The writer
 * ------------------------------------------------------------------------- */

/*
 * The order the lanes are written in: ascending docid.
 *
 * NOT the order the callback saw them, and the difference is a bug this task's own
 * regression test caught.  A heap scan does not visit tuples in ascending ctid
 * order -- RelationGetBufferForTuple() places a small row on an earlier page when
 * the current one has no room for it, and a row whose vector is NULL is exactly the
 * small one -- so callback order made warp i "the i-th tuple the scan reached".
 * Two things break:
 *
 *	 - "the warp is the bolt's dense docid space" (doc/ARCHITECTURE.md sect. 3) is
 *	   the property every cross-modal skip rests on, and it is a claim about docids.
 *	 - the page image stops being deterministic, which sect. 7 forbids: a
 *	   synchronized seqscan may start anywhere in the relation, so two builds of one
 *	   table would order the same lanes differently and differ on disk.
 *
 * Returns a palloc'd permutation: perm[w] is the accumulator index of warp w.
 */
static int
vec_cmp_docid(const void *a, const void *b)
{
	uint64		x = ((const uint64 *) a)[0];
	uint64		y = ((const uint64 *) b)[0];

	/* No tie is possible: a docid is derived from a ctid and every heap tuple has
	 * its own.  Compared as unsigned, because a subtraction would overflow. */
	if (x < y)
		return -1;
	return x > y ? 1 : 0;
}

static uint32 *
vec_docid_order(const WeaveVecAccum *acc)
{
	/* (docid, accumulator index) pairs, sorted by the first: one array so the sort
	 * moves both halves together without an indirection per comparison. */
	uint64	   *pair = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->nlane * 2 * sizeof(uint64));
	uint32	   *perm = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->nlane * sizeof(uint32));
	uint32		i;

	for (i = 0; i < acc->nlane; i++)
	{
		pair[2 * i] = acc->docid[i];
		pair[2 * i + 1] = (uint64) i;
	}
	qsort(pair, acc->nlane, 2 * sizeof(uint64), vec_cmp_docid);
	for (i = 0; i < acc->nlane; i++)
		perm[i] = (uint32) pair[2 * i + 1];
	pfree(pair);
	return perm;
}

/*
 * One block's statistics, computed from the codes that were just packed.
 *
 * The reconstructions are decoded from the PACKED BLOCK rather than from the
 * accumulator's codes, and that is not a detour.  Sect. 7.3 requires the merge
 * producer to recompute these statistics from codes it MOVED -- it has no floats
 * and must not re-encode -- so computing them the same way here means the two
 * producers cannot disagree about the same block, and it exercises
 * weave_pack_lane/weave_unpack_lane as a pair on every build.
 *
 * `recon`, `lane_*`, `slot`, `cen` and `cencode` are caller-owned scratch, sized
 * for a full block, so this allocates nothing per block.
 */
static void
vec_block_stats(const WeaveVecAccum *acc, const WeaveVecWeftGeom *g,
				const uint32 *perm, uint32 blockno, const uint8 *block,
				WeaveVecDirRec *rec, float *recon, float *lane_scale,
				float *lane_norm, int *slot, float *cen, uint8 *cencode,
				uint8 *tmpcode)
{
	uint32		first = blockno * (uint32) WEAVE_VEC_BLOCK;
	int			nlanes = weave_vecweft_block_lanes(g, blockno);
	int			nlive = 0;
	int			s;

	for (s = 0; s < nlanes; s++)
	{
		uint32		lane = perm[first + (uint32) s];

		if (!acc->live[lane])
			continue;
		weave_unpack_lane(acc->layout, g->dim, g->bits, block, s, tmpcode);
		weave_decode(&acc->q, tmpcode, acc->scale[lane],
					 recon + (Size) nlive * g->dim);
		/* firstwarp is the WARP position of lane 0, which after the docid sort is
		 * blockno*32 -- the lane's position in the bolt's dense docid space, not
		 * the accumulator index `lane` it came from. */
		lane_scale[nlive] = acc->scale[lane];
		lane_norm[nlive] = acc->norm[lane];
		slot[nlive] = s;
		nlive++;
	}

	if (nlive == 0)
	{
		/*
		 * Every lane of this block is dead -- 32 consecutive documents with no
		 * vector, which a NULL-heavy column produces routinely.  There is no
		 * centroid and no bound to state, so the record is zeros plus firstwarp
		 * and the centroid code is zeros.  weave_vecdir_read() reports such a
		 * block ("block has no live lanes") while still returning it, because it
		 * is a legitimate state and not a corruption: vacuum produces it too.
		 */
		MemSet(rec, 0, sizeof(*rec));
		rec->firstwarp = first;
		MemSet(cencode, 0, (Size) g->codebytes);
		return;
	}

	if (weave_vecblock_stats(rec, &acc->q, recon, lane_scale, lane_norm, slot,
							 nlive, first, cen, cencode) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not compute vector block statistics for block %u",
						blockno)));
}

BlockNumber
weave_vec_write_weft(Relation index, WeaveVecAccum *acc)
{
	WeaveVecWeftGeom g;
	VecChain	codes;
	VecChain	dir;
	uint8	   *block;
	uint8	   *cencode;
	uint8	   *tmpcode;
	float	   *recon;
	float	   *cen;
	float		lane_scale[WEAVE_VEC_BLOCK];
	float		lane_norm[WEAVE_VEC_BLOCK];
	int			slot[WEAVE_VEC_BLOCK];
	uint32		b;
	uint32	   *perm;
	BlockNumber root;
	Page		page;

	if (!acc->active || !acc->ready || acc->nlane == 0)
		return InvalidBlockNumber;

	/*
	 * Not one live lane: every document in this segment had a NULL or zero
	 * vector.  Write NOTHING -- no VMETA page, no descriptor entry, zero bytes.
	 * A weft of nothing but dead lanes carries no information and would still
	 * cost ceil(nvec/32) * (dim/509 + 1) pages, and sect. 7.2's promise is that
	 * an index without vector data costs literally zero vector bytes.  The
	 * absence is safe to read: a bolt with no VECTOR descriptor is a bolt the
	 * scan skips, which is the correct answer for a segment with no vectors.
	 */
	if (acc->nlive == 0)
		return InvalidBlockNumber;

	if (weave_vecweft_geom(&g, WEAVE_VECPAGE_PAYLOAD, acc->dim, acc->bits,
						   (int) acc->layout, acc->nlane) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot store a vector weft of %d dimensions at %d bits",
						acc->dim, acc->bits)));

	perm = vec_docid_order(acc);
	block = (uint8 *) palloc(g.blockbytes);
	tmpcode = (uint8 *) palloc(g.codebytes);
	cencode = (uint8 *) palloc(g.codebytes);
	cen = (float *) palloc((Size) g.dim * sizeof(float));
	recon = (float *) palloc((Size) WEAVE_VEC_BLOCK * g.dim * sizeof(float));

	vec_chain_init(&codes, index, WEAVE_PK_VCODES);
	vec_chain_init(&dir, index, WEAVE_PK_VDIR);

	for (b = 0; b < g.nblocks; b++)
	{
		WeaveVecDirRec rec;
		uint32		first = b * (uint32) WEAVE_VEC_BLOCK;
		int			nlanes = weave_vecweft_block_lanes(&g, b);
		int			dirslot = weave_vecdir_slot_index(b, g.rpp);
		int			s;
		int			k;

		CHECK_FOR_INTERRUPTS();

		/*
		 * ZERO THE BLOCK BUFFER BEFORE PACKING.  weave_block_codebytes() rounds
		 * the per-vector code size up 32 times instead of once, so a block buffer
		 * carries 0-28 bytes that no pack function ever writes (the note above
		 * weave_block_codebytes() in weave/quantize.h, and sect. 7).  Those bytes
		 * land on a page: uninitialized, they make the page image
		 * nondeterministic, which breaks a GenericXLog delta over a rewritten page
		 * and any cross-architecture fixture hash of a VCODES page.  A dead lane's
		 * bits are zeroed for the same reason -- livemask is what makes a lane
		 * live, but the image must not depend on what the allocator handed us.
		 */
		MemSet(block, 0, g.blockbytes);
		for (s = 0; s < nlanes; s++)
		{
			uint32		lane = perm[first + (uint32) s];

			if (!acc->live[lane])
				continue;
			weave_pack_lane(acc->layout, g.dim, g.bits, block, s,
							acc->code + (Size) lane * acc->codebytes);
		}

		vec_block_stats(acc, &g, perm, b, block, &rec, recon, lane_scale,
						lane_norm, slot, cen, cencode, tmpcode);

		/* A fresh directory page every rpp records; record `b` goes in slot
		 * b % rpp of it, which is the O(1) addressing the fixed-size record buys
		 * (weave_vecdir_page_index / weave_vecdir_slot_index). */
		if (dirslot == 0)
		{
			uint32		nrecs = g.nblocks - b;

			if (nrecs > (uint32) g.rpp)
				nrecs = (uint32) g.rpp;
			page = vec_chain_append(&dir);
			if (weave_vecdir_page_init(PageGetContents(page),
									   WEAVE_VECPAGE_PAYLOAD,
									   WEAVE_VECPAGE_PAYLOAD, b,
									   (int) nrecs) != 0)
				elog(ERROR, "could not initialize a vector directory page for block %u", b);
			vec_page_used(page, (int) (sizeof(WeaveVecDirHdr) +
									   (Size) nrecs * sizeof(WeaveVecDirRec)));
		}
		if (weave_vecdir_write(PageGetContents(dir.page), WEAVE_VECPAGE_PAYLOAD,
							   WEAVE_VECPAGE_PAYLOAD, dirslot, &rec) != 0)
			elog(ERROR, "could not write the directory record for vector block %u", b);

		/*
		 * The block's strips, in the order weave_vecweft_strip_plan() defines:
		 * lane strips then centroid strips, block-major.  ONE page per strip; the
		 * greedy several-strips-per-page packing sect. 7.1 leaves to the writer is
		 * not implemented, so the low-dim waste it describes is real and recorded
		 * rather than claimed away.
		 */
		for (k = 0; k < g.strips_per_block; k++)
		{
			WeaveVecStripPlan p;
			uint32		i = b * (uint32) g.strips_per_block + (uint32) k;
			int			n;

			if (weave_vecweft_strip_plan(&g, i, &p) != 0)
				elog(ERROR, "no plan for vector strip %u of %u", i, g.nstrips);

			page = vec_chain_append(&codes);
			if ((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0)
				n = weave_censtrip_build(PageGetContents(page),
										 WEAVE_VECPAGE_PAYLOAD, &g, &p, cencode);
			else
				n = weave_strip_build(PageGetContents(page),
									  WEAVE_VECPAGE_PAYLOAD, acc->layout,
									  g.dim, g.bits, p.blockno, p.j0,
									  p.ncoords, p.flags, block);
			if (n < 0)
				elog(ERROR, "could not build vector strip %u (block %u, j0 %d)",
					 i, p.blockno, p.j0);
			vec_page_used(page, n);
		}
	}

	vec_chain_close(&codes);
	vec_chain_close(&dir);

	/*
	 * The VMETA page LAST, so every root it names is known -- the same ordering
	 * rule weave_attach_chandesc() follows, and for the same reason: a root that
	 * is computed rather than recorded is how a sibling project wrote one chain
	 * over another's data four times (doc/specs/SEGMENT_FORMAT.md sect. 8 item 5).
	 */
	{
		Buffer		buf = weave_new_buffer(index);
		GenericXLogState *state = GenericXLogStart(index);
		WeaveVecMeta *m;

		root = BufferGetBlockNumber(buf);
		page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
		weave_init_page(page, WEAVE_PK_VMETA);
		m = (WeaveVecMeta *) PageGetContents(page);
		MemSet(m, 0, sizeof(*m));
		m->magic = WEAVE_VMETA_MAGIC;
		m->version = WEAVE_VMETA_VERSION;
		m->dim = (uint16) g.dim;
		m->bits = (uint8) g.bits;
		m->metric = (uint8) acc->metric;
		m->layout = (uint8) acc->layout;
		m->flags = 0;			/* inputs are not unit-normalized; no graph */
		m->nvec = acc->nlane;
		m->nblocks = g.nblocks;
		m->dirstart = dir.first;
		m->codestart = codes.first;
		m->graphstart = InvalidBlockNumber;	/* V9 */

		/*
		 * TQ+ calibration stays OFF, and that is a measurement rather than an
		 * omission: doc/specs/VECTOR_CHANNEL.md sect. 5 records it as harmful at
		 * the widths and dimensions this channel ships.  Invalid means "identity",
		 * so a reader needs no flag to know there is nothing to apply.
		 */
		m->calibstart = InvalidBlockNumber;
		m->calibsample = 0;
		m->calibtime = 0;

		vec_page_used(page, (int) sizeof(WeaveVecMeta));
		GenericXLogFinish(state);
		UnlockReleaseBuffer(buf);
	}

	pfree(recon);
	pfree(cen);
	pfree(cencode);
	pfree(tmpcode);
	pfree(block);
	pfree(perm);

	elog(DEBUG1, "pg_weave build: index \"%s\": vector weft of %u lanes (%u live) in %u blocks, %u strips, %u directory pages",
		 RelationGetRelationName(index), acc->nlane, acc->nlive, g.nblocks,
		 g.nstrips, g.ndirpages);
	return root;
}

/* ---------------------------------------------------------------------------
 * The reader
 *
 * Every failure is a returned false plus a static string, never an ereport.  Two
 * of the three callers cannot afford a throw: weave_check() must report a corrupt
 * weft as a violated invariant and carry on (a PG_CATCH without a subtransaction
 * is the worse trade -- see weave_read_chandesc()), and weave_free_segment() runs
 * inside a merge, where a throw is how an index becomes permanently unvacuumable
 * (doc/GAPS.md G15).
 * ------------------------------------------------------------------------- */

/* Read the VMETA page's contents, validating only what makes the struct
 * meaningful.  Split from weave_vec_weft_open() because the free path wants the
 * two chain roots out of a page whose geometry it does not care about: a weft
 * whose dim is garbage still has pages to reclaim. */
static bool
vec_meta_read(Relation index, BlockNumber root, WeaveVecMeta *out,
			  const char **why)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	Buffer		buf;
	Page		page;

	*why = NULL;
	if (root == InvalidBlockNumber || root == WEAVE_METAPAGE_BLKNO ||
		root >= nblocks)
	{
		*why = "vector weft root block is out of range";
		return false;
	}

	buf = ReadBuffer(index, root);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (PageIsNew(page))
	{
		UnlockReleaseBuffer(buf);
		*why = "the vector weft root page is uninitialized";
		return false;
	}
	/* WeavePageHasKind(), never `flags & WEAVE_VMETA`: WEAVE_PK_VMETA is an
	 * extended kind id (17) and a bitwise test against it is always false
	 * (include/weave/vector.h, task X1). */
	if (!WeavePageHasKind(page, WEAVE_PK_VMETA))
	{
		UnlockReleaseBuffer(buf);
		*why = "the vector weft root page is not a WEAVE_VMETA page";
		return false;
	}
	memcpy(out, PageGetContents(page), sizeof(WeaveVecMeta));
	UnlockReleaseBuffer(buf);

	if (out->magic != WEAVE_VMETA_MAGIC)
	{
		*why = "the WEAVE_VMETA page has the wrong magic";
		return false;
	}
	if (out->version != WEAVE_VMETA_VERSION)
	{
		/* An unknown version is an ERROR-class refusal, not a best-effort read
		 * (doc/CONVENTIONS.md decision 3) -- reported, because this reader does
		 * not throw. */
		*why = "the WEAVE_VMETA page is a format version this build does not read";
		return false;
	}
	return true;
}

bool
weave_vec_weft_open(Relation index, BlockNumber root, WeaveVecWeft *out,
					const char **why)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	MemSet(out, 0, sizeof(*out));
	out->index = index;
	out->root = root;

	if (!vec_meta_read(index, root, &out->meta, why))
		return false;

	if (out->meta.layout != (uint8) WEAVE_PACK_LANE)
	{
		/*
		 * A VECMAJOR weft cannot be stored as coordinate strips at all -- a
		 * coordinate cut splits vectors there -- so a page chain claiming to be
		 * one is refused rather than reinterpreted.  weave/vecpage.h states the
		 * rule; this is the reader honouring it.
		 */
		*why = "the vector weft claims a pack layout this page format cannot hold";
		return false;
	}
	if (out->meta.nvec == 0)
	{
		*why = "the vector weft claims no lanes";
		return false;
	}
	if (out->meta.dirstart == InvalidBlockNumber ||
		out->meta.dirstart == WEAVE_METAPAGE_BLKNO ||
		out->meta.dirstart >= nblocks ||
		out->meta.codestart == InvalidBlockNumber ||
		out->meta.codestart == WEAVE_METAPAGE_BLKNO ||
		out->meta.codestart >= nblocks)
	{
		*why = "a vector weft chain root is out of range";
		return false;
	}
	if (weave_vecweft_geom(&out->geom, WEAVE_VECPAGE_PAYLOAD, out->meta.dim,
						   out->meta.bits, (int) out->meta.layout,
						   out->meta.nvec) != 0)
	{
		*why = "the vector weft's geometry cannot be stored in this page size";
		return false;
	}
	if (out->geom.nblocks != out->meta.nblocks)
	{
		*why = "the vector weft's block count disagrees with its lane count";
		return false;
	}
	return true;
}

BlockNumber
weave_vec_weft_root(Relation index, const WeaveSegMeta *seg)
{
	WeaveChannelDesc weft[WEAVE_MAX_WEFTS];
	int			nweft = 0;
	int			i;

	if (seg->chandesc == InvalidBlockNumber)
		return InvalidBlockNumber;
	if (weave_read_chandesc(index, seg->chandesc, weft, WEAVE_MAX_WEFTS,
							&nweft) != WEAVE_CD_OK)
		return InvalidBlockNumber;
	for (i = 0; i < nweft; i++)
		if (weft[i].kind == (uint16) WEAVE_WK_VECTOR)
			return weft[i].root;
	return InvalidBlockNumber;
}

bool
weave_vec_dir_read(const WeaveVecWeft *w, uint32 blockno, WeaveVecDirRec *out,
				   const char **why)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(w->index);
	BlockNumber blk = w->meta.dirstart;
	int			want = weave_vecdir_page_index(blockno, w->geom.rpp);
	int			slot = weave_vecdir_slot_index(blockno, w->geom.rpp);
	int			p;

	*why = NULL;
	if (blockno >= w->geom.nblocks || want < 0 || slot < 0)
	{
		*why = "no such vector block";
		return false;
	}

	/*
	 * THE O(1) IS THE OFFSET, NOT THE PAGE.  A fixed-size record makes record i's
	 * page ordinal and slot pure arithmetic, which is what sect. 7.1 bought and
	 * what "score block i" needs -- but the directory is a nextblk CHAIN and the
	 * format stores no index over its pages, so reaching page ordinal `want`
	 * follows `want` links.  Cheap for the directory (one page per 28 blocks; 112
	 * pages at n=1M/960-d) and it is stated rather than implied because sect. 7.1
	 * reads as though the addressing were O(1) end to end.  See the same note on
	 * weave_vec_block_read(), where it is not cheap.
	 */
	for (p = 0; p <= want; p++)
	{
		Buffer		buf;
		Page		page;
		const WeaveVecDirHdr *h;
		bool		last = (p == want);
		bool		ok = true;

		CHECK_FOR_INTERRUPTS();
		if (blk == InvalidBlockNumber || blk == WEAVE_METAPAGE_BLKNO ||
			blk >= nblocks)
		{
			*why = "the vector directory chain ends before the block it must hold";
			return false;
		}
		buf = ReadBuffer(w->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_VDIR))
		{
			UnlockReleaseBuffer(buf);
			*why = "a block on the vector directory chain is not a directory page";
			return false;
		}
		h = (const WeaveVecDirHdr *) PageGetContents(page);
		if (h->first_blockno != (uint32) p * (uint32) w->geom.rpp)
		{
			/*
			 * The cross-check the page header exists for: a mislinked chain would
			 * otherwise answer for the wrong blocks, silently, and every bound the
			 * scan derived from it would belong to somebody else's 32 documents.
			 */
			UnlockReleaseBuffer(buf);
			*why = "a vector directory page does not start where the O(1) formula says it does";
			return false;
		}
		if (last)
			ok = weave_vecdir_read(PageGetContents(page), WEAVE_VECPAGE_PAYLOAD,
								   WEAVE_VECPAGE_PAYLOAD, slot, out, why) == 0;
		else
			blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
		if (last)
		{
			/*
			 * weave_vecdir_read() sets *why for a block with no live lanes while
			 * still returning success, because that is a legitimate state.  Only
			 * the return value decides.
			 */
			if (!ok)
				return false;
			*why = NULL;
			return true;
		}
	}
	*why = "the vector directory chain is shorter than its block count";
	return false;
}

bool
weave_vec_block_read(const WeaveVecWeft *w, uint32 blockno, uint8 *block,
					 uint8 *cencode, const char **why)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(w->index);
	BlockNumber blk = w->meta.codestart;
	bool	   *seen;
	int			nseen = 0;
	int			npages = 0;
	int			k;
	bool		ok = true;

	*why = NULL;
	if (blockno >= w->geom.nblocks)
	{
		*why = "no such vector block";
		return false;
	}

	MemSet(block, 0, w->geom.blockbytes);
	if (cencode != NULL)
		MemSet(cencode, 0, w->geom.codebytes);
	seen = (bool *) palloc0((Size) w->geom.strips_per_block * sizeof(bool));

	/*
	 * THE WALK IS THE COST, and it is not the one sect. 7.1 advertises.  Strips
	 * are block-major, so this block's strips are consecutive in the chain -- but
	 * nothing records WHERE, and WeaveVecDirRec is full (284 bytes, fixed by the
	 * O(1) requirement), so there is no room to record it without a format change.
	 * A reader therefore walks from codestart, which is O(pages in the weft) and
	 * not the ceil(dim/coords_per_page) the block-major argument promises.  V7's
	 * callers walk every block anyway (weave_check(), the round-trip test); V8's
	 * rerank window and vacuum's lane update cannot, and that is the task that has
	 * to add the index.  Recorded in sect. 7.1 as a correction, not left implicit.
	 */
	while (blk != InvalidBlockNumber && ok)
	{
		Buffer		buf;
		Page		page;
		const WeaveVecStripHdr *raw;
		WeaveVecStripHdr hdr;

		CHECK_FOR_INTERRUPTS();
		if (blk == WEAVE_METAPAGE_BLKNO || blk >= nblocks ||
			++npages > (int) nblocks)
		{
			*why = "the vector code chain leaves the relation or cycles";
			ok = false;
			break;
		}
		buf = ReadBuffer(w->index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || !WeavePageHasKind(page, WEAVE_PK_VCODES))
		{
			UnlockReleaseBuffer(buf);
			*why = "a block on the vector code chain is not a code page";
			ok = false;
			break;
		}
		raw = (const WeaveVecStripHdr *) PageGetContents(page);
		if (raw->blockno == blockno)
		{
			const uint8 *bytes = NULL;
			int			n;
			bool		iscen = (raw->flags & WEAVE_VSTRIP_F_CENTROID) != 0;

			if (iscen)
				n = weave_censtrip_parse(PageGetContents(page),
										 WEAVE_VECPAGE_PAYLOAD, &w->geom,
										 &hdr, &bytes, why);
			else
				n = weave_strip_parse(PageGetContents(page),
									  WEAVE_VECPAGE_PAYLOAD, w->geom.dim,
									  w->geom.bits, &hdr, &bytes, why);
			if (n < 0)
				ok = false;
			else
			{
				/*
				 * A strip is only accepted where the PLAN says it belongs.  This
				 * is what turns "the writer used the wrong j0" from a wrong answer
				 * into a detected fault: the coordinates a strip claims must be
				 * the coordinates its position in the weft calls for.
				 */
				int			want = -1;

				for (k = 0; k < w->geom.strips_per_block; k++)
				{
					WeaveVecStripPlan p;
					uint32		i = blockno * (uint32) w->geom.strips_per_block +
						(uint32) k;

					if (weave_vecweft_strip_plan(&w->geom, i, &p) != 0)
						continue;
					if (p.j0 == (int) hdr.j0 && p.ncoords == (int) hdr.ncoords &&
						((p.flags & WEAVE_VSTRIP_F_CENTROID) != 0) == iscen)
					{
						want = k;
						break;
					}
				}
				if (want < 0)
				{
					*why = "a vector strip carries a coordinate range the weft's layout does not call for";
					ok = false;
				}
				else if (seen[want])
				{
					*why = "two vector strips claim the same coordinate range of one block";
					ok = false;
				}
				else if (iscen)
				{
					if (cencode != NULL &&
						weave_censtrip_scatter(cencode, w->geom.codebytes,
											   &w->geom, &hdr, bytes) != 0)
					{
						*why = "a centroid strip does not fit the code it belongs to";
						ok = false;
					}
					else
					{
						seen[want] = true;
						nseen++;
					}
				}
				else if (weave_strip_scatter(block, w->geom.blockbytes,
											 w->geom.dim, w->geom.bits,
											 &hdr, bytes) != 0)
				{
					*why = "a lane strip does not fit the block it belongs to";
					ok = false;
				}
				else
				{
					seen[want] = true;
					nseen++;
				}
			}
		}
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
	}

	if (ok && nseen != w->geom.strips_per_block)
	{
		*why = "the vector code chain is missing strips for this block";
		ok = false;
	}
	pfree(seen);
	return ok;
}

void
weave_vec_free_weft(Relation index, BlockNumber root)
{
	WeaveVecMeta meta;
	const char *why = NULL;

	if (root == InvalidBlockNumber)
		return;

	/*
	 * Free the two chains the VMETA page names, then the page itself.  Reading it
	 * first is the only way to find them: sect. 7.1 puts a weft's roots in the
	 * weft's own header rather than in WeaveSegMeta, which is what lets a bolt
	 * without vectors cost zero bytes -- and it means a free path that only walked
	 * the descriptor's root would reclaim ONE page and leak every code and
	 * directory page behind it.  weave_check(deep)'s pages_reachable_or_freed is
	 * what notices, and doc/specs/SEGMENT_FORMAT.md sect. 6 is where that argument
	 * is made for the descriptor page itself.
	 *
	 * If the VMETA page cannot be read the chains are unreachable and stay
	 * leaked -- reported by that same invariant.  Freeing the root anyway, and not
	 * throwing, is deliberate: this runs inside a merge.
	 */
	if (vec_meta_read(index, root, &meta, &why))
	{
		if (meta.codestart != InvalidBlockNumber)
			weave_free_chain(index, meta.codestart);
		if (meta.dirstart != InvalidBlockNumber)
			weave_free_chain(index, meta.dirstart);
		if (meta.graphstart != InvalidBlockNumber)
			weave_free_chain(index, meta.graphstart);
		if (meta.calibstart != InvalidBlockNumber)
			weave_free_chain(index, meta.calibstart);
	}
	else
		elog(DEBUG1, "pg_weave: freeing vector weft at block %u: %s",
			 root, why != NULL ? why : "unreadable");
	weave_free_chain(index, root);
}

/* ---------------------------------------------------------------------------
 * SQL-callable introspection
 *
 * WHY IT IS HERE AND NOT IN src/am/amcheck.c.  amcheck.c owns the AM-wide
 * validator and weave_page_info(); these two read a format only the vector channel
 * understands, and putting them there would make the access method the owner of it.
 * The seam argument is the same one that put the writer in this file.
 *
 * WHY THEY EXIST AT ALL, which is not "for completeness".  Two of V7's properties
 * cannot be asserted from SQL without them, and both are the kind a count-based
 * test passes while broken:
 *
 *	 - a NULL vector must leave a DEAD LANE rather than shift the warp.  Every row
 *	   count is identical either way; the livemask is where the difference is.
 *	 - the geometry actually used must be the one the reloption asked for.  A weft
 *	   written at the wrong width scores wrongly and counts correctly.
 *
 * And a third, procedural reason, learned from the mutation run on this task's
 * predecessor (AGENTS.md, "the suite running is not the SITE running"): a SQL
 * assertion only tests the C function it actually reaches.  These two go through
 * weave_vec_weft_open() and weave_vec_dir_read(), so a mutation in either is
 * observable from SQL instead of hiding behind a query the planner answered some
 * other way.
 * ------------------------------------------------------------------------- */

/* Open an index by oid, refusing anything that is not a weave index, and read its
 * metapage.  Shared by both functions below; the metapage gate ERRORs on an unknown
 * version rather than reading segs[] at an offset it cannot justify. */
static Relation
vec_introspect_open(Oid indexoid, WeaveMetaPageData *meta)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	Buffer		mb;

	if (index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(index))));
	if (RelationGetNumberOfBlocks(index) == 0)
	{
		MemSet(meta, 0, sizeof(*meta));
		return index;			/* buildempty()/unbuilt: no rows, not an error */
	}
	mb = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	LockBuffer(mb, BUFFER_LOCK_SHARE);
	weave_check_meta(BufferGetPage(mb), index);
	weave_meta_from_page(BufferGetPage(mb), meta);
	UnlockReleaseBuffer(mb);
	return index;
}

static Tuplestorestate *
vec_introspect_tupstore(FunctionCallInfo fcinfo, TupleDesc *tupdesc)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Tuplestorestate *tupstore;
	MemoryContext oldcontext;

	if (rsinfo == NULL || !(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (get_call_result_type(fcinfo, NULL, tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = *tupdesc;
	MemoryContextSwitchTo(oldcontext);
	return tupstore;
}

/* The weft a bolt carries, or nothing.  Shared by both functions so a corrupt weft
 * is reported identically by each. */
static bool
vec_introspect_weft(Relation index, uint32 s, const WeaveSegMeta *seg,
					WeaveVecWeft *w)
{
	BlockNumber root;
	const char *why = NULL;

	if (seg->dictstart == InvalidBlockNumber)
		return false;			/* consumed slot */
	root = weave_vec_weft_root(index, seg);
	if (root == InvalidBlockNumber)
		return false;			/* this bolt carries no vector weft */
	if (!weave_vec_weft_open(index, root, w, &why))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("bolt %u of index \"%s\" has an unreadable vector weft at block %u",
						s, RelationGetRelationName(index), root),
				 errdetail("%s", why != NULL ? why : "unknown")));
	return true;
}

/*
 * weave_vec_meta(regclass) -> one row per bolt that carries a vector weft
 *
 * The geometry a reader would use, taken from the PAGE rather than from the
 * reloption: the two disagree exactly when the reloption changed after the weft was
 * written, which is legal (each bolt records the width it was built at) and is one
 * of the things this function exists to make visible.
 */
Datum
weave_vec_meta(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore = vec_introspect_tupstore(fcinfo, &tupdesc);
	WeaveMetaPageData meta;
	Relation	index = vec_introspect_open(indexoid, &meta);
	uint32		s;

	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveVecWeft w;
		Datum		values[10];
		bool		nulls[10];

		if (!vec_introspect_weft(index, s, &meta.segs[s], &w))
			continue;

		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum((int32) s);
		values[1] = Int64GetDatum((int64) w.root);
		values[2] = Int32GetDatum((int32) w.meta.dim);
		values[3] = Int32GetDatum((int32) w.meta.bits);
		values[4] = Int32GetDatum((int32) w.meta.metric);
		values[5] = Int32GetDatum((int32) w.meta.layout);
		values[6] = Int64GetDatum((int64) w.meta.nvec);
		values[7] = Int64GetDatum((int64) w.meta.nblocks);
		values[8] = Int64GetDatum((int64) w.meta.dirstart);
		values[9] = Int64GetDatum((int64) w.meta.codestart);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/*
 * weave_vec_blocks(regclass) -> one row per block of every vector weft
 *
 * Read through weave_vec_dir_read(), i.e. through the addressing a scan will use,
 * so this reports what a scan would see rather than what a second, separately
 * written walk would.  `nlive` is popcount(livemask) and `nlanes` is how many lane
 * slots the block covers (32 for all but the last), which is what makes "a NULL
 * vector left a dead lane" an assertion rather than an assumption.
 */
Datum
weave_vec_blocks(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore = vec_introspect_tupstore(fcinfo, &tupdesc);
	WeaveMetaPageData meta;
	Relation	index = vec_introspect_open(indexoid, &meta);
	uint32		s;

	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveVecWeft w;
		const char *why = NULL;
		uint32		b;

		if (!vec_introspect_weft(index, s, &meta.segs[s], &w))
			continue;

		for (b = 0; b < w.geom.nblocks; b++)
		{
			WeaveVecDirRec rec;
			Datum		values[11];
			bool		nulls[11];

			CHECK_FOR_INTERRUPTS();
			if (!weave_vec_dir_read(&w, b, &rec, &why))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("bolt %u of index \"%s\": block %u has no readable directory record",
								s, RelationGetRelationName(index), b),
						 errdetail("%s", why != NULL ? why : "unknown")));

			MemSet(nulls, 0, sizeof(nulls));
			values[0] = Int32GetDatum((int32) s);
			values[1] = Int64GetDatum((int64) b);
			values[2] = Int64GetDatum((int64) rec.firstwarp);
			values[3] = Int32GetDatum(weave_vecweft_block_lanes(&w.geom, b));
			values[4] = Int32GetDatum((int32) pg_popcount32(rec.livemask));
			values[5] = Int64GetDatum((int64) rec.livemask);
			values[6] = Float4GetDatum(rec.smax);
			values[7] = Float4GetDatum(rec.maxrecnorm);
			values[8] = Float4GetDatum(rec.minnorm);
			values[9] = Float4GetDatum(rec.censcale);
			values[10] = Float4GetDatum(rec.cenrad);
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	index_close(index, AccessShareLock);
	return (Datum) 0;
}
