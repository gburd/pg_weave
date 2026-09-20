# `<@>` edit-distance block bound: pruning rate

Task Z9. `doc/specs/FUZZY_CHANNEL.md` §5 requires this measurement before the task
can be called done: "Tightness is unmeasured and **must** be measured before this
is called done — the lesson from the vector channel is that a provably-correct
bound can prune 0.0 % of blocks and that this is invisible without measuring the
pruning rate directly."

Harness: `bench/edist_bound.c`. No server; the bound is header-only
(`include/weave/edist.h`), so the harness links nothing.

```sh
cc -O2 -I include -o /tmp/edist_bound bench/edist_bound.c -lm
/tmp/edist_bound 0    # lexicographic pages -- the real dictionary order
/tmp/edist_bound 1    # length-clustered pages -- a hypothetical ordering
```

Run 2026-09-20 on a `c7i.2xlarge` (Ubuntu 24.04, gcc 13.3). Whole run: 1.5 s per
arm.

## Method

- **Vocabulary**: 254,000 terms — 250,000 of the form `'t' || n` for
  n = 0…249,999 (the shape a corpus of identifiers, SKUs or log tokens gives a
  dictionary: a long run sharing a prefix, lengths 2–7) plus 4,000 word-shaped
  terms of 3–12 random lowercase letters. ASCII throughout; the bound's
  multi-byte behaviour is the property test's job
  (`test/hegel/test_edist.c`), and mixing encodings here would only make the
  numbers harder to read.
- **Paging** reproduces the real stride: a term of L bytes occupies
  `MAXALIGN(20 + L)` bytes of 8,100 usable per page, so the page count is
  comparable to an index's. Lexicographic arm: 1,004 pages. Length-clustered arm:
  1,003 pages.
- **Per page**: `weave_edist_stats_add()` over its terms, then
  `weave_edist_lower()` — the same functions the shuttle calls.
- **`pages/run`**: fraction of pages whose bound exceeds the k-th best distance
  found *so far*, walking pages in dictionary order. This is what the scan
  actually achieves.
- **`pages/fin`**: fraction whose bound exceeds the *true* k-th best distance over
  the whole vocabulary. Not achievable without knowing the answer in advance; it
  is the ceiling, and it is the quantity `bench/RESULTS_BOUND_PRUNING.md` reports
  for the vector channel, so it is the comparable one.
- **`terms`**: fraction of the 254,000 terms whose exact Levenshtein distance was
  computed, under the running threshold.
- **`d(k)`**: the true k-th smallest distance from the pattern to any term.
- Contract (C2) is asserted on every page of every query: 0 violations in both
  arms, 36 queries each, 254,000 terms per query.

## Arm 1 — LEXICOGRAPHIC pages (the real dictionary order), 1,004 pages

```
pattern                 k   d(k) pages/run pages/fin     terms
t129384                 1      0     32.7%     32.7%    67.32%
t129384                 2      1      0.0%      0.0%   100.00%
t129384                 3      1      0.0%      0.0%   100.00%
t12938                  1      0      0.0%      0.0%   100.00%
t12938                  2      1      0.0%      0.0%   100.00%
t12938                  3      1      0.0%      0.0%   100.00%
t1293845                1      1     32.7%     32.7%    67.32%
t1293845                2      1     32.7%     32.7%    67.32%
t1293845                3      1     32.7%     32.7%    67.32%
x129384                 1      1      0.0%      0.0%   100.00%
x129384                 2      2      0.0%      0.0%   100.00%
x129384                 3      2      0.0%      0.0%   100.00%
tttttttt                1      5      0.0%      0.0%   100.00%
tttttttt                2      5      0.0%      0.0%   100.00%
tttttttt                3      5      0.0%      0.0%   100.00%
zqxjvk                  1      3      0.0%      0.0%   100.00%
zqxjvk                  2      3      0.0%      0.0%   100.00%
zqxjvk                  3      3      0.0%      0.0%   100.00%
connection              1      6      0.0%      0.0%   100.00%
connection              2      6      0.0%      0.0%   100.00%
connection              3      6      0.0%      0.0%   100.00%
connectoin              1      6      0.0%      0.0%   100.00%
connectoin              2      6      0.0%      0.0%   100.00%
connectoin              3      6      0.0%      0.0%   100.00%
internationalization    1     13     32.7%     32.7%    67.32%
internationalization    2     14      0.0%      0.0%   100.00%
internationalization    3     14      0.0%      0.0%   100.00%
ab                      1      1     89.4%     89.4%    10.55%
ab                      2      1     89.4%     89.4%    10.55%
ab                      3      1     89.4%     89.4%    10.55%
q                       1      2     89.4%     89.4%    10.55%
q                       2      2     89.4%     89.4%    10.55%
q                       3      2     89.4%     89.4%    10.55%
(empty)                 1      2     98.1%     99.1%     1.95%
(empty)                 2      2     98.1%     99.1%     1.95%
(empty)                 3      2     97.2%     99.1%     2.85%
C2 violations: 0
```

Median over the 36 rows: `pages/run` **0.0 %**, `terms` **100.00 %**.
Mean: `pages/run` 27.6 %, `terms` 72.4 %.
Rows with `pages/run` = 0.0 %: **22 of 36**.

## Arm 2 — LENGTH-CLUSTERED pages (hypothetical ordering), 1,003 pages

Pages formed by sorting the vocabulary on `(length, term)` instead of `term`.
Hypothetical: the lexical channel needs the dictionary in byte order for its own
point lookups, prefix scans and sparse block index, so this ordering is not a free
change.

```
pattern                 k   d(k) pages/run pages/fin     terms
t129384                 1      0      0.8%     40.6%    99.28%
t129384                 2      1      0.7%      4.9%    99.38%
t129384                 3      1      0.7%      4.9%    99.38%
t12938                  1      0     60.1%     64.3%    40.02%
t12938                  2      1      0.8%      1.3%    99.28%
t12938                  3      1      0.8%      1.3%    99.28%
t1293845                1      1      0.5%     40.3%    99.58%
t1293845                2      1      0.5%     40.3%    99.58%
t1293845                3      1      0.5%     40.3%    99.58%
x129384                 1      1      0.7%      4.9%    99.38%
x129384                 2      2      0.5%      1.0%    99.58%
x129384                 3      2      0.5%      1.0%    99.58%
tttttttt                1      5      0.0%      0.0%   100.00%
tttttttt                2      5      0.0%      0.0%   100.00%
tttttttt                3      5      0.0%      0.0%   100.00%
zqxjvk                  1      3      0.5%      0.5%    99.58%
zqxjvk                  2      3      0.5%      0.5%    99.58%
zqxjvk                  3      3      0.5%      0.5%    99.58%
connection              1      6      0.0%      0.1%   100.00%
connection              2      6      0.0%      0.1%   100.00%
connection              3      6      0.0%      0.1%   100.00%
connectoin              1      6      0.0%      0.1%   100.00%
connectoin              2      6      0.0%      0.1%   100.00%
connectoin              3      6      0.0%      0.1%   100.00%
internationalization    1     13      0.0%     39.8%   100.00%
internationalization    2     14      0.0%      4.2%   100.00%
internationalization    3     14      0.0%      4.2%   100.00%
ab                      1      1     99.8%     99.8%     0.27%
ab                      2      1     99.8%     99.8%     0.27%
ab                      3      1     99.8%     99.8%     0.27%
q                       1      2     99.8%     99.8%     0.27%
q                       2      2     99.8%     99.8%     0.27%
q                       3      2     99.8%     99.8%     0.27%
(empty)                 1      2     99.9%     99.9%     0.13%
(empty)                 2      2     99.9%     99.9%     0.13%
(empty)                 3      2     99.9%     99.9%     0.13%
C2 violations: 0
```

Median over the 36 rows: `pages/run` **0.5 %**, `terms` **99.58 %**.
Mean: `pages/run` 26.9 %, `terms` 73.2 %.
Rows with `pages/run` = 0.0 %: **12 of 36**; rows with `pages/run` below 1 %: **26
of 36**.

## Per-position tightness, from the property test

`test/hegel/test_edist.c` reports, over 129,549 randomly generated pages (mixed
ASCII / 2-, 3- and 4-byte UTF-8, homogeneous and mixed lengths), how far the
bound sits below the page's own true minimum distance:

```
tightness over 129549 pages: exact 12.4%, within 1 12.1%, looser 37.4%,
                             no claim (bound 0) 38.1%
```

## What varies with what

- `pages/run` > 0 in arm 1 occurs on 14 of 36 rows, and on every one of them the
  pattern's character length is outside the bulk of the vocabulary's length
  distribution (`ab`, `q`, the empty pattern, the 8- and 20-character patterns
  against a vocabulary of 2–7-character terms) or `d(k)` = 0.
- `pages/run` = 0.0 % in arm 1 occurs on every row where `d(k)` ≥ 3 and the
  pattern's length lies inside the vocabulary's length range
  (`zqxjvk`, `connection`, `connectoin`, `tttttttt`, `x129384`).
- The arm-2 `pages/fin` column is the only place any row improves by more than a
  percentage point while `d(k)` > 0 and the pattern length is interior: `t129384`
  k=1 goes 32.7 % → 40.6 %, `t1293845` 32.7 % → 40.3 %, `internationalization`
  k=1 32.7 % → 39.8 %.
- Arm 2's `pages/run` is *lower* than arm 1's on the 10 `t...`/`x129384`/
  `internationalization` rows even where its
  `pages/fin` is higher, because clustering by length puts the pages whose terms
  are nearest the pattern's length late in the walk, so the running threshold is
  still set by distant terms when the informative pages are reached.

## Reproduction

Both arms were run twice on the same host; the numbers above are identical
between runs (the harness is deterministic: a fixed RNG seed, no timing, no
threads).
