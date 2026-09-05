/*-------------------------------------------------------------------------
 *
 * parser.c -- regex parser driver wrapping the Lime-generated parser and tokenizer
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
 * src/query/parser.c - regex parser driver.
 *
 * Wraps the Lime-generated parser and tokenizer into a single
 * weave_parse_regex() entry point.
 */

#include "postgres.h"

#include "weave/regex_ast.h"
#include "utils/memutils.h"

#include <string.h>

/* Lime-generated parser interface from regex_grammar.c */
extern void *pg_weave_rx_parseAlloc(void *(*mallocProc)(size_t));
extern void pg_weave_rx_parseFree(void *parser, void (*freeProc)(void *));
extern void pg_weave_rx_parse(void *parser, int token_kind, WeaveToken token_value,
					  struct WeaveParseCtx *ctx);

/*
 * Parse a regex pattern into an AST.
 *
 * On success, ctx->root is set to the AST root and true is returned.
 * On failure, ctx->syntax_error is true and ctx->errmsg is filled.
 *
 * All AST nodes are allocated in ctx->mcxt.
 */
bool
weave_parse_regex(WeaveParseCtx *ctx, const char *pattern, int len)
{
	volatile void *parser_v = NULL;
	WeaveToken tok;
	int tok_kind;

	/* Initialize context */
	memset(ctx, 0, sizeof(WeaveParseCtx));
	ctx->input = pattern;
	ctx->input_len = len;
	ctx->mcxt = CurrentMemoryContext;
	ctx->root = NULL;
	ctx->syntax_error = false;
	ctx->tokenizer_state = NULL;

	/*
	 * The Lime-generated parser is allocated with malloc() and must be
	 * freed with the matching free().  Wrap the parse in PG_TRY/PG_CATCH
	 * so the parser is freed even when the tokenizer or AST builders raise
	 * via ereport(ERROR).  Without this, every malformed input that hits
	 * an ereport() leaks ~2 KB of parser state for the lifetime of the
	 * backend.  Discovered via libFuzzer.
	 */
	PG_TRY();
	{
		parser_v = pg_weave_rx_parseAlloc(malloc);
		if (parser_v == NULL)
			elog(ERROR, "failed to allocate regex parser");

		while ((tok_kind = weave_tokenize_next(ctx, &tok)) > 0)
		{
			if (ctx->syntax_error)
				break;

			pg_weave_rx_parse((void *) parser_v, tok_kind, tok, ctx);

			if (ctx->syntax_error)
				break;
		}

		if (tok_kind < 0)
		{
			/* Tokenizer error already set the error message. */
		}
		else if (!ctx->syntax_error)
		{
			/* Send EOF token (token 0). */
			memset(&tok, 0, sizeof(tok));
			pg_weave_rx_parse((void *) parser_v, 0, tok, ctx);
		}
	}
	PG_FINALLY();
	{
		if (parser_v != NULL)
			pg_weave_rx_parseFree((void *) parser_v, free);
	}
	PG_END_TRY();

	/* Check for syntax error */
	if (ctx->syntax_error)
		return false;

	/* Check that we got a root */
	if (ctx->root == NULL)
	{
		ctx->syntax_error = true;
		snprintf(ctx->errmsg, sizeof(ctx->errmsg), "empty pattern");
		return false;
	}

	return true;
}
