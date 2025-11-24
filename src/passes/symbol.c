#include "passes/symbol.h"

#include "sys/debug.h"

#include <string.h>
#include <sys/param.h> /* for MAX() */

result_t
symbols_prepend(Arena *arena,
                struct symbol **head,
                const struct string_view *name,
                enum symbol_type stype,
                long long int n_args)
{
	struct symbol *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_PARSE_ALLOC);
	memset(node, 0, sizeof(*node));
	node->name = *name;
	node->stype = stype;
	node->n_args = n_args;
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
