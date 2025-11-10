#include "passes/ir.h"

#include "passes.h"
#include "passes/parse.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#define ir_alloc(dst)                                                          \
	do {                                                                   \
		(dst) = malloc(sizeof(*(dst)));                                \
		check_if((dst) == NULL, ERR_IR_ALLOC);                         \
		memset(dst, 0, sizeof(*(dst)));                                \
	} while (0)

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

static void
ir_op_list_free(struct ir_op *cursor)
{
	while (cursor != NULL) {
		struct ir_op *tmp = cursor;
		cursor = cursor->next;
		free(tmp);
	}
}

static void
ir_op_list_cleanup(struct ir_op **pp)
{
	ir_op_list_free(*pp);
}

void
ir_free(struct intermediate *ir)
{
	ir_op_list_free(ir ? ir->function.ops : NULL);
	free(ir);
}

void
ir_cleanup(struct intermediate **ir)
{
	ir_free(*ir);
}

static WARN_UNUSED result_t
ir_constant(const struct ast *a, struct ir_val *peek, struct ir_op **dst)
{
	if (peek == NULL) {
		ir_alloc(*dst);
		(**dst).opcode = IR_OP_UNARY_IDENTITY;
		peek = &(**dst).args[0];
	}
	assert(peek->subtype == IR_VAL_NONE);
	peek->subtype = IR_VAL_CONSTANT_INT;
	peek->num = a->u.num;
	return RESULT_OK;
}

static result_t ir_expression(const struct ast *a,
                              struct ir_val *peek,
                              struct ir_op **dst,
                              struct ir_env *env) WARN_UNUSED;

static WARN_UNUSED result_t
ir_unary_op(const struct ast *a, struct ir_op **dst, struct ir_env *env)
{
	struct ir_op *src __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	ir_alloc(src);

	switch (a->node_type) {
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		src->opcode = IR_OP_UNARY_COMPLEMENT;
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
		src->opcode = IR_OP_UNARY_NEGATE;
		break;
	case NODE_EXPRESSION_UNARY_NOT:
		src->opcode = IR_OP_UNARY_NOT;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *inner __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc) // TODO remove?
	check(ir_expression(a->u.op_unary.operand, &src->args[0], &inner, env));

	src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	src->args[1].num = env->generator++;

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

	src = NULL;   /* release ownership to caller */
	inner = NULL; /* release ownership to caller */
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_binary_op(const struct ast *a, struct ir_op **dst, struct ir_env *env)
{
	struct ir_op *src __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	ir_alloc(src);

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

	struct ir_op *left __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc) // TODO remove?
	check(ir_expression(a->u.op_binary.lhs, &src->args[0], &left, env));

	struct ir_op *right __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	check(ir_expression(a->u.op_binary.rhs, &src->args[1], &right, env));

	src->args[2].subtype = IR_VAL_TEMPORARY_VARIABLE;
	src->args[2].num = env->generator++;

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

	src = NULL;   /* release ownership to caller */
	left = NULL;  /* release ownership to caller */
	right = NULL; /* release ownership to caller */
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_logical_op_arm(const struct ast *a,
                  struct ir_op **dst,
                  struct ir_env *env,
                  bool jump_if_zero,
                  long long int jump_label)
{
	struct ir_val peek = {0};
	struct ir_op *inner __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	check(ir_expression(a, &peek, &inner, env));

	struct ir_op *jumper __attribute__((cleanup(ir_op_list_cleanup))) =
		NULL;
	ir_alloc(jumper);
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

	inner = NULL;  /* release ownership to caller */
	jumper = NULL; /* release ownership to caller */

	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_logical_op(const struct ast *a,
              struct ir_op **dst,
              struct ir_env *env,
              bool jump_if_zero)
{
	const long long int label_false = env->labels++;

	struct ir_op *left __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	check(ir_logical_op_arm(a->u.op_binary.lhs,
	                        &left,
	                        env,
	                        jump_if_zero,
	                        label_false));

	struct ir_op *right __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	check(ir_logical_op_arm(a->u.op_binary.rhs,
	                        &right,
	                        env,
	                        jump_if_zero,
	                        label_false));

	const long long int label_end = env->labels++;
	const long long int result_id = env->generator++;

	struct ir_op *footer __attribute__((cleanup(ir_op_list_cleanup))) =
		NULL;
	ir_alloc(footer);
	struct ir_op *foot_pos = footer;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT_INT;
	foot_pos->args[0].num = jump_if_zero ? 1 : 0;
	foot_pos->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	foot_pos->args[1].num = result_id;

	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc) // TODO remove?
	ir_alloc(foot_pos->next);
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_JUMP;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_end;

	ir_alloc(foot_pos->next);
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_LABEL;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_false;

	ir_alloc(foot_pos->next);
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_COPY;
	foot_pos->args[0].subtype = IR_VAL_CONSTANT_INT;
	foot_pos->args[0].num = jump_if_zero ? 0 : 1;
	foot_pos->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	foot_pos->args[1].num = result_id;

	ir_alloc(foot_pos->next);
	foot_pos = foot_pos->next;

	foot_pos->opcode = IR_OP_LABEL;
	foot_pos->args[0].subtype = IR_VAL_JUMP_TARGET_LABEL;
	foot_pos->args[0].num = label_end;

	ir_op_list_concat(left, right);
	ir_op_list_concat(right, footer);
	*dst = left;

	left = NULL;   /* release ownership to caller */
	right = NULL;  /* release ownership to caller */
	footer = NULL; /* release ownership to caller */
	return RESULT_OK;
}

result_t
ir_expression(const struct ast *a,
              struct ir_val *peek,
              struct ir_op **dst,
              struct ir_env *env)
{
	switch (a->node_type) {
	case NODE_CONSTANT_INT:
		check(ir_constant(a, peek, dst));
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
		check(ir_unary_op(a, dst, env));
		break;
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(ir_expression(a->u.op_unary.operand, peek, dst, env));
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
		check(ir_binary_op(a, dst, env));
		break;
	case NODE_EXPRESSION_LOGICAL_AND:
		check(ir_logical_op(a, dst, env, true));
		break;
	case NODE_EXPRESSION_LOGICAL_OR:
		check(ir_logical_op(a, dst, env, false));
		break;
	default:
		return make_result(ERR_IR_EXPECT_AST_NODE_EXPRESSION,
		                   (int)a->node_type);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_function(const struct ast *a, struct ir_function *dst, struct ir_env *env)
{
	assert(a->node_type == NODE_FUNCTION);
	assert(a->u.function.identifier->node_type == NODE_IDENTIFIER);
	dst->identifier = a->u.function.identifier->u.str;

	assert(a->u.op_unary.operand != NULL);
	check(ir_expression(a->u.function.statement, NULL, &dst->ops, env));

	if (env->generator > 0) {
		struct ir_op *last_op = NULL;
		ir_alloc(last_op);
		last_op->opcode = IR_OP_UNARY_IDENTITY;
		last_op->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		last_op->args[0].num = env->generator - 1;
		ir_op_list_concat(dst->ops, last_op);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_program(const struct ast *a, struct intermediate *dst)
{
	assert(a->node_type == NODE_PROGRAM);
	check(ir_function(a->u.program.entrypoint_function,
	                  &dst->function,
	                  &dst->env));
	return RESULT_OK;
}

result_t
ir_init(const struct ast *a, struct intermediate **ir)
{
	ir_alloc(*ir);
	check(ir_program(a, *ir));
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

#undef ir_alloc
