/*
 * test/hegel/pgshim_regex/postgres.h -- minimal backend-type shim.
 *
 * src/query/regex_tokens.c is the file under test.  It #includes the real
 * "postgres.h" the way every backend translation unit does, but everything
 * it actually needs from that header is a handful of typedefs and
 * palloc0() -- it never calls ereport()/elog() (checked: the only error
 * path in that file sets ctx->syntax_error and snprintf()s ctx->errmsg,
 * both plain C).  So rather than pull in a live backend, this directory is
 * put ahead of the real PostgreSQL include path (see the -I order in
 * test/hegel/test_regex_dash.c's build recipe) and supplies just that
 * surface, the same trick test_lev.c uses for pg_weave_lev.c.
 *
 * If regex_tokens.c ever grows a real ereport()/elog() call, this shim
 * will fail to link rather than silently stubbing it out -- the right
 * failure mode.
 */
#ifndef PGSHIM_POSTGRES_H
#define PGSHIM_POSTGRES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef size_t   Size;

static inline void *
palloc0(Size sz)
{
	void *p = malloc(sz);

	if (p != NULL)
		memset(p, 0, sz);
	return p;
}

#endif							/* PGSHIM_POSTGRES_H */
