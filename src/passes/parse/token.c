#include "passes/parse/token.h"

#include "passes/lex.h"

#include <assert.h>

void
token_consume(const struct token **tok)
{
	assert(*tok != NULL);
	*tok = (**tok).next;
}

bool
is_token_type(const struct token *tok, unsigned expected)
{
	return tok != NULL && tok->token_type == expected;
}

bool
is_token_variable_type(const struct token *tok)
{
	if (tok == NULL) {
		return false;
	}
	switch (tok->token_type) {
	case TOKEN_KEYWORD_VOID:
	case TOKEN_KEYWORD_CHAR:
	case TOKEN_KEYWORD_INT:
	case TOKEN_KEYWORD_LONG:
	case TOKEN_KEYWORD_SIGNED:
	case TOKEN_KEYWORD_UNSIGNED:
	case TOKEN_KEYWORD_DOUBLE:
		return true;
	default:
		break;
	}
	return false;
}

bool
is_token_maybe_function_prefix(const struct token *tok)
{
	return is_token_variable_type(tok) ||
	       is_token_type(tok, TOKEN_KEYWORD_STATIC) ||
	       is_token_type(tok, TOKEN_KEYWORD_EXTERN);
}
