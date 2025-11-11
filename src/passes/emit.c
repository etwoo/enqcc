#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char LINUX_LABEL_PREFIX[] = ".L";
static const char MACOS_FUNC_PREFIX[] = "_";
static const char MACOS_LABEL_PREFIX[] = "L";
static const char STR_OP_MOV_QUAD[] = "movq";
static const char STR_OP_POP_QUAD[] = "popq";
static const char STR_OP_PUSH_QUAD[] = "pushq";
static const char STR_OP_RET[] = "ret";
static const char STR_REG_EAX[] = "%eax";
static const char STR_REG_EDX[] = "%edx";
static const char STR_REG_R10[] = "%r10d";
static const char STR_REG_R11[] = "%r11d";
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
		// TODO: switch eax->eal, edx->dl, r10d->r10b, r11d-r11b for conditional jump and conditional set instructions
		switch (o->u.reg) {
		case ASM_REGISTER_AX:
			dprintf(fd, "%s", STR_REG_EAX);
			break;
		case ASM_REGISTER_DX:
			dprintf(fd, "%s", STR_REG_EDX);
			break;
		case ASM_REGISTER_R10:
			dprintf(fd, "%s", STR_REG_R10);
			break;
		case ASM_REGISTER_R11:
			dprintf(fd, "%s", STR_REG_R11);
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
		if (o->u.num == 0) {
			dprintf(fd, "(%s)", STR_REG_RBP);
		} else {
			dprintf(fd,
			        "%lld(%s)",
			        -1 * CODEGEN_BYTES_PER_VALUE * o->u.num,
			        STR_REG_RBP);
		}
		break;
	case ASM_OPERAND_JUMP_TARGET_LABEL:
		assert(0 && "ASM emit for LABEL operand unimplemented");
		break;
	}
}

static void
emit_asm_op(const struct asm_op *op, enum platform plat, int fd)
{
	const char *label_prefix = plat == PLATFORM_MACOS ? MACOS_LABEL_PREFIX
	                                                  : LINUX_LABEL_PREFIX;

	if (op->opcode != ASM_OP_LABEL) {
		dprintf(fd, "\t");
	}

	char *print_opcode = NULL;
	switch (op->opcode) {
	case ASM_OP_MOV:
		print_opcode = "movl";;
		break;
	case ASM_OP_UNARY_NEG:
		print_opcode = "negl";
		break;
	case ASM_OP_UNARY_NOT:
		print_opcode = "notl";
		break;
	case ASM_OP_BINARY_ADD:
		print_opcode = "addl";
		break;
	case ASM_OP_BINARY_SUBTRACT:
		print_opcode = "subl";
		break;
	case ASM_OP_BINARY_SUBTRACT_QUAD:
		print_opcode = "subq";
		break;
	case ASM_OP_BINARY_MULTIPLY:
		print_opcode = "imull";
		break;
	case ASM_OP_COMPARE:
		print_opcode = "cmpl";
		break;
	case ASM_OP_IDIV:
		print_opcode = "idivl";
		break;
	case ASM_OP_CDQ:
		print_opcode = "cdq";
		break;
	case ASM_OP_JMP:
		print_opcode = "jmp";
		break;
	case ASM_OP_JMP_IF_EQ:
		print_opcode = "je";
		break;
	case ASM_OP_JMP_IF_NEQ:
		print_opcode = "jne";
		break;
	case ASM_OP_JMP_IF_GT:
		print_opcode = "jg";
		break;
	case ASM_OP_JMP_IF_GTE:
		print_opcode = "jge";
		break;
	case ASM_OP_JMP_IF_LT:
		print_opcode = "jl";
		break;
	case ASM_OP_JMP_IF_LTE:
		print_opcode = "jle";
		break;
	case ASM_OP_SET_IF_EQ:
		print_opcode = "sete";
		break;
	case ASM_OP_SET_IF_NEQ:
		print_opcode = "setne";
		break;
	case ASM_OP_SET_IF_GT:
		print_opcode = "setg";
		break;
	case ASM_OP_SET_IF_GTE:
		print_opcode = "setge";
		break;
	case ASM_OP_SET_IF_LT:
		print_opcode = "setl";
		break;
	case ASM_OP_SET_IF_LTE:
		print_opcode = "setle";
		break;
	case ASM_OP_LABEL:
		assert(op->args[0].operand_type ==
		       ASM_OPERAND_JUMP_TARGET_LABEL);
		dprintf(fd, "%sL_foobar%lld:", label_prefix, op->args[0].u.num);
		break;
	case ASM_OP_RET:
		dprintf(fd,
		        "%s %s, %s\n",
		        STR_OP_MOV_QUAD,
		        STR_REG_RBP,
		        STR_REG_RSP);
		dprintf(fd, "\t%s %s\n", STR_OP_POP_QUAD, STR_REG_RBP);
		dprintf(fd, "\t%s", STR_OP_RET);
		break;
	}

	if (print_opcode == NULL) {
		return;
	}
	dprintf(fd, "%s", print_opcode);

	for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
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
	dprintf(fd, "\t%s %s\n", STR_OP_PUSH_QUAD, STR_REG_RBP);
	dprintf(fd, "\t%s %s, %s\n", STR_OP_MOV_QUAD, STR_REG_RSP, STR_REG_RBP);

	for (struct asm_op *op = cg->function.ops; op != NULL; op = op->next) {
		emit_asm_op(op, plat, fd);
	}

	emit_asm_footer(plat, fd);
}
