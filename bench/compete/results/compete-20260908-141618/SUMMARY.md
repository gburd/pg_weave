## Corpus identity gate

**FAIL -- no engine reported any data. There is nothing to compare; check the per-engine logs.**

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query |  |
|---|

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|

