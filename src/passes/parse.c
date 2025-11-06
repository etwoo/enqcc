#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

static WARN_UNUSED result_t
parse_alloc(struct ast **a, unsigned node_type)
{
	assert(a != NULL);
	*a = malloc(sizeof(**a));
	check_if(*a == NULL, ERR_LEX_ALLOC);
	memset(*a, 0, sizeof(**a));
	(**a).node_type = node_type;
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
parse_constant(const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_CONSTANT));
	check(parse_alloc(dst, NODE_CONSTANT_INT));
	assert(dst != NULL && *dst != NULL);

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
	check(parse_alloc(dst, NODE_IDENTIFIER));
	assert(dst != NULL && *dst != NULL);

	(**dst).u.str = (**tok).val;
	token_consume(tok);
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expression(const struct token **tok, struct ast **dst)
{
	assert(!is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)); // unimplemented

	if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_alloc(dst, NODE_EXPRESSION_UNARY_IDENTITY));
		assert(dst != NULL && *dst != NULL);
		check(parse_constant(tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_TILDE)) {
		check(parse_alloc(dst, NODE_EXPRESSION_UNARY_COMPLEMENT));
		assert(dst != NULL && *dst != NULL);
		token_consume(tok);
		check(parse_expression(tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_HYPHEN)) {
		check(parse_alloc(dst, NODE_EXPRESSION_UNARY_NEGATION));
		assert(dst != NULL && *dst != NULL);
		token_consume(tok);
		check(parse_expression(tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		check(parse_alloc(dst, NODE_EXPRESSION_PAREN_ENCLOSED));
		assert(dst != NULL && *dst != NULL);
		token_consume(tok);
		check(parse_expression(tok, &(**dst).u.op_unary.operand));
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
parse_statement(const struct token **tok, struct ast **dst)
{
	check(parse_alloc(dst, NODE_EXPRESSION_UNARY_IDENTITY));

	if (!is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_KEYWORD_RETURN);
	}
	token_consume(tok);

	check(parse_expression(tok, &(**dst).u.op_unary.operand));

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function(const struct token **tok, struct ast **dst)
{
	check(parse_alloc(dst, NODE_FUNCTION));

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
	check(parse_alloc(a, NODE_PROGRAM));
	assert(a != NULL && *a != NULL);

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
		debug("%*sEXPRESSION IDENTITY", (int)indent, "");
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_UNARY_NEGATION:
		debug("%*sEXPRESSION NEGATION", (int)indent, "");
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		debug("%*sEXPRESSION COMPLEMENT", (int)indent, "");
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		debug("%*sEXPRESSION PARENTHESIZED", (int)indent, "");
		parse_debug_print(a->u.op_unary.operand, indent + 1);
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
