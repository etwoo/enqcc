#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "result.h"

struct token;

result_t lex_init(const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_free(struct token *tok);
void lex_debug_print(struct token *tok);

#endif
