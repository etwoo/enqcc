#include "passes.h"
#include "passes/codegen.h"
#include "sys/array.h"
#include "sys/compiler_features.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>    /* for memcpy() */
#include <sys/param.h> /* for MAX() */

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char LINUX_LABEL_PREFIX[] = ".L";
static const char LINUX_SECTION_RODATA[] = ".rodata";
static const char MACOS_SYMBOL_WITH_LINKAGE_PREFIX[] = "_";
static const char MACOS_LABEL_PREFIX[] = "L";
static const char MACOS_SECTION_LITERAL8[] = ".literal8";
static const char DOUBLE_LABEL_ID[] = "double_";
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
get_section_fp_constants(enum platform plat)
{
	const char *result = NULL;
	switch (plat) {
	case PLATFORM_MACOS:
		result = MACOS_SECTION_LITERAL8;
		break;
	case PLATFORM_LINUX:
		result = LINUX_SECTION_RODATA;
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

static long long unsigned
get_double_as_quadword(double value)
{
	long long unsigned as_quadword = 0;
	static_assert(sizeof(value) <= sizeof(as_quadword),
	              "destination must be large enough to hold 64-bit double");
	memcpy(&as_quadword, &value, sizeof(value));
	return as_quadword;
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
		if (o->u.num > LLONG_MAX) {
			assert(o->u.num <= ULLONG_MAX);
			dprintf(fd, "$%llu", (long long unsigned)o->u.num);
		} else {
			dprintf(fd, "$%lld", (long long)o->u.num);
		}
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
			assert(o->u.num <= LLONG_MAX);
			dprintf(fd,
			        "%lld(%s)",
			        (long long)o->u.num,
			        STR_REG_RBP);
		}
		break;
	case ASM_OPERAND_JUMP_TARGET_LABEL:
		assert(o->u.num <= LLONG_MAX);
		dprintf(fd,
		        "%s%s%lld",
		        label_prefix,
		        CUSTOM_LABEL_ID,
		        (long long)o->u.num);
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
	case ASM_OPERAND_CONSTANT_DATA_DOUBLE:
		dprintf(fd,
		        "%s%s%llu(%s)",
		        label_prefix,
			DOUBLE_LABEL_ID,
			get_double_as_quadword(o->u.dnum),
		        STR_REG_RIP);

		break;
	}
}

static void
map_wordtype_to_register_alias(const struct asm_operand *o,
                               enum register_alias *dst)
{
	switch (o->word_type) {
	case ASM_WORD_32BIT:
		*dst = REGISTER_ALIAS_4BYTE;
		break;
	case ASM_WORD_64BIT:
		*dst = REGISTER_ALIAS_8BYTE;
		break;
	}
}

static void
emit_asm_op(const struct asm_op *op, enum platform plat, int fd)
{
	const char *label_prefix = get_label_prefix(plat);

	/*
	 * Choose a default register_alias value based on the asm_operand that
	 * most likely corresponds to the final destination of this asm_op.
	 */
	enum register_alias ralias_default = REGISTER_ALIAS_4BYTE;
	for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
		if (op->args[i].operand_type == ASM_OPERAND_NONE) {
			continue;
		}
		/* last operand's mapping wins */
		map_wordtype_to_register_alias(&op->args[i], &ralias_default);
	}

	/*
	 * Populate register_alias values with default value. Subsequent logic
	 * may customize these values where appropriate.
	 */
	enum register_alias ralias[ARRAY_SIZE(op->args)] = {0};
	for (size_t i = 0; i < ARRAY_SIZE(ralias); ++i) {
		ralias[i] = ralias_default;
	}

	/*
	 * Choose the overall opcode suffix based on the word_type of the
	 * destination operand (indicated by the value of ralias_default).
	 */
	char print_opcode_suffix = 0;
	switch (ralias_default) {
	case REGISTER_ALIAS_8BYTE:
		print_opcode_suffix = 'q';
		break;
	case REGISTER_ALIAS_4BYTE:
		print_opcode_suffix = 'l';
		break;
	case REGISTER_ALIAS_1BYTE:
		print_opcode_suffix = 'b';
		break;
	}

	if (op->opcode != ASM_OP_LABEL) {
		dprintf(fd, "\t");
	}

	/*
	 * Customize the final opcode prefix/suffix, register aliases, etc.
	 */
	const char *print_opcode = NULL;
	switch (op->opcode) {
	case ASM_OP_MOV:
		print_opcode = "mov";
		break;
	case ASM_OP_MOV_WITH_SIGN_EXTENSION:
		print_opcode = "movslq";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_4BYTE;
		assert(ralias[1] == REGISTER_ALIAS_8BYTE);
		break;
	case ASM_OP_MOV_WITH_ZERO_EXTENSION:
		assert(0 && "MOV W/ ZEROEXTENSION should have been eliminated");
		break;
	case ASM_OP_CVT_DOUBLE_TO_INT:
		print_opcode = "vcvttsd2si";
		break;
	case ASM_OP_CVT_DOUBLE_TO_UINT:
		print_opcode = "vcvttsd2usi"; /* AVX-512 */
		break;
	case ASM_OP_CVT_INT_TO_DOUBLE:
		print_opcode = "vcvtsi2sd";
		break;
	case ASM_OP_CVT_UINT_TO_DOUBLE:
		print_opcode = "vcvtusi2sd"; /* AVX-512 */
		break;
	case ASM_OP_UNARY_NEG:
		print_opcode = "neg";
		break;
	case ASM_OP_UNARY_NOT:
		print_opcode = "not";
		break;
	case ASM_OP_UNARY_DECREMENT:
		print_opcode = "dec";
		break;
	case ASM_OP_UNARY_INCREMENT:
		print_opcode = "inc";
		break;
	case ASM_OP_BINARY_ADD:
		print_opcode = "add"; // TODO: double addsd
		break;
	case ASM_OP_BINARY_SUBTRACT:
		print_opcode = "sub"; // TODO double subsd
		break;
	case ASM_OP_BINARY_MULTIPLY:
		print_opcode = "imul"; // TODO double mulsd
		break;
	case ASM_OP_BITWISE_AND:
		print_opcode = "and";
		break;
	case ASM_OP_BITWISE_OR:
		print_opcode = "or";
		break;
	case ASM_OP_BITWISE_XOR:
		print_opcode = "xor";
		break;
	case ASM_OP_BITWISE_SIGNED_SHIFT_LEFT:
		print_opcode = "sal";
		ralias[0] = REGISTER_ALIAS_1BYTE; /* %ecx -> %cl */
		break;
	case ASM_OP_BITWISE_SIGNED_SHIFT_RIGHT:
		print_opcode = "sar";
		ralias[0] = REGISTER_ALIAS_1BYTE; /* %ecx -> %cl */
		break;
	case ASM_OP_BITWISE_UNSIGNED_SHIFT_LEFT:
		print_opcode = "shl";
		ralias[0] = REGISTER_ALIAS_1BYTE; /* %ecx -> %cl */
		break;
	case ASM_OP_BITWISE_UNSIGNED_SHIFT_RIGHT:
		print_opcode = "shr";
		ralias[0] = REGISTER_ALIAS_1BYTE; /* %ecx -> %cl */
		break;
	case ASM_OP_COMPARE:
		print_opcode = "cmp"; // TODO: double comisd
		break;
	case ASM_OP_IDIV:
		print_opcode = "idiv";
		break;
	case ASM_OP_DIV:
		print_opcode = "div";
		break;
	case ASM_OP_DDIV:
		print_opcode = "divsd";
		break;
	case ASM_OP_CDQ:
		print_opcode = "cdq";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_CQO:
		print_opcode = "cqo";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP:
		print_opcode = "jmp";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_EQ:
		print_opcode = "je";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_NEQ:
		print_opcode = "jne";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_GT:
		print_opcode = "jg";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_GTE:
		print_opcode = "jge";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_LT:
		print_opcode = "jl";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_LTE:
		print_opcode = "jle";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_A:
		print_opcode = "ja";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_AE:
		print_opcode = "jae";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_B:
		print_opcode = "jb";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_JMP_IF_BE:
		print_opcode = "jbe";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_SET_IF_EQ:
		print_opcode = "sete";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_NEQ:
		print_opcode = "setne";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_GT:
		print_opcode = "setg";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_GTE:
		print_opcode = "setge";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_LT:
		print_opcode = "setl";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_LTE:
		print_opcode = "setle";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_A:
		print_opcode = "seta";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_AE:
		print_opcode = "setae";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_B:
		print_opcode = "setb";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_BE:
		print_opcode = "setbe";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_LABEL:
		assert(op->args[0].operand_type ==
		       ASM_OPERAND_JUMP_TARGET_LABEL);
		assert(op->args[0].u.num <= LLONG_MAX);
		dprintf(fd,
		        "%s%s%lld:\n",
		        label_prefix,
		        CUSTOM_LABEL_ID,
		        (long long)op->args[0].u.num);
		break;
	case ASM_OP_PUSH:
		print_opcode = STR_OP_PUSH_QUAD;
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_8BYTE;
		break;
	case ASM_OP_CALL:
		print_opcode = "call";
		print_opcode_suffix = 0;
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

	if (print_opcode_suffix > 0) {
		dprintf(fd, "%c", print_opcode_suffix);
	}

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

	const long long int alignment = ctype_to_size_bytes(var->c89type);

	if (var->c89type == CTYPE_DOUBLE) {
		assert(0 && "TODO: suport var->initial.as_double in emit.c");
	} else if (var->initial.as_integer != 0) {
		dprintf(fd, "\t.data\n\t.balign %lld\n", alignment);
		dprintf(fd, "%s%.*s:\n", vprefix, (int)vname->sz, vname->data);
		if (alignment == 4) {
			dprintf(fd, "\t.long ");
		} else {
			dprintf(fd, "\t.quad ");
		}
		if (var->initial.as_integer > LLONG_MAX) {
			dprintf(fd,
			        "%llu\n",
			        (long long unsigned)var->initial.as_integer);
		} else {
			dprintf(fd,
			        "%lld\n",
			        (long long)var->initial.as_integer);
		}
	} else {
		dprintf(fd, "\t.bss\n\t.balign %lld\n", alignment);
		dprintf(fd, "%s%.*s:\n", vprefix, (int)vname->sz, vname->data);
		dprintf(fd, "\t.zero %lld\n", alignment);
	}
}

struct fp_constant {
	double value;
	struct fp_constant *next;
};

static WARN_UNUSED result_t
emit_asm_fp_check(Arena *arena,
                  struct fp_constant **emitted,
                  double value,
                  bool *do_emit)
{
	*do_emit = false;

	for (struct fp_constant *i = *emitted; i != NULL; i = i->next) {
		if (i->value == value) {
			return RESULT_OK;
		}
	}

	struct fp_constant *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_EMIT_ALLOC);
	node->next = *emitted;
	*emitted = node;

	*do_emit = true; /* new constant: tell caller to emit */
	return RESULT_OK;
}

static void
emit_asm_fp_one(double value, enum platform plat, int fd)
{
	const char *section_fp_constants = get_section_fp_constants(plat);
	const char *label_prefix = get_label_prefix(plat);
	const long long unsigned as_quadword = get_double_as_quadword(value);

	dprintf(fd, "\t.section %s\n", section_fp_constants);
	dprintf(fd, "%s%s%llu:\n", label_prefix, DOUBLE_LABEL_ID, as_quadword);
	dprintf(fd, "\t.quad %llu", as_quadword);
}

static WARN_UNUSED result_t
emit_asm_fp_constants(Arena *arena,
                      const struct asm_function *f,
                      enum platform plat,
                      int fd)
{
	struct fp_constant *emitted = NULL;
	for (; f != NULL; f = f->next) {
		for (struct asm_op *op = f->ops; op != NULL; op = op->next) {
			for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
				if (op->args[i].operand_type !=
				    ASM_OPERAND_CONSTANT_DATA_DOUBLE) {
					continue;
				}
				bool do_emit = false;
				check(emit_asm_fp_check(arena,
				                        &emitted,
				                        op->args[i].u.dnum,
				                        &do_emit));
				if (do_emit) {
					emit_asm_fp_one(op->args[i].u.dnum,
					                plat,
					                fd);
				}
			}
		}
	}
	return RESULT_OK;
}

result_t
emit_asm(Arena *arena, const struct assembly *cg, enum platform plat, int fd)
{
	if (cg == NULL) {
		return RESULT_OK;
	}

	for (struct asm_variable *v = cg->variables; v != NULL; v = v->next) {
		emit_asm_var(v, plat, fd);
	}

	check(emit_asm_fp_constants(arena, cg->functions, plat, fd));

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		emit_asm_fn(f, plat, fd);
	}

	emit_asm_footer(plat, fd);
	return RESULT_OK;
}
