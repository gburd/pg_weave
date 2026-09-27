/* pg_weave 0.25.0 -> 0.26.0 */

-- Docvalues type slice 2 (doc/plans/2026-09-27-docvals-types-slice.md): four more
-- fixed-width facet types and float8 join the scalar / docvalues channel
-- (WEAVE_WK_DOCVALS), each as its own operator class in its own implicit family.
--
-- WHAT CHANGES, AND WHAT DOES NOT.  Only these SQL opclasses and the C encode
-- (weave_dv_type_for_oid / weave_dv_encode_datum, src/pages/docvals_page.c) are
-- new.  The on-disk store and the evaluator (weave/docvals.h) are UNCHANGED and
-- still int64: every type is mapped to an ORDER-PRESERVING int64 at build/insert
-- and the query constant by the same rule at scan, so weave_dv_eval_int8()'s single
-- signed comparison is exact for every type and the store never records the type.
-- Integers/date/bool widen; float8 uses the monotonic IEEE-754 transform
-- (weave_dv_encode_f8, whose order-isomorphism is proven in test/hegel/test_docvals.c).
--
-- Like int8_docval_ops these declare NO support FUNCTION (amsupport = 0); the AM
-- discriminates the channel by the opclass's family name, and the btree strategy
-- numbers 1..5 map to (< <= = >= >).  The scan disambiguates a docvalues `<` from a
-- lexical `@@@` by the column's channel, not the strategy (weave_rescan).
--
-- CROSS-TYPE members mirror int8_docval_ops's reasoning: the column is on the LEFT
-- and the constant's type varies, so each integer opclass admits the other integer
-- widths as the right-hand type (a bare literal is int4, a smallint parameter int2,
-- a bigint int8), and float8 admits a float4 constant.  A numeric literal against a
-- float8 column is out of scope this slice (numeric has no lossless int64 encoding);
-- write the constant as float8.

-- float8 -----------------------------------------------------------------------
CREATE OPERATOR FAMILY float8_docval_ops USING weave;
CREATE OPERATOR CLASS float8_docval_ops
    FOR TYPE float8 USING weave FAMILY float8_docval_ops AS
        OPERATOR 1 < (float8, float8),
        OPERATOR 2 <= (float8, float8),
        OPERATOR 3 = (float8, float8),
        OPERATOR 4 >= (float8, float8),
        OPERATOR 5 > (float8, float8);
ALTER OPERATOR FAMILY float8_docval_ops USING weave ADD
    OPERATOR 1 < (float8, float4),
    OPERATOR 2 <= (float8, float4),
    OPERATOR 3 = (float8, float4),
    OPERATOR 4 >= (float8, float4),
    OPERATOR 5 > (float8, float4);
COMMENT ON OPERATOR FAMILY float8_docval_ops USING weave IS
    'routes a float8 facet column to a weave index''s docvalues channel';

-- int4 -------------------------------------------------------------------------
CREATE OPERATOR FAMILY int4_docval_ops USING weave;
CREATE OPERATOR CLASS int4_docval_ops
    FOR TYPE int4 USING weave FAMILY int4_docval_ops AS
        OPERATOR 1 < (int4, int4),
        OPERATOR 2 <= (int4, int4),
        OPERATOR 3 = (int4, int4),
        OPERATOR 4 >= (int4, int4),
        OPERATOR 5 > (int4, int4);
ALTER OPERATOR FAMILY int4_docval_ops USING weave ADD
    OPERATOR 1 < (int4, int8),
    OPERATOR 2 <= (int4, int8),
    OPERATOR 3 = (int4, int8),
    OPERATOR 4 >= (int4, int8),
    OPERATOR 5 > (int4, int8),
    OPERATOR 1 < (int4, int2),
    OPERATOR 2 <= (int4, int2),
    OPERATOR 3 = (int4, int2),
    OPERATOR 4 >= (int4, int2),
    OPERATOR 5 > (int4, int2);
COMMENT ON OPERATOR FAMILY int4_docval_ops USING weave IS
    'routes an int4 facet column to a weave index''s docvalues channel';

-- int2 -------------------------------------------------------------------------
CREATE OPERATOR FAMILY int2_docval_ops USING weave;
CREATE OPERATOR CLASS int2_docval_ops
    FOR TYPE int2 USING weave FAMILY int2_docval_ops AS
        OPERATOR 1 < (int2, int2),
        OPERATOR 2 <= (int2, int2),
        OPERATOR 3 = (int2, int2),
        OPERATOR 4 >= (int2, int2),
        OPERATOR 5 > (int2, int2);
ALTER OPERATOR FAMILY int2_docval_ops USING weave ADD
    OPERATOR 1 < (int2, int8),
    OPERATOR 2 <= (int2, int8),
    OPERATOR 3 = (int2, int8),
    OPERATOR 4 >= (int2, int8),
    OPERATOR 5 > (int2, int8),
    OPERATOR 1 < (int2, int4),
    OPERATOR 2 <= (int2, int4),
    OPERATOR 3 = (int2, int4),
    OPERATOR 4 >= (int2, int4),
    OPERATOR 5 > (int2, int4);
COMMENT ON OPERATOR FAMILY int2_docval_ops USING weave IS
    'routes an int2 facet column to a weave index''s docvalues channel';

-- date -------------------------------------------------------------------------
CREATE OPERATOR FAMILY date_docval_ops USING weave;
CREATE OPERATOR CLASS date_docval_ops
    FOR TYPE date USING weave FAMILY date_docval_ops AS
        OPERATOR 1 < (date, date),
        OPERATOR 2 <= (date, date),
        OPERATOR 3 = (date, date),
        OPERATOR 4 >= (date, date),
        OPERATOR 5 > (date, date);
COMMENT ON OPERATOR FAMILY date_docval_ops USING weave IS
    'routes a date facet column to a weave index''s docvalues channel';

-- bool -------------------------------------------------------------------------
CREATE OPERATOR FAMILY bool_docval_ops USING weave;
CREATE OPERATOR CLASS bool_docval_ops
    FOR TYPE bool USING weave FAMILY bool_docval_ops AS
        OPERATOR 1 < (bool, bool),
        OPERATOR 2 <= (bool, bool),
        OPERATOR 3 = (bool, bool),
        OPERATOR 4 >= (bool, bool),
        OPERATOR 5 > (bool, bool);
COMMENT ON OPERATOR FAMILY bool_docval_ops USING weave IS
    'routes a bool facet column to a weave index''s docvalues channel';
