/* pg_weave 0.11.0 -> 0.12.0 */

-- Z9: `<@>` edit-distance KNN ordering.
--
-- WHAT IT IS.  `wdoc <@> text` is the minimum character-level Levenshtein
-- distance from the pattern to any term of the document, and it is an ORDER BY
-- operator (strategy 3) of wdoc_lex_ops, so
--
--     SELECT ... ORDER BY d <@> 'connectoin' LIMIT 10
--
-- is answered by an index ordering scan that walks the segment dictionary with a
-- real numeric lower bound on the distance (include/weave/edist.h) instead of by
-- a Seq Scan computing the distance for every row.  It needs no WHERE clause;
-- the bare form is the one users write, and getting it wrong is the recorded
-- 7,000x cliff of task L7 / doc/GAPS.md G1.
--
-- WHY THE DOCUMENT-LEVEL DISTANCE IS A MINIMUM OVER TERMS, stated here because
-- an operator's meaning belongs next to its declaration.  A wdoc holds many
-- terms; ORDER BY needs one number.  The minimum is the only reduction under
-- which "ascending distance" means "closest match first".  A sum or an average
-- would punish a long document for containing extra words, and a maximum would
-- rank the document containing the pattern verbatim LAST.  So the left operand is
-- the indexed wdoc, the right is the raw pattern, and the value is
--
--     min over terms t of levenshtein(pattern, t)
--
-- A document with no terms has no distance and returns +Infinity, which sorts it
-- last; the index path never produces such a row, since a term-free document is
-- in no posting list.
--
-- THE VALUE IS contrib/fuzzystrmatch's levenshtein()'s, by construction: the C
-- function calls core's varstr_levenshtein(), which is the same function
-- levenshtein() calls, with the same unit -- CHARACTERS, not bytes -- and unit
-- insert/delete/substitute costs.  That agreement is what makes a differential
-- test against a seq-scan reference possible at all, and it is the reason
-- doc/GAPS.md G30 insisted the fuzzy channel count characters.  An implementation
-- that counted bytes would disagree with the oracle on exactly the inputs no
-- ASCII test contains.
--
-- Not IMMUTABLE by courtesy: it is a pure function of its two arguments (no
-- collation, no encoding-dependent folding beyond the server encoding's own
-- character boundaries), which an index ordering operator has to be.
CREATE FUNCTION weave_edist(wdoc, text)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_edist'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION weave_edist_commutator(text, wdoc)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_edist_commutator'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR <@> (
    LEFTARG    = wdoc,
    RIGHTARG   = text,
    PROCEDURE  = weave_edist,
    COMMUTATOR = <@>
);

CREATE OPERATOR <@> (
    LEFTARG    = text,
    RIGHTARG   = wdoc,
    PROCEDURE  = weave_edist_commutator,
    COMMUTATOR = <@>
);

-- Strategy 3, alongside @@@ (1) and <=> (2).  Only the index-side form
-- (wdoc <@> text) goes in the family: the commutator exists so the planner can
-- normalize `'pat' <@> d` into it, not so it can be indexed twice.
ALTER OPERATOR FAMILY wdoc_lex_ops USING weave ADD
    OPERATOR 3 <@> (wdoc, text) FOR ORDER BY pg_catalog.float_ops;
