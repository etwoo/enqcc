#ifndef COMPILER_PASSES_SEMA_EXPRESSION_H
#define COMPILER_PASSES_SEMA_EXPRESSION_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t sema_typecheck_expr(Arena *arena,
                             struct ast *a,
                             struct type_table *types) WARN_UNUSED;

#endif
