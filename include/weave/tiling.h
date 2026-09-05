/*-------------------------------------------------------------------------
 *
 * tiling.h -- Navarro tiling API for k>0 trigram extraction
 *
 * Imported from pg_tre e03d6a8 (MIT, same author) and renamed into the
 * weave namespace.  See doc/specs/IMPORT_pg_tre.md for the mapping and
 * doc/CHANNELS.md for how this fits the fuzzy/regex channel.
 *
 * Copyright (c) 2025-2026, Gregory Burd
 *
 *-------------------------------------------------------------------------
 */

/*
 * include/weave/tiling.h - Navarro tiling API for k>0 extraction.
 *
 * Phase 5: partition a pattern's trigram spine into k+1 tiles for
 * approximate matching via the pigeonhole principle.
 */

#ifndef WEAVE_TILING_H
#define WEAVE_TILING_H

#include "postgres.h"
#include "nodes/memnodes.h"

typedef struct RegexAst RegexAst;
typedef struct TrigramQuery TrigramQuery;

/*
 * Extract the trigram spine from an AST and tile it into k+1 groups.
 * Returns a TrigramQuery in DNF mode (OR across tiles, each tile is an
 * AND of its trigrams).  Returns false if extraction fails (sets
 * out->always_true = true).
 */
extern bool pg_weave_tile_query(const RegexAst *ast, int32 k,
                              TrigramQuery *out, MemoryContext cxt);

#endif /* WEAVE_TILING_H */
