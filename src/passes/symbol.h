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

enum {
	UNIQUE_NOT_YET = -1,
	UNIQUE_NOT_NECESSARY = -2,
};

enum initializer_state {
	INITIAL_VALUE_NO_INITIALIZER,
	INITIAL_VALUE_TENTATIVE,
	INITIAL_VALUE_CONSTANT,
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
	 *
	 * Note: the meaning of has_linkage currently depends on context. In
	 * particular, sema.c uses has_linkage to mean external linkage
	 * specifically, while parse.c overloads has_linkage to mean one of:
	 *
	 * - file scope declaration with internal linkage
	 * - file scope declaration with external linkage
	 * - block scope declaration with external linkage
	 */
	struct {
		bool has_linkage;
		enum initializer_state initial;
		long long int as_constant;
	} linkage;

	struct symbol *next;
};

result_t symbols_prepend(Arena *arena,
                         struct symbol **head,
                         const struct string_view *name,
                         enum symbol_type stype,
                         long long int n_args) WARN_UNUSED;
struct symbol *symbols_get(struct symbol *head,
                           const struct string_view *name,
                           bool stop_at_delimiter) WARN_UNUSED;
struct symbol *symbols_get_by_id(struct symbol *head,
                                 long long int target) WARN_UNUSED;
void symbols_reset_scope(struct symbol **symbols, struct symbol *reset_point);

#endif
