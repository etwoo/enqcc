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

#define FOREACH_AST_NODETYPE(F)                                                \
	F(PROGRAM)                                                             \
	F(FUNCTION)                                                            \
	F(FUNCTION_RETURN_STATEMENT)                                           \
	F(BLOCK)                                                               \
	F(DECLARATION)                                                         \
	F(IF_ELSE)                                                             \
	F(LOOP)                                                                \
	F(BREAK)                                                               \
	F(CONTINUE)                                                            \
	F(EXPRESSION_NULL)                                                     \
	F(EXPRESSION_UNARY_COMPLEMENT)                                         \
	F(EXPRESSION_UNARY_NEGATE)                                             \
	F(EXPRESSION_UNARY_NOT)                                                \
	F(EXPRESSION_PAREN_ENCLOSED)                                           \
	F(EXPRESSION_BINARY_ADD)                                               \
	F(EXPRESSION_BINARY_SUBTRACT)                                          \
	F(EXPRESSION_BINARY_MULTIPLY)                                          \
	F(EXPRESSION_BINARY_DIVIDE)                                            \
	F(EXPRESSION_BINARY_REMAINDER)                                         \
	F(EXPRESSION_BITWISE_AND)                                              \
	F(EXPRESSION_BITWISE_OR)                                               \
	F(EXPRESSION_BITWISE_XOR)                                              \
	F(EXPRESSION_BITWISE_SHIFT_LEFT)                                       \
	F(EXPRESSION_BITWISE_SHIFT_RIGHT)                                      \
	F(EXPRESSION_LOGICAL_AND)                                              \
	F(EXPRESSION_LOGICAL_OR)                                               \
	F(EXPRESSION_COMPARE_EQUAL)                                            \
	F(EXPRESSION_COMPARE_NOT_EQUAL)                                        \
	F(EXPRESSION_COMPARE_LESS_THAN)                                        \
	F(EXPRESSION_COMPARE_LESS_THAN_EQ)                                     \
	F(EXPRESSION_COMPARE_MORE_THAN)                                        \
	F(EXPRESSION_COMPARE_MORE_THAN_EQ)                                     \
	F(EXPRESSION_VARIABLE_ASSIGNMENT)                                      \
	F(EXPRESSION_VARIABLE_USAGE)                                           \
	F(EXPRESSION_TERNARY_CONDITIONAL)                                      \
	F(EXPRESSION_FUNCTION_CALL)                                            \
	F(EXPRESSION_FUNCTION_CALL_ARGUMENTS)                                  \
	F(CONSTANT_INT)

#define TO_ENUM(node_type) NODE_##node_type,
enum ast_nodetype { FOREACH_AST_NODETYPE(TO_ENUM) };
#undef TO_ENUM

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
