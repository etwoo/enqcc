#include "passes/codegen.h"

#include "passes.h"
#include "passes/ir.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h> /* for LLONG_MAX */
#include <stdbool.h>
#include <sys/param.h> /* for MIN() and MAX() */

const long long int CODEGEN_BYTES_PER_VALUE = 4;
static const long long int CODEGEN_BYTES_PER_STACK_PUSH = 8;
enum {
	ARGS_PASSED_VIA_REGISTER = 6,
};
static const unsigned REGISTER_FOR_ARG[] = {
	ASM_REGISTER_DI,
	ASM_REGISTER_SI,
	ASM_REGISTER_DX,
	ASM_REGISTER_CX,
	ASM_REGISTER_R8,
	ASM_REGISTER_R9,
};

static WARN_UNUSED result_t
codegen_alloc_op(Arena *arena, struct asm_op **dst)
{
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if((dst) == NULL, ERR_CODEGEN_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_alloc_modify_rsp(Arena *arena,
                         struct asm_op **dst,
                         bool add,
                         long long int n)
{
	check(codegen_alloc_op(arena, dst));
	(**dst).opcode =
		add ? ASM_OP_BINARY_ADD_QUAD : ASM_OP_BINARY_SUBTRACT_QUAD;
	(**dst).args[0].operand_type = ASM_OPERAND_IMMEDIATE;
	(**dst).args[0].u.num = n;
	(**dst).args[1].operand_type = ASM_OPERAND_REGISTER;
	(**dst).args[1].u.reg = ASM_REGISTER_RSP;
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
codegen_set_operand_eax(struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_REGISTER;
	dst->u.reg = ASM_REGISTER_AX;
}

static void
codegen_set_operand_r10(struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_REGISTER;
	dst->u.reg = ASM_REGISTER_R10;
}

static void
codegen_set_operand_r11(struct asm_operand *dst)
{
	dst->operand_type = ASM_OPERAND_REGISTER;
	dst->u.reg = ASM_REGISTER_R11;
}

static void
codegen_set_operand_pseudo(struct asm_operand *dst, struct intermediate *ir)
{
	dst->operand_type = ASM_OPERAND_PSEUDO_REGISTER;
	dst->u.num = ir->env.generator++;
}

static void
codegen_map_operand(const struct ir_val *src, struct asm_operand *dst)
{
	switch (src->subtype) {
	case IR_VAL_NONE:
		assert(0 && "unset operand in 2-arg/3-arg op");
		break;
	case IR_VAL_CONSTANT_INT:
		dst->operand_type = ASM_OPERAND_IMMEDIATE;
		break;
	case IR_VAL_TEMPORARY_VARIABLE:
		dst->operand_type = ASM_OPERAND_PSEUDO_REGISTER;
		break;
	case IR_VAL_JUMP_TARGET_LABEL:
		dst->operand_type = ASM_OPERAND_JUMP_TARGET_LABEL;
		break;
	}
	dst->u.num = src->num;
}

static void
codegen_copy_operand(const struct asm_operand *src, struct asm_operand *dst)
{
	memcpy(dst, src, sizeof(*dst));
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

	const long long int stack_padding = n_args % 2 == 1 ? 8 : 0;
	if (stack_padding > 0) {
		check(codegen_alloc_subq_rsp(arena, dst, stack_padding));
		dst = &(**dst).next;
	}

	for (size_t i = 0; i < n_args && i < ARGS_PASSED_VIA_REGISTER; ++i) {
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[i], &(**dst).args[0]);
		(**dst).args[1].operand_type = ASM_OPERAND_REGISTER;
		(**dst).args[1].u.reg = REGISTER_FOR_ARG[i];
		dst = &(**dst).next;
	}

	long long int stack_args = 0;
	for (size_t i = n_args; i >= ARGS_PASSED_VIA_REGISTER; --i) {
		size_t pos = i - 1;
		check(codegen_alloc_op(arena, dst));
		if (src->args[pos].subtype == IR_VAL_CONSTANT_INT) {
			(**dst).opcode = ASM_OP_PUSH;
			codegen_map_operand(&src->args[pos], &(**dst).args[0]);
		} else {
			(**dst).opcode = ASM_OP_MOV;
			codegen_map_operand(&src->args[pos], &(**dst).args[0]);
			codegen_set_operand_eax(&(**dst).args[1]);
			dst = &(**dst).next;
			check(codegen_alloc_op(arena, dst));
			(**dst).opcode = ASM_OP_PUSH;
			codegen_set_operand_eax(&(**dst).args[0]);
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
		(CODEGEN_BYTES_PER_STACK_PUSH * stack_args) + stack_padding;
	if (stack_deallocate > 0) {
		check(codegen_alloc_addq_rsp(arena, dst, stack_deallocate));
		dst = &(**dst).next;
	}

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	codegen_set_operand_eax(&(**dst).args[0]);
	codegen_map_operand(&src->args[n_args], &(**dst).args[1]);
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_statement_one(Arena *arena,
                      const struct ir_op *src,
                      struct asm_op **dst)
{
	assert(*dst == NULL);
	check(codegen_alloc_op(arena, dst));

	switch (src->opcode) {
	case IR_OP_RET:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_RET;
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
	case IR_OP_BINARY_ADD:
	case IR_OP_BINARY_SUBTRACT:
	case IR_OP_BINARY_MULTIPLY:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
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
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		break;
	case IR_OP_BINARY_DIVIDE:
	case IR_OP_BINARY_REMAINDER:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&(**dst).args[1]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_CDQ;
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_IDIV;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		dst = &(**dst).next;
		check(codegen_alloc_op(arena, dst));
		(**dst).opcode = ASM_OP_MOV;
		switch (src->opcode) {
		case IR_OP_BINARY_DIVIDE:
			codegen_set_operand_eax(&(**dst).args[0]);
			break;
		case IR_OP_BINARY_REMAINDER:
			(**dst).args[0].operand_type = ASM_OPERAND_REGISTER;
			(**dst).args[0].u.reg = ASM_REGISTER_DX;
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
			(**dst).opcode = ASM_OP_SET_IF_LT;
			break;
		case IR_OP_COMPARE_LESS_THAN_EQ:
			(**dst).opcode = ASM_OP_SET_IF_LTE;
			break;
		case IR_OP_COMPARE_MORE_THAN:
			(**dst).opcode = ASM_OP_SET_IF_GT;
			break;
		case IR_OP_COMPARE_MORE_THAN_EQ:
			(**dst).opcode = ASM_OP_SET_IF_GTE;
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
                           struct intermediate *ir,
                           long long int pos,
                           struct asm_op **dst)
{
	assert(pos < ARGS_PASSED_VIA_REGISTER);
	static_assert(ARGS_PASSED_VIA_REGISTER <= ARRAY_SIZE(REGISTER_FOR_ARG),
	              "table does not cover all register-passed arg positions");

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	(**dst).args[0].operand_type = ASM_OPERAND_REGISTER;
	(**dst).args[0].u.reg = REGISTER_FOR_ARG[pos];
	codegen_set_operand_pseudo(&(**dst).args[1], ir);
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_copy_stack_to_pseudo(Arena *arena,
                             struct intermediate *ir,
                             long long int pos,
                             struct asm_op **dst)
{
	assert(pos >= ARGS_PASSED_VIA_REGISTER);

	const long long int stack_base = 16;
	const long long int stack_bytes_per = CODEGEN_BYTES_PER_STACK_PUSH;
	const long long int stack_offset = pos - ARGS_PASSED_VIA_REGISTER;

	check(codegen_alloc_op(arena, dst));
	(**dst).opcode = ASM_OP_MOV;
	(**dst).args[0].operand_type = ASM_OPERAND_STACK;
	(**dst).args[0].u.num = stack_base + (stack_bytes_per * stack_offset);
	codegen_set_operand_pseudo(&(**dst).args[1], ir);

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function_params(Arena *arena,
                        struct intermediate *ir,
                        long long int n_args,
                        struct asm_op **dst)
{
	for (long long int i = 0; i < n_args; ++i) {
		if (i < ARGS_PASSED_VIA_REGISTER) {
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
                 struct intermediate *ir,
                 const struct ir_function *f,
                 struct asm_function **dst)
{
	assert(dst != NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_CODEGEN_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	(**dst).identifier = f->identifier;

	struct asm_op **dst_ops = &(**dst).ops;

	assert(*dst_ops == NULL);
	check(codegen_function_params(arena, ir, f->n_args, dst_ops));

	while (*dst_ops != NULL) {
		dst_ops = &(**dst_ops).next;
	}

	assert(*dst_ops == NULL);
	check(codegen_statement(arena, f->ops, dst_ops));

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_program(Arena *arena,
                struct intermediate *ir,
                struct asm_function **dst)
{
	for (struct ir_function *f = ir->functions; f != NULL; f = f->next) {
		check(codegen_function(arena, ir, f, dst));
		dst = &(**dst).next;
	}
	return RESULT_OK;
}

result_t
codegen_init(Arena *arena, struct intermediate *ir, struct assembly **cg)
{
	*cg = arena_alloc(arena, sizeof(**cg));
	check_if(*cg == NULL, ERR_CODEGEN_ALLOC);
	memset(*cg, 0, sizeof(**cg));
	check(codegen_program(arena, ir, &(**cg).functions));
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_replace_pseudoregisters_fn(struct asm_function *cg,
                                   long long int range[2],
                                   bool preflight)
{
	for (struct asm_op *op = cg->ops; op != NULL; op = op->next) {
		for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
			struct asm_operand *arg = &op->args[i];
			if (arg->operand_type != ASM_OPERAND_PSEUDO_REGISTER) {
				continue;
			}

			if (preflight) {
				assert(range != NULL);
				range[0] = MIN(range[0], arg->u.num);
				range[1] = MAX(range[1], arg->u.num);
			} else {
				arg->operand_type = ASM_OPERAND_STACK;
				assert(arg->u.num >= range[0]);
				assert(arg->u.num <= range[1]);
				debug("Map PSEUDO %lld to STACK %lld",
				      arg->u.num,
				      arg->u.num - range[0]);
				arg->u.num -= (range[0] - 1);
			}
		}
	}
	return RESULT_OK;
}

result_t
codegen_replace_pseudoregisters(struct assembly *cg)
{
	debug("Replacing pseudoregisters with stack addresses");

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		long long int range[2] = {LLONG_MAX, 0};
		check(codegen_replace_pseudoregisters_fn(f, range, true));
		debug("Found pseudoregister ID range: [%lld, %lld]",
		      range[0],
		      range[1]);
		if (range[0] == LLONG_MAX || range[1] == 0) {
			continue;
		}

		assert(f->stack_usage == 0);
		f->stack_usage = 1 + (range[1] - range[0]);
		f->stack_usage *= CODEGEN_BYTES_PER_VALUE;
		debug("Updated stack usage of %.*s to %lld",
		      (int)f->identifier.sz,
		      f->identifier.data,
		      f->stack_usage);

		check(codegen_replace_pseudoregisters_fn(f, range, false));
	}

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

static WARN_UNUSED result_t
codegen_fixup_apply(Arena *arena,
                    struct asm_function *cg,
                    struct asm_op **new_prev,
                    struct asm_op **new_cur,
                    bool (*fix_init)(struct asm_op *cur,
                                     struct fix *trampoline))
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
	if (!((cur->opcode == ASM_OP_MOV || cur->opcode == ASM_OP_BINARY_ADD ||
	       cur->opcode == ASM_OP_BINARY_SUBTRACT ||
	       cur->opcode == ASM_OP_COMPARE) &&
	      cur->args[0].operand_type == ASM_OPERAND_STACK &&
	      cur->args[1].operand_type == ASM_OPERAND_STACK)) {
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
	codegen_set_operand_r10(&trampoline->ops[0]->args[1]);
	codegen_set_operand_r10(&trampoline->ops[1]->args[0]);

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
	codegen_set_operand_r11(&trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = ASM_OP_COMPARE;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[1]->args[0]);
	codegen_set_operand_r11(&trampoline->ops[1]->args[1]);

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
	if (!(cur->opcode == ASM_OP_IDIV &&
	      cur->args[0].operand_type == ASM_OPERAND_IMMEDIATE)) {
		return false;
	}

	trampoline->sz = 2;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r10(&trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = ASM_OP_IDIV;
	codegen_set_operand_r10(&trampoline->ops[1]->args[0]);

	return true;
}

/*
 *
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
	      cur->args[1].operand_type == ASM_OPERAND_STACK)) {
		return false;
	}

	trampoline->sz = 3;

	trampoline->ops[0]->opcode = ASM_OP_MOV;
	codegen_copy_operand(&cur->args[1], &trampoline->ops[0]->args[0]);
	codegen_set_operand_r11(&trampoline->ops[0]->args[1]);

	trampoline->ops[1]->opcode = ASM_OP_BINARY_MULTIPLY;
	codegen_copy_operand(&cur->args[0], &trampoline->ops[1]->args[0]);
	codegen_set_operand_r11(&trampoline->ops[1]->args[1]);

	trampoline->ops[2]->opcode = ASM_OP_MOV;
	codegen_set_operand_r11(&trampoline->ops[2]->args[0]);
	codegen_copy_operand(&cur->args[1], &trampoline->ops[2]->args[1]);
	return true;
}

static WARN_UNUSED result_t
codegen_fixup_instructions_fn(Arena *arena, struct asm_function *cg)
{
	check(codegen_fixup_alloc_stack(arena, cg));

	struct asm_op *prev = NULL;
	struct asm_op *cur = cg->ops;
	while (cur != NULL) {
		struct asm_op *orig[2] = {prev, cur};
		check(codegen_fixup_apply(arena, cg, &prev, &cur, fix_s2s));
		check(codegen_fixup_apply(arena, cg, &prev, &cur, fix_cmp));
		check(codegen_fixup_apply(arena, cg, &prev, &cur, fix_div));
		check(codegen_fixup_apply(arena, cg, &prev, &cur, fix_mul));
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
	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		check(codegen_fixup_instructions_fn(arena, f));
	}
	return RESULT_OK;
}

static void
codegen_debug_print_operand(const struct asm_operand *operand)
{
	switch (operand->operand_type) {
	case ASM_OPERAND_NONE:
		break;
	case ASM_OPERAND_IMMEDIATE:
		debug("  IMMEDIATE %lld", operand->u.num);
		break;
	case ASM_OPERAND_REGISTER:
		switch (operand->u.reg) {
		case ASM_REGISTER_AX:
			debug("  EAX");
			break;
		case ASM_REGISTER_CX:
			debug("  ECX");
			break;
		case ASM_REGISTER_DX:
			debug("  EDX");
			break;
		case ASM_REGISTER_DI:
			debug("  EDI");
			break;
		case ASM_REGISTER_SI:
			debug("  ESI");
			break;
		case ASM_REGISTER_R8:
			debug("  R8");
			break;
		case ASM_REGISTER_R9:
			debug("  R9");
			break;
		case ASM_REGISTER_R10:
			debug("  R10");
			break;
		case ASM_REGISTER_R11:
			debug("  R11");
			break;
		case ASM_REGISTER_RSP:
			debug("  RSP");
			break;
		}
		break;
	case ASM_OPERAND_PSEUDO_REGISTER:
		debug("  PSEUDO %lld", operand->u.num);
		break;
	case ASM_OPERAND_STACK:
		debug("  STACK %lld",
		      -1 * CODEGEN_BYTES_PER_VALUE * operand->u.num);
		break;
	case ASM_OPERAND_JUMP_TARGET_LABEL:
		debug("  LABEL %lld", operand->u.num);
		break;
	case ASM_OPERAND_CALL_TARGET_FUNCTION:
		debug("  FUNCTION %.*s",
		      (int)operand->u.function.sz,
		      operand->u.function.data);
		break;
	}
}

static void
codegen_debug_print_op(const struct asm_op *op)
{
	bool print_operands = true;

	switch (op->opcode) {
	case ASM_OP_MOV:
		debug("MOV");
		break;
	case ASM_OP_UNARY_NEG:
		debug("NEG");
		break;
	case ASM_OP_UNARY_NOT:
		debug("NOT");
		break;
	case ASM_OP_BINARY_ADD:
	case ASM_OP_BINARY_ADD_QUAD:
		debug("ADD");
		break;
	case ASM_OP_BINARY_SUBTRACT:
	case ASM_OP_BINARY_SUBTRACT_QUAD:
		debug("SUBTRACT");
		break;
	case ASM_OP_BINARY_MULTIPLY:
		debug("MULTIPLY");
		break;
	case ASM_OP_COMPARE:
		debug("COMPARE");
		break;
	case ASM_OP_IDIV:
		debug("IDIV");
		break;
	case ASM_OP_CDQ:
		debug("CDQ");
		break;
	case ASM_OP_JMP:
		debug("JMP");
		break;
	case ASM_OP_JMP_IF_EQ:
		debug("JMP_IF_EQ");
		break;
	case ASM_OP_JMP_IF_NEQ:
		debug("JMP_IF_NEQ");
		break;
	case ASM_OP_JMP_IF_GT:
		debug("JMP_IF_GT");
		break;
	case ASM_OP_JMP_IF_GTE:
		debug("JMP_IF_GTE");
		break;
	case ASM_OP_JMP_IF_LT:
		debug("JMP_IF_LT");
		break;
	case ASM_OP_JMP_IF_LTE:
		debug("JMP_IF_LTE");
		break;
	case ASM_OP_SET_IF_EQ:
		debug("SET_IF_EQ");
		break;
	case ASM_OP_SET_IF_NEQ:
		debug("SET_IF_NEQ");
		break;
	case ASM_OP_SET_IF_GT:
		debug("SET_IF_GT");
		break;
	case ASM_OP_SET_IF_GTE:
		debug("SET_IF_GTE");
		break;
	case ASM_OP_SET_IF_LT:
		debug("SET_IF_LT");
		break;
	case ASM_OP_SET_IF_LTE:
		debug("SET_IF_LTE");
		break;
	case ASM_OP_LABEL:
		debug("MARK_LABEL");
		break;
	case ASM_OP_PUSH:
		debug("PUSH");
		break;
	case ASM_OP_CALL:
		debug("CALL");
		break;
	case ASM_OP_RET:
		debug("RET");
		print_operands = false;
		break;
	}

	for (size_t i = 0; print_operands && i < ARRAY_SIZE(op->args); ++i) {
		codegen_debug_print_operand(&op->args[i]);
	}
}

void
codegen_debug_print(const struct assembly *cg)
{
	debug("PROGRAM");

	for (struct asm_function *f = cg->functions; f != NULL; f = f->next) {
		const struct string_view *fname = &f->identifier;
		debug("FUNCTION %.*s", (int)fname->sz, fname->data);
		debug("  STACK_USAGE %lld", f->stack_usage);

		for (struct asm_op *op = f->ops; op != NULL; op = op->next) {
			codegen_debug_print_op(op);
		}
	}
}
