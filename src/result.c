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
	case ERR_LEX_OPEN_SOURCE_FILE:
		s = my_asprintf("Error opening %s: %s", r.msg, my_strerror(r));
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
	case ERR_PARSE_CONSTANT_EXPECT_TOKEN_CONSTANT:
		s = strdup("Parsing constant expr expects TOKEN_CONSTANT");
		break;
	case ERR_PARSE_FUNC_EXPECT_RETURN_TYPE_INT:
		s = strdup(
			"Parsing function expects TOKEN_KEYWORD_INT in return "
			"type position");
		break;
	case ERR_PARSE_FUNC_NAME_EXPECT_TOKEN_IDENTIFIER:
		s = strdup(
			"Parsing function expects TOKEN_IDENTIFIER in function "
			"name position");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_OPEN:
		s = strdup("Parsing function expects TOKEN_PAREN_OPEN before "
		           "argument list");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_KEYWORD_VOID:
		s = strdup("Parsing function expects TOKEN_PAREN_VOID as "
		           "argument list");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_PAREN_CLOSE:
		s = strdup("Parsing function expects TOKEN_PAREN_CLOSE after "
		           "argument list");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_OPEN:
		s = strdup("Parsing function expects TOKEN_BRACE_OPEN before "
		           "function body statement(s)");
		break;
	case ERR_PARSE_FUNC_EXPECT_TOKEN_BRACE_CLOSE:
		s = strdup("Parsing function expects TOKEN_BRACE_CLOSE after "
		           "function body statement(s)");
		break;
	case ERR_PARSE_PROG_EXPECT_END:
		s = strdup("Parsing program expects end of token stream after "
		           "function definition(s)");
		break;
	case ERR_PARSE_STMT_EXPECT_TOKEN_KEYWORD_RETURN:
		s = strdup("Parsing statement expects TOKEN_KEYWORD_RETURN "
		           "before expression");
		break;
	case ERR_PARSE_STMT_EXPECT_TOKEN_SEMICOLON:
		s = strdup("Parsing statement expects TOKEN_SEMICOLON after "
		           "expression");
		break;
	case ERR_TMPFILE:
		s = my_asprintf("Error in tmpfile(): %s", my_strerror(r));
		break;
	case ERR_TMPFILE_FILENO:
		s = my_asprintf("Error fileno()-ing tmpfile: %s",
		                my_strerror(r));
		break;
	case ERR_TMPFILE_DUP:
		s = my_asprintf("Error dup()-ing tmpfile: %s", my_strerror(r));
		break;
	case ERR_TMPFILE_FSTAT:
		s = my_asprintf("Error fstat()-ing tmpfile: %s",
		                my_strerror(r));
		break;
	case ERR_TMPFILE_MMAP:
		s = my_asprintf("Error mmap()-ing tmpfile: %s", my_strerror(r));
		break;
	case ERR_TMPFILE_LSEEK:
		s = my_asprintf("Error seeking in tmpfile: %s", my_strerror(r));
		break;
	case ERR_TMPFILE_FTRUNCATE:
		s = my_asprintf("Error truncating tmpfile: %s", my_strerror(r));
		break;
	}

	return s;
}

void
result_str_cleanup(char **s)
{
	free(*s);
}
