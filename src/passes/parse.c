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

static WARN_UNUSED result_t
resolve_var_usage(const struct symbol *head, struct ast_symbol *var)
{
	info("%s(ast=%p) start", __func__, (void *)var);
	static_assert(NOT_YET_UNIQUE < 0, "sentinel must be a negative number");
	if (var->unique != NOT_YET_UNIQUE) {
		info("unexpectedly resolving var %.*s that already has "
		     "unique=%lld",
		     (int)var->name.sz,
		     var->name.data,
		     var->unique);
	}
	assert(var->unique == NOT_YET_UNIQUE);
	const struct symbol *resolution = symbols_get(head, &var->name, false);
	if (resolution == NULL) {
		return make_result(ERR_SEMA_UNDECLARED_VARIABLE_USAGE,
		                   var->name.data,
		                   var->name.sz);
	}
	var->unique = resolution->unique;
	info("%s(%.*s) -> %lld",
	     __func__,
	     (int)var->name.sz,
	     var->name.data,
	     var->unique);
	return RESULT_OK;
}

static result_t
resolve_block(Arena *arena, struct ast *a, struct symbol **sym) WARN_UNUSED;

static WARN_UNUSED result_t
resolve_expr(Arena *arena, struct ast *a, struct symbol **sym)
{
	info("%s(ast=%p) start", __func__, (void *)a);

	switch (a->node_type) {
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_DECLARATION:
	case NODE_BLOCK:
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
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		if (a->u.op_binary.lhs->node_type !=
		    NODE_EXPRESSION_VARIABLE_USAGE) {
			return make_result(ERR_SEMA_DECL_INVALID_LVALUE);
		}
		__attribute__((fallthrough));
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
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_decl(Arena *arena, struct ast *a, struct symbol **sym)
{
	info("%s(ast=%p) start", __func__, (void *)a);
	assert(a->node_type == NODE_DECLARATION);

	const struct symbol *dup =
		symbols_get(*sym, &a->u.declare.identifier.name, true);
	if (dup != NULL) {
		info("ERROR ON symbol %.*s",
		     (int)a->u.declare.identifier.name.sz,
		     a->u.declare.identifier.name.data);
		return make_result(ERR_SEMA_DUPLICATE_VARIABLE_DECLARATION,
		                   dup->name.data,
		                   dup->name.sz);
	}

	check(symbols_prepend(arena, sym, &a->u.declare.identifier.name));
	assert(a->u.declare.identifier.name.sz == (**sym).name.sz &&
	       0 == strncmp(a->u.declare.identifier.name.data,
	                    (**sym).name.data,
	                    (**sym).name.sz));
	a->u.declare.identifier.unique = (**sym).unique;
	info("added symbol %.*s", (int)(**sym).name.sz, (**sym).name.data);

	if (a->u.declare.init != NULL) {
		check(resolve_expr(arena, a->u.declare.init, sym));
	}
	info("%s(ast=%p) done", __func__, (void *)a);
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_block(Arena *arena, struct ast *a, struct symbol **sym)
{
	info("%s(ast=%p) start", __func__, (void *)a);

	if (*sym != NULL) {
		(**sym).level_delimiter = true;
	}

	for (; a != NULL; a = a->u.block.next) {
		assert(a->node_type == NODE_BLOCK);

		struct ast *cur_item = a->u.block.item;
		if (cur_item == NULL) {
			continue;
		}

		struct symbol *resetter = NULL;
		switch (cur_item->node_type) {
		case NODE_DECLARATION:
			check(resolve_decl(arena, cur_item, sym));
			break;
		case NODE_BLOCK:
			resetter = *sym;
			check(resolve_block(arena, cur_item, sym));
			if (resetter != NULL) {
				resetter->unique = (**sym).unique;
			}
			*sym = resetter;
			break;
		default:
			check(resolve_expr(arena, cur_item, sym));
			break;
		}
	}

	if (*sym != NULL) {
		(**sym).level_delimiter = false;
	}

	info("%s(ast=%p) done", __func__, (void *)a);
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function(Arena *arena, struct ast *a, struct symbol **sym)
{
	info("%s(ast=%p) start", __func__, (void *)a);
	assert(a->node_type == NODE_FUNCTION);
	check(resolve_block(arena, a->u.function.block, sym));
	info("%s(ast=%p) done", __func__, (void *)a);
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

static WARN_UNUSED bool
is_token_type(const struct token *tok, unsigned expected)
{
	return tok != NULL && tok->token_type == expected;
}

static void
token_consume(const struct token **tok)
{
	*tok = (**tok).next;
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
parse_factor(Arena *arena, const struct token **tok, struct ast **dst)
{
	assert(!is_token_type(*tok, TOKEN_HYPHEN_HYPHEN)); // unimplemented
	if (is_token_type(*tok, TOKEN_CONSTANT)) {
		check(parse_constant(arena, tok, dst));
	} else if (is_token_type(*tok, TOKEN_IDENTIFIER)) {
		check(parse_alloc(arena, dst, NODE_EXPRESSION_VARIABLE_USAGE));
		(**dst).u.var.name = (**tok).val;
		(**dst).u.var.unique = NOT_YET_UNIQUE;
		token_consume(tok);
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
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_VARIABLE_USAGE:
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

static WARN_UNUSED result_t
parse_decl(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_DECLARATION));

	if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		return make_result(ERR_PARSE_DECL_EXPECT_TYPE_INT);
	}
	token_consume(tok);

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

static result_t parse_block(Arena *arena,
                            const struct token **tok,
                            struct ast **dst,
                            struct symbol **sym) WARN_UNUSED;
static result_t parse_if_else(Arena *arena,
                              const struct token **tok,
                              struct ast **dst,
                              struct symbol **sym) WARN_UNUSED;

static WARN_UNUSED result_t
parse_stmt(Arena *arena,
           const struct token **tok,
           struct ast **dst,
           struct symbol **sym)
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
		info("%s() calls parse_block(*dst=%p)", __func__, (void *)*dst);
		check(parse_block(arena, tok, dst, sym));
	} else if (is_token_type(*tok, TOKEN_KEYWORD_IF)) {
		check(parse_if_else(arena, tok, dst, sym));
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
parse_block(Arena *arena,
            const struct token **tok,
            struct ast **dst,
            struct symbol **sym)
{
	info("%s() start dst=%p *dst=%p", __func__, (void *)dst, (void *)*dst);

	if (!is_token_type(*tok, TOKEN_BRACE_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN);
	}
	token_consume(tok);

	info("start loop with dst %p (*dst %p)", (void *)dst, (void *)*dst);
	while (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		check(parse_alloc(arena, dst, NODE_BLOCK));
		if (is_token_type(*tok, TOKEN_KEYWORD_INT)) {
			check(parse_decl(arena, tok, &(**dst).u.block.item));
		} else {
			check(parse_stmt(arena,
			                 tok,
			                 &(**dst).u.block.item,
			                 sym));
		}
		dst = &(**dst).u.block.next;
		info("prepare for next loop iter with dst %p (*dst %p)",
		     (void *)dst,
		     (void *)*dst);
	}
	info("end loop with dst %p (*dst %p)", (void *)dst, (void *)*dst);

	if (!is_token_type(*tok, TOKEN_BRACE_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE);
	}
	token_consume(tok);

	info("%s() done", __func__);
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_if_else(Arena *arena,
              const struct token **tok,
              struct ast **dst,
              struct symbol **sym)
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

	check(parse_stmt(arena, tok, &(**dst).u.if_.then_clause, sym));

	if (!is_token_type(*tok, TOKEN_KEYWORD_ELSE)) {
		return RESULT_OK;
	}
	token_consume(tok);

	check(parse_stmt(arena, tok, &(**dst).u.if_.else_clause, sym));
	return RESULT_OK;
}

static WARN_UNUSED result_t
parse_function(Arena *arena,
               const struct token **tok,
               struct ast **dst,
               struct symbol **sym)
{
	check(parse_alloc(arena, dst, NODE_FUNCTION));

	if (!is_token_type(*tok, TOKEN_KEYWORD_INT)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_IDENTIFIER)) {
		return make_result(ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER);
	}
	(**dst).u.function.identifier.name = (**tok).val;
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_OPEN)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_KEYWORD_VOID)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_KEYWORD_VOID);
	}
	token_consume(tok);

	if (!is_token_type(*tok, TOKEN_PAREN_CLOSE)) {
		return make_result(ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE);
	}
	token_consume(tok);

	info("%s() calls parse_block(*dst=%p)",
	     __func__,
	     (void *)(**dst).u.function.block);
	check(parse_block(arena, tok, &(**dst).u.function.block, sym));
	return RESULT_OK;
}

result_t
parse_init(Arena *arena,
           const struct token *tok,
           struct ast **a,
           struct symbol **sym)
{
	check(parse_alloc(arena, a, NODE_PROGRAM));
	check(parse_function(arena,
	                     &tok,
	                     &(**a).u.program.entrypoint_function,
	                     sym));
	if (tok != NULL) {
		return make_result(ERR_PARSE_PROG_EXPECT_END);
	}
	if (sym != NULL) {
		check(resolve_function(arena,
		                       (**a).u.program.entrypoint_function,
		                       sym));
	}

	return RESULT_OK;
}

static void
parse_debug_print_ast_symbol(const char *description,
                             const struct ast_symbol *asym,
                             size_t indent)
{
	debug("%*s%s", (int)indent, "", description);
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
}

void
parse_debug_print(const struct ast *a, size_t indent)
{
	assert(indent <= INT_MAX);
	switch (a->node_type) {
	case NODE_PROGRAM:
		debug("%*sPROGRAM", (int)indent, "");
		parse_debug_print(a->u.program.entrypoint_function, indent + 1);
		break;
	case NODE_FUNCTION:
		parse_debug_print_ast_symbol("FUNCTION",
		                             &a->u.function.identifier,
		                             indent);
		debug("%*sBODY", (int)(indent + 1), "");
		if (a->u.function.block != NULL) {
			parse_debug_print(a->u.function.block, indent + 2);
		}
		break;
	case NODE_BLOCK:
		debug("%*sBLOCK ITEM", (int)indent, "");
		if (a->u.block.item != NULL) {
			parse_debug_print(a->u.block.item, indent + 1);
			if (a->u.block.next != NULL) {
				parse_debug_print(a->u.block.next, indent);
			}
		}
		break;
	case NODE_DECLARATION:
		parse_debug_print_ast_symbol("DECLARATION",
		                             &a->u.declare.identifier,
		                             indent);
		if (a->u.declare.init != NULL) {
			debug("%*sINITIALIZER", (int)(indent + 1), "");
			parse_debug_print(a->u.declare.init, indent + 2);
		}
		break;
	case NODE_IF_ELSE:
		debug("%*sIF", (int)indent, "");
		debug("%*sCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.if_.condition, indent + 2);
		debug("%*sTHEN", (int)indent + 1, "");
		parse_debug_print(a->u.if_.then_clause, indent + 2);
		if (a->u.if_.else_clause != NULL) {
			debug("%*sELSE", (int)indent + 1, "");
			parse_debug_print(a->u.if_.else_clause, indent + 2);
		}
		break;
	case NODE_EXPRESSION_NULL:
		debug("%*sEXPRESSION NULL", (int)indent, "");
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		switch (a->node_type) {
		case NODE_FUNCTION_RETURN_STATEMENT:
			debug("%*sRETURN", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_NEGATE:
			debug("%*sEXPRESSION NEGATE", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_NOT:
			debug("%*sEXPRESSION NOT", (int)indent, "");
			break;
		case NODE_EXPRESSION_UNARY_COMPLEMENT:
			debug("%*sEXPRESSION COMPLEMENT", (int)indent, "");
			break;
		case NODE_EXPRESSION_PAREN_ENCLOSED:
			debug("%*sEXPRESSION PARENTHESIZED", (int)indent, "");
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		parse_debug_print(a->u.op_unary.operand, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
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
		switch (a->node_type) {
		case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
			debug("%*sEXPRESSION ASSIGN", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_ADD:
			debug("%*sEXPRESSION ADD", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_SUBTRACT:
			debug("%*sEXPRESSION SUBTRACT", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_MULTIPLY:
			debug("%*sEXPRESSION MULTIPLY", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_DIVIDE:
			debug("%*sEXPRESSION DIVIDE", (int)indent, "");
			break;
		case NODE_EXPRESSION_BINARY_REMAINDER:
			debug("%*sEXPRESSION REMAINDER", (int)indent, "");
			break;
		case NODE_EXPRESSION_LOGICAL_AND:
			debug("%*sEXPRESSION LOGICAL AND", (int)indent, "");
			break;
		case NODE_EXPRESSION_LOGICAL_OR:
			debug("%*sEXPRESSION LOGICAL OR", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_EQUAL:
			debug("%*sEXPRESSION COMPARE EQUAL", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
			debug("%*sEXPRESSION NOT EQUAL", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_LESS_THAN:
			debug("%*sEXPRESSION LESS THAN", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
			debug("%*sEXPRESSION LESS THAN OR EQ", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_MORE_THAN:
			debug("%*sEXPRESSION MORE THAN", (int)indent, "");
			break;
		case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
			debug("%*sEXPRESSION MORE THAN OR EQ", (int)indent, "");
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		parse_debug_print(a->u.op_binary.lhs, indent + 1);
		parse_debug_print(a->u.op_binary.rhs, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		parse_debug_print_ast_symbol("EXPRESSION VARIABLE USAGE",
		                             &a->u.var,
		                             indent);
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		debug("%*sEXPRESSION TERNARY CONDITIONAL", (int)indent, "");
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
	case NODE_CONSTANT_INT:
		debug("%*sCONSTANT %lld", (int)indent, "", a->u.num);
		break;
	}
}
