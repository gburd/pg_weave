/* pg_weave 0.26.0 -> 0.27.0 */

-- Docvalues text slice (doc/plans/2026-09-28-docvals-text-slice.md): a text
-- facet joins the scalar / docvalues channel (WEAVE_WK_DOCVALS) as its own
-- operator class in its own implicit family.
--
-- WHAT CHANGES, AND WHAT DOES NOT.  Unlike the 0.26.0 types, text has no
-- order-preserving int64 encoding under a collation, so a text column does not
-- go through weave_dv_encode_datum.  Each segment stores a DICTIONARY of its
-- distinct values sorted under the column's collation plus one dictionary
-- ordinal per docid (store v3, weave/docvals.h); a query constant is resolved
-- to ordinal boundaries per segment.  The v1/v2 int8 stores are unchanged.
--
-- The collation must be DETERMINISTIC: a non-deterministic one calls
-- byte-distinct strings equal, so "the distinct values" of a segment is not
-- well defined and `=` answered by ordinal would drop rows.  CREATE INDEX
-- refuses such a column.  A collation drift (a libc/ICU upgrade reordering
-- strings) invalidates the dictionary exactly as it invalidates a btree; the
-- remedy is REINDEX.
--
-- Like int8_docval_ops this declares NO support FUNCTION (amsupport = 0); the
-- AM discriminates the channel by the opclass's family name, and the btree
-- strategy numbers 1..5 map to (< <= = >= >).  A varchar column uses this
-- opclass by binary coercion, as btree's text_ops serves varchar.

-- text -------------------------------------------------------------------------
CREATE OPERATOR FAMILY text_docval_ops USING weave;
CREATE OPERATOR CLASS text_docval_ops
    FOR TYPE text USING weave FAMILY text_docval_ops AS
        OPERATOR 1 < (text, text),
        OPERATOR 2 <= (text, text),
        OPERATOR 3 = (text, text),
        OPERATOR 4 >= (text, text),
        OPERATOR 5 > (text, text);
COMMENT ON OPERATOR FAMILY text_docval_ops USING weave IS
    'routes a text facet column to a weave index''s docvalues channel';
