/*-------------------------------------------------------------------------
 *
 * pg_weave_sm.h
 *		pg_weave's namespaced view of the vendored sparsemap library.
 *
 * Always include this instead of vendor/sm.h directly.  It defines
 * SPARSEMAP_PREFIX so every sparsemap public symbol is renamed to
 * __pg_weave_<name> (e.g. sm_add -> __pg_weave_sm_add).  This prevents dynamic
 * linker collisions if another extension in the same backend also links its
 * own copy of sparsemap.  The vendored sm.c defines the same prefix, so the
 * definitions and these declarations resolve to the same namespaced symbols.
 *
 * IDENTIFICATION
 *	  pg_weave_sm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WEAVE_SM_H
#define WEAVE_SM_H

#ifndef SPARSEMAP_PREFIX
#define SPARSEMAP_PREFIX __pg_weave_
#endif

/* expose the sm_t layout so callers can stack-allocate maps (sm_init/sm_open) */
#define SM_EXPOSE_STRUCT

#include "weave/sparsemap_impl.h"

#endif							/* WEAVE_SM_H */
