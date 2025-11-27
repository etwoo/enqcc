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

#define FOREACH_AST_NODE_INFIX(F, P)                                           \
	F(P##BINARY_ADD, TOKEN_PLUS_SIGN)                                      \
	F(P##BINARY_SUBTRACT, TOKEN_HYPHEN)                                    \
	F(P##BINARY_MULTIPLY, TOKEN_ASTERISK)                                  \
	F(P##BINARY_DIVIDE, TOKEN_FORWARD_SLASH)                               \
	F(P##BINARY_REMAINDER, TOKEN_PERCENT_SIGN)                             \
	F(P##BITWISE_AND, TOKEN_AMPERSAND)                                     \
	F(P##BITWISE_OR, TOKEN_VERT_BAR)                                       \
	F(P##BITWISE_XOR, TOKEN_CARET)                                         \
	F(P##BITWISE_SHIFT_LEFT, TOKEN_LESS_THAN_LESS_THAN)                    \
	F(P##BITWISE_SHIFT_RIGHT, TOKEN_MORE_THAN_MORE_THAN)                   \
	F(P##LOGICAL_AND, TOKEN_AMPERSAND_AMPERSAND)                           \
	F(P##LOGICAL_OR, TOKEN_VERT_BAR_VERT_BAR)                              \
	F(P##VARIABLE_ASSIGNMENT, TOKEN_EQUAL_SIGN)                            \
	F(P##COMPARE_EQUAL, TOKEN_EQUAL_SIGN_EQUAL_SIGN)                       \
	F(P##COMPARE_NOT_EQUAL, TOKEN_EXCLAMATION_EQUAL_SIGN)                  \
	F(P##COMPARE_LESS_THAN, TOKEN_LESS_THAN)                               \
	F(P##COMPARE_LESS_THAN_EQ, TOKEN_LESS_THAN_EQUAL_SIGN)                 \
	F(P##COMPARE_MORE_THAN, TOKEN_MORE_THAN)                               \
	F(P##COMPARE_MORE_THAN_EQ, TOKEN_MORE_THAN_EQUAL_SIGN)                 \
	F(P##COMPOUND_ASSIGN_ADD, TOKEN_PLUS_SIGN_EQUAL_SIGN)                  \
	F(P##COMPOUND_ASSIGN_SUB, TOKEN_HYPHEN_EQUAL_SIGN)                     \
	F(P##COMPOUND_ASSIGN_MUL, TOKEN_ASTERISK_EQUAL_SIGN)                   \
	F(P##COMPOUND_ASSIGN_DIV, TOKEN_FORWARD_SLASH_EQUAL_SIGN)              \
	F(P##COMPOUND_ASSIGN_REM, TOKEN_PERCENT_SIGN_EQUAL_SIGN)               \
	F(P##COMPOUND_ASSIGN_AND, TOKEN_AMPERSAND_EQUAL_SIGN)                  \
	F(P##COMPOUND_ASSIGN_OR, TOKEN_VERT_BAR_EQUAL_SIGN)                    \
	F(P##COMPOUND_ASSIGN_XOR, TOKEN_CARET_EQUAL_SIGN)                      \
	F(P##COMPOUND_ASSIGN_SHL, TOKEN_LESS_THAN_LESS_THAN_EQUAL_SIGN)        \
	F(P##COMPOUND_ASSIGN_SHR, TOKEN_MORE_THAN_MORE_THAN_EQUAL_SIGN)        \
	F(P##TERNARY_CONDITIONAL, TOKEN_QUESTION)

#define FOREACH_AST_NODE_EXPRESSION(F, G, P)                                   \
	F(P##NULL)                                                             \
	F(P##UNARY_COMPLEMENT)                                                 \
	F(P##UNARY_NEGATE)                                                     \
	F(P##UNARY_NOT)                                                        \
	F(P##PAREN_ENCLOSED)                                                   \
	F(P##PREDECREMENT)                                                     \
	F(P##POSTDECREMENT)                                                    \
	F(P##PREINCREMENT)                                                     \
	F(P##POSTINCREMENT)                                                    \
	F(P##VARIABLE_USAGE)                                                   \
	F(P##FUNCTION_CALL)                                                    \
	F(P##FUNCTION_CALL_ARGUMENTS)                                          \
	FOREACH_AST_NODE_INFIX(G, P)

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
	FOREACH_AST_NODE_EXPRESSION(F, G, EXPRESSION_)

#define TO_ENUM(node_type) NODE_##node_type,
#define TO_ENUM_ALT(node_type, token_type) NODE_##node_type,
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
