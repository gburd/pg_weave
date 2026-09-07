## Corpus identity gate

PASS -- all 3 engines report fingerprint `21f4642d7962c7156449d88caad02822`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | gin | textsearch | weave |
|---|---|---|---|
| ranked_rare_k10 | 15.766 [15.336–16.076] p99 17.660 (n=200, cv=0.10) rows=9958 | 0.307 [0.307–0.308] p99 0.330 (n=200, cv=0.03) | 2.738 [2.736–2.739] p99 2.780 (n=200, cv=0.01) rows=9958 |
| ranked_rare_k100 | 16.497 [15.907–16.801] p99 18.306 (n=200, cv=0.13) rows=9958 | 1.252 [1.251–1.256] p99 1.344 (n=200, cv=0.01) | 10.417 [10.410–10.434] p99 10.702 (n=200, cv=0.01) rows=9958 |
| bare_orderby_rare | 191.727 [191.591–191.917] p99 194.612 (n=200, cv=0.01) | — | 2.738 [2.736–2.740] p99 2.774 (n=200, cv=0.01) |
| ranked_mid_k10 | 55.993 [55.779–56.119] p99 57.800 (n=200, cv=0.02) rows=37481 | 0.628 [0.627–0.628] p99 0.653 (n=200, cv=0.01) | 11.606 [11.604–11.610] p99 11.729 (n=200, cv=0.00) rows=37481 |
| ranked_mid_k100 | 43.144 [43.029–43.318] p99 45.975 (n=200, cv=0.02) rows=37481 | 1.680 [1.679–1.683] p99 1.711 (n=200, cv=0.01) | 25.767 [25.748–25.819] p99 26.396 (n=200, cv=0.01) rows=37481 |
| bare_orderby_mid | 190.939 [190.810–191.065] p99 192.979 (n=200, cv=0.00) | — | 11.622 [11.617–11.627] p99 11.882 (n=200, cv=0.01) |
| ranked_common_k10 | 159.865 [159.612–160.101] p99 170.552 (n=200, cv=0.02) rows=358593 | 2.401 [2.396–2.403] p99 2.461 (n=200, cv=0.01) | 17.996 [17.990–18.006] p99 18.387 (n=200, cv=0.01) rows=358593 |
| ranked_common_k100 | 161.332 [160.822–161.672] p99 176.976 (n=200, cv=0.03) rows=358593 | 4.611 [4.607–4.614] p99 4.649 (n=200, cv=0.00) | 42.412 [42.386–42.481] p99 43.334 (n=200, cv=0.01) rows=358593 |
| bare_orderby_common | 194.774 [194.648–194.932] p99 196.722 (n=200, cv=0.00) | — | 18.492 [18.484–18.501] p99 18.708 (n=200, cv=0.01) |
| count_common | 136.130 [135.383–136.608] p99 142.293 (n=200, cv=0.02) rows=358593 | — | 0.681 [0.665–0.704] p99 0.956 (n=200, cv=0.07) rows=358593 |
| count_and | 0.970 [0.968–0.972] p99 1.130 (n=200, cv=0.02) rows=192 | — | 0.935 [0.934–0.936] p99 0.969 (n=200, cv=0.01) rows=192 |
| count_or2 | 41.634 [41.462–41.840] p99 43.613 (n=200, cv=0.03) | — | 1.569 [1.567–1.571] p99 1.623 (n=200, cv=0.01) |
| count_not | 143.267 [142.373–143.798] p99 149.984 (n=200, cv=0.02) | — | 14.106 [14.074–14.159] p99 14.768 (n=200, cv=0.02) |
| count_prefix | 193.462 [192.608–194.200] p99 203.446 (n=200, cv=0.02) | — | 75.924 [75.887–75.954] p99 76.508 (n=200, cv=0.00) |
| count_rare_marker | 1.279 [1.276–1.281] p99 1.323 (n=200, cv=0.01) rows=2000 | — | 0.675 [0.666–0.692] p99 0.758 (n=200, cv=0.04) rows=2000 |
| wandk004_rare_k10 | — | — | 4.639 [4.633–4.645] p99 4.723 (n=200, cv=0.01) |
| wandk004_rare_k100 | — | — | 22.882 [22.859–22.907] p99 23.114 (n=200, cv=0.00) |
| wandk004_mid_k10 | — | — | 23.090 [23.069–23.108] p99 23.278 (n=200, cv=0.00) |
| wandk004_mid_k100 | — | — | 55.757 [55.655–55.809] p99 56.304 (n=200, cv=0.01) |
| wandk004_common_k10 | — | — | 34.665 [34.578–35.172] p99 35.473 (n=200, cv=0.01) |
| wandk004_common_k100 | — | — | 93.765 [93.644–93.930] p99 94.494 (n=200, cv=0.01) |
| wandk008_rare_k10 | — | — | 5.024 [5.022–5.028] p99 5.061 (n=200, cv=0.00) |
| wandk008_rare_k100 | — | — | 12.710 [12.706–12.713] p99 12.760 (n=200, cv=0.00) |
| wandk008_mid_k10 | — | — | 22.953 [22.950–22.956] p99 23.341 (n=200, cv=0.00) |
| wandk008_mid_k100 | — | — | 37.103 [37.097–37.112] p99 37.237 (n=200, cv=0.00) |
| wandk008_common_k10 | — | — | 35.370 [35.339–35.407] p99 36.064 (n=200, cv=0.01) |
| wandk008_common_k100 | — | — | 60.230 [60.193–60.264] p99 60.969 (n=200, cv=0.00) |
| wandk016_rare_k10 | — | — | 2.315 [2.313–2.316] p99 2.340 (n=200, cv=0.00) |
| wandk016_rare_k100 | — | — | 19.964 [19.961–19.968] p99 20.081 (n=200, cv=0.00) |
| wandk016_mid_k10 | — | — | 11.372 [11.370–11.375] p99 11.418 (n=200, cv=0.00) |
| wandk016_mid_k100 | — | — | 44.147 [44.112–44.195] p99 44.869 (n=200, cv=0.01) |
| wandk016_common_k10 | — | — | 17.633 [17.613–17.653] p99 17.797 (n=200, cv=0.01) |
| wandk016_common_k100 | — | — | 75.858 [75.827–75.940] p99 76.565 (n=200, cv=0.00) |
| wandk032_rare_k10 | — | — | 2.736 [2.734–2.738] p99 2.763 (n=200, cv=0.00) |
| wandk032_rare_k100 | — | — | 10.474 [10.451–10.503] p99 10.734 (n=200, cv=0.01) |
| wandk032_mid_k10 | — | — | 11.909 [11.903–11.919] p99 12.186 (n=200, cv=0.01) |
| wandk032_mid_k100 | — | — | 26.303 [26.277–26.311] p99 26.451 (n=200, cv=0.00) |
| wandk032_common_k10 | — | — | 18.349 [18.327–18.364] p99 18.503 (n=200, cv=0.00) |
| wandk032_common_k100 | — | — | 43.064 [43.039–43.082] p99 43.334 (n=200, cv=0.00) |
| wandk064_rare_k10 | — | — | 4.548 [4.543–4.554] p99 4.629 (n=200, cv=0.01) |
| wandk064_rare_k100 | — | — | 17.729 [17.716–17.753] p99 18.205 (n=200, cv=0.01) |
| wandk064_mid_k10 | — | — | 12.384 [12.372–12.403] p99 12.621 (n=200, cv=0.01) |
| wandk064_mid_k100 | — | — | 32.795 [32.754–32.811] p99 33.325 (n=200, cv=0.01) |
| wandk064_common_k10 | — | — | 20.018 [19.982–20.038] p99 20.216 (n=200, cv=0.01) |
| wandk064_common_k100 | — | — | 58.414 [58.377–58.445] p99 58.892 (n=200, cv=0.00) |
| wandk100_rare_k10 | — | — | 6.175 [6.174–6.179] p99 6.205 (n=200, cv=0.00) |
| wandk100_rare_k100 | — | — | 6.211 [6.210–6.213] p99 6.249 (n=200, cv=0.00) |
| wandk100_mid_k10 | — | — | 13.093 [13.090–13.102] p99 13.380 (n=200, cv=0.01) |
| wandk100_mid_k100 | — | — | 13.120 [13.115–13.124] p99 13.177 (n=200, cv=0.00) |
| wandk100_common_k10 | — | — | 22.068 [22.058–22.078] p99 22.495 (n=200, cv=0.01) |
| wandk100_common_k100 | — | — | 22.136 [22.119–22.158] p99 22.424 (n=200, cv=0.01) |
| wandk200_rare_k10 | — | — | 11.335 [11.332–11.337] p99 11.658 (n=200, cv=0.00) |
| wandk200_rare_k100 | — | — | 11.358 [11.356–11.360] p99 11.449 (n=200, cv=0.00) |
| wandk200_mid_k10 | — | — | 17.189 [17.186–17.193] p99 17.343 (n=200, cv=0.00) |
| wandk200_mid_k100 | — | — | 17.213 [17.209–17.217] p99 17.396 (n=200, cv=0.01) |
| wandk200_common_k10 | — | — | 31.411 [31.400–31.443] p99 31.858 (n=200, cv=0.01) |
| wandk200_common_k100 | — | — | 31.590 [31.577–31.603] p99 31.844 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (0.307) | weave (2.738) | 8.92x | yes |
| ranked_rare_k100 | textsearch (1.252) | weave (10.417) | 8.32x | yes |
| bare_orderby_rare | weave (2.738) | gin (191.727) | 70.02x | yes |
| ranked_mid_k10 | textsearch (0.628) | weave (11.606) | 18.48x | yes |
| ranked_mid_k100 | textsearch (1.680) | weave (25.767) | 15.34x | yes |
| bare_orderby_mid | weave (11.622) | gin (190.939) | 16.43x | yes |
| ranked_common_k10 | textsearch (2.401) | weave (17.996) | 7.50x | yes |
| ranked_common_k100 | textsearch (4.611) | weave (42.412) | 9.20x | yes |
| bare_orderby_common | weave (18.492) | gin (194.774) | 10.53x | yes |
| count_common | weave (0.681) | gin (136.130) | 199.90x | yes |
| count_and | weave (0.935) | gin (0.970) | 1.04x | yes |
| count_or2 | weave (1.569) | gin (41.634) | 26.54x | yes |
| count_not | weave (14.106) | gin (143.267) | 10.16x | yes |
| count_prefix | weave (75.924) | gin (193.462) | 2.55x | yes |
| count_rare_marker | weave (0.675) | gin (1.279) | 1.89x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| gin | gin/tsvector in postgresql 17.6 | 21.673453043 | 139 MB | 29 |
| textsearch | pg_textsearch 1.5.0-dev @f940210f | 7.71355489 | 103 MB | 30 |
| weave | pg_weave 0.4.0 | 53.106120549 | 79 MB | 29 |

