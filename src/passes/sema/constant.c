#include "passes/sema/constant.h"

#include "passes/parse.h"
#include "passes/sema/walk.h"

#include <assert.h>
#include <limits.h>

static WARN_UNUSED result_t
visit_cnt(struct ast **ast_handle,
          const struct ctype *dst_type MAYBE_UNUSED,
          void *userdata)
{
	const struct ast *a = *ast_handle;
	assert(a->node_type == NODE_EXPRESSION_INITIALIZER);

	long long unsigned *count = userdata;
	if (a->u.init.single != NULL) {
		(*count)++;
	}

	return RESULT_OK;
}

static const long long int INT_TO_CHAR_TRUNCATOR = 256;
static const long long int LONG_TO_INT_TRUNCATOR = 4294967296;

void
make_initializer_bytes(const struct ast *a,
                       const struct ctype *dst_type,
                       struct constant_bytes *out)
{
	assert(a->node_type == NODE_CONSTANT);
	assert(!ctype_is_aggregate(dst_type));
	out->byte_count = ctype_to_size_bytes(dst_type);

	if (dst_type->t == CTYPE_DOUBLE) {
		double tmp = 0;
		switch (a->expr_type.t) {
		case CTYPE_CHAR:
		case CTYPE_SIGNED_CHAR:
		case CTYPE_UNSIGNED_CHAR:
		case CTYPE_INT:
		case CTYPE_UNSIGNED_INT:
		case CTYPE_LONG:
		case CTYPE_UNSIGNED_LONG:
		case CTYPE_POINTER_TO:
			tmp = (double)a->u.num;
			break;
		case CTYPE_DOUBLE:
			tmp = a->u.double_;
			break;
		case CTYPE_ARRAY_OF:
		case CTYPE_STRUCT:
		case CTYPE_VOID:
			assert(0); /* logic error in caller */
			break;
		}
		out->byte_value = get_double_as_quadword(tmp);
		return;
	}

	int128_t x = 0;
	switch (a->expr_type.t) {
	case CTYPE_CHAR:
	case CTYPE_SIGNED_CHAR:
	case CTYPE_UNSIGNED_CHAR:
	case CTYPE_INT:
	case CTYPE_UNSIGNED_INT:
	case CTYPE_LONG:
	case CTYPE_UNSIGNED_LONG:
	case CTYPE_POINTER_TO:
		x = a->u.num;
		break;
	case CTYPE_DOUBLE:
		x = (int128_t)a->u.double_;
		break;
	case CTYPE_ARRAY_OF:
	case CTYPE_STRUCT:
	case CTYPE_VOID:
		assert(0); /* logic error in caller */
		break;
	}

	if ((dst_type->t == CTYPE_CHAR && x > CHAR_MAX) ||
	    (dst_type->t == CTYPE_SIGNED_CHAR && x > SCHAR_MAX) ||
	    (dst_type->t == CTYPE_UNSIGNED_CHAR && x > UCHAR_MAX)) {
		x %= INT_TO_CHAR_TRUNCATOR;
	}

	if ((dst_type->t == CTYPE_INT && x > INT_MAX) ||
	    (dst_type->t == CTYPE_UNSIGNED_INT && x > UINT_MAX)) {
		x %= LONG_TO_INT_TRUNCATOR;
	}

	assert(x >= 0); /* negative constants currently unsupported */
	out->byte_value = x;
}

static WARN_UNUSED result_t
visit_pop(struct ast **ast_handle, const struct ctype *dst_type, void *userdata)
{
	const struct ast *a = *ast_handle;
	assert(a->node_type == NODE_EXPRESSION_INITIALIZER);

	const struct ast *s = a->u.init.single;
	if (s == NULL) {
		return RESULT_OK;
	}

	struct constant_bytes **pos = (struct constant_bytes **)userdata;
	switch (s->node_type) {
	case NODE_CONSTANT:
		make_initializer_bytes(s, dst_type, *pos);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		assert(s->u.var.stype == SYMBOL_STRING_LITERAL);
		(**pos).unique = s->u.var.unique;
		(**pos).byte_count = 8; /* set .quad for str literal */
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}
	(*pos)++;

	return RESULT_OK;
}

// TODO: add zeros for any padding between members
// TODO: add zeros for any padding after final member
// TODO: use type_table aggregate_size, member_offset
//
// maybe make sema_walk_initializer() callback take type_table entry including
// ctype as member, instead of taking just the ctype; convert existing callers
// to access ctype member, then change make_initializer() to use type_entry to
// access member_offset, accumulate as necessary, add zero padding to
// constant_initializer (while leaving AST as-is)
//
// ^^^ above should handle inter-member padding; can then handle final
// aggregate_size - offset -> ending padding in a final step
//
// TODO: add callbacks to complement visit(), something like
//
// struct sema_initializer_ops {
//     result_t (*struct_enter)(const struct type_entry *struct_info);
//     result_t (*member_visit)(struct ast **a,
//                              const struct type_member *member_info,
//                              void *userdata);
//     result_t (*struct_exit)(const struct type_entry *struct_info);
// };
//
// each time we recurse into any member that is itself a struct, so that we can
// push() onto stack maintained in userdata that holds a new variable tracking
// offset into current object, which then allows exit() to add final padding on
// structs-within-structs and then pop() that sub-struct's offset, allowing
// recursive caller to continue at its respective offset
//
//   -> alternative: use fixed-size array and track recursion depth, similar to
//   sema_label_loops_state container array + depth size_t
//
// TODO: maybe possible to convert sema_expr_types_initializer_zero_pad() to
// walk API, using new struct_enter() callback, instead of visit()
result_t
make_initializer(Arena *arena,
                 struct ast *a,
                 const struct ctype *dst_type,
                 struct type_table *t,
                 struct constant_initializer *out)
{
	long long unsigned count = 0;
	check(sema_walk_initializer(&a, dst_type, t, visit_cnt, &count));
	assert(count > 0);

	out->count = count;
	out->elements = arena_alloc(arena, out->count * sizeof(*out->elements));
	check_if(out->elements == NULL, ERR_SEMA_ALLOC);

	struct constant_bytes *pos = out->elements;
	check(sema_walk_initializer(&a, dst_type, t, visit_pop, (void *)&pos));
	assert((size_t)(pos - out->elements) == out->count);

	return RESULT_OK;
}
