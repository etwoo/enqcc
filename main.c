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

int
main(int argc, char *argv[])
{
	enum {
		ACTION_USAGE_HELP,
		ACTION_USAGE_ERROR,
	} action = 0;

	int synonym = 0;
	struct option lo[] = {
		{"help", no_argument, &synonym, 'h'},
		{NULL, 0, NULL, 0},
	};

	int opt = 0;
	while ((opt = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
		switch (opt == 0 ? synonym : opt) {
		case 'h':
			action = MAX(action, ACTION_USAGE_HELP);
			break;
		default:
			action = MAX(action, ACTION_USAGE_ERROR);
			break;
		}
	}

	int rc = EX_USAGE;  /* assume invalid arguments by default */
	FILE *out = stderr; /* assume output to stderr by default */

	switch (action) {
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
