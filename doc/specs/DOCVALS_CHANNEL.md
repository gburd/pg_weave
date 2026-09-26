# Specification: the scalar / docvalues channel

Status: **design, not built.** Channel kind `WEAVE_CH_DOCVALS`, weft kind
`WEAVE_WK_DOCVALS` (id 4), page kind `WEAVE_PK_DOCVALS` (id 25) are all **reserved**
in the tree (`include/weave/channel.h`, `chandesc.h`, `pagekind.h`); nothing writes or
reads them yet. Task series to be assigned in `doc/PHASES.md` (Phase V / a new Phase D).

Headers to create: `include/weave/docvals.h` (backend-independent store reader/validator,
following `docvalid.h`/`chandesc.h`/`pagebound.h` split), plus opclass SQL. Implementation:
`src/pages/docvals_page.c` (writer/reader) and planner glue in `src/am/customscan.c`.

Read `include/weave/channel.h` and `include/weave/gate.h` first. This channel does **not**
score; it is a **gate** (C5), and the correctness contract is §7.

## 1. Why this exists, and the number that justifies it

`bench/RESULTS_DOCVALS_PRIZE.md` (2026-09-26, hard rule 9): a scalar-facet-gated fused
query today gets **slower** as the predicate tightens — vector blocks scored *grow*
41,400 → 108,000 → 165,600 on fiqa across selectivity 0.1 → 0.001, because the facet is an
executor Filter with no docid bound and the scan must consume ever more of the
vector-ordered stream to fill *k* (Rows Removed by Filter 67 → 11,820). That is the exact
over-fetch/collapsing-recall failure mode claim 3 (`ARCHITECTURE.md` §9) is defined against.
A **pushable** gate holds claim 3 (blocks *fall* 35k → 2k), so the fused mechanism is
present — the scalar predicate simply cannot reach it. This channel lets it, capturing up
to **82.8× (fiqa) / 129.6× (scifact)** fewer vector blocks at 0.1 % selectivity.

This is what makes claim 3 general: "a selective `WHERE` makes a vector query faster" for
`WHERE price < x`, not only for another lexical term (the weak, near-circular form the gate
sweep measures today). `doc/PHASES.md` V17's last sentence names this gap.

## 2. What is already built, and what is new

The **gate-consumption side already exists.** `weave_gate_shuttle_from_tidset()`
(`gate.h`) turns a sorted docid set into a required (C5) shuttle; the fused core seeks
through it with `weave_gate_seek` / `weave_gate_block_may_match`, skipping vector work over
docids the gate excludes (`amscan.c:8272`, today fed from the lexical query's matching
TIDs, with the `WEAVE_CH_DOCVALS` label a reporting choice). **Reuse it unchanged.**

New, and only this:

1. **On-disk per-docid value store** (§3), one **weft per docvals column per bolt**, exactly
   as the vector weft is one weft per `wvec` column per bolt. Root reached through the bolt's
   `WeaveSegMeta.chandesc` slot `WEAVE_WK_DOCVALS`.
2. **Predicate → qualifying-docid-set** evaluation over that store (§5), feeding the
   existing gate shuttle.
3. **Planner pushdown** (§8): a comparison on a docvals column becomes an index gate, not an
   executor Filter.

## 3. On-disk format (`WEAVE_PK_DOCVALS`)

One weft per docvals column, `chandesc` slot kind `WEAVE_WK_DOCVALS`, root = a
`WEAVE_PK_DOCVALS` page chain. Values are keyed by **segment-local docid** (the bolt's dense
segment-local docid space, shared by every weft in the bolt), so lookup is an array index,
not a search.

**The store is self-contained in the global docid space (decided 2026-09-26, build-order
step 1).** The fused core drives every channel in the **global** docid space
(`weave_tid_to_docid()` = heap block × `MaxHeapTuplesPerPage` + offset — sparse), which is
where the lexical and gate channels live; the vector weft's dense lane index is relabelled
into it by its warp map (`vecdocmap.h`). A docvalues gate must therefore emit **global**
docids, and the only per-bolt structure that maps a dense index to a global docid is that
warp map — which exists **only when the bolt has a vector column**. Claim 3 (a selective
`WHERE` makes the scan faster) must hold for a facet gate whether or not a vector column is
present, so the docvalues store does **not** borrow the warp map: it carries **its own**
strictly-ascending array of the global docid each dense index denotes, parallel to the value
array. `weave_dv_eval_int8()` then emits global docids directly, ready for
`weave_gate_shuttle_from_tidset()` with no external map. (The alternative — reuse the vector
warp map — was rejected because it silently makes the facet gate require a vector column, a
surprising limitation that undermines the general claim-3 story. Cost of the chosen design:
+8 bytes/doc on disk.)

**Versioned header** (format v1), sized so the deferred zone-map (§ decision 2) is a
non-breaking addition — it carries a `zonemap_off` that is 0 in v1, following the X1 pattern
of self-describing spare fields:

| region | v1 contents |
|---|---|
| header | magic, format version, value typid (int8-kind in v1), ndocs, `null_off` (0 in v1), `zonemap_off` (0 in v1), `values_off`, `docids_off` |
| null bitmap | one bit per docid, 1 = NULL; absent if the column is `NOT NULL` (v1 is NOT NULL: build ERRORs on a NULL) |
| values | dense per-docid value array (see below) |
| docids | dense **strictly-ascending** `uint64` array: the global docid (`weave_tid_to_docid`) each dense index denotes — the self-contained map above |
| *(reserved)* | zone-map: per-block min/max, added later without a version bump |

**Fixed-width types (int2/4/8, float4/8, date, timestamp, bool):** a dense array indexed by
docid, `typlen` bytes each. No encoding. `date`/`timestamp` are stored as their native
int representation and compared as integers.

**Text / varchar:** dictionary-encoded. A per-weft **dictionary of distinct values sorted by
the column's collation**, plus a dense array of fixed-width **dictionary ordinals** keyed by
docid. Equality is an ordinal compare; range (`name < 'M'`) is an ordinal **range** because
the dictionary is collation-sorted (resolve the constant to an ordinal boundary once, then
compare ordinals). The sorted dictionary is the same structure SuRF (Z3) and the deferred
zone-map want, and it makes the low-cardinality facet case (category, status, country —
the normal one) small. High-cardinality text degrades gracefully to a larger dictionary.

On-disk bytes are not trusted (`CONVENTIONS.md`): every field is validated by a pure
`include/weave/docvals.h` validator with a planted-bug fuzz target (§9). A corrupt store
`ERROR`s, never crashes, never returns a wrong gate set.

## 4. Column → channel mapping (opclasses)

A column becomes a docvals weft by carrying a **docvals opclass**, exactly as `wdoc`
carries `wdoc_lex_ops` and `wvec` carries `wvec_ip_ops`. One opclass per supported type
family (`int8_docval_ops`, `float8_docval_ops`, `text_docval_ops`, `date_docval_ops`, …),
each mapping the **standard btree comparison strategies** so a committer recognises them:

```
strategy 1 = <    2 = <=    3 = =    4 = >=    5 = >
```

These numbers are btree's, reused here on a distinct operator family, which is the same
per-family reuse `gram_ops` and `wdoc_lex_ops` already rely on (`am.h`); the scan
disambiguates by attribute. `CREATE INDEX ... USING weave (body wdoc_lex_ops, emb
wvec_ip_ops, price int8_docval_ops, category text_docval_ops)`.

## 5. Predicate → gate set

At scan start, for each pushed docvals qual `col <op> const`:

1. Resolve `const` to the stored representation (text: to a dictionary ordinal boundary).
2. One linear pass over the store's value array; emit the docids satisfying the predicate,
   **sorted**, in whatever key space the gate constructor consumes (the existing lexical
   gate already establishes that space via `weave_gate_shuttle_from_tidset`; docvals reuses
   the same constructor and the same space rather than inventing one).
3. **Multiple docvals predicates AND** by intersecting their sorted sets (or evaluating a
   conjunction in one pass). The result feeds `weave_gate_shuttle_from_tidset(WEAVE_CH_DOCVALS, …)`.

The pass is O(ndocs) over a compact array (~8 MB at 1M rows × int8) — cheap against the
vector work it saves (the whole point of §1). The deferred zone-map optimises **this pass**
by skipping value blocks whose [min,max] cannot satisfy the predicate; it changes no answer,
only the pass's cost, which is why it is a later non-breaking addition and not v1.

## 6. NULL semantics

A NULL value satisfies **no** comparison operator (SQL three-valued logic: `NULL < x` is
UNKNOWN, row excluded). The null bitmap is consulted first; a NULL docid is never emitted
into a comparison gate set. `IS NULL` / `IS NOT NULL` pushdown is **deferred** — those
predicates fall back to an executor Filter, which is correct (just unaccelerated) — so v1's
pushdown surface is the five comparison operators only. This keeps the false-negative
surface (§7) to one rule: emit iff not-null and comparison true.

## 7. The correctness contract (the docvals (C5) analog)

The channel does not score, so (C1)/(C2)'s `block_max ≥ score` does not apply verbatim. The
equivalent hazard is identical in kind, though: a gate that **omits a qualifying docid**
silently drops a valid row, and no fixed-expected-output test catches it (the answers stay
plausible, just missing) — the same class hard rule 1 exists for. The contract is therefore:

> The materialized gate set equals **exactly** the set of docids whose stored value
> satisfies the predicate, which equals the set the heap would return for the same qual.

Two property tests, both mandatory before merge (hard rule 1):

- **Store round-trip / predicate exactness** (backend-independent, `test/hegel/`): over
  random values, nulls, and constants, the set emitted by §5 == the set computed directly
  from the same values by the reference predicate, for every operator and type, including
  the text dictionary-ordinal boundary resolution (the one place an off-by-one silently
  drops or admits a row).
- **Index vs heap agreement** (`sql/`): the pushed-down gate returns the same rows as the
  identical qual answered by a seq scan, across selectivities, on a real corpus.

Plus `test/fuzz/fuzz_docvals` with a planted-bug variant that must abort (§9), and the
adversity gates every weft owes: crash-recovery TAP, torn-write, ASan/UBSan, and a
concurrency test (`PRODUCTION_READINESS.md` gates 7–11).

## 8. Planner pushdown

`customscan.c` must recognise a `col <op> const` on a docvals column and route it into the
gate path (`so->plainTids` today materialises the query's TIDs; the docvals set is produced
the same way and unioned into the required set), rather than leaving it an executor Filter.
The gate sweep's threat 2 applies (`bench/gatesweep.sh`): a missing index condition is
silent, so the plan must be **asserted** to carry an `Index Cond` on the docvals qual, or
the benchmark measures the executor and reports a beautiful flat curve. Both `enable_seqscan`
and `enable_bitmapscan` off in the test, and the plan checked for the index cond.

## 9. Build, flush, merge, vacuum

- **Build:** the build callback already receives every indexed attribute; write each
  docvals column's value into its weft's value array at the row's segment-local docid. Text
  builds the per-segment dictionary (collect distinct, sort by collation, assign ordinals).
- **Pending / flush:** a docvals value for a not-yet-flushed row lives in the pending buffer
  alongside its vector/lexical data; flush writes it into the new segment's store. (G29's
  lesson: a channel that does not scan pending is absent from answers until flush — the
  docvals gate must either see pending values or the documented limitation extends to it.)
- **Merge:** concatenate value arrays in docid order; **re-dictionary** text (union the
  input dictionaries, re-sort, remap ordinals) — the merge producer-2 pattern
  (`v7-merge-producer2`). `check-alloc`/`check-pdlower` apply to every new reader.
- **Vacuum:** values for tombstoned docids are dropped on the rewrite like any weft; the
  store participates in the one vacuum, one WAL stream (100 % GenericXLog, hard rule 2).

## 10. Upgrade path

New page kind `WEAVE_PK_DOCVALS` is already reserved and the v6 format's version gate makes
an old `.so` fail closed on a docvals-bearing index (X1). Extension bump 0.23.0 → 0.24.0
adds the opclasses and any SQL-visible functions. An index built before this channel simply
has no docvals weft in any bolt's `chandesc`; a docvals qual on such an index finds no weft
and falls back to an executor Filter (correct, unaccelerated).

## 11. Build order (de-risking hard rule 7)

The design is the full surface; the **implementation** brings it up one fixed-width type
end-to-end before generalising, so the fused interaction is never debugged against a
half-built store:

1. `int8`, comparison operators, single segment, NOT NULL — write store, pushdown, gate,
   both property tests. **Re-run `bench/RESULTS_DOCVALS_PRIZE.md`'s spike and confirm the
   scalar arm now falls with selectivity like the lexical arm** (the end-to-end proof).
2. `float8`, `int4/2`, `date`, `bool` — same fixed-width path, per-type opclass + oracle.
3. NULLs (null bitmap + exclusion) and the crash/torn-write/concurrency gates.
4. Text: dictionary encoding, collation-sorted, ordinal-boundary resolution + its exactness
   test — the highest-risk piece, done last against a store the rest of the stack trusts.
5. Merge (re-dictionary), then multiple docvals columns (AND intersection).

Each step is a `doc/PHASES.md` task with its gate; steps 1–3 are provisional until step 5's
scale run (hard rule 12 territory for the merge/vacuum parts).

## 12. Alternatives considered

- **Gate a plain `ORDER BY emb <#> q` instead of the fused scan.** Measured: prize **1.0×**.
  A plain vector scan scores every block regardless of the gate (lexical Index Cond *or*
  scalar Filter), so claim 3's savings exist only in the fused path. The channel targets the
  fused objective; a docvals gate on a plain vector order-by is correct but buys nothing.
  (`RESULTS_DOCVALS_PRIZE.md`, the instructive negative.)
- **On-disk zone-map (per-block min/max) in v1.** Deferred. It optimises §5's pass, not the
  answer, and adds a hard-rule-1 block bound (min/max true over the block, incl. collation
  for text). v1 reserves format space for it (§3) and captures the full prize without it,
  because the prize is the fused scan's skipped vector work, which the materialised set
  already delivers.
- **Inline variable-width text (offset+blob) instead of a dictionary.** Rejected: range
  scans would compare every value with the collation, and storage scales with total text
  length. The dictionary makes equality and range ordinal-compares and is small for the
  low-cardinality facets this is for.
- **A stored value column via a heap fetch (bitmap-style).** That is exactly today's
  executor Filter — the thing §1 measures as the loss. The value must live in the index,
  keyed by docid, to participate in one index / WAL / vacuum / visibility (claim 1) and to
  produce the gate without a heap round-trip.

## 13. Deferred / not in v1

Zone-map block-skip; `IS NULL`/`IS NOT NULL` pushdown; `IN (...)`/set membership; arrays and
composite types; expression facets (`lower(x)`); cross-column correlation. Each is additive
and none breaks the v1 format.
