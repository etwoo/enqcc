#ifndef COMPILER_PASSES_CODEGEN_H
#define COMPILER_PASSES_CODEGEN_H

#include "sys/string_view.h"

struct asm_operand {
	enum {
		ASM_OPERAND_NONE,
		ASM_OPERAND_IMMEDIATE,
		ASM_OPERAND_REGISTER,
		ASM_OPERAND_PSEUDO_REGISTER,
		ASM_OPERAND_STACK,
	} operand_type;
	union {
		long long int num;
		enum {
			ASM_REGISTER_AX,
			ASM_REGISTER_R10, /* aka scratch */
			ASM_REGISTER_R11, /* aka scratch */
			ASM_REGISTER_RSP, /* aka frame pointer */
		} reg;
	} u;
};

struct asm_op {
	enum {
		ASM_OP_MOV,
		ASM_OP_UNARY_NEG,
		ASM_OP_UNARY_NOT,
		ASM_OP_BINARY_ADD,
		ASM_OP_BINARY_SUBTRACT,
		ASM_OP_BINARY_SUBTRACT_QUAD,
		ASM_OP_BINARY_MULTIPLY,
		ASM_OP_IDIV, /* divide AX+DX by given divisor */
		ASM_OP_CDQ,  /* convert to quadword, aka sign extend AX->DX */
		ASM_OP_RET,
	} opcode;
	struct asm_operand args[2];
	struct asm_op *next;
};

struct asm_function {
	struct string_view identifier;
	struct asm_op *ops;
};

struct assembly {
	struct asm_function function;
};

extern const long long int CODEGEN_BYTES_PER_VALUE;

#endif
