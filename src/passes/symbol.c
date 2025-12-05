#include "passes/symbol.h"

#include "sys/array.h"
#include "sys/debug.h"

#include <string.h>
#include <sys/param.h> /* for MAX() */

static const char *const CTYPE_AS_STR[] = {
	"INT",
	"LONG",
	"UNSIGNED INT",
	"UNSIGNED LONG",
};

const char *
ctype_to_str(enum ctype c)
{
	assert(c < ARRAY_SIZE(CTYPE_AS_STR));
	return CTYPE_AS_STR[c];
}

long long int
ctype_to_size_bytes(enum ctype c)
{
	long long int b = 0;
	switch (c) {
	case CTYPE_INT:
	case CTYPE_LONG:
		b = 4;
		break;
	case CTYPE_UNSIGNED_INT:
	case CTYPE_UNSIGNED_LONG:
		b = 8;
		break;
	}
	return b;
}

bool
ctype_is_signed(enum ctype c)
{
	bool b = true;
	switch (c) {
	case CTYPE_INT:
	case CTYPE_LONG:
		b = true;
		break;
	case CTYPE_UNSIGNED_INT:
	case CTYPE_UNSIGNED_LONG:
		b = false;
		break;
	}
	return b;
}

enum ctype
get_common_ctype(enum ctype lhs, enum ctype rhs)
{
	return MAX(lhs, rhs);
}

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

result_t
symbols_prepend(Arena *arena,
                struct symbol **head,
                const struct string_view *name,
                enum symbol_type stype,
                enum ctype c89type)
{
	struct symbol *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_SYMBOL_ALLOC);
	memset(node, 0, sizeof(*node));
	node->name = *name;
	node->stype = stype;
	node->c89type = c89type;
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
	char *mangled_str = arena_sprintf(arena,
	                                  "%.*s%c%lld",
	                                  (int)s->name.sz,
	                                  s->name.data,
	                                  MANGLE_DELIMITER,
	                                  s->unique);
	check_if(mangled_str == NULL, ERR_SYMBOL_ALLOC);
	s->name.data = mangled_str;
	s->name.sz = strlen(mangled_str);
	return RESULT_OK;
}
