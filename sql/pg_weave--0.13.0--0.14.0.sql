/* pg_weave 0.13.0 -> 0.14.0 */

-- F2.1: the SQL surface of fuse(), doc/specs/FUSED_TOPK.md sect. 7 and 7a.
--
-- WHAT fuse() IS.  One ORDER BY expression over several retrieval channels:
--
--   SELECT id FROM docs
--    ORDER BY fuse(body <=> 'postgres index'::wquery,   -- lexical, BM25
--                  emb  <=> $1::wvec,                   -- vector, cosine
--                  weights => '{0.4, 0.6}') LIMIT 10;
--
-- ITS ARGUMENTS ARE SCORES (larger = more relevant) AND ITS VALUE IS THE NEGATED
-- WEIGHTED SUM, so the implicit ASC of an ORDER BY is best-first.  The spelling
-- above passes DISTANCES, and that is not a contradiction: `fuse(col <=> q, ...)`
-- is recognized by the planner support function below, which replaces each
-- argument whose operator names a channel with a call that recovers that
-- channel's score from its distance -- weave_lexscore() for the lexical `<=>`
-- (whose distance is 1/(1 + score)) and weave_cosscore() for `wvec <=> wvec`
-- (cosine distance, so score = 1 - d).  An argument the planner cannot attribute
-- to a channel is LEFT ALONE and taken as a score, which is what this
-- declaration says it is; guessing a distance map from a float would be
-- inventing a channel, and sect. 7 requires that the query never simply fail.
--
-- sect. 7a (2) originally specified 1/(1 + sum) rather than negation, on the
-- grounds that it is the map `<=>` already uses.  SUPERSEDED 2026-09-21 by F2.1:
-- that map is total only because BM25 is >= 0, and a recovered cosine score is
-- negative for vectors pointing apart, so a fused sum can reach the pole at -1
-- where 1/(1+S) stops being monotone.  The consequence of a non-monotone
-- ordering map is a WRONG ORDER, not an error.  src/am/fusepath.c's header
-- comment carries the reasoning next to the arithmetic.
--
-- WHAT IS NOT HERE.  The index pushdown.  Until tasks F6 (a lexical shuttle) and
-- F7 (a wvec ORDER BY opfamily member) land there is no channel a fused index
-- scan could consume, so every fuse() query is answered by a Sort over the
-- executable implementation.  That answer is correct arithmetic over the values
-- the operators can compute OUTSIDE an index, which sect. 7a (1) establishes is
-- not the same ranking the index produces -- weave_distance() has no corpus, so
-- it scores with df = 1 and avgdl = |D|.  sql/fuse_fallback.sql pins that
-- divergence as a tested property instead of letting it surprise someone.

-- The two score-recovery functions.  STRICT, so a NULL distance yields a NULL
-- score and fuse() then yields NULL.  Both are total on every float8 rather than
-- only on their operator's range: they are reachable from hand-written SQL, and a
-- function that raises inside a sort key turns a user's arithmetic slip into a
-- failed query.
CREATE FUNCTION weave_lexscore(float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_lexscore'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

CREATE FUNCTION weave_cosscore(float8)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_cosscore'
LANGUAGE C IMMUTABLE PARALLEL SAFE STRICT;

COMMENT ON FUNCTION weave_lexscore(float8) IS
    'recover a BM25 score from the lexical <=> distance 1/(1+score)';
COMMENT ON FUNCTION weave_cosscore(float8) IS
    'recover a cosine similarity from the wvec <=> cosine distance';

-- The planner support function.  It handles SupportRequestSimplify and nothing
-- else, it declines (returns NULL) whenever any catalog lookup it needs fails --
-- an ereport() from the planner would break whatever query happened to mention
-- fuse() -- and it is idempotent, because it rewrites only OpExprs and only
-- returns a new expression when it actually changed one.
CREATE FUNCTION weave_fuse_support(internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'weave_fuse_support'
LANGUAGE C STRICT;

-- CREATE FUNCTION ... SUPPORT requires superuser, which costs nothing here: an
-- extension script runs as the superuser performing the install, and for a
-- `trusted` extension installed by a non-superuser it runs as the bootstrap
-- superuser.  A user cannot reach the clause by writing their own function, which
-- is the point of the restriction.
--
-- The overloads, 2 through 8 score arguments.  Eight is not a formula: it is
-- more channels than the six this index carries, so a query that needs more is
-- expressing something the fused scan cannot serve anyway.
--
-- NOT STRICT, and it must not be: `weights` DEFAULTs to NULL, so a STRICT fuse()
-- would return NULL for every call that does not spell its weights out.  NULL
-- weights means equal weights of 1.0 each -- unnormalized, so adding a channel
-- does not silently rescale the channels already there -- and a NULL score
-- argument yields NULL.
--
-- All of them are one C symbol, weave_fuse, which reads its arity from
-- PG_NARGS(); the last argument is always the weights array.  Every overload
-- carries SUPPORT weave_fuse_support, because a query may write the recognized
-- `col <=> q` spelling at any arity.
CREATE FUNCTION fuse(float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

CREATE FUNCTION fuse(float8, float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

CREATE FUNCTION fuse(float8, float8, float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

CREATE FUNCTION fuse(float8, float8, float8, float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

CREATE FUNCTION fuse(float8, float8, float8, float8, float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

CREATE FUNCTION fuse(float8, float8, float8, float8, float8, float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

CREATE FUNCTION fuse(float8, float8, float8, float8, float8, float8, float8, float8,
                     weights float4[] DEFAULT NULL)
RETURNS float8
AS 'MODULE_PATHNAME', 'weave_fuse'
LANGUAGE C IMMUTABLE PARALLEL SAFE SUPPORT weave_fuse_support;

-- The comment goes on the two-argument overload, which is the one a reader looks
-- up, and it says the two things that are not guessable from the signature.
COMMENT ON FUNCTION fuse(float8, float8, float4[]) IS
    'fused ranking key: the negated weighted sum of per-channel SCORES (larger = more relevant), so ORDER BY fuse(...) LIMIT k is best-first. Writing fuse(col <=> q, ...) is recognized by the planner, which converts each such DISTANCE into the score it came from; any other argument is taken as a score already. Pass weights BY NAME -- weights => ''{0.4,0.6}'' -- because a positional untyped literal is ambiguous between a score and the weights array. Weights must be finite and greater than zero, one per score argument; omitted means 1.0 each.';
