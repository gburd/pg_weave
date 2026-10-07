# RESULTS_TSVECTOR_CAPS: what BM25 loses when tf and length come from a tsvector

Task: `doc/PHASES.md` **M7**, step 1 ("measure first", maintainer decision 2026-10-06,
tsvector option 1). Harness: `bench/tsvcaps_job.sh` (the job), `bench/tsvcaps.py`
(Wikipedia fetch and strip, qrels-free rank agreement, paired bootstrap),
`bench/ndcg.py` (the existing scorer). Every number below comes from EC2 Debian 13,
PostgreSQL 17.11, pg_weave 0.28.0 (`c7i.4xlarge`), run through `bench/aws/run.sh ...
script`. Smoke (regression, isolation, all 33 TAP files) was green before each run.

| run | commit | contents |
|---|---|---|
| `pgweave-20261007-021914-c54f` | `f566bc0` | BEIR x3, enwiki part 1 whole (20,913 articles) and as 400-word chunks, enwiki part 6 (first 20,000 articles) |
| `pgweave-20261007-031455-7a75` | `4e8a368` | the same plus **known-item title queries with qrels** and a paired bootstrap; part 6 at **60,000** articles (3x); enwiki part 3 (30,000 articles) whole and as 256-word chunks |
| `pgweave-20261007-041651-d0de` | `1bd452d` | probes only (`BEIR= WIKIS=`), for probe P8 (doclen through wdoc I/O); P1-P7 reproduce runs 1 and 2 |

## Headline

1. **Chunked corpora lose nothing. The conjecture holds.** On five chunked corpora (scifact,
   nfcorpus, fiqa, Wikipedia part 1 in 400-word windows, Wikipedia part 3 in 256-word
   windows), the caps bite on at most 1 document in 57,600 (fiqa), 1 in 195,069 (400-word
   chunks) and 0 in 257,686 (256-word chunks). Under the `simple` config the
   tsvector-derived ranking is the exact ranking: top-10 overlap 1.0000 on scifact,
   nfcorpus and 256-word chunks, 0.9994 on fiqa and 0.9992 on 400-word chunks, and the
   paired nDCG@10 delta is 0.0000 on all five.
2. **Whole long documents lose, and how often depends on the corpus.** Of whole Wikipedia
   articles (`simple`), **33.5 %** of enwiki part 1 (old, long, heavily edited articles),
   **16.9 %** of part 3 and **5.5 %** of part 6 have a lexeme that hits the 255-position tf
   cap. **0.82 % / 0.19 % / 0.05 %** pass position 16,383, and for those documents a
   tsvector-derived length averages **0.76-0.82x** of the true length. Top-10 overlap with
   exact BM25 falls to **0.976 / 0.985 / 0.992**, and only **36 % / 56 % / 71 %** of
   queries keep an identical top-10. On known-item title queries the nDCG@10 change is
   **-0.0048** on part 1 (95 % CI -0.014 .. +0.001, 4 of 199 queries changed), and
   -0.0002 and +0.0013 on parts 6 and 3. Every CI spans zero. **The loss shows in the
   ranking but not in a quality metric**, at the 200-query resolution this measurement
   has. That is the hard-rule-8 headline: tsvector input is inexact on whole long
   documents, and nobody can tell that from nDCG alone.
3. **Under a stopword config the biggest effect is not a cap.** It is the definition of
   document length. `to_wdoc(regconfig, text)` counts every token, stopwords included.
   A tsvector cannot: it stores no length, and `to_wdoc(tsvector)` uses the sum of
   position counts, which leaves stopwords out (`english`: stopwords are 32 % of tokens
   on scifact and 48 % on fiqa). That changes **4-12 % of top-10 results** even on short
   passages and chunks (overlap 0.88-0.96). It is **quality-neutral** on all five chunked
   corpora (paired nDCG@10 delta between -0.0014 and +0.0048, every CI spans 0), but it
   is a different ranking. Defining length as **the last position** instead (arm
   `tsvmaxpos`) removes most of the difference: overlap 0.977-0.9997 on chunks, 0.994-0.998
   on whole articles.
4. **Side finding, a real bug outside M7 (`doc/GAPS.md` G89, data-loss class): a `wdoc`
   with more than 16,383 tokens cannot be read back.** See "wdoc round trip" below. Every such article fails `d::text::wdoc`: **260 of 260**
   across three Wikipedia parts under `simple` (259 under `english`). Text `COPY` and
   binary `COPY` fail the same way, which means `pg_dump`/restore of a stored `wdoc`
   column breaks on them.
5. **Second side finding (`doc/GAPS.md` G90): `wdoc` text and binary I/O silently replace
   the stored length with the sum of tf.** `to_wdoc('english', 'the cat sat on the mat')`
   has length 6 and reads back as 3, through both `d::text::wdoc` and binary `COPY`, with
   no error. A dump/restore therefore turns an `exact` column into this measurement's
   `sumtf` arm.

**Is option 1 sound?** Yes for chunked or short documents, the RAG case. **Not exact**
for documents over ~16k tokens, and **not exact for tf** once any lexeme in a document
repeats more than 255 times, which happens in Wikipedia-length articles (>2,000
tokens) and not in chunks. A stripped tsvector is unsound for BM25 (positive control
below: -0.03 to -0.07 nDCG@10 on BEIR, -0.24 to -0.52 on title queries). The operator class
should do three things: (a) **refuse** a stripped tsvector, or a mixed one with some
positionless entries, because it cannot carry tf; (b) **count and expose** documents
where a lexeme reached 255 positions or a position reached 16,383, since only those are
inexact; (c) **take doclen from the last position**, not from the sum of tf, so the
tsvector column ranks like `to_wdoc(regconfig, text)` under a stopword config. See
"What the operator class should do" below.

## What each representation derives (read from the C)

| representation | tf | doclen | positions |
|---|---|---|---|
| `to_wdoc(regconfig, text)` (`to_wdoc_byid` -> `wdoc_from_parsed`, `src/query/tsanalyze.c`) | run count over parsed words: **exact, uncapped** | `prs->pos`: **every token, stopwords included**, uncapped | `LIMITPOS`'d: every token past 16,383 is stored **at 16383** (the round-trip bug) |
| `to_wdoc(tsvector)` (`to_wdoc_from_tsvector`, `src/query/tsanalyze.c:242`) | `POSDATALEN` = number of stored positions (**capped at 255**); 1 if the entry has none | `weave_doc_build` sums tf (`src/query/doc.c:161`): **stopwords excluded, capped tf** | kept iff every entry has them |
| the tsvector itself (PG17 `to_tsany.c` `uniqueWORD`) | at most `MAXNUMPOS - 1` = **255** positions per lexeme | none stored | `LIMITPOS` to 16,383; once a lexeme's last position is 16,383 no more are added, so **every occurrence past 16,383 collapses to one** |

The cap is **255**, not 256: `uniqueWORD` adds a position only while
`apos[0] < MAXNUMPOS - 1`. Probe P1 measures it on this server. One lexeme repeated
1,000 times gives a tsvector with 255 positions and `to_wdoc(tsv)` length 255, against
an exact length of 1,000.

## The arms

One table per (corpus, config), one weave index per arm, all queried as
`SELECT id FROM t WHERE col @@@ q ORDER BY col <=> q LIMIT 100`, so the representation
in `col` is the only variable. The plan of one query per arm is asserted to be
`Index Scan using ix_<arm>` with `Order By` (`plans.txt`). Every arm's index-scan
counter rose by exactly the number of queries (`scancheck.tsv`: 0 failures), so every
ranked answer came from the weave index. None came from a seq scan.

| arm | column | what it isolates |
|---|---|---|
| `exact` | `to_wdoc(cfg, body)` | the reference |
| `exact2` | the same index after `REINDEX`, re-queried | determinism control: must agree 1.0000 |
| `sumtf` | `exact \|\| to_wdoc(''::tsvector)` | exact tf, doclen redefined as the sum of tf (what `to_wdoc(tsvector)` uses, without its caps) |
| `tsv` | `to_wdoc(to_tsvector(cfg, body))` | **what option 1 converts today** |
| `tsvmaxpos` | `tsv` plus a padding term so doclen = max(last position, sum of tf) | what an operator class could derive instead |
| `strip` | `to_wdoc(strip(to_tsvector(cfg, body)))` | **positive control**: tf = 1. Must lose, or the harness cannot see a loss |

`tsv` against `sumtf` (label `tsv_vs_sumtf`) shows **the caps alone**, because the two
arms share the length definition. `tsv` against `exact` shows the caps plus the length
definition. Configs: `simple` (no stopwords, so the length definitions coincide below
the caps) and `english`.

Queries. BEIR: the dataset's test queries, tokenized like `bench/fuse.sh` (lower, split
on non-alphanumerics, drop length <= 2 and the operator words, first 20 terms,
OR-joined, normalized by the config). Wikipedia: (i) **known-item title queries**: 200
article titles of 2-6 words in md5 order, built like a BEIR query, judged relevant to
that article (whole corpus) or to every chunk of it (chunked corpus). This gives
Wikipedia an nDCG. (ii) **df-band queries** with no qrels, scored by overlap only: from
`ts_stat`, lexemes `^[a-z][a-z0-9]{2,19}$` banded by document frequency (rare = 2-10
docs, mid = 0.1-1 %, common = 1-10 %, vcommon >= 10 %), 25 queries of 1, 2 and 3
OR-ed terms per band in md5 order, plus `captf`: 50 single-term queries on lexemes that
reach 255 positions in some document, where a tf-cap effect must show if it shows
anywhere.

## Measure 1: how often a cap bites

Run 2 (part 6 at 60k articles; run 1's 20k sample gave 5.58 % / 0.060 %):

| corpus | docs | tokens/doc avg / p99 / max | any lexeme at 255 positions (`simple`) | any position at 16,383 | `to_tsvector` errors (1 MB) | largest tsvector |
|---|---:|---|---:|---:|---:|---:|
| scifact | 5,183 | 228 / 490 / 1,603 | 0 | 0 | 0 | 10 kB |
| nfcorpus | 3,633 | 245 / 463 / 1,544 | 0 | 0 | 0 | 12 kB |
| fiqa | 57,600 | 137 / 662 / 3,091 | **1** (0.002 %) | 0 | 0 | 15 kB |
| enwiki p1, 400-word chunks | 195,069 | 387 / 454 / 1,315 | **1** (0.0005 %) | 0 | 0 | 6.6 kB |
| enwiki p3, 256-word chunks | 257,686 | 246 / 295 / 790 | 0 | 0 | 0 | 5.4 kB |
| enwiki p1, whole | 20,913 | 3,606 / 16,023 / 34,276 | **6,998 (33.5 %)** | **172 (0.82 %)** | 0 | 132 kB |
| enwiki p3, whole (first 30,000) | 30,000 | 2,116 / 11,339 / 48,143 | **5,067 (16.9 %)** | **56 (0.19 %)** | 0 | 139 kB |
| enwiki p6, whole (first 60,000) | 60,000 | 1,116 / 7,346 / 43,291 | **3,306 (5.5 %)** | **32 (0.053 %)** | 0 | 207 kB |

Under `english` the tf cap bites less (7.1 % / 1.5 % / 0.37 % on p1 / p3 / p6), because
the lexemes that repeat 255 times are mostly stopwords. The position cap is the same, because positions
count stopwords.

**The 1 MB limit was never reached.** The largest tsvector on any corpus was 207 kB,
from a 291 kB article (p6). Probe P3 shows where the limit is: 40,000 distinct 32-character
tokens (1.45 MB of lexeme and position data) fail with `string is too long for tsvector
(1450948 bytes, max 1048575 bytes)`. Positions per lexeme are capped, and past 16,383
they collapse, so a real document reaches the limit mainly through distinct-lexeme bytes:
it needs a vocabulary of about 1 MB in one document. No article came within 5x of it, and
any document that large has long since crossed the position cap.

**Past 16,383 tokens the length is wrong by a lot.** Averaged over the documents that
cross it, `to_wdoc(tsv)` reports 0.82x / 0.78x / 0.76x (p1 / p3 / p6) of the true length under
`simple`, and 0.58x / 0.56x / 0.57x under `english`. With `tsvmaxpos` it is 0.89x / 0.84x
/ 0.82x: the last position is 16,383 no matter how long the document is, so beyond that point no
derivation can recover the length. Probe P2 (30,000 tokens, 1,000 words x 30): the
tsvector keeps 17,382 positions. That is 16,383 real ones plus one collapsed
occurrence per lexeme, and every lexeme's tf comes out as 17 or 18 instead of 30.

## Measure 2: ranking quality

### nDCG@10, BEIR (qrels), run 2. Paired delta against `exact` with a 95 % bootstrap CI over queries

| corpus / cfg | exact | `tsv` | Δ `tsv` (CI) | `tsvmaxpos` Δ (CI) | `strip` Δ (CI) |
|---|---:|---:|---|---|---|
| scifact / simple | 0.6702 | 0.6702 | 0 (0 queries changed) | 0 | **-0.028** (-0.048 .. -0.008) |
| scifact / english | 0.6959 | 0.6965 | +0.0005 (-0.0066 .. +0.0076) | +0.0010 (0 .. +0.003) | **-0.041** (-0.062 .. -0.021) |
| nfcorpus / simple | 0.3034 | 0.3034 | 0 (0 changed) | 0 | **-0.047** (-0.060 .. -0.035) |
| nfcorpus / english | 0.3231 | 0.3217 | -0.0014 (-0.0040 .. +0.0012) | -0.0001 (-0.0002 .. 0) | **-0.051** (-0.065 .. -0.039) |
| fiqa / simple | 0.2311 | 0.2311 | -0.0000 (1 changed) | 0 | **-0.057** (-0.071 .. -0.043) |
| fiqa / english | 0.2470 | 0.2474 | +0.0004 (-0.0049 .. +0.0057) | +0.0002 (-0.0007 .. +0.0010) | **-0.071** (-0.087 .. -0.054) |

Recall@100 behaves the same way (`quality.tsv`): `tsv` within 0.003 of exact everywhere,
`strip` 0.008-0.09 below.

### nDCG@10, Wikipedia known-item title queries (199 queries per corpus), run 2

| corpus / cfg | exact | `tsv` | Δ `tsv` (CI), queries changed | `tsvmaxpos` Δ (CI) | `strip` Δ (CI) |
|---|---:|---:|---|---|---|
| enwiki p1 whole / simple | 0.8904 | 0.8856 | -0.0048 (-0.0141 .. +0.0010), 4 | -0.0036 (-0.0117 .. +0.0011) | **-0.491** (-0.551 .. -0.430) |
| enwiki p1 whole / english | 0.8808 | 0.8735 | -0.0074 (-0.0184 .. +0.0006), 7 | -0.0036 (-0.0117 .. +0.0011) | **-0.524** (-0.584 .. -0.460) |
| enwiki p3 whole / simple | 0.8555 | 0.8568 | +0.0013 (-0.0001 .. +0.0034), 4 | 0 | **-0.424** (-0.487 .. -0.361) |
| enwiki p3 whole / english | 0.8448 | 0.8465 | +0.0017 (-0.0076 .. +0.0103), 12 | 0 | **-0.455** (-0.523 .. -0.390) |
| enwiki p6 whole / simple | 0.9320 | 0.9318 | -0.0002 (-0.0006 .. 0), 1 | 0 | **-0.283** (-0.336 .. -0.231) |
| enwiki p6 whole / english | 0.9082 | 0.9125 | +0.0043 (0 .. +0.0100), 3 | 0 | **-0.296** (-0.349 .. -0.245) |
| enwiki p1 400-word chunks / simple | 0.6784 | 0.6784 | 0 (0 changed) | 0 | **-0.331** (-0.373 .. -0.293) |
| enwiki p1 400-word chunks / english | 0.6833 | 0.6841 | +0.0008 (-0.0048 .. +0.0066), 83 | -0.0009 (-0.0031 .. +0.0012) | **-0.352** (-0.393 .. -0.311) |
| enwiki p3 256-word chunks / simple | 0.6591 | 0.6591 | 0 (0 changed) | 0 | **-0.248** (-0.285 .. -0.212) |
| enwiki p3 256-word chunks / english | 0.6473 | 0.6521 | +0.0048 (-0.0025 .. +0.0115), 85 | -0.0002 (-0.0019 .. +0.0014) | **-0.236** (-0.273 .. -0.200) |

(A chunked corpus scores lower because every chunk of the article counts as relevant, so
the ideal top-10 is ten chunks of one article, which BM25 on a title rarely returns.)

The sign is negative only on part 1, the corpus where the caps bite most, and no CI
excludes zero at about 200 queries. p6 / english's lower bound is exactly 0, which
means a *gain* of 0.0043 from 3 changed queries. Known-item title queries are a forgiving test: the article that
carries the title usually wins by a wide margin, so a rank change below position 1
rarely moves nDCG. The overlap numbers below show how much the ranking actually moves.

### Rank agreement with `exact` (no qrels; every query), run 2

`ov10` = mean |top-10 ∩ exact top-10| / 10, `same10` = share of queries whose top-10
is the same list in the same order. On Wikipedia the query set is the df-band queries,
`captf`, and the title queries (`agree.tsv` has every group separately).

| corpus / cfg | queries | `tsv` ov10 / same10 | `tsv_vs_sumtf` (caps only) ov10 | `tsvmaxpos` ov10 / same10 | `strip` ov10 | `exact2` ov10 |
|---|---:|---|---:|---|---:|---:|
| scifact / simple | 300 | 1.0000 / 1.000 | 1.0000 | 1.0000 / 1.000 | 0.691 | 1.0000 |
| scifact / english | 300 | 0.9517 / 0.107 | 1.0000 | 0.9997 / 0.997 | 0.670 | 1.0000 |
| nfcorpus / simple | 296 | 1.0000 / 1.000 | 1.0000 | 1.0000 / 1.000 | 0.638 | 1.0000 |
| nfcorpus / english | 306 | 0.9634 / 0.255 | 1.0000 | 0.9997 / 0.987 | 0.633 | 1.0000 |
| fiqa / simple | 648 | 0.9994 / 0.982 | 0.9994 | 1.0000 / 1.000 | 0.543 | 1.0000 |
| fiqa / english | 648 | 0.8904 / 0.006 | 1.0000 | 0.9944 / 0.810 | 0.490 | 1.0000 |
| enwiki p1 400-word chunks / simple | 499 | 0.9992 / 0.978 | 0.9992 | 1.0000 / 1.000 | 0.369 | 1.0000 |
| enwiki p1 400-word chunks / english | 499 | 0.9100 / 0.054 | 1.0000 | 0.9820 / 0.573 | 0.342 | 1.0000 |
| enwiki p3 256-word chunks / simple | 483 | 1.0000 / 1.000 | 1.0000 | 1.0000 / 1.000 | 0.418 | 1.0000 |
| enwiki p3 256-word chunks / english | 475 | 0.8834 / 0.038 | 1.0000 | 0.9768 / 0.497 | 0.399 | 1.0000 |
| **enwiki p1 whole / simple** | 549 | **0.9763 / 0.357** | **0.9763** | 0.9951 / 0.825 | 0.426 | 1.0000 |
| **enwiki p1 whole / english** | 549 | **0.9501 / 0.151** | **0.9945** | 0.9958 / 0.820 | 0.400 | 1.0000 |
| **enwiki p3 whole / simple** | 549 | **0.9847 / 0.556** | **0.9847** | 0.9960 / 0.842 | 0.446 | 1.0000 |
| **enwiki p3 whole / english** | 549 | **0.9423 / 0.126** | **0.9945** | 0.9945 / 0.822 | 0.409 | 1.0000 |
| **enwiki p6 whole / simple** | 550 | **0.9916 / 0.713** | **0.9916** | 0.9978 / 0.871 | 0.434 | 1.0000 |
| **enwiki p6 whole / english** | 550 | **0.9422 / 0.175** | **0.9965** | 0.9975 / 0.875 | 0.400 | 1.0000 |

Reproduction (run 1, df-band and `captf` queries only, 350 per corpus): p1 whole /
simple `tsv` 0.9783 / 0.380 (run 2, the same query groups: 0.9794 / 0.377), `tsvmaxpos`
0.9946 (run 2: 0.9946). p6 whole / simple at 20k articles: 0.9894 (run 2 at 60k: 0.9916).
BEIR and the 400-word chunks: identical to four decimals between the runs, except fiqa
`tsv` 0.9995 against 0.9994.

On whole Wikipedia under `simple` the caps cost the most on the most common terms
(part 1, `vcommon` 3-term queries: ov10 0.948, same10 0.00) and nothing on rare ones
(`rare_1`: 1.0000 on all three parts). That fits the mechanism: only a term that
repeats 255+ times in one document gets capped. `captf` (terms that actually hit the
cap) gives 0.982-0.984 on all three parts. `tsvmaxpos` recovers about three quarters of
the gap (p1: 0.9951) and cannot recover the rest, because tf itself is capped.

## Measure 3: heap bytes

Run 2. Run 1 gives identical per-value averages on every corpus both runs measured,
and table sizes within 0.6 %. Per-value `pg_column_size` averaged, and `pg_total_relation_size` of a two-column (id, value) table holding that
representation. TOAST compression `pglz`.

| corpus / cfg | text bytes (raw / stored) | tsvector | wdoc (with positions) | wdoc, no positions | table: text / tsvector / wdoc / wdoc no-pos (MB) |
|---|---|---:|---:|---:|---|
| scifact / simple | 1,501 / 1,063 | 2,108 | 2,272 | 1,772 | 6.2 / 12.4 / 13.5 / 10.6 |
| scifact / english | 1,501 / 1,062 | 1,496 | 1,682 | 1,320 | 6.2 / 8.9 / 9.9 / 7.7 |
| nfcorpus / english | 1,593 / 1,120 | 1,589 | 1,783 | 1,395 | 4.7 / 6.6 / 7.7 / 5.6 |
| fiqa / english | 769 / 667 | 768 | 889 | 717 | 41.7 / 48.2 / 56.2 / 45.0 |
| enwiki p1 400-word chunks / english | 2,404 / 1,708 | 2,492 | 2,755 | 2,149 | 368 / 546 / 614 / 484 |
| enwiki p3 256-word chunks / english | 1,513 / 1,290 | 1,667 | 1,849 | 1,495 | 358 / 483 / 549 / 423 |
| enwiki p1 whole / simple | 22,433 / 12,041 | 21,034 | **34,249** | 14,278 | 265 / 454 / **728** / 313 |
| enwiki p1 whole / english | 22,433 / 12,041 | 15,189 | **23,520** | 11,334 | 265 / 332 / **505** / 251 |
| enwiki p3 whole / english | 13,006 / 7,200 | 9,749 | 13,876 | 7,572 | 233 / 312 / 436 / 246 |
| enwiki p6 whole / english | 6,823 / 3,902 | 5,538 | 7,430 | 4,441 | 258 / 363 / 478 / 294 |

A `wdoc` with positions is **8-16 % larger than a tsvector** on short passages and chunks,
and **1.3-1.6x larger** on long whole articles. On long articles it is **1.9-2.8x the
stored text**. Two
causes: a wdoc stores 4 bytes per position where a tsvector stores 2, and a long
document's position list compresses poorly. A wdoc without positions (`sumtf`'s
column: tf only) is smaller than the tsvector everywhere. Converting at the index
boundary (option 1) **saves** the wdoc column completely, because the heap keeps only
the tsvector the user already had. Run 1, wiki1 simple: 454 MB of tsvector instead of
454 MB plus 728 MB of wdoc.

The weave index itself is the same size whichever representation it was built from
(`idxsize.tsv`: `ix_tsv` within 0.06 % of `ix_exact` on every corpus). Positions are not
indexed by default, so the index only sees tf and length.

## Controls, and what they rule out

- **Determinism (`exact2`):** the same table REINDEXed and re-queried agrees 1.0000 /
  same10 1.000 on every corpus and config in both runs, and paired Δ is exactly 0. So
  any disagreement in the other arms is the representation, not the scan.
- **Positive control (`strip`):** loses on every corpus, with every CI excluding zero
  (-0.028 to -0.071 nDCG@10 on BEIR, -0.24 to -0.52 on title queries; ov10 0.34-0.69). The
  harness can see a tf loss.
- **Between runs (hard rule 10):** for the same arm, BEIR nDCG@10 differs between run 1
  and run 2 by at most 0.0002 (`exact`, nfcorpus) and MRR@10 by at most 0.0062 (`strip`,
  nfcorpus). Per-value sizes are identical between the runs, and table sizes agree within 0.6 %. The most likely cause is the parallel `CREATE TABLE AS` that builds each
  table: physical row order, and so docid order, differs between runs, and docid breaks
  BM25 score ties. Within a run, `exact2` shows the scan itself is deterministic. A
  cross-run difference smaller than this floor is not a result.
- **Second scale / second corpus (hard rule 11):** the chunked "nothing lost" result
  holds on five chunked corpora (two chunk sizes) across two runs. The whole-document
  result holds on three disjoint Wikipedia parts (p1, p3, p6). The p6 rate is the same
  at 20k and 60k articles (5.58 % / 5.51 % tf-capped, 0.060 % / 0.053 %
  position-capped), and p1's overlap reproduces between runs (0.9783 / 0.9794).
- **Index scans actually ran:** `scancheck.tsv` has 0 rows where index scans < queries,
  in either run.

## wdoc round trip (side finding, outside M7: `doc/GAPS.md` G89 and G90)

> **FIXED 2026-10-07 on `wt/g89`** (both gaps; run `pgweave-20261007-140258-8e42`). The
> measurements below are of the code before that fix and stand as written. See the "Fix"
> notes in `doc/GAPS.md` G89 and G90.

`to_wdoc(regconfig, text)` stores parsetext's `LIMITPOS`'d positions, so every token past
16,383 gets position 16383 and a term that recurs there gets **duplicate** positions.
`weave_doc_build` (`src/query/doc.c:165`) rejects a non-ascending position list. Both
`wdoc_in`'s canonical parse and `wdoc_recv` go through it. So:

- probe P5, 20,000 tokens (`'a b ' x 10000`): `d::text::wdoc` **ERROR** `invalid wdoc:
  positions must be ascending within a term`; text `COPY TO` + `COPY FROM`: **ERROR**;
  binary `COPY`: **ERROR** `invalid binary wdoc: ...`. 10,000 tokens: OK.
  `to_wdoc(text)` (the built-in analyzer, uncapped positions) at 20,000 tokens: OK;
- on real data, every article over 16,383 tokens fails the text round trip: **172 of 172**
  in enwiki p1 under `simple` (171 under `english`), **56 of 56** in p3, **32 of 32** in
  p6.

`pg_dump` writes a `wdoc` column through `wdoc_out` and restores it through `wdoc_in`, so
**a dump of a table with a stored `to_wdoc(regconfig, text)` column holding one such
document does not restore**. The same applies to text-format logical replication and
`COPY`. This is independent of tsvector. It was found because this measurement had to
build exactly those documents. All three runs reproduce probe P5.

**The length does not survive I/O either (G90, probe P8, run `pgweave-20261007-041651-d0de`).**
`wdoc_out` prints no length, and `wdoc_recv` reads the length and discards it
(`src/query/doc.c:579`). Both inputs rebuild with length = sum of tf.
`to_wdoc('english', 'the cat sat on the mat')`: stored length 6, after `d::text::wdoc` 3,
after binary `COPY` 3. Nothing raises an error. Under a stopword config, a restored
`to_wdoc(regconfig, text)` column ranks like the `sumtf` arm above (top-10 overlap
0.88-0.96 against the original on BEIR and Wikipedia chunks). The `exact` arm in this
measurement was built in place and never went through I/O, so it is not affected.

## What the operator class should do (input to M7 step 2, not decided here)

Items 1 and 2 are **open maintainer decisions**, recorded in `doc/PHASES.md` M7.

1. **Refuse stripped input.** A tsvector with any positionless entry carries no tf, and
   BM25 over it is the `strip` arm: -0.03 to -0.07 nDCG@10 on BEIR, -0.5 on title
   queries. `to_wdoc(tsvector)` today degrades it silently to tf = 1.
2. **Length from the last position, not the sum of tf.** The last position is the exact
   token count (stopwords included) below 16,383. That makes a `tsvector` column rank
   like `to_wdoc(regconfig, text)` under a stopword config: overlap 0.994-0.9997 instead
   of 0.89-0.96 on BEIR, quality-neutral either way. The padding-term construction here
   only stands in for it; the opclass would set `doclen` directly. It does change what
   `to_wdoc(tsvector)` means today, so it is a decision.
3. **Count and expose capped documents** (already in M7's gate): any lexeme at 255 positions, or any position
   at 16,383. That count is the exact set of documents whose BM25 is approximate. 0 to
   0.002 % on every chunked corpus measured, 5.5-33.5 % of whole Wikipedia articles.

Not measured: PostgreSQL 18 (its tsvector caps are unchanged in the source), corpora of
whole documents other than Wikipedia (the pgsql-hackers archive's mbox downloads
require a community-account login, so it was not used), and query latency (all arms go
through the same index type, and latency is not what this task asks).
