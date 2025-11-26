#include "passes/ir.h"

#include "passes.h"
#include "passes/parse.h"
#include "passes/symbol.h"
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

static void
ir_val_from_ast_variable_like(const struct ast *src, struct ir_val *dst)
{
	assert(src->node_type == NODE_DECLARATION ||
	       src->node_type == NODE_EXPRESSION_VARIABLE_ASSIGNMENT ||
	       src->node_type == NODE_EXPRESSION_VARIABLE_USAGE);

	const struct ast_symbol *sym = NULL;
	switch (src->node_type) {
	case NODE_DECLARATION:
		sym = &src->u.declare.identifier;
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		sym = &src->u.op_binary.lhs->u.var;
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
         const struct ast *a,
         struct intermediate *ir,
         struct ir_op **block_ops)
{
	struct ir_op *head = NULL;

	struct ir_op **dst = block_ops;
	for (; a != NULL; a = a->u.block.next) {
		assert(a->node_type == NODE_BLOCK);

		if (a->u.block.item == NULL) {
			continue;
		}

		if (a->u.block.item->node_type == NODE_FUNCTION) {
			/*
			 * For IR purposes, ignore function declarations that
			 * appear inside other blocks.
			 */
			continue;
		}

		struct ir_val dummy = {0};
		check(ir_expr(arena, a->u.block.item, ir, dst, &dummy));
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
	ir_val_from_ast_variable_like(a, &assigner->args[1]);

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
                   const struct ast *ast_clause,
                   struct intermediate *ir,
                   const struct ir_val *jump_operand,
                   const long long int jump_label,
                   const long long int assign_result_unique,
                   struct if_else_prep *out)
{
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
	check(ir_expr(arena, ast_clause, ir, &out->body, &body_return));

	if (assign_result_unique >= 0) {
		check(ir_alloc_op(arena, &out->assign_result));
		out->assign_result->opcode = IR_OP_COPY;
		ir_val_copy(&body_return, &out->assign_result->args[0]);
		out->assign_result->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
		out->assign_result->args[1].num = assign_result_unique;
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
	assert(a->node_type == NODE_IF_ELSE ||
	       a->node_type == NODE_EXPRESSION_TERNARY_CONDITIONAL);

	const bool has_else = (a->u.if_.else_clause != NULL);
	const long long int cond_jump_to = ir->env.labels++;
	const long long int end_jump_to = has_else ? ir->env.labels++ : -1;
	const long long int assign_result_unique =
		a->node_type == NODE_EXPRESSION_TERNARY_CONDITIONAL
			? ir->env.generator++
			: -1;

	struct ir_op *cond_ops = NULL;
	struct ir_val cond_return = {0};
	check(ir_expr(arena, a->u.if_.condition, ir, &cond_ops, &cond_return));

	struct if_else_prep then_p = {0};
	check(ir_if_else_prepare(arena,
	                         a->u.if_.then_clause,
	                         ir,
	                         &cond_return,
	                         cond_jump_to,
	                         assign_result_unique,
	                         &then_p));

	struct if_else_prep or_p = {0};
	if (has_else) {
		check(ir_if_else_prepare(arena,
		                         a->u.if_.else_clause,
		                         ir,
		                         NULL,
		                         end_jump_to,
		                         assign_result_unique,
		                         &or_p));
	}

	if (assign_result_unique >= 0) {
		assert(return_value->subtype == IR_VAL_NONE);
		return_value->subtype = IR_VAL_TEMPORARY_VARIABLE;
		return_value->num = assign_result_unique;
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

	struct ir_val dummy = {0};

	struct ir_op *body = NULL;
	memset(&dummy, 0, sizeof(dummy));
	check(ir_expr(arena, a->u.loop.body, ir, &body, &dummy));

	struct ir_op *continue_label = NULL;
	check(ir_alloc_op(arena, &continue_label));
	continue_label->opcode = IR_OP_LABEL;
	ir_val_copy(&go_continue, &continue_label->args[0]);

	struct ir_op *incr = NULL;
	memset(&dummy, 0, sizeof(dummy));
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
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		unary->opcode = IR_OP_COPY;
		ast_inner = a->u.op_binary.rhs;
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
	unary->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	if (a->node_type == NODE_EXPRESSION_VARIABLE_ASSIGNMENT) {
		assert(a->u.op_binary.lhs->node_type ==
		       NODE_EXPRESSION_VARIABLE_USAGE);
		ir_val_from_ast_variable_like(a, &unary->args[1]);
	} else {
		unary->args[1].num = ir->env.generator++;
	}
	assert(return_value->subtype == IR_VAL_NONE);
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
	binary->args[2].subtype = IR_VAL_TEMPORARY_VARIABLE;
	binary->args[2].num = ir->env.generator++;
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
	return_value->subtype = IR_VAL_TEMPORARY_VARIABLE;
	return_value->num = ir->env.generator++;

	struct ir_op *footer = NULL;
	check(ir_alloc_op(arena, &footer));
	struct ir_op *foot_pos = footer;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT_INT;
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
	foot_pos->args[0].subtype = IR_VAL_CONSTANT_INT;
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
             const struct ast *a,
             struct intermediate *ir,
             struct ir_op **dst,
             struct ir_op *caller,
             size_t *pos)
{
	assert(a->node_type == NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS);

	while (a != NULL && a->u.call_args.expr != NULL) {
		struct ir_val arg_value = {0};
		check(ir_expr(arena, a->u.call_args.expr, ir, dst, &arg_value));
		assert(arg_value.subtype != IR_VAL_NONE);

		assert(*pos < FUNCTION_PARAMETER_LIMIT);
		ir_val_copy(&arg_value, &caller->args[*pos]);
		*pos = *pos + 1;

		a = a->u.call_args.next;
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
	if (a->u.call.arguments != NULL) {
		struct ast *args = a->u.call.arguments;
		check(ir_call_args(arena, args, ir, &inner, caller, &pos));
	}

	caller->args[pos].subtype = IR_VAL_TEMPORARY_VARIABLE;
	caller->args[pos].num = ir->env.generator++;

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
	case NODE_CONSTANT_INT:
		assert(return_value->subtype == IR_VAL_NONE);
		return_value->subtype = IR_VAL_CONSTANT_INT;
		return_value->num = a->u.num;
		assert(*dst == NULL); /* does not create new dst op */
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		assert(return_value->subtype == IR_VAL_NONE);
		check(ir_ret_op(arena, a, ir, dst));
		break;
	case NODE_BLOCK:
		check(ir_block(arena, a, ir, dst));
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
	case NODE_EXPRESSION_VARIABLE_USAGE:
		assert(return_value->subtype == IR_VAL_NONE);
		ir_val_from_ast_variable_like(a, return_value);
		break;
	case NODE_EXPRESSION_NULL:
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
		check(ir_unary_op(arena, a, ir, dst, return_value));
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
		f->params[i].subtype = IR_VAL_TEMPORARY_VARIABLE;
		f->params[i].num = cur->unique;
		++i;
	}

	check(ir_block(arena, a->u.function.block, ir, &f->ops));

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
		(**return_0).args[0].subtype = IR_VAL_CONSTANT_INT;
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
	(**dst).linkage = ir_map_linkage(s->linkage.linkage);

	switch (s->linkage.initial) {
	case INITIAL_VALUE_NO_INITIALIZER:
		assert(0); /* logic error in caller */
		break;
	case INITIAL_VALUE_TENTATIVE:
		(**dst).u.initial_as_ll = 0;
		break;
	case INITIAL_VALUE_CONSTANT:
		(**dst).u.initial_as_ll = s->linkage.as_constant;
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
	       a->u.program.globals->node_type == NODE_FUNCTION ||
	       a->u.program.globals->node_type == NODE_DECLARATION);

	struct ir_function **dst_fun = &ir->functions;
	a = a->u.program.globals;
	while (a != NULL) {
		switch (a->node_type) {
		case NODE_FUNCTION:
			if (a->u.function.block != NULL) {
				check(ir_func(arena, a, ir, dst_fun));
				assert(*dst_fun != NULL);
				dst_fun = &(**dst_fun).next;
			}
			a = a->u.function.next;
			break;
		case NODE_DECLARATION:
			/* skip variables, and use symbol_table instead */
			a = a->u.declare.next;
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
			break;
		case IR_VAL_CONSTANT_INT:
			debug("  CONSTANT %lld", op->args[i].num);
			break;
		case IR_VAL_TEMPORARY_VARIABLE:
			debug("  VARIABLE tmp.%lld", op->args[i].num);
			break;
		case IR_VAL_JUMP_TARGET_LABEL:
			debug("  LABEL label_%lld", op->args[i].num);
			break;
		case IR_VAL_VARIABLE_DATA:
			debug("  DATA %.*s",
			      (int)op->args[i].varname.sz,
			      op->args[i].varname.data);
			break;
		}
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
		switch (v->linkage) {
		case IR_LINKAGE_INTERNAL:
			debug("  VARIABLE LINKAGE INTERNAL");
			break;
		case IR_LINKAGE_EXTERNAL:
			debug("  VARIABLE LINKAGE EXTERNAL");
			break;
		}
		debug("  VARIABLE INIT %lld", v->u.initial_as_ll);
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
