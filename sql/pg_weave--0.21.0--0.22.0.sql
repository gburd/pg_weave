/* pg_weave 0.21.0 -> 0.22.0 */

-- weave_index_stats() gains ndeleted: the tombstoned document count, summed over live
-- segments.  doc/GAPS.md G47.
--
-- WHY A GUARD NEEDED IT BEFORE THE GUARD WAS WRITTEN.  G47's call adopted a predictive
-- fourth term for weave_index_is_compacted(): compute, from the free space map, what size a
-- low-bias pack would leave the index at, and decline the pass when it would not shrink.
-- The formula predicted 6 of 6 steady states EXACTLY -- and then mispredicted by 409 pages,
-- in the direction that skips a pass which would have reclaimed 11.2 %, on the one state
-- where the rewrite physically drops tombstoned postings and the live page count therefore
-- changes mid-pass.  So the guard is only sound when there are no tombstones to drop, and
-- NOTHING REACHABLE FROM SQL COULD SEE THAT: ndocs already has ndeleted subtracted.
--
-- A guard whose precondition cannot be asserted in a test is the same hazard as a
-- diagnostic GUC that does not exist in the build -- it reads as enabled and examines
-- nothing (AGENTS.md, twelfth member).  The number is therefore surfaced BEFORE the guard
-- is written, so the guard's control can be written against it.
--
-- It is also the quantity pg_weave.vacuum_tombstone_frac is compared against, which users
-- have been asked to tune with no way to see either side of the comparison.
--
-- DROP + CREATE, for the fourth time and for the same reason the 0.10->0.11, 0.12->0.13 and
-- 0.20->0.21 scripts give: a function whose whole result is OUT parameters has those
-- parameters AS ITS RETURN TYPE, and PostgreSQL will not change an existing function's
-- return type.  weave_index_stats() takes an argument as well, so the signature to drop is
-- the argument list, not an empty one.
DROP FUNCTION weave_index_stats(regclass);

CREATE FUNCTION weave_index_stats(regclass,
                                OUT ndocs float8, OUT avgdl float8,
                                OUT nterms bigint, OUT ndeleted float8)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_index_stats'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_index_stats(regclass) IS
    'corpus statistics from the index metapage: live documents, average document length, distinct terms, and tombstoned documents awaiting a rewrite';
