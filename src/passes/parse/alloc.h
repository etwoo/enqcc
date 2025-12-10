#ifndef COMPILER_PASSES_PARSE_ALLOC_H
#define COMPILER_PASSES_PARSE_ALLOC_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;
struct flat;

result_t parse_alloc(Arena *arena, struct ast **dst, unsigned nt) WARN_UNUSED;
result_t flat_alloc(Arena *arena, struct flat **dst) WARN_UNUSED;

#endif
