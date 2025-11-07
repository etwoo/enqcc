#ifndef COMPILER_PASSES_IR_H
#define COMPILER_PASSES_IR_H

#include "sys/string_view.h"

struct intermediate {
	enum {
		IR_PROGRAM,
		IR_FUNCTION,
		IR_VAL_CONSTANT_INT,
		IR_VAL_VARIABLE,
		IR_OP_UNARY_IDENTITY, /* aka return */
		IR_OP_UNARY_NEGATE,
		IR_OP_UNARY_COMPLEMENT,
	} subtype;
};

struct ir_val_constant {
	struct intermediate base;
	long long int num;
};

struct ir_val_variable {
	struct intermediate base;
	struct string_view identifier;
};

struct ir_op {
	struct intermediate base;
	struct intermediate *args[1]; /* TODO: 1->2 for binary ops */
	struct ir_op *next;
};

struct ir_function {
	struct intermediate base;
	struct string_view identifier;
	struct ir_op *ops;
};

struct ir_program {
	struct intermediate base;
	struct ir_function function;
};

#endif
