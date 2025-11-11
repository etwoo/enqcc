#ifndef COMPILER_SYMBOLS_H
#define COMPILER_SYMBOLS_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"
#include "sys/string_view.h"

// TODO: for typedef support, add tracking for types (like variables)
// TODO: change symbol table datastructure, avoid quadratic behavior in caller
struct symbol {
	struct string_view name;
	long long int unique;
	struct symbol *next;
};

result_t symbols_prepend(Arena *arena,
                         struct symbol **head,
                         const struct string_view *name) WARN_UNUSED;
const struct symbol *symbols_get(const struct symbol *head,
                                 const struct string_view *name) WARN_UNUSED;

#endif
