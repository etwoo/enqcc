#include "passes/ir.h"

#include "passes.h"
#include "passes/parse.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdlib.h>

static long long int
generate_unique_id_for_ir_tmp(void)
{
	static long long int generator = 1;
	return generator++;
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
ir_op_cleanup(struct ir_op **pp)
{
	ir_free_op_list(*pp);
}

static WARN_UNUSED result_t
ir_expression(const struct ast *a, struct ir_val *peek, struct ir_op **dst)
{
	switch (a->node_type) {
	case NODE_CONSTANT_INT: {
		assert(peek != NULL); // TODO: convert to check_if()
		assert(peek->subtype == IR_VAL_NONE); // TODO: check_if()
		peek->subtype = IR_VAL_CONSTANT_INT;
		peek->num = a->u.num;
		break;
	}
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_UNARY_NEGATION:
	case NODE_EXPRESSION_UNARY_COMPLEMENT: {
		struct ir_op *src __attribute__((cleanup(ir_op_cleanup))) =
			malloc(sizeof(*src));
		check_if(src == NULL, ERR_IR_ALLOC);
		memset(src, 0, sizeof(*src));
		switch (a->node_type) {
		case NODE_EXPRESSION_UNARY_IDENTITY:
			src->opcode = IR_OP_UNARY_IDENTITY;
			break;
		case NODE_EXPRESSION_UNARY_NEGATION:
			src->opcode = IR_OP_UNARY_NEGATE;
			break;
		case NODE_EXPRESSION_UNARY_COMPLEMENT:
			src->opcode = IR_OP_UNARY_COMPLEMENT;
			break;
		default:
			assert(0);
			break;
		}
		src->args[1].subtype = IR_VAL_TEMPORARY_VARIABLE;
		src->args[1].num = generate_unique_id_for_ir_tmp();

		struct ir_op *inner_ops = NULL;
		// NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
		check(ir_expression(a->u.op_unary.operand,
		                    &src->args[0],
		                    &inner_ops));

		if (src->args[0].subtype == IR_VAL_CONSTANT_INT) {
			assert(src->next == NULL);
			src->next = inner_ops;
			assert(*dst == NULL);
			*dst = src;
			src = NULL; /* release ownership to caller */
		} else {
			assert(inner_ops != NULL); // TODO: can this happen?
			while (inner_ops->next != NULL) {
				inner_ops = inner_ops->next;
			}
			assert(inner_ops->next == NULL);
			inner_ops->next = src;
			assert(*dst == NULL);
			*dst = inner_ops;
			src = NULL; /* release ownership to caller */
		}
		break;
	}
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(ir_expression(a->u.op_unary.operand, peek, dst));
		break;
	default:
		// TODO: replace msg below with check_if() error
		info("unexpected non-expr within ast_statement: %u",
		     a->node_type);
		break;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_function(const struct ast *a, struct ir_function *dst)
{
	assert(a->node_type == NODE_FUNCTION);
	assert(a->u.function.identifier->node_type == NODE_IDENTIFIER);
	dst->identifier = a->u.function.identifier->u.str;
	assert(a->u.op_unary.operand != NULL);
	check(ir_expression(a->u.function.statement, NULL, &dst->ops));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_program(const struct ast *a, struct intermediate *dst)
{
	assert(a->node_type == NODE_PROGRAM);
	check(ir_function(a->u.program.entrypoint_function, &dst->function));
	return RESULT_OK;
}

result_t
ir_init(const struct ast *a, struct intermediate **ir)
{
	*ir = malloc(sizeof(**ir));
	check_if(*ir == NULL, ERR_IR_ALLOC);
	memset(*ir, 0, sizeof(**ir));
	check(ir_program(a, *ir));
	return RESULT_OK;
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
			break;
		case IR_VAL_CONSTANT_INT:
			debug("  CONSTANT(%lld)", op->args[i].num);
			break;
		case IR_VAL_TEMPORARY_VARIABLE:
			debug("  VARIABLE(tmp.%lld)", op->args[i].num);
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
