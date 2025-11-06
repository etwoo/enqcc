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
	struct string_view val;
	struct ast *children[2];
};

#endif
