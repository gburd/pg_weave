/* pg_weave 0.3.0 -> 0.4.0
 *
 * Task L10: weave_index_size_detail(), the prerequisite for L8 (deterministic
 * index size) and G2 (the 1.7-1.9x size gap against tsvector+GIN).
 *
 * The gap cannot be attacked by guessing which structure is fat: some of it is
 * the unavoidable price of storing what BM25 needs, and some of it is per-segment
 * overhead multiplied by the segment count. This tells them apart by counting.
 */

\echo Use "ALTER EXTENSION pg_weave UPDATE TO '0.4.0'" to load this file. \quit

CREATE FUNCTION weave_index_size_detail(regclass)
RETURNS TABLE (kind text, npages bigint, bytes bigint, pct float8,
               free_bytes bigint, free_pct float8)
AS 'MODULE_PATHNAME', 'weave_index_size_detail'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_index_size_detail(regclass) IS
    'per-page-kind byte breakdown of a weave index; sums to pg_relation_size';
