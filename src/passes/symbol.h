#ifndef COMPILER_SYMBOLS_H
#define COMPILER_SYMBOLS_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"
#include "sys/string_view.h"

#include <stdbool.h>

enum {
	NOT_YET_UNIQUE = -1,
};

enum symbol_type {
	SYMBOL_VARIABLE,
	SYMBOL_FUNCTION_DECLARATION,
	SYMBOL_FUNCTION_DEFINITION,
};

enum symbol_linkage {
	SYMBOL_LINKAGE_NONE = 0,
	SYMBOL_LINKAGE_INTERNAL = 1000, /* +1000, to discourage unintentional */
	SYMBOL_LINKAGE_EXTERNAL = 2000, /* casts from other types, like bool  */
};

bool is_internal(enum symbol_linkage linkage) WARN_UNUSED;
bool is_external(enum symbol_linkage linkage) WARN_UNUSED;
bool some_linkage(enum symbol_linkage linkage) WARN_UNUSED;

struct symbol_linkage_state {
	enum symbol_linkage linkage;
	enum {
		INITIAL_VALUE_NO_INITIALIZER,
		INITIAL_VALUE_TENTATIVE,
		INITIAL_VALUE_CONSTANT,
	} initial;
	long long int as_constant;
};

enum symbol_scope {
	SCOPE_UNSPECIFIED,
	SCOPE_BLOCK,
	SCOPE_FILE,
};

// TODO: for typedef support, add tracking for types (like variables)
struct symbol {
	struct string_view name;
	enum symbol_type stype;
	long long int n_args; /* number of func params, if SYMBOL_FUNCTION_* */
	long long int unique; /* unique ID for this symbol */
	bool level_delimiter; /* trigger new nesting level if prepending here */
	long long int cookie; /* maximum unique ID observed in any node */

	/*
	 * http://en.cppreference.com/w/c/language/storage_class_specifiers.html
	 * http://en.cppreference.com/w/c/language/extern.html
	 *
	 * We use linkage as an organizing concept and not extern/static/none
	 * because the mapping between the two can be unintuitive and require
	 * non-local reasoning about the code being compiled. For example,
	 * consider keyword extern appearing on a *re*declaration of a
	 * file-scope variable, declared earlier with internal linkage:
	 *
	 *   static int x = 0;
	 *   extern int x;
	 *   int main(void)
	 *   {
	 *       return x;
	 *   }
	 *
	 * This results in linkage for `x` remaining internal. In other words,
	 * in this particular case, use of keyword extern leads to a result
	 * similar to use of keyword static alone!
	 */
	struct symbol_linkage_state linkage;
	enum symbol_scope scope_if_specified;

	struct symbol *next;
};

result_t symbols_prepend(Arena *arena,
                         struct symbol **head,
                         const struct string_view *name,
                         enum symbol_type stype) WARN_UNUSED;
struct symbol *symbols_get(struct symbol *head,
                           const struct string_view *name,
                           bool stop_at_delimiter) WARN_UNUSED;
void symbols_reset_scope(struct symbol **symbols, struct symbol *reset_point);

struct symbol *symbols_get_scoped(struct symbol *head,
                                  const struct string_view *name,
                                  enum symbol_scope scope) WARN_UNUSED;

#endif
