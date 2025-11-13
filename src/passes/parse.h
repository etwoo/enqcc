#ifndef COMPILER_PASSES_PARSE_H
#define COMPILER_PASSES_PARSE_H

#include "sys/string_view.h"

struct ast_symbol {
	struct string_view name;
	long long int unique;
};

struct ast {
	enum {
		NODE_PROGRAM,
		NODE_FUNCTION,
		NODE_FUNCTION_RETURN_STATEMENT,
		NODE_BLOCK,
		NODE_DECLARATION,
		NODE_EXPRESSION_NULL,
		NODE_EXPRESSION_UNARY_COMPLEMENT,
		NODE_EXPRESSION_UNARY_NEGATE,
		NODE_EXPRESSION_UNARY_NOT,
		NODE_EXPRESSION_PAREN_ENCLOSED,
		NODE_EXPRESSION_BINARY_ADD,
		NODE_EXPRESSION_BINARY_SUBTRACT,
		NODE_EXPRESSION_BINARY_MULTIPLY,
		NODE_EXPRESSION_BINARY_DIVIDE,
		NODE_EXPRESSION_BINARY_REMAINDER,
		NODE_EXPRESSION_LOGICAL_AND,
		NODE_EXPRESSION_LOGICAL_OR,
		NODE_EXPRESSION_COMPARE_EQUAL,
		NODE_EXPRESSION_COMPARE_NOT_EQUAL,
		NODE_EXPRESSION_COMPARE_LESS_THAN,
		NODE_EXPRESSION_COMPARE_LESS_THAN_EQ,
		NODE_EXPRESSION_COMPARE_MORE_THAN,
		NODE_EXPRESSION_COMPARE_MORE_THAN_EQ,
		NODE_EXPRESSION_VARIABLE_USAGE,
		NODE_EXPRESSION_VARIABLE_ASSIGNMENT,
		NODE_CONSTANT_INT,
	} node_type;
	union {
		struct {
			struct ast *entrypoint_function;
		} program;
		struct {
			struct ast_symbol identifier;
			struct ast *block;
		} function;
		struct {
			struct ast *item;
			struct ast *next;
		} block;
		struct {
			struct ast_symbol identifier;
			struct ast *init;
		} declare;
		struct {
			struct ast *operand;
		} op_unary;
		struct {
			struct ast *lhs;
			struct ast *rhs;
		} op_binary;
		struct ast_symbol var; /* NODE_EXPRESSION_VARIABLE_USAGE */
		long long int num;     /* NODE_CONSTANT_INT */
	} u;
};

#endif
