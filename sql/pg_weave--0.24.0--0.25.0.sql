-- pg_weave 0.24.0 -> 0.25.0
--
-- Cross-type comparison operators for the docvalues channel's int8 operator
-- family, so the NATURAL spelling `price < 100` pushes down as an index gate.
--
-- WHY THIS IS NEEDED.  int8_docval_ops (0.24.0) declared only the same-type
-- operators OPERATOR n <op>(int8, int8).  But an unadorned integer literal is
-- int4, so `price < 100` on a bigint column resolves to <(int8, int4)
-- (int84lt), which is NOT a member of the int8-only family -- so core cannot
-- match it to the docvalues index column and the qual stays an executor Filter
-- (the scan never gets to gate on it).  The user then has to write
-- `price < 100::bigint` to get the gate.  The btree integer_ops family carries
-- the cross-type operators for exactly this reason; the docvalues family reuses
-- the same operators here.
--
-- The scan already widens the constant to int64 from the scankey's SUBTYPE
-- (weave_rescan in src/am/amscan.c reads sk_subtype and DatumGetInt32/Int16),
-- so an int4 or int2 constant compares exactly against the stored int64 value
-- with no on-disk or evaluator change: this migration is opclass metadata only.
--
-- WHY BOTH (int8,int4) AND (int8,int2), AND WHY NOT THE REVERSE.  The column is
-- int8, so a comparison the planner matches to the index has the column on the
-- left after commutation (core commutes `100 > price` to `price < 100`), so the
-- left type is always int8; only the right (constant) type varies over the
-- integer widths a literal or parameter can take: int4 (the default) and int2.
-- A wider constant than the column is not a widening the docvalues path needs --
-- there is no type wider than int8 here.
--
-- These are search strategies (purpose 's'), same numbering as the same-type
-- operators, so weave_dv_eval_int8()'s WeaveDvStrat mapping is unchanged.

ALTER OPERATOR FAMILY int8_docval_ops USING weave ADD
    OPERATOR 1 < (int8, int4),
    OPERATOR 2 <= (int8, int4),
    OPERATOR 3 = (int8, int4),
    OPERATOR 4 >= (int8, int4),
    OPERATOR 5 > (int8, int4),
    OPERATOR 1 < (int8, int2),
    OPERATOR 2 <= (int8, int2),
    OPERATOR 3 = (int8, int2),
    OPERATOR 4 >= (int8, int2),
    OPERATOR 5 > (int8, int2);
