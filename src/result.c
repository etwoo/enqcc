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
	}

	return s;
}

void
result_str_cleanup(char **s)
{
	free(*s);
}
