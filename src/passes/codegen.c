#include "passes/codegen.h"

#include "passes.h"
#include "passes/ir.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h> /* for LLONG_MIN and LLONG_MAX */
#include <stdbool.h>
#include <sys/param.h> /* for MIN() and MAX() */

#define TO_ENUM(register_name, b8, b4, b1) ASM_REGISTER_##register_name,
static const enum asm_register CALL_REG[] = {FOREACH_CALL_REGISTER(TO_ENUM)};
static const enum asm_register CALL_FP[] = {FOREACH_FP_CALL_REGISTER(TO_ENUM)};
#undef TO_ENUM

static const long long int CODEGEN_REGISTER_ARGS = ARRAY_SIZE(CALL_REG);
static const long long int CODEGEN_FP_REGISTER_ARGS = ARRAY_SIZE(CALL_FP);
static const long long int CODEGEN_BYTES_PER_VALUE = 4;
static const long long int CODEGEN_BYTES_PER_PUSH = 8;
static const long long int CODEGEN_BYTES_ARG_FIRST = 16;
static const long long int CODEGEN_BYTES_STACK_ALIGN_TOP_OR_ARR = 16;
static const long unsigned UINT_TO_DOUBLE_MAGIC_L1 = 0x43300000;
static const long unsigned UINT_TO_DOUBLE_MAGIC_L2 = 0x45300000;
static const long long unsigned UINT_TO_DOUBLE_MAGIC_Q1 = 0x4330000000000000;
static const long long unsigned UINT_TO_DOUBLE_MAGIC_Q2 = 0x4530000000000000;

static WARN_UNUSED result_t
codegen_alloc_op(Arena *arena, struct asm_op **dst)
{
	static struct asm_op dummy_workaround_clang_analyzer_null_pointer = {0};

	*dst = arena_alloc(arena, sizeof(**dst));
	if (*dst == NULL) {
		/*
		 * See parse_init() for an explanation of this workaround.
		 */
		*dst = &dummy_workaround_clang_analyzer_null_pointer;
		return make_result(ERR_CODEGEN_ALLOC);
	}

	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

static void
codegen_op_list_concat(struct asm_op *first, struct asm_op *second)
{
	assert(first != NULL);
	while (first->next != NULL) {
		first = first->next;
	}
	assert(first->next == NULL);
	first->next = second;
}

static void
codegen_op_list_prepend(struct asm_op *new_head, struct asm_op **head)
{
	codegen_op_list_concat(new_head, *head);
	*head = new_head;
}

bool
is_xmm_register(const struct asm_operand *o)
{
	return o->operand_type == ASM_OPERAND_REGISTER &&
	       o->u.reg >= ASM_REGISTER_XMM0 && o->u.reg <= ASM_REGISTER_XMM15;
}

static void
codegen_set_operand_immediate_zero(struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_IMMEDIATE;
	dst->u.num = 0;
}

static void
codegen_map_ctype_impl(const struct ctype *c, struct asm_operand *dst)
{
	switch (c->t) {
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_UNSIGNED_CHAR:
		dst->word_type = ASM_WORD_08BIT;
		break;
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
		dst->word_type = ASM_WORD_32BIT;
		break;
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
	case CTYPE_DOUBLE:
	case CTYPE_POINTER_TO:
	case CTYPE_ARRAY_OF:
		dst->word_type = ASM_WORD_64BIT;
		break;
	}
}

static void
codegen_map_ctype(const struct ir_val *src, struct asm_operand *dst)
{
	codegen_map_ctype_impl(&src->c89type, dst);
}

static void
codegen_set_operand_memory(const struct ir_val *basis,
                           enum asm_register reg,
                           long long int offset,
                           struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_MEMORY;
	dst->u.mem.offset = offset;
	dst->u.mem.reg = reg;
	codegen_map_ctype(basis, dst);
}

static void
codegen_set_operand_stack(const struct ir_val *basis,
                          long long int offset,
                          struct asm_operand *dst)
{
	codegen_set_operand_memory(basis, ASM_REGISTER_RBP, offset, dst);
}

static void
codegen_set_operand_indexed(const struct ir_val *basis,
                            long long int scale,
                            struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_INDEXED;
	dst->u.idx.base = ASM_REGISTER_AX;
	dst->u.idx.index = ASM_REGISTER_DX;
	dst->u.idx.scale = scale;
	codegen_map_ctype(basis, dst);
}

static void
codegen_set_operand_register(const struct ir_val *basis,
                             enum asm_register reg,
                             struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_REGISTER;
	dst->u.reg = reg;
	codegen_map_ctype(basis, dst);
}

static void
codegen_set_operand_pseudo(const struct ir_val *basis, struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_PSEUDO_REGISTER;
	dst->u.num = basis->num;
	codegen_map_ctype(basis, dst);
}

static void
codegen_set_operand_eax(const struct ir_val *basis, struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_REGISTER;
	dst->u.reg = ASM_REGISTER_AX;
	codegen_map_ctype(basis, dst);
}

#define MAKE_SETTER(fn, register_value)                                        \
	static void fn(const struct asm_operand *basis,                        \
	               struct asm_operand *dst)                                \
	{                                                                      \
		dst->operand_type = ASM_OPERAND_REGISTER;                      \
		dst->u.reg = register_value;                                   \
		dst->word_type = basis->word_type;                             \
	}
MAKE_SETTER(codegen_set_operand_ecx, ASM_REGISTER_CX)
MAKE_SETTER(codegen_set_operand_r10, ASM_REGISTER_R10)
MAKE_SETTER(codegen_set_operand_r11, ASM_REGISTER_R11)

static const struct asm_operand OPERAND_RSP_64BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_RSP,
};
static const struct asm_operand OPERAND_RAX_64BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_AX,
};
static const struct asm_operand OPERAND_RAX_32BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_32BIT,
	.u.reg = ASM_REGISTER_AX,
};
static const struct asm_operand OPERAND_RAX_08BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_08BIT,
	.u.reg = ASM_REGISTER_AX,
};
static const struct asm_operand OPERAND_RCX_64BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_CX,
};
static const struct asm_operand OPERAND_RDX_64BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_DX,
};
static const struct asm_operand OPERAND_R10_64BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_R10,
};
static const struct asm_operand OPERAND_XMM0 = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_XMM0,
};
static const struct asm_operand OPERAND_XMM1 = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_XMM1,
};
static const struct asm_operand OPERAND_XMM2 = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_XMM2,
};
static const struct asm_operand OPERAND_XMM15 = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_XMM15,
};

static void
codegen_map_operand(const struct ir_val *src, struct asm_operand *dst)
{
	if (ctype_is_array(&src->c89type) &&
	    src->subtype == IR_VAL_TEMPORARY_VARIABLE) {
		dst->operand_type = ASM_OPERAND_PSEUDO_MEMORY;
		dst->u.pseudo_mem.num = src->num;
		dst->u.pseudo_mem.offset = src->offset;
		dst->u.pseudo_mem.total_bytes =
			ctype_to_size_bytes(&src->c89type);
		const struct ctype *innermost = &src->c89type;
		while (ctype_is_array(innermost)) {
			innermost = innermost->referent;
		}
		codegen_map_ctype_impl(innermost, dst);
		return;
	}

	switch (src->subtype) {
	case IR_VAL_NONE:
		assert(0 && "unset operand in 2-arg/3-arg/4-arg op");
		break;
	case IR_VAL_CONSTANT:
		switch (src->c89type.t) {
		case CTYPE_CHAR:
		case CTYPE_SIGNED_CHAR:
		case CTYPE_UNSIGNED_CHAR:
		case CTYPE_INT:
		case CTYPE_UNSIGNED_INT:
		case CTYPE_LONG:
		case CTYPE_UNSIGNED_LONG:
		case CTYPE_POINTER_TO:
		case CTYPE_ARRAY_OF:
			dst->operand_type = ASM_OPERAND_IMMEDIATE;
			dst->u.num = src->num;
			break;
		case CTYPE_DOUBLE:
			dst->operand_type = ASM_OPERAND_CONSTANT_DATA_DOUBLE;
			dst->u.dnum = src->dnum;
			break;
		}
		break;
	case IR_VAL_TEMPORARY_VARIABLE:
		dst->operand_type = ASM_OPERAND_PSEUDO_REGISTER;
		dst->u.num = src->num;
		break;
	case IR_VAL_JUMP_TARGET_LABEL:
		dst->operand_type = ASM_OPERAND_JUMP_TARGET_LABEL;
		dst->u.num = src->num;
		break;
	case IR_VAL_VARIABLE_DATA:
		dst->operand_type = ASM_OPERAND_VARIABLE_DATA;
		dst->u.variable = src->varname;
		break;
	case IR_VAL_STRING_LITERAL:
		dst->operand_type = ASM_OPERAND_CONSTANT_STRING;
		dst->u.num = src->num;
		break;
	}

	codegen_map_ctype(src, dst);
}

static void
codegen_map_operands_all(const struct ir_op *src, struct asm_op *dst)
{
	for (size_t i = 0; i < ARRAY_SIZE(dst->args); ++i) {
		codegen_map_operand(&src->args[i], &dst->args[i]);
	}
}

static void
codegen_copy_operand(const struct asm_operand *src, struct asm_operand *dst)
{
	memcpy(dst, src, sizeof(*dst));
}

static WARN_UNUSED enum asm_linkage
codegen_map_linkage(enum ir_linkage linkage)
{
	switch (linkage) {
	case IR_LINKAGE_INTERNAL:
		return ASM_LINKAGE_INTERNAL;
	case IR_LINKAGE_EXTERNAL:
		return ASM_LINKAGE_EXTERNAL;
	}
	assert(0); /* logic error in caller */
}

static WARN_UNUSED result_t
codegen_alloc_modify_rsp(Arena *arena,
                         struct asm_op **dst,
                         bool add,
                         long long int n)
{
	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = add ? ASM_OP_BINARY_ADD : ASM_OP_BINARY_SUBTRACT;
	(**dst).args[0].operand_type = ASM_OPERAND_IMMEDIATE;
	(**dst).args[0].u.num = n;
	(**dst).args[1] = OPERAND_RSP_64BIT;
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_alloc_subq_rsp(Arena *arena, struct asm_op **dst, long long int n)
{
	check(codegen_alloc_modify_rsp(arena, dst, false, n));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_alloc_addq_rsp(Arena *arena, struct asm_op **dst, long long int n)
{
	check(codegen_alloc_modify_rsp(arena, dst, true, n));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_op_call(Arena *arena, const struct ir_op *src, struct asm_op **dst)
{
	assert(src->opcode == IR_OP_CALL);
	assert(src->fun.data != NULL && src->fun.sz > 0);

	size_t n_args = 0;
	for (size_t i = 0; i < ARRAY_SIZE(src->args); ++i) {
		if (src->args[i].subtype == IR_VAL_NONE) {
			break;
		}
		++n_args;
	}
	assert(n_args > 0); /* one arg minimum, for return value at least */
	--n_args;

	const long long int stack_padding =
		n_args % 2 == 1 ? CODEGEN_BYTES_PER_PUSH : 0;
	if (stack_padding > 0) {
		check(codegen_alloc_subq_rsp(arena, dst, stack_padding));
		dst = &(**dst).next;
	}

	struct ir_op dummy = {0};
	long long int n_stack = 0;
	long long int n_general = 0;
	long long int n_double = 0;

	for (size_t i = 0; i < n_args; ++i) {
		bool is_fp = ctype_is_floating_point(&src->args[i].c89type);

		const enum asm_register *dst_reg = NULL;
		if (is_fp && n_double < CODEGEN_FP_REGISTER_ARGS) {
			dst_reg = &CALL_FP[n_double++];
		} else if (!is_fp && n_general < CODEGEN_REGISTER_ARGS) {
			dst_reg = &CALL_REG[n_general++];
		}

		if (dst_reg != NULL) {
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&src->args[i], &(**dst).args[0]);
			codegen_set_operand_register(&src->args[i],
			                             *dst_reg,
			                             &(**dst).args[1]);
			dst = &(**dst).next;
		} else {
			dummy.args[n_stack++] = src->args[i];
		}
	}

	for (size_t i = n_stack; i > 0; --i) {
		size_t pos = i - 1;
		check(codegen_alloc_op(arena, dst));
		if (dummy.args[pos].subtype == IR_VAL_CONSTANT ||
		    ctype_to_size_bytes(&dummy.args[pos].c89type) ==
		            CODEGEN_BYTES_PER_PUSH) {
			(**dst).opcode = ASM_OP_PUSH;
			codegen_map_operand(&dummy.args[pos], &(**dst).args[0]);
		} else {
			/*
			 * To pass a 32-bit argument via stack, we first copy
			 * to %eax (32-bit), then push %rax (64-bit).
			 */
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&dummy.args[pos], &(**dst).args[0]);
			codegen_set_operand_eax(&dummy.args[pos],
			                        &(**dst).args[1]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_PUSH;
			(**dst).args[0] = OPERAND_RAX_64BIT;
		}
		dst = &(**dst).next;
	}

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_CALL;
	(**dst).args[0].operand_type = ASM_OPERAND_CALL_TARGET_FUNCTION;
	(**dst).args[0].u.function = src->fun;
	dst = &(**dst).next;

	const long long int stack_deallocate =
		(CODEGEN_BYTES_PER_PUSH * n_stack) + stack_padding;
	if (stack_deallocate > 0) {
		check(codegen_alloc_addq_rsp(arena, dst, stack_deallocate));
		dst = &(**dst).next;
	}

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	if (ctype_is_floating_point(&src->args[n_args].c89type)) {
		(**dst).args[0] = OPERAND_XMM0;
	} else {
		codegen_set_operand_eax(&src->args[n_args], &(**dst).args[0]);
	}
	codegen_map_operand(&src->args[n_args], &(**dst).args[1]);
	return RESULT_OK;
}

static WARN_UNUSED bool
in_place_update(const struct ir_op *src, size_t result_pos)
{
	if (src->args[0].subtype != src->args[result_pos].subtype ||
	    src->args[0].num != src->args[result_pos].num ||
	    src->args[0].varname.sz != src->args[result_pos].varname.sz) {
		return false;
	}
	if ((src->args[0].varname.data == NULL) !=
	    (src->args[result_pos].varname.data == NULL)) {
		return false;
	}
	if (src->args[0].varname.data == NULL) {
		assert(src->args[result_pos].varname.data == NULL);
		return true;
	}
	return (0 == strncmp(src->args[0].varname.data,
	                     src->args[result_pos].varname.data,
	                     src->args[0].varname.sz));
}

static WARN_UNUSED result_t
codegen_statement_fp(Arena *arena, const struct ir_op *src, struct asm_op **dst)
{
	switch (src->opcode) {
	case IR_OP_RET:
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		(**dst).args[1] = OPERAND_XMM0;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_RET;
		break;
	case IR_OP_UNARY_NEGATE:
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		(**dst).args[1] = OPERAND_XMM1;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0].operand_type = ASM_OPERAND_CONSTANT_DATA_DOUBLE;
		(**dst).args[0].u.dnum = -0.0;
		(**dst).args[1] = OPERAND_XMM0;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_DOUBLE_BITWISE_XOR;
		(**dst).args[0] = OPERAND_XMM0;
		(**dst).args[1] = OPERAND_XMM1;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0] = OPERAND_XMM1;
		codegen_map_operand(&src->args[1], &(**dst).args[1]);
		break;
	case IR_OP_UNARY_DECREMENT:
	case IR_OP_UNARY_INCREMENT:
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0].operand_type = ASM_OPERAND_CONSTANT_DATA_DOUBLE;
		(**dst).args[0].u.dnum = 1.0;
		(**dst).args[1] = OPERAND_XMM0;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = src->opcode == IR_OP_UNARY_DECREMENT
		                         ? ASM_OP_DOUBLE_BINARY_SUBTRACT
		                         : ASM_OP_DOUBLE_BINARY_ADD;
		(**dst).args[0] = OPERAND_XMM0;
		codegen_map_operand(&src->args[0], &(**dst).args[1]);
		break;
	case IR_OP_BINARY_ADD:
	case IR_OP_BINARY_SUBTRACT:
	case IR_OP_BINARY_MULTIPLY:
	case IR_OP_BINARY_DIVIDE:
		check(codegen_alloc_op(arena, dst));
		if (!in_place_update(src, 2)) {
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&src->args[0], &(**dst).args[0]);
			codegen_map_operand(&src->args[2], &(**dst).args[1]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
		}
		switch (src->opcode) {
		case IR_OP_BINARY_ADD:
			(**dst).opcode = ASM_OP_DOUBLE_BINARY_ADD;
			break;
		case IR_OP_BINARY_SUBTRACT:
			(**dst).opcode = ASM_OP_DOUBLE_BINARY_SUBTRACT;
			break;
		case IR_OP_BINARY_MULTIPLY:
			(**dst).opcode = ASM_OP_DOUBLE_BINARY_MULTIPLY;
			break;
		case IR_OP_BINARY_DIVIDE:
			(**dst).opcode = ASM_OP_DOUBLE_BINARY_DIVIDE;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		break;
	case IR_OP_COMPARE_EQUAL:
	case IR_OP_COMPARE_NOT_EQUAL:
	case IR_OP_COMPARE_LESS_THAN:
	case IR_OP_COMPARE_LESS_THAN_EQ:
	case IR_OP_COMPARE_MORE_THAN:
	case IR_OP_COMPARE_MORE_THAN_EQ:
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_DOUBLE_COMPARE;
		/* note inverted arg order: IR_OP_COMPARE_* -> ASM_OP_COMPARE */
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		codegen_map_operand(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		switch (src->opcode) {
		case IR_OP_COMPARE_EQUAL:
		case IR_OP_COMPARE_LESS_THAN:
		case IR_OP_COMPARE_LESS_THAN_EQ:
		case IR_OP_COMPARE_MORE_THAN:
		case IR_OP_COMPARE_MORE_THAN_EQ:
			(**dst).opcode = ASM_OP_MOV;
			codegen_set_operand_immediate_zero(&(**dst).args[0]);
			(**dst).args[1] = OPERAND_RCX_64BIT;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_SET_IF_NP;
			(**dst).args[0] = OPERAND_RCX_64BIT;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			switch (src->opcode) {
			case IR_OP_COMPARE_EQUAL:
				(**dst).opcode = ASM_OP_SET_IF_EQ;
				break;
			case IR_OP_COMPARE_LESS_THAN:
				(**dst).opcode = ASM_OP_SET_IF_B;
				break;
			case IR_OP_COMPARE_LESS_THAN_EQ:
				(**dst).opcode = ASM_OP_SET_IF_BE;
				break;
			case IR_OP_COMPARE_MORE_THAN:
				(**dst).opcode = ASM_OP_SET_IF_A;
				break;
			case IR_OP_COMPARE_MORE_THAN_EQ:
				(**dst).opcode = ASM_OP_SET_IF_AE;
				break;
			default:
				assert(0); /* logic error in caller */
				break;
			}
			codegen_map_operand(&src->args[2], &(**dst).args[0]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_BITWISE_AND;
			(**dst).args[0] = OPERAND_RCX_64BIT;
			codegen_map_operand(&src->args[2], &(**dst).args[1]);
			break;
		case IR_OP_COMPARE_NOT_EQUAL:
			(**dst).opcode = ASM_OP_MOV;
			codegen_set_operand_immediate_zero(&(**dst).args[0]);
			(**dst).args[1] = OPERAND_RCX_64BIT;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_SET_IF_P;
			(**dst).args[0] = OPERAND_RCX_64BIT;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_SET_IF_NEQ;
			codegen_map_operand(&src->args[2], &(**dst).args[0]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_BITWISE_OR;
			(**dst).args[0] = OPERAND_RCX_64BIT;
			codegen_map_operand(&src->args[2], &(**dst).args[1]);
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		break;
	case IR_OP_CTYPE_DOUBLE_TO_INT:
	case IR_OP_CTYPE_INT_TO_DOUBLE:
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = src->opcode == IR_OP_CTYPE_DOUBLE_TO_INT
		                         ? ASM_OP_CVT_DOUBLE_TO_INT
		                         : ASM_OP_CVT_INT_TO_DOUBLE;
		codegen_map_operands_all(src, *dst);
		if (ctype_is_charlike(&src->args[0].c89type)) {
			assert(src->opcode == IR_OP_CTYPE_INT_TO_DOUBLE);
			assert((**dst).args[0].word_type == ASM_WORD_08BIT);
			struct asm_op *to_prepend = NULL;
			check(codegen_alloc_op(arena, &to_prepend));
			to_prepend->opcode = ASM_OP_MOV_WITH_ZERO_EXTENSION;
			codegen_map_operand(&src->args[0],
			                    &to_prepend->args[0]);
			to_prepend->args[1] = OPERAND_RAX_32BIT;
			(**dst).args[0] = OPERAND_RAX_32BIT;
			codegen_op_list_prepend(to_prepend, dst);
		} else if (ctype_is_charlike(&src->args[1].c89type)) {
			assert(src->opcode == IR_OP_CTYPE_DOUBLE_TO_INT);
			assert((**dst).args[1].word_type == ASM_WORD_08BIT);
			(**dst).args[1] = OPERAND_RAX_32BIT;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_MOV;
			(**dst).args[0] = OPERAND_RAX_08BIT;
			codegen_map_operand(&src->args[1], &(**dst).args[1]);
			assert((**dst).args[1].word_type == ASM_WORD_08BIT);
		}
		break;
	case IR_OP_CTYPE_DOUBLE_TO_UINT:
		/*
		 * Mimic output of clang for double -> unsigned long.
		 *
		 * With `#pragma STDC FENV_ACCESS ON`, clang seems to produce
		 * code similar to gcc and Nora Sandler's book. However, since
		 * we don't currently support compiling code that checks
		 * floating point status flags or sets non-default floating
		 * point control modes, we can copy the branchless approach
		 * clang takes in the optimistic case.
		 *
		 * https://github.com/llvm/llvm-project/issues/8472#issuecomment-980888781
		 * https://github.com/llvm/llvm-project/issues/8472#issuecomment-980888775
		 * https://github.com/llvm/llvm-project/issues/18060#issuecomment-3580497536
		 *
		 * movsd     -8(%rbp), %xmm0    ; source arg at -8(%rbp)
		 * movsd     .Lfoo(%rip), %xmm2 ; INT_MAX as double constant
		 * movq      %xmm0, %xmm1       ; ... or movaps
		 * subsd     %xmm2, %xmm1
		 * cvttsd2si %xmm1, %rcx
		 * cvttsd2si %xmm0, %rax
		 * movq      %rax, %rdx
		 * sarq      $63, %rdx
		 * andq      %rdx, %rcx
		 * orq       %rcx, %rax
		 * movq      %rax, -16(%rbp)    ; result at -16(%rbp)
		 */
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		(**dst).args[1] = OPERAND_XMM0;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0].operand_type = ASM_OPERAND_CONSTANT_DATA_DOUBLE;
		(**dst).args[0].u.dnum = (double)INT_MAX;
		(**dst).args[1] = OPERAND_XMM2;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0] = OPERAND_XMM0;
		(**dst).args[1] = OPERAND_XMM1;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_DOUBLE_BINARY_SUBTRACT;
		(**dst).args[0] = OPERAND_XMM2;
		(**dst).args[1] = OPERAND_XMM1;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_CVT_DOUBLE_TO_INT;
		(**dst).args[0] = OPERAND_XMM1;
		(**dst).args[1] = OPERAND_RCX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_CVT_DOUBLE_TO_INT;
		(**dst).args[0] = OPERAND_XMM0;
		(**dst).args[1] = OPERAND_RAX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0] = OPERAND_RAX_64BIT;
		(**dst).args[1] = OPERAND_RDX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_BITWISE_SIGNED_SHIFT_RIGHT;
		(**dst).args[0].operand_type = ASM_OPERAND_IMMEDIATE;
		(**dst).args[0].u.num = 64 - 1;
		(**dst).args[1] = OPERAND_RDX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_BITWISE_AND;
		(**dst).args[0] = OPERAND_RDX_64BIT;
		(**dst).args[1] = OPERAND_RCX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_BITWISE_OR;
		(**dst).args[0] = OPERAND_RCX_64BIT;
		(**dst).args[1] = OPERAND_RAX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		(**dst).args[0] = OPERAND_RAX_64BIT;
		codegen_map_operand(&src->args[1], &(**dst).args[1]);
		break;
	case IR_OP_CTYPE_UINT_TO_DOUBLE:
		check(codegen_alloc_op(arena, dst));
		if (src->args[0].c89type.t == CTYPE_UNSIGNED_CHAR ||
		    src->args[0].c89type.t == CTYPE_UNSIGNED_INT) {
			(**dst).opcode =
				ctype_is_charlike(&src->args[0].c89type)
					? ASM_OP_MOV_WITH_ZERO_EXTENSION
					: ASM_OP_MOV;
			codegen_map_operand(&src->args[0], &(**dst).args[0]);
			codegen_set_operand_eax(&src->args[0],
			                        &(**dst).args[1]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_CVT_INT_TO_DOUBLE;
			(**dst).args[0] = OPERAND_RAX_64BIT;
			codegen_map_operand(&src->args[1], &(**dst).args[1]);
		} else {
			assert(src->args[0].c89type.t == CTYPE_UNSIGNED_LONG);
			/*
			 * Mimic output of clang for unsigned long -> double.
			 *
			 * movq      -8(%rbp), %xmm0   ; source arg at -8(%rbp)
			 * punpckldq 0x00000000000000004530000043300000, %xmm0
			 * subpd     0x4330000000000000, %xmm0
			 * movq      %xmm0, %xmm1      ; ... or movaps
			 * unpckhpd  %xmm0, %xmm0
			 * addsd     %xmm1, %xmm0
			 * movsd     %xmm0, -16(%rbp)  ; result at -16(%rbp)
			 *
			 * See comment on IR_OP_CTYPE_DOUBLE_TO_UINT as well.
			 */
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&src->args[0], &(**dst).args[0]);
			(**dst).args[1] = OPERAND_XMM0;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_VEC_DOUBLE_UNPACK_INTERLEAVE_LO;
			(**dst).args[0].operand_type =
				ASM_OPERAND_CONSTANT_DATA_VEC_LONGS;
			(**dst).args[0].u.longs[0] = UINT_TO_DOUBLE_MAGIC_L1;
			(**dst).args[0].u.longs[1] = UINT_TO_DOUBLE_MAGIC_L2;
			(**dst).args[1] = OPERAND_XMM0;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_VEC_DOUBLE_BINARY_SUBTRACT;
			(**dst).args[0].operand_type =
				ASM_OPERAND_CONSTANT_DATA_VEC_QUADS;
			(**dst).args[0].u.quads[0] = UINT_TO_DOUBLE_MAGIC_Q1;
			(**dst).args[0].u.quads[1] = UINT_TO_DOUBLE_MAGIC_Q2;
			(**dst).args[1] = OPERAND_XMM0;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_MOV;
			(**dst).args[0] = OPERAND_XMM0;
			(**dst).args[1] = OPERAND_XMM1;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_VEC_DOUBLE_UNPACK_INTERLEAVE_HI;
			(**dst).args[0] = OPERAND_XMM0;
			(**dst).args[1] = OPERAND_XMM0;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_DOUBLE_BINARY_ADD;
			(**dst).args[0] = OPERAND_XMM1;
			(**dst).args[1] = OPERAND_XMM0;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_MOV;
			(**dst).args[0] = OPERAND_XMM0;
			codegen_map_operand(&src->args[1], &(**dst).args[1]);
		}
		break;
	case IR_OP_JUMP_IF_ZERO:
	case IR_OP_JUMP_IF_NOT_ZERO:
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		(**dst).args[1] = OPERAND_RAX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		(**dst).args[1] = OPERAND_RCX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_DOUBLE_COMPARE;
		(**dst).args[0].operand_type = ASM_OPERAND_CONSTANT_DATA_DOUBLE;
		(**dst).args[0].u.dnum = 0.0;
		codegen_map_operand(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_SET_IF_NEQ;
		(**dst).args[0] = OPERAND_RAX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_SET_IF_P;
		(**dst).args[0] = OPERAND_RCX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_BITWISE_OR;
		(**dst).args[0] = OPERAND_RAX_64BIT;
		(**dst).args[1] = OPERAND_RCX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_COMPARE;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		(**dst).args[1] = OPERAND_RCX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = src->opcode == IR_OP_JUMP_IF_ZERO
		                         ? ASM_OP_JMP_IF_EQ
		                         : ASM_OP_JMP_IF_NEQ;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		break;
	default:
		break; /* fallback to common handling in caller */
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_statement_one(Arena *arena,
                      const struct ir_op *src,
                      struct asm_op **dst)
{
	assert(*dst == NULL);
	if (ctype_is_floating_point(&src->args[0].c89type) ||
	    ctype_is_floating_point(&src->args[1].c89type)) {
		check(codegen_statement_fp(arena, src, dst));
		if (*dst != NULL) {
			return RESULT_OK;
		} /* else, fallthrough to common handling */
	}

	check(codegen_alloc_op(arena, dst));

	/* guess overall op signedness ahead of time */
	const bool a_signed = ctype_is_signed(&src->args[0].c89type);

	switch (src->opcode) {
	case IR_OP_RET:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_RET;
		break;
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_COMPLEMENT:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operands_all(src, *dst);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = src->opcode == IR_OP_UNARY_NEGATE
		                         ? ASM_OP_UNARY_NEG
		                         : ASM_OP_UNARY_NOT;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		break;
	case IR_OP_UNARY_DECREMENT:
	case IR_OP_UNARY_INCREMENT:
		(**dst).opcode = src->opcode == IR_OP_UNARY_DECREMENT
		                         ? ASM_OP_UNARY_DECREMENT
		                         : ASM_OP_UNARY_INCREMENT;
		assert(in_place_update(src, 1));
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		break;
	case IR_OP_BINARY_ADD:
	case IR_OP_BINARY_SUBTRACT:
	case IR_OP_BINARY_MULTIPLY:
	case IR_OP_BITWISE_AND:
	case IR_OP_BITWISE_OR:
	case IR_OP_BITWISE_XOR:
	case IR_OP_BITWISE_SHIFT_LEFT:
	case IR_OP_BITWISE_SHIFT_RIGHT:
		if (!in_place_update(src, 2)) {
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&src->args[0], &(**dst).args[0]);
			codegen_map_operand(&src->args[2], &(**dst).args[1]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
		}
		switch (src->opcode) {
		case IR_OP_BINARY_ADD:
			(**dst).opcode = ASM_OP_BINARY_ADD;
			break;
		case IR_OP_BINARY_SUBTRACT:
			(**dst).opcode = ASM_OP_BINARY_SUBTRACT;
			break;
		case IR_OP_BINARY_MULTIPLY:
			(**dst).opcode = ASM_OP_BINARY_MULTIPLY;
			break;
		case IR_OP_BITWISE_AND:
			(**dst).opcode = ASM_OP_BITWISE_AND;
			break;
		case IR_OP_BITWISE_OR:
			(**dst).opcode = ASM_OP_BITWISE_OR;
			break;
		case IR_OP_BITWISE_XOR:
			(**dst).opcode = ASM_OP_BITWISE_XOR;
			break;
		case IR_OP_BITWISE_SHIFT_LEFT:
			(**dst).opcode =
				a_signed ? ASM_OP_BITWISE_SIGNED_SHIFT_LEFT
					 : ASM_OP_BITWISE_UNSIGNED_SHIFT_LEFT;
			break;
		case IR_OP_BITWISE_SHIFT_RIGHT:
			(**dst).opcode =
				a_signed ? ASM_OP_BITWISE_SIGNED_SHIFT_RIGHT
					 : ASM_OP_BITWISE_UNSIGNED_SHIFT_RIGHT;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		break;
	case IR_OP_BINARY_DIVIDE:
	case IR_OP_BINARY_REMAINDER:
		/* copy dividend to eax */
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		/* sign-extend dividend from eax into edx */
		check(codegen_alloc_op(arena, dst));
		switch (src->args[0].c89type.t) {
		case CTYPE_CHAR:
		case CTYPE_SIGNED_CHAR:
		case CTYPE_UNSIGNED_CHAR:
			assert(0 && "char div should have been cast to int");
			break;
		case CTYPE_INT:
			(**dst).opcode = ASM_OP_CDQ;
			break;
		case CTYPE_LONG:
			(**dst).opcode = ASM_OP_CQO;
			break;
		case CTYPE_UNSIGNED_INT:
		case CTYPE_UNSIGNED_LONG:
			(**dst).opcode = ASM_OP_MOV;
			codegen_set_operand_immediate_zero(&(**dst).args[0]);
			(**dst).args[1] = OPERAND_RDX_64BIT;
			break;
		case CTYPE_DOUBLE:
			assert(0 && "double div/rem should lead elsewhere");
			break;
		case CTYPE_POINTER_TO:
		case CTYPE_ARRAY_OF:
			assert(0 && "ptr div/rem should have been rejected");
			break;
		}
		dst = &(**dst).next;
		/* prepare divisor and idiv op */
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = a_signed ? ASM_OP_IDIV : ASM_OP_DIV;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		assert(ctype_is_equal(&src->args[0].c89type,
		                      &src->args[1].c89type));
		dst = &(**dst).next;
		/* copy result from eax (quotient) or edx (remainder) */
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		switch (src->opcode) {
		case IR_OP_BINARY_DIVIDE:
			codegen_set_operand_eax(&src->args[0],
			                        &(**dst).args[0]);
			break;
		case IR_OP_BINARY_REMAINDER:
			codegen_set_operand_register(&src->args[0],
			                             ASM_REGISTER_DX,
			                             &(**dst).args[0]);
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		break;
	case IR_OP_UNARY_NOT:
		(**dst).opcode = ASM_OP_COMPARE;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		codegen_map_operand(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		codegen_map_operand(&src->args[1], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_SET_IF_EQ;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		break;
	case IR_OP_COMPARE_EQUAL:
	case IR_OP_COMPARE_NOT_EQUAL:
	case IR_OP_COMPARE_LESS_THAN:
	case IR_OP_COMPARE_LESS_THAN_EQ:
	case IR_OP_COMPARE_MORE_THAN:
	case IR_OP_COMPARE_MORE_THAN_EQ:
		(**dst).opcode = ASM_OP_COMPARE;
		/* note inverted arg order: IR_OP_COMPARE_* -> ASM_OP_COMPARE */
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		codegen_map_operand(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		switch (src->opcode) {
		case IR_OP_COMPARE_EQUAL:
			(**dst).opcode = ASM_OP_SET_IF_EQ;
			break;
		case IR_OP_COMPARE_NOT_EQUAL:
			(**dst).opcode = ASM_OP_SET_IF_NEQ;
			break;
		case IR_OP_COMPARE_LESS_THAN:
			(**dst).opcode =
				a_signed ? ASM_OP_SET_IF_LT : ASM_OP_SET_IF_B;
			break;
		case IR_OP_COMPARE_LESS_THAN_EQ:
			(**dst).opcode =
				a_signed ? ASM_OP_SET_IF_LTE : ASM_OP_SET_IF_BE;
			break;
		case IR_OP_COMPARE_MORE_THAN:
			(**dst).opcode =
				a_signed ? ASM_OP_SET_IF_GT : ASM_OP_SET_IF_A;
			break;
		case IR_OP_COMPARE_MORE_THAN_EQ:
			(**dst).opcode =
				a_signed ? ASM_OP_SET_IF_GTE : ASM_OP_SET_IF_AE;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[2], &(**dst).args[0]);
		break;
	case IR_OP_COPY:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operands_all(src, *dst);
		break;
	case IR_OP_CTYPE_SIGN_EXTEND:
	case IR_OP_CTYPE_ZERO_EXTEND:
		(**dst).opcode = src->opcode == IR_OP_CTYPE_SIGN_EXTEND
		                         ? ASM_OP_MOV_WITH_SIGN_EXTENSION
		                         : ASM_OP_MOV_WITH_ZERO_EXTENSION;
		codegen_map_operands_all(src, *dst);
		assert((**dst).args[0].word_type < ASM_WORD_64BIT);
		assert((**dst).args[0].word_type < (**dst).args[1].word_type);
		break;
	case IR_OP_CTYPE_TRUNCATE:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operands_all(src, *dst);
		assert((**dst).args[1].word_type < ASM_WORD_64BIT);
		assert((**dst).args[0].word_type > (**dst).args[1].word_type);
		/* truncate -> use CTYPE_INT/CTYPE_CHAR's's worth of source */
		(**dst).args[0].word_type = (**dst).args[1].word_type;
		break;
	case IR_OP_CTYPE_DOUBLE_TO_INT:
	case IR_OP_CTYPE_DOUBLE_TO_UINT:
	case IR_OP_CTYPE_INT_TO_DOUBLE:
	case IR_OP_CTYPE_UINT_TO_DOUBLE:
		assert(0); /* should be handled by codegen_statement_fp() */
		break;
	case IR_OP_GET_ADDRESS:
		(**dst).opcode = ASM_OP_LEA;
		codegen_map_operands_all(src, *dst);
		assert((**dst).args[1].word_type == ASM_WORD_64BIT);
		break;
	case IR_OP_LOAD:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&src->args[0], &(**dst).args[1]);
		/* IR_OP_LOAD should operate on a 64-bit pointer */
		(**dst).args[1].word_type = ASM_WORD_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_memory(&src->args[0],
		                           ASM_REGISTER_AX,
		                           0,
		                           &(**dst).args[0]);
		codegen_map_operand(&src->args[1], &(**dst).args[1]);
		break;
	case IR_OP_STORE:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		codegen_set_operand_eax(&src->args[1], &(**dst).args[1]);
		assert((**dst).args[0].word_type == ASM_WORD_64BIT);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_memory(&src->args[0],
		                           ASM_REGISTER_AX,
		                           0,
		                           &(**dst).args[1]);
		break;
	case IR_OP_POINTER_ADD:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		(**dst).args[1] = OPERAND_RAX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		(**dst).args[1] = OPERAND_RDX_64BIT;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		assert(src->args[2].subtype == IR_VAL_CONSTANT);
		if (src->args[2].num == 1 || src->args[2].num == 2 ||
		    src->args[2].num == 4 || src->args[2].num == 8) {
			(**dst).opcode = ASM_OP_LEA;
			codegen_set_operand_indexed(
				&src->args[0],
				(long long int)src->args[2].num,
				&(**dst).args[0]);
			codegen_map_operand(&src->args[3], &(**dst).args[1]);
		} else {
			(**dst).opcode = ASM_OP_BINARY_MULTIPLY;
			codegen_map_operand(&src->args[2], &(**dst).args[0]);
			(**dst).args[1] = OPERAND_RDX_64BIT;
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_LEA;
			codegen_set_operand_indexed(&src->args[0],
			                            1,
			                            &(**dst).args[0]);
			codegen_map_operand(&src->args[3], &(**dst).args[1]);
		}
		/* ASM_OP_LEA should produce a 64-bit pointer */
		(**dst).args[1].word_type = ASM_WORD_64BIT;
		break;
	case IR_OP_JUMP:
		(**dst).opcode = ASM_OP_JMP;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		break;
	case IR_OP_JUMP_IF_ZERO:
	case IR_OP_JUMP_IF_NOT_ZERO:
		(**dst).opcode = ASM_OP_COMPARE;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		codegen_map_operand(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = src->opcode == IR_OP_JUMP_IF_ZERO
		                         ? ASM_OP_JMP_IF_EQ
		                         : ASM_OP_JMP_IF_NEQ;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		break;
	case IR_OP_LABEL:
		(**dst).opcode = ASM_OP_LABEL;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		break;
	case IR_OP_CALL:
		check(codegen_op_call(arena, src, dst));
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_statement(Arena *arena, const struct ir_op *src, struct asm_op **dst)
{
	while (src != NULL) {
		check(codegen_statement_one(arena, src, dst));
		src = src->next;
		while (*dst != NULL) {
			dst = &(**dst).next;
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_copy_reg_to_pseudo(Arena *arena,
                           const struct ir_val *src,
                           long long int pos,
                           struct asm_op **dst)
{
	const bool is_fp = ctype_is_floating_point(&src->c89type);
	const enum asm_register reg = is_fp ? CALL_FP[pos] : CALL_REG[pos];

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	codegen_set_operand_register(src, reg, &(**dst).args[0]);
	codegen_set_operand_pseudo(src, &(**dst).args[1]);
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_copy_stack_to_pseudo(Arena *arena,
                             const struct ir_val *src,
                             long long int stack_pos,
                             struct asm_op **dst)
{
	assert(stack_pos >= 0);
	const long long int stack_offset =
		CODEGEN_BYTES_ARG_FIRST + (CODEGEN_BYTES_PER_PUSH * stack_pos);
	assert(stack_offset % CODEGEN_BYTES_PER_VALUE == 0);

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	codegen_set_operand_stack(src, stack_offset, &(**dst).args[0]);
	codegen_set_operand_pseudo(src, &(**dst).args[1]);

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function_params(Arena *arena,
                        const struct ir_val *ir,
                        struct asm_op **dst)
{
	long long int n_general = 0;
	long long int n_double = 0;
	long long int n_stack = 0;
	for (long long int i = 0; i < FUNCTION_PARAMETER_LIMIT; ++i) {
		if (ir[i].subtype == IR_VAL_NONE) {
			break;
		}

		const bool is_fp = ctype_is_floating_point(&ir[i].c89type);
		if (is_fp && n_double < CODEGEN_FP_REGISTER_ARGS) {
			check(codegen_copy_reg_to_pseudo(arena,
			                                 &ir[i],
			                                 n_double,
			                                 dst));
		} else if (!is_fp && n_general < CODEGEN_REGISTER_ARGS) {
			check(codegen_copy_reg_to_pseudo(arena,
			                                 &ir[i],
			                                 n_general,
			                                 dst));
		} else {
			check(codegen_copy_stack_to_pseudo(arena,
			                                   &ir[i],
			                                   n_stack++,
			                                   dst));
		}

		if (is_fp) {
			++n_double;
		} else {
			++n_general;
		}

		dst = &(**dst).next;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function(Arena *arena,
                 const struct ir_function *ir,
                 struct asm_function **dst)
{
	assert(dst != NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_CODEGEN_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	(**dst).identifier = ir->identifier;
	(**dst).linkage = codegen_map_linkage(ir->linkage);

	struct asm_op **dst_ops = &(**dst).ops;
	assert(*dst_ops == NULL);
	check(codegen_function_params(arena, ir->params, dst_ops));

	while (*dst_ops != NULL) {
		dst_ops = &(**dst_ops).next;
	}

	assert(*dst_ops == NULL);
	check(codegen_statement(arena, ir->ops, dst_ops));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_variable(Arena *arena,
                 const struct ir_variable *ir,
                 struct asm_variable **dst)
{
	assert(dst != NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_CODEGEN_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	(**dst).identifier = ir->identifier;
	(**dst).linkage = codegen_map_linkage(ir->linkage);
	(**dst).initializer = ir->initializer;
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_string_literal(Arena *arena,
                       const struct ir_str *ir,
                       struct asm_str **dst)
{
	assert(dst != NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_CODEGEN_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	(**dst).string_unique = ir->string_unique;
	(**dst).initializer = ir->initializer;
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_program(Arena *arena,
                const struct intermediate *ir,
                struct assembly **dst)
{
	struct asm_function **dst_fun = &(**dst).functions;
	for (struct ir_function *f = ir->functions; f != NULL; f = f->next) {
		check(codegen_function(arena, f, dst_fun));
		assert(*dst_fun != NULL);
		dst_fun = &(**dst_fun).next;
	}

	struct asm_variable **dst_var = &(**dst).variables;
	for (struct ir_variable *v = ir->variables; v != NULL; v = v->next) {
		check(codegen_variable(arena, v, dst_var));
		assert(*dst_var != NULL);
		dst_var = &(**dst_var).next;
	}

	struct asm_str **dst_str = &(**dst).string_literals;
	for (struct ir_str *s = ir->string_literals; s != NULL; s = s->next) {
		check(codegen_string_literal(arena, s, dst_str));
		assert(*dst_str != NULL);
		dst_str = &(**dst_str).next;
	}

	return RESULT_OK;
}

result_t
codegen_init(Arena *arena, const struct intermediate *ir, struct assembly **cg)
{
	*cg = arena_alloc(arena, sizeof(**cg));
	check_if(*cg == NULL, ERR_CODEGEN_ALLOC);
	memset(*cg, 0, sizeof(**cg));
	check(codegen_program(arena, ir, cg));
	return RESULT_OK;
}

static WARN_UNUSED long long int
round_up_to_multiple_of(long long int n, long long int base)
{
	const long long int rounded = (((n + base - 1) / base)) * base;
	assert(rounded >= n);
	assert(rounded - n < base);
	assert(rounded % base == 0);
	return rounded;
}

static WARN_UNUSED result_t
codegen_replace_pseudo_fn(struct asm_function *cg,
                          long long int range[2],
                          long long int *offsets,
                          bool preflight)
{
	long long int cursor = 0;
	for (struct asm_op *op = cg->ops; op != NULL; op = op->next) {
		for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
			struct asm_operand *arg = &op->args[i];
			if (arg->operand_type != ASM_OPERAND_PSEUDO_REGISTER &&
			    arg->operand_type != ASM_OPERAND_PSEUDO_MEMORY) {
				continue;
			}

			if (preflight) {
				range[0] = MIN(range[0], arg->u.num);
				range[1] = MAX(range[1], arg->u.num);
				continue;
			}

			assert(arg->u.num >= range[0]);
			assert(arg->u.num <= range[1]);
			assert(range[0] >= 0);
			const int128_t idx = arg->u.num - range[0];

			assert(offsets != NULL);
			const bool got_offset = (offsets[idx] != 0);
			if (got_offset) {
				/* use already-computed offset value for idx */
			} else if (arg->operand_type ==
			           ASM_OPERAND_PSEUDO_MEMORY) {
				cursor += arg->u.pseudo_mem.total_bytes;
				cursor = round_up_to_multiple_of(
					cursor,
					CODEGEN_BYTES_STACK_ALIGN_TOP_OR_ARR);
				offsets[idx] = cursor;
			} else {
				switch (arg->word_type) {
				case ASM_WORD_08BIT:
					cursor += 1;
					break;
				case ASM_WORD_32BIT:
					cursor += CODEGEN_BYTES_PER_VALUE;
					cursor = round_up_to_multiple_of(
						cursor,
						CODEGEN_BYTES_PER_VALUE);
					break;
				case ASM_WORD_64BIT:
					cursor += CODEGEN_BYTES_PER_VALUE * 2;
					cursor = round_up_to_multiple_of(
						cursor,
						CODEGEN_BYTES_PER_PUSH);
					break;
				}
				offsets[idx] = cursor;
			}

			long long int computed_offset = offsets[idx];
			if (arg->operand_type == ASM_OPERAND_PSEUDO_MEMORY) {
				computed_offset -= arg->u.pseudo_mem.offset;
				assert(computed_offset > 0);
			}

			arg->operand_type = ASM_OPERAND_MEMORY;
			arg->u.mem.offset = -1 * computed_offset;
			arg->u.mem.reg = ASM_REGISTER_RBP;
			/* leave arg->word_type as-is */
		}
	}
	return RESULT_OK;
}

result_t
codegen_replace_pseudo(Arena *arena, struct assembly *cg)
{
	debug("Replacing pseudovalues with stack addresses");

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		long long int range[2] = {LLONG_MAX, LLONG_MIN};
		check(codegen_replace_pseudo_fn(f, range, NULL, true));

		if (range[0] == LLONG_MAX || range[1] == LLONG_MIN) {
			continue;
		}

		const long long int size = 1 + range[1] - range[0];
		assert(size > 0);
		assert(size <= 4096); /* if exceeded, refactor */

		long long int *off = arena_alloc(arena, sizeof(*off) * size);
		check(codegen_replace_pseudo_fn(f, range, off, false));

		assert(f->stack_usage == 0);
		for (long long int i = 0; i < size; ++i) {
			f->stack_usage = MAX(f->stack_usage, off[i]);
		}
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_fixup_alloc_stack(Arena *arena, struct asm_function *cg)
{
	if (cg->stack_usage == 0) {
		return RESULT_OK;
	}

	struct asm_op *alloc_stack = NULL;
	const long long int fix =
		round_up_to_multiple_of(cg->stack_usage,
	                                CODEGEN_BYTES_STACK_ALIGN_TOP_OR_ARR);
	check(codegen_alloc_subq_rsp(arena, &alloc_stack, fix));
	codegen_op_list_prepend(alloc_stack, &cg->ops);
	return RESULT_OK;
}

struct fix {
	size_t sz;
	struct asm_op *ops[3];
};

typedef bool (*fixer)(struct asm_op *cur, struct fix *trampoline);

static WARN_UNUSED result_t
codegen_fixup_apply(Arena *arena,
                    struct asm_function *cg,
                    struct asm_op **new_prev,
                    struct asm_op **new_cur,
                    fixer fix_init)
{
	struct asm_op *prev = *new_prev;
	struct asm_op *cur = *new_cur;

	struct fix trampoline = {0};
	for (size_t i = 0; i < ARRAY_SIZE(trampoline.ops); ++i) {
		check(codegen_alloc_op(arena, &trampoline.ops[i]));
	}

	if (!fix_init(cur, &trampoline)) {
		return RESULT_OK; /* no fixup necessary */
	}

	assert(cur);
	if (prev == NULL) {
		assert(cg->ops == cur);
	} else {
		assert(prev->next == cur);
	}

	/*
	 * Split containing list at <cur>, excluding <cur> from both halves.
	 */
	struct asm_op *remainder = cur->next;
	cur->next = NULL;
	if (prev != NULL) {
		prev->next = NULL;
	}

	assert(trampoline.sz >= 1);
	if (prev == NULL) {
		/*
		 * Set trampoline as new head of containing list.
		 */
		cg->ops = trampoline.ops[0];
	} else {
		/*
		 * Insert trampoline sublist into containing list.
		 */
		codegen_op_list_concat(prev, trampoline.ops[0]);
	}
	assert(trampoline.sz >= 2);
	codegen_op_list_concat(trampoline.ops[0], trampoline.ops[1]);
	if (trampoline.sz >= 3) {
		codegen_op_list_concat(trampoline.ops[1], trampoline.ops[2]);
	}
	codegen_op_list_concat(trampoline.ops[trampoline.sz - 1], remainder);

	/*
	 * Prepare list cursor positions for next loop iteration.
	 */
	*new_prev = trampoline.ops[trampoline.sz - 1];
	*new_cur = remainder;

	/*
	 * Release ownership of trampoline sublist to caller.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(trampoline.ops); ++i) {
		trampoline.ops[i] = NULL;
	}

	return RESULT_OK;
}

static WARN_UNUSED bool
in_memory(struct asm_operand *o)
{
	return o->operand_type == ASM_OPERAND_MEMORY ||
	       o->operand_type == ASM_OPERAND_VARIABLE_DATA ||
	       o->operand_type == ASM_OPERAND_CONSTANT_DATA_DOUBLE ||
	       o->operand_type == ASM_OPERAND_CONSTANT_DATA_VEC_LONGS ||
	       o->operand_type == ASM_OPERAND_CONSTANT_DATA_VEC_QUADS ||
	       o->operand_type == ASM_OPERAND_CONSTANT_STRING;
}

/*
 * Prepare a trampoline by memcpy()-ing invalid instructions where both
 * operands are ASM_OPERAND_MEMORY:
 *
 *     movl -4(%rbp), -8(%rbp)
 *
 * ... into temporary copies. Modify these temporaries to perform the same
 * logical operation in an actually-valid way:
 *
 *     movl -4(%rbp), %r10d
 *     movl %r10d, -8(%rbp)
 *
 * ... and then splice these new ops into the original containing list.
 */
static WARN_UNUSED bool
fix_s2s(struct asm_op *cur, struct fix *trampoline)
{
	const bool binary_ish_opcode = cur->opcode == ASM_OP_BINARY_ADD ||
	                               cur->opcode == ASM_OP_BINARY_SUBTRACT ||
	                               cur->opcode == ASM_OP_BITWISE_AND ||
	                               cur->opcode == ASM_OP_BITWISE_OR ||
	                               cur->opcode == ASM_OP_BITWISE_XOR ||
	                               cur->opcode == ASM_OP_COMPARE;
	const bool candidate_opcode =
		cur->opcode == ASM_OP_MOV || binary_ish_opcode;
	const bool candidate_operand_0 = in_memory(&cur->args[0]);
	const bool candidate_operand_1 = in_memory(&cur->args[1]);
	if (!(candidate_opcode && candidate_operand_0 && candidate_operand_1)) {
		return false;
	}

	trampoline->sz = 2;
	for (size_t i = 0; i < trampoline->sz; ++i) {
		memcpy(trampoline->ops[i], cur, sizeof(*cur));
		trampoline->ops[i]->next = NULL;
	}
	if (binary_ish_opcode) {
		trampoline->ops[0]->opcode = ASM_OP_MOV;
	}
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[0]->args[1]);
	codegen_set_operand_r10(&cur->args[1], &trampoline->ops[1]->args[0]);

	return true;
}

/*
 * An immediate (constant) value that does not fit into an int needs to bounce
 * through a register before an arithmetic op can use it as an operand.
 *
 * Ditto for ASM_OP_MOV op with a large immediate value as a source and an
 * ASM_OPERAND_MEMORY as a destination.
 *
 * Loading an immediate value into an XMM* register typically uses a global
 * constant as a source, but if necessary, bouncing through a general purpose
 * register seems to work as well.
 */
static WARN_UNUSED bool
fix_imm_big(struct asm_op *cur, struct fix *trampoline)
{
	if (!(((cur->opcode == ASM_OP_BINARY_ADD ||
	        cur->opcode == ASM_OP_BINARY_SUBTRACT ||
	        cur->opcode == ASM_OP_BINARY_MULTIPLY ||
	        cur->opcode == ASM_OP_BITWISE_AND ||
	        cur->opcode == ASM_OP_BITWISE_OR ||
	        cur->opcode == ASM_OP_BITWISE_XOR ||
	        cur->opcode == ASM_OP_COMPARE || /* cmpq  */
	        cur->opcode == ASM_OP_PUSH) &&   /* pushq */
	       cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE &&
	       /*
	        * An ASM_WORD_64BIT immediate value can clearly exceed INT_MAX,
	        * but note: an unsigned value in ASM_WORD_32BIT can, as well!
	        */
	       cur->args[0].u.num > INT_MAX) ||
	      (cur->opcode == ASM_OP_MOV &&
	       cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE &&
	       cur->args[0].u.num > INT_MAX && in_memory(&cur->args[1])) ||
	      (cur->opcode == ASM_OP_MOV &&
	       cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE &&
	       is_xmm_register(&cur->args[1])))) {
		return false;
	}

	trampoline->sz = 2;
	for (size_t i = 0; i < trampoline->sz; ++i) {
		memcpy(trampoline->ops[i], cur, sizeof(*cur));
		trampoline->ops[i]->next = NULL;
	}
	trampoline->ops[0]->opcode = ASM_OP_MOV;
	trampoline->ops[0]->args[1] = OPERAND_R10_64BIT;
	trampoline->ops[1]->args[0] = OPERAND_R10_64BIT;

	return true;
}

/*
 * Translate:
 *
 *     cmpl %eax, $5
 *
 * ... into:
 *
 *     movl $5, $r11d
 *     cmpl %eax, %r11d
 */
static WARN_UNUSED bool
fix_cmp(struct asm_op *cur, struct fix *trampoline)
{
	if (!(cur->opcode == ASM_OP_COMPARE &&
	      cur->args[1].operand_type == ASM_OPERAND_IMMEDIATE)) {
		return false;
	}

	trampoline->sz = 2;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[1], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r11(&cur->args[1], &trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = ASM_OP_COMPARE;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[1]->args[0]);
	codegen_set_operand_r11(&cur->args[0], &trampoline->ops[1]->args[1]);

	return true;
}

/*
 * Translate:
 *
 *     idivl $3
 *
 * ... into:
 *
 *     movl  $3, $r10d
 *     idivl %r10d
 */
static WARN_UNUSED bool
fix_div(struct asm_op *cur, struct fix *trampoline)
{
	if (!((cur->opcode == ASM_OP_IDIV || cur->opcode == ASM_OP_DIV) &&
	      cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE)) {
		return false;
	}

	trampoline->sz = 2;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = cur->opcode;
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[1]->args[0]);

	return true;
}

/*
 * Translate:
 *
 *     imull $3, -4(%rbp)
 *
 * ... into:
 *
 *     movl  -4(%rbp), $r11d
 *     imull $3, %r11d
 *     movl  $r11d, -4(%rbp)
 */
static WARN_UNUSED bool
fix_mul(struct asm_op *cur, struct fix *trampoline)
{
	if (!(cur->opcode == ASM_OP_BINARY_MULTIPLY &&
	      in_memory(&cur->args[1]))) {
		return false;
	}

	trampoline->sz = 3;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[1], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r11(&cur->args[1], &trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = ASM_OP_BINARY_MULTIPLY;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[1]->args[0]);
	codegen_set_operand_r11(&cur->args[0], &trampoline->ops[1]->args[1]);

	trampoline->ops[2]->opcode = ASM_OP_MOV;
	codegen_set_operand_r11(&cur->args[1], &trampoline->ops[2]->args[0]);
	codegen_copy_operand(&cur->args[1], &trampoline->ops[2]->args[1]);

	return true;
}

/*
 * Translate:
 *
 *     sall -4(%rbp), %eax
 *
 * ... into:
 *
 *     movl -4(%rbp), %cl
 *     sall %cl, %eax
 */
static WARN_UNUSED bool
fix_shift(struct asm_op *cur, struct fix *trampoline)
{
	if (!((cur->opcode == ASM_OP_BITWISE_SIGNED_SHIFT_LEFT ||
	       cur->opcode == ASM_OP_BITWISE_SIGNED_SHIFT_RIGHT ||
	       cur->opcode == ASM_OP_BITWISE_UNSIGNED_SHIFT_LEFT ||
	       cur->opcode == ASM_OP_BITWISE_UNSIGNED_SHIFT_RIGHT) &&
	      cur->args[0].operand_type != ASM_OPERAND_IMMEDIATE)) {
		return false;
	}

	trampoline->sz = 2;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	codegen_set_operand_ecx(&cur->args[0], &trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = cur->opcode;
	codegen_set_operand_ecx(&cur->args[1], &trampoline->ops[1]->args[0]);
	codegen_copy_operand(&cur->args[1], &trampoline->ops[1]->args[1]);

	return true;
}

/*
 * Translate:
 *
 *     movslq $10, -16(%rbp)
 *
 * ... into:
 *
 *     movl   $10, %r10d
 *     movslq %r10d, %r11 # note: src uses 32-bit alias, dst uses 64-bit alias
 *     movq   %r11, -16(%rbp)
 */
static WARN_UNUSED bool
fix_movsx_and_movzx(struct asm_op *cur, struct fix *trampoline)
{
	if (!(((cur->opcode == ASM_OP_MOV_WITH_SIGN_EXTENSION &&
	        cur->args[0].word_type < ASM_WORD_64BIT) ||
	       (cur->opcode == ASM_OP_MOV_WITH_ZERO_EXTENSION &&
	        cur->args[0].word_type < ASM_WORD_32BIT)) &&
	      (cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE ||
	       in_memory(&cur->args[1])))) {
		return false;
	}

	trampoline->sz = 3;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	assert(trampoline->ops[0]->args[0].word_type < ASM_WORD_64BIT);
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[0]->args[1]);
	assert(trampoline->ops[0]->args[0].word_type ==
	       trampoline->ops[0]->args[1].word_type);

	trampoline->ops[1]->opcode = cur->opcode;
	codegen_copy_operand(&trampoline->ops[0]->args[1],
	                     &trampoline->ops[1]->args[0]);
	codegen_set_operand_r11(&cur->args[1], &trampoline->ops[1]->args[1]);
	assert(trampoline->ops[1]->args[0].word_type <
	       trampoline->ops[1]->args[1].word_type);

	trampoline->ops[2]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&trampoline->ops[1]->args[1],
	                     &trampoline->ops[2]->args[0]);
	codegen_copy_operand(&cur->args[1], &trampoline->ops[2]->args[1]);
	assert(trampoline->ops[2]->args[0].word_type ==
	       trampoline->ops[2]->args[1].word_type);
	assert(trampoline->ops[0]->args[0].word_type <
	       trampoline->ops[2]->args[1].word_type);

	return true;
}

/*
 * Translate the simple case:
 *
 *     movzx -16(%rbp), %rax
 *
 * ... into:
 *
 *     movl -16(%rbp), %eax
 *
 * Translate the slightly more complicated case:
 *
 *     movzx $10, -16(%rbp)
 *
 * ... into:
 *
 *     movl $10, %r11d
 *     movq %r11, -16(%rbp)
 */
static WARN_UNUSED bool
fix_movzx_32bit(struct asm_op *cur, struct fix *trampoline)
{
	if (cur->opcode != ASM_OP_MOV_WITH_ZERO_EXTENSION) {
		return false;
	}

	if (cur->args[0].word_type == ASM_WORD_08BIT) {
		/* handled by fix_movsx_and_movzx() */
		return false;
	}

	assert(cur->args[0].word_type == ASM_WORD_32BIT);

	if (cur->args[1].operand_type == ASM_OPERAND_REGISTER) {
		trampoline->sz = 1;
		memcpy(trampoline->ops[0], cur, sizeof(*cur));
		trampoline->ops[0]->next = NULL;
		trampoline->ops[0]->opcode = ASM_OP_MOV;
	} else {
		trampoline->sz = 2;
		for (size_t i = 0; i < trampoline->sz; ++i) {
			memcpy(trampoline->ops[i], cur, sizeof(*cur));
			trampoline->ops[i]->next = NULL;
			trampoline->ops[i]->opcode = ASM_OP_MOV;
		}
		codegen_set_operand_r11(&cur->args[0],
		                        &trampoline->ops[0]->args[1]);
		assert(trampoline->ops[0]->args[1].word_type < ASM_WORD_64BIT);
		codegen_set_operand_r11(&cur->args[1],
		                        &trampoline->ops[1]->args[0]);
		assert(trampoline->ops[0]->args[1].word_type <
		       trampoline->ops[1]->args[0].word_type);
	}

	return true;
}

/*
 * Translate:
 *
 *     vcvttsd2siq -8(%rbp), -16(%rbp)
 *
 * ... into:
 *
 *     vcvttsd2siq -8(%rbp), %r11
 *     movq        %r11, -16(%rbp)
 */
static WARN_UNUSED bool
fix_cvt_double_to_int(struct asm_op *cur, struct fix *trampoline)
{
	if (!(cur->opcode == ASM_OP_CVT_DOUBLE_TO_INT &&
	      in_memory(&cur->args[1]))) {
		return false;
	}

	trampoline->sz = 2;
	for (size_t i = 0; i < trampoline->sz; ++i) {
		memcpy(trampoline->ops[i], cur, sizeof(*cur));
		trampoline->ops[i]->next = NULL;
	}
	codegen_set_operand_r11(&cur->args[0], &trampoline->ops[0]->args[1]);
	trampoline->ops[1]->opcode = ASM_OP_MOV;
	codegen_set_operand_r11(&cur->args[1], &trampoline->ops[1]->args[0]);

	return true;
}

/*
 * Translate:
 *
 *     vcvtsi2sdq $10, -16(%rbp)
 *
 * ... into:
 *
 *     movq       $10, %r10
 *     vcvtsi2sdq %r10, %xmm15
 *     movsd      %xmm15, -16(%rbp)
 */
static WARN_UNUSED bool
fix_cvt_int_to_double(struct asm_op *cur, struct fix *trampoline)
{
	if (!(cur->opcode == ASM_OP_CVT_INT_TO_DOUBLE &&
	      (cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE ||
	       in_memory(&cur->args[1])))) {
		return false;
	}

	trampoline->sz = 3;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = cur->opcode;
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[1]->args[0]);
	trampoline->ops[1]->args[1] = OPERAND_XMM15;

	trampoline->ops[2]->opcode = ASM_OP_MOV;
	trampoline->ops[2]->args[0] = OPERAND_XMM15;
	codegen_copy_operand(&cur->args[1], &trampoline->ops[2]->args[1]);

	return true;
}

/*
 * Translate:
 *
 *     comisd -8(%rbp), -16(%rbp)
 *
 * ... into:
 *
 *     movsd  -16(%rbp), %xmm15
 *     comisd -8(%rbp), %xmm15
 *
 * Ditto for addsd, subsd, mulsd, and divsd.
 */
static WARN_UNUSED bool
fix_arithmetic_on_double(struct asm_op *cur, struct fix *trampoline)
{
	if (!((cur->opcode == ASM_OP_DOUBLE_COMPARE ||
	       cur->opcode == ASM_OP_DOUBLE_BINARY_ADD ||
	       cur->opcode == ASM_OP_DOUBLE_BINARY_SUBTRACT ||
	       cur->opcode == ASM_OP_DOUBLE_BINARY_MULTIPLY ||
	       cur->opcode == ASM_OP_DOUBLE_BINARY_DIVIDE ||
	       cur->opcode == ASM_OP_DOUBLE_BITWISE_XOR) &&
	      in_memory(&cur->args[1]))) {
		return false;
	}

	trampoline->sz = 2;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[1], &trampoline->ops[0]->args[0]);
	trampoline->ops[0]->args[1] = OPERAND_XMM15;

	trampoline->ops[1]->opcode = cur->opcode;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[1]->args[0]);
	trampoline->ops[1]->args[1] = OPERAND_XMM15;

	if (cur->opcode != ASM_OP_DOUBLE_COMPARE) {
		trampoline->sz++;
		trampoline->ops[2]->opcode = ASM_OP_MOV;
		trampoline->ops[2]->args[0] = OPERAND_XMM15;
		codegen_copy_operand(&cur->args[1],
		                     &trampoline->ops[2]->args[1]);
	}

	return true;
}

/*
 * Translate:
 *
 *     leaq -4(%rbp), -16(%rbp)
 *
 * ... into:
 *
 *     leaq -4(%rbp), %r10
 *     movq %r10, -16(%rbp)
 */
static WARN_UNUSED bool
fix_lea(struct asm_op *cur, struct fix *trampoline)
{
	if (!(cur->opcode == ASM_OP_LEA && in_memory(&cur->args[1]))) {
		return false;
	}

	trampoline->sz = 2;

	trampoline->ops[0]->opcode = cur->opcode;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r10(&cur->args[1], &trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = ASM_OP_MOV;
	codegen_set_operand_r10(&cur->args[1], &trampoline->ops[1]->args[0]);
	codegen_copy_operand(&cur->args[1], &trampoline->ops[1]->args[1]);

	return true;
}

static WARN_UNUSED result_t
codegen_fixup_function(Arena *arena, struct asm_function *cg, fixer fix_init)
{
	struct asm_op *prev = NULL;
	struct asm_op *cur = cg->ops;
	while (cur != NULL) {
		struct asm_op *orig[2] = {prev, cur};
		check(codegen_fixup_apply(arena, cg, &prev, &cur, fix_init));
		if (cur == orig[1]) {
			assert(prev == orig[0]);
			prev = cur;
			cur = cur->next; /* no fixup -> default cur update */
		}
	}

	return RESULT_OK;
}

result_t
codegen_fixup_instructions(Arena *arena, struct assembly *cg)
{
	debug("Fixing up invalid instructions");
	const fixer fixers[] = {
		fix_s2s,
		fix_imm_big,
		fix_cmp,
		fix_div,
		fix_mul,
		fix_shift,
		fix_movsx_and_movzx,
		fix_movzx_32bit,
		fix_cvt_double_to_int,
		fix_cvt_int_to_double,
		fix_arithmetic_on_double,
		fix_lea,
	};
	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		check(codegen_fixup_alloc_stack(arena, f));
		for (size_t i = 0; i < ARRAY_SIZE(fixers); ++i) {
			check(codegen_fixup_function(arena, f, fixers[i]));
		}
	}
	return RESULT_OK;
}

#define TO_STR(register_name, b8, b4, b1) #register_name,
static const char *const REGISTER_NAMES[] = {FOREACH_ASM_REGISTER(TO_STR)};
#undef TO_STR

static void
codegen_debug_print_operand(const struct asm_operand *operand)
{
	switch (operand->operand_type) {
	case ASM_OPERAND_NONE:
		return;
	case ASM_OPERAND_IMMEDIATE:
		if (operand->u.num > LLONG_MAX) {
			assert(operand->u.num <= ULLONG_MAX);
			debug("  IMMEDIATE %llu",
			      (long long unsigned)operand->u.num);
		} else {
			debug("  IMMEDIATE %lld", (long long)operand->u.num);
		}
		break;
	case ASM_OPERAND_REGISTER:
		debug("  REGISTER %s", REGISTER_NAMES[operand->u.reg]);
		break;
	case ASM_OPERAND_PSEUDO_REGISTER:
		debug("  PSEUDO %lld", (long long)operand->u.num);
		break;
	case ASM_OPERAND_MEMORY:
		debug("  MEMORY %lld(%s)",
		      operand->u.mem.offset,
		      REGISTER_NAMES[operand->u.mem.reg]);
		break;
	case ASM_OPERAND_PSEUDO_MEMORY:
		debug("  PSEUDOMEM %lld(%lld)",
		      operand->u.pseudo_mem.offset,
		      (long long)operand->u.pseudo_mem.num);
		break;
	case ASM_OPERAND_INDEXED:
		debug("  INDEXED (%s, %s, %lld)",
		      REGISTER_NAMES[operand->u.idx.base],
		      REGISTER_NAMES[operand->u.idx.index],
		      operand->u.idx.scale);
		break;
	case ASM_OPERAND_JUMP_TARGET_LABEL:
		debug("  LABEL %lld", (long long)operand->u.num);
		break;
	case ASM_OPERAND_CALL_TARGET_FUNCTION:
		debug("  FUNCTION %.*s",
		      (int)operand->u.function.sz,
		      operand->u.function.data);
		break;
	case ASM_OPERAND_VARIABLE_DATA:
		debug("  DATA %.*s",
		      (int)operand->u.variable.sz,
		      operand->u.variable.data);
		break;
	case ASM_OPERAND_CONSTANT_DATA_DOUBLE:
		debug("  CONSTANT DOUBLE %f", operand->u.dnum);
		break;
	case ASM_OPERAND_CONSTANT_DATA_VEC_LONGS:
		debug("  CONSTANT VEC LONGS 0x%lx 0x%lx 0x%lx 0x%lx",
		      operand->u.longs[0],
		      operand->u.longs[1],
		      operand->u.longs[2],
		      operand->u.longs[3]);
		break;
	case ASM_OPERAND_CONSTANT_DATA_VEC_QUADS:
		debug("  CONSTANT VEC QUADS 0x%llx 0x%llx",
		      operand->u.quads[0],
		      operand->u.quads[1]);
		break;
	case ASM_OPERAND_CONSTANT_STRING:
		debug("  CONSTANT STRING %lld", (long long)operand->u.num);
		break;
	}

	switch (operand->word_type) {
	case ASM_WORD_08BIT:
		debug("    WORD TYPE: 8-BIT HALFWORD (SINGLE BYTE)");
		break;
	case ASM_WORD_32BIT:
		debug("    WORD TYPE: 32-BIT WORD");
		break;
	case ASM_WORD_64BIT:
		debug("    WORD TYPE: 64-BIT QUADWORD");
		break;
	}
}

#define TO_STR(opcode) #opcode,
static const char *const OPCODE_NAMES[] = {FOREACH_ASM_OPCODE(TO_STR)};
#undef TO_STR

static void
codegen_debug_print_op(const struct asm_op *op)
{
	debug("%s", OPCODE_NAMES[op->opcode]);
	for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
		codegen_debug_print_operand(&op->args[i]);
	}
}

void
codegen_debug_print(const struct assembly *cg)
{
	debug("PROGRAM");

	for (struct asm_variable *v = cg->variables; v != NULL; v = v->next) {
		const struct string_view *vname = &v->identifier;
		debug("VARIABLE %.*s", (int)vname->sz, vname->data);
		debug("  LINKAGE %s",
		      v->linkage == ASM_LINKAGE_EXTERNAL ? "EXTERNAL"
		                                         : "INTERNAL");
		debug("  INITIALIZER");
		constant_debug_print(v->initializer, 4);
	}

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		const struct string_view *fname = &f->identifier;
		debug("FUNCTION %.*s", (int)fname->sz, fname->data);
		debug("  LINKAGE %s",
		      f->linkage == ASM_LINKAGE_EXTERNAL ? "EXTERNAL"
		                                         : "INTERNAL");
		debug("  STACK_USAGE %lld", f->stack_usage);
		for (struct asm_op *op = f->ops; op != NULL; op = op->next) {
			codegen_debug_print_op(op);
		}
	}
}
