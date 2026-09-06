/* pg_weave 0.1.0 -> 0.2.0
 *
 * Task V1 in doc/PHASES.md: the wvec vector type.
 *
 * wvec is a distinct type from pgvector's `vector` on purpose, so that both
 * extensions can be installed in one database and a migration can be
 * incremental.  Operator names, strategy numbers, and edge-case semantics
 * (including <#> being NEGATIVE inner product, and cosine over a zero vector
 * returning NaN) match pgvector exactly -- diverging there would be a migration
 * trap rather than an improvement.  See doc/MIGRATION.md.
 *
 * The index access method does not accept wvec columns yet; that is tasks
 * V7-V9.  Everything here is the type, its operators, and the codec
 * introspection functions that make quantization error measurable on a real
 * corpus without building an index first.
 */

\echo Use "ALTER EXTENSION pg_weave UPDATE TO '0.2.0'" to load this file. \quit

-- ---------------------------------------------------------------------------
-- The type
-- ---------------------------------------------------------------------------

CREATE TYPE wvec;

CREATE FUNCTION wvec_in(cstring, oid, integer) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_out(wvec) RETURNS cstring
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_recv(internal, oid, integer) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_send(wvec) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_typmod_in(cstring[]) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE wvec (
    INPUT     = wvec_in,
    OUTPUT    = wvec_out,
    RECEIVE   = wvec_recv,
    SEND      = wvec_send,
    TYPMOD_IN = wvec_typmod_in,
    STORAGE   = external,
    -- external, not extended: a compressed vector cannot be scanned in place,
    -- and every read of a vector reads all of it, so compression buys nothing
    -- and costs a detoast on every distance computation.
    INTERNALLENGTH = variable,
    ALIGNMENT = double
);

COMMENT ON TYPE wvec IS 'single-precision vector for the weave vector channel';

-- ---------------------------------------------------------------------------
-- Casts
-- ---------------------------------------------------------------------------

CREATE FUNCTION wvec_enforce_typmod(wvec, integer, boolean) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (wvec AS wvec)
    WITH FUNCTION wvec_enforce_typmod(wvec, integer, boolean) AS IMPLICIT;

CREATE FUNCTION wvec_from_float4_array(real[], integer, boolean) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (real[] AS wvec)
    WITH FUNCTION wvec_from_float4_array(real[], integer, boolean) AS ASSIGNMENT;

CREATE FUNCTION wvec_to_float4_array(wvec) RETURNS real[]
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (wvec AS real[])
    WITH FUNCTION wvec_to_float4_array(wvec) AS IMPLICIT;

-- ---------------------------------------------------------------------------
-- Accessors
-- ---------------------------------------------------------------------------

CREATE FUNCTION wvec_dims(wvec) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_norm(wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Returns NULL for a zero vector rather than a vector of NaNs: a zero vector
-- has no direction, and propagating NaN corrupts every later distance silently.
CREATE FUNCTION wvec_l2_normalize(wvec) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ---------------------------------------------------------------------------
-- Distance functions
-- ---------------------------------------------------------------------------

CREATE FUNCTION wvec_l2_distance(wvec, wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_l2_squared_distance(wvec, wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_inner_product(wvec, wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_negative_inner_product(wvec, wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_cosine_distance(wvec, wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_l1_distance(wvec, wvec) RETURNS double precision
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Operator names and meanings are pgvector's.  <#> is the NEGATIVE inner
-- product so that ascending order returns the largest inner product first,
-- which is what an index ORDER BY requires.
CREATE OPERATOR <-> (
    LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_l2_distance,
    COMMUTATOR = '<->'
);

CREATE OPERATOR <#> (
    LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_negative_inner_product,
    COMMUTATOR = '<#>'
);

CREATE OPERATOR <=> (
    LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_cosine_distance,
    COMMUTATOR = '<=>'
);

CREATE OPERATOR <+> (
    LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_l1_distance,
    COMMUTATOR = '<+>'
);

COMMENT ON OPERATOR <-> (wvec, wvec) IS 'L2 distance';
COMMENT ON OPERATOR <#> (wvec, wvec) IS 'negative inner product';
COMMENT ON OPERATOR <=> (wvec, wvec) IS 'cosine distance';
COMMENT ON OPERATOR <+> (wvec, wvec) IS 'L1 distance';

-- ---------------------------------------------------------------------------
-- Arithmetic
-- ---------------------------------------------------------------------------

CREATE FUNCTION wvec_add(wvec, wvec) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_sub(wvec, wvec) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION wvec_mul(wvec, wvec) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR + (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_add,
                   COMMUTATOR = '+');
CREATE OPERATOR - (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_sub);
CREATE OPERATOR * (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_mul,
                   COMMUTATOR = '*');

-- ---------------------------------------------------------------------------
-- Comparison and btree support
--
-- These exist so wvec works with DISTINCT, GROUP BY, ORDER BY, and a btree
-- index -- not because lexicographic order on a vector is meaningful.  Order is
-- element-wise then by dimension: arbitrary, but total and stable.
-- ---------------------------------------------------------------------------

CREATE FUNCTION wvec_cmp(wvec, wvec) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION wvec_eq(wvec, wvec) RETURNS boolean
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION wvec_ne(wvec, wvec) RETURNS boolean
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION wvec_lt(wvec, wvec) RETURNS boolean
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION wvec_le(wvec, wvec) RETURNS boolean
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION wvec_gt(wvec, wvec) RETURNS boolean
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION wvec_ge(wvec, wvec) RETURNS boolean
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR =  (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_eq,
                    COMMUTATOR = '=', NEGATOR = '<>',
                    RESTRICT = eqsel, JOIN = eqjoinsel, HASHES, MERGES);
CREATE OPERATOR <> (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_ne,
                    COMMUTATOR = '<>', NEGATOR = '=',
                    RESTRICT = neqsel, JOIN = neqjoinsel);
CREATE OPERATOR <  (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_lt,
                    COMMUTATOR = '>', NEGATOR = '>=',
                    RESTRICT = scalarltsel, JOIN = scalarltjoinsel);
CREATE OPERATOR <= (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_le,
                    COMMUTATOR = '>=', NEGATOR = '>',
                    RESTRICT = scalarlesel, JOIN = scalarlejoinsel);
CREATE OPERATOR >  (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_gt,
                    COMMUTATOR = '<', NEGATOR = '<=',
                    RESTRICT = scalargtsel, JOIN = scalargtjoinsel);
CREATE OPERATOR >= (LEFTARG = wvec, RIGHTARG = wvec, PROCEDURE = wvec_ge,
                    COMMUTATOR = '<=', NEGATOR = '<',
                    RESTRICT = scalargesel, JOIN = scalargejoinsel);

CREATE OPERATOR CLASS wvec_ops DEFAULT FOR TYPE wvec USING btree AS
    OPERATOR 1 <  (wvec, wvec),
    OPERATOR 2 <= (wvec, wvec),
    OPERATOR 3 =  (wvec, wvec),
    OPERATOR 4 >= (wvec, wvec),
    OPERATOR 5 >  (wvec, wvec),
    FUNCTION 1 wvec_cmp(wvec, wvec);

-- ---------------------------------------------------------------------------
-- Codec introspection
--
-- The quantizer works and is property-tested (test/hegel/test_quantize.c,
-- 17741 checks) but the index side does not exist yet.  These functions make it
-- measurable on a real corpus in the meantime:
--
--   SELECT percentile_cont(0.95) WITHIN GROUP (
--            ORDER BY wvec_l2_distance(v, weave_quantize_roundtrip(v, 4))
--                     / wvec_norm(v))
--     FROM embeddings;
--
-- gives the p95 relative reconstruction error, which is the number that
-- actually predicts recall -- answerable without building an index.
-- ---------------------------------------------------------------------------

CREATE FUNCTION weave_quantize_roundtrip(wvec, integer DEFAULT 4) RETURNS wvec
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION weave_quantize_size(integer, integer DEFAULT 4) RETURNS integer
    AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

COMMENT ON FUNCTION weave_quantize_roundtrip(wvec, integer) IS
    'encode and decode through the weave quantizer, to measure quantization error';
COMMENT ON FUNCTION weave_quantize_size(integer, integer) IS
    'bytes one vector occupies at a given dimension and code width';
