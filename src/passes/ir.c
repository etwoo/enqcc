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

static void
ir_op_list_concat(struct ir_op *first, struct ir_op *second)
{
	assert(first != NULL);
	while (first->next != NULL) {
		first = first->next;
	}
	first->next = second;
}

static result_t ir_expr(Arena *arena,
                        const struct ast *a,
                        struct intermediate *ir,
                        struct ir_op **dst,
                        struct ir_val *return_value) WARN_UNUSED;

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

	memcpy(&assigner->args[0], &inner_return, sizeof(inner_return));
	assigner->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	assigner->args[1].num = a->u.declare.identifier.unique;

	ir_op_list_concat(inner, assigner);
	*dst = inner;
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

	memcpy(&unary->args[0], &inner_return, sizeof(inner_return));
	unary->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	unary->args[1].num = ir->env.generator++;
	assert(return_value->subtype == IR_VAL_NONE);
	memcpy(return_value, &unary->args[1], sizeof(*return_value));

	/*
	 * Emit IR in this order:
	 *
	 * 1) existing ops created by caller
	 * 2) results of recursive invocation of ir_expr()
	 * 3) the present UNARY_OP(opcode, ..., TMPVAR)
	 */
	ir_op_list_concat(inner, unary);
	*dst = inner;
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
	check(ir_expr(arena, a->u.op_binary.lhs, ir, &right, &right_return));
	assert(right_return.subtype != IR_VAL_NONE);

	memcpy(&binary->args[0], &left_return, sizeof(left_return));
	memcpy(&binary->args[1], &right_return, sizeof(right_return));
	binary->args[2].subtype = IR_VAL_TEMPORARY_VARIABLE;
	binary->args[2].num = ir->env.generator++;
	assert(return_value->subtype == IR_VAL_NONE);
	memcpy(return_value, &binary->args[2], sizeof(*return_value));

	/*
	 * Emit IR in this order:
	 *
	 * 1) existing ops created by caller
	 * 2) results of recursive invocations of ir_expr()
	 * 3) the present BINARY_OP(opcode, ..., TMPVAR)
	 */
	ir_op_list_concat(left, right);
	ir_op_list_concat(right, binary);
	*dst = left;
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
	 * do not make an <return_value> visible to our caller.
	 */
	struct ir_op *inner = NULL;
	struct ir_val inner_return = {0};
	check(ir_expr(arena, a, ir, &inner, &inner_return));
	assert(inner_return.subtype != IR_VAL_NONE);

	struct ir_op *jumper = NULL;
	check(ir_alloc_op(arena, &jumper));
	jumper->opcode = jz ? IR_OP_JUMP_IF_ZERO : IR_OP_JUMP_IF_NOT_ZERO;
	memcpy(&jumper->args[0], &inner_return, sizeof(inner_return));
	jumper->args[1].subtype = IR_VAL_JUMP_TARGET_LABEL;
	jumper->args[1].num = jump_label;

	ir_op_list_concat(inner, jumper);
	*dst = inner;
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

	ir_op_list_concat(left, right);
	ir_op_list_concat(right, footer);
	*dst = left;
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
		break;
	case NODE_DECLARATION:
		if (a->u.declare.init != NULL) {
			check(ir_decl_init(arena, a, ir, dst));
		}
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		assert(return_value->subtype == IR_VAL_NONE);
		return_value->subtype = IR_VAL_TEMPORARY_VARIABLE;
		return_value->num = a->u.var.unique;
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
	default:
		return make_result(ERR_IR_EXPECT_AST_NODE_EXPRESSION,
		                   (int)a->node_type);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_block(Arena *arena,
         const struct ast *a,
         struct intermediate *ir,
         struct ir_op **block_ops,
         struct ir_val *return_value)
{
	struct ir_op *head = NULL;
	struct ir_val block_return = {0};

	struct ir_op **dst = block_ops;
	while (a != NULL) {
		assert(a->node_type == NODE_BLOCK);
		memset(&block_return, 0, sizeof(block_return));

		check(ir_expr(arena, a->u.block.item, ir, dst, &block_return));

		if (head == NULL) {
			head = *dst;
		}

		if (*dst != NULL) {
			dst = &ir_op_list_back(*dst)->next;
		} else {
			/*
			 * Sanity-check typical reasons for lack of new ir_op:
			 *
			 * - null expression
			 * - declaration without an initialization expression
			 */
			struct ast *cur = a->u.block.item;
			assert(cur->node_type ==
			               NODE_FUNCTION_RETURN_STATEMENT ||
			       cur->node_type == NODE_EXPRESSION_NULL ||
			       (cur->node_type == NODE_DECLARATION &&
			        cur->u.declare.init == NULL));
		}

		a = a->u.block.next;
	}

	*block_ops = head;
	assert(return_value->subtype == IR_VAL_NONE);
	memcpy(return_value, &block_return, sizeof(*return_value));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_function(Arena *arena, const struct ast *a, struct intermediate *ir)
{
	struct ir_function *f = &ir->function;

	assert(a->node_type == NODE_FUNCTION);
	f->identifier = a->u.function.identifier.name;
	check(ir_block(arena, a->u.function.block, ir, &f->ops, &ir->eax_val));

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
	struct ir_op *return_val_or_0 = NULL;
	check(ir_alloc_op(arena, &return_val_or_0));
	return_val_or_0->opcode = IR_OP_RET;
	if (ir->eax_val.subtype == IR_VAL_NONE) {
		return_val_or_0->args[0].subtype = IR_VAL_CONSTANT_INT;
		return_val_or_0->args[0].num = 0;
	} else {
		memcpy(return_val_or_0, &ir->eax_val, sizeof(*return_val_or_0));
	}
	if (f->ops != NULL) {
		ir_op_list_concat(f->ops, return_val_or_0);
	} else {
		f->ops = return_val_or_0;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_program(Arena *arena, const struct ast *a, struct intermediate *ir)
{
	assert(a->node_type == NODE_PROGRAM);
	check(ir_function(arena, a->u.program.entrypoint_function, ir));
	return RESULT_OK;
}

result_t
ir_init(Arena *arena,
        const struct ast *a,
        struct intermediate **ir,
        struct symbol **sym)
{
	*ir = arena_alloc(arena, sizeof(**ir));
	check_if(*ir == NULL, ERR_IR_ALLOC);
	memset(*ir, 0, sizeof(**ir));
	(**ir).env.generator = *sym == NULL ? 0 : (**sym).unique;
	check(ir_program(arena, a, *ir));
	return RESULT_OK;
}

static void
ir_debug_print_one(const struct ir_op *op)
{
	size_t required_args = 0;
	switch (op->opcode) {
	case IR_OP_RET:
		debug("RETURN");
		break;
	case IR_OP_UNARY_COMPLEMENT:
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_NOT:
	case IR_OP_JUMP:
	case IR_OP_LABEL:
		required_args = 1;
		debug("UNARY");
		switch (op->opcode) {
		case IR_OP_UNARY_COMPLEMENT:
			debug("  COMPLEMENT");
			break;
		case IR_OP_UNARY_NEGATE:
			debug("  NEGATE");
			break;
		case IR_OP_UNARY_NOT:
			debug("  NOT");
			break;
		case IR_OP_JUMP:
			debug("  JUMP");
			break;
		case IR_OP_LABEL:
			debug("  MARK_LABEL");
			break;
		default:
			assert(0); /* logic error in caller */
		}
		break;
	case IR_OP_BINARY_ADD:
	case IR_OP_BINARY_SUBTRACT:
	case IR_OP_BINARY_MULTIPLY:
	case IR_OP_BINARY_DIVIDE:
	case IR_OP_BINARY_REMAINDER:
	case IR_OP_COMPARE_EQUAL:
	case IR_OP_COMPARE_NOT_EQUAL:
	case IR_OP_COMPARE_LESS_THAN:
	case IR_OP_COMPARE_LESS_THAN_EQ:
	case IR_OP_COMPARE_MORE_THAN:
	case IR_OP_COMPARE_MORE_THAN_EQ:
	case IR_OP_COPY:
	case IR_OP_JUMP_IF_ZERO:
	case IR_OP_JUMP_IF_NOT_ZERO:
		required_args = 2;
		debug("BINARY");
		switch (op->opcode) {
		case IR_OP_BINARY_ADD:
			debug("  ADD");
			break;
		case IR_OP_BINARY_SUBTRACT:
			debug("  SUBTRACT");
			break;
		case IR_OP_BINARY_MULTIPLY:
			debug("  MULTIPLY");
			break;
		case IR_OP_BINARY_DIVIDE:
			debug("  DIVIDE");
			break;
		case IR_OP_BINARY_REMAINDER:
			debug("  REMAINDER");
			break;
		case IR_OP_COMPARE_EQUAL:
			debug("  COMPARE_EQUAL");
			break;
		case IR_OP_COMPARE_NOT_EQUAL:
			debug("  NOT_EQUAL");
			break;
		case IR_OP_COMPARE_LESS_THAN:
			debug("  LESS_THAN");
			break;
		case IR_OP_COMPARE_LESS_THAN_EQ:
			debug("  LESS_THAN_OR_EQUAL");
			break;
		case IR_OP_COMPARE_MORE_THAN:
			debug("  MORE_THAN");
			break;
		case IR_OP_COMPARE_MORE_THAN_EQ:
			debug("  MORE_THAN_OR_EQUAL");
			break;
		case IR_OP_COPY:
			debug("  COPY");
			break;
		case IR_OP_JUMP_IF_ZERO:
			debug("  JUMP_IF_ZERO");
			break;
		case IR_OP_JUMP_IF_NOT_ZERO:
			debug("  JUMP_IF_NOT_ZERO");
			break;
		default:
			assert(0); /* logic error in caller */
		}
		break;
	}

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

	const struct string_view *entrypoint = &ir->function.identifier;
	debug("FUNC %.*s", (int)entrypoint->sz, entrypoint->data);

	ir_debug_print_list(ir->function.ops);
}
