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

#define ir_alloc(dst, init_type)                                               \
	do {                                                                   \
		(dst) = malloc(sizeof(*(dst)));                                \
		check_if((dst) == NULL, ERR_IR_ALLOC);                         \
		memset(dst, 0, sizeof(*(dst)));                                \
		(dst)->base.subtype = init_type;                               \
	} while (0)

static void
ir_alloc_guard(struct intermediate **pp)
{
	free(*pp);
}

static WARN_UNUSED result_t
ir_expression(const struct ast *a, struct ir_op **dst)
{
	switch (a->node_type) {
	case NODE_CONSTANT_INT: {
		struct ir_val_constant *ir_constant = NULL;
		ir_constant = malloc(sizeof(*ir_constant));
		check_if(ir_constant == NULL, ERR_IR_ALLOC);
		memset(ir_constant, 0, sizeof(*ir_constant));
		ir_constant->base.base.subtype = IR_VAL_CONSTANT_INT;
		ir_constant->num = a->u.num;
		*dst = &ir_constant->base;
		break;
	}
	case NODE_EXPRESSION_UNARY_NEGATION:
	case NODE_EXPRESSION_UNARY_COMPLEMENT: {
		struct ir_op *var_src = NULL;
		check(ir_expression(a->u.op_unary.operand, &var_src));
		struct intermediate *var_src_owner
			__attribute((cleanup(ir_alloc_guard))) = &var_src->base;

		struct ir_val_temporary_variable *var_dst = NULL;
		ir_alloc(var_dst, IR_VAL_TEMPORARY_VARIABLE);
		var_dst->unique_id = generate_unique_id_for_ir_tmp();
		struct intermediate *var_dst_owner
			__attribute((cleanup(ir_alloc_guard))) = &var_dst->base;

		ir_alloc(*dst, // NOLINT(clang-analyzer-unix.Malloc)
		         a->node_type == NODE_EXPRESSION_UNARY_NEGATION
		                 ? IR_OP_UNARY_NEGATE
		                 : IR_OP_UNARY_COMPLEMENT);
		(**dst).args[0] = var_src_owner;
		var_src_owner = NULL; /* release and transfer ownership */
		(**dst).args[1] = var_dst_owner;
		var_dst_owner = NULL; /* release and transfer ownership */
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
ir_debug_print(const struct intermediate *ir, size_t indent)
{
	if (ir == NULL) {
		return;
	}

	switch (ir->subtype) {
	case IR_PROGRAM: {
		debug("%*sPROGRAM", (int)indent, "");
		const struct ir_program *p = (const struct ir_program *)ir;
		ir_debug_print(&p->function.base, indent /* same indent as caller */);
		break;
	}
	case IR_FUNCTION: {
		const struct ir_function *f = (const struct ir_function *)ir;
		const struct string_view *str = &f->identifier;
		debug("%*sFUNC%.*s", (int)indent, "", (int)str->sz, str->data);
		ir_debug_print(&f->ops->base, indent /* same indent as caller */);
		break;
	}
	case IR_VAL_CONSTANT_INT: {
		const struct ir_val_constant *val =
			(const struct ir_val_constant *)ir;
		debug("%*sCONSTANT %lld", (int)indent, "", val->num);
		break;
	}
	case IR_VAL_TEMPORARY_VARIABLE: {
		const struct ir_val_temporary_variable *tmpvar =
			(const struct ir_val_temporary_variable *)ir;
		debug("%*sVARIABLE %lld", (int)indent, "", tmpvar->unique_id);
		break;
	}
	case IR_OP_UNARY_IDENTITY:
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_COMPLEMENT: {
		debug("%*sUNARY", (int)indent, "");
		switch (ir->subtype) {
		case IR_OP_UNARY_IDENTITY:
			debug("%*sIDENTITY", (int)indent + 2, "");
			break;
		case IR_OP_UNARY_NEGATE:
			debug("%*sNEGATION", (int)indent + 2, "");
			break;
		case IR_OP_UNARY_COMPLEMENT:
			debug("%*sCOMPLEMENT", (int)indent + 2, "");
			break;
		default:
			break;
		}
		const struct ir_op *ops = (const struct ir_op *)ir;
		for (size_t i = 0; i < ARRAY_SIZE(ops->args); ++i) {
			ir_debug_print(ops->args[i], indent + 2);
		}
		ir_debug_print(&ops->next->base, indent);
		break;
	}
	}
}
