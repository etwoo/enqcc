#include "passes/parse.h"

#include "lang/symbol.h"
#include "passes.h"
#include "passes/lex.h"
#include "passes/parse/alloc.h"
#include "passes/parse/constant.h"
#include "passes/parse/debug.h"
#include "passes/parse/declaration.h"
#include "passes/parse/entrypoint.h"
#include "passes/parse/resolve.h"
#include "passes/parse/token.h"
#include "sys/array.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static WARN_UNUSED result_t
flat_alloc(Arena *arena, struct flat **dst)
{
	assert(dst != NULL && *dst == NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_alloc_if_unset(Arena *arena, struct ast **dst)
{
	if (*dst == NULL) {
		checked_alloc(arena, dst, NODE_EXPRESSION_NULL);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_symbol(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_IDENTIFIER));

	struct string_view str = (**tok).val;
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		checked_alloc(arena, dst, NODE_EXPRESSION_VARIABLE_USAGE);
		(**dst).u.var.name = str;
		(**dst).u.var.unique = NOT_YET_UNIQUE;
		return RESULT_OK;
	}

	checked_alloc(arena, dst, NODE_EXPRESSION_FUNCTION_CALL);
	(**dst).u.call.identifier.name = str;
	(**dst).u.call.identifier.unique = NOT_YET_UNIQUE;

	assert(is_token_type(*tok, TOKEN_PAREN_OPEN));
	token_consume(tok);

	if (is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		token_consume(tok);
		return RESULT_OK; /* zero-arg function call */
	}

	struct flat **dst_args = &(**dst).u.call.args;
	while (true) {
		check(flat_alloc(arena, dst_args));
		check(parse_expr(arena, tok, &(**dst_args).car, 0));
		dst_args = &(**dst_args).cdr;

		if (!is_token_type(*tok, TOKEN_COMMA)) {
			break;
		}
		token_consume(tok);
	}

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_CALL_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED bool
parse_needs_weird_hack_for_cast_lhs_precedence(const struct ast *a)
{
	assert(a->node_type == NODE_EXPRESSION_CAST);
	switch (a->u.cast.expr->node_type) {
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		return true;
	default:
		break;
	}
	return false;
}

static WARN_UNUSED result_t
parse_factor(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(dst != NULL && *dst == NULL);
	size_t got_match = SIZE_MAX;

#define TO_CANDIDATE(nodet, tokent) {tokent, NODE_##nodet},
	struct {
		enum lex_tokentype token_type;
		enum ast_nodetype node_type;
	} prefix_ops[] = {FOREACH_AST_NODE_EXPRESSION_PREFIX_OP(TO_CANDIDATE)};
#undef TO_CANDIDATE
	for (size_t i = 0; i < ARRAY_SIZE(prefix_ops); ++i) {
		if (is_token_type(*tok, prefix_ops[i].token_type)) {
			got_match = i;
			break;
		}
	}

	if (got_match < SIZE_MAX) {
		assert(got_match < ARRAY_SIZE(prefix_ops));
		checked_alloc(arena, dst, prefix_ops[got_match].node_type);
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_constant(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_symbol(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN) &&
	           *tok != NULL && /* avoid NULL dereference on (**tok).next */
	           is_token_variable_type((**tok).next)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_EXPRESSION_CAST);
		check(parse_type(arena,
		                 PARSE_DECLARATOR_ABSTRACT,
		                 tok,
		                 &(**dst).u.cast.to_type,
		                 NULL));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_CAST_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
		check(parse_expr(arena, tok, &(**dst).u.cast.expr, 0));
		assert((**dst).u.cast.expr != NULL);
		if (parse_needs_weird_hack_for_cast_lhs_precedence(*dst)) {
			struct ast *cast_original = *dst;
			struct ast *assign_original = (**dst).u.cast.expr;
			struct ast *lhs_original =
				(**dst).u.cast.expr->u.op_binary.lhs;
			/* cast LHS of assignment, not assignment as a whole */
			(**dst).u.cast.expr->u.op_binary.lhs = cast_original;
			(**dst).u.cast.expr->u.op_binary.lhs->u.cast.expr =
				lhs_original;
			*dst = assign_original;
		}
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		checked_alloc(arena, dst, NODE_EXPRESSION_PAREN_ENCLOSED);
		token_consume(tok);
		check(parse_expr(arena, tok, &(**dst).u.op_unary.operand, 0));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
	} else {
		return make_result(ERR_PARSE_EXPR_EXPECT_REASONABLE);
	}

	assert(*dst != NULL);

	struct ast *post = NULL;
	if (is_token_type(*tok, TOKEN_PLUS_SIGN_PLUS_SIGN)) {
		checked_alloc(arena, &post, NODE_EXPRESSION_POSTINCREMENT);
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)) {
		checked_alloc(arena, &post, NODE_EXPRESSION_POSTDECREMENT);
		token_consume(tok);
	} else {
		return RESULT_OK;
	}
	/*
	 * Wrap the inner expr in postincrement/postdecrement.
	 */
	post->u.op_unary.operand = *dst;
	*dst = post;

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expr_check_next_token(Arena *arena,
                            const struct token *tok,
                            struct ast **a)
{
#define TO_CANDIDATE(nodet, tokent) {tokent, NODE_##nodet},
	struct {
		enum lex_tokentype token_type;
		enum ast_nodetype node_type;
	} infix_ops[] = {FOREACH_AST_NODE_EXPRESSION_INFIX_OP(TO_CANDIDATE)};
#undef TO_CANDIDATE
	for (size_t i = 0; i < ARRAY_SIZE(infix_ops); ++i) {
		if (is_token_type(tok, infix_ops[i].token_type)) {
			checked_alloc(arena, a, infix_ops[i].node_type);
			break;
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_ternary_middle(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_expr(arena, tok, dst, 0));
	if (!is_token_type(*tok, TOKEN_COLON)) {
		return make_result(ERR_PARSE_EXPR_EXPECT_COLON_IN_TERNARY_OP);
	}
	token_consume(tok);
	return RESULT_OK;
}

static const unsigned PRECEDENCE_INCREMENT = 10;

static WARN_UNUSED unsigned
get_precedence(const struct ast *a)
{
	unsigned precedence = 0;
	switch (a->node_type) {
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_AND:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_XOR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_OR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_LOGICAL_AND:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_LOGICAL_OR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		precedence += PRECEDENCE_INCREMENT;
		break;
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_BLOCK:
	case NODE_DECLARATION:
	case NODE_IF_ELSE:
	case NODE_LOOP:
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_SWITCH:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_CAST:
	case NODE_CONSTANT:
		assert(0); /* logic error in caller */
		break;
	}
	return precedence;
}

/*
 * Some references on precedence climbing:
 *
 * https://en.wikipedia.org/wiki/Operator-precedence_parser
 * https://eli.thegreenplace.net/2012/08/02/parsing-expressions-by-precedence-climbing
 * https://www.oilshell.org/blog/2016/11/01.html
 */
result_t
parse_expr(Arena *arena,
           const struct token **tok,
           struct ast **dst,
           unsigned minimum_precedence)
{
	struct ast *left = NULL;
	check(parse_factor(arena, tok, &left));

	while (true) {
		struct ast *bop = NULL;
		check(parse_expr_check_next_token(arena, *tok, &bop));
		if (bop == NULL) {
			break;
		}

		const bool is_right_associative =
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_ADD ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_SUB ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_MUL ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_DIV ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_REM ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_AND ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_OR ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_XOR ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_SL ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_SR ||
			bop->node_type == NODE_EXPRESSION_VARIABLE_ASSIGNMENT ||
			bop->node_type == NODE_EXPRESSION_TERNARY_CONDITIONAL;
		const unsigned inc = is_right_associative ? 0 : 1;

		const unsigned precedence = get_precedence(bop);
		if (precedence < minimum_precedence) {
			break;
		}

		token_consume(tok);

		struct ast *right = NULL;
		if (bop->node_type == NODE_EXPRESSION_TERNARY_CONDITIONAL) {
			struct ast *middle = NULL;
			check(parse_ternary_middle(arena, tok, &middle));
			check(parse_expr(arena, tok, &right, precedence + inc));
			bop->u.op_ternary.condition = left;
			bop->u.op_ternary.then_expr = middle;
			bop->u.op_ternary.else_expr = right;
		} else {
			check(parse_expr(arena, tok, &right, precedence + inc));
			bop->u.op_binary.lhs = left;
			bop->u.op_binary.rhs = right;
		}
		left = bop;
	}

	*dst = left;
	return RESULT_OK;
}

static result_t parse_stmt(Arena *arena,
                           const struct token **tok,
                           struct ast **dst,
                           bool *call_again) WARN_UNUSED;

result_t
parse_block(Arena *arena, const struct token **tok, struct ast **dst_outer)
{
	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	checked_alloc(arena, dst_outer, NODE_BLOCK);
	struct flat **dst = &(**dst_outer).u.block.statements;

	if (is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		token_consume(tok);
		check(flat_alloc(arena, dst));
		checked_alloc(arena, &(**dst).car, NODE_EXPRESSION_NULL);
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
			check(parse_stmt(arena, tok, &(**dst).car, &dummy));
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

static WARN_UNUSED result_t
parse_stmt_multi(Arena *arena, const struct token **tok, struct flat **dst)
{
	bool call_again = true;
	for (; call_again; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_stmt(arena, tok, &(**dst).car, &call_again));
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

	checked_alloc(arena, dst, NODE_IF_ELSE);
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
	checked_alloc(arena, dst_outer, NODE_BLOCK);
	struct flat **dst = &(**dst_outer).u.block.statements;
	check(flat_alloc(arena, dst));
	assert(*dst != NULL);

	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		check(parse_alloc_if_unset(arena, &(**dst).car));
		token_consume(tok);
	} else if (is_token_variable_type(*tok)) {
		check(parse_fn_or_var_declaration(arena, 0, tok, &(**dst).car));
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

	checked_alloc(arena, dst, NODE_LOOP);
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

	checked_alloc(arena, dst, NODE_SWITCH);
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

	if (!is_token_type(*tok, TOKEN_CONSTANT)) {
		return make_result(ERR_PARSE_CASE_EXPECT_CONSTANT);
	}

	checked_alloc(arena, dst, NODE_CASE);
	check(parse_constant(arena, tok, &(**dst).u.case_.constant));
	(**dst).u.case_.unique = UNSET_SWITCH_ID;

	if (!is_token_type(*tok, TOKEN_COLON)) {
		return make_result(ERR_PARSE_CASE_EXPECT_COLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_stmt(Arena *arena,
           const struct token **tok,
           struct ast **dst,
           bool *call_again)
{
	assert(dst != NULL && *dst == NULL);

	bool expect_semicolon_after = false;
	*call_again = false;

	if (is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_FUNCTION_RETURN_STATEMENT);
		check(parse_expr(arena, tok, &(**dst).u.op_unary.operand, 0));
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_EXPRESSION_NULL);
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
		checked_alloc(arena, dst, NODE_BREAK);
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_CONTINUE)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_CONTINUE);
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_GOTO) &&
	           is_token_type((**tok).next, TOKEN_IDENTIFIER)) {
		checked_alloc(arena, dst, NODE_GOTO);
		(**dst).u.goto_.target_label = (**tok).next->val;
		(**dst).u.goto_.target_unique = UNSET_LABEL_ID;
		token_consume(tok);
		token_consume(tok);
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER) &&
	           is_token_type((**tok).next, TOKEN_COLON)) {
		checked_alloc(arena, dst, NODE_LABEL);
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
		checked_alloc(arena, dst, NODE_CASE_DEFAULT);
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
	checked_alloc(arena, a, NODE_PROGRAM);

	struct flat **dst = &(**a).u.program.globals;
	for (; tok != NULL; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_fn_or_var_declaration(
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

result_t
cast_if(Arena *arena, const struct ctype *cast_to, struct ast **a)
{
	if (*a == NULL || ctype_is_equal(&(**a).expr_type, cast_to)) {
		return RESULT_OK;
	}
	struct ast *cast_wrap = NULL;
	checked_alloc(arena, &cast_wrap, NODE_EXPRESSION_CAST);
	check(ctype_copy(arena, cast_to, &cast_wrap->expr_type));
	check(ctype_copy(arena, cast_to, &cast_wrap->u.cast.to_type));
	cast_wrap->u.cast.expr = *a;
	*a = cast_wrap;
	return RESULT_OK;
}
