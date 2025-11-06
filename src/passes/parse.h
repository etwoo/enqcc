#ifndef COMPILER_PASSES_PARSE_H
#define COMPILER_PASSES_PARSE_H

#include "sys/string_view.h"

struct ast {
	enum {
		NODE_PROGRAM,
		NODE_FUNCTION,
		NODE_STATEMENT,
		NODE_EXPRESSION_PRIMITIVE,
		NODE_EXPRESSION_UNARY_NEGATION,
		NODE_EXPRESSION_UNARY_BITWISE_COMPLEMENT,
		NODE_EXPRESSION_PAREN_ENCLOSED,
		NODE_IDENTIFIER,
		NODE_CONSTANT_INT,
	} node_type;
};

struct ast_identifier {
	struct ast base;
	struct string_view token;
};

struct ast_constant {
	struct ast base;
	long long int num;
};

struct ast_expression_constant {
	struct ast base;
	struct ast_constant constant;
};

struct ast_expression_unary_op {
	struct ast base;
	struct ast *operand;
};

struct ast_expression_paren_enclosed {
	struct ast base;
	struct ast *enclosed;
};

struct ast_statement {
	struct ast base;
	struct ast *return_expression;
};

struct ast_function {
	struct ast base;
	struct ast_identifier identifier;
	struct ast_statement statement;
};

struct ast_program {
	struct ast base;
	struct ast_function function;
};

#endif
