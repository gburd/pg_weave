/*-------------------------------------------------------------------------
 *
 * pattern_cache.h -- per-session compiled-regex cache API
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
 * pattern_cache.h - Per-session LRU cache of compiled TRE regex patterns.
 *
 * Must be included from a translation unit that includes postgres.h.
 */

#ifndef WEAVE_CACHE_H
#define WEAVE_CACHE_H

/*
 * Initialize the cache (idempotent, called from _PG_init or on first use).
 */
void weave_cache_init(void);

/*
 * Look up a compiled regex for the given pattern. Returns the cached
 * compiled handle if found, otherwise compiles and caches it.
 * Raises ereport(ERROR) on compile failure.
 */
void *weave_cache_lookup(const char *pattern, int pattern_len);

/*
 * Like weave_cache_lookup(), but pins the returned compiled handle so the
 * LRU cannot evict/free it until weave_cache_release() is called with the
 * same handle.
 */
void *weave_cache_lookup_pinned(const char *pattern, int pattern_len);

/*
 * Release a pin taken by weave_cache_lookup_pinned(). Matches by pointer
 * identity; a NULL handle is a no-op.
 */
void weave_cache_release(void *compiled);

#endif /* WEAVE_CACHE_H */
