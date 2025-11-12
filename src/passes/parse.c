#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h> /* for strtoll() */

static WARN_UNUSED result_t
parse_alloc(Arena *arena, struct ast **dst, unsigned ntype)
{
	static struct ast dummy_workaround_clang_analyzer_null_pointer = {0};

	*dst = arena_alloc(arena, sizeof(**dst));
	if (*dst == NULL) {
		/*
		 * Workaround spurious clang-analyzer-core.NullDereference
		 * warnings at callsites of this helper function. The Clang
		 * Static Analyzer seems to forget that (*dst != NULL) when
		 * this function returns RESULT_OK, which should then protect
		 * callers from accessing a NULL pointer when they use
		 * check(parse_alloc(...)) properly.
		 */
		*dst = &dummy_workaround_clang_analyzer_null_pointer;
		return make_result(ERR_LEX_ALLOC);
	}

	memset(*dst, 0, sizeof(**dst));
	(**dst).node_type = ntype;
	return RESULT_OK;
}

static WARN_UNUSED bool
is_token_type(const struct token *tok, unsigned expected)
{
	return tok != NULL && tok->token_type == expected;
}

static void
token_consume(const struct token **tok)
{
	*tok = (**tok).next;
}

static WARN_UNUSED result_t
parse_constant(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_CONSTANT));
	check(parse_alloc(arena, dst, NODE_CONSTANT_INT));

	/*
	 * strtoll() does not update errno on success, so we must clear it
	 * explicitly if we want a predictable value.
	 */
	errno = 0;

	(**dst).u.num = strtoll((**tok).val.data, NULL, 0);
	if (errno != 0) {
		return make_result(ERR_PARSE_CONSTANT_STRTOLL,
		                   errno,
		                   (**tok).val.data,
		                   (**tok).val.sz);
	}

	token_consume(tok);
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_identifier(Arena *arena, const struct token **tok, struct ast **dst)
{
	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER);
	}
	check(parse_alloc(arena, dst, NODE_IDENTIFIER));

	(**dst).u.str = (**tok).val;
	token_consume(tok);
	return RESULT_OK;
}

static result_t parse_expression(Arena *arena,
                                 const struct token **tok,
                                 struct ast **dst,
                                 unsigned minimum_precedence) WARN_UNUSED;

static WARN_UNUSED result_t
parse_factor(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(!is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)); // unimplemented
	if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_IDENTITY));
		check(parse_constant(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_VARIABLE_USAGE));
		check(parse_identifier(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_TILDE)) {
		check(parse_alloc(arena,
		                  dst,
		                  NODE_EXPRESSION_UNARY_COMPLEMENT));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_HYPHEN)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_NEGATE));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_EXCLAMATION)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_NOT));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_PAREN_ENCLOSED));
		token_consume(tok);
		check(parse_expression(arena,
		                       tok,
		                       &(**dst).u.op_unary.operand,
		                       0));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
	} else {
		return make_result(ERR_PARSE_EXPR_EXPECT_REASONABLE);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expression_check_next_token(Arena *arena,
                                  const struct token *tok,
                                  struct ast **a)
{
	result_t r = RESULT_OK;
	if (is_token_type(tok, TOKEN_PLUS_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_ADD);
	} else if (is_token_type(tok, TOKEN_HYPHEN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_SUBTRACT);
	} else if (is_token_type(tok, TOKEN_ASTERISK)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_MULTIPLY);
	} else if (is_token_type(tok, TOKEN_FORWARD_SLASH)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_DIVIDE);
	} else if (is_token_type(tok, TOKEN_PERCENT_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_REMAINDER);
	} else if (is_token_type(tok, TOKEN_AMPERSAND_AMPERSAND)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_LOGICAL_AND);
	} else if (is_token_type(tok, TOKEN_VERT_BAR_VERT_BAR)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_LOGICAL_OR);
	} else if (is_token_type(tok, TOKEN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_VARIABLE_ASSIGNMENT);
	} else if (is_token_type(tok, TOKEN_EQUAL_SIGN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_EQUAL);
	} else if (is_token_type(tok, TOKEN_EXCLAMATION_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_NOT_EQUAL);
	} else if (is_token_type(tok, TOKEN_LESS_THAN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_LESS_THAN);
	} else if (is_token_type(tok, TOKEN_LESS_THAN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_LESS_THAN_EQ);
	} else if (is_token_type(tok, TOKEN_MORE_THAN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_MORE_THAN);
	} else if (is_token_type(tok, TOKEN_MORE_THAN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_MORE_THAN_EQ);
	} else {
		return RESULT_OK;
	}
	return r;
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
	case NODE_EXPRESSION_LOGICAL_AND:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_LOGICAL_OR:
		precedence += PRECEDENCE_INCREMENT;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		precedence += PRECEDENCE_INCREMENT;
		break;
	case NODE_FUNCTION:
	case NODE_PROGRAM:
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_IDENTIFIER:
	case NODE_CONSTANT_INT:
	case NODE_EXPRESSION_VARIABLE_USAGE:
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
static WARN_UNUSED result_t
parse_expression(Arena *arena,
                 const struct token **tok,
                 struct ast **dst,
                 unsigned minimum_precedence)
{
	struct ast *left = NULL;
	check(parse_factor(arena, tok, &left));

	while (true) {
		struct ast *bop = NULL;
		check(parse_expression_check_next_token(arena, *tok, &bop));
		if (bop == NULL) {
			break;
		}

		const unsigned precedence = get_precedence(bop);
		if (precedence < minimum_precedence) {
			break;
		}

		token_consume(tok);

		struct ast *right = NULL;
		check(parse_expression(arena, tok, &right, precedence + 1));

		bop->u.op_binary.lhs = left;
		bop->u.op_binary.rhs = right;
		left = bop;
	}

	*dst = left;
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_statement(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_IDENTITY));

	if (!is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_KEYWORD_RETURN);
	}
	token_consume(tok);

	check(parse_expression(arena, tok, &(**dst).u.op_unary.operand, 0));

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_FUNCTION));

	if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT);
	}
	token_consume(tok);

	check(parse_identifier(arena, tok, &(**dst).u.function.identifier));

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_KEYWORD_VOID)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_KEYWORD_VOID);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	check(parse_statement(arena, tok, &(**dst).u.function.statement));

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

result_t
parse_init(Arena *arena, const struct token *tok, struct ast **a)
{
	check(parse_alloc(arena, a, NODE_PROGRAM));
	check(parse_function(arena,
	                     &tok,
	                     &(**a).u.program.entrypoint_function));
	if (tok != NULL) {
		return make_result(ERR_PARSE_PROG_EXPECT_END);
	}

	return RESULT_OK;
}

void
parse_debug_print(const struct ast *a, size_t indent)
{
	assert(indent <= INT_MAX);
	switch (a->node_type) {
	case NODE_PROGRAM:
		debug("%*sPROGRAM", (int)indent, "");
		parse_debug_print(a->u.program.entrypoint_function, indent + 1);
		break;
	case NODE_FUNCTION:
		debug("%*sFUNCTION", (int)indent, "");
		debug("%*sNAME", (int)(indent + 1), "");
		parse_debug_print(a->u.function.identifier, indent + 2);
		debug("%*sBODY", (int)(indent + 1), "");
		parse_debug_print(a->u.function.statement, indent + 2);
		break;
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		switch (a->node_type) {
		case NODE_EXPRESSION_UNARY_IDENTITY:
			debug("%*sEXPRESSION IDENTITY", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_NEGATE:
			debug("%*sEXPRESSION NEGATE", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_NOT:
			debug("%*sEXPRESSION NOT", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_COMPLEMENT:
			debug("%*sEXPRESSION COMPLEMENT", (int)indent, "");
			break;
		case NODE_EXPRESSION_PAREN_ENCLOSED:
			debug("%*sEXPRESSION PARENTHESIZED", (int)indent, "");
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		switch (a->node_type) {
		case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
			debug("%*sEXPRESSION ASSIGN", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_ADD:
			debug("%*sEXPRESSION ADD", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_SUBTRACT:
			debug("%*sEXPRESSION SUBTRACT", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_MULTIPLY:
			debug("%*sEXPRESSION MULTIPLY", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_DIVIDE:
			debug("%*sEXPRESSION DIVIDE", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_REMAINDER:
			debug("%*sEXPRESSION REMAINDER", (int)indent, "");
			break;
		case NODE_EXPRESSION_LOGICAL_AND:
			debug("%*sEXPRESSION LOGICAL AND", (int)indent, "");
			break;
		case NODE_EXPRESSION_LOGICAL_OR:
			debug("%*sEXPRESSION LOGICAL OR", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_EQUAL:
			debug("%*sEXPRESSION COMPARE EQUAL", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
			debug("%*sEXPRESSION NOT EQUAL", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_LESS_THAN:
			debug("%*sEXPRESSION LESS THAN", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
			debug("%*sEXPRESSION LESS THAN OR EQ", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_MORE_THAN:
			debug("%*sEXPRESSION MORE THAN", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
			debug("%*sEXPRESSION MORE THAN OR EQ", (int)indent, "");
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		parse_debug_print(a->u.op_binary.lhs, indent + 1);
		parse_debug_print(a->u.op_binary.rhs, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_IDENTIFIER: {
		const struct string_view *s = &a->u.str;
		const char *desc = NULL;
		switch (a->node_type) {
		case NODE_EXPRESSION_VARIABLE_USAGE:
			desc = "EXPRESSION VARIABLE USAGE";
			break;
		case NODE_IDENTIFIER:
			desc = "IDENT";
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		debug("%*s%s %.*s", (int)indent, "", desc, (int)s->sz, s->data);
		break;
	}
	case NODE_CONSTANT_INT:
		debug("%*sCONSTANT %lld", (int)indent, "", a->u.num);
		break;
	}
}
