#include "passes.h"
#include "passes/parse.h"
#include "passes/sema/conversion.h"
#include "passes/sema/expression.h"
#include "passes/sema/flow.h"
#include "passes/sema/linkage.h"
#include "passes/sema/pointer.h"
#include "passes/sema/string.h"
#include "passes/sema/struct.h"
#include "passes/sema/walk.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>

static WARN_UNUSED result_t
sema_compound_assignment(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	enum ast_nodetype new_type = 0;
	switch (a->node_type) {
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
		new_type = NODE_EXPRESSION_BINARY_ADD;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
		new_type = NODE_EXPRESSION_BINARY_SUBTRACT;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
		new_type = NODE_EXPRESSION_BINARY_MULTIPLY;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
		new_type = NODE_EXPRESSION_BINARY_DIVIDE;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
		new_type = NODE_EXPRESSION_BINARY_REMAINDER;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
		new_type = NODE_EXPRESSION_BITWISE_AND;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
		new_type = NODE_EXPRESSION_BITWISE_OR;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
		new_type = NODE_EXPRESSION_BITWISE_XOR;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
		new_type = NODE_EXPRESSION_BITWISE_SHIFT_LEFT;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
		new_type = NODE_EXPRESSION_BITWISE_SHIFT_RIGHT;
		break;
	default:
		return RESULT_OK;
	}

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	new_node->node_type = new_type;
	check(ctype_copy(arena, &a->expr_type, &new_node->expr_type));
	new_node->u.op_binary = a->u.op_binary;
	/* XXX: copying u.op_binary does *not* deep-copy all descendants */

	a->node_type = NODE_EXPRESSION_VARIABLE_ASSIGNMENT;
	a->u.op_binary.rhs = new_node;
	/* retain existing a->u.op_binary.lhs */

	/*
	 * Doubly-link compound assignment nodes to one another, in
	 * preparation for kludge in ir_assignment().
	 */
	new_node->u.op_binary.lhs->kludge.compound_assignment_twin =
		a->u.op_binary.lhs;
	a->u.op_binary.lhs->kludge.compound_assignment_twin =
		new_node->u.op_binary.lhs;

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_subscript(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	if (a->node_type != NODE_EXPRESSION_SUBSCRIPT) {
		return RESULT_OK;
	}

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	new_node->node_type = NODE_EXPRESSION_BINARY_ADD;
	new_node->u.op_binary = a->u.op_binary;

	a->node_type = NODE_EXPRESSION_UNARY_DEREFERENCE;
	a->u.op_unary.operand = new_node;

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_struct_pointer(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	if (a->node_type != NODE_EXPRESSION_STRUCT_POINTER) {
		return RESULT_OK;
	}

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	new_node->node_type = NODE_EXPRESSION_UNARY_DEREFERENCE;
	new_node->u.op_unary.operand = a->u.member_access.lhs;

	a->node_type = NODE_EXPRESSION_STRUCT_MEMBER;
	a->u.member_access.lhs = new_node;

	return RESULT_OK;
}

static WARN_UNUSED bool
is_node_lvalue(const struct ast *a)
{
	while (a->node_type == NODE_EXPRESSION_PAREN_ENCLOSED) {
		a = a->u.op_unary.operand;
	}
	return a->node_type == NODE_EXPRESSION_VARIABLE_USAGE ||
	       (a->node_type == NODE_EXPRESSION_UNARY_DEREFERENCE &&
	        !ctype_is_void_ptr(&a->u.op_unary.operand->expr_type)) ||
	       (a->node_type == NODE_EXPRESSION_STRUCT_MEMBER &&
	        is_node_lvalue(a->u.member_access.lhs));
}

static WARN_UNUSED result_t
sema_lvalue(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool allow_array = false;
	bool allow_struct = false;

	const struct ast *to_check = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		to_check = a->u.op_binary.lhs;
		allow_array = false;
		allow_struct = true;
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		to_check = a->u.op_unary.operand;
		allow_array = false;
		allow_struct = false;
		break;
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		to_check = a->u.op_unary.operand;
		allow_array = true;
		allow_struct = true;
		break;
	default:
		return RESULT_OK;
	}

	if (!is_node_lvalue(to_check)) {
		/*
		 * See related assertions in src/passes/ir.c on u.op_binary.lhs
		 * and NODE_EXPRESSION_VARIABLE_USAGE.
		 */
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE);
	}

	if (!allow_array && ctype_is_array(&to_check->expr_type)) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_ARRAY);
	}

	if (!allow_struct && ctype_is_struct(&to_check->expr_type)) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_STRUCT);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_var_usage(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type == NODE_EXPRESSION_VARIABLE_USAGE &&
	    (a->u.var.stype == SYMBOL_FUNCTION_DECLARATION ||
	     a->u.var.stype == SYMBOL_FUNCTION_DEFINITION)) {
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_INVALID_FUNC,
		                   a->u.var.name.data,
		                   a->u.var.name.sz);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_label_locations(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type != NODE_BLOCK) {
		return RESULT_OK;
	}

	struct string_view name = {0};
	const struct flat *cursor = a->u.block.statements;
	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		switch (cursor->car->node_type) {
		case NODE_LABEL:
			name = cursor->car->u.label.name;
			break;
		case NODE_CASE:
		case NODE_CASE_DEFAULT:
			name.data = "<case>";
			name.sz = strlen(name.data);
			break;
		default:
			continue;
		}
		/*
		 * Check for C23 extensions that the testsuite requires us to
		 * reject with an error, rather than merely warning.
		 */
		if (cursor->cdr == NULL) {
			/* Reject label/case at the very end of a block! */
			return make_result(ERR_SEMA_LABEL_AT_BLOCK_END,
			                   name.data,
			                   name.sz);
		}
		if (cursor->cdr->car->node_type == NODE_DECLARATION ||
		    cursor->cdr->car->node_type == NODE_STRUCT) {
			/* Reject label/case followed by a var declaration! */
			return make_result(
				ERR_SEMA_LABEL_FOLLOWED_BY_DECLARATION,
				name.data,
				name.sz);
		}
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_fn_call(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type == NODE_EXPRESSION_FUNCTION_CALL &&
	    a->u.var.stype == SYMBOL_VARIABLE) {
		return make_result(ERR_SEMA_FUNCTION_CALL_UNCALLABLE,
		                   a->u.var.name.data,
		                   a->u.var.name.sz);
	}
	return RESULT_OK;
}

static WARN_UNUSED bool
is_scalar(const struct ctype *c)
{
	/*
	 * Allow CTYPE_ARRAY_OF, despite it not truly being scalar, assuming
	 * other code will perform array-to-pointer decay.
	 */
	return !ctype_is_void(c) && !ctype_is_struct(c);
}

static WARN_UNUSED result_t
sema_non_scalar(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool scalar = true;

	switch (a->node_type) {
	case NODE_IF_ELSE:
		scalar = is_scalar(&a->u.if_.condition->expr_type);
		break;
	case NODE_LOOP:
		if (a->u.loop.precond->node_type != NODE_EXPRESSION_NULL &&
		    !is_scalar(&a->u.loop.precond->expr_type)) {
			scalar = false;
		}
		if (a->u.loop.postcond->node_type != NODE_EXPRESSION_NULL &&
		    !is_scalar(&a->u.loop.postcond->expr_type)) {
			scalar = false;
		}
		break;
	case NODE_SWITCH:
		scalar = is_scalar(&a->u.switch_.control->expr_type);
		break;
	case NODE_CASE:
		scalar = is_scalar(&a->u.case_.constant->expr_type);
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		scalar = is_scalar(&a->u.op_unary.operand->expr_type);
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
		scalar = is_scalar(&a->u.op_binary.lhs->expr_type) &&
		         is_scalar(&a->u.op_binary.rhs->expr_type);
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		scalar = is_scalar(&a->u.op_ternary.condition->expr_type);
		if (is_scalar(&a->u.op_ternary.then_expr->expr_type) !=
		    is_scalar(&a->u.op_ternary.else_expr->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_TERNARY_MISMATCH);
		}
		break;
	case NODE_EXPRESSION_CAST:
		if (ctype_is_aggregate(&a->u.cast.to_type)) {
			return make_result(
				ERR_SEMA_CAST_TO_ARRAY_OR_STRUCT_INVALID);
		}
		if (is_scalar(&a->u.cast.to_type)) {
			scalar = is_scalar(&a->u.cast.expr->expr_type);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		/* assignment allows struct, in addition to scalar values */
		scalar = !ctype_is_void(&a->u.op_binary.lhs->expr_type) &&
		         !ctype_is_void(&a->u.op_binary.rhs->expr_type);
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		/* guaranteed by sema_lvalue(), is_node_lvalue() */
		assert(is_scalar(&a->u.op_unary.operand->expr_type));
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		/* sema_expr_types() already guarantees ctype_is_pointer() */
		assert(is_scalar(&a->u.op_unary.operand->expr_type));
		break;
	default:
		break;
	}

	if (!scalar) {
		return make_result(ERR_SEMA_OPERAND_SCALAR_REQUIRED);
	}
	return RESULT_OK;
}

struct sema_incomplete_state {
	struct type_table *types;
	struct ast *prev;
};

static WARN_UNUSED result_t
sema_incomplete(struct ast *a, void *userdata MAYBE_UNUSED)
{
	struct sema_incomplete_state *state = userdata;
	struct type_table *types = state->types;

	const bool parent_is_addr_of =
		(state->prev != NULL &&
	         state->prev->node_type == NODE_EXPRESSION_UNARY_ADDRESS_OF &&
	         state->prev->u.op_unary.operand == a);

	switch (a->node_type) {
	case NODE_EXPRESSION_UNARY_SIZE_OF:
		if (ctype_is_incomplete(&a->u.op_unary.operand->expr_type,
		                        types)) {
			return make_result(ERR_SEMA_OPERAND_SIZEOF_INCOMPLETE);
		}
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		if (!parent_is_addr_of &&
		    ctype_is_pointer(&a->u.op_unary.operand->expr_type) &&
		    ctype_is_incomplete(
			    a->u.op_unary.operand->expr_type.referent,
			    types)) {
			return make_result(ERR_SEMA_OPERAND_DEREF_INCOMPLETE);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		if (!parent_is_addr_of &&
		    ctype_is_incomplete(&a->expr_type, types)) {
			return make_result(
				ERR_SEMA_VARIABLE_USAGE_TYPE_INCOMPLETE,
				a->u.var.name.data,
				a->u.var.name.sz);
		}
		break;
	default:
		break;
	}

	state->prev = a;
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_double(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool valid = true;

	switch (a->node_type) {
	case NODE_SWITCH:
		if (ctype_is_floating_point(&a->u.switch_.control->expr_type)) {
			valid = false;
		}
		break;
	case NODE_CASE:
		if (ctype_is_floating_point(&a->u.case_.constant->expr_type)) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		if (ctype_is_floating_point(&a->expr_type)) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		if (ctype_is_floating_point(&a->u.op_binary.lhs->expr_type) ||
		    ctype_is_floating_point(&a->u.op_binary.rhs->expr_type)) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		if ((ctype_is_floating_point(&a->u.op_binary.lhs->expr_type) &&
		     ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) ||
		    (ctype_is_floating_point(&a->u.op_binary.rhs->expr_type) &&
		     ctype_is_pointer(&a->u.op_binary.lhs->expr_type))) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_CAST:
		if ((ctype_is_floating_point(&a->u.cast.to_type) &&
		     ctype_is_pointer(&a->u.cast.expr->expr_type)) ||
		    (ctype_is_floating_point(&a->u.cast.expr->expr_type) &&
		     ctype_is_pointer(&a->u.cast.to_type))) {
			valid = false;
		}
		break;
	default:
		break;
	}

	if (!valid) {
		return make_result(ERR_SEMA_OPERAND_DOUBLE_INVALID);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
symbols_finalize(Arena *arena, struct symbol_table *s, struct type_table *types)
{
	for (struct symbol *v = s->variables; v != NULL; v = v->next) {
		if (v->linkage.initial == INITIAL_VALUE_TENTATIVE) {
			check(constant_set_zero(arena,
			                        &v->c89type,
			                        types,
			                        &v->linkage.initializer));
		}
	}
	return RESULT_OK;
}

result_t
sema_typecheck(Arena *arena,
               struct ast *a,
               long long int *label_generator,
               struct symbol_table *s,
               struct type_table *types)
{
	struct sema_ops ops = {0};

	debug("Expanding compound assignment statements");
	ops.node_enter = sema_compound_assignment;
	check(sema_walk(a, &ops, arena));

	debug("Expanding array subscript expressions");
	ops.node_enter = sema_subscript;
	check(sema_walk(a, &ops, arena));

	debug("Expanding struct pointer expressions");
	ops.node_enter = sema_struct_pointer;
	check(sema_walk(a, &ops, arena));

	debug("Expanding string literals and hoisting if necessary");
	check(sema_typecheck_strlit(arena, a, s, types));

	debug("Checking variable usage");
	ops.node_enter = sema_var_usage;
	check(sema_walk(a, &ops, NULL));

	debug("Checking label locations");
	ops.node_enter = sema_label_locations;
	check(sema_walk(a, &ops, NULL));

	debug("Checking function calls");
	ops.node_enter = sema_fn_call;
	check(sema_walk(a, &ops, NULL));

	debug("Propagating expression types");
	check(sema_typecheck_expr(arena, a, types));

	debug("Checking for invalid lvalues");
	ops.node_enter = sema_lvalue;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid usage of non-scalar expressions");
	ops.node_enter = sema_non_scalar;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid usage of incomplete types");
	ops.node_enter = sema_incomplete;
	{
		struct sema_incomplete_state incomplete_state = {0};
		incomplete_state.types = types;
		check(sema_walk(a, &ops, &incomplete_state));
	}

	debug("Checking for invalid double usage");
	ops.node_enter = sema_double;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid pointer usage");
	check(sema_typecheck_ptr(arena, a, types));

	debug("Checking for invalid struct usage");
	check(sema_typecheck_struct(arena, a));

	debug("Inserting cast expressions for implicit conversions");
	check(sema_typecheck_conversion(arena, a, types));

	debug("Labeling loops, loop breaks, and continues");
	check(sema_label_loops(arena, a, label_generator));

	debug("Labeling goto statements and labels");
	check(sema_label_gotos(arena, a, label_generator));

	debug("Determining linkage for function and variable symbols");
	check(sema_typecheck_linkage(arena, a, s, types));

	debug("Finalizing initializers for symbols with linkage");
	check(symbols_finalize(arena, s, types));

	return RESULT_OK;
}
