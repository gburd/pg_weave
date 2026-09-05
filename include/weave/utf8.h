/*-------------------------------------------------------------------------
 *
 * utf8.h -- UTF-8 codepoint streaming API
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
 * include/weave/utf8.h - UTF-8 codepoint streaming for trigram extraction.
 *
 * Phase 3.5: migrate from byte-based trigrams to codepoint-based trigrams.
 * Pure ASCII is unchanged (each byte is a codepoint). Multi-byte UTF-8
 * characters are decoded to int32 codepoints before hashing.
 *
 * BREAKING CHANGE: indexes built on byte trigrams must be REINDEXed after
 * upgrade. WEAVE_FORMAT_VERSION is bumped to detect this.
 */

#ifndef WEAVE_UTF8_H
#define WEAVE_UTF8_H

#include "postgres.h"

/*
 * Streaming UTF-8 decoder for trigram extraction.
 * Reads codepoints sequentially from a UTF-8 byte string.
 */
typedef struct PgWeaveCpStream
{
    const unsigned char *src;
    int src_len;
    int src_pos;
} PgWeaveCpStream;

/*
 * Initialize a codepoint stream over the given UTF-8 text.
 */
extern void pg_weave_cpstream_init(PgWeaveCpStream *s, const char *text, int len);

/*
 * Read the next codepoint from the stream.
 * Returns:
 *   0x0000..0x10FFFF: valid codepoint
 *   -1: end of stream
 *   -2: invalid UTF-8 sequence (ereport ERROR with context)
 */
extern int32 pg_weave_cpstream_next(PgWeaveCpStream *s);

/*
 * Return the current byte offset in the source string.
 * Used to record trigram positions (positions are byte offsets, not codepoint
 * indices, because TRE's recheck operates on byte strings).
 */
static inline int
pg_weave_cpstream_pos(const PgWeaveCpStream *s)
{
    return s->src_pos;
}

#endif /* WEAVE_UTF8_H */
