## Corpus identity gate

PASS -- all 1 engines report fingerprint `a6461d245bf2cd8dc37ede76f707c33e`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | weave |
|---|---|
| ranked_rare_k10 | 2.805 [2.802–2.809] p99 2.844 (n=200, cv=0.01) rows=9971 |
| ranked_rare_k100 | 3.426 [3.420–3.431] p99 3.465 (n=200, cv=0.01) rows=9971 |
| bare_orderby_rare | 2.806 [2.803–2.808] p99 2.843 (n=200, cv=0.01) |
| ranked_mid_k10 | 10.079 [10.068–10.093] p99 10.275 (n=200, cv=0.01) rows=39683 |
| ranked_mid_k100 | 11.079 [11.053–11.108] p99 11.320 (n=200, cv=0.01) rows=39683 |
| bare_orderby_mid | 10.191 [10.158–10.212] p99 10.458 (n=200, cv=0.01) |
| ranked_common_k10 | 15.293 [15.273–15.306] p99 15.709 (n=200, cv=0.01) rows=1741439 |
| ranked_common_k100 | 16.305 [16.293–16.320] p99 16.543 (n=200, cv=0.00) rows=1741439 |
| bare_orderby_common | 15.272 [15.256–15.290] p99 15.455 (n=200, cv=0.01) |
| count_common | 2.085 [2.054–2.119] p99 2.345 (n=200, cv=0.04) rows=1741439 |
| count_and | 0.816 [0.815–0.818] p99 0.867 (n=200, cv=0.02) rows=212 |
| count_or2 | 1.410 [1.408–1.411] p99 1.449 (n=200, cv=0.01) |
| count_not | 72.964 [72.868–73.176] p99 74.894 (n=200, cv=0.01) |
| count_prefix | 762.825 [762.451–763.275] p99 767.657 (n=200, cv=0.00) |
| count_rare_marker | 2.045 [2.043–2.053] p99 2.212 (n=200, cv=0.03) rows=2000 |
| wandk004_rare_k10 | 2.546 [2.542–2.550] p99 2.590 (n=200, cv=0.01) |
| wandk004_rare_k100 | 6.837 [6.832–6.848] p99 6.999 (n=200, cv=0.01) |
| wandk004_mid_k10 | 9.538 [9.532–9.552] p99 9.677 (n=200, cv=0.01) |
| wandk004_mid_k100 | 21.361 [21.339–21.383] p99 21.869 (n=200, cv=0.01) |
| wandk004_common_k10 | 11.832 [11.812–11.869] p99 12.044 (n=200, cv=0.01) |
| wandk004_common_k100 | 35.389 [35.329–35.480] p99 35.831 (n=200, cv=0.01) |
| wandk008_rare_k10 | 2.550 [2.546–2.553] p99 2.593 (n=200, cv=0.01) |
| wandk008_rare_k100 | 6.875 [6.858–6.897] p99 7.082 (n=200, cv=0.01) |
| wandk008_mid_k10 | 9.757 [9.745–9.768] p99 9.911 (n=200, cv=0.01) |
| wandk008_mid_k100 | 21.790 [21.752–21.836] p99 22.120 (n=200, cv=0.01) |
| wandk008_common_k10 | 11.956 [11.946–11.970] p99 12.113 (n=200, cv=0.01) |
| wandk008_common_k100 | 35.587 [35.568–35.610] p99 35.849 (n=200, cv=0.00) |
| wandk016_rare_k10 | 2.580 [2.577–2.582] p99 2.628 (n=200, cv=0.01) |
| wandk016_rare_k100 | 7.153 [7.111–7.180] p99 7.631 (n=200, cv=0.03) |
| wandk016_mid_k10 | 9.676 [9.660–9.697] p99 9.866 (n=200, cv=0.01) |
| wandk016_mid_k100 | 21.710 [21.675–21.736] p99 21.970 (n=200, cv=0.01) |
| wandk016_common_k10 | 11.810 [11.771–11.831] p99 12.088 (n=200, cv=0.01) |
| wandk016_common_k100 | 35.301 [35.215–35.356] p99 35.672 (n=200, cv=0.01) |
| wandk032_rare_k10 | 2.810 [2.808–2.815] p99 2.872 (n=200, cv=0.01) |
| wandk032_rare_k100 | 3.449 [3.444–3.457] p99 3.585 (n=200, cv=0.01) |
| wandk032_mid_k10 | 10.145 [10.133–10.159] p99 10.345 (n=200, cv=0.01) |
| wandk032_mid_k100 | 10.840 [10.818–10.858] p99 11.062 (n=200, cv=0.01) |
| wandk032_common_k10 | 15.275 [15.264–15.287] p99 15.395 (n=200, cv=0.01) |
| wandk032_common_k100 | 16.193 [16.162–16.227] p99 16.394 (n=200, cv=0.01) |
| wandk064_rare_k10 | 3.696 [3.690–3.699] p99 3.787 (n=200, cv=0.01) |
| wandk064_rare_k100 | 4.309 [4.306–4.311] p99 4.367 (n=200, cv=0.01) |
| wandk064_mid_k10 | 10.984 [10.975–10.995] p99 11.152 (n=200, cv=0.01) |
| wandk064_mid_k100 | 12.015 [11.970–12.061] p99 12.263 (n=200, cv=0.01) |
| wandk064_common_k10 | 22.780 [22.777–22.789] p99 23.034 (n=200, cv=0.00) |
| wandk064_common_k100 | 23.625 [23.607–23.637] p99 23.813 (n=200, cv=0.00) |
| wandk100_rare_k10 | 5.436 [5.432–5.441] p99 5.478 (n=200, cv=0.00) |
| wandk100_rare_k100 | 6.091 [6.083–6.113] p99 6.268 (n=200, cv=0.01) |
| wandk100_mid_k10 | 12.464 [12.452–12.484] p99 12.780 (n=200, cv=0.01) |
| wandk100_mid_k100 | 13.271 [13.227–13.304] p99 13.574 (n=200, cv=0.02) |
| wandk100_common_k10 | 29.180 [29.172–29.185] p99 29.407 (n=200, cv=0.00) |
| wandk100_common_k100 | 30.104 [30.095–30.114] p99 30.270 (n=200, cv=0.00) |
| wandk200_rare_k10 | 12.293 [12.269–12.308] p99 12.491 (n=200, cv=0.02) |
| wandk200_rare_k100 | 13.002 [12.972–13.039] p99 13.303 (n=200, cv=0.01) |
| wandk200_mid_k10 | 18.185 [18.176–18.197] p99 18.330 (n=200, cv=0.00) |
| wandk200_mid_k100 | 19.173 [19.165–19.192] p99 19.546 (n=200, cv=0.01) |
| wandk200_common_k10 | 53.362 [53.356–53.372] p99 53.518 (n=200, cv=0.00) |
| wandk200_common_k100 | 54.280 [54.265–54.286] p99 54.438 (n=200, cv=0.00) |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| weave | pg_weave 0.5.0 | 194.217665058 | 625 MB | 29 |

