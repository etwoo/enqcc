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
codegen_statement_one(const struct ir_op *src, struct asm_op **dst)
{
	assert(*dst == NULL);
	codegen_alloc(*dst);

	switch (src->opcode) {
	case IR_OP_UNARY_IDENTITY:
		// assert(src->args[0].subtype == IR_VAL_CONSTANT_INT); // TODO
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0].operand_type = ASM_OPERAND_IMMEDIATE;
		(**dst).args[0].u.num = src->args[0].num;
		(**dst).args[1].operand_type = ASM_OPERAND_REGISTER;
		(**dst).args[1].u.reg = ASM_REGISTER_AX;
		dst = &(**dst).next;
		codegen_alloc(*dst);
		(**dst).opcode = ASM_OP_RET;
		break;
	case IR_OP_UNARY_NEGATE:
		info("HELLO1 TODO IMPLEMENT NEGATE");
		break;
	case IR_OP_UNARY_COMPLEMENT:
		info("HELLO1 TODO IMPLEMENT COMPLEMENT");
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_statement(const struct ir_op *src, struct asm_op **dst)
{
	while (src != NULL) {
		check(codegen_statement_one(src, dst));
		src = src->next;
		while (*dst != NULL) {
			dst = &(**dst).next;
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function(const struct ir_function *ir, struct asm_function *dst)
{
	dst->identifier = ir->identifier;
	check(codegen_statement(ir->ops, &dst->ops));
	return RESULT_OK;
}

result_t
codegen_init(const struct intermediate *ir, struct assembly **cg)
{
	codegen_alloc(*cg);
	check(codegen_function(&ir->function, &(**cg).function));
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
		struct asm_op *ops = cg->function.ops;
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
		debug("  IMMEDIATE %lld", operand->u.num);
		break;
	case ASM_OPERAND_REGISTER:
		switch (operand->u.reg) {
		case ASM_REGISTER_AX:
			debug("  EAX");
			break;
		case ASM_REGISTER_R10:
			debug("  R10");
			break;
		}
		break;
	}
}

static void
codegen_debug_print_op(const struct asm_op *op)
{
	switch (op->opcode) {
	case ASM_OP_MOV:
		debug("MOV");
		for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
			codegen_debug_print_operand(&op->args[i]);
		}
		break;
	case ASM_OP_RET:
		debug("RET");
		break;
	}
}

void
codegen_debug_print(const struct assembly *cg)
{
	if (cg == NULL) {
		return;
	}

	debug("PROGRAM");

	const struct string_view *fname = &cg->function.identifier;
	debug("FUNCTION %.*s", (int)fname->sz, fname->data);

	for (struct asm_op *op = cg->function.ops; op != NULL; op = op->next) {
		codegen_debug_print_op(op);
	}
}
