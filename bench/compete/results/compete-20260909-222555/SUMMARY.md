## Corpus identity gate

PASS -- all 1 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | weave |
|---|---|
| ranked_rare_k10 | 2.830 [2.828–2.833] p99 2.870 (n=200, cv=0.00) rows=9971 |
| ranked_rare_k100 | 3.445 [3.444–3.450] p99 3.504 (n=200, cv=0.01) rows=9971 |
| bare_orderby_rare | 2.837 [2.835–2.841] p99 2.878 (n=200, cv=0.01) |
| ranked_mid_k10 | 10.174 [10.169–10.184] p99 10.445 (n=200, cv=0.01) rows=39683 |
| ranked_mid_k100 | 10.803 [10.800–10.809] p99 11.247 (n=200, cv=0.01) rows=39683 |
| bare_orderby_mid | 10.253 [10.244–10.265] p99 10.466 (n=200, cv=0.01) |
| ranked_common_k10 | 14.986 [14.961–15.004] p99 15.211 (n=200, cv=0.01) rows=1741439 |
| ranked_common_k100 | 15.863 [15.833–15.910] p99 16.239 (n=200, cv=0.01) rows=1741439 |
| bare_orderby_common | 14.830 [14.808–14.872] p99 15.282 (n=200, cv=0.01) |
| count_common | 2.078 [2.058–2.113] p99 2.341 (n=200, cv=0.04) rows=1741439 |
| count_and | 0.820 [0.820–0.821] p99 0.860 (n=200, cv=0.01) rows=212 |
| count_or2 | 1.426 [1.421–1.430] p99 1.472 (n=200, cv=0.01) |
| count_not | 72.185 [72.058–72.356] p99 74.090 (n=200, cv=0.01) |
| count_prefix | 763.752 [763.603–764.014] p99 768.058 (n=200, cv=0.00) |
| count_rare_marker | 2.096 [2.066–2.121] p99 2.326 (n=200, cv=0.04) rows=2000 |
| wandk004_rare_k10 | 2.595 [2.592–2.597] p99 2.640 (n=200, cv=0.01) |
| wandk004_rare_k100 | 6.898 [6.896–6.903] p99 6.991 (n=200, cv=0.00) |
| wandk004_mid_k10 | 9.842 [9.823–9.858] p99 10.030 (n=200, cv=0.01) |
| wandk004_mid_k100 | 21.745 [21.716–21.773] p99 22.255 (n=200, cv=0.01) |
| wandk004_common_k10 | 11.528 [11.502–11.544] p99 11.879 (n=200, cv=0.01) |
| wandk004_common_k100 | 34.976 [34.926–35.062] p99 35.449 (n=200, cv=0.01) |
| wandk008_rare_k10 | 2.580 [2.579–2.583] p99 2.609 (n=200, cv=0.00) |
| wandk008_rare_k100 | 6.902 [6.897–6.907] p99 7.059 (n=200, cv=0.01) |
| wandk008_mid_k10 | 9.730 [9.719–9.741] p99 9.861 (n=200, cv=0.01) |
| wandk008_mid_k100 | 21.689 [21.642–21.710] p99 22.070 (n=200, cv=0.01) |
| wandk008_common_k10 | 11.662 [11.635–11.675] p99 11.872 (n=200, cv=0.01) |
| wandk008_common_k100 | 34.899 [34.809–35.004] p99 35.503 (n=200, cv=0.01) |
| wandk016_rare_k10 | 2.587 [2.585–2.589] p99 2.618 (n=200, cv=0.01) |
| wandk016_rare_k100 | 6.937 [6.933–6.941] p99 7.039 (n=200, cv=0.01) |
| wandk016_mid_k10 | 9.781 [9.767–9.795] p99 9.947 (n=200, cv=0.01) |
| wandk016_mid_k100 | 21.733 [21.697–21.776] p99 22.204 (n=200, cv=0.01) |
| wandk016_common_k10 | 11.398 [11.388–11.413] p99 11.816 (n=200, cv=0.01) |
| wandk016_common_k100 | 34.790 [34.704–34.866] p99 35.471 (n=200, cv=0.01) |
| wandk032_rare_k10 | 2.827 [2.826–2.831] p99 2.863 (n=200, cv=0.01) |
| wandk032_rare_k100 | 3.457 [3.456–3.460] p99 3.489 (n=200, cv=0.00) |
| wandk032_mid_k10 | 10.227 [10.214–10.253] p99 10.419 (n=200, cv=0.01) |
| wandk032_mid_k100 | 10.820 [10.812–10.830] p99 11.181 (n=200, cv=0.01) |
| wandk032_common_k10 | 14.908 [14.879–14.931] p99 15.252 (n=200, cv=0.01) |
| wandk032_common_k100 | 15.943 [15.924–15.973] p99 16.317 (n=200, cv=0.02) |
| wandk064_rare_k10 | 3.708 [3.705–3.710] p99 3.748 (n=200, cv=0.00) |
| wandk064_rare_k100 | 4.346 [4.341–4.351] p99 4.408 (n=200, cv=0.01) |
| wandk064_mid_k10 | 11.115 [11.102–11.120] p99 11.297 (n=200, cv=0.01) |
| wandk064_mid_k100 | 11.772 [11.759–11.788] p99 12.139 (n=200, cv=0.01) |
| wandk064_common_k10 | 22.344 [22.281–22.395] p99 22.666 (n=200, cv=0.01) |
| wandk064_common_k100 | 23.380 [23.348–23.404] p99 23.581 (n=200, cv=0.01) |
| wandk100_rare_k10 | 5.444 [5.441–5.447] p99 5.499 (n=200, cv=0.00) |
| wandk100_rare_k100 | 6.093 [6.087–6.099] p99 6.387 (n=200, cv=0.01) |
| wandk100_mid_k10 | 12.603 [12.586–12.618] p99 12.752 (n=200, cv=0.01) |
| wandk100_mid_k100 | 13.260 [13.209–13.284] p99 13.626 (n=200, cv=0.01) |
| wandk100_common_k10 | 28.883 [28.841–28.909] p99 29.100 (n=200, cv=0.01) |
| wandk100_common_k100 | 29.686 [29.654–29.708] p99 29.933 (n=200, cv=0.01) |
| wandk200_rare_k10 | 12.217 [12.182–12.244] p99 12.463 (n=200, cv=0.01) |
| wandk200_rare_k100 | 12.756 [12.746–12.767] p99 13.259 (n=200, cv=0.01) |
| wandk200_mid_k10 | 18.177 [18.160–18.195] p99 18.318 (n=200, cv=0.00) |
| wandk200_mid_k100 | 19.045 [19.021–19.064] p99 19.258 (n=200, cv=0.01) |
| wandk200_common_k10 | 52.877 [52.864–52.888] p99 53.091 (n=200, cv=0.00) |
| wandk200_common_k100 | 53.764 [53.746–53.781] p99 54.279 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| weave | pg_weave 0.5.0 | 340.86066283 | 625 MB | 29 |

