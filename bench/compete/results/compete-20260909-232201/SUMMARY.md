## Corpus identity gate

PASS -- all 1 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | weave |
|---|---|
| ranked_rare_k10 | 2.802 [2.800–2.804] p99 2.837 (n=200, cv=0.01) rows=9971 |
| ranked_rare_k100 | 3.402 [3.399–3.405] p99 3.442 (n=200, cv=0.00) rows=9971 |
| bare_orderby_rare | 2.807 [2.803–2.809] p99 2.846 (n=200, cv=0.01) |
| ranked_mid_k10 | 10.077 [10.072–10.084] p99 10.186 (n=200, cv=0.00) rows=39683 |
| ranked_mid_k100 | 10.648 [10.633–10.654] p99 10.814 (n=200, cv=0.01) rows=39683 |
| bare_orderby_mid | 10.022 [10.001–10.036] p99 10.101 (n=200, cv=0.00) |
| ranked_common_k10 | 14.779 [14.775–14.783] p99 14.980 (n=200, cv=0.00) rows=1741439 |
| ranked_common_k100 | 15.462 [15.431–15.627] p99 16.096 (n=200, cv=0.01) rows=1741439 |
| bare_orderby_common | 14.920 [14.897–14.934] p99 15.168 (n=200, cv=0.01) |
| count_common | 2.093 [2.082–2.127] p99 2.341 (n=200, cv=0.04) rows=1741439 |
| count_and | 0.832 [0.824–0.837] p99 0.873 (n=200, cv=0.02) rows=212 |
| count_or2 | 1.421 [1.420–1.423] p99 1.466 (n=200, cv=0.01) |
| count_not | 70.498 [70.405–70.627] p99 72.382 (n=200, cv=0.01) |
| count_prefix | 764.667 [764.451–764.971] p99 767.802 (n=200, cv=0.00) |
| count_rare_marker | 2.140 [2.125–2.151] p99 2.312 (n=200, cv=0.03) rows=2000 |
| wandk004_rare_k10 | 2.570 [2.567–2.572] p99 2.599 (n=200, cv=0.00) |
| wandk004_rare_k100 | 6.772 [6.770–6.776] p99 6.800 (n=200, cv=0.00) |
| wandk004_mid_k10 | 9.613 [9.609–9.617] p99 9.653 (n=200, cv=0.00) |
| wandk004_mid_k100 | 21.001 [20.987–21.013] p99 21.304 (n=200, cv=0.00) |
| wandk004_common_k10 | 11.501 [11.492–11.510] p99 11.716 (n=200, cv=0.02) |
| wandk004_common_k100 | 34.134 [34.114–34.169] p99 35.058 (n=200, cv=0.01) |
| wandk008_rare_k10 | 2.557 [2.555–2.559] p99 2.583 (n=200, cv=0.00) |
| wandk008_rare_k100 | 6.731 [6.727–6.736] p99 6.774 (n=200, cv=0.00) |
| wandk008_mid_k10 | 9.557 [9.550–9.565] p99 9.642 (n=200, cv=0.01) |
| wandk008_mid_k100 | 21.081 [21.070–21.094] p99 21.613 (n=200, cv=0.01) |
| wandk008_common_k10 | 11.455 [11.444–11.464] p99 11.627 (n=200, cv=0.01) |
| wandk008_common_k100 | 34.160 [34.126–34.221] p99 35.093 (n=200, cv=0.01) |
| wandk016_rare_k10 | 2.556 [2.554–2.559] p99 2.590 (n=200, cv=0.01) |
| wandk016_rare_k100 | 6.752 [6.750–6.756] p99 6.806 (n=200, cv=0.00) |
| wandk016_mid_k10 | 9.544 [9.538–9.553] p99 9.623 (n=200, cv=0.00) |
| wandk016_mid_k100 | 21.247 [21.208–21.275] p99 21.545 (n=200, cv=0.01) |
| wandk016_common_k10 | 11.403 [11.401–11.406] p99 11.456 (n=200, cv=0.00) |
| wandk016_common_k100 | 34.039 [34.021–34.052] p99 34.561 (n=200, cv=0.00) |
| wandk032_rare_k10 | 2.804 [2.802–2.807] p99 2.836 (n=200, cv=0.00) |
| wandk032_rare_k100 | 3.397 [3.396–3.401] p99 3.433 (n=200, cv=0.00) |
| wandk032_mid_k10 | 10.033 [10.026–10.039] p99 10.200 (n=200, cv=0.01) |
| wandk032_mid_k100 | 10.616 [10.598–10.633] p99 10.990 (n=200, cv=0.01) |
| wandk032_common_k10 | 14.892 [14.863–14.922] p99 15.135 (n=200, cv=0.01) |
| wandk032_common_k100 | 15.383 [15.379–15.388] p99 15.443 (n=200, cv=0.00) |
| wandk064_rare_k10 | 3.658 [3.655–3.660] p99 3.688 (n=200, cv=0.00) |
| wandk064_rare_k100 | 4.252 [4.247–4.254] p99 4.303 (n=200, cv=0.01) |
| wandk064_mid_k10 | 10.862 [10.856–10.872] p99 10.952 (n=200, cv=0.00) |
| wandk064_mid_k100 | 11.596 [11.590–11.604] p99 11.757 (n=200, cv=0.01) |
| wandk064_common_k10 | 22.032 [22.022–22.041] p99 22.532 (n=200, cv=0.01) |
| wandk064_common_k100 | 22.672 [22.665–22.681] p99 22.998 (n=200, cv=0.00) |
| wandk100_rare_k10 | 5.355 [5.353–5.360] p99 5.391 (n=200, cv=0.00) |
| wandk100_rare_k100 | 5.976 [5.972–5.979] p99 6.022 (n=200, cv=0.00) |
| wandk100_mid_k10 | 12.218 [12.207–12.228] p99 12.351 (n=200, cv=0.00) |
| wandk100_mid_k100 | 12.853 [12.842–12.861] p99 13.109 (n=200, cv=0.01) |
| wandk100_common_k10 | 28.767 [28.609–28.796] p99 28.990 (n=200, cv=0.01) |
| wandk100_common_k100 | 28.996 [28.982–29.010] p99 29.676 (n=200, cv=0.01) |
| wandk200_rare_k10 | 11.934 [11.925–11.942] p99 12.050 (n=200, cv=0.00) |
| wandk200_rare_k100 | 12.523 [12.512–12.537] p99 12.707 (n=200, cv=0.00) |
| wandk200_mid_k10 | 17.715 [17.707–17.726] p99 17.819 (n=200, cv=0.00) |
| wandk200_mid_k100 | 18.359 [18.354–18.367] p99 18.615 (n=200, cv=0.01) |
| wandk200_common_k10 | 52.468 [52.419–52.520] p99 52.887 (n=200, cv=0.00) |
| wandk200_common_k100 | 53.147 [53.087–53.211] p99 53.813 (n=200, cv=0.01) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| weave | pg_weave 0.5.0 | 274.598026384 | 625 MB | 29 |

