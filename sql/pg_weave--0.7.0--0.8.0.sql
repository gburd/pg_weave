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
