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
ir_append_to_list(struct ir_op *cursor, struct ir_op *node)
{
	assert(cursor != NULL);
	while (cursor->next != NULL) {
		cursor = cursor->next;
	}
	assert(cursor->next == NULL);
	cursor->next = node;
}

static void
ir_free_op_list(struct ir_op *cursor)
{
	while (cursor != NULL) {
		struct ir_op *tmp = cursor;
		cursor = cursor->next;
		free(tmp);
	}
}

static void
ir_cleanup_op_list(struct ir_op **pp)
{
	ir_free_op_list(*pp);
}

void
ir_free(struct intermediate *ir)
{
	ir_free_op_list(ir ? ir->function.ops : NULL);
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
	struct ir_op *src __attribute__((cleanup(ir_cleanup_op_list))) = NULL;
	ir_alloc(src);

	switch (a->node_type) {
	case NODE_EXPRESSION_UNARY_NEGATION:
		src->opcode = IR_OP_UNARY_NEGATE;
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		src->opcode = IR_OP_UNARY_COMPLEMENT;
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	struct ir_op *inner __attribute__((cleanup(ir_cleanup_op_list))) = NULL;
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
		assert(src->next == NULL);
		src->next = inner;
		assert(*dst == NULL);
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
		assert(inner != NULL);
		ir_append_to_list(inner, src);

		assert(*dst == NULL);
		*dst = inner;
	}
	src = NULL;   /* release ownership to caller */
	inner = NULL; /* release ownership to caller */

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
	case NODE_EXPRESSION_UNARY_NEGATION:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		check(ir_unary_op(a, dst, env));
		break;
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(ir_expression(a->u.op_unary.operand, peek, dst, env));
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
		ir_append_to_list(dst->ops, last_op);
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
	switch (op->opcode) {
	case IR_OP_UNARY_IDENTITY:
		debug("RETURN");
		break;
	case IR_OP_UNARY_NEGATE:
		debug("UNARY");
		debug("  NEGATION");
		break;
	case IR_OP_UNARY_COMPLEMENT:
		debug("UNARY");
		debug("  COMPLEMENT");
		break;
	}

	for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
		switch (op->args[i].subtype) {
		case IR_VAL_NONE:
			assert(i > 0 && op->opcode == IR_OP_UNARY_IDENTITY &&
			       "invalid op with unset operand");
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
