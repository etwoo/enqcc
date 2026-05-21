#include "passes/parse/expression.h"

#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/constant.h"
#include "passes/parse/declaration.h"
#include "passes/parse/token.h"
#include "sys/array.h"

#include <assert.h>
#include <inttypes.h>

static WARN_UNUSED result_t
parse_sizeof_with_parens(Arena *arena,
                         const struct token **tok,
                         struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_SIZEOF));
	token_consume(tok);
	assert(is_token_type(*tok, TOKEN_PAREN_OPEN));
	token_consume(tok);

	check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_SIZE_OF));

	const struct token *rewind = *tok;
	struct ctype tmp = {0};

	auto_result try_type =
		parse_type(arena, PARSE_DECLARATOR_ABSTRACT, tok, &tmp, NULL);
	if (try_type.err == OK) {
		check(parse_alloc_null_expr(arena,
		                            &(**dst).u.op_unary.operand));
		/* override CTYPE_VOID for NODE_EXPRESSION_NULL */
		check(ctype_copy(arena,
		                 &tmp,
		                 &(**dst).u.op_unary.operand->expr_type));
	} else {
		*tok = rewind;
		check(parse_expr(arena, tok, &(**dst).u.op_unary.operand, 0));
	}

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_SIZEOF_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_symbol(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_IDENTIFIER));

	struct string_view str = (**tok).val;
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_VARIABLE_USAGE));
		(**dst).u.var.name = str;
		(**dst).u.var.unique = NOT_YET_UNIQUE;
		return RESULT_OK;
	}

	check(parse_alloc(arena, dst, NODE_EXPRESSION_FUNCTION_CALL));
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
parse_postfix(Arena *arena, const struct token **tok, struct ast **dst)
{
	struct ast *post = NULL;
	if (is_token_type(*tok, TOKEN_PLUS_SIGN_PLUS_SIGN)) {
		check(parse_alloc(arena, &post, NODE_EXPRESSION_POSTINCREMENT));
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)) {
		check(parse_alloc(arena, &post, NODE_EXPRESSION_POSTDECREMENT));
		token_consume(tok);
	}

	/*
	 * Wrap the inner expr in postincrement/postdecrement.
	 */
	if (post != NULL) {
		post->u.op_unary.operand = *dst;
		*dst = post;
	}

	return RESULT_OK;
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

	if (got_match < SIZE_MAX &&
	    prefix_ops[got_match].node_type == NODE_EXPRESSION_UNARY_SIZE_OF &&
	    is_token_type((**tok).next, TOKEN_PAREN_OPEN)) {
		check(parse_sizeof_with_parens(arena, tok, dst));
		/* non-parenthesized sizeof handled below */
	} else if (got_match < SIZE_MAX) {
		assert(got_match < ARRAY_SIZE(prefix_ops));
		check(parse_alloc(arena, dst, prefix_ops[got_match].node_type));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (can_parse_constant(*tok)) {
		check(parse_constant(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_CONSTANT_STR)) {
		check(parse_alloc(arena, dst, NODE_CONSTANT_STR));
		(**dst).u.str = (**tok).val;
		check(ctype_alloc_str_literal(arena,
		                              &(**tok).val,
		                              &(**dst).expr_type));
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_symbol(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN) &&
	           is_token_variable_type((**tok).next)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_EXPRESSION_CAST));
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
		check(parse_alloc(arena, dst, NODE_EXPRESSION_PAREN_ENCLOSED));
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
	check(parse_postfix(arena, tok, dst));

	while (is_token_type(*tok, TOKEN_SQUARE_BRACKET_OPEN)) {
		token_consume(tok);

		struct ast *postfix = NULL;
		check(parse_alloc(arena, &postfix, NODE_EXPRESSION_SUBSCRIPT));

		/* make array subscript expr into parent of prev/next exprs */
		postfix->u.op_binary.lhs = *dst;
		check(parse_expr(arena, tok, &postfix->u.op_binary.rhs, 0));
		*dst = postfix;

		if (!is_token_type(*tok, TOKEN_SQUARE_BRACKET_CLOSE)) {
			return make_result(
				ERR_PARSE_EXPR_EXPECT_TOKEN_SQ_BRACKET_CLOSE);
		}
		token_consume(tok);
	}

	check(parse_postfix(arena, tok, dst));
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
			check(parse_alloc(arena, a, infix_ops[i].node_type));
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
	case NODE_EXPRESSION_INITIALIZER:
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_UNARY_SIZE_OF:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
	case NODE_EXPRESSION_SUBSCRIPT:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_CAST:
	case NODE_CONSTANT:
	case NODE_CONSTANT_STR:
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
