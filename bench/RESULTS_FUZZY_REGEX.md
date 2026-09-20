# Result: Z5 (fuzzy `term~1`/`term~2`) and Z6 (character-class regex) latency gates

Date: 2026-09-20. Harness: `bench/fuzzy.sh` via `bench/aws/run.sh c7i.4xlarge fuzzy`.
Commit under test: `6ca1ee7261c2a9adb39fa3a4f3e2d14a387709b3` (`bench: fuzzy/regex
gate harness (Z5, Z6)`, on top of `bb06636`).
Reproduce: `AWS_PROFILE=hotdog AWS_REGION=us-east-2 bench/aws/run.sh c7i.4xlarge fuzzy`.
Raw log: `bench/aws/out/pgweave-20260920-020939/fuzzy.log`.

Both routes -- `weave_fuzzy_terms()` and `weave_regex_terms()` (`src/am/amscan.c`)
-- were implemented before this run and had only dev-box, not-for-the-record
numbers (see the Z5/Z6 rows in `doc/PHASES.md`). This is the first `bench/aws/run.sh`
run behind either gate, and therefore the first commit-tied number.

## Setup

| | |
|---|---|
| instance | `c7i.4xlarge`, 16 vCPU |
| CPU | Intel(R) Xeon(R) Platinum 8488C |
| PostgreSQL | 17.11 (PGDG, Ubuntu 24.04), `shared_buffers` 12619 MB (of ~30 GB RAM), `maintenance_work_mem` 2 GB, `work_mem` 256 MB, `effective_cache_size` 22 GB, `jit=off` |
| corpus | 1,000,000 rows, one stored `wdoc` column (`to_wdoc('simple', body)`), 8 tokens/row: 1 id-shaped token from a 10,000-value vocabulary (`'e' || lpad(((g::bigint*7919) % 10000)::text,4,'0')`) + 7 filler tokens from a ~250,000-value vocabulary (`'t' || ((g::bigint*7 + i*104729) % 250000)`); heap 516 MB |
| index arms | same table, one index at a time: `off` = `CREATE INDEX ... USING weave (d)` (default reloption), `on` = `... WITH (trigrams = on)` |
| index size | `off` 42 MB (43,597,824 B) / `on` 51 MB (53,452,800 B), both `pg_relation_size` |
| session settings | `max_parallel_workers_per_gather = 0`, `enable_seqscan = off`, for every timed and correctness query |
| method | warm: REPS=7 in one session, first dropped, p50/p99 from `EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON)`'s own Execution Time. **Every arm run TWICE** (pass A, pass B) |
| correctness | every index `count(*)` checked against a no-index reference (fuzzy: `levenshtein_less_equal` over the row's tokens; regex: `~` over the row's tokens) **before any timing**, in both arms |

Pass A and pass B are on the **same host and the same instance**
(`i-0a645f10e77ecd682`) -- one scale. Hard rule 11's second-scale reproduction is
still owed; nothing here claims it.

Smoke (build, lint gates, `installcheck`, TAP) ran first and passed: `Files=19,
Tests=269 ... Result: PASS` (`bench/aws/out/pgweave-20260920-020939/installcheck.log`).

## Correctness

34/34 checks passed (17 queries x 2 arms): every index `count(*)` equalled the
no-index reference. No mismatch, no abort. Full list in the raw log, lines 13-30
(arm off) and 93-110 (arm on); representative:

| query | index count | reference count |
|---|---:|---:|
| `t123456~1` | 1,484 | 1,484 |
| `t123456~2` | 35,676 | 35,676 |
| `t7~1` | 784 | 784 |
| `t7~2` | 9,856 | 9,856 |
| `/e12[0-9]{2}/` | 10,000 | 10,000 |
| `/e1234/` | 100 | 100 |
| `/^e12/` | 10,000 | 10,000 |
| `/e[0-9]{4}/` | 1,000,000 | 1,000,000 |

## Latency, milliseconds -- arm `off` (trigrams = off, the default)

### Pass A

| query | p50 | p99 |
|---|---:|---:|
| `t123456~1` | 85.54 | 85.78 |
| `t123456~2` | 292.77 | 293.32 |
| `t204711~1` | 92.82 | 93.00 |
| `t204711~2` | 260.88 | 261.18 |
| `t98765~1` | 78.83 | 79.06 |
| `t98765~2` | 225.59 | 225.84 |
| `t31337~1` | 72.97 | 73.13 |
| `t31337~2` | 228.23 | 229.32 |
| `t7~1` | 58.05 | 58.26 |
| `t7~2` | 207.98 | 208.30 |
| `/e12[0-9]{2}/` | 49.34 | 49.79 |
| `/e34[0-9]{2}/` | 47.80 | 48.15 |
| `/e56[0-9]{2}/` | 47.33 | 47.43 |
| `/e1234/` | 48.43 | 48.90 |
| `/e5678/` | 46.29 | 46.44 |
| `/^e12/` | 22.89 | 23.06 |
| `/e[0-9]{4}/` (fanout wall) | 317.29 | 317.78 |

### Pass B

| query | p50 | p99 |
|---|---:|---:|
| `t123456~1` | 85.53 | 85.72 |
| `t123456~2` | 292.60 | 292.68 |
| `t204711~1` | 92.92 | 93.14 |
| `t204711~2` | 260.66 | 260.80 |
| `t98765~1` | 78.75 | 79.01 |
| `t98765~2` | 225.47 | 225.66 |
| `t31337~1` | 73.02 | 73.17 |
| `t31337~2` | 228.67 | 229.38 |
| `t7~1` | 57.95 | 58.12 |
| `t7~2` | 207.92 | 208.01 |
| `/e12[0-9]{2}/` | 49.58 | 49.71 |
| `/e34[0-9]{2}/` | 47.40 | 47.68 |
| `/e56[0-9]{2}/` | 47.58 | 47.81 |
| `/e1234/` | 48.31 | 48.48 |
| `/e5678/` | 46.51 | 46.67 |
| `/^e12/` | 23.26 | 23.28 |
| `/e[0-9]{4}/` (fanout wall) | 317.84 | 318.60 |

Within-arm spread (max - min of the two passes' p50, arm `off`): fuzzy queries
0.01-0.44 ms, regex queries 0.24-0.55 ms except the anchored query at 0.37 ms --
all under 1 ms, i.e. the two passes agree closely.

## Latency, milliseconds -- arm `on` (trigrams = on)

### Pass A

| query | p50 | p99 |
|---|---:|---:|
| `t123456~1` | 85.44 | 85.66 |
| `t123456~2` | 292.71 | 293.23 |
| `t204711~1` | 92.71 | 93.09 |
| `t204711~2` | 260.48 | 260.85 |
| `t98765~1` | 79.04 | 79.18 |
| `t98765~2` | 225.45 | 225.72 |
| `t31337~1` | 73.03 | 73.63 |
| `t31337~2` | 228.65 | 229.17 |
| `t7~1` | 58.02 | 58.16 |
| `t7~2` | 207.71 | 208.53 |
| `/e12[0-9]{2}/` | 1.08 | 1.09 |
| `/e34[0-9]{2}/` | 1.06 | 1.08 |
| `/e56[0-9]{2}/` | 1.07 | 1.14 |
| `/e1234/` | 0.17 | 0.17 |
| `/e5678/` | 0.14 | 0.14 |
| `/^e12/` | 1.04 | 1.07 |
| `/e[0-9]{4}/` (fanout wall) | 316.94 | 319.14 |

### Pass B

| query | p50 | p99 |
|---|---:|---:|
| `t123456~1` | 85.35 | 85.59 |
| `t123456~2` | 293.29 | 293.46 |
| `t204711~1` | 92.73 | 92.98 |
| `t204711~2` | 260.14 | 260.42 |
| `t98765~1` | 78.97 | 79.19 |
| `t98765~2` | 225.43 | 225.60 |
| `t31337~1` | 72.76 | 72.83 |
| `t31337~2` | 228.63 | 228.87 |
| `t7~1` | 58.01 | 58.33 |
| `t7~2` | 207.86 | 208.24 |
| `/e12[0-9]{2}/` | 1.05 | 1.08 |
| `/e34[0-9]{2}/` | 1.07 | 1.15 |
| `/e56[0-9]{2}/` | 1.08 | 1.09 |
| `/e1234/` | 0.18 | 0.19 |
| `/e5678/` | 0.12 | 0.14 |
| `/^e12/` | 1.05 | 1.13 |
| `/e[0-9]{4}/` (fanout wall) | 319.25 | 320.57 |

Fuzzy p50s are within noise of arm `off` (largest delta 0.31 ms) -- consistent with
the channel-stats table below, where `fuzzy_trgm` is 0 in both arms: the trigram
weft never serves these queries regardless of the reloption. Regex p50s drop by
1-2 orders of magnitude on every narrowable pattern (e.g. `/e12[0-9]{2}/` 49 ms ->
1.08 ms), because `regex_trgm` goes from 0 to 1 -- the weft narrows the dictionary
walk. The fanout-wall query (`/e[0-9]{4}/`, matches all 1,000,000 rows) is
~317-320 ms in **both** arms: no narrowing is possible when every term matches, so
this is posting decode/merge cost, the same wall Z5's `~2` queries hit.

## `weave_channel_stats()` deltas per query (reset, run once, read)

Columns: `fuzzy_dict`, `fuzzy_trgm`, `regex_dict`, `regex_trgm`, `terms_expanded`,
`dict_pages`.

### Arm `off`

| query | fuzzy_dict | fuzzy_trgm | regex_dict | regex_trgm | terms_expanded | dict_pages |
|---|---:|---:|---:|---:|---:|---:|
| `t123456~1` | 1 | 0 | 0 | 0 | 0 | 373 |
| `t123456~2` | 1 | 0 | 0 | 0 | 0 | 983 |
| `t204711~1` | 1 | 0 | 0 | 0 | 0 | 303 |
| `t204711~2` | 1 | 0 | 0 | 0 | 0 | 836 |
| `t98765~1` | 1 | 0 | 0 | 0 | 0 | 213 |
| `t98765~2` | 1 | 0 | 0 | 0 | 0 | 717 |
| `t31337~1` | 1 | 0 | 0 | 0 | 0 | 230 |
| `t31337~2` | 1 | 0 | 0 | 0 | 0 | 724 |
| `t7~1` | 1 | 0 | 0 | 0 | 0 | 170 |
| `t7~2` | 1 | 0 | 0 | 0 | 0 | 596 |
| `/e12[0-9]{2}/` | 0 | 0 | 1 | 0 | 100 | 1020 |
| `/e34[0-9]{2}/` | 0 | 0 | 1 | 0 | 100 | 1020 |
| `/e56[0-9]{2}/` | 0 | 0 | 1 | 0 | 100 | 1020 |
| `/e1234/` | 0 | 0 | 1 | 0 | 1 | 1020 |
| `/e5678/` | 0 | 0 | 1 | 0 | 1 | 1020 |
| `/^e12/` | 0 | 0 | 1 | 0 | 100 | 1020 |
| `/e[0-9]{4}/` | 0 | 0 | 1 | 0 | 10000 | 1020 |

### Arm `on`

| query | fuzzy_dict | fuzzy_trgm | regex_dict | regex_trgm | terms_expanded | dict_pages |
|---|---:|---:|---:|---:|---:|---:|
| `t123456~1` | 1 | 0 | 0 | 0 | 0 | 373 |
| `t123456~2` | 1 | 0 | 0 | 0 | 0 | 983 |
| `t204711~1` | 1 | 0 | 0 | 0 | 0 | 303 |
| `t204711~2` | 1 | 0 | 0 | 0 | 0 | 836 |
| `t98765~1` | 1 | 0 | 0 | 0 | 0 | 213 |
| `t98765~2` | 1 | 0 | 0 | 0 | 0 | 717 |
| `t31337~1` | 1 | 0 | 0 | 0 | 0 | 230 |
| `t31337~2` | 1 | 0 | 0 | 0 | 0 | 724 |
| `t7~1` | 1 | 0 | 0 | 0 | 0 | 170 |
| `t7~2` | 1 | 0 | 0 | 0 | 0 | 596 |
| `/e12[0-9]{2}/` | 0 | 0 | 1 | 1 | 100 | 6 |
| `/e34[0-9]{2}/` | 0 | 0 | 1 | 1 | 100 | 14 |
| `/e56[0-9]{2}/` | 0 | 0 | 1 | 1 | 100 | 23 |
| `/e1234/` | 0 | 0 | 1 | 1 | 1 | 5 |
| `/e5678/` | 0 | 0 | 1 | 1 | 1 | 23 |
| `/^e12/` | 0 | 0 | 1 | 1 | 100 | 6 |
| `/e[0-9]{4}/` | 0 | 0 | 1 | 0 | 10000 | 1020 |

Every fuzzy leaf shows `fuzzy_dict = 1`, `fuzzy_trgm = 0` in both arms: `term~k`
is served exclusively by the `uleven` dictionary walk here, never by the trigram
funnel, regardless of the reloption. Every regex leaf shows `regex_dict = 1`; on
the `on` arm every narrowable pattern (all but the fanout wall) also shows
`regex_trgm = 1` and a `dict_pages` count that collapses from 1,020 (the whole
dictionary, walked once per segment) to single digits -- the trigram weft
intersection at work. The fanout-wall pattern `/e[0-9]{4}/` keeps `regex_trgm = 0`
and `dict_pages = 1020` in both arms: it cannot be narrowed (every term matches),
so the weft buys nothing there, matching the flat ~317-320 ms latency above.

## Gates by the numbers

Reported p50 is the worse (max) of pass A and pass B, per query, per arm --
the conservative read given hard rule 10.

- **Z5, `term~1`, gate p50 <= 200 ms:** worst observed p50 across the five terms is
  92.92 ms (arm `off`, `t204711~1`, pass B) / 92.73 ms (arm `on`). **Gate met** by
  the numbers above.
- **Z5, `term~2`, gate p50 <= 200 ms:** worst observed p50 across the five terms is
  292.77 ms (arm `off`, `t123456~2`, pass A) / 293.29 ms (arm `on`, pass B).
  **Gate not met** by the numbers above.
- **Z6, character-class regex `e12[0-9]{2}`, gate p50 <= 100 ms:** 49.58 ms
  (arm `off`, pass B) / 1.08 ms (arm `on`, pass A). **Gate met** by the numbers
  above, in both arms.

## What this does not tell us

- **One scale, one host.** Pass A and pass B are the same instance
  (`i-0a645f10e77ecd682`); hard rule 11's second-scale reproduction is still owed
  for both gates.
- **Vocabulary shape is synthetic and narrow**: the fuzzy terms probe a ~250,000-
  value filler vocabulary and the regex patterns probe a 10,000-value id
  vocabulary on the same corpus. A vocabulary with different length or alphabet
  distribution was not tried.
- **No cold-start numbers.** Only warm, in-session latency was measured, per the
  task's method; a fresh-backend first-scan cost (as `bench/lexical.sh` reports
  separately) is unmeasured here.
- **The `~2` shortfall's cause is not re-diagnosed here.** `doc/PHASES.md`'s Z5 row
  already attributes the `k=2` cost to fanout (dictionary terms within distance 2,
  and the postings unioned across them) rather than automaton stepping, based on a
  dev-box read; this run does not re-derive that diagnosis, only the for-the-record
  latency number.
