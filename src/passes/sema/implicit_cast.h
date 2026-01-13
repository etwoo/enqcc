#ifndef COMPILER_PASSES_SEMA_IMPLICIT_CAST_H
#define COMPILER_PASSES_SEMA_IMPLICIT_CAST_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t sema_typecheck_implicit_cast(Arena *arena,
                                      struct ast *a,
                                      struct type_table *types) WARN_UNUSED;
result_t
cast_if(Arena *arena, const struct ctype *cast_to, struct ast **a) WARN_UNUSED;

#endif
