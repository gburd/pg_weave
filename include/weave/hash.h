/*-------------------------------------------------------------------------
 *
 * hash.h -- trigram hash and order-preserving trigram-key functions
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
 * include/weave/hash.h - hash functions for pg_weave.
 *
 * Phase 2 uses a simple 64-bit hash for byte trigrams.  Phase 3.5 extends
 * to Unicode codepoint trigrams for proper multi-byte UTF-8 support.
 */

#ifndef WEAVE_HASH_H
#define WEAVE_HASH_H

#include "postgres.h"

/*
 * Hash a codepoint trigram (3 int32 values) to a 64-bit value.
 * This is the primary interface for Phase 3.5+.
 */
extern uint64 pg_weave_hash_trigram_cp(const int32 cp[3]);

/*
 * Hash a byte trigram (3 bytes) to a 64-bit value.
 * Legacy interface: for ASCII text, equivalent to pg_weave_hash_trigram_cp
 * with each byte treated as a codepoint.
 */
extern uint64 pg_weave_hash_trigram(const uint8 *trigram);

/*
 * Order-preserving trigram key (3.2.0, for the SuRF range filter).
 *
 * Packs a 3-codepoint trigram into a single uint64 whose unsigned
 * ordering equals the lexicographic ordering of the trigram's codepoint
 * sequence.  Each Unicode codepoint fits in 21 bits (max 0x10FFFF), so
 * three of them fit in 63 bits:
 *
 *     key = (cp0 << 42) | (cp1 << 21) | cp2
 *
 * Unlike pg_weave_hash_trigram_cp (a murmur hash that destroys order), this
 * key lets a text prefix map to a contiguous trigram-key range, which is
 * what the SuRF filter exploits for LIKE / ^-anchored prefix pruning.
 */
#define WEAVE_CP_BITS      21
#define WEAVE_CP_MASK      ((uint64) ((1u << WEAVE_CP_BITS) - 1))

static inline uint64
pg_weave_trigram_key_cp(const int32 cp[3])
{
	return (((uint64) (cp[0] & WEAVE_CP_MASK)) << (2 * WEAVE_CP_BITS)) |
	       (((uint64) (cp[1] & WEAVE_CP_MASK)) << (1 * WEAVE_CP_BITS)) |
	       ((uint64) (cp[2] & WEAVE_CP_MASK));
}

#endif /* WEAVE_HASH_H */
