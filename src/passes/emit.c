#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char MACOS_FUNC_PREFIX[] = "_";
static const char STR_OP_MOV_QUAD[] = "movq";
static const char STR_OP_MOV[] = "movl";
static const char STR_OP_NEG[] = "negl";
static const char STR_OP_NOT[] = "notl";
static const char STR_OP_POP_QUAD[] = "popq";
static const char STR_OP_PUSH_QUAD[] = "pushq";
static const char STR_OP_RET[] = "ret";
static const char STR_OP_SUB_QUAD[] = "subq";
static const char STR_REG_EAX[] = "%eax";
static const char STR_REG_R10[] = "%r10d";
static const char STR_REG_RSP[] = "%rsp"; /* aka frame pointer */
static const char STR_REG_RBP[] = "%rbp"; /* aka stack pointer */

static void
emit_asm_footer(enum platform plat, int fd)
{
	if (plat == PLATFORM_LINUX) {
		dprintf(fd, "%s", LINUX_NX);
	}
}

static void
emit_asm_operand(const struct asm_operand *o, int fd)
{
	switch (o->operand_type) {
	case ASM_OPERAND_NONE:
		assert(0); /* logic error in caller */
		break;
	case ASM_OPERAND_IMMEDIATE:
		dprintf(fd, "$%lld", o->u.num);
		break;
	case ASM_OPERAND_REGISTER:
		switch (o->u.reg) {
		case ASM_REGISTER_AX:
			dprintf(fd, "%s", STR_REG_EAX);
			break;
		case ASM_REGISTER_R10:
			dprintf(fd, "%s", STR_REG_R10);
			break;
		case ASM_REGISTER_RSP:
			dprintf(fd, "%s", STR_REG_RSP);
			break;
		}
		break;
	case ASM_OPERAND_PSEUDO_REGISTER:
		assert(0 && "PSEUDOREGISTER should have been eliminated");
		break;
	case ASM_OPERAND_STACK:
		dprintf(fd,
		        "%lld(%s)",
		        -1 * CODEGEN_BYTES_PER_VALUE * o->u.num,
		        STR_REG_RBP);
		break;
	}
}

static void
emit_asm_op(const struct asm_op *op, int fd)
{
	dprintf(fd, "\t");

	bool print_operands = true;

	switch (op->opcode) {
	case ASM_OP_MOV:
		dprintf(fd, "%s", STR_OP_MOV);
		break;
	case ASM_OP_SUB:
		dprintf(fd, "%s", STR_OP_SUB_QUAD);
		break;
	case ASM_OP_UNARY_NEG:
		dprintf(fd, "%s", STR_OP_NEG);
		break;
	case ASM_OP_UNARY_NOT:
		dprintf(fd, "%s", STR_OP_NOT);
		break;
	case ASM_OP_RET:
		dprintf(fd,
		        "%s %s %s\n",
		        STR_OP_MOV_QUAD,
		        STR_REG_RBP,
		        STR_REG_RSP);
		dprintf(fd, "%s %s\n", STR_OP_POP_QUAD, STR_REG_RBP);
		dprintf(fd, "%s", STR_OP_RET);
		print_operands = false;
		break;
	}

	for (size_t i = 0; print_operands && i < ARRAY_SIZE(op->args); ++i) {
		if (op->args[i].operand_type == ASM_OPERAND_NONE) {
			continue;
		}
		if (i == 0) {
			dprintf(fd, " ");
		} else {
			dprintf(fd, ", ");
		}
		emit_asm_operand(&op->args[i], fd);
	}

	dprintf(fd, "\n");
}

void
emit_asm(const struct assembly *cg, enum platform plat, int fd)
{
	if (cg == NULL) {
		return;
	}

	const char *fprefix = plat == PLATFORM_MACOS ? MACOS_FUNC_PREFIX : "";
	dprintf(fd, "\t.globl %smain\n", fprefix);

	const struct string_view *fname = &cg->function.identifier;
	dprintf(fd, "%s%.*s:\n", fprefix, (int)fname->sz, fname->data);
	dprintf(fd, "%s %s\n", STR_OP_PUSH_QUAD, STR_REG_RBP);
	dprintf(fd, "%s %s %s\n", STR_OP_MOV_QUAD, STR_REG_RSP, STR_REG_RBP);

	for (struct asm_op *op = cg->function.ops; op != NULL; op = op->next) {
		emit_asm_op(op, fd);
	}

	emit_asm_footer(plat, fd);
}
