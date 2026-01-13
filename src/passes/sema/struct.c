#include "passes/sema/struct.h"

#include "passes/parse.h"
#include "passes/sema/walk.h"

struct sema_struct_state {
	Arena *arena;
	struct ctype expected_return_type;
};

static WARN_UNUSED result_t
sema_struct(struct ast *a, void *userdata)
{
	struct sema_struct_state *state = userdata;
	Arena *arena = state->arena;

	switch (a->node_type) {
	case NODE_FUNCTION:
		if (a->u.function.block != NULL) {
			check(ctype_copy(arena,
			                 &a->u.function.return_type,
			                 &state->expected_return_type));
		}
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		if (ctype_is_struct_mismatch(
			    &state->expected_return_type,
			    &a->u.op_unary.operand->expr_type)) {
			return make_result(
				ERR_SEMA_RETURN_STATEMENT_STRUCT_MISMATCH);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		if (ctype_is_struct_mismatch(&a->u.op_binary.lhs->expr_type,
		                             &a->u.op_binary.rhs->expr_type)) {
			return make_result(ERR_SEMA_ASSIGNMENT_STRUCT_MISMATCH);
		}
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		if (ctype_is_struct_mismatch(
			    &a->u.op_ternary.then_expr->expr_type,
			    &a->u.op_ternary.else_expr->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_TERNARY_MISMATCH);
		}
		break;
	default:
		break;
	}
	return RESULT_OK;
}

result_t
sema_typecheck_struct(Arena *arena, struct ast *a)
{
	struct sema_ops ops = {
		.node_enter = sema_struct,
	};
	struct sema_struct_state state = {
		.arena = arena,
	};
	check(sema_walk(a, &ops, &state));
	return RESULT_OK;
}
