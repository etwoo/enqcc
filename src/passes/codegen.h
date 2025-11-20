#ifndef COMPILER_PASSES_CODEGEN_H
#define COMPILER_PASSES_CODEGEN_H

#include "sys/string_view.h"

#define FOREACH_CALL_REGISTER(F)                                               \
	F(DI, "rdi", "edi", "dil")                                             \
	F(SI, "rsi", "esi", "sil")                                             \
	F(DX, "rdx", "edx", "dl")                                              \
	F(CX, "rcx", "ecx", "cl")                                              \
	F(R8, "r8", "r8d", "r8b")                                              \
	F(R9, "r9", "r9d", "r9b")

#define FOREACH_ASM_REGISTER(F)                                                \
	FOREACH_CALL_REGISTER(F)                                               \
	F(AX, "rax", "eax", "al")                                              \
	F(R10, "r10", "r10d", "r10b")                                          \
	F(R11, "r11", "r11d", "r11b")                                          \
	F(RSP, "rsp", "rsp", "r11d")

#define TO_ENUM(register_name, b8, b4, b1) ASM_REGISTER_##register_name,
enum asm_register { FOREACH_ASM_REGISTER(TO_ENUM) };
#undef TO_ENUM

struct asm_operand {
	enum {
		ASM_OPERAND_NONE,
		ASM_OPERAND_IMMEDIATE,
		ASM_OPERAND_REGISTER,
		ASM_OPERAND_PSEUDO_REGISTER,
		ASM_OPERAND_STACK,
		ASM_OPERAND_JUMP_TARGET_LABEL,
		ASM_OPERAND_CALL_TARGET_FUNCTION,
	} operand_type;
	union {
		long long int num;
		enum asm_register reg;
		struct string_view function;
	} u;
};

#define FOREACH_ASM_OPCODE(F)                                                  \
	F(MOV)                                                                 \
	F(UNARY_NEG)                                                           \
	F(UNARY_NOT)                                                           \
	F(BINARY_ADD)                                                          \
	F(BINARY_ADD_QUAD)                                                     \
	F(BINARY_SUBTRACT)                                                     \
	F(BINARY_SUBTRACT_QUAD)                                                \
	F(BINARY_MULTIPLY)                                                     \
	F(COMPARE)                                                             \
	F(IDIV)                                                                \
	F(CDQ)                                                                 \
	F(JMP)                                                                 \
	F(JMP_IF_EQ)                                                           \
	F(JMP_IF_NEQ)                                                          \
	F(JMP_IF_GT)                                                           \
	F(JMP_IF_GTE)                                                          \
	F(JMP_IF_LT)                                                           \
	F(JMP_IF_LTE)                                                          \
	F(SET_IF_EQ)                                                           \
	F(SET_IF_NEQ)                                                          \
	F(SET_IF_GT)                                                           \
	F(SET_IF_GTE)                                                          \
	F(SET_IF_LT)                                                           \
	F(SET_IF_LTE)                                                          \
	F(LABEL)                                                               \
	F(PUSH)                                                                \
	F(CALL)                                                                \
	F(RET)

#define TO_ENUM(opcode) ASM_OP_##opcode,
enum asm_opcode { FOREACH_ASM_OPCODE(TO_ENUM) };
#undef TO_ENUM

struct asm_op {
	enum asm_opcode opcode;
	struct asm_operand args[2];
	struct asm_op *next;
};

struct asm_function {
	struct string_view identifier;
	long long int stack_usage;
	struct asm_op *ops;
	struct asm_function *next;
};

struct assembly {
	struct asm_function *functions;
};

#endif
