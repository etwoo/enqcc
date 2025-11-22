#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "passes/symbol.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h> /* for strtoll() */
#include <string.h>

enum {
	NOT_YET_UNIQUE = -1,
};

static void
map_symbol_members(const struct symbol *src, struct ast_symbol *dst)
{
	dst->unique = src->unique;
	dst->stype = src->stype;
}

static WARN_UNUSED result_t
resolve_var_usage(struct symbol *head, struct ast_symbol *var)
{
	static_assert(NOT_YET_UNIQUE < 0, "sentinel must be a negative number");
	assert(var->unique == NOT_YET_UNIQUE);

	const struct symbol *resolved = symbols_get(head, &var->name, false);
	if (resolved == NULL) {
		return make_result(ERR_SEMA_VARIABLE_USAGE_WITHOUT_DECLARATION,
		                   var->name.data,
		                   var->name.sz);
	}

	map_symbol_members(resolved, var);
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_call(struct symbol *head, struct ast_symbol *callee)
{
	static_assert(NOT_YET_UNIQUE < 0, "sentinel must be a negative number");
	assert(callee->unique == NOT_YET_UNIQUE);

	const struct symbol *resolved = symbols_get(head, &callee->name, false);
	if (resolved == NULL) {
		return make_result(ERR_SEMA_FUNCTION_CALL_UNDECLARED,
		                   callee->name.data,
		                   callee->name.sz);
	}

	map_symbol_members(resolved, callee);
	return RESULT_OK;
}

static result_t
resolve_block(Arena *arena, struct ast *a, struct symbol **sym) WARN_UNUSED;
static result_t
resolve_function(Arena *arena, struct ast *a, struct symbol **sym) WARN_UNUSED;

static WARN_UNUSED result_t
resolve_expr(Arena *arena, struct ast *a, struct symbol **sym)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_BLOCK:
	case NODE_DECLARATION:
		assert(0); /* logic error in caller */
		break;
	case NODE_IF_ELSE:
		check(resolve_expr(arena, a->u.if_.condition, sym));
		if (a->u.if_.then_clause->node_type == NODE_BLOCK) {
			check(resolve_block(arena, a->u.if_.then_clause, sym));
		} else {
			check(resolve_expr(arena, a->u.if_.then_clause, sym));
		}
		if (a->u.if_.else_clause == NULL) {
			/* skip missing else clause */
		} else if (a->u.if_.else_clause->node_type == NODE_BLOCK) {
			check(resolve_block(arena, a->u.if_.else_clause, sym));
		} else {
			check(resolve_expr(arena, a->u.if_.else_clause, sym));
		}
		break;
	case NODE_LOOP:
		check(resolve_expr(arena, a->u.loop.precond, sym));
		check(resolve_expr(arena, a->u.loop.postcond, sym));
		check(resolve_expr(arena, a->u.loop.incr, sym));
		/*
		 * Recurse into u.loop.body only _after_ resolving variables in
		 * u.loop.postcond and u.loop.incr. This prevents variables
		 * declared in the loop body from polluting what variables are
		 * visible to the loop's controlling expressions. For example,
		 * variable resolution should emit an error on `a` below:
		 *
		 *    do {
		 *        int a = a + 1;
		 *    } while (a < 100);
		 *
		 * Variable resolution should also emit an error on `y` here:
		 *
		 *    for (int x = 0; x < 10; y = 10) {
		 *        int y = 100;
		 *    }
		 */
		if (a->u.loop.body->node_type == NODE_BLOCK) {
			check(resolve_block(arena, a->u.loop.body, sym));
		} else {
			check(resolve_expr(arena, a->u.loop.body, sym));
		}
		break;
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_EXPRESSION_NULL:
	case NODE_CONSTANT_INT:
		break; /* no resolution work to do */
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(resolve_expr(arena, a->u.op_unary.operand, sym));
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		check(resolve_expr(arena, a->u.op_binary.lhs, sym));
		check(resolve_expr(arena, a->u.op_binary.rhs, sym));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		check(resolve_var_usage(*sym, &a->u.var));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(resolve_expr(arena, a->u.op_ternary.condition, sym));
		check(resolve_expr(arena, a->u.op_ternary.then_expr, sym));
		if (a->u.op_ternary.else_expr != NULL) {
			check(resolve_expr(arena,
			                   a->u.op_ternary.else_expr,
			                   sym));
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(resolve_function_call(*sym, &a->u.call.identifier));
		if (a->u.call.arguments != NULL) {
			check(resolve_expr(arena, a->u.call.arguments, sym));
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
		check(resolve_expr(arena, a->u.call_args.expr, sym));
		if (a->u.call_args.next != NULL) {
			check(resolve_expr(arena, a->u.call_args.next, sym));
		}
		break;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_decl(Arena *arena, struct ast *a, struct symbol **sym)
{
	assert(a->node_type == NODE_DECLARATION);

	const struct symbol *dup =
		symbols_get(*sym, &a->u.declare.identifier.name, true);
	const bool is_global = (a->u.declare.specifier == SPECIFIER_EXTERN);

	info("%s() sym.level_delimiter=%s var=%.*s dup=%p dup->linkage.is_global=%s, is_global=%s",
	     __func__,
	     (*sym) && (**sym).level_delimiter ? "yes" : "no",
	     (int)a->u.declare.identifier.name.sz,
	     a->u.declare.identifier.name.data,
	     (void *)dup,
	     dup == NULL              ? ""
	     : dup->linkage.is_global ? "yes"
	                              : "no",
	     is_global ? "yes" : "no");

	if (dup != NULL && !(dup->linkage.is_global && is_global)) {
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_DUPLICATE,
		                   dup->name.data,
		                   dup->name.sz);
	}

	if (dup == NULL) {
		check(symbols_prepend(arena,
		                      sym,
		                      &a->u.declare.identifier.name,
		                      SYMBOL_VARIABLE,
		                      0));
		map_symbol_members(*sym, &a->u.declare.identifier);
		(**sym).linkage.is_global = is_global;
	}

	if (a->u.declare.init != NULL) {
		check(resolve_expr(arena, a->u.declare.init, sym));
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_block_with_delimiter(Arena *arena,
                             struct ast *a,
                             struct symbol **sym,
                             struct symbol *level_delimiter_point)
{
	const bool level_delimiter_already_managed = // TODO refactor
		level_delimiter_point && level_delimiter_point->level_delimiter;
	if (*sym != NULL && !level_delimiter_already_managed) {
		assert(level_delimiter_point != NULL);
		info("%s() sets level_delimiter at %.*s", __func__, (int)level_delimiter_point->name.sz, level_delimiter_point->name.data);
		level_delimiter_point->level_delimiter = true;
	}

	struct symbol *outer_resetter = *sym;

	for (; a != NULL; a = a->u.block.next) {
		assert(a->node_type == NODE_BLOCK);

		struct ast *cur_item = a->u.block.item;
		if (cur_item == NULL) {
			continue;
		}

		struct symbol *resetter = NULL;
		switch (cur_item->node_type) {
		case NODE_FUNCTION:
			check(resolve_function(arena, cur_item, sym));
			break;
		case NODE_DECLARATION:
			check(resolve_decl(arena, cur_item, sym));
			break;
		case NODE_BLOCK:
			resetter = *sym;
			check(resolve_block(arena, cur_item, sym));
			symbols_reset_scope(sym, resetter);
			break;
		default:
			check(resolve_expr(arena, cur_item, sym));
			break;
		}
	}

	symbols_reset_scope(sym, outer_resetter);

	if (*sym != NULL && !level_delimiter_already_managed) {
		assert(level_delimiter_point != NULL);
		info("%s() unsets level_delimiter at %.*s", __func__, (int)level_delimiter_point->name.sz, level_delimiter_point->name.data);
		level_delimiter_point->level_delimiter = false;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_block(Arena *arena, struct ast *a, struct symbol **sym)
{
	check(resolve_block_with_delimiter(arena, a, sym, *sym));
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_params_one(Arena *arena,
                            struct ast_symbol *a,
                            struct symbol **sym)
{
	check(symbols_prepend(arena, sym, &a->name, SYMBOL_VARIABLE, 0));
	map_symbol_members(*sym, a);
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_params(Arena *arena, struct ast_symbol *a, struct symbol **sym)
{
	FOREACH_FUNCTION_PARAMETER (cur, a) {
		check(resolve_function_params_one(arena, cur, sym));
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function(Arena *arena, struct ast *a, struct symbol **sym)
{
	assert(a->node_type == NODE_FUNCTION);

	const bool is_def = (a->u.function.block != NULL);
	struct symbol *dup =
		symbols_get(*sym, &a->u.function.identifier.name, false);
	if (dup == NULL ||                   /* new symbol in this scope */
	    dup->stype == SYMBOL_VARIABLE) { /* ... or func shadows var  */
		long long int n_args = 0;
		FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
			++n_args;
		}
		check(symbols_prepend(arena,
		                      sym,
		                      &a->u.function.identifier.name,
		                      is_def ? SYMBOL_FUNCTION_DEFINITION
		                             : SYMBOL_FUNCTION_DECLARATION,
		                      n_args));
		map_symbol_members(*sym, &a->u.function.identifier);
	} else if (is_def) {
		map_symbol_members(dup, &a->u.function.identifier);
		assert(dup->stype == SYMBOL_FUNCTION_DECLARATION);
		dup->stype = SYMBOL_FUNCTION_DEFINITION;
	} else {
		map_symbol_members(dup, &a->u.function.identifier);
	}

	struct symbol *before_params = *sym;
	const bool level_delimiter_already_managed = // TODO refactor
		before_params && before_params->level_delimiter;
	if (*sym != NULL) {
		// TODO: level_delimiter should prevent resolve_decl() from reaching bar entirely, such that is_global is never compared at all!
		assert(before_params != NULL);
		info("%s() sets level_delimiter at %.*s", __func__, (int)before_params->name.sz, before_params->name.data);
		before_params->level_delimiter = true;
	}

	check(resolve_function_params(arena, a->u.function.params, sym));

	if (is_def) {
		check(resolve_block_with_delimiter(arena,
		                                   a->u.function.block,
		                                   sym,
		                                   before_params));
	}

	symbols_reset_scope(sym, before_params);
	if (before_params && !level_delimiter_already_managed) {
		info("%s() unsets level_delimiter at %.*s", __func__, (int)before_params->name.sz, before_params->name.data);
		before_params->level_delimiter = false;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_alloc(Arena *arena, struct ast **dst, unsigned ntype)
{
	static struct ast dummy_workaround_clang_analyzer_null_pointer = {0};

	*dst = arena_alloc(arena, sizeof(**dst));
	if (*dst == NULL) {
		/*
		 * Workaround spurious clang-analyzer-core.NullDereference
		 * warnings at callsites of this helper function. The Clang
		 * Static Analyzer seems to forget that (*dst != NULL) when
		 * this function returns RESULT_OK, which should then protect
		 * callers from accessing a NULL pointer when they use
		 * check(parse_alloc(...)) properly.
		 */
		*dst = &dummy_workaround_clang_analyzer_null_pointer;
		return make_result(ERR_PARSE_ALLOC);
	}

	memset(*dst, 0, sizeof(**dst));
	(**dst).node_type = ntype;
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_alloc_if_unset(Arena *arena, struct ast **dst)
{
	if (*dst == NULL) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_NULL));
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
is_token_maybe_function_prefix(const struct token *tok)
{
	return is_token_type(tok, TOKEN_KEYWORD_INT) ||
	       is_token_type(tok, TOKEN_KEYWORD_STATIC) ||
	       is_token_type(tok, TOKEN_KEYWORD_EXTERN);
}

static WARN_UNUSED result_t
parse_constant(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_CONSTANT));
	check(parse_alloc(arena, dst, NODE_CONSTANT_INT));

	/*
	 * strtoll() does not update errno on success, so we must clear it
	 * explicitly if we want a predictable value.
	 */
	errno = 0;

	(**dst).u.num = strtoll((**tok).val.data, NULL, 0);
	if (errno != 0) {
		return make_result(ERR_PARSE_CONSTANT_STRTOLL,
		                   errno,
		                   (**tok).val.data,
		                   (**tok).val.sz);
	}

	token_consume(tok);
	return RESULT_OK;
}

static result_t parse_expr(Arena *arena,
                           const struct token **tok,
                           struct ast **dst,
                           unsigned minimum_precedence) WARN_UNUSED;

static WARN_UNUSED result_t
parse_symbol(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(is_token_type(*tok, TOKEN_IDENTIFIER));

	struct string_view str = (**tok).val;
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_VARIABLE_USAGE));
		(**dst).u.var.name = str;
		(**dst).u.var.unique = NOT_YET_UNIQUE;
		return RESULT_OK;
	}

	check(parse_alloc(arena, dst, NODE_EXPRESSION_FUNCTION_CALL));
	(**dst).u.call.identifier.name = str;
	(**dst).u.call.identifier.unique = NOT_YET_UNIQUE;

	assert(is_token_type(*tok, TOKEN_PAREN_OPEN));
	token_consume(tok);

	if (is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		token_consume(tok);
		return RESULT_OK; /* zero-arg function call */
	}

	dst = &(**dst).u.call.arguments;
	while (true) {
		check(parse_alloc(arena,
		                  dst,
		                  NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS));
		check(parse_expr(arena, tok, &(**dst).u.call_args.expr, 0));
		dst = &(**dst).u.call_args.next;

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

static WARN_UNUSED result_t
parse_factor(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(!is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)); // unimplemented
	if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_constant(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_symbol(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_TILDE)) {
		check(parse_alloc(arena,
		                  dst,
		                  NODE_EXPRESSION_UNARY_COMPLEMENT));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_HYPHEN)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_NEGATE));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_EXCLAMATION)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_UNARY_NOT));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_PAREN_ENCLOSED));
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
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_expr_check_next_token(Arena *arena,
                            const struct token *tok,
                            struct ast **a)
{
	result_t r = RESULT_OK;
	if (is_token_type(tok, TOKEN_PLUS_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_ADD);
	} else if (is_token_type(tok, TOKEN_HYPHEN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_SUBTRACT);
	} else if (is_token_type(tok, TOKEN_ASTERISK)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_MULTIPLY);
	} else if (is_token_type(tok, TOKEN_FORWARD_SLASH)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_DIVIDE);
	} else if (is_token_type(tok, TOKEN_PERCENT_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_BINARY_REMAINDER);
	} else if (is_token_type(tok, TOKEN_AMPERSAND_AMPERSAND)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_LOGICAL_AND);
	} else if (is_token_type(tok, TOKEN_VERT_BAR_VERT_BAR)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_LOGICAL_OR);
	} else if (is_token_type(tok, TOKEN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_VARIABLE_ASSIGNMENT);
	} else if (is_token_type(tok, TOKEN_EQUAL_SIGN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_EQUAL);
	} else if (is_token_type(tok, TOKEN_EXCLAMATION_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_NOT_EQUAL);
	} else if (is_token_type(tok, TOKEN_LESS_THAN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_LESS_THAN);
	} else if (is_token_type(tok, TOKEN_LESS_THAN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_LESS_THAN_EQ);
	} else if (is_token_type(tok, TOKEN_MORE_THAN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_MORE_THAN);
	} else if (is_token_type(tok, TOKEN_MORE_THAN_EQUAL_SIGN)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_COMPARE_MORE_THAN_EQ);
	} else if (is_token_type(tok, TOKEN_QUESTION)) {
		r = parse_alloc(arena, a, NODE_EXPRESSION_TERNARY_CONDITIONAL);
	}
	return r;
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
	case NODE_EXPRESSION_LOGICAL_AND:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_LOGICAL_OR:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		precedence += PRECEDENCE_INCREMENT;
		__attribute__((fallthrough));
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
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
	case NODE_CONSTANT_INT:
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

static WARN_UNUSED bool
parse_peek_ahead_function_maybe(const struct token *tok)
{
	bool typed = false;
	for (; tok != NULL; tok = tok->next) {
		if (is_token_maybe_function_prefix(tok)) {
			/* seek past return type and specifiers */
			typed = typed || is_token_type(tok, TOKEN_KEYWORD_INT);
		} else if (is_token_type(tok, TOKEN_IDENTIFIER)) {
			/* ... until we reach the first TOKEN_IDENTIFIER  */
			/* ... and then check if TOKEN_PAREN_OPEN follows */
			const struct token *next = tok->next;
			return typed && is_token_type(next, TOKEN_PAREN_OPEN);
		} else {
			/* don't try to peek past other token types */
			break;
		}
		// TODO: if we add typedefs, the heuristic above will need to
		// change to accept custom return types (not just int) *AND* to
		// seek past those custom return types while looking for the
		// variable/function name; the crux is that these custom return
		// types -- created as typedefs earlier in the program -- will
		// appear as TOKEN_IDENTIFIER from the lexer, i.e. the same
		// type of token as the function name
	}
	return false;
}

static WARN_UNUSED result_t
parse_specifiers(bool expect_var, /* or expect_function */
                 const struct token **tok,
                 enum ast_specifier *dst)
{
	size_t type_count = 0;
	size_t specifier_count = 0;

	while (is_token_maybe_function_prefix(*tok)) {
		if (is_token_type(*tok, TOKEN_KEYWORD_INT)) {
			++type_count;
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
		return make_result(
			expect_var ? ERR_PARSE_DECL_SPECIFIER_DUPLICATE
				   : ERR_PARSE_FUNC_SPECIFIER_DUPLICATE);
	}

	if (type_count > 1) {
		return make_result(
			expect_var ? ERR_PARSE_DECL_TYPE_DUPLICATE
				   : ERR_PARSE_FUNC_RETURN_TYPE_DUPLICATE);
	}

	if (type_count == 0) {
		return make_result(
			expect_var ? ERR_PARSE_DECL_EXPECT_TYPE_INT
				   : ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_decl(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_DECLARATION));

	enum ast_specifier specifier = SPECIFIER_NONE;
	check(parse_specifiers(true, tok, &specifier));
	(**dst).u.declare.specifier = specifier;

	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_DECL_EXPECT_TOKEN_IDENTIFIER);
	}
	(**dst).u.declare.identifier.name = (**tok).val;
	token_consume(tok);

	if (is_token_type(*tok, TOKEN_EQUAL_SIGN)) {
		token_consume(tok);
		check(parse_expr(arena, tok, &(**dst).u.declare.init, 0));
	}

	if (!is_token_type(*tok, TOKEN_SEMICOLON)) {
		return make_result(ERR_PARSE_DECL_EXPECT_TOKEN_SEMICOLON);
	}
	token_consume(tok);

	return RESULT_OK;
}

static result_t parse_function(Arena *arena,
                               const struct token **tok,
                               struct ast **dst) WARN_UNUSED;
static result_t parse_stmt(Arena *arena,
                           const struct token **tok,
                           struct ast **dst) WARN_UNUSED;

static WARN_UNUSED result_t
parse_block(Arena *arena, const struct token **tok, struct ast **dst)
{
	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	if (is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_BLOCK));
		dst = &(**dst).u.block.item;
		check(parse_alloc(arena, dst, NODE_EXPRESSION_NULL));
		return RESULT_OK;
	}

	while (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		check(parse_alloc(arena, dst, NODE_BLOCK));
		struct ast **item_dst = &(**dst).u.block.item;
		if (parse_peek_ahead_function_maybe(*tok)) {
			check(parse_function(arena, tok, item_dst));
		} else if (is_token_maybe_function_prefix(*tok)) {
			check(parse_decl(arena, tok, item_dst));
		} else {
			check(parse_stmt(arena, tok, item_dst));
		}
		dst = &(**dst).u.block.next;
	}

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

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

	check(parse_alloc(arena, dst, NODE_IF_ELSE));
	check(parse_expr(arena, tok, &(**dst).u.if_.condition, 0));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	check(parse_stmt(arena, tok, &(**dst).u.if_.then_clause));

	if (!is_token_type(*tok, TOKEN_KEYWORD_ELSE)) {
		return RESULT_OK;
	}
	token_consume(tok);

	check(parse_stmt(arena, tok, &(**dst).u.if_.else_clause));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_loop_for_init(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_BLOCK));

	struct ast **item_dst = &(**dst).u.block.item;
	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		check(parse_alloc_if_unset(arena, item_dst));
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		check(parse_decl(arena, tok, item_dst));
	} else {
		check(parse_expr(arena, tok, item_dst, 0));
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
		dst = &(**dst).u.block.next;
		check(parse_alloc(arena, dst, NODE_BLOCK));
		dst = &(**dst).u.block.item;
	}

	check(parse_alloc(arena, dst, NODE_LOOP));
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

	check(parse_stmt(arena, tok, &(**dst).u.loop.body));

	if (loop_type == PARSE_LOOP_DO) {
		check(parse_loop_do_while_suffix(arena, tok, dst));
	}

	check(parse_alloc_if_unset(arena, &(**dst).u.loop.precond));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.body));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.incr));
	check(parse_alloc_if_unset(arena, &(**dst).u.loop.postcond));

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_stmt(Arena *arena, const struct token **tok, struct ast **dst)
{
	bool expect_semicolon_after = false;

	if (is_token_type(*tok, TOKEN_KEYWORD_RETURN)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_FUNCTION_RETURN_STATEMENT));
		check(parse_expr(arena, tok, &(**dst).u.op_unary.operand, 0));
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_EXPRESSION_NULL));
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
		check(parse_alloc(arena, dst, NODE_BREAK));
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_KEYWORD_CONTINUE)) {
		token_consume(tok);
		check(parse_alloc(arena, dst, NODE_CONTINUE));
		(**dst).u.num = UNSET_LOOP_ID;
		expect_semicolon_after = true;
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

static WARN_UNUSED result_t
parse_function_params_impl(const struct token **tok,
                           struct ast_symbol **dst,
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

		if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
			return make_result(
				ERR_PARSE_FUNC_PARAM_EXPECT_TYPE_INT);
		}
		token_consume(tok);

		if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
			return make_result(
				ERR_PARSE_FUNC_PARAM_EXPECT_TOKEN_IDENTIFIER);
		}
		if (dst != NULL) {
			assert(*count <= count_in);
			(*dst)[*count].name = (**tok).val;
			(*dst)[*count].unique = NOT_YET_UNIQUE;
		}
		token_consume(tok);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function_params(Arena *arena,
                      const struct token **tok,
                      struct ast_symbol **dst)
{
	if (is_token_type(*tok, TOKEN_KEYWORD_VOID)) {
		token_consume(tok);
		return RESULT_OK;
	}

	long long int count = 0;
	{
		const struct token *copy = *tok;
		check(parse_function_params_impl(&copy, NULL, &count));
	}
	if (count > 0) {
		size_t bytes = sizeof(**dst) * (count + 1);
		*dst = arena_alloc(arena, bytes);
		check_if(*dst == NULL, ERR_PARSE_ALLOC);
		memset(*dst, 0, bytes);
		check(parse_function_params_impl(tok, dst, &count));
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_FUNCTION));

	enum ast_specifier specifier = SPECIFIER_NONE;
	check(parse_specifiers(false, tok, &specifier));
	(**dst).u.function.specifier = specifier;

	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER);
	}
	(**dst).u.function.identifier.name = (**tok).val;
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	check(parse_function_params(arena, tok, &(**dst).u.function.params));

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

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

result_t
parse_init(Arena *arena,
           const struct token *tok,
           struct ast **a,
           struct symbol **sym)
{
	check(parse_alloc(arena, a, NODE_PROGRAM));
	struct ast *original = *a;

	a = &original->u.program.globals;
	while (tok != NULL) {
		if (parse_peek_ahead_function_maybe(tok)) {
			check(parse_function(arena, &tok, a));
			a = &(**a).u.function.next;
		} else {
			check(parse_decl(arena, &tok, a));
			a = &(**a).u.declare.next;
		}
	}

	a = &original->u.program.globals;
	while (sym != NULL && *a != NULL) {
		switch ((**a).node_type) {
		case NODE_FUNCTION:
			check(resolve_function(arena, *a, sym));
			a = &(**a).u.function.next;
			break;
		case NODE_DECLARATION:
			check(symbols_prepend(arena,
			                      sym,
			                      &(**a).u.declare.identifier.name,
			                      SYMBOL_VARIABLE,
			                      0));
			map_symbol_members(*sym, &(**a).u.declare.identifier);
			(**sym).linkage.is_global = true;
			a = &(**a).u.declare.next;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
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

#define TO_STR(node_type) #node_type,
static const char *const NODETYPE_NAMES[] = {FOREACH_AST_NODETYPE(TO_STR)};
#undef TO_STR

void
parse_debug_print(const struct ast *a, size_t indent)
{
	assert(indent <= INT_MAX);
	debug("%*s%s", (int)indent, "", NODETYPE_NAMES[a->node_type]);

	switch (a->node_type) {
	case NODE_PROGRAM:
		parse_debug_print(a->u.program.globals, indent + 1);
		break;
	case NODE_FUNCTION:
		parse_debug_print_ast_symbol("NAME",
		                             &a->u.function.identifier,
		                             indent + 1);
		parse_debug_print_ast_spec(a->u.function.specifier, indent + 1);
		FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
			parse_debug_print_ast_symbol("PARAMETER",
			                             cur,
			                             indent + 1);
		}
		debug("%*sBODY", (int)(indent + 1), "");
		if (a->u.function.block != NULL) {
			parse_debug_print(a->u.function.block, indent + 2);
		}
		if (a->u.function.next != NULL) {
			parse_debug_print(a->u.function.next, indent);
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			parse_debug_print(a->u.block.item, indent + 1);
			if (a->u.block.next != NULL) {
				parse_debug_print(a->u.block.next, indent);
			}
		}
		break;
	case NODE_DECLARATION:
		parse_debug_print_ast_symbol(NULL,
		                             &a->u.declare.identifier,
		                             indent);
		parse_debug_print_ast_spec(a->u.declare.specifier, indent + 1);
		if (a->u.declare.init != NULL) {
			debug("%*sINITIALIZER", (int)(indent + 1), "");
			parse_debug_print(a->u.declare.init, indent + 2);
		}
		if (a->u.declare.next != NULL) {
			parse_debug_print(a->u.declare.next, indent);
		}
		break;
	case NODE_IF_ELSE:
		debug("%*sCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.if_.condition, indent + 2);
		debug("%*sTHEN", (int)indent + 1, "");
		parse_debug_print(a->u.if_.then_clause, indent + 2);
		if (a->u.if_.else_clause != NULL) {
			debug("%*sELSE", (int)indent + 1, "");
			parse_debug_print(a->u.if_.else_clause, indent + 2);
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
		parse_debug_print(a->u.loop.body, indent + 2);
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
		debug("%*sLOOP ID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.num,
		      a->u.num == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_CONTINUE:
		debug("%*sLOOP ID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.num,
		      a->u.num == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_EXPRESSION_NULL:
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		parse_debug_print(a->u.op_binary.lhs, indent + 1);
		parse_debug_print(a->u.op_binary.rhs, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		parse_debug_print_ast_symbol("EXPRESSION VARIABLE USAGE",
		                             &a->u.var,
		                             indent);
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
		if (a->u.call.arguments != NULL) {
			debug("%*sARGUMENTS", (int)indent, "");
			parse_debug_print(a->u.call.arguments, indent + 1);
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
		parse_debug_print(a->u.call_args.expr, indent + 1);
		if (a->u.call_args.next != NULL) {
			parse_debug_print(a->u.call_args.next, indent);
		}
		break;
	case NODE_CONSTANT_INT:
		debug("%*sVALUE %lld", (int)indent + 1, "", a->u.num);
		break;
	}
}
