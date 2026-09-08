/* config.h -- hand-maintained replacement for TRE's autoheader output.
 *
 * NOT UPSTREAM.  Written for pg_weave.  See
 * vendor/tre/local_includes/tre-config.h for the rationale and for the
 * upstream pin.  Only the vendored TRE translation units get this directory
 * on their include path (see the Makefile), so nothing else in pg_weave can
 * accidentally pick up a header named "config.h".
 *
 * TRE's own sources consult exactly the macros below; every other HAVE_ in
 * upstream's config.h.in is for the agrep/tests/NLS parts that pg_weave does
 * not build.  Deliberate choices:
 *
 *   - TRE_USE_ALLOCA is left undefined.  Upstream's alloca() path sizes a
 *     single stack allocation from the compiled NFA and the input length,
 *     which in a PostgreSQL backend is an unbounded stack overrun rather
 *     than an ereport.  pg_tre configures --without-alloca for the same
 *     reason; malloc() is the safe path.
 *   - HAVE_GETTEXT is left undefined: no NLS, so regerror() returns TRE's
 *     built-in English strings and libintl is not a link dependency.
 */

#ifndef WEAVE_VENDOR_TRE_BUILD_CONFIG_H
#define WEAVE_VENDOR_TRE_BUILD_CONFIG_H 1

/* Single source of truth for the macros that <tre.h> also needs. */
#include "local_includes/tre-config.h"

/* Wide-character classification. */
#define HAVE_WCTYPE_H 1
#define HAVE_WCTYPE 1
#define HAVE_ISWCTYPE 1
#define HAVE_ISWBLANK 1

/* Multibyte conversion.  mbrtowc/mbstate_t are C95. */
#define HAVE_MBRTOWC 1
#define HAVE_MBSTATE_T 1

/* wchar -> multibyte, used only by tre-parse.c's error path.  The restartable
   form is POSIX and absent from MSVC's CRT; fall back to wcstombs there. */
#ifdef _MSC_VER
#define HAVE_WCSTOMBS 1
#else
#define HAVE_WCSRTOMBS 1
#endif

/* isblank() is C99.  isascii() is POSIX-only (MSVC spells it _isascii), and
   TRE only uses it to narrow a fast path, so leaving it undefined on MSVC
   costs nothing. */
#define HAVE_ISBLANK 1
#ifndef _MSC_VER
#define HAVE_ISASCII 1
#endif

/* Field of regex_t in which TRE stashes its internal tre_tnfa_t.  Upstream's
   configure emits `value' whenever TRE_USE_SYSTEM_REGEX_H is off (which it
   is), because then regex_t is TRE's own struct from tre.h.  src/query/
   re_match.c reads preg->value directly for weave_pattern_num_states(). */
#define TRE_REGEX_T_FIELD value

/* Not used unless TRE_USE_ALLOCA is defined, which it is not. */
/* #undef HAVE_ALLOCA_H */
/* #undef HAVE_MALLOC_H */
/* #undef TRE_USE_ALLOCA */

/* No NLS. */
/* #undef HAVE_GETTEXT */

/* Upstream's package identification, from configure.ac. */
#define PACKAGE "tre"
#define PACKAGE_NAME "TRE"
#define PACKAGE_VERSION "0.9.0"

#endif							/* WEAVE_VENDOR_TRE_BUILD_CONFIG_H */
