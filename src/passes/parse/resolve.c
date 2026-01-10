#include "passes/parse/resolve.h"

#include "passes/parse.h"

#include <assert.h>
#include <sys/param.h> /* for MAX() */

static WARN_UNUSED result_t
map_symbol_members(Arena *arena,
                   const struct symbol *src,
                   struct ast_symbol *dst_symbol,
                   struct ctype *dst_expr_type)
{
	dst_symbol->unique = src->unique;
	dst_symbol->stype = src->stype;
	dst_symbol->ltype = src->linkage.linkage;
	check(ctype_copy(arena, &src->c89type, dst_expr_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_symbol(Arena *arena,
               struct symbol *head,
               struct ast_symbol *asym,
               struct ctype *expr_type,
               unsigned errtype)
{
	static_assert(NOT_YET_UNIQUE < 0, "sentinel must be a negative number");
	assert(asym->unique == NOT_YET_UNIQUE);

	const struct symbol *resolved = symbols_get_anywhere(head, &asym->name);
	if (resolved == NULL) {
		return make_result(errtype, asym->name.data, asym->name.sz);
	}

	check(map_symbol_members(arena, resolved, asym, expr_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_var_usage(Arena *arena,
                  struct symbol *head,
                  struct ast_symbol *var,
                  struct ctype *expr_type)
{
	check(resolve_symbol(arena,
	                     head,
	                     var,
	                     expr_type,
	                     ERR_SEMA_VARIABLE_USAGE_WITHOUT_DECLARATION));
	return RESULT_OK;
}

static WARN_UNUSED result_t
resolve_function_call(Arena *arena,
                      struct symbol *head,
                      struct ast_symbol *callee,
                      struct ctype *return_type)
{
	check(resolve_symbol(arena,
	                     head,
	                     callee,
	                     return_type,
	                     ERR_SEMA_FUNCTION_CALL_UNDECLARED));
	return RESULT_OK;
}

static result_t
resolve_block(Arena *arena, struct flat *a, struct symbol **sym) WARN_UNUSED;

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
	case NODE_STRUCT:
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
	case NODE_CONSTANT:
	case NODE_CONSTANT_STR:
		break; /* no resolution work to do */
	case NODE_EXPRESSION_INITIALIZER:
		check(resolve_expr(arena, a->u.init.single, sym));
		for (struct flat *f = a->u.init.multi; f != NULL; f = f->cdr) {
			check(resolve_expr(arena, f->car, sym));
		}
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_UNARY_SIZE_OF:
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
	case NODE_EXPRESSION_SUBSCRIPT:
		check(resolve_expr(arena, a->u.op_binary.lhs, sym));
		check(resolve_expr(arena, a->u.op_binary.rhs, sym));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		check(resolve_var_usage(arena, *sym, &a->u.var, &a->expr_type));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(resolve_expr(arena, a->u.op_ternary.condition, sym));
		check(resolve_expr(arena, a->u.op_ternary.then_expr, sym));
		check(resolve_expr(arena, a->u.op_ternary.else_expr, sym));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(resolve_function_call(arena,
		                            *sym,
		                            &a->u.call.identifier,
		                            &a->expr_type));
		for (struct flat *f = a->u.call.args; f != NULL; f = f->cdr) {
			check(resolve_expr(arena, f->car, sym));
		}
		break;
	case NODE_EXPRESSION_CAST:
		check(resolve_expr(arena, a->u.cast.expr, sym));
		break;
	}

	return RESULT_OK;
}

result_t
resolve_declaration(Arena *arena,
                    struct ast *a,
                    struct symbol **symbols,
                    enum symbol_linkage assume_linkage)
{
	assert(a->node_type == NODE_DECLARATION);

	const struct string_view *varname = &a->u.declare.identifier.name;
	const enum symbol_linkage linkage =
		MAX(assume_linkage,
	            a->u.declare.specifier == SPECIFIER_EXTERN
	                    ? SYMBOL_LINKAGE_EXTERNAL
	                    : SYMBOL_LINKAGE_NONE);

	const struct symbol *in_scope = symbols_get_limited(*symbols, varname);
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
			symbols_get_anywhere(*symbols, varname);

		check(symbols_prepend(arena,
		                      symbols,
		                      &a->u.declare.identifier.name,
		                      SYMBOL_VARIABLE,
		                      &a->u.declare.var_type));
		(**symbols).linkage.linkage = linkage;
		resolved = *symbols;

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
			(**symbols).unique = anywhere->unique;
			(**symbols).linkage.linkage = anywhere->linkage.linkage;
		}
	}

	check(map_symbol_members(arena,
	                         resolved,
	                         &a->u.declare.identifier,
	                         &a->expr_type));
	/* sema.c detects if a->u.declare.var_type and expr_type conflict */

	if (a->u.declare.init != NULL) {
		check(resolve_expr(arena, a->u.declare.init, symbols));
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
			check(resolve_declaration(arena,
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
	struct ctype adjust_type = {0};
	check(ctype_copy(arena, &a->parameter_type, &adjust_type));
	ctype_array_decay_to_pointer(&adjust_type);

	check(symbols_prepend(arena,
	                      sym,
	                      &a->symbol.name,
	                      SYMBOL_VARIABLE,
	                      &adjust_type));

	struct ctype dummy = {0};
	check(map_symbol_members(arena, *sym, &a->symbol, &dummy));
	assert(ctype_is_equal(&dummy, &adjust_type));
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

result_t
resolve_function(Arena *arena, struct ast *a, struct symbol **symbols)
{
	assert(a->node_type == NODE_FUNCTION);

	const bool is_def = (a->u.function.block != NULL);
	check(symbols_prepend(arena,
	                      symbols,
	                      &a->u.function.identifier.name,
	                      is_def ? SYMBOL_FUNCTION_DEFINITION
	                             : SYMBOL_FUNCTION_DECLARATION,
	                      &a->u.function.return_type));
	struct ctype dummy = {0};
	check(map_symbol_members(arena,
	                         *symbols,
	                         &a->u.function.identifier,
	                         &dummy));
	assert(ctype_is_equal(&dummy, &a->u.function.return_type));

	struct symbol *before_params = *symbols;
	const bool cleanup = level_delimiter_prepare(before_params);

	check(resolve_function_params(arena, a->u.function.params, symbols));

	if (is_def) {
		assert(a->u.function.block->node_type == NODE_BLOCK);
		struct flat *function_body =
			a->u.function.block->u.block.statements;
		check(resolve_block_with_delimiter(arena,
		                                   function_body,
		                                   symbols,
		                                   before_params));
	}

	symbols_reset_scope(symbols, before_params);
	if (cleanup) {
		before_params->level_delimiter = false;
	}
	return RESULT_OK;
}
