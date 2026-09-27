/*-------------------------------------------------------------------------
 *
 * docvals_page.c
 *		Persist and reload the int8 docvalues store on a WEAVE_PK_DOCVALS
 *		page chain.
 *
 * The store format itself is backend-independent (include/weave/docvals.h):
 * a fixed WeaveDocvalsHeader followed by a dense int64 value array, one entry
 * per segment-local docid.  This file is the AM half -- the page chain the
 * image lives on, and the two ways to move it across that boundary:
 *
 *	 weave_docvals_write()  serialize header+values once, lay the bytes across a
 *							fresh chain of WEAVE_PK_DOCVALS pages, return the root.
 *	 weave_docvals_load()   walk the chain, reassemble one contiguous image, run
 *							the pure validator, and ERROR if it is not sound.
 *
 * WHY NOT weave_write_blob()/weave_read_blob().  Those hard-code
 * WEAVE_PK_TRGM_DATA and validate NO page kind at all -- they follow nextblk and
 * trust pd_lower (see the WEAVE_PK_SURF rationale in weave/am.h).  A docvalues
 * gate that trusted such bytes could silently emit the wrong docid set, which is
 * a wrong answer rather than an error (doc/CONVENTIONS.md decision 2).  So this
 * mirrors weave_write_surf()/weave_read_surf() exactly: its own page kind, a
 * reader that refuses any page that is not demonstrably a docvalues page, and a
 * pd_lower read that goes through weave_page_entry_end() so a torn header cannot
 * point the copy past the page (check-pdlower, doc/GAPS.md G22).
 *
 * WHY THERE IS NO LENGTH FIELD ON THE CHAIN.  The pages carry exactly the image
 * bytes and nothing else, so the image length is the sum of the pages' payloads
 * -- which the reader computes by walking the chain BEFORE it allocates.  The
 * store's own header carries ndocs, and weave_docvals_validate() rejects any
 * image whose reassembled length disagrees with values_off + ndocs*8, so a torn
 * chain is caught with no extra field to keep in step, and the allocation is
 * bounded by real relation pages rather than by a count read out of a possibly
 * corrupt header.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/pages/docvals_page.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "weave/am.h"
#include "weave/docvals.h"

#include "access/generic_xlog.h"
#include "catalog/pg_type_d.h"	/* INT8OID/INT4OID/.../FLOAT8OID for the type map */
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/date.h"			/* DateADT / DatumGetDateADT (date facet encode) */
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Image bytes one WEAVE_PK_DOCVALS page carries.  Identical shape to
 * WEAVE_SURFPAGE_PAYLOAD: a whole page minus the header and the opaque area.
 */
#define WEAVE_DOCVALS_PAYLOAD \
	(BLCKSZ - (int) MAXALIGN(SizeOfPageHeaderData) - (int) MAXALIGN(sizeof(WeavePageOpaqueData)))

/*
 * Serialize the int8 store (header + n int64 values + n uint64 global docids)
 * into one contiguous buffer.  Returns a palloc'd image and sets *len_out to its
 * byte length.
 *
 * The layout is byte-for-byte what weave_docvals_validate() expects and what the
 * WEAVE_DOCVALS_TEST_HELPERS build (weave_docvals_build) produces, so a store
 * written here reads back through the same validator the standalone property
 * test drives.  The docids array is the dense-index -> global-docid map the gate
 * emits through (see include/weave/docvals.h): it must be strictly ascending,
 * which weave_docvals_write_weft() guarantees by sorting and the validator
 * re-checks on the way back in.
 */
static uint8 *
docvals_serialize(const int64 *vals, const uint64 *docids,
				  const uint8 *nullbits, uint32 n, Size *len_out)
{
	WeaveDocvalsHeader h;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	uint32		doff = voff + n * 8u;
	uint32		noff = (uint32) WEAVE_DV_MAXALIGN((Size) doff + (Size) n * 8u);
	bool		has_nulls = (nullbits != NULL);
	Size		len;
	uint8	   *img;

	/*
	 * The header's local WEAVE_DV_MAXALIGN must agree with the backend's
	 * MAXALIGN, or the offset the store records and the offset a reader recomputes
	 * would differ.  docvals.h anticipates this assertion in the comment on
	 * WEAVE_DV_MAXALIGN; make it rather than assume it.  The second assert mirrors
	 * the first for the null-bitmap offset: it must equal the single MAXALIGNed
	 * post-docids offset the header/validator recompute (weave_docvals_validate()
	 * rejects any other value), so the byte the writer packs a bit into and the
	 * byte weave_docvals_isnull() reads it back from are the same byte.
	 */
	Assert(voff == (uint32) MAXALIGN(sizeof(WeaveDocvalsHeader)));
	Assert(noff == (uint32) MAXALIGN((Size) doff + (Size) n * 8u));

	/*
	 * check-alloc: n is corpus-scale (one value and one docid per docid in the
	 * segment), so the image size derives from a corpus-scale quantity and goes
	 * through the huge-safe path -- the exact allocation class behind four real
	 * crashes in this extension's ancestor (AGENTS.md's lint table).
	 * values_off + n*8 + n*8 is computed in Size; on a 64-bit target n (uint32) *
	 * 8 cannot wrap.  When a bitmap is present, the store runs to the aligned
	 * post-docids offset plus ceil(n/8) bytes (the v2 shape).
	 */
	if (has_nulls)
		len = (Size) noff + (Size) ((n + 7u) / 8u);
	else
		len = (Size) voff + (Size) n * 8 + (Size) n * 8;
	img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE(len);

	h.magic = WEAVE_DOCVALS_MAGIC;
	h.version = 2;
	h.typid_kind = 1;			/* int8 */
	h.ndocs = n;
	h.null_off = has_nulls ? noff : 0;
	h.zonemap_off = 0;
	h.values_off = voff;
	h.docids_off = doff;
	memcpy(img, &h, sizeof(h));

	if (n > 0)
	{
		memcpy(img + voff, vals, (Size) n * 8);
		memcpy(img + doff, docids, (Size) n * 8);
	}

	if (has_nulls)
	{
		uint32		nbytes = (n + 7u) / 8u;
		uint32		i;

		/*
		 * Pack the per-doc byte array into the bit-per-doc bitmap: bit i set iff
		 * docid i is NULL, byte i>>3 bit i&7 -- exactly the reader's shape in
		 * weave_docvals_isnull().  Zero the region first so unused trailing bits
		 * (beyond n) stay 0.
		 */
		memset(img + noff, 0, nbytes);
		for (i = 0; i < n; i++)
			if (nullbits[i])
				img[noff + (i >> 3)] |= (uint8) (1u << (i & 7u));
	}

	*len_out = len;
	return img;
}

BlockNumber
weave_docvals_write(Relation index, GenericXLogState *state,
					const int64 *vals, const uint64 *docids,
					const uint8 *nullbits, uint32 n)
{
	BlockNumber first = InvalidBlockNumber;
	Buffer		prevbuf = InvalidBuffer;
	Page		prevpage = NULL;
	GenericXLogState *prevstate = NULL;
	Size		len;
	Size		off = 0;
	uint8	   *img = docvals_serialize(vals, docids, nullbits, n, &len);

	Assert(len > 0);			/* always >= sizeof(WeaveDocvalsHeader) */

	/*
	 * The caller passes its GenericXLogState only to name the ordering contract
	 * it is part of: the payload pages are written HERE, each in its own
	 * GenericXLog cycle (so the chain has no page-count limit), and the root is
	 * returned only after every page exists.  The caller records that root LAST,
	 * in the channel descriptor, under `state` -- so a root that is recorded is
	 * always a root that exists, the ordering rule weave_attach_chandesc() and
	 * weave_vec_write_weft() both follow (a root computed rather than recorded is
	 * how a sibling project wrote one chain over another's data four times,
	 * doc/specs/SEGMENT_FORMAT.md sect. 8 item 5).
	 */
	(void) state;

	do
	{
		Buffer		buf = weave_new_buffer(index);
		BlockNumber blk = BufferGetBlockNumber(buf);
		GenericXLogState *pgstate = GenericXLogStart(index);
		Page		page = GenericXLogRegisterBuffer(pgstate, buf,
													 GENERIC_XLOG_FULL_IMAGE);
		Size		chunk = Min(len - off, (Size) WEAVE_DOCVALS_PAYLOAD);

		weave_init_page(page, WEAVE_PK_DOCVALS);
		memcpy((char *) PageGetContents(page), img + off, chunk);
		((PageHeader) page)->pd_lower =
			((char *) PageGetContents(page) - (char *) page) + chunk;

		if (prevbuf != InvalidBuffer)
		{
			WeavePageGetOpaque(prevpage)->nextblk = blk;
			GenericXLogFinish(prevstate);
			UnlockReleaseBuffer(prevbuf);
		}
		else
			first = blk;

		prevbuf = buf;
		prevpage = page;
		prevstate = pgstate;
		off += chunk;
	} while (off < len);

	/* The last page terminates the chain: its nextblk stays InvalidBlockNumber
	 * (weave_init_page leaves it so), which is what the reader's walk stops on. */
	GenericXLogFinish(prevstate);
	UnlockReleaseBuffer(prevbuf);
	pfree(img);
	return first;
}

/*
 * One pass over the chain from `root`.  With `dst` NULL it only measures and
 * validates the pages; with `dst` set it copies at most `cap` bytes into it.
 * Returns the payload byte count, or -1 with *detail set.
 *
 * ONE walker, TWO modes (the weave_surf_walk() shape): the measure pass sizes
 * the buffer the copy pass fills, and a disagreement between two separately
 * written walkers is a buffer overrun.
 */
static int64
docvals_walk(Relation index, BlockNumber root, uint8 *dst, Size cap,
			 const char **detail)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber blk = root;
	int64		total = 0;
	int64		npages = 0;

	*detail = NULL;
	if (blk == InvalidBlockNumber || blk == WEAVE_METAPAGE_BLKNO)
	{
		*detail = "docvalues weft root block is invalid";
		return -1;
	}

	while (blk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		Size		avail;

		CHECK_FOR_INTERRUPTS();	/* between pages, no buffer lock held */
		if (blk >= nblocks)
		{
			*detail = "docvalues chain leaves the relation";
			return -1;
		}
		if (++npages > (int64) nblocks)
		{
			*detail = "docvalues chain exceeds the relation length (cycle?)";
			return -1;
		}

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page))
		{
			UnlockReleaseBuffer(buf);
			*detail = "docvalues chain reaches an uninitialized page";
			return -1;
		}

		/*
		 * WeavePageHasKind(), never `flags & WEAVE_...`: WEAVE_PK_DOCVALS is an
		 * INTEGER id under the escape bit, so a bitwise AND compiles and is
		 * always false (weave/pagekind.h).  This is the L17 class of bug.
		 */
		if (!WeavePageHasKind(page, WEAVE_PK_DOCVALS))
		{
			UnlockReleaseBuffer(buf);
			*detail = "a block on the docvalues chain is not a docvalues page";
			return -1;
		}

		/*
		 * pd_lower comes off disk under BUFFER_LOCK_SHARE; weave_page_entry_end()
		 * validates it as an integer before forming a pointer (check-pdlower).
		 * Clamp to the payload so a torn header cannot make us read into the
		 * opaque area.
		 */
		avail = (Size) (weave_page_entry_end(page) -
						(char *) PageGetContents(page));
		if (avail > (Size) WEAVE_DOCVALS_PAYLOAD)
			avail = (Size) WEAVE_DOCVALS_PAYLOAD;
		if (dst != NULL)
		{
			if ((Size) total + avail > cap)
			{
				UnlockReleaseBuffer(buf);
				*detail = "docvalues chain grew between the measure and copy passes";
				return -1;
			}
			memcpy(dst + total, PageGetContents(page), avail);
		}
		total += (int64) avail;
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
	}

	if (total == 0)
	{
		*detail = "docvalues chain carries no bytes";
		return -1;
	}
	return total;
}

const void *
weave_docvals_load(Relation index, BlockNumber root,
				   MemoryContext cxt, uint32 *ndocs_out)
{
	const char *detail = NULL;
	const char *why;
	int64		len;
	uint8	   *img;
	WeaveDocvalsHeader h;
	MemoryContext old;

	if (ndocs_out != NULL)
		*ndocs_out = 0;

	/* Measure pass: page-level soundness only, no allocation yet. */
	len = docvals_walk(index, root, NULL, 0, &detail);
	if (len < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupt docvalues page chain in index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("%s (block %u)", detail, root),
				 errhint("REINDEX the index to rebuild it.")));

	/*
	 * check-alloc: bounded by pages that actually exist rather than a count read
	 * out of the image, so a corrupt header cannot ask for gigabytes -- but the
	 * honest bound is still corpus-scale (the whole value array), so this goes
	 * through the huge-safe path.  Allocated in the caller's context so the
	 * validated image outlives this call.
	 */
	old = MemoryContextSwitchTo(cxt);
	img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE((Size) len);
	MemoryContextSwitchTo(old);

	if (docvals_walk(index, root, img, (Size) len, &detail) != len)
	{
		pfree(img);
		if (detail == NULL)
			detail = "docvalues chain length changed between passes";
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupt docvalues page chain in index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("%s (block %u)", detail, root),
				 errhint("REINDEX the index to rebuild it.")));
	}

	/*
	 * The trust boundary.  Nothing above interpreted a byte of the payload; the
	 * pure validator is the ONE place the reassembled bytes are checked against
	 * the store format before any value is read (doc/CONVENTIONS.md decision 2).
	 */
	why = weave_docvals_validate(img, (size_t) len);
	if (why != NULL)
	{
		pfree(img);
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupt docvalues store in index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("%s (block %u)", why, root),
				 errhint("REINDEX the index to rebuild it.")));
	}

	if (ndocs_out != NULL)
	{
		memcpy(&h, img, sizeof(h));
		*ndocs_out = h.ndocs;
	}
	return (const void *) img;
}

/* ---------------------------------------------------------------------------
 * Build-time accumulator (WeaveDocvalsAccum, include/weave/am.h)
 *
 * One (docid, value) pair per indexed document, appended in heap-scan order by
 * the build callback; weave_docvals_write_weft() sorts them into
 * strictly-ascending docid order and lays down the store.  The mirror of the
 * vector accumulator (src/vector/vecwrite.c), kept far simpler because there is
 * nothing to quantize: a docvalue is its own bytes.
 * ------------------------------------------------------------------------- */

/*
 * Map a facet column's type OID to its WeaveDvType.  ERROR (not a default) on an
 * unsupported type: a column reaches here only by wearing a <type>_docval_ops
 * opclass, and every such opclass corresponds to a case below, so an unknown OID
 * means the opclass registry and this map disagree -- a build bug, not user error.
 */
WeaveDvType
weave_dv_type_for_oid(Oid atttypid)
{
	switch (atttypid)
	{
		case INT8OID:
			return WEAVE_DV_T_INT8;
		case INT4OID:
			return WEAVE_DV_T_INT4;
		case INT2OID:
			return WEAVE_DV_T_INT2;
		case BOOLOID:
			return WEAVE_DV_T_BOOL;
		case DATEOID:
			return WEAVE_DV_T_DATE;
		case FLOAT8OID:
			return WEAVE_DV_T_FLOAT8;
		default:
			elog(ERROR, "pg_weave: unsupported docvalues column type OID %u", atttypid);
			return WEAVE_DV_T_INT8; /* unreachable */
	}
}

/*
 * Encode one raw Datum of a supported facet type to the stored, order-preserving
 * int64 (weave/docvals.h).  Integer/date/bool widen (already order-preserving);
 * float8 goes through the monotonic transform.  The query constant is encoded by
 * the SAME rule at scan time, so weave_dv_eval_int8()'s signed comparison is exact.
 */
int64
weave_dv_encode_datum(WeaveDvType dvtype, Datum value)
{
	switch (dvtype)
	{
		case WEAVE_DV_T_INT8:
			return DatumGetInt64(value);
		case WEAVE_DV_T_INT4:
			return (int64) DatumGetInt32(value);
		case WEAVE_DV_T_INT2:
			return (int64) DatumGetInt16(value);
		case WEAVE_DV_T_BOOL:
			return DatumGetBool(value) ? 1 : 0;
		case WEAVE_DV_T_DATE:
			return (int64) DatumGetDateADT(value);
		case WEAVE_DV_T_FLOAT8:
			return weave_dv_encode_f8(DatumGetFloat8(value));
	}
	elog(ERROR, "pg_weave: unknown WeaveDvType %d", (int) dvtype);
	return 0;					/* unreachable */
}

int64
weave_dv_encode_const(WeaveDvType coltype, Oid consttype, Datum arg)
{
	/*
	 * A float8 column's gate compares in the float-encoded domain, so a constant
	 * of EITHER float type is widened to double and encoded the same way the
	 * stored values were; nothing else is a defined cross-type here.
	 */
	if (coltype == WEAVE_DV_T_FLOAT8)
	{
		double		d = (consttype == FLOAT4OID)
			? (double) DatumGetFloat4(arg)
			: DatumGetFloat8(arg);

		return weave_dv_encode_f8(d);
	}

	/*
	 * Integer family (int2/int4/int8/date/bool): every value is order-preserved by
	 * a plain widening to int64, so a cross-type integer constant is read at ITS
	 * OWN width and widened -- the stored column values were widened the same way,
	 * so the signed comparison is exact regardless of which integer width either
	 * side is.
	 */
	switch (consttype)
	{
		case INT2OID:
			return (int64) DatumGetInt16(arg);
		case INT4OID:
			return (int64) DatumGetInt32(arg);
		case INT8OID:
			return DatumGetInt64(arg);
		case DATEOID:
			return (int64) DatumGetDateADT(arg);
		case BOOLOID:
			return DatumGetBool(arg) ? 1 : 0;
		default:
			elog(ERROR, "pg_weave: unsupported docvalues constant type OID %u",
				 consttype);
			return 0;			/* unreachable */
	}
}

void
weave_docvals_accum_init(WeaveDocvalsAccum *acc, MemoryContext ctx, bool active,
						 WeaveDvType dvtype)
{
	acc->ctx = ctx;
	acc->active = active;
	acc->dvtype = dvtype;
	acc->docid = NULL;
	acc->value = NULL;
	acc->isnull = NULL;			/* lazily allocated alongside docid/value */
	acc->n = 0;
	acc->cap = 0;
	acc->nulls = 0;
}

void
weave_docvals_accum_reset(WeaveDocvalsAccum *acc)
{
	/*
	 * NULL the pointers and zero the counters, exactly like
	 * weave_vec_accum_reset(): the caller frees acc->ctx with
	 * MemoryContextReset() right after, so a pfree here would only pre-empt that,
	 * and a pointer left set would dangle into the freed context.  active and ctx
	 * survive so the next segment reuses the same accumulator.
	 */
	acc->docid = NULL;
	acc->value = NULL;
	acc->isnull = NULL;
	acc->n = 0;
	acc->cap = 0;
	acc->nulls = 0;
}

void
weave_docvals_accum_add(WeaveDocvalsAccum *acc, ItemPointer tid,
						Datum value, bool isnull)
{
	if (!acc->active)
		return;

	/*
	 * A NULL is RECORDED, not refused (store v2, doc/specs/DOCVALS_CHANNEL.md
	 * sect. 6).  The pair still occupies a dense docid slot so the docid space
	 * stays contiguous, but its stored value is a placeholder (0) the evaluator
	 * never reads -- the packed null bitmap masks this docid out before any
	 * comparison.  A non-NULL value is encoded to the stored order-preserving
	 * int64 as before.  The null-aware pair append records the null bit and the
	 * count in one place.
	 */
	weave_docvals_accum_add_pair_null(acc, weave_tid_to_docid(tid),
									  isnull ? 0 : weave_dv_encode_datum(acc->dvtype, value),
									  isnull);
}

void
weave_docvals_accum_add_pair(WeaveDocvalsAccum *acc, uint64 docid, int64 value)
{
	/* The historical, non-null entry point: a plain (docid, value) append.
	 * Kept as a thin wrapper so its callers (amscan and any non-null pair
	 * source) need no isnull argument; the null-aware append below does the
	 * work. */
	weave_docvals_accum_add_pair_null(acc, docid, value, false);
}

void
weave_docvals_accum_add_pair_null(WeaveDocvalsAccum *acc, uint64 docid,
								  int64 value, bool isnull)
{
	if (!acc->active)
		return;

	if (acc->n >= acc->cap)
	{
		/* corpus-scale: one pair per indexed document, so the doubling `cap`
		 * goes through the huge-safe allocator (check-alloc).  isnull grows in
		 * lockstep with docid/value so the three arrays stay parallel across
		 * both add entry points. */
		uint32		want = acc->cap ? acc->cap * 2 : 1024;

		acc->docid = acc->docid
			? WEAVE_REALLOC_MAYBE_HUGE(acc->docid, (Size) want * sizeof(uint64))
			: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(uint64));
		acc->value = acc->value
			? WEAVE_REALLOC_MAYBE_HUGE(acc->value, (Size) want * sizeof(int64))
			: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(int64));
		acc->isnull = acc->isnull
			? WEAVE_REALLOC_MAYBE_HUGE(acc->isnull, (Size) want * sizeof(uint8))
			: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(uint8));
		acc->cap = want;
	}

	acc->docid[acc->n] = docid;
	/* value is a placeholder the evaluator never reads when isnull: the packed
	 * null bitmap masks this docid out before any comparison. */
	acc->value[acc->n] = value;
	acc->isnull[acc->n] = isnull ? 1 : 0;
	acc->n++;
	if (isnull)
		acc->nulls++;
}

/* (docid, value) pair for the sort: one array so qsort moves both halves
 * together without an indirection per comparison, the vec_docid_order() shape. */
typedef struct DocvalsPair
{
	uint64		docid;
	int64		value;
	uint8		isnull;			/* travels with the pair through the sort so a
								 * NULL keeps masking its own docid after reorder */
} DocvalsPair;

static int
docvals_cmp_pair(const void *a, const void *b)
{
	uint64		x = ((const DocvalsPair *) a)->docid;
	uint64		y = ((const DocvalsPair *) b)->docid;

	/* No tie is possible: a docid is derived from a ctid and every heap tuple
	 * has its own.  Compared as unsigned; a subtraction would overflow. */
	if (x < y)
		return -1;
	return x > y ? 1 : 0;
}

BlockNumber
weave_docvals_write_weft(Relation index, WeaveDocvalsAccum *acc)
{
	DocvalsPair *pairs;
	uint64	   *docids;
	int64	   *vals;
	uint8	   *nullbits = NULL;
	BlockNumber root;
	uint32		i;

	if (!acc->active || acc->n == 0)
		return InvalidBlockNumber;

	/*
	 * Sort into strictly-ascending docid order -- the store's invariant and the
	 * order the gate emits through -- then split into the two contiguous arrays
	 * the writer takes.  check-alloc: acc->n is corpus-scale, so every size
	 * derived from it uses the huge-safe allocator.
	 */
	pairs = (DocvalsPair *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(DocvalsPair));
	for (i = 0; i < acc->n; i++)
	{
		pairs[i].docid = acc->docid[i];
		pairs[i].value = acc->value[i];
		pairs[i].isnull = acc->isnull[i];
	}
	qsort(pairs, acc->n, sizeof(DocvalsPair), docvals_cmp_pair);

	docids = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(uint64));
	vals = (int64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(int64));

	/*
	 * Build the per-doc nullbits array (in sorted, dense order) only when this
	 * weft actually carries a NULL, so a NOT-NULL segment lays down the exact
	 * v2-without-bitmap shape (null_off == 0).  check-alloc: acc->n is
	 * corpus-scale, so the byte array goes through the huge-safe allocator.
	 */
	if (acc->nulls > 0)
		nullbits = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(uint8));

	for (i = 0; i < acc->n; i++)
	{
		docids[i] = pairs[i].docid;
		vals[i] = pairs[i].value;
		if (nullbits != NULL)
			nullbits[i] = pairs[i].isnull;
	}

	/* GenericXLogState is NULL here: weave_docvals_write() writes each payload
	 * page on its own cycle and only names the parameter to document the
	 * record-the-root-last contract (see its comment); the caller records the
	 * returned root in the channel descriptor LAST.  nullbits is NULL for a
	 * NOT-NULL segment (no bitmap) and non-NULL when acc->nulls > 0. */
	root = weave_docvals_write(index, NULL, vals, docids, nullbits, acc->n);

	if (nullbits != NULL)
		pfree(nullbits);
	pfree(vals);
	pfree(docids);
	pfree(pairs);
	return root;
}
