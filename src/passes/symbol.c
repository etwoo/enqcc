#include "passes/symbol.h"

#include "sys/debug.h"

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

result_t
symbols_prepend(Arena *arena,
                struct symbol **head,
                const struct string_view *name,
                enum symbol_type stype)
{
	struct symbol *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_PARSE_ALLOC);
	memset(node, 0, sizeof(*node));
	node->name = *name;
	node->stype = stype;
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

struct symbol *
symbols_get(struct symbol *head,
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

void
symbols_reset_scope(struct symbol **symbols, struct symbol *reset_point)
{
	if (reset_point != NULL) {
		reset_point->cookie = (**symbols).cookie;
	}
	*symbols = reset_point;
}

struct symbol *
symbols_get_scoped(struct symbol *head,
                   const struct string_view *name,
                   enum symbol_scope scope)
{
	while (head != NULL) {
		if (head->name.sz == name->sz &&
		    0 == strncmp(head->name.data, name->data, name->sz) &&
		    head->scope_if_specified == scope) {
			return head;
		}
		head = head->next;
	}
	return NULL;
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

static const char MANGLE_DELIMTER = '.';

bool
is_mangled(struct symbol *s)
{
	return (memchr(s->name.data, MANGLE_DELIMTER, s->name.sz) != NULL);
}

result_t
mangle_name(Arena *arena, struct symbol *s)
{
	char *mangled_str = arena_sprintf(arena,
	                                  "%.*s%c%lld",
	                                  (int)s->name.sz,
	                                  s->name.data,
					  MANGLE_DELIMTER,
	                                  s->unique);
	check_if(mangled_str == NULL, ERR_SEMA_ALLOC);
	s->name.data = mangled_str;
	s->name.sz = strlen(mangled_str);
	return RESULT_OK;
}
