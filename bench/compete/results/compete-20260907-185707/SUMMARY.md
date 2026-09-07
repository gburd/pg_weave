## Corpus identity gate

PASS -- all 2 engines report fingerprint `21f4642d7962c7156449d88caad02822`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | textsearch | weave |
|---|---|---|
| ranked_rare_k10 | 0.308 [0.308–0.309] p99 0.333 (n=200, cv=0.02) | 2.335 [2.333–2.337] p99 2.371 (n=200, cv=0.01) rows=9958 |
| ranked_rare_k100 | 1.258 [1.256–1.260] p99 1.298 (n=200, cv=0.01) | 20.058 [20.054–20.061] p99 20.096 (n=200, cv=0.00) rows=9958 |
| ranked_mid_k10 | 0.628 [0.627–0.628] p99 0.656 (n=200, cv=0.01) | 11.463 [11.460–11.469] p99 11.579 (n=200, cv=0.00) rows=37481 |
| ranked_mid_k100 | 1.686 [1.683–1.690] p99 1.751 (n=200, cv=0.01) | 44.439 [44.421–44.488] p99 45.276 (n=200, cv=0.01) rows=37481 |
| ranked_common_k10 | 2.389 [2.387–2.401] p99 2.564 (n=200, cv=0.03) | 19.017 [19.007–19.032] p99 19.158 (n=200, cv=0.00) rows=358593 |
| ranked_common_k100 | 4.652 [4.647–4.657] p99 4.908 (n=200, cv=0.02) | 81.336 [81.289–81.400] p99 81.844 (n=200, cv=0.01) rows=358593 |
| bare_orderby_rare | — | 2.335 [2.333–2.337] p99 2.364 (n=200, cv=0.00) |
| bare_orderby_mid | — | 11.461 [11.448–11.471] p99 11.648 (n=200, cv=0.00) |
| bare_orderby_common | — | 19.131 [19.113–19.149] p99 19.462 (n=200, cv=0.01) |
| count_common | — | 0.656 [0.655–0.667] p99 0.754 (n=200, cv=0.04) rows=358593 |
| count_and | — | 0.918 [0.917–0.921] p99 0.958 (n=200, cv=0.02) rows=192 |
| count_or2 | — | 1.560 [1.557–1.564] p99 1.597 (n=200, cv=0.01) |
| count_not | — | 14.054 [14.028–14.075] p99 14.861 (n=200, cv=0.01) |
| count_prefix | — | 75.737 [75.702–75.777] p99 76.283 (n=200, cv=0.00) |
| count_rare_marker | — | 0.658 [0.658–0.663] p99 0.757 (n=200, cv=0.04) rows=2000 |
| wandk004_rare_k10 | — | 4.677 [4.672–4.680] p99 4.735 (n=200, cv=0.00) |
| wandk004_rare_k100 | — | 22.391 [22.386–22.397] p99 22.790 (n=200, cv=0.00) |
| wandk004_mid_k10 | — | 22.986 [22.982–22.994] p99 23.278 (n=200, cv=0.00) |
| wandk004_mid_k100 | — | 55.991 [55.972–56.028] p99 57.108 (n=200, cv=0.01) |
| wandk004_common_k10 | — | 37.626 [37.614–37.642] p99 38.285 (n=200, cv=0.00) |
| wandk004_common_k100 | — | 99.494 [99.453–99.567] p99 101.207 (n=200, cv=0.01) |
| wandk008_rare_k10 | — | 5.087 [5.084–5.090] p99 5.136 (n=200, cv=0.00) |
| wandk008_rare_k100 | — | 12.791 [12.784–12.797] p99 13.039 (n=200, cv=0.01) |
| wandk008_mid_k10 | — | 23.177 [23.173–23.182] p99 23.545 (n=200, cv=0.00) |
| wandk008_mid_k100 | — | 37.502 [37.481–37.520] p99 38.226 (n=200, cv=0.01) |
| wandk008_common_k10 | — | 38.422 [38.408–38.435] p99 39.216 (n=200, cv=0.01) |
| wandk008_common_k100 | — | 64.350 [64.338–64.370] p99 65.599 (n=200, cv=0.01) |
| wandk016_rare_k10 | — | 2.342 [2.340–2.343] p99 2.367 (n=200, cv=0.00) |
| wandk016_rare_k100 | — | 20.057 [20.053–20.063] p99 20.348 (n=200, cv=0.00) |
| wandk016_mid_k10 | — | 11.454 [11.450–11.457] p99 11.519 (n=200, cv=0.00) |
| wandk016_mid_k100 | — | 44.230 [44.224–44.240] p99 44.773 (n=200, cv=0.00) |
| wandk016_common_k10 | — | 18.813 [18.805–18.825] p99 19.370 (n=200, cv=0.01) |
| wandk016_common_k100 | — | 80.643 [80.569–80.742] p99 81.940 (n=200, cv=0.01) |
| wandk032_rare_k10 | — | 2.754 [2.753–2.755] p99 2.781 (n=200, cv=0.00) |
| wandk032_rare_k100 | — | 10.468 [10.462–10.488] p99 10.666 (n=200, cv=0.01) |
| wandk032_mid_k10 | — | 11.667 [11.664–11.671] p99 11.845 (n=200, cv=0.00) |
| wandk032_mid_k100 | — | 25.906 [25.885–25.924] p99 26.448 (n=200, cv=0.01) |
| wandk032_common_k10 | — | 19.565 [19.556–19.581] p99 19.982 (n=200, cv=0.01) |
| wandk032_common_k100 | — | 45.715 [45.681–45.785] p99 46.595 (n=200, cv=0.01) |
| wandk064_rare_k10 | — | 4.548 [4.545–4.550] p99 4.603 (n=200, cv=0.01) |
| wandk064_rare_k100 | — | 17.698 [17.691–17.710] p99 18.319 (n=200, cv=0.01) |
| wandk064_mid_k10 | — | 12.431 [12.423–12.442] p99 12.725 (n=200, cv=0.01) |
| wandk064_mid_k100 | — | 33.126 [33.091–33.148] p99 33.524 (n=200, cv=0.01) |
| wandk064_common_k10 | — | 21.413 [21.387–21.444] p99 21.848 (n=200, cv=0.01) |
| wandk064_common_k100 | — | 62.134 [62.044–62.234] p99 62.891 (n=200, cv=0.01) |
| wandk100_rare_k10 | — | 6.268 [6.263–6.277] p99 6.403 (n=200, cv=0.01) |
| wandk100_rare_k100 | — | 6.251 [6.248–6.253] p99 6.423 (n=200, cv=0.01) |
| wandk100_mid_k10 | — | 13.193 [13.186–13.200] p99 13.531 (n=200, cv=0.01) |
| wandk100_mid_k100 | — | 13.235 [13.224–13.244] p99 13.629 (n=200, cv=0.01) |
| wandk100_common_k10 | — | 23.842 [23.822–23.861] p99 24.238 (n=200, cv=0.01) |
| wandk100_common_k100 | — | 23.861 [23.845–23.886] p99 24.093 (n=200, cv=0.00) |
| wandk200_rare_k10 | — | 11.448 [11.434–11.483] p99 11.801 (n=200, cv=0.01) |
| wandk200_rare_k100 | — | 11.470 [11.460–11.482] p99 11.635 (n=200, cv=0.01) |
| wandk200_mid_k10 | — | 17.370 [17.354–17.400] p99 17.716 (n=200, cv=0.01) |
| wandk200_mid_k100 | — | 17.366 [17.344–17.409] p99 17.797 (n=200, cv=0.01) |
| wandk200_common_k10 | — | 33.355 [33.342–33.381] p99 33.899 (n=200, cv=0.00) |
| wandk200_common_k100 | — | 33.380 [33.360–33.397] p99 33.820 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (0.308) | weave (2.335) | 7.58x | yes |
| ranked_rare_k100 | textsearch (1.258) | weave (20.058) | 15.94x | yes |
| ranked_mid_k10 | textsearch (0.628) | weave (11.463) | 18.25x | yes |
| ranked_mid_k100 | textsearch (1.686) | weave (44.439) | 26.36x | yes |
| ranked_common_k10 | textsearch (2.389) | weave (19.017) | 7.96x | yes |
| ranked_common_k100 | textsearch (4.652) | weave (81.336) | 17.48x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| textsearch | pg_textsearch 1.5.0-dev @f940210f | 8.590521508 | 103 MB | 30 |
| weave | pg_weave 0.4.0 | 55.577627655 | 79 MB | 29 |

