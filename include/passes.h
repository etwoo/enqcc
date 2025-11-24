#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "arena.h"
#include "result.h"

struct token;

result_t lex_init(Arena *arena, const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_debug_print(const struct token *tok);

struct ast;

result_t parse_init(Arena *arena,
                    const struct token *tok,
                    struct ast **a,
                    long long *generator) __attribute__((warn_unused_result));
void parse_debug_print(const struct ast *a, size_t indent);

struct symbol;
struct symbol_table {
	struct symbol *functions;
	struct symbol *variables;
};

result_t sema_label_loops(struct ast *a, long long int *generator)
	__attribute__((warn_unused_result));
result_t sema_typecheck(Arena *arena, struct ast *a, struct symbol_table *s)
	__attribute__((warn_unused_result));

struct intermediate;

result_t ir_init(Arena *arena,
                 const struct ast *a,
                 long long int base_id,
                 long long int base_label,
                 struct symbol_table *sym,
                 struct intermediate **ir) __attribute__((warn_unused_result));
void ir_debug_print(const struct intermediate *ir);

struct assembly;

result_t codegen_init(Arena *arena,
                      const struct intermediate *ir,
                      struct assembly **cg) __attribute__((warn_unused_result));
result_t codegen_replace_pseudoregisters(struct assembly *cg)
	__attribute__((warn_unused_result));
result_t codegen_fixup_instructions(Arena *arena, struct assembly *cg)
	__attribute__((warn_unused_result));
void codegen_debug_print(const struct assembly *cg);

enum platform {
	PLATFORM_MACOS,
	PLATFORM_LINUX,
};

void emit_asm(const struct assembly *cg, enum platform plat, int fd);

#endif
