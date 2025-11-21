#include "passes/symbol.h"

#include "sys/debug.h"

#include <limits.h> /* for LLONG_MIN */
#include <string.h>
#include <sys/param.h> /* for MAX() */

enum {
	UNIQUE_NOT_NEEDED = -1,
};

result_t
symbols_prepend(Arena *arena,
                struct symbol **head,
                const struct string_view *name,
                enum symbol_type stype,
                enum symbol_linkage linkage,
                enum symbol_storage_class storage,
                long long int n_args)
{
	struct symbol *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_PARSE_ALLOC);
	memset(node, 0, sizeof(*node));

	node->name = *name;
	node->stype = stype;
	node->linkage = linkage;
	node->storage = storage;
	node->n_args = n_args;
	node->unique = linkage == LINKAGE_NONE ? 0 : UNIQUE_NOT_NEEDED;

	if (*head != NULL) {
		struct symbol *previous_unique = *head;
		while (node->unique == 0 && previous_unique != NULL) {
			if (previous_unique->unique != UNIQUE_NOT_NEEDED) {
				node->unique = previous_unique->unique + 1;
			}
		}

		node->level = (**head).level;
		if ((**head).level_delimiter) {
			node->level++;
		}

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
