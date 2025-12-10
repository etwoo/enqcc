#ifndef C_LANGUAGE_TYPES_H
#define C_LANGUAGE_TYPES_H

#include "arena.h"
#include "result.h"

/* note: order of values below determines integer conversion rank */
#define FOREACH_CTYPE(F)                                                       \
	F(INT)                                                                 \
	F(UNSIGNED_INT)                                                        \
	F(LONG)                                                                \
	F(UNSIGNED_LONG)                                                       \
	F(DOUBLE)                                                              \
	F(POINTER_TO)

struct ctype {
#define TO_ENUM(t) CTYPE_##t,
	enum { FOREACH_CTYPE(TO_ENUM) } t;
#undef TO_ENUM
	struct ctype *referent; /* CTYPE_POINTER */
};

result_t ctype_alloc(Arena *arena, struct ctype **dst) WARN_UNUSED;
result_t ctype_copy(Arena *arena,
                    const struct ctype *src,
                    struct ctype *dst) WARN_UNUSED;
const char *ctype_to_str(struct ctype *c, char *stor, size_t cap);
long long int ctype_to_size_bytes(struct ctype *c) WARN_UNUSED;
bool ctype_is_signed(struct ctype *c) WARN_UNUSED;
bool ctype_is_floating_point(struct ctype *c) WARN_UNUSED;
enum ctype get_common_ctype(struct ctype *lhs, struct ctype *rhs) WARN_UNUSED;
bool ctype_is_equal(struct ctype *lhs, struct ctype *rhs) WARN_UNUSED;

#endif
