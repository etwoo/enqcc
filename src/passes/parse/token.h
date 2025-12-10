#ifndef COMPILER_PASSES_PARSE_LEX_TOKEN_H
#define COMPILER_PASSES_PARSE_LEX_TOKEN_H

#include "result.h"
#include "sys/compiler_features.h"

#include <stdbool.h>

struct token;

void token_consume(const struct token **tok);
bool is_token_type(const struct token *tok, unsigned expected) WARN_UNUSED;
bool is_token_variable_type(const struct token *tok) WARN_UNUSED;
bool is_token_maybe_function_prefix(const struct token *tok) WARN_UNUSED;

#endif
