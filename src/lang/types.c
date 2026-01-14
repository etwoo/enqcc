#include "lang/types.h"

#include "sys/array.h"

#include <assert.h>
#include <stdio.h>     /* for snprintf() */
#include <string.h>    /* for memset */
#include <sys/param.h> /* for MIN() and MAX() */

const struct ctype LIKE_PTRDIFF_T = {.t = CTYPE_LONG};
const struct ctype LIKE_SIZE_T = {.t = CTYPE_UNSIGNED_LONG};

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
ctype_alloc_str_literal(Arena *arena,
                        const struct string_view *src,
                        struct ctype *dst)
{
	assert(dst != NULL);
	dst->t = CTYPE_ARRAY_OF;
	dst->sz = src->sz;
	assert(dst->referent == NULL);
	check(ctype_alloc(arena, &dst->referent));
	dst->referent->t = CTYPE_CHAR;
	return RESULT_OK;
}

result_t
ctype_copy(Arena *arena, const struct ctype *src, struct ctype *dst)
{
	assert(src != NULL);
	assert(dst != NULL);
	dst->t = src->t;
	dst->maybe_null_pointer_constant = src->maybe_null_pointer_constant;
	dst->sz = src->sz;
	dst->tag_name = src->tag_name;
	dst->tag_unique = src->tag_unique;

	dst->referent = NULL;
	if (src->referent != NULL) {
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

	if ((ctype_is_pointer(c) || ctype_is_struct(c)) && cap > copied + 1) {
		stor[copied++] = ' ';
		if (ctype_is_array(c) || ctype_is_struct(c)) {
			size_t remaining = cap - copied;
			size_t num = ctype_is_array(c) ? c->sz
			                               : (size_t)c->tag_unique;
			int required =
				snprintf(stor + copied, remaining, "%zu ", num);
			if (required < 0 || (size_t)required + 1 > remaining) {
				/* snprintf() indicates insuffient space */
				return stor;
			}
			copied += required;
		}
		if (c->referent != NULL) {
			ctype_to_str(c->referent, stor + copied, cap - copied);
		} /* else: tolerate pointer types pending referent */
		if (ctype_is_struct(c)) {
			size_t remaining = cap - copied;
			if (c->tag_name.sz + 1 > remaining) {
				return stor;
			}
			memcpy(stor + copied, c->tag_name.data, c->tag_name.sz);
			copied += c->tag_name.sz;
			stor[copied++] = '\0';
		}
	}

	return stor;
}

long long int
ctype_to_size_bytes_with_types(const struct ctype *c, struct type_table *t)
{
	long long int b = 0;
	struct type_table *type_entry = NULL;

	switch (c->t) {
	case CTYPE_VOID:
		b = 0;
		break;
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_UNSIGNED_CHAR:
		b = 1;
		break;
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
		b = (long long int)c->sz;
		b *= ctype_to_size_bytes_with_types(c->referent, t);
		break;
	case CTYPE_STRUCT:
		assert(t && "struct size lookup requires type table");
		type_entry = types_find(t, c);
		assert(type_entry != NULL); /* caller ensures complete type */
		assert(type_entry->n_members > 0);
		b = type_entry->aggregate_size;
		break;
	}

	return b;
}

long long int
ctype_to_size_bytes(const struct ctype *c)
{
	return ctype_to_size_bytes_with_types(c, NULL);
}

long long int
ctype_to_alignment(const struct ctype *c, struct type_table *t)
{
	long long int align = 0;
	if (ctype_is_array(c)) {
		align = ctype_to_size_bytes_with_types(c->referent, t);
	} else {
		align = ctype_to_size_bytes_with_types(c, t);
	}
	return align;
}

bool
ctype_is_integer(const struct ctype *c)
{
	bool b = true;
	switch (c->t) {
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_UNSIGNED_CHAR:
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
		b = true;
		break;
	case CTYPE_VOID:
	case CTYPE_DOUBLE:
	case CTYPE_POINTER_TO:
	case CTYPE_ARRAY_OF:
	case CTYPE_STRUCT:
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
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_INT:
	case CTYPE_LONG:
	case CTYPE_DOUBLE:
		b = true;
		break;
	case CTYPE_VOID:
	case CTYPE_UNSIGNED_CHAR:
	case CTYPE_UNSIGNED_INT:
	case CTYPE_UNSIGNED_LONG:
	case CTYPE_POINTER_TO:
	case CTYPE_ARRAY_OF:
	case CTYPE_STRUCT:
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
ctype_is_charlike(const struct ctype *c)
{
	switch (c->t) {
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_UNSIGNED_CHAR:
		return true;
	default:
		break;
	}
	return false;
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
ctype_is_strlike_array(const struct ctype *c)
{
	/* consider array of any character type as str-like */
	return c->t == CTYPE_ARRAY_OF && ctype_is_charlike(c->referent);
}

bool
ctype_is_strlike_ptr(const struct ctype *c)
{
	/* consider pointer to char as str-like; exclude {,un}signed char */
	return c->t == CTYPE_POINTER_TO && c->referent->t == CTYPE_CHAR;
}

bool
ctype_is_void(const struct ctype *c)
{
	return c->t == CTYPE_VOID;
}

bool
ctype_is_void_ptr(const struct ctype *c)
{
	return c->t == CTYPE_POINTER_TO && ctype_is_void(c->referent);
}

bool
ctype_is_struct(const struct ctype *c)
{
	return c->t == CTYPE_STRUCT;
}

bool
ctype_is_aggregate(const struct ctype *c)
{
	return ctype_is_array(c) || ctype_is_struct(c);
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

static WARN_UNUSED bool
ctype_is_equal_impl(const struct ctype *lhs,
                    const struct ctype *rhs,
                    bool array_to_pointer_decay)
{
	if (ctype_is_array(lhs) && ctype_is_array(rhs) && lhs->sz != rhs->sz) {
		return false;
	}

	if (((ctype_is_array(lhs) && ctype_is_pointer(rhs)) ||
	     (ctype_is_pointer(lhs) && ctype_is_array(rhs))) &&
	    array_to_pointer_decay) {
		return ctype_is_equal(lhs->referent, rhs->referent);
	}

	if (lhs->t != rhs->t) {
		return false;
	}

	if (ctype_is_struct(lhs) && lhs->tag_unique != rhs->tag_unique) {
		return false;
	}

	if ((lhs->referent == NULL) != (rhs->referent == NULL)) {
		return false;
	}

	return (lhs->referent == NULL && rhs->referent == NULL) ||
	       /* array_to_pointer_decay==false for referent(s) */
	       ctype_is_equal_impl(lhs->referent, rhs->referent, false);
}

bool
ctype_is_equal(const struct ctype *lhs, const struct ctype *rhs)
{
	return ctype_is_equal_impl(lhs, rhs, true);
}

bool
ctype_is_struct_mismatch(const struct ctype *lhs, const struct ctype *rhs)
{
	return (ctype_is_struct(lhs) != ctype_is_struct(rhs)) ||
	       (ctype_is_struct(lhs) && !ctype_is_equal(lhs, rhs));
}

void
ctype_array_decay_to_pointer(struct ctype *c)
{
	if (ctype_is_array(c)) {
		c->t = CTYPE_POINTER_TO;
		c->maybe_null_pointer_constant = false;
		c->sz = 0;
		/* leave referent as-is */
	}
	assert(!ctype_is_array(c));
}

result_t
types_prepend(Arena *arena, struct type_table **head, struct ctype *new_type)
{
	assert(ctype_is_struct(new_type));
	assert(new_type->tag_unique == 0);

	struct type_table *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_CTYPE_ALLOC);
	memset(node, 0, sizeof(*node));
	assert(node->n_members == 0);  /* struct def not yet complete */
	assert(node->members == NULL); /* struct def not yet complete */

	check(ctype_copy(arena, new_type, &node->c));
	if (*head == NULL) {
		node->c.tag_unique = 8192; /* avoid zero as struct tag ID */
	} else {
		node->c.tag_unique = (**head).c.tag_unique + 1;
	}

	/* unique-ify argument copy of new struct ctype, as well */
	new_type->tag_unique = node->c.tag_unique;
	assert(ctype_is_equal(new_type, &node->c));

	node->next = *head;
	*head = node;
	return RESULT_OK;
}

struct type_table *
types_find(struct type_table *head, const struct ctype *needle)
{
	while (head != NULL) {
		if (ctype_is_equal(needle, &head->c)) {
			return head;
		}
		head = head->next;
	}
	return NULL;
}

bool
ctype_is_incomplete(const struct ctype *c, struct type_table *t)
{
	struct type_table *entry = NULL;

	bool result = false;
	switch (c->t) {
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_UNSIGNED_CHAR:
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
	case CTYPE_DOUBLE:
	case CTYPE_POINTER_TO:
		break;
	case CTYPE_ARRAY_OF:
		result = ctype_is_incomplete(c->referent, t);
		break;
	case CTYPE_STRUCT:
		entry = types_find(t, c);
		result = (entry == NULL || entry->n_members == 0);
		break;
	case CTYPE_VOID:
		result = true;
		break;
	}

	return result;
}

bool
ctype_is_ptr_to_incomplete(const struct ctype *c, struct type_table *t)
{
	return c->t == CTYPE_POINTER_TO && ctype_is_incomplete(c->referent, t);
}

struct ctype *
ctype_of_member(struct type_table *type_entry,
                const struct string_view *member_name)
{
	for (long long unsigned i = 0; i < type_entry->n_members; ++i) {
		const struct string_view *candidate =
			&type_entry->members[i].member_name;
		if (member_name->sz == candidate->sz &&
		    0 == strncmp(member_name->data,
		                 candidate->data,
		                 candidate->sz)) {
			return &type_entry->members[i].member_type;
		}
	}
	return NULL;
}

long long int
round_up_to_multiple_of(long long int n, long long int base)
{
	const long long int rounded = (((n + base - 1) / base)) * base;
	assert(rounded >= n);
	assert(rounded == 0 || rounded - n < base);
	assert(rounded % base == 0);
	return rounded;
}
