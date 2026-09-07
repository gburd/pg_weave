## Corpus identity gate

PASS -- all 4 engines report fingerprint `21f4642d7962c7156449d88caad02822`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | fts | gin | textsearch | weave |
|---|---|---|---|---|
| ranked_rare_k10 | 6.334 [6.329–6.340] p99 6.389 (n=200, cv=0.00) rows=9958 | 12.442 [12.344–12.539] p99 16.328 (n=200, cv=0.07) rows=9958 | 0.308 [0.307–0.308] p99 0.346 (n=200, cv=0.03) | 6.322 [6.319–6.327] p99 6.419 (n=200, cv=0.01) rows=9958 |
| ranked_rare_k100 | 6.373 [6.369–6.377] p99 6.417 (n=200, cv=0.00) rows=9958 | 16.942 [16.845–17.127] p99 18.252 (n=200, cv=0.06) rows=9958 | 1.258 [1.257–1.261] p99 1.297 (n=200, cv=0.01) | 6.338 [6.332–6.344] p99 6.438 (n=200, cv=0.01) rows=9958 |
| bare_orderby_rare | 127.780 [127.698–127.857] p99 129.311 (n=200, cv=0.00) | 191.450 [191.197–191.623] p99 196.896 (n=200, cv=0.01) | — | 6.305 [6.300–6.316] p99 6.450 (n=200, cv=0.01) |
| ranked_mid_k10 | 12.660 [12.652–12.669] p99 12.747 (n=200, cv=0.00) rows=37481 | 56.050 [55.946–56.227] p99 58.955 (n=200, cv=0.02) rows=37481 | 0.626 [0.626–0.627] p99 0.658 (n=200, cv=0.01) | 13.354 [13.349–13.359] p99 13.471 (n=200, cv=0.00) rows=37481 |
| ranked_mid_k100 | 12.705 [12.700–12.709] p99 12.753 (n=200, cv=0.00) rows=37481 | 43.545 [43.297–43.767] p99 47.002 (n=200, cv=0.03) rows=37481 | 1.678 [1.675–1.681] p99 1.716 (n=200, cv=0.01) | 13.444 [13.439–13.452] p99 13.547 (n=200, cv=0.00) rows=37481 |
| bare_orderby_mid | 126.170 [126.114–126.259] p99 127.388 (n=200, cv=0.00) | 190.625 [190.493–190.781] p99 197.019 (n=200, cv=0.01) | — | 13.429 [13.420–13.434] p99 13.595 (n=200, cv=0.01) |
| ranked_common_k10 | 20.828 [20.813–20.836] p99 21.072 (n=200, cv=0.01) rows=358593 | 159.557 [159.034–159.795] p99 170.670 (n=200, cv=0.02) rows=358593 | 2.398 [2.395–2.400] p99 2.435 (n=200, cv=0.01) | 23.671 [23.660–23.679] p99 23.759 (n=200, cv=0.00) rows=358593 |
| ranked_common_k100 | 20.824 [20.815–20.834] p99 21.124 (n=200, cv=0.01) rows=358593 | 156.890 [156.596–157.027] p99 164.584 (n=200, cv=0.01) rows=358593 | 4.609 [4.606–4.611] p99 4.684 (n=200, cv=0.00) | 23.656 [23.649–23.665] p99 23.750 (n=200, cv=0.00) rows=358593 |
| bare_orderby_common | 125.503 [125.399–125.582] p99 126.654 (n=200, cv=0.00) | 193.006 [192.855–193.263] p99 195.193 (n=200, cv=0.00) | — | 23.543 [23.538–23.549] p99 23.619 (n=200, cv=0.00) |
| count_common | 0.676 [0.663–0.689] p99 0.755 (n=200, cv=0.04) rows=358593 | 131.475 [130.969–131.712] p99 137.045 (n=200, cv=0.02) rows=358593 | — | 0.706 [0.705–0.706] p99 0.755 (n=200, cv=0.02) rows=358593 |
| count_and | 0.819 [0.815–0.823] p99 0.863 (n=200, cv=0.02) rows=192 | 0.970 [0.969–0.972] p99 1.127 (n=200, cv=0.03) rows=192 | — | 0.912 [0.910–0.913] p99 0.944 (n=200, cv=0.01) rows=192 |
| count_or2 | 1.440 [1.439–1.441] p99 1.466 (n=200, cv=0.01) | 38.596 [38.493–38.763] p99 41.046 (n=200, cv=0.02) | — | 1.550 [1.546–1.553] p99 1.596 (n=200, cv=0.01) |
| count_not | 6943.790 [6941.240–6946.572] p99 6970.566 (n=200, cv=0.00) | 141.415 [140.627–141.956] p99 147.502 (n=200, cv=0.02) | — | 7008.642 [7007.726–7009.544] p99 7028.360 (n=200, cv=0.00) |
| count_prefix | 74.585 [74.555–74.615] p99 75.217 (n=200, cv=0.00) | 188.934 [188.108–189.705] p99 199.217 (n=200, cv=0.02) | — | 75.931 [75.888–75.958] p99 76.436 (n=200, cv=0.00) |
| count_rare_marker | 0.695 [0.679–0.707] p99 0.758 (n=200, cv=0.04) rows=2000 | 1.229 [1.226–1.231] p99 1.311 (n=200, cv=0.02) rows=2000 | — | 0.708 [0.708–0.709] p99 0.758 (n=200, cv=0.02) rows=2000 |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (0.308) | weave (6.322) | 20.53x | yes |
| ranked_rare_k100 | textsearch (1.258) | weave (6.338) | 5.04x | yes |
| bare_orderby_rare | weave (6.305) | fts (127.780) | 20.27x | yes |
| ranked_mid_k10 | textsearch (0.626) | fts (12.660) | 20.22x | yes |
| ranked_mid_k100 | textsearch (1.678) | fts (12.705) | 7.57x | yes |
| bare_orderby_mid | weave (13.429) | fts (126.170) | 9.40x | yes |
| ranked_common_k10 | textsearch (2.398) | fts (20.828) | 8.69x | yes |
| ranked_common_k100 | textsearch (4.609) | fts (20.824) | 4.52x | yes |
| bare_orderby_common | weave (23.543) | fts (125.503) | 5.33x | yes |
| count_common | fts (0.676) | weave (0.706) | 1.04x | yes |
| count_and | fts (0.819) | weave (0.912) | 1.11x | yes |
| count_or2 | fts (1.440) | weave (1.550) | 1.08x | yes |
| count_not | gin (141.415) | fts (6943.790) | 49.10x | yes |
| count_prefix | fts (74.585) | weave (75.931) | 1.02x | yes |
| count_rare_marker | fts (0.695) | weave (0.708) | 1.02x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| fts | pg_fts 1.5.9 @unknown | 53.638185372 | 79 MB | 30 |
| gin | gin/tsvector in postgresql 17.6 | 23.221131743 | 139 MB | 29 |
| textsearch | pg_textsearch 1.5.0-dev @f940210f | 8.015931639 | 103 MB | 30 |
| weave | pg_weave 0.4.0 | 57.543531395 | 79 MB | 29 |

