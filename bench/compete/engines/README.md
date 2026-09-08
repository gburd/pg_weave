# Competitor engines

One script per engine, one EC2 host per engine, five verbs each:

```
bash <engine>.sh provision            # OS, PostgreSQL from source, this engine
bash <engine>.sh load <corpus>        # shared corpus + this engine's indexed column
bash <engine>.sh index                # timed CREATE INDEX, then prewarm
bash <engine>.sh gate                 # correctness gates -> gates.jsonl
bash <engine>.sh measure <n> <warmup> <run>   # raw timing samples -> raw.jsonl
```

`weave.sh` is the reference implementation of the contract; `common.sh` holds
every shared step and must be used rather than reimplemented. Line 3-5 of each
script carries an `# axis:` comment that the orchestrator greps to choose the
instance type (`# axis: vector` gets the bigger box).

Engines cannot be co-installed: pg_search, pg_textsearch and VectorChord-bm25 all
define an access method named `bm25`, and pg_tre and pg_trgm both define `%`
(`pg_fts/bench/RESULTS_4WAY_2026-07-29.md:22-25`). Hence one host per engine,
always.

## Capability matrix

This table exists so a missing row in the results tables is read as what it is --
a capability the engine does not have -- rather than as a harness bug or an
oversight. An engine that can only answer four of fifteen queries is not
"fastest"; it is narrower, and the narrowness belongs next to the latency.

| capability | pg_weave | pg_fts | pg_textsearch | tsvector + GIN |
|---|---|---|---|---|
| match predicate (`WHERE`) | yes, `d @@@ q` | yes, `d @@@ q` | **no** | yes, `tsv @@ q` |
| BM25 ranking | yes, `d <=> q` | yes, `d <=> q` | yes, `content <@> q` | **no** -- `ts_rank_cd`, not BM25 |
| index-assisted top-k (ORDER BY pushdown) | yes | yes | yes | **no** -- rank computed per matching row |
| bare `ORDER BY` with no `WHERE` | yes (task L7) | **no** -- `amoptionalkey=false` | yes (its only form) | **no** -- seq scan + top-N sort |
| `count(*)` pushdown | yes, `Custom Scan (WeaveCount)` | yes, `Custom Scan (FtsCount)`, needs preload | **no** | partial -- bitmap index scan, no dedicated count path |
| boolean AND / OR | yes | yes | **no** | yes |
| boolean NOT | yes, `a & !b` | yes, `a & !b` | **no** | yes, `a & !b` |
| prefix | yes, `word*` | yes, `word*` | **no** | yes, `word:*` |
| phrase | yes | yes | **no** | yes, `<->` |
| fuzzy / regex / vector in the same index | yes (project thesis) | no | no | no |
| self-compacting build | yes (task L8) | **no** -- needs `fts_merge` + `fts_vacuum` | compacts at commit | pending list, empty after a fresh build |
| exposes document frequency | yes | yes | **no** -- only derivable by index exhaustion, 474 s for a common term | yes |

## Consequences for the results tables

- **pg_textsearch fills only the `ranked_*` rows.** It has no match predicate, so
  `count_common`, `count_and`, `count_or2`, `count_not`, `count_prefix` and
  `count_rare_marker` are not written to its spec at all. Its `ranked_*` form is
  itself the bare-`ORDER BY` shape, so `bare_orderby_*` is not duplicated for it.
- **pg_textsearch has no match-count column.** Match counts are mandatory
  alongside latency (`STRATEGY.md` §3.4) because engines with different analyzers
  match different numbers of documents. This engine exposes no `df` function and
  the only derivation is index exhaustion, which took 474 s for one common term
  in the prior run. Its counts are read from the GIN and pg_weave arms, which
  index byte-identical `content` (asserted by `fingerprint_sql`).
- **GIN and pg_fts show a seq scan for `bare_orderby_*`, on purpose.** Those
  queries carry no `expect_plan`, because for those engines the seq scan is the
  correct and expected plan: pg_fts is `amoptionalkey=false`
  (`pg_fts/pg_fts_am.c:6255`) and `ts_rank_cd` is not an ordering operator in
  `gin_tsvector_ops`. Asserting an index there would record a PLAN FAIL for
  correct behaviour. The rows are measured anyway, to size the gap task L7
  closed rather than assert it in prose.
- **GIN is not top-k parity gated.** `ts_rank_cd` is not BM25, so a parity gate
  against the BM25 oracle would fail on a definitional difference. The ranking
  difference is recorded here, not as a red FAIL beside a working engine.
- **pg_fts is charged for its own compaction.** pg_weave compacts at the end of
  `CREATE INDEX` (task L8); pg_fts does not. `fts.sh` puts `fts_merge` and
  `fts_vacuum` inside the timed statement, so its build-time and index-size
  columns describe the same physical state pg_weave's do. Timing the bare
  `CREATE INDEX` would publish a ~3.4x inflated size, which is unfair *to*
  pg_fts.
- **pg_fts and pg_textsearch both need `shared_preload_libraries`.** pg_fts needs
  it only for the `count(*)` CustomScan
  (`pg_fts/bench/RESULTS_4WAY_2026-07-29.md:103-104`); pg_textsearch fails
  `CREATE EXTENSION` without it
  (`pg_fts/bench/data_5way_159/pgts_bench_result.txt:8-11`). Both set `PRELOAD`
  before `start_pg`, and build the extension before starting the server, because
  a server told to preload a missing library will not start.

## Query labels are a contract

The analyzer joins engines by label, so a renamed label does not produce a
mismatch -- it produces a silently missing comparison. `weave.sh`'s labels are
canonical:

```
ranked_rare_k10   ranked_rare_k100    bare_orderby_rare    count_common
ranked_mid_k10    ranked_mid_k100     bare_orderby_mid     count_and
ranked_common_k10 ranked_common_k100  bare_orderby_common  count_or2
                                                           count_not
                                                           count_prefix
                                                           count_rare_marker
```

Every spec must also carry `fingerprint_sql`, `index_size_sql`, `version_sql`
and `build_seconds` read from `$OUTDIR/build.txt`. `fingerprint_sql` is
`SELECT md5(string_agg(h, '' ORDER BY id)) FROM (SELECT id, md5(content) AS h FROM docs) t` verbatim in every
engine: the analyzer fails the whole run when two engines disagree, which is the
mechanical form of the check whose absence forced the retraction of an entire
5-way comparison.
