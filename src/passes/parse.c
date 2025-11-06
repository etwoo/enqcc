#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <stdbool.h>
#include <stdlib.h>

static WARN_UNUSED result_t
parse_alloc(struct ast **a)
{
	*a = malloc(sizeof(**a));
	check_if(*a == NULL, ERR_PARSE_ALLOC);
	memset(*a, 0, sizeof(**a));
	return RESULT_OK;
}

static WARN_UNUSED bool
is_token_type(const struct token *tok, unsigned expected)
{
	return tok != NULL && tok->token_type == expected;
}

static void
token_consume(struct token **tok)
{
	*tok = (*tok)->next;
}

static WARN_UNUSED result_t
parse_constant(struct token **tok, struct ast **a)
{
	check(parse_alloc(a));
	struct ast *node = *a;
	node->node_type = NODE_CONSTANT_INT;

	if (!is_token_type(*tok, TOKEN_CONSTANT)) {
		return make_result(ERR_PARSE_CONSTANT_EXPECT_TOKEN_CONSTANT);
	}
	node->val = (*tok)->val;
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expression(struct token **tok, struct ast **a)
{
	check(parse_constant(tok, a));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_statement(struct token **tok, struct ast **a)
{
	check(parse_alloc(a));
	struct ast *node = *a;
	node->node_type = NODE_STATEMENT;

	if (!is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_KEYWORD_RETURN);
	}
	token_consume(tok);

	check(parse_expression(tok, a));

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}
static WARN_UNUSED result_t
parse_function(struct token **tok, struct ast **a)
{
	check(parse_alloc(a));
	struct ast *node = *a;
	node->node_type = NODE_FUNCTION;

	if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER);
	}
	check(parse_alloc(&node->children[0]));
	node->children[0]->node_type = NODE_IDENTIFIER;
	node->children[0]->val = (*tok)->val;
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

	check(parse_statement(tok, &node->children[1]));

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_program(struct token **tok, struct ast **a)
{
	check(parse_alloc(a));
	struct ast *node = *a;
	node->node_type = NODE_PROGRAM;
	check(parse_function(tok, &node->children[0]));
	return RESULT_OK;
}

result_t
parse_init(struct token *tok, struct ast **a)
{
	check(parse_program(&tok, a));
	if (tok != NULL) {
		return make_result(ERR_PARSE_PROG_EXPECT_END);
	}
	return RESULT_OK;
}

void
parse_free(struct ast *a)
{
	if (a != NULL) {
		for (size_t i = 0; i < ARRAY_SIZE(a->children); ++i) {
			parse_free(a->children[i]);
		}
		free(a);
	}
}

void
parse_cleanup(struct ast **a)
{
	parse_free(*a);
}

static void
parse_debug_one(const struct ast *a, int indent)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
		debug("%*sPROG", indent, "");
		break;
	case NODE_FUNCTION:
		debug("%*sFUNC %.*s", indent, "", (int)a->val.sz, a->val.data);
		break;
	case NODE_STATEMENT:
		debug("%*sSTMT return", indent, "");
		break;
	case NODE_EXPRESSION:
		debug("%*sEXPR int", indent, "");
		break;
	case NODE_IDENTIFIER:
		debug("%*sIDENT %.*s", indent, "", (int)a->val.sz, a->val.data);
		break;
	case NODE_CONSTANT_INT:
		debug("%*sCONST %.*s", indent, "", (int)a->val.sz, a->val.data);
		break;
	}
}

void
parse_debug_print(const struct ast *a, size_t indent)
{
	if (a != NULL) {
		parse_debug_one(a, (int)indent);
		for (size_t i = 0; i < ARRAY_SIZE(a->children); ++i) {
			parse_debug_print(a->children[i], indent + 1);
		}
	}
}
