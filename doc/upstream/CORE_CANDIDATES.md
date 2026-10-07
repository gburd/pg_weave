# Core PostgreSQL changes that would correct or improve a pg_weave issue

**Standing instruction (maintainer, 2026-10-07):** whenever a pg_weave issue could be
corrected or improved by a change to PostgreSQL itself, record it here for later. Never
block pg_weave work on one: every row below has a workaround that ships in the extension,
and the row says what it is. The ORDER-BY-values proposal is the one the maintainer is
already carrying on -hackers (`ORDERBY_VALUES_TO_TLIST.md`).

Each row gives the pg_weave issue, the core change, what the extension does in the meantime,
and the -hackers history where there is one. "Size" is a guess at how hard the core change
would be to land: **S** a small format-free patch, **M** a new API or catalog field, **L** an
on-disk format change to a core type. Status is `idea` until someone writes a patch.

## Planner and executor

| # | pg_weave issue | core change | workaround in the extension | history | size | status |
|---|---|---|---|---|---|---|
| C1 | G86 (`d <=> q` re-evaluated per row), F3 (`score()`), G86's owed `fuse()` reuse | let an ordering index scan hand its `xs_orderbyvals` to the target list (`IndexAmRoutine.amorderbyvalsexact`) | `weave_current_distance()` planted by a `planner_hook` on RESJUNK entries only (0.29.0) | Tom Lane 2024 objection ("Why is FOR ORDER BY function getting called", Chris Cleveland); Heikki's 2023 junk-column thread | M | **maintainer is carrying it** (`ORDERBY_VALUES_TO_TLIST.md`, `0001-orderby-values-to-tlist.patch`) |
| C2 | G87 (the AM is not told the LIMIT, so a LIMIT 10 ranked scan runs at width 128) | pass a bound on the rows the executor will pull (LIMIT + OFFSET when constant) to `amrescan` / the scan descriptor, the way `ExecSetTupleBound()` already does for Sort | `planner_hook` writes k into the query Const's reserved header field (`WeaveQueryData.flags`); impossible for the vector route without spending `wvec`'s reserved word (deferred) | `ExecSetTupleBound()` exists for Sort / MergeAppend / Gather. A 2026-10-07 search of -hackers found no proposal to pass the bound to an index AM; closest is Chris Cleveland, 2021-08-02, "Passing data out of indexam or tableam" (`CABSN6Vf4W9ntWwfHq2RuWHjOK1xPux5766_4mb4vR-zpu9AsfQ@mail.gmail.com`), which is C1's problem | M | idea |
| C3 | G39 (a query-less scan of an indexed table fails on PG18 with `enable_seqscan = off`) | an AM property meaning "cannot serve an unrestricted, unordered scan" that the planner honours, instead of relying on `amoptionalkey` plus a cost guard | the AM raises an error for that shape; see G39 | not searched yet | S-M | idea |

## Text search types (`tsvector`, the parser)

| # | pg_weave issue | core change | workaround in the extension | history | size | status |
|---|---|---|---|---|---|---|
| C4 | G89 (positions past token 16,383 collapse to 16,383); M7 (a tsvector cannot carry a long document's positions) | `parsetext()` exposes the unclamped token ordinal per word (e.g. an `int32` beside `ParsedWord.pos`, or a variant that does not `LIMITPOS()`) | starts `prs->pos` at `PG_INT32_MIN` and unwraps `pos mod 65536`; refuses a document with >= 65,535 consecutive stopwords (`src/query/tsanalyze.c`) | none found | S (format-free: `ParsedWord` is in-memory only) | idea |
| C5 | M7 (BM25 needs per-document length; a tsvector stores none, and length from positions is wrong past 16,383) | a SQL function returning a tsvector's token count as the number of distinct positions (today `length(tsvector)` counts lexemes, and `ts_rank`'s normalization 2 uses that) | DONE in the extension 2026-10-07 (M7 step 2): every wdoc path, `to_wdoc(tsvector)` and `tsvector_lex_ops` take doclen = distinct positions, a positionless entry counting 1 (`weave_doc_default_len()`, `src/query/doc.c`) | none found | S | idea |
| C6 | M7 (tf capped at 255 positions per lexeme; position cap 16,383; 1 MB limit) | widen `WordEntryPos` (14-bit position) and `MAXNUMPOS`, remove the 1 MB limit | `wdoc` is the exact path; the operator class counts and exposes capped documents | Heikki 2007-09-13 asked why 256 (`46E91A51.80103@enterprisedb.com`); Teodor: ranking cost, and a very frequent word is a stopword or spam (`46E92622.5030601@sigaev.ru`). Kurbangaliev 2017 patch removed the 1 MB limit (`20170801170846.66e3ab06@wp.localdomain`), stalled on pg_upgrade conversion cost (~15 % on reads) and "RUM is not in core" | L | idea; history says unlikely |
| C10 | M7 step 2 (`tsv @@@ 'literal'` is ambiguous once `tsvector_lex_ops` exists) | remove core's deprecated `@@@ (tsvector, tsquery)` / `(tsquery, tsvector)` operators (documented as deprecated, "obsolete version of @@", since 8.3) so an extension can give `@@@` a tsvector meaning without making the bare-literal form ambiguous | `tsvector_lex_ops` uses `@@@ (tsvector, wquery)`; a caller must type the literal (`'q'::wquery`), and `sql/tsvector_input.sql` records the ambiguity error (0.30.0) | not searched yet (no `postgresq` tool in the session that found it) | S (catalog removal; compatibility is the whole debate) | idea |
| C11 | M7 step 2 (a mixed tsvector -- `tsv \|\| 'tag'::tsvector` -- has entries with and without positions, and core's phrase evaluation answers MAYBE then false for any phrase through the positionless entry) | none needed for correctness; a `tsvector` function that reports whether a value is stripped or mixed (or a `strip`-state flag) would let users find these values without an extension | `weave_index_tsvector_stats()` counts them per index; the opclass keeps the positions a mixed value has (ordinal 0 = unknown position) and matches core's `@@` phrase answers | not searched yet | S | idea |
| C7 | M7 (the caps change ranking silently) | document, in the text-search chapter, that the per-lexeme and position caps make `ts_rank` / `ts_rank_cd` approximate on long documents, and say where | none needed | Sushant Sinha 2009 reported a `ts_rank_cd` bug past 16,383 (`1233209252.18692.24.camel@dragflick`) | S (docs) | idea |

## Regular expressions

| # | pg_weave issue | core change | workaround in the extension | history | size | status |
|---|---|---|---|---|---|---|
| C8 | G88 (approximate regex needs a second engine), G92 (TRE's three-atom limit) | approximate matching (`{~k}`) in core's ARE engine | vendored TRE (BSD-2) for any pattern containing `{~`; at most 3 approximate atoms | searched 2026-10-07: only general regex-engine threads ("Future of our regular expression code", 2012, which compares TRE among others; Joel Jacobson's 2021 performance work); no approximate-matching proposal | L | idea; long shot |
| C9 | G91 class (an extension prefilter disagreeing with the engine it prefilters) | expose ARE's compiled NFA (or a "required substrings" extraction) to extensions, as `pg_trgm` would also want | the extension parses the pattern with its own (pg_tre-derived) grammar and a whitelist of what both dialects agree on (`weave_regex_narrowable()`) | `pg_trgm`'s regex support walks core's colour NFA via `regexport.h` -- an existing, narrower API worth reading before proposing anything | M | idea |

## How to add a row

When an issue's fix in pg_weave is a workaround for something core could do better, add a
row in the same commit as the workaround: the GAPS / PHASES id, the core change in one
sentence, the workaround and where it lives, and a -hackers search (the `postgresq` MCP
server, or the archives) for prior art. A row with no history search says "not searched yet"
rather than nothing.
