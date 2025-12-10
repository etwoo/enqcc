#ifndef COMPILER_PASSES_PARSE_CONSTANT_H
#define COMPILER_PASSES_PARSE_CONSTANT_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct token;
struct ast;

result_t
parse_constant(Arena *arena, const struct token **tok, struct ast **dst);

#endif
