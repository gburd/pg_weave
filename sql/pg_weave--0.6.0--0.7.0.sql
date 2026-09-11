/* pg_weave 0.6.0 -> 0.7.0
 *
 * Format v7: a bolt carries the FUZZY weft -- the LOUDS-Sparse SuRF trie over its
 * own vocabulary, on a WEAVE_PK_SURF page chain, registered as a WEAVE_WK_FUZZY
 * descriptor on the bolt's existing WEAVE_CHANDESC page.  Task Z3;
 * doc/specs/FUZZY_CHANNEL.md section 3, doc/specs/SEGMENT_FORMAT.md sections 2, 6 and 9.
 *
 * NO REINDEX IS REQUIRED and this script rewrites nothing.  WeaveSegMeta gained
 * no field: the whole point of the v6 descriptor page is that a new weft's root
 * is recorded in the bolt's self-description, so a v3/v4/v5/v6 bolt keeps saying
 * exactly what it said before ("I have no fuzzy weft") and costs zero bytes for
 * it.  Bolts written after the upgrade carry a trie; one relation carries both,
 * which is the same per-object dual-read every format change since v3 has used.
 *
 * WHY THE VERSION BUMPED WHEN NO STRUCT CHANGED.  A 0.6.0 .so understands the
 * descriptor page and would read such a bolt happily -- but weave_free_segment()
 * in that build frees only the wefts it knows about, so every merge would leak
 * the entire trie, unreachable and unflagged, reclaimable by nothing short of a
 * REINDEX.  doc/specs/SEGMENT_FORMAT.md section 8 item 2 used exactly this
 * argument for v5 -> v6 ("that binary would read chandesc as padding and leak one
 * page per merged bolt"), and the conclusion is the same: refusing is correct.
 * weave_check_meta() is the gate, with the REINDEX hint.
 *
 * WHAT THIS DOES NOT DO: route any query through the trie.  Prefix (Z4), fuzzy
 * (Z5) and regex (Z6) are separate tasks.  What 0.7.0 delivers is the structure
 * on disk, verified by weave_check(), and loadable.
 */

-- weave_surf_stats(): what each bolt's fuzzy weft actually costs, and the only
-- SQL-reachable caller of the trie LOADER (weave_surf_load in src/am/am.c).
--
-- Both halves of that matter.  A size report that leaves a new structure's bytes
-- unattributed is how a structure gets big unnoticed, so `bytes` here is the
-- exact image length and weave_index_size_detail() gained a 'surf_trie' bucket
-- for the pages it occupies.  And an ERROR path with no caller is an ERROR path
-- that has never run: t/011_chandesc_corruption.pl records that
-- weave_chandesc_required() shipped with zero callers and was therefore
-- reachable only from a C call nothing in the tree made.  This function is what
-- lets t/013_surf_corruption.pl reach the loader's refusal through a real
-- backend call, the same way an eventual prefix scan will.
CREATE FUNCTION weave_surf_stats(idx regclass)
RETURNS TABLE (bolt int, nterms bigint, nslots bigint, nnodes bigint,
               nterminal bigint, ntrunc bigint, maxdepth int,
               bytes bigint, npages bigint)
AS 'MODULE_PATHNAME', 'weave_surf_stats'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_surf_stats(regclass) IS
    'per-bolt shape and byte cost of the fuzzy (SuRF) weft; ERRORs on a corrupt trie';
