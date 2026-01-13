#ifndef COMPILER_PASSES_SEMA_STRING_H
#define COMPILER_PASSES_SEMA_STRING_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;
struct symbol_table;

result_t sema_typecheck_strlit(Arena *arena,
                               struct ast *a,
                               struct symbol_table *symbols,
                               struct type_table *types) WARN_UNUSED;

#endif
