#include "sys/debug.h"

#include <assert.h>
#include <math.h> /* for log10() */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>    /* for strlen() */
#include <sys/param.h> /* for MIN() */

enum {
	LOG_LVL_MAX_LEN = 5,
	FILENAME_LINENO_COLUMN_WIDTH = 9,
};

static __attribute__((format(printf, 4, 0))) void
vlog(const char *level,
     const char *fname,
     unsigned int lineno,
     const char *pattern,
     va_list ap)
{
	const int lineno_digits = 1 + (int)log10(lineno);
	assert(lineno_digits <= 4); /* source file contains <10k lines */

	char fname_prefix[FILENAME_LINENO_COLUMN_WIDTH] = {0};
	const size_t fname_capacity = sizeof(fname_prefix) - 1 - lineno_digits;

	const char *end = memchr(fname, '.', strlen(fname));
	assert(end != NULL); /* source filename ends with .c extension */
	memcpy(fname_prefix, fname, MIN((size_t)(end - fname), fname_capacity));
	const int width = (int)strlen(fname_prefix) + lineno_digits + 1;

	int padding = 0;
	if (width < FILENAME_LINENO_COLUMN_WIDTH) {
		padding = FILENAME_LINENO_COLUMN_WIDTH - width;
	}

	fprintf(stderr,
	        "%*s %s:%u%*s| ",
	        LOG_LVL_MAX_LEN,
	        level,
	        fname_prefix,
	        lineno,
	        padding,
	        "");
	vfprintf(stderr, pattern, ap);
	fputc('\n', stderr);
}

void
debug_at_line(const char *fname, unsigned int lineno, const char *pattern, ...)
{
#ifdef WITH_DEBUG_LOG
	va_list ap = {0};
	va_start(ap, pattern);
	vlog("DEBUG", fname, lineno, pattern, ap);
	va_end(ap);
#else
	(void)fname;
	(void)lineno;
	(void)pattern;
#endif
}

void
info_at_line(const char *fname, unsigned int lineno, const char *pattern, ...)
{
	va_list ap = {0};
	va_start(ap, pattern);
	vlog("INFO", fname, lineno, pattern, ap);
	va_end(ap);
}
