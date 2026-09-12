# Result: what does one heap-rerank candidate cost in page reads?

Date: 2026-09-12. Harness: `bench/rerank_io.sh` (self-contained: builds its own
cluster, loads its own corpus, runs its own guards). PostgreSQL 17.11,
`shared_buffers = 32MB`, 250,000 rows per configuration, rerank window 100,
`pgbench` single client, 20 s per arm, cluster restarted before every arm.

**Local run. Page counts are authoritative; latency is not.** Page reads are a
property of the storage layout, so any machine must reproduce them. The latency
column here runs against a warm OS page cache and is further inflated by `md5()`,
which the hot query uses to force a full detoast. The deciding figure for the
Phase V gate is cold-cache p50 against pgvector HNSW on EBS, and this run does
not produce it — it produces the *prediction* that run has to match.

## The question

`bench/RESULTS_BITWIDTH_SWEEP.md` left Phase V one surviving shape: 3-bit codes
in the index plus an exact float32 rerank of a top-100 window (recall@10 = 1.0000
on both corpora, index at 0.067× pgvector HNSW). A stored float32 sidecar is
refuted by arithmetic, so the rerank must read full precision from the heap.
`doc/specs/VECTOR_CHANNEL.md` §2.1.1 priced that at "up to 100 heap fetches" and
named the cold figure as the decider. Nobody had counted the fetches.

`wvec` is `STORAGE = external` (`sql/pg_weave--0.1.0--0.2.0.sql:47`), so past
about 490 dimensions the vector is pushed out of line and a rerank candidate is a
toast-index descent plus chunk reads rather than one heap fetch.

## The answer

Total pages read per rerank candidate, scattered candidates (the real pattern —
100 arbitrary docids from a fused top-k):

| dim | B/vector | stored | heap | toast idx | toast | **total/candidate** | **per 100-window** |
|---|---:|---|---:|---:|---:|---:|---:|
| 200 | 800 | inline | 0.877 | 0.000 | 0.000 | **0.877** | 88 |
| 384 | 1,536 | inline | 0.932 | 0.000 | 0.000 | **0.932** | 93 |
| 768 | 3,072 | 2 chunks | 0.369 | 0.302 | 0.989 | **1.693** | 169 |
| 1024 | 4,096 | 3 chunks | 0.444 | 0.540 | 1.321 | **2.388** | 239 |
| 1536 | 6,144 | 4 chunks | 0.398 | 0.600 | 0.994 | **2.043** | 204 |

At Phase V's gate dimension of 1024, a 100-candidate rerank window costs about
**239 random page reads**, not 100.

## The hypothesis that motivated this was wrong, in the cheap direction

The script was written expecting ~5 pages per candidate at 1024-d — one page per
toast chunk plus the index descent — which would have made §2.1.1 wrong by 5×.
That is refuted. **Pages are not chunks.** A value's toast chunks are inserted
consecutively and about four 2,032-byte chunks pack into one 8 KB page, so the
chunks of a single vector overwhelmingly share a page.

Verified directly rather than reasoned: at 1536-d the toast relation holds exactly
**250,000 pages for 250,000 values** — 1.000 pages per value, four chunks in one
page — and the measured read count is 0.994 per candidate.

So the toast path costs about **1.5–2.5× the inline path, not 5×**, and the
correction to §2.1.1 is 2.4× rather than 5×.

## 1024-d is worse than 1536-d, and the reason is page straddling

The non-monotonicity is real and reproducible: 1024-d reads 2.388 pages per
candidate against 1536-d's 2.043, despite the vector being 33% smaller.

- 1536-d is 4 chunks = 8,128 B, which fills one page almost exactly. Stored at
  **1.000 pages/value**, so a value never straddles: 0.994 toast reads.
- 1024-d is 3 chunks = 6,096 B, so 1.34 values fit per page. Stored at **0.667
  pages/value**, and a randomly chosen value straddles a page boundary most of
  the time: 1.321 toast reads, a 33% surcharge on a smaller vector.

This is a layout artifact, not a size effect, and it is actionable: **a rerank
representation whose chunk count divides evenly into a toast page reads fewer
pages than a smaller one that does not.** Any future decision to store rerank
data ourselves should pick sizes that land on that boundary.

## What is bimodal, and why it matters more than the ratio

`shared_buffers` is 32 MB here against 1.3 GB of toast at 1024-d, deliberately, so
that reads are counted rather than cached. That is the cold regime the gate asks
about. In the other regime — the vectors fit in the buffer pool — every number in
the table above goes to approximately zero and the rerank is pure CPU.

The interesting consequence is that the two regimes have different boundaries for
the two designs. pgvector HNSW must hold 4,096 B/vector plus graph links to be
fast; weave's 3-bit codes are 384 B/vector, **0.067×**. So there is a corpus size
range where the codes fit in RAM and HNSW's index does not. That is where the
rerank's page reads are affordable and HNSW's traversal is not. It is also the
regime this local run cannot measure.

## What decides the gate, and what this run says about it

239 random reads per query converts to latency only via device latency and
**queue depth**:

- serialized on EBS gp3 at ~0.35 ms per random read: ~84 ms, which loses to
  pgvector HNSW by a wide margin;
- pipelined at depth 32: ~2.6 ms, which is competitive.

The candidate TIDs are all known before the first fetch, because the fused top-k
produces the whole window at once. So the reads *can* be issued together —
`effective_io_concurrency`, `posix_fadvise`, or a read stream. HNSW's graph
traversal is inherently dependent: it cannot know its next node until the current
one is scored. **The gate therefore turns on prefetch depth, which is a structural
advantage of this shape rather than a tuning parameter**, and that is the thing
the EC2 run must measure.

Not measured here, and not to be assumed: whether detoasting can in fact be
prefetched. The mechanism argues it cannot, as things stand. Detoasting is lazy —
it happens inside expression evaluation, one datum at a time — and
`heap_fetch_toast_slice()` walks the toast index with a synchronous systable scan.
There is no AIO or read-stream path for toast in PostgreSQL 17. So today's 239
reads are **serialized by construction**, which puts this shape at the ~84 ms end
of the range, not the ~2.6 ms end.

Getting to the good end needs real code, and the shape of it is two dependent
prefetch rounds rather than one: the toast pointers live in the main heap tuples,
so a rerank would (1) prefetch the 100 heap pages, which a bitmap heap scan
already knows how to do, read the pointers, then (2) prefetch the toast chunk
pages before detoasting. Two rounds of depth-100 prefetch is plausibly ~1 ms; one
round is not enough, because round 2's page numbers are not knowable until round 1
has landed. L6 (`read_stream`) is in the ledger already, but for posting pages, not
for this.

**This is the strongest argument for shrinking the rerank window rather than
speeding up the fetch**, since the window scales the read count linearly and needs
no new I/O machinery. The window had only ever been measured at 10 (a no-op for
recall@10, since exact rescoring of the top 10 cannot introduce an eleventh
document) and at 100, so everything decision-relevant sat in the gap.

**That gap is now measured, and it pays 5×.** `bench/RESULTS_BITWIDTH_SWEEP.md`'s
window sweep, run the same day: at 4 bits a top-**20** window reaches recall@10
0.9940 on GIST-960d and 0.9970 on GloVe-200d, both over the gate, at 512 B/vector
= 0.090× HNSW. Priced with the 2.388 figure above, that is **48 random page reads
per query instead of 239**. The 100-candidate window buys 1.0000, which the gate
never asked for.

Serialized, 48 reads is still ~17 ms cold on EBS gp3 and probably still over
`p50 ≤ 2× pgvector HNSW`. So the cut brings the target within reach of a modest
prefetch instead of an ambitious one; it does not close the gate on its own.

## The guards, and the one that was wrong

Two ways for this measurement to look cheap while being meaningless, both guarded:

1. **The corpus fitting in the pool.** Configuration guard, checked before any
   query: the relation holding the vectors must exceed 4× `shared_buffers`, or the
   script aborts. The first smoke run of this harness failed exactly this way — 20k
   rows fit in a 128 MB pool, the full arm read 0.139 toast pages per candidate,
   and it looked like a result.
2. **The hot query not materialising the vector.** On external storage,
   `length()` and `substring()` fetch only the slices they need. Discrimination
   guard: a slice arm must read strictly fewer pages than a full arm. Passes at
   **5.595 vs 1.160** pages/candidate.

That second guard runs at **8192-d**, not at the dims under test, and the reason
is the third broken guard of this cycle. The first version compared slice against
full at every dim and reported failures at 768-d and 1536-d where slice and full
both measured 0.98–0.99. That was not a failure. Both arms touch the *same single
page*, so a page-counting guard cannot distinguish how many chunks each arm read,
no matter how correct both the arms and the counter are. The guard needed a
configuration where a value spans several pages (8192-d is 32 KB, 4.125 stored
pages per value) before it could discriminate at all.

**A guard whose unit is wrong reports failures as confidently as successes.**
Three guards this cycle failed silently by passing; this one failed loudly by
being wrong, which is cheaper but has the same root cause — nobody had checked
that the guard could observe the thing it was asserting about.

## Reproduce

```
bash bench/rerank_io.sh                       # defaults above
DIMS=1024 NROWS=1000000 SB=128MB bash bench/rerank_io.sh
```

`OUT` defaults to `/scratch/pgw-rerank-io/out`; `pages.tsv` and `layout.tsv` hold
the two tables above.
