## Corpus identity gate

PASS -- all 2 engines report fingerprint `a111e41cec2d22617541f7f2b2f76327`.

## Access-path gate

PASS -- every measured query used its expected access path.

## Latency, milliseconds (server-side Execution Time)

`p50 [ci95] p99 (n, cv)`; `rows` is the match count and must be read alongside latency -- engines with different analyzers match different numbers of documents.

| query | gin | weave |
|---|---|---|
| ranked_rare_k10 | 1.063 [1.058–1.067] p99 1.116 (n=50, cv=0.02) rows=998 | 1.194 [1.194–1.202] p99 1.235 (n=50, cv=0.01) rows=998 |
| ranked_rare_k100 | 1.066 [1.063–1.070] p99 1.135 (n=50, cv=0.02) rows=998 | 1.221 [1.220–1.230] p99 1.274 (n=50, cv=0.01) rows=998 |
| bare_orderby_rare | 29.333 [29.273–29.364] p99 29.657 (n=50, cv=0.01) | 1.199 [1.196–1.206] p99 1.284 (n=50, cv=0.01) |
| ranked_mid_k10 | 5.031 [4.856–5.091] p99 5.493 (n=50, cv=0.05) rows=3753 | 2.436 [2.433–2.439] p99 2.466 (n=50, cv=0.01) rows=3753 |
| ranked_mid_k100 | 4.833 [4.743–4.892] p99 5.684 (n=50, cv=0.06) rows=3753 | 2.466 [2.464–2.468] p99 2.505 (n=50, cv=0.01) rows=3753 |
| bare_orderby_mid | 29.296 [29.251–29.339] p99 29.601 (n=50, cv=0.00) | 2.443 [2.439–2.447] p99 2.524 (n=50, cv=0.01) |
| ranked_common_k10 | 34.975 [34.942–35.084] p99 35.506 (n=50, cv=0.01) rows=35895 | 5.094 [5.089–5.098] p99 5.118 (n=50, cv=0.00) rows=35895 |
| ranked_common_k100 | 20.800 [20.722–20.983] p99 21.782 (n=50, cv=0.02) rows=35895 | 5.134 [5.130–5.144] p99 5.171 (n=50, cv=0.00) rows=35895 |
| bare_orderby_common | 29.743 [29.702–29.777] p99 29.985 (n=50, cv=0.00) | 5.115 [5.111–5.120] p99 5.145 (n=50, cv=0.00) |
| count_common | 14.497 [14.471–14.546] p99 14.811 (n=50, cv=0.01) rows=35895 | 0.073 [0.073–0.074] p99 0.105 (n=50, cv=0.06) rows=35895 |
| count_and | 0.096 [0.096–0.096] p99 0.105 (n=50, cv=0.01) rows=23 | 0.088 [0.088–0.088] p99 0.098 (n=50, cv=0.02) rows=23 |
| count_or2 | 2.792 [2.770–2.843] p99 3.003 (n=50, cv=0.03) | 0.153 [0.152–0.153] p99 0.179 (n=50, cv=0.03) |
| count_not | 14.718 [14.615–14.890] p99 15.399 (n=50, cv=0.02) | 603.902 [603.489–604.256] p99 606.069 (n=50, cv=0.00) |
| count_prefix | 18.930 [18.828–19.003] p99 19.927 (n=50, cv=0.02) | 7.112 [7.101–7.121] p99 7.161 (n=50, cv=0.00) |
| count_rare_marker | 0.088 [0.088–0.088] p99 0.100 (n=50, cv=0.02) rows=200 | 0.077 [0.077–0.078] p99 0.090 (n=50, cv=0.02) rows=200 |

## Winner per query, and whether the margin is supported

A margin is only claimed when the two medians' bootstrap 95% CIs do not overlap. Overlapping CIs are reported as a tie regardless of the point estimates.

| query | best | second | ratio | CIs disjoint? |
|---|---|---|---|---|
| ranked_rare_k10 | gin (1.063) | weave (1.194) | 1.12x | yes |
| ranked_rare_k100 | gin (1.066) | weave (1.221) | 1.15x | yes |
| bare_orderby_rare | weave (1.199) | gin (29.333) | 24.46x | yes |
| ranked_mid_k10 | weave (2.436) | gin (5.031) | 2.07x | yes |
| ranked_mid_k100 | weave (2.466) | gin (4.833) | 1.96x | yes |
| bare_orderby_mid | weave (2.443) | gin (29.296) | 11.99x | yes |
| ranked_common_k10 | weave (5.094) | gin (34.975) | 6.87x | yes |
| ranked_common_k100 | weave (5.134) | gin (20.800) | 4.05x | yes |
| bare_orderby_common | weave (5.115) | gin (29.743) | 5.81x | yes |
| count_common | weave (0.073) | gin (14.497) | 198.59x | yes |
| count_and | weave (0.088) | gin (0.096) | 1.09x | yes |
| count_or2 | weave (0.153) | gin (2.792) | 18.25x | yes |
| count_not | gin (14.718) | weave (603.902) | 41.03x | yes |
| count_prefix | weave (7.112) | gin (18.930) | 2.66x | yes |
| count_rare_marker | weave (0.077) | gin (0.088) | 1.14x | yes |

## Build time and index size

| engine | version | build s | index size | non-default GUCs |
|---|---|---:|---:|---:|
| gin | gin/tsvector in postgresql 17.6 | 2.615245479 | 27 MB | 29 |
| weave | pg_weave 0.4.0 | 5.204445186 | 19 MB | 29 |

