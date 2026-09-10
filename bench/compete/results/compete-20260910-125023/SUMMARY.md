## Corpus identity gate

PASS -- all 2 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | textsearch | weave |
|---|---|---|
| ranked_rare_k10 | 1.252 [1.250–1.255] p99 1.287 (n=200, cv=0.01) | 1.508 [1.506–1.510] p99 1.545 (n=200, cv=0.01) rows=9971 |
| ranked_rare_k100 | 8.726 [8.723–8.729] p99 8.789 (n=200, cv=0.00) | 2.203 [2.199–2.212] p99 2.401 (n=200, cv=0.02) rows=9971 |
| ranked_mid_k10 | 1.624 [1.621–1.628] p99 1.661 (n=200, cv=0.01) | 6.191 [6.186–6.196] p99 6.284 (n=200, cv=0.01) rows=39683 |
| ranked_mid_k100 | 9.123 [9.121–9.127] p99 9.167 (n=200, cv=0.00) | 7.201 [7.192–7.210] p99 7.337 (n=200, cv=0.01) rows=39683 |
| ranked_common_k10 | 3.142 [3.140–3.144] p99 3.175 (n=200, cv=0.00) | 14.919 [14.900–14.940] p99 15.085 (n=200, cv=0.01) rows=1741439 |
| ranked_common_k100 | 14.836 [14.829–14.841] p99 14.931 (n=200, cv=0.00) | 15.870 [15.846–15.885] p99 16.055 (n=200, cv=0.01) rows=1741439 |
| bare_orderby_rare | — | 1.507 [1.505–1.510] p99 1.549 (n=200, cv=0.01) |
| bare_orderby_mid | — | 6.235 [6.227–6.244] p99 6.330 (n=200, cv=0.01) |
| bare_orderby_common | — | 14.685 [14.668–14.694] p99 14.811 (n=200, cv=0.00) |
| count_common | — | 2.195 [2.195–2.195] p99 2.342 (n=200, cv=0.01) rows=1741439 |
| count_and | — | 0.817 [0.816–0.818] p99 0.854 (n=200, cv=0.01) rows=212 |
| count_or2 | — | 1.440 [1.437–1.443] p99 1.496 (n=200, cv=0.01) |
| count_not | — | 76.672 [76.448–76.802] p99 78.270 (n=200, cv=0.03) |
| count_prefix | — | 766.590 [766.358–766.988] p99 770.863 (n=200, cv=0.00) |
| count_rare_marker | — | 2.198 [2.198–2.199] p99 2.313 (n=200, cv=0.01) rows=2000 |
| wandk004_rare_k10 | — | 1.317 [1.314–1.320] p99 1.394 (n=200, cv=0.01) |
| wandk004_rare_k100 | — | 4.376 [4.349–4.400] p99 4.518 (n=200, cv=0.02) |
| wandk004_mid_k10 | — | 5.805 [5.796–5.815] p99 5.907 (n=200, cv=0.01) |
| wandk004_mid_k100 | — | 13.825 [13.814–13.836] p99 14.008 (n=200, cv=0.01) |
| wandk004_common_k10 | — | 11.593 [11.580–11.617] p99 11.850 (n=200, cv=0.01) |
| wandk004_common_k100 | — | 34.002 [33.978–34.024] p99 34.163 (n=200, cv=0.00) |
| wandk008_rare_k10 | — | 1.310 [1.308–1.313] p99 1.345 (n=200, cv=0.01) |
| wandk008_rare_k100 | — | 4.206 [4.199–4.213] p99 4.403 (n=200, cv=0.01) |
| wandk008_mid_k10 | — | 5.718 [5.714–5.723] p99 5.806 (n=200, cv=0.01) |
| wandk008_mid_k100 | — | 13.609 [13.596–13.626] p99 13.842 (n=200, cv=0.01) |
| wandk008_common_k10 | — | 11.626 [11.616–11.643] p99 11.762 (n=200, cv=0.01) |
| wandk008_common_k100 | — | 34.061 [34.044–34.080] p99 34.339 (n=200, cv=0.00) |
| wandk016_rare_k10 | — | 1.317 [1.314–1.320] p99 1.373 (n=200, cv=0.01) |
| wandk016_rare_k100 | — | 4.408 [4.396–4.417] p99 4.572 (n=200, cv=0.02) |
| wandk016_mid_k10 | — | 5.814 [5.808–5.819] p99 5.905 (n=200, cv=0.01) |
| wandk016_mid_k100 | — | 13.775 [13.758–13.787] p99 13.938 (n=200, cv=0.01) |
| wandk016_common_k10 | — | 11.653 [11.633–11.670] p99 11.812 (n=200, cv=0.01) |
| wandk016_common_k100 | — | 34.195 [34.173–34.233] p99 34.504 (n=200, cv=0.00) |
| wandk032_rare_k10 | — | 1.510 [1.508–1.512] p99 1.552 (n=200, cv=0.01) |
| wandk032_rare_k100 | — | 2.195 [2.188–2.200] p99 2.265 (n=200, cv=0.01) |
| wandk032_mid_k10 | — | 6.218 [6.211–6.228] p99 6.316 (n=200, cv=0.01) |
| wandk032_mid_k100 | — | 7.220 [7.204–7.231] p99 7.344 (n=200, cv=0.01) |
| wandk032_common_k10 | — | 14.853 [14.832–14.886] p99 15.060 (n=200, cv=0.01) |
| wandk032_common_k100 | — | 15.888 [15.858–15.904] p99 16.085 (n=200, cv=0.01) |
| wandk064_rare_k10 | — | 2.198 [2.196–2.201] p99 2.251 (n=200, cv=0.01) |
| wandk064_rare_k100 | — | 2.924 [2.918–2.931] p99 3.072 (n=200, cv=0.02) |
| wandk064_mid_k10 | — | 7.036 [7.028–7.042] p99 7.119 (n=200, cv=0.01) |
| wandk064_mid_k100 | — | 7.944 [7.931–7.959] p99 8.098 (n=200, cv=0.01) |
| wandk064_common_k10 | — | 21.853 [21.835–21.869] p99 22.109 (n=200, cv=0.01) |
| wandk064_common_k100 | — | 22.782 [22.768–22.796] p99 23.102 (n=200, cv=0.01) |
| wandk100_rare_k10 | — | 3.471 [3.468–3.476] p99 3.528 (n=200, cv=0.01) |
| wandk100_rare_k100 | — | 4.081 [4.078–4.085] p99 4.149 (n=200, cv=0.01) |
| wandk100_mid_k10 | — | 8.192 [8.180–8.204] p99 8.345 (n=200, cv=0.01) |
| wandk100_mid_k100 | — | 9.267 [9.247–9.284] p99 9.436 (n=200, cv=0.01) |
| wandk100_common_k10 | — | 28.395 [28.373–28.413] p99 28.598 (n=200, cv=0.01) |
| wandk100_common_k100 | — | 29.392 [29.367–29.412] p99 29.587 (n=200, cv=0.01) |
| wandk200_rare_k10 | — | 9.442 [9.433–9.449] p99 9.552 (n=200, cv=0.00) |
| wandk200_rare_k100 | — | 10.425 [10.418–10.440] p99 10.599 (n=200, cv=0.01) |
| wandk200_mid_k10 | — | 14.029 [14.024–14.037] p99 14.118 (n=200, cv=0.00) |
| wandk200_mid_k100 | — | 14.946 [14.937–14.959] p99 15.100 (n=200, cv=0.00) |
| wandk200_common_k10 | — | 52.511 [52.492–52.521] p99 52.666 (n=200, cv=0.01) |
| wandk200_common_k100 | — | 53.478 [53.456–53.486] p99 53.622 (n=200, cv=0.01) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (1.252) | weave (1.508) | 1.20x | yes |
| ranked_rare_k100 | weave (2.203) | textsearch (8.726) | 3.96x | yes |
| ranked_mid_k10 | textsearch (1.624) | weave (6.191) | 3.81x | yes |
| ranked_mid_k100 | weave (7.201) | textsearch (9.123) | 1.27x | yes |
| ranked_common_k10 | textsearch (3.142) | weave (14.919) | 4.75x | yes |
| ranked_common_k100 | textsearch (14.836) | weave (15.870) | 1.07x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| textsearch | pg_textsearch 1.5.0-dev @3c77689e | 45.862444886 | 873 MB | 30 |
| weave | pg_weave 0.5.0 | 205.557302968 | 626 MB | 29 |

