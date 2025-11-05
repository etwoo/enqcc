#ifndef COMPILER_PASSES_PARSE_H
#define COMPILER_PASSES_PARSE_H

#include "sys/string_view.h"

struct ast {
	enum {
		NODE_PROGRAM,
		NODE_FUNCTION,
		NODE_STATEMENT,
		NODE_EXPRESSION,
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

struct ast_expression {
	struct ast base;
	struct ast_constant constant;
};

struct ast_statement {
	struct ast base;
	struct ast_expression return_expression;
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
