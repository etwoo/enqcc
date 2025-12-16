#ifndef C_LANGUAGE_TYPES_H
#define C_LANGUAGE_TYPES_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"

#include <stdbool.h>

/* note: order of values below determines integer conversion rank */
#define FOREACH_CTYPE(F)                                                       \
	F(INT)                                                                 \
	F(UNSIGNED_INT)                                                        \
	F(LONG)                                                                \
	F(UNSIGNED_LONG)                                                       \
	F(DOUBLE)                                                              \
	F(POINTER_TO)                                                          \
	F(ARRAY_OF)

struct ctype {
#define TO_ENUM(t) CTYPE_##t,
	enum { FOREACH_CTYPE(TO_ENUM) } t;
#undef TO_ENUM
	bool maybe_null_pointer_constant;
	struct ctype *referent; /* CTYPE_POINTER_TO, CTYPE_ARRAY_OF */
	long long unsigned sz;  /* CTYPE_ARRAY_OF */
};

result_t ctype_alloc(Arena *arena, struct ctype **dst) WARN_UNUSED;
result_t ctype_copy(Arena *arena,
                    const struct ctype *src,
                    struct ctype *dst) WARN_UNUSED;
const char *ctype_to_str(const struct ctype *c, char *stor, size_t cap);
long long int ctype_to_size_bytes(const struct ctype *c) WARN_UNUSED;
bool ctype_is_integer(const struct ctype *c) WARN_UNUSED;
bool ctype_is_signed(const struct ctype *c) WARN_UNUSED;
bool ctype_is_floating_point(const struct ctype *c) WARN_UNUSED;
bool ctype_is_pointer(const struct ctype *c) WARN_UNUSED;
bool ctype_nullptr_ish(const struct ctype *c) WARN_UNUSED;
const struct ctype *get_common_ctype(const struct ctype *lhs,
                                     const struct ctype *rhs) WARN_UNUSED;
bool ctype_is_equal(const struct ctype *lhs,
                    const struct ctype *rhs) WARN_UNUSED;

#endif
