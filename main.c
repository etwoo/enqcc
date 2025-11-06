#include "passes.h"
#include "result.h"

#include <errno.h>
#include <getopt.h> /* for getopt_long() */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h> /* for MAX() */
#include <sysexits.h>
#include <unistd.h> /* for close() */

static __attribute__((format(printf, 1, 2))) void
to_stderr(const char *pattern, ...)
{
	va_list ap = {0};
	va_start(ap, pattern);
	fprintf(stderr, "ERROR: ");
	vfprintf(stderr, pattern, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static __attribute__((warn_unused_result)) int
result_to_status(result_t r)
{
	auto_result owner = r;
	if (owner.err) {
		auto_result_str str = result_to_str(owner);
		to_stderr("%s", str ? str : "[result_to_str() -> NULL]");
		return EX_SOFTWARE;
	}
	return EX_OK;
}

typedef enum {
	ACTION_ALL_PASSES,
	ACTION_LEX,
	ACTION_LEX_PARSE,
	ACTION_LEX_PARSE_ASM,
	ACTION_USAGE_HELP,
	ACTION_USAGE_ERROR,
} compiler_action;

static __attribute__((warn_unused_result)) result_t
compile(const char *src, compiler_action action)
{
	struct token *tok __attribute__((cleanup(lex_cleanup))) = NULL;
	check(lex_init(src, &tok));
	lex_debug_print(tok);

	if (action != ACTION_ALL_PASSES && action < ACTION_LEX_PARSE) {
		return RESULT_OK;
	}

	struct ast *a __attribute__((cleanup(parse_cleanup))) = NULL;
	check(parse_init(tok, &a));
	parse_debug_print(a, 0);

	if (action != ACTION_ALL_PASSES && action < ACTION_LEX_PARSE_ASM) {
		return RESULT_OK;
	}

	struct assembly *cg __attribute__((cleanup(codegen_cleanup))) = NULL;
	check(codegen_init(a, &cg));
	codegen_debug_print(cg);

	if (action != ACTION_ALL_PASSES) {
		return RESULT_OK;
	}

	// emit code to disk

	return RESULT_OK;
}

int
main(int argc, char *argv[])
{
	compiler_action action = ACTION_ALL_PASSES;

	int synonym = 0;
	struct option lo[] = {
		{"codegen", no_argument, &synonym, 'c'},
		{"help", no_argument, &synonym, 'h'},
		{"lex", no_argument, &synonym, 'l'},
		{"parse", no_argument, &synonym, 'p'},
		{NULL, 0, NULL, 0},
	};

	int opt = 0;
	while ((opt = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
		switch (opt == 0 ? synonym : opt) {
		case 'c':
			action = MAX(action, ACTION_LEX_PARSE_ASM);
			break;
		case 'h':
			action = MAX(action, ACTION_USAGE_HELP);
			break;
		case 'l':
			action = MAX(action, ACTION_LEX);
			break;
		case 'p':
			action = MAX(action, ACTION_LEX_PARSE);
			break;
		default:
			action = MAX(action, ACTION_USAGE_ERROR);
			break;
		}
	}

	int rc = EX_USAGE;  /* assume invalid arguments by default */
	FILE *out = stderr; /* assume output to stderr by default */

	switch (action) {
	case ACTION_ALL_PASSES:
	case ACTION_LEX:
	case ACTION_LEX_PARSE:
	case ACTION_LEX_PARSE_ASM:
		if (optind >= argc) {
			to_stderr("Missing input file argument");
		} else {
			rc = result_to_status(compile(argv[optind], action));
		}
		break;
	case ACTION_USAGE_HELP:
		rc = EX_OK;
		out = stdout;
		__attribute__((fallthrough));
	case ACTION_USAGE_ERROR:
		fprintf(out, "Usage: %s [options] <url>\nOptions:\n", argv[0]);
		for (struct option *o = lo; o->name; ++o) {
			fprintf(out, "  -%c, --%s\n", o->val, o->name);
		}
		break;
	}

	return rc;
}
