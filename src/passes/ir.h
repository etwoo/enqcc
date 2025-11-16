#ifndef COMPILER_PASSES_IR_H
#define COMPILER_PASSES_IR_H

#include "sys/string_view.h"

#define FUNCTION_PARAMETER_LIMIT 32

struct ir_val {
	enum {
		IR_VAL_NONE,
		IR_VAL_CONSTANT_INT,
		IR_VAL_TEMPORARY_VARIABLE,
		IR_VAL_JUMP_TARGET_LABEL,
	} subtype;
	long long int num; /* numeric value, variable ID, etc */
};

struct ir_op {
	enum {
		IR_OP_RET,
		IR_OP_UNARY_COMPLEMENT,
		IR_OP_UNARY_NEGATE,
		IR_OP_UNARY_NOT,
		IR_OP_BINARY_ADD,
		IR_OP_BINARY_SUBTRACT,
		IR_OP_BINARY_MULTIPLY,
		IR_OP_BINARY_DIVIDE,
		IR_OP_BINARY_REMAINDER,
		IR_OP_COMPARE_EQUAL,
		IR_OP_COMPARE_NOT_EQUAL,
		IR_OP_COMPARE_LESS_THAN,
		IR_OP_COMPARE_LESS_THAN_EQ,
		IR_OP_COMPARE_MORE_THAN,
		IR_OP_COMPARE_MORE_THAN_EQ,
		IR_OP_COPY,
		IR_OP_JUMP,
		IR_OP_JUMP_IF_ZERO,
		IR_OP_JUMP_IF_NOT_ZERO,
		IR_OP_LABEL,
		IR_OP_CALL,
	} opcode;
	struct string_view fun;
	struct ir_val args[FUNCTION_PARAMETER_LIMIT];
	struct ir_op *next;
};

struct ir_function {
	struct string_view identifier;
	struct ir_val params[FUNCTION_PARAMETER_LIMIT];
	struct ir_op *ops;
	struct ir_function *next;
};

struct ir_env {
	long long int generator;
	long long int labels;
};

struct intermediate {
	struct ir_function *functions;
	struct ir_env env;
};

#endif
