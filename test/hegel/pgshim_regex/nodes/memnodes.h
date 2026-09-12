/*
 * test/hegel/pgshim_regex/nodes/memnodes.h -- minimal MemoryContext shim.
 *
 * include/weave/regex_ast.h includes the real "nodes/memnodes.h" solely to
 * name the MemoryContext type for WeaveParseCtx::mcxt.  regex_tokens.c (the
 * file under test) never dereferences that field, so an opaque pointer
 * typedef is enough to make the struct compile.
 */
#ifndef PGSHIM_MEMNODES_H
#define PGSHIM_MEMNODES_H

typedef struct MemoryContextData *MemoryContext;

#endif							/* PGSHIM_MEMNODES_H */
