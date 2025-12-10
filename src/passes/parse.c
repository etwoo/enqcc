#include "passes/parse.h"

#include "lang/symbol.h"
#include "passes.h"
#include "passes/lex.h"
#include "passes/parse/alloc.h"
#include "passes/parse/constant.h"
#include "passes/parse/resolve.h"
#include "sys/array.h"
#include "sys/debug.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

static WARN_UNUSED result_t
flat_alloc(Arena *arena, struct flat **dst)
{
	assert(dst != NULL && *dst == NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_alloc_if_unset(Arena *arena, struct ast **dst)
{
	if (*dst == NULL) {
		checked_alloc(arena, dst, NODE_EXPRESSION_NULL);
	}
	return RESULT_OK;
}

static WARN_UNUSED bool
is_token_type(const struct token *tok, unsigned expected)
{
	return tok != NULL && tok->token_type == expected;
}

static void
token_consume(const struct token **tok)
{
	assert(*tok != NULL);
	*tok = (**tok).next;
}

static WARN_UNUSED bool
is_token_variable_type(const struct token *tok)
{
	if (tok == NULL) {
		return false;
	}
	switch (tok->token_type) {
	case TOKEN_KEYWORD_INT:
	case TOKEN_KEYWORD_LONG:
	case TOKEN_KEYWORD_SIGNED:
	case TOKEN_KEYWORD_UNSIGNED:
	case TOKEN_KEYWORD_DOUBLE:
		return true;
	default:
		break;
	}
	return false;
}

static WARN_UNUSED bool
is_token_maybe_function_prefix(const struct token *tok)
{
	return is_token_variable_type(tok) ||
	       is_token_type(tok, TOKEN_KEYWORD_STATIC) ||
	       is_token_type(tok, TOKEN_KEYWORD_EXTERN);
}

static result_t parse_expr(Arena *arena,
                           const struct token **tok,
                           struct ast **dst,
                           unsigned minimum_precedence) WARN_UNUSED;

static const uint32_t PARSE_DECLARATOR_ABSTRACT = 0x1;

static result_t parse_type(Arena *arena,
                           uint32_t flags,
                           const struct token **tok,
                           struct ctype *var_type,
                           struct string_view *identifier) WARN_UNUSED;

static WARN_UNUSED result_t
parse_symbol(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_IDENTIFIER));

	struct string_view str = (**tok).val;
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		checked_alloc(arena, dst, NODE_EXPRESSION_VARIABLE_USAGE);
		(**dst).u.var.name = str;
		(**dst).u.var.unique = NOT_YET_UNIQUE;
		return RESULT_OK;
	}

	checked_alloc(arena, dst, NODE_EXPRESSION_FUNCTION_CALL);
	(**dst).u.call.identifier.name = str;
	(**dst).u.call.identifier.unique = NOT_YET_UNIQUE;

	assert(is_token_type(*tok, TOKEN_PAREN_OPEN));
	token_consume(tok);

	if (is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		token_consume(tok);
		return RESULT_OK; /* zero-arg function call */
	}

	struct flat **dst_args = &(**dst).u.call.args;
	while (true) {
		check(flat_alloc(arena, dst_args));
		check(parse_expr(arena, tok, &(**dst_args).car, 0));
		dst_args = &(**dst_args).cdr;

		if (!is_token_type(*tok, TOKEN_COMMA)) {
			break;
		}
		token_consume(tok);
	}

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_CALL_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED bool
parse_needs_weird_hack_for_cast_lhs_precedence(const struct ast *a)
{
	assert(a->node_type == NODE_EXPRESSION_CAST);
	switch (a->u.cast.expr->node_type) {
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		return true;
	default:
		break;
	}
	return false;
}

static WARN_UNUSED result_t
parse_factor(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(dst != NULL && *dst == NULL);
	size_t got_match = SIZE_MAX;

#define TO_CANDIDATE(nodet, tokent) {tokent, NODE_##nodet},
	struct {
		enum lex_tokentype token_type;
		enum ast_nodetype node_type;
	} prefix_ops[] = {FOREACH_AST_NODE_EXPRESSION_PREFIX_OP(TO_CANDIDATE)};
#undef TO_CANDIDATE
	for (size_t i = 0; i < ARRAY_SIZE(prefix_ops); ++i) {
		if (is_token_type(*tok, prefix_ops[i].token_type)) {
			got_match = i;
			break;
		}
	}

	if (got_match < SIZE_MAX) {
		assert(got_match < ARRAY_SIZE(prefix_ops));
		checked_alloc(arena, dst, prefix_ops[got_match].node_type);
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_constant(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_symbol(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN) &&
	           *tok != NULL && /* avoid NULL dereference on (**tok).next */
	           is_token_variable_type((**tok).next)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_EXPRESSION_CAST);
		check(parse_type(arena,
		                 PARSE_DECLARATOR_ABSTRACT,
		                 tok,
		                 &(**dst).u.cast.to_type,
		                 NULL));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_CAST_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
		check(parse_expr(arena, tok, &(**dst).u.cast.expr, 0));
		assert((**dst).u.cast.expr != NULL);
		if (parse_needs_weird_hack_for_cast_lhs_precedence(*dst)) {
			struct ast *cast_original = *dst;
			struct ast *assign_original = (**dst).u.cast.expr;
			struct ast *lhs_original =
				(**dst).u.cast.expr->u.op_binary.lhs;
			/* cast LHS of assignment, not assignment as a whole */
			(**dst).u.cast.expr->u.op_binary.lhs = cast_original;
			(**dst).u.cast.expr->u.op_binary.lhs->u.cast.expr =
				lhs_original;
			*dst = assign_original;
		}
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		checked_alloc(arena, dst, NODE_EXPRESSION_PAREN_ENCLOSED);
		token_consume(tok);
		check(parse_expr(arena, tok, &(**dst).u.op_unary.operand, 0));
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
	} else {
		return make_result(ERR_PARSE_EXPR_EXPECT_REASONABLE);
	}

	assert(*dst != NULL);

	struct ast *post = NULL;
	if (is_token_type(*tok, TOKEN_PLUS_SIGN_PLUS_SIGN)) {
		checked_alloc(arena, &post, NODE_EXPRESSION_POSTINCREMENT);
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)) {
		checked_alloc(arena, &post, NODE_EXPRESSION_POSTDECREMENT);
		token_consume(tok);
	} else {
		return RESULT_OK;
	}
	/*
	 * Wrap the inner expr in postincrement/postdecrement.
	 */
	post->u.op_unary.operand = *dst;
	*dst = post;

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expr_check_next_token(Arena *arena,
                            const struct token *tok,
                            struct ast **a)
{
#define TO_CANDIDATE(nodet, tokent) {tokent, NODE_##nodet},
	struct {
		enum lex_tokentype token_type;
		enum ast_nodetype node_type;
	} infix_ops[] = {FOREACH_AST_NODE_EXPRESSION_INFIX_OP(TO_CANDIDATE)};
#undef TO_CANDIDATE
	for (size_t i = 0; i < ARRAY_SIZE(infix_ops); ++i) {
		if (is_token_type(tok, infix_ops[i].token_type)) {
			checked_alloc(arena, a, infix_ops[i].node_type);
			break;
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_ternary_middle(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_expr(arena, tok, dst, 0));
	if (!is_token_type(*tok, TOKEN_COLON)) {
		return make_result(ERR_PARSE_EXPR_EXPECT_COLON_IN_TERNARY_OP);
	}
	token_consume(tok);
	return RESULT_OK;
}

static const unsigned PRECEDENCE_INCREMENT = 10;

static WARN_UNUSED unsigned
get_precedence(const struct ast *a)
{
	unsigned precedence = 0;
	switch (a->node_type) {
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_AND:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_XOR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_OR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_LOGICAL_AND:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_LOGICAL_OR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		precedence += PRECEDENCE_INCREMENT;
		break;
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_BLOCK:
	case NODE_DECLARATION:
	case NODE_IF_ELSE:
	case NODE_LOOP:
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_SWITCH:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_CAST:
	case NODE_CONSTANT:
		assert(0); /* logic error in caller */
		break;
	}
	return precedence;
}

/*
 * Some references on precedence climbing:
 *
 * https://en.wikipedia.org/wiki/Operator-precedence_parser
 * https://eli.thegreenplace.net/2012/08/02/parsing-expressions-by-precedence-climbing
 * https://www.oilshell.org/blog/2016/11/01.html
 */
static WARN_UNUSED result_t
parse_expr(Arena *arena,
           const struct token **tok,
           struct ast **dst,
           unsigned minimum_precedence)
{
	struct ast *left = NULL;
	check(parse_factor(arena, tok, &left));

	while (true) {
		struct ast *bop = NULL;
		check(parse_expr_check_next_token(arena, *tok, &bop));
		if (bop == NULL) {
			break;
		}

		const bool is_right_associative =
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_ADD ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_SUB ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_MUL ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_DIV ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_REM ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_AND ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_OR ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_XOR ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_SL ||
			bop->node_type == NODE_EXPRESSION_COMPOUND_ASSIGN_SR ||
			bop->node_type == NODE_EXPRESSION_VARIABLE_ASSIGNMENT ||
			bop->node_type == NODE_EXPRESSION_TERNARY_CONDITIONAL;
		const unsigned inc = is_right_associative ? 0 : 1;

		const unsigned precedence = get_precedence(bop);
		if (precedence < minimum_precedence) {
			break;
		}

		token_consume(tok);

		struct ast *right = NULL;
		if (bop->node_type == NODE_EXPRESSION_TERNARY_CONDITIONAL) {
			struct ast *middle = NULL;
			check(parse_ternary_middle(arena, tok, &middle));
			check(parse_expr(arena, tok, &right, precedence + inc));
			bop->u.op_ternary.condition = left;
			bop->u.op_ternary.then_expr = middle;
			bop->u.op_ternary.else_expr = right;
		} else {
			check(parse_expr(arena, tok, &right, precedence + inc));
			bop->u.op_binary.lhs = left;
			bop->u.op_binary.rhs = right;
		}
		left = bop;
	}

	*dst = left;
	return RESULT_OK;
}

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
	} atom;
	union {
		struct declarator *in_parens;
		struct declarator *pointee;
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
	bool check_function_next = false;

	if (0 != (flags & PARSE_DECLARATOR_ABSTRACT) &&
	    is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom = DECLARATOR_ABSTRACT_BASE;
		assert(identifier == NULL);
		/* leave TOKEN_PAREN_CLOSE in place for caller to consume */
	} else if (0 == (flags & PARSE_DECLARATOR_ABSTRACT) &&
	           is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(declarator_alloc(arena, dst));
		assert(*dst != NULL);
		(**dst).atom = DECLARATOR_IDENTIFIER;
		assert(identifier != NULL);
		*identifier = (**tok).val;
		token_consume(tok);
		check_function_next = true;
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
		check_function_next = true;
	} else if (is_token_type(*tok, TOKEN_ASTERISK)) {
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
	} else {
		return make_result(ERR_PARSE_DECL_ATOM_EXPECT_REASONABLE);
	}

	if (check_function_next && is_token_type(*tok, TOKEN_PAREN_OPEN)) {
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

static WARN_UNUSED result_t
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

static result_t parse_block(Arena *arena,
                            const struct token **tok,
                            struct ast **dst_outer) WARN_UNUSED;

static const uint32_t PARSE_DECLARATION_ACCEPT_FUNCTION = 0x100;

static WARN_UNUSED result_t
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
		checked_alloc(arena, dst, NODE_DECLARATION);
		(**dst).u.declare.specifier = fn_or_var_specifier;
		check(ctype_copy(arena,
		                 &fn_return_or_var_type,
		                 &(**dst).u.declare.var_type));
		(**dst).u.declare.identifier.name = fn_or_var_name;

		if (is_token_type(*tok, TOKEN_EQUAL_SIGN)) {
			token_consume(tok);
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

	checked_alloc(arena, dst, NODE_FUNCTION);
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

static result_t parse_stmt(Arena *arena,
                           const struct token **tok,
                           struct ast **dst,
                           bool *call_again) WARN_UNUSED;

static WARN_UNUSED result_t
parse_block(Arena *arena, const struct token **tok, struct ast **dst_outer)
{
	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	checked_alloc(arena, dst_outer, NODE_BLOCK);
	struct flat **dst = &(**dst_outer).u.block.statements;

	if (is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		token_consume(tok);
		check(flat_alloc(arena, dst));
		checked_alloc(arena, &(**dst).car, NODE_EXPRESSION_NULL);
		return RESULT_OK;
	}

	while (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		check(flat_alloc(arena, dst));
		if (is_token_maybe_function_prefix(*tok)) {
			check(parse_fn_or_var_declaration(
				arena,
				PARSE_DECLARATION_ACCEPT_FUNCTION,
				tok,
				&(**dst).car));
		} else {
			bool dummy = false;
			check(parse_stmt(arena, tok, &(**dst).car, &dummy));
			/* can ignore dummy; we loop unconditionally here */
		}
		assert(*dst != NULL);
		dst = &(**dst).cdr;
	}

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_stmt_multi(Arena *arena, const struct token **tok, struct flat **dst)
{
	bool call_again = true;
	for (; call_again; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_stmt(arena, tok, &(**dst).car, &call_again));
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_if_else(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_IF));
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	checked_alloc(arena, dst, NODE_IF_ELSE);
	check(parse_expr(arena, tok, &(**dst).u.if_.condition, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	check(parse_stmt_multi(arena, tok, &(**dst).u.if_.then_clause));

	if (!is_token_type(*tok, TOKEN_KEYWORD_ELSE)) {
		return RESULT_OK;
	}
	token_consume(tok);

	check(parse_stmt_multi(arena, tok, &(**dst).u.if_.else_clause));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_loop_for_init(Arena *arena,
                    const struct token **tok,
                    struct ast **dst_outer)
{
	checked_alloc(arena, dst_outer, NODE_BLOCK);
	struct flat **dst = &(**dst_outer).u.block.statements;
	check(flat_alloc(arena, dst));
	assert(*dst != NULL);

	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		check(parse_alloc_if_unset(arena, &(**dst).car));
		token_consume(tok);
	} else if (is_token_variable_type(*tok)) {
		check(parse_fn_or_var_declaration(arena, 0, tok, &(**dst).car));
	} else {
		check(parse_expr(arena, tok, &(**dst).car, 0));
		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_loop_do_while_suffix(Arena *arena,
                           const struct token **tok,
                           struct ast **dst)
{
	if (!is_token_type(*tok, TOKEN_KEYWORD_WHILE)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_WHILE);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	check(parse_expr(arena, tok, &(**dst).u.loop.postcond, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

enum {
	UNSET_LOOP_ID = -1,
	UNSET_LABEL_ID = -2,
	UNSET_SWITCH_ID = -3,
};

static WARN_UNUSED result_t
parse_loop(Arena *arena, const struct token **tok, struct ast **dst)
{
	enum {
		PARSE_LOOP_DO,
		PARSE_LOOP_WHILE,
		PARSE_LOOP_FOR,
	} loop_type = 0;
	if (is_token_type(*tok, TOKEN_KEYWORD_DO)) {
		loop_type = PARSE_LOOP_DO;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_WHILE)) {
		loop_type = PARSE_LOOP_WHILE;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_FOR)) {
		loop_type = PARSE_LOOP_FOR;
	} else {
		assert(0); /* logic error in caller */
	}
	token_consume(tok);

	if (loop_type == PARSE_LOOP_FOR || loop_type == PARSE_LOOP_WHILE) {
		if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN);
		}
		token_consume(tok);
	}

	if (loop_type == PARSE_LOOP_FOR) {
		/*
		 * Create block in case for-init declares a loop variable.
		 */
		check(parse_loop_for_init(arena, tok, dst));
		assert((**dst).node_type == NODE_BLOCK);
		/*
		 * Arrange for loop body to be allocated into next block item,
		 * following first item that holds loop variable declaration.
		 */
		check(flat_alloc(arena, &(**dst).u.block.statements->cdr));
		dst = &(**dst).u.block.statements->cdr->car;
	}

	checked_alloc(arena, dst, NODE_LOOP);
	(**dst).u.loop.label_end = UNSET_LOOP_ID;
	(**dst).u.loop.label_continue = UNSET_LOOP_ID;
	(**dst).u.loop.label_start = UNSET_LOOP_ID;

	if ((loop_type == PARSE_LOOP_FOR &&
	     !is_token_type(*tok, TOKEN_SEMICOLON)) ||
	    loop_type == PARSE_LOOP_WHILE) {
		check(parse_expr(arena, tok, &(**dst).u.loop.precond, 0));
	}

	if (loop_type == PARSE_LOOP_FOR) {
		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);

		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			check(parse_expr(arena, tok, &(**dst).u.loop.incr, 0));
		}
	}

	if (loop_type == PARSE_LOOP_FOR || loop_type == PARSE_LOOP_WHILE) {
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
	}

	check(parse_stmt_multi(arena, tok, &(**dst).u.loop.body));

	if (loop_type == PARSE_LOOP_DO) {
		check(parse_loop_do_while_suffix(arena, tok, dst));
	}

	check(parse_alloc_if_unset(arena, &(**dst).u.loop.precond));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.incr));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.postcond));

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_switch(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_SWITCH));
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	checked_alloc(arena, dst, NODE_SWITCH);
	(**dst).u.switch_.label_default = UNSET_SWITCH_ID;
	(**dst).u.switch_.label_end = UNSET_SWITCH_ID;
	check(parse_expr(arena, tok, &(**dst).u.switch_.control, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	check(parse_stmt_multi(arena, tok, &(**dst).u.switch_.body));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_case(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_KEYWORD_CASE));
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_CONSTANT)) {
		return make_result(ERR_PARSE_CASE_EXPECT_CONSTANT);
	}

	checked_alloc(arena, dst, NODE_CASE);
	check(parse_constant(arena, tok, &(**dst).u.case_.constant));
	(**dst).u.case_.unique = UNSET_SWITCH_ID;

	if (!is_token_type(*tok, TOKEN_COLON)) {
		return make_result(ERR_PARSE_CASE_EXPECT_COLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_stmt(Arena *arena,
           const struct token **tok,
           struct ast **dst,
           bool *call_again)
{
	assert(dst != NULL && *dst == NULL);

	bool expect_semicolon_after = false;
	*call_again = false;

	if (is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_FUNCTION_RETURN_STATEMENT);
		check(parse_expr(arena, tok, &(**dst).u.op_unary.operand, 0));
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_EXPRESSION_NULL);
	} else if (is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		check(parse_block(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_IF)) {
		check(parse_if_else(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_DO) ||
	           is_token_type(*tok, TOKEN_KEYWORD_WHILE) ||
	           is_token_type(*tok, TOKEN_KEYWORD_FOR)) {
		check(parse_loop(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_BREAK)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_BREAK);
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_CONTINUE)) {
		token_consume(tok);
		checked_alloc(arena, dst, NODE_CONTINUE);
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_GOTO) &&
	           is_token_type((**tok).next, TOKEN_IDENTIFIER)) {
		checked_alloc(arena, dst, NODE_GOTO);
		(**dst).u.goto_.target_label = (**tok).next->val;
		(**dst).u.goto_.target_unique = UNSET_LABEL_ID;
		token_consume(tok);
		token_consume(tok);
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER) &&
	           is_token_type((**tok).next, TOKEN_COLON)) {
		checked_alloc(arena, dst, NODE_LABEL);
		(**dst).u.label.name = (**tok).val;
		(**dst).u.label.unique = UNSET_LABEL_ID;
		token_consume(tok);
		token_consume(tok);
		*call_again = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_SWITCH)) {
		check(parse_switch(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_CASE)) {
		check(parse_case(arena, tok, dst));
		*call_again = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_DEFAULT) &&
	           is_token_type((**tok).next, TOKEN_COLON)) {
		checked_alloc(arena, dst, NODE_CASE_DEFAULT);
		(**dst).u.case_.constant = NULL;
		(**dst).u.case_.unique = UNSET_SWITCH_ID;
		token_consume(tok);
		token_consume(tok);
		*call_again = true;
	} else {
		check(parse_expr(arena, tok, dst, 0));
		expect_semicolon_after = true;
	}

	if (expect_semicolon_after) {
		if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
			return make_result(
				ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON);
		}
		token_consume(tok);
	}

	return RESULT_OK;
}

result_t
parse_init(Arena *arena,
           const struct token *tok,
           struct ast **a,
           long long int *generator)
{
	checked_alloc(arena, a, NODE_PROGRAM);

	struct flat **dst = &(**a).u.program.globals;
	for (; tok != NULL; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		check(parse_fn_or_var_declaration(
			arena,
			PARSE_DECLARATION_ACCEPT_FUNCTION,
			&tok,
			&(**dst).car));
	}

	struct symbol *symbols = NULL;

	struct flat *cursor = (**a).u.program.globals;
	for (; generator != NULL && cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		switch (cursor->car->node_type) {
		case NODE_FUNCTION:
			check(resolve_function(arena, cursor->car, &symbols));
			break;
		case NODE_DECLARATION:
			check(resolve_declaration(arena,
			                          cursor->car,
			                          &symbols,
			                          SYMBOL_LINKAGE_EXTERNAL));
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
	}

	if (generator != NULL && symbols != NULL) {
		*generator = symbols->cookie;
	}
	return RESULT_OK;
}

static void
parse_debug_print_ast_symbol(const char *description,
                             const struct ast_symbol *asym,
                             size_t indent)
{
	if (description != NULL) {
		debug("%*s%s", (int)indent, "", description);
	}
	debug("%*sIDENTIFIER %.*s",
	      (int)indent + 1,
	      "",
	      (int)asym->name.sz,
	      asym->name.data);
	debug("%*sIDENTIFIER.UNIQUE: %lld%s",
	      (int)indent + 1,
	      "",
	      asym->unique,
	      asym->unique == NOT_YET_UNIQUE ? " (not unique)" : "");

	const char *symbol_type_as_str = NULL;
	switch (asym->stype) {
	case SYMBOL_VARIABLE:
		symbol_type_as_str = "VARIABLE";
		break;
	case SYMBOL_FUNCTION_DECLARATION:
		symbol_type_as_str = "FUNCTION DECLARATION";
		break;
	case SYMBOL_FUNCTION_DEFINITION:
		symbol_type_as_str = "FUNCTION DEFINITION";
		break;
	}
	debug("%*sIDENTIFIER.TYPE: %s",
	      (int)indent + 1,
	      "",
	      symbol_type_as_str);

	const char *linkage_as_str = NULL;
	switch (asym->ltype) {
	case SYMBOL_LINKAGE_NONE:
		linkage_as_str = "NONE";
		break;
	case SYMBOL_LINKAGE_INTERNAL:
		linkage_as_str = "INTERNAL";
		break;
	case SYMBOL_LINKAGE_EXTERNAL:
		linkage_as_str = "EXTERNAL";
		break;
	}

	debug("%*sIDENTIFIER.LINKAGE: %s", (int)indent + 1, "", linkage_as_str);
}

static void
parse_debug_print_ast_spec(enum ast_specifier specifier, size_t indent)
{
	const char *spec_as_str = NULL;
	switch (specifier) {
	case SPECIFIER_NONE:
		break;
	case SPECIFIER_STATIC:
		spec_as_str = "STATIC";
		break;
	case SPECIFIER_EXTERN:
		spec_as_str = "EXTERN";
		break;
	}
	if (spec_as_str != NULL) {
		debug("%*sSPECIFIER: %s", (int)indent, "", spec_as_str);
	}
}

void parse_debug_print_flat(const struct flat *a, size_t indent);

#define TO_STR(node_type, ...) #node_type,
static const char *const NODETYPE_NAMES[] = {FOREACH_AST_NODE(TO_STR)};
#undef TO_STR

void
parse_debug_print(const struct ast *a, size_t indent)
{
	char tmp[128] = {0};

	assert(indent <= INT_MAX);
	debug("%*s%s%s%s%s",
	      (int)indent,
	      "",
	      NODETYPE_NAMES[a->node_type],
	      a->node_type >= NODE_CONSTANT ? " [" : "",
	      a->node_type >= NODE_CONSTANT
	              ? ctype_to_str(&a->expr_type, tmp, sizeof(tmp))
	              : "",
	      a->node_type >= NODE_CONSTANT ? "]" : "");

	switch (a->node_type) {
	case NODE_PROGRAM:
		parse_debug_print_flat(a->u.program.globals, indent + 1);
		break;
	case NODE_FUNCTION:
		parse_debug_print_ast_symbol("NAME",
		                             &a->u.function.identifier,
		                             indent + 1);
		parse_debug_print_ast_spec(a->u.function.specifier, indent + 1);
		debug("%*sRETURNS: %s",
		      (int)(indent + 1),
		      "",
		      ctype_to_str(&a->u.function.return_type,
		                   tmp,
		                   sizeof(tmp)));
		FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
			parse_debug_print_ast_symbol("PARAMETER",
			                             &cur->symbol,
			                             indent + 1);
			debug("%*sPARAMETER.TYPE: %s",
			      (int)indent + 2,
			      "",
			      ctype_to_str(&cur->parameter_type,
			                   tmp,
			                   sizeof(tmp)));
		}
		debug("%*sBODY", (int)(indent + 1), "");
		if (a->u.function.block != NULL) {
			parse_debug_print(a->u.function.block, indent + 2);
		}
		break;
	case NODE_BLOCK:
		parse_debug_print_flat(a->u.block.statements, indent + 1);
		break;
	case NODE_DECLARATION:
		parse_debug_print_ast_symbol(NULL,
		                             &a->u.declare.identifier,
		                             indent);
		parse_debug_print_ast_spec(a->u.declare.specifier, indent + 1);
		debug("%*sVARIABLE.TYPE: %s",
		      (int)indent + 1,
		      "",
		      ctype_to_str(&a->u.declare.var_type, tmp, sizeof(tmp)));
		if (a->u.declare.init != NULL) {
			debug("%*sINITIALIZER", (int)(indent + 1), "");
			parse_debug_print(a->u.declare.init, indent + 2);
		}
		break;
	case NODE_IF_ELSE:
		debug("%*sCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.if_.condition, indent + 2);
		debug("%*sTHEN", (int)indent + 1, "");
		parse_debug_print_flat(a->u.if_.then_clause, indent + 2);
		if (a->u.if_.else_clause != NULL) {
			debug("%*sELSE", (int)indent + 1, "");
			parse_debug_print_flat(a->u.if_.else_clause,
			                       indent + 2);
		}
		break;
	case NODE_LOOP:
		debug("%*sLOOP ID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.loop.label_start,
		      a->u.loop.label_start == UNSET_LOOP_ID ? " (unset)" : "");
		debug("%*sPRECONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.loop.precond, indent + 2);
		debug("%*sBODY", (int)indent + 1, "");
		parse_debug_print_flat(a->u.loop.body, indent + 2);
		debug("%*sCONTINUE LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.loop.label_continue,
		      a->u.loop.label_continue == UNSET_LOOP_ID ? " (unset)"
		                                                : "");
		debug("%*sINCREMENTER", (int)indent + 1, "");
		parse_debug_print(a->u.loop.incr, indent + 2);
		debug("%*sPOSTCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.loop.postcond, indent + 2);
		debug("%*sEND LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.loop.label_end,
		      a->u.loop.label_end == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_BREAK:
		debug("%*sLOOP/SWITCH ID %lld%s",
		      (int)indent + 1,
		      "",
		      (long long)a->u.num,
		      a->u.num == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_CONTINUE:
		debug("%*sLOOP ID %lld%s",
		      (int)indent + 1,
		      "",
		      (long long)a->u.num,
		      a->u.num == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_GOTO:
		debug("%*sTARGET LABEL %.*s",
		      (int)indent + 1,
		      "",
		      (int)a->u.goto_.target_label.sz,
		      a->u.goto_.target_label.data);
		debug("%*sTARGET ID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.goto_.target_unique,
		      a->u.goto_.target_unique == UNSET_LABEL_ID ? " (unset)"
		                                                 : "");
		break;
	case NODE_LABEL:
		debug("%*sNAME %.*s",
		      (int)indent + 1,
		      "",
		      (int)a->u.label.name.sz,
		      a->u.label.name.data);
		debug("%*sID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.label.unique,
		      a->u.label.unique == UNSET_LABEL_ID ? " (unset)" : "");
		break;
	case NODE_SWITCH:
		debug("%*sCONTROL", (int)indent + 1, "");
		parse_debug_print(a->u.switch_.control, indent + 2);
		debug("%*sBODY", (int)indent + 1, "");
		parse_debug_print_flat(a->u.switch_.body, indent + 2);
		debug("%*sSWITCH DEFAULT LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.switch_.label_default,
		      a->u.switch_.label_default == UNSET_SWITCH_ID ? " (unset)"
		                                                    : "");
		debug("%*sSWITCH END LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.switch_.label_end,
		      a->u.switch_.label_end == UNSET_SWITCH_ID ? " (unset)"
		                                                : "");
		debug("%*sSEMANTIC CASE INFORMATION", (int)indent + 1, "");
		parse_debug_print_flat(a->u.switch_.label_cases, indent + 2);
		break;
	case NODE_CASE:
		parse_debug_print(a->u.case_.constant, indent + 1);
		__attribute__((fallthrough));
	case NODE_CASE_DEFAULT:
		debug("%*sCASE LABEL: %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.case_.unique,
		      a->u.case_.unique == UNSET_SWITCH_ID ? " (unset)" : "");
		break;
	case NODE_EXPRESSION_NULL:
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
		parse_debug_print(a->u.op_binary.lhs, indent + 1);
		parse_debug_print(a->u.op_binary.rhs, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		parse_debug_print_ast_symbol(NULL, &a->u.var, indent);
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		debug("%*sCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.op_ternary.condition, indent + 2);
		debug("%*sTHEN", (int)indent + 1, "");
		parse_debug_print(a->u.op_ternary.then_expr, indent + 2);
		if (a->u.op_ternary.else_expr != NULL) {
			debug("%*sELSE", (int)indent + 1, "");
			parse_debug_print(a->u.op_ternary.else_expr,
			                  indent + 2);
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		parse_debug_print_ast_symbol("FUNCTION",
		                             &a->u.call.identifier,
		                             indent + 1);
		debug("%*sARGUMENTS", (int)indent + 1, "");
		parse_debug_print_flat(a->u.call.args, indent + 2);
		break;
	case NODE_EXPRESSION_CAST:
		debug("%*sCAST.TO: %s",
		      (int)(indent + 1),
		      "",
		      ctype_to_str(&a->u.cast.to_type, tmp, sizeof(tmp)));
		parse_debug_print(a->u.cast.expr, indent + 1);
		break;
	case NODE_CONSTANT:
		if (ctype_is_floating_point(&a->expr_type)) {
			debug("%*sVALUE %f", (int)indent + 1, "", a->u.double_);
		} else {
			debug("%*sVALUE %lld",
			      (int)indent + 1,
			      "",
			      (long long)a->u.num);
		}
		break;
	}
}

void
parse_debug_print_flat(const struct flat *a, size_t indent)
{
	const struct flat *cursor = a;
	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		parse_debug_print(cursor->car, indent);
	}
}

result_t
cast_if(Arena *arena, const struct ctype *cast_to, struct ast **a)
{
	if (*a == NULL || ctype_is_equal(&(**a).expr_type, cast_to)) {
		return RESULT_OK;
	}
	struct ast *cast_wrap = NULL;
	checked_alloc(arena, &cast_wrap, NODE_EXPRESSION_CAST);
	check(ctype_copy(arena, cast_to, &cast_wrap->expr_type));
	check(ctype_copy(arena, cast_to, &cast_wrap->u.cast.to_type));
	cast_wrap->u.cast.expr = *a;
	*a = cast_wrap;
	return RESULT_OK;
}
