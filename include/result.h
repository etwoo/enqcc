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
		ERR_IR_ALLOC,
		ERR_IR_EXPECT_AST_NODE_EXPRESSION,
		ERR_LEX_OPEN_SOURCE_FILE,
		ERR_LEX_ALLOC,
		ERR_LEX_NO_MATCH,
		ERR_LEX_IDENTIFIER_CONSTANT_KEYWORD_PEEK_ERROR,
		ERR_PARSE_ALLOC,
		ERR_PARSE_CONSTANT_STRTOLL,
		ERR_PARSE_DECL_EXPECT_TYPE_INT,
		ERR_PARSE_DECL_TYPE_DUPLICATE,
		ERR_PARSE_DECL_SPECIFIER_DUPLICATE,
		ERR_PARSE_DECL_EXPECT_TOKEN_IDENTIFIER,
		ERR_PARSE_DECL_EXPECT_TOKEN_SEMICOLON,
		ERR_PARSE_EXPR_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_EXPR_EXPECT_COLON_IN_TERNARY_OP,
		ERR_PARSE_EXPR_EXPECT_REASONABLE,
		ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT,
		ERR_PARSE_FUNC_RETURN_TYPE_DUPLICATE,
		ERR_PARSE_FUNC_SPECIFIER_DUPLICATE,
		ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER,
		ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_FUNC_PARAM_EXPECT_TYPE_INT,
		ERR_PARSE_FUNC_PARAM_EXPECT_TOKEN_IDENTIFIER,
		ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_FUNC_EXPECT_TOKEN_SEMICOLON_OR_BRACE_OPEN,
		ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN,
		ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE,
		ERR_PARSE_CALL_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_IF_ELSE_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_OPEN,
		ERR_PARSE_LOOP_EXPECT_TOKEN_PAREN_CLOSE,
		ERR_PARSE_LOOP_EXPECT_TOKEN_SEMICOLON,
		ERR_PARSE_LOOP_EXPECT_TOKEN_WHILE,
		ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON,
		ERR_SEMA_ALLOC,
		ERR_SEMA_BREAK_OUTSIDE,
		ERR_SEMA_CONTINUE_OUTSIDE,
		ERR_SEMA_GOTO_NONEXISTENT_LABEL,
		ERR_SEMA_LABEL_DUPLICATE,
		ERR_SEMA_LABEL_FOLLOWED_BY_DECLARATION,
		ERR_SEMA_FUNCTION_CALL_UNCALLABLE,
		ERR_SEMA_FUNCTION_CALL_UNDECLARED,
		ERR_SEMA_FUNCTION_CALL_WRONG_NUMBER_OF_ARGS,
		ERR_SEMA_FUNCTION_DEFINITION_CONFLICT,
		ERR_SEMA_FUNCTION_DEFINITION_DUPLICATE,
		ERR_SEMA_FUNCTION_DEFINITION_NESTED,
		ERR_SEMA_FUNCTION_DEFINITION_PARAM_DUPLICATE,
		ERR_SEMA_FUNCTION_LINKAGE_BLOCK_SCOPE,
		ERR_SEMA_FUNCTION_LINKAGE_CONFLICT,
		ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE,
		ERR_SEMA_VARIABLE_DECLARATION_DUPLICATE,
		ERR_SEMA_VARIABLE_DECLARATION_EXTERN_INIT,
		ERR_SEMA_VARIABLE_DECLARATION_EXTERN_MISMATCH,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_DUPLICATE,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_INIT,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_LINKAGE,
		ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_MISMATCH,
		ERR_SEMA_VARIABLE_DECLARATION_INVALID_FUNC,
		ERR_SEMA_VARIABLE_DECLARATION_STATIC_INIT,
		ERR_SEMA_VARIABLE_USAGE_WITHOUT_DECLARATION,
		ERR_SYMBOL_ALLOC,
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
