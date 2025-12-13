#include "passes/ir.h"

#include "lang/symbol.h"
#include "passes.h"
#include "passes/parse.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdbool.h>

static WARN_UNUSED result_t
ir_alloc_op(Arena *arena, struct ir_op **dst)
{
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_IR_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

static struct ir_op *
ir_op_list_back(struct ir_op *p)
{
	assert(p != NULL);
	while (p != NULL && p->next != NULL) {
		p = p->next;
	}
	return p;
}

static struct ir_op *
ir_op_list_concat(struct ir_op *first, struct ir_op *second)
{
	struct ir_op *head = NULL;
	if (first == NULL) {
		head = second;
	} else {
		head = first;
		while (first->next != NULL) {
			first = first->next;
		}
		first->next = second;
	}
	return head;
}

static void
ir_val_copy(const struct ir_val *src, struct ir_val *dst)
{
	memcpy(dst, src, sizeof(*dst));
}

/*
 * Related: sema_lvalue() in src/passes/sema.c
 */
static WARN_UNUSED const struct ast *
ir_unpack_parens(const struct ast *a)
{
	while (a->node_type == NODE_EXPRESSION_PAREN_ENCLOSED) {
		a = a->u.op_unary.operand;
	}
	return a;
}

static WARN_UNUSED const struct ast *
ir_assignment_lvalue_suitable_for_store(const struct ast *a)
{
	const struct ast *candidate = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		candidate = ir_unpack_parens(a->u.op_binary.lhs);
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		candidate = ir_unpack_parens(a->u.op_unary.operand);
		break;
	default:
		break;
	}

	if (candidate != NULL &&
	    candidate->node_type == NODE_EXPRESSION_UNARY_DEREFERENCE) {
		const struct ast *inner =
			ir_unpack_parens(candidate->u.op_unary.operand);
		if (inner->node_type != NODE_EXPRESSION_UNARY_ADDRESS_OF) {
			return inner;
		}
	}

	return NULL;
}

static WARN_UNUSED result_t
ir_val_from_ast_variable_like(Arena *arena,
                              const struct ast *src,
                              struct ir_val *dst)
{
	const struct ast_symbol *sym = NULL;
	switch (src->node_type) {
	case NODE_DECLARATION:
		sym = &src->u.declare.identifier;
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		// TODO: deal with pointer deref as part of lvalue
		assert(ir_unpack_parens(src->u.op_unary.operand)->node_type ==
		       NODE_EXPRESSION_VARIABLE_USAGE);
		sym = &ir_unpack_parens(src->u.op_unary.operand)->u.var;
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		if (src->u.op_binary.lhs->node_type == NODE_EXPRESSION_CAST) {
			/* unpack nodes inserted by sema_implicit_cast() */
			const struct ast *cast_envelope = src->u.op_binary.lhs;
			// TODO: deal with pointer deref as part of lvalue
			assert(cast_envelope->u.cast.expr->node_type ==
			       NODE_EXPRESSION_VARIABLE_USAGE);
			sym = &cast_envelope->u.cast.expr->u.var;
		} else {
			assert(src->u.op_binary.lhs->node_type ==
			       NODE_EXPRESSION_VARIABLE_USAGE);
			sym = &src->u.op_binary.lhs->u.var;
		}
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		sym = &src->u.var;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	if (some_linkage(sym->ltype)) {
		dst->subtype = IR_VAL_VARIABLE_DATA;
		dst->varname = sym->name;
	} else {
		dst->subtype = IR_VAL_TEMPORARY_VARIABLE;
	}

	dst->num = sym->unique;
	check(ctype_copy(arena, &src->expr_type, &dst->c89type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_val_tmpvar(Arena *arena,
              long long int unique,
              const struct ctype *vtype,
              struct ir_val *dst)
{
	dst->subtype = IR_VAL_TEMPORARY_VARIABLE;
	dst->num = unique;
	check(ctype_copy(arena, vtype, &dst->c89type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_val_tmpvar_gen(Arena *arena,
                  struct intermediate *ir,
                  const struct ctype *vtype,
                  struct ir_val *dst)
{
	check(ir_val_tmpvar(arena, ir->env.generator++, vtype, dst));
	return RESULT_OK;
}

static WARN_UNUSED enum ir_linkage
ir_map_linkage(enum symbol_linkage linkage)
{
	assert(some_linkage(linkage));
	if (is_external(linkage)) {
		return IR_LINKAGE_EXTERNAL;
	}
	return IR_LINKAGE_INTERNAL;
}

static result_t ir_expr(Arena *arena,
                        const struct ast *a,
                        struct intermediate *ir,
                        struct ir_op **dst,
                        struct ir_val *return_value) WARN_UNUSED;

static WARN_UNUSED result_t
ir_ret_op(Arena *arena,
          const struct ast *a,
          struct intermediate *ir,
          struct ir_op **dst)
{
	assert(a->node_type == NODE_FUNCTION_RETURN_STATEMENT);

	struct ir_op *inner = NULL;
	struct ir_val inner_return = {0};
	check(ir_expr(arena, a->u.op_unary.operand, ir, &inner, &inner_return));
	assert(inner_return.subtype != IR_VAL_NONE);

	struct ir_op *returner = NULL;
	check(ir_alloc_op(arena, &returner));
	returner->opcode = IR_OP_RET;
	ir_val_copy(&inner_return, &returner->args[0]);

	*dst = ir_op_list_concat(inner, returner);
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_block(Arena *arena,
         const struct flat *cursor,
         struct intermediate *ir,
         struct ir_op **block_ops)
{
	struct ir_op *head = NULL;
	struct ir_op **dst = block_ops;

	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);

		if (cursor->car->node_type == NODE_FUNCTION) {
			/*
			 * For IR purposes, ignore function declarations that
			 * appear inside other blocks.
			 */
			continue;
		}

		struct ir_val dummy = {0};
		check(ir_expr(arena, cursor->car, ir, dst, &dummy));
		/*
		 * Currently, <block_return> value of each overall block
		 * expression is unused. Discard it after each loop iteration.
		 */

		if (head == NULL) {
			head = *dst;
		}

		if (*dst != NULL) {
			dst = &ir_op_list_back(*dst)->next;
		}
	}

	*block_ops = head;
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_decl_init(Arena *arena,
             const struct ast *a,
             struct intermediate *ir,
             struct ir_op **dst)
{
	assert(a->node_type == NODE_DECLARATION);

	struct ir_op *assigner = NULL;
	check(ir_alloc_op(arena, &assigner));
	assigner->opcode = IR_OP_COPY;

	struct ir_op *inner = NULL;
	struct ir_val inner_return = {0};
	check(ir_expr(arena, a->u.declare.init, ir, &inner, &inner_return));

	ir_val_copy(&inner_return, &assigner->args[0]);
	check(ir_val_from_ast_variable_like(arena, a, &assigner->args[1]));

	*dst = ir_op_list_concat(inner, assigner);
	return RESULT_OK;
}

struct if_else_prep {
	struct ir_op *jumper;
	struct ir_op *body;
	struct ir_op *assign_result;
	struct ir_op *jump_target;
};

static WARN_UNUSED result_t
ir_if_else_prepare(Arena *arena,
                   const struct flat *ast_clause,
                   const struct ast *ast_clause_returning_value,
                   struct intermediate *ir,
                   const struct ir_val *jump_operand,
                   const long long int jump_label,
                   const long long int assign_result_unique,
                   struct if_else_prep *out)
{
	/* non-NULL ast_clause XOR non-NULL ast_clause_returning_value */
	assert((ast_clause == NULL) != (ast_clause_returning_value == NULL));
	/* assign_result_unique set -> ast_clause_returning_value set */
	assert(assign_result_unique < 0 || ast_clause_returning_value != NULL);

	check(ir_alloc_op(arena, &out->jumper));
	if (jump_operand != NULL) {
		out->jumper->opcode = IR_OP_JUMP_IF_ZERO;
		ir_val_copy(jump_operand, &out->jumper->args[0]);
		out->jumper->args[1].subtype = IR_VAL_JUMP_TARGET_LABEL;
		out->jumper->args[1].num = jump_label;
	} else {
		out->jumper->opcode = IR_OP_JUMP;
		out->jumper->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
		out->jumper->args[0].num = jump_label;
	}

	struct ir_val body_return = {0};
	if (ast_clause != NULL) {
		check(ir_block(arena, ast_clause, ir, &out->body));
	} else if (ast_clause_returning_value != NULL) {
		check(ir_expr(arena,
		              ast_clause_returning_value,
		              ir,
		              &out->body,
		              &body_return));
	} else {
		assert(0); /* logic error in caller */
	}

	if (assign_result_unique >= 0) {
		check(ir_alloc_op(arena, &out->assign_result));
		out->assign_result->opcode = IR_OP_COPY;
		ir_val_copy(&body_return, &out->assign_result->args[0]);
		check(ir_val_tmpvar(arena,
		                    assign_result_unique,
		                    &ast_clause_returning_value->expr_type,
		                    &out->assign_result->args[1]));
	}

	check(ir_alloc_op(arena, &out->jump_target));
	out->jump_target->opcode = IR_OP_LABEL;
	out->jump_target->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	out->jump_target->args[0].num = jump_label;
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_if_else(Arena *arena,
           const struct ast *a,
           struct intermediate *ir,
           struct ir_op **dst,
           struct ir_val *return_value)
{
	bool has_else = false;
	bool ternary = false;
	switch (a->node_type) {
	case NODE_IF_ELSE:
		has_else = (a->u.if_.else_clause != NULL);
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		has_else = true;
		ternary = true;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	const long long int cond_jump_to = ir->env.labels++;
	const long long int end_jump_to = has_else ? ir->env.labels++ : -1;
	const long long int assign_result_unique =
		ternary ? ir->env.generator++ : -1;

	struct ir_op *cond_ops = NULL;
	struct ir_val cond_return = {0};
	check(ir_expr(arena, a->u.if_.condition, ir, &cond_ops, &cond_return));

	struct if_else_prep then_p = {0};
	check(ir_if_else_prepare(arena,
	                         ternary ? NULL : a->u.if_.then_clause,
	                         ternary ? a->u.op_ternary.then_expr : NULL,
	                         ir,
	                         &cond_return,
	                         cond_jump_to,
	                         assign_result_unique,
	                         &then_p));

	struct if_else_prep or_p = {0};
	if (has_else) {
		check(ir_if_else_prepare(arena,
		                         ternary ? NULL : a->u.if_.else_clause,
		                         ternary ? a->u.op_ternary.else_expr
		                                 : NULL,
		                         ir,
		                         NULL,
		                         end_jump_to,
		                         assign_result_unique,
		                         &or_p));
	}

	if (assign_result_unique >= 0) {
		assert(return_value->subtype == IR_VAL_NONE);
		check(ir_val_tmpvar(arena,
		                    assign_result_unique,
		                    &a->expr_type,
		                    return_value));
	}

	struct ir_op *collect[] = {
		cond_ops,
		then_p.jumper,
		then_p.body,
		then_p.assign_result,
		or_p.jumper,
		then_p.jump_target,
		or_p.body,
		or_p.assign_result,
		or_p.jump_target,
	};
	for (size_t i = 0; i < ARRAY_SIZE(collect); ++i) {
		*dst = ir_op_list_concat(*dst, collect[i]);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_loop(Arena *arena,
        const struct ast *a,
        struct intermediate *ir,
        struct ir_op **dst)
{
	assert(a->node_type == NODE_LOOP);

	struct ir_val go_start = {0};
	go_start.subtype = IR_VAL_JUMP_TARGET_LABEL;
	go_start.num = a->u.loop.label_start;

	struct ir_val go_continue = {0};
	go_continue.subtype = IR_VAL_JUMP_TARGET_LABEL;
	go_continue.num = a->u.loop.label_continue;

	struct ir_val go_end = {0};
	go_end.subtype = IR_VAL_JUMP_TARGET_LABEL;
	go_end.num = a->u.loop.label_end;

	struct ir_op *start_label = NULL;
	check(ir_alloc_op(arena, &start_label));
	start_label->opcode = IR_OP_LABEL;
	ir_val_copy(&go_start, &start_label->args[0]);

	struct ir_op *precond = NULL;
	struct ir_val precond_return = {0};
	check(ir_expr(arena, a->u.loop.precond, ir, &precond, &precond_return));

	struct ir_op *precond_jumper = NULL;
	if (a->u.loop.precond->node_type != NODE_EXPRESSION_NULL) {
		assert(precond_return.subtype != IR_VAL_NONE);
		check(ir_alloc_op(arena, &precond_jumper));
		precond_jumper->opcode = IR_OP_JUMP_IF_ZERO;
		ir_val_copy(&precond_return, &precond_jumper->args[0]);
		ir_val_copy(&go_end, &precond_jumper->args[1]);
	}

	struct ir_op *body = NULL;
	check(ir_block(arena, a->u.loop.body, ir, &body));

	struct ir_op *continue_label = NULL;
	check(ir_alloc_op(arena, &continue_label));
	continue_label->opcode = IR_OP_LABEL;
	ir_val_copy(&go_continue, &continue_label->args[0]);

	struct ir_op *incr = NULL;
	struct ir_val dummy = {0};
	check(ir_expr(arena, a->u.loop.incr, ir, &incr, &dummy));

	struct ir_op *postcond = NULL;
	struct ir_val postcond_return = {0};
	check(ir_expr(arena,
	              a->u.loop.postcond,
	              ir,
	              &postcond,
	              &postcond_return));

	struct ir_op *postcond_jumper = NULL;
	if (a->u.loop.postcond->node_type != NODE_EXPRESSION_NULL) {
		assert(postcond_return.subtype != IR_VAL_NONE);
		check(ir_alloc_op(arena, &postcond_jumper));
		postcond_jumper->opcode = IR_OP_JUMP_IF_ZERO;
		ir_val_copy(&postcond_return, &postcond_jumper->args[0]);
		ir_val_copy(&go_end, &postcond_jumper->args[1]);
	}

	struct ir_op *jump_back_to_start = NULL;
	check(ir_alloc_op(arena, &jump_back_to_start));
	jump_back_to_start->opcode = IR_OP_JUMP;
	ir_val_copy(&go_start, &jump_back_to_start->args[0]);

	struct ir_op *end_label = NULL;
	check(ir_alloc_op(arena, &end_label));
	end_label->opcode = IR_OP_LABEL;
	ir_val_copy(&go_end, &end_label->args[0]);

	struct ir_op *collect[] = {
		start_label,
		precond,
		precond_jumper,
		body,
		continue_label,
		incr,
		postcond,
		postcond_jumper,
		jump_back_to_start,
		end_label,
	};
	for (size_t i = 0; i < ARRAY_SIZE(collect); ++i) {
		*dst = ir_op_list_concat(*dst, collect[i]);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_loop_control_op(Arena *arena, const struct ast *a, struct ir_op **dst)
{
	assert(a->node_type == NODE_BREAK || a->node_type == NODE_CONTINUE);

	check(ir_alloc_op(arena, dst));
	assert(*dst != NULL);
	(**dst).opcode = IR_OP_JUMP;
	(**dst).args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	(**dst).args[0].num = a->u.num;

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_goto(Arena *arena, const struct ast *a, struct ir_op **dst)
{
	assert(a->node_type == NODE_GOTO);

	check(ir_alloc_op(arena, dst));
	assert(*dst != NULL);
	(**dst).opcode = IR_OP_JUMP;
	(**dst).args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	(**dst).args[0].num = a->u.goto_.target_unique;

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_label(Arena *arena, long long int label_unique, struct ir_op **dst)
{
	check(ir_alloc_op(arena, dst));
	assert(*dst != NULL);
	(**dst).opcode = IR_OP_LABEL;
	(**dst).args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	(**dst).args[0].num = label_unique;
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_switch(Arena *arena,
          const struct ast *a,
          struct intermediate *ir,
          struct ir_op **dst)
{
	assert(a->node_type == NODE_SWITCH);

	struct ir_op *control = NULL;
	struct ir_val control_return = {0};
	check(ir_expr(arena,
	              a->u.switch_.control,
	              ir,
	              &control,
	              &control_return));

	struct ir_op *case_jumpers = NULL;
	for (struct flat *f = a->u.switch_.label_cases; f != NULL; f = f->cdr) {
		assert(f->car->node_type == NODE_CASE);

		struct ir_op *case_expr = NULL;
		struct ir_val case_return = {0};
		check(ir_expr(arena,
		              f->car->u.case_.constant,
		              ir,
		              &case_expr,
		              &case_return));

		struct ir_op *case_cmp = NULL;
		check(ir_alloc_op(arena, &case_cmp));
		case_cmp->opcode = IR_OP_COMPARE_EQUAL;
		ir_val_copy(&control_return, &case_cmp->args[0]);
		ir_val_copy(&case_return, &case_cmp->args[1]);
		check(ir_val_tmpvar_gen(
			arena,
			ir,
			&(struct ctype){
				.t = CTYPE_INT, /* effectively cast to bool */
			},
			&case_cmp->args[2]));

		struct ir_op *jumper = NULL;
		check(ir_alloc_op(arena, &jumper));
		jumper->opcode = IR_OP_JUMP_IF_NOT_ZERO;
		ir_val_copy(&case_cmp->args[2], &jumper->args[0]);
		jumper->args[1].subtype = IR_VAL_JUMP_TARGET_LABEL;
		jumper->args[1].num = f->car->u.case_.unique;

		case_jumpers = ir_op_list_concat(
			ir_op_list_concat(case_expr, case_cmp),
			ir_op_list_concat(jumper, case_jumpers));
	}

	struct ir_op *default_jumper = NULL;
	if (a->u.switch_.label_default >= 0) {
		check(ir_alloc_op(arena, &default_jumper));
		default_jumper->opcode = IR_OP_JUMP;
		default_jumper->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
		default_jumper->args[0].num = a->u.switch_.label_default;
	}

	struct ir_op *body = NULL;
	check(ir_block(arena, a->u.switch_.body, ir, &body));

	struct ir_op *end_jumper = NULL;
	check(ir_alloc_op(arena, &end_jumper));
	end_jumper->opcode = IR_OP_JUMP;
	end_jumper->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	end_jumper->args[0].num = a->u.switch_.label_end;

	struct ir_op *end_label = NULL;
	check(ir_alloc_op(arena, &end_label));
	end_label->opcode = IR_OP_LABEL;
	ir_val_copy(&end_jumper->args[0], &end_label->args[0]);

	struct ir_op *collect[] = {
		control,
		case_jumpers,
		default_jumper,
		end_jumper,
		body,
		end_label,
	};
	for (size_t i = 0; i < ARRAY_SIZE(collect); ++i) {
		*dst = ir_op_list_concat(*dst, collect[i]);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_assignment(Arena *arena,
              const struct ast *a,
              struct intermediate *ir,
              struct ir_op **dst,
              struct ir_val *return_value)
{
	struct ir_op *lvalue_addr_for_store = NULL;
	struct ir_val lvalue_addr_for_store_return = {0};
	if (ir_assignment_lvalue_suitable_for_store(a) != NULL) {
		/* Compute referent of LHS lvalue */
		const struct ast *address_expr =
			ir_assignment_lvalue_suitable_for_store(a);
		check(ir_expr(arena,
		              address_expr,
		              ir,
		              &lvalue_addr_for_store, /* may remain NULL */
		              &lvalue_addr_for_store_return));
	}

	struct ir_op *rhs_ops = NULL;
	struct ir_val rhs_return = {0};
	check(ir_expr(arena, a->u.op_binary.rhs, ir, &rhs_ops, &rhs_return));
	assert(rhs_return.subtype != IR_VAL_NONE);

	struct ir_op *assigner = NULL;
	check(ir_alloc_op(arena, &assigner));
	ir_val_copy(&rhs_return, &assigner->args[0]);

	if (ir_assignment_lvalue_suitable_for_store(a) != NULL) {
		assigner->opcode = IR_OP_STORE;
		ir_val_copy(&lvalue_addr_for_store_return, &assigner->args[1]);
	} else {
		assigner->opcode = IR_OP_COPY;
		check(ir_val_from_ast_variable_like(arena,
		                                    a,
		                                    &assigner->args[1]));
	}

	assert(return_value->subtype == IR_VAL_NONE);
	ir_val_copy(&assigner->args[1], return_value);

	struct ir_op *collect[] = {
		lvalue_addr_for_store,
		rhs_ops,
		assigner,
	};
	for (size_t i = 0; i < ARRAY_SIZE(collect); ++i) {
		*dst = ir_op_list_concat(*dst, collect[i]);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_incr_decr(Arena *arena,
             const struct ast *a,
             struct intermediate *ir,
             struct ir_op **dst,
             struct ir_val *return_value)
{
	struct ir_op *lvalue_addr_for_store = NULL;
	struct ir_val lvalue_addr_for_store_return = {0};

	struct ir_op *load_working_copy = NULL;
	struct ir_val load_working_copy_return = {0};

	if (ir_assignment_lvalue_suitable_for_store(a) != NULL) {
		const struct ast *address_expr =
			ir_assignment_lvalue_suitable_for_store(a);
		check(ir_expr(arena,
		              address_expr,
		              ir,
		              &lvalue_addr_for_store, /* may remain NULL */
		              &lvalue_addr_for_store_return));

		check(ir_alloc_op(arena, &load_working_copy));
		load_working_copy->opcode = IR_OP_LOAD;
		ir_val_copy(&lvalue_addr_for_store_return,
		            &load_working_copy->args[0]);
		assert(ctype_is_pointer(&address_expr->expr_type));
		check(ir_val_tmpvar_gen(arena,
		                        ir,
		                        address_expr->expr_type.referent,
		                        &load_working_copy->args[1]));
		ir_val_copy(&load_working_copy->args[1],
		            &load_working_copy_return);
	}

	struct ir_op *incr = NULL;
	check(ir_alloc_op(arena, &incr));

	switch (a->node_type) {
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
		incr->opcode = IR_OP_UNARY_DECREMENT;
		break;
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		incr->opcode = IR_OP_UNARY_INCREMENT;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *store_updated_value = NULL;

	if (ir_assignment_lvalue_suitable_for_store(a) != NULL) {
		ir_val_copy(&load_working_copy_return, &incr->args[0]);

		check(ir_alloc_op(arena, &store_updated_value));
		store_updated_value->opcode = IR_OP_STORE;
		ir_val_copy(&load_working_copy_return,
		            &store_updated_value->args[0]);
		ir_val_copy(&lvalue_addr_for_store_return,
		            &store_updated_value->args[1]);
	} else {
		check(ir_val_from_ast_variable_like(arena, a, &incr->args[0]));
	}
	ir_val_copy(&incr->args[0], &incr->args[1]);

	struct ir_op *stash_value_before_changes = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		check(ir_alloc_op(arena, &stash_value_before_changes));
		if (ir_assignment_lvalue_suitable_for_store(a) != NULL) {
			stash_value_before_changes->opcode = IR_OP_LOAD;
			ir_val_copy(&lvalue_addr_for_store_return,
			            &stash_value_before_changes->args[0]);
		} else {
			stash_value_before_changes->opcode = IR_OP_COPY;
			check(ir_val_from_ast_variable_like(
				arena,
				a,
				&stash_value_before_changes->args[0]));
		}
		check(ir_val_tmpvar_gen(arena,
		                        ir,
		                        &a->expr_type,
		                        &stash_value_before_changes->args[1]));
		ir_val_copy(&stash_value_before_changes->args[1], return_value);
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
		assert(return_value->subtype == IR_VAL_NONE);
		ir_val_copy(&incr->args[1], return_value);
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *collect[] = {
		lvalue_addr_for_store,
		stash_value_before_changes,
		load_working_copy,
		incr,
		store_updated_value,
	};
	for (size_t i = 0; i < ARRAY_SIZE(collect); ++i) {
		*dst = ir_op_list_concat(*dst, collect[i]);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_unary_op(Arena *arena,
            const struct ast *a,
            struct intermediate *ir,
            struct ir_op **dst,
            struct ir_val *return_value)
{
	struct ir_op *unary = NULL;
	check(ir_alloc_op(arena, &unary));

	struct ast *ast_inner = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		unary->opcode = IR_OP_UNARY_COMPLEMENT;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
		unary->opcode = IR_OP_UNARY_NEGATE;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_NOT:
		unary->opcode = IR_OP_UNARY_NOT;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		unary->opcode = IR_OP_LOAD;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		unary->opcode = IR_OP_GET_ADDRESS;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_CAST:
		if (ctype_is_equal(&a->u.cast.expr->expr_type,
		                   &a->u.cast.to_type)) {
			/* early return if inner expr type makes cast no-op */
			return ir_expr(arena,
			               a->u.cast.expr,
			               ir,
			               dst,
			               return_value);
		} else if (ctype_is_floating_point(
				   &a->u.cast.expr->expr_type) !=
		           ctype_is_floating_point(&a->u.cast.to_type)) {
			const bool src_fp = ctype_is_floating_point(
				&a->u.cast.expr->expr_type);
			const bool src_signed =
				ctype_is_signed(&a->u.cast.expr->expr_type);
			const bool dst_fp =
				ctype_is_floating_point(&a->u.cast.to_type);
			const bool dst_signed =
				ctype_is_signed(&a->u.cast.to_type);
			if (src_fp && dst_signed) {
				unary->opcode = IR_OP_CTYPE_DOUBLE_TO_INT;
			} else if (src_fp && !dst_signed) {
				unary->opcode = IR_OP_CTYPE_DOUBLE_TO_UINT;
			} else if (src_signed && dst_fp) {
				unary->opcode = IR_OP_CTYPE_INT_TO_DOUBLE;
			} else if (!src_signed && dst_fp) {
				unary->opcode = IR_OP_CTYPE_UINT_TO_DOUBLE;
			} else {
				assert(0); /* mistake in truth table above */
			}
		} else if (ctype_to_size_bytes(&a->u.cast.expr->expr_type) ==
		           ctype_to_size_bytes(&a->u.cast.to_type)) {
			unary->opcode = IR_OP_COPY;
		} else if (ctype_to_size_bytes(&a->u.cast.expr->expr_type) >
		           ctype_to_size_bytes(&a->u.cast.to_type)) {
			unary->opcode = IR_OP_CTYPE_TRUNCATE;
		} else if (ctype_is_signed(&a->u.cast.expr->expr_type)) {
			unary->opcode = IR_OP_CTYPE_SIGN_EXTEND;
		} else {
			unary->opcode = IR_OP_CTYPE_ZERO_EXTEND;
		}
		ast_inner = a->u.cast.expr;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *inner = NULL;
	struct ir_val inner_return = {0};
	check(ir_expr(arena, ast_inner, ir, &inner, &inner_return));
	assert(inner_return.subtype != IR_VAL_NONE);

	ir_val_copy(&inner_return, &unary->args[0]);
	check(ir_val_tmpvar_gen(arena, ir, &a->expr_type, &unary->args[1]));
	ir_val_copy(&unary->args[1], return_value);

	/*
	 * Emit IR in this order:
	 *
	 * 1) existing ops created by caller
	 * 2) results of recursive invocation of ir_expr()
	 * 3) the present UNARY_OP(opcode, ..., TMPVAR)
	 */
	*dst = ir_op_list_concat(inner, unary);
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_binary_op(Arena *arena,
             const struct ast *a,
             struct intermediate *ir,
             struct ir_op **dst,
             struct ir_val *return_value)
{
	struct ir_op *binary = NULL;
	check(ir_alloc_op(arena, &binary));

	switch (a->node_type) {
	case NODE_EXPRESSION_BINARY_ADD:
		binary->opcode = IR_OP_BINARY_ADD;
		break;
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		binary->opcode = IR_OP_BINARY_SUBTRACT;
		break;
	case NODE_EXPRESSION_BINARY_MULTIPLY:
		binary->opcode = IR_OP_BINARY_MULTIPLY;
		break;
	case NODE_EXPRESSION_BINARY_DIVIDE:
		binary->opcode = IR_OP_BINARY_DIVIDE;
		break;
	case NODE_EXPRESSION_BINARY_REMAINDER:
		binary->opcode = IR_OP_BINARY_REMAINDER;
		break;
	case NODE_EXPRESSION_BITWISE_AND:
		binary->opcode = IR_OP_BITWISE_AND;
		break;
	case NODE_EXPRESSION_BITWISE_OR:
		binary->opcode = IR_OP_BITWISE_OR;
		break;
	case NODE_EXPRESSION_BITWISE_XOR:
		binary->opcode = IR_OP_BITWISE_XOR;
		break;
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
		binary->opcode = IR_OP_BITWISE_SHIFT_LEFT;
		break;
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		binary->opcode = IR_OP_BITWISE_SHIFT_RIGHT;
		break;
	case NODE_EXPRESSION_COMPARE_EQUAL:
		binary->opcode = IR_OP_COMPARE_EQUAL;
		break;
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		binary->opcode = IR_OP_COMPARE_NOT_EQUAL;
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
		binary->opcode = IR_OP_COMPARE_LESS_THAN;
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
		binary->opcode = IR_OP_COMPARE_LESS_THAN_EQ;
		break;
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
		binary->opcode = IR_OP_COMPARE_MORE_THAN;
		break;
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		binary->opcode = IR_OP_COMPARE_MORE_THAN_EQ;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *left = NULL;
	struct ir_val left_return = {0};
	check(ir_expr(arena, a->u.op_binary.lhs, ir, &left, &left_return));
	assert(left_return.subtype != IR_VAL_NONE);

	struct ir_op *right = NULL;
	struct ir_val right_return = {0};
	check(ir_expr(arena, a->u.op_binary.rhs, ir, &right, &right_return));
	assert(right_return.subtype != IR_VAL_NONE);

	ir_val_copy(&left_return, &binary->args[0]);
	ir_val_copy(&right_return, &binary->args[1]);
	check(ir_val_tmpvar_gen(arena, ir, &a->expr_type, &binary->args[2]));

	assert(return_value->subtype == IR_VAL_NONE);
	ir_val_copy(&binary->args[2], return_value);

	/*
	 * Emit IR in this order:
	 *
	 * 1) existing ops created by caller
	 * 2) results of recursive invocations of ir_expr()
	 * 3) the present BINARY_OP(opcode, ..., TMPVAR)
	 */
	*dst = ir_op_list_concat(left, ir_op_list_concat(right, binary));
	return RESULT_OK;
}

/*
 * Note: unlike most ir_* helper functions, which populate a <return_value> out
 * parameter, this function influences control flow through IR_OP_JUMP_IF_ZERO
 * and IR_OP_JUMP_IF_NOT_ZERO.
 */
static WARN_UNUSED result_t
ir_logical_op_arm(Arena *arena,
                  const struct ast *a,
                  struct intermediate *ir,
                  struct ir_op **dst,
                  bool jz,
                  long long int jump_label)
{
	/*
	 * The <inner_return> value generated here is never used directly by
	 * the caller; it only affects control flow indirectly. Accordingly, we
	 * only use <inner_return> to set up the following IR_OP_JUMP_*, but we
	 * do not make a <return_value> visible to our caller.
	 */
	struct ir_op *inner = NULL;
	struct ir_val inner_return = {0};
	check(ir_expr(arena, a, ir, &inner, &inner_return));

	struct ir_op *jumper = NULL;
	check(ir_alloc_op(arena, &jumper));
	jumper->opcode = jz ? IR_OP_JUMP_IF_ZERO : IR_OP_JUMP_IF_NOT_ZERO;
	ir_val_copy(&inner_return, &jumper->args[0]);
	jumper->args[1].subtype = IR_VAL_JUMP_TARGET_LABEL;
	jumper->args[1].num = jump_label;

	*dst = ir_op_list_concat(inner, jumper);
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_logical_op(Arena *arena,
              const struct ast *a,
              struct intermediate *ir,
              struct ir_op **dst,
              struct ir_val *return_value,
              bool jz)
{
	assert(a->node_type == NODE_EXPRESSION_LOGICAL_AND ||
	       a->node_type == NODE_EXPRESSION_LOGICAL_OR);

	const long long int lf = ir->env.labels++;

	struct ir_op *left = NULL;
	check(ir_logical_op_arm(arena, a->u.op_binary.lhs, ir, &left, jz, lf));

	struct ir_op *right = NULL;
	check(ir_logical_op_arm(arena, a->u.op_binary.rhs, ir, &right, jz, lf));

	const long long int label_end = ir->env.labels++;

	assert(return_value->subtype == IR_VAL_NONE);
	check(ir_val_tmpvar_gen(arena, ir, &a->expr_type, return_value));

	struct ir_op *footer = NULL;
	check(ir_alloc_op(arena, &footer));
	struct ir_op *foot_pos = footer;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT;
	foot_pos->args[0].num = jz ? 1 : 0;
	foot_pos->args[1].subtype = return_value->subtype;
	foot_pos->args[1].num = return_value->num;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_JUMP;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_end;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_LABEL;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = lf;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT;
	foot_pos->args[0].num = jz ? 0 : 1;
	foot_pos->args[1].subtype = return_value->subtype;
	foot_pos->args[1].num = return_value->num;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_LABEL;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_end;

	*dst = ir_op_list_concat(left, ir_op_list_concat(right, footer));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_call_args(Arena *arena,
             const struct flat *args,
             struct intermediate *ir,
             struct ir_op **dst,
             struct ir_op *caller,
             size_t *pos)
{
	for (; args != NULL; args = args->cdr) {
		struct ir_val arg_value = {0};
		check(ir_expr(arena, args->car, ir, dst, &arg_value));
		assert(arg_value.subtype != IR_VAL_NONE);

		assert(*pos < FUNCTION_PARAMETER_LIMIT);
		ir_val_copy(&arg_value, &caller->args[*pos]);
		*pos = *pos + 1;

		if (*dst != NULL) {
			dst = &ir_op_list_back(*dst)->next;
		}
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_call(Arena *arena,
        const struct ast *a,
        struct intermediate *ir,
        struct ir_op **dst,
        struct ir_val *return_value)
{
	assert(a->node_type == NODE_EXPRESSION_FUNCTION_CALL);

	struct ir_op *caller = NULL;
	check(ir_alloc_op(arena, &caller));
	caller->opcode = IR_OP_CALL;
	caller->fun = a->u.call.identifier.name;

	struct ir_op *inner = NULL;
	size_t pos = 0;
	if (a->u.call.args != NULL) {
		struct flat *args = a->u.call.args;
		check(ir_call_args(arena, args, ir, &inner, caller, &pos));
	}

	check(ir_val_tmpvar_gen(arena, ir, &a->expr_type, &caller->args[pos]));

	assert(return_value->subtype == IR_VAL_NONE);
	ir_val_copy(&caller->args[pos], return_value);

	*dst = ir_op_list_concat(inner, caller);
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_expr(Arena *arena,
        const struct ast *a,
        struct intermediate *ir,
        struct ir_op **dst,
        struct ir_val *return_value)
{
	switch (a->node_type) {
	case NODE_CONSTANT:
		assert(return_value->subtype == IR_VAL_NONE);
		return_value->subtype = IR_VAL_CONSTANT;
		switch (a->expr_type.t) {
		case CTYPE_INT:
		case CTYPE_UNSIGNED_INT:
		case CTYPE_LONG:
		case CTYPE_UNSIGNED_LONG:
		case CTYPE_POINTER_TO:
			return_value->num = a->u.num;
			break;
		case CTYPE_DOUBLE:
			return_value->dnum = a->u.double_;
			break;
		}
		check(ctype_copy(arena, &a->expr_type, &return_value->c89type));
		assert(*dst == NULL); /* does not create new dst op */
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		assert(return_value->subtype == IR_VAL_NONE);
		check(ir_ret_op(arena, a, ir, dst));
		break;
	case NODE_BLOCK:
		check(ir_block(arena, a->u.block.statements, ir, dst));
		break;
	case NODE_DECLARATION:
		if (a->u.declare.init != NULL) {
			check(ir_decl_init(arena, a, ir, dst));
		}
		break;
	case NODE_IF_ELSE:
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(ir_if_else(arena, a, ir, dst, return_value));
		break;
	case NODE_LOOP:
		check(ir_loop(arena, a, ir, dst));
		break;
	case NODE_BREAK:
	case NODE_CONTINUE:
		check(ir_loop_control_op(arena, a, dst));
		break;
	case NODE_GOTO:
		check(ir_goto(arena, a, dst));
		break;
	case NODE_LABEL:
		check(ir_label(arena, a->u.label.unique, dst));
		break;
	case NODE_SWITCH:
		check(ir_switch(arena, a, ir, dst));
		break;
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
		check(ir_label(arena, a->u.case_.unique, dst));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		assert(return_value->subtype == IR_VAL_NONE);
		check(ir_val_from_ast_variable_like(arena, a, return_value));
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		check(ir_assignment(arena, a, ir, dst, return_value));
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		check(ir_incr_decr(arena, a, ir, dst, return_value));
		break;
	case NODE_EXPRESSION_NULL:
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_CAST:
		check(ir_unary_op(arena, a, ir, dst, return_value));
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		if ((a->node_type == NODE_EXPRESSION_UNARY_DEREFERENCE &&
		     ir_unpack_parens(a->u.op_unary.operand)->node_type ==
		             NODE_EXPRESSION_UNARY_ADDRESS_OF) ||
		    (a->node_type == NODE_EXPRESSION_UNARY_ADDRESS_OF &&
		     ir_unpack_parens(a->u.op_unary.operand)->node_type ==
		             NODE_EXPRESSION_UNARY_DEREFERENCE)) {
			/* treat *& and &* as no-op */
			struct ast *grandchild =
				ir_unpack_parens(a->u.op_unary.operand)
					->u.op_unary.operand;
			check(ir_expr(arena,
			              grandchild,
			              ir,
			              dst,
			              return_value));
		} else {
			check(ir_unary_op(arena, a, ir, dst, return_value));
		}
		break;
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(ir_expr(arena,
		              a->u.op_unary.operand,
		              ir,
		              dst,
		              return_value));
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
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		check(ir_binary_op(arena, a, ir, dst, return_value));
		break;
	case NODE_EXPRESSION_LOGICAL_AND:
		check(ir_logical_op(arena, a, ir, dst, return_value, true));
		break;
	case NODE_EXPRESSION_LOGICAL_OR:
		check(ir_logical_op(arena, a, ir, dst, return_value, false));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(ir_call(arena, a, ir, dst, return_value));
		break;
	default:
		return make_result(ERR_IR_EXPECT_AST_NODE_EXPRESSION,
		                   (int)a->node_type);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_func(Arena *arena,
        const struct ast *a,
        struct intermediate *ir,
        struct ir_function **dst)
{
	assert(a->node_type == NODE_FUNCTION);

	assert(dst != NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_IR_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	struct ir_function *f = *dst;
	f->identifier = a->u.function.identifier.name;

	size_t i = 0;
	FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
		check(ir_val_tmpvar(arena,
		                    cur->symbol.unique,
		                    &cur->parameter_type,
		                    &f->params[i]));
		++i;
	}

	assert(a->u.function.block != NULL);
	assert(a->u.function.block->node_type == NODE_BLOCK);
	check(ir_block(arena,
	               a->u.function.block->u.block.statements,
	               ir,
	               &f->ops));

	/*
	 * If necessary, add a final, often-unreachable `return 0` instruction
	 * at the end of every function, to make functions like:
	 *
	 *     int main(void)
	 *     {
	 *     }
	 *
	 * behave like:
	 *
	 *     int main(void)
	 *     {
	 *         return 0;
	 *     }
	 */
	struct ir_op *last_op = f->ops ? ir_op_list_back(f->ops) : NULL;
	if (last_op == NULL || last_op->opcode != IR_OP_RET) {
		struct ir_op **return_0 = last_op ? &last_op->next : &f->ops;
		check(ir_alloc_op(arena, return_0));
		assert(*return_0 != NULL);
		(**return_0).opcode = IR_OP_RET;
		(**return_0).args[0].subtype = IR_VAL_CONSTANT;
		(**return_0).args[0].num = 0;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_var(Arena *arena, struct symbol *s, struct ir_variable **dst)
{
	assert(dst != NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_IR_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	(**dst).identifier = s->name;
	check(ctype_copy(arena, &s->c89type, &(**dst).c89type));
	(**dst).linkage = ir_map_linkage(s->linkage.linkage);

	switch (s->linkage.initial) {
	case INITIAL_VALUE_NO_INITIALIZER:
		assert(0); /* logic error in caller */
		break;
	case INITIAL_VALUE_TENTATIVE:
		(**dst).initial.as_integer = 0;
		break;
	case INITIAL_VALUE_CONSTANT:
		(**dst).initial = s->linkage.as_constant;
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_program(Arena *arena,
           const struct ast *a,
           struct symbol_table *sym,
           struct intermediate *ir)
{
	assert(a->node_type == NODE_PROGRAM);
	assert(a->u.program.globals == NULL ||
	       a->u.program.globals->car->node_type == NODE_FUNCTION ||
	       a->u.program.globals->car->node_type == NODE_DECLARATION);

	struct ir_function **dst_fun = &ir->functions;
	struct flat *cursor = a->u.program.globals;
	for (; cursor != NULL; cursor = cursor->cdr) {
		switch (cursor->car->node_type) {
		case NODE_FUNCTION:
			if (cursor->car->u.function.block != NULL) {
				check(ir_func(arena, cursor->car, ir, dst_fun));
				assert(*dst_fun != NULL);
				dst_fun = &(**dst_fun).next;
			}
			break;
		case NODE_DECLARATION:
			/* skip variables, and use symbol_table instead */
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
	}

	/* O(n^2) caused by O(n) search of ir->functions for each symbol */
	for (struct symbol *s = sym->functions; s != NULL; s = s->next) {
		assert(s->stype == SYMBOL_FUNCTION_DECLARATION ||
		       s->stype == SYMBOL_FUNCTION_DEFINITION);
		struct ir_function *f = ir->functions;
		for (; f != NULL; f = f->next) {
			if (s->name.sz == f->identifier.sz &&
			    0 == strncmp(s->name.data,
			                 f->identifier.data,
			                 s->name.sz)) {
				f->linkage = ir_map_linkage(s->linkage.linkage);
				break;
			}
		}
	}

	struct ir_variable **dst_var = &ir->variables;
	for (struct symbol *s = sym->variables; s != NULL; s = s->next) {
		assert(s->stype == SYMBOL_VARIABLE);
		if (s->linkage.initial == INITIAL_VALUE_NO_INITIALIZER) {
			continue;
		}
		check(ir_var(arena, s, dst_var));
		assert(*dst_var != NULL);
		dst_var = &(**dst_var).next;
	}

	return RESULT_OK;
}

result_t
ir_init(Arena *arena,
        const struct ast *a,
        long long int base_id,
        long long int base_label,
        struct symbol_table *sym,
        struct intermediate **ir)
{
	*ir = arena_alloc(arena, sizeof(**ir));
	check_if(*ir == NULL, ERR_IR_ALLOC);
	memset(*ir, 0, sizeof(**ir));
	(**ir).env.generator = base_id;
	(**ir).env.labels = base_label;
	check(ir_program(arena, a, sym, *ir));
	return RESULT_OK;
}

#define TO_STR_AND_N_ARGS(opcode, n_args) {#opcode, n_args},
static const struct {
	const char *name;
	size_t required_args;
} OPCODE_NAMES[] = {FOREACH_IR_OPCODE(TO_STR_AND_N_ARGS)};
#undef TO_STR_AND_N_ARGS

static void
ir_debug_print_one(const struct ir_op *op)
{
	debug("%s", OPCODE_NAMES[op->opcode].name);
	if (op->opcode == IR_OP_CALL) {
		debug("  FUNCTION %.*s", (int)op->fun.sz, op->fun.data);
	}

	const size_t required_args = OPCODE_NAMES[op->opcode].required_args;
	for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
		switch (op->args[i].subtype) {
		case IR_VAL_NONE:
			assert(i >= required_args &&
			       "op lacks required operand");
			continue;
		case IR_VAL_CONSTANT:
			if (ctype_is_floating_point(&op->args[i].c89type)) {
				debug("  CONSTANT %f", op->args[i].dnum);
			} else {
				debug("  CONSTANT %lld",
				      (long long)op->args[i].num);
			}
			break;
		case IR_VAL_TEMPORARY_VARIABLE:
			debug("  VAR tmp.%lld", (long long)op->args[i].num);
			break;
		case IR_VAL_JUMP_TARGET_LABEL:
			debug("  LABEL label_%lld", (long long)op->args[i].num);
			break;
		case IR_VAL_VARIABLE_DATA:
			debug("  DATA %.*s",
			      (int)op->args[i].varname.sz,
			      op->args[i].varname.data);
			break;
		}

		char tmp[128] = {0};
		debug("    TYPE %s",
		      ctype_to_str(&op->args[i].c89type, tmp, sizeof(tmp)));
	}
}

static void
ir_debug_print_list(const struct ir_op *cursor)
{
	while (cursor != NULL) {
		ir_debug_print_one(cursor);
		cursor = cursor->next;
	}
}

void
ir_debug_print(const struct intermediate *ir)
{
	debug("PROGRAM");

	for (struct ir_variable *v = ir->variables; v != NULL; v = v->next) {
		const struct string_view *vname = &v->identifier;
		debug("VARIABLE %.*s", (int)vname->sz, vname->data);

		char tmp[128] = {0};
		debug("  VARIABLE TYPE %s",
		      ctype_to_str(&v->c89type, tmp, sizeof(tmp)));

		switch (v->linkage) {
		case IR_LINKAGE_INTERNAL:
			debug("  VARIABLE LINKAGE INTERNAL");
			break;
		case IR_LINKAGE_EXTERNAL:
			debug("  VARIABLE LINKAGE EXTERNAL");
			break;
		}
		if (ctype_is_floating_point(&v->c89type)) {
			debug("  VARIABLE INIT %f", v->initial.as_double);
		} else {
			debug("  VARIABLE INIT %lld",
			      (long long)v->initial.as_integer);
		}
	}

	for (struct ir_function *f = ir->functions; f != NULL; f = f->next) {
		const struct string_view *fname = &f->identifier;
		debug("FUNCTION %.*s", (int)fname->sz, fname->data);
		switch (f->linkage) {
		case IR_LINKAGE_INTERNAL:
			debug("  FUNCTION LINKAGE INTERNAL");
			break;
		case IR_LINKAGE_EXTERNAL:
			debug("  FUNCTION LINKAGE EXTERNAL");
			break;
		}
		ir_debug_print_list(f->ops);
	}
}
