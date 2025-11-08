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

#define codegen_dup(dst, src)                                                  \
	do {                                                                   \
		codegen_alloc(dst);                                            \
		memcpy(dst, src, sizeof(*(dst)));                              \
	} while (0)

static void
codegen_op_list_contat(struct asm_op *first, struct asm_op *second)
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
	codegen_op_list_contat(new_head, *head);
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

static void
codegen_op_list_cleanup(struct asm_op **pp)
{
	codegen_op_list_free(*pp);
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
codegen_map_operand(const struct ir_val *src, struct asm_operand *dst)
{
	switch (src->subtype) {
	case IR_VAL_NONE:
		assert(0 && "unset operand in 2-arg op");
		break;
	case IR_VAL_CONSTANT_INT:
		dst->operand_type = ASM_OPERAND_IMMEDIATE;
		dst->u.num = src->num;
		break;
	case IR_VAL_TEMPORARY_VARIABLE:
		dst->operand_type = ASM_OPERAND_PSEUDO_REGISTER;
		dst->u.num = src->num;
		break;
	}
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
		(**dst).args[1].operand_type = ASM_OPERAND_REGISTER;
		(**dst).args[1].u.reg = ASM_REGISTER_AX;
		dst = &(**dst).next;
		codegen_alloc(*dst);
		(**dst).opcode = ASM_OP_RET;
		break;
	case IR_OP_UNARY_NEGATE:
	case IR_OP_UNARY_COMPLEMENT:
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
		case IR_OP_UNARY_COMPLEMENT:
			(**dst).opcode = ASM_OP_UNARY_NOT;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		codegen_map_operand(&src->args[1], &(**dst).args[0]);
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
codegen_stack(struct assembly *cg)
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
	alloc_stack->opcode = ASM_OP_SUB;
	alloc_stack->args[0].operand_type = ASM_OPERAND_IMMEDIATE;
	alloc_stack->args[0].u.num =
		CODEGEN_BYTES_PER_VALUE * (ir->env.generator - 1);
	alloc_stack->args[1].operand_type = ASM_OPERAND_REGISTER;
	alloc_stack->args[1].u.reg = ASM_REGISTER_RSP;

	codegen_op_list_prepend(alloc_stack, &cg->function.ops);
	return RESULT_OK;
}

static WARN_UNUSED result_t
codegen_fixup_stack_to_stack(struct asm_op *prev,
                             struct asm_op *cur,
                             struct asm_op **new_prev,
                             struct asm_op **new_cur)
{
	assert(prev && cur);

	/*
	 * TODO: try removing NOLINT from this helper method, then rm comment
	 *
	 * clang-analyzer does not seem to understand how
	 * __attribute__((cleanup)) on trampoline affects the rest of
	 * the loop body. The NOLINT markers for:
	 *
	 *   clang-analyzer-unix.Malloc
	 *   clang-analyzer-deadcode.DeadStores
	 *
	 * ... in the rest of the loop body suppress the associated
	 * clang-tidy warnings. We should remove the annotations if
	 * clang-tidy changes in the future (or this code evolves).
	 */
	struct asm_op *trampoline_head
		__attribute__((cleanup(codegen_op_list_cleanup))) = NULL;
	codegen_dup(trampoline_head, cur);
	trampoline_head->next = NULL;
	trampoline_head->args[1].operand_type = ASM_OPERAND_REGISTER;
	trampoline_head->args[1].u.reg = ASM_REGISTER_R10;

	struct asm_op *trampoline_tail
		__attribute__((cleanup(codegen_op_list_cleanup))) = NULL;
	// NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
	codegen_dup(trampoline_tail, cur);
	trampoline_tail->next = NULL;
	trampoline_tail->args[0].operand_type = ASM_OPERAND_REGISTER;
	trampoline_tail->args[0].u.reg = ASM_REGISTER_R10;

	/*
	 * Split containing list at <cur>, excluding <cur> from both halves.
	 */
	struct asm_op *remainder = cur->next;
	cur->next = NULL;
	prev->next = NULL;

	/*
	 * Insert trampoline sublist into containing list.
	 */
	codegen_op_list_contat(prev, trampoline_head);
	codegen_op_list_contat(trampoline_head, trampoline_tail);
	codegen_op_list_contat(trampoline_tail, remainder);

	/*
	 * Prepare list cursor positions for next loop iteration.
	 */
	*new_prev = trampoline_tail;
	*new_cur = remainder;

	/*
	 * Release ownership of scoped temporary pointers to caller.
	 */
	trampoline_head = NULL;
	trampoline_tail = NULL;

	return RESULT_OK;
}

result_t
codegen_fixup(const struct intermediate *ir, struct assembly *cg)
{
	debug("Fixing up invalid instructions");

	if (ir->env.generator > 1) {
		check(codegen_fixup_alloc_stack(ir, cg));
	}

	struct asm_op *prev = NULL;
	struct asm_op *cur = cg->function.ops;
	while (cur != NULL) {
		if (cur->opcode != ASM_OP_MOV ||
		    cur->args[0].operand_type != ASM_OPERAND_STACK ||
		    cur->args[1].operand_type != ASM_OPERAND_STACK) {
			prev = cur;
			cur = cur->next;
			continue;
		}
		assert(prev != NULL &&
		       "should never need trampoline on very first op, which "
		       "should always be something like setting up function "
		       "context, substracting from frame pointer RSP, etc");
		check(codegen_fixup_stack_to_stack(prev, cur, &prev, &cur));
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
		case ASM_REGISTER_R10:
			debug("  R10");
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
	case ASM_OP_SUB:
		debug("SUB");
		break;
	case ASM_OP_UNARY_NEG:
		debug("NEG");
		break;
	case ASM_OP_UNARY_NOT:
		debug("NOT");
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
