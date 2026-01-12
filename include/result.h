#ifndef RESULT_H
#define RESULT_H

#include <stddef.h> /* for size_t */

/*
 * result_t: generic result type used by compiler subsystems
 */
typedef struct {
	enum {
		OK = 0,
		ERR_CODEGEN_ALLOC,
		ERR_EMIT_FILE_OPEN,
		ERR_EMIT_ALLOC,
		ERR_IR_ALLOC,
		ERR_IR_EXPECT_AST_NODE_EXPRESSION,
		ERR_LEX_OPEN_SOURCE_FILE,
		ERR_LEX_ALLOC,
		ERR_LEX_CHAR_ESCAPE_INVALID,
		ERR_LEX_CHAR_EXPECT_MORE,
		ERR_LEX_CHAR_INVALID_EMPTY,
		ERR_LEX_CHAR_INVALID_MULTICHAR,
		ERR_LEX_CHAR_INVALID_NEWLINE,
		ERR_LEX_FLOAT_EXPONENT_NO_DIGITS,
		ERR_LEX_NO_MATCH,
		ERR_LEX_IDENTIFIER_CONSTANT_KEYWORD_PEEK_ERROR,
		ERR_PARSE_ALLOC,
		ERR_PARSE_CALL_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_CASE_EXPECT_CONSTANT,
		ERR_PARSE_CASE_EXPECT_COLON,
		ERR_PARSE_CAST_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_CONSTANT_STRTOD,
		ERR_PARSE_CONSTANT_STRTOULL,
		ERR_PARSE_CONSTANT_TOO_LARGE,
		ERR_PARSE_DECL_ATOM_ARRAY_SIZE_FLOATING_POINT,
		ERR_PARSE_DECL_ATOM_ARRAY_SIZE_NON_CONSTANT,
		ERR_PARSE_DECL_ATOM_ARRAY_SIZE_NON_POSITIVE,
		ERR_PARSE_DECL_ATOM_EARLY_SUBSCRIPT,
		ERR_PARSE_DECL_ATOM_EXPECT_PAREN_CLOSE,
		ERR_PARSE_DECL_ATOM_EXPECT_SQ_BRACKET_CLOSE,
		ERR_PARSE_DECL_ATOM_FUNC_PTR_UNSUPPORTED,
		ERR_PARSE_DECL_ATOM_MEMBER_ACCESS_INVALID,
		ERR_PARSE_DECL_ATOM_PARAMS_EXPECT_PAREN_CLOSE,
		ERR_PARSE_DECL_ATOM_PARAMS_NESTING,
		ERR_PARSE_DECL_ATOM_PARENS_INVALID,
		ERR_PARSE_DECL_ATOM_POINTER_AFTER_PARENS,
		ERR_PARSE_DECL_EXPECT_BRACE_CLOSE,
		ERR_PARSE_DECL_EXPECT_TYPE,
		ERR_PARSE_DECL_EXPECT_TOKEN_SEMICOLON,
		ERR_PARSE_DECL_IDENTIFIER_MISSING,
		ERR_PARSE_DECL_IDENTIFIER_UNEXPECTED,
		ERR_PARSE_DECL_SPECIFIER_DUPLICATE,
		ERR_PARSE_DECL_TYPE_ARRAY_INCOMPLETE,
		ERR_PARSE_DECL_TYPE_CHAR_INVALID,
		ERR_PARSE_DECL_TYPE_DOUBLE_INVALID,
		ERR_PARSE_DECL_TYPE_DUPLICATE,
		ERR_PARSE_DECL_TYPE_STRUCT_INVALID,
		ERR_PARSE_DECL_TYPE_VOID_INVALID,
		ERR_PARSE_DECL_TYPE_VOID_PARAM_TYPE,
		ERR_PARSE_DECL_TYPE_VOID_VAR_TYPE,
		ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_EXPR_EXPECT_TOKEN_SQ_BRACKET_CLOSE,
		ERR_PARSE_EXPR_EXPECT_COLON_IN_TERNARY_OP,
		ERR_PARSE_EXPR_EXPECT_REASONABLE,
		ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_FUNC_EXPECT_TOKEN_SEMICOLON_OR_BRACE_OPEN,
		ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN,
		ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE,
		ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON,
		ERR_PARSE_LOOP_EXPECT_TOKEN_WHILE,
		ERR_PARSE_MEMBER_ACCESS_EXPECT_IDENTIFIER,
		ERR_PARSE_SIZEOF_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON,
		ERR_PARSE_STRUCT_DECL_EMPTY_INVALID,
		ERR_PARSE_STRUCT_DECL_MEMBER_INIT,
		ERR_PARSE_STRUCT_DECL_MEMBER_SPEC,
		ERR_PARSE_STRUCT_EXPECT_TOKEN_SEMICOLON,
		ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_SWITCH_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_SEMA_ALLOC,
		ERR_SEMA_BREAK_OUTSIDE,
		ERR_SEMA_CASE_DEFAULT_DUPLICATE,
		ERR_SEMA_CASE_DEFAULT_OUTSIDE,
		ERR_SEMA_CASE_DUPLICATE,
		ERR_SEMA_CASE_OUTSIDE,
		ERR_SEMA_CAST_TO_ARRAY_OR_STRUCT_INVALID,
		ERR_SEMA_CONTINUE_OUTSIDE,
		ERR_SEMA_GOTO_NONEXISTENT_LABEL,
		ERR_SEMA_INIT_COMPOUND_EMPTY,
		ERR_SEMA_INIT_COMPOUND_EXCESS_ELEMENTS,
		ERR_SEMA_INIT_SCALAR_WITH_COMPOUND,
		ERR_SEMA_INIT_STR_LITERAL_INVALID,
		ERR_SEMA_LABEL_DUPLICATE,
		ERR_SEMA_LABEL_FOLLOWED_BY_DECLARATION,
		ERR_SEMA_LABEL_AT_BLOCK_END,
		ERR_SEMA_FUNCTION_CALL_UNCALLABLE,
		ERR_SEMA_FUNCTION_CALL_UNDECLARED,
		ERR_SEMA_FUNCTION_CALL_WRONG_NUMBER_OF_ARGS,
		ERR_SEMA_FUNCTION_DEFINITION_CONFLICT,
		ERR_SEMA_FUNCTION_DEFINITION_DUPLICATE,
		ERR_SEMA_FUNCTION_DEFINITION_NESTED,
		ERR_SEMA_FUNCTION_DEFINITION_PARAM_DUPLICATE,
		ERR_SEMA_FUNCTION_DEFINITION_PARAM_INCOMPLETE,
		ERR_SEMA_FUNCTION_DEFINITION_RETURN_INCOMPLETE,
		ERR_SEMA_FUNCTION_LINKAGE_BLOCK_SCOPE,
		ERR_SEMA_FUNCTION_LINKAGE_CONFLICT,
		ERR_SEMA_FUNCTION_RETURN_TYPE_ARRAY,
		ERR_SEMA_OPERAND_ADD_POINTER_BOTH,
		ERR_SEMA_OPERAND_ADD_POINTER_VOID,
		ERR_SEMA_OPERAND_ADDRESS_OF_INVALID,
		ERR_SEMA_OPERAND_CHAR_ARRAY_SIZE,
		ERR_SEMA_OPERAND_DEREF_INVALID,
		ERR_SEMA_OPERAND_DEREF_VOID_PTR,
		ERR_SEMA_OPERAND_DOUBLE_INVALID,
		ERR_SEMA_OPERAND_MEMBER_INCOMPLETE,
		ERR_SEMA_OPERAND_MEMBER_INVALID,
		ERR_SEMA_OPERAND_MEMBER_NONEXISTENT,
		ERR_SEMA_OPERAND_POINTER_CONFLICT,
		ERR_SEMA_OPERAND_POINTER_INVALID,
		ERR_SEMA_OPERAND_POINTER_LHS_VS_NOT_RHS,
		ERR_SEMA_OPERAND_POINTER_RHS_VS_NOT_LHS,
		ERR_SEMA_OPERAND_SCALAR_REQUIRED,
		ERR_SEMA_OPERAND_SIZEOF_INCOMPLETE,
		ERR_SEMA_OPERAND_TERNARY_MISMATCH,
		ERR_SEMA_RETURN_STATEMENT_EXPECT_VALUE,
		ERR_SEMA_RETURN_STATEMENT_EXPECT_VOID,
		ERR_SEMA_RETURN_STATEMENT_STRUCT_MISMATCH,
		ERR_SEMA_STRUCT_MEMBER_NAME_DUPLICATE,
		ERR_SEMA_STRUCT_MEMBER_TYPE_INCOMPLETE,
		ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE,
		ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_ARRAY,
		ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_STRUCT,
		ERR_SEMA_VARIABLE_DECLARATION_DUPLICATE,
		ERR_SEMA_VARIABLE_DECLARATION_EXTERN_INIT,
		ERR_SEMA_VARIABLE_DECLARATION_EXTERN_MISMATCH,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_DUPLICATE,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_INIT,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_LINKAGE,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_MISMATCH,
		ERR_SEMA_VARIABLE_DECLARATION_INVALID_FUNC,
		ERR_SEMA_VARIABLE_DECLARATION_STATIC_INIT,
		ERR_SEMA_VARIABLE_DECLARATION_STRUCT_DUPLICATE,
		ERR_SEMA_VARIABLE_DECLARATION_STRUCT_INCOMPLETE,
		ERR_SEMA_VARIABLE_DECLARATION_STRUCT_INVALID,
		ERR_SEMA_VARIABLE_DECLARATION_TYPE_CONFLICT,
		ERR_SEMA_VARIABLE_USAGE_WITHOUT_DECLARATION,
		ERR_SYMBOL_ALLOC,
		ERR_CTYPE_ALLOC,
	} err;
	int num; /* may hold errno, CURLcode, CURLUcode, etc */
	char *msg;
} result_t;

/*
 * RESULT_OK: sentinel that represents generic success, not specific to any
 * particular subsystem or function
 */
extern const result_t RESULT_OK;

/*
 * Implementation details of make_result(); result_t users can ignore the
 * following macro glue.
 */

#define DECLARE_MAKERESULT_IMPL(x, ...)                                        \
	result_t make_result_##x(__VA_ARGS__)                                  \
		__attribute__((warn_unused_result)) __attribute__((cold))

DECLARE_MAKERESULT_IMPL(t, int typ);
DECLARE_MAKERESULT_IMPL(ti, int typ, int num);
DECLARE_MAKERESULT_IMPL(ts, int typ, const char *msg);
DECLARE_MAKERESULT_IMPL(tss, int typ, const char *msg, size_t sz);
DECLARE_MAKERESULT_IMPL(tis, int typ, int num, const char *msg);
DECLARE_MAKERESULT_IMPL(tiss, int typ, int num, const char *msg, size_t sz);

#undef DECLARE_MAKERESULT_IMPL

#define make_result_str_arg(suffix)                                            \
	char * : make_result_##suffix, const char * : make_result_##suffix
#define make_result_2arg(x, y)                                                 \
	_Generic(y, int: make_result_ti, make_result_str_arg(ts))(x, y)
#define make_result_3arg(x, y, z)                                              \
	_Generic(y, int: make_result_tis, make_result_str_arg(tss))(x, y, z)

#define CHOOSE_MACRO_BY_ARGN(w, x, y, z, NAME, ...) NAME

/*
 * Create a result_t by passing any of the following sets of arguments:
 *
 * - an ERR_* value (alone)
 * - an ERR_* value and an errno (or similar int status code)
 * - an ERR_* value and a string (null-terminated or explicit span)
 * - an ERR_* value, an errno, and a string (null-terminated or explicit span)
 */
#define make_result(...)                                                       \
	CHOOSE_MACRO_BY_ARGN(__VA_ARGS__,                                      \
	                     make_result_tiss,                                 \
	                     make_result_3arg,                                 \
	                     make_result_2arg,                                 \
	                     make_result_t,                                    \
	                     make_result_sentinel)                             \
	(__VA_ARGS__)

/*
 * Return if <expr> yields a non-OK result_t.
 */
#define check(expr)                                                            \
	do {                                                                   \
		result_t x = expr;                                             \
		if (__builtin_expect(x.err != OK, 0)) {                        \
			return x;                                              \
		}                                                              \
	} while (0)

/*
 * Return a result_t if a given (arbitrary) condition evaluates to true.
 */
#define check_if(cond, ...)                                                    \
	do {                                                                   \
		if (cond) {                                                    \
			return make_result(__VA_ARGS__);                       \
		}                                                              \
	} while (0)

/*
 * Return on non-zero <val>, while also capturing <val> in the result_t.
 *
 * Note: <val> would typically originate from CURLcode, CURLUcode, or similar.
 */
#define check_if_num(val, err_type) check_if(val, err_type, (int)(val))

/*
 * Convenience helper for use with __attribute__((cleanup))
 */
void result_cleanup(result_t *p);

/*
 * Take ownership of a result_t value and free() its members upon completion.
 */
#define auto_result result_t __attribute__((cleanup(result_cleanup)))

/*
 * Convert a result_t into a human-readable error message.
 *
 * Note: caller has responsibility to free() the returned pointer.
 */
char *result_to_str(result_t r) __attribute__((warn_unused_result));

/*
 * Convenience helper for use with __attribute__((cleanup))
 */
void result_str_cleanup(char **s);

/*
 * Take ownership of a result_to_str() value and free() it upon completion.
 */
#define auto_result_str char *__attribute__((cleanup(result_str_cleanup)))

#endif
