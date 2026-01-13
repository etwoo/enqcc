#include "passes/sema/flow.h"

#include "passes/parse.h"
#include "passes/sema/constant.h"
#include "passes/sema/walk.h"

#include <assert.h>
#include <limits.h>
#include <string.h> /* for memset() */

enum {
	BLOCK_NESTING_LIMIT = 128,
};

enum containing_statement_type {
	CONTAINING_LOOP,
	CONTAINING_SWITCH,
};

struct containing_statement {
	struct ast *origin;
	enum containing_statement_type statement;
};

struct sema_label_loops_state {
	Arena *arena;
	long long int generator;
	struct containing_statement container[BLOCK_NESTING_LIMIT];
	size_t depth;
};

static WARN_UNUSED struct containing_statement *
has_container(struct sema_label_loops_state *state,
              enum containing_statement_type target)
{
	for (size_t idx = state->depth; idx > 0; --idx) {
		if (state->container[idx - 1].statement == target) {
			return &state->container[idx - 1];
		}
	}
	return NULL;
}

static WARN_UNUSED int128_t
guess(const struct ast *a, const struct ctype *expected_type)
{
	int128_t value = 0;
	struct constant_bytes tmp = {0};
	int128_t l_tmp = 0;
	int128_t r_tmp = 0;

	switch (a->node_type) {
	case NODE_CONSTANT:
		make_initializer_bytes(a, expected_type, &tmp);
		value = tmp.byte_value;
		break;
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		value = guess(a->u.op_unary.operand, expected_type);
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		value = guess(a->u.op_unary.operand, expected_type);
		assert(value <= ULLONG_MAX);
		value = ~(long long unsigned)value;
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
		value = -1 * guess(a->u.op_unary.operand, expected_type);
		break;
	case NODE_EXPRESSION_UNARY_NOT:
		value = !guess(a->u.op_unary.operand, expected_type);
		break;
	case NODE_EXPRESSION_BINARY_ADD:
		value = guess(a->u.op_binary.lhs, expected_type) +
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		value = guess(a->u.op_binary.lhs, expected_type) -
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_BINARY_MULTIPLY:
		value = guess(a->u.op_binary.lhs, expected_type) *
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
		value = guess(a->u.op_binary.rhs, expected_type);
		if (value == 0) {
			/* avoid divide by zero, return arbitrary guess */
			value = 1;
		}
		switch (a->node_type) {
		case NODE_EXPRESSION_BINARY_DIVIDE:
			value = guess(a->u.op_binary.lhs, expected_type) /
			        value;
			break;
		case NODE_EXPRESSION_BINARY_REMAINDER:
			value = guess(a->u.op_binary.lhs, expected_type) %
			        value;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		break;
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		l_tmp = guess(a->u.op_binary.lhs, expected_type);
		r_tmp = guess(a->u.op_binary.rhs, expected_type);
		assert(l_tmp <= ULLONG_MAX && r_tmp <= ULLONG_MAX);
		switch (a->node_type) {
		case NODE_EXPRESSION_BITWISE_AND:
			value = (long long unsigned)l_tmp &
			        (long long unsigned)r_tmp;
			break;
		case NODE_EXPRESSION_BITWISE_OR:
			value = (long long unsigned)l_tmp |
			        (long long unsigned)r_tmp;
			break;
		case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
			value = (long long unsigned)l_tmp
			        << (long long unsigned)r_tmp;
			break;
		case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
			value = (long long unsigned)l_tmp >>
			        (long long unsigned)r_tmp;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		break;
	case NODE_EXPRESSION_LOGICAL_AND:
		value = guess(a->u.op_binary.lhs, expected_type) &&
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_LOGICAL_OR:
		value = guess(a->u.op_binary.lhs, expected_type) ||
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_COMPARE_EQUAL:
		value = guess(a->u.op_binary.lhs, expected_type) ==
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		value = guess(a->u.op_binary.lhs, expected_type) !=
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
		value = guess(a->u.op_binary.lhs, expected_type) <
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
		value = guess(a->u.op_binary.lhs, expected_type) <=
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
		value = guess(a->u.op_binary.lhs, expected_type) >
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		value = guess(a->u.op_binary.lhs, expected_type) >=
		        guess(a->u.op_binary.rhs, expected_type);
		break;
	default:
		break;
	}

	return value;
}

static WARN_UNUSED int128_t
guess_case_value(const struct ast *containing_case,
                 const struct ctype *expected_type)
{
	assert(containing_case->node_type == NODE_CASE);
	return guess(containing_case->u.case_.constant, expected_type);
}

static WARN_UNUSED result_t
make_case(Arena *arena,
          long long int existing_unique,
          struct ast **dst,
          struct ctype *control_type,
          int128_t new_value)
{
	*dst = arena_alloc(arena, sizeof(**dst));
	check_if(*dst == NULL, ERR_SEMA_ALLOC);
	memset(*dst, 0, sizeof(**dst));

	(**dst).node_type = NODE_CASE;
	(**dst).u.case_.unique = existing_unique;

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	memset(new_node, 0, sizeof(*new_node));

	new_node->node_type = NODE_CONSTANT;
	new_node->u.num = new_value;
	check(ctype_copy(arena, control_type, &new_node->expr_type));

	(**dst).u.case_.constant = new_node;
	return RESULT_OK;
}

static WARN_UNUSED result_t
case_prepend(Arena *arena,
             struct ast *containing_switch,
             const struct ast *new_case)
{
	assert(containing_switch->node_type == NODE_SWITCH);
	assert(new_case->node_type == NODE_CASE);

	struct ctype *control_type =
		&containing_switch->u.switch_.control->expr_type;
	const int128_t new_value = guess_case_value(new_case, control_type);

	struct flat *head = containing_switch->u.switch_.label_cases;
	for (; head != NULL; head = head->cdr) {
		assert(head->car->node_type == NODE_CASE);
		const int128_t existing_value =
			guess_case_value(head->car, control_type);
		if (new_value == existing_value) {
			return make_result(ERR_SEMA_CASE_DUPLICATE,
			                   (int)new_value);
		}
	}

	struct flat *node = arena_alloc(arena, sizeof(*node));
	check_if(node == NULL, ERR_SEMA_ALLOC);
	memset(node, 0, sizeof(*node));

	/*
	 * Synthesize NODE_CASE equivalent to <new_case>, only with
	 * u.case_.constant replaced with simplified <new_value>.
	 */
	check(make_case(arena,
	                new_case->u.case_.unique,
	                &node->car,
	                control_type,
	                new_value));

	node->cdr = containing_switch->u.switch_.label_cases;
	containing_switch->u.switch_.label_cases = node;
	return RESULT_OK;
}

enum {
	UNSET_DEFAULT_CASE_SENTINEL = -100,
};

static WARN_UNUSED result_t
sema_enter_loop_id(struct ast *a, void *userdata)
{
	struct sema_label_loops_state *state = userdata;
	struct containing_statement *containing = NULL;
	struct ast *origin = NULL;

	switch (a->node_type) {
	case NODE_FUNCTION:
		assert(state->depth == 0);
		break;
	case NODE_LOOP:
		assert(state->depth < BLOCK_NESTING_LIMIT);
		containing = &state->container[state->depth];
		containing->origin = a;
		containing->statement = CONTAINING_LOOP;
		state->depth++;
		a->u.loop.label_start = state->generator++;
		a->u.loop.label_continue = state->generator++;
		a->u.loop.label_end = state->generator++;
		break;
	case NODE_BREAK:
		if (state->depth == 0) {
			return make_result(ERR_SEMA_BREAK_OUTSIDE);
		}
		origin = state->container[state->depth - 1].origin;
		switch (state->container[state->depth - 1].statement) {
		case CONTAINING_LOOP:
			assert(origin->node_type == NODE_LOOP);
			a->u.num = origin->u.loop.label_end;
			break;
		case CONTAINING_SWITCH:
			assert(origin->node_type == NODE_SWITCH);
			a->u.num = origin->u.switch_.label_end;
			break;
		}
		break;
	case NODE_CONTINUE:
		containing = has_container(state, CONTAINING_LOOP);
		if (containing == NULL) {
			return make_result(ERR_SEMA_CONTINUE_OUTSIDE);
		}
		assert(containing->origin->node_type == NODE_LOOP);
		a->u.num = containing->origin->u.loop.label_continue;
		break;
	case NODE_SWITCH:
		assert(state->depth < BLOCK_NESTING_LIMIT);
		containing = &state->container[state->depth];
		containing->origin = a;
		containing->statement = CONTAINING_SWITCH;
		state->depth++;
		a->u.switch_.label_default = UNSET_DEFAULT_CASE_SENTINEL;
		a->u.switch_.label_end = state->generator++;
		break;
	case NODE_CASE:
		containing = has_container(state, CONTAINING_SWITCH);
		if (containing == NULL) {
			return make_result(ERR_SEMA_CASE_OUTSIDE);
		}
		a->u.case_.unique = state->generator++;
		assert(containing->origin->node_type == NODE_SWITCH);
		check(case_prepend(state->arena, containing->origin, a));
		break;
	case NODE_CASE_DEFAULT:
		containing = has_container(state, CONTAINING_SWITCH);
		if (containing == NULL) {
			return make_result(ERR_SEMA_CASE_DEFAULT_OUTSIDE);
		}
		a->u.case_.unique = state->generator++;
		assert(containing->origin->node_type == NODE_SWITCH);
		if (containing->origin->u.switch_.label_default !=
		    UNSET_DEFAULT_CASE_SENTINEL) {
			return make_result(ERR_SEMA_CASE_DEFAULT_DUPLICATE);
		}
		containing->origin->u.switch_.label_default = a->u.case_.unique;
		break;
	default:
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_exit_loop_id(struct ast *a, void *userdata)
{
	struct sema_label_loops_state *state = userdata;

	switch (a->node_type) {
	case NODE_LOOP:
	case NODE_SWITCH:
		assert(state->depth > 0);
		state->depth--;
		break;
	case NODE_FUNCTION:
		assert(state->depth == 0);
		break;
	default:
		break;
	}

	return RESULT_OK;
}

result_t
sema_label_loops(Arena *arena, struct ast *a, long long int *generator)
{
	struct sema_ops ops = {
		.node_enter = sema_enter_loop_id,
		.node_exit = sema_exit_loop_id,
	};
	struct sema_label_loops_state state = {
		.arena = arena,
		.generator = *generator,
		.depth = 0,
	};
	check(sema_walk(a, &ops, &state));
	*generator = state.generator;
	return RESULT_OK;
}

static const unsigned FLAG_USED_AS_LABEL = 0x1;
static const unsigned FLAG_USED_AS_GOTO_TARGET = 0x2;

struct label {
	struct string_view name;
	long long int id;
	unsigned flags;
	struct label *next;
};

static WARN_UNUSED struct label *
labels_get(struct label *head, const struct string_view *name)
{
	for (struct label *i = head; i != NULL; i = i->next) {
		if (name->sz == i->name.sz &&
		    0 == strncmp(name->data, i->name.data, name->sz)) {
			return i;
		}
	}
	return NULL;
}

struct sema_label_gotos_state {
	Arena *arena;
	long long int generator;
	struct label *labels;
};

static WARN_UNUSED result_t
labels_prepend(struct sema_label_gotos_state *state,
               const struct string_view *name,
               struct label **match)
{
	assert(name->sz > 0 && name->data != NULL);

	*match = labels_get(state->labels, name);
	if (*match != NULL) {
		return RESULT_OK;
	}

	struct label *node = arena_alloc(state->arena, sizeof(*node));
	check_if(node == NULL, ERR_SEMA_ALLOC);
	memset(node, 0, sizeof(*node));

	node->name = *name;
	node->id = ++state->generator;
	node->next = state->labels;
	state->labels = node;
	*match = node;
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_enter_goto_id(struct ast *a, void *userdata)
{
	struct sema_label_gotos_state *state = userdata;
	struct label *match = NULL;

	switch (a->node_type) {
	case NODE_FUNCTION:
		assert(state->labels == NULL);
		break;
	case NODE_GOTO:
		check(labels_prepend(state, &a->u.goto_.target_label, &match));
		match->flags |= FLAG_USED_AS_GOTO_TARGET;
		a->u.goto_.target_unique = match->id;
		break;
	case NODE_LABEL:
		check(labels_prepend(state, &a->u.label.name, &match));
		if (0 != (match->flags & FLAG_USED_AS_LABEL)) {
			return make_result(ERR_SEMA_LABEL_DUPLICATE,
			                   match->name.data,
			                   match->name.sz);
		}
		match->flags |= FLAG_USED_AS_LABEL;
		a->u.label.unique = match->id;
		break;
	default:
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_exit_goto_id(struct ast *a, void *userdata)
{
	if (a->node_type != NODE_FUNCTION) {
		return RESULT_OK;
	}

	struct sema_label_gotos_state *state = userdata;

	for (struct label *i = state->labels; i != NULL; i = i->next) {
		if (0 != (i->flags & FLAG_USED_AS_GOTO_TARGET) &&
		    0 == (i->flags & FLAG_USED_AS_LABEL)) {
			return make_result(ERR_SEMA_GOTO_NONEXISTENT_LABEL,
			                   i->name.data,
			                   i->name.sz);
		}
	}

	state->labels = NULL;
	return RESULT_OK;
}

result_t
sema_label_gotos(Arena *arena, struct ast *a, long long int *generator)
{
	struct sema_ops ops = {
		.node_enter = sema_enter_goto_id,
		.node_exit = sema_exit_goto_id,
	};
	struct sema_label_gotos_state state = {
		.arena = arena,
		.generator = *generator,
		.labels = NULL,
	};
	check(sema_walk(a, &ops, &state));
	*generator = state.generator;
	return RESULT_OK;
}
