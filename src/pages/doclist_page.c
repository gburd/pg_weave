/*-------------------------------------------------------------------------
 *
 * doclist_page.c
 *		The v12 per-bolt DOCUMENT LIST: its page chain, its loader, and the
 *		"docset" every reader that must enumerate a bolt's documents uses.
 *
 * doc/specs/SEGMENT_FORMAT.md sect. 6 "The document list" is the spec, and
 * include/weave/doclist.h the image.  This file is the AM half:
 *
 *	 weave_doclist_write()	lay an image on a fresh WEAVE_PK_DOCLIST chain
 *	 weave_doclist_root()	the bolt's WEAVE_WK_DOCLIST root, or Invalid
 *	 weave_doclist_load()	read + validate a bolt's list (NULL when absent)
 *	 weave_segment_docset()	the bolt's documents: the list when present, else
 *							the legacy union postings U docvalues U warp
 *
 * WHY NOT weave_write_blob().  It hard-codes WEAVE_PK_TRGM_DATA and its reader
 * validates no page kind; the surf and docvalues chains say why that is not
 * acceptable for a structure a reader trusts (include/weave/am.h).  This is
 * their chain shape: the chain carries the image and nothing else, so the image
 * length is the sum of the page payloads, measured before allocating.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "weave/weave.h"
#include "weave/am.h"
#include "weave/doclist.h"
#include "weave/vector.h"

#include "access/generic_xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"

#define WEAVE_DOCLIST_PAYLOAD \
	(BLCKSZ - (int) MAXALIGN(SizeOfPageHeaderData) - (int) MAXALIGN(sizeof(WeavePageOpaqueData)))

/*
 * Lay `len` bytes across a fresh chain of WEAVE_PK_DOCLIST pages, one page per
 * GenericXLog cycle (no page-count limit, no oversized record), FULL_IMAGE
 * because every page is new.  Returns the first block.  The caller records the
 * root LAST, on the descriptor page, so a recorded root always exists.
 */
static BlockNumber
doclist_write_image(Relation index, const uint8 *img, Size len)
{
	BlockNumber first = InvalidBlockNumber;
	Buffer		prevbuf = InvalidBuffer;
	Page		prevpage = NULL;
	GenericXLogState *prevstate = NULL;
	Size		off = 0;

	Assert(len >= WEAVE_DOCLIST_HDRSIZE);
	do
	{
		Buffer		buf = weave_new_buffer(index);
		BlockNumber blk = BufferGetBlockNumber(buf);
		GenericXLogState *state = GenericXLogStart(index);
		Page		page = GenericXLogRegisterBuffer(state, buf,
													 GENERIC_XLOG_FULL_IMAGE);
		Size		chunk = Min(len - off, (Size) WEAVE_DOCLIST_PAYLOAD);

		weave_init_page(page, WEAVE_PK_DOCLIST);
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
		prevstate = state;
		off += chunk;
	} while (off < len);

	GenericXLogFinish(prevstate);
	UnlockReleaseBuffer(prevbuf);
	return first;
}

BlockNumber
weave_doclist_write(Relation index, const uint64 *all, Size n,
					const uint64 *nul, Size nnull, bool complete)
{
	Size		len = 0;
	uint8	   *img;
	BlockNumber root;

	if (n == 0)
		return InvalidBlockNumber;	/* a bolt with no documents has no list */
	img = weave_doclist_encode(all, n, nul, nnull,
							   complete ? WEAVE_DOCLIST_F_COMPLETE : 0, &len);
	if (img == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory encoding a weave document list")));
	PG_TRY();
	{
		/* the validator every reader runs, on the writer's side too: a writer
		 * that produces an image its own reader refuses is a bolt nothing can
		 * read, free or vacuum */
		const char *why = weave_doclist_check(img, len);

		if (why != NULL)
			elog(ERROR, "pg_weave: document list writer produced an invalid image: %s",
				 why);
		root = doclist_write_image(index, img, len);
	}
	PG_FINALLY();
	{
		free(img);				/* malloc'd by weave_doclist_encode */
	}
	PG_END_TRY();
	return root;
}

/*
 * One pass over the chain: with dst NULL it measures, else it copies at most
 * cap bytes.  Returns the payload length or -1 with *detail set.  One walker,
 * two modes, so the measure and the copy cannot disagree (weave_surf_walk()).
 */
static int64
doclist_walk(Relation index, BlockNumber root, uint8 *dst, Size cap,
			 const char **detail)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);
	BlockNumber blk = root;
	int64		total = 0;
	int64		npages = 0;

	*detail = NULL;
	if (blk == InvalidBlockNumber || blk == WEAVE_METAPAGE_BLKNO)
	{
		*detail = "document list root block is invalid";
		return -1;
	}
	while (blk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		Size		avail;

		CHECK_FOR_INTERRUPTS();
		if (blk >= nblocks)
		{
			*detail = "document list chain leaves the relation";
			return -1;
		}
		if (++npages > (int64) nblocks)
		{
			*detail = "document list chain exceeds the relation length (cycle?)";
			return -1;
		}
		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || WeavePageIsFreed(page) ||
			!WeavePageHasKind(page, WEAVE_PK_DOCLIST))
		{
			UnlockReleaseBuffer(buf);
			*detail = "a block on the document list chain is not a document list page";
			return -1;
		}
		avail = (Size) (weave_page_entry_end(page) - (char *) PageGetContents(page));
		if (avail > (Size) WEAVE_DOCLIST_PAYLOAD)
			avail = (Size) WEAVE_DOCLIST_PAYLOAD;
		if (dst != NULL)
		{
			if ((Size) total + avail > cap)
			{
				UnlockReleaseBuffer(buf);
				*detail = "document list chain grew between the measure and copy passes";
				return -1;
			}
			memcpy(dst + total, PageGetContents(page), avail);
		}
		total += (int64) avail;
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buf);
	}
	return total;
}

BlockNumber
weave_doclist_root(Relation index, const WeaveSegMeta *seg)
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
		if (weft[i].kind == (uint16) WEAVE_WK_DOCLIST)
			return weft[i].root;
	return InvalidBlockNumber;
}

/*
 * Read and validate the list rooted at `root` into ascending palloc'd arrays.
 * Returns false with *detail set on any page-level or image problem; never
 * throws for those, so weave_check() can report and a scan can retry on a
 * moved directory generation before calling it corruption.
 */
bool
weave_doclist_read(Relation index, BlockNumber root, WeaveDocset *ds,
				   const char **detail)
{
	int64		len;
	uint8	   *img;
	WeaveDocListHeader h;
	const char *why;

	memset(ds, 0, sizeof(*ds));
	len = doclist_walk(index, root, NULL, 0, detail);
	if (len < 0)
		return false;
	if (len < (int64) WEAVE_DOCLIST_HDRSIZE)
	{
		*detail = "document list chain is shorter than the image header";
		return false;
	}
	img = (uint8 *) WEAVE_ALLOC_MAYBE_HUGE((Size) len);
	if (doclist_walk(index, root, img, (Size) len, detail) != len)
	{
		pfree(img);
		if (*detail == NULL)
			*detail = "document list chain length changed between passes";
		return false;
	}
	why = weave_doclist_check(img, (size_t) len);
	if (why != NULL)
	{
		pfree(img);
		*detail = why;
		return false;
	}
	memcpy(&h, img, sizeof(h));
	/* bounded by the validated image (a sparsemap member costs at least one
	 * bit of a chunk the image carries), not by an untrusted count */
	ds->ids = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) h.ndocs * sizeof(uint64));
	ds->nullids = (uint64 *) WEAVE_ALLOC_MAYBE_HUGE((Size) Max(h.nnull, 1) * sizeof(uint64));
	if (!weave_doclist_decode(img, (size_t) len, ds->ids, ds->nullids))
	{
		pfree(img);
		weave_docset_free(ds);
		*detail = "document list does not decode";
		return false;
	}
	ds->n = (Size) h.ndocs;
	ds->nnull = (Size) h.nnull;
	ds->has_list = true;
	ds->complete = (h.flags & WEAVE_DOCLIST_F_COMPLETE) != 0;
	pfree(img);
	return true;
}

void
weave_docset_free(WeaveDocset *ds)
{
	if (ds->ids)
		pfree(ds->ids);
	if (ds->nullids)
		pfree(ds->nullids);
	memset(ds, 0, sizeof(*ds));
}

/* grow-append for the legacy union; corpus-scale, huge-safe */
static void
docset_push(uint64 **v, Size *n, Size *cap, uint64 x)
{
	if (*n >= *cap)
	{
		*cap = *cap ? *cap * 2 : 1024;
		*v = *v ? (uint64 *) WEAVE_REALLOC_MAYBE_HUGE(*v, *cap * sizeof(uint64))
			: (uint64 *) WEAVE_ALLOC_MAYBE_HUGE(*cap * sizeof(uint64));
	}
	(*v)[(*n)++] = x;
}

/*
 * The documents a bolt holds.  ONE function for every reader that needs the
 * enumeration -- bulkdelete, the merge, the NOT universe -- so "which documents
 * are in this bolt" has one answer (doc/GAPS.md G78/G80 were two readers each
 * answering it from postings).
 *
 * A v12 bolt: its document list, ERROR on a list that does not validate (a bolt
 * that names a list it cannot read is corrupt; the caller decides whether a
 * moved generation explains it, see weave_doclist_read()).
 *
 * A pre-v12 bolt: postings U docvalues docids U vector warp docids.  The union
 * is load-bearing: with postings alone, a merge of an old bolt would carry a
 * zero-term document's docvalue into an output whose list does not contain it,
 * and G80 would become permanent in the merged bolt.  nullids is empty --
 * pre-v12 builds never indexed a NULL document.
 */
void
weave_segment_docset(Relation index, const WeaveSegMeta *seg, WeaveDocset *ds)
{
	BlockNumber root = weave_doclist_root(index, seg);
	uint64	   *v = NULL;
	Size		n = 0,
				cap = 0;

	if (root != InvalidBlockNumber)
	{
		const char *detail = NULL;

		if (!weave_doclist_read(index, root, ds, &detail))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("weave index \"%s\" has a corrupt document list",
							RelationGetRelationName(index)),
					 errdetail("%s", detail != NULL ? detail : "unknown"),
					 errhint("REINDEX the index.")));
		return;
	}

	memset(ds, 0, sizeof(*ds));

	/* postings */
	{
		uint64	   *p = NULL;
		Size		np = 0;
		Size		i;

		weave_segment_posting_docids(index, seg, &p, &np);
		for (i = 0; i < np; i++)
			docset_push(&v, &n, &cap, p[i]);
		if (p)
			pfree(p);
	}

	/* docvalues docids (every row the store recorded, NULL values included) */
	{
		AttrNumber	attno = 0;
		BlockNumber dvroot = weave_docvals_root_for_segment(index, seg, &attno);

		if (dvroot != InvalidBlockNumber)
		{
			uint32		ndv = 0;
			uint32		i;
			bool		text = (weave_dv_type_for_oid(
										TupleDescAttr(RelationGetDescr(index),
													  attno - 1)->atttypid)
								== WEAVE_DV_T_TEXT);
			const void *img = weave_docvals_load(index, dvroot, CurrentMemoryContext,
												 &ndv,
												 text ? WEAVE_DV_KIND_TEXT
												 : WEAVE_DV_KIND_INT8);

			for (i = 0; i < ndv; i++)
				docset_push(&v, &n, &cap, weave_docvals_docid(img, i));
			pfree((void *) img);
		}
	}

	/* vector warp docids (every lane, dead ones included: a NULL vector still
	 * occupies its document's warp position) */
	{
		BlockNumber vroot = weave_vec_weft_root(index, seg);

		if (vroot != InvalidBlockNumber)
		{
			WeaveVecWeft w;
			const char *why = NULL;

			if (weave_vec_weft_open(index, vroot, &w, &why))
			{
				WeaveVecWarpCursor c;
				uint64		d;

				weave_vec_warp_begin(&c, &w);
				while (weave_vec_warp_next(&c, &d, &why))
					docset_push(&v, &n, &cap, d);
				weave_vec_warp_end(&c);
			}
		}
	}

	ds->ids = v;
	ds->n = (Size) weave_doclist_sort_uniq(v, n);
	ds->nullids = NULL;
	ds->nnull = 0;
	ds->has_list = false;
	ds->complete = false;
}

/* Binary search in an ascending docid array. */
bool
weave_docids_contains(const uint64 *v, Size n, uint64 x)
{
	Size		lo = 0,
				hi = n;

	while (lo < hi)
	{
		Size		m = lo + (hi - lo) / 2;

		if (v[m] < x)
			lo = m + 1;
		else
			hi = m;
	}
	return lo < n && v[lo] == x;
}
