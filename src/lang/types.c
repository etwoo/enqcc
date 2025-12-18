#include "lang/types.h"

#include "sys/array.h"

#include <assert.h>
#include <stdio.h>     /* for snprintf() */
#include <string.h>    /* for memset */
#include <sys/param.h> /* for MIN() and MAX() */

result_t
ctype_alloc(Arena *arena, struct ctype **dst)
{
	assert(dst != NULL && *dst == NULL);
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_CTYPE_ALLOC);
	memset(*dst, 0, sizeof(**dst));
	return RESULT_OK;
}

result_t
ctype_copy(Arena *arena, const struct ctype *src, struct ctype *dst)
{
	assert(src != NULL && dst != NULL);
	dst->t = src->t;
	dst->maybe_null_pointer_constant = src->maybe_null_pointer_constant;
	dst->sz = src->sz;

	if (src->referent != NULL) {
		dst->referent = NULL;
		check(ctype_alloc(arena, &dst->referent));
		check(ctype_copy(arena, src->referent, dst->referent));
	}

	return RESULT_OK;
}

#define TO_STR(t) #t,
static const char *const CTYPE_AS_STR[] = {FOREACH_CTYPE(TO_STR)};
#undef TO_STR

const char *
ctype_to_str(const struct ctype *c, char *stor, size_t cap)
{
	assert(c != NULL);
	assert(c->t < ARRAY_SIZE(CTYPE_AS_STR));

	size_t copied = strlcpy(stor, CTYPE_AS_STR[c->t], cap);

	if ((c->t == CTYPE_POINTER_TO || c->t == CTYPE_ARRAY_OF) &&
	    cap > copied + 1) {
		stor[copied++] = ' ';
		if (c->t == CTYPE_ARRAY_OF) {
			size_t remaining = cap - copied;
			size_t required = snprintf(stor + copied,
			                           remaining,
			                           "%llu ",
			                           c->sz);
			if (required + 1 > remaining || required < 0) {
				/* snprintf() indicates insuffient space */
				return stor;
			}
			copied += required;
		}
		if (c->referent != NULL) {
			ctype_to_str(c->referent, stor + copied, cap - copied);
		} /* else: tolerate incomplete types */
	}

	return stor;
}

long long int
ctype_to_size_bytes(const struct ctype *c)
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
	case CTYPE_ARRAY_OF:
		assert(c->sz > 0 && c->sz < LLONG_MAX);
		b = (long long int)c->sz * ctype_to_size_bytes(c->referent);
		break;
	}
	return b;
}

bool
ctype_is_integer(const struct ctype *c)
{
	bool b = true;
	switch (c->t) {
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
		b = true;
		break;
	case CTYPE_DOUBLE:
	case CTYPE_POINTER_TO:
	case CTYPE_ARRAY_OF:
		b = false;
		break;
	}
	return b;
}

bool
ctype_is_signed(const struct ctype *c)
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
	case CTYPE_ARRAY_OF:
		b = false;
		break;
	}
	return b;
}

bool
ctype_is_floating_point(const struct ctype *c)
{
	return c->t == CTYPE_DOUBLE;
}

bool
ctype_is_pointer(const struct ctype *c)
{
	return c->t == CTYPE_POINTER_TO || ctype_is_array(c);
}

bool
ctype_is_array(const struct ctype *c)
{
	return c->t == CTYPE_ARRAY_OF;
}

bool
ctype_nullptr_ish(const struct ctype *c)
{
	return c->maybe_null_pointer_constant;
}

const struct ctype *
get_common_ctype(const struct ctype *lhs, const struct ctype *rhs)
{
	assert(lhs != NULL && rhs != NULL);
	if (lhs->t == CTYPE_POINTER_TO && rhs->t == CTYPE_POINTER_TO) {
		const struct ctype *inner =
			get_common_ctype(lhs->referent, rhs->referent);
		return inner == lhs->referent ? lhs : rhs;
		/* can be optimistic here; sema.c rejects pointer mismatches */
	}
	return lhs->t >= rhs->t ? lhs : rhs;
}

bool
ctype_is_equal(const struct ctype *lhs, const struct ctype *rhs)
{
	if (lhs->t != rhs->t) {
		return false;
	}
	if (lhs->t == CTYPE_ARRAY_OF && lhs->sz != rhs->sz) {
		return false;
	}
	if ((lhs->referent == NULL) != (rhs->referent == NULL)) {
		return false;
	}
	return (lhs->referent == NULL && rhs->referent == NULL) ||
	       ctype_is_equal(lhs->referent, rhs->referent);
}
