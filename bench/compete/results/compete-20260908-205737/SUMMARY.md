## Corpus identity gate

PASS -- all 1 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | weave |
|---|---|
| ranked_rare_k10 | 2.920 [2.914–2.923] p99 2.964 (n=200, cv=0.01) rows=9971 |
| ranked_rare_k100 | 3.550 [3.547–3.555] p99 3.596 (n=200, cv=0.01) rows=9971 |
| bare_orderby_rare | 2.937 [2.933–2.939] p99 2.968 (n=200, cv=0.01) |
| ranked_mid_k10 | 10.459 [10.439–10.485] p99 10.759 (n=200, cv=0.01) rows=39683 |
| ranked_mid_k100 | 11.097 [11.074–11.121] p99 11.277 (n=200, cv=0.01) rows=39683 |
| bare_orderby_mid | 10.536 [10.533–10.542] p99 10.710 (n=200, cv=0.01) |
| ranked_common_k10 | 15.494 [15.454–15.534] p99 16.078 (n=200, cv=0.01) rows=1741439 |
| ranked_common_k100 | 16.360 [16.323–16.413] p99 16.897 (n=200, cv=0.01) rows=1741439 |
| bare_orderby_common | 15.456 [15.442–15.469] p99 15.640 (n=200, cv=0.01) |
| count_common | 2.092 [2.056–2.126] p99 2.341 (n=200, cv=0.04) rows=1741439 |
| count_and | 0.827 [0.826–0.828] p99 0.850 (n=200, cv=0.01) rows=212 |
| count_or2 | 1.403 [1.398–1.405] p99 1.430 (n=200, cv=0.01) |
| count_not | 71.586 [71.490–71.677] p99 73.247 (n=200, cv=0.01) |
| count_prefix | 764.748 [764.411–765.075] p99 768.990 (n=200, cv=0.00) |
| count_rare_marker | 2.073 [2.055–2.098] p99 2.301 (n=200, cv=0.03) rows=2000 |
| wandk004_rare_k10 | 2.667 [2.663–2.672] p99 2.720 (n=200, cv=0.01) |
| wandk004_rare_k100 | 7.074 [7.069–7.083] p99 7.191 (n=200, cv=0.01) |
| wandk004_mid_k10 | 10.051 [10.043–10.064] p99 10.230 (n=200, cv=0.01) |
| wandk004_mid_k100 | 22.401 [22.387–22.425] p99 22.627 (n=200, cv=0.00) |
| wandk004_common_k10 | 12.263 [12.239–12.282] p99 12.597 (n=200, cv=0.01) |
| wandk004_common_k100 | 36.323 [36.288–36.345] p99 36.716 (n=200, cv=0.01) |
| wandk008_rare_k10 | 2.667 [2.664–2.671] p99 2.712 (n=200, cv=0.01) |
| wandk008_rare_k100 | 7.153 [7.147–7.159] p99 7.279 (n=200, cv=0.01) |
| wandk008_mid_k10 | 10.102 [10.091–10.111] p99 10.227 (n=200, cv=0.01) |
| wandk008_mid_k100 | 22.411 [22.400–22.424] p99 22.588 (n=200, cv=0.00) |
| wandk008_common_k10 | 12.297 [12.282–12.325] p99 12.625 (n=200, cv=0.01) |
| wandk008_common_k100 | 36.310 [36.262–36.340] p99 36.690 (n=200, cv=0.01) |
| wandk016_rare_k10 | 2.672 [2.669–2.676] p99 2.719 (n=200, cv=0.01) |
| wandk016_rare_k100 | 7.120 [7.113–7.127] p99 7.240 (n=200, cv=0.01) |
| wandk016_mid_k10 | 10.077 [10.067–10.089] p99 10.236 (n=200, cv=0.01) |
| wandk016_mid_k100 | 22.501 [22.480–22.518] p99 22.674 (n=200, cv=0.00) |
| wandk016_common_k10 | 12.272 [12.255–12.288] p99 12.622 (n=200, cv=0.01) |
| wandk016_common_k100 | 36.305 [36.273–36.324] p99 36.783 (n=200, cv=0.01) |
| wandk032_rare_k10 | 2.917 [2.914–2.921] p99 2.953 (n=200, cv=0.01) |
| wandk032_rare_k100 | 3.524 [3.521–3.527] p99 3.580 (n=200, cv=0.01) |
| wandk032_mid_k10 | 10.531 [10.519–10.545] p99 10.671 (n=200, cv=0.01) |
| wandk032_mid_k100 | 11.493 [11.474–11.506] p99 11.668 (n=200, cv=0.01) |
| wandk032_common_k10 | 15.687 [15.678–15.710] p99 16.185 (n=200, cv=0.01) |
| wandk032_common_k100 | 16.637 [16.609–16.675] p99 17.169 (n=200, cv=0.01) |
| wandk064_rare_k10 | 3.819 [3.814–3.823] p99 3.865 (n=200, cv=0.01) |
| wandk064_rare_k100 | 4.449 [4.445–4.452] p99 4.491 (n=200, cv=0.00) |
| wandk064_mid_k10 | 11.520 [11.513–11.531] p99 11.644 (n=200, cv=0.00) |
| wandk064_mid_k100 | 12.457 [12.451–12.466] p99 12.591 (n=200, cv=0.00) |
| wandk064_common_k10 | 23.184 [23.171–23.200] p99 23.983 (n=200, cv=0.02) |
| wandk064_common_k100 | 24.114 [24.100–24.132] p99 24.936 (n=200, cv=0.02) |
| wandk100_rare_k10 | 5.634 [5.629–5.638] p99 5.680 (n=200, cv=0.00) |
| wandk100_rare_k100 | 6.335 [6.326–6.342] p99 6.456 (n=200, cv=0.01) |
| wandk100_mid_k10 | 12.953 [12.947–12.963] p99 13.106 (n=200, cv=0.00) |
| wandk100_mid_k100 | 13.848 [13.831–13.858] p99 13.989 (n=200, cv=0.00) |
| wandk100_common_k10 | 29.650 [29.634–29.669] p99 30.599 (n=200, cv=0.01) |
| wandk100_common_k100 | 30.558 [30.548–30.572] p99 31.511 (n=200, cv=0.01) |
| wandk200_rare_k10 | 12.532 [12.522–12.543] p99 12.638 (n=200, cv=0.00) |
| wandk200_rare_k100 | 13.487 [13.462–13.514] p99 13.757 (n=200, cv=0.01) |
| wandk200_mid_k10 | 18.581 [18.569–18.598] p99 19.099 (n=200, cv=0.01) |
| wandk200_mid_k100 | 19.518 [19.510–19.529] p99 19.640 (n=200, cv=0.00) |
| wandk200_common_k10 | 53.960 [53.947–53.974] p99 55.323 (n=200, cv=0.01) |
| wandk200_common_k100 | 54.935 [54.922–54.948] p99 56.311 (n=200, cv=0.01) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| weave | pg_weave 0.5.0 | 353.812861752 | 625 MB | 29 |

