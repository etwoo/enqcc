#ifndef COMPILER_PASSES_SEMA_WALK_H
#define COMPILER_PASSES_SEMA_WALK_H

#include "arena.h"
#include "lang/types.h"
#include "result.h"
#include "sys/compiler_features.h"

struct ast;

struct sema_ops {
	result_t (*node_enter)(struct ast *a, void *userdata);
	result_t (*node_exit)(struct ast *a, void *userdata);
};
result_t sema_walk(struct ast *a, struct sema_ops *ops, void *u) WARN_UNUSED;

result_t sema_walk_initializer(struct ast **ast_handle,
                               const struct ctype *dst_type,
                               struct type_table *tt,
                               result_t (*visit)(struct ast **,
                                                 const struct ctype *,
                                                 void *),
                               void *userdata) WARN_UNUSED;

#endif
