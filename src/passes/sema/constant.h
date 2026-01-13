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

result_t make_initializer(Arena *arena,
                          struct ast *init,
                          const struct ctype *dst_type,
                          struct type_table *types,
                          struct constant_initializer *out) WARN_UNUSED;
void make_initializer_bytes(const struct ast *a,
                            const struct ctype *dst_type,
                            struct constant_bytes *out);

#endif
