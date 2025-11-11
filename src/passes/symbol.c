#include "passes/symbol.h"

#include "sys/debug.h"

#include <string.h>

result_t
symbols_prepend(Arena *arena,
                struct symbol **head,
                const struct string_view *name)
{
	struct symbol *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_PARSE_ALLOC);
	memset(node, 0, sizeof(*node));
	node->next = *head;
	node->name = *name;
	node->unique = *head == NULL ? 0 : (**head).unique + 1;
	*head = node;
	return RESULT_OK;
}

const struct symbol *
symbols_get(const struct symbol *head, const struct string_view *name)
{
	while (head != NULL) {
		if (head->name.sz == name->sz &&
		    0 == strncmp(head->name.data, name->data, name->sz)) {
			return head;
		}
		head = head->next;
	}
	return NULL;
}
