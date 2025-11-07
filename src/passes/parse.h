#ifndef COMPILER_PASSES_PARSE_H
#define COMPILER_PASSES_PARSE_H

#include "sys/string_view.h"

struct ast {
	enum {
		NODE_PROGRAM,
		NODE_FUNCTION,
		NODE_EXPRESSION_UNARY_IDENTITY, /* aka return */
		NODE_EXPRESSION_UNARY_NEGATION,
		NODE_EXPRESSION_UNARY_COMPLEMENT,
		NODE_EXPRESSION_PAREN_ENCLOSED,
		NODE_IDENTIFIER,
		NODE_CONSTANT_INT,
	} node_type;
	union {
		struct {
			struct ast *entrypoint_function;
		} program;
		struct {
			struct ast *identifier;
			struct ast *statement;
		} function;
		struct {
			struct ast *operand;
		} op_unary;
		struct string_view str; /* NODE_IDENTIFIER */
		long long int num;      /* NODE_CONSTANT_INT */
	} u;
};

#endif
