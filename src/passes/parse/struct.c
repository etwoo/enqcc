#include "passes/parse/struct.h"

#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/declaration.h"
#include "passes/parse/token.h"
#include "sys/debug.h"

#include <assert.h>

result_t
parse_struct_declaration(Arena *arena,
                         const struct token **tok,
                         struct ast **dst_struct)
{
	check(parse_alloc(arena, dst_struct, NODE_STRUCT));

	assert(is_token_struct_prefix(*tok));
	assert(is_token_type(*tok, TOKEN_KEYWORD_STRUCT));
	token_consume(tok);

	assert(is_token_type(*tok, TOKEN_IDENTIFIER));
	// TODO: set ast_symbol.stype?
	(**dst_struct).u.struct_.identifier.name = (**tok).val;
	token_consume(tok);

	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		assert((**dst_struct).u.struct_.members == NULL);
		token_consume(tok);
		return RESULT_OK;
	}

	assert(is_token_type(*tok, TOKEN_BRACE_OPEN));
	token_consume(tok);

	struct flat **dst = &(**dst_struct).u.struct_.members;
	while (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		check(flat_alloc(arena, dst));
		check(parse_fn_or_var_or_struct_declaration(arena,
		                                            0,
		                                            tok,
		                                            &(**dst).car));
		dst = &(**dst).cdr;
	}

	assert(is_token_type(*tok, TOKEN_BRACE_CLOSE));
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_STRUCT_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	if ((**dst_struct).u.struct_.members == NULL) {
		return make_result(ERR_PARSE_STRUCT_EMPTY_INVALID);
	}
	return RESULT_OK;
}
