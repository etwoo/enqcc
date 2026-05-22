#include "passes/parse/alloc.h"

#include "passes/parse.h"
#include "sys/alloc.h"

#include <assert.h>

result_t
parse_alloc(Arena *arena, struct ast **dst, unsigned nt)
{
	assert(dst != NULL && *dst == NULL);
	*dst = zalloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	(**dst).node_type = nt;
	return RESULT_OK;
}

result_t
parse_alloc_null_expr(Arena *arena, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_EXPRESSION_NULL));
	assert(*dst != NULL);
	(**dst).expr_type.t = CTYPE_VOID;
	return RESULT_OK;
}

result_t
flat_alloc(Arena *arena, struct flat **dst)
{
	assert(dst != NULL && *dst == NULL);
	*dst = zalloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_PARSE_ALLOC);
	return RESULT_OK;
}
