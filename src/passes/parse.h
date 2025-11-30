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

struct ast_parameter {
	struct ast_symbol symbol;
	enum ctype parameter_type;
};

struct ast_case {
	long long int constant;
	long long int unique;
	struct ast_case *next;
};

#define FOREACH_AST_NODE_EXPRESSION_PREFIX_OP(F)                               \
	F(EXPRESSION_UNARY_COMPLEMENT, TOKEN_TILDE)                            \
	F(EXPRESSION_UNARY_NEGATE, TOKEN_HYPHEN)                               \
	F(EXPRESSION_UNARY_NOT, TOKEN_EXCLAMATION)                             \
	F(EXPRESSION_PREDECREMENT, TOKEN_HYPHEN_HYPHEN)                        \
	F(EXPRESSION_PREINCREMENT, TOKEN_PLUS_SIGN_PLUS_SIGN)

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

#define FOREACH_AST_NODE_EXPRESSION(F)                                         \
	F(EXPRESSION_NULL)                                                     \
	F(EXPRESSION_PAREN_ENCLOSED)                                           \
	F(EXPRESSION_POSTDECREMENT)                                            \
	F(EXPRESSION_POSTINCREMENT)                                            \
	F(EXPRESSION_VARIABLE_USAGE)                                           \
	F(EXPRESSION_FUNCTION_CALL)                                            \
	F(EXPRESSION_FUNCTION_CALL_ARGUMENTS)                                  \
	F(EXPRESSION_CAST)                                                     \
	FOREACH_AST_NODE_EXPRESSION_PREFIX_OP(F)                               \
	FOREACH_AST_NODE_EXPRESSION_INFIX_OP(F)

#define FOREACH_AST_NODE(F)                                                    \
	F(PROGRAM)                                                             \
	F(FUNCTION)                                                            \
	F(FUNCTION_RETURN_STATEMENT)                                           \
	F(BLOCK)                                                               \
	F(DECLARATION)                                                         \
	F(IF_ELSE)                                                             \
	F(LOOP)                                                                \
	F(BREAK)                                                               \
	F(CONTINUE)                                                            \
	F(GOTO)                                                                \
	F(LABEL)                                                               \
	F(SWITCH)                                                              \
	F(CASE)                                                                \
	F(CASE_DEFAULT)                                                        \
	F(CONSTANT_INT)                                                        \
	F(CONSTANT_LONG)                                                       \
	FOREACH_AST_NODE_EXPRESSION(F)

#define TO_ENUM(nodet, ...) NODE_##nodet,
enum ast_nodetype { FOREACH_AST_NODE(TO_ENUM) };
#undef TO_ENUM

/* flat collection of syntax elements */
struct flat;

/* hierarchical tree of syntax elements */
struct ast {
	enum ast_nodetype node_type;
	union {
		struct {
			struct flat *globals;
		} program;
		struct {
			struct ast_symbol identifier;
			enum ast_specifier specifier;
			enum ctype return_type;
			struct ast_parameter *params;
			struct ast *block;
		} function;
		struct {
			struct flat *statements;
		} block;
		struct {
			struct ast_symbol identifier;
			enum ast_specifier specifier;
			enum ctype var_type;
			struct ast *init;
		} declare;
		struct {
			struct ast *condition;
			struct flat *then_clause;
			struct flat *else_clause;
		} if_;
		struct {
			struct ast *precond;
			struct flat *body;
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
		struct {
			struct string_view target_label;
			long long int target_unique;
		} goto_;
		struct {
			struct string_view name;
			long long int unique;
		} label;
		struct {
			struct ast *control;
			struct flat *body;
			long long int label_default;
			long long int label_end;
			struct ast_case *label_cases; /* computed by sema.c */
		} switch_;
		struct {
			struct string_view constant;
			long long int unique;
		} case_;
		struct {
			enum ctype to_type;
			struct ast *expr;
		} cast;
		struct ast_symbol var; /* NODE_EXPRESSION_VARIABLE_USAGE */
		long long int num;     /* NODE_CONSTANT_INT */
	} u;
	enum ctype expr_type;
};

struct flat {
	struct ast *car;
	struct flat *cdr;
};

/*
 * Utility macro for iterating over dynamically allocated u.function.params
 * array, delimited by a final `struct string_view` with NULL data.
 */
#define FOREACH_FUNCTION_PARAMETER(iter, arr)                                  \
	for (struct ast_parameter * (iter) = arr;                              \
	     (iter) != NULL && (iter)->symbol.name.data != NULL &&             \
	     (iter)->symbol.name.sz > 0;                                       \
	     ++(iter))

#endif
