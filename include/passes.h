#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "result.h"

struct token;

result_t lex_init(const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_free(struct token *tok);
void lex_cleanup(struct token **tok);
void lex_debug_print(const struct token *tok);

struct ast;

result_t parse_init(const struct token *tok, struct ast **a)
	__attribute__((warn_unused_result));
void parse_free(struct ast *a);
void parse_cleanup(struct ast **a);
void parse_debug_print(const struct ast *a, size_t indent);

struct intermediate;

result_t ir_init(const struct ast *a, struct intermediate **ir)
	__attribute__((warn_unused_result));
void ir_free(struct intermediate *ir);
void ir_cleanup(struct intermediate **ir);
void ir_debug_print(const struct intermediate *ir);

struct assembly;

result_t codegen_init(const struct intermediate *ir, struct assembly **cg)
	__attribute__((warn_unused_result));
void codegen_free(struct assembly *cg);
void codegen_cleanup(struct assembly **cg);
void codegen_debug_print(const struct assembly *cg);

enum platform {
	PLATFORM_MACOS,
	PLATFORM_LINUX,
};

void emit_asm(const struct assembly *cg, enum platform plat, int fd);

#endif
