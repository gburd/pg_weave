# Specification: the fuzzy, regex, and prefix channel

Status: **Z3 done — the SuRF trie is on disk, verified and loadable. The rest of the
sources are imported and not wired.** Tasks **Z1**–**Z9** in
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
| prefix / range / anchored | SuRF, LOUDS-Sparse trie | `src/query/surftrie.c` (over vocabulary terms; **wired**) | pg_weave (Z3) |
| prefix over trigram keys | SuRF over uint64 keys | `src/query/surf.c` | pg_tre |
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

Status: **on disk and verified.** The pure core (§3.1) plus the AM half — the page
chain, the descriptor registration, the `weave_check()` gate, and a reader — are
implemented. **No query is routed through it yet**; that is Z4 (prefix), Z5 (fuzzy)
and Z6 (regex).

Page kind `WEAVE_PK_SURF`, **id 21 in the extended kind space** — already reserved
by X1, see the authoritative table in `doc/specs/SEGMENT_FORMAT.md` §2. It is an
integer id in `WeavePageOpaqueData.kind` under the escape bit, **not a bit**:
read it with `WeavePageHasKind()`, never with `flags & ...`, which compiles and
is always false. (This section previously said "bit 14"; that predates the v6
kind space and was wrong by the time it was written.)

Built during bolt flush and merge, from the dictionary, which is already sorted —
so construction needs no sort. Keys are dictionary terms; values are term
ordinals. A LOUDS-Sparse encoding is succinct: close to the information-theoretic
minimum for the trie shape, with rank/select over bitvectors supplying
navigation.

What it answers that the existing dictionary walk does not do as well: prefix
enumeration (`term*`) without scanning a dictionary page chain, and range
predicates, and the anchored case of a regex (`/^foo/`) directly.

Invariant for `weave_check()`: trie membership is **exactly** the bolt's
dictionary term set. Not a subset, not a superset. A one-sided error here silently
changes results, so the check must compare both directions.

### 3.1 The Z3 deliverable: a pure core, plus the AM half

`include/weave/surftrie.h` + `src/query/surftrie.c` are the backend-independent
builder, reader and validator; `test/hegel/test_surf.c` is the property test and
`test/fuzz/fuzz_surftrie.c` the corruption harness.

The AM half, sequenced after L1 split the `src/am/am.c` unity build:

| piece | where | why there |
|---|---|---|
| vocabulary collection | inside `weave_write_dictionary_iter()` (`src/am/ambuild.c`) | the trie must be built from the **same term sequence the dictionary writer emitted**. There are two dictionary writers (an in-memory array at bolt flush, a spilled stream at merge); building at each call site would mean two places that must independently agree with the dictionary. Feeding a collector from inside the writer makes the agreement structural. |
| build + page chain | `weave_build_surf_weft()` (`src/am/ambuild.c`), called from `weave_write_segment()` and `weave_merge_segments_streaming()` right after the dictionary is written | the seam is the segment writer, and `src/am/ambuild.c` owns segment writers |
| the chain writer/reader | `weave_write_surf()`, `weave_read_surf()`, `weave_surf_load()` (`src/am/am.c`) | `src/am/am.c` owns page and segment machinery |
| the gate | `surf_trie_matches_dictionary` in `src/am/amcheck.c` | §3 above, and `doc/specs/SEGMENT_FORMAT.md` §9 |
| byte accounting | a `surf_trie` bucket in `weave_index_size_detail()`, plus `weave_surf_stats()` (`src/am/amsize.c`) | a structure whose bytes are unattributed is a structure that gets big unnoticed |

Two things the writer does that are not obvious from the format:

1. **On merge the trie is REBUILT, not merged.** A merged bolt's vocabulary is the
   union of its inputs' minus terms whose every posting was tombstoned, so its
   level-order slot numbering, its rank/select tables and every one of its term
   ordinals differ from both inputs. Two LOUDS-Sparse images cannot be
   concatenated or unioned in place. Rebuilding is nearly free because the merged
   term sequence is what the dictionary writer just emitted.
2. **A vocabulary the format cannot represent completely gets NO weft at all,**
   not a partial one. Absent is safe: a caller that finds no fuzzy weft falls back
   to the dictionary walk, which is slower and correct. Incomplete is a false
   negative. So a zero-length term (`WEAVE_SURF_EMPTY_TERM`) or a vocabulary above
   the format cap omits the weft with a `WARNING`, and the descriptor simply has no
   `WEAVE_WK_FUZZY` entry. An input that is not strictly ascending is different in
   kind — it means the dictionary on disk is not sorted either, which also breaks
   the sparse block index and the k-way merge — so that one is an `ERROR`, and it is
   the only free check the tree has of the "dictionary terms are strictly ascending
   within a bolt" invariant `SEGMENT_FORMAT.md` §9 still owes.

Note what is *not* here: `include/weave/surf.h` / `src/query/surf.c` are the
imported pg_tre SuRF over **uint64 trigram keys** (`pg_weave_surf_*`, palloc,
`ereport`). That is a different structure over a different key space and it stays.
Z3's trie is over **variable-length vocabulary terms**, which is the substitution
§1 rests on, and it is a separate file so it can be linked into a plain `gcc`
invocation (`include/weave/for.h` is the house exemplar and `doc/TESTING.md` says
why).

### 3.2 The error is one-sided, and that is a correctness contract

SuRF answers **false positives, never false negatives**. `weave_surftrie_may_contain()`
returning false means *definitely absent*; returning true means *possibly present*.
A false negative silently drops rows and, per `AGENTS.md` hard rule 1, no
fixed-expected-output regression test catches it. The direction is stated in the
header next to the function and asserted directionally by the property test:
every dictionary term **must** report present; a non-member **may** report present.

Where the one-sidedness actually bites is term length. The format represents the
first `WEAVE_SURFTRIE_MAX_DEPTH` (255) bytes of a term. A longer term is indexed
**truncated** to that depth and its slot is marked `trunc`, so every query key
sharing those 255 bytes reports present — a false positive requiring recheck. The
alternative designs are both worse:

- *Reject the build.* A vocabulary containing one 300-byte token (base64, a URL, a
  DNA string) then has no fuzzy channel at all.
- *Skip the long term.* That is precisely a false negative, wearing a build-time
  disguise.

For the same reason a **zero-length term is an error, not a skip**: the slot-based
terminal marking has no slot at depth 0 to hang it on, so the builder returns
`WEAVE_SURF_EMPTY_TERM` and makes the caller deal with it rather than dropping a
term the dictionary contains.

### 3.3 On-disk layout, v1

One contiguous little-endian image, `WEAVE_SURFTRIE_MAGIC` = `"WST1"`. The AM
lays it on a `WEAVE_PK_SURF` page chain; the image is position-independent and
length-checked, so page chaining is not part of this format.

> **Where §3.3 was wrong.** It said "with the existing blob writer". That cannot be
> done: `weave_write_blob()` hard-codes `WEAVE_PK_TRGM_DATA` — which is exactly why
> the livedocs blob lands on trigram-data pages and `WEAVE_PK_LIVEDOCS` has no
> writer at all (`SEGMENT_FORMAT.md` §2). Reusing it would have put the trie on
> pages whose kind says "trigram data", and `weave_check()` and
> `weave_index_size_detail()` would then have had no way to tell the two apart.
> `weave_read_blob()` is worse for this purpose: it validates no page kind, follows
> `nextblk` and trusts `pd_lower`. For a structure whose entire job is to be a
> filter that never produces a false negative, the reader must refuse bytes that
> are not demonstrably its own. `weave_write_surf()` / `weave_read_surf()` in
> `src/am/am.c` are the pair, and the reader checks
> `WeavePageHasKind(page, WEAVE_PK_SURF)` on every page.
>
> **Where §3.3 was underspecified: how the reader knows the image length.** "The
> image is length-checked" presumes a length, and the section never said where it
> comes from. It comes from the chain: the pages carry the image bytes and nothing
> else, so the length is the sum of their payloads, and the reader walks the chain
> to compute it *before* it allocates. That is not a detail — a length stored in the
> descriptor or in a per-chain header would be a second source of truth for a
> number the bytes already determine, exactly the objection §3.3 raises against an
> offset table, and it would also let a corrupt count drive a 1.3 GB allocation.
> Deriving it from the chain bounds the allocation by pages that exist.

Every multi-byte integer is written **byte-wise little-endian** and every read
goes through a shift-assembly helper. There is no struct overlay, no padding, and
no alignment requirement — a decision, not laziness: the image is parsed straight
out of a buffer page at an offset the writer chose, so a struct overlay would make
correctness depend on that offset, and `-fsanitize=undefined` would be right to
complain. It also makes the format identical on every architecture.

| offset | field | notes |
|---:|---|---|
| 0 | `magic` u32 | `0x57535431` |
| 4 | `version` u16 | 1; anything else is an ERROR (`doc/CONVENTIONS.md` decision 3) |
| 6 | `flags` u16 | must be 0 |
| 8 | `nterms` u32 | vocabulary size; 0 is legal (header-only 32-byte image) |
| 12 | `nslots` u32 | trie slots = distinct term prefixes, ≤ 2^28−1 |
| 16 | `nnodes` u32 | trie nodes |
| 20 | `nterminal` u32 | slots that end a term (or a truncated run) |
| 24 | `ntrunc` u32 | terminal slots at max depth that collapsed longer terms |
| 28 | `maxdepth` u16 | deepest slot, 1..255 |
| 30 | `reserved` u16 | must be 0 |

then, back to back, with `BW = 8·⌈nslots/64⌉`, `NSB = ⌈nslots/512⌉`,
`NSEL = ⌈nnodes/64⌉`:

| section | bytes | meaning |
|---|---:|---|
| `labels` | `nslots` | one byte per slot, level-order (BFS) |
| `haschild` | `BW` | bit per slot: this slot has a child node |
| `louds` | `BW` | bit per slot: this slot is the first of its node |
| `terminal` | `BW` | bit per slot: the path ending here is a member |
| `trunc` | `BW` | bit per slot: terminal by truncation, so a *maybe* |
| `rank_haschild` | `4·NSB` | ones in `haschild` before each 512-bit superblock |
| `rank_terminal` | `4·NSB` | same for `terminal`; maps a terminal slot to its ordinal index |
| `select_louds` | `4·NSEL` | slot index of every 64th set `louds` bit |
| `ords` | `4·nterminal` | first vocabulary ordinal each terminal slot covers |

Total size is a pure function of the counts, and the reader requires
`len == that total` exactly. No slack, no offset table: an offset table is a
second source of truth for where a section starts, and the fuzz corpus would
then contain images that are internally consistent but disagree with the counts.

Four design points worth the words:

1. **A separate `terminal` bitmap instead of SuRF's terminator label.** Upstream
   SuRF marks "a key ends here and also continues" by inserting a reserved
   terminator byte (`0xFF`) as a label. pg_weave cannot: on a non-UTF-8 server a
   term may legitimately contain `0xFF`, and the collision is a wrong answer in
   the false-negative direction. One bit per slot buys the hazard away.
2. **`ords` is per terminal slot, indexed by `rank_terminal`.** Term ordinals are
   what Z4 needs to reach posting lists without a dictionary lookup. In level
   order the terminal ranks are *not* lexicographic, so the array is indexed by
   rank rather than assumed to be sorted — and the deep validator asserts that a
   lexicographic DFS *does* see the ordinals strictly ascending starting at 0,
   which is the machine-checkable statement of "this trie is the sorted dictionary".
3. **Rank and select accelerators are validated against a recomputation, not
   trusted.** A corrupt superblock counter does not crash; it navigates to the
   wrong node and returns a wrong answer, i.e. a false negative. `open()`
   recomputes both tables in one pass and rejects a mismatch.
4. **Acyclicity is a validated field, not an assumption.** For every `haschild`
   slot the child node must start *after* the slot. That single inequality is what
   makes every walk terminate on hostile bytes; without it a corrupt image can
   make enumeration spin forever, which no timeout in a validator would explain.

### 3.4 Validation, in two layers

`weave_surftrie_open()` is the memory-safety layer: header, exact size, tail bits
zero, `popcount(louds) == nnodes`, `popcount(haschild) == nnodes − 1` (every
non-root node is the child of exactly one slot), `popcount(terminal) == nterminal`,
`trunc ⊆ terminal`, labels strictly ascending within each node, rank tables and
select samples equal to a recomputation, and the acyclicity inequality above.
**After `open()` returns `WEAVE_SURF_OK`, no query can read outside the image or
fail to terminate.** That is the property the fuzz target asserts.

`weave_surftrie_validate()` is the semantic layer `weave_check()` calls: a full
lexicographic DFS proving every slot and node is reachable from the root, the
terminal count is exactly what the header claims, the observed maximum depth
equals `maxdepth`, and the ordinals are `0 = ord₀ < ord₁ < … < nterms`. Together
with a dictionary walk that is set equality in both directions, which is the Z3
gate.

**Neither layer is sufficient, and that is measured rather than asserted.**
`t/013_surf_corruption.pl` bumps the header's `nterms` by one: no section length
depends on `nterms` (only `nslots`, `nnodes` and `nterminal` do), so the image is
still exactly as long as its own counts imply, every popcount identity holds,
every accelerator table still equals a recomputation, and the ordinal-range test
`ord < nterms` only got looser — **both layers accept it.** The same test then sets
the label byte of the *last* slot to `0xFF`, which preserves "labels strictly
ascending within a node" (`0xFF` is the largest byte, and that slot ends its node)
and touches no bitmap: structurally perfect, semantically a different vocabulary.
Only the comparison against the dictionary sees either one. That is why the gate is
a cross-structure invariant and not "run the validator".

The byte cost, measured on a 200,000-row build of short text: **~5.5 bytes per
vocabulary term**, flat across a 3.7× change in vocabulary size (274,244 terms →
1,514,433 B; 74,244 terms → 410,061 B), which is 17.1 % of the dictionary it
indexes and 5–7 % of the whole index at that corpus size. The table and the caveat
about how that share moves as the vocabulary saturates are in
`doc/specs/SEGMENT_FORMAT.md` §6.

One of those checks is provably redundant, and it is recorded rather than removed.
Mutation testing showed that deleting the reachability comparison changes no test
outcome, because `popcount(haschild) = nnodes − 1` makes the rank-derived child
index a bijection onto nodes `1..nnodes−1`, so every node has exactly one parent
slot; acyclicity then forces node starts to increase along any parent chain, so
induction gives reachability from node 0. It stays because §9 asks for
reachability by name, it costs two comparisons on a pass that is already walking
the trie, and a future change to any of the three premises would otherwise take
the property with it silently. The general point: an uncaught mutation is either a
missing test or a redundant check, and which one it is has to be established.

### 3.5 Allocation, because this is vocabulary-scale

`make check-alloc` exists because four real pg_fts crashes were one allocation
sized from a corpus- or vocabulary-scale quantity without the huge-safe variant, and
a trie over the vocabulary is exactly that. So the core allocates **nothing at
all**: `weave_surftrie_size()` returns the exact image length and
`weave_surftrie_build()` writes into a caller-supplied buffer, refusing with
`WEAVE_SURF_NOSPACE` rather than growing it. The AM's future writer allocates that
one buffer through `WEAVE_ALLOC_MAYBE_HUGE`.

The builder also uses no scratch memory. Construction is one pass per depth over
the sorted term array — at depth *d* the nodes are exactly the maximal runs of
terms sharing a *d*-byte prefix, and sortedness makes those runs contiguous — so
there is no BFS queue to size. The cost is O(nterms · maxdepth) byte-comparisons,
paid twice (measure, then emit), and the two passes are literally the same
function driven by two sinks, so a measure/emit disagreement — which would be a
buffer overrun — is not expressible.

### 3.6 The neighbourhood automaton that walks it — task Z5

The trie is a structure; this is the thing that walks it. `include/weave/uleven.h`
takes a query term, an edit budget *k*, and *any* iterator over the vocabulary in
ascending unsigned-byte order, and emits exactly the vocabulary terms within
distance *k*. The iterator abstraction (`WeaveUlevVocab`, a `next` plus an
optional `skip`) is why the same code is driven by a plain sorted array in
`test/hegel/test_uleven.c` today and by the on-disk `WEAVE_PK_SURF` chain later,
with nothing in the core needing PostgreSQL at all.

Four properties, each of which is a decision and not an accident:

1. **Exact, not one-sided.** Unlike §3.2's trie, which answers false positives
   and never false negatives, this emits term *t* if and only if
   `dist(query, t) ≤ k`. Exactness is what lets `term~k` skip a heap recheck.
   The composition is only as exact as its iterator: driven by a *truncating*
   SuRF enumeration it inherits SuRF's false positives — the safe direction —
   and whoever wires the trie owes that recheck.

2. **The dead-prefix skip is an optimization and must stay one.**
   `weave_uleven_match()` reports `WEAVE_ULEVEN_DEAD` with the byte length of a
   prefix no within-*k* string can extend, and the expander hands that to
   `skip`. With `skip == NULL` the answer must be *identical*, only slower. A
   dead prefix one byte too **short** covers more terms and silently drops rows,
   which is hard rule 1's failure mode exactly: the property test asserts the
   two runs emit identical sets, and that assertion is what catches it.

3. **The edit unit is the character, not the byte.** `levenshtein()` from
   `contrib/fuzzystrmatch` counts characters, and Z9's gate is a randomized
   differential test against it, so a byte-unit automaton would fail that gate at
   *k*=1 on any two-byte substitution — a false negative visible only to
   non-ASCII users and invisible to every fixed-output test.
   `WEAVE_ULEVEN_BYTE` is not a fallback but the correct mode on a single-byte
   server encoding, where a byte *is* a character.

4. **Levenshtein, not Damerau.** An adjacent transposition costs 2, so `~1` does
   not match `hte` against `the`. This is for agreement with `levenshtein()`, per
   the same Z9 gate. Damerau would be a new unit mode, not a change to this one:
   it changes which rows a query returns.

Malformed bytes decode one at a time to the pseudo-unit `0xDC00 + byte`, a range
valid UTF-8 cannot produce because encoded surrogates are rejected, so the decode
stays **injective** over all byte strings. Folding bad bytes to one replacement
character would instead make two distinct terms compare at distance 0 — a wrong
answer that surfaces as a *missing* row once a caller de-duplicates.

That last property is where randomized testing was measurably not enough. With
the injectivity offset removed, a pool of ~1.9 M random malformed-byte pairs
produced **zero** failures; the collision it creates needs the specific pair
(a truncated `0xC2` lead against the valid two-byte encoding of U+00C2), which is
now a pinned directed case in the test. **A property test's random generator
covers the space it is shaped like, and a needle-shaped bug survives it.** Six
mutations were injected into the implementation and each was confirmed to fail
the test; two of them were caught by exactly one property, which is why
"skip == no-skip" and "expand == brute force" are separate assertions rather than
one.

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
| Z3 | `weave_check()` trie/dictionary set equality, both directions | **done**: `surf_trie_matches_dictionary` (`src/am/amcheck.c`), plus `sql/surf.sql`, `t/012_surf_crash_recovery.pl` and `t/013_surf_corruption.pl` |
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
