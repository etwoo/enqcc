#ifndef COMPILER_PASSES_LEX_H
#define COMPILER_PASSES_LEX_H

#include "sys/string_view.h"

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
	struct string_view val;
	struct token *next;
};

#endif
