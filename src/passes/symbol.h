#ifndef COMPILER_SYMBOLS_H
#define COMPILER_SYMBOLS_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"
#include "sys/string_view.h"

#include <stdbool.h>

enum symbol_type {
	SYMBOL_VARIABLE,
	SYMBOL_FUNCTION_DECLARATION,
	SYMBOL_FUNCTION_DEFINITION,
};

enum symbol_linkage {
	LINKAGE_NONE,
	LINKAGE_INTERNAL,
	LINKAGE_EXTERNAL,
};

enum symbol_storage_class {
	STORAGE_AUTOMATIC,
	STORAGE_GLOBAL,
};

// TODO: for typedef support, add tracking for types (like variables)
struct symbol {
	struct string_view name;
	enum symbol_type stype;
	enum symbol_linkage linkage;
	enum symbol_storage_class storage;
	long long int n_args; /* number of func params, if SYMBOL_FUNCTION_* */
	long long int unique; /* unique ID for this symbol */
	long long int level;  /* nesting level of symbol declaration */
	bool level_delimiter; /* trigger new nesting level if prepending here */
	long long int cookie; /* maximum unique ID observed in any node */
	struct symbol *next;
};

result_t symbols_prepend(Arena *arena,
                         struct symbol **head,
                         const struct string_view *name,
                         enum symbol_type stype,
                         enum symbol_linkage linkage,
                         enum symbol_storage_class storage,
                         long long int n_args) WARN_UNUSED;
struct symbol *symbols_get(struct symbol *head,
                           const struct string_view *name,
                           bool stop_at_delimiter) WARN_UNUSED;
void symbols_reset_scope(struct symbol **symbols, struct symbol *reset_point);

#endif
