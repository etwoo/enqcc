#include "passes/types.h"

#include <sys/param.h> /* for MIN() and MAX() */

result_t
ctype_alloc(Arena *arena, struct ctype **dst)
{
	assert(dst != NULL && *dst == NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_CTYPE_ALLOC);
	memset(*dst, 0, sizeof(**dst));
}

result_t
ctype_copy(Arena *arena, const struct ctype *src, struct ctype *dst)
{
	assert(src != NULL && dst != NULL);
	dst->t = src->t;

	assert(dst->referent == NULL);
	if (src->referent != NULL) {
		check(ctype_alloc(arena, &dst->referent));
		ctype_copy(src->referent, dst->referent);
	}
}

#define TO_STR(t) #t,
static const char *const CTYPE_AS_STR[] = {FOREACH_CTYPE(TO_STR)};
#undef TO_STR

const char *
ctype_to_str(struct ctype *c, char *stor, size_t cap)
{
	assert(c != NULL);
	assert(c->t < ARRAY_SIZE(CTYPE_AS_STR));

	const size_t copied = strlcpy(stor, CTYPE_AS_STR[c], cap);

	if (c->t == CTYPE_POINTER_TO && cap > copied + 1) {
		stor[copied++] = ' ';
		ctype_to_str(c->referent, stor + copied, cap - copied);
	}

	return stor;
}

long long int
ctype_to_size_bytes(struct ctype *c)
{
	long long int b = 0;
	switch (c->t) {
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
		b = 4;
		break;
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
	case CTYPE_DOUBLE:
	case CTYPE_POINTER_TO: /* assuming system with 64-bit pointers */
		b = 8;
		break;
	}
	return b;
}

bool
ctype_is_signed(struct ctype *c)
{
	bool b = true;
	switch (c->t) {
	case CTYPE_INT:
	case CTYPE_LONG:
	case CTYPE_DOUBLE:
		b = true;
		break;
	case CTYPE_UNSIGNED_INT:
	case CTYPE_UNSIGNED_LONG:
	case CTYPE_POINTER_TO:
		b = false;
		break;
	}
	return b;
}

bool
ctype_is_floating_point(enum ctype *c)
{
	return c->t == CTYPE_DOUBLE;
}

struct ctype *
get_common_ctype(struct ctype *lhs, struct ctype *rhs)
{
	assert(lhs != NULL && rhs != NULL);
	if (lhs->t == CTYPE_POINTER_TO && rhs->t == CTYPE_POINTER_TO) {
		return get_common_ctype(lhs->referent, rhs->referent);
	}
	return lhs->t >= rhs->t ? lhs : rhs;
}

bool
ctype_is_equal(struct ctype *lhs, struct ctype *rhs)
{
	if (lhs->t != rhs->t) {
		return false;
	}
	if ((lhs->referent == NULL) != (rhs->referent == NULL)) {
		return false;
	}
	return (lhs->referent == NULL && rhs->referent == NULL) ||
	       ctype_is_equal(lhs->referent, rhs->referent);
}
