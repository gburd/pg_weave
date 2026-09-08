# Result: where CREATE INDEX spends its time

Date: 2026-09-08. `bench/build_profile.sh` via `DO_PROFILE=1 bench/compete/orchestrate.sh`.
Corpus `synth-2m-long` (2,000,000 docs, 120.1 words/doc), `r6id.4xlarge`,
PostgreSQL 17.6 built from source with `-O2`, unstripped.

## Why this exists

After L12, `doc/GAPS.md` recorded G5 (build time) as **"no further route identified"**.
That was asserted **without a profile** — precisely the mistake `doc/PHASES.md` warns
about for L9. This is the attribution, and it overturns the claim.

## Phase accounting

Three builds on the same heap, differences isolating the optional work so the heap
scan and analysis cost cancels:

| phase | time |
|---|---:|
| `positions=on` cost (B − A) | 34.2 s |
| `trigrams=on` cost (C − A) | 12.0 s |
| **irreducible default core (A)** | **349.6 s** |

Both optional features are cheap. **The default path is essentially the entire cost**,
so G5 has to attack A or nothing.

## Symbol profile of the default build

```
37.51%  hash_search_with_hash_value
        |--17.44%-- weave_merge_segments_streaming  (the merge)
        |--13.62%-- add_posting <- weave_build_callback <- heapam_index_build_range_scan
```

**Over a third of build time is PostgreSQL dynahash lookups.** Nothing else comes
close.

## Root cause, and it is not subtle

`src/am/am.c`:

```c
#define WEAVE_TERMKEYLEN 64
typedef struct TermKey { char key[WEAVE_TERMKEYLEN]; } TermKey;

ctl.keysize = sizeof(TermKey);            /* 64 bytes, always */
build_ht = hash_create("weave build terms", 1024, &ctl,
                       HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
```

`HASH_BLOBS` on a fixed 64-byte key means every lookup hashes **all 64 bytes** and
compares **all 64 bytes**, regardless of the term's actual length. `make_termkey`
zero-pads.

The call count is what makes it dominate: `add_posting` runs once per **term
occurrence**, so at 2,000,000 documents × ~120 words that is roughly **240 million
lookups**, each hashing and memcmp-ing 64 bytes where a typical English term needs
about 10. That is ~6× more hashing than the data requires, plus dynahash's
bucket-chain pointer chasing on every probe.

The merge path pays the same cost again, once per term per segment.

## So G5 has a route after all — two, in fact

Recorded as new task **L15**, and the claim in `doc/GAPS.md` is corrected.

1. **Hash and compare only the real term length.** Store the length in the key and
   supply `HASH_FUNCTION` + `HASH_COMPARE` that respect it. Contained, roughly twenty
   lines, and it cuts both the hash input and the `memcmp` from 64 bytes to the actual
   length. This is the cheap one and should be measured first.

2. **Replace dynahash with `lib/simplehash.h`.** PostgreSQL's own open-addressing
   template, used by HashAgg and others; no per-entry pointer chasing. Typically
   2–3× faster than dynahash for lookup-heavy loops. Larger change, and it should
   only be attempted after (1) shows how much of the 37.5% is hashing versus
   probing.

An upper bound worth stating plainly: eliminating **all** of the hash cost would take
the build from 328 s to about 205 s, which is 4.4× behind pg_textsearch rather than
6.96×. So this narrows G5 substantially but does not close it, and the remaining 205 s
is still unattributed beyond "not hashing".

## What this does not tell us

- **One profile, one corpus, one configuration.** 120 words/doc synthetic text; the
  term-length distribution of real prose differs and the 64-vs-10-byte ratio with it.
- **The remaining 62.5% is not attributed.** The profile was truncated at the top
  symbol; sort, WAL, and page I/O have not been separated.
- **`perf --call-graph dwarf` at 199 Hz** over a 60-second window of a 350-second
  build — it samples the middle of the build, not the whole of it, so a phase that
  only runs at the start or end is under-represented.
- **No confidence interval.** Build time was measured once per configuration.
