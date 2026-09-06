---
name: weave-bench
description: Use when benchmarking pg_weave or recording a performance result — choosing a corpus, warm vs cold methodology, writing a RESULTS file, or running on EC2 with the bene AWS profile. Load this before producing any number that will end up in a document, and before launching any cloud instance.
---

# Benchmarking pg_weave credibly

## The rule that matters most

**Verify correctness before recording a latency.**

pg_turbovec published a "we win 2.3x on warm p50" claim that had to be retracted.
The cause: a pre-AVX2 scalar-fallback bug returned results fast and **wrong**. The
benchmark measured a broken code path. Re-measured under contention control after
the fix, the real number was 490x *slower* than pgvector HNSW, and
`pg_turbovec/docs/PARITY_GAPS.md` now carries the retraction inline.

A benchmark of a broken fast path is worse than no benchmark, because it gets
quoted. Every latency run must be preceded by a correctness check on the same
build: same query, row sets compared against a seq-scan or exhaustive reference.

## Methodology

- **One engine per host.** Never benchmark pg_weave and a competitor on the same
  instance. Shared buffer cache, shared page cache, and shared thermals make the
  numbers meaningless. pg_fts's 5-way comparison used one dedicated instance per
  engine, which is why its numbers held up.
- **Warm, median of 5, first run dropped.** State it explicitly. Report cold
  separately when it is the point (e.g. a prefetch change).
- **Prewarm deliberately** (`pg_prewarm` or a full scan) and say so.
- **`shared_buffers` large enough to hold the index**, or say that it is not and
  that you are therefore measuring I/O.
- **Report p50 and p99.** A p50 win with a p99 loss is a regression for anyone
  with a latency SLO.
- **Report the mechanism counter, not just the clock.** For the fused scorer that
  is the `score()`-call ratio (`doc/specs/FUSED_TOPK.md` §8). If the ratio is not
  much lower than the baseline, a latency win is luck and will not survive a
  different corpus.

## Corpora

| corpus | for | note |
|---|---|---|
| Wikipedia 20231101.en, first 2.19 M articles | lexical, the 5-way comparison | pg_fts's benchmarks use exactly this; stay comparable |
| MS MARCO passage | nDCG for the fused scorer | has real relevance judgments |
| BEIR subset | fusion quality generalization | at least 2 datasets before claiming a quality win |
| Cohere-wiki 1 M × 1024-d | vector recall/latency/storage | the corpus pg_turbovec's 490x loss was measured on |
| GloVe 200-d | vector worst case | turbovec names d=200 as hardest for the asymptotic-Beta codebook assumption. Do not skip it because it is unflattering |
| `/usr/share/dict/words` Zipfian sample, 1 M rows | fuzzy/regex | pg_tre's `doc/perf.md` uses this; stay comparable |

Comparability with the source projects' own numbers is worth more than a nicer
corpus. If pg_fts measured a term with df=10,875 on Wikipedia, measure that term.

## Writing a RESULTS file

`bench/RESULTS_BOUND_PRUNING.md` is the template. In order:

1. Date, harness path, and the **exact commands to reproduce**.
2. Setup: hardware, PostgreSQL version, settings that matter, corpus and size,
   what "warm" meant.
3. A table of numbers.
4. **What the numbers mean**, including any conclusion that changed the design.
5. **What this does not tell us** — dimensions not covered, corpora not tried, and
   any metric that is an upper bound rather than a measurement.

Section 5 is not modesty. It is what stops the number being over-quoted later.

## Standalone C benchmarks

Some questions do not need a database. `bench/bound_pruning.c` answers "does the
block bound prune?" by linking the codec directly:

```sh
gcc -O2 -I include -o /tmp/bp bench/bound_pruning.c \
    src/vector/quantize.c src/vector/pack.c -lm
/tmp/bp 1   # coherent warp order
/tmp/bp 0   # random warp order
```

That harness also asserts bound soundness on every block of every query, so it
doubles as a large randomized correctness test. Prefer this shape when the question
is about an algorithm rather than about PostgreSQL: seconds to run, reproducible on
a laptop, and it answered a design question that would otherwise have surfaced
months into implementation.

## EC2, with the bene profile

Profile `bene`, account 292759875395. Verify first:

```sh
aws sts get-caller-identity --profile bene
```

Workflow: launch, tune, load, measure, record, **terminate**.

Instance guidance, from what the source projects actually used:

- Lexical / mixed: `r6id.4xlarge` (16 vCPU, 128 GB, local NVMe) — pg_fts's 5-way.
- Large-scale stress: `i4i.8xlarge` (NVMe) — pg_tre's at-scale qualification.
- Put the data directory on local instance storage, not EBS, unless you are
  deliberately measuring EBS.

Tuning to apply **and to record**: `shared_buffers`, `maintenance_work_mem`,
hugepages, CPU governor set to performance, THP setting, NUMA balancing off. An
untuned number and a tuned number differ by more than most of the changes we
measure, so an unrecorded setting makes the result unusable.

**Termination is mandatory and must survive failure.** Write the runner so the
instance dies on the error path too:

```sh
trap 'aws ec2 terminate-instances --profile bene --instance-ids "$IID"' EXIT
```

Then verify it actually died:

```sh
aws ec2 describe-instances --profile bene --instance-ids "$IID" \
  --query 'Reservations[].Instances[].State.Name'
```

Check for strays before finishing a session:

```sh
aws ec2 describe-instances --profile bene \
  --filters Name=instance-state-name,Values=running \
  --query 'Reservations[].Instances[].[InstanceId,InstanceType,LaunchTime]'
```

A forgotten bare-metal instance costs more than the benchmark was worth.

For the full launch/tune/measure/terminate procedure the `aws-benchmark` skill
covers the substrate; this file only adds the pg_weave-specific parts.
