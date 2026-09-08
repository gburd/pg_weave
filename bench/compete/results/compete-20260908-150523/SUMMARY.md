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
| ranked_rare_k10 | 79.815 [79.705–79.938] p99 81.440 (n=200, cv=0.04) rows=9971 | 1.251 [1.250–1.253] p99 1.282 (n=200, cv=0.01) | 2.938 [2.936–2.941] p99 2.969 (n=200, cv=0.00) rows=9971 |
| ranked_rare_k100 | 80.277 [80.206–80.348] p99 81.560 (n=200, cv=0.03) rows=9971 | 8.766 [8.763–8.769] p99 8.845 (n=200, cv=0.00) | 3.567 [3.565–3.570] p99 3.599 (n=200, cv=0.00) rows=9971 |
| bare_orderby_rare | 2392.061 [2389.231–2394.640] p99 2433.154 (n=200, cv=0.01) | — | 2.945 [2.944–2.947] p99 2.981 (n=200, cv=0.00) |
| ranked_mid_k10 | 292.657 [291.208–294.190] p99 301.571 (n=200, cv=0.01) rows=39683 | 1.628 [1.624–1.631] p99 1.665 (n=200, cv=0.01) | 10.561 [10.554–10.568] p99 10.614 (n=200, cv=0.00) rows=39683 |
| ranked_mid_k100 | 115.694 [115.583–115.929] p99 122.963 (n=200, cv=0.02) rows=39683 | 9.174 [9.172–9.176] p99 9.210 (n=200, cv=0.00) | 11.167 [11.161–11.170] p99 11.258 (n=200, cv=0.00) rows=39683 |
| bare_orderby_mid | 2391.501 [2389.000–2394.217] p99 2418.437 (n=200, cv=0.00) | — | 10.520 [10.512–10.525] p99 10.604 (n=200, cv=0.00) |
| ranked_common_k10 | PLAN FAIL | 3.136 [3.134–3.137] p99 3.167 (n=200, cv=0.00) | 15.415 [15.402–15.424] p99 15.753 (n=200, cv=0.01) rows=1741439 |
| ranked_common_k100 | PLAN FAIL | 14.851 [14.849–14.854] p99 14.948 (n=200, cv=0.00) | 16.052 [16.040–16.090] p99 16.480 (n=200, cv=0.01) rows=1741439 |
| bare_orderby_common | 2454.505 [2451.798–2457.498] p99 2514.720 (n=200, cv=0.01) | — | 15.404 [15.397–15.421] p99 15.853 (n=200, cv=0.01) |
| count_common | PLAN FAIL | — | 2.056 [2.046–2.084] p99 2.306 (n=200, cv=0.03) rows=1741439 |
| count_and | 1.047 [1.046–1.050] p99 1.087 (n=200, cv=0.01) rows=212 | — | 0.828 [0.827–0.829] p99 0.860 (n=200, cv=0.01) rows=212 |
| count_or2 | 54.791 [54.684–54.969] p99 57.488 (n=200, cv=0.02) | — | 1.407 [1.405–1.409] p99 1.441 (n=200, cv=0.01) |
| count_not | PLAN FAIL | — | 69.680 [69.567–69.925] p99 71.313 (n=200, cv=0.01) |
| count_prefix | PLAN FAIL | — | 764.596 [764.316–764.800] p99 768.681 (n=200, cv=0.00) |
| count_rare_marker | 1.296 [1.295–1.299] p99 1.337 (n=200, cv=0.01) rows=2000 | — | 2.077 [2.058–2.106] p99 2.326 (n=200, cv=0.03) rows=2000 |
| wandk004_rare_k10 | — | — | 2.679 [2.677–2.680] p99 2.704 (n=200, cv=0.01) |
| wandk004_rare_k100 | — | — | 7.137 [7.134–7.140] p99 7.174 (n=200, cv=0.00) |
| wandk004_mid_k10 | — | — | 10.073 [10.070–10.080] p99 10.130 (n=200, cv=0.00) |
| wandk004_mid_k100 | — | — | 22.150 [22.135–22.169] p99 22.692 (n=200, cv=0.01) |
| wandk004_common_k10 | — | — | 12.021 [12.015–12.031] p99 12.174 (n=200, cv=0.00) |
| wandk004_common_k100 | — | — | 35.595 [35.573–35.614] p99 36.336 (n=200, cv=0.01) |
| wandk008_rare_k10 | — | — | 2.677 [2.675–2.681] p99 2.708 (n=200, cv=0.01) |
| wandk008_rare_k100 | — | — | 7.089 [7.086–7.094] p99 7.134 (n=200, cv=0.00) |
| wandk008_mid_k10 | — | — | 10.056 [10.050–10.062] p99 10.218 (n=200, cv=0.00) |
| wandk008_mid_k100 | — | — | 22.131 [22.118–22.137] p99 22.333 (n=200, cv=0.01) |
| wandk008_common_k10 | — | — | 12.038 [12.029–12.054] p99 12.296 (n=200, cv=0.01) |
| wandk008_common_k100 | — | — | 35.542 [35.530–35.560] p99 36.085 (n=200, cv=0.00) |
| wandk016_rare_k10 | — | — | 2.686 [2.683–2.689] p99 2.724 (n=200, cv=0.01) |
| wandk016_rare_k100 | — | — | 7.094 [7.091–7.101] p99 7.266 (n=200, cv=0.01) |
| wandk016_mid_k10 | — | — | 10.034 [10.031–10.040] p99 10.093 (n=200, cv=0.00) |
| wandk016_mid_k100 | — | — | 22.127 [22.114–22.136] p99 22.368 (n=200, cv=0.00) |
| wandk016_common_k10 | — | — | 12.042 [12.036–12.052] p99 12.299 (n=200, cv=0.01) |
| wandk016_common_k100 | — | — | 35.542 [35.516–35.571] p99 36.420 (n=200, cv=0.01) |
| wandk032_rare_k10 | — | — | 2.938 [2.936–2.941] p99 2.976 (n=200, cv=0.01) |
| wandk032_rare_k100 | — | — | 3.549 [3.546–3.553] p99 3.581 (n=200, cv=0.00) |
| wandk032_mid_k10 | — | — | 10.521 [10.517–10.524] p99 10.641 (n=200, cv=0.00) |
| wandk032_mid_k100 | — | — | 11.149 [11.144–11.165] p99 11.289 (n=200, cv=0.00) |
| wandk032_common_k10 | — | — | 15.397 [15.387–15.425] p99 15.674 (n=200, cv=0.01) |
| wandk032_common_k100 | — | — | 16.141 [16.107–16.157] p99 16.694 (n=200, cv=0.01) |
| wandk064_rare_k10 | — | — | 3.838 [3.834–3.840] p99 3.872 (n=200, cv=0.00) |
| wandk064_rare_k100 | — | — | 4.466 [4.463–4.469] p99 4.508 (n=200, cv=0.00) |
| wandk064_mid_k10 | — | — | 11.491 [11.488–11.496] p99 11.545 (n=200, cv=0.00) |
| wandk064_mid_k100 | — | — | 12.132 [12.129–12.138] p99 12.246 (n=200, cv=0.00) |
| wandk064_common_k10 | — | — | 22.861 [22.839–22.908] p99 23.167 (n=200, cv=0.00) |
| wandk064_common_k100 | — | — | 23.632 [23.607–23.654] p99 24.210 (n=200, cv=0.01) |
| wandk100_rare_k10 | — | — | 5.622 [5.619–5.625] p99 5.681 (n=200, cv=0.00) |
| wandk100_rare_k100 | — | — | 6.236 [6.233–6.238] p99 6.266 (n=200, cv=0.00) |
| wandk100_mid_k10 | — | — | 12.885 [12.881–12.889] p99 13.002 (n=200, cv=0.00) |
| wandk100_mid_k100 | — | — | 13.531 [13.524–13.534] p99 13.648 (n=200, cv=0.00) |
| wandk100_common_k10 | — | — | 29.387 [29.361–29.447] p99 29.801 (n=200, cv=0.00) |
| wandk100_common_k100 | — | — | 30.215 [30.176–30.258] p99 30.892 (n=200, cv=0.01) |
| wandk200_rare_k10 | — | — | 12.423 [12.420–12.426] p99 12.605 (n=200, cv=0.00) |
| wandk200_rare_k100 | — | — | 13.047 [13.044–13.050] p99 13.305 (n=200, cv=0.00) |
| wandk200_mid_k10 | — | — | 18.560 [18.552–18.566] p99 18.698 (n=200, cv=0.00) |
| wandk200_mid_k100 | — | — | 19.169 [19.162–19.178] p99 19.359 (n=200, cv=0.00) |
| wandk200_common_k10 | — | — | 53.965 [53.923–54.023] p99 54.464 (n=200, cv=0.00) |
| wandk200_common_k100 | — | — | 54.812 [54.781–54.867] p99 55.340 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | textsearch (1.251) | weave (2.938) | 2.35x | yes |
| ranked_rare_k100 | weave (3.567) | textsearch (8.766) | 2.46x | yes |
| bare_orderby_rare | weave (2.945) | gin (2392.061) | 812.24x | yes |
| ranked_mid_k10 | textsearch (1.628) | weave (10.561) | 6.49x | yes |
| ranked_mid_k100 | textsearch (9.174) | weave (11.167) | 1.22x | yes |
| bare_orderby_mid | weave (10.520) | gin (2391.501) | 227.33x | yes |
| ranked_common_k10 | textsearch (3.136) | weave (15.415) | 4.92x | yes |
| ranked_common_k100 | textsearch (14.851) | weave (16.052) | 1.08x | yes |
| bare_orderby_common | weave (15.404) | gin (2454.505) | 159.34x | yes |
| count_and | weave (0.828) | gin (1.047) | 1.26x | yes |
| count_or2 | weave (1.407) | gin (54.791) | 38.94x | yes |
| count_rare_marker | gin (1.296) | weave (2.077) | 1.60x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| gin | gin/tsvector in postgresql 17.6 | 235.758379757 | 1120 MB | 29 |
| textsearch | pg_textsearch 1.5.0-dev @f940210f | 45.074548261 | 873 MB | 30 |
| weave | pg_weave 0.5.0 | 495.919274069 | 625 MB | 29 |

