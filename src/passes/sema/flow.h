#ifndef COMPILER_PASSES_SEMA_FLOW_H
#define COMPILER_PASSES_SEMA_FLOW_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t sema_label_loops(Arena *arena,
                          struct ast *a,
                          long long int *generator) WARN_UNUSED;
result_t sema_label_gotos(Arena *arena,
                          struct ast *a,
                          long long int *generator) WARN_UNUSED;

#endif
