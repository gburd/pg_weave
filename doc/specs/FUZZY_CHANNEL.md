# Specification: the fuzzy, regex, and prefix channel

Status: **sources imported from pg_tre, not wired.** Tasks **Z1**–**Z9** in
`doc/PHASES.md`. Import mapping and wiring TODO: `doc/specs/IMPORT_pg_tre.md`.

## 1. The substitution this whole channel rests on

pg_tre inverts trigrams over the **corpus**: one posting list per trigram, listing
documents. pg_weave inverts them over the **vocabulary**: one posting list per
trigram, listing *dictionary term ordinals*.

That is the entire design difference, and it is asymptotic rather than
constant-factor. By Heaps' law the vocabulary of a corpus of *n* tokens grows as
roughly `n^β` with β ≈ 0.5, so the vocabulary-inverted structure grows as the
square root of the corpus and stops growing almost entirely once the corpus is
large. The corpus-inverted structure grows linearly, forever.

Measured consequences, from `pg_tre/doc/perf.md` at 1 M rows of short text
(~85 chars, Zipfian sample of `/usr/share/dict/words`):

| | pg_tre (corpus trigrams) | pg_trgm (GIN) | pg_weave target |
|---|---:|---:|---|
| index size | 3843 MB | 159 MB | low hundreds of MB |
| build time | 80 s | 28 s | ≤ 60 s |
| trigram emissions at build | 83.5 M | — | ~ distinct-vocabulary-sized |
| `connection refused` exact, 10k matches | 18.5 s | 35 ms | single-digit ms |
| `databse` fuzzy k=1, 50k matches | 7.3 s | n/a | ≤ 200 ms |
| `E-[0-9]{4}` regex, 0 matches | 1.5 s | n/a | ≤ 100 ms |
| `connectoin refused` k=2 `<@>` top-10 | 6.4 s | n/a | ≤ 500 ms |

The right-hand column is a **target**, not a measurement. `bench/RESULTS_FUZZY.md`
is owed and the Phase Z gate is beating every pg_tre number on that exact corpus
while staying within 3× of pg_trgm's index size with `cgram` off.

Why the exact-match case (18.5 s vs pg_trgm's 35 ms vs a *seq scan's* 69 ms) is so
bad in pg_tre and should mostly vanish: with ~24k distinct trigrams whose posting
lists are physically scattered across thousands of pages, candidate extraction is
I/O-bound. In pg_weave the first stage touches only the vocabulary — small, dense,
and often already in shared buffers because the lexical channel just read it — and
the second stage reuses the lexical channel's own posting lists, which are already
FOR-packed with block-max skipping.

Also relevant: pg_tre's build wall is **temp disk**, not memory —
`pg_tre/LIMITATIONS.md` measures ~64 B per trigram *occurrence* and documents a
production user hitting ~21 GB of temp at 2.1 % of a body corpus, making the AM
unusable past ~500k rows of long text. Emitting one tuple per (trigram, *term*)
instead of per (trigram, *occurrence*) is what removes that wall, because the
vocabulary is bounded by Heaps' law while occurrences are not.

## 2. The funnel

```
pattern
  │
  ├─ anchored / prefix / range ──▶ SuRF trie over the bolt vocabulary
  ├─ fuzzy  term~k ──────────────▶ universal-Levenshtein neighbourhood expansion
  │                                over the vocabulary trigram map
  ├─ regex  /re/ ────────────────▶ regex AST ──▶ Navarro trigram tiling
  │                                          ──▶ vocabulary candidates
  └─ LIKE ───────────────────────▶ translate to one of the above
  │
  ▼  candidate TERMS  (term ordinals within this bolt)
  │
  ▼  candidate DOCS   (the lexical channel's own posting lists — already fast)
  │
  ▼  heap recheck with the exact matcher (vendored TRE)
```

| stage | structure | source file | origin |
|---|---|---|---|
| prefix / range / anchored | SuRF, LOUDS-Sparse trie | `src/query/surf.c` | pg_tre |
| fuzzy neighbourhood | universal Levenshtein (Mihov–Schulz) | `src/query/uleven.c` | pg_tre |
| fuzzy verification | bounded Levenshtein automaton | `src/query/lev.c` | pg_fts |
| regex parse | LALR grammar → AST | `src/query/regex_grammar.c`, `regex_ast.c` | pg_tre |
| regex → trigrams | Navarro-style tiling | `src/query/tiling.c` | pg_tre |
| LIKE → pattern | translation | `src/query/like_translate.c` | pg_tre |
| trigram → terms | vocabulary trigram sparsemap | `src/query/trgm.c`, `src/pages/trgm_page.c` | pg_fts |
| exact verification | TRE regex matcher | `src/query/re_match.c` + vendored TRE | pg_tre + BSD-2 |

Two implementations of Levenshtein coexist on purpose and it is not redundancy:
`uleven.c` *expands* a pattern into the trigram neighbourhood that could contain a
match within edit distance k (a generator), and `lev.c` *decides* whether a
specific candidate term is within k (an acceptor). Using the generator as an
acceptor would be slow; using the acceptor as a generator is impossible.

## 3. SuRF over the vocabulary — task Z3

New page kind `WEAVE_SURF` (bit 14; see the allocation table in
`doc/specs/SEGMENT_FORMAT.md`, and note the bit-exhaustion problem recorded there
that v5 must resolve first).

Built during bolt flush and merge, from the dictionary, which is already sorted —
so construction is a single ordered pass with no sort. Keys are dictionary terms;
values are term ordinals. A LOUDS-Sparse encoding is succinct: close to the
information-theoretic minimum for the trie shape, with rank/select over bitvectors
supplying navigation.

What it answers that the existing dictionary walk does not do as well: prefix
enumeration (`term*`) without scanning a dictionary page chain, and range
predicates, and the anchored case of a regex (`/^foo/`) directly.

Invariant for `weave_check()`: trie membership is **exactly** the bolt's
dictionary term set. Not a subset, not a superset. A one-sided error here silently
changes results, so the check must compare both directions.

## 4. The boolean-gate shuttle — task Z7

Fuzzy, regex, prefix, and LIKE are *predicates*, not scores. Under contract (C5)
of `include/weave/channel.h`:

```
block_max()  =  +INF   if any candidate term has a posting in this block
                -INF   otherwise
score()      =   0.0   for a match
                -INF   for a non-match
```

The fused scorer's arithmetic then handles them with no special case: `-INF`
propagates through the sum and the document is discarded.

A boolean channel is therefore the **cheapest** thing to add to the fused scorer:
there is no bound-tightness work to do at all, because `±∞` is exactly tight for a
predicate. Contrast the vector channel, where getting the bound right took a
measurement campaign (`bench/RESULTS_BOUND_PRUNING.md`) and changed the storage
layout.

## 5. `<@>` edit-distance KNN — task Z9

This one is **not** boolean and does need a real numeric bound. Ordering by
`text <@> pattern` returns rows by ascending edit distance, so
`score = -edit_distance` and a bound requires a *lower* bound on edit distance for
every term in the block.

Two cheap lower bounds, take the max:

1. **Length difference.** `ed(a, b) >= | |a| - |b| |`. The dictionary already
   stores term length, and a block's min and max term length bound it.
2. **Trigram deficit.** A single edit changes at most 3 trigrams of a string
   (fewer at the boundaries). So if `a` and `b` share `s` trigrams out of
   `T(a)` and `T(b)` respectively,
   `ed(a, b) >= ceil( (max(T(a), T(b)) - s) / 3 )`.
   Per block we store the maximum trigram overlap with... nothing, because the
   pattern is not known at build time. What *is* available cheaply is the maximum
   `T(term)` over the block, which with the query's `T(pattern)` and the best
   possible overlap gives a usable bound.

Hence:

```
ed_lower(block) = max( length_deficit(block),
                       ceil( (max_T_block - T_pattern) / 3 ),
                       ceil( (T_pattern - max_T_block) / 3 ) )
block_max()     = -ed_lower(block)
```

Both terms are monotone in quantities available from the block header, satisfying
(C3). Tightness is unmeasured and **must** be measured before this is called done
— the lesson from the vector channel is that a provably-correct bound can prune
0.0 % of blocks and that this is invisible without measuring the pruning rate
directly. `bench/bound_pruning.c` is the shape to copy.

## 6. The limitation, stated plainly

**Vocabulary trigrams are token-aligned. They cannot answer a pattern that crosses
a token boundary.**

`LIKE '%tion refu%'` spans the end of one token and the start of the next. No
amount of vocabulary indexing finds it, because the string `n refu` exists in no
single vocabulary entry. Character-stream trigrams over the *corpus* are required,
and for that specific job **pg_trgm's GIN is close to optimal** — it is a
well-tuned inverted index over exactly the right thing.

So pg_weave ships an **opt-in** corpus-level character-trigram channel:

```sql
CREATE INDEX ... USING weave (sku gram_ops);   -- corpus trigrams, page kind WEAVE_CGRAM
```

and makes two commitments about it:

1. With `cgram` **off**, pg_weave cannot answer unanchored cross-token substring
   search from the index and will not pretend to. The planner must fall back to a
   seq scan and the documentation must say so.
2. With `cgram` **on**, pg_weave does **not** claim to beat pg_trgm on index size.
   `bench/RESULTS_CGRAM.md` must record the honest size comparison.

Writing this down prominently is deliberate. The temptation, once the vocabulary
funnel is working beautifully for the 95 % of real patterns that are token-aligned,
is to quietly imply it handles the other 5 % too. That is how a project loses
credibility on the four claims it can actually support
(`doc/ARCHITECTURE.md` §9).

## 7. Build cost model

pg_tre: one emitted tuple per (trigram, occurrence) → ~64 B × 83.5 M = the
multi-GB temp-disk wall.

pg_weave: one emitted tuple per (trigram, **term**). The dictionary is built
anyway by the lexical channel, and its terms are already sorted, so the trigram
map is produced by a single pass over the dictionary with no additional sort and no
spill. Expected temp disk: **zero** beyond what the lexical build already uses.

With `cgram` on, the corpus-level model returns and so does the wall. Hence
opt-in, and hence `weave_estimate_index_build()` (adapted from pg_tre's
`tre_estimate_index_build`) must exist and must be mentioned in the docs before
anyone enables it on a multi-GB column.

## 8. Test plan

| gate | test | notes |
|---|---|---|
| Z1 | build with fuzzy sources in `OBJS` | needs vendored TRE, BSD-2; record commit in `doc/LICENSING.md` |
| Z2 | GUCs visible in `pg_settings` | wiring list is in `doc/specs/IMPORT_pg_tre.md` |
| Z3 | `weave_check()` trie/dictionary set equality, both directions | property test, not a fixed-output test |
| Z4 | prefix p50 ≤ today's; `EXPLAIN` shows the surf channel | |
| Z5 | k=1 and k=2 index-accelerated, p50 ≤ 200 ms at 1 M rows | vs pg_tre's 5.5 s / 7.3 s |
| Z6 | `E-[0-9]{4}` p50 ≤ 100 ms | vs pg_tre's 1.5 s |
| Z7 | (C1)+(C2) property test for the gate shuttle | `test/hegel/test_bounds.c` |
| Z8 | randomized differential test vs pg_trgm | thousands of generated patterns, row sets compared exactly |
| Z9 | randomized differential test vs seq-scan `levenshtein()`; bound pruning rate measured | both required; correctness alone is not the gate |

The differential tests (Z8, Z9) matter more than the latency gates. A fuzzy search
that is fast and subtly wrong is worse than pg_trgm, and the only way to know is to
generate patterns and compare against a reference implementation on the same data.
pg_tre's at-scale stress suite reported zero correctness mismatches over 250k–10M
rows; that is the bar.
