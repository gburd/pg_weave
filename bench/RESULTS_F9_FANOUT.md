# F9 fanout: how much a document-space `<@>` channel has to merge

Task F9 (`doc/PHASES.md`) owes this before code: "the fanout cost measured rather than
assumed (hard rule 9)". A document-space `<@>` shuttle publishes document positions,
so it must merge the postings of every dictionary term within its threshold. This
measures how many that is.

## Setup

- 2026-10-01, local workstation (`/scratch/pg_weave/lpg.sh`). Counts only, no latency.
  The counts are deterministic, so the host does not matter.
- Corpus: BEIR **scifact**, 5,183 documents, `to_wdoc('english', title || text)`.
  Vocabulary 42,805 stemmed terms, 488,974 postings.
- Patterns: 12 domain words, each misspelled by one edit (`protien`, `cancr`, ...),
  plus a few spelled correctly (`insulin`, `tumour`).
- Distance: the `<@>` operator itself, `to_wdoc('simple', term) <@> pattern`, per
  vocabulary term.
- Harness: `/scratch/pg_weave/f9fan.sql`. It reads the terms back out of `wdoc::text`,
  so it measures what the index stores.

## Numbers

Terms (`tN`) and postings (`postN`) within distance N:

| pattern | t0 | post0 | t1 | post1 | t2 | post2 | t3 | post3 |
|---|---|---|---|---|---|---|---|---|
| cancr | 0 | 0 | 24 | 893 | 94 | 1,169 | 1,434 | 14,016 |
| celss | 0 | 0 | 1 | 1 | 214 | 4,195 | 1,352 | 17,055 |
| diabetis | 0 | 0 | 6 | 8 | 16 | 243 | 20 | 279 |
| genom | 7 | 528 | 15 | 658 | 119 | 2,962 | 1,112 | 11,136 |
| infecton | 0 | 0 | 4 | 5 | 41 | 615 | 75 | 1,060 |
| insulin | 21 | 229 | 22 | 230 | 29 | 273 | 85 | 784 |
| macrophag | 7 | 187 | 15 | 199 | 16 | 201 | 18 | 229 |
| mutaton | 0 | 0 | 4 | 4 | 7 | 456 | 51 | 1,358 |
| neuronal | 3 | 4 | 3 | 4 | 20 | 460 | 51 | 735 |
| protien | 0 | 0 | 0 | 0 | 72 | 1,711 | 226 | 6,294 |
| tumour | 13 | 137 | 57 | 795 | 60 | 813 | 166 | 3,426 |
| vacine | 0 | 0 | 3 | 5 | 18 | 129 | 267 | 3,297 |

At the **k = 10 threshold**, the smallest distance at which at least 10 postings exist:

| pattern | d(10) | terms | postings | % of all postings |
|---|---|---|---|---|
| cancr | 1 | 24 | 893 | 0.18 % |
| celss | 2 | 214 | 4,195 | 0.86 % |
| diabetis | 2 | 16 | 243 | 0.05 % |
| genom | 0 | 7 | 528 | 0.11 % |
| infecton | 2 | 41 | 615 | 0.13 % |
| insulin | 0 | 21 | 229 | 0.05 % |
| macrophag | 0 | 7 | 187 | 0.04 % |
| mutaton | 2 | 7 | 456 | 0.09 % |
| neuronal | 2 | 20 | 460 | 0.09 % |
| protien | 2 | 72 | 1,711 | 0.35 % |
| tumour | 0 | 13 | 137 | 0.03 % |
| vacine | 2 | 18 | 129 | 0.03 % |

## What the numbers mean

1. **At the threshold a fused top-k needs, the merge is small.** The worst case is
   0.86 % of postings, and every case is under 4,200 documents. A document-space
   shuttle that materializes `(docid, min distance)` for the admitted terms and then
   walks that sorted array is O(admitted postings), and that is cheap here.
2. **The cost is not bounded by k, and grows by roughly 10x per extra edit.** From d = 2
   to d = 3, `cancr` grows 1,169 -> 14,016 and `celss` 4,195 -> 17,055. The threshold
   itself must come from the pass, the way `weave_edist_pass()` already widens d on
   demand, never from a constant.
3. **A fused `<@>` channel ranks by min distance, and a fused score must be a SUM.**
   `weave_edistscore(d) = -d`, so a channel that does not reach a document contributes
   0, and 0 is BETTER than every real score, -d with d >= 0. That is G71's trap
   exactly, in a third channel. It is not an implementation detail: under `fuse()` a
   document with no term within the channel's threshold must not outrank one that has
   such a term. The fix G71 used for the vector channel applies, a REQUIRED gate over
   "has a term within the threshold". But the threshold is a property of the PASS, so a
   gated `<@>` channel makes the fused answer depend on the pass width. See
   `doc/specs/FUSED_TOPK.md` sect. 7d for the design consequence.

## What this does NOT tell us

- **One corpus, English-stemmed, 42k vocabulary.** A code or log corpus, where terms are
  identifiers sharing prefixes, has a much denser neighbourhood at d <= 2;
  `bench/RESULTS_EDIST_BOUND.md`'s `t1293...` vocabulary is that case, at 67-100 % of
  terms scored. The fanout there is unmeasured.
- **No latency.** The merge is O(postings) by construction; whether that beats the
  fallback is a separate measurement, owed once a shuttle exists.
- **Patterns chosen by hand.** Twelve is enough to see the shape and too few for a
  distribution.
