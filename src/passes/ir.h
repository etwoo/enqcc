#ifndef COMPILER_PASSES_IR_H
#define COMPILER_PASSES_IR_H

#include "passes/symbol.h"
#include "sys/string_view.h"

#define FUNCTION_PARAMETER_LIMIT 32

struct ir_val {
	enum {
		IR_VAL_NONE,
		IR_VAL_CONSTANT_INT,
		IR_VAL_TEMPORARY_VARIABLE,
		IR_VAL_JUMP_TARGET_LABEL,
		IR_VAL_VARIABLE_DATA,
	} subtype;
	long long int num;          /* numeric value, variable ID, etc */
	struct string_view varname; /* symbol name, if linkage */
	enum ctype c89type;
};

#define FOREACH_IR_OPCODE(F)                                                   \
	F(RET, 0)                                                              \
	F(CTYPE_SIGN_EXTEND, 1)                                                \
	F(CTYPE_TRUNCATE, 1)                                                   \
	F(UNARY_COMPLEMENT, 1)                                                 \
	F(UNARY_NEGATE, 1)                                                     \
	F(UNARY_NOT, 1)                                                        \
	F(UNARY_DECREMENT, 1)                                                  \
	F(UNARY_INCREMENT, 1)                                                  \
	F(BINARY_ADD, 2)                                                       \
	F(BINARY_SUBTRACT, 2)                                                  \
	F(BINARY_MULTIPLY, 2)                                                  \
	F(BINARY_DIVIDE, 2)                                                    \
	F(BINARY_REMAINDER, 2)                                                 \
	F(BITWISE_AND, 2)                                                      \
	F(BITWISE_OR, 2)                                                       \
	F(BITWISE_XOR, 2)                                                      \
	F(BITWISE_SHIFT_LEFT, 2)                                               \
	F(BITWISE_SHIFT_RIGHT, 2)                                              \
	F(COMPARE_EQUAL, 2)                                                    \
	F(COMPARE_NOT_EQUAL, 2)                                                \
	F(COMPARE_LESS_THAN, 2)                                                \
	F(COMPARE_LESS_THAN_EQ, 2)                                             \
	F(COMPARE_MORE_THAN, 2)                                                \
	F(COMPARE_MORE_THAN_EQ, 2)                                             \
	F(COPY, 2)                                                             \
	F(JUMP, 1)                                                             \
	F(JUMP_IF_ZERO, 2)                                                     \
	F(JUMP_IF_NOT_ZERO, 2)                                                 \
	F(LABEL, 1)                                                            \
	F(CALL, 0)

#define TO_ENUM(opcode, op_requires_n_args) IR_OP_##opcode,
enum ir_opcode { FOREACH_IR_OPCODE(TO_ENUM) };
#undef TO_ENUM

struct ir_op {
	enum ir_opcode opcode;
	struct string_view fun;
	struct ir_val args[FUNCTION_PARAMETER_LIMIT];
	struct ir_op *next;
};

enum ir_linkage {
	IR_LINKAGE_INTERNAL,
	IR_LINKAGE_EXTERNAL,
};

struct ir_function {
	struct string_view identifier;
	enum ir_linkage linkage;
	struct ir_val params[FUNCTION_PARAMETER_LIMIT];
	struct ir_op *ops;
	struct ir_function *next;
};

struct ir_variable {
	struct string_view identifier;
	enum ir_linkage linkage;
	struct {
		long long int initial_as_ll;
	} u;
	struct ir_variable *next;
};

struct ir_env {
	long long int generator;
	long long int labels;
};

struct intermediate {
	struct ir_function *functions;
	struct ir_variable *variables;
	struct ir_env env;
};

#endif
