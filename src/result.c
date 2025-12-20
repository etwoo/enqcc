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
	case ERR_EMIT_ALLOC:
		s = strdup("Cannot allocate emit tracking datastructure");
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
	case ERR_LEX_FLOAT_EXPONENT_NO_DIGITS:
		s = my_asprintf("Floating point exponent has no digits: %s",
		                r.msg);
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
	case ERR_PARSE_CALL_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing function call expects TOKEN_PAREN_CLOSE "
		           "after function argument list");
		break;
	case ERR_PARSE_CASE_EXPECT_CONSTANT:
		s = strdup("Parsing case expects integer constant");
		break;
	case ERR_PARSE_CASE_EXPECT_COLON:
		s = strdup("Parsing case expects colon after constant");
		break;
	case ERR_PARSE_CAST_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing cast expects TOKEN_PAREN_CLOSE after type");
		break;
	case ERR_PARSE_CONSTANT_STRTOD:
		s = my_asprintf(
			"Parsing constant expr %s with strtod() failed: %s",
			r.msg,
			my_strerror(r));
		break;
	case ERR_PARSE_CONSTANT_STRTOULL:
		s = my_asprintf(
			"Parsing constant expr %s with strtoull() failed: %s",
			r.msg,
			my_strerror(r));
		break;
	case ERR_PARSE_CONSTANT_TOO_LARGE:
		s = my_asprintf(
			"Parsing constant expr %s: too large for int or long",
			r.msg);
		break;
	case ERR_PARSE_DECL_ATOM_ARRAY_SIZE_FLOATING_POINT:
		s = strdup("Parsing declarator expects integer array size");
		break;
	case ERR_PARSE_DECL_ATOM_ARRAY_SIZE_NON_CONSTANT:
		s = strdup("Parsing declarator expects positive integer "
		           "constant expression as array size");
		break;
	case ERR_PARSE_DECL_ATOM_ARRAY_SIZE_NON_POSITIVE:
		s = strdup("Parsing declarator expects positive array size");
		break;
	case ERR_PARSE_DECL_ATOM_EARLY_SUBSCRIPT:
		s = strdup("Parsing (non-abstract) declarator expects "
		           "identifier before array subscript "
		           "TOKEN_SQUARE_BRACKET_OPEN");
		break;
	case ERR_PARSE_DECL_ATOM_EXPECT_PAREN_CLOSE:
		s = strdup("Parsing declarator expects TOKEN_PAREN_CLOSE after "
		           "TOKEN_PAREN_OPEN and inner declarator");
		break;
	case ERR_PARSE_DECL_ATOM_EXPECT_SQ_BRACKET_CLOSE:
		s = strdup(
			"Parsing declarator expects TOKEN_SQUARE_BRACKET_CLOSE "
			"after TOKEN_SQUARE_BRACKET_OPEN and array size");
		break;
	case ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED:
		s = strdup(
			"Parsing declarator: function pointers not supported");
		break;
	case ERR_PARSE_DECL_ATOM_PARAMS_EXPECT_PAREN_CLOSE:
		s = strdup("Parsing declarator expects TOKEN_PAREN_CLOSE after "
		           "TOKEN_PAREN_OPEN and function parameters");
		break;
	case ERR_PARSE_DECL_ATOM_PARAMS_NESTING:
		s = strdup("Parsing declarator: encountered multiple levels of "
		           "function declarations, suggesting use of function "
		           "pointers, which are not supported");
		break;
	case ERR_PARSE_DECL_ATOM_PARENS_INVALID:
		s = strdup("Parsing declarator: invalid paren-grouping");
		break;
	case ERR_PARSE_DECL_EXPECT_BRACE_CLOSE:
		s = strdup("Parsing compound initializer expects "
		           "TOKEN_BRACE_CLOSE after TOKEN_BRACE_OPEN and "
		           "initializer expression");
		break;
	case ERR_PARSE_DECL_EXPECT_TYPE:
		s = strdup("Parsing variable or function declaration expects "
		           "valid type in type position");
		break;
	case ERR_PARSE_DECL_EXPECT_TOKEN_SEMICOLON:
		s = strdup("Parsing variable declaration expects "
		           "TOKEN_SEMICOLON after initializer expression");
		break;
	case ERR_PARSE_DECL_IDENTIFIER_MISSING:
		s = strdup("Parsing declarator: missing identifier");
		break;
	case ERR_PARSE_DECL_IDENTIFIER_UNEXPECTED:
		s = strdup("Parsing declarator: unexpected identifier in "
		           "abstract declarator");
		break;
	case ERR_PARSE_DECL_SPECIFIER_DUPLICATE:
		s = strdup("Duplicate variable or function specifier");
		break;
	case ERR_PARSE_DECL_TYPE_DOUBLE_INVALID:
		s = strdup("Type 'double' cannot be combined with "
		           "int/long/signed/unsigned");
		break;
	case ERR_PARSE_DECL_TYPE_DUPLICATE:
		s = strdup("Duplicate basic type, like `int int` or "
		           "`signed unsigned`");
		break;
	case ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing paren-enclosed expression expects "
		           "TOKEN_PAREN_CLOSE after expression");
		break;
	case ERR_PARSE_EXPR_EXPECT_TOKEN_SQ_BRACKET_CLOSE:
		s = strdup("Parsing array subscript expression expects "
		           "TOKEN_SQUARE_BRACKET_CLOSE after "
		           "TOKEN_SQUARE_BRACKET_OPEN and array index value");
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
	case ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN:
		s = strdup("Parsing function expects TOKEN_PAREN_OPEN before "
		           "argument list");
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
	case ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_OPEN:
		s = strdup("Parsing switch statement expects TOKEN_PAREN_OPEN "
		           "before controlling expression");
		break;
	case ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing switch statement expects TOKEN_PAREN_CLOSE "
		           "after controlling expression");
		break;
	case ERR_SEMA_ALLOC:
		s = strdup("Cannot allocate sema data");
		break;
	case ERR_SEMA_BREAK_OUTSIDE:
		s = strdup("Invalid break with no enclosing loop");
		break;
	case ERR_SEMA_CASE_DEFAULT_DUPLICATE:
		s = strdup("Duplicate default label in switch");
		break;
	case ERR_SEMA_CASE_DEFAULT_OUTSIDE:
		s = strdup("Invalid default case with no enclosing switch");
		break;
	case ERR_SEMA_CASE_DUPLICATE:
		s = my_asprintf("Duplicate case value: %d", r.num);
		break;
	case ERR_SEMA_CASE_OUTSIDE:
		s = strdup("Invalid case with no enclosing switch");
		break;
	case ERR_SEMA_GOTO_NONEXISTENT_LABEL:
		s = my_asprintf("goto targets non-existent label: %s", r.msg);
		break;
	case ERR_SEMA_INIT_COMPOUND_EMPTY:
		s = strdup("Empty compound initializer is a C23 extension");
		break;
	case ERR_SEMA_INIT_COMPOUND_EXCESS_ELEMENTS:
		s = my_asprintf(
			"Excess elements in compound initializer for array %s",
			r.msg);
		break;
	case ERR_SEMA_INIT_SCALAR_WITH_COMPOUND:
		s = strdup("Scalar variable given compound initializer "
		           "expression");
		break;
	case ERR_SEMA_LABEL_DUPLICATE:
		s = my_asprintf("Duplicate label: %s", r.msg);
		break;
	case ERR_SEMA_LABEL_FOLLOWED_BY_DECLARATION:
		s = my_asprintf("Label %s followed by a variable declaration "
		                "is a C23 extension",
		                r.msg);
		break;
	case ERR_SEMA_LABEL_AT_BLOCK_END:
		s = my_asprintf("Label %s at end of compound statement "
		                "is a C23 extension",
		                r.msg);
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
	case ERR_SEMA_FUNCTION_LINKAGE_BLOCK_SCOPE:
		s = my_asprintf("Block-scope declaration of function %s cannot "
		                "have static storage class",
		                r.msg);
		break;
	case ERR_SEMA_FUNCTION_LINKAGE_CONFLICT:
		s = my_asprintf("static declaration of function %s follows "
		                "non-static declaration",
		                r.msg);
		break;
	case ERR_SEMA_FUNCTION_RETURN_TYPE_ARRAY:
		s = my_asprintf("Function %s cannot return array type", r.msg);

		break;
	case ERR_SEMA_OPERAND_ADDRESS_OF_INVALID:
		s = strdup("Address-of operator & requires lvalue argument");
		break;
	case ERR_SEMA_OPERAND_DEREF_INVALID:
		s = strdup("Pointer dereference operator * requires argument "
		           "of type CTYPE_POINTER_TO");
		break;
	case ERR_SEMA_OPERAND_DOUBLE_INVALID:
		s = strdup(
			"Complement ~, remainder %, and bitwise operations "
			"cannot take arguments of type 'double'; also, values "
			"of type 'double' cannot be cast to pointer type, and "
			"pointer values cannot be cast to type 'double'");
		break;
	case ERR_SEMA_OPERAND_ADD_POINTER_BOTH:
		s = strdup("Addition cannot take two pointer operands");
		break;
	case ERR_SEMA_OPERAND_POINTER_CONFLICT:
		s = strdup("Conflicting pointer types");
		break;
	case ERR_SEMA_OPERAND_POINTER_INVALID:
		s = strdup("Statement or expression cannot take argument of "
		           "pointer type");
		break;
	case ERR_SEMA_OPERAND_POINTER_LHS_VS_NOT_RHS:
		s = strdup("Pointer LHS cannot be compared/converted to "
		           "non-pointer RHS");
		break;
	case ERR_SEMA_OPERAND_POINTER_RHS_VS_NOT_LHS:
		s = strdup("Pointer RHS cannot be compared/converted to "
		           "non-pointer LHS");
		break;
	case ERR_SEMA_OPERAND_SUBSCRIPT_INVALID:
		s = strdup("Array subscript operator [] requires one operand "
		           "of type CTYPE_POINTER_TO or CTYPE_ARRAY_OF and "
		           "another operand of integer type");
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE:
		s = strdup("Invalid lvalue in variable assignment");
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_ARRAY:
		s = strdup("Array type is not assignable");
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_DUPLICATE:
		s = my_asprintf("Duplicate variable declaration: %s", r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_EXTERN_INIT:
		s = my_asprintf("extern declaration of block-scope variable %s "
		                "should not have an initializer",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_EXTERN_MISMATCH:
		s = my_asprintf(
			"extern declaration of block-scope variable %s "
			"redeclares function %s as a different kind of symbol",
			r.msg,
			r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_DUPLICATE:
		s = my_asprintf("Duplicate file-scope variable definition: %s",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_INIT:
		s = my_asprintf(
			"File-scope variable %s has non-constant initializer",
			r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_LINKAGE:
		s = my_asprintf("Declaration of file-scope variable %s with "
		                "conflicting linkage",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_MISMATCH:
		s = my_asprintf(
			"Declaration of file-scope variable %s redeclares "
			"function %s as a different kind of symbol",
			r.msg,
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
	case ERR_SEMA_VARIABLE_DECLARATION_STATIC_INIT:
		s = my_asprintf("Block-scope variable %s with static storage "
		                "class has non-constant initializer",
		                r.msg);
		break;
	case ERR_SEMA_VARIABLE_DECLARATION_TYPE_CONFLICT:
		s = my_asprintf("Conflicting variable declarations/definitions "
		                "with different types: %s",
		                r.msg);
		break;
	case ERR_SYMBOL_ALLOC:
		s = strdup("Cannot allocate symbol");
		break;
	case ERR_CTYPE_ALLOC:
		s = strdup("Cannot allocate C type representation");
		break;
	}

	return s;
}

void
result_str_cleanup(char **s)
{
	free(*s);
}
