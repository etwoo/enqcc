#ifndef COMPILER_PASSES_PARSE_ALLOC_H
#define COMPILER_PASSES_PARSE_ALLOC_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

result_t parse_alloc(Arena *arena, struct ast **dst, unsigned nt) WARN_UNUSED;

/* workaround spurious clang-analyzer-core.NullDereference warn at callsites */
#define checked_alloc(arena, dst, ntype)                                       \
	do {                                                                   \
		assert((dst) != NULL);                                         \
		check(parse_alloc(arena, dst, ntype));                         \
		assert(*(dst) != NULL);                                        \
	} while (0)

#endif
