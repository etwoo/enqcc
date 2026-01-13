#include "passes/sema/pointer.h"

#include "passes/parse.h"
#include "passes/sema/walk.h"

#include <assert.h>
#include <stdint.h>

static const uint32_t SPC_ZERO_AS_NULL = 0x1;
static const uint32_t SPC_VOIDP_AS_ANY_PTR = 0x2;

static WARN_UNUSED result_t
sema_pointer_cmp_impl(const struct ctype *lhs,
                      const struct ctype *rhs,
                      uint32_t flags)
{
	const bool ish = (0 != (flags & SPC_ZERO_AS_NULL));
	const bool void_ptr_as_any_ptr = (0 != (flags & SPC_VOIDP_AS_ANY_PTR));

	if (ctype_is_equal(lhs, rhs)) {
		/* given equality, nothing more to check */
	} else if (ctype_is_strlike_array(lhs) && ctype_is_strlike_array(rhs)) {
		if (lhs->sz < rhs->sz - 1) { /* -1 for (maybe) NUL terminator */
			return make_result(ERR_SEMA_OPERAND_CHAR_ARRAY_SIZE);
		} /* else: RHS string may be shorter than LHS capacity */
	} else if (ctype_is_pointer(lhs) && ctype_is_pointer(rhs)) {
		if (void_ptr_as_any_ptr &&
		    (ctype_is_void_ptr(lhs) || ctype_is_void_ptr(rhs))) {
			/* void* converts to/from any other pointer type */
		} else {
			return make_result(ERR_SEMA_OPERAND_POINTER_CONFLICT);
		}
	} else if (ctype_is_pointer(lhs) && (!ish || !ctype_nullptr_ish(rhs))) {
		return make_result(ERR_SEMA_OPERAND_POINTER_LHS_VS_NOT_RHS);
	} else if (ctype_is_pointer(rhs) && (!ish || !ctype_nullptr_ish(lhs))) {
		return make_result(ERR_SEMA_OPERAND_POINTER_RHS_VS_NOT_LHS);
	}

	return RESULT_OK;
}

result_t
sema_pointer_cmp(const struct ctype *lhs, const struct ctype *rhs)
{
	check(sema_pointer_cmp_impl(lhs,
	                            rhs,
	                            SPC_ZERO_AS_NULL | SPC_VOIDP_AS_ANY_PTR));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_pointer_cmp_ptr_math(struct ast *a, struct type_table *types)
{
	switch (a->node_type) {
	case NODE_EXPRESSION_BINARY_ADD:
		if (!ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		    !ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			/* no pointer types involved; nothing more to check */
		} else if (ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		           ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_ADD_POINTER_BOTH);
		} else if (ctype_is_ptr_to_incomplete(
				   &a->u.op_binary.lhs->expr_type,
				   types) ||
		           ctype_is_ptr_to_incomplete(
				   &a->u.op_binary.rhs->expr_type,
				   types)) {
			return make_result(ERR_SEMA_OPERAND_ADD_POINTER_VOID);
		}
		break;
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		if (ctype_is_ptr_to_incomplete(&a->u.op_binary.lhs->expr_type,
		                               types) ||
		    ctype_is_ptr_to_incomplete(&a->u.op_binary.rhs->expr_type,
		                               types)) {
			return make_result(ERR_SEMA_OPERAND_ADD_POINTER_VOID);
		}
		if (ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		    ctype_is_integer(&a->u.op_binary.rhs->expr_type)) {
			assert(ctype_is_equal(&a->expr_type,
			                      &a->u.op_binary.lhs->expr_type));
		} else {
			check(sema_pointer_cmp_impl(
				&a->u.op_binary.lhs->expr_type,
				&a->u.op_binary.rhs->expr_type,
				0));
		}
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}
	return RESULT_OK;
}

struct sema_pointer_state {
	Arena *arena;
	struct type_table *types;
	struct ctype expected_return_type;
};

static WARN_UNUSED result_t
sema_pointer(struct ast *a, void *userdata)
{
	struct sema_pointer_state *state = userdata;
	Arena *arena = state->arena;
	struct type_table *types = state->types;

	switch (a->node_type) {
	case NODE_FUNCTION:
		if (a->u.function.block != NULL) {
			check(ctype_copy(arena,
			                 &a->u.function.return_type,
			                 &state->expected_return_type));
		}
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		check(sema_pointer_cmp(&state->expected_return_type,
		                       &a->u.op_unary.operand->expr_type));
		break;
	case NODE_DECLARATION:
		if (a->u.declare.init != NULL) {
			uint32_t flags = SPC_VOIDP_AS_ANY_PTR;
			/* do not allow zero->nullptr for array init */
			if (!ctype_is_array(&a->u.declare.var_type)) {
				flags |= SPC_ZERO_AS_NULL;
			}
			check(sema_pointer_cmp_impl(
				&a->u.declare.var_type,
				&a->u.declare.init->expr_type,
				flags));
		}
		break;
	case NODE_SWITCH:
		if (ctype_is_pointer(&a->u.switch_.control->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_CASE:
		if (ctype_is_pointer(&a->u.case_.constant->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
		if (ctype_is_pointer(&a->u.op_unary.operand->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		if (ctype_is_void_ptr(&a->u.op_unary.operand->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_DEREF_VOID_PTR);
		}
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		if (ctype_is_ptr_to_incomplete(
			    &a->u.op_unary.operand->expr_type,
			    types)) {
			return make_result(ERR_SEMA_OPERAND_ADD_POINTER_VOID);
		}
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		check(sema_pointer_cmp_ptr_math(a, types));
		break;
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		check(sema_pointer_cmp(&a->u.op_binary.lhs->expr_type,
		                       &a->u.op_binary.rhs->expr_type));
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		check(sema_pointer_cmp_impl(&a->u.op_binary.lhs->expr_type,
		                            &a->u.op_binary.rhs->expr_type,
		                            0));
		break;
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		if (ctype_is_pointer(&a->u.op_binary.lhs->expr_type) ||
		    ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		check(sema_pointer_cmp(&a->u.op_binary.lhs->expr_type,
		                       &a->u.op_binary.rhs->expr_type));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(sema_pointer_cmp(&a->u.op_ternary.then_expr->expr_type,
		                       &a->u.op_ternary.else_expr->expr_type));
		break;
	default:
		break;
	}
	return RESULT_OK;
}

result_t
sema_typecheck_ptr(Arena *arena, struct ast *a, struct type_table *types)
{
	struct sema_ops ops = {
		.node_enter = sema_pointer,
	};
	struct sema_pointer_state state = {
		.arena = arena,
		.types = types,
	};
	check(sema_walk(a, &ops, &state));
	return RESULT_OK;
}
