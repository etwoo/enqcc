#include "passes/ir.h"

#include "passes.h"
#include "passes/parse.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
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
	assert(first->next == NULL);
	first->next = second;
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
	case NODE_EXPRESSION_UNARY_NEGATE:
		src->opcode = IR_OP_UNARY_NEGATE;
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		src->opcode = IR_OP_UNARY_COMPLEMENT;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *inner __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
	check(ir_expression(a->u.op_unary.operand, &src->args[0], &inner, env));

	src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
	src->args[1].num = env->generator++;

	if (src->args[0].subtype == IR_VAL_CONSTANT_INT) {
		/*
		 * Reached a terminal constant. Emit IR in this order:
		 *
		 * 1) existing ops created by caller
		 * 2) the present UNARY_OP(opcode, CONST(...), TMP)
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
		 * 3) the present UNARY_OP(opcode, ..., TMP)
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
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *left __attribute__((cleanup(ir_op_list_cleanup))) = NULL;
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
		 *   BINARY_OP(opcode, CONST(...), CONST(...), TMP)
		 */
		*dst = src;
	} else if (src->args[0].subtype == IR_VAL_CONSTANT_INT) {
		assert(left == NULL);
		/*
		 * Related testcases from writing-a-c-compiler-tests repo:
		 *
		 * - ./tests/chapter_3/valid/parens.c
		 * - ./tests/chapter_3/valid/precedence.c
		 * - ./tests/chapter_3/valid/sub_neg.c
		 * - ./tests/chapter_3/valid/sub.c
		 */
		src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
		ir_op_list_concat(right, src);
		*dst = right;
	} else if (src->args[1].subtype == IR_VAL_CONSTANT_INT) {
		assert(right == NULL);
		/*
		 * Related testcases from writing-a-c-compiler-tests repo:
		 * - ./tests/chapter_3/valid/associativity.c
		 * - ./tests/chapter_3/valid/associativity_2.c
		 * - ./tests/chapter_3/valid/associativity_3.c
		 * - ./tests/chapter_3/valid/associativity_and_precedence.c
		 * - ./tests/chapter_3/valid/div_neg.c
		 * - ./tests/chapter_3/valid/unop_add.c
		 */
		src->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		ir_op_list_concat(left, src);
		*dst = left;
	} else {
		src->args[0].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[0].num = src->args[1].num - 1;
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
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
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
		check(ir_binary_op(a, dst, env));
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
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_COMPLEMENT:
		required_args = 1;
		debug("UNARY");
		switch (op->opcode) {
		case IR_OP_UNARY_NEGATE:
			debug("  NEGATE");
			break;
		case IR_OP_UNARY_COMPLEMENT:
			debug("  COMPLEMENT");
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
