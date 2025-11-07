#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"

#include <stdio.h>

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char MACOS_FUNC_PREFIX[] = "_";
static const char STR_OP_MOV[] = "movl";
static const char STR_OP_RET[] = "ret";
static const char STR_REGISTER_EAX[] = "%eax";
static const char STR_REGISTER_R10[] = "%r10";

static void
emit_asm_footer(enum platform plat, int fd)
{
	if (plat == PLATFORM_LINUX) {
		dprintf(fd, "%s", LINUX_NX);
	}
}

static void
emit_asm_operand(const struct asm_operand *operand, int fd)
{
	switch (operand->operand_type) {
	case ASM_OPERAND_IMMEDIATE:
		dprintf(fd, "$%lld", operand->u.num);
		break;
	case ASM_OPERAND_REGISTER:
		switch (operand->u.reg) {
		case ASM_REGISTER_AX:
			dprintf(fd, "%s", STR_REGISTER_EAX);
			break;
		case ASM_REGISTER_R10:
			dprintf(fd, "%s", STR_REGISTER_R10);
			break;
		}
		break;

	}
}

void
emit_asm(const struct assembly *cg, enum platform plat, int fd)
{
	if (cg == NULL) {
		return;
	}

	const char *fprefix = plat == PLATFORM_MACOS ? MACOS_FUNC_PREFIX : "";

	switch (cg->statement_type) {
	case ASM_PROGRAM: {
		dprintf(fd, "\t.globl %smain\n", fprefix);
		const struct asm_program *p = (const struct asm_program *)cg;
		emit_asm(&p->function.base, plat, fd);
		break;
	}
	case ASM_FUNCTION: {
		const struct asm_function *f = (const struct asm_function *)cg;
		const struct string_view *str = &f->identifier;
		dprintf(fd, "%s%.*s:\n", fprefix, (int)str->sz, str->data);
		emit_asm(&f->ops->base, plat, fd);
		break;
	}
	case ASM_OP_MOV: {
		dprintf(fd, "\t%s ", STR_OP_MOV);
		const struct asm_op *ops = (const struct asm_op *)cg;
		for (size_t i = 0; i < ARRAY_SIZE(ops->args); ++i) {
			if (i > 0) {
				dprintf(fd, ", ");
			}
			emit_asm_operand(&ops->args[i], fd);
		}
		dprintf(fd, "\n");
		emit_asm(&ops->next->base, plat, fd);
		break;
	}
	case ASM_OP_RET:
		dprintf(fd, "\t%s\n", STR_OP_RET);
		const struct asm_op *ops = (const struct asm_op *)cg;
		emit_asm(&ops->next->base, plat, fd);
		break;
	}

	emit_asm_footer(plat, fd);
}
