# Plan: docvals type slice 2 — float8 / int4 / int2 / date / bool (DOCVALS_CHANNEL §11 step 2)

Status: proposed 2026-09-27. Prereq: the int8 slice (done; G49/G51/G52 fixed, rule-12
discharged at 10M — `bench/RESULTS_DOCVALS_SCALE.md`). The int8 store, eval, merge, pending
(v11) and gate are the proven template; this generalises the TYPE without touching any of it.

## The one design idea

**The on-disk store and the eval stay int64 and UNCHANGED.** Every supported type is mapped
to an **order-preserving int64** at build/insert, and the query constant is mapped by the
SAME function at scan; `weave_dv_eval_int8`'s signed int64 comparison then gives the right
answer for every type. So there is **no wire-format change, no WEAVE_VERSION bump**, and the
merge/pending/vacuum machinery proven at 10M is reused verbatim — the only new code is the
per-type Datum→int64 encode and the SQL opclasses.

Encodings (all order-preserving on the type's `<`):
- **int8**: identity (`DatumGetInt64`).
- **int4 / int2**: sign-extend (`(int64) DatumGetInt32/16`). Already order-preserving.
- **bool**: `false→0, true→1`.
- **date**: `date` is int32 days; sign-extend to int64. Order-preserving.
- **float8**: the classic monotonic IEEE-754→int64 bijection —
  `bits = double_bits; key = (bits>>63) ? ~bits : (bits | 2^63)`. Maps −inf<…<−0<+0<…<+inf
  to ascending int64. **NaN**: PostgreSQL float8 orders NaN as GREATER than all and NaN=NaN,
  so canonicalise `isnan(d) → INT64_MAX` (above +inf) rather than trust the bit pattern
  (−NaN would otherwise sort to the bottom). **−0.0**: `−0.0` bits = 2^63 → key 0; `+0.0` →
  key 2^63; that makes −0.0 < +0.0, but PG treats them EQUAL. Canonicalise `d==0 → +0.0`
  before encoding so both zeros map to the same key. Both edge rules are exactly what the
  oracle test (§ tests) exists to catch.

`WeaveDvType {INT8,INT4,INT2,BOOL,DATE,FLOAT8}` + two pure helpers:
`weave_dv_encode_datum(WeaveDvType, Datum)` (build/insert) and
`weave_dv_encode_const(WeaveDvType col, Oid subtype, Datum)` (scan). The float encode is pure
and standalone → lives in `include/weave/docvals.h` beside the eval, with a property test.

## Tasks (TDD; coordinator compiles + gates)

### T1 — the encodes + their property test (standalone, FIRST)
`include/weave/docvals.h`: `weave_dv_type` enum; `weave_dv_encode_f8(double)->int64` (pure,
with the NaN/−0 canonicalisation). `test/hegel/test_docvals.c`: a property that for random
doubles (incl. ±inf, ±0, NaN, subnormals) `a < b (PG float8 semantics) ⇔ encode(a) <
encode(b)`, and `encode` is injective except the two canonicalised pairs. Teeth: a planted
bug (drop the sign flip) must fail. This is hard rule 1 for the float channel.

### T2 — opclass registry + per-type build/scan wiring (no SQL yet)
- `src/am/am.c` `weave_opfamily_kinds[]`: add float8/int4/int2/date/bool `_docval_ops` →
  `WEAVE_WK_DOCVALS`. Update the errhint list.
- `WeaveDocvalsAccum` gains `WeaveDvType dvtype`; `weave_docvals_accum_init` takes it.
  `weave_docvals_accum_add` encodes Datum per dvtype (backend helper mapping atttypid→dvtype
  + the encode; float via the standalone `weave_dv_encode_f8`).
- build/insert (`ambuild.c`) resolve the docvals column's `atttypid` → dvtype (from the
  index tupdesc) and pass it to accum init / the pending item is TYPE-AGNOSTIC (still a raw
  int64 — but the INSERT must encode before storing, so `weave_insert` encodes per column
  type into the v11 item's `docval`; the flush then stores it verbatim, no re-encode).
  NB: the v11 pending `docval` becomes "the encoded int64", so pending stays type-agnostic.
- scan (`amscan.c`): resolve dvattno's atttypid → dvtype, `weave_dv_encode_const` the
  constant (handles same-type + the int cross-types; float8 col takes a float8/float4 const).

### T3 — SQL opclasses + cross-type operators + ext bump
`sql/pg_weave--0.25.0--0.26.0.sql`: five opclasses in five implicit families
(`float8_docval_ops` … `bool_docval_ops`), each mapping btree strategies 1–5 to the docvals
operators, mirroring `int8_docval_ops`. Cross-type members where they make sense (int2/int4/
int8 interoperate; date takes date; bool takes bool; float8 takes float8/float4). Bump
`pg_weave.control` default_version 0.25.0→0.26.0; add to Makefile DATA. (This is the FIRST
docvals change that adds SQL objects, so unlike the v11 fix it DOES bump the ext version.)

### T4 — regression + oracle
`sql/docvals.sql`: a per-type section — for each of float8/int4/int2/date/bool, a small table
+ `weave (d, col <type>_docval_ops)`, `dv_agree` over `< <= = >= >` incl. a cross-type const,
and for float8 the ±0/NaN/±inf rows. `expected/docvals.out` regenerated via pg_regress vs lpg
(full REGRESS list). Extend `test_docvals`/`fuzz_docvals` only if the store changed — it does
not, so the float ENCODE test (T1) is the new property coverage.

### T5 — gates + EC2 validation
Local: installcheck pg17/pg18, check-standalone (incl. the new float-encode property),
check-alloc/pdlower/rename, tap. Then extend `bench/aws/docvals_scale.sql` with a second
index on a float8 facet (same 10M table, add a float8 column) and re-run the docvals EC2 job
— index==heap at scale for float8 too (the encode under a real merge/pending). Rule 11: the
prize already holds; correctness is the bar here. Record in `RESULTS_DOCVALS_SCALE.md`.

### T6 — docs
`DOCVALS_CHANNEL.md` §11 step 2 marked done; the family table (§ opclass list) updated;
`doc/GAPS.md` if anything surfaces. Memory note.

## Out of scope (later, §11 steps 3–5)
NULLs + null bitmap; text dictionary; numeric/timestamp/uuid; multi-column docvals AND.
`float4` is offered only as a cross-type CONST against a float8 column, not its own store,
this slice (a float4 column store is a trivial follow-on once float8's encode is proven).

## Risks
- **float8 ordering is the whole risk** and T1's property test is the mitigation — written and
  passing BEFORE any wiring (hard rule 16: sweep the axis the output byte-depends on).
- cross-type const encode for float (float4 const vs float8 col) must widen float4→float8
  THEN encode, not encode float4 bits — the oracle covers it.
- amvalidate must still accept the new opclasses (they carry no support procs; the family-name
  check is all that runs) — covered by CREATE EXTENSION in the regression.
