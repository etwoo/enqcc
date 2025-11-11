#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "arena.h"
#include "result.h"

struct token;

result_t lex_init(Arena *arena, const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_debug_print(const struct token *tok);

struct ast;

result_t parse_init(Arena *arena, const struct token *tok, struct ast **a)
	__attribute__((warn_unused_result));
void parse_debug_print(const struct ast *a, size_t indent);

struct intermediate;

result_t ir_init(Arena *arena, const struct ast *a, struct intermediate **ir)
	__attribute__((warn_unused_result));
void ir_debug_print(const struct intermediate *ir);

struct assembly;

result_t codegen_init(const struct intermediate *ir, struct assembly **cg)
	__attribute__((warn_unused_result));
result_t codegen_replace_pseudoregisters(struct assembly *cg)
	__attribute__((warn_unused_result));
result_t codegen_fixup_instructions(const struct intermediate *ir,
                                    struct assembly *cg)
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
