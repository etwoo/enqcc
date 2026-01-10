#include "passes/parse/declaration.h"

#include "passes.h" /* for lex_debug_print() */
#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/block.h"
#include "passes/parse/constant.h"
#include "passes/parse/expression.h"
#include "passes/parse/struct.h"
#include "passes/parse/token.h"
#include "sys/array.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h> /* for ULLONG_MAX */
#include <string.h> /* for memset() */

struct parse_basic_type_state {
	size_t n_void;
	size_t n_char;
	size_t n_int;
	size_t n_long;
	size_t n_signed;
	size_t n_unsigned;
	size_t n_double;
	size_t n_struct;
	struct string_view name;
};

static void
parse_basic_type_accumulate(const struct token **tok,
                            struct parse_basic_type_state *state)
{
	if (!is_token_variable_type(*tok)) {
		return;
	}

	assert(*tok != NULL);
	switch ((**tok).token_type) {
	case TOKEN_KEYWORD_VOID:
		state->n_void++;
		break;
	case TOKEN_KEYWORD_CHAR:
		state->n_char++;
		break;
	case TOKEN_KEYWORD_INT:
		state->n_int++;
		break;
	case TOKEN_KEYWORD_LONG:
		state->n_long++;
		break;
	case TOKEN_KEYWORD_SIGNED:
		state->n_signed++;
		break;
	case TOKEN_KEYWORD_UNSIGNED:
		state->n_unsigned++;
		break;
	case TOKEN_KEYWORD_DOUBLE:
		state->n_double++;
		break;
	case TOKEN_KEYWORD_STRUCT:
		state->n_struct++;
		/* is_token_variable_type() guarantees TOKEN_IDENTIFIER next */
		token_consume(tok);
		assert(is_token_type(*tok, TOKEN_IDENTIFIER));
		state->name = (**tok).val;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}
}

static WARN_UNUSED result_t
parse_basic_type_finalize(struct parse_basic_type_state *state,
                          struct ctype *var_type)
{
	size_t any = 0;
#define COUNT_WHILE_REJECTING_DUP(member)                                      \
	if (state->member > 1) {                                               \
		return make_result(ERR_PARSE_DECL_TYPE_DUPLICATE);             \
	}                                                                      \
	any += state->member;
	COUNT_WHILE_REJECTING_DUP(n_void);
	COUNT_WHILE_REJECTING_DUP(n_char);
	COUNT_WHILE_REJECTING_DUP(n_int);
	COUNT_WHILE_REJECTING_DUP(n_long); /* "long long" unsupported */
	COUNT_WHILE_REJECTING_DUP(n_signed);
	COUNT_WHILE_REJECTING_DUP(n_unsigned);
	COUNT_WHILE_REJECTING_DUP(n_double);
	COUNT_WHILE_REJECTING_DUP(n_struct);
#undef COUNT_WHILE_REJECTING_DUP

	if (state->n_signed > 0 && state->n_unsigned > 0) {
		return make_result(ERR_PARSE_DECL_TYPE_DUPLICATE);
	}

	if (any == 0) {
		return make_result(ERR_PARSE_DECL_EXPECT_TYPE);
	}

	if (state->n_struct > 0) {
		assert(state->name.sz > 0 && state->name.data != NULL);
		assert(any >= state->n_struct);
		const size_t others = any - state->n_struct;
		if (others > 0) {
			return make_result(ERR_PARSE_DECL_TYPE_STRUCT_INVALID);
		}
		var_type->t = CTYPE_STRUCT;
		var_type->tag_name = state->name;
		return RESULT_OK;
	}

	if (state->n_void > 0) {
		assert(any >= state->n_void);
		const size_t others = any - state->n_void;
		if (others > 0) {
			return make_result(ERR_PARSE_DECL_TYPE_VOID_INVALID);
		}
		var_type->t = CTYPE_VOID;
		return RESULT_OK;
	}

	if (state->n_double > 0) {
		assert(any >= state->n_double);
		const size_t others = any - state->n_double;
		if (others > 0) {
			return make_result(ERR_PARSE_DECL_TYPE_DOUBLE_INVALID);
		}
		var_type->t = CTYPE_DOUBLE;
		return RESULT_OK;
	}

	if (state->n_char > 0) {
		assert(any >= state->n_char);
		size_t others = any - state->n_char;
		others -= state->n_signed;
		others -= state->n_unsigned;
		if (others) {
			return make_result(ERR_PARSE_DECL_TYPE_CHAR_INVALID);
		}
		if (state->n_unsigned > 0) {
			var_type->t = CTYPE_UNSIGNED_CHAR;
		} else if (state->n_signed > 0) {
			var_type->t = CTYPE_SIGNED_CHAR;
		} else {
			var_type->t = CTYPE_CHAR;
		}
		return RESULT_OK;
	}

	if (state->n_long > 0) {
		if (state->n_unsigned > 0) {
			var_type->t = CTYPE_UNSIGNED_LONG;
		} else {
			var_type->t = CTYPE_LONG;
		}
		return RESULT_OK;
	}

	if (state->n_unsigned > 0) {
		var_type->t = CTYPE_UNSIGNED_INT;
	} else {
		assert(state->n_int == 1 || state->n_signed == 1);
		var_type->t = CTYPE_INT;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_specifiers(const struct token **tok,
                 enum ast_specifier *dst,
                 struct ctype *var_type)
{
	struct parse_basic_type_state state = {0};
	size_t specifier_count = 0;

	while (is_token_maybe_function_prefix(*tok)) {
		if (is_token_variable_type(*tok)) {
			parse_basic_type_accumulate(tok, &state);
		} else if (is_token_type(*tok, TOKEN_KEYWORD_STATIC)) {
			*dst = SPECIFIER_STATIC;
			++specifier_count;
		} else if (is_token_type(*tok, TOKEN_KEYWORD_EXTERN)) {
			*dst = SPECIFIER_EXTERN;
			++specifier_count;
		} else {
			assert(0); /* logic error in caller */
		}
		token_consume(tok);
	}

	if (specifier_count > 1) {
		return make_result(ERR_PARSE_DECL_SPECIFIER_DUPLICATE);
	}

	check(parse_basic_type_finalize(&state, var_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function_params_impl(Arena *arena,
                           const struct token **tok,
                           struct ast_parameter **dst,
                           long long int *count)
{
	const long long int count_in = *count;

	bool first = true;
	for (*count = 0; true; *count = *count + 1) {
		if (first) {
			first = false;
		} else {
			if (!is_token_type(*tok, TOKEN_COMMA)) {
				break;
			}
			token_consume(tok);
		}

		struct ctype parameter_type = {0};
		struct string_view identifier = {0};
		check(parse_type(arena, 0, tok, &parameter_type, &identifier));

		if (dst != NULL) {
			assert(*count <= count_in);
			(*dst)[*count].symbol.name = identifier;
			(*dst)[*count].symbol.unique = NOT_YET_UNIQUE;
			(*dst)[*count].parameter_type = parameter_type;
		}
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function_params(Arena *arena,
                      const struct token **tok,
                      struct ast_parameter **dst)
{
	if (is_token_type(*tok, TOKEN_KEYWORD_VOID) &&
	    is_token_type((**tok).next, TOKEN_PAREN_CLOSE)) {
		token_consume(tok);
		return RESULT_OK;
	}

	long long int count = 0;
	{
		const struct token *copy = *tok;
		check(parse_function_params_impl(arena, &copy, NULL, &count));
	}
	if (count > 0) {
		size_t bytes = sizeof(**dst) * (count + 1);
		*dst = arena_alloc(arena, bytes);
		check_if(*dst == NULL, ERR_PARSE_ALLOC);
		memset(*dst, 0, bytes);
		check(parse_function_params_impl(arena, tok, dst, &count));
	}

	return RESULT_OK;
}

const uint32_t PARSE_DECLARATOR_ABSTRACT = 0x200;
const uint32_t PARSE_DECLARATOR_ACCEPT_FUNCTION_PARAMS = 0x400;

struct token_group {
	struct token *tokens;
	struct token_group *child;
};

static void
token_group_debug_print(const struct token_group *group)
{
	size_t group_number = 0;
	for (; group != NULL; group = group->child) {
		debug("token group %zu", group_number);
		lex_debug_print(group->tokens);
		++group_number;
	}
}

static WARN_UNUSED result_t
parse_declarator_group_precheck(const struct token *tok)
{
	size_t closing_paren_countdown = 0;
	bool got_at_least_one_closing_paren = false;
	for (; tok != NULL; tok = tok->next) {
		if (is_token_type(tok, TOKEN_IDENTIFIER) ||
		    (is_token_type(tok, TOKEN_PAREN_CLOSE) &&
		     closing_paren_countdown == 0)) {
			break;
		}
		if (is_token_type(tok, TOKEN_PAREN_OPEN)) {
			closing_paren_countdown++;
		} else if (is_token_type(tok, TOKEN_PAREN_CLOSE)) {
			if (closing_paren_countdown > 0) {
				closing_paren_countdown--;
			}
			got_at_least_one_closing_paren = true;
		} else if (is_token_type(tok, TOKEN_ASTERISK) &&
		           got_at_least_one_closing_paren) {
			/*
			 * Pointer declarators can't appear after parenthesized
			 * expressions. For example: `(int (*)*)` -> invalid
			 */
			return make_result(
				ERR_PARSE_DECL_ATOM_POINTER_AFTER_PARENS);
		}
	}
	return RESULT_OK;
}

static result_t
parse_declarator_group_split(Arena *arena,
                             uint32_t flags,
                             const struct token **tok,
                             struct token_group **dst) WARN_UNUSED;

/*
 * Flatten expressions like `(((foobar)))` into `foobar`.
 */
static WARN_UNUSED size_t
parse_needs_weird_hack_for_paren_lonely_symbol(const struct token *tok)
{
	enum {
		COUNT_PAREN_OPEN,
		GOT_IDENTIFIER,
		GOT_MISMATCH,
	} parse_state = COUNT_PAREN_OPEN;

	size_t closing_paren_countdown = 0;
	size_t max_paren_nesting = 0;

	for (; tok != NULL && parse_state != GOT_MISMATCH; tok = tok->next) {
		switch (parse_state) {
		case COUNT_PAREN_OPEN:
			if (is_token_type(tok, TOKEN_PAREN_OPEN)) {
				closing_paren_countdown++;
				max_paren_nesting = closing_paren_countdown;
			} else if (is_token_type(tok, TOKEN_IDENTIFIER)) {
				parse_state = GOT_IDENTIFIER;
				if (closing_paren_countdown == 0) {
					return 0; /* no parens to flatten */
				}
			} else {
				parse_state = GOT_MISMATCH;
			}
			break;
		case GOT_IDENTIFIER:
			assert(closing_paren_countdown > 0);
			if (is_token_type(tok, TOKEN_PAREN_CLOSE)) {
				closing_paren_countdown--;
				if (closing_paren_countdown == 0) {
					assert(max_paren_nesting > 0);
					return max_paren_nesting;
				}
			} else {
				parse_state = GOT_MISMATCH;
			}
			break;
		default:
			parse_state = GOT_MISMATCH;
			break;
		}
	}

	return 0; /* tokens do not match required pattern */
}

static void
parse_declarator_group_split_is_done(uint32_t flags,
                                     const struct token **tok,
                                     bool *done,
                                     size_t *closing_paren_countdown)
{
#define FOREACH_LEX_DONE(F)                                                    \
	FOREACH_LEX_CHAR_REPEAT(F)                                             \
	FOREACH_LEX_CHAR_EQUALS_SIGN(F)
#define TO_E(candidate, enum_value) enum_value,
	/* tokens that forcibly end all declarator parsing */
	const enum lex_tokentype force_done[] = {FOREACH_LEX_DONE(TO_E)};
	/* tokens that forcibly end abstract declarator parsing */
	const enum lex_tokentype force_done_a[] = {FOREACH_LEX_KEYWORD(TO_E)};
#undef TO_E
#undef FOREACH_LEX_DONE

	if (is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		if (*closing_paren_countdown == 0) {
			*done = true;
		} else {
			(*closing_paren_countdown)--;
		}
	}

	const bool accept_fn_params =
		(0 != (flags & PARSE_DECLARATOR_ACCEPT_FUNCTION_PARAMS));

	if (is_token_type(*tok, TOKEN_SEMICOLON) ||
	    is_token_type(*tok, TOKEN_EQUAL_SIGN) ||
	    is_token_type(*tok, TOKEN_BRACE_OPEN) ||
	    (is_token_type(*tok, TOKEN_COMMA) && !accept_fn_params)) {
		*done = true;
	}

	for (size_t i = 0; i < ARRAY_SIZE(force_done); ++i) {
		if (is_token_type(*tok, force_done[i])) {
			*done = true;
		}
	}

	if (0 != (flags & PARSE_DECLARATOR_ABSTRACT)) {
		for (size_t i = 0; i < ARRAY_SIZE(force_done_a); ++i) {
			if (is_token_type(*tok, force_done_a[i])) {
				*done = true;
			}
		}
	}
}

static WARN_UNUSED result_t
parse_declarator_group_split_impl(Arena *arena,
                                  uint32_t flags,
                                  const struct token **tok,
                                  bool *done,
                                  bool *got_identifier,
                                  size_t *closing_paren_countdown,
                                  struct token_group **dst)
{
	while (*tok != NULL) {
		const size_t unpack_lonely_symbol =
			parse_needs_weird_hack_for_paren_lonely_symbol(*tok);
		for (size_t i = unpack_lonely_symbol; i > 0; --i) {
			assert(is_token_type(*tok, TOKEN_PAREN_OPEN));
			token_consume(tok);
		}

		if (is_token_type(*tok, TOKEN_PAREN_OPEN) &&
		    *got_identifier == false) {
			break;
		}

		parse_declarator_group_split_is_done(flags,
		                                     tok,
		                                     done,
		                                     closing_paren_countdown);
		if (*done == true) {
			break;
		}

		struct token **dst_token = &(**dst).tokens;
		while (*dst_token != NULL) {
			dst_token = &(**dst_token).next;
		}

		*dst_token = arena_alloc(arena, sizeof(**dst_token));
		check_if(*dst_token == NULL, ERR_PARSE_ALLOC);
		memcpy(*dst_token, *tok, sizeof(**dst_token));
		(**dst_token).next = NULL;

		if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
			*got_identifier = true;
		}
		if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
			(*closing_paren_countdown)++;
		}
		token_consume(tok);

		for (size_t i = unpack_lonely_symbol; i > 0; --i) {
			assert(is_token_type(*tok, TOKEN_PAREN_CLOSE));
			token_consume(tok);
		}
	}

	if (is_token_type(*tok, TOKEN_PAREN_OPEN) && *got_identifier == false) {
		token_consume(tok);
		if ((**dst).child != NULL) {
			return make_result(ERR_PARSE_DECL_ATOM_PARENS_INVALID);
		}
		check(parse_declarator_group_split(arena,
		                                   flags,
		                                   tok,
		                                   &(**dst).child));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_EXPECT_PAREN_CLOSE);
		}
		token_consume(tok);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_declarator_group_split(Arena *arena,
                             uint32_t flags,
                             const struct token **tok,
                             struct token_group **dst)
{
	assert(dst != NULL && *dst == NULL);

	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	bool done = false;
	bool got_identifier = false;
	size_t closing_paren_countdown = 0;

	while (!done && *tok != NULL) {
		check(parse_declarator_group_split_impl(
			arena,
			flags,
			tok,
			&done,
			&got_identifier,
			&closing_paren_countdown,
			dst));
	}

	return RESULT_OK;
}

struct token_group_scan {
	size_t got_indirection;
	const struct token *got_identifier;
	const struct token *got_subscript;
	const struct token *got_params;
};

static WARN_UNUSED result_t
parse_declarator_group_scan_one(uint32_t flags,
                                const struct token *t,
                                bool is_leaf_group,
                                struct token_group_scan *scan,
                                size_t *closing_paren_countdown)
{
	switch (t->token_type) {
	case TOKEN_ASTERISK:
		if (scan->got_params == NULL) {
			scan->got_indirection++;
		} /* else ignore function parameter indirection */
		break;
	case TOKEN_IDENTIFIER:
		if (scan->got_identifier == NULL) {
			if (!is_leaf_group) {
				return make_result(
					ERR_PARSE_DECL_ATOM_PARENS_INVALID);
			}
			scan->got_identifier = t;
		} /* else: ignore function parameter identifiers */
		break;
	case TOKEN_SQUARE_BRACKET_OPEN:
		if (*closing_paren_countdown == 0 &&
		    scan->got_subscript == NULL) {
			scan->got_subscript = t;
		} /* else: ignore function parameter subscripts */
		if (scan->got_subscript != NULL &&
		    scan->got_identifier == NULL &&
		    0 == (flags & PARSE_DECLARATOR_ABSTRACT) && is_leaf_group) {
			return make_result(ERR_PARSE_DECL_ATOM_EARLY_SUBSCRIPT);
		}
		break;
	case TOKEN_PAREN_OPEN:
		if (scan->got_params == NULL) {
			scan->got_params = t;
		} else if (*closing_paren_countdown == 0) {
			/*
			 * Treat multiple sets of function parameters
			 * as function pointer usage, and reject.
			 */
			return make_result(
				ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED);
		}
		(*closing_paren_countdown)++;
		break;
	case TOKEN_PAREN_CLOSE:
		if (*closing_paren_countdown > 0) {
			(*closing_paren_countdown)--;
		}
		break;
	case TOKEN_PERIOD:
	case TOKEN_ARROW:
		return make_result(ERR_PARSE_DECL_ATOM_MEMBER_ACCESS_INVALID);
		break;
	default:
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_declarator_group_scan(uint32_t flags,
                            const struct token_group *group,
                            struct token_group_scan *scan)
{
	const bool is_leaf_group = (group->child == NULL);
	size_t closing_paren_countdown = 0;
	for (const struct token *t = group->tokens; t != NULL; t = t->next) {
		check(parse_declarator_group_scan_one(
			flags,
			t,
			is_leaf_group,
			scan,
			&closing_paren_countdown));
	}
	return RESULT_OK;
}

struct declarator {
	size_t top_level_pointer_indirection;
	struct {
		size_t pointer_indirection;
	} prefix;
	struct {
		struct ctype *type_fragment;
	} postfix;
	struct {
		struct string_view *identifier;
		bool *got_function;
		struct ast_parameter **params;
	} out;
};

static WARN_UNUSED result_t
parse_declarator_group_postfix(Arena *arena,
                               const struct token **tok,
                               struct declarator *dst)
{
	struct ctype *new_fragment = NULL;
	struct ctype **dst_fragment = &new_fragment;

	while (*tok != NULL) {
		if (!is_token_type(*tok, TOKEN_SQUARE_BRACKET_OPEN)) {
			break;
		}
		token_consume(tok);

		if (!can_parse_constant(*tok)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_ARRAY_SIZE_NON_CONSTANT);
		}

		struct ast *constant = NULL;
		check(parse_constant(arena, tok, &constant));
		assert(constant != NULL);
		if (ctype_is_floating_point(&constant->expr_type)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_ARRAY_SIZE_FLOATING_POINT);
		}
		if (constant->u.num == 0) {
			return make_result(
				ERR_PARSE_DECL_ATOM_ARRAY_SIZE_NON_POSITIVE);
		}

		if (!is_token_type(*tok, TOKEN_SQUARE_BRACKET_CLOSE)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_EXPECT_SQ_BRACKET_CLOSE);
		}
		token_consume(tok);

		/* parse_constant() limited to range of strtoull() */
		assert(constant->u.num <= ULLONG_MAX);

		/* accumulate new fragment by appending */
		check(ctype_alloc(arena, dst_fragment));
		(**dst_fragment).t = CTYPE_ARRAY_OF;
		(**dst_fragment).sz = (long long unsigned)constant->u.num;
		dst_fragment = &(**dst_fragment).referent;
	}

	if (new_fragment != NULL) {
		for (; dst->prefix.pointer_indirection > 0;
		     --dst->prefix.pointer_indirection) {
			check(ctype_alloc(arena, dst_fragment));
			(**dst_fragment).t = CTYPE_POINTER_TO;
			dst_fragment = &(**dst_fragment).referent;
		}
		assert(dst->prefix.pointer_indirection == 0);

		assert(*dst_fragment == NULL);
		/* prepend new fragment onto existing type_fragment */
		*dst_fragment = dst->postfix.type_fragment;
		dst->postfix.type_fragment = new_fragment;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_declarator_group_fn_params(Arena *arena,
                                 const struct token **tok,
                                 struct declarator *dst)
{
	if (*tok == NULL) {
		return RESULT_OK;
	}

	assert(is_token_type(*tok, TOKEN_PAREN_OPEN));
	token_consume(tok);

	if (*dst->out.got_function) {
		return make_result(ERR_PARSE_DECL_ATOM_PARAMS_NESTING);
	}

	check(parse_function_params(arena, tok, dst->out.params));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(
			ERR_PARSE_DECL_ATOM_PARAMS_EXPECT_PAREN_CLOSE);
	}
	token_consume(tok);

	*dst->out.got_function = true;
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_declarator_by_group(Arena *arena,
                          uint32_t flags,
                          const struct token_group *group,
                          struct declarator *dst)
{
	assert(dst != NULL);

	struct token_group_scan scan = {0};
	check(parse_declarator_group_scan(flags, group, &scan));

	dst->prefix.pointer_indirection += scan.got_indirection;
	if (scan.got_identifier != NULL) {
		*dst->out.identifier = scan.got_identifier->val;
	}
	if (group->child == NULL && scan.got_subscript == NULL) {
		assert(dst->top_level_pointer_indirection == 0);
		dst->top_level_pointer_indirection =
			dst->prefix.pointer_indirection;
	}

	check(parse_declarator_group_postfix(arena, &scan.got_subscript, dst));
	check(parse_declarator_group_fn_params(arena, &scan.got_params, dst));

	if (group->child != NULL) {
		check(parse_declarator_by_group(arena,
		                                flags,
		                                group->child,
		                                dst));
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_declarator(Arena *arena,
                 uint32_t flags,
                 const struct token **tok,
                 struct declarator *dst)
{
	check(parse_declarator_group_precheck(*tok));

	struct token_group *group = NULL;
	check(parse_declarator_group_split(arena, flags, tok, &group));
	token_group_debug_print(group);

	check(parse_declarator_by_group(arena, flags, group, dst));

	if (0 == (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    dst->out.identifier->data == NULL) {
		return make_result(ERR_PARSE_DECL_IDENTIFIER_MISSING);
	}
	if (0 != (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    dst->out.identifier->data != NULL) {
		return make_result(ERR_PARSE_DECL_IDENTIFIER_UNEXPECTED);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
map_declarator_to_ctype(Arena *arena,
                        struct ctype *basic_type,
                        const struct declarator *src,
                        struct ctype **dst)
{
	assert(dst != NULL && *dst == NULL);

	for (size_t i = src->top_level_pointer_indirection; i > 0; --i) {
		check(ctype_alloc(arena, dst));
		(**dst).t = CTYPE_POINTER_TO;
		dst = &(**dst).referent;
	}

	if (src->postfix.type_fragment != NULL) {
		check(ctype_alloc(arena, dst));
		check(ctype_copy(arena, src->postfix.type_fragment, *dst));
		assert(ctype_is_pointer(*dst));
		while (*dst != NULL) {
			assert(ctype_is_pointer(*dst));
			dst = &(**dst).referent;
		}
	}

	check(ctype_alloc(arena, dst));
	check(ctype_copy(arena, basic_type, *dst));
	return RESULT_OK;
}

static WARN_UNUSED bool
ctype_has_fragment_array_of_incomplete(const struct ctype *c)
{
	// TODO: traverse struct members for incomplete types
	for (; ctype_is_pointer(c); c = c->referent) {
		if (ctype_is_array(c) && ctype_is_incomplete(c->referent)) {
			return true;
		}
	}
	return false;
}

static WARN_UNUSED result_t
parse_specifiers_and_type(Arena *arena,
                          const struct token **tok,
                          enum ast_specifier *dst,
                          struct ctype *var_type,
                          struct string_view *identifier,
                          bool *got_function,
                          struct ast_parameter **params)
{
	struct ctype basic_type = {0};
	check(parse_specifiers(tok, dst, &basic_type));

	struct declarator decl = {0};
	decl.out.identifier = identifier;
	decl.out.got_function = got_function;
	decl.out.params = params;
	check(parse_declarator(arena,
	                       PARSE_DECLARATOR_ACCEPT_FUNCTION_PARAMS,
	                       tok,
	                       &decl));

	struct ctype *tmp = NULL;
	check(map_declarator_to_ctype(arena, &basic_type, &decl, &tmp));
	check(ctype_copy(arena, tmp, var_type));

	if (ctype_has_fragment_array_of_incomplete(var_type)) {
		return make_result(ERR_PARSE_DECL_TYPE_ARRAY_INCOMPLETE);
	}
	return RESULT_OK;
}

result_t
parse_type(Arena *arena,
           uint32_t flags,
           const struct token **tok,
           struct ctype *var_type,
           struct string_view *identifier)
{
	struct parse_basic_type_state state = {0};
	while (is_token_variable_type(*tok)) {
		parse_basic_type_accumulate(tok, &state);
		token_consume(tok);
	}

	struct ctype basic_type = {0};
	check(parse_basic_type_finalize(&state, &basic_type));

	bool got_function = false;
	struct string_view dummy_id = {0};
	struct ast_parameter *dummy_params = NULL;

	struct declarator decl = {0};
	decl.out.identifier = identifier != NULL ? identifier : &dummy_id;
	decl.out.got_function = &got_function;
	decl.out.params = &dummy_params;
	check(parse_declarator(arena, flags, tok, &decl));

	if (got_function) {
		return make_result(ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED);
	}

	struct ctype *tmp = NULL;
	check(map_declarator_to_ctype(arena, &basic_type, &decl, &tmp));
	check(ctype_copy(arena, tmp, var_type));

	if (ctype_has_fragment_array_of_incomplete(var_type)) {
		return make_result(ERR_PARSE_DECL_TYPE_ARRAY_INCOMPLETE);
	}
	return RESULT_OK;
}

static result_t parse_initializer(Arena *arena,
                                  const struct token **tok,
                                  struct ast **dst) WARN_UNUSED;

static WARN_UNUSED result_t
parse_initializer_arr(Arena *arena, const struct token **tok, struct flat **dst)
{
	assert(is_token_type(*tok, TOKEN_BRACE_OPEN));

	do {
		token_consume(tok);
		if (is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
			break; /* allow final comma after last field */
		}
		check(flat_alloc(arena, dst));
		check(parse_initializer(arena, tok, &(**dst).car));
		dst = &(**dst).cdr;
	} while (is_token_type(*tok, TOKEN_COMMA));

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_DECL_EXPECT_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_initializer(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_EXPRESSION_INITIALIZER));

	if (is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		check(parse_initializer_arr(arena, tok, &(**dst).u.init.multi));
	} else {
		check(parse_expr(arena, tok, &(**dst).u.init.single, 0));
	}

	return RESULT_OK;
}

const uint32_t PARSE_DECLARATION_ACCEPT_FUNCTION = 0x100;

result_t
parse_fn_or_var_or_struct_declaration(Arena *arena,
                                      uint32_t flags,
                                      const struct token **tok,
                                      struct ast **dst)
{
	if (is_token_struct_prefix(*tok)) {
		check(parse_struct_declaration(arena, tok, dst));
		return RESULT_OK;
	}

	enum ast_specifier fn_or_var_specifier = SPECIFIER_NONE;
	struct ctype fn_return_or_var_type = {0};
	struct string_view fn_or_var_name = {0};
	bool got_function = false;
	struct ast_parameter *fn_params_maybe = NULL;

	check(parse_specifiers_and_type(arena,
	                                tok,
	                                &fn_or_var_specifier,
	                                &fn_return_or_var_type,
	                                &fn_or_var_name,
	                                &got_function,
	                                &fn_params_maybe));

	if (!got_function) {
		/* handle non-function variable declaration */
		check(parse_alloc(arena, dst, NODE_DECLARATION));
		(**dst).u.declare.specifier = fn_or_var_specifier;
		check(ctype_copy(arena,
		                 &fn_return_or_var_type,
		                 &(**dst).u.declare.var_type));
		(**dst).u.declare.identifier.name = fn_or_var_name;

		if (ctype_is_void(&fn_return_or_var_type)) {
			return make_result(ERR_PARSE_DECL_TYPE_VOID_VAR_TYPE,
			                   fn_or_var_name.data,
			                   fn_or_var_name.sz);
		}

		if (is_token_type(*tok, TOKEN_EQUAL_SIGN)) {
			token_consume(tok);
			check(parse_initializer(arena,
			                        tok,
			                        &(**dst).u.declare.init));
		}

		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_DECL_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);

		return RESULT_OK;
	} /* else: handle function declaration */

	if (0 == (flags & PARSE_DECLARATION_ACCEPT_FUNCTION)) {
		return make_result(ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED);
	}

	check(parse_alloc(arena, dst, NODE_FUNCTION));
	(**dst).u.function.specifier = fn_or_var_specifier;
	check(ctype_copy(arena,
	                 &fn_return_or_var_type,
	                 &(**dst).u.function.return_type));
	(**dst).u.function.identifier.name = fn_or_var_name;
	(**dst).u.function.params = fn_params_maybe;

	FOREACH_FUNCTION_PARAMETER (cur, fn_params_maybe) {
		if (ctype_is_void(&cur->parameter_type)) {
			return make_result(ERR_PARSE_DECL_TYPE_VOID_PARAM_TYPE,
			                   cur->symbol.name.data,
			                   cur->symbol.name.sz);
		}
	}

	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		assert((**dst).u.function.block == NULL);
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		check(parse_block(arena, tok, &(**dst).u.function.block));
	} else {
		return make_result(
			ERR_PARSE_FUNC_EXPECT_TOKEN_SEMICOLON_OR_BRACE_OPEN);
	}

	return RESULT_OK;
}
