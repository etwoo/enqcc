#ifndef COMPILER_PASSES_H
#define COMPILER_PASSES_H

#include "arena.h"
#include "result.h"

struct token;

result_t lex_init(Arena *arena, const char *src, struct token **tok)
	__attribute__((warn_unused_result));
void lex_debug_print(const struct token *tok);

struct ast;
struct type_table;

result_t parse_init(Arena *arena,
                    const struct token *tok,
                    struct ast **a,
                    long long *generator,
                    struct type_table **types)
	__attribute__((warn_unused_result));
void parse_debug_print(const struct ast *a, size_t indent);

struct symbol;
struct symbol_table {
	struct symbol *functions;
	struct symbol *variables;
	struct symbol *string_literals;
};

result_t sema_typecheck(Arena *arena,
                        struct ast *a,
                        long long int *label_generator,
                        struct symbol_table *s,
                        struct type_table *types)
	__attribute__((warn_unused_result));
void sema_debug_print_variables(const struct symbol_table *s);

struct intermediate;

result_t ir_init(Arena *arena,
                 const struct ast *a,
                 long long int base_id,
                 long long int base_label,
                 struct symbol_table *sym,
                 struct type_table *types,
                 struct intermediate **ir) __attribute__((warn_unused_result));
void ir_debug_print(const struct intermediate *ir);

struct assembly;

result_t codegen_init(Arena *arena,
                      const struct intermediate *ir,
                      struct assembly **cg) __attribute__((warn_unused_result));
result_t codegen_replace_pseudo(Arena *arena, struct assembly *cg)
	__attribute__((warn_unused_result));
result_t codegen_fixup_instructions(Arena *arena, struct assembly *cg)
	__attribute__((warn_unused_result));
void codegen_debug_print(const struct assembly *cg);

enum platform {
	PLATFORM_MACOS,
	PLATFORM_LINUX,
};

result_t emit_asm(Arena *arena,
                  const struct assembly *cg,
                  struct symbol_table *s,
                  enum platform plat,
                  int fd) __attribute__((warn_unused_result));

#endif
