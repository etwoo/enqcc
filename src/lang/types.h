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
bool ctype_is_struct_mismatch(const struct ctype *lhs,
                              const struct ctype *rhs) WARN_UNUSED;
void ctype_array_decay_to_pointer(struct ctype *c);

/*
 * From "Writing a C Compiler" by Nora Sandler, Chapter 15, Section "Type
 * Checking Pointer Arithmetic":
 *
 *   To type check addition involving a pointer and an integer, we first
 *   convert the integer operand to a long. This will simplify later
 *   compiler passes, when pointer indices will need to be 8 bytes wide
 *   so that we can add them to 8-byte memory addresses. This conversion
 *   doesn't come from the C standard; we're just adding it for our own
 *   convenience. But it also doesn’t violate the standard; converting a
 *   valid array index to long won't change its value, so the result of
 *   the whole expression is the same either way. (If an integer is too
 *   big to represent as a long, we can safely assume that it's not a
 *   valid array index, since no hardware supports arrays with anywhere
 *   close to 263 elements.)
 */
extern const struct ctype LIKE_PTRDIFF_T;

/*
 * From "Writing a C Compiler" by Nora Sandler, Chapter 17, Section "sizeof
 * Expressions":
 *
 *   A sizeof expression has type size_t; in our implementation, that's
 *   just unsigned long.
 */
extern const struct ctype LIKE_SIZE_T;

struct type_member {
	struct string_view member_name;
	struct ctype member_type;
	long long int member_offset;
};

struct type_table {
	struct ctype c;
	long long int aggregate_size;
	long long int aggregate_alignment;
	long long unsigned n_members;
	struct type_member *members __attribute__((counted_by(n_members)));
	struct type_table *next;
};

result_t types_prepend(Arena *arena,
                       struct type_table **head,
                       struct ctype *new_type) WARN_UNUSED;
struct type_table *types_find(struct type_table *head,
                              const struct ctype *needle) WARN_UNUSED;
long long int ctype_to_size_bytes_with_types(const struct ctype *c,
                                             struct type_table *t) WARN_UNUSED;
long long int ctype_to_alignment(const struct ctype *c,
                                 struct type_table *t) WARN_UNUSED;
bool ctype_is_incomplete(const struct ctype *c,
                         struct type_table *t) WARN_UNUSED;
bool ctype_is_ptr_to_incomplete(const struct ctype *c,
                                struct type_table *t) WARN_UNUSED;
struct type_member *
ctype_find_member(struct type_table *type_entry,
                  const struct string_view *member_name) WARN_UNUSED;
struct ctype *
ctype_of_member(struct type_table *type_entry,
                const struct string_view *member_name) WARN_UNUSED;

long long int round_up_to_multiple_of(long long int n,
                                      long long int base) WARN_UNUSED;

#endif
