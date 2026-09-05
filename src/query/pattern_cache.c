/*-------------------------------------------------------------------------
 *
 * pattern_cache.c -- per-session LRU cache of compiled regex patterns
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
 * pattern_cache.c - Per-session LRU cache of compiled TRE regex patterns.
 *
 * Compiled patterns persist across queries in a session.  Fixed capacity
 * with LRU eviction.  Pattern strings are palloc'd in TopMemoryContext;
 * compiled regex handles are malloc'd by TRE and freed via weave_free_pattern.
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "weave/pattern_cache.h"
#include "weave/weave.h"
#include "weave/re_match.h"

#define WEAVE_CACHE_SLOTS 32

/*
 * Hard ceiling on the regex pattern length we will even attempt to
 * compile.  A pattern longer than this is almost certainly an attack or
 * a mistake; rejecting it early bounds parser-stack and NFA-compile
 * cost before weave_compile_pattern runs.
 */
#define WEAVE_MAX_PATTERN_LEN (64 * 1024)

typedef struct WeaveCacheSlot
{
    char   *pattern;        /* palloc'd copy in TopMemoryContext */
    int     pattern_len;
    void   *compiled;       /* opaque handle from weave_compile_pattern */
    uint64  last_used;
    int     pinned;         /* >0 => in use by a scan, must not evict/free */
} WeaveCacheSlot;

static WeaveCacheSlot *cache_slots = NULL;
static uint64 cache_clock = 0;

void
weave_cache_init(void)
{
    MemoryContext oldctx;

    if (cache_slots != NULL)
        return;

    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    cache_slots = palloc0(sizeof(WeaveCacheSlot) * WEAVE_CACHE_SLOTS);
    MemoryContextSwitchTo(oldctx);
}

static void
evict_slot(WeaveCacheSlot *slot)
{
    if (slot->pattern != NULL)
    {
        pfree(slot->pattern);
        slot->pattern = NULL;
    }
    if (slot->compiled != NULL)
    {
        weave_free_pattern(slot->compiled);
        slot->compiled = NULL;
    }
    slot->pattern_len = 0;
    slot->last_used = 0;
    slot->pinned = 0;
}

/*
 * Internal lookup.  When pin is true, the returned slot's compiled
 * handle is pinned (refcount++) so it cannot be evicted or freed until
 * a matching weave_cache_release().  A pinned entry held across a long,
 * re-entrant scan would otherwise be evicted by the LRU once >= 32
 * other patterns are compiled, freeing the handle out from under the
 * caller (use-after-free).
 *
 * On a miss with every slot pinned we compile the pattern but do NOT
 * cache it (out_uncached set to the fresh handle); the caller is
 * responsible for freeing such a handle via weave_cache_release().
 */
static void *
weave_cache_lookup_internal(const char *pattern, int pattern_len, bool pin)
{
    int             i;
    WeaveCacheSlot   *target;
    uint64          min_used;
    int             weave_err;
    void           *compiled;
    MemoryContext   oldctx;

    weave_cache_init();

    if (pattern_len < 0 || pattern_len > WEAVE_MAX_PATTERN_LEN)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("pg_weave: regex pattern length %d exceeds maximum %d",
                        pattern_len, WEAVE_MAX_PATTERN_LEN)));

    /* Search existing entries */
    for (i = 0; i < WEAVE_CACHE_SLOTS; i++)
    {
        WeaveCacheSlot *slot = &cache_slots[i];

        if (slot->compiled == NULL)
            continue;
        if (slot->pattern_len == pattern_len &&
            memcmp(slot->pattern, pattern, pattern_len) == 0)
        {
            slot->last_used = ++cache_clock;
            if (pin)
                slot->pinned++;
            return slot->compiled;
        }
    }

    /* Cache miss: compile pattern.  Arm a wall-clock compile deadline
     * (pg_weave.compile_timeout_ms) so a pathological bounded-repetition
     * pattern cannot spin the backend uninterruptibly inside TRE's AST
     * expansion. */
    pg_weave_arm_compile_deadline(0);
    PG_TRY();
    {
        compiled = weave_compile_pattern(pattern, pattern_len, &weave_err);
    }
    PG_FINALLY();
    {
        pg_weave_disarm_compile_deadline();
    }
    PG_END_TRY();
    /* Raise a distinct timeout error if the compile aborted on the
     * deadline (weave_compile_pattern returns NULL in that case too). */
    pg_weave_check_compile_timeout();
    if (compiled == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
                 errmsg("invalid regular expression: %s",
                        weave_errmsg(weave_err))));

    /*
     * Reject patterns whose compiled automaton is large enough to make
     * matching pathologically slow.  pg_weave.max_nfa_states is the
     * documented DoS guardrail; enforce it here, before the pattern can
     * ever reach the match path or be cached.  Per-string match cost is
     * roughly O(num_states * string_len * max_cost), so an unbounded
     * state count is the primary lever an attacker has.
     */
    {
        int nstates = weave_pattern_num_states(compiled);

        /*
         * H5 hardening: num_states is a plain int read out of TRE's
         * internal tnfa.  A healthy compile yields a small non-negative
         * count, but treat a negative value (which would otherwise slip
         * past the `>` guard and disable the DoS cap) as corruption and
         * refuse the pattern.  -1 specifically means "no internal NFA",
         * which should never happen for a successfully compiled handle.
         */
        if (nstates < 0)
        {
            weave_free_pattern(compiled);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
                     errmsg("pg_weave: compiled regex reported an invalid "
                            "NFA state count (%d)", nstates)));
        }

        if (nstates > pg_weave_max_nfa_states)
        {
            weave_free_pattern(compiled);
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_weave: regex compiles to %d NFA states, "
                            "exceeding pg_weave.max_nfa_states = %d",
                            nstates, pg_weave_max_nfa_states),
                     errhint("Simplify the pattern or raise "
                             "pg_weave.max_nfa_states.")));
        }
    }

    /*
     * Find an empty slot, or the LRU slot for eviction.  Pinned slots
     * are skipped: their compiled handle is live in some scan loop and
     * freeing it would be a use-after-free.
     */
    target = NULL;
    min_used = 0;
    for (i = 0; i < WEAVE_CACHE_SLOTS; i++)
    {
        if (cache_slots[i].compiled == NULL)
        {
            target = &cache_slots[i];
            break;
        }
        if (cache_slots[i].pinned > 0)
            continue;
        if (target == NULL || cache_slots[i].last_used < min_used)
        {
            min_used = cache_slots[i].last_used;
            target = &cache_slots[i];
        }
    }

    /*
     * Every slot is pinned (>= 32 patterns live in concurrent/re-entrant
     * scans).  Don't evict anything; hand back the freshly compiled
     * handle uncached.  When pinned, the caller owns it and must free it
     * via weave_cache_release(); when not pinned this leaks one compile,
     * which is preferable to corrupting a live entry and is bounded by
     * the (rare) all-pinned condition.
     */
    if (target == NULL)
        return compiled;

    evict_slot(target);

    /* Store the new entry */
    oldctx = MemoryContextSwitchTo(TopMemoryContext);
    target->pattern = palloc(pattern_len + 1);
    memcpy(target->pattern, pattern, pattern_len);
    target->pattern[pattern_len] = '\0';
    MemoryContextSwitchTo(oldctx);

    target->pattern_len = pattern_len;
    target->compiled = compiled;
    target->last_used = ++cache_clock;
    target->pinned = pin ? 1 : 0;

    return compiled;
}

void *
weave_cache_lookup(const char *pattern, int pattern_len)
{
    return weave_cache_lookup_internal(pattern, pattern_len, false);
}

/*
 * Like weave_cache_lookup(), but pins the returned compiled handle so the
 * LRU cannot evict/free it until weave_cache_release() is called with the
 * same handle.  Use this when the handle is held across calls that can
 * re-enter the cache (e.g. a scan loop that compiles other patterns).
 */
void *
weave_cache_lookup_pinned(const char *pattern, int pattern_len)
{
    return weave_cache_lookup_internal(pattern, pattern_len, true);
}

/*
 * Release a pin taken by weave_cache_lookup_pinned().  Matches the handle
 * by pointer identity.  If the handle was returned uncached (all slots
 * pinned at lookup time), it is not found in any slot and is freed here.
 * A NULL handle is a no-op.
 */
void
weave_cache_release(void *compiled)
{
    int i;

    if (compiled == NULL || cache_slots == NULL)
        return;

    for (i = 0; i < WEAVE_CACHE_SLOTS; i++)
    {
        if (cache_slots[i].compiled == compiled)
        {
            if (cache_slots[i].pinned > 0)
                cache_slots[i].pinned--;
            return;
        }
    }

    /* Not cached: an uncached handle handed out under all-pinned. */
    weave_free_pattern(compiled);
}
