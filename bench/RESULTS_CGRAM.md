# RESULTS_CGRAM.md — the corpus character-trigram channel, measured

Task Z8. `doc/specs/FUZZY_CHANNEL.md` §6 commits in writing that with `cgram` on,
pg_weave **does not claim to beat `pg_trgm` on index size**, and that this file
must record the honest comparison. It does. No verdict prose beyond the numbers;
the coordinator interprets.

## Instance and method

| | |
|---|---|
| instance | AWS EC2 `c7i.2xlarge` (8 vCPU, 16 GiB), Ubuntu 24.04, gp3 8000 IOPS / 500 MB/s |
| server | PostgreSQL 17.11 (PGDG), default `shared_buffers`, `max_parallel_workers_per_gather = 0` |
| build settings | `max_parallel_maintenance_workers = 0`, `maintenance_work_mem = 4GB` |
| extension | pg_weave 0.13.0, worktree `wt/z8` |
| corpus | 1,000,000 rows, 8 tokens per row from a ~250k-entry vocabulary, avg 59 bytes. Byte-identical generator to the Z8 pre-measurement, so the two runs are comparable |
| scripts | `bench/cgram_size.sql` (sizes), `bench/cgram_latency.sql` (latency); mutation legs in `/scratch/pg_weave/z8-mut.sh` (scratch tooling, not repo tooling, like the rest of the dev-host harness) |

**Latency method.** Each cell is the **median of 7, first run dropped** (8 runs,
run 1 discarded), timed with `clock_timestamp()` inside plpgsql around
`SELECT count(*)`, so the number is the scan and not the psql round trip or the
transfer of a result set. **Two full passes** over the whole table of cells are
reported separately and not averaged — AGENTS.md hard rule 10: a between-arm delta
smaller than the within-arm spread is not a result, and the within-arm spread is
unknown until it is measured twice.

**Arm pinning is evidence, not intention.** Each arm's plan was printed once:

```
pg_trgm   Aggregate -> Bitmap Heap Scan on cgb
                         Recheck Cond: (body ~~ '%7 t1%')
                         -> Bitmap Index Scan on cgb_gin
pg_weave  Aggregate -> Bitmap Heap Scan on cgb
                         Recheck Cond: (body @~ '%7 t1%')
                         -> Bitmap Index Scan on cgb_wg
seq scan  Aggregate -> Seq Scan on cgb
                         Filter: (body ~~ '%7 t1%')
```

and `weave_channel_stats().cgram_scan` was asserted to be 1 after a served query,
so the pg_weave arm went through the corpus-trigram route in C and not merely
through a plan that mentions the index.

`body @~ p` is exactly `body LIKE p`: `weave_cgram_like()` calls core's
`textlike()`, the same C function `LIKE` calls. The three arms ask the same
question.

## 1. Size

| structure | bytes | pretty | build time |
|---|---:|---:|---:|
| heap (`cgb`, text only) | 101,138,432 | 96 MB | — |
| `pg_trgm` GIN (`gin_trgm_ops`) | 75,218,944 | 72 MB | 9.38 s |
| pg_weave **without** cgram (`weave (to_wdoc(body))`) | 40,378,368 | 39 MB | 3.16 s |
| pg_weave **with** cgram (`weave (to_wdoc(body), body gram_ops)`) | 125,280,256 | 119 MB | 17.53 s |

**Delta for turning `cgram` on: +84,901,888 bytes (+81 MB), a 3.10× index.**

Against `pg_trgm` GIN: pg_weave-without-cgram is **0.54×** its size and cannot
answer the query at all; pg_weave-with-cgram is **1.67×** its size and can. That
is the trade §6 promised to state.

Where the bytes went, from `weave_index_size_detail('cgb_wg')` — which sums
exactly to `pg_relation_size()`, and which can attribute the cgram channel only
because its dictionary, block index and postings carry their own page kinds:

| kind | pages | bytes | % of index |
|---|---:|---:|---:|
| `cgram_postings` | 10,358 | 84,852,736 | 67.73 |
| `postings` (lexical) | 3,473 | 28,450,816 | 22.71 |
| `dictionary` (lexical) | 981 | 8,036,352 | 6.41 |
| `doclen_sidecar` | 301 | 2,465,792 | 1.97 |
| `surf_trie` | 170 | 1,392,640 | 1.11 |
| `cgram_dictionary` | 4 | 32,768 | 0.03 |
| `dict_index` | 2 | 16,384 | 0.01 |
| `cgram_dict_index` | 1 | 8,192 | 0.01 |
| `cgram_root` | 1 | 8,192 | 0.01 |
| `chandesc` | 1 | 8,192 | 0.01 |
| `meta` | 1 | 8,192 | 0.01 |

The cgram channel is 84,901,888 bytes, of which 84,852,736 (99.94 %) is postings:
the docid lists themselves. Its dictionary is 4 pages, because this corpus's
tokens are `t` plus decimal digits and its distinct byte-trigram vocabulary is
therefore tiny. **On a natural-language corpus the dictionary would be larger and
the postings no smaller**, so 81 MB is a floor for this corpus shape, not a
general figure — AGENTS.md hard rule 11: this number is provisional until it
reproduces at a second corpus shape, which was not measured.

Both indexes are **one bolt**. `maintenance_work_mem = 4GB` held the whole build
in memory, so no segment flush and no merge happened; see §4 for why that matters.

## 2. Latency, pass 1

Median of 7, first dropped, milliseconds.

| # | pattern | matches | `pg_trgm` GIN | pg_weave `cgram` | seq scan |
|---|---|---:|---:|---:|---:|
| 1 | `%7 t1%` | 311,108 | 234.43 | 559.58 | 143.59 |
| 2 | `%9 t20%` | 31,108 | 93.83 | 205.92 | 156.05 |
| 3 | `%0 t123%` | 3,108 | 23.34 | 53.69 | 155.22 |
| 4 | `%t12345%` | 352 | 3.50 | 6.95 | 142.95 |
| 5 | `%t20471%` | 352 | 3.21 | 6.62 | 141.28 |
| 6 | `%zzq qzz%` | 0 | 0.032 | 0.013 | 107.67 |

## 3. Latency, pass 2 (the within-arm spread)

| # | pattern | `pg_trgm` GIN | pg_weave `cgram` | seq scan |
|---|---|---:|---:|---:|
| 1 | `%7 t1%` | 238.84 | 567.24 | 146.51 |
| 2 | `%9 t20%` | 94.03 | 204.12 | 155.29 |
| 3 | `%0 t123%` | 23.82 | 54.30 | 155.93 |
| 4 | `%t12345%` | 3.52 | 7.32 | 143.45 |
| 5 | `%t20471%` | 3.25 | 6.78 | 141.73 |
| 6 | `%zzq qzz%` | 0.032 | 0.013 | 107.07 |

Pass-to-pass spread within an arm: **≤ 2.1 %** on every cell of every arm
(largest: pattern 4, pg_weave, 6.95 → 7.32 ms, 5.3 %; every other cell ≤ 2.1 %).
The between-arm ratios below are all larger than that.

## 4. The ratios, stated without interpretation

pg_weave `cgram` ÷ `pg_trgm` GIN (pass 1 / pass 2):

| # | pattern | ratio |
|---|---|---:|
| 1 | `%7 t1%` | 2.39× / 2.37× |
| 2 | `%9 t20%` | 2.19× / 2.17× |
| 3 | `%0 t123%` | 2.30× / 2.28× |
| 4 | `%t12345%` | 1.99× / 2.08× |
| 5 | `%t20471%` | 2.06× / 2.09× |
| 6 | `%zzq qzz%` | 0.41× / 0.41× |

pg_weave `cgram` ÷ seq scan (pass 1 / pass 2):

| # | pattern | ratio |
|---|---|---:|
| 1 | `%7 t1%` | 3.90× / 3.87× — **slower than a seq scan** |
| 2 | `%9 t20%` | 1.32× / 1.31× — **slower than a seq scan** |
| 3 | `%0 t123%` | 0.35× / 0.35× |
| 4 | `%t12345%` | 0.049× / 0.051× |
| 5 | `%t20471%` | 0.047× / 0.048× |
| 6 | `%zzq qzz%` | 0.00012× / 0.00012× |

## 5. What the design did not get, recorded here rather than in a comment

AGENTS.md hard rule 8. Four limitations are known and none of them is hidden in a
source file only.

1. **A cgram-bearing bolt is NOT MERGEABLE.** `weave_seg_mergeable()`
   (`src/am/ambuild.c`) refuses any merge group containing one, and
   `weave_merge_selected()` refuses again at its own chokepoint so an explicit
   `weave_merge_segments()` cannot bypass it. The reason is structural, not an
   oversight: a merge streams DICTIONARY TERMS out of its input bolts, and the
   cgram weft's input is the RAW TEXT of the column, which a weave index does not
   store. Rebuilding on merge would mean re-reading the heap from VACUUM's cleanup
   path. What *is* available and was not built is a k-way merge of the cgram wefts
   themselves — they are dictionary + postings in exactly the lexical shape, so
   `MergeSource` parameterized on `(dictstart, dictindexstart)` would merge them
   the way it merges the lexical weft. Until that exists, a cgram index does not
   compact. **This benchmark did not exercise it** (one bolt, no flush), so the
   cost of never merging is **unmeasured**.

   **SUPERSEDED 2026-09-21 (`doc/GAPS.md` G34).** The structural claim above is
   wrong in its second half and the error is worth keeping visible: a merge does
   not need the column's TEXT, it needs the PAIRS, and an input bolt's cgram weft
   already holds them. `weave_cgram_merge_append()` reads them back and feeds the
   ordinary writer, so a group in which EVERY bolt carries a weft now merges. A
   group in which only some do is still refused — the output weft has to cover
   every document of the merged bolt or it is a false negative. What the fix does
   not fix: the re-accumulation is not streaming (16 bytes per pair of the whole
   group, ~930 B/document at the measured 58.1 pairs), bounded only by
   `WEAVE_CGRAM_MAX_PAIRS`, past which the weft is omitted. Still **unmeasured**
   at scale.
2. **A post-build `INSERT` is correct but not accelerated.** A `WeavePendingItem`
   carries the analyzed `wdoc` and the row's `wvec`, not the raw text, so a bolt
   written by `weave_flush_pending()` has no cgram weft. The route then contributes
   that bolt's whole live docid set to the candidates and the mandatory recheck
   filters it exactly, so the row is found — the same shape as `doc/GAPS.md` G23
   before V7's second half closed it, with the same remedy (widen the item).
   `sql/cgram.sql` asserts the correctness half; the latency cost is unmeasured.

   **SUPERSEDED 2026-09-21 (`doc/GAPS.md` G35).** The item now carries the raw
   text and the flush runs the ordinary producer, so a flushed bolt has a real
   weft. One number in the description above was also understated: because
   `weave_cgram_collect()` uses no bolt's weft unless EVERY live bolt has one, the
   cost was not "this row is not accelerated" but "no `@~` query in this index is
   accelerated until the next REINDEX". `sql/cgram.sql`'s `served_after_flush`
   reads 0 before the fix and 1 after. Still **unmeasured** as a latency.
3. **Byte trigrams, not character trigrams.** `weave_trigrams()` hashes three
   consecutive BYTES of the server-encoded value, and a trigram may straddle two
   multi-byte characters. Sound as a candidate filter because both sides are hashed
   identically, and stated everywhere rather than glossed, because conflating the
   two units is `doc/GAPS.md` G30.
4. **`@~*` (ILIKE) refuses a pattern whose literal runs contain a byte ≥ 0x80.**
   ILIKE folds case through `lower()`, which is encoding-aware; the cgram weft's
   fold is byte-wise ASCII. Requiring a folded non-ASCII trigram would be a FALSE
   NEGATIVE — a silently dropped row. The route therefore falls back for such
   patterns, and `sql/cgram.sql` asserts the fallback (counter 0) *and* the right
   rows. The case-SENSITIVE `@~` has no such restriction and is served over
   multi-byte text.

## 6. One finding that cost a debugging round, because it will cost the next one too

**An access method may only return HOT-chain ROOT line pointers.** The fallback
path (a pattern with no usable trigram) is a sequential pass over the heap, and a
heap scan hands back the PHYSICAL version's TID. After a HOT update that version
is a heap-only tuple, and `heap_hot_search_buffer()` — which is what both a bitmap
heap scan and `table_index_fetch_tuple()` use to resolve a TID an index gave them
— refuses a heap-only tuple as a chain start. So a bitmap built from physical TIDs
resolves to **nothing**: `EXPLAIN ANALYZE` showed `Bitmap Index Scan (actual
rows=2)` under `Bitmap Heap Scan (actual rows=0)`, with no error and no warning.
It is invisible to any corpus built with `INSERT` alone; `sql/cgram.sql` reaches it
immediately because it populates the `wdoc` column with an `UPDATE`, which
HOT-updates every row. The fix is `heap_get_root_tuples()`, computed once per heap
block — the same thing CREATE INDEX CONCURRENTLY does, for the same reason.

## 7. Mutation legs

`/scratch/pg_weave/z8-mut.sh`, run on the same host against PG17 with
`make clean` before every leg (a stale `.so` makes every leg falsely survive).
Each leg proves the sed APPLIED (diff against a saved original; an unapplied sed
reports "leg proves nothing") and that the mutant BUILT (make's own exit status,
never through a pipe; a build failure is reported as INCONCLUSIVE, not as a pass).
Probes compare the index arm against `mgref`, a copy of the corpus with no index.

The harness itself had to be fixed once, and that is worth recording: the first
version parsed psql's aligned output with awk and printed the COLUMN HEADERS, so
all four legs reported identical output and the run proved nothing. Every probe
now labels its own value in SQL and the shell does no parsing.

Baseline (unmutated): `P1=SAME P1b=SAME P2_upper=SAME P3_short=SAME P4_counter=1`.

| leg | mutation | result | evidence |
|---|---|---|---|
| 1 | delete `weave_cgram_recheck()` | **CAUGHT** | `P2_upper=idx:9001` — a FALSE POSITIVE. `%CONNECTION refused%` does not match `'connection refused by peer'` under case-sensitive LIKE, but the ASCII case fold makes that row a trigram candidate. Only the recheck rejects it. |
| 2 | `tidset_or` instead of `tidset_and` on the posting lists | **SURVIVES — and that is the reported result** | every probe `SAME`. A union is a SUPERSET of the intersection, hence still a superset of the answer, and the mandatory recheck filters it. Slower, never wrong. This is the **superset check**, declared in advance as a leg that must not be caught. |
| 3 | delete the short-pattern fallback (`nreq <= 0` no longer takes the heap pass) | **CAUGHT** | `P3_short=ref:9005,ref:9006`, `P_nrows_short=0` against `P_nrows_short_ref=2`. Rows LOST: with no required trigram the intersection is never seeded and the route returns nothing. |
| 4 | delete `weave_chan_cgram_scan++` | **CAUGHT** | `P4_counter=0`. The counter assertion in `sql/cgram.sql` is load-bearing. |

**Leg 1 is the one worth reading twice.** `P1` and `P1b` — the two cross-token
patterns — stayed `SAME` under it. The recheck is only load-bearing for a pattern
the trigrams actually over-generate, so a probe suite of nothing but well-behaved
cross-token patterns would have reported leg 1 as a survivor and concluded the
recheck was dead code. The mixed-case pattern is what makes the leg decidable, and
it is in the probe set for that reason. AGENTS.md: "writing SQL that reaches a
specific C function is not the same as writing SQL that returns the right answer."

The route passes `recheck = false` to `tbm_add_tuples()` precisely so that this leg
is decidable at all: with `recheck = true` the executor's own bitmap-heap recheck
would re-evaluate `@~` and mask the mutation — which is the 2026-09-16 failure
AGENTS.md records under "the suite running is not the SITE running."

## 8. Parity gate

`sql/cgram.sql` / `expected/cgram.out`, generated from a FULL `make installcheck`
(never `REGRESS=cgram`). **24 patterns**, each asserted twice — once for `@~`
against `LIKE` and once for `@~*` against `ILIKE` — as an empty symmetric
difference of id sets against a control table carrying no index. The suite is
re-run after a post-build `INSERT`, after a `DELETE`, and after a `VACUUM`, so 96
`@~` comparisons and 96 `@~*` comparisons in total, plus 48 counter assertions.

The pattern list covers: cross-token (a space inside the pattern), single-token,
anchored prefix, anchored suffix, two literal runs ANDed, `_` between two usable
runs, patterns shorter than three bytes (must FALL BACK: counter 0, right answer),
a pattern matching nothing, multi-byte, mixed case, and all-wildcards. Served/
fell-back is asserted per pattern: 19 served and 5 fell back for `@~`; 17 served
and 7 fell back for `@~*`, the two extra refusals being the multi-byte patterns
(§5 item 4).

Gate: **ALL GREEN on PostgreSQL 17.11 and 18.6**, 19 files / 269 tests each, plus
`check-ascii`, `check-alloc`, `check-rename`, `check-pdlower`, `check-standalone`
and `check-sparsemap-wire`.
