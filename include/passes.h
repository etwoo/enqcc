#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "result.h"

struct token;

result_t lex_init(const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_free(struct token *tok);
void lex_cleanup(struct token **tok); /* use with __attribute__((cleanup)) */
void lex_debug_print(const struct token *tok);

struct ast;

result_t parse_init(struct token *tok, struct ast **a)
	__attribute__((warn_unused_result));
void parse_free(struct ast *a);
void parse_cleanup(struct ast **a); /* use with __attribute__((cleanup)) */
void parse_debug_print(const struct ast *a, size_t indent);

#endif
