#include "passes/codegen.h"

#include "passes.h"
#include "passes/ir.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

const long long int CODEGEN_BYTES_PER_VALUE = 4;

#define codegen_alloc(dst)                                                     \
	do {                                                                   \
		(dst) = malloc(sizeof(*(dst)));                                \
		check_if((dst) == NULL, ERR_CODEGEN_ALLOC);                    \
		memset(dst, 0, sizeof(*(dst)));                                \
	} while (0)

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
codegen_op_list_free(struct asm_op *cursor)
{
	while (cursor != NULL) {
		struct asm_op *tmp = cursor;
		cursor = cursor->next;
		free(tmp);
	}
}

void
codegen_free(struct assembly *cg)
{
	codegen_op_list_free(cg ? cg->function.ops : NULL);
	free(cg);
}

void
codegen_cleanup(struct assembly **cg)
{
	codegen_free(*cg);
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
codegen_statement_one(const struct ir_op *src, struct asm_op **dst)
{
	assert(*dst == NULL);
	codegen_alloc(*dst);

	switch (src->opcode) {
	case IR_OP_UNARY_IDENTITY:
		(**dst).opcode = ASM_OP_MOV;
		codegen_map_operand(&src->args[0], &(**dst).args[0]);
		codegen_set_operand_eax(&(**dst).args[1]);
		dst = &(**dst).next;
		codegen_alloc(*dst);
		(**dst).opcode = ASM_OP_RET;
		break;
	case IR_OP_UNARY_COMPLEMENT:
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_NOT:
		(**dst).opcode = ASM_OP_MOV;
		for (size_t i = 0; i < ARRAY_SIZE((**dst).args); ++i) {
			codegen_map_operand(&src->args[i], &(**dst).args[i]);
		}
		dst = &(**dst).next;
		codegen_alloc(*dst);
		switch (src->opcode) {
		case IR_OP_UNARY_NEGATE:
			(**dst).opcode = ASM_OP_UNARY_NEG;
			break;
		case IR_OP_UNARY_NOT:
			// TODO: verify that IR for boolean not and bitwise
			// complement both become same ASM, `notl`
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
		codegen_alloc(*dst);
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
		codegen_alloc(*dst);
		(**dst).opcode = ASM_OP_CDQ;
		dst = &(**dst).next;
		codegen_alloc(*dst);
		(**dst).opcode = ASM_OP_IDIV;
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
		dst = &(**dst).next;
		codegen_alloc(*dst);
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
		codegen_alloc(*dst);
		(**dst).opcode = ASM_OP_MOV;
		codegen_set_operand_immediate_zero(&(**dst).args[0]);
		codegen_map_operand(&src->args[2], &(**dst).args[1]);
		dst = &(**dst).next;
		codegen_alloc(*dst);
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
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
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
		codegen_alloc(*dst);
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
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_statement(const struct ir_op *src, struct asm_op **dst)
{
	while (src != NULL) {
		check(codegen_statement_one(src, dst));
		src = src->next;
		while (*dst != NULL) {
			dst = &(**dst).next;
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_function(const struct ir_function *ir, struct asm_function *dst)
{
	dst->identifier = ir->identifier;
	check(codegen_statement(ir->ops, &dst->ops));
	return RESULT_OK;
}

result_t
codegen_init(const struct intermediate *ir, struct assembly **cg)
{
	codegen_alloc(*cg);
	check(codegen_function(&ir->function, &(**cg).function));
	return RESULT_OK;
}

result_t
codegen_replace_pseudoregisters(struct assembly *cg)
{
	debug("Replacing pseudoregisters with stack addresses");
	for (struct asm_op *op = cg->function.ops; op != NULL; op = op->next) {
		for (size_t i = 0; i < ARRAY_SIZE(op->args); ++i) {
			struct asm_operand *arg = &op->args[i];
			if (arg->operand_type == ASM_OPERAND_PSEUDO_REGISTER) {
				arg->operand_type = ASM_OPERAND_STACK;
			}
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_fixup_alloc_stack(const struct intermediate *ir, struct assembly *cg)
{
	struct asm_op *alloc_stack = NULL;
	codegen_alloc(alloc_stack);
	alloc_stack->opcode = ASM_OP_BINARY_SUBTRACT_QUAD;
	alloc_stack->args[0].operand_type = ASM_OPERAND_IMMEDIATE;
	alloc_stack->args[0].u.num =
		CODEGEN_BYTES_PER_VALUE * (ir->env.generator - 1);
	alloc_stack->args[1].operand_type = ASM_OPERAND_REGISTER;
	alloc_stack->args[1].u.reg = ASM_REGISTER_RSP;

	codegen_op_list_prepend(alloc_stack, &cg->function.ops);
	return RESULT_OK;
}

struct fix {
	size_t sz;
	struct asm_op *ops[3];
};

static void
fix_cleanup(struct fix *trampoline)
{
	for (size_t i = 0; i < ARRAY_SIZE(trampoline->ops); ++i) {
		codegen_op_list_free(trampoline->ops[i]);
		trampoline->ops[i] = NULL;
	}
}

static WARN_UNUSED result_t
codegen_fixup_apply(struct assembly *cg,
                    struct asm_op *prev,
                    struct asm_op *cur,
                    struct asm_op **new_prev,
                    struct asm_op **new_cur,
                    bool (*fix_init)(struct asm_op *cur,
                                     struct fix *trampoline))
{
	struct fix trampoline __attribute__((cleanup(fix_cleanup))) = {0};
	for (size_t i = 0; i < ARRAY_SIZE(trampoline.ops); ++i) {
		// NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
		codegen_alloc(trampoline.ops[i]);
	}

	if (!fix_init(cur, &trampoline)) {
		return RESULT_OK; /* no fixup necessary */
	}

	assert(cur);
	if (prev == NULL) {
		assert(cg->function.ops == cur);
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
		cg->function.ops = trampoline.ops[0];
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

	/*
	 * Clean up node that we've just removed from the containing list.
	 */
	codegen_op_list_free(cur);

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

result_t
codegen_fixup_instructions(const struct intermediate *ir, struct assembly *cg)
{
	debug("Fixing up invalid instructions");

	if (ir->env.generator > 1) {
		check(codegen_fixup_alloc_stack(ir, cg));
	}

	struct asm_op *prev = NULL;
	struct asm_op *cur = cg->function.ops;
	while (cur != NULL) {
		struct asm_op *orig[2] = {prev, cur};
		check(codegen_fixup_apply(cg, prev, cur, &prev, &cur, fix_s2s));
		check(codegen_fixup_apply(cg, prev, cur, &prev, &cur, fix_cmp));
		check(codegen_fixup_apply(cg, prev, cur, &prev, &cur, fix_div));
		check(codegen_fixup_apply(cg, prev, cur, &prev, &cur, fix_mul));
		if (cur == orig[1]) {
			assert(prev == orig[0]);
			prev = cur;
			cur = cur->next; /* no fixup -> default cur update */
		}
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
		case ASM_REGISTER_DX:
			debug("  EDX");
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

	const struct string_view *fname = &cg->function.identifier;
	debug("FUNCTION %.*s", (int)fname->sz, fname->data);

	for (struct asm_op *op = cg->function.ops; op != NULL; op = op->next) {
		codegen_debug_print_op(op);
	}
}

#undef codegen_alloc
