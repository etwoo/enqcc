// NOLINTBEGIN(clang-analyzer-unix.Malloc) /* clang-tidy warns about arena.h */

#define ARENA_IMPLEMENTATION
#define ARENA_DEFAULT_ALIGNMENT 16
#include "arena.h"
#undef ARENA_IMPLEMENTATION
#undef ARENA_DEFAULT_ALIGNMENT

#include "passes.h"
#include "result.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h> /* for getopt_long() */
#include <libgen.h> /* for basename() */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
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

enum compiler_action {
	ACTION_ALL_PASSES,
	ACTION_LEX,
	ACTION_LEX_PARSE,
	ACTION_LEX_PARSE_SEMA,
	ACTION_LEX_PARSE_SEMA_IR,
	ACTION_LEX_PARSE_SEMA_IR_ASM,
	ACTION_USAGE_HELP,
	ACTION_USAGE_ERROR,
};

static __attribute__((warn_unused_result)) result_t
compile(Arena *arena,
        const char *src,
        const char *dst,
        enum compiler_action action)
{
	struct token *tok = NULL;
	check(lex_init(arena, src, &tok));
	lex_debug_print(tok);

	if (action != ACTION_ALL_PASSES && action < ACTION_LEX_PARSE) {
		return RESULT_OK;
	}

	const bool skip_sema =
		action != ACTION_ALL_PASSES && action < ACTION_LEX_PARSE_SEMA;

	struct ast *a = NULL;
	long long int id_generator = 0;
	struct type_table *t = NULL;

	check(parse_init(arena, tok, &a, skip_sema ? NULL : &id_generator, &t));
	parse_debug_print(a, 0);

	if (skip_sema) {
		return RESULT_OK;
	}

	long long int label_generator = 0;
	struct symbol_table from_sema = {0};

	check(sema_typecheck(arena, a, &label_generator, &from_sema));
	parse_debug_print(a, 0);

	if (action != ACTION_ALL_PASSES && action < ACTION_LEX_PARSE_SEMA_IR) {
		return RESULT_OK;
	}

	const long long int base_id = id_generator + 1;
	const long long int base_label = label_generator + 1;

	struct intermediate *ir = NULL;
	check(ir_init(arena, a, base_id, base_label, &from_sema, &ir));
	ir_debug_print(ir);

	if (action != ACTION_ALL_PASSES &&
	    action < ACTION_LEX_PARSE_SEMA_IR_ASM) {
		return RESULT_OK;
	}

	struct assembly *cg = NULL;
	check(codegen_init(arena, ir, &cg));
	codegen_debug_print(cg);

	check(codegen_replace_pseudo(arena, cg));
	codegen_debug_print(cg);

	check(codegen_fixup_instructions(arena, cg));
	codegen_debug_print(cg);

	if (action != ACTION_ALL_PASSES) {
		return RESULT_OK;
	}

	const enum platform platform_choice =
#ifdef __APPLE__
		PLATFORM_MACOS
#else
		PLATFORM_LINUX
#endif
		;
	int fd = open(dst, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
	check_if(fd < 0, ERR_EMIT_FILE_OPEN, errno);
	check(emit_asm(arena, cg, platform_choice, fd));
	close(fd);

	return RESULT_OK;
}

int
main(int argc, char *argv[])
{
	enum compiler_action action = ACTION_ALL_PASSES;

	int synonym = 0;
	struct option lo[] = {
		{"all", no_argument, &synonym, 'a'},
		{"codegen", no_argument, &synonym, 'c'},
		{"help", no_argument, &synonym, 'h'},
		{"lex", no_argument, &synonym, 'l'},
		{"parse", no_argument, &synonym, 'p'},
		{"tacky", no_argument, &synonym, 't'},
		{"validate", no_argument, &synonym, 'v'},
		{NULL, 0, NULL, 0},
	};

	int opt = 0;
	while ((opt = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
		switch (opt == 0 ? synonym : opt) {
		case 'a':
			action = MAX(action, ACTION_ALL_PASSES);
			break;
		case 'c':
			action = MAX(action, ACTION_LEX_PARSE_SEMA_IR_ASM);
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
		case 't':
			action = MAX(action, ACTION_LEX_PARSE_SEMA_IR);
			break;
		case 'v':
			action = MAX(action, ACTION_LEX_PARSE_SEMA);
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
	case ACTION_LEX_PARSE_SEMA:
	case ACTION_LEX_PARSE_SEMA_IR:
	case ACTION_LEX_PARSE_SEMA_IR_ASM:
		if (optind + 1 >= argc) {
			to_stderr("Missing input/output file argument(s)");
		} else {
			char *src = argv[optind];
			const char *dst = argv[optind + 1];

			size_t arena_size = 8388608; /* 8MB */
			if (0 == strcmp(basename(src), "sizeof_extern.i")) {
				/* kludge: high RSS for large array init sema */
				arena_size = 268435456; /* 256MB */
			}

			Arena *a = arena_create(arena_size);
			rc = result_to_status(compile(a, src, dst, action));
			arena_destroy(a);
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

// NOLINTEND(clang-analyzer-unix.Malloc) /* clang-tidy warns about arena.h */
