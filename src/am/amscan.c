/*-------------------------------------------------------------------------
 *
 * pg_weave_am_scan.c
 *		Bitmap scan for the weave access method.
 *
 * Included directly into pg_weave_am.c (it shares static page helpers).  It
 * evaluates an wquery by set algebra over posting lists (a term yields the
 * TIDs whose document contains it; AND intersects, OR unions, NOT complements
 * against the indexed universe) for the bitmap and index-only scans, and runs
 * block-max WAND / MaxScore top-k for the <=> ordering scan.  Fuzzy/regex use
 * a Levenshtein automaton / trigram funnel; counts use a visibility-map-aware
 * bulk path.  Results are exact against @@@ semantics; the boolean and ranked
 * paths need no heap access beyond MVCC visibility.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_weave_am_scan.c
 *
 *-------------------------------------------------------------------------
 */

/* A materialized, sorted, duplicate-free set of TIDs. */
typedef struct TidSet
{
	ItemPointerData *tids;
	int			n;
} TidSet;

/* forward decl: trigram-index candidate lookup (pg_weave_trgm_index.c) */
static bool weave_trgm_candidates(Relation index, BlockNumber trgmstart,
								 BlockNumber dictstart,
								 const char *term, int termlen,
								 int min_trigrams, bool is_regex, bool has_doclen_col,
								 TidSet *out);
static void weave_collect_matches(Relation index, WeaveQuery query, TidSet *out, bool *recheck);
static void weave_recheck_exact(Relation index, WeaveQuery query, TidSet *set);
static double weave_query_maxhits(Relation index, WeaveQuery q, double N);
/* forward decl: blob reader (pg_weave_trgm_index.c, included after this file) */
static uint8 *weave_read_blob(Relation index, BlockNumber blk, Size len);

/*
 * EOF-tolerant page read for the scan's directory-following chain walks.
 *
 * A scan reads the segment directory (metapage + per-segment chain heads) under
 * a transient SHARE lock, releases it, then follows dict/posting chains by
 * block number, re-checking the metapage `generation` afterward and retrying if
 * it moved (the A1 race).  But a concurrent weave_vacuum can TRUNCATE the index
 * tail; a block number from the pre-truncation snapshot then points past EOF and
 * ReadBuffer raises a hard "could not read blocks N: read only 0 of 8192" ERROR
 * before the generation re-check can discard the stale result.  So a chain walk
 * must treat an out-of-range block as END OF CHAIN: return InvalidBuffer, let
 * the walk stop, and let the caller's generation guard restart from a fresh
 * (post-truncation) directory.  RelationGetNumberOfBlocks reads the smgr size
 * cache (refreshed on the truncation's relcache invalidation), so this is cheap.
 */
static inline Buffer
weave_scan_readbuf(Relation index, BlockNumber blk)
{
	if (blk == InvalidBlockNumber || blk >= RelationGetNumberOfBlocks(index))
		return InvalidBuffer;
	return ReadBuffer(index, blk);
}

/* Max terms in a phrase chain we evaluate positionally, and the per-docid
 * position scratch bound.  A phrase with more terms, or a per-(term,doc) tf
 * beyond WEAVE_PHRASE_POSBUF, falls back to the (correct) recheck path.  16383
 * matches the analyzer's MAXENTRYPOS cap, so a well-formed posting never
 * exceeds it. */
#define WEAVE_QUERY_MAX_PHRASE_TERMS 32
#define WEAVE_PHRASE_POSBUF 16384

/*
 * Does a dictionary entry starting at `de` fit within a page ending at `end`?
 *
 * Dictionary pages are read under only BUFFER_LOCK_SHARE.  A concurrent
 * merge/vacuum can free a segment's pages while a concurrent insert recycles
 * and overwrites them (pg_weave recycles freed pages with no deletion-xid gate),
 * so a scan that snapshotted the segment directory before that change can read
 * a recycled page mid-walk.  If de->termlen is then garbage, the entry stride
 * and the term-compare run past the page (out-of-bounds read -> SIGSEGV) and a
 * decoded df can be a multi-gigabyte "invalid memory alloc request size".  Every
 * reader dict walk must confirm the entry header AND its term bytes fit before
 * trusting de->termlen; on a miss it stops the page walk (a bounded wrong /
 * incomplete result), and the scan's generation re-check then detects the stale
 * read and restarts.  Same contract as the block-decode guards.
 */
static inline bool
weave_dict_entry_fits(const WeaveDictEntry *de, const char *end)
{
	const char *t = (const char *) de + offsetof(WeaveDictEntry, term);

	return t <= end && t + de->termlen <= end;
}

/* A scored heap tuple (score, or distance in an ordering scan). */
typedef struct ScoredTid
{
	ItemPointerData tid;
	double		score;
}			ScoredTid;

static int weave_topk_visible(Relation index, WeaveQuery q, int k,
							 bool as_distance, ScoredTid **out);
static int weave_topk_candidates_range(Relation index, WeaveQuery q, int wantk,
									  uint64 docid_lo, uint64 docid_hi,
									  ScoredTid **out);

typedef struct WeaveScanOpaqueData
{
	WeaveQuery	query;			/* copied into the scan's context */
	bool		queryValid;
	/* ordering-scan (amgettuple) state, materialized on first call.
	 *
	 * Two arrays, and the split between them is what makes a widening
	 * incremental (task L14):
	 *
	 *  - cand[]    the ranked candidates of the CURRENT WAND pass, descending
	 *              score, not yet visibility-checked.  candpos is how far the
	 *              MVCC probe has walked.
	 *  - ordered[] the visible rows already materialized, ascending distance.
	 *              ACCUMULATED ACROSS PASSES: a widening keeps this array,
	 *              re-probes none of it, and filters the new pass's candidates
	 *              against it, so a recompute extends the previous pass instead
	 *              of repeating its output. */
	bool		orderInit;		/* has the first pass run? */
	ScoredTid  *ordered;		/* visible results so far, ascending distance */
	int			nordered;		/* how many are materialized */
	int			maxordered;		/* allocated slots in ordered[] */
	int			ordpos;			/* next result to return */
	ScoredTid  *cand;			/* current pass's candidates, descending score */
	int			ncand;
	int			candpos;		/* next candidate to visibility-probe */
	bool		candfull;		/* the pass filled its top-k heap, so matches
								 * may exist beyond cand[] */
	int			curk;			/* candidate width of the current pass */
	double		maxhits;		/* provable upper bound on result size (cap growth) */
	/* plain-scan (amgettuple, no ORDER BY) state for index-only counts */
	bool		plainInit;		/* have we materialized the matching TIDs? */
	ItemPointerData *plainTids; /* sorted matching TIDs */
	int			nplain;
	int			plainpos;
	bool		plainRecheck;	/* results need a heap recheck (fuzzy/regex) */
	IndexTuple	plainItup;		/* cached all-NULL itup for index-only scans */
	TupleDesc	plainItupDesc;
} WeaveScanOpaqueData;

typedef WeaveScanOpaqueData *WeaveScanOpaque;

/* ranked-scan growth (L14); defined next to the visibility machinery */
static int weave_topk_candidates_guarded(Relation index, WeaveQuery q, int wantk,
										ScoredTid **out);
static int weave_ord_width(int k);
static void weave_ord_pass(Relation index, WeaveScanOpaque so);
static void weave_ord_probe(Relation index, WeaveScanOpaque so, int want);
static bool weave_ord_grow(Relation index, WeaveScanOpaque so);

static int
cmp_tid(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

static void
tidset_sort_uniq(TidSet *s)
{
	int			i,
				j;

	/* A garbage .n (from a stale/recycled read under concurrent merge) would
	 * qsort/scan s->tids out of bounds; treat an implausible count as empty and
	 * let the scan's generation re-check restart.  See tidset_sane(). */
	if (s->n < 0 || (Size) s->n > MaxAllocSize / sizeof(ItemPointerData))
	{
		s->tids = NULL;
		s->n = 0;
		return;
	}
	if (s->n <= 1)
		return;
	qsort(s->tids, s->n, sizeof(ItemPointerData), cmp_tid);
	for (i = 0, j = 1; j < s->n; j++)
		if (ItemPointerCompare(&s->tids[i], &s->tids[j]) != 0)
			s->tids[++i] = s->tids[j];
	s->n = i + 1;
}

/*
 * Tombstone (deleted-docid) sets, one per segment, loaded from each segment's
 * livedocs blob.  VACUUM (weave_bulkdelete) records docids of vacuumed heap
 * tuples here; scans/counts MUST subtract them, because the index-only and
 * count paths trust the visibility map and would otherwise report a
 * vacuumed-and-reused heap slot as a match.  hasany is false (the common,
 * delete-free case) => zero overhead: no membership checks at all.
 */
typedef struct WeaveTombstones
{
	bool		hasany;
	uint32		nseg;
	uint8	  **blobs;			/* per-segment palloc'd blob, or NULL */
	sm_t	   *maps;			/* per-segment opened sm_t (valid iff blobs[i]) */
	bool	   *present;		/* whether segment i has a tombstone map */
}			WeaveTombstones;

static void
weave_tombstones_load(Relation index, const WeaveMetaPageData *meta, WeaveTombstones *t)
{
	uint32		s;

	t->hasany = false;
	t->nseg = meta->nsegments;
	t->blobs = NULL;
	t->maps = NULL;
	t->present = NULL;
	for (s = 0; s < meta->nsegments; s++)
		if (meta->segs[s].livedocs != InvalidBlockNumber &&
			meta->segs[s].livedocslen > 0)
		{
			t->hasany = true;
			break;
		}
	if (!t->hasany)
		return;

	t->blobs = (uint8 **) palloc0(meta->nsegments * sizeof(uint8 *));
	/*
	 * sm_t (struct sparsemap) is declared with 8-byte alignment; plain palloc
	 * only guarantees MAXALIGN (4 on ILP32), so allocate the array 8-aligned to
	 * satisfy that requirement (a misaligned sm_t trips -fsanitize=alignment).
	 */
	t->maps = (sm_t *) palloc_aligned(meta->nsegments * sizeof(sm_t), 8, 0);
	memset(t->maps, 0, meta->nsegments * sizeof(sm_t));
	t->present = (bool *) palloc0(meta->nsegments * sizeof(bool));
	for (s = 0; s < meta->nsegments; s++)
	{
		const WeaveSegMeta *sg = &meta->segs[s];

		if (sg->livedocs != InvalidBlockNumber && sg->livedocslen > 0)
		{
			t->blobs[s] = weave_read_blob(index, sg->livedocs, sg->livedocslen);
			sm_open(&t->maps[s], (uint8_t *) t->blobs[s], sg->livedocslen);
			t->present[s] = true;
		}
	}
}

static void
weave_tombstones_free(WeaveTombstones *t)
{
	uint32		s;

	if (!t->hasany)
		return;
	for (s = 0; s < t->nseg; s++)
		if (t->present[s])
			pfree(t->blobs[s]);
	pfree(t->blobs);
	pfree(t->maps);
	pfree(t->present);
}

/*
 * Drop TIDs tombstoned in ONE specific segment from a TidSet in place.
 * Tombstones are per-segment: a docid deleted in segment A must only be
 * suppressed among matches produced BY segment A -- the same heap TID may have
 * been reused by a live document in a newer segment or the pending list, and
 * that document must not be filtered.  Applied to each segment's own match
 * contribution at collection time.
 */
static void
weave_filter_tombstoned_seg(WeaveTombstones *t, uint32 segidx, TidSet *s)
{
	int			i,
				j = 0;
	uint64		stackids[256];
	bool		stackres[256];
	uint64	   *ids;
	bool	   *res;

	if (!t->hasany || s->n == 0 || segidx >= t->nseg || !t->present[segidx])
		return;
	if (s->n < 0 || (Size) s->n > MaxAllocSize / sizeof(ItemPointerData))
	{
		/* garbage count from a stale/recycled read: treat as empty (the scan's
		 * generation re-check restarts).  Guards the s->tids[] sweep below. */
		s->tids = NULL;
		s->n = 0;
		return;
	}

	/*
	 * Batched membership: extract this set's docids (already ascending, since
	 * the TidSet is TID-sorted and docid is monotonic in TID) and test them
	 * all in one left-to-right sweep with sm_contains_many -- O(chunks + n)
	 * instead of n independent head-walks.  Use a stack buffer for the common
	 * small case to avoid palloc; fall back to palloc only for large sets.
	 */
	if (s->n <= (int) lengthof(stackids))
	{
		ids = stackids;
		res = stackres;
	}
	else
	{
		ids = (uint64 *) palloc(s->n * sizeof(uint64));
		res = (bool *) palloc(s->n * sizeof(bool));
	}

	for (i = 0; i < s->n; i++)
		ids[i] = weave_tid_to_docid(&s->tids[i]);

	sm_contains_many(&t->maps[segidx], ids, res, (size_t) s->n);

	for (i = 0; i < s->n; i++)
		if (!res[i])
			s->tids[j++] = s->tids[i];
	s->n = j;

	if (ids != stackids)
	{
		pfree(ids);
		pfree(res);
	}
}

/* Read the metapage for corpus stats + dictstart. */
static void
weave_read_meta(Relation index, WeaveMetaPageData *out)
{
	Buffer		buffer = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	Page		page;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	weave_check_meta(page, index);
	weave_meta_from_page(page, out);	/* version-aware: expands a v3 metapage */
	UnlockReleaseBuffer(buffer);
}

/*
 * Read just the current segment-directory generation (cheap SHARE-locked
 * metapage peek).  A scan records this at its metapage snapshot and re-checks
 * it after collecting; if it moved, a concurrent merge/vacuum may have freed +
 * recycled pages the scan read from a now-stale segment descriptor, so the scan
 * must discard its result and restart from a fresh snapshot.
 */
static uint32
weave_read_meta_generation(Relation index)
{
	Buffer		buffer = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);
	uint32		gen;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	/* generation is at the SAME offset only within one format version; read it
	 * version-aware so a v3 metapage's generation (at the v3 offset) is correct. */
	{
		WeaveMetaPageData m;

		weave_meta_from_page(BufferGetPage(buffer), &m);
		gen = m.generation;
	}
	UnlockReleaseBuffer(buffer);
	return gen;
}

/*
 * Use a segment's sparse block index to find the single dictionary page that
 * could contain `term`: the last index entry whose term <= target.  Returns
 * that page's block number, or `dictstart` if the segment has no block index
 * (empty segment or pre-index format).  The located page is the ONLY page that
 * can hold the term (the next page's first term is > target), so point lookups
 * scan just that page.
 */
static BlockNumber
weave_dict_seek(Relation index, const WeaveSegMeta *seg,
			   const char *term, int termlen)
{
	BlockNumber iblk = seg->dictindexstart;
	BlockNumber best = seg->dictstart;

	while (iblk != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		bool		overshot = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buf = ReadBuffer(index, iblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;
		while (ptr < end)
		{
			WeaveDictIndexEntry *ie = (WeaveDictIndexEntry *) ptr;
			int			cmplen = Min((int) ie->termlen, termlen);
			int			c = memcmp(ie->term, term, cmplen);

			if (c == 0)
				c = (int) ie->termlen - termlen;
			if (c <= 0)
				best = ie->blk;		/* entry term <= target: candidate page */
			else
			{
				overshot = true;	/* entries are sorted; no need to go further */
				break;
			}
			ptr += MAXALIGN(offsetof(WeaveDictIndexEntry, term) + ie->termlen);
		}
		UnlockReleaseBuffer(buf);
		if (overshot)
			break;
		iblk = next;
	}
	return best;
}

/*
 * Look up a term in the dictionary; on hit, read its full posting list into a
 * TidSet.  Returns true if found.  weave_dict_seek uses the segment's sparse
 * block index to jump straight to the one dictionary page that can hold the
 * term (scanning the whole chain only for a segment that predates the index).
 */
static bool
weave_lookup_term(Relation index, const WeaveSegMeta *seg,
				 const char *term, int termlen, TidSet *out)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	out->tids = NULL;
	out->n = 0;

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr;
		char	   *end;
		BlockNumber firstposting = InvalidBlockNumber;
		uint32		firstoffset = 0;
		uint32		df = 0;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				firstposting = de->firstposting;
				firstoffset = de->firstoffset;
				df = de->df;
				found = true;
				break;
			}
			ptr += esize;
		}
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buffer);

		if (found)
		{
			/* read exactly this term's df postings from the shared chain */
			WeavePosting *post;
			int			np = weave_decode_term(index, firstposting, firstoffset,
										  df, &post, NULL, false, NULL, true,
										  seg->doclenstart == InvalidBlockNumber);
			ItemPointerData *tids = palloc(Max(np, 1) * sizeof(ItemPointerData));
			int			n = 0;
			int			i;

			for (i = 0; i < np; i++)
				tids[n++] = post[i].tid;
			pfree(post);
			out->tids = tids;
			out->n = n;
			tidset_sort_uniq(out);
			return true;
		}
		if (onlyone)
			break;				/* block index located the only possible page */
	}
	return false;
}

/* set operations on sorted TidSets */

/*
 * Galloping (exponential) search: return the least index >= lo in t[0..n) whose
 * tid >= key.  Used to skip runs when intersecting a small set against a large
 * one (O(|small| * log|large|) instead of O(|small|+|large|)).
 */
static inline int
tidset_gallop(const ItemPointerData *t, int n, int lo, const ItemPointerData *key)
{
	int			step = 1;
	int			hi;

	while (lo < n && ItemPointerCompare((ItemPointer) &t[lo], (ItemPointer) key) < 0)
	{
		if (lo + step < n &&
			ItemPointerCompare((ItemPointer) &t[lo + step], (ItemPointer) key) < 0)
		{
			lo += step;
			step <<= 1;
		}
		else
			break;
	}
	/* binary search in (lo, min(lo+step, n)] */
	hi = Min(lo + step, n - 1);
	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (ItemPointerCompare((ItemPointer) &t[mid], (ItemPointer) key) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * A TidSet's .n must be a plausible element count.  Under concurrent merge a
 * scan can read a segment whose pages were freed and recycled (pg_weave recycles
 * freed pages with no deletion-xid gate), yielding a decoded set whose .n is
 * garbage (e.g. leftover pointer bytes -> ~1.4 billion).  Feeding that to the
 * set-algebra primitives below made them palloc a multi-gigabyte result
 * ("invalid memory alloc request size") and walk a.tids[]/b.tids[] out of
 * bounds (SIGSEGV) -- the field-reported crash under read+insert+merge.  The
 * primitives treat an implausible operand as EMPTY: a bounded wrong (under-
 * count) result, never a crash, and the scan's generation re-check then detects
 * the stale read and restarts with a fresh directory snapshot.
 */
#define TIDSET_MAX_N ((int) (MaxAllocSize / sizeof(ItemPointerData)))
static inline TidSet
tidset_sane(TidSet s)
{
	if (s.n < 0 || s.n > TIDSET_MAX_N)
	{
		s.tids = NULL;
		s.n = 0;
	}
	return s;
}

static TidSet
tidset_and(TidSet a, TidSet b)
{
	TidSet		r;
	int			i = 0,
				j = 0,
				k = 0;

	a = tidset_sane(a);
	b = tidset_sane(b);
	r.tids = palloc(Min(a.n, b.n) * sizeof(ItemPointerData) + 1);

	/*
	 * When the sets differ greatly in size, gallop the smaller through the
	 * larger (skip-list style) so a highly selective AND does not touch every
	 * posting of the common term.  Otherwise a linear merge is cheapest.
	 */
	if (a.n > 0 && b.n > 0 && (a.n > 4 * b.n || b.n > 4 * a.n))
	{
		const ItemPointerData *sm = a.n <= b.n ? a.tids : b.tids;
		const ItemPointerData *lg = a.n <= b.n ? b.tids : a.tids;
		int			sn = Min(a.n, b.n);
		int			ln = Max(a.n, b.n);
		int			li = 0;
		int			si;

		for (si = 0; si < sn; si++)
		{
			li = tidset_gallop(lg, ln, li, &sm[si]);
			if (li >= ln)
				break;
			if (ItemPointerCompare((ItemPointer) &lg[li], (ItemPointer) &sm[si]) == 0)
				r.tids[k++] = sm[si];
		}
		r.n = k;
		return r;
	}

	while (i < a.n && j < b.n)
	{
		int			c = ItemPointerCompare(&a.tids[i], &b.tids[j]);

		if (c == 0)
		{
			r.tids[k++] = a.tids[i];
			i++;
			j++;
		}
		else if (c < 0)
			i++;
		else
			j++;
	}
	r.n = k;
	return r;
}

static TidSet
tidset_or(TidSet a, TidSet b)
{
	TidSet		r;
	int			i = 0,
				j = 0,
				k = 0;

	a = tidset_sane(a);
	b = tidset_sane(b);
	r.tids = palloc((a.n + b.n) * sizeof(ItemPointerData) + 1);
	while (i < a.n && j < b.n)
	{
		int			c = ItemPointerCompare(&a.tids[i], &b.tids[j]);

		if (c == 0)
		{
			r.tids[k++] = a.tids[i];
			i++;
			j++;
		}
		else if (c < 0)
			r.tids[k++] = a.tids[i++];
		else
			r.tids[k++] = b.tids[j++];
	}
	while (i < a.n)
		r.tids[k++] = a.tids[i++];
	while (j < b.n)
		r.tids[k++] = b.tids[j++];
	r.n = k;
	return r;
}

/* a AND NOT b (b subtracted from a) */
static TidSet
tidset_andnot(TidSet a, TidSet b)
{
	TidSet		r;
	int			i = 0,
				j = 0,
				k = 0;

	a = tidset_sane(a);
	b = tidset_sane(b);
	r.tids = palloc(a.n * sizeof(ItemPointerData) + 1);
	while (i < a.n)
	{
		if (j >= b.n)
			r.tids[k++] = a.tids[i++];
		else
		{
			int			c = ItemPointerCompare(&a.tids[i], &b.tids[j]);

			if (c == 0)
			{
				i++;
				j++;
			}
			else if (c < 0)
				r.tids[k++] = a.tids[i++];
			else
				j++;
		}
	}
	r.n = k;
	return r;
}

/*
 * weave_lookup_prefix -- union the posting lists of every dictionary term that
 * begins with the given prefix.  Dictionary entries are byte-sorted, so the
 * matching terms are contiguous: seek (via the sparse per-page block index) to
 * the page that can hold the prefix, then scan forward only while entries could
 * still start with the prefix, stopping at the first term that sorts past it.
 * Sublinear in the dictionary rather than a full scan.
 */
static void
weave_lookup_prefix(Relation index, const WeaveSegMeta *seg,
				   const char *prefix, int prefixlen, TidSet *out)
{
	BlockNumber blk = weave_dict_seek(index, seg, prefix, prefixlen);
	int			cap = 32;
	int			n = 0;
	ItemPointerData *tids = palloc(cap * sizeof(ItemPointerData));
	bool		done = false;

	while (blk != InvalidBlockNumber && !done)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;
			int			cmplen;
			int			c;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
			cmplen = Min((int) de->termlen, prefixlen);
			c = memcmp(de->term, prefix, cmplen);

			if (c < 0 || (c == 0 && (int) de->termlen < prefixlen))
			{
				/* term sorts before the prefix: not there yet, keep scanning */
				ptr += esize;
				continue;
			}
			if (c > 0)
			{
				/* first prefixlen bytes exceed the prefix: sorted, so no more
				 * matches can follow -- stop */
				done = true;
				break;
			}
			/* c == 0 and de->termlen >= prefixlen: a prefix match */
			{
				WeavePosting *post;
				int			np = weave_decode_term(index, de->firstposting,
												  de->firstoffset, de->df,
												  &post, NULL, false, NULL, true,
												  seg->doclenstart == InvalidBlockNumber);
				int			k;

				for (k = 0; k < np; k++)
				{
					if (n >= cap)
					{
						cap *= 2;
						tids = repalloc(tids, cap * sizeof(ItemPointerData));
					}
					tids[n++] = post[k].tid;
				}
				pfree(post);
			}
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		blk = next;
	}

	out->tids = tids;
	out->n = n;
	tidset_sort_uniq(out);
}

/*
 * Evaluate the query into a TidSet via a stack machine over the RPN items.
 * NOT is handled specially: a bare NOT is only meaningful as "a AND NOT b", so
 * we track whether each stack entry is "positive" (a TID set) or "negative"
 * (the complement of a TID set).  AND/OR combine them with De Morgan; a top-
 * level negative result is complemented against all indexed TIDs (the universe).
 *
 * THE UNIVERSE IS BUILT LAZILY, and that is a performance requirement rather
 * than a style preference.  Building it reads EVERY posting list of EVERY term
 * in the segment -- Sum(df) work, the whole expanded inverted index, typically
 * two orders of magnitude more than ndocs.  It was previously built eagerly
 * whenever the query contained any NOT at all, but De Morgan means the common
 * shape `a & !b` reduces to andnot(a, b) and never consults it: the universe is
 * needed only when the TOP-LEVEL result is still negated, i.e. for a purely
 * negative query like `!b`.  Measured cost of getting this wrong, on a 200k-row
 * corpus: `count(*) WHERE d @@@ 'common & !rare'` took 603.9 ms against
 * tsvector+GIN's 14.7 ms -- a 41x loss, entirely spent materialising a set the
 * evaluation then discarded.  Boolean NOT was in no predecessor's benchmark
 * matrix, which is why it went unmeasured for the whole life of the fork.
 */
typedef struct EvalVal
{
	TidSet		set;
	bool		negated;		/* true => set represents docs NOT to include */
} EvalVal;

/*
 * Everything weave_universe_bounded needs, so the evaluator can defer the call.
 * `built` memoises within one evaluation; a query can only need the universe
 * once, but the flag keeps that a local fact rather than an assumption.
 */
typedef struct UniverseSrc
{
	Relation	index;
	BlockNumber dictstart;
	double		ndocs;
	bool		has_doclen_col;
	bool		built;
	TidSet		set;
} UniverseSrc;

static TidSet weave_universe_bounded(Relation index, BlockNumber dictstart,
									 double ndocs, bool has_doclen_col);

static TidSet
universe_get(UniverseSrc *u)
{
	if (!u->built)
	{
		u->set = weave_universe_bounded(u->index, u->dictstart, u->ndocs,
										u->has_doclen_col);
		u->built = true;
	}
	return u->set;
}

static TidSet
weave_eval_query(Relation index, const WeaveSegMeta *seg, WeaveQuery q,
				UniverseSrc *universe)
{
	EvalVal    *stack;
	int			top = 0;
	uint32		i;
	TidSet		result;

	if (q->nitems == 0)
	{
		result.tids = NULL;
		result.n = 0;
		return result;
	}

	stack = palloc(q->nitems * sizeof(EvalVal));

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			TidSet		s;

			if (it->flags & WEAVE_QF_PREFIX)
				weave_lookup_prefix(index, seg,
								   WEAVE_QUERY_ITEMTEXT(q, it), it->termlen, &s);
			else if (!weave_lookup_term(index, seg,
									   WEAVE_QUERY_ITEMTEXT(q, it), it->termlen, &s))
			{
				/*
				 * weave_lookup_term writes *out ONLY when the term is present in
				 * this segment; on a miss it returns false and leaves `s`
				 * UNINITIALIZED.  Pushing that uninitialized TidSet left stale
				 * stack bytes in .tids/.n (e.g. .n picking up a leftover
				 * pointer's low word), which a later tidset_or/tidset_and read as
				 * a multi-billion-element set -> "invalid memory alloc request
				 * size" / SIGSEGV.  A term absent from a segment is the empty set.
				 * (weave_lookup_prefix always writes *out, so it needs no guard.)
				 */
				s.tids = NULL;
				s.n = 0;
			}
			stack[top].set = s;
			stack[top].negated = false;
			top++;
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			Assert(top >= 1);
			stack[top - 1].negated = !stack[top - 1].negated;
		}
		else					/* AND / OR */
		{
			EvalVal		b = stack[--top];
			EvalVal		a = stack[--top];
			EvalVal		res;

			if (it->op == WEAVE_OP_AND || it->op == WEAVE_OP_PHRASE)
			{
				/* PHRASE is treated as AND for candidate generation; the
				 * bitmap heap recheck (@@@) enforces adjacency exactly. */
				if (!a.negated && !b.negated)
				{
					res.set = tidset_and(a.set, b.set);
					res.negated = false;
				}
				else if (!a.negated && b.negated)
				{
					res.set = tidset_andnot(a.set, b.set);
					res.negated = false;
				}
				else if (a.negated && !b.negated)
				{
					res.set = tidset_andnot(b.set, a.set);
					res.negated = false;
				}
				else			/* !a AND !b = !(a OR b) */
				{
					res.set = tidset_or(a.set, b.set);
					res.negated = true;
				}
			}
			else				/* OR */
			{
				if (!a.negated && !b.negated)
				{
					res.set = tidset_or(a.set, b.set);
					res.negated = false;
				}
				else if (a.negated && b.negated)	/* !a OR !b = !(a AND b) */
				{
					res.set = tidset_and(a.set, b.set);
					res.negated = true;
				}
				else
				{
					/* positive OR negative: !x OR y = !(x AND NOT y) */
					TidSet		pos = a.negated ? b.set : a.set;
					TidSet		neg = a.negated ? a.set : b.set;

					res.set = tidset_andnot(neg, pos);
					res.negated = true;
				}
			}
			stack[top++] = res;
		}
	}

	Assert(top == 1);
	if (stack[0].negated)
		result = tidset_andnot(universe_get(universe), stack[0].set);
	else
		result = stack[0].set;

	return result;
}

/*
 * weave_fuzzy_terms -- collect the postings of every dictionary term within edit
 * distance k of `term`, using the Levenshtein automaton (pg_weave_lev.c) directly
 * over the sorted dictionary.  This is EXACT: only true within-k terms are
 * collected, so no heap recheck is needed (unlike the trigram funnel, which
 * over-generates candidates that must be re-verified per doc).  Returns true
 * (always applicable); *out is a sorted TidSet.  For query terms longer than
 * the automaton bound, returns false so the caller falls back to the funnel.
 */
static bool
weave_fuzzy_terms(Relation index, const WeaveSegMeta *seg,
				 const char *term, int termlen, int k, TidSet *out)
{
	WeaveLevAut	aut;
	BlockNumber blk;
	ItemPointerData *tids;
	unsigned char nextkey[WEAVE_LEV_MAXQ + 2];

	/* per-matching-term sorted runs, merged (not sorted) at the end */
	ItemPointerData **runs = NULL;
	int		   *runlen = NULL;
	int			nruns = 0;
	int			runcap = 0;
	int64		total = 0;

	if (termlen > WEAVE_LEV_MAXQ)
		return false;			/* fall back to trigram funnel + recheck */

	aut.q = (const unsigned char *) term;
	aut.m = termlen;
	aut.k = k;

	/*
	 * Automaton-guided dictionary walk.  Terms are byte-sorted, so when a term
	 * dead-ends at prefix cand[0..deadlen), every term sharing that prefix is
	 * also a dead end -- we jump past them by seeking (via the per-page block
	 * index) to the smallest string greater than that prefix.  This turns an
	 * O(all terms) scan into roughly O(matching terms + boundaries), the effect
	 * an FST/DFA intersection gives.
	 */
	blk = seg->dictstart;
	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		bool		reseek = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;
			int			deadlen;
			bool		match;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if (abs((int) de->termlen - termlen) <= k)
				match = weave_lev_match_prefix(&aut,
											 (const unsigned char *) de->term,
											 (int) de->termlen, &deadlen);
			else
			{
				/* run the automaton anyway to learn the dead prefix for skipping */
				match = weave_lev_match_prefix(&aut,
											 (const unsigned char *) de->term,
											 (int) de->termlen, &deadlen);
				match = false;		/* length filter still rules it out */
			}

			if (match)
			{
				WeavePosting *post;
				int			np = weave_decode_term(index, de->firstposting,
												  de->firstoffset, de->df,
												  &post, NULL, false, NULL, true,
												  seg->doclenstart == InvalidBlockNumber);

				/* keep this term's docid-sorted TIDs as a run for k-way merge */
				if (np > 0)
				{
					ItemPointerData *run = palloc(np * sizeof(ItemPointerData));
					int			i;

					for (i = 0; i < np; i++)
						run[i] = post[i].tid;
					if (nruns >= runcap)
					{
						runcap = Max(runcap * 2, 16);
						runs = runs ? repalloc(runs, runcap * sizeof(ItemPointerData *))
							: palloc(runcap * sizeof(ItemPointerData *));
						runlen = runlen ? repalloc(runlen, runcap * sizeof(int))
							: palloc(runcap * sizeof(int));
					}
					runs[nruns] = run;
					runlen[nruns] = np;
					nruns++;
					total += np;
				}
				pfree(post);
				ptr += esize;
				continue;
			}

			/*
			 * Dead end at cand[0..deadlen).  The next term that could match is
			 * >= that prefix with its last byte incremented.  If that key is
			 * beyond the next dictionary entry, seek to it via the block index
			 * (jumping whole pages); otherwise just step to the next entry.
			 */
			/*
			 * Skip only on a GENUINE prefix death: the automaton exceeded k while
			 * still consuming the term (deadlen < termlen), so every term sharing
			 * cand[0..deadlen) is dead.  If deadlen == termlen the term was fully
			 * consumed without dying (it failed only on length or final accept),
			 * so LONGER terms with this prefix may still match -- must NOT skip.
			 */
			if (deadlen > 0 && deadlen < (int) de->termlen)
			{
				int			kl = deadlen;

				memcpy(nextkey, de->term, kl);
				/* increment the last byte of the dead prefix; carry on 0xff */
				while (kl > 0 && nextkey[kl - 1] == 0xff)
					kl--;
				if (kl == 0)
				{
					/* prefix is all 0xff: nothing greater can match; done */
					UnlockReleaseBuffer(buffer);
					goto done;
				}
				nextkey[kl - 1]++;

				/* is the very next entry already >= nextkey? then no gain */
				{
					char	   *nptr = ptr + esize;

					if (nptr < end)
					{
						WeaveDictEntry *nde = (WeaveDictEntry *) nptr;
						int			cmplen = Min((int) nde->termlen, kl);
						int			c = memcmp(nde->term, nextkey, cmplen);

						if (c > 0 || (c == 0 && (int) nde->termlen >= kl))
						{
							ptr = nptr;		/* next entry is past the dead run */
							continue;
						}
					}
				}
				/* seek to the page holding nextkey; only jump if it advances to a
				 * LATER page (else keep scanning this page linearly -- avoids
				 * re-seeking onto the same/earlier page and looping) */
				{
					BlockNumber tgt = weave_dict_seek(index, seg,
													 (const char *) nextkey, kl);

					if (tgt != InvalidBlockNumber && tgt != blk)
					{
						reseek = true;
						blk = tgt;
						UnlockReleaseBuffer(buffer);
						break;
					}
				}
				/* target is on this same page: just step to the next entry */
				ptr += esize;
				continue;
			}
			ptr += esize;
		}
		if (reseek)
			continue;
		UnlockReleaseBuffer(buffer);
		blk = next;
	}

done:
	/*
	 * k-way merge the per-term docid-sorted runs into one sorted, de-duplicated
	 * TID array.  Each posting list is already docid-ordered, so merging avoids
	 * the O(n log n) qsort over the whole (up to ~1.3M) union -- which a profiler
	 * showed was the dominant fuzzy-count cost (a 1.28M qsort with a
	 * function-pointer comparator is ~400ms) -- replacing it with O(n log k)
	 * and cheap inline comparisons.
	 */
	{
		int		   *pos;			/* current index into each run */
		int		   *heap;			/* min-heap of run indices by current head TID */
		int			hn = 0;
		int			nout = 0;
		int			r;

		tids = palloc(Max(total, 1) * sizeof(ItemPointerData));
		pos = palloc0(Max(nruns, 1) * sizeof(int));
		heap = palloc(Max(nruns, 1) * sizeof(int));

#define RUN_HEAD(ri) (&runs[(ri)][pos[(ri)]])
#define HEAP_LESS(x, y) (ItemPointerCompare(RUN_HEAD(heap[x]), RUN_HEAD(heap[y])) < 0)
		/* build the heap with each non-empty run's head */
		for (r = 0; r < nruns; r++)
		{
			if (runlen[r] > 0)
			{
				int			c = hn++;

				heap[c] = r;
				while (c > 0 && HEAP_LESS(c, (c - 1) / 2))
				{
					int			t = heap[c];

					heap[c] = heap[(c - 1) / 2];
					heap[(c - 1) / 2] = t;
					c = (c - 1) / 2;
				}
			}
		}
		while (hn > 0)
		{
			int			best = heap[0];
			int			c = 0;

			CHECK_FOR_INTERRUPTS();	/* per merged posting; no lock held (chain released above) */
			if (nout == 0 ||
				ItemPointerCompare(&tids[nout - 1], RUN_HEAD(best)) != 0)
				tids[nout++] = *RUN_HEAD(best);
			pos[best]++;
			if (pos[best] >= runlen[best])
			{
				heap[0] = heap[--hn];	/* drop exhausted run */
			}
			/* sift down heap[0] */
			for (;;)
			{
				int			l = 2 * c + 1,
							ri = 2 * c + 2,
							sm = c;

				if (hn == 0)
					break;
				if (l < hn && HEAP_LESS(l, sm))
					sm = l;
				if (ri < hn && HEAP_LESS(ri, sm))
					sm = ri;
				if (sm == c)
					break;
				{
					int			t = heap[c];

					heap[c] = heap[sm];
					heap[sm] = t;
					c = sm;
				}
			}
		}
#undef RUN_HEAD
#undef HEAP_LESS
		out->tids = tids;
		out->n = nout;
	}
	return true;
}

/* Build the universe: the set of DISTINCT TIDs present in any posting list of
 * the segment.  Used by top-level NOT and by the regex/fuzzy fallback when no
 * trigram acceleration is available.
 *
 * The distinct TID count is bounded by the segment's live+dead doc count
 * (ndocs), but the raw postings summed across every term total Sum(df) -- the
 * whole expanded inverted index, often two orders of magnitude larger than
 * ndocs.  Accumulating all of them before a single final sort_uniq made the
 * scratch buffer grow to Sum(df) TIDs and could reach a multi-gigabyte
 * "invalid memory alloc request size" on a high-vocabulary corpus.  We instead
 * fold (sort_uniq) whenever the buffer grows past a small multiple of ndocs,
 * so peak memory stays O(ndocs) regardless of Sum(df). */
static TidSet
weave_universe_bounded(Relation index, BlockNumber dictstart, double ndocs,
					  bool has_doclen_col)
{
	TidSet		u;
	BlockNumber blk = dictstart;
	int			cap = 64;
	int			n = 0;
	ItemPointerData *tids = palloc(cap * sizeof(ItemPointerData));

	/* Fold threshold: once the buffer holds more than fold_at raw TIDs,
	 * sort_uniq collapses it back to the <= ndocs distinct ones.  A distinct
	 * count can never exceed ndocs, so this keeps peak memory O(ndocs) instead
	 * of O(Sum(df)).  Guard against a bogus/zero ndocs with a floor. */
	int			fold_at;

	{
		double		f = ndocs * 2.0 + 1024.0;

		if (f > (double) TIDSET_MAX_N)
			f = (double) TIDSET_MAX_N;
		fold_at = (f < 1024.0) ? 1024 : (int) f;
	}

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;
			WeavePosting *post;
			int			np;
			int			k;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);
			np = weave_decode_term(index, de->firstposting,
								  de->firstoffset, de->df,
								  &post, NULL, false, NULL, true,
								  has_doclen_col);

			for (k = 0; k < np; k++)
			{
				if (n >= cap)
				{
					/* Fold before growing past the ndocs-relative threshold:
					 * collapse duplicates so the buffer stays O(ndocs), never
					 * O(Sum df).  After folding, either there is now room (the
					 * common case -- fall through to store) or the distinct set
					 * itself is genuinely near cap, in which case we grow. */
					if (n >= fold_at)
					{
						u.tids = tids;
						u.n = n;
						tidset_sort_uniq(&u);
						tids = u.tids;
						n = u.n;
					}
					if (n >= cap)
					{
						if ((Size) cap * 2 * sizeof(ItemPointerData) > MaxAllocSize)
							ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("pg_weave: candidate set for this query is too large"),
									 errhint("Build the index WITH (trigrams = on) so fuzzy/regex/NOT queries can be accelerated.")));
						cap *= 2;
						tids = repalloc(tids, cap * sizeof(ItemPointerData));
					}
				}
				tids[n++] = post[k].tid;
			}
			pfree(post);
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		blk = next;
	}

	u.tids = tids;
	u.n = n;
	tidset_sort_uniq(&u);
	return u;
}

IndexScanDesc
weave_beginscan(Relation r, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(r, nkeys, norderbys);
	WeaveScanOpaque so = (WeaveScanOpaque) palloc0(sizeof(WeaveScanOpaqueData));

	so->query = NULL;
	so->queryValid = false;
	so->orderInit = false;
	so->ordered = NULL;
	so->nordered = 0;
	so->maxordered = 0;
	so->ordpos = 0;
	so->cand = NULL;
	so->ncand = 0;
	so->candpos = 0;
	so->candfull = false;
	so->curk = 0;
	so->maxhits = 0;
	so->plainInit = false;
	so->plainTids = NULL;
	so->nplain = 0;
	so->plainpos = 0;
	so->plainRecheck = false;
	scan->opaque = so;
	/* the AM owns allocation of the order-by result arrays */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals = palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = palloc(sizeof(bool) * norderbys);
	}
	return scan;
}

void
weave_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			ScanKey orderbys, int norderbys)
{
	WeaveScanOpaque so = (WeaveScanOpaque) scan->opaque;

	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));

	so->queryValid = false;
	if (scan->numberOfKeys >= 1)
	{
		WeaveQuery	q = DatumGetWQuery(scan->keyData[0].sk_argument);

		so->query = q;
		so->queryValid = true;
	}

	/* ordering scan: the query is the <=> operator's right operand */
	if (orderbys && scan->numberOfOrderBys > 0)
		memmove(scan->orderByData, orderbys,
				scan->numberOfOrderBys * sizeof(ScanKeyData));
	so->orderInit = false;
	so->ordered = NULL;
	so->nordered = 0;
	so->maxordered = 0;
	so->ordpos = 0;
	so->cand = NULL;
	so->ncand = 0;
	so->candpos = 0;
	so->candfull = false;
	so->curk = 0;
	so->maxhits = 0;
	so->plainInit = false;
	so->plainTids = NULL;
	so->nplain = 0;
	so->plainpos = 0;
	so->plainRecheck = false;
	if (scan->numberOfOrderBys >= 1)
	{
		so->query = DatumGetWQuery(scan->orderByData[0].sk_argument);
		so->queryValid = true;
	}
}

/*
 * weave_canreturn: whether the index can return a column value for an
 * index-only scan.  The weave index is NOT covering, so it cannot -- see the
 * body.
 */
bool
weave_canreturn(Relation index, int attno)
{
	/*
	 * The weave index is NOT covering: it stores analyzed postings, not the
	 * original wdoc, so it cannot reproduce a column value.  Returning true
	 * caused an index-only scan that SELECTs the indexed column to yield NULLs
	 * (a placeholder tuple).  Return false so the planner never uses an
	 * index-only scan to fetch a real attribute.  (count(*)/EXISTS need no
	 * attribute but still include the @@@ restriction column in the IOS
	 * coverage check, so they run through a bitmap/plain index scan; our
	 * visibility-map-aware weave_count() is the explicit fast count.)
	 */
	return false;
}

/*
 * Fill scan->xs_itup with a cached all-NULL index tuple if the executor ever
 * requests one for an index-only scan (xs_want_itup).  Currently inactive:
 * weave_canreturn() returns false, so the planner never chooses an index-only
 * scan and xs_want_itup is never set -- this is a guarded no-op kept so the
 * gettuple paths remain correct if a covering capability is ever added.
 */
static inline void
weave_set_itup(IndexScanDesc scan, WeaveScanOpaque so)
{
	if (!scan->xs_want_itup)
		return;
	if (so->plainItup == NULL)
	{
		TupleDesc	td = RelationGetDescr(scan->indexRelation);
		Datum	   *values = palloc0(sizeof(Datum) * td->natts);
		bool	   *isnull = palloc(sizeof(bool) * td->natts);
		int			a;

		for (a = 0; a < td->natts; a++)
			isnull[a] = true;
		so->plainItup = index_form_tuple(td, values, isnull);
		so->plainItupDesc = td;
		pfree(values);
		pfree(isnull);
	}
	scan->xs_itup = so->plainItup;
	scan->xs_itupdesc = so->plainItupDesc;
}

/*
 * weave_gettuple: ordering scan for ORDER BY (wdoc <=> wquery) LIMIT k.
 * On the first call it computes the block-max WAND top-k (visibility-filtered)
 * into scan state, then returns tuples one per call in ascending distance
 * (descending relevance), setting xs_orderbyvals so the executor can honor the
 * ORDER BY without a sort.  Only forward scans are supported.
 */
bool
weave_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	WeaveScanOpaque so = (WeaveScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "weave: only forward ordering scans are supported");

	/*
	 * amoptionalkey is true (see weave_handler), so the planner may hand us a
	 * scan with no restriction clause.  That is intended: it is how the keyless
	 * ordering scan works, and weave_rescan takes the query from the order-by
	 * argument in that case.
	 *
	 * But a scan with NEITHER a scan key NOR an order-by has no query at all,
	 * and there is nothing correct to return.  Silently returning zero rows
	 * would make such a plan produce an empty result set -- a wrong answer, not
	 * a slow one -- so fail loudly.  Reachable via a partial index, where a
	 * useful predicate alone can justify a path.
	 */
	if (scan->numberOfKeys == 0 && scan->numberOfOrderBys == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a weave index scan requires a query"),
				 errdetail("The scan has neither a @@@ restriction nor an ORDER BY <=> ordering clause."),
				 errhint("Add \"WHERE col @@@ query\" or \"ORDER BY col <=> query\".")));

	if (!so->queryValid || so->query == NULL)
		return false;

	/*
	 * Plain scan (no ORDER BY <=>): stream the matching TIDs in heap order for
	 * a plain Index Scan.  (The common @@@ path is the bitmap scan via
	 * amgetbitmap; this amgettuple path serves a plain index scan when the
	 * planner chooses one, e.g. with bitmap scans disabled.)  The matches come
	 * from the same evaluator, so results are identical; the executor applies
	 * MVCC visibility on the heap fetch.
	 */
	if (scan->numberOfOrderBys == 0)
	{
		if (!so->plainInit)
		{
			TidSet		m;

			pgstat_count_index_scan(scan->indexRelation);
			weave_collect_matches(scan->indexRelation, so->query, &m, &so->plainRecheck);
			so->plainTids = m.tids;
			so->nplain = m.n;
			so->plainpos = 0;
			so->plainInit = true;
		}
		if (so->plainpos >= so->nplain)
			return false;
		scan->xs_heaptid = so->plainTids[so->plainpos++];
		scan->xs_recheck = so->plainRecheck;
		weave_set_itup(scan, so);
		return true;
	}

	if (!so->orderInit)
	{
		/*
		 * Adaptive-width WAND.  Start narrow so a small LIMIT (the common first
		 * page) does minimal work -- WAND prunes hard for a small k -- and widen
		 * on demand.  The pass's candidates are handed out LAZILY (visibility is
		 * checked a batch at a time in weave_ord_probe), so the whole width of
		 * the pass is available to the executor, not just the first
		 * pg_weave.wand_initial_k rows of it: the x4 over-fetch that used to
		 * exist only to survive MVCC filtering now also sets how deep a single
		 * pass can serve.  See weave_ord_pass and doc/GAPS.md G13.
		 */
		double		N;
		WeaveMetaPageData m0;

		pgstat_count_index_scan(scan->indexRelation);
		weave_read_meta(scan->indexRelation, &m0);
		N = m0.ndocs < 1.0 ? 1.0 : m0.ndocs;
		so->maxhits = weave_query_maxhits(scan->indexRelation, so->query, N);
		/*
		 * Initial WAND k.  PostgreSQL gives an access method no way to learn the
		 * query's LIMIT, so the scan starts at some k and grows x4 on demand.
		 *
		 * This value was 100, chosen so the whole LIMIT 11..100 range is served by
		 * ONE pass rather than a pass-then-recompute.  The competitive benchmark
		 * showed what that costs: pg_weave's ranked latency is IDENTICAL at
		 * LIMIT 10 and LIMIT 100 -- measured k100/k10 ratios of 1.003, 1.007 and
		 * 0.999 across the rare, mid and common bands -- because a LIMIT 10 query
		 * does a k=100 pass.  Timescale pg_textsearch, over the same corpus and
		 * query shape, scales 1.9x-4.1x with k and is 10-21x faster at k=10.
		 * A top-k engine that does not get cheaper as k shrinks is not pruning for
		 * the dominant query shape, which is a first page of ten results.
		 *
		 * Made a GUC so the trade can be swept in one benchmark run instead of
		 * guessed at: bench/compete sweeps it and bench/RESULTS_WAND_K.md records
		 * the frontier the default is chosen from.  See doc/GAPS.md G13.
		 */
		so->curk = weave_ord_width(pg_weave_wand_initial_k);
		weave_ord_pass(scan->indexRelation, so);
		so->ordpos = 0;
		so->orderInit = true;
	}

	/*
	 * Produce the next visible row.  Three sources, in increasing cost:
	 *
	 *  1. already materialized in ordered[] -- free;
	 *  2. the current pass's remaining candidates -- one heap probe each;
	 *  3. a wider pass (weave_ord_grow) -- a full WAND recompute.
	 *
	 * (3) EXTENDS the scan rather than repeating it: ordered[] survives, so the
	 * rows already handed out are neither re-probed nor re-emitted.
	 */
	while (so->ordpos >= so->nordered)
	{
		if (so->candpos < so->ncand)
		{
			/*
			 * Probe a geometrically growing batch rather than exactly one row,
			 * so the heap open and fetch setup amortize over the rows that
			 * follow.  batch > nordered always (ordpos >= nordered here), so
			 * each iteration consumes at least one candidate and cannot spin.
			 */
			int			batch = so->nordered < INT_MAX / 2 ? so->nordered * 2 : INT_MAX;

			batch = Max(batch, 16);
			batch = Max(batch, so->ordpos + 1);
			weave_ord_probe(scan->indexRelation, so, batch);
			if (so->ordpos < so->nordered)
				break;
		}
		/* candidates exhausted: widen the pass, or the scan is complete */
		if (!weave_ord_grow(scan->indexRelation, so))
			return false;
	}

	scan->xs_heaptid = so->ordered[so->ordpos].tid;
	scan->xs_recheck = false;	/* score computed exactly from the index */
	weave_set_itup(scan, so);
	if (scan->numberOfOrderBys > 0)
	{
		IndexOrderByDistance dist;
		Oid			typ = FLOAT8OID;

		dist.value = so->ordered[so->ordpos].score;
		dist.isnull = false;
		index_store_float8_orderby_distances(scan, &typ, &dist, false);
	}
	so->ordpos++;
	return true;
}

/*
 * ------------------- positional phrase evaluation (from postings) -------------
 *
 * When the index is built WITH (positions=on), a phrase/NEAR query can be
 * answered DIRECTLY from the posting lists -- no heap access, no recheck.  We
 * intersect the phrase's terms on docid (cheap and selective), then verify
 * adjacency per surviving docid using the stored token positions via the shared
 * weave_phrase_step_pos() -- the exact adjacency logic the heap recheck uses, so
 * the result is identical.  need_recheck is then false and the recheck cliff is
 * gone.
 *
 * This fast path handles a PURE PHRASE CHAIN: an RPN of plain (non prefix/
 * fuzzy/regex) term operands combined only by WEAVE_OP_PHRASE -- i.e. the shape
 * to_wquery produces for "a b c" and NEAR(a b c, k).  Anything mixing phrase
 * with boolean AND/OR/NOT, or a phrase term that is a prefix/fuzzy/regex, is
 * left to the existing AND + recheck path (still correct, just slower).
 */

/* one term's postings decoded with positions, docid-sorted */
typedef struct PosPosting
{
	uint64		docid;
	uint32	   *pos;			/* tf ascending positions (into an arena) */
	int			npos;
}			PosPosting;

typedef struct PosTermList
{
	PosPosting *posts;
	int			nposts;
	WeavePosting *raw;			/* decoder output (owns tids/pos slots) */
	uint32	   *arena;			/* positions arena to free */
}			PosTermList;

/*
 * Is the query a pure phrase chain we can evaluate positionally?  Returns the
 * ordered list of term-operand indices (into query->items) via *termidx and
 * their count via *nterms, plus the max phrase distance (all PHRASE ops share
 * the chain).  Requires: items are VAL/PHRASE only, every VAL is a plain term
 * (no PREFIX/FUZZY/REGEX), and the RPN is the canonical left-deep phrase chain
 * (v1 v2 PHRASE v3 PHRASE ...).  For NEAR the per-op distance may differ per
 * step; we carry each step's distance in *dist[].
 */
static bool
weave_phrase_chain(WeaveQuery q, int *termidx, uint32 *stepdist, int *nterms)
{
	int			nt = 0;
	uint32		i;
	int			stack = 0;

	if (q->nitems < 3)
		return false;			/* need at least v v PHRASE */

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX))
				return false;
			if (nt >= WEAVE_QUERY_MAX_PHRASE_TERMS)
				return false;
			/*
			 * Only the CANONICAL LEFT-DEEP chain (v1 v2 PHRASE v3 PHRASE ...)
			 * is safe here: it evaluates as phrase(phrase(v1,v2),v3), pairing
			 * consecutive terms -- which is what "a b c" emits and what this
			 * loop's left-to-right stepdist chaining computes.  NEAR(a b c,k)
			 * instead emits all terms first then the PHRASE ops (v1 v2 v3
			 * PHRASE PHRASE), which the recheck stack machine evaluates
			 * RIGHT-deep as phrase(v1,phrase(v2,v3)) -- a different match set.
			 * A VAL pushed while >=2 operands are already pending is that
			 * non-left-deep shape: bail to the (correct) recheck path.
			 */
			if (stack >= 2)
				return false;
			termidx[nt++] = (int) i;
			stack++;
		}
		else if (it->type == WEAVE_QI_OPR && it->op == WEAVE_OP_PHRASE)
		{
			if (stack < 2)
				return false;
			/* step k joins term (nt-1) to its predecessor: record its distance */
			stepdist[nt - 2] = it->distance;
			stack--;			/* phrase collapses two operands to one */
		}
		else
			return false;		/* any boolean op / other operator: not pure */
	}
	*nterms = nt;
	return (stack == 1 && nt >= 2);
}

static int
cmp_pospost_docid(const void *a, const void *b)
{
	uint64		da = ((const PosPosting *) a)->docid;
	uint64		db = ((const PosPosting *) b)->docid;

	return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/*
 * Look up one term in a segment and decode its postings WITH positions,
 * docid-sorted.  Returns:
 *   WEAVE_POSLOOKUP_OK      -- found, positions present (out is populated)
 *   WEAVE_POSLOOKUP_ABSENT  -- term not in this segment (clean empty phrase)
 *   WEAVE_POSLOOKUP_NOPOS   -- found but a block dropped positions (Sum(tf)
 *                             overflowed a page): caller MUST fall back to the
 *                             recheck path for correctness.
 */
typedef enum
{
	WEAVE_POSLOOKUP_OK = 0,
	WEAVE_POSLOOKUP_ABSENT,
	WEAVE_POSLOOKUP_NOPOS
}			WeavePosLookup;

static WeavePosLookup
weave_lookup_term_pos(Relation index, const WeaveSegMeta *seg,
					 const char *term, int termlen, PosTermList *out)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	out->posts = NULL;
	out->nposts = 0;
	out->raw = NULL;
	out->arena = NULL;

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber firstposting = InvalidBlockNumber;
		uint32		firstoffset = 0;
		uint32		df = 0;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				firstposting = de->firstposting;
				firstoffset = de->firstoffset;
				df = de->df;
				found = true;
				break;
			}
			ptr += esize;
		}
		blk = WeavePageGetOpaque(page)->nextblk;
		UnlockReleaseBuffer(buffer);

		if (found)
		{
			WeavePosting *post;
			uint32	   *arena = NULL;
			int			np = weave_decode_term(index, firstposting, firstoffset,
											  df, &post, NULL, true, &arena, false,
										  seg->doclenstart == InvalidBlockNumber);
			PosPosting *pp = (PosPosting *) palloc(Max(np, 1) * sizeof(PosPosting));
			int			k;

			for (k = 0; k < np; k++)
			{
				if (post[k].pos == NULL && post[k].tf > 0)
				{
					/* a block dropped positions: cannot verify adjacency here */
					pfree(pp);
					pfree(post);
					if (arena)
						pfree(arena);
					return WEAVE_POSLOOKUP_NOPOS;
				}
				pp[k].docid = weave_tid_to_docid(&post[k].tid);
				pp[k].pos = post[k].pos;
				pp[k].npos = (int) post[k].tf;
			}
			if (np > 1)
				qsort(pp, np, sizeof(PosPosting), cmp_pospost_docid);
			out->posts = pp;
			out->nposts = np;
			out->raw = post;
			out->arena = arena;
			return WEAVE_POSLOOKUP_OK;
		}
		if (onlyone)
			break;
	}
	return WEAVE_POSLOOKUP_ABSENT;	/* term absent in this segment */
}

static void
weave_free_posterm(PosTermList *pl)
{
	if (pl->posts)
		pfree(pl->posts);
	if (pl->raw)
		pfree(pl->raw);
	if (pl->arena)
		pfree(pl->arena);
	pl->posts = NULL;
	pl->raw = NULL;
	pl->arena = NULL;
	pl->nposts = 0;
}

/* find the PosPosting for docid via binary search; NULL if absent */
static PosPosting *
weave_pospost_find(PosTermList *pl, uint64 docid)
{
	int			lo = 0,
				hi = pl->nposts - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (pl->posts[mid].docid < docid)
			lo = mid + 1;
		else if (pl->posts[mid].docid > docid)
			hi = mid - 1;
		else
			return &pl->posts[mid];
	}
	return NULL;
}

/*
 * Evaluate a pure phrase chain positionally over one segment, appending exact
 * matches to *out (a growable TidSet-like buffer).  Returns true on success;
 * false means "fall back to recheck" (a term lacked positions).  seg_tombs/s
 * are used to drop tombstoned docids from this segment's contribution.
 */
static bool
weave_phrase_eval_seg(Relation index, const WeaveSegMeta *seg, WeaveQuery q,
					 const int *termidx, const uint32 *stepdist, int nterms,
					 ItemPointerData **tids, int *ntids, int *captids)
{
	PosTermList *tl = (PosTermList *) palloc0(nterms * sizeof(PosTermList));	/* alloc-ok: nterms = query term count */
	int			t;
	int			base = -1;		/* index of the smallest-df (driving) term */
	PosPosting *driver;
	int			di;
	bool		ok = true;
	uint32	   *acc = (uint32 *) palloc(WEAVE_PHRASE_POSBUF * sizeof(uint32));
	uint32	   *tmp = (uint32 *) palloc(WEAVE_PHRASE_POSBUF * sizeof(uint32));

	for (t = 0; t < nterms; t++)
	{
		WeaveQueryItem *it = &q->items[termidx[t]];
		WeavePosLookup rc = weave_lookup_term_pos(index, seg,
												WEAVE_QUERY_ITEMTEXT(q, it),
												it->termlen, &tl[t]);

		if (rc == WEAVE_POSLOOKUP_NOPOS)
		{
			ok = false;			/* found but positions missing: fall back to recheck */
			goto done;
		}
		if (rc == WEAVE_POSLOOKUP_ABSENT)
			goto done;			/* term absent: phrase cannot match this segment */
		if (base < 0 || tl[t].nposts < tl[base].nposts)
			base = t;
	}

	/* drive the docid intersection from the smallest posting list */
	driver = tl[base].posts;
	for (di = 0; di < tl[base].nposts; di++)
	{
		uint64		docid = driver[di].docid;
		PosPosting *pp[WEAVE_QUERY_MAX_PHRASE_TERMS];
		bool		allpresent = true;
		int			nacc;
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();	/* per driver posting; posting lists in memory, no lock held */

		for (t = 0; t < nterms; t++)
		{
			pp[t] = (t == base) ? &driver[di] : weave_pospost_find(&tl[t], docid);
			if (pp[t] == NULL)
			{
				allpresent = false;
				break;
			}
		}
		if (!allpresent)
			continue;

		/* chain phrase_step across the terms: acc starts as term 0's positions */
		if (pp[0]->npos > WEAVE_PHRASE_POSBUF)
		{
			ok = false;			/* pathological tf: fall back (bounded buffer) */
			goto done;
		}
		memcpy(acc, pp[0]->pos, pp[0]->npos * sizeof(uint32));
		nacc = pp[0]->npos;
		for (t = 1; t < nterms && nacc > 0; t++)
		{
			int			nout = 0;

			if (pp[t]->npos > WEAVE_PHRASE_POSBUF)
			{
				ok = false;
				goto done;
			}
			weave_phrase_step_pos(acc, nacc, pp[t]->pos, pp[t]->npos,
								stepdist[t - 1], tmp, &nout);
			memcpy(acc, tmp, nout * sizeof(uint32));
			nacc = nout;
		}
		if (nacc > 0)
		{
			weave_docid_to_tid(docid, &tid);
			if (*ntids >= *captids)
			{
				*captids = Max(*captids * 2, 16);
				*tids = repalloc(*tids, (Size) *captids * sizeof(ItemPointerData));
			}
			(*tids)[(*ntids)++] = tid;
		}
	}

done:
	for (t = 0; t < nterms; t++)
		weave_free_posterm(&tl[t]);
	pfree(tl);
	pfree(acc);
	pfree(tmp);
	return ok;
}

/*
 * weave_collect_matches: evaluate the scan's query across all segments + the
 * pending list; return matching TIDs (sorted, unique) and a *recheck flag
 * (true iff any term used the over-generating trigram funnel / regex / NOT-
 * universe path).  Shared by the bitmap scan and the plain gettuple scan.
 */
static void
weave_collect_matches(Relation index, WeaveQuery query, TidSet *out, bool *recheck)
{
	WeaveMetaPageData meta;
	TidSet		acc;
	TidSet		pending_acc;
	WeaveTombstones seg_tombs;
	bool		has_fuzzy_regex = false;
	bool		has_not = false;
	bool		has_phrase = false;
	bool		has_weighted = false;
	bool		need_recheck = false;
	bool		use_pos_phrase = false;	/* positional phrase fast path applies */
	int			pterm[WEAVE_QUERY_MAX_PHRASE_TERMS];
	uint32		pstep[WEAVE_QUERY_MAX_PHRASE_TERMS] = {0};
	int			npterm = 0;
	ItemPointerData *ptids = NULL;
	int			nptids = 0;
	int			captids = 0;
	uint32		i;
	uint32		s;
	uint32		gen0;			/* directory generation at the metapage snapshot */
	int			gen_retries = 0;

	acc.tids = NULL;
	acc.n = 0;
	*recheck = false;
	if (query == NULL)
	{
		*out = acc;
		return;
	}

	weave_read_meta(index, &meta);

#ifdef WEAVE_TEST_HOOKS
	/*
	 * TEST-ONLY window: after snapshotting the segment directory but before
	 * reading any segment page, briefly acquire+release the configured advisory
	 * lock.  A blocker session holding pg_advisory_lock(key) stalls the scan
	 * here while a merger frees the snapshotted segment and an inserter recycles
	 * its pages -- exposing the A1 scan-vs-merge recycle race deterministically.
	 * No effect when the key is 0 (default / production).
	 */
	if (pg_weave_test_pause_advisory_key != 0)
	{
		LOCKTAG		tag;
		int64		k = pg_weave_test_pause_advisory_key;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId,
							 (uint32) (k >> 32), (uint32) k, 1);
		(void) LockAcquire(&tag, ShareLock, true, false);
		LockRelease(&tag, ShareLock, true);
	}
#endif

collect_retry:
	gen0 = meta.generation;
	acc.tids = NULL;
	acc.n = 0;

	for (i = 0; i < query->nitems; i++)
	{
		WeaveQueryItem *it = &query->items[i];

		if (it->type == WEAVE_QI_OPR && it->op == WEAVE_OP_NOT)
			has_not = true;
		if (it->type == WEAVE_QI_OPR && it->op == WEAVE_OP_PHRASE)
			has_phrase = true;
		if (it->type == WEAVE_QI_VAL && (it->flags & (WEAVE_QF_FUZZY | WEAVE_QF_REGEX)))
			has_fuzzy_regex = true;
		if (it->type == WEAVE_QI_VAL && (it->flags & WEAVE_QF_WEIGHTED))
			has_weighted = true;
	}

	/*
	 * A weight-restricted (term:LABEL) query cannot be answered from the index
	 * alone -- the posting positions carry only the ordinal, not the field
	 * label (labels live in the heap wdoc value).  So the index over-generates
	 * (every doc containing the term) and the heap recheck applies the label
	 * filter, exactly as it does for fuzzy/regex.  Force recheck.
	 */
	if (has_weighted)
		need_recheck = true;

	/*
	 * Positional phrase fast path: if the index carries token positions
	 * (WITH positions=on) and the query is a pure phrase chain, evaluate it
	 * DIRECTLY from the posting lists -- intersect on docid, verify adjacency
	 * from the stored positions -- with NO heap access and need_recheck=false.
	 * This is the cliff fix.  When positions are off, or the phrase mixes with
	 * boolean operators, we keep the AND + heap-recheck path below (correct,
	 * slower).
	 */
	if (has_phrase && !has_fuzzy_regex && !has_not && !has_weighted &&
		weave_index_wants_positions(index) &&
		weave_phrase_chain(query, pterm, pstep, &npterm))
		use_pos_phrase = true;

	/*
	 * Load per-segment tombstones once.  Each segment's match contribution is
	 * filtered against THAT segment's own tombstone map before being unioned,
	 * because a heap TID deleted in one segment may be reused by a live doc in
	 * another segment or the pending list.
	 */
	weave_tombstones_load(index, &meta, &seg_tombs);

	for (s = 0; s < meta.nsegments; s++)
	{
		WeaveSegMeta *sg = &meta.segs[s];
		TidSet		universe;			/* only for the fuzzy/regex fallback below */
		UniverseSrc uni;				/* lazy source for the boolean evaluator */

		CHECK_FOR_INTERRUPTS();	/* per segment; no lock/window held (meta is in memory) */
		if (sg->dictstart == InvalidBlockNumber)
			continue;

		if (has_fuzzy_regex)
		{
			TidSet		cands;
			bool		any_trgm = false;
			bool		exact = (query->nitems == 1);
			uint32		qi;

			cands.tids = NULL;
			cands.n = 0;
			for (qi = 0; qi < query->nitems; qi++)
			{
				WeaveQueryItem *it = &query->items[qi];
				TidSet		ts;

				if (it->type != WEAVE_QI_VAL ||
					!(it->flags & (WEAVE_QF_FUZZY | WEAVE_QF_REGEX)))
					continue;

				if (it->flags & WEAVE_QF_FUZZY)
				{
					if (weave_fuzzy_terms(index, sg,
										 WEAVE_QUERY_ITEMTEXT(query, it),
										 it->termlen, (int) it->distance, &ts))
					{
						cands = tidset_or(cands, ts);
						any_trgm = true;
						continue;
					}
				}
				exact = false;
				if (weave_trgm_candidates(index, sg->trgmstart,
										 sg->dictstart,
										 WEAVE_QUERY_ITEMTEXT(query, it),
										 it->termlen, 3,
										 (it->flags & WEAVE_QF_REGEX) != 0,
										 sg->doclenstart == InvalidBlockNumber, &ts))
				{
					cands = tidset_or(cands, ts);
					any_trgm = true;
				}
				else
				{
					any_trgm = false;
					break;
				}
			}
			if (any_trgm)
			{
				if (!exact)
					need_recheck = true;
				if (cands.n > 0)
				{
					weave_filter_tombstoned_seg(&seg_tombs, s, &cands);
					if (cands.n > 0)
						acc = tidset_or(acc, cands);
				}
			}
			else
			{
				/*
				 * No trigram acceleration for this fuzzy/regex term (index built
				 * without trigrams, or the pattern is too short to yield the
				 * minimum trigrams).  We fall back to rechecking every document
				 * in the segment against the pattern -- correct, just slower, and
				 * the documented behavior of a trigrams-off index (see the
				 * "trigrams reloption" regression test).  weave_universe_bounded
				 * folds duplicates as it goes so the candidate scratch stays
				 * O(ndocs); the older unbounded collect grew to Sum(df) and could
				 * reach a multi-gigabyte "invalid memory alloc request size" on a
				 * high-vocabulary corpus.
				 */
				need_recheck = true;
				universe = weave_universe_bounded(index, sg->dictstart, sg->ndocs,
												 sg->doclenstart == InvalidBlockNumber);
				if (universe.n > 0)
				{
					weave_filter_tombstoned_seg(&seg_tombs, s, &universe);
					if (universe.n > 0)
						acc = tidset_or(acc, universe);
				}
			}
			continue;
		}

		/*
		 * Do NOT build the universe here.  It is handed to the evaluator as a
		 * lazy source and materialised only if the top-level result is still
		 * negated after De Morgan -- see the comment on weave_eval_query.  The
		 * eager build cost 603.9 ms on `common & !rare` at 200k rows for a set
		 * that was then discarded.
		 */
		uni.index = index;
		uni.dictstart = sg->dictstart;
		uni.ndocs = sg->ndocs;
		uni.has_doclen_col = (sg->doclenstart == InvalidBlockNumber);
		uni.built = false;
		uni.set.tids = NULL;
		uni.set.n = 0;
		universe.tids = NULL;
		universe.n = 0;

		if (use_pos_phrase)
		{
			/* evaluate the phrase from this segment's positional postings; the
			 * matched TIDs accumulate in ptids across segments, and are folded
			 * into acc after the loop.  A false return means a term lacked
			 * positions (a rare page-overflow block) -- abandon the fast path
			 * and fall back to the AND + recheck path for correctness. */
			int			seg_start = nptids;

			if (ptids == NULL)
			{
				captids = 64;
				ptids = (ItemPointerData *) palloc(captids * sizeof(ItemPointerData));
			}
			if (!weave_phrase_eval_seg(index, sg, query, pterm, pstep, npterm,
									  &ptids, &nptids, &captids))
			{
				/* fall back: restart collection from scratch via the AND path */
				use_pos_phrase = false;
				if (ptids)
				{
					pfree(ptids);
					ptids = NULL;
				}
				nptids = 0;
				if (acc.tids)
				{
					pfree(acc.tids);
					acc.tids = NULL;
				}
				acc.n = 0;
				s = (uint32) -1;	/* restart the segment loop (s++ -> 0) */
				continue;
			}
			/* filter ONLY this segment's new hits (ptids[seg_start..nptids])
			 * against THIS segment's tombstones -- they are docid-ascending (the
			 * driver posting list is docid-sorted).  Prior segments' hits were
			 * already filtered against their own maps. */
			if (nptids > seg_start)
			{
				TidSet		phr;

				phr.tids = ptids + seg_start;
				phr.n = nptids - seg_start;
				weave_filter_tombstoned_seg(&seg_tombs, s, &phr);
				nptids = seg_start + phr.n;
			}
			continue;
		}

		{
			TidSet		result = weave_eval_query(index,
												 sg, query, &uni);

			if (result.n > 0)
			{
				weave_filter_tombstoned_seg(&seg_tombs, s, &result);
				if (result.n > 0)
				{
					/* PHRASE/NEAR is evaluated as AND here (positions=off or a
					 * non-pure-phrase query); the heap wdoc carries positions,
					 * so a heap recheck of @@@ enforces adjacency exactly. */
					if (has_phrase)
						need_recheck = true;
					acc = tidset_or(acc, result);
				}
			}
		}
	}

	/* fold in the positional-phrase matches (already tombstone-filtered per
	 * segment); need_recheck stays false -- the positions gave the exact set.
	 * ptids is per-segment-ascending but not globally sorted (segments overlap
	 * in docid range), so sort+uniq before the merge. */
	if (use_pos_phrase && nptids > 0)
	{
		TidSet		phr;

		phr.tids = ptids;
		phr.n = nptids;
		tidset_sort_uniq(&phr);
		acc = tidset_or(acc, phr);
	}
	if (ptids)
		pfree(ptids);

	/* pending list: verbatim docs matched by the exact per-doc matcher.
	 * Collect these separately from the segment matches: a pending doc is a
	 * live heap tuple that was just inserted, so it must NOT be subjected to
	 * the segment tombstone filter below -- a reused heap slot (same TID as a
	 * previously deleted, tombstoned doc) would otherwise be wrongly dropped. */
	pending_acc.tids = NULL;
	pending_acc.n = 0;
	if (meta.pendinghead != InvalidBlockNumber)
	{
		BlockNumber blk = meta.pendinghead;

		while (blk != InvalidBlockNumber)
		{
			Buffer		buffer;
			Page		page;
			char	   *ptr,
					   *end;
			BlockNumber next;

			CHECK_FOR_INTERRUPTS();	/* between pages, no buffer lock held: safe to let a cancel unwind */
			buffer = weave_scan_readbuf(index, blk);
			if (buffer == InvalidBuffer)
				break;		/* block truncated by a concurrent weave_vacuum: end of chain */
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buffer);
			ptr = (char *) PageGetContents(page);
			end = (char *) page + ((PageHeader) page)->pd_lower;
			next = WeavePageGetOpaque(page)->nextblk;

			while (ptr < end)
			{
				WeavePendingItem *pi = (WeavePendingItem *) ptr;
				WeaveDoc		pdoc;

				/*
				 * Bounds-guard the pending item before trusting pi->doclen.
				 * The pending list is read under only BUFFER_LOCK_SHARE, and a
				 * concurrent flush (INSERT pending-buffer -> segment, VACUUM,
				 * weave_merge) clears the list and frees these pages, which a
				 * concurrent insert can recycle and overwrite (pg_weave recycles
				 * freed pages with no deletion-xid gate).  A scan that snapshotted
				 * pendinghead before that then walks a recycled page whose
				 * pi->doclen is arbitrary; without this guard weave_doc_is_valid /
				 * weave_doc_matches read out of bounds and the ptr advance runs off
				 * the page (observed as a wild multi-gigabyte allocation / crash
				 * under concurrent merge + ingestion).  If the header or the
				 * doclen-sized body does not fit the page, stop the page walk; the
				 * scan's generation re-check then detects the stale read and
				 * restarts.
				 */
				if ((char *) pi + sizeof(WeavePendingItem) > end ||
					(char *) pi + MAXALIGN(sizeof(WeavePendingItem) + (Size) pi->doclen) > end)
					break;
				pdoc = (WeaveDoc) ((char *) pi + sizeof(WeavePendingItem));

				/* A pending doc is raw page bytes; validate before the matcher
				 * walks its offsets, so a torn/corrupt page cannot segfault a
				 * SELECT.  A malformed doc is simply not matched (and flagged). */
				if (!weave_doc_is_valid(pdoc, pi->doclen))
					ereport(WARNING,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("pg_weave: skipping malformed pending document in index \"%s\" during scan",
									RelationGetRelationName(index)),
							 errhint("REINDEX the index to rebuild it from the heap.")));
				else if (weave_doc_matches(pdoc, query))
				{
					TidSet		one;

					one.tids = &pi->tid;
					one.n = 1;
					pending_acc = tidset_or(pending_acc, one);	/* exact per-doc match */				}
				ptr += MAXALIGN(sizeof(WeavePendingItem) + pi->doclen);
			}
			UnlockReleaseBuffer(buffer);
			blk = next;
		}
	}

	tidset_sort_uniq(&acc);
	/*
	 * Segment matches were already filtered against each segment's own
	 * tombstone map above; pending matches are live tuples and are never
	 * tombstoned.  Just release the loaded maps.
	 */
	weave_tombstones_free(&seg_tombs);
	/* fold in the (unfiltered) pending matches and re-uniq */
	if (pending_acc.n > 0)
	{
		acc = tidset_or(acc, pending_acc);
		tidset_sort_uniq(&acc);
	}
	/*
	 * Concurrency guard: if the segment directory changed while we were reading
	 * (a concurrent merge/vacuum/bulkdelete may have freed + recycled pages we
	 * read from a now-stale segment descriptor), our result may be a stale read.
	 * Discard it and redo from a fresh snapshot.  Bounded so a pathological
	 * merge storm can't spin forever; after the cap we proceed with the last
	 * result (still no worse than the pre-fix behavior, and merges are rare
	 * relative to a scan).
	 */
	if (weave_read_meta_generation(index) != gen0 && gen_retries++ < 10)
	{
		if (acc.tids)
			pfree(acc.tids);
		if (pending_acc.tids)
			pfree(pending_acc.tids);
		if (ptids)
			pfree(ptids);
		ptids = NULL;
		nptids = 0;
		captids = 0;
		need_recheck = false;
		weave_tombstones_free(&seg_tombs);
		weave_read_meta(index, &meta);
		goto collect_retry;
	}

	*out = acc;
	*recheck = need_recheck;
}

int64
weave_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	WeaveScanOpaque so = (WeaveScanOpaque) scan->opaque;
	TidSet		matches;
	bool		recheck;

	/*
	 * amoptionalkey is true (see weave_handler), so the planner may hand us a
	 * scan with no restriction clause.  That is intended: it is how the keyless
	 * ordering scan works, and weave_rescan takes the query from the order-by
	 * argument in that case.
	 *
	 * But a scan with NEITHER a scan key NOR an order-by has no query at all,
	 * and there is nothing correct to return.  Silently returning zero rows
	 * would make such a plan produce an empty result set -- a wrong answer, not
	 * a slow one -- so fail loudly.  Reachable via a partial index, where a
	 * useful predicate alone can justify a path.
	 */
	if (scan->numberOfKeys == 0 && scan->numberOfOrderBys == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a weave index scan requires a query"),
				 errdetail("The scan has neither a @@@ restriction nor an ORDER BY <=> ordering clause."),
				 errhint("Add \"WHERE col @@@ query\" or \"ORDER BY col <=> query\".")));

	if (!so->queryValid || so->query == NULL)
		return 0;
	/* Count the index scan for pg_stat_user_indexes.idx_scan; idx_tup_read is
	 * added by index_getbitmap() from our return value. */
	pgstat_count_index_scan(scan->indexRelation);
	weave_collect_matches(scan->indexRelation, so->query, &matches, &recheck);
	if (matches.n > 0)
		tbm_add_tuples(tbm, matches.tids, matches.n, recheck);
	return matches.n;
}

/*
 * weave_recheck_exact: shrink `set` to the EXACT @@@ match set.
 *
 * weave_collect_matches returns recheck=true for queries the index over-
 * generates (PHRASE/NEAR: adjacency not enforced by the positionless posting
 * lists; FUZZY/REGEX: the trigram funnel yields candidates).  The bitmap-heap
 * scan hands these to the executor with a recheck flag so it re-evaluates @@@
 * against the heap wdoc.  The ranked <=> scan and weave_count() have no
 * executor recheck, so they must do it here: recompute the indexed wdoc from
 * each candidate's live heap tuple (evaluating the index expression, exactly
 * as build/insert do) and drop any that fail weave_doc_matches.  After this the
 * TID set is precisely what "WHERE d @@@ q" (with heap recheck) admits.
 *
 * Non-live tuples (not visible / vacuumed) are dropped too; the caller's own
 * MVCC visibility pass would drop them anyway, so this never widens the set.
 */
static void
weave_recheck_exact(Relation index, WeaveQuery query, TidSet *set)
{
	Relation	heap;
	IndexInfo  *indexInfo;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	Snapshot	snap = GetActiveSnapshot();
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			i,
				keep = 0;

	if (set->n == 0)
		return;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	indexInfo = BuildIndexInfo(index);
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(heap, NULL);
	econtext->ecxt_scantuple = slot;
#if PG_VERSION_NUM >= 190000
	fetch = table_index_fetch_begin(heap, SO_NONE);
#else
	fetch = table_index_fetch_begin(heap);
#endif

	for (i = 0; i < set->n; i++)
	{
		ItemPointerData tid = set->tids[i];
		bool		call_again = false;
		bool		all_dead = false;

		CHECK_FOR_INTERRUPTS();	/* per candidate; no index buffer lock held (heap fetch self-manages) */
		ExecClearTuple(slot);
		if (table_index_fetch_tuple(fetch, &tid, snap, slot,
									&call_again, &all_dead))
		{
			WeaveDoc		doc;

			FormIndexDatum(indexInfo, slot, estate, values, isnull);
			if (!isnull[0])
			{
				doc = (WeaveDoc) PG_DETOAST_DATUM(values[0]);
				if (weave_doc_matches(doc, query))
					set->tids[keep++] = set->tids[i];
			}
		}
		ResetExprContext(econtext);
	}

	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	table_close(heap, AccessShareLock);
	set->n = keep;
}

void
weave_endscan(IndexScanDesc scan)
{
	/* memory is freed with the scan's context */
}

/* ----- index-maintained corpus statistics (stage 5) ----- */

/*
 * Look up a term's dictionary entry (df, max_tf, first posting block) without
 * reading any postings.  Returns true if found.  This is what the lazy WAND
 * cursors need to start; postings are then paged in on demand.
 */
static bool
weave_lookup_dict(Relation index, const WeaveSegMeta *seg,
				 const char *term, int termlen,
				 uint32 *df, uint32 *max_tf, BlockNumber *firstposting,
				 uint32 *firstoffset)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;			/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				*df = de->df;
				*max_tf = de->max_tf;
				*firstposting = de->firstposting;
				*firstoffset = de->firstoffset;
				found = true;
				break;
			}
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		if (found)
			return true;
		if (onlyone)
			break;				/* block index located the only possible page */
		blk = next;
	}
	*df = 0;
	*max_tf = 0;
	*firstposting = InvalidBlockNumber;
	*firstoffset = 0;
	return false;
}

/* Look up the document frequency of a term in the index, 0 if absent. */
static uint32
weave_lookup_df(Relation index, const WeaveSegMeta *seg,
			   const char *term, int termlen)
{
	BlockNumber blk = weave_dict_seek(index, seg, term, termlen);
	bool		onlyone = (seg->dictindexstart != InvalidBlockNumber);

	while (blk != InvalidBlockNumber)
	{
		Buffer		buffer;
		Page		page;
		char	   *ptr,
				   *end;
		BlockNumber next;
		uint32		df = 0;
		bool		found = false;

		CHECK_FOR_INTERRUPTS();		/* between pages, no buffer lock held: safe to let a cancel unwind */
		buffer = weave_scan_readbuf(index, blk);
		if (buffer == InvalidBuffer)
			break;			/* block truncated by a concurrent weave_vacuum: end of chain */
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		ptr = (char *) PageGetContents(page);
		end = (char *) page + ((PageHeader) page)->pd_lower;
		next = WeavePageGetOpaque(page)->nextblk;

		while (ptr < end)
		{
			WeaveDictEntry *de = (WeaveDictEntry *) ptr;
			Size		esize;

			if (!weave_dict_entry_fits(de, end))
				break;		/* recycled/corrupt page: stop (see weave_dict_entry_fits) */
			esize = MAXALIGN(offsetof(WeaveDictEntry, term) + de->termlen);

			if ((int) de->termlen == termlen &&
				memcmp(de->term, term, termlen) == 0)
			{
				df = de->df;
				found = true;
				break;
			}
			ptr += esize;
		}
		UnlockReleaseBuffer(buffer);
		if (found)
			return df;
		if (onlyone)
			break;
		blk = next;
	}
	return 0;
}

PG_FUNCTION_INFO_V1(weave_index_nsegments);

/* weave_index_nsegments(regclass) -> int : number of live segments.
 * Works on an in-progress (indisvalid=f) index so a build can be polled; returns
 * NULL if the metapage is not yet a valid pg_weave metapage (very early in a build
 * or a non-weave relation) rather than erroring, so a monitoring query is safe. */
Datum
weave_index_nsegments(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	WeaveMetaPageData meta;
	bool		ok = false;

	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam == get_index_am_oid("weave", true) &&
		RelationGetNumberOfBlocks(index) > WEAVE_METAPAGE_BLKNO)
	{
		Buffer		buf = ReadBuffer(index, WEAVE_METAPAGE_BLKNO);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		if (WeavePageGetMeta(BufferGetPage(buf))->magic == WEAVE_MAGIC)
		{
			memcpy(&meta, WeavePageGetMeta(BufferGetPage(buf)), sizeof(meta));
			ok = true;
		}
		UnlockReleaseBuffer(buf);
	}
	index_close(index, AccessShareLock);
	if (!ok)
		PG_RETURN_NULL();
	PG_RETURN_INT32((int32) meta.nsegments);
}

PG_FUNCTION_INFO_V1(weave_index_stats);

/* weave_index_stats(regclass) -> (ndocs float8, avgdl float8, nterms bigint) */
Datum
weave_index_stats(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	index;
	WeaveMetaPageData meta;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	index = index_open(indexoid, AccessShareLock);
	if (index->rd_rel->relam != get_index_am_oid("weave", true))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a weave index",
						RelationGetRelationName(index))));
	weave_read_meta(index, &meta);
	index_close(index, AccessShareLock);

	values[0] = Float8GetDatum(meta.ndocs);
	values[1] = Float8GetDatum(meta.ndocs > 0 ?
							   meta.sumdoclen / meta.ndocs : 0.0);
	{
		uint32		s;
		int64		nterms = 0;

		for (s = 0; s < meta.nsegments; s++)
			nterms += meta.segs[s].nterms;
		values[2] = Int64GetDatum(nterms);	/* bigint: no int32 wrap at scale */
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

PG_FUNCTION_INFO_V1(weave_index_df);

/* weave_index_df(regclass, wquery) -> float8[] of df per distinct query term */
Datum
weave_index_df(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	WeaveQuery	q = PG_GETARG_WQUERY(1);
	Relation	index;
	WeaveMetaPageData meta;
	Datum	   *elems;
	int			n = 0;
	uint32		i;
	ArrayType  *result;

	index = index_open(indexoid, AccessShareLock);
	weave_read_meta(index, &meta);

	elems = (Datum *) palloc(q->nitems * sizeof(Datum));
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			uint64		df = 0;		/* summed across segments; uint64 so a term in
									 * >2^32 docs does not wrap (consumed as double) */
			uint32		s;

			/* document frequency is summed across all segments */
			for (s = 0; s < meta.nsegments; s++)
				df += weave_lookup_df(index, &meta.segs[s],
									 WEAVE_QUERY_ITEMTEXT(q, it), it->termlen);
			elems[n++] = Float8GetDatum((double) (df == 0 ? 1 : df));
		}
	}
	index_close(index, AccessShareLock);

	result = construct_array(elems, n, FLOAT8OID, 8, true, 'd');
	PG_FREE_IF_COPY(q, 1);
	PG_RETURN_ARRAYTYPE_P(result);
}

/* ----- index-only scored top-K search (WAND-style) ----- */

#include "funcapi.h"
#include "access/htup_details.h"
#include "utils/builtins.h"

/*
 * weave_search(index regclass, query wquery, k int)
 *   -> setof (ctid tid, score float8)
 *
 * Index-only BM25 top-k: scores are computed entirely from the index (postings
 * give per-doc tf, the dictionary gives df and the max-tf impact bound, the
 * metapage gives N and avgdl) with no heap access.  A WAND-style upper-bound
 * check on each document's best possible score prunes documents that cannot
 * enter the current top-k, which is the early-termination win.
 *
 * Cursors load posting pages lazily and use each page's block-max_tf (stored in
 * the page opaque) to skip entire pages whose best possible contribution cannot
 * beat the current top-k threshold -- block-max WAND -- so most of a long
 * posting list is never decoded.  Per-document |D| is read from the postings
 * for exact BM25 length normalization.
 */
/* ----- document-at-a-time block-max WAND top-k (item 2) ----- */

static int
cmp_scored_desc(const void *a, const void *b)
{
	double		sa = ((const ScoredTid *) a)->score;
	double		sb = ((const ScoredTid *) b)->score;

	if (sa < sb)
		return 1;
	if (sa > sb)
		return -1;
	return 0;
}

/*
 * A per-term cursor for the WAND merge.  posts is the term's docid-sorted
 * posting list; cursors load posting pages lazily from the index and skip
 * whole pages via the page block-max when they cannot beat the threshold.
 */
typedef struct WandCursor
{
	Relation	index;
	BlockNumber curblk;			/* page holding the current block */
	uint32		curoff;			/* byte offset of the CURRENT block on curblk */
	int			nread;			/* postings consumed so far (stop at df) */
	BlockNumber firstblk;		/* first posting block for the term */
	uint32		firstoff;		/* byte offset of the term's first block */
	uint32		df;				/* term document frequency (postings to read) */
	int			termidx;		/* ordinal VAL/term index (for the BoolGate) */

	/*
	 * Current block only, decoded LAZILY: docids are unpacked eagerly (needed
	 * to pivot/skip), but tf and doclen stay bit-packed in blkbuf and are
	 * extracted per-posting on demand (weave_for_get) only when a posting is
	 * actually scored -- so blocks pruned by block-max never pay for tf/dl.
	 */
	unsigned char *blkbuf;		/* copy of the current block's FOR payload */
	uint64		docids[WEAVE_BLOCK_SIZE];	/* decoded docids of current block */
	uint32		tfoff;			/* offset of tf column within blkbuf */
	uint32		dloff;			/* offset of doclen column within blkbuf (v3 only) */
	bool		has_doclen_col;	/* v3 segment: doclen inline in the block (read at
								 * dloff).  v4 segment: false -- doclen comes from
								 * the doclen sidecar cursor. */
	WeaveDoclenCursor doclenc;	/* v4: forward-cursored sidecar reader (reads only
								 * the pages covering scored docids, not the whole
								 * segment sidecar up front). */
	int			blkcount;		/* postings in the current block */
	uint32		blk_max_tf;		/* block-max tf (from header) */
	uint32		blk_min_dl;		/* block-min |D| (from header) */
	int			cur;			/* index within the current block */
	uint64		docid;			/* current docid (UINT64_MAX = exhausted) */

	double		idf;
	double		avgdl;
	double		k1b_inv_avgdl;	/* precomputed k1*b/avgdl (norm hot path) */
	double		k1_1mb;			/* precomputed k1*(1-b) */
	double		idf_k1p1;		/* precomputed idf*(k1+1) */
	double		max_contrib;	/* term-wide upper bound (shortest-doc norm) */
	WeaveTombstones *tombs;		/* loaded per-segment tombstones (or NULL) */
	uint32		segidx;			/* which segment this cursor's postings belong to */
	sm_cursor_t tombcursor;		/* forward-resume cursor for tombstone lookups.
								 * The WAND scan visits this cursor's docids in
								 * monotonically non-decreasing order and the
								 * tombstone map is read-only for the scan's
								 * lifetime, so a single-chunk resume cursor turns
								 * the otherwise O(postings x chunks) head-walk
								 * into O(postings + chunks).  (An 8-way MRU cache
								 * degenerates to a per-lookup head-walk once an
								 * ascending scan runs past its 8 cached chunks --
								 * the cause of the tombstone-bloat blowup.) */
	/*
	 * Optional docid range [docid_lo, docid_hi) for a parallel worker's slice.
	 * docid_lo = 0 and docid_hi = UINT64_MAX means "whole term" (the serial
	 * path).  The cursor seeks to docid_lo at prime time and reports itself
	 * exhausted (docid = UINT64_MAX) once it reaches docid_hi, so the WAND/
	 * MaxScore loops need no range awareness -- they already stop when every
	 * cursor is exhausted.  Ranges partition the corpus disjointly across
	 * workers, so each worker scores a disjoint candidate set exactly.
	 */
	uint64		docid_lo;
	uint64		docid_hi;
}			WandCursor;

static inline void wand_skip_own_tombstoned(WandCursor *c);
static void wand_seek(WandCursor *c, uint64 target);

static inline uint64
tid_to_docid_s(ItemPointer tid)
{
	return (uint64) ItemPointerGetBlockNumber(tid) *
		(uint64) MaxHeapTuplesPerPage +
		(uint64) ItemPointerGetOffsetNumber(tid);
}

/*
 * Lazily load the next page-worth of THIS TERM's postings into the cursor.
 * Decodes blocks starting at (c->curblk, c->curoff) until the page ends or the
 * term's df is exhausted, remembering where to resume (curblk/curoff) so a huge
 * term is streamed a page at a time -- WAND/BMW can then skip most of it without
 * ever decoding it (the whole point of block-max WAND).
 */
static void
wand_load_block(WandCursor *c)
{
	Buffer		buf;
	Page		page;
	char	   *p,
			   *pend;
	WeaveBlockHdr *bh;
	const unsigned char *stream;
	uint64		gaps[WEAVE_BLOCK_SIZE];
	uint64		base;
	int			cnt;
	int			glen;
	int			tflen;
	int			i;

	if (c->blkbuf)
	{
		pfree(c->blkbuf);
		c->blkbuf = NULL;
	}
	if (c->curblk == InvalidBlockNumber || c->nread >= (int) c->df)
	{
		c->blkcount = 0;
		c->cur = 0;
		c->docid = UINT64_MAX;
		return;
	}

	buf = ReadBuffer(c->index, c->curblk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	pend = (char *) page + ((PageHeader) page)->pd_lower;
	p = (char *) page + c->curoff;

	/* skip any empty tail; advance across pages until a real block or EOF */
	while (!(p + sizeof(WeaveBlockHdr) <= pend) ||
		   ((WeaveBlockHdr *) p)->count == 0)
	{
		BlockNumber next = WeavePageGetOpaque(page)->nextblk;

		UnlockReleaseBuffer(buf);
		if (next == InvalidBlockNumber)
		{
			c->curblk = InvalidBlockNumber;
			c->blkcount = 0;
			c->cur = 0;
			c->docid = UINT64_MAX;
			return;
		}
		c->curblk = next;
		c->curoff = MAXALIGN(SizeOfPageHeaderData);
		CHECK_FOR_INTERRUPTS();	/* buffer already released above, next not yet read: no lock held */
		buf = ReadBuffer(c->index, c->curblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		pend = (char *) page + ((PageHeader) page)->pd_lower;
		p = (char *) page + c->curoff;
	}

	bh = (WeaveBlockHdr *) p;
	stream = (const unsigned char *) (bh + 1);
	cnt = (int) bh->count;
	if (bh->count == 0 || bh->count > (uint32) WEAVE_BLOCK_SIZE)
		cnt = WEAVE_BLOCK_SIZE;	/* defensive: uint32 count, guard both ends */

	/*
	 * Validate the block payload fits within the page BEFORE trusting bytelen.
	 * This page is pinned only BUFFER_LOCK_SHARE; a scan can run concurrently
	 * with a merge/vacuum that frees this segment's pages and a concurrent
	 * insert/flush that recycles and OVERWRITES the freed block (pg_weave recycles
	 * freed pages with no deletion-xid gate).  If that happened between the
	 * segment-directory snapshot and this read, bh->bytelen/posbytelen are
	 * whatever bytes now occupy the page -- e.g. a multi-gigabyte length, which
	 * turned palloc(bh->bytelen) into "invalid memory alloc request size" and
	 * the following memcpy/decode into an out-of-bounds read (SIGSEGV) under
	 * concurrent merge + ingestion.  A genuine block's payload always fits the
	 * page; if it does not, treat it as end-of-chain for this cursor (the
	 * scan's generation re-check catches the stale read and restarts).  Same
	 * bounded-wrong-result-not-a-crash contract as the other decode guards.
	 */
	if (stream + (Size) bh->bytelen + (Size) bh->posbytelen > (const unsigned char *) pend)
	{
		UnlockReleaseBuffer(buf);
		c->curblk = InvalidBlockNumber;
		c->blkcount = 0;
		c->cur = 0;
		c->docid = UINT64_MAX;
		return;
	}

	/* copy the block's FOR payload so tf/dl bytes stay valid after we unlock */
	c->blkbuf = (unsigned char *) palloc(bh->bytelen);
	memcpy(c->blkbuf, stream, bh->bytelen);

	/* eagerly decode ONLY docids (gaps); record tf/dl column offsets for lazy
	 * per-posting access -- pruned blocks never touch tf/dl */
	glen = weave_for_unpack(c->blkbuf, cnt, gaps);
	tflen = weave_for_bytelen(c->blkbuf + glen, cnt);
	c->tfoff = (uint32) glen;
	c->dloff = (uint32) (glen + tflen);	/* v3: doclen column follows tf; unused for v4 */

	base = ((uint64) bh->first_docid_hi << 32) | bh->first_docid_lo;
	for (i = 0; i < cnt; i++)
	{
		base += gaps[i];		/* first gap is 0 from first_docid */
		c->docids[i] = base;
	}
	c->blkcount = cnt;
	c->blk_max_tf = bh->max_tf;
	c->blk_min_dl = bh->min_doclen;
	c->cur = 0;
	c->docid = c->docids[0];
	/* advance the resume pointer to the next block (or next page).  Skip past
	 * BOTH the three FOR columns (bytelen) and the trailing positions column
	 * (posbytelen) -- the WAND scan never decodes positions, but it must step
	 * over them to find the next block's header. */
	{
		char	   *nextp = (char *) MAXALIGN((char *) (bh + 1) + bh->bytelen + bh->posbytelen);

		if (nextp + sizeof(WeaveBlockHdr) <= pend &&
			c->nread + cnt < (int) c->df)
			c->curoff = (uint32) (nextp - (char *) page);
		else
		{
			c->curblk = WeavePageGetOpaque(page)->nextblk;
			c->curoff = MAXALIGN(SizeOfPageHeaderData);
		}
	}
	c->nread += cnt;
	UnlockReleaseBuffer(buf);
}

/* Prime the cursor at the term's first block and load it. */
static void
wand_prime(WandCursor *c)
{
	c->blkbuf = NULL;
	c->curblk = c->firstblk;
	c->curoff = c->firstoff;
	c->nread = 0;
	if (c->firstblk == InvalidBlockNumber || c->df == 0)
	{
		c->blkcount = 0;
		c->cur = 0;
		c->docid = UINT64_MAX;
		return;
	}
	wand_load_block(c);
	/* seek to this cursor's docid range start (parallel worker slice); for the
	 * serial path docid_lo == 0 so this is a no-op */
	if (c->docid_lo > 0 && c->docid != UINT64_MAX)
		wand_seek(c, c->docid_lo);
	else
		wand_skip_own_tombstoned(c);
}

/* The block-max contribution upper bound for the current 128-block.
 * Uses the block's max_tf AND min |D|: impact is increasing in tf and
 * decreasing in |D|, so impact(max_tf, min_dl) is a sound (and much tighter
 * than the shortest-possible-doc) upper bound for every posting in the block.
 *
 * SOUNDNESS with the v4 doclen sidecar: scoring reads the QUANTIZED doclen
 * (weave_byte_to_doclen, which truncates -> the effective |D| is <= the exact
 * |D|, i.e. a doc can score HIGHER than its exact length implies).  The block
 * header stores the EXACT min |D| from build time; using it directly would make
 * the bound too small (a quantized-down doc could out-score the bound and be
 * wrongly skipped -- WAND unsoundness that misses true top-k docs on multi-term
 * AND).  So for a sidecar (v4) cursor we bound with the quantized-floor of the
 * block min |D|, matching the smallest effective |D| scoring can produce. */
static inline double
wand_block_max_contrib(WandCursor *c)
{
	double		k1 = 1.2;
	double		mtf = (double) c->blk_max_tf;
	uint32		mindl_raw = c->blk_min_dl;
	double		mindl = (double) (c->has_doclen_col
									 ? mindl_raw			/* v3: exact inline doclen */
									 : weave_byte_to_doclen(weave_doclen_to_byte(mindl_raw)));

	return c->idf * mtf * (k1 + 1.0) / (mtf + c->k1_1mb + c->k1b_inv_avgdl * mindl);
}

/* True if the cursor's CURRENT docid is tombstoned in the cursor's OWN
 * segment.  Tombstones are per-segment, so a cursor must ignore only its own
 * segment's deletions -- a reused heap TID that is live in another segment or
 * the pending list must still be produced by the segments that legitimately
 * contain it. */
static inline bool
wand_cur_own_tombstoned(WandCursor *c)
{
	if (c->tombs == NULL || !c->tombs->hasany || c->docid == UINT64_MAX)
		return false;
	if (c->segidx >= c->tombs->nseg || !c->tombs->present[c->segidx])
		return false;
	/* Forward-resume cursor: the WAND scan feeds this cursor docids in
	 * monotonically non-decreasing order, so sm_contains resumes the chunk
	 * walk from the last located chunk instead of head-walking from chunk 0.
	 * This is O(1) amortized even when the segment carries millions of
	 * tombstones (the tombstone-bloat pathology). */
	return sm_contains(&c->tombs->maps[c->segidx], c->docid,
					   &c->tombcursor);
}

/* After the current docid is (re)positioned, skip forward over any docids
 * deleted in this cursor's own segment.  Loads successive blocks as needed;
 * wand_load_block does not itself skip, so there is no recursion. */
static inline void
wand_skip_own_tombstoned(WandCursor *c)
{
	while (wand_cur_own_tombstoned(c))
	{
		c->cur++;
		if (c->cur < c->blkcount)
			c->docid = c->docids[c->cur];
		else
			wand_load_block(c);
	}
	/* enforce the worker's docid range upper bound: past it, this cursor is
	 * done (its slice ends before docid_hi; another worker owns the rest) */
	if (c->docid >= c->docid_hi)
		c->docid = UINT64_MAX;
}

/* Advance the cursor to the next posting, loading the next block if needed. */
static void
wand_next(WandCursor *c)
{
	c->cur++;
	if (c->cur < c->blkcount)
		c->docid = c->docids[c->cur];
	else
		wand_load_block(c);		/* stream the next block of this term */
	wand_skip_own_tombstoned(c);
}

/* Exact BM25 contribution of the current posting.  tf and |D| are extracted
 * from the block's still-packed FOR columns ON DEMAND (weave_for_get) -- only
 * for postings actually scored, so pruned blocks never decode tf/dl. */
static inline double
wand_contrib_cur(WandCursor *c)
{
	double		tf = (double) weave_for_get(c->blkbuf + c->tfoff, c->cur);
	double		dl = c->has_doclen_col
		? (double) weave_for_get(c->blkbuf + c->dloff, c->cur)	/* v3: inline */
		: (double) weave_doclen_cursor_lookup(&c->doclenc, c->docid);	/* v4: sidecar */
	double		norm = tf + c->k1_1mb + c->k1b_inv_avgdl * dl;

	return c->idf_k1p1 * tf / norm;
}

/*
 * Skip the cursor past the rest of its current 128-block: load the next block.
 * Sound to call ONLY when this cursor is the single one at/before the pivot
 * (its block is then a contiguous docid run that blocksum bounded whole, and no
 * other cursor holds a docid in the block's range).  The abandoned block never
 * had its tf/doclen decoded (block-max pruning pays only for docids), so this
 * is O(1) amortized and is what keeps common-term BMW fast.  Always makes
 * forward progress.
 */
static void
wand_skip_block(WandCursor *c)
{
	wand_load_block(c);
	wand_skip_own_tombstoned(c);
}

/*
 * Advance the cursor's paging state (curblk/curoff/nread) past whole 128-blocks
 * whose docids are all < target, reading only block HEADERS (no FOR decode).
 * A block is entirely below target when the NEXT block's first_docid <= target
 * (blocks are docid-ordered); the last block on a chain we cannot prove-skip
 * this way, so we stop and let the caller decode it.  This is what lets a seek
 * over a high-df term skip hundreds of thousands of postings without decoding.
 */
static void
wand_skip_blocks(WandCursor *c, uint64 target)
{
	while (c->curblk != InvalidBlockNumber && c->nread < (int) c->df)
	{
		Buffer		buf;
		Page		page;
		char	   *p,
				   *pend;
		BlockNumber nextblk;
		bool		stopped = false;

		CHECK_FOR_INTERRUPTS();	/* between posting-list pages, no buffer lock held: safe to unwind */
		buf = ReadBuffer(c->index, c->curblk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		pend = (char *) page + ((PageHeader) page)->pd_lower;
		nextblk = WeavePageGetOpaque(page)->nextblk;
		p = (char *) page + c->curoff;
		while (p + sizeof(WeaveBlockHdr) <= pend && c->nread < (int) c->df)
		{
			WeaveBlockHdr *bh = (WeaveBlockHdr *) p;
			char	   *nextp;

			if (bh->count == 0)
			{
				stopped = true;
				break;
			}
			nextp = (char *) MAXALIGN((char *) (bh + 1) + bh->bytelen + bh->posbytelen);
			/* can we prove this whole block is < target? need the next block's
			 * first_docid (on this page) to be <= target. */
			if (nextp + sizeof(WeaveBlockHdr) <= pend)
			{
				WeaveBlockHdr *nb = (WeaveBlockHdr *) nextp;
				uint64		nbfirst = ((uint64) nb->first_docid_hi << 32) | nb->first_docid_lo;

				if (nbfirst <= target)
				{
					/* whole block < target: skip it (headers only) */
					c->nread += (int) bh->count;
					c->curoff = (uint32) (nextp - (char *) page);
					p = nextp;
					continue;
				}
			}
			/* this block may contain target (or is the page's last block): stop
			 * so the caller decodes from here */
			stopped = true;
			break;
		}
		if (!stopped)
		{
			/* consumed all blocks on this page as skippable; move to next page */
			c->curblk = nextblk;
			c->curoff = MAXALIGN(SizeOfPageHeaderData);
			UnlockReleaseBuffer(buf);
			continue;
		}
		UnlockReleaseBuffer(buf);
		return;
	}
}

/* Advance a cursor to the first posting with docid >= target (or exhaust). */
static void
wand_seek(WandCursor *c, uint64 target)
{
	if (c->docid >= target)
	{
		wand_skip_own_tombstoned(c);
		return;
	}
	/* first, fast-forward within the current (already-decoded) block's docids */
	while (c->cur < c->blkcount && c->docids[c->cur] < target)
		c->cur++;
	if (c->cur < c->blkcount)
	{
		c->docid = c->docids[c->cur];
		wand_skip_own_tombstoned(c);
		return;
	}
	/* current block exhausted: skip whole undecoded blocks by header, then
	 * load the block containing target and land on it */
	wand_skip_blocks(c, target);
	for (;;)
	{
		wand_load_block(c);
		if (c->docid == UINT64_MAX)
			return;
		while (c->cur < c->blkcount && c->docids[c->cur] < target)
			c->cur++;
		if (c->cur < c->blkcount)
		{
			c->docid = c->docids[c->cur];
			wand_skip_own_tombstoned(c);
			return;
		}
		/* target beyond this block; loop to load/skip the next */
	}
}

/*
 * DocidFilter: an optional docid-membership gate for the ranked scan.
 *
 * A candidate doc may enter the top-k heap only if its docid is a member.
 * `docids` is sorted ascending (membership = binary search).  A NULL filter
 * means "admit everything" -- the pure-OR fast path, where the term
 * disjunction the WAND engine ranks IS the boolean match set, so no gating is
 * needed and there is zero overhead.  For any non-pure-OR query (AND/NOT/
 * PHRASE/prefix/fuzzy/regex) the filter carries the exact @@@ boolean match
 * set (from weave_collect_matches), so the ranked traversal -- otherwise
 * disjunctive -- returns only docs @@@ accepts.
 */
typedef struct DocidFilter
{
	const uint64 *docids;		/* sorted ascending, or NULL for "admit all" */
	int			n;
} DocidFilter;

/* True if docid is admitted by the filter (NULL filter admits everything). */
static inline bool
docid_admitted(const DocidFilter *f, uint64 docid)
{
	int			lo,
				hi;

	if (f == NULL || f->docids == NULL)
		return true;			/* pure-OR fast path: no gating */
	lo = 0;
	hi = f->n - 1;
	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;
		uint64		v = f->docids[mid];

		if (v == docid)
			return true;
		else if (v < docid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/*
 * weave_query_is_pure_or: true iff the query's boolean structure is a plain
 * disjunction of plain terms -- only OR operators, and every operand is an
 * exact term (no PREFIX/FUZZY/REGEX flag, no AND/NOT/PHRASE).  For such a
 * query the WAND term-disjunction == the @@@ match set, so the ranked scan
 * needs no membership filter.  Any other shape (AND/NOT/PHRASE, or a
 * prefix/fuzzy/regex operand, whose contribution the scorer flattens but @@@
 * evaluates as a set) is NOT pure OR and must be filtered.
 */
static bool
weave_query_is_pure_or(WeaveQuery q)
{
	uint32		i;

	if (q == NULL || q->nitems == 0)
		return true;
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX))
				return false;
		}
		else					/* operator */
		{
			if (it->op != WEAVE_OP_OR)
				return false;
		}
	}
	return true;
}

/*
 * weave_query_is_pure_boolean: true iff the query is a boolean combination
 * (AND/OR/NOT, no PHRASE/NEAR) of PLAIN term operands (no PREFIX/FUZZY/REGEX).
 * For such a query, a doc's @@@ membership is a pure function of WHICH query
 * terms are present in the doc -- exactly what the WAND scan knows at each
 * pivot (which cursors sit at the pivot docid).  So membership can be decided
 * LAZILY at heap-admission time by evaluating the query's RPN over term
 * presence, instead of pre-collecting the whole @@@ match set.
 *
 * Broader than weave_query_is_pure_or (which is the all-OR special case).  Any
 * PHRASE/NEAR operator, or any prefix/fuzzy/regex operand, is NOT pure boolean
 * -- those need positions or term-expansion the cursor-presence test cannot
 * see, so they keep the collect+recheck+DocidFilter path.
 */
static bool
weave_query_is_pure_boolean(WeaveQuery q)
{
	uint32		i;

	if (q == NULL || q->nitems == 0)
		return true;
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX))
				return false;
		}
		else					/* operator */
		{
			if (it->op != WEAVE_OP_AND && it->op != WEAVE_OP_OR &&
				it->op != WEAVE_OP_NOT)
				return false;	/* PHRASE/NEAR */
		}
	}
	return true;
}

/*
 * BoolGate: lazy boolean @@@ membership over term presence at a pivot docid.
 *
 * For a pure-boolean query (see weave_query_is_pure_boolean) the DocidFilter's
 * pre-collected match set is unnecessary: at each scored pivot the WAND scan
 * already knows which query terms are present (a cursor sits at pivot_docid).
 * `present[t]` (indexed by the ordinal VAL/term index that weave_query_terms
 * assigns) is set by the caller, then bool_gate_admits evaluates the query's
 * RPN with each VAL leaf = present[its term index] and AND/OR/NOT combining.
 * This is the exact @@@ truth value for the doc, computed WITHOUT a collect
 * pass.  `q` is pure boolean, so the stack machine mirrors weave_doc_matches's
 * boolean cases (no phrase, no prefix/fuzzy/regex leaves).
 */
typedef struct BoolGate
{
	WeaveQuery	q;				/* pure-boolean query (NULL => no gate) */
	bool	   *present;			/* present[termidx], nterms entries */
	bool	   *stack;			/* scratch RPN stack, nitems entries */
	int			nterms;
}			BoolGate;

/*
 * Evaluate the pure-boolean query's RPN over the current present[] flags.
 * Leaf VAL i (i = ordinal among VAL items) is true iff present[i]; AND/OR/NOT
 * combine.  Returns the doc's @@@ membership.  NULL gate admits everything.
 */
static inline bool
bool_gate_admits(const BoolGate *g)
{
	WeaveQuery	q;
	bool	   *st;
	int			top = 0;
	int			vi = 0;
	uint32		i;

	if (g == NULL || g->q == NULL)
		return true;
	q = g->q;
	st = g->stack;
	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
			st[top++] = g->present[vi++];
		else if (it->op == WEAVE_OP_NOT)
			st[top - 1] = !st[top - 1];
		else if (it->op == WEAVE_OP_AND)
		{
			st[top - 2] = st[top - 2] && st[top - 1];
			top--;
		}
		else					/* WEAVE_OP_OR */
		{
			st[top - 2] = st[top - 2] || st[top - 1];
			top--;
		}
	}
	return top == 1 ? st[0] : false;
}

/*
 * Fill the gate's present[] from which cursors sit at `pivot_docid`, then
 * evaluate.  A term is present iff ANY of its (per-segment) cursors is at the
 * pivot.  NULL gate admits everything (pure-OR / DocidFilter path).
 */
static inline bool
bmw_gate_admits(BoolGate *g, WandCursor *cursors, int nterms, uint64 pivot_docid)
{
	int			i;

	if (g == NULL || g->q == NULL)
		return true;
	for (i = 0; i < g->nterms; i++)
		g->present[i] = false;
	for (i = 0; i < nterms; i++)
		if (cursors[i].docid == pivot_docid)
			g->present[cursors[i].termidx] = true;
	return bool_gate_admits(g);
}

/*
 * weave_search_wand: exact top-k identical to the accumulate path, but using
 * document-at-a-time WAND so that documents which cannot enter the current
 * top-k are skipped via the per-term max-contribution bounds.  Returns the
 * number of (tid, score) results written to *out (palloc'd), capped at k.
 *
 * `filter` (may be NULL) gates heap admission by docid membership: a
 * fully-scored candidate enters the top-k only if the filter admits its docid.
 * This threads the exact @@@ boolean match set into the proven single-pass
 * WAND/MaxScore traversal WITHOUT changing the traversal or the block-skip /
 * threshold math -- filtered docs simply never reach the heap (and so never
 * raise the threshold).
 */
static int
weave_search_bmw(WandCursor *cursors, int nterms, int k, const DocidFilter *filter,
			   BoolGate *gate, ScoredTid **out)
{
	ScoredTid  *heap;			/* min-heap of current top-k by score */
	int			nheap = 0;
	double		threshold = 0.0;
	int			t;

	heap = (ScoredTid *) palloc(Max(k, 1) * sizeof(ScoredTid));

	/* prime each cursor with its first page */
	for (t = 0; t < nterms; t++)
		wand_prime(&cursors[t]);

	for (;;)
	{
		int			i,
					j;
		uint64		pivot_docid;
		double		maxsum;
		double		score;

		CHECK_FOR_INTERRUPTS();	/* per document-at-a-time step; cursor blocks are palloc'd copies, no lock held */

		/* selection-sort cursors by current docid (nterms is small) */
		for (i = 0; i < nterms; i++)
			for (j = i + 1; j < nterms; j++)
				if (cursors[j].docid < cursors[i].docid)
				{
					WandCursor	tmp = cursors[i];

					cursors[i] = cursors[j];
					cursors[j] = tmp;
				}

		if (cursors[0].docid == UINT64_MAX)
			break;				/* all exhausted */

		/*
		 * WAND pivot: accumulate max_contrib in docid order until the running
		 * sum could exceed the threshold; that cursor's docid is the pivot.
		 */
		maxsum = 0.0;
		pivot_docid = UINT64_MAX;
		for (i = 0; i < nterms; i++)
		{
			if (cursors[i].docid == UINT64_MAX)
				break;
			maxsum += cursors[i].max_contrib;
			if (maxsum > threshold || nheap < k)
			{
				pivot_docid = cursors[i].docid;
				break;
			}
		}
		if (pivot_docid == UINT64_MAX)
			break;				/* no document can beat the threshold */

		/*
		 * Block-max WAND (BMW) refinement: the pivot passed the term-wide
		 * bound, but the *current blocks* may bound tighter.  Sum the per-block
		 * max contribution of every cursor whose docid <= pivot; if even that
		 * cannot beat the threshold, no document up to the pivot can enter the
		 * top-k, so skip the earliest cursor past its current 128-block instead
		 * of scoring.  Sound because block max_tf >= every tf in the block.
		 */
		if (nheap >= k)
		{
			double		blocksum = 0.0;
			int			nle = 0;		/* cursors with docid <= pivot */
			int			lei = -1;		/* index of the (single) such cursor */

			for (i = 0; i < nterms; i++)
			{
				if (cursors[i].docid == UINT64_MAX)
					break;
				if (cursors[i].docid <= pivot_docid)
				{
					blocksum += wand_block_max_contrib(&cursors[i]);
					nle++;
					lei = i;
				}
			}
			if (blocksum <= threshold)
			{
				/*
				 * FAST PATH -- exactly one cursor is at/before the pivot AND
				 * no other term's cursor falls within that cursor's current
				 * block's docid range.  Then blocksum bounded the WHOLE block,
				 * every docid in it contains ONLY this term (all other cursors
				 * sit strictly past the block's last docid), so no document in
				 * the block -- even under a conjunctive/AND score -- can beat the
				 * threshold.  Skipping the entire block is exact and O(1).
				 *
				 * The block-range guard is essential for correctness with
				 * multi-term AND: "other cursors are past the PIVOT" does NOT
				 * imply "past the block"; a cursor at docid in (pivot, blocklast]
				 * means a document in the skipped range could contain BOTH terms
				 * and score sum-of-both -- which blocksum (one term) never
				 * bounded.  Skipping it then drops true top-k AND docs (the
				 * pre-existing multi-term AND recall gap).
				 *
				 * SAFE PATH -- two or more cursors sit at/before the pivot, OR a
				 * cursor overlaps the block's docid range: advance every
				 * at-or-before cursor to just past the pivot instead; blocksum
				 * proved nothing up to and including pivot_docid can win, so this
				 * is exact regardless of conjunction.
				 */
				bool		block_isolated = false;

				if (nle == 1 && cursors[lei].blkcount > 0)
				{
					uint64		blocklast = cursors[lei].docids[cursors[lei].blkcount - 1];

					block_isolated = true;
					for (i = 0; i < nterms; i++)
						if (i != lei && cursors[i].docid != UINT64_MAX &&
							cursors[i].docid <= blocklast)
						{
							block_isolated = false;
							break;
						}
				}
				if (block_isolated)
					wand_skip_block(&cursors[lei]);
				else
					for (i = 0; i < nterms; i++)
						if (cursors[i].docid != UINT64_MAX &&
							cursors[i].docid <= pivot_docid)
							wand_seek(&cursors[i], pivot_docid + 1);
				continue;
			}
		}

		/* if the smallest docid equals the pivot, score it fully */
		if (cursors[0].docid == pivot_docid)
		{
			ItemPointerData tid;

			weave_docid_to_tid(pivot_docid, &tid);

			score = 0.0;
			for (i = 0; i < nterms; i++)
				if (cursors[i].docid == pivot_docid)
					score += wand_contrib_cur(&cursors[i]);

			/* push into the top-k min-heap -- but only if the docid is admitted
			 * by the boolean match-set gate.  Two equivalent gates:
			 *  - DocidFilter (filter): binary-search a pre-collected @@@ set
			 *    (used for non-pure-boolean: phrase/near/prefix/fuzzy/regex).
			 *  - BoolGate (gate): evaluate the query's RPN LAZILY over which
			 *    terms have a cursor at the pivot (pure-boolean AND/NOT); no
			 *    collect pass.  For pure-OR both are NULL (admit all).
			 * A rejected doc does NOT count toward k and does NOT raise the
			 * threshold; the traversal and its block-skip math are unchanged
			 * (the threshold may just stay lower longer, costing pruning only,
			 * never correctness). */
			if (docid_admitted(filter, pivot_docid) &&
				bmw_gate_admits(gate, cursors, nterms, pivot_docid))
			{
			if (nheap < k)
			{
				heap[nheap].tid = tid;
				heap[nheap].score = score;
				nheap++;
				if (nheap == k)
				{
					threshold = heap[0].score;
					for (i = 1; i < nheap; i++)
						if (heap[i].score < threshold)
							threshold = heap[i].score;
				}
			}
			else if (score > threshold)
			{
				int			minpos = 0;

				for (i = 1; i < nheap; i++)
					if (heap[i].score < heap[minpos].score)
						minpos = i;
				heap[minpos].tid = tid;
				heap[minpos].score = score;
				threshold = heap[0].score;
				for (i = 1; i < nheap; i++)
					if (heap[i].score < threshold)
						threshold = heap[i].score;
			}
			}					/* end docid_admitted gate */

			/* advance every cursor positioned at the pivot */
			for (i = 0; i < nterms; i++)
				if (cursors[i].docid == pivot_docid)
					wand_next(&cursors[i]);
		}
		else
		{
			/*
			 * Advance every cursor before the pivot up to pivot_docid.  Use a
			 * seek (block-skipping) rather than stepping one posting at a time:
			 * for a high-df term this skips entire 128-blocks whose docids are
			 * all below the pivot, instead of decoding hundreds of thousands of
			 * postings individually (the Q5/Q7 cost).
			 */
			for (i = 0; i < nterms; i++)
				if (cursors[i].docid < pivot_docid)
					wand_seek(&cursors[i], pivot_docid);
		}
	}

	/* release any still-loaded block buffers + doclen page directories */
	for (t = 0; t < nterms; t++)
	{
		if (cursors[t].blkbuf)
			pfree(cursors[t].blkbuf);
		weave_doclen_cursor_free(&cursors[t].doclenc);
	}

	qsort(heap, nheap, sizeof(ScoredTid), cmp_scored_desc);
	*out = heap;
	return nheap;
}

/*
 * weave_search_maxscore: exact top-k via the MaxScore algorithm.  Cursors are
 * split into ESSENTIAL and NON-ESSENTIAL sets by ascending max_contrib: a
 * suffix of low-impact terms whose cumulative max_contrib cannot, by itself,
 * reach the current threshold is non-essential -- a document containing only
 * non-essential terms can never enter the top-k.  We therefore iterate
 * candidate docids from the ESSENTIAL cursors only (document-at-a-time over the
 * smallest essential docid), then add the non-essential terms' contributions by
 * seeking.  As the threshold rises, more terms become non-essential, so long
 * queries do progressively less work.  Complements BMW (which excels on short
 * queries); identical exact top-k.
 */
static int
weave_search_maxscore(WandCursor *cursors, int nterms, int k,
					const DocidFilter *filter, BoolGate *gate, ScoredTid **out)
{
	ScoredTid  *heap;
	int			nheap = 0;
	double		threshold = 0.0;
	double	   *suffix;			/* suffix[i] = sum of max_contrib[i..nterms) */
	int			t,
				i,
				j;
	int			first_essential;	/* cursors[first_essential..) are essential */

	heap = (ScoredTid *) palloc(Max(k, 1) * sizeof(ScoredTid));
	suffix = (double *) palloc((nterms + 1) * sizeof(double));	/* alloc-ok: nterms = query term count */

	for (t = 0; t < nterms; t++)
		wand_prime(&cursors[t]);

	/* order cursors by ascending term-wide max_contrib (once; it is static) */
	for (i = 0; i < nterms; i++)
		for (j = i + 1; j < nterms; j++)
			if (cursors[j].max_contrib < cursors[i].max_contrib)
			{
				WandCursor	tmp = cursors[i];

				cursors[i] = cursors[j];
				cursors[j] = tmp;
			}
	suffix[nterms] = 0.0;
	for (i = nterms - 1; i >= 0; i--)
		suffix[i] = suffix[i + 1] + cursors[i].max_contrib;

	first_essential = 0;

	for (;;)
	{
		uint64		cand = UINT64_MAX;
		double		score;

		CHECK_FOR_INTERRUPTS();	/* per document-at-a-time step; cursor blocks are palloc'd copies, no lock held */

		/* recompute the essential boundary from the current threshold: the
		 * longest low-impact prefix whose max_contrib sum <= threshold is
		 * non-essential */
		if (nheap >= k)
		{
			while (first_essential < nterms &&
				   suffix[first_essential + 1] <= threshold)
				first_essential++;
		}

		/* smallest docid among essential cursors drives the iteration */
		for (i = first_essential; i < nterms; i++)
			if (cursors[i].docid < cand)
				cand = cursors[i].docid;
		if (cand == UINT64_MAX)
			break;				/* essential cursors exhausted */

		/* score cand: essential contributions + upper bound of non-essentials */
		score = 0.0;
		for (i = first_essential; i < nterms; i++)
			if (cursors[i].docid == cand)
				score += wand_contrib_cur(&cursors[i]);

		/* early-exit check: essential score + all non-essential max <= threshold
		 * => cand cannot make the top-k, skip the non-essential lookups */
		if (!(nheap >= k && score + suffix[first_essential] <= threshold))
		{
			/* add exact non-essential contributions by seeking to cand */
			for (i = 0; i < first_essential; i++)
			{
				wand_seek(&cursors[i], cand);
				if (cursors[i].docid == cand)
					score += wand_contrib_cur(&cursors[i]);
			}

			/* gate heap admission by the boolean match-set (DocidFilter for
			 * non-pure-boolean, or the lazy BoolGate over term-presence at cand
			 * for pure-boolean AND/NOT).  A rejected doc never enters the heap
			 * and never raises the threshold.  The essential/non-essential
			 * traversal (incl. the non-essential seeks above) is unchanged --
			 * only heap insertion is gated. */
			if (docid_admitted(filter, cand) &&
				bmw_gate_admits(gate, cursors, nterms, cand))
			{
			if (nheap < k)
			{
				ItemPointerData tid;

				weave_docid_to_tid(cand, &tid);
				heap[nheap].tid = tid;
				heap[nheap].score = score;
				nheap++;
				if (nheap == k)
				{
					threshold = heap[0].score;
					for (i = 1; i < nheap; i++)
						if (heap[i].score < threshold)
							threshold = heap[i].score;
				}
			}
			else if (score > threshold)
			{
				ItemPointerData tid;
				int			minpos = 0;

				weave_docid_to_tid(cand, &tid);
				for (i = 1; i < nheap; i++)
					if (heap[i].score < heap[minpos].score)
						minpos = i;
				heap[minpos].tid = tid;
				heap[minpos].score = score;
				threshold = heap[0].score;
				for (i = 1; i < nheap; i++)
					if (heap[i].score < threshold)
						threshold = heap[i].score;
			}
			}					/* end docid_admitted gate */
		}

		/* advance every essential cursor sitting at cand */
		for (i = first_essential; i < nterms; i++)
			if (cursors[i].docid == cand)
				wand_next(&cursors[i]);
	}

	for (t = 0; t < nterms; t++)
	{
		if (cursors[t].blkbuf)
			pfree(cursors[t].blkbuf);
		weave_doclen_cursor_free(&cursors[t].doclenc);
	}

	qsort(heap, nheap, sizeof(ScoredTid), cmp_scored_desc);
	*out = heap;
	return nheap;
}

/*
 * Dispatch to the top-k algorithm best suited to the query shape.  BMW excels
 * on short queries (tight block-max pruning, cheap pivot); MaxScore does
 * progressively less work as terms become non-essential, winning on long
 * queries / large k.  Both return the identical exact top-k.
 */
static int
weave_search_wand(WandCursor *cursors, int nterms, int k,
				const DocidFilter *filter, BoolGate *gate, ScoredTid **out)
{
	if (nterms >= 4)
		return weave_search_maxscore(cursors, nterms, k, filter, gate, out);
	return weave_search_bmw(cursors, nterms, k, filter, gate, out);
}

/*
 * weave_query_maxhits: an upper bound on the number of documents a query can
 * match, computed by walking the RPN with a stack -- VAL pushes the term's
 * global df; AND/PHRASE -> min of operands; OR -> sum; NOT -> corpus N (a NOT
 * can match almost everything).  Fuzzy/regex terms over-generate and have no
 * cheap df, so any such term makes the bound N (unbounded for our purposes).
 * Used to decide whether the ordering scan can compute the WHOLE result set in
 * one WAND pass (avoiding the adaptive-k recompute) when the result is small.
 */
static double
weave_query_maxhits(Relation index, WeaveQuery q, double N)
{
	WeaveMetaPageData meta;
	double	   *stack;
	int			top = 0;
	uint32		i;
	double		result;

	if (q->nitems == 0)
		return 0;
	weave_read_meta(index, &meta);
	stack = (double *) palloc(q->nitems * sizeof(double));

	for (i = 0; i < q->nitems; i++)
	{
		WeaveQueryItem *it = &q->items[i];

		if (it->type == WEAVE_QI_VAL)
		{
			if (it->flags & (WEAVE_QF_FUZZY | WEAVE_QF_REGEX | WEAVE_QF_PREFIX))
				stack[top++] = N;	/* over-generating: no cheap bound */
			else
			{
				uint64		gdf = 0;	/* summed across segments; uint64 (consumed as double) */
				uint32		s;

				for (s = 0; s < meta.nsegments; s++)
				{
					uint32		df,
								mtf;
					BlockNumber fb;
					uint32		fo;

					if (weave_lookup_dict(index, &meta.segs[s],
										 WEAVE_QUERY_ITEMTEXT(q, it), it->termlen,
										 &df, &mtf, &fb, &fo))
						gdf += df;
				}
				stack[top++] = (double) gdf;
			}
		}
		else if (it->op == WEAVE_OP_NOT)
		{
			/* !x can match up to N docs */
			if (top >= 1)
				stack[top - 1] = N;
		}
		else					/* AND / OR / PHRASE: binary */
		{
			double		b = (top >= 1) ? stack[--top] : 0;
			double		a = (top >= 1) ? stack[--top] : 0;

			if (it->op == WEAVE_OP_OR)
				stack[top++] = a + b;
			else				/* AND, PHRASE: bounded by the smaller side */
				stack[top++] = Min(a, b);
		}
	}
	result = (top >= 1) ? stack[top - 1] : N;
	pfree(stack);
	return Min(result, N);
}

/*
 * weave_topk_visible: shared top-k engine for both the weave_search SRF and the
 * amgettuple ordering scan.  Runs block-max WAND / MaxScore over the index's
 * SEGMENTS for `q`, over-fetches candidates so MVCC visibility filtering still
 * yields k visible rows, drops tombstoned docs, and returns them (palloc'd in
 * the current context) sorted by descending score.  When as_distance is true,
 * each result's .score field is replaced by the ordering distance 1/(1+score)
 * (ascending distance = the same order).  The index must already be open; the
 * base table is opened here for the visibility check.  Returns the number of
 * visible results.
 *
 * NOTE: ranked results cover the merged SEGMENTS only; documents still in the
 * pending write buffer (inserted since the last flush) are searchable by @@@
 * and counted by weave_count(), but are not ranked here until a flush folds them
 * into a segment (automatic on VACUUM, or immediate via weave_merge()).  Ranking
 * pending docs would require per-doc scoring outside the WAND cursors; deferred
 * intentionally, since pending is transient and bounded.
 */
static int
weave_topk_candidates_range(Relation index, WeaveQuery q, int wantk,
						   uint64 docid_lo, uint64 docid_hi, ScoredTid **out)
{
	WeaveMetaPageData meta;
	double		N,
				avgdl;
	const char **terms;
	int		   *lens;
	int			nterms;
	WandCursor *cursors;
	ScoredTid  *cand;
	WeaveDoclenDirCache *doclendir;	/* relcache-cached page directory over the v4
									 * doclen sidecars (built once per backend, keyed
									 * by generation); borrowed by the cursors below */
	WeaveDoclenResident *doclenres = NULL;	/* one shared resident doclen block per
											 * SEGMENT, so a multi-term query does not
											 * decode the same block once per term */
	WeaveTombstones tombs;
	DocidFilter filter;
	DocidFilter *filterp = NULL;
	uint64	   *filter_docids = NULL;
	BoolGate	gate;
	BoolGate   *gatep = NULL;
	int			ncand;
	int			t,
				nactive = 0;
	double		k1 = 1.2;

	if (wantk < 1)
		wantk = 1;

	weave_read_meta(index, &meta);
	N = meta.ndocs < 1.0 ? 1.0 : meta.ndocs;
	avgdl = meta.ndocs > 0 ? meta.sumdoclen / meta.ndocs : 1.0;
	/* relcache page directory over the v4 doclen sidecars (built once per backend,
	 * keyed by meta.generation); NULL if no v4 sidecar segment exists */
	doclendir = weave_doclendir_cache(index, &meta);
	if (doclendir != NULL)
		doclenres = (WeaveDoclenResident *)
			palloc0(sizeof(WeaveDoclenResident) * Max((int) meta.nsegments, 1));

	/*
	 * Boolean-structure gating.  The WAND cursors below rank the term
	 * DISJUNCTION (weave_query_terms flattens operators), a SUPERSET of the @@@
	 * match set for any non-pure-OR query, so heap admission must be gated to
	 * the exact @@@ set.  Two gates, both byte-identical top-k:
	 *
	 *  - PURE-BOOLEAN (AND/OR/NOT over plain terms, no phrase/prefix/fuzzy/
	 *    regex): the LAZY BoolGate.  A doc's @@@ truth is a pure function of
	 *    which query terms are present, which the WAND scan already knows at
	 *    each pivot (which cursors sit at the pivot).  So evaluate the query's
	 *    RPN over cursor-presence at admission time -- NO collect pass.  This
	 *    removes the redundant weave_collect_matches materialization that made
	 *    ranked AND/NOT slow (e.g. `year & hungary` no longer materializes all
	 *    735k `year` postings before the WAND scan).
	 *
	 *  - NON-PURE-BOOLEAN (phrase/near/prefix/fuzzy/regex present): the exact
	 *    @@@ set must be computed (positions / term-expansion the cursor test
	 *    cannot see) -- keep weave_collect_matches + recheck + DocidFilter.
	 *
	 *  - PURE-OR: neither gate (the disjunction IS the @@@ set); NULL, zero
	 *    overhead, as before.
	 */
	if (!weave_query_is_pure_or(q) && weave_query_is_pure_boolean(q))
	{
		/* lazy path: gate built after cursors so nterms is known */
		gatep = &gate;
	}
	else if (!weave_query_is_pure_or(q))
	{
		TidSet		matches;
		bool		recheck;
		int			i;

		weave_collect_matches(index, q, &matches, &recheck);
		/*
		 * matches is the SAME boolean set @@@ uses through this index (it is
		 * built by the identical evaluator).  For fuzzy/regex/NOT-universe and
		 * PHRASE/NEAR it OVER-generates (recheck=true): fuzzy/regex is a
		 * trigram-funnel candidate set, and a PHRASE is the AND-set (the
		 * positionless posting lists cannot enforce adjacency).  The bitmap-
		 * heap scan resolves this with an executor recheck of @@@; the ranked
		 * scan has none, so we recheck here -- shrink matches to the EXACT set
		 * against the heap wdoc.  After this the docid filter is precise, so
		 * the ranked scan never admits a doc "WHERE d @@@ q" would reject.
		 *
		 * COMPLETENESS caveat: the WAND cursors are built from the LITERAL query
		 * terms (weave_query_terms), so a doc that matches only via a fuzzy/prefix/
		 * regex EXPANSION (no posting for the literal term) is never generated as
		 * a ranked candidate.  The recheck only shrinks, so ranked fuzzy/prefix/
		 * regex results are a correct SUBSET of the @@@ matches, not the full set.
		 * PHRASE/NEAR/boolean are exact.  Use @@@ for exhaustive fuzzy/prefix.
		 *
		 * TidSet is TID-sorted and weave_tid_to_docid is monotonic in TID
		 * order, so the docid array comes out sorted (binary-searchable).
		 */
		if (recheck)
			weave_recheck_exact(index, q, &matches);
		if (matches.n > 0)
		{
			filter_docids = (uint64 *) palloc(matches.n * sizeof(uint64));
			for (i = 0; i < matches.n; i++)
				filter_docids[i] = weave_tid_to_docid(&matches.tids[i]);
		}
		filter.docids = filter_docids;
		filter.n = matches.n;
		filterp = &filter;
		/* empty match set: nothing satisfies @@@, so no candidates */
		if (matches.n == 0)
		{
			if (matches.tids)
				pfree(matches.tids);
			*out = NULL;
			return 0;
		}
	}

	nterms = weave_query_terms(q, &terms, &lens);
	/* up to one cursor per (term, segment) */
	cursors = (WandCursor *) palloc(Max(nterms * Max((int) meta.nsegments, 1), 1) *	/* alloc-ok: query terms x <=128 segments */
									sizeof(WandCursor));

	/* per-segment tombstones: each cursor skips docids deleted in its own
	 * segment, so reused heap TIDs live in another segment still rank */
	weave_tombstones_load(index, &meta, &tombs);

	/* v4 doclen sidecar: each cursor gets a FORWARD cursor over its segment's
	 * sidecar chain (initialised per cursor below), reading only the pages that
	 * cover the docids it actually scores -- NOT a whole-segment preload, which
	 * on a many-segment index dominated the scan (the 1.5.0 slowdown report). */

	for (t = 0; t < nterms; t++)
	{
		uint64		gdf = 0;	/* summed across segments; uint64 (feeds IDF as double) */
		uint32		s;
		double		idf;
		double		b = 0.75;

		/* global df across all segments -> IDF (segments share the corpus) */
		for (s = 0; s < meta.nsegments; s++)
		{
			uint32		df,
						max_tf;
			BlockNumber firstblk;
			uint32		firstoff;

			if (weave_lookup_dict(index, &meta.segs[s], terms[t], lens[t],
								 &df, &max_tf, &firstblk, &firstoff))
				gdf += df;
		}
		if (gdf == 0)
			continue;			/* term absent in every segment */
		idf = log(1.0 + (N - (double) gdf + 0.5) / ((double) gdf + 0.5));

		/* one cursor per segment that contains the term */
		for (s = 0; s < meta.nsegments; s++)
		{
			uint32		df,
						max_tf;
			BlockNumber firstblk;
			uint32		firstoff;
			double		mtf;

			if (!weave_lookup_dict(index, &meta.segs[s], terms[t], lens[t],
								  &df, &max_tf, &firstblk, &firstoff))
				continue;
			mtf = (double) max_tf;
			cursors[nactive].index = index;
			cursors[nactive].firstblk = firstblk;
			cursors[nactive].firstoff = firstoff;
			cursors[nactive].df = df;
			cursors[nactive].termidx = t;
			cursors[nactive].blkbuf = NULL;
			cursors[nactive].blkcount = 0;
			cursors[nactive].cur = 0;
			cursors[nactive].docid = 0;
			cursors[nactive].idf = idf;
			cursors[nactive].avgdl = avgdl;
			cursors[nactive].k1b_inv_avgdl = k1 * b / avgdl;
			cursors[nactive].k1_1mb = k1 * (1.0 - b);
			cursors[nactive].idf_k1p1 = idf * (k1 + 1.0);
			cursors[nactive].max_contrib =
				idf * mtf * (k1 + 1.0) / (mtf + k1 * (1.0 - b));
			cursors[nactive].tombs = &tombs;
			cursors[nactive].segidx = s;
			cursors[nactive].has_doclen_col =
				(meta.segs[s].doclenstart == InvalidBlockNumber);
			weave_doclen_cursor_init(&cursors[nactive].doclenc, index,
									meta.segs[s].doclenstart, doclendir,
									doclenres ? &doclenres[s] : NULL);
			cursors[nactive].docid_lo = docid_lo;
			cursors[nactive].docid_hi = docid_hi;
			{
				sm_cursor_t ini = SM_CURSOR_INIT;

				cursors[nactive].tombcursor = ini;
			}
			nactive++;
		}
	}

	/* build the lazy BoolGate now that nterms is known (pure-boolean path) */
	if (gatep != NULL)
	{
		gate.q = q;
		gate.nterms = nterms;
		gate.present = (bool *) palloc0(Max(nterms, 1) * sizeof(bool));	/* alloc-ok: nterms = query term count */
		gate.stack = (bool *) palloc(Max(q->nitems, 1) * sizeof(bool));
	}

	ncand = weave_search_wand(cursors, nactive, wantk, filterp, gatep, &cand);
	weave_tombstones_free(&tombs);
	if (filter_docids)
		pfree(filter_docids);
	if (gatep != NULL)
	{
		pfree(gate.present);
		pfree(gate.stack);
	}

	*out = cand;
	return ncand;
}

/*
 * weave_topk_visible: serial top-k for the weave_search SRF.  (The amgettuple
 * ordering scan drives the pass itself -- see weave_ord_pass -- so that it can
 * hand out candidates lazily and keep them across a widening.)  Generates
 * candidates over the WHOLE corpus (docid range
 * [0, MAX)) then applies MVCC visibility, over-fetching (wantk = k*4) so k
 * visible rows survive.  When as_distance is true each result's .score is the
 * ordering distance 1/(1+score).  Returns visible results (palloc'd) sorted by
 * descending score.
 *
 * A heavy-delete workload can make more than the over-fetch fraction of the
 * top candidates invisible, leaving nvis < k.  The amgettuple path retries and
 * grows on its own, but the weave_search SRF calls this once, so we grow wantk
 * and re-generate here: double wantk (capped) and retry whenever the loop ends
 * short AND the last batch was full (ncand == wantk, i.e. more candidates
 * existed).  If ncand < wantk the corpus is exhausted, so we stop.
 */
static int
weave_topk_visible(Relation index, WeaveQuery q, int k, bool as_distance,
				  ScoredTid **out)
{
	ScoredTid  *cand;
	ScoredTid  *results = NULL;
	int			ncand;
	int			nvis = 0;
	int			i;
	int			wantk = Max(k * 4, 64);
	int			wantk_cap;
	Snapshot	snap = GetActiveSnapshot();
	Relation	heap;
	IndexFetchTableData *fetch;

	if (k < 1)
		k = 1;
	wantk_cap = Max(k * 64, 4096);

	for (;;)
	{
		/*
		 * Candidate generation reads segment pages under only per-page SHARE
		 * locks off a metapage snapshot, so a concurrent merge/vacuum can free
		 * and recycle them mid-scan (the A1 race); weave_topk_candidates_guarded
		 * brackets that with the directory generation.  The MVCC visibility loop
		 * below reads the heap, not the index, so it needs no guard.
		 */
		ncand = weave_topk_candidates_guarded(index, q, wantk, &cand);

		/* Drop any results from a prior (short) attempt before re-filling. */
		if (results)
			pfree(results);
		results = (ScoredTid *) palloc(k * sizeof(ScoredTid));
		nvis = 0;

		heap = table_open(index->rd_index->indrelid, AccessShareLock);
#if PG_VERSION_NUM >= 190000
		fetch = table_index_fetch_begin(heap, SO_NONE);
#else
		fetch = table_index_fetch_begin(heap);
#endif
		for (i = 0; i < ncand && nvis < k; i++)
		{
			ItemPointerData tid = cand[i].tid;
			bool		call_again = false;
			bool		all_dead = false;
			TupleTableSlot *slot = table_slot_create(heap, NULL);

			if (table_index_fetch_tuple(fetch, &tid, snap, slot,
										&call_again, &all_dead))
			{
				results[nvis] = cand[i];
				if (as_distance)
					results[nvis].score = 1.0 / (1.0 + cand[i].score);
				nvis++;
			}
			ExecDropSingleTupleTableSlot(slot);
		}
		table_index_fetch_end(fetch);
		table_close(heap, AccessShareLock);
		if (cand)
			pfree(cand);

		/*
		 * Enough visible rows, or the corpus is exhausted (ncand < wantk means
		 * generation returned fewer than we asked for -- no more candidates),
		 * or we have hit the growth cap: stop.  Otherwise double wantk and redo.
		 */
		if (nvis >= k || ncand < wantk || wantk >= wantk_cap)
			break;
		wantk = Min(wantk * 2, wantk_cap);
	}

	*out = results;
	return nvis;
}

/*
 * ---------------------- incremental ranked-scan growth (L14) ----------------
 *
 * PostgreSQL gives an index access method no way to learn the query's LIMIT, so
 * a ranked scan must guess how deep to go and widen when the executor asks for
 * more.  Before L14 a widening RECOMPUTED THE WHOLE PASS and threw the previous
 * one away, which made the cost of a deep page a function of HOW MANY TIMES the
 * scan had been redone rather than of the depth reached: from
 * wand_initial_k = 16 a LIMIT 100 query ran three complete passes (16, 64, 256),
 * from 32 two, from 100 one -- so the k frontier came out non-monotonic and the
 * knob traded a small LIMIT against a deep one (bench/RESULTS_WAND_K.md).
 *
 * Two changes make a widening EXTEND the previous pass:
 *
 *  1. Visibility is checked LAZILY, a batch at a time, across the pass's whole
 *     candidate list.  That list was always Max(k*4, 64) wide -- the over-fetch
 *     existed so k rows survived MVCC filtering -- but everything past the k-th
 *     visible row was discarded.  Keeping it lets one pass serve ~4x deeper, and
 *     a small LIMIT now probes the heap FEWER times than before (as many rows as
 *     are actually pulled, not k).
 *
 *  2. The accumulated visible rows survive a widening.  They are not re-probed
 *     and, crucially, not re-emitted: the new pass's candidates are filtered
 *     against the TIDs already materialized, so the scan continues where it
 *     stopped instead of replaying its own output at an offset.
 *
 * What is NOT incremental: the per-term posting cursors still restart at docid 0
 * on a widening, so the wider pass re-reads postings.  That is not fixable by
 * bookkeeping -- a document pruned under the narrow pass's threshold can belong
 * in the wider top-k, so the wider pass must be able to revisit it.  The saving
 * here is the output side (heap probes, re-emission) plus the ~4x deeper reach
 * per pass; the posting decode is task L2's (impact-ordered postings) problem.
 */

/*
 * weave_ord_width: candidate width for a ranked pass at nominal k.
 *
 * Identical to the over-fetch weave_topk_visible has always used, so the FIRST
 * pass of a scan reads exactly what it read before this change; what differs is
 * that all of it is now reachable by the executor.  Clamped so that
 * width * sizeof(ScoredTid) cannot overflow along the x4 widening ladder.
 */
#define WEAVE_ORD_WIDTH_MAX (INT_MAX / 32)

static int
weave_ord_width(int k)
{
	if (k < 1)
		k = 1;
	if (k > WEAVE_ORD_WIDTH_MAX / 4)
		return WEAVE_ORD_WIDTH_MAX;
	return Max(k * 4, 64);
}

/*
 * weave_ord_pass: run one ranked candidate pass at width so->curk and install
 * its result as the scan's candidate list.
 *
 * Candidates already materialized into so->ordered by an earlier, narrower pass
 * are removed from the new list by TID.  That is what makes a widening an
 * extension: those rows keep their original position and distance, are not
 * probed against the heap again, and cannot be emitted twice.  (The predecessor
 * of this code resumed by INDEX -- it recomputed the whole visible list and
 * restarted at the count already returned -- which assumed the wider pass
 * reproduced the narrower one's prefix exactly, including among tied scores.
 * Filtering by TID needs no such assumption.)
 *
 * Ordering across the boundary does still rely on the earlier pass being exact,
 * which it is: a document in an exact top-W1 has fewer than W1 <= W2 documents
 * scoring above it, so nothing the wider pass newly finds outranks a row already
 * handed out.  A document merged into the index BETWEEN two passes could; a
 * ranked scan is not order-stable under concurrent modification and was not
 * before this change either.
 */
static void
weave_ord_pass(Relation index, WeaveScanOpaque so)
{
	ScoredTid  *cand = NULL;
	int			ncand;
	int			i;

	ncand = weave_topk_candidates_guarded(index, so->query, so->curk, &cand);

	/*
	 * COMPLETENESS SIGNAL.  A pass whose top-k heap never filled scored and
	 * admitted EVERY matching document: the threshold stays 0.0 until the heap
	 * holds k entries, and every pruning rule in weave_search_bmw() and
	 * weave_search_maxscore() is gated on nheap >= k, so nothing was skipped.
	 * ncand < curk therefore means "this is the entire match set" -- an exact
	 * stop for the widening ladder, stronger than the weave_query_maxhits
	 * estimate it supplements.
	 */
	so->candfull = (ncand >= so->curk);

	if (ncand > 0 && so->nordered > 0)
	{
		ItemPointerData *seen;
		int			j = 0;

		seen = (ItemPointerData *)
			palloc(so->nordered * sizeof(ItemPointerData));	/* alloc-ok: one TID per row already materialized, bounded by the previous pass width, which allocated a wider array itself */
		for (i = 0; i < so->nordered; i++)
			seen[i] = so->ordered[i].tid;
		qsort(seen, so->nordered, sizeof(ItemPointerData), cmp_tid);
		for (i = 0; i < ncand; i++)
			if (bsearch(&cand[i].tid, seen, so->nordered,
						sizeof(ItemPointerData), cmp_tid) == NULL)
				cand[j++] = cand[i];
		ncand = j;
		pfree(seen);
	}

	if (so->cand)
		pfree(so->cand);
	so->cand = cand;
	so->ncand = ncand;
	so->candpos = 0;
}

/*
 * weave_ord_probe: extend so->ordered with MVCC-visible rows drawn from the
 * candidate list, until it holds `want` rows or the candidates run out.
 *
 * The candidate list is the exact top-N by score, so its visible members are
 * the exact top-M visible in the same order for every M it reaches: appending
 * them in candidate order keeps ordered[] a correct score-ordered prefix.
 */
static void
weave_ord_probe(Relation index, WeaveScanOpaque so, int want)
{
	Snapshot	snap = GetActiveSnapshot();
	Relation	heap;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;

	if (so->candpos >= so->ncand || so->nordered >= want)
		return;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);
#if PG_VERSION_NUM >= 190000
	fetch = table_index_fetch_begin(heap, SO_NONE);
#else
	fetch = table_index_fetch_begin(heap);
#endif
	slot = table_slot_create(heap, NULL);

	while (so->candpos < so->ncand && so->nordered < want)
	{
		ScoredTid  *c = &so->cand[so->candpos];
		ItemPointerData tid = c->tid;
		bool		call_again = false;
		bool		all_dead = false;

		so->candpos++;
		if (!table_index_fetch_tuple(fetch, &tid, snap, slot,
									 &call_again, &all_dead))
			continue;

		if (so->nordered >= so->maxordered)
		{
			int			newmax;

			if (so->maxordered == 0)
				/* size to the pass, but do not front-load a very wide one for a
				 * scan that may only pull a handful of rows */
				newmax = Min(Max(so->ncand, 64), 4096);
			else if (so->maxordered > INT_MAX / 2)
				newmax = INT_MAX;
			else
				newmax = so->maxordered * 2;
			if (so->ordered == NULL)
				so->ordered = (ScoredTid *)
					palloc(newmax * sizeof(ScoredTid));	/* alloc-ok: one entry per visible row materialized, bounded by the pass width, which allocated a same-sized array itself */
			else
				so->ordered = (ScoredTid *)
					repalloc(so->ordered, newmax * sizeof(ScoredTid));	/* alloc-ok: as above */
			so->maxordered = newmax;
		}
		so->ordered[so->nordered].tid = c->tid;
		/* ordering distance: 1/(1+score); ascending distance == descending score */
		so->ordered[so->nordered].score = 1.0 / (1.0 + c->score);
		so->nordered++;
	}

	ExecDropSingleTupleTableSlot(slot);
	table_index_fetch_end(fetch);
	table_close(heap, AccessShareLock);
}

/*
 * weave_ord_grow: widen the ranked pass because the executor wants more rows
 * than the current one can supply.  Returns false when the scan is complete.
 *
 * The ONLY correct stops are ones that PROVE no further match exists.  An
 * ORDER BY <=> index scan is an amcanorderbyop (KNN) scan and MUST be able to
 * return EVERY matching tuple in score order -- the executor's LIMIT bounds how
 * many are actually pulled, so the access method must not impose a ceiling of
 * its own (doing so silently truncated a query matching more than the ceiling to
 * that ceiling; see the "orderby distance scan undercounts" report).  Two proofs
 * are used, in this order:
 *
 *  1. !candfull -- the pass's top-k heap never filled, so it pruned nothing and
 *     its candidate list IS the complete match set.  Exact.
 *  2. curk >= maxhits -- the pass was at least as wide as the provable upper
 *     bound on the query's match count (weave_query_maxhits), so no wider pass
 *     can find more.  Nearly redundant now: at that width the heap cannot fill,
 *     so (1) fires anyway.  Kept as a cheap early stop.
 *
 * A broad query with a large (or no) LIMIT is inherently expensive here, but
 * correctness wins; a small-LIMIT first page is still served by the narrow first
 * pass alone.
 */
static bool
weave_ord_grow(Relation index, WeaveScanOpaque so)
{
	if (!so->candfull)
		return false;			/* the pass returned the complete match set */
	if ((double) so->curk >= so->maxhits)
		return false;			/* already as wide as every possible match */
	if (so->curk >= WEAVE_ORD_WIDTH_MAX)
		return false;			/* cannot widen further */

	so->curk = (so->curk > WEAVE_ORD_WIDTH_MAX / 4)
		? WEAVE_ORD_WIDTH_MAX : so->curk * 4;
	weave_ord_pass(index, so);
	return true;
}

/*
 * weave_topk_candidates_guarded: weave_topk_candidates_range over the whole
 * corpus, bracketed against the A1 race.
 *
 * Candidate generation reads segment pages under only per-page SHARE locks off a
 * metapage snapshot, so a concurrent merge/vacuum can free and recycle those
 * pages mid-scan.  Re-read the directory generation afterwards: if it moved the
 * candidates may have come off recycled pages, so discard them and redo from a
 * fresh snapshot, bounded at 10 attempts.  If every attempt races, return zero
 * candidates -- never a nonzero count with a NULL array, which is what the
 * inline copy of this loop this function replaced could do.
 */
static int
weave_topk_candidates_guarded(Relation index, WeaveQuery q, int wantk,
							 ScoredTid **out)
{
	int			gen_retries = 0;

	do
	{
		ScoredTid  *cand = NULL;
		uint32		gen0 = weave_read_meta_generation(index);
		int			ncand = weave_topk_candidates_range(index, q, wantk, 0,
														UINT64_MAX, &cand);

		if (weave_read_meta_generation(index) == gen0)
		{
			*out = cand;
			return ncand;
		}
		if (cand)
			pfree(cand);
	} while (gen_retries++ < 10);

	*out = NULL;
	return 0;
}

/*
 * weave_count_dictdf_fastpath: answer count(*) for a SINGLE plain positive term
 * straight from the dictionary df, with ZERO posting decode and ZERO heap
 * probe, when it is provably exact.  Returns the count, or -1 when any gate
 * fails (caller then takes the full weave_collect_matches path).
 *
 * A term's dictionary df is the number of documents that contained the term
 * AT INDEX TIME, summed here across all segments.  Sum(df) equals the count of
 * documents visible to the current snapshot ONLY under all of these gates --
 * each closes a way Sum(df) could diverge from the live/visible truth:
 *
 *  (1) The query is exactly one plain positive term: nitems==1, the item is a
 *      WEAVE_QI_VAL, and it carries none of PREFIX/FUZZY/REGEX (a prefix matches
 *      many dict terms, fuzzy/regex fan out via the trigram funnel -- none is a
 *      single df).  No operators means no AND/OR/NOT/PHRASE.  So the match set
 *      is exactly this one term's postings, i.e. exactly Sum(df) documents.
 *      (This term also never sets recheck, so the full path would be exact too.)
 *
 *  (2) Every segment has ndeleted==0 AND livedocslen==0 (no tombstones).  A
 *      tombstone marks an index posting whose heap doc was deleted; df still
 *      counts it, so ANY tombstone makes Sum(df) an OVERCOUNT.  Requiring zero
 *      tombstones everywhere means every counted posting is a doc that was
 *      never deleted-then-vacuumed.
 *
 *  (3) npending==0.  Newly inserted docs live verbatim in the pending list
 *      until merged; they are NOT in any segment df.  A nonzero pending list
 *      could add matches df does not see (undercount), so bail.
 *
 *  (4) The heap is ENTIRELY all-visible to this snapshot: every heap page is
 *      marked all-visible in the visibility map.  The VM all-visible bit is set
 *      only when every tuple on the page is visible to ALL snapshots (it is the
 *      same guarantee the full path relies on to skip a heap probe), so this is
 *      snapshot-safe -- not a stale pg_class heuristic.  This gate is what makes
 *      Sum(df) equal the VISIBLE count rather than the index-time count: with no
 *      tombstones (2) the index believes every posting doc is live, and an all-
 *      visible heap confirms every one of those docs is in fact visible now and
 *      none was deleted-but-not-yet-vacuumed (a dead tuple would leave its page
 *      NOT all-visible) nor inserted-after-build outside pending (also not all-
 *      visible).  Hence each of the Sum(df) postings is one visible document,
 *      one-to-one, and the count is exact.
 *
 * Any concurrent change to tombstones/pending/segments between our metapage
 * read and returning would invalidate the arithmetic; we re-check the directory
 * generation at the end (as the full path does) and bail to it on any change.
 *
 * Note: cost is one O(nblocks) VM scan + one dict lookup per segment, replacing
 * a decode of every posting of a common term.  Falls back on any doubt -- a
 * wrong count is worse than a slow one.
 */
static int64
weave_count_dictdf_fastpath(Relation index, WeaveQuery q)
{
	WeaveMetaPageData meta;
	WeaveQueryItem *it;
	const char *term;
	int			termlen;
	uint32		gen0;
	uint64		sumdf = 0;
	uint32		s;
	Relation	heap;
	BlockNumber nblocks;
	BlockNumber blk;
	Buffer		vmbuf = InvalidBuffer;
	bool		all_visible = true;

	/* Gate (1): exactly one plain positive term. */
	if (q == NULL || q->nitems != 1)
		return -1;
	it = &q->items[0];
	if (it->type != WEAVE_QI_VAL ||
		(it->flags & (WEAVE_QF_PREFIX | WEAVE_QF_FUZZY | WEAVE_QF_REGEX | WEAVE_QF_WEIGHTED)) != 0)
		return -1;				/* weighted (term:LABEL) needs the heap recheck, not Sum(df) */

	weave_read_meta(index, &meta);
	gen0 = meta.generation;

	/* Gate (3): no unmerged pending docs. */
	if (meta.npending != 0)
		return -1;

	/* Gate (2): no tombstones in any segment. */
	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
		if (meta.segs[s].ndeleted != 0 || meta.segs[s].livedocslen != 0)
			return -1;

	/* Sum the term's df across the segments where it is present. */
	term = WEAVE_QUERY_ITEMTEXT(q, it);
	termlen = (int) it->termlen;
	for (s = 0; s < meta.nsegments && s < WEAVE_MAX_SEGMENTS; s++)
	{
		WeaveSegMeta *sg = &meta.segs[s];
		uint32		df,
					max_tf,
					foff;
		BlockNumber fpost;

		if (sg->dictstart == InvalidBlockNumber)
			continue;
		if (weave_lookup_dict(index, sg, term, termlen,
							 &df, &max_tf, &fpost, &foff))
			sumdf += df;
	}

	/*
	 * Gate (4): the whole heap must be all-visible to this snapshot.  Scan the
	 * VM over every heap page; any page not marked all-visible aborts the fast
	 * path.  (RelationGetNumberOfBlocks is the current physical length; a page
	 * appended by a concurrent inserter is not all-visible, so it fails here or
	 * is caught by the generation re-check below.)
	 */
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	nblocks = RelationGetNumberOfBlocks(heap);
	for (blk = 0; blk < nblocks; blk++)
	{
		if (!VM_ALL_VISIBLE(heap, blk, &vmbuf))
		{
			all_visible = false;
			break;
		}
	}
	if (vmbuf != InvalidBuffer)
		ReleaseBuffer(vmbuf);
	table_close(heap, AccessShareLock);
	if (!all_visible)
		return -1;

	/*
	 * Concurrency: if the segment directory changed while we read it (a merge/
	 * vacuum could have added tombstones or folded pending docs), our gates and
	 * df sum may be stale.  Bail to the full path, which restarts on generation
	 * change itself.
	 */
	if (weave_read_meta_generation(index) != gen0)
		return -1;

	return (int64) sumdf;
}

/*
 * weave_count_visible: MVCC-correct count of documents matching `q`, computed in
 * bulk from the index without the per-tuple executor round-trips of an
 * index(-only) scan.  Collect the matching TIDs (sorted), then count those
 * visible to the snapshot: on an all-visible heap page (visibility map) the
 * whole run of TIDs counts without touching the heap; only pages the VM does
 * not mark all-visible are probed with table_index_fetch_tuple.  This is the
 * count-pushdown path the CustomScan uses to answer count(*) at index speed.
 * If `recheck` is set (fuzzy/regex/NOT/PHRASE over-generation), the collected
 * TIDs are a SUPERSET of the exact matches, so weave_recheck_exact() shrinks
 * them to the precise set (recomputing the heap wdoc and re-running @@@)
 * before the visibility count.  Without recheck the collected TIDs are already
 * exact and we only need visibility.
 */
static int64
weave_count_visible(Relation index, WeaveQuery q)
{
	TidSet		matches;
	bool		recheck;
	Snapshot	snap = GetActiveSnapshot();
	Relation	heap;
	IndexFetchTableData *fetch = NULL;
	Buffer		vmbuf = InvalidBuffer;
	int64		count = 0;
	int			i;

	/*
	 * The count pushdown (CustomScan), weave_count() and weave_count_visible_oid()
	 * all funnel through here and bypass the executor's index-scan machinery, so
	 * account for the scan explicitly: one index scan, and idx_tup_read = the
	 * matching index entries (like a bitmap index scan reports its bitmap size).
	 */
	pgstat_count_index_scan(index);

	/*
	 * Fast path: a single plain term over a fully-visible, tombstone-free,
	 * pending-free index is counted from the dictionary df alone -- no posting
	 * decode, no heap probe.  Returns -1 (fall through) on any doubt.
	 */
	{
		int64		fast = weave_count_dictdf_fastpath(index, q);

		if (fast >= 0)
		{
			pgstat_count_index_tuples(index, fast);
			return fast;
		}
	}

	weave_collect_matches(index, q, &matches, &recheck);
	/*
	 * Shrink over-generated sets (fuzzy/regex/PHRASE/NEAR) to the exact @@@
	 * match set against the heap wdoc; after this the count is precise.
	 */
	if (recheck)
		weave_recheck_exact(index, q, &matches);
	pgstat_count_index_tuples(index, matches.n);
	if (matches.n == 0)
		return 0;

	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	/*
	 * The collected TIDs are now exactly the matches (over-generation already
	 * rechecked above), so we only need visibility.  Count visible via the VM,
	 * heap-probing only pages the map does not mark all-visible.
	 */
	for (i = 0; i < matches.n; i++)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&matches.tids[i]);

		if (VM_ALL_VISIBLE(heap, blk, &vmbuf))
		{
			/* whole page visible: this TID counts, no heap access */
			count++;
			continue;
		}
		/* page not all-visible: probe the heap for this TID's visibility */
		if (fetch == NULL)
		{
#if PG_VERSION_NUM >= 190000
			fetch = table_index_fetch_begin(heap, SO_NONE);
#else
			fetch = table_index_fetch_begin(heap);
#endif
		}
		{
			ItemPointerData tid = matches.tids[i];
			bool		ca = false,
						ad = false;
			TupleTableSlot *slot = table_slot_create(heap, NULL);

			if (table_index_fetch_tuple(fetch, &tid, snap, slot, &ca, &ad))
				count++;
			ExecDropSingleTupleTableSlot(slot);
		}
	}
	if (fetch != NULL)
		table_index_fetch_end(fetch);
	if (vmbuf != InvalidBuffer)
		ReleaseBuffer(vmbuf);
	table_close(heap, AccessShareLock);
	return count;
}

/*
 * weave_count_visible_oid: same as weave_count() but callable from C with an index
 * OID (used by the COUNT-pushdown CustomScan).  Opens the index under
 * AccessShareLock, counts, closes.
 */
int64
weave_count_visible_oid(Oid indexoid, WeaveQuery q)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	int64		c = weave_count_visible(index, q);

	index_close(index, AccessShareLock);
	return c;
}

PG_FUNCTION_INFO_V1(weave_count);

/* weave_count(regclass, wquery) -> bigint : MVCC-correct count via the index */
Datum
weave_count(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	WeaveQuery	q = PG_GETARG_WQUERY(1);
	Relation	index;
	int64		c;

	index = index_open(indexoid, AccessShareLock);
	c = weave_count_visible(index, q);
	index_close(index, AccessShareLock);
	PG_RETURN_INT64(c);
}

PG_FUNCTION_INFO_V1(weave_search);

Datum
weave_search(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ScoredTid  *results;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			indexoid = PG_GETARG_OID(0);
		WeaveQuery	q = PG_GETARG_WQUERY(1);
		int			k = PG_GETARG_INT32(2);
		MemoryContext oldctx;
		Relation	index;
		int			nvis;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		index = index_open(indexoid, AccessShareLock);
		/* This native top-k bypasses the executor's index-scan machinery; count
		 * the scan and the returned index entries ourselves. */
		pgstat_count_index_scan(index);
		nvis = weave_topk_visible(index, q, k, false, &results);
		pgstat_count_index_tuples(index, nvis);
		index_close(index, AccessShareLock);

		funcctx->max_calls = nvis;
		funcctx->user_fctx = results;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	results = (ScoredTid *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};
		HeapTuple	tuple;
		ItemPointer tidcopy = palloc(sizeof(ItemPointerData));

		*tidcopy = results[funcctx->call_cntr].tid;
		values[0] = PointerGetDatum(tidcopy);
		values[1] = Float8GetDatum(results[funcctx->call_cntr].score);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ----- lexical anomaly detection: rare-term (low-df) dictionary tail ----- */

/* term-df hash key width; matches the build side's WEAVE_TERMKEYLEN ceiling */
#define WEAVE_ANOM_TERMKEYLEN 64

/*
 * weave_anomalous_docs(index regclass, k int, max_df int)
 *   -> setof (ctid tid, score float8, rarest_term text, min_df int)
 *
 * Surface the top-k most lexically-anomalous documents: those containing
 * globally rare terms.  A document's anomaly score is the MAX idf over its
 * terms -- i.e. it is driven by the document's single rarest term.  Because the
 * rarest terms have the SHORTEST posting lists, this is answered by walking the
 * LOW-df tail of the dictionary and emitting those few documents; the vast bulk
 * of the dictionary (common, high-df terms) is skipped before any posting is
 * decoded, so it is cheap and NOT a full-corpus scan.
 *
 * idf = log(1 + (N - df + 0.5)/(df + 0.5)), the same rarity value BM25 uses.
 * `df` is the GLOBAL document frequency: a term is summed across all segments
 * (segments share one corpus) before its idf is computed, so a doc split across
 * two segments is not made to look artificially rare.
 *
 * `max_df` caps which terms count as "rare": only terms with global df <=
 * max_df contribute.  When NULL (-1 from the strict SQL wrapper's default) it
 * defaults to max(N/1000, 1) -- a small fraction of the corpus, keeping the
 * walk on the low-df tail.
 *
 * MVCC: the returned ctids are index-resident heap pointers (like weave_search);
 * this is an analytic/heuristic result, not a query result, so no per-doc heap
 * visibility check is done -- the caller should join ctid back to the table and
 * filter for visibility if needed.  Per-segment tombstones ARE honored so
 * deleted docs are not reported as anomalies.
 */

/* term -> summed-across-segments df; key is the NUL-terminated term text.
 * Note: a term longer than WEAVE_ANOM_TERMKEYLEN-1 is truncated for the df
 * key (same ceiling the build side takes at 64); two such terms sharing that
 * prefix would share a df bucket -> slightly conservative rarity.  Widen the
 * key or chain (like add_posting) if long distinct tokens must rank exactly. */
typedef struct AnomTermDf
{
	char		term[WEAVE_ANOM_TERMKEYLEN]; /* dynahash string key */
	uint64		gdf;			/* global df, summed across segments (uint64 so a
								 * term in >2^32 docs does not wrap) */
} AnomTermDf;

/* per-document running max: its best (rarest) term's idf and that term's df */
typedef struct AnomDoc
{
	uint64		docid;			/* dynahash blob key */
	double		score;			/* best idf seen for this doc */
	uint32		min_df;			/* the driving term's global df */
	char	   *rarest_term;	/* palloc'd copy of the driving term */
	int			rarest_len;
}			AnomDoc;

typedef struct AnomResult
{
	ItemPointerData tid;
	double		score;
	char	   *rarest_term;
	int			rarest_len;
	int32		min_df;
}			AnomResult;

static int
cmp_anom_desc(const void *a, const void *b)
{
	double		sa = ((const AnomResult *) a)->score;
	double		sb = ((const AnomResult *) b)->score;

	if (sa < sb)
		return 1;
	if (sa > sb)
		return -1;
	return 0;
}

PG_FUNCTION_INFO_V1(weave_anomalous_docs);

Datum
weave_anomalous_docs(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AnomResult *results;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			indexoid;
		int			k = PG_ARGISNULL(1) ? 100 : PG_GETARG_INT32(1);
		/* max_df NULL -> auto (small fraction of N); the func is non-STRICT so a
		 * NULL default reaches C rather than short-circuiting to no rows */
		bool		max_df_null = PG_ARGISNULL(2);
		int			max_df_arg = max_df_null ? -1 : PG_GETARG_INT32(2);
		MemoryContext oldctx;
		Relation	index;
		WeaveMetaPageData meta;
		WeaveTombstones tombs;
		TupleDesc	tupdesc;
		HTAB	   *dfht;		/* term -> global df */
		HTAB	   *docht;		/* docid -> AnomDoc */
		HASHCTL		ctl;
		double		N;
		int			max_df;
		uint32		s;
		HASH_SEQ_STATUS seq;
		AnomDoc    *dslot;
		int			ndocs_found;
		int			nout;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		if (PG_ARGISNULL(0))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("index argument must not be null")));
		indexoid = PG_GETARG_OID(0);

		if (k <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("k must be positive")));

		index = index_open(indexoid, AccessShareLock);
		if (index->rd_rel->relam != get_index_am_oid("weave", true))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a weave index",
							RelationGetRelationName(index))));
		weave_read_meta(index, &meta);
		N = meta.ndocs;

		/* effective max_df: arg, else a small fraction of N (floor 1) */
		if (!max_df_null && max_df_arg >= 0)
			max_df = max_df_arg;
		else
			max_df = Max((int) (N / 1000.0), 1);

		/* empty index (or nothing to rank): return no rows */
		if (N <= 0 || meta.nsegments == 0)
		{
			index_close(index, AccessShareLock);
			funcctx->max_calls = 0;
			MemoryContextSwitchTo(oldctx);
			funcctx = SRF_PERCALL_SETUP();
			SRF_RETURN_DONE(funcctx);
		}

		weave_tombstones_load(index, &meta, &tombs);

		/*
		 * Pass 1 -- walk every segment's DICTIONARY (no postings decoded) and
		 * sum df per distinct term text into dfht.  This is cheap: dict pages
		 * only, one entry per distinct term.  It gives the true global df so a
		 * term appearing in several segments is not mistaken for rarer than it
		 * is.
		 */
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = WEAVE_ANOM_TERMKEYLEN;
		ctl.entrysize = sizeof(AnomTermDf);
		ctl.hcxt = CurrentMemoryContext;
		dfht = hash_create("weave anomaly term df", 4096, &ctl,
						   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

		for (s = 0; s < meta.nsegments; s++)
		{
			BlockNumber blk = meta.segs[s].dictstart;

			while (blk != InvalidBlockNumber)
			{
				Buffer		buffer;
				Page		page;
				char	   *ptr,
						   *end;
				BlockNumber next;

				CHECK_FOR_INTERRUPTS();	/* between dict pages, no buffer lock held: safe to unwind */
				buffer = weave_scan_readbuf(index, blk);
				if (buffer == InvalidBuffer)
					break;	/* block truncated by a concurrent weave_vacuum: end of chain */
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
				page = BufferGetPage(buffer);
				ptr = (char *) PageGetContents(page);
				end = (char *) page + ((PageHeader) page)->pd_lower;
				next = WeavePageGetOpaque(page)->nextblk;

				while (ptr < end)
				{
					WeaveDictEntry *de = (WeaveDictEntry *) ptr;
					Size		esize = MAXALIGN(offsetof(WeaveDictEntry, term) +
												 de->termlen);
					char		key[WEAVE_ANOM_TERMKEYLEN];
					int			klen = Min((int) de->termlen,
										   WEAVE_ANOM_TERMKEYLEN - 1);
					AnomTermDf *te;
					bool		found;

					memcpy(key, de->term, klen);
					key[klen] = '\0';
					te = (AnomTermDf *) hash_search(dfht, key, HASH_ENTER,
													&found);
					if (!found)
						te->gdf = 0;
					te->gdf += de->df;
					ptr += esize;
				}
				UnlockReleaseBuffer(buffer);
				blk = next;
			}
		}

		/*
		 * Pass 2 -- walk the dicts again; for terms whose GLOBAL df <= max_df
		 * (the rare tail only), decode their (few) postings and keep, per
		 * document, the maximum idf seen (the doc's rarest term).  Common terms
		 * are skipped BEFORE any posting decode -- this is the cheap-tail
		 * filter that keeps the whole thing off a full-corpus scan.
		 */
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(AnomDoc);
		ctl.hcxt = CurrentMemoryContext;
		docht = hash_create("weave anomaly docs", 4096, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		for (s = 0; s < meta.nsegments; s++)
		{
			BlockNumber blk = meta.segs[s].dictstart;

			while (blk != InvalidBlockNumber)
			{
				Buffer		buffer;
				Page		page;
				char	   *ptr,
						   *end;
				BlockNumber next;

				CHECK_FOR_INTERRUPTS();	/* between dict pages, no buffer lock held: safe to unwind */
				buffer = weave_scan_readbuf(index, blk);
				if (buffer == InvalidBuffer)
					break;	/* block truncated by a concurrent weave_vacuum: end of chain */
				LockBuffer(buffer, BUFFER_LOCK_SHARE);
				page = BufferGetPage(buffer);
				ptr = (char *) PageGetContents(page);
				end = (char *) page + ((PageHeader) page)->pd_lower;
				next = WeavePageGetOpaque(page)->nextblk;

				while (ptr < end)
				{
					WeaveDictEntry *de = (WeaveDictEntry *) ptr;
					Size		esize = MAXALIGN(offsetof(WeaveDictEntry, term) +
												 de->termlen);
					char		key[WEAVE_ANOM_TERMKEYLEN];
					int			klen = Min((int) de->termlen,
										   WEAVE_ANOM_TERMKEYLEN - 1);
					AnomTermDf *te;
					uint64		gdf;
					double		idf;
					WeavePosting *post;
					int			np,
								j;

					memcpy(key, de->term, klen);
					key[klen] = '\0';
					te = (AnomTermDf *) hash_search(dfht, key, HASH_FIND, NULL);
					gdf = te ? te->gdf : de->df;

					/* THE cheap-tail filter: skip common terms before decode.
					 * Compare as unsigned against the (non-negative) max_df so a
					 * huge gdf cannot cast to a negative int and slip past the
					 * filter (max_df is >= 1 here). */
					if (gdf == 0 || gdf > (uint64) max_df)
					{
						ptr += esize;
						continue;
					}

					idf = log(1.0 + (N - (double) gdf + 0.5) /
							  ((double) gdf + 0.5));

					np = weave_decode_term(index, de->firstposting,
										  de->firstoffset, de->df,
										  &post, NULL, false, NULL, true,
										  meta.segs[s].doclenstart == InvalidBlockNumber);
					for (j = 0; j < np; j++)
					{
						uint64		docid = weave_tid_to_docid(&post[j].tid);
						AnomDoc    *doc;
						bool		found;

						/* skip docs tombstoned in THIS segment */
						if (tombs.hasany && tombs.present[s] &&
							sm_contains(&tombs.maps[s], docid, NULL))
							continue;

						doc = (AnomDoc *) hash_search(docht, &docid, HASH_ENTER,
													  &found);
						if (!found || idf > doc->score)
						{
							if (!found)
								doc->docid = docid;
							doc->score = idf;
							doc->min_df = gdf;
							doc->rarest_term = (char *) palloc(de->termlen + 1);
							memcpy(doc->rarest_term, de->term, de->termlen);
							doc->rarest_term[de->termlen] = '\0';
							doc->rarest_len = de->termlen;
						}
					}
					pfree(post);
					ptr += esize;
				}
				UnlockReleaseBuffer(buffer);
				blk = next;
			}
		}

		weave_tombstones_free(&tombs);
		index_close(index, AccessShareLock);

		/* collect the rare-tail doc set and take the top-k by score */
		ndocs_found = (int) hash_get_num_entries(docht);
		results = (AnomResult *) palloc(Max(ndocs_found, 1) *	/* alloc-ok: rare-tail result set (low-df anomaly scan) */
										sizeof(AnomResult));
		nout = 0;
		hash_seq_init(&seq, docht);
		while ((dslot = (AnomDoc *) hash_seq_search(&seq)) != NULL)
		{
			weave_docid_to_tid(dslot->docid, &results[nout].tid);
			results[nout].score = dslot->score;
			results[nout].min_df = (int32) dslot->min_df;
			results[nout].rarest_term = dslot->rarest_term;
			results[nout].rarest_len = dslot->rarest_len;
			nout++;
		}

		qsort(results, nout, sizeof(AnomResult), cmp_anom_desc);
		if (nout > k)
			nout = k;

		funcctx->max_calls = nout;
		funcctx->user_fctx = results;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	results = (AnomResult *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		AnomResult *r = &results[funcctx->call_cntr];
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};
		HeapTuple	tuple;
		ItemPointer tidcopy = palloc(sizeof(ItemPointerData));

		*tidcopy = r->tid;
		values[0] = PointerGetDatum(tidcopy);
		values[1] = Float8GetDatum(r->score);
		values[2] = PointerGetDatum(cstring_to_text_with_len(r->rarest_term,
															 r->rarest_len));
		values[3] = Int32GetDatum(r->min_df);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}

