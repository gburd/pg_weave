/* pg_weave 0.15.0 -> 0.16.0 */

-- F2.2: the TRANSPORT KEY that carries fuse()'s weights into amrescan.
--
-- WHAT THIS IS FOR, in one sentence: an access method sees ORDER BY operators and
-- nothing else, so the only way to hand it a float4[] of per-channel weights is to
-- make the array the right operand of an ORDER BY operator that exists for no other
-- reason.  `<~>` is that operator.  It is never evaluated.
--
-- THE SHAPE, and why it is this one.  doc/specs/FUSED_TOPK.md sect. 7a records the
-- maintainer's decision (plan shape (A)): src/am/fusepath.c hand-builds an
-- IndexPath whose `indexorderbys` are the individual per-channel `col <op> query`
-- OpExprs -- one scan key each, which is how the AM learns which channels to build
-- and in which order -- plus ONE extra key `col <~> '{w1,w2,...}'::float4[]`.
-- weave_rescan() (src/am/amscan.c) recognizes strategy 5 on that key, reads the
-- array, and assigns weights to the scored channels in the order the scored keys
-- arrived.  That order is the order of fuse()'s score arguments, which is the order
-- of the weights array, so the correspondence is positional and needs no names.
--
-- THE TWO ALTERNATIVES AND WHY THEY LOST (sect. 7a again, kept here because a
-- future reader will find this operator before they find the spec):
--
--   * A CustomScan.  Rejected: it would have to re-implement nodeIndexscan's heap
--     fetch, visibility check, qual recheck and EPQ handling, which is exactly where
--     MVCC bugs live.  An IndexPath gets all four from core for free.
--   * One real operator over a new composite query type carrying query + weights.
--     Rejected, but only on surface: it works and needs no planner code at all, and
--     it is the fallback position if (A) proves unworkable.  It makes the SQL
--     surface stop looking like sect. 7's `fuse(a <=> x, b <=> y, weights => ...)`.
--
-- AND WHY IT IS AN ORDER BY KEY AND NOT A QUAL.  A marker operator in a qual would
-- eventually be EXECUTED: `indexqualorig` is re-evaluated by the executor during an
-- EPQ recheck (a concurrent UPDATE under READ COMMITTED, or a FOR UPDATE row lock),
-- so a qual-borne marker is a query that works until someone else updates the row.
-- Order-by expressions are not re-evaluated that way: the executor keeps
-- `indexorderbyorig` only to feed a reorder queue, which this AM does not request
-- (xs_recheckorderby stays false).  So the C function below can be an unconditional
-- ERROR, which is the strongest possible statement that nothing may evaluate it.
--
-- STRATEGY 5, AND amstrategies HAD TO MOVE FROM 4 TO 5 FIRST.  ALTER OPERATOR
-- FAMILY validates a member number against the access method's amstrategies, so the
-- bump in src/am/am.c (weave_handler) is a precondition of these two members
-- existing and not a tidy-up.  1..4 are all spoken for across the three families --
-- `@@@`/`@~`/`<->` share 1 in different families, `<=>` is 2, `<@>` is 3, `<#>` is
-- 4 -- and weave_rescan() dispatches order-by keys on sk_strategy ALONE, so a
-- transport key that reused any of them would be indistinguishable from a real
-- channel request.  The constant is WEAVE_STRAT_FUSE_WEIGHTS in include/weave/am.h,
-- next to the two vector numbers and the note that says why a number is a decision.
--
-- TWO MEMBERS, ONE PER OPCLASS THAT CAN CARRY THE KEY.  The key has to hang off
-- SOME index column, because indexorderbycols names one and the executor resolves
-- the strategy against that column's operator family.  Today src/am/fusepath.c
-- always attaches it to the LEXICAL column -- every weave index has exactly one
-- (weave_index_layout() throws otherwise), so the wdoc form is always available.
-- The wvec form is added in the same breath because doc/specs/FUSED_TOPK.md sect. 7a
-- specifies both and because a future fused shape whose only channel is the vector
-- one would have no wdoc column to hang it on; leaving it out would make that a
-- catalog migration rather than a planner change.
CREATE FUNCTION weave_fuse_transport(wdoc, float4[])
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse_transport'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION weave_fuse_transport(wvec, float4[])
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse_transport'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_fuse_transport(wdoc, float4[]) IS
    'internal: carries fuse() weights into a weave index scan; raises if evaluated';
COMMENT ON FUNCTION weave_fuse_transport(wvec, float4[]) IS
    'internal: carries fuse() weights into a weave index scan; raises if evaluated';

CREATE OPERATOR <~> (
    LEFTARG = wdoc,
    RIGHTARG = float4[],
    PROCEDURE = weave_fuse_transport
);

CREATE OPERATOR <~> (
    LEFTARG = wvec,
    RIGHTARG = float4[],
    PROCEDURE = weave_fuse_transport
);

COMMENT ON OPERATOR <~> (wdoc, float4[]) IS
    'fused-scan weights transport; only meaningful inside a weave index scan';
COMMENT ON OPERATOR <~> (wvec, float4[]) IS
    'fused-scan weights transport; only meaningful inside a weave index scan';

ALTER OPERATOR FAMILY wdoc_lex_ops USING weave ADD
    OPERATOR 5 <~> (wdoc, float4[]) FOR ORDER BY pg_catalog.float_ops;

ALTER OPERATOR FAMILY wvec_weave_ops USING weave ADD
    OPERATOR 5 <~> (wvec, float4[]) FOR ORDER BY pg_catalog.float_ops;
