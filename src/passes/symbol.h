#ifndef COMPILER_SYMBOLS_H
#define COMPILER_SYMBOLS_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"
#include "sys/string_view.h"

#include <stdbool.h>

// TODO: for typedef support, add tracking for types (like variables)
// TODO: change symbol table datastructure, avoid quadratic behavior in caller
struct symbol {
	struct string_view name;
	long long int unique; /* unique ID for this symbol */
	long long int level;  /* nesting level of symbol declaration */
	bool level_delimiter; /* trigger new nesting level if prepending here */
	long long int cookie; /* maximum unique ID observed in any node */
	struct symbol *next;
};

result_t symbols_prepend(Arena *arena,
                         struct symbol **head,
                         const struct string_view *name) WARN_UNUSED;
const struct symbol *symbols_get(const struct symbol *head,
                                 const struct string_view *name,
                                 bool stop_at_delimiter) WARN_UNUSED;

#endif
