/*-------------------------------------------------------------------------
 *
 * docvals_page.c
 *		Persist and reload the docvalues store (int8-encoded v1/v2, or the
 *		dictionary-encoded text v3) on a WEAVE_PK_DOCVALS page chain.
 *
 * The store format itself is backend-independent (include/weave/docvals.h):
 * a fixed WeaveDocvalsHeader followed by a dense int64 value array, one entry
 * per segment-local docid.  For an int8-encoded column (v1/v2) a slot is the
 * order-preserving int64 image of the value; for a text column (v3) it is an
 * ordinal into a per-segment dictionary that follows the arrays.  This file is
 * the AM half -- the page chain the image lives on, and the ways to move it
 * across that boundary:
 *
 *	 weave_docvals_write()  serialize an int8 header+values once, lay the bytes
 *							across a fresh chain of WEAVE_PK_DOCVALS pages,
 *							return the root.
 *	 weave_docvals_write_weft()  the accumulator's writer.  For a TEXT
 *							accumulator (docvals_write_weft_text) it builds the
 *							segment's dictionary from the raw bytes the
 *							accumulator collected -- distinct values sorted
 *							under the column collation -- assigns each row its
 *							ordinal, and serializes the v3 image
 *							(docvals_serialize_text).  A merge feeds its inputs'
 *							dictionary entries back in as bytes, so every output
 *							segment gets a fresh dictionary.
 *	 weave_docvals_load()   walk the chain, reassemble one contiguous image, run
 *							the pure validator, and ERROR if it is not sound or
 *							not of the kind the caller expects.
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
 * image whose reassembled length disagrees with the length its header implies
 * (values, docids, bitmap and, for v3, the dictionary), so a torn
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
#include "fmgr.h"				/* PG_DETOAST_DATUM_PACKED (text facet accumulate) */
#include "storage/bufmgr.h"
#include "utils/date.h"			/* DateADT / DatumGetDateADT (date facet encode) */
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"		/* varstr_cmp: the text dictionary's collation order */

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
	h.dict_off = 0;				/* v1/v2 shape: no dictionary (store v3 is text) */
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

/*
 * Serialize a v3 (text) store: the v2 shape with each value slot holding a
 * dictionary ordinal (a placeholder 0 for a NULL doc), then the MAXALIGNed
 * dictionary region -- uint32 ndict, uint32 offs[ndict + 1], the packed entry
 * bytes.  Byte-for-byte what weave_docvals_build_text() (the
 * WEAVE_DOCVALS_TEST_HELPERS reference in weave/docvals.h) produces for the same
 * inputs: the whole image is zeroed first, so every padding byte (header bytes
 * the struct does not name, the post-docids and post-bitmap alignment gaps) is
 * 0 and the image is fully determined by its inputs.
 *
 * Every header offset is a uint32, and dict_off is the last one the header
 * records, so it is the one whose overflow must be refused here (the dictionary
 * offsets are blob-relative and the caller has bounded the blob to UINT32_MAX).
 */
static uint8 *
docvals_serialize_text(const int64 *ords, const uint64 *docids,
					   const uint8 *nullbits, uint32 n,
					   const uint32 *offs, uint32 ndict,
					   const char *blob, Size *len_out)
{
	WeaveDocvalsHeader h;
	Size		voff = WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	Size		doff = voff + (Size) n * 8u;
	Size		dend = doff + (Size) n * 8u;
	Size		noff = WEAVE_DV_MAXALIGN(dend);
	bool		has_nulls = (nullbits != NULL);
	Size		bend = has_nulls ? noff + (Size) ((n + 7u) / 8u) : dend;
	Size		dictoff = WEAVE_DV_MAXALIGN(bend);
	Size		offsbase = dictoff + 4u;
	Size		blobbase = offsbase + ((Size) ndict + 1u) * 4u;
	Size		bloblen = (Size) offs[ndict];
	Size		len = blobbase + bloblen;
	uint8	   *img;
	uint32		i;

	Assert(voff == (Size) MAXALIGN(sizeof(WeaveDocvalsHeader)));
	Assert(noff == (Size) MAXALIGN(dend));
	Assert(dictoff == (Size) MAXALIGN(bend));

	if (dictoff > (Size) PG_UINT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("text docvalues store is too large"),
				 errdetail("The dictionary offset of a segment of %u documents exceeds the 4 GB a store header can address.",
						   n)));

	/*
	 * check-alloc: n, ndict and the blob are all corpus-scale, so the image size
	 * goes through the huge-safe path.  Zero it whole: cheaper to reason about
	 * than zeroing each gap, and the reference builder's output is defined as
	 * "every padding byte 0".
	 */
	img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE(len);
	memset(img, 0, len);

	memset(&h, 0, sizeof(h));
	h.magic = WEAVE_DOCVALS_MAGIC;
	h.version = 3;
	h.typid_kind = WEAVE_DV_KIND_TEXT;
	h.ndocs = n;
	h.null_off = has_nulls ? (uint32) noff : 0;
	h.zonemap_off = 0;
	h.values_off = (uint32) voff;
	h.docids_off = (uint32) doff;
	h.dict_off = (uint32) dictoff;
	memcpy(img, &h, sizeof(h));

	if (n > 0)
	{
		memcpy(img + voff, ords, (Size) n * 8u);
		memcpy(img + doff, docids, (Size) n * 8u);
	}

	/* Same bit shape as docvals_serialize(); the region is already zero. */
	if (has_nulls)
	{
		for (i = 0; i < n; i++)
			if (nullbits[i])
				img[noff + (i >> 3)] |= (uint8) (1u << (i & 7u));
	}

	memcpy(img + dictoff, &ndict, sizeof(uint32));
	memcpy(img + offsbase, offs, ((Size) ndict + 1u) * sizeof(uint32));
	if (bloblen > 0)
		memcpy(img + blobbase, blob, bloblen);

	*len_out = len;
	return img;
}

/*
 * Lay a serialized image across a fresh WEAVE_PK_DOCVALS chain and return its
 * root; frees img.  Shared by the int8 (weave_docvals_write) and text
 * (weave_docvals_write_weft) writers so the two kinds of store cannot differ in
 * how they reach disk -- only in what they serialize.
 */
static BlockNumber
docvals_write_image(Relation index, GenericXLogState *state,
					uint8 *img, Size len)
{
	BlockNumber first = InvalidBlockNumber;
	Buffer		prevbuf = InvalidBuffer;
	Page		prevpage = NULL;
	GenericXLogState *prevstate = NULL;
	Size		off = 0;

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

BlockNumber
weave_docvals_write(Relation index, GenericXLogState *state,
					const int64 *vals, const uint64 *docids,
					const uint8 *nullbits, uint32 n)
{
	Size		len;
	uint8	   *img = docvals_serialize(vals, docids, nullbits, n, &len);

	return docvals_write_image(index, state, img, len);
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

/*
 * The loader without the ereport: returns the validated image, or NULL with
 * *errmsg_out ("page chain" or "store" -- which of the two messages the throwing
 * loader raises) and *detail_out set.  Exists for the SCAN (doc/GAPS.md G68): a
 * scan reads the chain named by a directory snapshot, and a concurrent merge can
 * free and recycle those pages under it, so a scan that meets an unsound chain
 * must first ask whether the directory generation moved -- a race, which it
 * retries -- before calling it corruption.  Everything the throwing loader
 * trusts, this checks identically; only the reporting differs.
 */
const void *
weave_docvals_try_load(Relation index, BlockNumber root, MemoryContext cxt,
					   uint32 *ndocs_out, int want_kind,
					   const char **errmsg_out, char **detail_out)
{
	const char *detail = NULL;
	const char *why;
	int64		len;
	uint8	   *img;
	WeaveDocvalsHeader h;
	MemoryContext old;

	if (ndocs_out != NULL)
		*ndocs_out = 0;
	*errmsg_out = NULL;
	*detail_out = NULL;

	/* Measure pass: page-level soundness only, no allocation yet. */
	len = docvals_walk(index, root, NULL, 0, &detail);
	if (len < 0)
	{
		*errmsg_out = "page chain";
		*detail_out = psprintf("%s (block %u)", detail, root);
		return NULL;
	}

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
		*errmsg_out = "page chain";
		*detail_out = psprintf("%s (block %u)", detail, root);
		return NULL;
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
		*errmsg_out = "store";
		*detail_out = psprintf("%s (block %u)", why, root);
		return NULL;
	}

	memcpy(&h, img, sizeof(h));
	if ((int) h.typid_kind != want_kind)
	{
		pfree(img);
		*errmsg_out = "store";
		*detail_out = psprintf("store value kind %u does not match the column's kind %d (block %u)",
							   (unsigned) h.typid_kind, want_kind, root);
		return NULL;
	}

	if (ndocs_out != NULL)
		*ndocs_out = h.ndocs;
	return (const void *) img;
}

/* Report a failed weave_docvals_try_load() as the corruption it is.  Two full
 * messages rather than one assembled from `what`, so each stays translatable. */
void
weave_docvals_report_corrupt(Relation index, const char *what, const char *detail)
{
	if (strcmp(what, "store") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("corrupt docvalues store in index \"%s\"",
						RelationGetRelationName(index)),
				 errdetail("%s", detail),
				 errhint("REINDEX the index to rebuild it.")));
	ereport(ERROR,
			(errcode(ERRCODE_INDEX_CORRUPTED),
			 errmsg("corrupt docvalues page chain in index \"%s\"",
					RelationGetRelationName(index)),
			 errdetail("%s", detail),
			 errhint("REINDEX the index to rebuild it.")));
}

const void *
weave_docvals_load(Relation index, BlockNumber root,
				   MemoryContext cxt, uint32 *ndocs_out, int want_kind)
{
	const char *what;
	char	   *detail;
	const void *img;

	img = weave_docvals_try_load(index, root, cxt, ndocs_out, want_kind,
								 &what, &detail);
	if (img == NULL)
		weave_docvals_report_corrupt(index, what, detail);
	return img;
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

			/*
			 * text_docval_ops is declared FOR TYPE text, and a varchar column
			 * reaches it by binary coercion (btree's text_ops precedent).  The
			 * index tuple descriptor then carries the HEAP column's type, since
			 * the opclass declares no STORAGE type, so both OIDs name the same
			 * facet: the payload bytes are identical.
			 */
		case TEXTOID:
		case VARCHAROID:
			return WEAVE_DV_T_TEXT;
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
		case WEAVE_DV_T_TEXT:
			/* no int64 image exists; the text path stores dictionary ordinals */
			elog(ERROR, "pg_weave: text docvalues have no int64 encoding");
			break;
	}
	elog(ERROR, "pg_weave: unknown WeaveDvType %d", (int) dvtype);
	return 0;					/* unreachable */
}

int64
weave_dv_encode_const(WeaveDvType coltype, Oid consttype, Datum arg)
{
	/* int64 domain only: a text constant is resolved to dictionary boundaries
	 * per segment by the scan, never encoded. */
	if (coltype == WEAVE_DV_T_TEXT)
		elog(ERROR, "pg_weave: text docvalues constant has no int64 encoding");

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

int
weave_dv_varstr_cmp(void *ctx, const void *a, uint32 alen,
					const void *b, uint32 blen)
{
	/*
	 * Entries and keys are raw text payloads (no varlena header); a dictionary
	 * entry is bounded by the store's validated length and a key by the query
	 * text, both far below INT_MAX, which is varstr_cmp's length type.
	 */
	return varstr_cmp((const char *) a, (int) alen, (const char *) b, (int) blen,
					  *(Oid *) ctx);
}

void
weave_docvals_accum_init(WeaveDocvalsAccum *acc, MemoryContext ctx, bool active,
						 WeaveDvType dvtype, Oid collation)
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

	/*
	 * An ACTIVE text accumulator must know its collation: the dictionary is
	 * sorted under it at write time, and there is no later point at which it
	 * could be recovered.  The caller resolved it (and refused a
	 * non-deterministic one) from the index column.
	 */
	Assert(!(active && dvtype == WEAVE_DV_T_TEXT) || OidIsValid(collation));
	acc->collation = collation;
	acc->tbytes = NULL;
	acc->tlen = 0;
	acc->tcap = 0;
	acc->toff = NULL;
	acc->tlenv = NULL;
}

void
weave_docvals_accum_reset(WeaveDocvalsAccum *acc)
{
	/*
	 * NULL the pointers and zero the counters, exactly like
	 * weave_vec_accum_reset(): the caller frees acc->ctx with
	 * MemoryContextReset() right after, so a pfree here would only pre-empt that,
	 * and a pointer left set would dangle into the freed context.  active, ctx,
	 * dvtype and collation survive so the next segment reuses the same
	 * accumulator; the text arena is cleared with the rest.
	 */
	acc->docid = NULL;
	acc->value = NULL;
	acc->isnull = NULL;
	acc->n = 0;
	acc->cap = 0;
	acc->nulls = 0;
	acc->tbytes = NULL;
	acc->tlen = 0;
	acc->tcap = 0;
	acc->toff = NULL;
	acc->tlenv = NULL;
}

/*
 * Make room for one more row.  corpus-scale: one row per indexed document, so
 * the doubling `cap` goes through the huge-safe allocator (check-alloc).  isnull
 * grows in lockstep with docid/value so the arrays stay parallel across every
 * add entry point; in TEXT mode toff/tlenv join the lockstep.  Allocates in the
 * CURRENT context, as the int8 path always has (its callers run in acc->ctx);
 * the text entry point switches to acc->ctx itself.
 */
static void
docvals_accum_grow(WeaveDocvalsAccum *acc)
{
	uint32		want;

	if (acc->n < acc->cap)
		return;

	if (acc->cap > PG_UINT32_MAX / 2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many documents for one docvalues segment")));
	want = acc->cap ? acc->cap * 2 : 1024;

	acc->docid = acc->docid
		? WEAVE_REALLOC_MAYBE_HUGE(acc->docid, (Size) want * sizeof(uint64))
		: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(uint64));
	acc->value = acc->value
		? WEAVE_REALLOC_MAYBE_HUGE(acc->value, (Size) want * sizeof(int64))
		: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(int64));
	acc->isnull = acc->isnull
		? WEAVE_REALLOC_MAYBE_HUGE(acc->isnull, (Size) want * sizeof(uint8))
		: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(uint8));
	if (acc->dvtype == WEAVE_DV_T_TEXT)
	{
		acc->toff = acc->toff
			? WEAVE_REALLOC_MAYBE_HUGE(acc->toff, (Size) want * sizeof(uint64))
			: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(uint64));
		acc->tlenv = acc->tlenv
			? WEAVE_REALLOC_MAYBE_HUGE(acc->tlenv, (Size) want * sizeof(uint32))
			: WEAVE_ALLOC_MAYBE_HUGE((Size) want * sizeof(uint32));
	}
	acc->cap = want;
}

void
weave_docvals_accum_add(WeaveDocvalsAccum *acc, ItemPointer tid,
						Datum value, bool isnull)
{
	if (!acc->active)
		return;

	/*
	 * TEXT: record the raw payload bytes; the ordinal is assigned when the
	 * whole segment's dictionary is built (weave_docvals_write_weft).  The
	 * datum may be toasted (compressed or short-header), so it is detoasted in
	 * packed form -- a 1-byte-header value is read in place -- and a copy made
	 * by the detoast is freed at once: add_text has copied the bytes into the
	 * arena, and a corpus-scale build must not keep one copy per row.
	 */
	if (acc->dvtype == WEAVE_DV_T_TEXT)
	{
		uint64		docid = weave_tid_to_docid(tid);
		struct varlena *raw;
		struct varlena *v;

		if (isnull)
		{
			weave_docvals_accum_add_text(acc, docid, NULL, 0, true);
			return;
		}
		raw = (struct varlena *) DatumGetPointer(value);
		v = PG_DETOAST_DATUM_PACKED(value);
		weave_docvals_accum_add_text(acc, docid, VARDATA_ANY(v),
									 (uint32) VARSIZE_ANY_EXHDR(v), false);
		if (v != raw)
			pfree(v);
		return;
	}

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
weave_docvals_accum_add_text(WeaveDocvalsAccum *acc, uint64 docid,
							 const char *p, uint32 len, bool isnull)
{
	MemoryContext old;

	if (!acc->active)
		return;
	if (acc->dvtype != WEAVE_DV_T_TEXT)
		elog(ERROR, "pg_weave: text docvalue appended to a non-text docvalues accumulator");

	if (isnull)
		len = 0;				/* a NULL row owns no arena bytes */

	old = MemoryContextSwitchTo(acc->ctx);
	docvals_accum_grow(acc);

	if (len > 0 && acc->tlen + (Size) len > acc->tcap)
	{
		/*
		 * check-alloc: the arena holds every row's bytes, corpus-scale, so the
		 * doubling tcap is huge-safe.  Grow to at least what this row needs so
		 * a single value larger than the doubled capacity still fits.
		 */
		Size		want = acc->tcap ? acc->tcap * 2 : (Size) 8192;

		if (want < acc->tlen + (Size) len)
			want = acc->tlen + (Size) len;
		acc->tbytes = acc->tbytes
			? WEAVE_REALLOC_MAYBE_HUGE(acc->tbytes, want)
			: WEAVE_ALLOC_MAYBE_HUGE(want);
		acc->tcap = want;
	}
	MemoryContextSwitchTo(old);

	acc->docid[acc->n] = docid;
	acc->value[acc->n] = 0;		/* ordinal placeholder; set at write time */
	acc->isnull[acc->n] = isnull ? 1 : 0;
	acc->toff[acc->n] = (uint64) acc->tlen;
	acc->tlenv[acc->n] = len;
	if (len > 0)
	{
		memcpy(acc->tbytes + acc->tlen, p, len);
		acc->tlen += (Size) len;
	}
	acc->n++;
	if (isnull)
		acc->nulls++;
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

	/*
	 * An encoded int64 means nothing to a text accumulator (its values are
	 * per-segment ordinals, assigned only when the dictionary is built), and a
	 * row appended here would carry no bytes for the writer to place.  The
	 * flush/merge callers route a text column through add_text instead.
	 */
	if (acc->dvtype == WEAVE_DV_T_TEXT)
		elog(ERROR, "pg_weave: int64 docvalue appended to a text docvalues accumulator");

	docvals_accum_grow(acc);

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

/*
 * TEXT dictionary construction.  The context carries what the comparators need
 * to turn a dense (docid-sorted) position into its row's bytes: the sorted pairs
 * (whose `value` holds the ORIGINAL accumulator row index in text mode -- see
 * docvals_write_weft_text) and the accumulator's arena.  `rep` maps a distinct
 * value's id to a dense position holding its bytes, for the collation sort.
 */
typedef struct DocvalsTextCtx
{
	const DocvalsPair *pairs;
	const WeaveDocvalsAccum *acc;
	const uint32 *rep;
	Oid			collation;
} DocvalsTextCtx;

/* The payload bytes of dense position k.  A zero-length span never forms a
 * pointer into a possibly-NULL arena (an all-'' segment allocates none). */
static inline const char *
docvals_text_span(const DocvalsTextCtx *c, uint32 k, uint32 *len)
{
	uint32		row = (uint32) c->pairs[k].value;

	*len = c->acc->tlenv[row];
	if (*len == 0)
		return "";
	return c->acc->tbytes + c->acc->toff[row];
}

/* Bytewise order (memcmp, then length) of two byte strings: total, and equal
 * exactly when the strings are byte-identical. */
static inline int
docvals_bytes_cmp(const char *ap, uint32 alen, const char *bp, uint32 blen)
{
	int			r = memcmp(ap, bp, Min(alen, blen));

	if (r != 0)
		return r;
	return (alen < blen) ? -1 : (alen > blen ? 1 : 0);
}

/* Order two DENSE POSITIONS bytewise -- the cheap first sort, which only has to
 * make byte-identical values adjacent so each run collapses to one value. */
static int
docvals_text_bytecmp(const void *a, const void *b, void *arg)
{
	const DocvalsTextCtx *c = (const DocvalsTextCtx *) arg;
	uint32		alen;
	uint32		blen;
	const char *ap = docvals_text_span(c, *(const uint32 *) a, &alen);
	const char *bp = docvals_text_span(c, *(const uint32 *) b, &blen);

	return docvals_bytes_cmp(ap, alen, bp, blen);
}

/*
 * Order two DISTINCT-VALUE ids by the column collation, with a BYTEWISE
 * tie-break so the order is total and deterministic even if the collation ever
 * called two different byte strings equal (the check after the sort refuses
 * that case; the tie-break only keeps the sort well defined).  varstr_cmp
 * already breaks ties bytewise under a deterministic collation, so the
 * tie-break never decides anything there.
 */
static int
docvals_text_collcmp(const void *a, const void *b, void *arg)
{
	const DocvalsTextCtx *c = (const DocvalsTextCtx *) arg;
	uint32		alen;
	uint32		blen;
	const char *ap = docvals_text_span(c, c->rep[*(const uint32 *) a], &alen);
	const char *bp = docvals_text_span(c, c->rep[*(const uint32 *) b], &blen);
	int			r;

	r = varstr_cmp(ap, (int) alen, bp, (int) blen, c->collation);
	if (r != 0)
		return r;
	return docvals_bytes_cmp(ap, alen, bp, blen);
}

/*
 * Write a v3 (text) store from a TEXT accumulator.
 *
 * 1. Sort rows into strictly-ascending docid order -- the same DocvalsPair sort
 *    the int8 path uses, with `value` carrying the original row index so each
 *    row's byte span travels with its docid and null bit.
 * 2. Sort the non-NULL dense positions BYTEWISE and collapse each run of
 *    byte-identical values to one distinct value, remembering per row which
 *    distinct value it is.  Cheap (memcmp, no collation call) and it shrinks
 *    the collation sort from one entry per row to one per distinct value --
 *    the whole point for a low-cardinality facet.
 * 3. Sort only the distinct values under the collation; a value's position in
 *    that order is its ordinal, and each row's ordinal is looked up through its
 *    distinct id.  The dictionary is therefore STRICTLY ascending under the
 *    collation, which is what the scan's boundary search
 *    (weave_dv_dict_lower_bound/upper_bound) requires: a duplicate entry would
 *    make hi - lo == 2 for an `=` constant, and two entries out of order would
 *    make the binary search skip rows.
 * 4. Serialize (docvals_serialize_text) and lay the image across a chain.
 *
 * A NULL row keeps a placeholder ordinal 0 (masked by the bitmap); '' is a real
 * entry, told from NULL only by isnull -- never by its length.
 */
static BlockNumber
docvals_write_weft_text(Relation index, WeaveDocvalsAccum *acc)
{
	DocvalsPair *pairs;
	DocvalsTextCtx tc;
	uint64	   *docids;
	int64	   *ords;
	uint8	   *nullbits = NULL;
	uint32	   *idx;
	uint32	   *rep;			/* distinct id -> a dense position of its bytes */
	uint32	   *dord;			/* ordinal -> distinct id (collation order) */
	uint32	   *rank;			/* distinct id -> ordinal */
	uint32	   *offs;
	char	   *blob;
	uint32		nn = 0;
	uint32		ndict = 0;
	uint64		bloblen = 0;
	uint8	   *img;
	Size		len;
	uint32		i;

	Assert(OidIsValid(acc->collation));

	/*
	 * check-alloc: acc->n is corpus-scale, so every array sized from it (and
	 * from nn/ndict, which it bounds) uses the huge-safe allocator.
	 */
	pairs = (DocvalsPair *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(DocvalsPair));
	for (i = 0; i < acc->n; i++)
	{
		pairs[i].docid = acc->docid[i];
		pairs[i].value = (int64) i; /* original row: locates its byte span */
		pairs[i].isnull = acc->isnull[i];
	}
	qsort(pairs, acc->n, sizeof(DocvalsPair), docvals_cmp_pair);

	docids = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(uint64));
	ords = (int64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(int64));
	idx = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(acc->n, 1) * sizeof(uint32));
	if (acc->nulls > 0)
		nullbits = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE((Size) acc->n * sizeof(uint8));

	for (i = 0; i < acc->n; i++)
	{
		if ((i & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
		docids[i] = pairs[i].docid;
		ords[i] = 0;			/* NULL placeholder; non-NULL set below */
		if (nullbits != NULL)
			nullbits[i] = pairs[i].isnull;
		if (!pairs[i].isnull)
			idx[nn++] = i;
	}

	tc.pairs = pairs;
	tc.acc = acc;
	tc.rep = NULL;
	tc.collation = acc->collation;

	/*
	 * Step 2: bytewise sort, collapse.  ords[k] temporarily holds row k's
	 * DISTINCT id; step 3 rewrites it to the ordinal.
	 */
	qsort_arg(idx, nn, sizeof(uint32), docvals_text_bytecmp, &tc);
	rep = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(nn, 1) * sizeof(uint32));
	for (i = 0; i < nn; i++)
	{
		uint32		k = idx[i];
		uint32		l;
		const char *p = docvals_text_span(&tc, k, &l);

		CHECK_FOR_INTERRUPTS();
		if (ndict > 0)
		{
			uint32		pl;
			const char *pp = docvals_text_span(&tc, rep[ndict - 1], &pl);

			if (pl == l && (l == 0 || memcmp(pp, p, l) == 0))
			{
				ords[k] = (int64) (ndict - 1);
				continue;
			}
		}
		bloblen += (uint64) l;
		if (bloblen > (uint64) PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("text docvalues dictionary is too large"),
					 errdetail("The distinct values of one segment of index \"%s\" exceed 4 GB.",
							   RelationGetRelationName(index))));
		rep[ndict] = k;
		ords[k] = (int64) ndict;
		ndict++;
	}

	/*
	 * Step 3: sort the distinct values under the collation.  Adjacent values
	 * the collation calls equal cannot share an ordinal (the scan answers `=`
	 * by ordinal, so it would drop one of them) nor take two (the dictionary
	 * would not be strictly ascending).  They are byte-distinct by
	 * construction, and varstr_cmp breaks ties bytewise under a deterministic
	 * collation -- the only kind CREATE INDEX accepts -- so this cannot happen;
	 * it is checked because writing it would be a silent wrong answer.
	 */
	dord = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(ndict, 1) * sizeof(uint32));
	rank = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(ndict, 1) * sizeof(uint32));
	for (i = 0; i < ndict; i++)
		dord[i] = i;
	tc.rep = rep;
	qsort_arg(dord, ndict, sizeof(uint32), docvals_text_collcmp, &tc);
	for (i = 0; i < ndict; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (i > 0)
		{
			uint32		al;
			uint32		bl;
			const char *ap = docvals_text_span(&tc, rep[dord[i - 1]], &al);
			const char *bp = docvals_text_span(&tc, rep[dord[i]], &bl);

			if (varstr_cmp(ap, (int) al, bp, (int) bl, acc->collation) == 0)
				elog(ERROR, "pg_weave: byte-distinct text docvalues of index \"%s\" compare equal under collation %u",
					 RelationGetRelationName(index), acc->collation);
		}
		rank[dord[i]] = i;
	}
	for (i = 0; i < nn; i++)
	{
		uint32		k = idx[i];

		if ((i & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
		ords[k] = (int64) rank[(uint32) ords[k]];
	}

	/* The packed dictionary: offs[ndict + 1] then the entry bytes, in ordinal
	 * order. */
	offs = (uint32 *) WEAVE_ALLOC_MAYBE_HUGE(((Size) ndict + 1) * sizeof(uint32));
	blob = (char *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(bloblen, 1));
	offs[0] = 0;
	for (i = 0; i < ndict; i++)
	{
		uint32		l;
		const char *p = docvals_text_span(&tc, rep[dord[i]], &l);

		if (l > 0)
			memcpy(blob + offs[i], p, l);
		offs[i + 1] = offs[i] + l;	/* <= bloblen <= UINT32_MAX: no wrap */
	}

	img = docvals_serialize_text(ords, docids, nullbits, acc->n,
								 offs, ndict, blob, &len);

	/*
	 * The writer's output must pass the reader's trust boundary; checked here
	 * (cassert builds) so a writer bug fails at CREATE INDEX, not at the first
	 * scan that loads the store.
	 */
	Assert(weave_docvals_validate(img, (size_t) len) == NULL);

	pfree(blob);
	pfree(offs);
	pfree(rank);
	pfree(dord);
	pfree(rep);
	if (nullbits != NULL)
		pfree(nullbits);
	pfree(idx);
	pfree(ords);
	pfree(docids);
	pfree(pairs);

	/* NULL state: the record-the-root-last contract, as in the int8 path. */
	return docvals_write_image(index, NULL, img, len);
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

	if (acc->dvtype == WEAVE_DV_T_TEXT)
		return docvals_write_weft_text(index, acc);

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
