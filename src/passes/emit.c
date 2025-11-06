#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"

#include <stdio.h>
#include <unistd.h>

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char MACOS_FUNC_PREFIX[] = "_";
static const char STR_OP_MOV[] = "movl";
static const char STR_OP_RET[] = "ret";
static const char STR_REGISTER_EAX[] = "%eax";

static void
emit_asm_footer(platform plat, int fd)
{
	if (plat == PLATFORM_LINUX) {
		write(fd, LINUX_NX, sizeof(LINUX_NX) - 1);
	}
}

static void
emit_asm_operand(const struct asm_operand *operand, int fd)
{
	switch (operand->operand_type) {
	case ASM_OPERAND_IMMEDIATE:
		dprintf(fd, "$%lld", operand->num);
		break;
	case ASM_OPERAND_REGISTER_EAX:
		dprintf(fd, "%s", STR_REGISTER_EAX);
		break;
	}
}

void
emit_asm(const struct assembly *g, platform plat, int fd)
{
	if (g == NULL) {
		return;
	}

	const char *fp = plat == PLATFORM_MACOS ? MACOS_FUNC_PREFIX : "";
	const struct string_view *str = NULL;
	const struct asm_op *ops = NULL;

	switch (g->statement_type) {
	case ASM_PROGRAM:
		dprintf(fd, "\t.globl %smain\n", fp);
		emit_asm(&((const struct asm_program *)g)->function.base,
		         plat,
		         fd);
		break;
	case ASM_FUNCTION:
		str = &((const struct asm_function *)g)->identifier;
		dprintf(fd, "%s%.*s:\n", fp, (int)str->sz, str->data);
		emit_asm(&((const struct asm_function *)g)->ops->base,
		         plat,
		         fd);
		break;
	case ASM_OP_MOV:
		dprintf(fd, "\t%s ", STR_OP_MOV);
		ops = (const struct asm_op *)g;
		for (size_t i = 0; i < ARRAY_SIZE(ops->args); ++i) {
			if (i > 0) {
				write(fd, ", ", 2);
			}
			emit_asm_operand(&ops->args[i], fd);
		}
		write(fd, "\n", 1);
		emit_asm(&ops->next->base, plat, fd);
		break;
	case ASM_OP_RET:
		dprintf(fd, "\t%s\n", STR_OP_RET);
		ops = (const struct asm_op *)g;
		emit_asm(&ops->next->base, plat, fd);
		break;
	}

	emit_asm_footer(plat, fd);
}
