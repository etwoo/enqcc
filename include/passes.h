#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "result.h"

struct token {
	enum {
		TOKEN_IDENTIFIER,
		TOKEN_CONSTANT,
		TOKEN_KEYWORD_RETURN,
		TOKEN_KEYWORD_VOID,
		TOKEN_KEYWORD_INT,
		TOKEN_PAREN_OPEN,
		TOKEN_PAREN_CLOSE,
		TOKEN_BRACE_OPEN,
		TOKEN_BRACE_CLOSE,
		TOKEN_SEMICOLON,
	} token_type;
	struct string_view *value;
	struct token *next;
};

result_t lex_init(const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_free(struct token *tok);
void lex_debug_print(struct token *tok);

#endif
