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

// TODO: generalize constant_value -> initial_value; scalar _or_ compound value
//
// should i fold this into ctype, maybe that will make static init for structs
// easier later? for now, arrays are simple, flattened, all same type, but
// later, structs will have arbitrary nesting, different types at each offset?
//
// ... or maybe use `struct ast` here, with assumption that all internal nodes
// are NODE_EXPRESSION_INITIALIZER and all leaf nodes are NODE_CONSTANT, with no
// dynamic expressions, negations, etc present?
//
// find some way to fold all info together, e.g. avoid having ir_variable and
// asm_variable have to retain c89type separate from this constant_value (scalar
// or vector), just so that emit_asm_var knows whether to emit double/long/int;
// all of this info -- double/long/int, how many elements, zero padding at end,
// etc -- should be rolled up into a single struct, istead of being spread
// across multiple members that each have to propagate from symbols (created in
// sema) to ir_variable to asm_variable to emit.c
//
//    -> maybe flatten double -> quadword earlier, such that static init values
//    can be ignorant of doubles entirely!
//
//    can retain double crap for floating point constants in ir_val, since that
//    already tracks ctype in its own way
//
//    ... but translating double to quadword representation earlier would maybe
//    let us switch back to just as_integer and remove as_double here!
//
//    ... which would then simplify array representation, could just be an array
//    of int128_t, would only need to distinguish long vs quad
union constant_value {
	int128_t as_integer;
	double as_double;
};

struct symbol_linkage_state {
	enum symbol_linkage linkage;
	enum {
		INITIAL_VALUE_NO_INITIALIZER,
		INITIAL_VALUE_TENTATIVE,
		INITIAL_VALUE_CONSTANT,
	} initial;
	union constant_value as_constant;
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
