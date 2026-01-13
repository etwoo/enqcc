#ifndef C_LANGUAGE_TYPES_H
#define C_LANGUAGE_TYPES_H

#include "arena.h"
#include "result.h"
#include "sys/compiler_features.h"
#include "sys/string_view.h"

#include <stdbool.h>

/* note: order of values below determines integer conversion rank */
#define FOREACH_CTYPE(F)                                                       \
	F(CHAR)                                                                \
	F(SIGNED_CHAR)                                                         \
	F(UNSIGNED_CHAR)                                                       \
	F(INT)                                                                 \
	F(UNSIGNED_INT)                                                        \
	F(LONG)                                                                \
	F(UNSIGNED_LONG)                                                       \
	F(DOUBLE)                                                              \
	F(ARRAY_OF)                                                            \
	F(POINTER_TO)                                                          \
	F(STRUCT)                                                              \
	F(VOID)

struct ctype {
#define TO_ENUM(t) CTYPE_##t,
	enum { FOREACH_CTYPE(TO_ENUM) } t;
#undef TO_ENUM
	bool maybe_null_pointer_constant;
	struct ctype *referent;      /* CTYPE_POINTER_TO, CTYPE_ARRAY_OF */
	long long unsigned sz;       /* CTYPE_ARRAY_OF */
	struct string_view tag_name; /* CTYPE_STRUCT */
	long long int tag_unique;    /* CTYPE_STRUCT */
};

result_t ctype_alloc(Arena *arena, struct ctype **dst) WARN_UNUSED;
result_t ctype_alloc_str_literal(Arena *arena,
                                 const struct string_view *src,
                                 struct ctype *dst) WARN_UNUSED;
result_t ctype_copy(Arena *arena,
                    const struct ctype *src,
                    struct ctype *dst) WARN_UNUSED;
const char *ctype_to_str(const struct ctype *c, char *stor, size_t cap);
long long int ctype_to_size_bytes(const struct ctype *c) WARN_UNUSED;
bool ctype_is_integer(const struct ctype *c) WARN_UNUSED;
bool ctype_is_signed(const struct ctype *c) WARN_UNUSED;
bool ctype_is_floating_point(const struct ctype *c) WARN_UNUSED;
bool ctype_is_charlike(const struct ctype *c) WARN_UNUSED;
bool ctype_is_pointer(const struct ctype *c) WARN_UNUSED;
bool ctype_is_array(const struct ctype *c) WARN_UNUSED;
bool ctype_is_strlike_array(const struct ctype *c) WARN_UNUSED;
bool ctype_is_strlike_ptr(const struct ctype *c) WARN_UNUSED;
bool ctype_is_void(const struct ctype *c) WARN_UNUSED;
bool ctype_is_void_ptr(const struct ctype *c) WARN_UNUSED;
bool ctype_is_struct(const struct ctype *c) WARN_UNUSED;
bool ctype_is_aggregate(const struct ctype *c) WARN_UNUSED;
bool ctype_nullptr_ish(const struct ctype *c) WARN_UNUSED;
const struct ctype *get_common_ctype(const struct ctype *lhs,
                                     const struct ctype *rhs) WARN_UNUSED;
bool ctype_is_equal(const struct ctype *lhs,
                    const struct ctype *rhs) WARN_UNUSED;
void ctype_array_decay_to_pointer(struct ctype *c);

struct type_table {
	struct ctype c;
	// TODO: total size of this struct
	long long unsigned n_members;
	struct {
		struct string_view member_name;
		struct ctype member_type;
		// TODO: alignment of each member
	} *members __attribute__((counted_by(n_members)));
	struct type_table *next;
};

result_t types_prepend(Arena *arena,
                       struct type_table **head,
                       struct ctype *new_type) WARN_UNUSED;
struct type_table *types_find(struct type_table *head,
                              const struct ctype *needle) WARN_UNUSED;
bool ctype_is_incomplete(const struct ctype *c,
                         struct type_table *t) WARN_UNUSED;
bool ctype_is_ptr_to_incomplete(const struct ctype *c,
                                struct type_table *t) WARN_UNUSED;
struct ctype *
ctype_of_member(struct type_table *type_entry,
                const struct string_view *member_name) WARN_UNUSED;

#endif
