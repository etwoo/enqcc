#ifndef COMPILER_SYMBOLS_H
#define COMPILER_SYMBOLS_H

#include "arena.h"
#include "lang/int128_t.h"
#include "lang/types.h"
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

struct constant_bytes {
	long long unsigned byte_count;
	long long unsigned byte_value; /* may contain double as quadword */
};

struct constant_initializer {
	long long unsigned count;
	struct constant_bytes *elements;
};

long long unsigned get_double_as_quadword(double value) WARN_UNUSED;
result_t constant_set_zero(Arena *arena,
                           struct constant_initializer *ci) WARN_UNUSED;
result_t constant_make_zero(Arena *arena,
                            struct constant_initializer **dst) WARN_UNUSED;
bool constant_is_zero(const struct constant_initializer *ci) WARN_UNUSED;
long long unsigned constant_byte_count(const struct constant_initializer *ci);
void constant_debug_print(const struct constant_initializer *ci, size_t indent);

struct symbol_linkage_state {
	enum symbol_linkage linkage;
	enum {
		INITIAL_VALUE_NO_INITIALIZER,
		INITIAL_VALUE_TENTATIVE,
		INITIAL_VALUE_CONSTANT,
	} initial;
	struct constant_initializer initializer;
};

struct symbol {
	struct string_view name;
	enum symbol_type stype;
	struct ctype c89type; /* variable type or function return type */
	long long int unique; /* unique ID for this symbol */
	long long int cookie; /* maximum unique ID observed in any node */
	bool level_delimiter; /* limit between symbols_get_*() contexts */

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

	/*
	 * Pointer to generic arena-allocated data, specific to a given caller.
	 */
	void *auxiliary;

	struct symbol *next;
};

result_t symbols_prepend(Arena *arena,
                         struct symbol **head,
                         const struct string_view *name,
                         enum symbol_type stype,
                         struct ctype *c89type) WARN_UNUSED;
struct symbol *symbols_get_limited(struct symbol *head,
                                   const struct string_view *name) WARN_UNUSED;
struct symbol *symbols_get_anywhere(struct symbol *head,
                                    const struct string_view *name) WARN_UNUSED;
struct symbol *symbols_get_unique(struct symbol *head,
                                  long long int unique) WARN_UNUSED;
void symbols_reset_scope(struct symbol **symbols, struct symbol *reset_point);

bool is_mangled(struct symbol *sym) WARN_UNUSED;
result_t mangle_name(Arena *arena, struct symbol *sym) WARN_UNUSED;

#endif
