#include "lang/symbol.h"

#include "sys/array.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h> /* for MAX() */

bool
is_internal(enum symbol_linkage linkage)
{
	return linkage == SYMBOL_LINKAGE_INTERNAL;
}

bool
is_external(enum symbol_linkage linkage)
{
	return linkage == SYMBOL_LINKAGE_EXTERNAL;
}

bool
some_linkage(enum symbol_linkage linkage)
{
	return is_external(linkage) || is_internal(linkage);
}

long long unsigned
get_double_as_quadword(double value)
{
	long long unsigned as_quadword = 0;
	static_assert(sizeof(value) <= sizeof(as_quadword),
	              "destination must be large enough to hold 64-bit double");
	memcpy(&as_quadword, &value, sizeof(value));
	return as_quadword;
}

static WARN_UNUSED long long unsigned
get_initializer_element_count(const struct ctype *c)
{
	if (ctype_is_array(c)) {
		return c->sz * get_initializer_element_count(c->referent);
	}
	return 1;
}

static WARN_UNUSED long long unsigned
get_initializer_element_size_bytes(const struct ctype *c)
{
	if (ctype_is_array(c)) {
		return get_initializer_element_size_bytes(c->referent);
	}
	return ctype_to_size_bytes(c);
}

result_t
constant_set_zero(Arena *arena,
                  const struct ctype *c89type,
                  struct constant_initializer *ci)
{
	ci->count = get_initializer_element_count(c89type);
	ci->elements = arena_alloc(arena, ci->count * sizeof(*ci->elements));
	check_if(ci->elements == NULL, ERR_SEMA_ALLOC);
	memset(ci->elements, 0, ci->count * sizeof(*ci->elements));

	const long long unsigned element_size_bytes =
		get_initializer_element_size_bytes(c89type);

	for (long long unsigned i = 0; i < ci->count; ++i) {
		ci->elements[i].byte_count = element_size_bytes;
		ci->elements[i].byte_value = 0;
	}
	return RESULT_OK;
}

result_t
constant_make_zero(Arena *arena,
                   const struct ctype *c89type,
                   struct constant_initializer **dst)
{
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_SYMBOL_ALLOC);
	check(constant_set_zero(arena, c89type, *dst));
	return RESULT_OK;
}

bool
constant_is_zero(const struct constant_initializer *ci)
{
	assert(ci->count > 0);
	for (long long unsigned i = 0; i < ci->count; ++i) {
		if (ci->elements[i].byte_value != 0 ||
		    ci->elements[i].unique != 0) {
			return false;
		}
	}
	return true;
}

long long unsigned
constant_byte_count(const struct constant_initializer *ci)
{
	long long unsigned byte_count = 0;
	for (long long unsigned i = 0; i < ci->count; ++i) {
		byte_count += ci->elements[i].byte_count;
	}
	return byte_count;
}

void
constant_debug_print(const struct constant_initializer *ci, size_t indent)
{
	for (long long unsigned i = 0; i < ci->count; ++i) {
		if (i > 32) {
			debug("%*s(skipping next %llu elements ...)",
			      (int)indent,
			      "",
			      ci->count - i);
			break;
		}
		debug("%*sSIZE:  %llu",
		      (int)indent,
		      "",
		      ci->elements[i].byte_count);
		debug("%*sVALUE: 0x%llx",
		      (int)indent,
		      "",
		      ci->elements[i].byte_value);
		if (ci->elements[i].unique > 0) {
			debug("%*sREFERENCE TO STRING: str.%lld",
			      (int)indent,
			      "",
			      ci->elements[i].unique);
		}
	}
}

result_t
symbols_prepend(Arena *arena,
                struct symbol **head,
                const struct string_view *name,
                enum symbol_type stype,
                struct ctype *c89type)
{
	struct symbol *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_SYMBOL_ALLOC);
	memset(node, 0, sizeof(*node));
	node->name = *name;
	node->stype = stype;
	check(ctype_copy(arena, c89type, &node->c89type));
	if (*head != NULL) {
		long long int base = MAX((**head).unique, (**head).cookie);
		node->unique = base + 1;
		node->cookie = MAX(node->unique, (**head).cookie);
	}
	node->level_delimiter = false;
	node->next = *head;
	*head = node;
	return RESULT_OK;
}

static WARN_UNUSED struct symbol *
symbols_get_impl(struct symbol *head,
                 const struct string_view *name,
                 bool stop_at_delimiter)
{
	while (head != NULL) {
		if (stop_at_delimiter && head->level_delimiter) {
			break;
		}
		if (head->name.sz == name->sz &&
		    0 == strncmp(head->name.data, name->data, name->sz)) {
			return head;
		}
		head = head->next;
	}
	return NULL;
}

struct symbol *
symbols_get_limited(struct symbol *head, const struct string_view *name)
{
	return symbols_get_impl(head, name, true);
}

struct symbol *
symbols_get_anywhere(struct symbol *head, const struct string_view *name)
{
	return symbols_get_impl(head, name, false);
}

struct symbol *
symbols_get_unique(struct symbol *head, long long int unique)
{
	while (head != NULL) {
		if (head->unique == unique) {
			return head;
		}
		head = head->next;
	}
	return NULL;
}

void
symbols_reset_scope(struct symbol **symbols, struct symbol *reset_point)
{
	if (reset_point != NULL) {
		reset_point->cookie = (**symbols).cookie;
	}
	*symbols = reset_point;
}

static const char MANGLE_DELIMITER = '.';

bool
is_mangled(struct symbol *s)
{
	return (memchr(s->name.data, MANGLE_DELIMITER, s->name.sz) != NULL);
}

result_t
mangle_name(Arena *arena, struct symbol *s)
{
	char *mangled_str = NULL;
	int rc = asprintf(&mangled_str,
	                  "%.*s%c%lld",
	                  (int)s->name.sz,
	                  s->name.data,
	                  MANGLE_DELIMITER,
	                  s->unique);
	check_if(rc < 0, ERR_SYMBOL_ALLOC);

	s->name.sz = strlen(mangled_str);

	char *arena_copy = arena_alloc(arena, strlen(mangled_str));
	if (arena_copy == NULL) {
		free(mangled_str);
		return make_result(ERR_SYMBOL_ALLOC);
	}

	memcpy(arena_copy, mangled_str, s->name.sz); /* exclude NUL */
	s->name.data = arena_copy;

	free(mangled_str);
	return RESULT_OK;
}
