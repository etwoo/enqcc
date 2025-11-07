#include "passes/ir.h"

#include "passes.h"
#include "passes/parse.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdlib.h>

#define ir_alloc(dst, init_type)                                               \
	do {                                                                   \
		(dst) = malloc(sizeof(*(dst)));                                \
		check_if((dst) == NULL, ERR_IR_ALLOC);                         \
		memset(dst, 0, sizeof(*(dst)));                                \
		(dst)->base.subtype = init_type;                               \
	} while (0)

static WARN_UNUSED result_t
ir_expression(const struct ast *a, struct ir_op **dst)
{
	switch (a->node_type) {
	case NODE_CONSTANT_INT: {
		ir_alloc(*dst, IR_OP_UNARY_IDENTITY);
		struct ir_val_constant *ir_constant = NULL;
		ir_alloc(ir_constant, IR_VAL_CONSTANT_INT);
		ir_constant->num = a->u.num;
		(*dst)->args[0] = &ir_constant->base;
		break;
	}
	case NODE_EXPRESSION_UNARY_NEGATION:
	case NODE_EXPRESSION_UNARY_COMPLEMENT: {
		ir_alloc(*dst,
		         a->node_type == NODE_EXPRESSION_UNARY_NEGATION
		                 ? IR_OP_UNARY_NEGATE
		                 : IR_OP_UNARY_COMPLEMENT);
		check(ir_expression(a->u.op_unary.operand, &(**dst).next));
		// TODO: set destination of unary op to temporary variable
		break;
	}
	case NODE_EXPRESSION_UNARY_IDENTITY:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(ir_expression(a->u.op_unary.operand, dst));
		break;
	default:
		assert(0 && "unexpected non-expr within ast_statement");
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_statement(const struct ast *a, struct ir_op **dst)
{
	assert(a->u.op_unary.operand != NULL);
	check(ir_expression(a->u.op_unary.operand, dst));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_function(const struct ast *a, struct ir_function *dst)
{
	assert(a->node_type == NODE_FUNCTION);
	assert(a->u.function.identifier->node_type == NODE_IDENTIFIER);
	assert(dst->base.subtype == IR_FUNCTION);
	dst->identifier = a->u.function.identifier->u.str;
	check(ir_statement(a->u.function.statement, &dst->ops));
	return RESULT_OK;
}

static WARN_UNUSED result_t
ir_program(const struct ast *a, struct ir_program *dst)
{
	assert(a->node_type == NODE_PROGRAM);
	assert(dst->base.subtype == IR_PROGRAM);
	dst->function.base.subtype = IR_FUNCTION;
	check(ir_function(a->u.program.entrypoint_function, &dst->function));
	return RESULT_OK;
}

result_t
ir_init(const struct ast *a, struct intermediate **ir)
{
	struct ir_program *program = NULL;
	ir_alloc(program, IR_PROGRAM);
	*ir = &program->base;

	check(ir_program(a, program));
	return RESULT_OK;
}

void
ir_free(struct intermediate *ir)
{
	if (ir != NULL) {
		assert(ir->subtype == IR_PROGRAM);
		struct ir_program *program = (struct ir_program *)ir;

		struct ir_op *ops = program->function.ops;
		while (ops != NULL) {
			struct ir_op *tmp = ops;
			ops = ops->next;
			for (size_t i = 0; i < ARRAY_SIZE(tmp->args); ++i) {
				free(tmp->args[i]);
			}
			free(tmp);
		}

		free(ir);
	}
}

void
ir_cleanup(struct intermediate **ir)
{
	ir_free(*ir);
}

void
ir_debug_print(const struct intermediate *ir)
{
	if (ir == NULL) {
		return;
	}

	switch (ir->subtype) {
	case IR_PROGRAM: {
		debug("PROGRAM");
		const struct ir_program *p = (const struct ir_program *)ir;
		ir_debug_print(&p->function.base);
		break;
	}
	case IR_FUNCTION: {
		const struct ir_function *f = (const struct ir_function *)ir;
		const struct string_view *str = &f->identifier;
		debug("FUNCTION %.*s", (int)str->sz, str->data);
		ir_debug_print(&f->ops->base);
		break;
	}
	case IR_VAL_CONSTANT_INT: {
		const struct ir_val_constant *val =
			(const struct ir_val_constant *)ir;
		debug("CONSTANT %lld", val->num);
		break;
	}
	case IR_VAL_VARIABLE:
		assert(0 && "ir_debug_print + IR_VAL_VARIABLE: unimplemented");
		break;
	case IR_OP_UNARY_IDENTITY:
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_COMPLEMENT: {
		switch (ir->subtype) {
		case IR_OP_UNARY_IDENTITY:
			debug("UNARY IDENTITY");
			break;
		case IR_OP_UNARY_NEGATE:
			debug("UNARY NEGATION");
			break;
		case IR_OP_UNARY_COMPLEMENT:
			debug("UNARY COMPLEMENT");
			break;
		default:
			break;
		}
		const struct ir_op *ops = (const struct ir_op *)ir;
		for (size_t i = 0; i < ARRAY_SIZE(ops->args); ++i) {
			ir_debug_print(ops->args[i]);
		}
		ir_debug_print(&ops->next->base);
		break;
	}
	}
}
