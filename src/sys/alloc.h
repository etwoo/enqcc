#ifndef ALLOC_H
#define ALLOC_H

#include "arena.h"

void *zalloc(Arena *arena, size_t sz);
void *deepcopy(Arena *arena, const void *src, size_t sz);

#endif
