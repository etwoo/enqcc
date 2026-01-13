#include "passes/sema/conversion.h"

#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/sema/pointer.h"
#include "passes/sema/walk.h"

#include <assert.h>

struct sema_conversion_state {
	Arena *arena;
	struct type_table *types;
	struct ctype expected_return_type;
};

static WARN_UNUSED result_t
visit_conversion(struct ast **init,
                 const struct ctype *expected_type,
                 void *userdata)
{
	struct sema_conversion_state *state = userdata;
	Arena *arena = state->arena;

	assert((**init).node_type == NODE_EXPRESSION_INITIALIZER);
	if ((**init).u.init.single != NULL) {
		check(sema_pointer_cmp(expected_type, &(**init).expr_type));
		check(cast_if(arena, expected_type, init));
	}

	return RESULT_OK;
}

/*
 * See sema_expr_types_initializer() for related logic.
 */
static WARN_UNUSED result_t
sema_conversion_initializer(struct ast *a, struct sema_conversion_state *state)
{
	assert(a->node_type == NODE_DECLARATION);
	if (a->u.declare.init == NULL) {
		return RESULT_OK;
	}
	check(sema_walk_initializer(&a->u.declare.init,
	                            &a->u.declare.var_type,
	                            state->types,
	                            visit_conversion,
	                            state));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_conversion(struct ast *a, void *userdata)
{
	struct sema_conversion_state *state = userdata;
	Arena *arena = state->arena;
	const struct ctype *common = NULL;

	switch (a->node_type) {
	case NODE_FUNCTION:
		check(ctype_copy(arena,
		                 &a->u.function.return_type,
		                 &state->expected_return_type));
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		if (ctype_is_struct_mismatch(
			    &state->expected_return_type,
			    &a->u.op_unary.operand->expr_type)) {
			return make_result(
				ERR_SEMA_RETURN_STATEMENT_STRUCT_MISMATCH);
		} else if (ctype_is_void(&state->expected_return_type) ==
		           ctype_is_void(&a->u.op_unary.operand->expr_type)) {
			/* void function XOR void return statement */
		} else if (ctype_is_void(&state->expected_return_type)) {
			return make_result(
				ERR_SEMA_RETURN_STATEMENT_EXPECT_VOID);
		} else {
			return make_result(
				ERR_SEMA_RETURN_STATEMENT_EXPECT_VALUE);
		}
		check(cast_if(arena,
		              &state->expected_return_type,
		              &a->u.op_unary.operand));
		break;
	case NODE_DECLARATION:
		check(sema_conversion_initializer(a, state));
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		common = get_common_ctype(&a->u.op_binary.lhs->expr_type,
		                          &a->u.op_binary.rhs->expr_type);
		if ((a->node_type == NODE_EXPRESSION_BINARY_ADD ||
		     a->node_type == NODE_EXPRESSION_BINARY_SUBTRACT) &&
		    ((ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		      ctype_is_integer(&a->u.op_binary.rhs->expr_type)) ||
		     (ctype_is_integer(&a->u.op_binary.lhs->expr_type) &&
		      ctype_is_pointer(&a->u.op_binary.rhs->expr_type)))) {
			check(cast_if(
				arena,
				&LIKE_PTRDIFF_T,
				ctype_is_integer(&a->u.op_binary.lhs->expr_type)
					? &a->u.op_binary.lhs
					: &a->u.op_binary.rhs));
		} else {
			check(cast_if(arena, common, &a->u.op_binary.lhs));
			check(cast_if(arena, common, &a->u.op_binary.rhs));
		}
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		if (ctype_is_struct_mismatch(&a->u.op_binary.lhs->expr_type,
		                             &a->u.op_binary.rhs->expr_type)) {
			return make_result(ERR_SEMA_ASSIGNMENT_STRUCT_MISMATCH);
		}
		__attribute__((fallthrough));
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		check(cast_if(arena,
		              &a->u.op_binary.lhs->expr_type,
		              &a->u.op_binary.rhs));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		if (ctype_is_struct_mismatch(
			    &a->u.op_ternary.then_expr->expr_type,
			    &a->u.op_ternary.else_expr->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_TERNARY_MISMATCH);
		}
		common =
			get_common_ctype(&a->u.op_ternary.then_expr->expr_type,
		                         &a->u.op_ternary.else_expr->expr_type);
		check(cast_if(arena, common, &a->u.op_ternary.then_expr));
		check(cast_if(arena, common, &a->u.op_ternary.else_expr));
		break;
	default:
		break;
	}
	return RESULT_OK;
}

result_t
sema_typecheck_conversion(Arena *arena, struct ast *a, struct type_table *types)
{
	struct sema_ops ops = {
		.node_enter = sema_conversion,
	};
	struct sema_conversion_state state = {
		.arena = arena,
		.types = types,
	};
	check(sema_walk(a, &ops, &state));
	return RESULT_OK;
}

result_t
cast_if(Arena *arena, const struct ctype *cast_to, struct ast **ast_handle)
{
	struct ast *a = *ast_handle;
	if (a == NULL || ctype_is_equal(&a->expr_type, cast_to)) {
		return RESULT_OK;
	}

	struct ast *cast_wrap = NULL;
	check(parse_alloc(arena, &cast_wrap, NODE_EXPRESSION_CAST));
	check(ctype_copy(arena, cast_to, &cast_wrap->expr_type));
	check(ctype_copy(arena, cast_to, &cast_wrap->u.cast.to_type));
	cast_wrap->u.cast.expr = a;

	*ast_handle = cast_wrap;
	return RESULT_OK;
}

struct ast **
cast_unpack(struct ast **a)
{
	while ((**a).node_type == NODE_EXPRESSION_CAST) {
		/* unpack nodes inserted by sema_conversion() */
		a = &(**a).u.cast.expr;
	}
	return a;
}
