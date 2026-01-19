#include "passes/sema/walk.h"

#include "passes/parse.h"
#include "passes/sema/conversion.h"

#include <assert.h>

static WARN_UNUSED result_t
sema_walk_flat(struct flat *a, struct sema_ops *ops, void *u)
{
	const struct flat *cursor = a;
	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		check(sema_walk(cursor->car, ops, u));
	}
	return RESULT_OK;
}

result_t
sema_walk(struct ast *a, struct sema_ops *ops, void *u)
{
	if (a == NULL) {
		return RESULT_OK;
	}

	if (ops->node_enter != NULL) {
		check(ops->node_enter(a, u));
	}

	switch (a->node_type) {
	case NODE_PROGRAM:
		check(sema_walk_flat(a->u.program.globals, ops, u));
		break;
	case NODE_FUNCTION:
		check(sema_walk(a->u.function.block, ops, u));
		break;
	case NODE_BLOCK:
		check(sema_walk_flat(a->u.block.statements, ops, u));
		break;
	case NODE_DECLARATION:
		check(sema_walk(a->u.declare.init, ops, u));
		break;
	case NODE_IF_ELSE:
		check(sema_walk(a->u.if_.condition, ops, u));
		check(sema_walk_flat(a->u.if_.then_clause, ops, u));
		check(sema_walk_flat(a->u.if_.else_clause, ops, u));
		break;
	case NODE_LOOP:
		check(sema_walk(a->u.loop.precond, ops, u));
		check(sema_walk_flat(a->u.loop.body, ops, u));
		check(sema_walk(a->u.loop.incr, ops, u));
		check(sema_walk(a->u.loop.postcond, ops, u));
		break;
	case NODE_SWITCH:
		check(sema_walk(a->u.switch_.control, ops, u));
		check(sema_walk_flat(a->u.switch_.body, ops, u));
		break;
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
		break;
	case NODE_EXPRESSION_INITIALIZER:
		check(sema_walk(a->u.init.single, ops, u));
		check(sema_walk_flat(a->u.init.multi, ops, u));
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
		check(sema_walk(a->u.op_unary.operand, ops, u));
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
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
	case NODE_EXPRESSION_SUBSCRIPT:
		check(sema_walk(a->u.op_binary.lhs, ops, u));
		check(sema_walk(a->u.op_binary.rhs, ops, u));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(sema_walk(a->u.op_binary.lhs, ops, u));
		check(sema_walk(a->u.op_ternary.condition, ops, u));
		check(sema_walk(a->u.op_ternary.then_expr, ops, u));
		check(sema_walk(a->u.op_ternary.else_expr, ops, u));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(sema_walk_flat(a->u.call.args, ops, u));
		break;
	case NODE_EXPRESSION_CAST:
		check(sema_walk(a->u.cast.expr, ops, u));
		break;
	case NODE_EXPRESSION_STRUCT_MEMBER:
	case NODE_EXPRESSION_STRUCT_POINTER:
		check(sema_walk(a->u.member_access.lhs, ops, u));
		break;
	case NODE_STRUCT:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_NULL:
	case NODE_CONSTANT:
	case NODE_CONSTANT_STR:
		break;
	}

	if (ops->node_exit != NULL) {
		check(ops->node_exit(a, u));
	}

	return RESULT_OK;
}

struct sema_trampoline_state {
	result_t (*inner)(struct ast **, const struct ctype *, void *);
	void *inner_userdata;
};

static WARN_UNUSED result_t
sema_walk_initializer_trampoline(struct ast **ast_pp,
                                 const struct ctype *dst_type,
                                 const struct type_member *member_info
                                         MAYBE_UNUSED,
                                 void *userdata)
{
	struct sema_trampoline_state *state = userdata;
	check(state->inner(ast_pp, dst_type, state->inner_userdata));
	return RESULT_OK;
}

result_t
sema_walk_initializer(struct ast **ast_pp,
                      const struct ctype *dst_type,
                      struct type_table *tt,
                      result_t (*visit)(struct ast **,
                                        const struct ctype *,
                                        void *),
                      void *ud)
{
	struct sema_initializer_ops ops = {
		.visit = sema_walk_initializer_trampoline,
	};
	struct sema_trampoline_state state = {
		.inner = visit,
		.inner_userdata = ud,
	};
	check(sema_walk_initializer_scope(ast_pp, dst_type, tt, &ops, &state));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_walk_initializer_s_impl(struct ast **ast_pp,
                             const struct ctype *dst_type,
                             const struct type_member *dst_member,
                             struct type_table *tt,
                             struct sema_initializer_ops *ops,
                             void *ud)
{
	assert(dst_type != NULL);

	struct ast **original_pp = ast_pp;
	ast_pp = cast_unpack(ast_pp);
	assert((**ast_pp).node_type == NODE_EXPRESSION_INITIALIZER);
	const bool early_return = ((**ast_pp).u.init.single != NULL);

	if (ops->visit != NULL) {
		check(ops->visit(original_pp, dst_type, dst_member, ud));
	}

	if (early_return) {
		return RESULT_OK;
	}

	struct type_table *type_entry = NULL;
	if (ctype_is_struct(dst_type)) {
		type_entry = types_find(tt, dst_type);
		assert(type_entry != NULL);
	} else {
		assert(ctype_is_pointer(dst_type));
		assert(dst_type->referent != NULL);
	}

	if (ctype_is_struct(dst_type) && ops->struct_enter != NULL) {
		check(ops->struct_enter(type_entry, ud));
	}

	long long unsigned element_count = 0;

	for (struct flat *f = (**ast_pp).u.init.multi; f != NULL; f = f->cdr) {
		if (type_entry != NULL &&
		    element_count >= type_entry->n_members) {
			/*
			 * See sema_expr_types(), visit_expr(), and
			 * ERR_SEMA_INIT_COMPOUND_EXCESS_ELEMENTS.
			 */
			break;
		}

		const struct ctype *c = NULL;
		const struct type_member *m = NULL;
		if (ctype_is_struct(dst_type)) {
			assert(type_entry != NULL);
			m = &type_entry->members[element_count];
			c = &m->member_type;
		} else {
			c = dst_type->referent;
		}

		check(sema_walk_initializer_s_impl(&f->car, c, m, tt, ops, ud));
		++element_count;
	}

	if (ctype_is_struct(dst_type) && ops->struct_exit != NULL) {
		check(ops->struct_exit(type_entry, ud));
	}
	return RESULT_OK;
}

result_t
sema_walk_initializer_scope(struct ast **ast_pp,
                            const struct ctype *dst_type,
                            struct type_table *tt,
                            struct sema_initializer_ops *ops,
                            void *ud)
{
	const struct type_member *tm = NULL;
	check(sema_walk_initializer_s_impl(ast_pp, dst_type, tm, tt, ops, ud));
	return RESULT_OK;
}
