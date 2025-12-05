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
#undef TO_ENUM

static const long long int CODEGEN_REGISTER_ARGS = ARRAY_SIZE(CALL_REG);
static const long long int CODEGEN_BYTES_PER_VALUE = 4;
static const long long int CODEGEN_BYTES_PER_PUSH = 8;
static const long long int CODEGEN_BYTES_ARG_FIRST = 16;

static WARN_UNUSED result_t
codegen_alloc_op(Arena *arena, struct asm_op **dst)
{
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if((dst) == NULL, ERR_CODEGEN_ALLOC);
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

static void
codegen_set_operand_immediate_zero(struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_IMMEDIATE;
	dst->u.num = 0;
}

static void
codegen_map_ctype(const struct ir_val *src, struct asm_operand *dst)
{
	switch (src->c89type) {
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
		dst->word_type = ASM_WORD_32BIT;
		break;
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
		dst->word_type = ASM_WORD_64BIT;
		break;
	}
}

static WARN_UNUSED long long int
codegen_get_alignment(const struct ir_variable *ir)
{
	return ctype_to_size_bytes(ir->c89type);
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
static const struct asm_operand OPERAND_R10_32BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_32BIT,
	.u.reg = ASM_REGISTER_R10,
};
static const struct asm_operand OPERAND_R11_64BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_64BIT,
	.u.reg = ASM_REGISTER_R11,
};
static const struct asm_operand OPERAND_R11_32BIT = {
	ASM_OPERAND_REGISTER,
	ASM_WORD_32BIT,
	.u.reg = ASM_REGISTER_R11,
};

static void
codegen_map_operand(const struct ir_val *src, struct asm_operand *dst)
{
	switch (src->subtype) {
	case IR_VAL_NONE:
		assert(0 && "unset operand in 2-arg/3-arg op");
		break;
	case IR_VAL_CONSTANT_INT:
		dst->operand_type = ASM_OPERAND_IMMEDIATE;
		dst->u.num = src->num;
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
	}

	codegen_map_ctype(src, dst);
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

	for (size_t i = 0; i < n_args && i < CODEGEN_REGISTER_ARGS; ++i) {
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[i], &(**dst).args[0]);
		codegen_set_operand_register(&src->args[i],
		                             CALL_REG[i],
		                             &(**dst).args[1]);
		dst = &(**dst).next;
	}

	long long int stack_args = 0;
	for (size_t i = n_args; i > CODEGEN_REGISTER_ARGS; --i) {
		size_t pos = i - 1;
		check(codegen_alloc_op(arena, dst));
		if (src->args[pos].subtype == IR_VAL_CONSTANT_INT ||
		    src->args[pos].c89type == CTYPE_LONG) {
			(**dst).opcode = ASM_OP_PUSH;
			codegen_map_operand(&src->args[pos], &(**dst).args[0]);
		} else {
			/*
			 * To pass a 32-bit argument via stack, we first copy
			 * to %eax (32-bit), then push %rax (64-bit).
			 */
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&src->args[pos], &(**dst).args[0]);
			codegen_set_operand_eax(&src->args[pos],
			                        &(**dst).args[1]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_PUSH;
			(**dst).args[0] = OPERAND_RAX_64BIT;
		}
		dst = &(**dst).next;
		++stack_args;
	}

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_CALL;
	(**dst).args[0].operand_type = ASM_OPERAND_CALL_TARGET_FUNCTION;
	(**dst).args[0].u.function = src->fun;
	dst = &(**dst).next;

	const long long int stack_deallocate =
		(CODEGEN_BYTES_PER_PUSH * stack_args) + stack_padding;
	if (stack_deallocate > 0) {
		check(codegen_alloc_addq_rsp(arena, dst, stack_deallocate));
		dst = &(**dst).next;
	}

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	codegen_set_operand_eax(&src->args[n_args], &(**dst).args[0]);
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
codegen_statement_one(Arena *arena,
                      const struct ir_op *src,
                      struct asm_op **dst)
{
	assert(*dst == NULL);
	check(codegen_alloc_op(arena, dst));

	/* guess overall op signedness ahead of time */
	const bool a_signed = ctype_is_signed(src->args[0].c89type);

	switch (src->opcode) {
	case IR_OP_RET:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&src->args[0], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_RET;
		break;
	case IR_OP_CTYPE_SIGN_EXTEND:
	case IR_OP_CTYPE_ZERO_EXTEND:
		switch (src->opcode) {
		case IR_OP_CTYPE_SIGN_EXTEND:
			(**dst).opcode = ASM_OP_MOV_WITH_SIGN_EXTENSION;
			break;
		case IR_OP_CTYPE_ZERO_EXTEND:
			(**dst).opcode = ASM_OP_MOV_WITH_ZERO_EXTENSION;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		for (size_t i = 0; i < ARRAY_SIZE((**dst).args); ++i) {
			codegen_map_operand(&src->args[i], &(**dst).args[i]);
		}
		assert((**dst).args[0].word_type == ASM_WORD_32BIT);
		assert((**dst).args[1].word_type == ASM_WORD_64BIT);
		break;
	case IR_OP_CTYPE_TRUNCATE:
		(**dst).opcode = ASM_OP_MOV;
		for (size_t i = 0; i < ARRAY_SIZE((**dst).args); ++i) {
			codegen_map_operand(&src->args[i], &(**dst).args[i]);
		}
		/* to truncate, only move CTYPE_INT's worth of source */
		(**dst).args[0].word_type = ASM_WORD_32BIT;
		break;
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_COMPLEMENT:
		(**dst).opcode = ASM_OP_MOV;
		for (size_t i = 0; i < ARRAY_SIZE((**dst).args); ++i) {
			codegen_map_operand(&src->args[i], &(**dst).args[i]);
		}
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		switch (src->opcode) {
		case IR_OP_UNARY_NEGATE:
			(**dst).opcode = ASM_OP_UNARY_NEG;
			break;
		case IR_OP_UNARY_COMPLEMENT:
			(**dst).opcode = ASM_OP_UNARY_NOT;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		break;
	case IR_OP_UNARY_DECREMENT:
	case IR_OP_UNARY_INCREMENT:
		switch (src->opcode) {
		case IR_OP_UNARY_DECREMENT:
			(**dst).opcode = ASM_OP_UNARY_DECREMENT;
			break;
		case IR_OP_UNARY_INCREMENT:
			(**dst).opcode = ASM_OP_UNARY_INCREMENT;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
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
		switch (src->args[0].c89type) {
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
		}
		dst = &(**dst).next;
		/* prepare divisor and idiv op */
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = a_signed ? ASM_OP_IDIV : ASM_OP_DIV;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		assert(src->args[0].c89type == src->args[1].c89type);
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
		for (size_t i = 0; i < ARRAY_SIZE((**dst).args); ++i) {
			codegen_map_operand(&src->args[i], &(**dst).args[i]);
		}
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
		switch (src->opcode) {
		case IR_OP_JUMP_IF_ZERO:
			(**dst).opcode = ASM_OP_JMP_IF_EQ;
			break;
		case IR_OP_JUMP_IF_NOT_ZERO:
			(**dst).opcode = ASM_OP_JMP_IF_NEQ;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
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
                           const struct ir_val *ir,
                           long long int pos,
                           struct asm_op **dst)
{
	assert(pos < CODEGEN_REGISTER_ARGS);

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	codegen_set_operand_register(&ir[pos], CALL_REG[pos], &(**dst).args[0]);
	codegen_set_operand_pseudo(&ir[pos], &(**dst).args[1]);
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_copy_stack_to_pseudo(Arena *arena,
                             const struct ir_val *ir,
                             long long int pos,
                             struct asm_op **dst)
{
	assert(pos >= CODEGEN_REGISTER_ARGS);

	const long long int stack_pos = pos - CODEGEN_REGISTER_ARGS;
	assert(stack_pos >= 0);
	const long long int stack_offset =
		CODEGEN_BYTES_ARG_FIRST + (CODEGEN_BYTES_PER_PUSH * stack_pos);
	assert(stack_offset % CODEGEN_BYTES_PER_VALUE == 0);

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	(**dst).args[0].operand_type = ASM_OPERAND_STACK;
	(**dst).args[0].u.num = stack_offset;
	codegen_map_ctype(&ir[pos], &(**dst).args[0]);
	codegen_set_operand_pseudo(&ir[pos], &(**dst).args[1]);

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function_params(Arena *arena,
                        const struct ir_val *ir,
                        struct asm_op **dst)
{
	for (long long int i = 0; i < FUNCTION_PARAMETER_LIMIT; ++i) {
		if (ir[i].subtype == IR_VAL_NONE) {
			break;
		}
		if (i < CODEGEN_REGISTER_ARGS) {
			check(codegen_copy_reg_to_pseudo(arena, ir, i, dst));
		} else {
			check(codegen_copy_stack_to_pseudo(arena, ir, i, dst));
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
	(**dst).alignment = codegen_get_alignment(ir);
	(**dst).linkage = codegen_map_linkage(ir->linkage);
	(**dst).u.initial_as_int128 = ir->u.initial_as_int128;
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
codegen_replace_pseudoregisters_fn(struct asm_function *cg,
                                   long long int range[2],
                                   long long int *offsets,
                                   bool preflight)
{
	long long int cursor = 0;
	for (struct asm_op *op = cg->ops; op != NULL; op = op->next) {
		for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
			struct asm_operand *arg = &op->args[i];
			if (arg->operand_type != ASM_OPERAND_PSEUDO_REGISTER) {
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
			const long long int idx = arg->u.num - range[0];

			assert(offsets != NULL);
			if (offsets[idx] == 0) {
				switch (arg->word_type) {
				case ASM_WORD_32BIT:
					cursor += CODEGEN_BYTES_PER_VALUE;
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

			arg->operand_type = ASM_OPERAND_STACK;
			arg->u.num = -1 * offsets[idx];
		}
	}
	return RESULT_OK;
}

result_t
codegen_replace_pseudoregisters(Arena *arena, struct assembly *cg)
{
	debug("Replacing pseudoregisters with stack addresses");

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		long long int range[2] = {LLONG_MAX, LLONG_MIN};
		check(codegen_replace_pseudoregisters_fn(f, range, NULL, true));

		if (range[0] == LLONG_MAX || range[1] == LLONG_MIN) {
			continue;
		}

		const long long int size = 1 + range[1] - range[0];
		assert(size > 0);
		assert(size <= 4096); /* if exceeded, refactor */

		long long int *off = arena_alloc(arena, sizeof(*off) * size);
		check(codegen_replace_pseudoregisters_fn(f, range, off, false));

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
	const long long int fix = round_up_to_multiple_of(cg->stack_usage, 16);
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

/*
 * Prepare a trampoline by memcpy()-ing invalid instructions where both
 * operands are ASM_OPERAND_STACK:
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
	const bool candidate_opcode =
		cur->opcode == ASM_OP_MOV || /* see block comment above */
		cur->opcode == ASM_OP_BINARY_ADD ||
		cur->opcode == ASM_OP_BINARY_SUBTRACT ||
		cur->opcode == ASM_OP_BITWISE_AND ||
		cur->opcode == ASM_OP_BITWISE_OR ||
		cur->opcode == ASM_OP_BITWISE_XOR ||
		cur->opcode == ASM_OP_COMPARE;
	const bool candidate_operand_0 =
		cur->args[0].operand_type == ASM_OPERAND_STACK ||
		cur->args[0].operand_type == ASM_OPERAND_VARIABLE_DATA;
	const bool candidate_operand_1 =
		cur->args[1].operand_type == ASM_OPERAND_STACK ||
		cur->args[1].operand_type == ASM_OPERAND_VARIABLE_DATA;
	if (!(candidate_opcode && candidate_operand_0 && candidate_operand_1)) {
		return false;
	}

	trampoline->sz = 2;
	for (size_t i = 0; i < trampoline->sz; ++i) {
		memcpy(trampoline->ops[i], cur, sizeof(*cur));
		trampoline->ops[i]->next = NULL;
	}
	if (cur->opcode != ASM_OP_MOV) {
		trampoline->ops[0]->opcode = ASM_OP_MOV;
	}
	codegen_set_operand_r10(&cur->args[0], &trampoline->ops[0]->args[1]);
	codegen_set_operand_r10(&cur->args[1], &trampoline->ops[1]->args[0]);

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
	      (cur->args[1].operand_type == ASM_OPERAND_STACK ||
	       cur->args[1].operand_type == ASM_OPERAND_VARIABLE_DATA))) {
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
 *     mov    $10, %r10d
 *     movslq %r10d, %r11 # note: src uses 32-bit alias, dst uses 64-bit alias
 *     mov    %r11, -16(%rbp)
 */
static WARN_UNUSED bool
fix_movsx(struct asm_op *cur, struct fix *trampoline)
{
	if (!(cur->opcode == ASM_OP_MOV_WITH_SIGN_EXTENSION &&
	      (cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE ||
	       cur->args[1].operand_type == ASM_OPERAND_STACK ||
	       cur->args[1].operand_type == ASM_OPERAND_VARIABLE_DATA))) {
		return false;
	}

	trampoline->sz = 3;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	assert(cur->args[0].word_type == ASM_WORD_32BIT);
	trampoline->ops[0]->args[1] = OPERAND_R10_32BIT;

	trampoline->ops[1]->opcode = ASM_OP_MOV_WITH_SIGN_EXTENSION;
	trampoline->ops[1]->args[0] = OPERAND_R10_32BIT;
	trampoline->ops[1]->args[1] = OPERAND_R11_64BIT;

	trampoline->ops[2]->opcode = ASM_OP_MOV;
	trampoline->ops[2]->args[0] = OPERAND_R11_64BIT;
	codegen_copy_operand(&cur->args[1], &trampoline->ops[2]->args[1]);
	assert(cur->args[1].word_type == ASM_WORD_64BIT);

	return true;
}

static WARN_UNUSED bool
fix_movzx(struct asm_op *cur, struct fix *trampoline)
{
	if (cur->opcode != ASM_OP_MOV_WITH_ZERO_EXTENSION) {
		return false;
	}

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
		trampoline->ops[0]->args[1] = OPERAND_R11_32BIT;
		trampoline->ops[1]->args[0] = OPERAND_R11_64BIT;
	}

	return true;
}

/*
 * An immediate (constant) value that does not fit into an int needs to bounce
 * through a register before an arithmetic op can use it as an operand.
 *
 * Ditto for ASM_OP_MOV op with a large immediate value as a source and an
 * ASM_OPERAND_STACK as a destination.
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
	       cur->args[0].u.num > INT_MAX &&
	       (cur->args[1].operand_type == ASM_OPERAND_STACK ||
	        cur->args[1].operand_type == ASM_OPERAND_VARIABLE_DATA)))) {
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
		fix_movsx,
		fix_movzx,
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
	case ASM_OPERAND_STACK:
		debug("  STACK %lld", (long long)operand->u.num);
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
	}

	switch (operand->word_type) {
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
		debug("  ALIGNMENT %lld", v->alignment);
		debug("  LINKAGE %s",
		      v->linkage == ASM_LINKAGE_EXTERNAL ? "EXTERNAL"
		                                         : "INTERNAL");
		debug("  INITIAL VALUE %lld",
		      (long long)v->u.initial_as_int128);
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
