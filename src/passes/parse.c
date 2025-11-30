#include "passes/parse.h"

#include "passes.h"
#include "passes/lex.h"
#include "passes/symbol.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h> /* for strtoll() */
#include <string.h>
#include <sys/param.h> /* for MAX() */

static const char LITERAL_DEFAULT[] = "default";

static void
map_symbol_members(const struct symbol *src, struct ast_symbol *dst)
{
	dst->unique = src->unique;
	dst->stype = src->stype;
	dst->ltype = src->linkage.linkage;
}

static WARN_UNUSED result_t
resolve_symbol(struct symbol *head, struct ast_symbol *asym, unsigned errtype)
{
	static_assert(NOT_YET_UNIQUE < 0, "sentinel must be a negative number");
	assert(asym->unique == NOT_YET_UNIQUE);

	const struct symbol *resolved = symbols_get_anywhere(head, &asym->name);
	if (resolved == NULL) {
		return make_result(errtype, asym->name.data, asym->name.sz);
	}

	map_symbol_members(resolved, asym);
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_var_usage(struct symbol *head, struct ast_symbol *var)
{
	check(resolve_symbol(head,
	                     var,
	                     ERR_SEMA_VARIABLE_USAGE_WITHOUT_DECLARATION));
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_call(struct symbol *head, struct ast_symbol *callee)
{
	check(resolve_symbol(head, callee, ERR_SEMA_FUNCTION_CALL_UNDECLARED));
	return RESULT_OK;
}

static result_t
resolve_block(Arena *arena, struct flat *a, struct symbol **sym) WARN_UNUSED;
static result_t
resolve_function(Arena *arena, struct ast *a, struct symbol **sym) WARN_UNUSED;

static WARN_UNUSED result_t
resolve_expr(Arena *arena, struct ast *a, struct symbol **sym)
{
	if (a == NULL) {
		return RESULT_OK;
	}

	switch (a->node_type) {
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_BLOCK:
	case NODE_DECLARATION:
		assert(0); /* logic error in caller */
		break;
	case NODE_IF_ELSE:
		check(resolve_expr(arena, a->u.if_.condition, sym));
		check(resolve_block(arena, a->u.if_.then_clause, sym));
		check(resolve_block(arena, a->u.if_.else_clause, sym));
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
		check(resolve_block(arena, a->u.loop.body, sym));
		break;
	case NODE_SWITCH:
		check(resolve_expr(arena, a->u.switch_.control, sym));
		check(resolve_block(arena, a->u.switch_.body, sym));
		break;
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
	case NODE_EXPRESSION_NULL:
	case NODE_CONSTANT_INT:
		break; /* no resolution work to do */
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		check(resolve_expr(arena, a->u.op_unary.operand, sym));
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
		check(resolve_expr(arena, a->u.op_binary.lhs, sym));
		check(resolve_expr(arena, a->u.op_binary.rhs, sym));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		check(resolve_var_usage(*sym, &a->u.var));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(resolve_expr(arena, a->u.op_ternary.condition, sym));
		check(resolve_expr(arena, a->u.op_ternary.then_expr, sym));
		check(resolve_expr(arena, a->u.op_ternary.else_expr, sym));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(resolve_function_call(*sym, &a->u.call.identifier));
		check(resolve_expr(arena, a->u.call.arguments, sym));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
		check(resolve_expr(arena, a->u.call_args.expr, sym));
		check(resolve_expr(arena, a->u.call_args.next, sym));
		break;
	case NODE_EXPRESSION_CAST:
		check(resolve_expr(arena, a->u.cast.expr, sym));
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_decl(Arena *arena,
             struct ast *a,
             struct symbol **sym,
             enum symbol_linkage assume_linkage)
{
	assert(a->node_type == NODE_DECLARATION);

	const struct string_view *varname = &a->u.declare.identifier.name;
	const enum symbol_linkage linkage =
		MAX(assume_linkage,
	            a->u.declare.specifier == SPECIFIER_EXTERN
	                    ? SYMBOL_LINKAGE_EXTERNAL
	                    : SYMBOL_LINKAGE_NONE);

	const struct symbol *in_scope = symbols_get_limited(*sym, varname);
	if (in_scope != NULL) {
		if (is_external(in_scope->linkage.linkage) &&
		    is_external(linkage)) {
			/*
			 * Declaring the same variable multiple times in the
			 * same scope is okay if both declarations are extern.
			 */
		} else {
			/*
			 * Otherwise, the declarations conflict.
			 */
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_DUPLICATE,
				in_scope->name.data,
				in_scope->name.sz);
		}
	}

	const struct symbol *resolved = NULL;
	if (in_scope != NULL) {
		resolved = in_scope;
	} else {
		const struct symbol *anywhere =
			symbols_get_anywhere(*sym, varname);

		check(symbols_prepend(arena,
		                      sym,
		                      &a->u.declare.identifier.name,
		                      SYMBOL_VARIABLE));
		(**sym).linkage.linkage = linkage;
		resolved = *sym;

		if (is_external(linkage) && /* This declaration is extern and */
		    anywhere != NULL &&     /* resolves to an existing var... */
		    some_linkage(anywhere->linkage.linkage)) { /* w/ linkage! */
			/*
			 * Make this re-declaration take on the unique ID and
			 * linkage characteristics of the existing variable
			 * pulled into scope, essentially creating a duplicate
			 * stub in the symbol table.
			 *
			 * We expect the caller to discard this stub when
			 * exiting this scope and proceeding to other scopes.
			 */
			(**sym).unique = anywhere->unique;
			(**sym).linkage.linkage = anywhere->linkage.linkage;
		}
	}
	map_symbol_members(resolved, &a->u.declare.identifier);

	if (a->u.declare.init != NULL) {
		check(resolve_expr(arena, a->u.declare.init, sym));
	}
	return RESULT_OK;
}

static WARN_UNUSED bool
level_delimiter_prepare(struct symbol *point)
{
	if (point == NULL) {
		return false;
	}

	if (point->level_delimiter) {
		/*
		 * This node already acts as a level_delimiter for an outer
		 * scope; do not clobber it!
		 */
		return false;
	}

	point->level_delimiter = true;
	return true;
}

static WARN_UNUSED result_t
resolve_block_with_delimiter(Arena *arena,
                             struct flat *a,
                             struct symbol **sym,
                             struct symbol *level_delimiter_point)
{
	assert(*sym != NULL);

	const bool cleanup = level_delimiter_prepare(level_delimiter_point);
	struct symbol *outer_resetter = *sym;

	for (; a != NULL; a = a->cdr) {
		struct ast *cur_item = a->car;
		assert(cur_item != NULL);

		struct symbol *resetter = NULL;
		switch (cur_item->node_type) {
		case NODE_FUNCTION:
			check(resolve_function(arena, cur_item, sym));
			break;
		case NODE_DECLARATION:
			check(resolve_decl(arena,
			                   cur_item,
			                   sym,
			                   SYMBOL_LINKAGE_NONE));
			break;
		case NODE_BLOCK:
			resetter = *sym;
			check(resolve_block(arena,
			                    cur_item->u.block.statements,
			                    sym));
			symbols_reset_scope(sym, resetter);
			break;
		default:
			check(resolve_expr(arena, cur_item, sym));
			break;
		}
	}

	symbols_reset_scope(sym, outer_resetter);
	if (cleanup) {
		level_delimiter_point->level_delimiter = false;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_block(Arena *arena, struct flat *a, struct symbol **sym)
{
	check(resolve_block_with_delimiter(arena, a, sym, *sym));
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_params_one(Arena *arena,
                            struct ast_parameter *a,
                            struct symbol **sym)
{
	check(symbols_prepend(arena, sym, &a->symbol.name, SYMBOL_VARIABLE));
	map_symbol_members(*sym, &a->symbol);
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_params(Arena *arena,
                        struct ast_parameter *a,
                        struct symbol **sym)
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
	check(symbols_prepend(arena,
	                      sym,
	                      &a->u.function.identifier.name,
	                      is_def ? SYMBOL_FUNCTION_DEFINITION
	                             : SYMBOL_FUNCTION_DECLARATION));
	map_symbol_members(*sym, &a->u.function.identifier);

	struct symbol *before_params = *sym;
	const bool cleanup = level_delimiter_prepare(before_params);

	check(resolve_function_params(arena, a->u.function.params, sym));

	if (is_def) {
		assert(a->u.function.block->node_type == NODE_BLOCK);
		struct flat *function_body =
			a->u.function.block->u.block.statements;
		check(resolve_block_with_delimiter(arena,
		                                   function_body,
		                                   sym,
		                                   before_params));
	}

	symbols_reset_scope(sym, before_params);
	if (cleanup) {
		before_params->level_delimiter = false;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_alloc(Arena *arena, struct ast **dst, unsigned ntype)
{
	static struct ast dummy_workaround_clang_analyzer_null_pointer = {0};

	assert(dst != NULL && *dst == NULL);
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
is_token_variable_type(const struct token *tok)
{
	if (tok == NULL) {
		return false;
	}
	switch (tok->token_type) {
	case TOKEN_KEYWORD_INT:
	case TOKEN_KEYWORD_LONG:
		return true;
	default:
		break;
	}
	return false;
}

static WARN_UNUSED enum ast_variable_type
map_token_type_to_variable_type(const struct token *tok)
{
	assert(is_token_variable_type(tok));

	enum ast_variable_type result = VARIABLE_TYPE_INT;
	switch (tok->token_type) {
	case TOKEN_KEYWORD_INT:
		result = VARIABLE_TYPE_INT;
		break;
	case TOKEN_KEYWORD_LONG:
		result = VARIABLE_TYPE_LONG;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	return result;
}

static WARN_UNUSED bool
is_token_maybe_function_prefix(const struct token *tok)
{
	return is_token_variable_type(tok) ||
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
		check(parse_alloc(arena, dst, prefix_ops[got_match].node_type));
		token_consume(tok);
		check(parse_factor(arena, tok, &(**dst).u.op_unary.operand));
	} else if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_constant(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_symbol(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_PAREN_OPEN) &&
	           *tok != NULL && /* avoid NULL dereference on (**tok).next */
	           is_token_variable_type((**tok).next)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_CAST));
		token_consume(tok);
		(**dst).u.cast.to_type = map_token_type_to_variable_type(*tok);
		token_consume(tok);
		if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
			return make_result(
				ERR_PARSE_CAST_EXPECT_TOKEN_PAREN_CLOSE);
		}
		token_consume(tok);
		check(parse_expr(arena, tok, &(**dst).u.cast.expr, 0));
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

	assert(*dst != NULL);

	struct ast *post = NULL;
	if (is_token_type(*tok, TOKEN_PLUS_SIGN_PLUS_SIGN)) {
		check(parse_alloc(arena, &post, NODE_EXPRESSION_POSTINCREMENT));
		token_consume(tok);
	} else if (is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)) {
		check(parse_alloc(arena, &post, NODE_EXPRESSION_POSTDECREMENT));
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
			check(parse_alloc(arena, a, infix_ops[i].node_type));
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
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
	case NODE_EXPRESSION_CAST:
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

static WARN_UNUSED bool
parse_peek_ahead_function_maybe(const struct token *tok)
{
	bool typed = false;
	for (; tok != NULL; tok = tok->next) {
		if (is_token_maybe_function_prefix(tok)) {
			/* seek past return type and specifiers */
			typed = typed || is_token_variable_type(tok);
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

static void
parse_type_signature_impl_accumulate(const struct token **tok,
                                     size_t *type_int_count,
                                     size_t *type_long_count)
{
	if (!is_token_variable_type(*tok)) {
		return;
	}

	assert(*tok != NULL);
	switch ((**tok).token_type) {
	case TOKEN_KEYWORD_INT:
		++(*type_int_count);
		break;
	case TOKEN_KEYWORD_LONG:
		++(*type_long_count);
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}
}

static WARN_UNUSED result_t
parse_type_signature_impl_finalize(bool expect_var, /* or expect_function */
                                   size_t type_int_count,
                                   size_t type_long_count,
                                   enum ast_variable_type *var_type)
{
	if (type_int_count > 1 || type_long_count > 2) {
		return make_result(
			expect_var ? ERR_PARSE_DECL_TYPE_DUPLICATE
				   : ERR_PARSE_FUNC_RETURN_TYPE_DUPLICATE);
	}

	if (type_int_count == 0 && type_long_count == 0) {
		return make_result(expect_var
		                           ? ERR_PARSE_DECL_EXPECT_TYPE
		                           : ERR_PARSE_FUNC_EXPECT_RETURN_TYPE);
	}

	switch (type_long_count) {
	case 2:
		assert(0 && "implement VARIABLE_TYPE_LONG_LONG");
		break;
	case 1:
		*var_type = VARIABLE_TYPE_LONG;
		break;
	case 0:
		assert(type_int_count == 1);
		*var_type = VARIABLE_TYPE_INT;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_type_signature(const struct token **tok, enum ast_variable_type *var_type)
{
	size_t type_int_count = 0;
	size_t type_long_count = 0;
	while (is_token_variable_type(*tok)) {
		parse_type_signature_impl_accumulate(tok,
		                                     &type_int_count,
		                                     &type_long_count);
		token_consume(tok);
	}
	check(parse_type_signature_impl_finalize(true,
	                                         type_int_count,
	                                         type_long_count,
	                                         var_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_specifiers(bool expect_var, /* or expect_function */
                 const struct token **tok,
                 enum ast_specifier *dst,
                 enum ast_variable_type *var_type)
{
	size_t type_int_count = 0;
	size_t type_long_count = 0;
	size_t specifier_count = 0;

	while (is_token_maybe_function_prefix(*tok)) {
		if (is_token_variable_type(*tok)) {
			parse_type_signature_impl_accumulate(tok,
			                                     &type_int_count,
			                                     &type_long_count);
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

	check(parse_type_signature_impl_finalize(expect_var,
	                                         type_int_count,
	                                         type_long_count,
	                                         var_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_decl(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_DECLARATION));
	check(parse_specifiers(true,
	                       tok,
	                       &(**dst).u.declare.specifier,
	                       &(**dst).u.declare.var_type));

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
                           struct ast **dst,
                           bool *call_again) WARN_UNUSED;

static WARN_UNUSED result_t
parse_block(Arena *arena, const struct token **tok, struct ast **dst_outer)
{
	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	check(parse_alloc(arena, dst_outer, NODE_BLOCK));
	struct flat **dst = &(**dst_outer).u.block.statements;

	if (is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		token_consume(tok);
		check(flat_alloc(arena, dst));
		check(parse_alloc(arena, &(**dst).car, NODE_EXPRESSION_NULL));
		return RESULT_OK;
	}

	while (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		check(flat_alloc(arena, dst));
		if (parse_peek_ahead_function_maybe(*tok)) {
			check(parse_function(arena, tok, &(**dst).car));
		} else if (is_token_maybe_function_prefix(*tok)) {
			check(parse_decl(arena, tok, &(**dst).car));
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

	check(parse_alloc(arena, dst, NODE_IF_ELSE));
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
	check(parse_alloc(arena, dst_outer, NODE_BLOCK));
	struct flat **dst = &(**dst_outer).u.block.statements;
	check(flat_alloc(arena, dst));
	assert(*dst != NULL);

	if (is_token_type(*tok, TOKEN_SEMICOLON)) {
		check(parse_alloc_if_unset(arena, &(**dst).car));
		token_consume(tok);
	} else if (is_token_variable_type(*tok)) {
		check(parse_decl(arena, tok, &(**dst).car));
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

	check(parse_alloc(arena, dst, NODE_SWITCH));
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

	check(parse_alloc(arena, dst, NODE_CASE));
	(**dst).u.case_.constant = (**tok).val;
	(**dst).u.case_.unique = UNSET_SWITCH_ID;
	token_consume(tok);

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
	} else if (is_token_type(*tok, TOKEN_KEYWORD_GOTO) &&
	           is_token_type((**tok).next, TOKEN_IDENTIFIER)) {
		check(parse_alloc(arena, dst, NODE_GOTO));
		(**dst).u.goto_.target_label = (**tok).next->val;
		(**dst).u.goto_.target_unique = UNSET_LABEL_ID;
		token_consume(tok);
		token_consume(tok);
		expect_semicolon_after = true;
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER) &&
	           is_token_type((**tok).next, TOKEN_COLON)) {
		check(parse_alloc(arena, dst, NODE_LABEL));
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
		check(parse_alloc(arena, dst, NODE_CASE_DEFAULT));
		(**dst).u.case_.constant.data = LITERAL_DEFAULT;
		(**dst).u.case_.constant.sz = sizeof(LITERAL_DEFAULT) - 1;
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

static WARN_UNUSED result_t
parse_function_params_impl(const struct token **tok,
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

		enum ast_variable_type parameter_type = VARIABLE_TYPE_INT;
		check(parse_type_signature(tok, &parameter_type));

		if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
			return make_result(
				ERR_PARSE_FUNC_PARAM_EXPECT_TOKEN_IDENTIFIER);
		}
		if (dst != NULL) {
			assert(*count <= count_in);
			(*dst)[*count].symbol.name = (**tok).val;
			(*dst)[*count].symbol.unique = NOT_YET_UNIQUE;
			(*dst)[*count].ptype = parameter_type;
		}
		token_consume(tok);
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
	check(parse_specifiers(false,
	                       tok,
	                       &(**dst).u.function.specifier,
	                       &(**dst).u.function.return_type));

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
           long long int *generator)
{
	check(parse_alloc(arena, a, NODE_PROGRAM));

	struct flat **dst = &(**a).u.program.globals;
	for (; tok != NULL; dst = &(**dst).cdr) {
		check(flat_alloc(arena, dst));
		if (parse_peek_ahead_function_maybe(tok)) {
			check(parse_function(arena, &tok, &(**dst).car));
		} else {
			check(parse_decl(arena, &tok, &(**dst).car));
		}
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
			check(resolve_decl(arena,
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

static void
parse_debug_print_ast_vartype(const char *description,
                              enum ast_variable_type var_type,
                              size_t indent)
{
	if (description != NULL) {
		debug("%*s%s", (int)indent, "", description);
	}
	const char *type_as_str = NULL;
	switch (var_type) {
	case VARIABLE_TYPE_INT:
		type_as_str = "INT";
		break;
	case VARIABLE_TYPE_LONG:
		type_as_str = "LONG";
		break;
	}
	debug("%*sTYPE: %s", (int)indent, "", type_as_str);
}

void parse_debug_print_flat(const struct flat *a, size_t indent);

#define TO_STR(node_type, ...) #node_type,
static const char *const NODETYPE_NAMES[] = {FOREACH_AST_NODE(TO_STR)};
#undef TO_STR

void
parse_debug_print(const struct ast *a, size_t indent)
{
	assert(indent <= INT_MAX);
	debug("%*s%s", (int)indent, "", NODETYPE_NAMES[a->node_type]);

	switch (a->node_type) {
	case NODE_PROGRAM:
		parse_debug_print_flat(a->u.program.globals, indent + 1);
		break;
	case NODE_FUNCTION:
		parse_debug_print_ast_symbol("NAME",
		                             &a->u.function.identifier,
		                             indent + 1);
		parse_debug_print_ast_spec(a->u.function.specifier, indent + 1);
		parse_debug_print_ast_vartype("RETURNS",
		                              a->u.function.return_type,
		                              indent + 1);
		FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
			parse_debug_print_ast_symbol("PARAMETER",
			                             &cur->symbol,
			                             indent + 1);
			parse_debug_print_ast_vartype("",
			                              cur->ptype,
			                              indent + 1);
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
		parse_debug_print_ast_vartype("",
		                              a->u.function.return_type,
		                              indent + 1);
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
		for (const struct ast_case *cur = a->u.switch_.label_cases;
		     cur != NULL;
		     cur = cur->next) {
			debug("%*sCASE.VALUE %lld",
			      (int)indent + 2,
			      "",
			      cur->constant);
			debug("%*sCASE.UNIQUE %lld",
			      (int)indent + 2,
			      "",
			      cur->unique);
		}
		break;
	case NODE_CASE:
		debug("%*sCASE VALUE %.*s",
		      (int)indent + 1,
		      "",
		      (int)a->u.case_.constant.sz,
		      a->u.case_.constant.data);
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
	case NODE_EXPRESSION_CAST:
		parse_debug_print_ast_vartype("",
		                              a->u.cast.to_type,
		                              indent + 1);
		parse_debug_print(a->u.cast.expr, indent + 1);
		break;
	case NODE_CONSTANT_INT:
		debug("%*sVALUE %lld", (int)indent + 1, "", a->u.num);
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
