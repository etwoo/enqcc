#include "passes/codegen.h"

#include "passes.h"
#include "passes/parse.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdlib.h>

#define codegen_alloc(dst)                                                     \
	do {                                                                   \
		(dst) = malloc(sizeof(*(dst)));                                \
		check_if((dst) == NULL, ERR_CODEGEN_ALLOC);                    \
		memset(dst, 0, sizeof(*(dst)));                                \
	} while (0)

static WARN_UNUSED result_t
codegen_statement(struct ast_statement *a, struct asm_op **dst)
{
	assert(a->base.node_type == NODE_STATEMENT);
	assert(a->return_expression.base.node_type == NODE_EXPRESSION);
	assert(a->return_expression.constant.base.node_type ==
	       NODE_CONSTANT_INT);

	assert(*dst == NULL);
	codegen_alloc(*dst);
	(*dst)->base.statement_type = ASM_OP_MOV;
	(*dst)->args[0].operand_type = ASM_OPERAND_IMMEDIATE;
	(*dst)->args[0].num = a->return_expression.constant.num;
	(*dst)->args[1].operand_type = ASM_OPERAND_REGISTER_EAX;

	assert((*dst)->next == NULL);
	codegen_alloc((*dst)->next);
	(*dst)->next->base.statement_type = ASM_OP_RET;

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function(struct ast_function *a, struct asm_function *dst)
{
	assert(a->base.node_type == NODE_FUNCTION);
	assert(dst->base.statement_type == ASM_FUNCTION);
	dst->identifier = a->identifier.token;
	check(codegen_statement(&a->statement, &dst->ops));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_program(struct ast_program *a, struct asm_program *dst)
{
	assert(a->base.node_type == NODE_PROGRAM);
	assert(dst->base.statement_type == ASM_PROGRAM);
	dst->function.base.statement_type = ASM_FUNCTION;
	check(codegen_function(&a->function, &dst->function));
	return RESULT_OK;
}

result_t
codegen_init(struct ast *a, struct assembly **generated)
{
	struct asm_program *program = NULL;
	codegen_alloc(program);
	program->base.statement_type = ASM_PROGRAM;
	*generated = &program->base;

	check(codegen_program((struct ast_program *)a, program));
	return RESULT_OK;
}

void
codegen_free(struct assembly *generated)
{
	if (generated != NULL) {
		assert(generated->statement_type == ASM_PROGRAM);
		struct asm_program *program = (struct asm_program *)generated;

		struct asm_op *ops = program->function.ops;
		while (ops != NULL) {
			struct asm_op *tmp = ops;
			ops = ops->next;
			free(tmp);
		}

		free(generated);
	}
}

void
codegen_cleanup(struct assembly **generated)
{
	codegen_free(*generated);
}

static void
codegen_debug_print_operand(const struct asm_operand *operand)
{
	switch (operand->operand_type) {
	case ASM_OPERAND_IMMEDIATE:
		debug("  IMMEDIATE %lld", operand->num);
		break;
	case ASM_OPERAND_REGISTER_EAX:
		debug("  EAX");
		break;
	}
}

void
codegen_debug_print(const struct assembly *g)
{
	if (g == NULL) {
		return;
	}

	const struct string_view *str = NULL;
	const struct asm_op *ops = NULL;
	switch (g->statement_type) {
	case ASM_PROGRAM:
		debug("PROGRAM");
		codegen_debug_print(
			&((const struct asm_program *)g)->function.base);
		break;
	case ASM_FUNCTION:
		str = &((const struct asm_function *)g)->identifier;
		debug("FUNCTION %.*s", (int)str->sz, str->data);
		codegen_debug_print(
			&((const struct asm_function *)g)->ops->base);
		break;
	case ASM_OP_MOV:
		debug("MOV");
		ops = (const struct asm_op *)g;
		for (size_t i = 0; i < ARRAY_SIZE(ops->args); ++i) {
			codegen_debug_print_operand(&ops->args[i]);
		}
		codegen_debug_print(&ops->next->base);
		break;
	case ASM_OP_RET:
		debug("RET");
		ops = (const struct asm_op *)g;
		codegen_debug_print(&ops->next->base);
		break;
	}
}
