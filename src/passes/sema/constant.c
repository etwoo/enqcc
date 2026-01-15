#include "passes/sema/constant.h"

#include "passes/parse.h"
#include "passes/sema/walk.h"

#include <assert.h>
#include <limits.h>

enum {
	STRUCT_NESTING_LIMIT = 32,
};

struct sema_count_state {
	long long int offset[STRUCT_NESTING_LIMIT];
	size_t depth;
	long long unsigned count;
};

static WARN_UNUSED result_t
count_enter(const struct type_table *t MAYBE_UNUSED, void *userdata)
{
	struct sema_count_state *state = userdata;
	state->depth++;
	assert(state->depth < STRUCT_NESTING_LIMIT);
	assert(state->offset[state->depth] == 0);
	return RESULT_OK;
}

static WARN_UNUSED result_t
count_exit(const struct type_table *t, void *userdata)
{
	struct sema_count_state *state = userdata;
	assert(state->depth > 0);

	const long long int offset = state->offset[state->depth];
	if (t->aggregate_size > offset) {
		state->count++;
	}

	state->offset[state->depth] = 0;
	state->depth--;
	return RESULT_OK;
}

static WARN_UNUSED result_t
count_visit(struct ast **ast_handle,
            const struct ctype *dst_type,
            const struct type_member *dst_member,
            void *userdata)
{
	struct sema_count_state *state = userdata;
	const struct ast *a = *ast_handle;
	assert(a->node_type == NODE_EXPRESSION_INITIALIZER);

	if (a->u.init.single == NULL) {
		return RESULT_OK;
	}

	if (dst_member != NULL) {
		long long int offset = state->offset[state->depth];
		if (dst_member->member_offset > offset) {
			state->count++;
		}
	}

	state->count++;

	if (dst_member != NULL) {
		state->offset[state->depth] = dst_member->member_offset;
	}
	state->offset[state->depth] += ctype_to_size_bytes(dst_type);

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

struct sema_populate_state {
	long long int offset[STRUCT_NESTING_LIMIT];
	size_t depth;
	struct constant_bytes *pos;
};

static WARN_UNUSED result_t
populate_enter(const struct type_table *t MAYBE_UNUSED, void *userdata)
{
	struct sema_populate_state *state = userdata;
	state->depth++;
	assert(state->depth < STRUCT_NESTING_LIMIT);
	assert(state->offset[state->depth] == 0);
	return RESULT_OK;
}

static WARN_UNUSED result_t
populate_exit(const struct type_table *t, void *userdata)
{
	struct sema_populate_state *state = userdata;
	assert(state->depth > 0);

	long long int offset = state->offset[state->depth];
	if (t->aggregate_size > offset) {
		state->pos->byte_value = 0;
		state->pos->byte_count = t->aggregate_size - offset;
		state->pos++;
	}

	state->offset[state->depth] = 0;
	state->depth--;
	return RESULT_OK;
}

static WARN_UNUSED result_t
populate_visit(struct ast **ast_handle,
               const struct ctype *dst_type,
               const struct type_member *dst_member,
               void *userdata)
{
	struct sema_populate_state *state = userdata;

	const struct ast *a = *ast_handle;
	assert(a->node_type == NODE_EXPRESSION_INITIALIZER);

	const struct ast *s = a->u.init.single;
	if (s == NULL) {
		return RESULT_OK;
	}

	if (dst_member != NULL) {
		long long int offset = state->offset[state->depth];
		if (dst_member->member_offset > offset) {
			state->pos->byte_value = 0;
			state->pos->byte_count =
				dst_member->member_offset - offset;
			state->pos++;
		}
	}

	switch (s->node_type) {
	case NODE_CONSTANT:
		make_initializer_bytes(s, dst_type, state->pos);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		assert(s->u.var.stype == SYMBOL_STRING_LITERAL);
		state->pos->unique = s->u.var.unique;
		state->pos->byte_count = 8; /* set .quad for str literal */
		break;
	default:
		assert(0); /* logic error in caller */
		break;
	}

	if (dst_member != NULL) {
		state->offset[state->depth] = dst_member->member_offset;
	}
	assert(state->pos->byte_count < LLONG_MAX);
	state->offset[state->depth] += (long long int)state->pos->byte_count;

	state->pos++;
	return RESULT_OK;
}

result_t
make_initializer(Arena *arena,
                 struct ast *a,
                 const struct ctype *dst_type,
                 struct type_table *t,
                 struct constant_initializer *out)
{
	struct sema_initializer_ops counter = {
		.struct_enter = count_enter,
		.visit = count_visit,
		.struct_exit = count_exit,
	};
	struct sema_count_state count = {0};
	check(sema_walk_initializer_scope(&a, dst_type, t, &counter, &count));
	assert(count.count > 0);

	out->count = count.count;
	out->elements = arena_alloc(arena, out->count * sizeof(*out->elements));
	check_if(out->elements == NULL, ERR_SEMA_ALLOC);

	struct sema_initializer_ops populater = {
		.struct_enter = populate_enter,
		.visit = populate_visit,
		.struct_exit = populate_exit,
	};
	struct sema_populate_state pop = {
		.pos = out->elements,
	};
	check(sema_walk_initializer_scope(&a, dst_type, t, &populater, &pop));
	assert(pop.pos >= out->elements);
	assert((long long unsigned)(pop.pos - out->elements) == out->count);

	return RESULT_OK;
}
