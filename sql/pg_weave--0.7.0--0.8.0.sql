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
