#ifndef COMPILER_PASSES_SEMA_POINTER_H
#define COMPILER_PASSES_SEMA_POINTER_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t sema_pointer_cmp(const struct ctype *lhs,
                          const struct ctype *rhs) WARN_UNUSED;
result_t sema_typecheck_ptr(Arena *arena,
                            struct ast *a,
                            struct type_table *types) WARN_UNUSED;

#endif
