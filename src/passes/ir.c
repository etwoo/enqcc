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

static void
ir_op_list_concat(struct ir_op *first, struct ir_op *second)
{
	assert(first != NULL);
	while (first->next != NULL) {
		first = first->next;
	}
	first->next = second;
}

static long long int
ir_op_list_find_last_tmpvar_id(struct ir_op *p)
{
	long long int result = -1;

	assert(p != NULL);
	for (; p != NULL; p = p->next) {
		for (size_t i = 0; i < ARRAY_SIZE(p->args); ++i) {
			if (p->args[i].subtype == IR_VAL_TEMPORARY_VARIABLE) {
				result = p->args[i].num;
			}
		}
	}

	assert(result >= 0);
	return result;
}

static WARN_UNUSED result_t
ir_constant(Arena *arena,
            const struct ast *a,
            struct ir_val *peek,
            struct ir_op **dst)
{
	if (peek == NULL) {
		check(ir_alloc_op(arena, dst));
		assert(*dst != NULL);
		(**dst).opcode = IR_OP_UNARY_IDENTITY;
		peek = &(**dst).args[0];
	}
	assert(peek->subtype == IR_VAL_NONE);
	peek->subtype = IR_VAL_CONSTANT_INT;
	peek->num = a->u.num;
	return RESULT_OK;
}

static result_t ir_expression(Arena *arena,
                              const struct ast *a,
                              struct intermediate *ir,
                              struct ir_val *peek,
                              struct ir_op **dst) WARN_UNUSED;

static WARN_UNUSED result_t
ir_unary_op(Arena *arena,
            const struct ast *a,
            struct intermediate *ir,
            struct ir_op **dst)
{
	struct ir_op *src = NULL;
	check(ir_alloc_op(arena, &src));

	struct ast *ast_inner = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		src->opcode = IR_OP_UNARY_COMPLEMENT;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
		src->opcode = IR_OP_UNARY_NEGATE;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_NOT:
		src->opcode = IR_OP_UNARY_NOT;
		ast_inner = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		src->opcode = IR_OP_COPY;
		ast_inner = a->u.op_binary.rhs;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *inner = NULL;
	check(ir_expression(arena, ast_inner, ir, &src->args[0], &inner));

	src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	src->args[1].num = ir->env.generator++;

	if (src->args[0].subtype == IR_VAL_CONSTANT_INT) {
		/*
		 * Reached a terminal constant. Emit IR in this order:
		 *
		 * 1) existing ops created by caller
		 * 2) the present UNARY_OP(opcode, CONSTANT(...), TMPVAR)
		 * 3) results of recursive invocation of ir_expression()
		 */
		ir_op_list_concat(src, inner);
		*dst = src;
	} else {
		src->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[0].num = src->args[1].num - 1;
		/*
		 * Peeked value is not a constant. Emit IR in this order:
		 *
		 * 1) existing ops created by caller
		 * 2) results of recursive invocation of ir_expression()
		 * 3) the present UNARY_OP(opcode, ..., TMPVAR)
		 */
		ir_op_list_concat(inner, src);
		*dst = inner;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_binary_op(Arena *arena,
             const struct ast *a,
             struct intermediate *ir,
             struct ir_op **dst)
{
	struct ir_op *src = NULL;
	check(ir_alloc_op(arena, &src));

	switch (a->node_type) {
	case NODE_EXPRESSION_BINARY_ADD:
		src->opcode = IR_OP_BINARY_ADD;
		break;
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		src->opcode = IR_OP_BINARY_SUBTRACT;
		break;
	case NODE_EXPRESSION_BINARY_MULTIPLY:
		src->opcode = IR_OP_BINARY_MULTIPLY;
		break;
	case NODE_EXPRESSION_BINARY_DIVIDE:
		src->opcode = IR_OP_BINARY_DIVIDE;
		break;
	case NODE_EXPRESSION_BINARY_REMAINDER:
		src->opcode = IR_OP_BINARY_REMAINDER;
		break;
	case NODE_EXPRESSION_COMPARE_EQUAL:
		src->opcode = IR_OP_COMPARE_EQUAL;
		break;
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		src->opcode = IR_OP_COMPARE_NOT_EQUAL;
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
		src->opcode = IR_OP_COMPARE_LESS_THAN;
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
		src->opcode = IR_OP_COMPARE_LESS_THAN_EQ;
		break;
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
		src->opcode = IR_OP_COMPARE_MORE_THAN;
		break;
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		src->opcode = IR_OP_COMPARE_MORE_THAN_EQ;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *left = NULL;
	check(ir_expression(arena,
	                    a->u.op_binary.lhs,
	                    ir,
	                    &src->args[0],
	                    &left));

	struct ir_op *right = NULL;
	check(ir_expression(arena,
	                    a->u.op_binary.rhs,
	                    ir,
	                    &src->args[1],
	                    &right));

	src->args[2].subtype = IR_VAL_TEMPORARY_VARIABLE;
	src->args[2].num = ir->env.generator++;

	if (src->args[0].subtype == IR_VAL_CONSTANT_INT &&
	    src->args[1].subtype == IR_VAL_CONSTANT_INT) {
		assert(left == NULL);
		assert(right == NULL);
		/*
		 * Reached terminal constants. Emit IR of the form:
		 *
		 *   BINARY_OP(opcode, CONSTANT(...), CONSTANT(...), TMPVAR)
		 */
		*dst = src;
	} else if (src->args[0].subtype == IR_VAL_CONSTANT_INT) {
		assert(left == NULL);
		assert(src->args[1].subtype == IR_VAL_NONE);
		src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[1].num = src->args[2].num - 1;
		/*
		 * Related testcases from writing-a-c-compiler-tests repo:
		 *
		 * - ./tests/chapter_3/valid/parens.c
		 * - ./tests/chapter_3/valid/precedence.c
		 * - ./tests/chapter_3/valid/sub_neg.c
		 * - ./tests/chapter_3/valid/sub.c
		 */
		ir_op_list_concat(right, src);
		*dst = right;
	} else if (src->args[1].subtype == IR_VAL_CONSTANT_INT) {
		assert(right == NULL);
		assert(src->args[0].subtype == IR_VAL_NONE);
		src->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[0].num = src->args[2].num - 1;
		/*
		 * Related testcases from writing-a-c-compiler-tests repo:
		 * - ./tests/chapter_3/valid/associativity.c
		 * - ./tests/chapter_3/valid/associativity_2.c
		 * - ./tests/chapter_3/valid/associativity_3.c
		 * - ./tests/chapter_3/valid/associativity_and_precedence.c
		 * - ./tests/chapter_3/valid/div_neg.c
		 * - ./tests/chapter_3/valid/unop_add.c
		 */
		ir_op_list_concat(left, src);
		*dst = left;
	} else {
		assert(src->args[0].subtype == IR_VAL_NONE);
		src->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[0].num = ir_op_list_find_last_tmpvar_id(left);
		assert(src->args[1].subtype == IR_VAL_NONE);
		src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[1].num = ir_op_list_find_last_tmpvar_id(right);
		/*
		 * Neither peeked value is a constant. Emit IR in this order:
		 *
		 * 1) existing ops created by caller
		 * 2) results of recursive invocations of ir_expression()
		 * 3) the present BINARY_OP(opcode, ..., TMPVAR)
		 */
		ir_op_list_concat(left, right);
		ir_op_list_concat(right, src);
		*dst = left;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_logical_op_arm(Arena *arena,
                  const struct ast *a,
                  struct intermediate *ir,
                  struct ir_op **dst,
                  bool jump_if_zero,
                  long long int jump_label)
{
	struct ir_val peek = {0};
	struct ir_op *inner = NULL;
	check(ir_expression(arena, a, ir, &peek, &inner));

	struct ir_op *jumper = NULL;
	check(ir_alloc_op(arena, &jumper));
	jumper->opcode =
		jump_if_zero ? IR_OP_JUMP_IF_ZERO : IR_OP_JUMP_IF_NOT_ZERO;

	if (peek.subtype == IR_VAL_CONSTANT_INT) {
		assert(inner == NULL);
		jumper->args[0].subtype = IR_VAL_CONSTANT_INT;
		jumper->args[0].num = peek.num;
	} else {
		assert(inner != NULL);
		jumper->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		jumper->args[0].num = ir_op_list_find_last_tmpvar_id(inner);
	}

	jumper->args[1].subtype = IR_VAL_JUMP_TARGET_LABEL;
	jumper->args[1].num = jump_label;

	if (peek.subtype == IR_VAL_CONSTANT_INT) {
		*dst = jumper;
	} else {
		ir_op_list_concat(inner, jumper);
		*dst = inner;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_logical_op(Arena *arena,
              const struct ast *a,
              struct intermediate *ir,
              struct ir_op **dst,
              bool jump_if_zero)
{
	const long long int label_false = ir->env.labels++;

	struct ir_op *left = NULL;
	check(ir_logical_op_arm(arena,
	                        a->u.op_binary.lhs,
	                        ir,
	                        &left,
	                        jump_if_zero,
	                        label_false));

	struct ir_op *right = NULL;
	check(ir_logical_op_arm(arena,
	                        a->u.op_binary.rhs,
	                        ir,
	                        &right,
	                        jump_if_zero,
	                        label_false));

	const long long int label_end = ir->env.labels++;
	const long long int result_id = ir->env.generator++;

	struct ir_op *footer = NULL;
	check(ir_alloc_op(arena, &footer));
	struct ir_op *foot_pos = footer;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT_INT;
	foot_pos->args[0].num = jump_if_zero ? 1 : 0;
	foot_pos->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	foot_pos->args[1].num = result_id;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_JUMP;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_end;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_LABEL;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_false;

	check(ir_alloc_op(arena, &foot_pos->next));
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT_INT;
	foot_pos->args[0].num = jump_if_zero ? 0 : 1;
	foot_pos->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	foot_pos->args[1].num = result_id;

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
ir_expression(Arena *arena,
              const struct ast *a,
              struct intermediate *ir,
              struct ir_val *peek,
              struct ir_op **dst)
{
	switch (a->node_type) {
	case NODE_CONSTANT_INT:
		check(ir_constant(arena, a, peek, dst));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		check(ir_alloc_op(arena, dst));
		(**dst).opcode = IR_OP_UNARY_IDENTITY;
		(**dst).args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		(**dst).args[0].num = a->u.var.unique;
		break;
	case NODE_DECLARATION:
		if (a->u.declare.init != NULL) {
			check(ir_expression(arena,
			                    a->u.declare.init,
			                    ir,
			                    NULL,
			                    dst));
		}
		break;
	case NODE_EXPRESSION_NULL:
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
		check(ir_unary_op(arena, a, ir, dst));
		break;
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(ir_expression(arena,
		                    a->u.op_unary.operand,
		                    ir,
		                    peek,
		                    dst));
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
		check(ir_binary_op(arena, a, ir, dst));
		break;
	case NODE_EXPRESSION_LOGICAL_AND:
		check(ir_logical_op(arena, a, ir, dst, true));
		break;
	case NODE_EXPRESSION_LOGICAL_OR:
		check(ir_logical_op(arena, a, ir, dst, false));
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
         struct ir_op **dst)
{
	while (a != NULL) {
		assert(a->node_type == NODE_BLOCK);
		check(ir_expression(arena, a->u.block.item, ir, NULL, dst));
		a = a->u.block.next;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_function(Arena *arena, const struct ast *a, struct intermediate *ir)
{
	struct ir_function *f = &ir->function;

	assert(a->node_type == NODE_FUNCTION);
	f->identifier = a->u.function.identifier.name;

	assert(a->u.op_unary.operand != NULL);
	check(ir_block(arena, a->u.function.block, ir, &f->ops));

	if (ir->env.generator > 0) {
		struct ir_op *last_op = NULL;
		check(ir_alloc_op(arena, &last_op));
		last_op->opcode = IR_OP_UNARY_IDENTITY;
		last_op->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		last_op->args[0].num = ir->env.generator - 1;
		ir_op_list_concat(f->ops, last_op);
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
	case IR_OP_UNARY_IDENTITY:
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
