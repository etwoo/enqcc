#include "passes/parse/alloc.h"

#include "passes/parse.h"

#include <assert.h>
#include <string.h> /* for memset() */

result_t
parse_alloc(Arena *arena, struct ast **dst, unsigned nt)
{
	assert(dst != NULL && *dst == NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	(**dst).node_type = nt;
	return RESULT_OK;
}
