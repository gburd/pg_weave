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
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Image bytes one WEAVE_PK_DOCVALS page carries.  Identical shape to
 * WEAVE_SURFPAGE_PAYLOAD: a whole page minus the header and the opaque area.
 */
#define WEAVE_DOCVALS_PAYLOAD \
	(BLCKSZ - (int) MAXALIGN(SizeOfPageHeaderData) - (int) MAXALIGN(sizeof(WeavePageOpaqueData)))

/*
 * Serialize the int8 store (header + n int64 values) into one contiguous buffer.
 * Returns a palloc'd image and sets *len_out to its byte length.
 *
 * The layout is byte-for-byte what weave_docvals_validate() expects and what the
 * WEAVE_DOCVALS_TEST_HELPERS build (weave_docvals_build) produces, so a store
 * written here reads back through the same validator the standalone property
 * test drives.
 */
static uint8 *
docvals_serialize(const int64 *vals, uint32 n, Size *len_out)
{
	WeaveDocvalsHeader h;
	uint32		voff = (uint32) WEAVE_DV_MAXALIGN(sizeof(WeaveDocvalsHeader));
	Size		len;
	uint8	   *img;

	/*
	 * The header's local WEAVE_DV_MAXALIGN must agree with the backend's
	 * MAXALIGN, or the offset the store records and the offset a reader recomputes
	 * would differ.  docvals.h anticipates this assertion in the comment on
	 * WEAVE_DV_MAXALIGN; make it rather than assume it.
	 */
	Assert(voff == (uint32) MAXALIGN(sizeof(WeaveDocvalsHeader)));

	/*
	 * check-alloc: n is corpus-scale (one value per docid in the segment), so
	 * the image size derives from a corpus-scale quantity and goes through the
	 * huge-safe path -- the exact allocation class behind four real crashes in
	 * this extension's ancestor (AGENTS.md's lint table).  values_off + n*8 is
	 * computed in Size; on a 64-bit target n (uint32) * 8 cannot wrap.
	 */
	len = (Size) voff + (Size) n * 8;
	img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE(len);

	h.magic = WEAVE_DOCVALS_MAGIC;
	h.version = 1;
	h.typid_kind = 1;			/* int8 */
	h.ndocs = n;
	h.null_off = 0;
	h.zonemap_off = 0;
	h.values_off = voff;
	h.reserved = 0;
	memcpy(img, &h, sizeof(h));

	if (n > 0)
		memcpy(img + voff, vals, (Size) n * 8);

	*len_out = len;
	return img;
}

BlockNumber
weave_docvals_write(Relation index, GenericXLogState *state,
					const int64 *vals, uint32 n)
{
	BlockNumber first = InvalidBlockNumber;
	Buffer		prevbuf = InvalidBuffer;
	Page		prevpage = NULL;
	GenericXLogState *prevstate = NULL;
	Size		len;
	Size		off = 0;
	uint8	   *img = docvals_serialize(vals, n, &len);

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
