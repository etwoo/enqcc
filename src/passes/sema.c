#include "passes.h"
#include "passes/parse.h"
#include "passes/symbol.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

static WARN_UNUSED result_t
sema_walk(Arena *arena,
          struct ast *a,
          result_t (*f)(Arena *arena, struct ast *a, void *userdata),
          void *u)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
		check(sema_walk(arena, a->u.program.globals, f, u));
		break;
	case NODE_FUNCTION:
		if (a->u.function.block != NULL) {
			check(sema_walk(arena, a->u.function.block, f, u));
		}
		if (a->u.function.next != NULL) {
			check(sema_walk(arena, a->u.function.next, f, u));
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			check(sema_walk(arena, a->u.block.item, f, u));
			if (a->u.block.next != NULL) {
				check(sema_walk(arena, a->u.block.next, f, u));
			}
		}
		break;
	case NODE_IF_ELSE:
		check(sema_walk(arena, a->u.if_.then_clause, f, u));
		if (a->u.if_.else_clause != NULL) {
			check(sema_walk(arena, a->u.if_.else_clause, f, u));
		}
		break;
	case NODE_LOOP:
		check(f(arena, a, u));
		check(sema_walk(arena, a->u.loop.precond, f, u));
		check(sema_walk(arena, a->u.loop.body, f, u));
		check(sema_walk(arena, a->u.loop.postcond, f, u));
		break;
	case NODE_BREAK:
		check(f(arena, a, u));
		break;
	case NODE_CONTINUE:
		check(f(arena, a, u));
		break;
	case NODE_DECLARATION:
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
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
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
	case NODE_CONSTANT_INT:
		break;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_label_loops_impl(Arena *arena MAYBE_UNUSED, struct ast *a, void *userdata)
{
	long long int *id = userdata;
	switch (a->node_type) {
	case NODE_LOOP:
		a->u.loop.label_start = ++*id;
		a->u.loop.label_continue = ++*id; /* see NODE_CONTINUE case */
		a->u.loop.label_end = ++*id;      /* see NODE_BREAK case */
		break;
	case NODE_BREAK:
		if (*id <= 0) {
			return make_result(ERR_SEMA_BREAK_OUTSIDE);
		}
		a->u.num = *id; /* most recent label_end */
		break;
	case NODE_CONTINUE:
		if (*id <= 0) {
			return make_result(ERR_SEMA_CONTINUE_OUTSIDE);
		}
		a->u.num = *id - 1; /* most recent label_continue */
		break;
	default:
		break;
	}

	return RESULT_OK;
}

result_t
sema_label_loops(struct ast *a, long long int *generator)
{
	debug("Labeling loops, loop breaks, and continues");
	*generator = 0;
	check(sema_walk(NULL, a, sema_label_loops_impl, generator));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_register_fn(Arena *arena, struct ast *a, struct symbol **s)
{
	assert(a->node_type == NODE_FUNCTION);

	long long int n_args = 0;
	FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
		++n_args;
	}

	const struct string_view *fname = &a->u.function.identifier.name;

	struct symbol *dup = symbols_get(*s, fname, false);
	if (dup == NULL) {
		check(symbols_prepend(arena,
		                      s,
		                      fname,
		                      SYMBOL_FUNCTION_DECLARATION,
		                      LINKAGE_EXTERNAL,
		                      n_args));
	} else if (n_args != dup->n_args) {
		return make_result(ERR_SEMA_CONFLICTING_FUNCTION_DEFINITION,
		                   fname->data,
		                   fname->sz);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_typecheck_fn(Arena *arena, struct ast *a, struct symbol **s)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
		check(sema_typecheck_fn(arena, a->u.program.globals, s));
		break;
	case NODE_FUNCTION:
		check(sema_register_fn(arena, a, s));
		if (a->u.function.block != NULL) {
			check(sema_typecheck_fn(arena, a->u.function.block, s));
		}
		if (a->u.function.next != NULL) {
			check(sema_typecheck_fn(arena, a->u.function.next, s));
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			check(sema_typecheck_fn(arena, a->u.block.item, s));
			if (a->u.block.next != NULL) {
				check(sema_typecheck_fn(arena,
				                        a->u.block.next,
				                        s));
			}
		}
		break;
	case NODE_IF_ELSE:
		check(sema_typecheck_fn(arena, a->u.if_.then_clause, s));
		if (a->u.if_.else_clause != NULL) {
			check(sema_typecheck_fn(arena,
			                        a->u.if_.else_clause,
			                        s));
		}
		break;
	case NODE_LOOP: {
		check(sema_typecheck_fn(arena, a->u.loop.body, s));
		break;
	}
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_DECLARATION:
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
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
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
	case NODE_CONSTANT_INT:
		break;
	}
	return RESULT_OK;
}

result_t
sema_typecheck(Arena *arena, struct ast *a)
{
	struct symbol *all_function_signatures = NULL;
	check(sema_typecheck_fn(arena, a, &all_function_signatures));
	return RESULT_OK;
}
