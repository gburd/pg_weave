/* pg_weave 0.5.0 -> 0.6.0
 *
 * Format v6: per-bolt channel descriptors, and the end of the flat page-kind
 * bitmap.  See doc/specs/SEGMENT_FORMAT.md sections 2, 6, 8 and 9.
 *
 * NO REINDEX IS REQUIRED and this script rewrites nothing.  A v3/v4/v5 index
 * stays readable: the metapage is read through a versioned reader
 * (weave_meta_from_page) that synthesizes chandesc = InvalidBlockNumber, meaning
 * "this bolt carries only the lexical weft", which is exactly what a pre-v6 bolt
 * is.  Bolts written after the upgrade get a WEAVE_CHANDESC page each, so one
 * relation carries both -- the same per-object dual-read the v4 doclen sidecar
 * and the v5 sidecar-encoding flag already use.
 *
 * The page-kind space stops being a flat uint16 bitmap in the same break: new
 * kinds are integer ids in WeavePageOpaqueData.kind, selected by a reserved
 * escape bit in `flags`.  The ten shipped kinds keep their one-hot bits, so
 * lexical pages written by 0.6.0 are byte-identical to those written by 0.5.0.
 *
 * What an OLDER pg_weave does with a v6 index: refuses it.  weave_check_meta()
 * gates on version <= WEAVE_VERSION and errors with a REINDEX hint.  That is the
 * intended behaviour -- an older binary would not free channel-descriptor pages
 * on merge, so it would leak one page per merged bolt.
 */

-- weave_check(): mechanical verification of the doc/specs/SEGMENT_FORMAT.md
-- section 9 invariants.  One row per invariant rather than an ERROR on the first
-- violation, so a corruption test can assert that a specific injected fault is
-- detected and an operator gets the whole list.
--
-- `deep` adds the O(relation) checks: full-relation page reachability and the
-- no-two-chains-overlap assertion.  Off by default because it walks every chain
-- and every page.
CREATE FUNCTION weave_check(idx regclass, deep boolean DEFAULT false)
RETURNS TABLE (invariant text, ok boolean, detail text)
AS 'MODULE_PATHNAME', 'weave_check'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_check(regclass, boolean) IS
    'verify the on-disk invariants of a weave index; one row per invariant';
