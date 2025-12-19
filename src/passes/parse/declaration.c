#include "passes/parse/declaration.h"

#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/block.h"
#include "passes/parse/constant.h"
#include "passes/parse/expression.h"
#include "passes/parse/token.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h> /* for ULLONG_MAX */
#include <string.h> /* for memset() */

struct parse_basic_type_state {
	size_t n_int;
	size_t n_long;
	size_t n_signed;
	size_t n_unsigned;
	size_t n_double;
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
	default:
		assert(0); /* logic error in caller */
		break;
	}
}

static WARN_UNUSED result_t
parse_basic_type_finalize(struct parse_basic_type_state *state,
                          struct ctype *var_type)
{
	if (state->n_int > 1 ||      /* int int -- invalid                 */
	    state->n_long > 1 ||     /* long long -- unsupported           */
	    state->n_signed > 1 ||   /* signed signed -- invalid           */
	    state->n_unsigned > 1 || /* unsigned unsigned -- invalid       */
	    state->n_double > 1 ||   /* double double -- invalid           */
	    (state->n_signed > 0 &&  /* signed/unsigned mutually exclusive */
	     state->n_unsigned > 0)) {
		return make_result(ERR_PARSE_DECL_TYPE_DUPLICATE);
	}

	if (state->n_int == 0 &&      /* Any particular type may occur zero   */
	    state->n_long == 0 &&     /* times, but there must exist at least */
	    state->n_signed == 0 &&   /* one non-zero count, from the valid   */
	    state->n_unsigned == 0 && /* options available.                   */
	    state->n_double == 0) {
		return make_result(ERR_PARSE_DECL_EXPECT_TYPE);
	}

	if (state->n_double > 0) {
		if (state->n_int > 0 ||      /* int double -- invalid      */
		    state->n_long > 0 ||     /* long double -- unsupported */
		    state->n_signed > 0 ||   /* signed double -- invalid   */
		    state->n_unsigned > 0) { /* unsigned double -- invalid */
			return make_result(ERR_PARSE_DECL_TYPE_DOUBLE_INVALID);
		}
		var_type->t = CTYPE_DOUBLE;
		return RESULT_OK;
	}

	switch (state->n_long) {
	case 1:
		if (state->n_unsigned > 0) {
			var_type->t = CTYPE_UNSIGNED_LONG;
		} else {
			var_type->t = CTYPE_LONG;
		}
		break;
	case 0:
		if (state->n_unsigned > 0) {
			var_type->t = CTYPE_UNSIGNED_INT;
		} else {
			assert(state->n_int == 1 || state->n_signed == 1);
			var_type->t = CTYPE_INT;
		}
		break;
	default:
		assert(0); /* logic error in caller */
		break;
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
	if (is_token_type(*tok, TOKEN_KEYWORD_VOID)) {
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

struct declarator {
	size_t basic_type_pointer_indirection;
	struct {
		size_t pointer_indirection;
	} prefix;
	struct {
		struct ctype type_fragment;
		struct ctype **current_referent;
	} postfix;
	struct {
		struct string_view *identifier;
		bool *got_function;
		struct ast_parameter **params;
	} out;
};

const uint32_t PARSE_DECLARATOR_ABSTRACT = 0x200;

static WARN_UNUSED result_t
parse_declarator(Arena *arena,
                 uint32_t flags,
                 const struct token **tok,
                 struct declarator *dst)
{
	assert(dst != NULL);

	if (0 != (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		/* leave TOKEN_PAREN_CLOSE in place for caller to consume */
		return RESULT_OK;
	}

	if (is_token_type(*tok, TOKEN_ASTERISK)) {
		token_consume(tok);
		dst->prefix.pointer_indirection++;
		check(parse_declarator(arena, flags, tok, dst));
		return RESULT_OK;
	}

	size_t maybe_array_of_pointers = 0;

	if (0 == (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    is_token_type(*tok, TOKEN_IDENTIFIER)) {
		*dst->out.identifier = (**tok).val;
		token_consume(tok);

		maybe_array_of_pointers = dst->prefix.pointer_indirection;
		dst->prefix.pointer_indirection = 0;
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		token_consume(tok);

		maybe_array_of_pointers = dst->prefix.pointer_indirection;
		dst->prefix.pointer_indirection = 0;
		check(parse_declarator(arena, flags, tok, dst));

		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_EXPECT_PAREN_CLOSE);
		}
		token_consume(tok);
	} else {
		return make_result(ERR_PARSE_DECL_ATOM_EXPECT_REASONABLE);
	}

	if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		if (*dst->out.got_function) {
			return make_result(ERR_PARSE_DECL_ATOM_PARAMS_NESTING);
		}
		token_consume(tok);

		check(parse_function_params(arena, tok, dst->out.params));

		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_PARAMS_EXPECT_PAREN_CLOSE);
		}
		token_consume(tok);

		*dst->out.got_function = true;
	}

	while (is_token_type(*tok, TOKEN_SQUARE_BRACKET_OPEN)) {
		token_consume(tok);

		if (!is_token_type(*tok, TOKEN_CONSTANT)) {
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

		if (!ctype_is_pointer(&dst->postfix.type_fragment)) {
			dst->postfix.type_fragment.t = CTYPE_ARRAY_OF;
			dst->postfix.type_fragment.sz =
				(long long unsigned)constant->u.num;
			dst->postfix.current_referent =
				&dst->postfix.type_fragment.referent;
		} else {
			assert(dst->postfix.current_referent != NULL);
			assert(*dst->postfix.current_referent == NULL);
			check(ctype_alloc(arena,
			                  dst->postfix.current_referent));
			(**dst->postfix.current_referent).t = CTYPE_ARRAY_OF;
			(**dst->postfix.current_referent).sz =
				(long long unsigned)constant->u.num;
			dst->postfix.current_referent =
				&(**dst->postfix.current_referent).referent;
		}

		for (; maybe_array_of_pointers > 0; --maybe_array_of_pointers) {
			assert(*dst->postfix.current_referent == NULL);
			check(ctype_alloc(arena,
			                  dst->postfix.current_referent));
			(**dst->postfix.current_referent).t = CTYPE_POINTER_TO;
			dst->postfix.current_referent =
				&(**dst->postfix.current_referent).referent;
		}
		assert(maybe_array_of_pointers == 0);
	}

	if (maybe_array_of_pointers > 0) {
		/*
		 * Pointer indirection did not apply to array element (sub)type
		 * via subscript postfix operator. Tell caller to apply pointer
		 * indirection to basic type instead.
		 */
		dst->basic_type_pointer_indirection = maybe_array_of_pointers;
		maybe_array_of_pointers = 0;
	}

	assert(maybe_array_of_pointers == 0);
	return RESULT_OK;
}

static WARN_UNUSED result_t
map_declarator_to_ctype(Arena *arena,
                        struct ctype *basic_type,
                        const struct declarator *src,
                        struct ctype **dst)
{
	assert(dst != NULL);
	assert(*dst == NULL);

	for (size_t i = src->basic_type_pointer_indirection; i > 0; --i) {
		check(ctype_alloc(arena, dst));
		(**dst).t = CTYPE_POINTER_TO;
		dst = &(**dst).referent;
	}

	if (ctype_is_pointer(&src->postfix.type_fragment)) {
		check(ctype_alloc(arena, dst));
		check(ctype_copy(arena, &src->postfix.type_fragment, *dst));
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
	check(parse_declarator(arena, 0, tok, &decl));

	struct ctype *tmp = NULL;
	check(map_declarator_to_ctype(arena, &basic_type, &decl, &tmp));
	check(ctype_copy(arena, tmp, var_type));
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
	struct ast_parameter *dummy_params = NULL;

	struct declarator decl = {0};
	decl.out.identifier = identifier;
	decl.out.got_function = &got_function;
	decl.out.params = &dummy_params;
	check(parse_declarator(arena, flags, tok, &decl));

	if (got_function) {
		return make_result(ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED);
	}

	struct ctype *tmp = NULL;
	check(map_declarator_to_ctype(arena, &basic_type, &decl, &tmp));
	check(ctype_copy(arena, tmp, var_type));
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
parse_fn_or_var_declaration(Arena *arena,
                            uint32_t flags,
                            const struct token **tok,
                            struct ast **dst)
{
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
