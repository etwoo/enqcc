#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

#define parse_alloc(dst, init_type)                                            \
	do {                                                                   \
		(dst) = malloc(sizeof(*(dst)));                                \
		check_if((dst) == NULL, ERR_PARSE_ALLOC);                      \
		memset(dst, 0, sizeof(*(dst)));                                \
		(dst)->base.node_type = init_type;                             \
	} while (0)

static WARN_UNUSED bool
is_token_type(const struct token *tok, unsigned expected)
{
	return tok != NULL && tok->token_type == expected;
}

static void
token_consume(const struct token **tok)
{
	*tok = (*tok)->next;
}

static WARN_UNUSED result_t
parse_constant(const struct token **tok, struct ast_constant *dst)
{
	dst->base.node_type = NODE_CONSTANT_INT;

	if (!is_token_type(*tok, TOKEN_CONSTANT)) {
		return make_result(ERR_PARSE_CONSTANT_EXPECT_TOKEN_CONSTANT);
	}

	/*
	 * strtoll() does not update errno on success, so we must clear it
	 * explicitly if we want a predictable value.
	 */
	errno = 0;

	dst->num = strtoll((*tok)->val.data, NULL, 0);
	if (errno != 0) {
		return make_result(ERR_PARSE_CONSTANT_STRTOLL,
		                   errno,
		                   (*tok)->val.data,
		                   (*tok)->val.sz);
	}

	token_consume(tok);
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expression(const struct token **tok, struct ast **dst)
{
	assert(!is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)); // unimplemented

	if (is_token_type(*tok, TOKEN_CONSTANT)) {
		struct ast_expression_constant *expr = NULL;
		parse_alloc(expr, NODE_EXPRESSION_PRIMITIVE);
		*dst = &expr->base;
		check(parse_constant(tok, &expr->constant));
	} else if (is_token_type(*tok, TOKEN_TILDE) ||
	           is_token_type(*tok, TOKEN_HYPHEN)) {
		struct ast_expression_unary_op *expr = NULL;
		parse_alloc(expr,
		            is_token_type(*tok, TOKEN_TILDE)
		                    ? NODE_EXPRESSION_UNARY_BITWISE_COMPLEMENT
		                    : NODE_EXPRESSION_UNARY_NEGATION);
		token_consume(tok);
		*dst = &expr->base;
		check(parse_expression(tok, &expr->operand));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		struct ast_expression_paren_enclosed *expr = NULL;
		parse_alloc(expr, NODE_EXPRESSION_PAREN_ENCLOSED);
		token_consume(tok);
		*dst = &expr->base;
		check(parse_expression(tok, &expr->enclosed));
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
parse_statement(const struct token **tok, struct ast_statement *dst)
{
	dst->base.node_type = NODE_STATEMENT;

	if (!is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_KEYWORD_RETURN);
	}
	token_consume(tok);

	check(parse_expression(tok, &dst->return_expression));

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function(const struct token **tok, struct ast_function *dst)
{
	dst->base.node_type = NODE_FUNCTION;

	if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER);
	}
	dst->identifier.base.node_type = NODE_IDENTIFIER;
	dst->identifier.token = (*tok)->val;
	token_consume(tok);

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

	check(parse_statement(tok, &dst->statement));

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_program(const struct token **tok, struct ast_program *dst)
{
	dst->base.node_type = NODE_PROGRAM;
	check(parse_function(tok, &dst->function));
	return RESULT_OK;
}

result_t
parse_init(const struct token *tok, struct ast **a)
{
	struct ast_program program = {0};
	check(parse_program(&tok, &program));
	if (tok != NULL) {
		return make_result(ERR_PARSE_PROG_EXPECT_END);
	}

	struct ast_program *out = malloc(sizeof(*out));
	check_if(out == NULL, ERR_PARSE_ALLOC);
	memcpy(out, &program, sizeof(*out));

	*a = &out->base;
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
	const struct string_view *s = NULL;
	switch (a->node_type) {
	case NODE_PROGRAM:
		debug("%*sPROGRAM", (int)indent, "");
		parse_debug_print(&((struct ast_program *)a)->function.base,
		                  indent + 1);
		break;
	case NODE_FUNCTION:
		debug("%*sFUNCTION", (int)indent, "");
		debug("%*sNAME", (int)(indent + 1), "");
		parse_debug_print(&((struct ast_function *)a)->identifier.base,
		                  indent + 2);
		debug("%*sBODY", (int)(indent + 1), "");
		parse_debug_print(&((struct ast_function *)a)->statement.base,
		                  indent + 2);
		break;
	case NODE_STATEMENT:
		debug("%*sSTATEMENT", (int)indent, "");
		parse_debug_print(
			((struct ast_statement *)a)->return_expression,
			indent + 1);
		break;
	case NODE_EXPRESSION_PRIMITIVE:
		debug("%*sEXPRESSION CONSTANT", (int)indent, "");
		parse_debug_print(
			&((struct ast_expression_constant *)a)->constant.base,
			indent + 1);
		break;
	case NODE_EXPRESSION_UNARY_NEGATION:
		debug("%*sEXPRESSION NEGATION", (int)indent, "");
		parse_debug_print(
			((struct ast_expression_unary_op *)a)->operand,
			indent + 1);
		break;
	case NODE_EXPRESSION_UNARY_BITWISE_COMPLEMENT:
		debug("%*sEXPRESSION BITWISE COMPLEMENT", (int)indent, "");
		parse_debug_print(
			((struct ast_expression_unary_op *)a)->operand,
			indent + 1);
		break;
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		debug("%*sEXPRESSION PARENTHESIZED", (int)indent, "");
		parse_debug_print(
			((struct ast_expression_paren_enclosed *)a)->enclosed,
			indent + 1);
		break;
	case NODE_IDENTIFIER:
		s = &((struct ast_identifier *)a)->token;
		debug("%*sIDENT %.*s", (int)indent, "", (int)s->sz, s->data);
		break;
	case NODE_CONSTANT_INT:
		debug("%*sCONSTANT %lld",
		      (int)indent,
		      "",
		      ((struct ast_constant *)a)->num);
		break;
	}
}
