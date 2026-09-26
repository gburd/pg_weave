/* pg_weave 0.23.0 -> 0.24.0 */

-- P3: `int8_docval_ops`, the SCALAR / DOCVALUES channel's first operator class.
--
-- WHAT IT IS.  A column joins the docvalues channel (WEAVE_WK_DOCVALS, spec
-- doc/specs/DOCVALS_CHANNEL.md sect. 4) by wearing this opclass: an int8 facet
-- stored as a dense per-docid array so a `WHERE price <op> const` restriction can
-- be evaluated over stored values into a gate set, the way a selective WHERE makes
-- a fused vector query FASTER instead of collapsing its recall.  As with every
-- weave opclass, the DISCRIMINATOR is the opclass, not the column type -- an int8
-- column is not by itself a docvals column; `USING weave (price int8_docval_ops)`
-- is the request -- which is exactly what weave_index_layout() reads to resolve
-- layout.dvattno.
--
-- The AM has amsupport = 0, so this opclass declares NO support FUNCTION: adding
-- one with ALTER OPERATOR FAMILY ... ADD FUNCTION would be rejected against
-- amsupport.  The AM does not need a cmp proc -- it evaluates the predicate itself
-- over the stored int8 values (weave_dv_eval_int8), so all the opclass must do is
-- NAME THE CHANNEL and enumerate which comparison operators route to it.

-- WHY CORE'S COMPARISON OPERATORS, and not dedicated ones like gram_ops's `@~`.
--
-- gram_ops minted `@~`/`@~*` precisely so that not every `LIKE` on the column
-- became an index candidate: its win is narrow and its cost (index bytes) is
-- broad, so it made the surface opt-in at the QUERY level too.  A scalar facet is
-- the opposite case.  It is queried as a plain `WHERE price < x` -- that IS the
-- claim-3 shape the channel exists to accelerate -- and there is no plainer
-- spelling to route to.  Minting `price @< x` would force every query to learn a
-- private operator for an ordinary comparison, defeating the point.
--
-- The cost of this choice is real and is deferred, not denied: with core's `<`
-- (etc.) in the family, EVERY such qual on the column becomes a candidate index
-- clause, including ones this AM's estimator does not model well.  Bounding that
-- with a cost model that declines the index when the facet is not selective is a
-- COST-MODEL concern owned by task P4; it is noted here so the deferral is visible
-- at the point that created the exposure.
--
-- Strategy numbers are btree's 1..5 (< <= = >= >) and must lie in 1..amstrategies
-- (5): ALTER OPERATOR FAMILY validates the strategy number against the access
-- method's amstrategies.  The scan disambiguates by attribute -- weave_rescan()
-- checks the column's WeaveWeftKind before reading a scan key -- so these numbers
-- need not differ from another channel's, only lie in range.
--
-- int8 is written rather than its SQL alias `bigint`; PostgreSQL accepts the
-- internal type name in an opclass definition.
CREATE OPERATOR FAMILY int8_docval_ops USING weave;

CREATE OPERATOR CLASS int8_docval_ops
    FOR TYPE int8 USING weave FAMILY int8_docval_ops AS
        OPERATOR 1 < (int8, int8),
        OPERATOR 2 <= (int8, int8),
        OPERATOR 3 = (int8, int8),
        OPERATOR 4 >= (int8, int8),
        OPERATOR 5 > (int8, int8);

COMMENT ON OPERATOR FAMILY int8_docval_ops USING weave IS
    'routes an int8 facet column to a weave index''s docvalues channel';
