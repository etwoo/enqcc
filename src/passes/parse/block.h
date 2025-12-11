#ifndef COMPILER_PASSES_PARSE_BLOCK_H
#define COMPILER_PASSES_PARSE_BLOCK_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

#include <stdbool.h>

struct token;
struct ast;
struct flat;

result_t parse_block(Arena *arena,
                     const struct token **tok,
                     struct ast **dst_outer) WARN_UNUSED;
result_t parse_stmt_multi(Arena *arena,
                          const struct token **tok,
                          struct flat **dst) WARN_UNUSED;
result_t parse_stmt_one(Arena *arena,
                        const struct token **tok,
                        struct ast **dst,
                        bool *call_again) WARN_UNUSED;

#endif
