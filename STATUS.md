# STATUS wt-limit (G87 LIMIT hint, G86 score reuse)
## Done
- G87 lexical <=> route: planner_hook weave_planner (customscan.c), GUC pg_weave.limit_hint,
  weave_ord_first_width (amscan.c), weave_query_same ignores header flags. sql/limit_hint.sql.
  PG17 installcheck 25/25 + isolation 2/2.
## Next
- mutants M1 (ignore flags) M2 (exact-k first pass without widening -- n/a: ladder untouched; use
  "first pass = k, candfull forced true") M3 (compare flags in weave_query_same)
- PG18 installcheck, TAP
- G86 (score reuse) -- separate commit
- fused route hint (so->fusek) -- owed, recorded in G87
