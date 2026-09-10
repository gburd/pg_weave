/* tre-config.h -- hand-maintained replacement for TRE's autoconf output.
 *
 * NOT UPSTREAM.  Written for pg_weave; everything else under vendor/tre/ is
 * laurikari/tre @ f864ed08a7499865c75b8b59c0cf39a9d59133fe (BSD-2, see
 * vendor/tre/LICENSE) plus vendor/tre/patches/tre-progress-hook.patch.
 *
 * Upstream generates this file from local_includes/tre-config.h.in by running
 * ./utils/autogen.sh && ./configure.  pg_weave deliberately does not run
 * autotools during its own build: TRE's sources are compiled as ordinary
 * translation units by the PGXS Makefile, so the handful of feature macros
 * TRE actually consults are settled here instead.  This file is the
 * "generated file checked in with its provenance" of doc/LICENSING.md rule 5.
 *
 * This header must contain only the bare minimum of definitions *without*
 * the TRE_ prefix, because it is pulled in by the public <tre.h>.
 */

#ifndef WEAVE_VENDOR_TRE_CONFIG_H
#define WEAVE_VENDOR_TRE_CONFIG_H 1

/* Define to 1 if you have the <libutf8.h> header file. */
/* #undef HAVE_LIBUTF8_H */

/* Define to 1 if the system has the type `reg_errcode_t'.  It does not; TRE
   defines its own in tre.h. */
/* #undef HAVE_REG_ERRCODE_T */

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <wchar.h> header file.  C95; assumed. */
#define HAVE_WCHAR_H 1

/* Approximate (edit-distance) matching is the entire reason pg_weave vendors
   TRE: the fuzzy channel's verification step calls tre_reganexec(). */
#define TRE_APPROX 1

/* Multibyte and wide-character support: required, PostgreSQL text can be
   UTF-8 and the fuzzy channel counts edits in characters, not bytes. */
#define TRE_MULTIBYTE 1
#define TRE_WCHAR 1

/* pg_weave links TRE's objects directly into pg_weave.so and calls only the
   tre_-prefixed entry points, so the system-regex ABI aliases stay off. */
/* #undef TRE_SYSTEM_REGEX_H_PATH */
/* #undef TRE_USE_SYSTEM_REGEX_H */

/* TRE version, from vendor/tre/configure.ac (AC_INIT([TRE], [0.9.0])). */
#define TRE_VERSION "0.9.0"
#define TRE_VERSION_1 0
#define TRE_VERSION_2 9
#define TRE_VERSION_3 0

#endif							/* WEAVE_VENDOR_TRE_CONFIG_H */
