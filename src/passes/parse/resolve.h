#ifndef COMPILER_PASSES_PARSE_RESOLVE_H
#define COMPILER_PASSES_PARSE_RESOLVE_H

#include "arena.h"
#include "lang/symbol.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t resolve_declaration(Arena *arena,
                             struct ast *a,
                             struct symbol **symbols,
                             struct type_table **types,
                             enum symbol_linkage assume_linkage) WARN_UNUSED;
result_t resolve_function(Arena *arena,
                          struct ast *a,
                          struct symbol **symbols,
                          struct type_table **types) WARN_UNUSED;
result_t resolve_struct(Arena *arena,
                        struct ast *a,
                        struct symbol **symbols,
                        struct type_table **types) WARN_UNUSED;

#endif
