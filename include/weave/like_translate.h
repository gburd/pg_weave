/*-------------------------------------------------------------------------
 *
 * like_translate.h -- LIKE/literal to regex lowering API
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
 * include/weave/like_translate.h - LIKE/literal -> regex lowering
 * for Phase A / A1 operator-class acceleration.
 */
#ifndef WEAVE_LIKE_TRANSLATE_H
#define WEAVE_LIKE_TRANSLATE_H

/*
 * Translate a SQL LIKE pattern (length `len`, escape char `escape`,
 * or '\0' for no escape processing) into a palloc'd regex string the
 * pg_weave parser accepts.  Unanchored so the trigram extractor can
 * harvest interior literal runs.
 */
extern char *pg_weave_like_to_regex(const char *like, int len, char escape);

/*
 * Translate a plain literal (for the `=` strategy) into a palloc'd
 * anchored regex matching exactly that string.
 */
extern char *pg_weave_literal_to_regex(const char *lit, int len);

#endif /* WEAVE_LIKE_TRANSLATE_H */
