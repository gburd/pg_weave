# Competitive landscape

Written 2026-09-06. Numbers are cited to their source; where a figure is a target
rather than a measurement it says so. `doc/PRODUCTION_READINESS.md` is the
companion document and should be read first — pg_weave is not shippable today and
nothing below implies otherwise.

## The field

| system | architecture | license | hybrid fusion | deployment |
|---|---|---|---|---|
| **pg_weave** | PostgreSQL index AM, C, one index many channels | PostgreSQL | fused threshold (specified) | wherever Postgres runs |
| **turbopuffer** | object-storage-first search engine, Rust | closed, SDK only | server-side RRF | SaaS, BYOC |
| **ParadeDB pg_search** | Tantivy embedded in a PG extension, Rust | AGPL-3.0 | RRF | self-host, cloud |
| **pgvector** | PG index AM (HNSW/IVFFlat) | PostgreSQL | none (vector only) | anywhere |
| **VectorChord** | PG extension, Rust | non-standard | partial | anywhere |
| **Timescale pg_textsearch** | PG index AM, C | PostgreSQL | none (BM25 only) | anywhere |
| **Qdrant / Weaviate / Milvus** | dedicated vector DB | Apache-2.0 mostly | varies | self-host, cloud |

## turbopuffer in detail

Sources: `turbopuffer.com/docs/architecture`, `/docs/limits`, `/docs/hybrid`,
`/pricing`, retrieved 2026-09-06.

### What it is

A search engine whose system of record is **object storage**. Rust binaries
(`./tpuf`) sit in front of S3; each namespace is an S3 prefix with its own
write-ahead log. Writes append a WAL file; a successful write is durable in S3.
Data is asynchronously indexed, and not-yet-indexed data is still searchable via a
slower exhaustive scan of the log tail. NVMe SSD caches hot namespaces, with query
routing biased for cache locality.

This is a genuinely good design for the problem it targets, and the economics are
the point: S3 durability at S3 prices, with compute detached from storage.

### Measured characteristics they publish

| | |
|---|---|
| cold query, 1 M documents | p50 **874 ms** (first query to a namespace) |
| warm query, 1 M documents | p50 **14 ms** |
| write latency | p50 **165 ms** for 500 kB |
| write throughput, per namespace | 10k writes/s @ 32 MB/s |
| write throughput, global | seen 10M+ writes/s @ 32 GB/s |
| **commit granularity** | **1 WAL entry per second per namespace** |
| queries, per namespace | 5k+/s |
| scale, per namespace | 128B docs @ 256 TB (seen 100B @ 200 TB) |
| scale, global | seen 1T+ docs @ 3 PB+, 250M+ namespaces |
| **vector recall@10** | **90–100 %** |
| consistency | strong by default; eventual option with up to ~1 h staleness |
| max unindexed data | 2 GB |
| max `limit.total` | 10k |
| hybrid | `multi_query` (≤16 queries) + server-side RRF |
| price | $16/mo min (launch), $256/mo (scale), ≥$4,096/mo + 35 % usage premium (enterprise, 99.95 % SLA) |

### Where turbopuffer is clearly better than pg_weave will ever be

Not "for now" — architecturally.

1. **Scale.** 1T+ documents across 3 PB, 250M+ namespaces. pg_weave has a
   **128-segment cap per index** and a single-node PostgreSQL relation underneath.
   Nothing in pg_weave's design reaches a trillion documents, and pretending
   otherwise would be silly.
2. **Storage economics.** S3 pricing for the system of record, compute scaled
   independently. pg_weave's data lives on whatever block storage the Postgres
   instance has, provisioned for peak.
3. **Multi-tenancy at extreme namespace counts.** 250M namespaces is a
   250M-relation problem in Postgres, which is not a thing you do.
4. **Operational burden is theirs.** SOC2, HIPAA-ready BAA, SSO, audit log
   streams, 99.95 % SLA, on-call. pg_weave is a `.so` you are responsible for.
5. **It exists and works now.** This is not a small advantage over a project whose
   central feature is unimplemented.

### Where pg_weave's design is better, if it is finished

1. **Transactional consistency with your data.** turbopuffer is a separate system:
   you write to Postgres, then write to turbopuffer, and you own the
   reconciliation problem forever — dual writes, backfills, drift, and a window
   where search disagrees with the database. pg_weave is an index on the table. A
   row and its searchability commit together, in one transaction, and roll back
   together. There is no sync pipeline because there is nothing to sync.
2. **Write latency and commit granularity.** turbopuffer's **1 WAL entry per second
   per namespace** means a write can take up to a second to become committed, at
   p50 165 ms. A PostgreSQL `INSERT` with a `weave` index commits in single-digit
   milliseconds and is immediately visible to the inserting transaction. For
   anything user-facing that writes and then reads its own write, that difference
   is the difference between working and needing a cache.
3. **No cold-start cliff.** 874 ms cold versus 14 ms warm is a 62× penalty, and
   it recurs whenever a namespace falls out of cache (documented as "hours" of
   inactivity). Their answer is a pre-flight warming query. pg_weave's index is in
   the same buffer cache as the rest of the database, with no separate warm/cold
   regime and no pinning strategy to design.
4. **Fusion.** turbopuffer's hybrid search is `multi_query` plus **server-side
   RRF** — the exact over-fetch-then-rank-fuse pattern that
   `doc/specs/FUSED_TOPK.md` exists to replace. It also caps a multi-query at 16
   sub-queries and `limit.total` at 10k. pg_weave's fused-threshold top-k, *when
   it works*, needs no over-fetch and produces a score with meaning rather than a
   rank reciprocal. This is the one axis where pg_weave is attempting something
   they are not.
5. **Filters that accelerate rather than degrade.** pg_weave pushes a predicate
   bitmap into graph traversal and into the SIMD block mask, so selectivity makes
   queries faster. turbopuffer builds exact metadata indexes, which is a real
   answer, but the fusion is still filter-then-search across separate structures.
6. **Recall you can pin.** turbopuffer publishes recall@10 of **90–100 %** — a
   ten-point range with no per-query control documented. pg_weave's
   `weave.vec_recall = exact` gives 1.000 at the cost of a scan, on demand, per
   query.
7. **Joins, and everything else Postgres does.** Filtering search results by a
   join against another table, aggregating them, wrapping them in a CTE, applying
   row-level security. In turbopuffer these are application code.
8. **Open source, PostgreSQL-licensed, self-hostable, no vendor.** turbopuffer is
   closed source with SDK-only access. For some buyers that is disqualifying
   regardless of merit.
9. **Cost at small and medium scale.** turbopuffer starts at $16/mo minimum and
   enterprise features start at $4,096/mo plus a 35 % usage premium. pg_weave costs
   whatever your existing Postgres costs, which for most applications that already
   run Postgres is zero marginal.

### The honest summary

**They are not really competitors, and the boundary is legible.**

turbopuffer is the right answer when the corpus is larger than one Postgres
instance should hold, when you have very many tenants, when storage cost dominates,
and when a second system with its own consistency model is acceptable.

pg_weave — *if finished* — is the right answer when the search corpus is already in
Postgres, when search must be transactionally consistent with the data, when write
latency matters, and when you would rather not operate two systems or reconcile
them.

The comparison that should worry pg_weave is not turbopuffer. It is
**pgvector + pg_fts + pg_trgm used separately**, which is free, finished, and
already installed. pg_weave has to beat *that*, and today it does not.

## ParadeDB pg_search

The nearest architectural competitor, and the one to actually beat.

**For it:** by far the largest adoption and mindshare, a funded company whose whole
product this is, and Tantivy — a mature Lucene-class engine giving strong ranked
speed, faceting, phrase, and a rich query surface. Measured 2.27 ms on common-term
ranked k=10 at 2.19 M documents where pg_fts took 74.7 ms
(`pg_fts/bench/RESULTS_5WAY_157`). That is a 32× loss on a query shape that
matters, and pg_weave inherits it until task L2 lands.

**Against it:** **AGPL-3.0**, which is a hard blocker inside many companies and
disqualifying for the contrib track. And Tantivy lives largely outside PostgreSQL's
buffer manager, WAL, and MVCC — a bolt-on engine, which complicates the
"behaves like a Postgres index" story and makes crash-safety a matter of trusting
their integration rather than reading `GenericXLog` call sites.

pg_weave's structural bet is exactly this: C, PostgreSQL license, 100 %
`GenericXLog`, `trusted`, no external engine. That is a real differentiator that
does not depend on winning a benchmark. It is also worth nothing if the extension
never gets finished.

## pgvector

The incumbent and the real adoption obstacle. 1M × 1024-d Cohere-wiki, HNSW:
**p50 5.2 ms at recall@10 0.96, 1953 MiB** (`pg_turbovec/docs/PARITY_GAPS.md`).

pg_weave's Phase V gate is recall ≥ 0.99, p50 ≤ 2× pgvector, storage ≤ 0.15×
pgvector, simultaneously. That is the bar, it is not met, and the flat-scan
predecessor missed it by **490×** on latency. Task V9 (graph over quantized codes)
is what closes it, and V9 is not started.

pgvector is also small, boring, and everywhere. `doc/ARCHITECTURE.md` §8.4 concedes
operational simplicity as a permanent loss.

## What pg_weave should claim, and to whom

To be said only once the gates in `doc/PRODUCTION_READINESS.md` pass:

> One PostgreSQL index for text, vector, fuzzy, and regex search, with fused
> top-k across all of them, transactionally consistent with your data, in C under
> the PostgreSQL license.

Audience: teams whose data is already in Postgres, whose corpus fits one instance
(millions to low hundreds of millions of rows), who need search consistent with
their transactions, and for whom AGPL or a second system is a problem.

Not the audience: anyone with a billion documents, anyone with a million tenants,
anyone for whom object-storage economics dominate. Those people should use
turbopuffer and pg_weave should say so.
