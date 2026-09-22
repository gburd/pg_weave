/* pg_weave 0.16.0 -> 0.17.0 */

-- F8: the two MISSING score-recovery functions, and the defect their absence was.
--
-- doc/GAPS.md G41.  fuse() sums SCORES -- higher is better -- and negates once at
-- the end so that ascending is best-first.  A channel argument spelled as a
-- DISTANCE therefore has to be recovered into a score first, which is what the
-- planner support function does by wrapping each recognized operator in one of
-- these functions (src/am/fusepath.c, weave_fuse_recover()).
--
-- 0.14.0 shipped the recovery for the lexical `<=>` and for the cosine
-- `wvec <=> wvec`, and stopped there -- because at that point no vector channel
-- could be fused and the point of the functions was the rewrite.  The consequence
-- was not a missing feature: `fuse(body <=> q, emb <-> v)` PARSED, RAN, and ranked
-- the vector channel BACKWARDS, because the raw `<->` distance went into the sum as
-- if it were a score and the final negation then put the farthest vector first.  No
-- test could see it, because both arms -- the executable fallback and (once F8
-- arrived) the pushdown -- would have been wrong in the same direction, and every
-- assertion this project writes about fuse() compares the two arms.
--
-- THE DOMAIN IS THE CHANNEL'S, NOT THE OPERATOR'S, which is why -d*d and not -d for
-- l2.  The weave vector weft scores in the metric's domain with higher better:
-- -||q - v||^2 for l2, the inner product for ip (include/weave/vecscan.h, "THE
-- DOMAIN RULE").  `<->` computes ||q - v||, so the score is -d*d; -d would rank a
-- lone vector channel identically but would weight it differently from the index at
-- every distance except 1, and a fused sum is arithmetic rather than a ranking.
-- `<#>` already computes the negated inner product, so the score is -d exactly.
--
-- STRICT and total on every float8, for the same two reasons the 0.14.0 pair are:
-- a NULL distance must yield a NULL score, and a function that raises inside a sort
-- key turns a user's arithmetic slip into a failed query.
CREATE FUNCTION weave_l2score(float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_l2score'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION weave_ipscore(float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_ipscore'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

-- And the third, for `<@>`, which CANNOT be fused yet (task F9 owes it a
-- document-space shuttle) and still needs this: the two halves of G41 are
-- independent.  The pushdown needs a shuttle; the FALLBACK's arithmetic needs only
-- the recovery function, and without it `fuse(body <=> q, body <@> p)` -- a shape
-- sql/fuse_pushdown.sql has run since F2.2 -- ranks the WORST spelling match first.
-- A Levenshtein distance is a non-negative integer and smaller is better, so the
-- score is -d.
CREATE FUNCTION weave_edistscore(float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_edistscore'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

COMMENT ON FUNCTION weave_l2score(float8) IS
    'recover the vector channel l2 score -d*d from the wvec <-> distance';
COMMENT ON FUNCTION weave_ipscore(float8) IS
    'recover the vector channel ip score -d from the wvec <#> negated inner product';
COMMENT ON FUNCTION weave_edistscore(float8) IS
    'recover a score -d from the wdoc <@> text edit distance';
