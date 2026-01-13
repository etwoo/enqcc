#include "passes/sema/expression.h"

#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/sema/conversion.h"
#include "passes/sema/walk.h"

#include <assert.h>
#include <string.h> /* for memset() */

/*
 * From "Writing a C Compiler" by Nora Sandler, Chapter 17, Section "sizeof
 * Expressions":
 *
 *   A sizeof expression has type size_t; in our implementation, that's
 *   just unsigned long.
 */
static const struct ctype LIKE_SIZE_T = {.t = CTYPE_UNSIGNED_LONG};

static WARN_UNUSED result_t
promote_if_char(Arena *arena, struct ast **a)
{
	if (*a != NULL && ctype_is_charlike(&(**a).expr_type)) {
		const struct ctype promote_type = {
			.t = CTYPE_INT,
		};
		check(cast_if(arena, &promote_type, a));
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_expr_types_initializer_zero_pad(Arena *arena,
                                     struct ast *init,
                                     const struct ctype *declaration_type,
                                     struct type_table *types)
{
	assert(init->node_type == NODE_EXPRESSION_INITIALIZER);

	if (init->u.init.single != NULL &&
	    ctype_is_strlike_array(&init->u.init.single->expr_type)) {
		return RESULT_OK;
	}

	if (!ctype_is_aggregate(declaration_type)) {
		if (init->u.init.single == NULL) {
			check(parse_alloc(arena,
			                  &init->u.init.single,
			                  NODE_CONSTANT));
			init->u.init.single->u.num = 0;
			init->u.init.single->expr_type.t = CTYPE_INT;
			init->u.init.single->expr_type
				.maybe_null_pointer_constant = true;
			init->expr_type.t = CTYPE_INT;
			init->expr_type.maybe_null_pointer_constant = true;
		}
		return RESULT_OK;
	}

	if (init->u.init.single != NULL) {
		assert(ctype_is_aggregate(&init->u.init.single->expr_type));
		assert(!ctype_is_struct_mismatch(
			declaration_type,
			&init->u.init.single->expr_type));
		return RESULT_OK;
	}

	assert(init->u.init.single == NULL);
	struct flat **dst = &init->u.init.multi;

	long long unsigned element_limit = 0;
	struct type_table *type_entry = NULL;

	if (ctype_is_struct(declaration_type)) {
		type_entry = types_find(types, declaration_type);
		assert(type_entry != NULL);
		element_limit = type_entry->n_members;
	} else {
		assert(ctype_is_pointer(declaration_type));
		assert(declaration_type->referent != NULL);
		element_limit = declaration_type->sz;
	}

	long long unsigned element_count = 0;
	for (; element_count < element_limit; ++element_count) {
		const struct ctype *c = NULL;
		if (ctype_is_struct(declaration_type)) {
			assert(type_entry != NULL);
			c = &type_entry->members[element_count].member_type;
		} else {
			c = declaration_type->referent;
		}

		if (*dst == NULL) {
			check(flat_alloc(arena, dst));
			check(parse_alloc(arena,
			                  &(**dst).car,
			                  NODE_EXPRESSION_INITIALIZER));
			check(ctype_copy(arena, c, &(**dst).car->expr_type));
		}

		check(sema_expr_types_initializer_zero_pad(arena,
		                                           (**dst).car,
		                                           c,
		                                           types));
		dst = &(**dst).cdr;
	}

	return RESULT_OK;
}

struct sema_expr_state {
	Arena *arena;
	struct type_table *types;
};

static WARN_UNUSED result_t
visit_expr(struct ast **ast_handle,
           const struct ctype *declaration_type,
           void *userdata)
{
	struct sema_expr_state *state = userdata;
	Arena *arena = state->arena;
	struct type_table *types = state->types;

	struct ast *init = *ast_handle;
	assert(init->node_type == NODE_EXPRESSION_INITIALIZER);

	if (init->u.init.single != NULL) {
		if (ctype_is_aggregate(declaration_type) &&
		    !ctype_is_aggregate(&init->u.init.single->expr_type)) {
			return make_result(ERR_SEMA_INIT_COMPOUND_WITH_SCALAR);
		}
		if (ctype_is_struct_mismatch(declaration_type,
		                             &init->u.init.single->expr_type)) {
			return make_result(ERR_SEMA_INIT_STRUCT_MISMATCH);
		}
		/*
		 * For scalar init, copy upward from constant to containing
		 * NODE_EXPRESSION_INITIALIZER.
		 */
		check(ctype_copy(arena,
		                 &init->u.init.single->expr_type,
		                 &init->expr_type));
		return RESULT_OK;
	}

	if (!ctype_is_aggregate(declaration_type)) {
		/*
		 * For now, reject compound initializers for scalar variables.
		 * In the future, it may make sense to support the special-case
		 * compound initializer {0} for scalar init.
		 */
		return make_result(ERR_SEMA_INIT_SCALAR_WITH_COMPOUND);
	}

	/*
	 * sema_str_literal() diverts array initializor of char pointer
	 * variable declaration; hence, should not appear here.
	 */
	assert(!ctype_is_strlike_ptr(declaration_type));

	/*
	 * For compound init, copy from LHS array type declaration to RHS
	 * compound init expression.
	 */
	check(ctype_copy(arena, declaration_type, &init->expr_type));

	long long unsigned element_count = 0;
	for (struct flat *f = init->u.init.multi; f != NULL; f = f->cdr) {
		++element_count;
	}

	if (element_count == 0) {
		return make_result(ERR_SEMA_INIT_COMPOUND_EMPTY);
	}

	long long unsigned element_limit = 0;
	if (ctype_is_struct(declaration_type)) {
		struct type_table *type_entry =
			types_find(types, declaration_type);
		assert(type_entry != NULL);
		element_limit = type_entry->n_members;
	} else {
		assert(ctype_is_pointer(declaration_type));
		element_limit = declaration_type->sz;
	}

	if (element_count > element_limit) {
		return make_result(ERR_SEMA_INIT_COMPOUND_EXCESS_ELEMENTS);
	}

	return RESULT_OK;
}

/*
 * See sema_conversion_initializer() for related logic.
 */
static WARN_UNUSED result_t
sema_expr_types_initializer(struct ast *a, struct sema_expr_state *state)
{
	assert(a->node_type == NODE_DECLARATION);
	if (a->u.declare.init == NULL) {
		return RESULT_OK;
	}
	check(sema_walk_initializer(&a->u.declare.init,
	                            &a->u.declare.var_type,
	                            state->types,
	                            visit_expr,
	                            state));
	check(sema_expr_types_initializer_zero_pad(state->arena,
	                                           a->u.declare.init,
	                                           &a->u.declare.var_type,
	                                           state->types));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_expr_types_struct_member(Arena *arena,
                              struct ast *a,
                              struct type_table *types)
{
	assert(a->node_type == NODE_EXPRESSION_STRUCT_MEMBER);
	const struct ctype *lhs_type = &a->u.member_access.lhs->expr_type;
	const struct string_view *member_name = &a->u.member_access.member.name;

	if (!ctype_is_struct(lhs_type)) {
		return make_result(ERR_SEMA_OPERAND_MEMBER_INVALID);
	}

	if (ctype_is_incomplete(lhs_type, types)) {
		return make_result(ERR_SEMA_OPERAND_MEMBER_INCOMPLETE);
	}

	struct type_table *type_entry = types_find(types, lhs_type);
	assert(type_entry != NULL); /* guaranteed by !ctype_is_incomplete() */

	struct ctype *member_type = ctype_of_member(type_entry, member_name);
	if (member_type == NULL) {
		return make_result(ERR_SEMA_OPERAND_MEMBER_NONEXISTENT,
		                   member_name->data,
		                   member_name->sz);
	}

	check(ctype_copy(arena, member_type, &a->expr_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_expr_types(struct ast *a, void *userdata)
{
	struct sema_expr_state *state = userdata;
	Arena *arena = state->arena;
	struct type_table *types = state->types;

	switch (a->node_type) {
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_BLOCK:
	case NODE_STRUCT:
	case NODE_IF_ELSE:
	case NODE_LOOP:
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
		break; /* expr_type has no meaning in this context */
	case NODE_DECLARATION:
		check(sema_expr_types_initializer(a, state));
		break;
	case NODE_EXPRESSION_INITIALIZER:
		break; /* handled by NODE_DECLARATION case */
	case NODE_SWITCH:
		check(promote_if_char(arena, &a->u.switch_.control));
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		check(promote_if_char(arena, &a->u.op_unary.operand));
		__attribute__((fallthrough));
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
	case NODE_FUNCTION_RETURN_STATEMENT:
		check(ctype_copy(arena,
		                 &a->u.op_unary.operand->expr_type,
		                 &a->expr_type));
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		if (!ctype_is_pointer(&a->u.op_unary.operand->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_DEREF_INVALID);
		}
		check(ctype_copy(arena,
		                 a->u.op_unary.operand->expr_type.referent,
		                 &a->expr_type));
		break;
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		/* reset if non-NULL referent -- sema_compound_assignment() */
		memset(&a->expr_type, 0, sizeof(a->expr_type));
		a->expr_type.t = CTYPE_POINTER_TO;
		check(ctype_alloc(arena, &a->expr_type.referent));
		check(ctype_copy(arena,
		                 &a->u.op_unary.operand->expr_type,
		                 a->expr_type.referent));
		break;
	case NODE_EXPRESSION_UNARY_SIZE_OF:
		check(ctype_copy(arena, &LIKE_SIZE_T, &a->expr_type));
		break;
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		check(promote_if_char(arena, &a->u.op_binary.lhs));
		check(promote_if_char(arena, &a->u.op_binary.rhs));
		__attribute__((fallthrough));
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
		a->expr_type.t = CTYPE_INT; /* effectively cast to bool */
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
		check(promote_if_char(arena, &a->u.op_binary.lhs));
		check(promote_if_char(arena, &a->u.op_binary.rhs));
		if (a->node_type == NODE_EXPRESSION_BINARY_SUBTRACT &&
		    ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		    ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			check(ctype_copy(arena,
			                 &LIKE_PTRDIFF_T,
			                 &a->expr_type));
		} else {
			check(ctype_copy(
				arena,
				get_common_ctype(
					&a->u.op_binary.lhs->expr_type,
					&a->u.op_binary.rhs->expr_type),
				&a->expr_type));
		}
		break;
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		if (a->node_type != NODE_EXPRESSION_VARIABLE_ASSIGNMENT) {
			check(promote_if_char(arena, &a->u.op_binary.lhs));
			check(promote_if_char(arena, &a->u.op_binary.rhs));
		}
		/*
		 * Shift left/right takes the LHS type, not the common
		 * type of the two sides. The number of shift bits on
		 * the RHS is typically small, but even if that value is
		 * large enough to require a type wider than the LHS,
		 * that should not result in sign extension.
		 *
		 * Variable assignment similarly takes the LHS type,
		 * corresponding to the assigned-to variable.
		 */
		check(ctype_copy(arena,
		                 &a->u.op_binary.lhs->expr_type,
		                 &a->expr_type));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(ctype_copy(
			arena,
			get_common_ctype(&a->u.op_ternary.then_expr->expr_type,
		                         &a->u.op_ternary.else_expr->expr_type),
			&a->expr_type));
		break;
	case NODE_EXPRESSION_CAST:
		check(ctype_copy(arena, &a->u.cast.to_type, &a->expr_type));
		break;
	case NODE_EXPRESSION_STRUCT_MEMBER:
		check(sema_expr_types_struct_member(arena, a, types));
		break;
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_CONSTANT:
		break; /* resolve_expr() in parse.c handles leaf nodes */
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
		assert(0 && "COMPOUND_ASSIGN_* should have been eliminated");
		break;
	case NODE_EXPRESSION_SUBSCRIPT:
		assert(0 && "SUBSCRIPT should have been eliminated");
		break;
	case NODE_EXPRESSION_STRUCT_POINTER:
		assert(0 && "STRUCT_POINTER should have been eliminated");
		break;
	case NODE_CONSTANT_STR:
		assert(0 && "CONSTANT_STR should have been eliminated");
		break;
	}

	return RESULT_OK;
}

result_t
sema_typecheck_expr(Arena *arena, struct ast *a, struct type_table *types)
{
	struct sema_ops ops = {
		.node_enter = NULL,
		.node_exit = sema_expr_types,
	};
	struct sema_expr_state state = {
		.arena = arena,
		.types = types,
	};
	check(sema_walk(a, &ops, &state));
	return RESULT_OK;
}
