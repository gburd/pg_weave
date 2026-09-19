/* pg_weave 0.9.0 -> 0.10.0 */

-- Z4 part 2: the SuRF vocabulary trie is RESIDENT, and four columns say so.
--
-- WHAT CHANGED UNDERNEATH.  A fuzzy weft's trie image costs about 5.52 bytes per
-- vocabulary term -- ~11 MB per bolt at a 2M-term vocabulary -- and until now every
-- consult reassembled the entire image from its page chain into one contiguous
-- palloc, with no reuse whatsoever.  Nothing in production paid that per query,
-- because the only SQL-reachable caller was the weave_surf_stats() diagnostic.  The
-- reason it had to be fixed first is that every remaining Z-phase task (prefix
-- routing, trie-accelerated fuzzy, regex tiling) consults the trie at QUERY time,
-- and none of them is measurable -- let alone shippable -- while one consult costs a
-- whole image.  There is now a bounded, backend-local cache of opened images keyed
-- by (relfilenode, weft root block, metapage generation), sized by
-- pg_weave.surf_cache_mb (default 32, 0 = disabled, which restores the old
-- behaviour exactly).
--
-- WHY surf_loads AND surf_bytes DID NOT CHANGE MEANING.  They still count
-- WHOLE-IMAGE loads and their bytes; a cache hit does not touch them.  That
-- separation is the entire measurement: "the trie is resident" is the claim that
-- consults outnumber loads, and it is unreadable if a hit also counts as a load.
--
-- surf_cache_bytes IS A GAUGE, NOT A COUNTER: bytes resident right now.
-- weave_channel_stats_reset() leaves it alone on purpose -- zeroing it would report
-- 0 resident bytes while the backend still holds the memory, which is the same class
-- of false zero the first draft of these counters shipped with.
--
-- AN IMAGE LARGER THAN THE WHOLE BUDGET IS NOT CACHED: it is loaded, used and
-- freed, since caching it would evict everything else to hold something that cannot
-- be kept.  From SQL that case reads as misses climbing while surf_cache_bytes stays
-- at zero.
--
-- WHY DROP + CREATE rather than CREATE OR REPLACE.  A function whose whole result is
-- OUT parameters has those parameters as its return type, and PostgreSQL refuses to
-- change the return type of an existing function -- so adding a column to
-- weave_channel_stats() cannot be done in place.  Nothing in the extension depends on
-- it (no view, no default, no other function), so the drop is safe; a user who has
-- built a view over it will see the dependency error here rather than a silently
-- changed result shape, which is the right failure.
DROP FUNCTION weave_channel_stats();

CREATE FUNCTION weave_channel_stats(
        OUT lex_term bigint,
        OUT prefix_dict bigint,
        OUT prefix_surf bigint,
        OUT fuzzy_dict bigint,
        OUT fuzzy_trgm bigint,
        OUT fuzzy_surf bigint,
        OUT regex_trgm bigint,
        OUT regex_surf bigint,
        OUT vector_scan bigint,
        OUT terms_expanded bigint,
        OUT dict_pages bigint,
        OUT surf_loads bigint,
        OUT surf_bytes bigint,
        OUT surf_cache_hits bigint,
        OUT surf_cache_misses bigint,
        OUT surf_cache_evicts bigint,
        OUT surf_cache_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_channel_stats'
LANGUAGE C PARALLEL RESTRICTED;

COMMENT ON FUNCTION weave_channel_stats() IS
    'which mechanism inside a weave index served this backend''s query leaves';
