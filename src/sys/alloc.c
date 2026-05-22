#include "sys/alloc.h"

#include "arena.h"

#include <string.h> /* for memset(), memcpy() */

void *
zalloc(Arena *arena, size_t sz)
{
	void *dst = arena_alloc(arena, sz);
	if (dst != NULL) {
		memset(dst, 0, sz);
	}
	return dst;
}

void *
deepcopy(Arena *arena, const void *src, size_t sz)
{
	void *dst = arena_alloc(arena, sz);
	if (dst != NULL) {
		memcpy(dst, src, sz);
	}
	return dst;
}
