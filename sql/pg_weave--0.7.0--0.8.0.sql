/* pg_weave 0.7.0 -> 0.8.0 */

-- Allocator outcome counters, so a bloat or reclaim investigation can find out
-- WHICH of the three allocation paths ran instead of inferring it.
--
-- Every page pg_weave allocates comes from the compaction low-bias list, the free
-- space map, or a relation extension.  Two separate investigations in this
-- codebase's lineage stalled on not knowing which, and both spent a cycle acting
-- on a guess: one asserted that the page-recycle XID gate was blocking reuse under
-- a share lock, the other that a per-page WAL record was the cost of freeing a
-- segment.  Both were plausible and neither was measured.
--
-- The counters distinguish the cases that reasoning cannot: a high `extend` with a
-- high `fsm_defer` means the recycle gate really is the constraint, while a high
-- `extend` with `fsm_defer = 0` means the free list was never consulted at all --
-- a completely different bug with a completely different fix.
--
-- Backend-local and cumulative since backend start or the last reset. Not
-- per-index: the allocator's state is backend-scoped, so a measurement run should
-- touch one index in one session.
CREATE FUNCTION weave_alloc_stats(OUT lowfree_reuse bigint,
                                  OUT lowfree_defer bigint,
                                  OUT lowfree_contended bigint,
                                  OUT fsm_reuse bigint,
                                  OUT fsm_defer bigint,
                                  OUT fsm_contended bigint,
                                  OUT extend bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_alloc_stats'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Zero this backend's allocator counters, so a measurement can bracket one
-- operation rather than reporting everything since connect.
CREATE FUNCTION weave_alloc_stats_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'weave_alloc_stats_reset'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- What a page IS: one row per page, or one row for one page.
--
-- weave_check(deep) reports that a page is leaked and cannot report what it is,
-- which is the whole diagnosis: the invariant row carries a count and a first
-- offender, and a leaked page's KIND is what names the write path that left it.
-- doc/GAPS.md G21 stalled on that missing reading for two days -- the extension had
-- no page-level introspection of any kind, so the alternatives were guessing at
-- crash sequences or decoding a page header by hand out of a data directory.
--
-- `blkno => NULL` (the default) dumps every page.  A leak is the row where
-- reachable IS false AND freed IS false AND uninitialized IS false; `kind` names
-- its writer, and `lsn` compared against the recovery end LSN says whether it was
-- written by the operation under suspicion or after it.
--
-- The reachability walk is O(relation) and runs even for a single block, because a
-- `reachable` column that is sometimes NULL is a column that gets misread.
--
-- kind/flags/kind_id/freed/nextblk/free_bytes are NULL when the page has no
-- weave-sized special area -- an uninitialized, torn, or foreign page. Reading the
-- opaque struct on a zero page would report the first bytes of nothing as flags.
CREATE FUNCTION weave_page_info(idx regclass, blkno bigint DEFAULT NULL)
RETURNS TABLE (blkno bigint, kind text, flags integer, kind_id integer,
               freed boolean, uninitialized boolean, reachable boolean,
               nextblk bigint, lsn pg_lsn, free_bytes integer)
AS 'MODULE_PATHNAME', 'weave_page_info'
LANGUAGE C PARALLEL SAFE;

COMMENT ON FUNCTION weave_page_info(regclass, bigint) IS
    'per-page kind, header flags, freed/reachable state and LSN of a weave index';

-- Not STRICT above: blkno IS NULL is the "every page" case, and a STRICT function
-- would return the empty set for it.
--
-- REVOKEd because index_open() performs no ACL check, so any user could otherwise
-- enumerate the page structure of any index in the database. No indexed content is
-- exposed, but this is pageinspect's shape and pageinspect's precedent applies.
REVOKE ALL ON FUNCTION weave_page_info(regclass, bigint) FROM PUBLIC;

-- The vector channel's operator class, and the reason it declares no operators.
--
-- Task V7 makes the access method multicolumn so that
--
--     CREATE INDEX ON docs USING weave (body wdoc_lex_ops, embedding wvec_weave_ops)
--
-- is one index, one WAL stream, one vacuum over one docid space -- claim 1 in
-- doc/ARCHITECTURE.md sect. 9. The access method routes each column to a channel by
-- its operator class, NOT by the column's type: doc/specs/FUZZY_CHANNEL.md declares
-- the corpus n-gram channel as `USING weave (sku gram_ops)` over an ordinary text
-- column, so two channels will share one input type and the type cannot be the
-- discriminator. That is why a wvec column needs an opclass of its own even before
-- anything can query it.
--
-- STORAGE-only, with no operator members, is deliberate: V7 is the storage half and
-- V8 is the scan. An opclass that advertised `<=>` before the access method could
-- execute a vector ordering would make the planner build index paths that fail at
-- run time -- on the query shape every pgvector user writes first. V8 adds the
-- members with ALTER OPERATOR FAMILY, next to the code that serves them.
--
-- Named wvec_weave_ops, not wvec_ops: the type already has a btree wvec_ops from
-- 0.2.0 for ordinary sorting, and the two belong to different access methods.
CREATE OPERATOR CLASS wvec_weave_ops DEFAULT FOR TYPE wvec USING weave AS
    STORAGE wvec;

COMMENT ON OPERATOR CLASS wvec_weave_ops USING weave IS
    'index a wvec column as the vector channel of a weave index (storage only until V8)';

-- The vector weft's geometry, per bolt, as a READER sees it (task V7).
--
-- The geometry comes off the WEAVE_VMETA page, not out of the `bits` reloption: a
-- bolt records the width it was built at, so changing the reloption leaves existing
-- bolts alone and this function is where that divergence is visible.  Without it
-- there is no way to assert from SQL that the weft was written at the width that was
-- asked for -- a weft at the wrong width scores wrongly and counts correctly.
CREATE FUNCTION weave_vec_meta(idx regclass)
RETURNS TABLE (segno integer, root bigint, dim integer, bits integer,
               metric integer, layout integer, nvec bigint, nblocks bigint,
               dirstart bigint, codestart bigint)
AS 'MODULE_PATHNAME', 'weave_vec_meta'
LANGUAGE C STRICT PARALLEL SAFE;

-- One row per 32-lane block, read through the same O(1) directory addressing a scan
-- uses.
--
-- `livemask` is the bit per lane that says whether that warp position has a vector,
-- and it is the only place a NULL vector is visible: a writer that SKIPPED a NULL
-- instead of leaving a dead lane produces identical row counts and associates every
-- later vector with the wrong document.  The five float columns are the (C2) bound
-- inputs; weave_check() recomputes them from the stored codes, and this function is
-- how a human reads them when it reports a mismatch.
CREATE FUNCTION weave_vec_blocks(idx regclass)
RETURNS TABLE (segno integer, blockno bigint, firstwarp bigint, nlanes integer,
               nlive integer, livemask bigint, smax real, maxrecnorm real,
               minnorm real, censcale real, cenrad real)
AS 'MODULE_PATHNAME', 'weave_vec_blocks'
LANGUAGE C STRICT PARALLEL SAFE;
