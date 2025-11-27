#ifndef COMPILER_PASSES_PARSE_H
#define COMPILER_PASSES_PARSE_H

#include "passes/symbol.h"
#include "sys/string_view.h"

enum ast_specifier {
	SPECIFIER_NONE,
	SPECIFIER_STATIC,
	SPECIFIER_EXTERN,
};

struct ast_symbol {
	struct string_view name;
	long long int unique;
	enum symbol_type stype;
	enum symbol_linkage ltype;
};

#define FOREACH_AST_NODE_EXPRESSION_INFIX_OP(F)                                \
	F(EXPRESSION_BINARY_ADD, TOKEN_PLUS_SIGN)                              \
	F(EXPRESSION_BINARY_SUBTRACT, TOKEN_HYPHEN)                            \
	F(EXPRESSION_BINARY_MULTIPLY, TOKEN_ASTERISK)                          \
	F(EXPRESSION_BINARY_DIVIDE, TOKEN_FORWARD_SLASH)                       \
	F(EXPRESSION_BINARY_REMAINDER, TOKEN_PERCENT_SIGN)                     \
	F(EXPRESSION_BITWISE_AND, TOKEN_AMPERSAND)                             \
	F(EXPRESSION_BITWISE_OR, TOKEN_VERT_BAR)                               \
	F(EXPRESSION_BITWISE_XOR, TOKEN_CARET)                                 \
	F(EXPRESSION_BITWISE_SHIFT_LEFT, TOKEN_LESS_THAN_LESS_THAN)            \
	F(EXPRESSION_BITWISE_SHIFT_RIGHT, TOKEN_MORE_THAN_MORE_THAN)           \
	F(EXPRESSION_LOGICAL_AND, TOKEN_AMPERSAND_AMPERSAND)                   \
	F(EXPRESSION_LOGICAL_OR, TOKEN_VERT_BAR_VERT_BAR)                      \
	F(EXPRESSION_VARIABLE_ASSIGNMENT, TOKEN_EQUAL_SIGN)                    \
	F(EXPRESSION_COMPARE_EQUAL, TOKEN_EQUAL_SIGN_EQUAL_SIGN)               \
	F(EXPRESSION_COMPARE_NOT_EQUAL, TOKEN_EXCLAMATION_EQUAL_SIGN)          \
	F(EXPRESSION_COMPARE_LESS_THAN, TOKEN_LESS_THAN)                       \
	F(EXPRESSION_COMPARE_LESS_THAN_EQ, TOKEN_LESS_THAN_EQUAL_SIGN)         \
	F(EXPRESSION_COMPARE_MORE_THAN, TOKEN_MORE_THAN)                       \
	F(EXPRESSION_COMPARE_MORE_THAN_EQ, TOKEN_MORE_THAN_EQUAL_SIGN)         \
	F(EXPRESSION_COMPOUND_ASSIGN_ADD, TOKEN_PLUS_SIGN_EQUAL_SIGN)          \
	F(EXPRESSION_COMPOUND_ASSIGN_SUB, TOKEN_HYPHEN_EQUAL_SIGN)             \
	F(EXPRESSION_COMPOUND_ASSIGN_MUL, TOKEN_ASTERISK_EQUAL_SIGN)           \
	F(EXPRESSION_COMPOUND_ASSIGN_DIV, TOKEN_FORWARD_SLASH_EQUAL_SIGN)      \
	F(EXPRESSION_COMPOUND_ASSIGN_REM, TOKEN_PERCENT_SIGN_EQUAL_SIGN)       \
	F(EXPRESSION_COMPOUND_ASSIGN_AND, TOKEN_AMPERSAND_EQUAL_SIGN)          \
	F(EXPRESSION_COMPOUND_ASSIGN_OR, TOKEN_VERT_BAR_EQUAL_SIGN)            \
	F(EXPRESSION_COMPOUND_ASSIGN_XOR, TOKEN_CARET_EQUAL_SIGN)              \
	F(EXPRESSION_COMPOUND_ASSIGN_SL, TOKEN_LESS_THAN_LESS_THAN_EQUAL_SIGN) \
	F(EXPRESSION_COMPOUND_ASSIGN_SR, TOKEN_MORE_THAN_MORE_THAN_EQUAL_SIGN) \
	F(EXPRESSION_TERNARY_CONDITIONAL, TOKEN_QUESTION)

#define FOREACH_AST_NODE_EXPRESSION_PREFIX_OP(F)                               \
	F(EXPRESSION_UNARY_COMPLEMENT, TOKEN_TILDE)                            \
	F(EXPRESSION_UNARY_NEGATE, TOKEN_HYPHEN)                               \
	F(EXPRESSION_UNARY_NOT, TOKEN_EXCLAMATION)                             \
	F(EXPRESSION_PREDECREMENT, TOKEN_HYPHEN_HYPHEN)                        \
	F(EXPRESSION_PREINCREMENT, TOKEN_PLUS_SIGN_PLUS_SIGN)

#define FOREACH_AST_NODE_EXPRESSION(F, G)                                      \
	F(EXPRESSION_NULL)                                                     \
	F(EXPRESSION_PAREN_ENCLOSED)                                           \
	F(EXPRESSION_POSTDECREMENT)                                            \
	F(EXPRESSION_POSTINCREMENT)                                            \
	F(EXPRESSION_VARIABLE_USAGE)                                           \
	F(EXPRESSION_FUNCTION_CALL)                                            \
	F(EXPRESSION_FUNCTION_CALL_ARGUMENTS)                                  \
	FOREACH_AST_NODE_EXPRESSION_PREFIX_OP(G)                               \
	FOREACH_AST_NODE_EXPRESSION_INFIX_OP(G)

#define FOREACH_AST_NODE(F, G)                                                 \
	F(PROGRAM)                                                             \
	F(FUNCTION)                                                            \
	F(FUNCTION_RETURN_STATEMENT)                                           \
	F(BLOCK)                                                               \
	F(DECLARATION)                                                         \
	F(IF_ELSE)                                                             \
	F(LOOP)                                                                \
	F(BREAK)                                                               \
	F(CONTINUE)                                                            \
	F(CONSTANT_INT)                                                        \
	FOREACH_AST_NODE_EXPRESSION(F, G)

#define TO_ENUM(nodet) NODE_##nodet,
#define TO_ENUM_ALT(nodet, tokent) NODE_##nodet,
enum ast_nodetype { FOREACH_AST_NODE(TO_ENUM, TO_ENUM_ALT) };
#undef TO_ENUM
#undef TO_ENUM_ALT

struct ast {
	enum ast_nodetype node_type;
	union {
		struct {
			struct ast *globals;
		} program;
		struct {
			struct ast_symbol identifier;
			enum ast_specifier specifier;
			struct ast_symbol *params;
			struct ast *block;
			struct ast *next;
		} function;
		struct {
			struct ast *item;
			struct ast *next;
		} block;
		struct {
			struct ast_symbol identifier;
			enum ast_specifier specifier;
			struct ast *init;
			struct ast *next;
		} declare;
		struct {
			struct ast *condition;
			struct ast *then_clause;
			struct ast *else_clause;
		} if_;
		struct {
			struct ast *precond;
			struct ast *body;
			struct ast *incr;
			struct ast *postcond;
			long long int label_end;
			long long int label_continue;
			long long int label_start; /* also ID of loop itself */
		} loop;
		struct {
			struct ast *operand;
		} op_unary;
		struct {
			struct ast *lhs;
			struct ast *rhs;
		} op_binary;
		struct {
			struct ast *condition;
			struct ast *then_expr;
			struct ast *else_expr;
		} op_ternary;
		struct {
			struct ast_symbol identifier;
			struct ast *arguments;
		} call;
		struct {
			struct ast *expr;
			struct ast *next;
		} call_args;
		struct ast_symbol var; /* NODE_EXPRESSION_VARIABLE_USAGE */
		long long int num;     /* NODE_CONSTANT_INT */
	} u;
};

/*
 * Utility macro for iterating over dynamically allocated u.function.params
 * array, delimited by a final `struct string_view` with NULL data.
 */
#define FOREACH_FUNCTION_PARAMETER(iter, arr)                                  \
	for (struct ast_symbol * (iter) = arr;                                 \
	     (iter) != NULL && (iter)->name.data != NULL &&                    \
	     (iter)->name.sz > 0;                                              \
	     ++(iter))

#endif
