#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

#define parse_alloc(expr, nt)                                                  \
	do {                                                                   \
		(expr) = malloc(sizeof(*(expr)));                              \
		check_if((expr) == NULL, ERR_LEX_ALLOC);                       \
		memset(expr, 0, sizeof(*(expr)));                              \
		(*(expr)).node_type = nt;                                      \
	} while (0);

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
parse_constant(const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_CONSTANT));
	parse_alloc(*dst, NODE_CONSTANT_INT);

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
parse_identifier(const struct token **tok, struct ast **dst)
{
	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER);
	}
	parse_alloc(*dst, NODE_IDENTIFIER);

	(**dst).u.str = (**tok).val;
	token_consume(tok);
	return RESULT_OK;
}

static result_t parse_expression(const struct token **tok,
                                 struct ast **dst,
                                 unsigned minimum_precedence) WARN_UNUSED;

static WARN_UNUSED result_t
parse_factor(const struct token **tok, struct ast **dst)
{
	assert(!is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)); // unimplemented
	if (is_token_type(*tok, TOKEN_CONSTANT)) {
		parse_alloc(*dst, NODE_EXPRESSION_UNARY_IDENTITY);
		check(parse_constant(tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_TILDE)) {
		parse_alloc(*dst, NODE_EXPRESSION_UNARY_COMPLEMENT);
		token_consume(tok);
		check(parse_factor(tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_HYPHEN)) {
		parse_alloc(*dst, NODE_EXPRESSION_UNARY_NEGATE);
		token_consume(tok);
		check(parse_factor(tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		parse_alloc(*dst, NODE_EXPRESSION_PAREN_ENCLOSED);
		token_consume(tok);
		check(parse_expression(tok, &(**dst).u.op_unary.operand, 0));
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

static const unsigned PRECEDENCE_LOW = 45;
static const unsigned PRECEDENCE_HIGH = 50;

static WARN_UNUSED unsigned
get_precedence(const struct ast *a)
{
	unsigned precedence = 0;
	switch (a->node_type) {
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		precedence = PRECEDENCE_LOW;
		break;
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
		precedence = PRECEDENCE_HIGH;
		break;
	default:
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
parse_expression(const struct token **tok,
                 struct ast **dst,
                 unsigned minimum_precedence)
{
	struct ast *left __attribute__((cleanup(parse_cleanup))) = NULL;
	check(parse_factor(tok, &left));

	while (true) {
		struct ast *bop __attribute__((cleanup(parse_cleanup))) = NULL;
		if (is_token_type(*tok, TOKEN_PLUS_SIGN)) {
			parse_alloc(bop, NODE_EXPRESSION_BINARY_ADD);
		} else if (is_token_type(*tok, TOKEN_HYPHEN)) {
			parse_alloc(bop, NODE_EXPRESSION_BINARY_SUBTRACT);
		} else if (is_token_type(*tok, TOKEN_ASTERISK)) {
			parse_alloc(bop, NODE_EXPRESSION_BINARY_MULTIPLY);
		} else if (is_token_type(*tok, TOKEN_FORWARD_SLASH)) {
			parse_alloc(bop, NODE_EXPRESSION_BINARY_DIVIDE);
		} else if (is_token_type(*tok, TOKEN_PERCENT_SIGN)) {
			parse_alloc(bop, NODE_EXPRESSION_BINARY_REMAINDER);
		} else {
			break;
		}

		const unsigned next_precedence = get_precedence(bop);
		if (next_precedence < minimum_precedence) {
			break;
		}

		token_consume(tok);

		struct ast *right __attribute__((cleanup(parse_cleanup))) =
			NULL;
		// NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
		check(parse_expression(tok, &right, next_precedence + 1));

		bop->u.op_binary.lhs = left;
		bop->u.op_binary.rhs = right;
		left = bop;

		right = NULL; /* release ownership */
		bop = NULL;   /* release ownership */
	}

	*dst = left;
	left = NULL; /* release ownership */
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_statement(const struct token **tok, struct ast **dst)
{
	parse_alloc(*dst, NODE_EXPRESSION_UNARY_IDENTITY);

	if (!is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_KEYWORD_RETURN);
	}
	token_consume(tok);

	check(parse_expression(tok, &(**dst).u.op_unary.operand, 0));

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function(const struct token **tok, struct ast **dst)
{
	parse_alloc(*dst, NODE_FUNCTION);

	if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT);
	}
	token_consume(tok);

	check(parse_identifier(tok, &(**dst).u.function.identifier));

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

	check(parse_statement(tok, &(**dst).u.function.statement));

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

result_t
parse_init(const struct token *tok, struct ast **a)
{
	parse_alloc(*a, NODE_PROGRAM);

	check(parse_function(&tok, &(**a).u.program.entrypoint_function));
	if (tok != NULL) {
		return make_result(ERR_PARSE_PROG_EXPECT_END);
	}

	return RESULT_OK;
}

void
parse_free(struct ast *a)
{
	free(a);
}

void
parse_cleanup(struct ast **a)
{
	parse_free(*a);
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
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		switch (a->node_type) {
		case NODE_EXPRESSION_UNARY_IDENTITY:
			debug("%*sEXPRESSION IDENTITY", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_NEGATE:
			debug("%*sEXPRESSION NEGATE", (int)indent, "");
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
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
		switch (a->node_type) {
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
		default:
			assert(0); /* logic error in caller */
			break;
		}
		parse_debug_print(a->u.op_binary.lhs, indent + 1);
		parse_debug_print(a->u.op_binary.rhs, indent + 1);
		break;
	case NODE_IDENTIFIER: {
		const struct string_view *s = &a->u.str;
		debug("%*sIDENT %.*s", (int)indent, "", (int)s->sz, s->data);
		break;
	}
	case NODE_CONSTANT_INT:
		debug("%*sCONSTANT %lld", (int)indent, "", a->u.num);
		break;
	}
}

#undef parse_alloc
