# pg_weave

**One PostgreSQL index for BM25 text, vector similarity, fuzzy, regex, and facet
search — with a single fused top-k threshold across all of them.**

Status: **0.1.0, early.** The lexical channel works and is field-tested (it is
`pg_fts` 1.5.8, forked). The fuzzy channel is imported and unwired. The vector
codec is implemented and property-tested; its storage and graph are not. The
fused scorer is specified and unimplemented. `doc/PHASES.md` is the honest
picture of what exists.

## The idea

`weave` is one index access method. A single index can carry several *channels*
over the same set of rows:

```sql
CREATE INDEX docs_weave ON docs USING weave (
    body      lex_ops   (positions, trigrams, analyzer = 'english'),
    embedding vec_cosine_ops (bits = 4, graph = on),
    sku       gram_ops
);
```

and one query can rank across them under one threshold:

```sql
SELECT id, score()
  FROM docs
 WHERE body @@@ 'postgres AND replication'   -- lexical predicate
   AND tenant_id = 42                        -- scalar predicate
 ORDER BY fuse(body      <=> 'postgres replication'::wquery,
               embedding <=> $1::wvec,
               weights => '{0.4, 0.6}')
 LIMIT 10;
```

The reason to put these in one index is not code reuse. It is that all channels
in a segment share one dense document-id space, so a bound derived in one channel
can skip work in another. Post-filtering an ANN search by a lexical or scalar
predicate is the single biggest pain point in real pgvector deployments; here the
predicate is evaluated into a bitmap and pushed *into* the graph traversal and
into the SIMD block mask. Selective predicates make the query faster instead of
collapsing recall.

`doc/ARCHITECTURE.md` §3 is the full argument, including why this is impossible
across three separate extensions.

## What it claims

Four things, deliberately:

1. Lexical, vector, fuzzy, regex, and facet queries from **one** index — one WAL
   stream, one vacuum, one visibility rule.
2. **Fused-threshold top-k** instead of over-fetch-plus-RRF: one threshold, no
   over-fetch, and a score that means something. `doc/specs/FUSED_TOPK.md`.
3. Queries that get **faster** as predicates get more selective.
4. C, PostgreSQL-licensed, MVCC/WAL-native, `trusted`, no external storage engine
   — therefore on the contrib track.

## What it does not claim

- **Exact recall, sublinear latency, and minimal storage simultaneously.** Pick
  two. `weave.vec_recall = exact` costs a scan; the graph is approximate the same
  way HNSW is.
- **Beating `pg_trgm` on index size for unanchored cross-token substring search.**
  Our trigrams are inverted over the *vocabulary*, which is asymptotically
  smaller but token-aligned and cannot answer `LIKE '%tion refu%'`. There is an
  opt-in corpus-level channel for that, and with it enabled we are not smaller
  than GIN. `doc/ARCHITECTURE.md` §7.
- **Operational simplicity.** pgvector is small and boring; this is not. No
  technical fix exists, only a drop-in compatibility surface and per-channel
  opt-in.

`doc/ARCHITECTURE.md` §8 lists every dimension where pg_weave loses, and which
of those are fundamental versus merely unbuilt.

## Provenance

pg_weave is mostly not new code, and saying which parts are matters:

| subsystem | origin | status |
|---|---|---|
| segment engine, WAL, vacuum, MVCC, CIC, lexical BM25 channel | `pg_fts` 1.5.8 (PostgreSQL license) | forked wholesale, field-tested |
| SuRF trie, universal-Levenshtein, regex→trigram tiling | `pg_tre` 3.2.1 (MIT, relicensed) | imported, **not wired** |
| quantizer: rotation, Lloyd–Max codebook, renormalization scale | `turbovec` 1.0.0 (MIT) | **reimplemented in C**, tested |
| Vamana graph over quantized codes; filter-in-traversal | `pg_turbovec`, `zvec` | ideas only; **no zvec code copied** |
| fused-threshold top-k | new | specified, unimplemented |

`ci/fork-rename.sh` is the exact, reviewable transformation that produced the
lexical channel. `doc/LICENSING.md` has the per-file table and the rule for
contributions.

## Building

```sh
make PG_CONFIG=$(command -v pg_config)
make install
make installcheck
```

Requires PostgreSQL 17 or later. With Nix:

```sh
nix build .#pg17                                   # build
nix build .#checks.x86_64-linux.installcheck-pg17  # regression + isolation
nix build .#checks.x86_64-linux.tap-pg17           # TAP
```

Current state on PG 17.11 and PG 18: 3 regression tests, 2 isolation tests, and
61 TAP tests green; clean build under clang with PostgreSQL's warning set, zero
warnings. The standalone codec test runs 17,741 property checks with no backend:

```sh
gcc -O2 -I include -o /tmp/tq test/hegel/test_quantize.c \
    src/vector/quantize.c src/vector/pack.c -lm && /tmp/tq
```

## A finding worth reading before you contribute

`bench/RESULTS_BOUND_PRUNING.md` records a measurement taken before the fused
scorer was written. The block bound originally specified for the vector channel —
the obvious analogue of block-max WAND's `max_tf` — prunes **0.0 %** of blocks. A
centroid-plus-radius bound prunes **99.6 %**, but only if the document-id space
is ordered so that code blocks are spatially coherent; in heap order it prunes
0.0 % again.

Both facts are invisible to correctness tests. Both would have surfaced months
into implementation. That is the standard this project holds itself to: measure
the thing the design depends on before building on top of it, and write down the
negative result.

## Documentation

| file | what |
|---|---|
| `doc/ARCHITECTURE.md` | the thesis, the vocabulary, provenance, and what we lose |
| `doc/PHASES.md` | the build-out contract: tasks, specs, and gates |
| `doc/specs/FUSED_TOPK.md` | the novel algorithm and its correctness argument |
| `doc/specs/VECTOR_CHANNEL.md` | quantizer, bound, kernels, graph |
| `doc/specs/FUZZY_CHANNEL.md` | the vocabulary funnel |
| `doc/specs/SEGMENT_FORMAT.md` | on-disk format and invariants |
| `doc/LICENSING.md` | provenance and license analysis |
| `AGENTS.md` | orientation for coding agents, including the hard rules |

## License

PostgreSQL License. See `LICENSE`.
