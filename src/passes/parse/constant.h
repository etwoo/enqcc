#ifndef COMPILER_PASSES_PARSE_CONSTANT_H
#define COMPILER_PASSES_PARSE_CONSTANT_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

#include <stdbool.h>

struct token;
struct ast;

bool can_parse_constant(const struct token *tok) WARN_UNUSED;
result_t parse_constant(Arena *arena,
                        const struct token **tok,
                        struct ast **dst) WARN_UNUSED;

#endif
