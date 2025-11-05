#ifndef COMPILER_PASSES_CODEGEN_H
#define COMPILER_PASSES_CODEGEN_H

#include "sys/string_view.h"

struct assembly {
	enum {
		ASM_PROGRAM,
		ASM_FUNCTION,
		ASM_OP_MOV,
		ASM_OP_RET,
	} statement_type;
};

struct asm_operand {
	enum {
		ASM_OPERAND_IMMEDIATE,
		ASM_OPERAND_REGISTER_EAX,
	} operand_type;
	long long int num;
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
