#ifndef COMPILER_PASSES_SEMA_LINKAGE_H
#define COMPILER_PASSES_SEMA_LINKAGE_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;
struct symbol_table;

result_t sema_typecheck_linkage(Arena *arena,
                                struct ast *a,
                                struct symbol_table *s,
                                struct type_table *types) WARN_UNUSED;
#endif
