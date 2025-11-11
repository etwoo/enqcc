#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"
#include "sys/compiler_features.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char LINUX_LABEL_PREFIX[] = ".L";
static const char MACOS_FUNC_PREFIX[] = "_";
static const char MACOS_LABEL_PREFIX[] = "L";
static const char CUSTOM_LABEL_ID[] = "boba_";
static const char STR_OP_MOV_QUAD[] = "movq";
static const char STR_OP_POP_QUAD[] = "popq";
static const char STR_OP_PUSH_QUAD[] = "pushq";
static const char STR_OP_RET[] = "ret";
static const char STR_REG_EAX[] = "%eax";
static const char STR_REG_EAX_LOWEST_BYTE[] = "%al";
static const char STR_REG_EDX[] = "%edx";
static const char STR_REG_EDX_LOWEST_BYTE[] = "%dl";
static const char STR_REG_R10[] = "%r10d";
static const char STR_REG_R10_LOWEST_BYTE[] = "%r10b";
static const char STR_REG_R11[] = "%r11d";
static const char STR_REG_R11_LOWEST_BYTE[] = "%r11b";
static const char STR_REG_RSP[] = "%rsp"; /* aka frame pointer */
static const char STR_REG_RBP[] = "%rbp"; /* aka stack pointer */

enum register_alias {
	REGISTER_ALIAS_4BYTE,
	REGISTER_ALIAS_1BYTE,
};

static WARN_UNUSED const char *
get_label_prefix(enum platform plat)
{
	const char *result = NULL;
	switch (plat) {
	case PLATFORM_MACOS:
		result = MACOS_LABEL_PREFIX;
		break;
	case PLATFORM_LINUX:
		result = LINUX_LABEL_PREFIX;
		break;
	}
	return result;
}

static void
emit_asm_footer(enum platform plat, int fd)
{
	if (plat == PLATFORM_LINUX) {
		dprintf(fd, "%s", LINUX_NX);
	}
}

static void
emit_asm_operand(const struct asm_operand *o,
                 enum platform plat,
                 enum register_alias ralias,
                 int fd)
{
	const char *label_prefix = get_label_prefix(plat);

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
			switch (ralias) {
			case REGISTER_ALIAS_4BYTE:
				dprintf(fd, "%s", STR_REG_EAX);
				break;
			case REGISTER_ALIAS_1BYTE:
				dprintf(fd, "%s", STR_REG_EAX_LOWEST_BYTE);
				break;
			}
			break;
		case ASM_REGISTER_DX:
			switch (ralias) {
			case REGISTER_ALIAS_4BYTE:
				dprintf(fd, "%s", STR_REG_EDX);
				break;
			case REGISTER_ALIAS_1BYTE:
				dprintf(fd, "%s", STR_REG_EDX_LOWEST_BYTE);
				break;
			}
			break;
		case ASM_REGISTER_R10:
			switch (ralias) {
			case REGISTER_ALIAS_4BYTE:
				dprintf(fd, "%s", STR_REG_R10);
				break;
			case REGISTER_ALIAS_1BYTE:
				dprintf(fd, "%s", STR_REG_R10_LOWEST_BYTE);
				break;
			}
			break;
		case ASM_REGISTER_R11:
			switch (ralias) {
			case REGISTER_ALIAS_4BYTE:
				dprintf(fd, "%s", STR_REG_R11);
				break;
			case REGISTER_ALIAS_1BYTE:
				dprintf(fd, "%s", STR_REG_R11_LOWEST_BYTE);
				break;
			}
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
		dprintf(fd,
		        "%s%s%lld",
		        label_prefix,
		        CUSTOM_LABEL_ID,
		        o->u.num);
		break;
	}
}

static void
emit_asm_op(const struct asm_op *op, enum platform plat, int fd)
{
	const char *label_prefix = get_label_prefix(plat);
	char *print_opcode = NULL;
	enum register_alias ralias = REGISTER_ALIAS_4BYTE;

	if (op->opcode != ASM_OP_LABEL) {
		dprintf(fd, "\t");
	}

	switch (op->opcode) {
	case ASM_OP_MOV:
		print_opcode = "movl";
		;
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
		ralias = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_NEQ:
		print_opcode = "setne";
		ralias = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_GT:
		print_opcode = "setg";
		ralias = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_GTE:
		print_opcode = "setge";
		ralias = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_LT:
		print_opcode = "setl";
		ralias = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_LTE:
		print_opcode = "setle";
		ralias = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_LABEL:
		assert(op->args[0].operand_type ==
		       ASM_OPERAND_JUMP_TARGET_LABEL);
		dprintf(fd,
		        "%s%s%lld:\n",
		        label_prefix,
		        CUSTOM_LABEL_ID,
		        op->args[0].u.num);
		break;
	case ASM_OP_RET:
		dprintf(fd,
		        "%s %s, %s\n",
		        STR_OP_MOV_QUAD,
		        STR_REG_RBP,
		        STR_REG_RSP);
		dprintf(fd, "\t%s %s\n", STR_OP_POP_QUAD, STR_REG_RBP);
		dprintf(fd, "\t%s\n", STR_OP_RET);
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
		emit_asm_operand(&op->args[i], plat, ralias, fd);
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
