# pg_weave

pg_weave is a PostgreSQL index access method, `weave`. One `CREATE INDEX` gives you six
kinds of retrieval over the same rows: BM25 ranked text search, vector nearest-neighbour
search, fuzzy (edit-distance) terms, regular expressions over tokens, prefix terms, and
substring (`LIKE '%...%'`) search through a character n-gram channel. A scalar column can
be indexed as a facet too. Every channel in a segment shares one document-id space. That
lets a selective filter, a lexical term or a facet such as `price < 100`, skip vector work
inside the scan instead of being applied afterwards.

**Status: 0.28.0, pre-1.0, not production-ready.** All six retrieval kinds ship. Two of
the three phase gates (Z and V) are not met, and some measured results are losses, listed
below. `doc/PRODUCTION_READINESS.md` is the gate list and `doc/GAPS.md` lists the known
defects.

## Install

PostgreSQL 17 or 18, built with PGXS:

```sh
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make install PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
```

```sql
CREATE EXTENSION pg_weave;
```

The extension is `trusted`, so a database owner can create it without superuser.
It needs no `shared_preload_libraries` entry.

TODO(with_llvm): result of the plain-install leg goes here.

## Example

TODO: paste from bench/aws/out/<run>/remote/readme_pg17.out.

## What it is fast at, and where it loses

TODO: from bench/RESULTS_*.md, each with its file.

## Read next

| file | what |
|---|---|
| `doc/ARCHITECTURE.md` | the design, the vocabulary, the four claims (§9) and the losses (§8) |
| `doc/PHASES.md` | every task, its spec and its gate |
| `doc/GAPS.md` | known defects and measured shortfalls |
| `doc/PRODUCTION_READINESS.md` | what has to be true before you should use it |
| `doc/specs/` | `FUSED_TOPK.md`, `VECTOR_CHANNEL.md`, `FUZZY_CHANNEL.md`, `SEGMENT_FORMAT.md` |
| `doc/CONVENTIONS.md`, `doc/TESTING.md` | how the code is written and tested |
| `AGENTS.md` | orientation for contributors, including the hard rules |

## License

PostgreSQL License. See `LICENSE`. Provenance per file is in `doc/LICENSING.md`.
