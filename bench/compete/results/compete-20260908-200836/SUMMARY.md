## Corpus identity gate

PASS -- all 1 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | weave |
|---|---|
| ranked_rare_k10 | 2.937 [2.933–2.942] p99 2.989 (n=200, cv=0.01) rows=9971 |
| ranked_rare_k100 | 3.553 [3.548–3.556] p99 3.603 (n=200, cv=0.01) rows=9971 |
| bare_orderby_rare | 2.942 [2.940–2.945] p99 3.022 (n=200, cv=0.01) |
| ranked_mid_k10 | 10.660 [10.643–10.674] p99 10.820 (n=200, cv=0.01) rows=39683 |
| ranked_mid_k100 | 11.509 [11.498–11.524] p99 11.754 (n=200, cv=0.01) rows=39683 |
| bare_orderby_mid | 10.664 [10.645–10.672] p99 10.799 (n=200, cv=0.01) |
| ranked_common_k10 | 15.742 [15.735–15.751] p99 15.857 (n=200, cv=0.00) rows=1741439 |
| ranked_common_k100 | 16.546 [16.525–16.565] p99 17.038 (n=200, cv=0.02) rows=1741439 |
| bare_orderby_common | 15.573 [15.538–15.604] p99 15.786 (n=200, cv=0.01) |
| count_common | 2.089 [2.067–2.120] p99 2.341 (n=200, cv=0.04) rows=1741439 |
| count_and | 0.823 [0.823–0.824] p99 0.860 (n=200, cv=0.01) rows=212 |
| count_or2 | 1.399 [1.393–1.401] p99 1.428 (n=200, cv=0.01) |
| count_not | 71.667 [71.584–71.772] p99 72.867 (n=200, cv=0.01) |
| count_prefix | 766.163 [765.968–766.438] p99 770.567 (n=200, cv=0.00) |
| count_rare_marker | 2.079 [2.059–2.107] p99 2.301 (n=200, cv=0.04) rows=2000 |
| wandk004_rare_k10 | 2.667 [2.665–2.673] p99 2.719 (n=200, cv=0.01) |
| wandk004_rare_k100 | 7.262 [7.250–7.269] p99 7.360 (n=200, cv=0.01) |
| wandk004_mid_k10 | 10.105 [10.093–10.120] p99 10.278 (n=200, cv=0.01) |
| wandk004_mid_k100 | 22.597 [22.585–22.625] p99 22.874 (n=200, cv=0.01) |
| wandk004_common_k10 | 12.323 [12.314–12.335] p99 12.459 (n=200, cv=0.00) |
| wandk004_common_k100 | 36.371 [36.161–36.425] p99 36.589 (n=200, cv=0.01) |
| wandk008_rare_k10 | 2.654 [2.650–2.659] p99 2.710 (n=200, cv=0.01) |
| wandk008_rare_k100 | 7.094 [7.088–7.103] p99 7.247 (n=200, cv=0.01) |
| wandk008_mid_k10 | 10.036 [10.014–10.058] p99 10.261 (n=200, cv=0.01) |
| wandk008_mid_k100 | 22.366 [22.331–22.418] p99 22.828 (n=200, cv=0.01) |
| wandk008_common_k10 | 12.108 [12.089–12.128] p99 12.358 (n=200, cv=0.01) |
| wandk008_common_k100 | 36.350 [36.323–36.376] p99 36.552 (n=200, cv=0.01) |
| wandk016_rare_k10 | 2.679 [2.671–2.684] p99 2.728 (n=200, cv=0.01) |
| wandk016_rare_k100 | 7.155 [7.147–7.162] p99 7.274 (n=200, cv=0.01) |
| wandk016_mid_k10 | 10.109 [10.097–10.123] p99 10.294 (n=200, cv=0.01) |
| wandk016_mid_k100 | 22.610 [22.593–22.636] p99 22.806 (n=200, cv=0.01) |
| wandk016_common_k10 | 12.245 [12.234–12.257] p99 12.420 (n=200, cv=0.01) |
| wandk016_common_k100 | 36.337 [36.322–36.364] p99 36.518 (n=200, cv=0.01) |
| wandk032_rare_k10 | 2.918 [2.913–2.923] p99 2.975 (n=200, cv=0.01) |
| wandk032_rare_k100 | 3.563 [3.558–3.570] p99 3.618 (n=200, cv=0.01) |
| wandk032_mid_k10 | 10.481 [10.468–10.497] p99 10.699 (n=200, cv=0.01) |
| wandk032_mid_k100 | 11.158 [11.117–11.179] p99 11.476 (n=200, cv=0.01) |
| wandk032_common_k10 | 15.539 [15.507–15.594] p99 15.753 (n=200, cv=0.01) |
| wandk032_common_k100 | 16.603 [16.573–16.622] p99 16.755 (n=200, cv=0.01) |
| wandk064_rare_k10 | 3.825 [3.819–3.832] p99 3.877 (n=200, cv=0.01) |
| wandk064_rare_k100 | 4.447 [4.440–4.452] p99 4.524 (n=200, cv=0.01) |
| wandk064_mid_k10 | 11.551 [11.535–11.567] p99 11.744 (n=200, cv=0.01) |
| wandk064_mid_k100 | 12.395 [12.360–12.430] p99 12.678 (n=200, cv=0.01) |
| wandk064_common_k10 | 23.213 [23.207–23.218] p99 23.297 (n=200, cv=0.00) |
| wandk064_common_k100 | 24.147 [24.138–24.160] p99 24.261 (n=200, cv=0.07) |
| wandk100_rare_k10 | 5.647 [5.642–5.654] p99 5.733 (n=200, cv=0.01) |
| wandk100_rare_k100 | 6.369 [6.361–6.378] p99 6.490 (n=200, cv=0.01) |
| wandk100_mid_k10 | 12.993 [12.985–13.005] p99 13.158 (n=200, cv=0.01) |
| wandk100_mid_k100 | 13.785 [13.753–13.817] p99 14.091 (n=200, cv=0.01) |
| wandk100_common_k10 | 29.574 [29.559–29.601] p99 29.784 (n=200, cv=0.00) |
| wandk100_common_k100 | 30.559 [30.545–30.576] p99 30.703 (n=200, cv=0.00) |
| wandk200_rare_k10 | 12.626 [12.620–12.641] p99 12.849 (n=200, cv=0.01) |
| wandk200_rare_k100 | 13.471 [13.443–13.492] p99 13.712 (n=200, cv=0.01) |
| wandk200_mid_k10 | 18.626 [18.613–18.636] p99 18.818 (n=200, cv=0.01) |
| wandk200_mid_k100 | 19.593 [19.584–19.610] p99 19.765 (n=200, cv=0.00) |
| wandk200_common_k10 | 53.958 [53.944–53.980] p99 54.125 (n=200, cv=0.00) |
| wandk200_common_k100 | 54.838 [54.823–54.852] p99 55.041 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| weave | pg_weave 0.5.0 | 356.53575244 | 625 MB | 29 |

