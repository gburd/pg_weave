# Migrating from pgvector, tsvector/GIN, and pg_trgm

Status: **specified, unimplemented.** Phase **M** in `doc/PHASES.md`.

## Why this document is not "polish"

Performance moves nobody. A DBA running pgvector + tsvector + pg_trgm in
production will not rewrite an application's queries for a better benchmark, no
matter how good the number is. They will consider a change that is `DROP
EXTENSION` / `CREATE EXTENSION` plus an index swap, and they will not consider
anything else.

So the compatibility surface is a first-class deliverable with its own phase and
its own gate, and the gate is: **a documented, tested path from a live
pgvector + tsvector + pg_trgm schema to pg_weave with zero application query
changes.**

## The `vector` type collision, and the decision

pgvector defines a type named `vector`. Defining a second type with that name
makes pg_weave and pgvector **mutually exclusive in one database**, which
forecloses incremental migration — the only kind anyone actually does.
pg_turbovec made this choice and it is the wrong one.

**Decision: coexist.** pg_weave's own type is `wvec`
(`include/weave/vector.h`), and casts to and from pgvector's `vector` are created
conditionally when pgvector is present. `WVec`'s layout deliberately mirrors
pgvector's so the cast is a header rewrite, not an element loop.

Gate M2: `CREATE EXTENSION vector; CREATE EXTENSION pg_weave;` both succeed in one
database, with working casts in both directions.

## Operator compatibility

Operator and strategy assignment follows pgvector exactly, so a query written
against pgvector keeps working after the index swap:

| operator | meaning | strategy |
|---|---|---|
| `<->` | L2 distance | 1 |
| `<#>` | negative inner product | 2 |
| `<=>` | cosine distance | 3 |
| `<+>` | L1 distance | 4 |

`<=>` is also the lexical channel's rank operator, on `wdoc <=> wquery`. There is
no ambiguity: operators are resolved by operand type. It does mean anyone reading
a query has to look at the operand types to know which channel is involved, which
is a readability cost accepted in exchange for pgvector compatibility.

`<+>` (L1) is accepted but **exact-only**: the quantizer is built around inner
products and L1 admits no useful compressed-domain bound. The planner costs it as
a full scan and the graph is not used. Better to say that here than to have
someone discover it from a slow query.

## Full-text compatibility

`tsvector` and `tsquery` casts, plus a `@@`-compatible operator, so existing GIN
queries run unchanged. The lexical channel already routes analysis through
PostgreSQL's own `regconfig` machinery — snowball stemmers, ispell, synonym and
thesaurus dictionaries, stopword lists — so language support is *inherited* rather
than reimplemented, and a migrated query gets the same lexemes it did before.

Gate M3: a corpus of real `to_tsquery` queries returns identical row sets before
and after.

Note the honest gap: `tsquery` → `wquery` is a helper plus a cast, not a
transparent rewrite, because `wquery` supports constructs (`NEAR`, `term~k`,
`/re/`) that `tsquery` has no syntax for and `tsquery` has weight-class
constructs that need mapping. Tracked in `doc/PHASES.md` under inherited debt.

## Trigram compatibility

`%`, `similarity()`, and `word_similarity()` over the opt-in `cgram` channel.
This is the one place where pg_weave does **not** claim to be better: pg_trgm's
GIN over corpus character trigrams is close to optimal for unanchored cross-token
substring search, and with `cgram` enabled pg_weave is not smaller. See
`doc/specs/FUZZY_CHANNEL.md` §6.

The migration advice, therefore, is honest and specific: migrate `pg_trgm` usage
to pg_weave when the patterns are token-aligned (prefix, fuzzy word match, regex
within a token), and keep `pg_trgm` when they are genuinely unanchored substring
searches over long text.

## The swap functions

```sql
SELECT weave_migrate_from_pgvector('items_embedding_idx');
SELECT weave_migrate_from_gin('docs_body_idx');
```

Each builds the replacement index, verifies query results match on a sample, and
swaps in place. Gate M5 is a TAP test that builds a pgvector index, migrates, and
verifies identical query results — not merely that the migration ran.

## Health checking

```sql
SELECT * FROM weave_check('docs_weave');       -- per-channel invariants
SELECT weave_index_degraded('docs_weave');     -- one boolean a monitor can alert on
```

`weave_check()` must return something a DBA can act on, not a dump. The invariant
list is in `doc/specs/SEGMENT_FORMAT.md` §9. Gate M6 is a corruption TAP test that
injects each fault class and confirms detection.

## What migration does not fix

The operational-simplicity loss in `doc/ARCHITECTURE.md` §8.4 is real and
permanent. pgvector is ~15k lines of code doing one thing; pg_weave is an order of
magnitude larger doing five. The mitigations are per-channel opt-in — so unused
machinery is *absent* from the index rather than merely idle — and a
`weave_check()` that is genuinely useful. Neither makes it as simple as pgvector,
and the README should not imply otherwise.
