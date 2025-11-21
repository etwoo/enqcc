#include "result.h"

#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* for strerror() */

const result_t RESULT_OK = {
	.err = OK,
};

static WARN_UNUSED __attribute__((format(printf, 1, 2))) char *
my_asprintf(const char *pattern, ...)
{
	char *p = NULL;

	va_list ap = {0};
	va_start(ap, pattern);
	int result = vasprintf(&p, pattern, ap);
	va_end(ap);

	return result < 0 ? NULL : p;
}

static WARN_UNUSED result_t
my_debug(result_t r)
{
	if (r.err != OK) {
		auto_result_str str = result_to_str(r);
		debug("Returning result: %s", str);
	}
	return r;
}

result_t
make_result_t(int typ)
{
	return my_debug((result_t){
		.err = typ,
		.num = 0,
		.msg = NULL,
	});
}

result_t
make_result_ti(int typ, int num)
{
	return my_debug((result_t){
		.err = typ,
		.num = num,
		.msg = NULL,
	});
}

result_t
make_result_ts(int typ, const char *msg)
{
	return my_debug((result_t){
		.err = typ,
		.num = 0,
		.msg = strdup(msg),
	});
}

result_t
make_result_tss(int typ, const char *msg, size_t sz)
{
	return my_debug((result_t){
		.err = typ,
		.num = 0,
		.msg = strndup(msg, sz),
	});
}

result_t
make_result_tis(int typ, int num, const char *msg)
{
	return my_debug((result_t){
		.err = typ,
		.num = num,
		.msg = strdup(msg),
	});
}

result_t
make_result_tiss(int typ, int num, const char *msg, size_t sz)
{
	return my_debug((result_t){
		.err = typ,
		.num = num,
		.msg = strndup(msg, sz),
	});
}

void
result_cleanup(result_t *p)
{
	free(p->msg);
	memset(p, 0, sizeof(*p));
}

static WARN_UNUSED const char *
my_strerror(result_t r)
{
	return strerror(r.num);
}

char *
result_to_str(result_t r)
{
	char *s = NULL;

	switch (r.err) {
	case OK:
		s = strdup("Success");
		break;
	case ERR_CODEGEN_ALLOC:
		s = strdup("Cannot allocate codegen element");
		break;
	case ERR_EMIT_FILE_OPEN:
		s = my_asprintf("Error opening output file %s: %s",
		                r.msg,
		                my_strerror(r));
		break;
	case ERR_IR_ALLOC:
		s = strdup("Cannot allocate intermediate representation");
		break;
	case ERR_IR_EXPECT_AST_NODE_EXPRESSION:
		s = my_asprintf("Cannot generate IR for AST node of type=%d "
		                "when expecting NODE_EXPRESSION_*",
		                r.num);
		break;
	case ERR_LEX_OPEN_SOURCE_FILE:
		s = my_asprintf("Error opening source file %s: %s",
		                r.msg,
		                my_strerror(r));
		break;
	case ERR_LEX_ALLOC:
		s = strdup("Cannot allocate token during lex");
		break;
	case ERR_LEX_NO_MATCH:
		s = my_asprintf("No matching expression to lex: %s", r.msg);
		break;
	case ERR_LEX_IDENTIFIER_CONSTANT_KEYWORD_PEEK_ERROR:
		s = my_asprintf(
			"Identifier, constant, or keyword \"%s\" followed by "
			"unexpected character '%c' during lex",
			r.msg,
			r.num);
		break;
	case ERR_PARSE_ALLOC:
		s = strdup("Cannot allocate ast node during parse");
		break;
	case ERR_PARSE_CONSTANT_STRTOLL:
		s = my_asprintf(
			"Parsing constant expr %s with strtoll() failed: %s",
			r.msg,
			my_strerror(r));
		break;
	case ERR_PARSE_DECL_EXPECT_TYPE_INT:
		s = strdup("Parsing variable declaration expects "
		           "TOKEN_KEYWORD_INT in type position");
		break;
	case ERR_PARSE_DECL_EXPECT_TYPE_REASONABLE:
		s = strdup("Parsing variable type; encountered unreasonable "
		           "token");
		break;
	case ERR_PARSE_DECL_TYPE_DUPLICATE:
		s = strdup("Duplicate variable type");
		break;
	case ERR_PARSE_DECL_SPECIFIER_DUPLICATE:
		s = strdup("Duplicate variable specifier");
		break;
	case ERR_PARSE_DECL_EXPECT_TOKEN_IDENTIFIER:
		s = strdup("Parsing variable declaration expects "
		           "TOKEN_IDENTIFIER in variable name position");
		break;
	case ERR_PARSE_DECL_EXPECT_TOKEN_SEMICOLON:
		s = strdup("Parsing variable declaration expects "
		           "TOKEN_SEMICOLON after initializer expression");
		break;
	case ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing paren-enclosed expression expects "
		           "TOKEN_PAREN_CLOSE after expression");
		break;
	case ERR_PARSE_EXPR_EXPECT_COLON_IN_TERNARY_OP:
		s = strdup("Parsing ternary conditional operator expects "
		           "TOKEN_COLON after then-expression and before "
		           "else-expression");
		break;
	case ERR_PARSE_EXPR_EXPECT_REASONABLE:
		s = strdup(
			"Parsing expression; encountered unreasonable token");
		break;
	case ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT:
		s = strdup("Parsing function expects TOKEN_KEYWORD_INT in "
		           "return type position");
		break;
	case ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_REASONABLE:
		s = my_asprintf("Parsing function return type; encountered "
		                "unreasonable token: %d",
		                r.num);
		break;
	case ERR_PARSE_FUNC_RETURN_TYPE_DUPLICATE:
		s = strdup("Duplicate function return type");
		break;
	case ERR_PARSE_FUNC_SPECIFIER_DUPLICATE:
		s = strdup("Duplicate function specifier");
		break;
	case ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER:
		s = strdup("Parsing function expects TOKEN_IDENTIFIER in "
		           "function name position");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN:
		s = strdup("Parsing function expects TOKEN_PAREN_OPEN before "
		           "argument list");
		break;
	case ERR_PARSE_FUNC_PARAM_EXPECT_TYPE_INT:
		s = strdup("Parsing function parameter expects "
		           "TOKEN_KEYWORD_INT in parameter type position");
		break;
	case ERR_PARSE_FUNC_PARAM_EXPECT_TOKEN_IDENTIFIER:
		s = strdup("Parsing function parameter expects "
		           "TOKEN_IDENTIFIER in parameter name position");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing function expects TOKEN_PAREN_CLOSE after "
		           "argument list");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_SEMICOLON_OR_BRACE_OPEN:
		s = strdup("Parsing function expects TOKEN_SEMICOLON or "
		           "TOKEN_BRACE_OPEN after TOKEN_PAREN_CLOSE");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN:
		s = strdup("Parsing function expects TOKEN_BRACE_OPEN before "
		           "function body statement(s)");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE:
		s = strdup("Parsing function expects TOKEN_BRACE_CLOSE after "
		           "function body statement(s)");
		break;
	case ERR_PARSE_CALL_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing function call expects TOKEN_PAREN_CLOSE "
		           "after function argument list");
		break;
	case ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_OPEN:
		s = strdup("Parsing if statement expects TOKEN_PAREN_OPEN "
		           "before controlling condition expression");
		break;
	case ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing if statement expects TOKEN_PAREN_CLOSE "
		           "after controlling condition expression");
		break;
	case ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN:
		s = strdup("Parsing loop expects TOKEN_PAREN_OPEN before "
		           "controlling condition expression");
		break;
	case ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing loop expects TOKEN_PAREN_CLOSE after "
		           "controlling condition expression");
		break;
	case ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON:
		s = strdup("Parsing loop expects TOKEN_SEMICOLON");
		break;
	case ERR_PARSE_LOOP_EXPECT_TOKEN_WHILE:
		s = strdup("Parsing loop expects TOKEN_KEYWORD_WHILE after "
		           "do-loop body and before do-loop controlling "
		           "expression");
		break;
	case ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON:
		s = strdup("Parsing statement expects TOKEN_SEMICOLON after "
		           "expression");
		break;
	case ERR_SEMA_BREAK_OUTSIDE:
		s = strdup("Invalid break with no enclosing loop");
		break;
	case ERR_SEMA_CONTINUE_OUTSIDE:
		s = strdup("Invalid continue with no enclosing loop");
		break;
	case ERR_SEMA_FUNCTION_CALL_UNCALLABLE:
		s = my_asprintf("Call of non-function variable: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_CALL_UNDECLARED:
		s = my_asprintf("Call of undeclared function: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_CALL_WRONG_NUMBER_OF_ARGS:
		s = my_asprintf("Incorrect arguments to call of: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_DEFINITION_CONFLICT:
		s = my_asprintf("Conflicting function definition: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_DEFINITION_DUPLICATE:
		s = my_asprintf("Duplicate function definition: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_DEFINITION_NESTED:
		s = my_asprintf("Nested function definition: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_DEFINITION_PARAM_DUPLICATE:
		s = my_asprintf("Duplicate function parameter: %s", r.msg);
		break;
	case ERR_SEMA_FUNCTION_DECLARATION_LINKAGE_CONFLICT:
		s = my_asprintf("Invalid static specifier for block scope "
		                "function declaration: %s",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE:
		s = strdup("Invalid lvalue in variable assignment");
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_DUPLICATE:
		s = my_asprintf("Duplicate variable declaration: %s", r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_INVALID_INIT:
		s = my_asprintf("Invalid initializer for block scope extern "
		                "variable declaration: %s",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_INVALID_FUNC:
		s = my_asprintf("Invalid use of function '%s' as lvalue or "
		                "rvalue; cannot assign value to function or "
		                "use function as a value",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_USAGE_WITHOUT_DECLARATION:
		s = my_asprintf("Reference to undeclared variable: %s", r.msg);
		break;
	}

	return s;
}

void
result_str_cleanup(char **s)
{
	free(*s);
}
