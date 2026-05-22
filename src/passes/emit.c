#include "passes.h"
#include "passes/codegen.h"
#include "sys/alloc.h"
#include "sys/array.h"
#include "sys/compiler_features.h"

#include <assert.h>
#include <math.h> /* for signbit() */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>    /* for free() */
#include <string.h>    /* for strlen() */
#include <sys/param.h> /* for MIN() */

static const char LINUX_NX[] = "\t.section .note.GNU-stack,\"\",@progbits\n";
static const char LINUX_LABEL_PREFIX[] = ".L";
static const char LINUX_SECTION_RODATA[] = ".section .rodata";
static const char MACOS_SYMBOL_WITH_LINKAGE_PREFIX[] = "_";
static const char MACOS_LABEL_PREFIX[] = "L";
static const char MACOS_SECTION_CSTRING[] = ".cstring";
static const char MACOS_SECTION_LITERAL8[] = ".literal8";
static const char DOUBLE_LABEL_ID[] = "double_";
static const char VEC_LONGS_LABEL_ID[] = "vecl_";
static const char VEC_QUADS_LABEL_ID[] = "vecq_";
static const char CUSTOM_LABEL_ID[] = "boba_";
static const char STR_OP_MOV_QUAD[] = "movq";
static const char STR_OP_POP_QUAD[] = "popq";
static const char *const STR_OP_PUSH_QUAD = "pushq";
static const char STR_OP_RET[] = "ret";
static const char STR_REG_RIP[] = "%rip";

enum register_alias {
	REGISTER_ALIAS_8BYTE,
	REGISTER_ALIAS_4BYTE,
	REGISTER_ALIAS_1BYTE,
};

#define TO_STR(register_name, b8, b4, b1) {"%" b8, "%" b4, "%" b1},
static const char *const REGISTER_AS_STR[][3] = {FOREACH_ASM_REGISTER(TO_STR)};
#undef TO_STR

/* aka frame pointer */
#define STR_REG_RSP REGISTER_AS_STR[ASM_REGISTER_RSP][REGISTER_ALIAS_4BYTE]
/* aka stack pointer */
#define STR_REG_RBP REGISTER_AS_STR[ASM_REGISTER_RBP][REGISTER_ALIAS_4BYTE]

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
get_section_string_literals(enum platform plat)
{
	const char *result = NULL;
	switch (plat) {
	case PLATFORM_MACOS:
		result = MACOS_SECTION_CSTRING;
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
	case ASM_OPERAND_MEMORY:
		dprintf(fd,
		        "%lld(%s)",
		        o->u.mem.offset,
		        REGISTER_AS_STR[o->u.mem.reg][REGISTER_ALIAS_8BYTE]);
		break;
	case ASM_OPERAND_PSEUDO_MEMORY:
		assert(0 && "PSEUDOMEMORY should have been eliminated");
		break;
	case ASM_OPERAND_INDEXED:
		dprintf(fd,
		        "(%s, %s, %lld)",
		        REGISTER_AS_STR[o->u.idx.base][REGISTER_ALIAS_8BYTE],
		        REGISTER_AS_STR[o->u.idx.index][REGISTER_ALIAS_8BYTE],
		        o->u.idx.scale);
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
		        "%s%s%llx(%s)",
		        label_prefix,
		        DOUBLE_LABEL_ID,
		        get_double_as_quadword(o->u.dnum),
		        STR_REG_RIP);
		break;
	case ASM_OPERAND_CONSTANT_DATA_VEC_LONGS:
		dprintf(fd,
		        "%s%s%lx%lx%lx%lx(%s)",
		        label_prefix,
		        VEC_LONGS_LABEL_ID,
		        o->u.longs[0],
		        o->u.longs[1],
		        o->u.longs[2],
		        o->u.longs[3],
		        STR_REG_RIP);
		break;
	case ASM_OPERAND_CONSTANT_DATA_VEC_QUADS:
		dprintf(fd,
		        "%s%s%llx%llx(%s)",
		        label_prefix,
		        VEC_QUADS_LABEL_ID,
		        o->u.quads[0],
		        o->u.quads[1],
		        STR_REG_RIP);
		break;
	case ASM_OPERAND_CONSTANT_STRING:
		dprintf(fd,
		        "%s.str.%lld(%s)",
		        label_prefix,
		        (long long)o->u.num,
		        STR_REG_RIP);
		break;
	}
}

static void
map_wordtype_to_register_alias(const struct asm_operand *o,
                               enum register_alias *dst)
{
	switch (o->word_type) {
	case ASM_WORD_08BIT:
		*dst = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_WORD_32BIT:
		*dst = REGISTER_ALIAS_4BYTE;
		break;
	case ASM_WORD_64BIT:
		*dst = REGISTER_ALIAS_8BYTE;
		break;
	}
}

static char
map_ralias_to_op_suffix(enum register_alias reg)
{
	char c = 0;
	switch (reg) {
	case REGISTER_ALIAS_8BYTE:
		c = 'q';
		break;
	case REGISTER_ALIAS_4BYTE:
		c = 'l';
		break;
	case REGISTER_ALIAS_1BYTE:
		c = 'b';
		break;
	}
	return c;
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

	/* Special-case for opcodes with a two-letter suffix, like movs. */
	char print_opcode_suffix_src = 0;

	/*
	 * Choose the overall opcode suffix based on the word_type of the
	 * destination operand (indicated by the value of ralias_default).
	 *
	 * Some instructions override this behavior and set the opcode suffix
	 * based on the word type of the source operand instead.
	 *
	 * Instructions that do not actually require a suffix reset this value
	 * to 0, aka NUL byte.
	 */
	char print_opcode_suffix = map_ralias_to_op_suffix(ralias_default);

	if (op->opcode != ASM_OP_LABEL) {
		dprintf(fd, "\t");
	}

	/*
	 * Customize the final opcode prefix/suffix, register aliases, etc.
	 */
	const char *print_opcode = NULL;
	switch (op->opcode) {
	case ASM_OP_MOV:
		if ((is_xmm_register(&op->args[0]) &&
		     op->args[1].operand_type == ASM_OPERAND_MEMORY) ||
		    (op->args[0].operand_type == ASM_OPERAND_MEMORY &&
		     is_xmm_register(&op->args[1]))) {
			print_opcode = "movsd";
			print_opcode_suffix = 0;
		} else {
			print_opcode = "mov";
		}
		break;
	case ASM_OP_MOV_WITH_SIGN_EXTENSION:
	case ASM_OP_MOV_WITH_ZERO_EXTENSION:
		print_opcode = (op->opcode == ASM_OP_MOV_WITH_SIGN_EXTENSION)
		                       ? "movs"
		                       : "movz";
		map_wordtype_to_register_alias(&op->args[0], &ralias[0]);
		print_opcode_suffix_src = map_ralias_to_op_suffix(ralias[0]);
		assert(ralias[0] > ralias[1]);
		assert(ralias[0] > REGISTER_ALIAS_8BYTE);
		assert(ralias[1] < REGISTER_ALIAS_1BYTE);
		break;
	case ASM_OP_CVT_DOUBLE_TO_INT:
		print_opcode = "cvttsd2si";
		break;
	case ASM_OP_CVT_INT_TO_DOUBLE:
		print_opcode = "cvtsi2sd";
		/*
		 * For conversion from integer types to double, choose the
		 * opcode suffix (l vs q) and source register alias (e.g. r10d
		 * vs r10) based on the width of the source operand, not the
		 * width of destination operand.
		 */
		map_wordtype_to_register_alias(&op->args[0], &ralias[0]);
		print_opcode_suffix = map_ralias_to_op_suffix(ralias[0]);
		break;
	case ASM_OP_LEA:
		print_opcode = "lea";
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
		print_opcode = "add";
		break;
	case ASM_OP_BINARY_SUBTRACT:
		print_opcode = "sub";
		break;
	case ASM_OP_BINARY_MULTIPLY:
		print_opcode = "imul";
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
		print_opcode = "cmp";
		break;
	case ASM_OP_IDIV:
		print_opcode = "idiv";
		break;
	case ASM_OP_DIV:
		print_opcode = "div";
		break;
	case ASM_OP_CDQ:
		print_opcode = "cdq";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_CQO:
		print_opcode = "cqo";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_DOUBLE_BINARY_ADD:
		print_opcode = "addsd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_DOUBLE_BINARY_SUBTRACT:
		print_opcode = "subsd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_DOUBLE_BINARY_MULTIPLY:
		print_opcode = "mulsd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_DOUBLE_BINARY_DIVIDE:
		print_opcode = "divsd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_DOUBLE_BITWISE_XOR:
		print_opcode = "xorpd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_DOUBLE_COMPARE:
		print_opcode = "comisd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_VEC_DOUBLE_BINARY_SUBTRACT:
		print_opcode = "subpd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_VEC_DOUBLE_UNPACK_INTERLEAVE_HI:
		print_opcode = "unpckhpd";
		print_opcode_suffix = 0;
		break;
	case ASM_OP_VEC_DOUBLE_UNPACK_INTERLEAVE_LO:
		print_opcode = "punpckld";
		/* retain print_opcode_suffix */
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
	case ASM_OP_SET_IF_P:
		print_opcode = "setp";
		print_opcode_suffix = 0;
		ralias[0] = REGISTER_ALIAS_1BYTE;
		break;
	case ASM_OP_SET_IF_NP:
		print_opcode = "setnp";
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

	if (print_opcode_suffix_src > 0) {
		dprintf(fd, "%c", print_opcode_suffix_src);
	}
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

static const long long unsigned MAX_ALIGNMENT = 16;

static void
emit_asm_initializer(const struct string_view *name,
                     const char *section,
                     const char *linkage, /* prefix, if symbol has linkage */
                     const struct constant_initializer *initializer,
                     enum platform plat,
                     int fd)
{
	const char *label_prefix = get_label_prefix(plat);

	const long long unsigned byte_count = constant_byte_count(initializer);
	const long long unsigned alignment =
		byte_count >= MAX_ALIGNMENT
			? MAX_ALIGNMENT
			: initializer->elements[0].byte_count;

	if (constant_is_zero(initializer)) {
		dprintf(fd, "\t.bss\n\t.balign %llu\n", alignment);
		dprintf(fd, "%s%.*s:\n", linkage, (int)name->sz, name->data);
		dprintf(fd, "\t.zero %llu\n", byte_count);
	} else {
		dprintf(fd, "\t%s\n", section);
		if (alignment > 1) {
			dprintf(fd, "\t.balign %llu\n", alignment);
		}
		dprintf(fd, "%s%.*s:\n", linkage, (int)name->sz, name->data);
		for (long long unsigned i = 0; i < initializer->count; ++i) {
			switch (initializer->elements[i].byte_count) {
			case 1:
				dprintf(fd, "\t.byte ");
				break;
			case 4:
				dprintf(fd, "\t.long ");
				break;
			case 8:
				dprintf(fd, "\t.quad ");
				break;
			default:
				assert(0); /* logic error in caller */
				break;
			}
			if (initializer->elements[i].unique > 0) {
				dprintf(fd,
				        "%s.str.%lld\n",
				        label_prefix,
				        initializer->elements[i].unique);
			} else {
				dprintf(fd,
				        "0x%llx\n",
				        initializer->elements[i].byte_value);
			}
		}
	}
}

static void
emit_asm_str(const struct asm_str *s, enum platform plat, int fd)
{
	const char *label_prefix = get_label_prefix(plat);
	assert(label_prefix != NULL);

	char *str = NULL;
	int rc = asprintf(&str, "%s.str.%lld", label_prefix, s->string_unique);
	assert(rc >= 0);

	const struct string_view sv = {
		.data = str,
		.sz = strlen(str),
	};
	const char *section_cstr = get_section_string_literals(plat);
	emit_asm_initializer(&sv, section_cstr, "", s->initializer, plat, fd);

	free(str);
}

static void
emit_asm_var(const struct asm_variable *v, enum platform plat, int fd)
{
	const struct string_view *vname = &v->identifier;
	const char *vprefix = get_symbol_with_linkage_prefix(plat);

	if (v->linkage == ASM_LINKAGE_EXTERNAL) {
		dprintf(fd,
		        "\t.globl %s%.*s\n",
		        get_symbol_with_linkage_prefix(plat),
		        (int)vname->sz,
		        vname->data);
	}

	emit_asm_initializer(vname, ".data", vprefix, v->initializer, plat, fd);
}

/*
 * This function only handles the magic numbers for IR_OP_CTYPE_UINT_TO_DOUBLE.
 */
static WARN_UNUSED result_t
emit_asm_fp_vector_constants(const struct asm_function *f,
                             enum platform plat,
                             int fd)
{
	struct asm_operand *got_longs = NULL;
	struct asm_operand *got_quads = NULL;
	for (; f != NULL; f = f->next) {
		for (struct asm_op *op = f->ops; op != NULL; op = op->next) {
			for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
				switch (op->args[i].operand_type) {
				case ASM_OPERAND_CONSTANT_DATA_VEC_LONGS:
					got_longs = &op->args[i];
					break;
				case ASM_OPERAND_CONSTANT_DATA_VEC_QUADS:
					got_quads = &op->args[i];
					break;
				default:
					break;
				}
			}
		}
	}

	const char *section_fp_constants = get_section_fp_constants(plat);
	const char *label_prefix = get_label_prefix(plat);

	if (got_longs != NULL || got_quads != NULL) {
		dprintf(fd, "\t%s\n", section_fp_constants);
		dprintf(fd, "\t.balign 16\n");
	}

	if (got_longs != NULL) {
		dprintf(fd,
		        "%s%s%lx%lx%lx%lx:\n",
		        label_prefix,
		        VEC_LONGS_LABEL_ID,
		        got_longs->u.longs[0],
		        got_longs->u.longs[1],
		        got_longs->u.longs[2],
		        got_longs->u.longs[3]);
		for (size_t i = 0; i < ARRAY_SIZE(got_longs->u.longs); ++i) {
			dprintf(fd, "\t.long 0x%lx\n", got_longs->u.longs[i]);
		}
	}

	if (got_quads != NULL) {
		dprintf(fd,
		        "%s%s%llx%llx:\n",
		        label_prefix,
		        VEC_QUADS_LABEL_ID,
		        got_quads->u.quads[0],
		        got_quads->u.quads[1]);
		for (size_t i = 0; i < ARRAY_SIZE(got_quads->u.quads); ++i) {
			dprintf(fd, "\t.quad 0x%llx\n", got_quads->u.quads[i]);
		}
	}

	return RESULT_OK;
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
		if (i->value == value &&
		    /* distinguish +0.0 from -0.0 */
		    ((0 == signbit(i->value)) == (0 == signbit(value)))) {
			return RESULT_OK;
		}
	}

	struct fp_constant *node = zalloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_EMIT_ALLOC);
	node->next = *emitted;
	node->value = value;
	*emitted = node;

	*do_emit = true; /* new constant: tell caller to emit */
	return RESULT_OK;
}

static void
emit_asm_fp_one(const double *value, enum platform plat, int fd)
{
	const char *section_fp_constants = get_section_fp_constants(plat);
	const char *label_prefix = get_label_prefix(plat);
	const long long unsigned as_quadword = get_double_as_quadword(*value);

	dprintf(fd, "\t%s\n", section_fp_constants);
	dprintf(fd,
	        "\t.balign %lld\n",
	        ctype_to_size_bytes(&(struct ctype){
			.t = CTYPE_DOUBLE,
		}));
	dprintf(fd, "%s%s%llx:\n", label_prefix, DOUBLE_LABEL_ID, as_quadword);
	dprintf(fd, "\t.quad 0x%llx\n", as_quadword);
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
					emit_asm_fp_one(&op->args[i].u.dnum,
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

	for (struct asm_str *s = cg->string_literals; s != NULL; s = s->next) {
		emit_asm_str(s, plat, fd);
	}

	for (struct asm_variable *v = cg->variables; v != NULL; v = v->next) {
		emit_asm_var(v, plat, fd);
	}

	check(emit_asm_fp_vector_constants(cg->functions, plat, fd));
	check(emit_asm_fp_constants(arena, cg->functions, plat, fd));

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		emit_asm_fn(f, plat, fd);
	}

	emit_asm_footer(plat, fd);
	return RESULT_OK;
}
