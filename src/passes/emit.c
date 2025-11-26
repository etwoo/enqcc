#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"
#include "sys/compiler_features.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char LINUX_LABEL_PREFIX[] = ".L";
static const char MACOS_SYMBOL_WITH_LINKAGE_PREFIX[] = "_";
static const char MACOS_LABEL_PREFIX[] = "L";
static const char CUSTOM_LABEL_ID[] = "boba_";
static const char STR_OP_MOV_QUAD[] = "movq";
static const char STR_OP_POP_QUAD[] = "popq";
static const char *const STR_OP_PUSH_QUAD = "pushq";
static const char STR_OP_RET[] = "ret";
static const char STR_REG_RSP[] = "%rsp"; /* aka frame pointer */
static const char STR_REG_RBP[] = "%rbp"; /* aka stack pointer */
static const char STR_REG_RIP[] = "%rip";

enum register_alias {
	REGISTER_ALIAS_8BYTE,
	REGISTER_ALIAS_4BYTE,
	REGISTER_ALIAS_1BYTE,
};

#define TO_STR(register_name, b8, b4, b1) {"%" b8, "%" b4, "%" b1},
static const char *const REGISTER_AS_STR[][3] = {FOREACH_ASM_REGISTER(TO_STR)};
#undef TO_STR

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

static WARN_UNUSED const char *
get_symbol_with_linkage_prefix(enum platform plat)
{
	const char *result = NULL;
	switch (plat) {
	case PLATFORM_MACOS:
		result = MACOS_SYMBOL_WITH_LINKAGE_PREFIX;
		break;
	case PLATFORM_LINUX:
		result = "";
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
	const char *fprefix = get_symbol_with_linkage_prefix(plat);
	const char *label_prefix = get_label_prefix(plat);

	switch (o->operand_type) {
	case ASM_OPERAND_NONE:
		assert(0); /* logic error in caller */
		break;
	case ASM_OPERAND_IMMEDIATE:
		dprintf(fd, "$%lld", o->u.num);
		break;
	case ASM_OPERAND_REGISTER:
		dprintf(fd, "%s", REGISTER_AS_STR[o->u.reg][ralias]);
		break;
	case ASM_OPERAND_PSEUDO_REGISTER:
		assert(0 && "PSEUDOREGISTER should have been eliminated");
		break;
	case ASM_OPERAND_STACK:
		if (o->u.num == 0) {
			dprintf(fd, "(%s)", STR_REG_RBP);
		} else {
			dprintf(fd, "%lld(%s)", o->u.num, STR_REG_RBP);
		}
		break;
	case ASM_OPERAND_JUMP_TARGET_LABEL:
		dprintf(fd,
		        "%s%s%lld",
		        label_prefix,
		        CUSTOM_LABEL_ID,
		        o->u.num);
		break;
	case ASM_OPERAND_CALL_TARGET_FUNCTION:
		dprintf(fd,
		        "%s%.*s",
		        fprefix,
		        (int)o->u.function.sz,
		        o->u.function.data);
		/*
		 * XXX: on Linux, CALL currently lack support for functions
		 * outside of the current translation unit, which require a
		 * @PLT suffix. One possible way to implement this: pass
		 * information about functions with definitions (i.e not just
		 * declarations) from sema.c to emit.c.
		 *
		 * In particular, sema_fn_signature_state() already tracks the
		 * necessary information. emit_asm_operand() could use this to
		 * determine which functions require the @PLT suffix.
		 */
		break;
	case ASM_OPERAND_VARIABLE_DATA:
		dprintf(fd,
		        "%s%.*s(%s)",
		        fprefix,
		        (int)o->u.variable.sz,
		        o->u.variable.data,
		        STR_REG_RIP);
		break;
	}
}

static void
emit_asm_op(const struct asm_op *op, enum platform plat, int fd)
{
	const char *label_prefix = get_label_prefix(plat);
	const char *print_opcode = NULL;

	enum register_alias ralias[ARRAY_SIZE(op->args)] = {0};
	for (size_t i = 0; i < ARRAY_SIZE(ralias); ++i) {
		ralias[i] = REGISTER_ALIAS_4BYTE;
	}

	if (op->opcode != ASM_OP_LABEL) {
		dprintf(fd, "\t");
	}

	switch (op->opcode) {
	case ASM_OP_MOV:
		print_opcode = "movl";
		break;
	case ASM_OP_UNARY_NEG:
		print_opcode = "negl";
		break;
	case ASM_OP_UNARY_NOT:
		print_opcode = "notl";
		break;
	case ASM_OP_UNARY_DECREMENT:
		print_opcode = "decl";
		break;
	case ASM_OP_UNARY_INCREMENT:
		print_opcode = "incl";
		break;
	case ASM_OP_BINARY_ADD:
		print_opcode = "addl";
		break;
	case ASM_OP_BINARY_ADD_QUAD:
		print_opcode = "addq";
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
	case ASM_OP_BITWISE_AND:
		print_opcode = "andl";
		break;
	case ASM_OP_BITWISE_OR:
		print_opcode = "orl";
		break;
	case ASM_OP_BITWISE_XOR:
		print_opcode = "xorl";
		break;
	case ASM_OP_BITWISE_SHIFT_LEFT:
		print_opcode = "sall";
		ralias[0] = REGISTER_ALIAS_1BYTE; /* %ecx -> %cl */
		assert(ralias[1] == REGISTER_ALIAS_4BYTE);
		break;
	case ASM_OP_BITWISE_SHIFT_RIGHT:
		print_opcode = "sarl";
		ralias[0] = REGISTER_ALIAS_1BYTE; /* %ecx -> %cl */
		assert(ralias[1] == REGISTER_ALIAS_4BYTE);
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
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_NEQ:
		print_opcode = "setne";
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_GT:
		print_opcode = "setg";
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_GTE:
		print_opcode = "setge";
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_LT:
		print_opcode = "setl";
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_LTE:
		print_opcode = "setle";
		ralias[0] = REGISTER_ALIAS_1BYTE;
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
	case ASM_OP_PUSH:
		print_opcode = STR_OP_PUSH_QUAD;
		ralias[0] = REGISTER_ALIAS_8BYTE;
		break;
	case ASM_OP_CALL:
		print_opcode = "call";
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
		emit_asm_operand(&op->args[i], plat, ralias[i], fd);
	}

	dprintf(fd, "\n");
}

static void
emit_asm_fn(const struct asm_function *fn, enum platform plat, int fd)
{
	const char *fprefix = get_symbol_with_linkage_prefix(plat);
	const struct string_view *fname = &fn->identifier;

	if (fn->linkage == ASM_LINKAGE_EXTERNAL) {
		dprintf(fd,
		        "\t.globl %s%.*s\n",
		        fprefix,
		        (int)fname->sz,
		        fname->data);
	}
	dprintf(fd, "\t.text\n");
	dprintf(fd, "%s%.*s:\n", fprefix, (int)fname->sz, fname->data);
	dprintf(fd, "\t%s %s\n", STR_OP_PUSH_QUAD, STR_REG_RBP);
	dprintf(fd, "\t%s %s, %s\n", STR_OP_MOV_QUAD, STR_REG_RSP, STR_REG_RBP);

	for (struct asm_op *op = fn->ops; op != NULL; op = op->next) {
		emit_asm_op(op, plat, fd);
	}
}

static void
emit_asm_var(const struct asm_variable *var, enum platform plat, int fd)
{
	const char *vprefix = get_symbol_with_linkage_prefix(plat);
	const struct string_view *vname = &var->identifier;

	if (var->linkage == ASM_LINKAGE_EXTERNAL) {
		dprintf(fd,
		        "\t.globl %s%.*s\n",
		        vprefix,
		        (int)vname->sz,
		        vname->data);
	}

	if (var->u.initial_as_ll != 0) {
		dprintf(fd, "\t.data\n\t.balign 4\n");
		dprintf(fd, "%s%.*s:\n", vprefix, (int)vname->sz, vname->data);
		dprintf(fd, "\t.long %lld\n", var->u.initial_as_ll);
	} else {
		dprintf(fd, "\t.bss\n\t.balign 4\n");
		dprintf(fd, "%s%.*s:\n", vprefix, (int)vname->sz, vname->data);
		dprintf(fd, "\t.zero 4\n");
	}
}

void
emit_asm(const struct assembly *cg, enum platform plat, int fd)
{
	if (cg == NULL) {
		return;
	}

	for (struct asm_variable *v = cg->variables; v != NULL; v = v->next) {
		emit_asm_var(v, plat, fd);
	}

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		emit_asm_fn(f, plat, fd);
	}

	emit_asm_footer(plat, fd);
}
