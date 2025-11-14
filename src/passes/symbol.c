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
	node->name = *name;
	if (*head == NULL) {
		node->unique = 0;
		node->level = 0;
	} else {
		node->unique = (**head).unique + 1;
		node->level = (**head).level;
		if ((**head).level_delimiter) {
			node->level++;
		}
	}
	node->level_delimiter = false;
	node->next = *head;
	*head = node;
	return RESULT_OK;
}

const struct symbol *
symbols_get(const struct symbol *head,
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
