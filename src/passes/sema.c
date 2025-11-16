#include "passes.h"
#include "passes/parse.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

static WARN_UNUSED result_t
sema_label_impl(struct ast *a, long long int *id)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
		check(sema_label_impl(a->u.program.globals, id));
		break;
	case NODE_FUNCTION:
		if (a->u.function.block != NULL) {
			check(sema_label_impl(a->u.function.block, id));
		}
		if (a->u.function.next != NULL) {
			check(sema_label_impl(a->u.function.next, id));
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			check(sema_label_impl(a->u.block.item, id));
			if (a->u.block.next != NULL) {
				check(sema_label_impl(a->u.block.next, id));
			}
		}
		break;
	case NODE_IF_ELSE:
		check(sema_label_impl(a->u.if_.then_clause, id));
		if (a->u.if_.else_clause != NULL) {
			check(sema_label_impl(a->u.if_.else_clause, id));
		}
		break;
	case NODE_LOOP: {
		a->u.loop.label_start = ++*id;
		a->u.loop.label_continue = ++*id; /* see NODE_CONTINUE case */
		a->u.loop.label_end = ++*id;      /* see NODE_BREAK case */
		check(sema_label_impl(a->u.loop.precond, id));
		check(sema_label_impl(a->u.loop.body, id));
		check(sema_label_impl(a->u.loop.postcond, id));
		break;
	}
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
	case NODE_CONSTANT_INT:
		break;
	}
	return RESULT_OK;
}

result_t
sema_label_loops(struct ast *a, long long int *generator)
{
	debug("Labeling loops, loop breaks, and continues");
	*generator = 0;
	check(sema_label_impl(a, generator));
	return RESULT_OK;
}
