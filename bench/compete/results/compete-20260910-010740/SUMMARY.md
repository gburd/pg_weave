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
| ranked_rare_k10 | 76.864 [76.748–77.081] p99 78.861 (n=200, cv=0.01) rows=9971 | 1.252 [1.250–1.253] p99 1.285 (n=200, cv=0.01) | 2.815 [2.812–2.818] p99 2.873 (n=200, cv=0.01) rows=9971 |
| ranked_rare_k100 | 77.326 [77.146–77.554] p99 79.194 (n=200, cv=0.01) rows=9971 | 8.762 [8.756–8.766] p99 8.897 (n=200, cv=0.01) | 3.446 [3.439–3.452] p99 3.567 (n=200, cv=0.01) rows=9971 |
| bare_orderby_rare | 2380.063 [2379.617–2380.651] p99 2393.858 (n=200, cv=0.00) | — | 2.821 [2.819–2.824] p99 2.862 (n=200, cv=0.01) |
| ranked_mid_k10 | 287.616 [287.422–287.769] p99 292.398 (n=200, cv=0.01) rows=39683 | 1.627 [1.625–1.630] p99 1.665 (n=200, cv=0.01) | 10.259 [10.245–10.270] p99 10.373 (n=200, cv=0.01) rows=39683 |
| ranked_mid_k100 | 113.319 [113.174–113.411] p99 117.747 (n=200, cv=0.01) rows=39683 | 9.197 [9.189–9.203] p99 9.296 (n=200, cv=0.00) | 11.178 [11.163–11.193] p99 11.333 (n=200, cv=0.01) rows=39683 |
| bare_orderby_mid | 2378.744 [2378.095–2379.431] p99 2392.222 (n=200, cv=0.00) | — | 10.261 [10.255–10.273] p99 10.356 (n=200, cv=0.01) |
| ranked_common_k10 | PLAN FAIL | 3.160 [3.157–3.163] p99 3.208 (n=200, cv=0.01) | 15.351 [15.343–15.361] p99 15.456 (n=200, cv=0.00) rows=1741439 |
| ranked_common_k100 | PLAN FAIL | 15.389 [15.374–15.409] p99 15.563 (n=200, cv=0.01) | 16.267 [16.257–16.281] p99 16.391 (n=200, cv=0.00) rows=1741439 |
| bare_orderby_common | 2429.054 [2428.390–2429.710] p99 2445.735 (n=200, cv=0.00) | — | 15.343 [15.333–15.355] p99 15.449 (n=200, cv=0.00) |
| count_common | PLAN FAIL | — | 2.071 [2.051–2.104] p99 2.341 (n=200, cv=0.04) rows=1741439 |
| count_and | 1.046 [1.045–1.047] p99 1.083 (n=200, cv=0.03) rows=212 | — | 0.819 [0.819–0.820] p99 0.853 (n=200, cv=0.01) rows=212 |
| count_or2 | 41.167 [40.370–42.504] p99 52.082 (n=200, cv=0.10) | — | 1.405 [1.403–1.407] p99 1.441 (n=200, cv=0.01) |
| count_not | PLAN FAIL | — | 72.971 [72.881–73.115] p99 74.170 (n=200, cv=0.01) |
| count_prefix | PLAN FAIL | — | 763.255 [762.954–763.485] p99 765.807 (n=200, cv=0.00) |
| count_rare_marker | 1.334 [1.332–1.336] p99 1.371 (n=200, cv=0.01) rows=2000 | — | 2.053 [2.045–2.067] p99 2.200 (n=200, cv=0.03) rows=2000 |
| wandk004_rare_k10 | — | — | 2.559 [2.557–2.562] p99 2.596 (n=200, cv=0.01) |
| wandk004_rare_k100 | — | — | 7.025 [7.010–7.043] p99 7.258 (n=200, cv=0.01) |
| wandk004_mid_k10 | — | — | 9.823 [9.814–9.834] p99 9.929 (n=200, cv=0.01) |
| wandk004_mid_k100 | — | — | 21.889 [21.861–21.906] p99 22.070 (n=200, cv=0.01) |
| wandk004_common_k10 | — | — | 11.973 [11.962–11.988] p99 12.093 (n=200, cv=0.01) |
| wandk004_common_k100 | — | — | 35.833 [35.814–35.847] p99 36.023 (n=200, cv=0.00) |
| wandk008_rare_k10 | — | — | 2.558 [2.555–2.562] p99 2.621 (n=200, cv=0.01) |
| wandk008_rare_k100 | — | — | 7.057 [7.041–7.067] p99 7.271 (n=200, cv=0.01) |
| wandk008_mid_k10 | — | — | 9.723 [9.714–9.735] p99 9.896 (n=200, cv=0.01) |
| wandk008_mid_k100 | — | — | 22.067 [22.032–22.094] p99 22.326 (n=200, cv=0.01) |
| wandk008_common_k10 | — | — | 12.010 [11.998–12.021] p99 12.131 (n=200, cv=0.01) |
| wandk008_common_k100 | — | — | 35.931 [35.919–35.946] p99 36.091 (n=200, cv=0.00) |
| wandk016_rare_k10 | — | — | 2.565 [2.561–2.567] p99 2.609 (n=200, cv=0.01) |
| wandk016_rare_k100 | — | — | 6.963 [6.949–6.985] p99 7.151 (n=200, cv=0.01) |
| wandk016_mid_k10 | — | — | 9.836 [9.819–9.851] p99 9.933 (n=200, cv=0.01) |
| wandk016_mid_k100 | — | — | 21.768 [21.733–21.793] p99 22.069 (n=200, cv=0.01) |
| wandk016_common_k10 | — | — | 11.978 [11.965–11.988] p99 12.163 (n=200, cv=0.01) |
| wandk016_common_k100 | — | — | 35.882 [35.866–35.895] p99 36.067 (n=200, cv=0.00) |
| wandk032_rare_k10 | — | — | 2.814 [2.812–2.819] p99 2.884 (n=200, cv=0.01) |
| wandk032_rare_k100 | — | — | 3.424 [3.420–3.429] p99 3.558 (n=200, cv=0.01) |
| wandk032_mid_k10 | — | — | 10.306 [10.299–10.319] p99 10.397 (n=200, cv=0.01) |
| wandk032_mid_k100 | — | — | 11.185 [11.176–11.199] p99 11.313 (n=200, cv=0.01) |
| wandk032_common_k10 | — | — | 15.349 [15.340–15.357] p99 15.442 (n=200, cv=0.00) |
| wandk032_common_k100 | — | — | 16.311 [16.301–16.325] p99 16.479 (n=200, cv=0.00) |
| wandk064_rare_k10 | — | — | 3.700 [3.696–3.704] p99 3.798 (n=200, cv=0.01) |
| wandk064_rare_k100 | — | — | 4.342 [4.337–4.350] p99 4.458 (n=200, cv=0.01) |
| wandk064_mid_k10 | — | — | 11.207 [11.197–11.223] p99 11.332 (n=200, cv=0.01) |
| wandk064_mid_k100 | — | — | 12.047 [12.031–12.064] p99 12.197 (n=200, cv=0.01) |
| wandk064_common_k10 | — | — | 22.720 [22.706–22.730] p99 22.819 (n=200, cv=0.00) |
| wandk064_common_k100 | — | — | 23.624 [23.614–23.638] p99 23.763 (n=200, cv=0.00) |
| wandk100_rare_k10 | — | — | 5.500 [5.492–5.508] p99 5.669 (n=200, cv=0.01) |
| wandk100_rare_k100 | — | — | 6.289 [6.270–6.309] p99 6.522 (n=200, cv=0.02) |
| wandk100_mid_k10 | — | — | 12.648 [12.642–12.657] p99 12.719 (n=200, cv=0.00) |
| wandk100_mid_k100 | — | — | 13.518 [13.507–13.527] p99 13.652 (n=200, cv=0.00) |
| wandk100_common_k10 | — | — | 29.210 [29.201–29.221] p99 29.296 (n=200, cv=0.00) |
| wandk100_common_k100 | — | — | 30.095 [30.087–30.109] p99 30.224 (n=200, cv=0.00) |
| wandk200_rare_k10 | — | — | 12.476 [12.467–12.485] p99 12.580 (n=200, cv=0.00) |
| wandk200_rare_k100 | — | — | 13.309 [13.299–13.322] p99 13.446 (n=200, cv=0.01) |
| wandk200_mid_k10 | — | — | 18.303 [18.296–18.313] p99 18.383 (n=200, cv=0.00) |
| wandk200_mid_k100 | — | — | 19.218 [19.207–19.228] p99 19.383 (n=200, cv=0.00) |
| wandk200_common_k10 | — | — | 53.377 [53.365–53.385] p99 53.511 (n=200, cv=0.00) |
| wandk200_common_k100 | — | — | 54.349 [54.334–54.360] p99 54.489 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (1.252) | weave (2.815) | 2.25x | yes |
| ranked_rare_k100 | weave (3.446) | textsearch (8.762) | 2.54x | yes |
| bare_orderby_rare | weave (2.821) | gin (2380.063) | 843.69x | yes |
| ranked_mid_k10 | textsearch (1.627) | weave (10.259) | 6.31x | yes |
| ranked_mid_k100 | textsearch (9.197) | weave (11.178) | 1.22x | yes |
| bare_orderby_mid | weave (10.261) | gin (2378.744) | 231.82x | yes |
| ranked_common_k10 | textsearch (3.160) | weave (15.351) | 4.86x | yes |
| ranked_common_k100 | textsearch (15.389) | weave (16.267) | 1.06x | yes |
| bare_orderby_common | weave (15.343) | gin (2429.054) | 158.32x | yes |
| count_and | weave (0.819) | gin (1.046) | 1.28x | yes |
| count_or2 | weave (1.405) | gin (41.167) | 29.30x | yes |
| count_rare_marker | gin (1.334) | weave (2.053) | 1.54x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| gin | gin/tsvector in postgresql 17.6 | 202.656868375 | 1120 MB | 29 |
| textsearch | pg_textsearch 1.5.0-dev @8dabad51 | 49.242678616 | 873 MB | 30 |
| weave | pg_weave 0.5.0 | 192.532875445 | 625 MB | 29 |

