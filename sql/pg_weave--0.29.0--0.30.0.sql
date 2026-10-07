/* pg_weave 0.29.0 -> 0.30.0 */

-- tsvector as a first-class weave index input (doc/PHASES.md M7).
--
--     CREATE INDEX ... USING weave (tsv tsvector_lex_ops, ...)
--
-- indexes an existing tsvector column with no query change beyond the operator.
-- The value is converted to a wdoc at the index boundary by the same function
-- to_wdoc(tsvector) uses (build, insert and the heap recheck all call it), so the
-- index holds what USING weave (to_wdoc(tsv)) would.  The heap keeps only the
-- tsvector.  tsvector caps tf at 255 positions per lexeme and positions at 16,383,
-- and stores no length, so BM25 from it is exact only below those caps
-- (bench/RESULTS_TSVECTOR_CAPS.md); weave_index_tsvector_stats() counts the
-- documents where a cap was reached.
--
-- The operators are the wdoc operators applied to to_wdoc(tsvector), so a
-- sequential scan, the recheck and the index answer the same question.  Core's
-- `tsvector @@ tsquery` is a separate task (M3).
--
-- One thing to know: core still ships the deprecated `@@@ (tsvector, tsquery)`,
-- so with an UNTYPED literal on the right, `tsv @@@ 'foo'` is ambiguous between
-- it and this one.  Write `tsv @@@ 'foo'::wquery` (or pass a wquery expression).

CREATE FUNCTION weave_tsv_match(tsvector, wquery)
RETURNS boolean
AS 'MODULE_PATHNAME', 'weave_tsv_match'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION weave_tsv_match_commutator(wquery, tsvector)
RETURNS boolean
AS 'MODULE_PATHNAME', 'weave_tsv_match_commutator'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR @@@ (
    LEFTARG    = tsvector,
    RIGHTARG   = wquery,
    PROCEDURE  = weave_tsv_match,
    COMMUTATOR = @@@,
    RESTRICT   = tsmatchsel,
    JOIN       = tsmatchjoinsel
);

CREATE OPERATOR @@@ (
    LEFTARG    = wquery,
    RIGHTARG   = tsvector,
    PROCEDURE  = weave_tsv_match_commutator,
    COMMUTATOR = @@@,
    RESTRICT   = tsmatchsel,
    JOIN       = tsmatchjoinsel
);

CREATE FUNCTION weave_tsv_distance(tsvector, wquery)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_tsv_distance'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION weave_tsv_distance_commutator(wquery, tsvector)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_tsv_distance_commutator'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <=> (
    LEFTARG    = tsvector,
    RIGHTARG   = wquery,
    PROCEDURE  = weave_tsv_distance,
    COMMUTATOR = <=>
);

CREATE OPERATOR <=> (
    LEFTARG    = wquery,
    RIGHTARG   = tsvector,
    PROCEDURE  = weave_tsv_distance_commutator,
    COMMUTATOR = <=>
);

-- Strategies 1 and 2 mean what they mean in wdoc_lex_ops.  Not DEFAULT: core's
-- gin/gist tsvector_ops are the expected default for the type, and a weave
-- tsvector column should be asked for by name.
CREATE OPERATOR CLASS tsvector_lex_ops
    FOR TYPE tsvector USING weave AS
    OPERATOR 1 @@@ (tsvector, wquery),
    OPERATOR 2 <=> (tsvector, wquery) FOR ORDER BY pg_catalog.float_ops;

-- Over the rows visible to the caller: documents, documents where a core
-- tsvector cap was reached (a lexeme at 255 positions or a position at 16,383:
-- tf or length then approximate), and documents with a lexeme without positions
-- (stripped or mixed: tf 1, no phrase).  A heap scan through the index's own
-- expression, so it is exact under MVCC and costs O(table).
CREATE FUNCTION weave_index_tsvector_stats(regclass,
                                           OUT ndocs bigint,
                                           OUT ncapped bigint,
                                           OUT npositionless bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'weave_index_tsvector_stats'
LANGUAGE C STRICT STABLE PARALLEL SAFE;
