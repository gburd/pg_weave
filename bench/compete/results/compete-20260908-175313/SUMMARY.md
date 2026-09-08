## Corpus identity gate

PASS -- all 3 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

**FAIL -- these measurements did not use the expected index and are excluded from the tables below:**

- gin / count_common
- gin / count_not
- gin / count_prefix
- gin / ranked_common_k10
- gin / ranked_common_k100

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | gin | textsearch | weave |
|---|---|---|---|
| ranked_rare_k10 | 78.040 [77.650–78.566] p99 82.848 (n=200, cv=0.02) rows=9971 | 1.255 [1.254–1.257] p99 1.289 (n=200, cv=0.01) | 2.921 [2.919–2.924] p99 2.955 (n=200, cv=0.00) rows=9971 |
| ranked_rare_k100 | 78.461 [77.959–79.362] p99 83.296 (n=200, cv=0.02) rows=9971 | 8.759 [8.757–8.763] p99 8.813 (n=200, cv=0.00) | 3.543 [3.538–3.546] p99 3.576 (n=200, cv=0.00) rows=9971 |
| bare_orderby_rare | 2387.689 [2386.628–2388.934] p99 2403.226 (n=200, cv=0.00) | — | 2.926 [2.925–2.932] p99 2.971 (n=200, cv=0.01) |
| ranked_mid_k10 | 292.985 [291.989–293.994] p99 304.732 (n=200, cv=0.02) rows=39683 | 1.635 [1.631–1.638] p99 1.666 (n=200, cv=0.01) | 10.478 [10.472–10.482] p99 10.550 (n=200, cv=0.00) rows=39683 |
| ranked_mid_k100 | 114.075 [113.751–114.298] p99 120.876 (n=200, cv=0.02) rows=39683 | 9.163 [9.160–9.166] p99 9.955 (n=200, cv=0.02) | 11.076 [11.071–11.081] p99 11.170 (n=200, cv=0.00) rows=39683 |
| bare_orderby_mid | 2388.136 [2386.746–2389.496] p99 2401.974 (n=200, cv=0.00) | — | 10.474 [10.466–10.479] p99 10.588 (n=200, cv=0.00) |
| ranked_common_k10 | PLAN FAIL | 3.151 [3.149–3.152] p99 3.188 (n=200, cv=0.00) | 15.206 [15.200–15.212] p99 15.261 (n=200, cv=0.00) rows=1741439 |
| ranked_common_k100 | PLAN FAIL | 14.940 [14.933–14.952] p99 15.472 (n=200, cv=0.01) | 15.828 [15.825–15.832] p99 15.880 (n=200, cv=0.00) rows=1741439 |
| bare_orderby_common | 2437.055 [2436.221–2438.273] p99 2455.018 (n=200, cv=0.00) | — | 15.234 [15.230–15.241] p99 15.308 (n=200, cv=0.00) |
| count_common | PLAN FAIL | — | 2.075 [2.047–2.120] p99 2.305 (n=200, cv=0.04) rows=1741439 |
| count_and | 1.032 [1.030–1.035] p99 1.077 (n=200, cv=0.05) rows=212 | — | 0.827 [0.826–0.828] p99 0.859 (n=200, cv=0.01) rows=212 |
| count_or2 | 44.428 [44.141–44.904] p99 53.221 (n=200, cv=0.07) | — | 1.414 [1.413–1.415] p99 1.439 (n=200, cv=0.01) |
| count_not | PLAN FAIL | — | 66.175 [66.080–66.297] p99 68.263 (n=200, cv=0.01) |
| count_prefix | PLAN FAIL | — | 760.323 [760.154–760.501] p99 764.444 (n=200, cv=0.00) |
| count_rare_marker | 1.299 [1.296–1.302] p99 1.335 (n=200, cv=0.01) rows=2000 | — | 2.054 [2.048–2.085] p99 2.344 (n=200, cv=0.04) rows=2000 |
| wandk004_rare_k10 | — | — | 2.666 [2.664–2.669] p99 2.700 (n=200, cv=0.00) |
| wandk004_rare_k100 | — | — | 7.072 [7.067–7.075] p99 7.340 (n=200, cv=0.01) |
| wandk004_mid_k10 | — | — | 9.980 [9.978–9.984] p99 10.052 (n=200, cv=0.00) |
| wandk004_mid_k100 | — | — | 22.004 [21.998–22.012] p99 22.096 (n=200, cv=0.00) |
| wandk004_common_k10 | — | — | 11.841 [11.836–11.843] p99 11.901 (n=200, cv=0.00) |
| wandk004_common_k100 | — | — | 35.755 [35.748–35.760] p99 35.957 (n=200, cv=0.00) |
| wandk008_rare_k10 | — | — | 2.665 [2.663–2.668] p99 2.704 (n=200, cv=0.00) |
| wandk008_rare_k100 | — | — | 7.041 [7.038–7.043] p99 7.078 (n=200, cv=0.00) |
| wandk008_mid_k10 | — | — | 9.978 [9.975–9.982] p99 10.058 (n=200, cv=0.00) |
| wandk008_mid_k100 | — | — | 21.968 [21.963–21.973] p99 22.034 (n=200, cv=0.00) |
| wandk008_common_k10 | — | — | 11.846 [11.843–11.851] p99 11.910 (n=200, cv=0.00) |
| wandk008_common_k100 | — | — | 35.970 [35.962–35.976] p99 36.048 (n=200, cv=0.00) |
| wandk016_rare_k10 | — | — | 2.673 [2.669–2.679] p99 2.735 (n=200, cv=0.01) |
| wandk016_rare_k100 | — | — | 7.063 [7.059–7.066] p99 7.124 (n=200, cv=0.00) |
| wandk016_mid_k10 | — | — | 9.970 [9.966–9.976] p99 10.041 (n=200, cv=0.00) |
| wandk016_mid_k100 | — | — | 21.951 [21.944–21.957] p99 22.009 (n=200, cv=0.00) |
| wandk016_common_k10 | — | — | 11.848 [11.843–11.852] p99 11.898 (n=200, cv=0.00) |
| wandk016_common_k100 | — | — | 35.768 [35.760–35.775] p99 35.927 (n=200, cv=0.00) |
| wandk032_rare_k10 | — | — | 2.916 [2.914–2.920] p99 2.955 (n=200, cv=0.01) |
| wandk032_rare_k100 | — | — | 3.546 [3.542–3.548] p99 3.582 (n=200, cv=0.00) |
| wandk032_mid_k10 | — | — | 10.448 [10.444–10.452] p99 10.516 (n=200, cv=0.00) |
| wandk032_mid_k100 | — | — | 11.037 [11.032–11.042] p99 11.128 (n=200, cv=0.00) |
| wandk032_common_k10 | — | — | 15.243 [15.239–15.249] p99 15.311 (n=200, cv=0.00) |
| wandk032_common_k100 | — | — | 15.841 [15.835–15.846] p99 15.899 (n=200, cv=0.00) |
| wandk064_rare_k10 | — | — | 3.820 [3.816–3.824] p99 3.853 (n=200, cv=0.00) |
| wandk064_rare_k100 | — | — | 4.439 [4.434–4.443] p99 4.487 (n=200, cv=0.00) |
| wandk064_mid_k10 | — | — | 11.394 [11.390–11.397] p99 11.462 (n=200, cv=0.00) |
| wandk064_mid_k100 | — | — | 12.013 [12.010–12.019] p99 12.092 (n=200, cv=0.00) |
| wandk064_common_k10 | — | — | 22.609 [22.605–22.615] p99 22.715 (n=200, cv=0.00) |
| wandk064_common_k100 | — | — | 23.247 [23.241–23.252] p99 23.372 (n=200, cv=0.00) |
| wandk100_rare_k10 | — | — | 5.615 [5.611–5.617] p99 5.668 (n=200, cv=0.00) |
| wandk100_rare_k100 | — | — | 6.251 [6.246–6.253] p99 6.294 (n=200, cv=0.00) |
| wandk100_mid_k10 | — | — | 12.789 [12.782–12.796] p99 12.882 (n=200, cv=0.01) |
| wandk100_mid_k100 | — | — | 13.425 [13.420–13.430] p99 13.500 (n=200, cv=0.00) |
| wandk100_common_k10 | — | — | 29.081 [29.075–29.085] p99 29.181 (n=200, cv=0.00) |
| wandk100_common_k100 | — | — | 29.673 [29.668–29.678] p99 29.751 (n=200, cv=0.00) |
| wandk200_rare_k10 | — | — | 12.392 [12.389–12.395] p99 12.463 (n=200, cv=0.00) |
| wandk200_rare_k100 | — | — | 13.014 [13.009–13.019] p99 13.087 (n=200, cv=0.00) |
| wandk200_mid_k10 | — | — | 18.456 [18.451–18.459] p99 18.553 (n=200, cv=0.00) |
| wandk200_mid_k100 | — | — | 19.045 [19.040–19.050] p99 19.121 (n=200, cv=0.00) |
| wandk200_common_k10 | — | — | 53.447 [53.439–53.455] p99 53.584 (n=200, cv=0.00) |
| wandk200_common_k100 | — | — | 54.037 [54.031–54.049] p99 54.418 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (1.255) | weave (2.921) | 2.33x | yes |
| ranked_rare_k100 | weave (3.543) | textsearch (8.759) | 2.47x | yes |
| bare_orderby_rare | weave (2.926) | gin (2387.689) | 816.02x | yes |
| ranked_mid_k10 | textsearch (1.635) | weave (10.478) | 6.41x | yes |
| ranked_mid_k100 | textsearch (9.163) | weave (11.076) | 1.21x | yes |
| bare_orderby_mid | weave (10.474) | gin (2388.136) | 228.01x | yes |
| ranked_common_k10 | textsearch (3.151) | weave (15.206) | 4.83x | yes |
| ranked_common_k100 | textsearch (14.940) | weave (15.828) | 1.06x | yes |
| bare_orderby_common | weave (15.234) | gin (2437.055) | 159.97x | yes |
| count_and | weave (0.827) | gin (1.032) | 1.25x | yes |
| count_or2 | weave (1.414) | gin (44.428) | 31.42x | yes |
| count_rare_marker | gin (1.299) | weave (2.054) | 1.58x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| gin | gin/tsvector in postgresql 17.6 | 239.224561305 | 1120 MB | 29 |
| textsearch | pg_textsearch 1.5.0-dev @f940210f | 47.093418871 | 873 MB | 30 |
| weave | pg_weave 0.5.0 | 328.023627821 | 625 MB | 29 |

