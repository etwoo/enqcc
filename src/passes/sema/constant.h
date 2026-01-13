#ifndef COMPILER_PASSES_SEMA_CONSTANT_H
#define COMPILER_PASSES_SEMA_CONSTANT_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;
struct constant_bytes;
struct constant_initializer;
struct ctype;
struct type_table;

// TODO: rename map_numeric_type() to be descriptive
// maybbe something like calculate_constant_initializer()
result_t map_numeric_type(Arena *arena,
                          struct ast *init,
                          const struct ctype *dst_type,
                          struct type_table *types,
                          struct constant_initializer *out) WARN_UNUSED;
void map_numeric_type_scalar(const struct ast *a,
                             const struct ctype *dst_type,
                             struct constant_bytes *out);

#endif
