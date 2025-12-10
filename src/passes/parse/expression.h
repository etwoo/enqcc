#ifndef COMPILER_PASSES_PARSE_EXPRESSION_H
#define COMPILER_PASSES_PARSE_EXPRESSION_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct token;
struct ast;

result_t parse_expr(Arena *arena,
                    const struct token **tok,
                    struct ast **dst,
                    unsigned minimum_precedence) WARN_UNUSED;

#endif
