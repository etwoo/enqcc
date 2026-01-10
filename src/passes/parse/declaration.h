#ifndef COMPILER_PASSES_PARSE_DECLARATION_H
#define COMPILER_PASSES_PARSE_DECLARATION_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"
#include "sys/string_view.h"

#include <inttypes.h>

struct token;
struct ast;

extern const uint32_t PARSE_DECLARATOR_ABSTRACT;

result_t parse_type(Arena *arena,
                    uint32_t flags,
                    const struct token **tok,
                    struct ctype *var_type,
                    struct string_view *identifier) WARN_UNUSED;

extern const uint32_t PARSE_DECLARATION_ACCEPT_FUNCTION;

result_t parse_fn_or_var_or_struct_declaration(Arena *arena,
                                               uint32_t flags,
                                               const struct token **tok,
                                               struct ast **dst) WARN_UNUSED;

#endif
