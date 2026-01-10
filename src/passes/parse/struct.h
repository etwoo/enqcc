#ifndef COMPILER_PASSES_PARSE_STRUCT_H
#define COMPILER_PASSES_PARSE_STRUCT_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct token;
struct ast;

result_t parse_struct_declaration(Arena *arena,
                                  const struct token **tok,
                                  struct ast **dst_struct) WARN_UNUSED;

#endif
