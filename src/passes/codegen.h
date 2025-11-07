#ifndef COMPILER_PASSES_CODEGEN_H
#define COMPILER_PASSES_CODEGEN_H

#include "sys/string_view.h"

struct assembly {
	enum {
		ASM_PROGRAM,
		ASM_FUNCTION,
		ASM_OP_MOV,
		//ASM_OP_UNARY_NEG,
		//ASM_OP_UNARY_NOT,
		//ASM_OP_ALLOC_STACK,
		ASM_OP_RET,
	} statement_type;
};

struct asm_operand {
	enum {
		ASM_OPERAND_IMMEDIATE,
		ASM_OPERAND_REGISTER,
		//ASM_OPERAND_PSEUDO_REGISTER,
		//ASM_OPERAND_STACK,
	} operand_type;
	union {
		long long int num;
		enum {
			ASM_REGISTER_AX,
			ASM_REGISTER_R10,
		} reg;
	} u;
};

struct asm_op {
	struct assembly base;
	struct asm_operand args[2];
	struct asm_op *next;
};

struct asm_function {
	struct assembly base;
	struct string_view identifier;
	struct asm_op *ops;
};

struct asm_program {
	struct assembly base;
	struct asm_function function;
};

#endif
