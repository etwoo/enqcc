#include "passes/parse/block.h"

#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/declaration.h"
#include "passes/parse/token.h"

#include <assert.h>

result_t
parse_block(Arena *arena, const struct token **tok, struct ast **dst_outer)
{
	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	check(parse_alloc(arena, dst_outer, NODE_BLOCK));
	struct flat **dst = &(**dst_outer).u.block.statements;

	if (is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		token_consume(tok);
		check(flat_alloc(arena, dst));
		check(parse_alloc(arena, &(**dst).car, NODE_EXPRESSION_NULL));
		return RESULT_OK;
	}

	while (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		check(flat_alloc(arena, dst));
		if (is_token_maybe_function_prefix(*tok)) {
			check(parse_fn_or_var_declaration(
				arena,
				PARSE_DECLARATION_ACCEPT_FUNCTION,
				tok,
				&(**dst).car));
		} else {
			bool dummy = false;
			check(parse_stmt_one(arena, tok, &(**dst).car, &dummy));
			/* can ignore dummy; we loop unconditionally here */
		}
		assert(*dst != NULL);
		dst = &(**dst).cdr;
	}

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

result_t
parse_stmt_multi(Arena *arena, const struct token **tok, struct flat **dst)
{
	bool call_again = true;
	for (; call_again; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_stmt_one(arena, tok, &(**dst).car, &call_again));
	}
	return RESULT_OK;
}
