#ifndef COMPILER_PASSES_CODEGEN_H
#define COMPILER_PASSES_CODEGEN_H

#include "lang/symbol.h"
#include "sys/string_view.h"

#define FOREACH_CALL_REGISTER(F)                                               \
	F(DI, "rdi", "edi", "dil")                                             \
	F(SI, "rsi", "esi", "sil")                                             \
	F(DX, "rdx", "edx", "dl")                                              \
	F(CX, "rcx", "ecx", "cl")                                              \
	F(R8, "r8", "r8d", "r8b")                                              \
	F(R9, "r9", "r9d", "r9b")

#define FOREACH_FP_CALL_REGISTER(F)                                            \
	F(XMM0, "xmm0", "xmm0", "xmm0")                                        \
	F(XMM1, "xmm1", "xmm1", "xmm1")                                        \
	F(XMM2, "xmm2", "xmm2", "xmm2")                                        \
	F(XMM3, "xmm3", "xmm3", "xmm3")                                        \
	F(XMM4, "xmm4", "xmm4", "xmm4")                                        \
	F(XMM5, "xmm5", "xmm5", "xmm5")                                        \
	F(XMM6, "xmm6", "xmm6", "xmm6")                                        \
	F(XMM7, "xmm7", "xmm7", "xmm7")

#define FOREACH_ASM_REGISTER(F)                                                \
	FOREACH_CALL_REGISTER(F)                                               \
	F(AX, "rax", "eax", "al")                                              \
	F(R10, "r10", "r10d", "r10b")                                          \
	F(R11, "r11", "r11d", "r11b")                                          \
	F(RSP, "rsp", "rsp", "rsp")                                            \
	F(RBP, "rbp", "rbp", "rbp")                                            \
	FOREACH_FP_CALL_REGISTER(F)                                            \
	F(XMM14, "xmm14", "xmm14", "xmm14")                                    \
	F(XMM15, "xmm15", "xmm15", "xmm15")

#define TO_ENUM(register_name, b8, b4, b1) ASM_REGISTER_##register_name,
enum asm_register { FOREACH_ASM_REGISTER(TO_ENUM) };
#undef TO_ENUM

struct asm_operand {
	enum {
		ASM_OPERAND_NONE,
		ASM_OPERAND_IMMEDIATE,
		ASM_OPERAND_REGISTER,
		ASM_OPERAND_PSEUDO_REGISTER,
		ASM_OPERAND_MEMORY,
		ASM_OPERAND_JUMP_TARGET_LABEL,
		ASM_OPERAND_CALL_TARGET_FUNCTION,
		ASM_OPERAND_VARIABLE_DATA,
		ASM_OPERAND_CONSTANT_DATA_DOUBLE,
		ASM_OPERAND_CONSTANT_DATA_VEC_LONGS,
		ASM_OPERAND_CONSTANT_DATA_VEC_QUADS,
	} operand_type;
	enum {
		ASM_WORD_32BIT, /* DWORD */
		ASM_WORD_64BIT, /* QWORD */
		ASM_WORD_POINTER_TO_32BIT,
		ASM_WORD_POINTER_TO_64BIT,
	} word_type;
	union {
		int128_t num;
		enum asm_register reg;
		struct {
			long long int offset;
			enum asm_register reg;
		} mem;                       /* MEMORY */
		struct string_view function; /* CALL_TARGET_FUNCTION */
		struct string_view variable; /* VARIABLE_DATA */
		double dnum;                 /* CONSTANT_DATA_DOUBLE */
		long unsigned longs[4];      /* CONSTANT_DATA_VEC_LONGS */
		long long unsigned quads[2]; /* CONSTANT_DATA_VEC_QUADS */
	} u;
};

bool is_xmm_register(const struct asm_operand *o) WARN_UNUSED;

#define FOREACH_ASM_OPCODE(F)                                                  \
	F(MOV)                                                                 \
	F(MOV_WITH_SIGN_EXTENSION)                                             \
	F(MOV_WITH_ZERO_EXTENSION)                                             \
	F(LEA)                                                                 \
	F(CVT_DOUBLE_TO_INT)                                                   \
	F(CVT_INT_TO_DOUBLE)                                                   \
	F(UNARY_NEG)                                                           \
	F(UNARY_NOT)                                                           \
	F(UNARY_DECREMENT)                                                     \
	F(UNARY_INCREMENT)                                                     \
	F(BINARY_ADD)                                                          \
	F(BINARY_SUBTRACT)                                                     \
	F(BINARY_MULTIPLY)                                                     \
	F(BITWISE_AND)                                                         \
	F(BITWISE_OR)                                                          \
	F(BITWISE_XOR)                                                         \
	F(BITWISE_SIGNED_SHIFT_LEFT)                                           \
	F(BITWISE_SIGNED_SHIFT_RIGHT)                                          \
	F(BITWISE_UNSIGNED_SHIFT_LEFT)                                         \
	F(BITWISE_UNSIGNED_SHIFT_RIGHT)                                        \
	F(COMPARE)                                                             \
	F(IDIV)                                                                \
	F(DIV)                                                                 \
	F(CDQ)                                                                 \
	F(CQO)                                                                 \
	F(DOUBLE_BINARY_ADD)                                                   \
	F(DOUBLE_BINARY_SUBTRACT)                                              \
	F(DOUBLE_BINARY_MULTIPLY)                                              \
	F(DOUBLE_BINARY_DIVIDE)                                                \
	F(DOUBLE_BITWISE_XOR)                                                  \
	F(DOUBLE_COMPARE)                                                      \
	F(VEC_DOUBLE_BINARY_SUBTRACT)                                          \
	F(VEC_DOUBLE_UNPACK_INTERLEAVE_HI)                                     \
	F(VEC_DOUBLE_UNPACK_INTERLEAVE_LO)                                     \
	F(JMP)                                                                 \
	F(JMP_IF_EQ)                                                           \
	F(JMP_IF_NEQ)                                                          \
	F(JMP_IF_GT)                                                           \
	F(JMP_IF_GTE)                                                          \
	F(JMP_IF_LT)                                                           \
	F(JMP_IF_LTE)                                                          \
	F(JMP_IF_A)                                                            \
	F(JMP_IF_AE)                                                           \
	F(JMP_IF_B)                                                            \
	F(JMP_IF_BE)                                                           \
	F(SET_IF_EQ)                                                           \
	F(SET_IF_NEQ)                                                          \
	F(SET_IF_GT)                                                           \
	F(SET_IF_GTE)                                                          \
	F(SET_IF_LT)                                                           \
	F(SET_IF_LTE)                                                          \
	F(SET_IF_A)                                                            \
	F(SET_IF_AE)                                                           \
	F(SET_IF_B)                                                            \
	F(SET_IF_BE)                                                           \
	F(SET_IF_P)                                                            \
	F(SET_IF_NP)                                                           \
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

enum asm_linkage {
	ASM_LINKAGE_INTERNAL,
	ASM_LINKAGE_EXTERNAL,
};

struct asm_function {
	struct string_view identifier;
	enum asm_linkage linkage;
	long long int stack_usage;
	struct asm_op *ops;
	struct asm_function *next;
};

struct asm_variable {
	struct string_view identifier;
	struct ctype c89type; /* determines alignment */
	enum asm_linkage linkage;
	union constant_value initial;
	struct asm_variable *next;
};

struct assembly {
	struct asm_function *functions;
	struct asm_variable *variables;
};

#endif
