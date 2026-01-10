#include "passes/parse.h"

#include "lang/symbol.h"
#include "passes.h"
#include "passes/lex.h"
#include "passes/parse/alloc.h"
#include "passes/parse/block.h"
#include "passes/parse/constant.h"
#include "passes/parse/debug.h"
#include "passes/parse/declaration.h"
#include "passes/parse/expression.h"
#include "passes/parse/resolve.h"
#include "passes/parse/token.h"
#include "sys/array.h"
#include "sys/debug.h"

#include <assert.h>

static WARN_UNUSED result_t
parse_alloc_if_unset(Arena *arena, struct ast **dst)
{
	if (*dst == NULL) {
		check(parse_alloc_null_expr(arena, dst));
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_stmt_multi(Arena *arena, const struct token **tok, struct flat **dst)
{
	bool call_again = true;
	for (; call_again; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_stmt_one(arena, tok, &(**dst).car, &call_again));
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_if_else(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_IF));
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	check(parse_alloc(arena, dst, NODE_IF_ELSE));
	check(parse_expr(arena, tok, &(**dst).u.if_.condition, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	check(parse_stmt_multi(arena, tok, &(**dst).u.if_.then_clause));

	if (!is_token_type(*tok, TOKEN_KEYWORD_ELSE)) {
		return RESULT_OK;
	}
	token_consume(tok);

	check(parse_stmt_multi(arena, tok, &(**dst).u.if_.else_clause));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_loop_for_init(Arena *arena,
                    const struct token **tok,
                    struct ast **dst_outer)
{
	check(parse_alloc(arena, dst_outer, NODE_BLOCK));
	struct flat **dst = &(**dst_outer).u.block.statements;
	check(flat_alloc(arena, dst));
	assert(*dst != NULL);

	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		check(parse_alloc_null_expr(arena, &(**dst).car));
		token_consume(tok);
	} else if (is_token_variable_type(*tok)) {
		check(parse_fn_or_var_or_struct_declaration(arena,
		                                            0,
		                                            tok,
		                                            &(**dst).car));
	} else {
		check(parse_expr(arena, tok, &(**dst).car, 0));
		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_loop_do_while_suffix(Arena *arena,
                           const struct token **tok,
                           struct ast **dst)
{
	if (!is_token_type(*tok, TOKEN_KEYWORD_WHILE)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_WHILE);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	check(parse_expr(arena, tok, &(**dst).u.loop.postcond, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_loop(Arena *arena, const struct token **tok, struct ast **dst)
{
	enum {
		PARSE_LOOP_DO,
		PARSE_LOOP_WHILE,
		PARSE_LOOP_FOR,
	} loop_type = 0;
	if (is_token_type(*tok, TOKEN_KEYWORD_DO)) {
		loop_type = PARSE_LOOP_DO;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_WHILE)) {
		loop_type = PARSE_LOOP_WHILE;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_FOR)) {
		loop_type = PARSE_LOOP_FOR;
	} else {
		assert(0); /* logic error in caller */
	}
	token_consume(tok);

	if (loop_type == PARSE_LOOP_FOR || loop_type == PARSE_LOOP_WHILE) {
		if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN);
		}
		token_consume(tok);
	}

	if (loop_type == PARSE_LOOP_FOR) {
		/*
		 * Create block in case for-init declares a loop variable.
		 */
		check(parse_loop_for_init(arena, tok, dst));
		assert((**dst).node_type == NODE_BLOCK);
		/*
		 * Arrange for loop body to be allocated into next block item,
		 * following first item that holds loop variable declaration.
		 */
		check(flat_alloc(arena, &(**dst).u.block.statements->cdr));
		dst = &(**dst).u.block.statements->cdr->car;
	}

	check(parse_alloc(arena, dst, NODE_LOOP));
	(**dst).u.loop.label_end = UNSET_LOOP_ID;
	(**dst).u.loop.label_continue = UNSET_LOOP_ID;
	(**dst).u.loop.label_start = UNSET_LOOP_ID;

	if ((loop_type == PARSE_LOOP_FOR &&
	     !is_token_type(*tok, TOKEN_SEMICOLON)) ||
	    loop_type == PARSE_LOOP_WHILE) {
		check(parse_expr(arena, tok, &(**dst).u.loop.precond, 0));
	}

	if (loop_type == PARSE_LOOP_FOR) {
		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);

		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			check(parse_expr(arena, tok, &(**dst).u.loop.incr, 0));
		}
	}

	if (loop_type == PARSE_LOOP_FOR || loop_type == PARSE_LOOP_WHILE) {
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
	}

	check(parse_stmt_multi(arena, tok, &(**dst).u.loop.body));

	if (loop_type == PARSE_LOOP_DO) {
		check(parse_loop_do_while_suffix(arena, tok, dst));
	}

	check(parse_alloc_if_unset(arena, &(**dst).u.loop.precond));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.incr));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.postcond));

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_switch(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_SWITCH));
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	check(parse_alloc(arena, dst, NODE_SWITCH));
	(**dst).u.switch_.label_default = UNSET_SWITCH_ID;
	(**dst).u.switch_.label_end = UNSET_SWITCH_ID;
	check(parse_expr(arena, tok, &(**dst).u.switch_.control, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	check(parse_stmt_multi(arena, tok, &(**dst).u.switch_.body));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_case(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_CASE));
	token_consume(tok);

	if (!can_parse_constant(*tok)) {
		return make_result(ERR_PARSE_CASE_EXPECT_CONSTANT);
	}

	check(parse_alloc(arena, dst, NODE_CASE));
	check(parse_constant(arena, tok, &(**dst).u.case_.constant));
	(**dst).u.case_.unique = UNSET_SWITCH_ID;

	if (!is_token_type(*tok, TOKEN_COLON)) {
		return make_result(ERR_PARSE_CASE_EXPECT_COLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

result_t
parse_stmt_one(Arena *arena,
               const struct token **tok,
               struct ast **dst,
               bool *call_again)
{
	assert(dst != NULL && *dst == NULL);

	bool expect_semicolon_after = false;
	*call_again = false;

	if (is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_FUNCTION_RETURN_STATEMENT));
		if (is_token_type(*tok, TOKEN_SEMICOLON)) {
			check(parse_alloc_null_expr(
				arena,
				&(**dst).u.op_unary.operand));
		} else {
			check(parse_expr(arena,
			                 tok,
			                 &(**dst).u.op_unary.operand,
			                 0));
		}
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		token_consume(tok);
		check(parse_alloc_null_expr(arena, dst));
	} else if (is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		check(parse_block(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_IF)) {
		check(parse_if_else(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_DO) ||
	           is_token_type(*tok, TOKEN_KEYWORD_WHILE) ||
	           is_token_type(*tok, TOKEN_KEYWORD_FOR)) {
		check(parse_loop(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_BREAK)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_BREAK));
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_CONTINUE)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_CONTINUE));
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_GOTO) &&
	           is_token_type((**tok).next, TOKEN_IDENTIFIER)) {
		check(parse_alloc(arena, dst, NODE_GOTO));
		(**dst).u.goto_.target_label = (**tok).next->val;
		(**dst).u.goto_.target_unique = UNSET_LABEL_ID;
		token_consume(tok);
		token_consume(tok);
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER) &&
	           is_token_type((**tok).next, TOKEN_COLON)) {
		check(parse_alloc(arena, dst, NODE_LABEL));
		(**dst).u.label.name = (**tok).val;
		(**dst).u.label.unique = UNSET_LABEL_ID;
		token_consume(tok);
		token_consume(tok);
		*call_again = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_SWITCH)) {
		check(parse_switch(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_CASE)) {
		check(parse_case(arena, tok, dst));
		*call_again = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_DEFAULT) &&
	           is_token_type((**tok).next, TOKEN_COLON)) {
		check(parse_alloc(arena, dst, NODE_CASE_DEFAULT));
		(**dst).u.case_.constant = NULL;
		(**dst).u.case_.unique = UNSET_SWITCH_ID;
		token_consume(tok);
		token_consume(tok);
		*call_again = true;
	} else {
		check(parse_expr(arena, tok, dst, 0));
		expect_semicolon_after = true;
	}

	if (expect_semicolon_after) {
		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);
	}

	return RESULT_OK;
}

result_t
parse_init(Arena *arena,
           const struct token *tok,
           struct ast **a,
           long long int *generator)
{
	check(parse_alloc(arena, a, NODE_PROGRAM));

	struct flat **dst = &(**a).u.program.globals;
	for (; tok != NULL; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_fn_or_var_or_struct_declaration(
			arena,
			PARSE_DECLARATION_ACCEPT_FUNCTION,
			&tok,
			&(**dst).car));
	}

	struct symbol *symbols = NULL;

	struct flat *cursor = (**a).u.program.globals;
	for (; generator != NULL && cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		switch (cursor->car->node_type) {
		case NODE_FUNCTION:
			check(resolve_function(arena, cursor->car, &symbols));
			break;
		case NODE_DECLARATION:
			check(resolve_declaration(arena,
			                          cursor->car,
			                          &symbols,
			                          SYMBOL_LINKAGE_EXTERNAL));
			break;
		case NODE_STRUCT:
			// assert(0 && "TODO resolve_struct_declaration()");
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
	}

	if (generator != NULL && symbols != NULL) {
		*generator = symbols->cookie;
	}
	return RESULT_OK;
}
