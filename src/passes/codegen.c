#include "passes/codegen.h"

#include "passes.h"
#include "passes/ir.h"
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
codegen_statement(const struct ir_op *src, struct asm_op **dst)
{
	assert(src->opcode == IR_OP_UNARY_IDENTITY);
	assert(src->args[0].subtype == IR_VAL_CONSTANT_INT);

	assert(*dst == NULL);
	codegen_alloc(*dst);
	(*dst)->base.statement_type = ASM_OP_MOV;
	(*dst)->args[0].operand_type = ASM_OPERAND_IMMEDIATE;
	(*dst)->args[0].num = src->args[0].num;
	(*dst)->args[1].operand_type = ASM_OPERAND_REGISTER_EAX;

	assert((*dst)->next == NULL);
	codegen_alloc((*dst)->next);
	(*dst)->next->base.statement_type = ASM_OP_RET;

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function(const struct ir_function *ir, struct asm_function *dst)
{
	assert(dst->base.statement_type == ASM_FUNCTION);
	dst->identifier = ir->identifier;
	check(codegen_statement(ir->ops, &dst->ops));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_program(const struct intermediate *ir, struct asm_program *dst)
{
	assert(dst->base.statement_type == ASM_PROGRAM);
	dst->function.base.statement_type = ASM_FUNCTION;
	check(codegen_function(&ir->function, &dst->function));
	return RESULT_OK;
}

result_t
codegen_init(const struct intermediate *ir, struct assembly **cg)
{
	struct asm_program *program = NULL;
	codegen_alloc(program);
	program->base.statement_type = ASM_PROGRAM;
	*cg = &program->base;

	check(codegen_program(ir, program));
	return RESULT_OK;
}

result_t
codegen_stack(struct assembly *cg)
{
	debug("Replacing pseudoregisters with stack addresses");
	(void)cg; // TODO
	return RESULT_OK;
}

result_t
codegen_fixup(struct assembly *cg)
{
	debug("Fixing up invalid instructions");
	(void)cg; // TODO
	return RESULT_OK;
}

void
codegen_free(struct assembly *cg)
{
	if (cg != NULL) {
		assert(cg->statement_type == ASM_PROGRAM);
		struct asm_program *program = (struct asm_program *)cg;

		struct asm_op *ops = program->function.ops;
		while (ops != NULL) {
			struct asm_op *tmp = ops;
			ops = ops->next;
			free(tmp);
		}

		free(cg);
	}
}

void
codegen_cleanup(struct assembly **cg)
{
	codegen_free(*cg);
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
codegen_debug_print(const struct assembly *cg)
{
	if (cg == NULL) {
		return;
	}

	switch (cg->statement_type) {
	case ASM_PROGRAM: {
		debug("PROGRAM");
		const struct asm_program *p = (const struct asm_program *)cg;
		codegen_debug_print(&p->function.base);
		break;
	}
	case ASM_FUNCTION: {
		const struct asm_function *f = (const struct asm_function *)cg;
		const struct string_view *str = &f->identifier;
		debug("FUNCTION %.*s", (int)str->sz, str->data);
		codegen_debug_print(&f->ops->base);
		break;
	}
	case ASM_OP_MOV: {
		debug("MOV");
		const struct asm_op *ops = (const struct asm_op *)cg;
		for (size_t i = 0; i < ARRAY_SIZE(ops->args); ++i) {
			codegen_debug_print_operand(&ops->args[i]);
		}
		codegen_debug_print(&ops->next->base);
		break;
	}
	case ASM_OP_RET: {
		debug("RET");
		const struct asm_op *ops = (const struct asm_op *)cg;
		codegen_debug_print(&ops->next->base);
		break;
	}
	}
}
