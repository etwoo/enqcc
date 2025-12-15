#include "passes/parse/declaration.h"

#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/block.h"
#include "passes/parse/constant.h"
#include "passes/parse/expression.h"
#include "passes/parse/token.h"

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
		DECLARATOR_ABSTRACT_BASE,
		DECLARATOR_IDENTIFIER,
		DECLARATOR_PARENTHESIZED,
		DECLARATOR_POINTER,
		DECLARATOR_ARRAY,
	} atom;
	union {
		struct declarator *in_parens;
		struct declarator *pointee;
		struct {
			struct declarator *element;
			long long unsigned sz;
		} array;
	} u;
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
		(**dst).atom = DECLARATOR_ABSTRACT_BASE;
		assert(identifier == NULL);
		return RESULT_OK;
		/* leave TOKEN_PAREN_CLOSE in place for caller to consume */
	}

	if (is_token_type(*tok, TOKEN_ASTERISK)) {
		token_consume(tok);
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom = DECLARATOR_POINTER;
		check(parse_declarator(arena,
		                       flags,
		                       tok,
		                       &(**dst).u.pointee,
		                       identifier,
		                       got_function,
		                       params));
		return RESULT_OK;
	}

	if (0 == (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom = DECLARATOR_IDENTIFIER;
		assert(identifier != NULL);
		*identifier = (**tok).val;
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		token_consume(tok);
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom = DECLARATOR_PARENTHESIZED;
		check(parse_declarator(arena,
		                       flags,
		                       tok,
		                       &(**dst).u.in_parens,
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

	while (is_token_type(*tok, TOKEN_SQUARE_BRACKET_OPEN)) {
		token_consume(tok);

		struct ast *constant = NULL;
		check(parse_constant(arena, tok, &constant));
		assert(constant && constant->node_type == NODE_CONSTANT);

		if (ctype_is_floating_point(&constant->expr_type)) {
			return make_result(
				ERR_PARSE_DECL_ATOM_ARRAY_SIZE_FLOATING_POINT);
		}
		if (constant->u.num <= 0) {
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
		tmp->atom = DECLARATOR_ARRAY;

		/* parse_constant() limited to range of strtoull() */
		assert(constant->u.num <= ULLONG_MAX);
		tmp->u.array.sz = (long long unsigned)constant->u.num;

		/* array declarator as postfix -> flip tmp and dst! */
		tmp->u.array.element = *dst;
		*dst = tmp;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
map_declarator_to_ctype(Arena *arena,
                        const struct ctype *basic,
                        const struct declarator *src,
                        struct ctype **dst)
{
	assert(src != NULL);
	assert(dst != NULL && *dst == NULL);

	switch (src->atom) {
	case DECLARATOR_ABSTRACT_BASE:
	case DECLARATOR_IDENTIFIER:
		check(ctype_alloc(arena, dst));
		check(ctype_copy(arena, basic, *dst));
		break;
	case DECLARATOR_PARENTHESIZED:
		check(map_declarator_to_ctype(arena,
		                              basic,
		                              src->u.in_parens,
		                              dst));
		break;
	case DECLARATOR_POINTER:
		check(ctype_alloc(arena, dst));
		(**dst).t = CTYPE_POINTER_TO;
		check(map_declarator_to_ctype(arena,
		                              basic,
		                              src->u.pointee,
		                              &(**dst).referent));
		break;
	case DECLARATOR_ARRAY:
		check(ctype_alloc(arena, dst));
		(**dst).t = CTYPE_ARRAY_OF;
		(**dst).sz = src->u.array.sz;
		check(map_declarator_to_ctype(arena,
		                              basic,
		                              src->u.array.element,
		                              &(**dst).referent));
		break;
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
			// TODO: check for compound initializer
			check(parse_expr(arena,
			                 tok,
			                 &(**dst).u.declare.init,
			                 0));
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
