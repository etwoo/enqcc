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
	enum {
		DECLARATOR_UNSET,
		DECLARATOR_ABSTRACT_BASE,
		DECLARATOR_IDENTIFIER,
		DECLARATOR_PARENTHESIZED,
		DECLARATOR_POINTER,
		DECLARATOR_ARRAY,
	} atom[2];
	union {
		struct declarator *in_parens;
		struct declarator *pointee;
		struct {
			struct declarator *element;
			long long unsigned sz;
		} array;
	} u[2];
};

static WARN_UNUSED result_t
declarator_alloc(Arena *arena, struct declarator **dst)
{
	assert(dst != NULL && *dst == NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

const uint32_t PARSE_DECLARATOR_ABSTRACT = 0x200;

static WARN_UNUSED result_t
parse_declarator(Arena *arena,
                 uint32_t flags,
                 const struct token **tok,
                 struct declarator **dst,
                 struct string_view *identifier,
                 bool *got_function,
                 struct ast_parameter **params)
{
	assert(dst != NULL);

	if (0 != (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom[0] = DECLARATOR_ABSTRACT_BASE;
		assert(identifier == NULL);
		return RESULT_OK;
		/* leave TOKEN_PAREN_CLOSE in place for caller to consume */
	}

	if (is_token_type(*tok, TOKEN_ASTERISK)) {
		token_consume(tok);
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom[0] = DECLARATOR_POINTER;
		check(parse_declarator(arena,
		                       flags,
		                       tok,
		                       &(**dst).u[0].pointee,
		                       identifier,
		                       got_function,
		                       params));
		return RESULT_OK;
	}

	if (0 == (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom[0] = DECLARATOR_IDENTIFIER;
		assert(identifier != NULL);
		*identifier = (**tok).val;
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		token_consume(tok);
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom[0] = DECLARATOR_PARENTHESIZED;
		check(parse_declarator(arena,
		                       flags,
		                       tok,
		                       &(**dst).u[0].in_parens,
		                       identifier,
		                       got_function,
		                       params));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_EXPECT_PAREN_CLOSE);
		}
		token_consume(tok);
	} else {
		return make_result(ERR_PARSE_DECL_ATOM_EXPECT_REASONABLE);
	}

	if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		if (*got_function) {
			return make_result(ERR_PARSE_DECL_ATOM_PARAMS_NESTING);
		}
		token_consume(tok);

		check(parse_function_params(arena, tok, params));

		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_PARAMS_EXPECT_PAREN_CLOSE);
		}
		token_consume(tok);

		assert(got_function != NULL);
		*got_function = true;
	}

	struct declarator *arr_leftmost = NULL;
	struct declarator *arr_rightmost = NULL;

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

		struct declarator *tmp = NULL;
		check(declarator_alloc(arena, &tmp));
		assert(tmp != NULL);
		tmp->atom[1] = DECLARATOR_ARRAY;

		/* parse_constant() limited to range of strtoull() */
		assert(constant->u.num <= ULLONG_MAX);
		tmp->u[1].array.sz = (long long unsigned)constant->u.num;

		if (arr_leftmost == NULL) {
			arr_leftmost = tmp;
		}

		if (arr_rightmost != NULL) {
			/* accumulate nesting of 2D/3D/etc arrays */
			arr_rightmost->u[1].array.element = tmp;
		}
		arr_rightmost = tmp;
	}

	if (arr_leftmost != NULL) {
		assert((**dst).atom[0] != DECLARATOR_UNSET);
		assert((**dst).atom[1] == DECLARATOR_UNSET);
		(**dst).atom[1] = DECLARATOR_ARRAY;
		(**dst).u[1].array.sz = arr_leftmost->u[1].array.sz;
		(**dst).u[1].array.element = arr_leftmost->u[1].array.element;
	}

	return RESULT_OK;
}

// TODO: refactor/rethink declarator->ctype mapping, currently incomprehensible
static WARN_UNUSED result_t
map_declarator_to_ctype(Arena *arena,
                        struct ctype *accum,
                        const struct declarator *src,
                        struct ctype **dst)
{
	info("%s(): accum %u lhs %u rhs %u",
	     __func__,
	     accum == NULL ? -1 : accum->t,
	     src->atom[0],
	     src->atom[1]);

	assert(src != NULL);
	assert(dst != NULL);

	struct ctype *dst_rhs = NULL;
	if (src->atom[1] != DECLARATOR_UNSET) {
		assert(src->atom[1] == DECLARATOR_ARRAY);

		check(ctype_alloc(arena, &dst_rhs));
		dst_rhs->t = CTYPE_ARRAY_OF;
		dst_rhs->sz = src->u[1].array.sz;
		info("%s() array size %llu", __func__, src->u[1].array.sz);

		if (src->u[1].array.element != NULL) {
			check(map_declarator_to_ctype(arena,
			                              accum,
			                              src->u[1].array.element,
			                              &dst_rhs->referent));
		}
	}

	char tmp[128] = {0};

	bool parens = false;
	struct ctype *dst_lhs = NULL;
	switch (src->atom[0]) {
	case DECLARATOR_UNSET:
		break;
	case DECLARATOR_ABSTRACT_BASE:
	case DECLARATOR_IDENTIFIER:
		info("%s() base case", __func__);
		check(ctype_alloc(arena, &dst_lhs));
		check(ctype_copy(arena, accum, dst_lhs));
		break;
	case DECLARATOR_PARENTHESIZED:
		info("%s() parens", __func__);
		parens = true;
		check(map_declarator_to_ctype(arena,
		                              NULL,
		                              src->u[0].in_parens,
		                              &dst_lhs));
		break;
	case DECLARATOR_POINTER:
		check(ctype_alloc(arena, &dst_lhs));
		dst_lhs->t = CTYPE_POINTER_TO;
		dst_lhs->referent = accum;
		info("%s() current accum before recursive call %s",
		     __func__,
		     ctype_to_str(dst_lhs, tmp, sizeof(tmp)));
		if (src->u[0].pointee != NULL) {
			check(map_declarator_to_ctype(arena,
			                              dst_lhs,
			                              src->u[0].pointee,
			                              dst));
		}
		info("%s() current accum after recursive call %s",
		     __func__,
		     ctype_to_str(dst_lhs, tmp, sizeof(tmp)));
		info("%s() current dst after recursive call %s",
		     __func__,
		     ctype_to_str(*dst, tmp, sizeof(tmp)));
		break;
	case DECLARATOR_ARRAY:
		assert(0); /* logic error in caller */
		break;
	}

	if (parens && dst_lhs != NULL && dst_rhs != NULL) {
		info("%s() merging lhs %s",
		     __func__,
		     ctype_to_str(dst_lhs, tmp, sizeof(tmp)));

		struct ctype *dst_rhs_innermost = dst_rhs;
		assert(ctype_is_array(dst_rhs_innermost));
		while (dst_rhs_innermost->referent != NULL &&
		       ctype_is_array(dst_rhs_innermost->referent)) {
			dst_rhs_innermost = dst_rhs_innermost->referent;
		}
		assert(ctype_is_array(dst_rhs_innermost));
		assert(dst_rhs_innermost->referent == NULL);
		dst_rhs_innermost->referent = accum;

		struct ctype *dst_lhs_innermost = dst_lhs;
		while (ctype_is_pointer(dst_lhs_innermost) &&
		       dst_lhs_innermost->referent != NULL &&
		       ctype_is_pointer(dst_lhs_innermost->referent)) {
			dst_lhs_innermost = dst_lhs_innermost->referent;
		}
		assert(ctype_is_pointer(dst_lhs_innermost));
		assert(dst_lhs_innermost->referent == NULL);
		info("%s() innermost lhs %s",
		     __func__,
		     ctype_to_str(dst_lhs_innermost, tmp, sizeof(tmp)));

		dst_lhs_innermost->referent = dst_rhs;
		*dst = dst_lhs;
		info("%s() merged lhs %s",
		     __func__,
		     ctype_to_str(dst_lhs, tmp, sizeof(tmp)));
	} else if (dst_lhs != NULL && dst_rhs != NULL) {
		info("%s() merging rhs %s",
		     __func__,
		     ctype_to_str(dst_rhs, tmp, sizeof(tmp)));
		struct ctype *dst_rhs_innermost = dst_rhs;
		assert(ctype_is_array(dst_rhs_innermost));
		while (dst_rhs_innermost->referent != NULL &&
		       ctype_is_array(dst_rhs_innermost->referent)) {
			dst_rhs_innermost = dst_rhs_innermost->referent;
		}
		assert(ctype_is_array(dst_rhs_innermost));
		assert(dst_rhs_innermost->referent == NULL);
		info("%s() innermost rhs %s",
		     __func__,
		     ctype_to_str(dst_rhs_innermost, tmp, sizeof(tmp)));
		dst_rhs_innermost->referent = dst_lhs;
		*dst = dst_rhs;
		info("%s() merged rhs %s",
		     __func__,
		     ctype_to_str(dst_rhs, tmp, sizeof(tmp)));
	} else if (dst_rhs != NULL && *dst == NULL) {
		*dst = dst_rhs;
		info("%s() force rhs %s",
		     __func__,
		     ctype_to_str(*dst, tmp, sizeof(tmp)));
	} else if (dst_lhs != NULL && *dst == NULL) {
		*dst = dst_lhs;
		info("%s() force lhs %s",
		     __func__,
		     ctype_to_str(*dst, tmp, sizeof(tmp)));
	}

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

	struct declarator *remainder = NULL;
	check(parse_declarator(arena,
	                       0,
	                       tok,
	                       &remainder,
	                       identifier,
	                       got_function,
	                       params));

	struct ctype *tmp = NULL;
	check(map_declarator_to_ctype(arena, &basic_type, remainder, &tmp));
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

	struct declarator *remainder = NULL;
	bool got_function = false;
	struct ast_parameter *params = NULL;
	check(parse_declarator(arena,
	                       flags,
	                       tok,
	                       &remainder,
	                       identifier,
	                       &got_function,
	                       &params));
	if (got_function) {
		return make_result(ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED);
	}

	struct ctype *tmp = NULL;
	check(map_declarator_to_ctype(arena, &basic_type, remainder, &tmp));
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
		if ((**dst).u.init.multi == NULL) {
			// TODO: move this check to sema.c
			return make_result(
				ERR_PARSE_DECL_EMPTY_COMPOUND_INITIALIZER);
		}
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
