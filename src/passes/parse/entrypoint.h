#ifndef COMPILER_PASSES_PARSE_ENTRYPOINT_H
#define COMPILER_PASSES_PARSE_ENTRYPOINT_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct token;
struct ast;

/*
 * Common _internal_ entrypoints for parsing, for use within the
 * src/passes/parse/... module.
 *
 * Consumers of the higher-level src/passes/parse.h APIs should not
 * have access to these functions, nor have any reason to call them.
 */
result_t parse_block(Arena *arena,
                     const struct token **tok,
                     struct ast **dst_outer) WARN_UNUSED;
result_t parse_expr(Arena *arena,
                    const struct token **tok,
                    struct ast **dst,
                    unsigned minimum_precedence) WARN_UNUSED;

#endif
