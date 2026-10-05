/* pg_weave 0.27.0 -> 0.28.0 */

-- weave_fuse_search() (doc/PHASES.md F3, doc/specs/FUSED_TOPK.md 7e): a
-- set-returning function that drives a real weave index scan with a fused
-- ORDER BY and hands back each row's fused score.
--
-- It exists ONLY as a workaround for a PostgreSQL core limitation: an index
-- scan cannot pass its ORDER BY value into the SELECT list, so
-- "SELECT fuse(...) ... ORDER BY fuse(...)" recomputes fuse() per row rather
-- than reading the score the fused pass already computed.
-- doc/upstream/ORDERBY_VALUES_TO_TLIST.md proposes the core change. If it ever
-- lands, the ORDER BY form is the intended surface and this SRF is optional.
--
-- Channel order is every lex entry, then every vec entry; weights has one
-- entry per channel in that order, NULL meaning 1.0 each.

CREATE FUNCTION weave_fuse_search(index regclass,
								  lex wquery[],
								  vec wvec[] DEFAULT NULL,
								  weights float4[] DEFAULT NULL,
								  k int DEFAULT 10,
								  OUT ctid tid,
								  OUT score float8,
								  OUT parts float4[])
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'weave_fuse_search'
LANGUAGE C PARALLEL RESTRICTED;

REVOKE ALL ON FUNCTION weave_fuse_search(regclass, wquery[], wvec[], float4[], int) FROM PUBLIC;

COMMENT ON FUNCTION weave_fuse_search(regclass, wquery[], wvec[], float4[], int) IS
'Fused top-k through a real weave index scan, returning (ctid, score, parts). '
'Channels: each lex entry, then each vec entry; weights one per channel (NULL = 1.0 each). '
'parts is currently always NULL. '
'WORKAROUND for a PostgreSQL core limitation: an index scan cannot hand its ORDER BY value '
'to the SELECT list. doc/upstream/ORDERBY_VALUES_TO_TLIST.md proposes that core change; if it '
'ever lands, SELECT fuse(...) ... ORDER BY fuse(...) becomes the intended surface and this '
'function optional.';
