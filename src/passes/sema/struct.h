#ifndef COMPILER_PASSES_SEMA_STRUCT_H
#define COMPILER_PASSES_SEMA_STRUCT_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t sema_typecheck_struct(Arena *arena, struct ast *a) WARN_UNUSED;

#endif
