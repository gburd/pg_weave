# Upstream proposal: let an ordering index scan hand its ORDER BY value to the target list

Status: **design + compiling patch sketch, not sent.** Written 2026-10-01 for pg_weave task
F3 (`doc/PHASES.md`); the patch is `doc/upstream/0001-orderby-values-to-tlist.patch`,
built against PostgreSQL `master` at `fcc0e27f45e`.

## 1. The problem, in one query

```sql
SELECT id, body <=> 'postgres index'::wquery AS score
  FROM docs
 ORDER BY body <=> 'postgres index'::wquery
 LIMIT 10;
```

The plan is an Index Scan with `Order By: (body <=> ...)`. The index AM computes each
row's ranking value while it walks the index, and **returns it** in
`IndexScanDesc.xs_orderbyvals`. The executor then **throws it away** and evaluates
`body <=> '...'` again in the scan's projection, once per output row. Two costs follow:

1. **Wasted work.** For pgvector this is a second full distance computation per row.
   Matthias van de Meent found it in pgvector#359, and Heikki's 2023 thread "Avoid
   computing ORDER BY junk columns unnecessarily" (msg
   `2ca5865b-4693-40e5-8f78-f3b45d5378fb@iki.fi`) is the general version.
2. **A different number.** This is the one that matters for search engines and that the
   junk-column work does not touch. A relevance score such as BM25 depends on corpus
   statistics (document frequency, average document length) that only the index has. The
   operator's function body, called on one heap row, cannot know them, so it returns an
   approximation. The query above therefore **orders by one number and displays
   another**. Chris Cleveland raised exactly this on -hackers in May 2024 ("Why is FOR
   ORDER BY function getting called when the index is handling ordering?",
   `CABSN6Vc0VXr+1w+dn0iUJ--LgGUwM6XwBxcXN+vF15fMsYE9cg@mail.gmail.com`):

   > It would also be nice if the orderbyval could be made available in the projection.
   > That way we could report the score() in the result set.

pg_weave hits this as F3. `doc/specs/FUSED_TOPK.md` sect. 7 promises
`SELECT id, score()` for a fused ranking, and the only implementations available today
are a backend-global "last row scored" variable or a separate SRF. The first returns
plausible wrong values as soon as a Sort or join intervenes. The second is a different
query shape.

## 2. Tom Lane's objection, and why this proposal is not what he objected to

In the same thread Tom said:

> An index ordering operator is an optimization that the planner may or may not choose to
> employ. If you've designed your code on the assumption that that's the only possible
> plan, it will break for any but the most trivial queries.

That objection is to **making the operator's own value index-dependent**, so that the
same SQL expression means different things under different plans. This proposal does not
change what `body <=> q` means anywhere.

What it changes is narrower. When an Index Scan **is** chosen and the AM reports its
value as **exact** (`xs_recheckorderby = false`), the executor may *substitute* that
value for a target-list occurrence of the identical ORDER BY expression. It may not
substitute anything else.

- **For an AM whose exact value equals the operator's result** (GiST on points, btree_gist,
  pg_trgm `<->`), the substitution is a pure optimization with no observable change. That
  is the pgvector/Heikki case.
- **For an AM whose value differs from the operator's** (pgvector's squared L2, every
  BM25 AM), the substitution is observable, and §4 is where the semantic question is
  argued rather than assumed.

## 3. Mechanism

The design mirrors what Index Only Scan already does for indexed columns. There, setrefs
rewrites target-list expressions that match an index column into `INDEX_VAR` references.
Here, it rewrites target-list expressions that match an ORDER BY expression into
references to the scan's ORDER BY values.

### 3.1 Plan time: `set_plan_refs`, case `T_IndexScan`

After the existing `fix_scan_list` calls, walk the target list. For each
`TargetEntry` whose expression is `equal()` to the i-th element of `indexorderbyorig`,
replace it with

```c
Var(varno = INDEX_VAR, varattno = i + 1, vartype = exprType(orig), ...)
```

and record `i` in a new `IndexScan.indexorderbytlist` bitmap, so EXPLAIN can show what
happened. No other node changes. IndexOnlyScan is untouched; it already uses `INDEX_VAR`
for index columns, so the new mapping is IndexScan-only for now. §6 lists IndexOnlyScan
as follow-up work.

Using `equal()` on the setrefs-processed expressions is the same matching discipline
`fix_upper_expr` uses for non-Var upper references. It is cheap and conservative.
Tom's 2023 concern (1), "the planner [must] not spend too much effort on looking for
subexpression matches", is met because only **top-level** target-list entries are
compared, against a list that is almost always of length 1. An ORDER BY expression
buried inside a larger target expression is not matched. That is a missed optimization,
not a wrong answer.

### 3.2 Run time: `nodeIndexscan.c`

`ExecInitIndexScan` builds a virtual slot `iss_OrderBySlot` with one attribute per ORDER
BY key, typed as the ORDER BY expressions. The scan's projection is told to read
`INDEX_VAR` from it: `ExecAssignScanProjectionInfo` already sets the expression
context's scan tuple, and `econtext->ecxt_scantuple` keeps serving heap Vars. The
`INDEX_VAR` Vars go through the default branch of execExpr's Var compilation, so they
need a slot of their own. The sketch uses `ecxt_innertuple`, which a scan node never
otherwise sets, and compiles `INDEX_VAR` Vars in an IndexScan's target list as
`EEOP_INNER_VAR`. That keeps the change out of the expression interpreter entirely.

On each returned row, `IndexNext` (and `IndexNextWithReorder`) store the AM's
`xs_orderbyvals` / `xs_orderbynulls` into `iss_OrderBySlot` before projecting:

- **Exact case** (`xs_recheckorderby == false`): the AM's value is the ordering value by
  contract, and it is what the slot receives.
- **Lossy case** (`xs_recheckorderby == true`): the executor has just recomputed
  `iss_OrderByValues` from the heap tuple in `EvalOrderByExpressions`, and **that**
  recomputed value goes into the slot. So a lossy AM gets the operator's own value, as
  today, without a second evaluation. A lossy AM's index value is only a lower bound, so
  substituting it would be wrong; this rule makes that impossible.
- **Reorder queue:** a tuple popped from the queue carries its `orderbyvals` copy, and
  those are the values the slot receives.

### 3.3 EXPLAIN

`EXPLAIN VERBOSE` prints `Output: id, (body <=> '...'::wquery)` today. With the patch it
still prints the expression, by deparsing the `INDEX_VAR` back through
`indexorderbyorig`, the same way IndexOnlyScan's `INDEX_VAR`s are deparsed through
`indextlist`. It adds one line, `Order By Values Used: 1`, so a user can tell the two
behaviours apart. That line is what a regression test asserts.

## 4. The semantic question, argued

**Is it acceptable that `SELECT body <=> q` returns the index's number under an Index
Scan and the operator's number under a Seq Scan?**

Arguments that it is:

1. **It is already true of the ORDER.** Under a Seq Scan + Sort the rows are ordered by
   the operator's value, and under an Index Scan by the index's. Every AM with an
   approximate or statistics-dependent ordering (pgvector HNSW/IVFFlat, every BM25
   extension) already makes the plan observable through `ORDER BY ... LIMIT`. Today the
   displayed number disagrees with the order it was sorted by, which is strictly worse
   than showing the number the rows were sorted by.
2. **The AM opts in per row.** `xs_recheckorderby = false` already means "the value I am
   reporting is the ordering value, and you may compare it". That is exactly the
   statement substitution needs. An AM that does not want the substitution reports
   `true`, and its displayed value stays the operator's.
3. **The opt-out costs nothing to keep exact.** If an operator class is not happy having
   its index value displayed, a new `amroutine` flag `amorderbyvalsexact` (default
   `false` for out-of-core AMs, `true` for GiST/SP-GiST, whose distances are exact) gates
   the setrefs rewrite. This proposal recommends the flag so that **no existing AM's
   behaviour changes unless it asks**. The cost is one bool, and the debate is then only
   about whether core AMs flip it.

Arguments against, which a reviewer will raise:

- **"The same expression gives different values under different plans."** True when an
  AM sets the flag. The mitigation is the flag itself, plus documenting that an AM which
  sets it is asserting index value = displayed value. This is the crux, and the proposal
  should go to -hackers framed as this question, not as a performance patch.
- **Volatile or set-returning ORDER BY expressions.** These are excluded already: an
  ordering operator must be immutable to be an index ORDER BY at all.
- **Collation and typmod.** The ORDER BY expression's result type is used verbatim, and
  only `float8`/`float4` are produced by `index_store_float8_orderby_distances`. So the
  rewrite is gated on `exprType(orig) IN (FLOAT8OID, FLOAT4OID)`, the only types whose
  values the AM can deliver today.
- **Junk columns.** This is orthogonal to Heikki's junk-column patch and composes with it.
  That patch stops computing ORDER BY values nobody displays; this one stops recomputing
  values somebody does display. With both, a pgvector `SELECT id, dist ... ORDER BY dist`
  computes the distance once per row instead of three times.

## 5. What pg_weave gets

With the flag set in `weave_handler()`:

```sql
SELECT id, fuse(body <=> q, emb <-> v) AS score
  FROM docs
 ORDER BY fuse(body <=> q, emb <-> v)
 LIMIT 10;
```

returns the fused scorer's own `-S`, exact per §3a, and `score()` becomes this SQL with
the sign flipped. `score_parts()` needs a little more: the AM would report one value per
ORDER BY key, and the fused scan already has one key per channel. A target list naming
the individual channel expressions (`body <=> q`, `emb <-> v`) would get each channel's
contribution, at no extra mechanism, provided the fused scan fills `xs_orderbyvals[i]`
per channel. Today it fills slot 0 only; `amscan.c` explains why the others are NULL.

## 6. Not in scope

- IndexOnlyScan (same idea, another node; follow-up).
- Bitmap scans (no ordering, nothing to substitute).
- Matching ORDER BY expressions nested inside a larger target expression.
- Non-float ORDER BY types.

## 7. Release targeting

This is a planner/executor change with a catalog-free API addition (one `IndexAmRoutine`
bool). That fits a major release, not a minor one: the next one open for features, via a
CommitFest entry. Nothing here prevents proposing it. The things that would are:

1. **The semantic question in §4 must be settled on -hackers first.** Tom has opposed the
   adjacent idea, and the patch should not be sent as "a performance fix" when the change
   that matters is semantic.
2. **Heikki's junk-column thread should be checked for current status.** If it is moving,
   this should be sequenced after it or folded into it, not offered as a competitor.
3. **Tests:** a `regress` case over GiST points (flag on, values identical, EXPLAIN shows
   `Order By Values Used`), a lossy-polygon case (the recomputed value is used), and a
   reorder-queue case. The sketch has none yet.

## 8. The patch sketch, and what was measured

`doc/upstream/0001-orderby-values-to-tlist.patch` is one commit (`b6577e1dd22` on branch
`orderbyval-proposal` in the `~/ws/postgres` bare repository, worktree
`/scratch/pg_weave/pg-orderbyval`). It is based on upstream `fcc0e27f45e`, the last
non-fork commit under `master`, and is 438 lines touching 11 files. It implements §3.1,
§3.2 and §3.3, and sets the flag for GiST and SP-GiST.

**The rewrite was proven to fire, not inferred from an unchanged answer.** The obvious
check, comparing returned values against the operator, cannot fail: for GiST points the
two are identical whether or not the rewrite happened. The positive control is an
expression index whose ORDER BY expression `cnt_pt(p) <-> point(5,5)` calls a plpgsql
function that counts its own invocations, on the same table and the same query:

| build | `cnt_pt` calls for 100 returned rows | values equal to the operator |
|---|---|---|
| upstream `fcc0e27f45e` | **100** (one per row, in the target list) | 100 / 100 |
| patched | **0** | 100 / 100 |

Also checked on the patched build, each against the operator recomputed under a Seq Scan:
- an exact GiST point scan (200 / 200 equal);
- **a lossy GiST polygon scan**, which exercises the recheck path and its recomputed values
  (300 / 300);
- a LATERAL nested loop with an outer-row parameter in the ORDER BY, which exercises
  rescans (150 / 150);
- an SP-GiST box scan under a `rank() OVER (ORDER BY ...)` window, with both values and
  ranks checked (3000 / 3000).

A target expression that only *contains* the ORDER BY expression, such as `(p <-> q) * 2`,
is not rewritten. A btree plan is untouched.

**Core regression suite (`meson test --suite regress`):** two files differ, `box` and
`polygon`, both in the same way. Above a rewritten scan, EXPLAIN deparses a WindowAgg's
`ORDER BY (b <-> q)` as `ORDER BY ((b <-> q))`, with one more parenthesis pair, because the
key is now a reference to a scan output column. That is how every upper-node reference to
a computed scan column already prints. The rows and ranks of that exact query were checked
equal to the unpatched answer. The fix is to accept the new expected output; it is not
done in the sketch.

**Also found, and NOT this patch:** a hand-built GiST opclass over the point support
functions that omits support function 11 (sortsupport) crashes **stock** master with
`TRAP: failed Assert("box->low.y <= box->high.y")` in `gistproc.c` during `CREATE INDEX`.
It reproduced on the unpatched build, which is how it was told apart from this change.
`gist_point_sortsupport` sorts points on their own, while the opclass's other functions
treat keys as boxes. It is a separate potential bug report (an assertion reachable from
SQL by a superuser defining an opclass), recorded here so it is not lost.

**Missing before it could be sent:**
- regression tests covering the four cases above, plus the counter control;
- the `box`/`polygon` expected-output updates;
- the `doc/src/sgml/indexam.sgml` text for `amorderbyvalsexact`;
- a pass over `contrib/` AMs (`bloom`, the test module `dummy_index_am`), which leave the
  flag false by default and should;
- IndexOnlyScan (§6).

And first of all, the -hackers conversation in §7.
