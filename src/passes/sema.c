#include "lang/symbol.h"
#include "passes.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdlib.h>    /* for strtoll() */
#include <sys/param.h> /* for MAX() */

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
static const struct ctype LIKE_PTRDIFF_T = {.t = CTYPE_LONG};

static WARN_UNUSED bool
is_node_lvalue(const struct ast *a)
{
	while (a->node_type == NODE_EXPRESSION_PAREN_ENCLOSED) {
		a = a->u.op_unary.operand;
	}
	return a->node_type == NODE_EXPRESSION_VARIABLE_USAGE ||
	       a->node_type == NODE_EXPRESSION_UNARY_DEREFERENCE;
}

static WARN_UNUSED const struct ast *
unpack_cast(const struct ast *a)
{
	while (a->node_type == NODE_EXPRESSION_CAST) {
		/* unpack nodes inserted by sema_implicit_cast() */
		a = a->u.cast.expr;
	}
	return a;
}

static WARN_UNUSED bool
is_node_constant(const struct ast *a)
{
	a = unpack_cast(a);
	if (a == NULL || a->node_type != NODE_EXPRESSION_INITIALIZER) {
		return false;
	}

	if (a->u.init.single != NULL) {
		const enum ast_nodetype nt = a->u.init.single->node_type;
		return nt == NODE_CONSTANT ||
		       (nt == NODE_EXPRESSION_VARIABLE_USAGE &&
		        a->u.init.single->u.var.stype == SYMBOL_STRING_LITERAL);
	}
	assert(a->u.init.multi != NULL);

	for (struct flat *f = a->u.init.multi; f != NULL; f = f->cdr) {
		if (!is_node_constant(f->car)) {
			return false;
		}
	}
	return true;
}

static WARN_UNUSED long long unsigned
count_initializer_elements(struct ctype *dst_type, const struct ast *a)
{
	assert(dst_type != NULL);

	a = unpack_cast(a);
	assert(a->node_type == NODE_EXPRESSION_INITIALIZER);

	const struct ast *s = a->u.init.single;
	if (s != NULL) {
		assert(is_node_constant(a));
		return 1;
	}
	assert(a->u.init.multi != NULL);
	assert(ctype_is_pointer(dst_type));

	long long unsigned count = 0;
	for (struct flat *f = a->u.init.multi; f != NULL; f = f->cdr) {
		count += count_initializer_elements(dst_type->referent, f->car);
	}
	return count;
}

static const long long int LONG_TO_INT_TRUNCATOR = 4294967296;

static void
map_numeric_type_scalar(const struct ast *a,
                        struct ctype *dst_type,
                        struct constant_bytes *out)
{
	assert(a->node_type == NODE_CONSTANT);
	assert(!ctype_is_array(dst_type));
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
		assert(0); /* logic error in caller */
		break;
	}

	if ((dst_type->t == CTYPE_INT && x > INT_MAX) ||
	    (dst_type->t == CTYPE_UNSIGNED_INT && x > UINT_MAX)) {
		x %= LONG_TO_INT_TRUNCATOR;
	}

	assert(x >= 0); /* negative constants currently unsupported */
	out->byte_value = x;
}

static void
populate_initializer_elements(const struct ast *a,
                              struct ctype *dst_type,
                              struct constant_bytes **pos)
{
	a = unpack_cast(a);
	assert(a->node_type == NODE_EXPRESSION_INITIALIZER);

	if (a->u.init.single != NULL) {
		const struct ast *s = a->u.init.single;
		switch (s->node_type) {
		case NODE_CONSTANT:
			map_numeric_type_scalar(s, dst_type, (*pos)++);
			break;
		case NODE_EXPRESSION_VARIABLE_USAGE:
			assert(s->u.var.stype == SYMBOL_STRING_LITERAL);
			// TODO: add pointer as initializer element, using
			// `.quad <label>` syntax (i.e. pointer as quad)
			return;
		default:
			assert(0); /* logic error in caller */
			break;
		}
		return;
	}
	assert(a->u.init.multi != NULL);

	for (struct flat *f = a->u.init.multi; f != NULL; f = f->cdr) {
		populate_initializer_elements(f->car, dst_type->referent, pos);
	}
}

static WARN_UNUSED result_t
map_numeric_type(Arena *arena,
                 const struct ast *init,
                 struct ctype *dst_type,
                 struct constant_initializer *out)
{
	out->count = count_initializer_elements(dst_type, init);
	assert(out->count > 0);
	out->elements = arena_alloc(arena, out->count * sizeof(*out->elements));
	check_if(out->elements == NULL, ERR_SEMA_ALLOC);
	struct constant_bytes *cursor = out->elements;
	populate_initializer_elements(init, dst_type, &cursor);
	return RESULT_OK;
}

struct sema_ops {
	result_t (*node_enter)(struct ast *a, void *userdata);
	result_t (*node_exit)(struct ast *a, void *userdata);
};

static result_t
sema_walk_flat(struct flat *a, const struct sema_ops *ops, void *u) WARN_UNUSED;

static WARN_UNUSED result_t
sema_walk(struct ast *a, const struct sema_ops *ops, void *u)
{
	if (a == NULL) {
		return RESULT_OK;
	}

	if (ops->node_enter != NULL) {
		check(ops->node_enter(a, u));
	}

	switch (a->node_type) {
	case NODE_PROGRAM:
		check(sema_walk_flat(a->u.program.globals, ops, u));
		break;
	case NODE_FUNCTION:
		check(sema_walk(a->u.function.block, ops, u));
		break;
	case NODE_BLOCK:
		check(sema_walk_flat(a->u.block.statements, ops, u));
		break;
	case NODE_DECLARATION:
		check(sema_walk(a->u.declare.init, ops, u));
		break;
	case NODE_IF_ELSE:
		check(sema_walk(a->u.if_.condition, ops, u));
		check(sema_walk_flat(a->u.if_.then_clause, ops, u));
		check(sema_walk_flat(a->u.if_.else_clause, ops, u));
		break;
	case NODE_LOOP:
		check(sema_walk(a->u.loop.precond, ops, u));
		check(sema_walk_flat(a->u.loop.body, ops, u));
		check(sema_walk(a->u.loop.incr, ops, u));
		check(sema_walk(a->u.loop.postcond, ops, u));
		break;
	case NODE_SWITCH:
		check(sema_walk(a->u.switch_.control, ops, u));
		check(sema_walk_flat(a->u.switch_.body, ops, u));
		break;
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
		break;
	case NODE_EXPRESSION_INITIALIZER:
		check(sema_walk(a->u.init.single, ops, u));
		check(sema_walk_flat(a->u.init.multi, ops, u));
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		check(sema_walk(a->u.op_unary.operand, ops, u));
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
	case NODE_EXPRESSION_SUBSCRIPT:
		check(sema_walk(a->u.op_binary.lhs, ops, u));
		check(sema_walk(a->u.op_binary.rhs, ops, u));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(sema_walk(a->u.op_binary.lhs, ops, u));
		check(sema_walk(a->u.op_ternary.condition, ops, u));
		check(sema_walk(a->u.op_ternary.then_expr, ops, u));
		check(sema_walk(a->u.op_ternary.else_expr, ops, u));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(sema_walk_flat(a->u.call.args, ops, u));
		break;
	case NODE_EXPRESSION_CAST:
		check(sema_walk(a->u.cast.expr, ops, u));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_NULL:
	case NODE_CONSTANT:
	case NODE_CONSTANT_STR:
		break;
	}

	if (ops->node_exit != NULL) {
		check(ops->node_exit(a, u));
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_walk_flat(struct flat *a, const struct sema_ops *ops, void *u)
{
	const struct flat *cursor = a;
	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		check(sema_walk(cursor->car, ops, u));
	}
	return RESULT_OK;
}

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
guess(const struct ast *a, struct ctype *expected_type)
{
	int128_t value = 0;
	struct constant_bytes tmp = {0};
	int128_t l_tmp = 0;
	int128_t r_tmp = 0;

	switch (a->node_type) {
	case NODE_CONSTANT:
		map_numeric_type_scalar(a, expected_type, &tmp);
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
guess_case_value(const struct ast *containing_case, struct ctype *expected_type)
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

static WARN_UNUSED result_t
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

static WARN_UNUSED result_t
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

static WARN_UNUSED result_t
sema_compound_assignment(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	enum ast_nodetype new_type = 0;
	switch (a->node_type) {
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
		new_type = NODE_EXPRESSION_BINARY_ADD;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
		new_type = NODE_EXPRESSION_BINARY_SUBTRACT;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
		new_type = NODE_EXPRESSION_BINARY_MULTIPLY;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
		new_type = NODE_EXPRESSION_BINARY_DIVIDE;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
		new_type = NODE_EXPRESSION_BINARY_REMAINDER;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
		new_type = NODE_EXPRESSION_BITWISE_AND;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
		new_type = NODE_EXPRESSION_BITWISE_OR;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
		new_type = NODE_EXPRESSION_BITWISE_XOR;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
		new_type = NODE_EXPRESSION_BITWISE_SHIFT_LEFT;
		break;
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
		new_type = NODE_EXPRESSION_BITWISE_SHIFT_RIGHT;
		break;
	default:
		return RESULT_OK;
	}

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	new_node->node_type = new_type;
	check(ctype_copy(arena, &a->expr_type, &new_node->expr_type));
	new_node->u.op_binary = a->u.op_binary;

	a->node_type = NODE_EXPRESSION_VARIABLE_ASSIGNMENT;
	a->u.op_binary.rhs = new_node;
	/* retain existing a->u.op_binary.lhs */

	/*
	 * Doubly-link compound assignment nodes to one another, in
	 * preparation for kludge in ir_assignment().
	 */
	new_node->u.op_binary.lhs->kludge.compound_assignment_twin =
		a->u.op_binary.lhs;
	a->u.op_binary.lhs->kludge.compound_assignment_twin =
		new_node->u.op_binary.lhs;

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_subscript(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	if (a->node_type != NODE_EXPRESSION_SUBSCRIPT) {
		return RESULT_OK;
	}

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	new_node->node_type = NODE_EXPRESSION_BINARY_ADD;
	check(ctype_copy(arena, &a->expr_type, &new_node->expr_type));
	new_node->u.op_binary = a->u.op_binary;

	a->node_type = NODE_EXPRESSION_UNARY_DEREFERENCE;
	a->u.op_unary.operand = new_node;

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_str_literal_expand(Arena *arena,
                        const struct string_view *lit,
                        struct flat **dst)
{
	assert(dst != NULL);
	assert(*dst == NULL);

	for (size_t i = 0; i <= lit->sz; ++i) {
		check(flat_alloc(arena, dst));
		check(parse_alloc(arena,
		                  &(**dst).car,
		                  NODE_EXPRESSION_INITIALIZER));
		check(parse_alloc(arena,
		                  &(**dst).car->u.init.single,
		                  NODE_CONSTANT));

		struct ast *new_node = (**dst).car->u.init.single;
		if (i < lit->sz) {
			new_node->u.num = (int)lit->data[i];
		} else {
			new_node->u.num = 0; /* implicit NUL terminator */
		}
		new_node->expr_type.t = CTYPE_INT;

		(**dst).car->expr_type.t = CTYPE_INT;
		dst = &(**dst).cdr;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_str_literal_as_init(Arena *arena, struct ast *init)
{
	assert(init != NULL);
	assert(init->node_type == NODE_EXPRESSION_INITIALIZER);

	if (init->u.init.single != NULL &&
	    init->u.init.single->node_type == NODE_CONSTANT_STR) {
		const struct string_view deepcopy = init->u.init.single->u.str;
		init->u.init.single = NULL;
		check(sema_str_literal_expand(arena,
		                              &deepcopy,
		                              &init->u.init.multi));
		return RESULT_OK;
	}

	for (struct flat *f = init->u.init.multi; f != NULL; f = f->cdr) {
		check(sema_str_literal_as_init(arena, f->car));
	}
	return RESULT_OK;
}

// TODO: handle string literals nested within array initializers
static WARN_UNUSED result_t
sema_str_literal_hoist(Arena *arena,
                       struct ctype *var_type,
                       struct ast *init,
                       struct ast **new_node,
                       struct symbol **s)
{
	struct ctype array_type = {0};
	check(ctype_copy(arena, var_type, &array_type));
	array_type.t = CTYPE_ARRAY_OF;
	assert(ctype_is_strlike_array(&array_type));

	/* translate u.init.multi into equivalent constant_initializer */
	assert(init->u.init.multi != NULL);
	struct constant_initializer initializer = {0};
	check(map_numeric_type(arena, init, &array_type, &initializer));

	array_type.sz = initializer.count;

	/* add constant_initializer to symbol table */
	struct string_view dummy_name = {0};
	check(symbols_prepend(arena,
	                      s,
	                      &dummy_name,
	                      SYMBOL_STRING_LITERAL,
	                      &array_type));
	(**s).linkage.initial = INITIAL_VALUE_CONSTANT;
	(**s).linkage.initializer = initializer;

	/* remove array init expression from AST */
	init->u.init.multi = NULL;

	/* create replacement initializer: simple variable reference */
	check(parse_alloc(arena, new_node, NODE_EXPRESSION_VARIABLE_USAGE));
	(**new_node).expr_type = array_type;

	/* make variable expr refer to string literal in symbol table */
	struct ast_symbol *new_var = &(**new_node).u.var;
	new_var->name = dummy_name;
	new_var->unique = (**s).unique;
	new_var->stype = SYMBOL_STRING_LITERAL;
	new_var->ltype = SYMBOL_LINKAGE_INTERNAL;

	return RESULT_OK;
}

struct sema_str_literal_state {
	Arena *arena;
	struct symbol *string_literal_symbols;
};

static WARN_UNUSED result_t
sema_str_literal(struct ast *a, void *userdata)
{
	struct sema_str_literal_state *state = userdata;
	Arena *arena = state->arena;
	struct symbol **symbols = &state->string_literal_symbols;

	if (a->node_type == NODE_CONSTANT_STR) {
		struct ast *fake_init = NULL;
		check(parse_alloc(arena,
		                  &fake_init,
		                  NODE_EXPRESSION_INITIALIZER));
		check(sema_str_literal_expand(arena,
		                              &a->u.str,
		                              &fake_init->u.init.multi));
		struct ast *new_node = NULL;
		check(sema_str_literal_hoist(arena,
		                             &a->expr_type,
		                             fake_init,
		                             &new_node,
		                             symbols));
		assert(new_node != NULL);
		memcpy(a, new_node, sizeof(*a));
		return RESULT_OK;
	}

	if (a->node_type == NODE_DECLARATION && a->u.declare.init != NULL) {
		check(sema_str_literal_as_init(arena, a->u.declare.init));
		if (ctype_is_strlike_ptr(&a->u.declare.var_type)) {
			struct ast *new_node = NULL;
			check(sema_str_literal_hoist(arena,
			                             &a->u.declare.var_type,
			                             a->u.declare.init,
			                             &new_node,
			                             symbols));
			a->u.declare.init->u.init.single = new_node;
		}
		return RESULT_OK;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_lvalue(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool allow_array = false;

	const struct ast *to_check = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		to_check = a->u.op_binary.lhs;
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		to_check = a->u.op_unary.operand;
		break;
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		to_check = a->u.op_unary.operand;
		allow_array = true;
		break;
	default:
		return RESULT_OK;
	}

	if (!is_node_lvalue(to_check)) {
		/*
		 * See related assertions in src/passes/ir.c on u.op_binary.lhs
		 * and NODE_EXPRESSION_VARIABLE_USAGE.
		 */
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE);
	}

	if (!allow_array && ctype_is_array(&to_check->expr_type)) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_ARRAY);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_var_usage(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type == NODE_EXPRESSION_VARIABLE_USAGE &&
	    (a->u.var.stype == SYMBOL_FUNCTION_DECLARATION ||
	     a->u.var.stype == SYMBOL_FUNCTION_DEFINITION)) {
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_INVALID_FUNC,
		                   a->u.var.name.data,
		                   a->u.var.name.sz);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_label_locations(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type != NODE_BLOCK) {
		return RESULT_OK;
	}

	struct string_view name = {0};
	const struct flat *cursor = a->u.block.statements;
	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		switch (cursor->car->node_type) {
		case NODE_LABEL:
			name = cursor->car->u.label.name;
			break;
		case NODE_CASE:
		case NODE_CASE_DEFAULT:
			name.data = "<case>";
			name.sz = strlen(name.data);
			break;
		default:
			continue;
		}
		/*
		 * Check for C23 extensions that the testsuite requires us to
		 * reject with an error, rather than merely warning.
		 */
		if (cursor->cdr == NULL) {
			/* Reject label/case at the very end of a block! */
			return make_result(ERR_SEMA_LABEL_AT_BLOCK_END,
			                   name.data,
			                   name.sz);
		}
		if (cursor->cdr->car->node_type == NODE_DECLARATION) {
			/* Reject label/case followed by a var declaration! */
			return make_result(
				ERR_SEMA_LABEL_FOLLOWED_BY_DECLARATION,
				name.data,
				name.sz);
		}
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_fn_call(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type == NODE_EXPRESSION_FUNCTION_CALL &&
	    a->u.var.stype == SYMBOL_VARIABLE) {
		return make_result(ERR_SEMA_FUNCTION_CALL_UNCALLABLE,
		                   a->u.var.name.data,
		                   a->u.var.name.sz);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_expr_types_initializer_zero_pad(Arena *arena,
                                     const struct ctype *declaration_type,
                                     struct ast *init)
{
	assert(init->node_type == NODE_EXPRESSION_INITIALIZER);

	if (init->u.init.single != NULL &&
	    ctype_is_strlike_array(&init->u.init.single->expr_type)) {
		return RESULT_OK;
	}

	if (!ctype_is_array(declaration_type)) {
		if (init->u.init.single == NULL) {
			check(parse_alloc(arena,
			                  &init->u.init.single,
			                  NODE_CONSTANT));
			init->u.init.single->u.num = 0;
			init->u.init.single->expr_type.t = CTYPE_INT;
			init->u.init.single->expr_type
				.maybe_null_pointer_constant = true;
			init->expr_type.t = CTYPE_INT;
		}
		return RESULT_OK;
	}

	assert(init->u.init.single == NULL);
	assert(declaration_type->referent != NULL);

	struct flat **dst = &init->u.init.multi;

	long long unsigned element_count = 0;
	for (; element_count < declaration_type->sz; ++element_count) {
		if (*dst == NULL) {
			check(flat_alloc(arena, dst));
			check(parse_alloc(arena,
			                  &(**dst).car,
			                  NODE_EXPRESSION_INITIALIZER));
			check(ctype_copy(arena,
			                 declaration_type->referent,
			                 &(**dst).car->expr_type));
		}
		check(sema_expr_types_initializer_zero_pad(
			arena,
			declaration_type->referent,
			(**dst).car));
		dst = &(**dst).cdr;
	}

	return RESULT_OK;
}

/*
 * See sema_implicit_cast_initializer() for related logic.
 */
static WARN_UNUSED result_t
sema_expr_types_initializer(Arena *arena,
                            struct ast_symbol *varname,
                            const struct ctype *declaration_type,
                            struct ast *init)
{
	if (init == NULL) {
		return RESULT_OK;
	}

	assert(init->node_type == NODE_EXPRESSION_INITIALIZER);

	if (init->u.init.single != NULL) {
		/*
		 * For scalar init, copy upward from constant to containing
		 * NODE_EXPRESSION_INITIALIZER.
		 */
		check(ctype_copy(arena,
		                 &init->u.init.single->expr_type,
		                 &init->expr_type));
		return RESULT_OK;
	}

	if (!ctype_is_pointer(declaration_type)) {
		/*
		 * For now, reject compound initializers for scalar variables.
		 * In the future, it may make sense to support the special-case
		 * compound initializer {0} for scalar init.
		 */
		return make_result(ERR_SEMA_INIT_SCALAR_WITH_COMPOUND);
	}

	/*
	 * sema_str_literal() diverts array initializor of char pointer
	 * variable declaration; hence, should not appear here.
	 */
	assert(!ctype_is_strlike_ptr(declaration_type));

	/*
	 * For compound init, copy from LHS array type declaration to RHS
	 * compound init expression.
	 */
	check(ctype_copy(arena, declaration_type, &init->expr_type));

	long long unsigned element_count = 0;

	/*
	 * Recurse into compound initializer elements.
	 */
	for (struct flat *f = init->u.init.multi; f != NULL; f = f->cdr) {
		check(sema_expr_types_initializer(arena,
		                                  varname,
		                                  declaration_type->referent,
		                                  f->car));
		++element_count;
	}

	if (element_count == 0) {
		return make_result(ERR_SEMA_INIT_COMPOUND_EMPTY);
	}
	if (element_count > init->expr_type.sz) {
		return make_result(ERR_SEMA_INIT_COMPOUND_EXCESS_ELEMENTS,
		                   varname->name.data,
		                   varname->name.sz);
	}

	/*
	 * Pad compound initializer with zeros as necessary.
	 */
	check(sema_expr_types_initializer_zero_pad(arena,
	                                           declaration_type,
	                                           init));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_expr_types(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	switch (a->node_type) {
	case NODE_PROGRAM:
	case NODE_FUNCTION:
	case NODE_BLOCK:
	case NODE_IF_ELSE:
	case NODE_LOOP:
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
	case NODE_SWITCH:
	case NODE_CASE:
	case NODE_CASE_DEFAULT:
		break; /* expr_type has no meaning in this context */
	case NODE_DECLARATION:
		check(sema_expr_types_initializer(arena,
		                                  &a->u.declare.identifier,
		                                  &a->u.declare.var_type,
		                                  a->u.declare.init));
		break;
	case NODE_EXPRESSION_INITIALIZER:
		break; /* handled by NODE_DECLARATION case */
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		check(ctype_copy(arena,
		                 &a->u.op_unary.operand->expr_type,
		                 &a->expr_type));
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		if (!ctype_is_pointer(&a->u.op_unary.operand->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_DEREF_INVALID);
		}
		check(ctype_copy(arena,
		                 a->u.op_unary.operand->expr_type.referent,
		                 &a->expr_type));
		break;
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		a->expr_type.t = CTYPE_POINTER_TO;
		assert(a->expr_type.referent == NULL);
		check(ctype_alloc(arena, &a->expr_type.referent));
		check(ctype_copy(arena,
		                 &a->u.op_unary.operand->expr_type,
		                 a->expr_type.referent));
		break;
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		a->expr_type.t = CTYPE_INT; /* effectively cast to bool */
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
		if (a->node_type == NODE_EXPRESSION_BINARY_SUBTRACT &&
		    ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		    ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			check(ctype_copy(arena,
			                 &LIKE_PTRDIFF_T,
			                 &a->expr_type));
		} else {
			check(ctype_copy(
				arena,
				get_common_ctype(
					&a->u.op_binary.lhs->expr_type,
					&a->u.op_binary.rhs->expr_type),
				&a->expr_type));
		}
		break;
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		/*
		 * Shift left/right takes the LHS type, not the common
		 * type of the two sides. The number of shift bits on
		 * the RHS is typically small, but even if that value is
		 * large enough to require a type wider than the LHS,
		 * that should not result in sign extension.
		 *
		 * Variable assignment similarly takes the LHS type,
		 * corresponding to the assigned-to variable.
		 */
		check(ctype_copy(arena,
		                 &a->u.op_binary.lhs->expr_type,
		                 &a->expr_type));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(ctype_copy(
			arena,
			get_common_ctype(&a->u.op_ternary.then_expr->expr_type,
		                         &a->u.op_ternary.else_expr->expr_type),
			&a->expr_type));
		break;
	case NODE_EXPRESSION_CAST:
		check(ctype_copy(arena, &a->u.cast.to_type, &a->expr_type));
		break;
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_CONSTANT:
		break; /* resolve_expr() in parse.c handles leaf nodes */
	case NODE_EXPRESSION_COMPOUND_ASSIGN_ADD:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SUB:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_MUL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_DIV:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_REM:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_AND:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_OR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_XOR:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SL:
	case NODE_EXPRESSION_COMPOUND_ASSIGN_SR:
		assert(0 && "COMPOUND_ASSIGN_* should have been eliminated");
		break;
	case NODE_EXPRESSION_SUBSCRIPT:
		assert(0 && "SUBSCRIPT should have been eliminated");
		break;
	case NODE_CONSTANT_STR:
		assert(0 && "CONSTANT_STR should have been eliminated");
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_double(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool valid = true;

	switch (a->node_type) {
	case NODE_SWITCH:
		if (ctype_is_floating_point(&a->u.switch_.control->expr_type)) {
			valid = false;
		}
		break;
	case NODE_CASE:
		if (ctype_is_floating_point(&a->u.case_.constant->expr_type)) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		if (ctype_is_floating_point(&a->expr_type)) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		if (ctype_is_floating_point(&a->u.op_binary.lhs->expr_type) ||
		    ctype_is_floating_point(&a->u.op_binary.rhs->expr_type)) {
			valid = false;
		}
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		if ((ctype_is_floating_point(&a->u.op_binary.lhs->expr_type) &&
		     ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) ||
		    (ctype_is_floating_point(&a->u.op_binary.rhs->expr_type) &&
		     ctype_is_pointer(&a->u.op_binary.lhs->expr_type))) {
			valid = false;
		}

		break;
	case NODE_EXPRESSION_CAST:
		if ((ctype_is_floating_point(&a->u.cast.to_type) &&
		     ctype_is_pointer(&a->u.cast.expr->expr_type)) ||
		    (ctype_is_floating_point(&a->u.cast.expr->expr_type) &&
		     ctype_is_pointer(&a->u.cast.to_type))) {
			valid = false;
		}
		break;
	default:
		break;
	}

	if (!valid) {
		return make_result(ERR_SEMA_OPERAND_DOUBLE_INVALID);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_pointer_cmp_impl(const struct ctype *lhs,
                      const struct ctype *rhs,
                      bool ish)
{
	if (ctype_is_equal(lhs, rhs)) {
		/* given equality, nothing more to check */
	} else if (ctype_is_strlike_array(lhs) && ctype_is_strlike_array(rhs)) {
		if (lhs->sz < rhs->sz - 1) { /* -1 for (maybe) NUL terminator */
			return make_result(ERR_SEMA_OPERAND_CHAR_ARRAY_SIZE);
		} /* else: RHS string may be shorter than LHS capacity */
	} else if (ctype_is_pointer(lhs) && ctype_is_pointer(rhs)) {
		return make_result(ERR_SEMA_OPERAND_POINTER_CONFLICT);
	} else if (ctype_is_pointer(lhs) && (!ish || !ctype_nullptr_ish(rhs))) {
		return make_result(ERR_SEMA_OPERAND_POINTER_LHS_VS_NOT_RHS);
	} else if (ctype_is_pointer(rhs) && (!ish || !ctype_nullptr_ish(lhs))) {
		return make_result(ERR_SEMA_OPERAND_POINTER_RHS_VS_NOT_LHS);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_pointer_cmp(const struct ctype *lhs, const struct ctype *rhs)
{
	check(sema_pointer_cmp_impl(lhs, rhs, true));
	return RESULT_OK;
}

struct sema_pointer_state {
	Arena *arena;
	struct ctype expected_return_type;
};

static WARN_UNUSED result_t
sema_pointer(struct ast *a, void *userdata)
{
	struct sema_pointer_state *state = userdata;
	Arena *arena = state->arena;

	switch (a->node_type) {
	case NODE_FUNCTION:
		if (a->u.function.block != NULL) {
			check(ctype_copy(arena,
			                 &a->u.function.return_type,
			                 &state->expected_return_type));
		}
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		check(sema_pointer_cmp(&state->expected_return_type,
		                       &a->u.op_unary.operand->expr_type));
		break;
	case NODE_DECLARATION:
		if (a->u.declare.init != NULL) {
			check(sema_pointer_cmp_impl(
				&a->u.declare.var_type,
				&a->u.declare.init->expr_type,
				/* do not allow zero->nullptr for array init */
				!ctype_is_array(&a->u.declare.var_type)));
		}
		break;
	case NODE_SWITCH:
		if (ctype_is_pointer(&a->u.switch_.control->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_CASE:
		if (ctype_is_pointer(&a->u.case_.constant->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
		if (ctype_is_pointer(&a->u.op_unary.operand->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_EXPRESSION_BINARY_ADD:
		if (!ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		    !ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			/* no pointer types involved; nothing more to check */
		} else if (ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		           ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_ADD_POINTER_BOTH);
		}
		break;
	case NODE_EXPRESSION_BINARY_SUBTRACT:
		if (ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		    ctype_is_integer(&a->u.op_binary.rhs->expr_type)) {
			assert(ctype_is_equal(&a->expr_type,
			                      &a->u.op_binary.lhs->expr_type));
		} else {
			check(sema_pointer_cmp_impl(
				&a->u.op_binary.lhs->expr_type,
				&a->u.op_binary.rhs->expr_type,
				false));
		}
		break;
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
		check(sema_pointer_cmp(&a->u.op_binary.lhs->expr_type,
		                       &a->u.op_binary.rhs->expr_type));
		break;
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		check(sema_pointer_cmp_impl(&a->u.op_binary.lhs->expr_type,
		                            &a->u.op_binary.rhs->expr_type,
		                            false));
		break;
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
		if (ctype_is_pointer(&a->u.op_binary.lhs->expr_type) ||
		    ctype_is_pointer(&a->u.op_binary.rhs->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_POINTER_INVALID);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		check(sema_pointer_cmp(&a->u.op_binary.lhs->expr_type,
		                       &a->u.op_binary.rhs->expr_type));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(sema_pointer_cmp(&a->u.op_ternary.then_expr->expr_type,
		                       &a->u.op_ternary.else_expr->expr_type));
		break;
	default:
		break;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
cast_if(Arena *arena, const struct ctype *cast_to, struct ast **a)
{
	if (*a == NULL || ctype_is_equal(&(**a).expr_type, cast_to)) {
		return RESULT_OK;
	}
	struct ast *cast_wrap = NULL;
	check(parse_alloc(arena, &cast_wrap, NODE_EXPRESSION_CAST));
	check(ctype_copy(arena, cast_to, &cast_wrap->expr_type));
	check(ctype_copy(arena, cast_to, &cast_wrap->u.cast.to_type));
	cast_wrap->u.cast.expr = *a;
	*a = cast_wrap;
	return RESULT_OK;
}

/*
 * See sema_expr_types_initializer() for related logic.
 */
static WARN_UNUSED result_t
sema_implicit_cast_initializer(Arena *arena,
                               const struct ctype *expected_type,
                               struct ast **init)
{
	if (*init == NULL) {
		return RESULT_OK;
	}

	assert((**init).node_type == NODE_EXPRESSION_INITIALIZER);

	if ((**init).u.init.single != NULL) {
		check(sema_pointer_cmp(expected_type, &(**init).expr_type));
		check(cast_if(arena, expected_type, init));
		return RESULT_OK;
	}

	/* single XOR multi */
	assert((**init).u.init.multi != NULL);
	/* array (multi) initializor must correspond to array type */
	assert(ctype_is_array(expected_type));
	assert(ctype_is_array(&(**init).expr_type));

	for (struct flat *f = (**init).u.init.multi; f != NULL; f = f->cdr) {
		check(sema_implicit_cast_initializer(arena,
		                                     expected_type->referent,
		                                     &f->car));
	}
	return RESULT_OK;
}

struct sema_implicit_cast_state {
	Arena *arena;
	struct ctype expected_return_type;
};

static WARN_UNUSED result_t
sema_implicit_cast(struct ast *a, void *userdata)
{
	struct sema_implicit_cast_state *state = userdata;
	Arena *arena = state->arena;
	const struct ctype *common = NULL;

	switch (a->node_type) {
	case NODE_FUNCTION:
		check(ctype_copy(arena,
		                 &a->u.function.return_type,
		                 &state->expected_return_type));
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
		check(cast_if(arena,
		              &state->expected_return_type,
		              &a->u.op_unary.operand));
		break;
	case NODE_DECLARATION:
		check(sema_implicit_cast_initializer(arena,
		                                     &a->u.declare.var_type,
		                                     &a->u.declare.init));
		break;
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_BITWISE_AND:
	case NODE_EXPRESSION_BITWISE_OR:
	case NODE_EXPRESSION_BITWISE_XOR:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
		common = get_common_ctype(&a->u.op_binary.lhs->expr_type,
		                          &a->u.op_binary.rhs->expr_type);
		if ((a->node_type == NODE_EXPRESSION_BINARY_ADD ||
		     a->node_type == NODE_EXPRESSION_BINARY_SUBTRACT) &&
		    ((ctype_is_pointer(&a->u.op_binary.lhs->expr_type) &&
		      ctype_is_integer(&a->u.op_binary.rhs->expr_type)) ||
		     (ctype_is_integer(&a->u.op_binary.lhs->expr_type) &&
		      ctype_is_pointer(&a->u.op_binary.rhs->expr_type)))) {
			check(cast_if(
				arena,
				&LIKE_PTRDIFF_T,
				ctype_is_integer(&a->u.op_binary.lhs->expr_type)
					? &a->u.op_binary.lhs
					: &a->u.op_binary.rhs));
		} else {
			check(cast_if(arena, common, &a->u.op_binary.lhs));
			check(cast_if(arena, common, &a->u.op_binary.rhs));
		}
		break;
	case NODE_EXPRESSION_BITWISE_SHIFT_LEFT:
	case NODE_EXPRESSION_BITWISE_SHIFT_RIGHT:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		check(cast_if(arena,
		              &a->u.op_binary.lhs->expr_type,
		              &a->u.op_binary.rhs));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		common =
			get_common_ctype(&a->u.op_ternary.then_expr->expr_type,
		                         &a->u.op_ternary.else_expr->expr_type);
		check(cast_if(arena, common, &a->u.op_ternary.then_expr));
		check(cast_if(arena, common, &a->u.op_ternary.else_expr));
		break;
	default:
		break;
	}
	return RESULT_OK;
}

struct sema_symbol_state {
	Arena *arena;
	struct flat *ast_program_globals;
	struct symbol *function_symbols;
	struct symbol *variable_symbols;
};

enum symbol_declaration_scope {
	SCOPE_BLOCK,
	SCOPE_FILE,
};

struct sema_symbol_auxiliary {
	long long int n_args;
	struct ctype *p_types; /* array of size n_args */
	enum symbol_declaration_scope dscope;
};

static WARN_UNUSED result_t
sema_alloc_auxiliary(Arena *arena, void **out_as_void_pp)
{
	struct sema_symbol_auxiliary **out =
		(struct sema_symbol_auxiliary **)out_as_void_pp;
	assert(*out == NULL);
	*out = arena_alloc(arena, sizeof(**out));
	check_if(*out == NULL, ERR_SYMBOL_ALLOC);
	memset(*out, 0, sizeof(**out));
	return RESULT_OK;
}

static WARN_UNUSED struct sema_symbol_auxiliary *
sema_get_auxiliary(struct symbol *s)
{
	assert(s != NULL && s->auxiliary != NULL);
	return s->auxiliary;
}

static WARN_UNUSED result_t
sema_fn_param_names(struct ast_parameter *params)
{
	struct ast_parameter *dup = NULL;

	/* O(n^2) search over <params> for duplicates */
	FOREACH_FUNCTION_PARAMETER (i, params) {
		struct string_view *iname = &i->symbol.name;
		FOREACH_FUNCTION_PARAMETER (j, i + 1) {
			struct string_view *jname = &j->symbol.name;
			if (iname->sz == jname->sz &&
			    0 == strncmp(iname->data, jname->data, iname->sz)) {
				dup = i;
				break;
			}
		}
		if (dup != NULL) {
			break;
		}
	}

	if (dup != NULL) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_PARAM_DUPLICATE,
		                   dup->symbol.name.data,
		                   dup->symbol.name.sz);
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_fn_decl_collect(Arena *arena,
                     const struct ast *a,
                     struct string_view *fname,
                     struct ctype *return_type,
                     long long int *n_args,
                     struct ctype **param_types,
                     bool *is_def,
                     enum symbol_linkage *linkage,
                     bool *has_specifier_static)
{
	assert(a->node_type == NODE_FUNCTION);
	memcpy(fname, &a->u.function.identifier.name, sizeof(*fname));

	check(ctype_copy(arena, &a->u.function.return_type, return_type));
	if (ctype_is_array(return_type)) {
		return make_result(ERR_SEMA_FUNCTION_RETURN_TYPE_ARRAY,
		                   fname->data,
		                   fname->sz);
	}

	long long int count = 0;
	FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
		++count;
	}

	*n_args = count;
	*param_types = arena_alloc(arena, sizeof(**param_types) * count);

	count = 0;
	FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
		check(ctype_copy(arena,
		                 &cur->parameter_type,
		                 &(*param_types)[count]));
		ctype_array_decay_to_pointer(&(*param_types)[count]);
		++count;
	}

	check(sema_fn_param_names(a->u.function.params));

	*is_def = (a->u.function.block != NULL);
	*linkage = (a->u.function.specifier != SPECIFIER_STATIC)
	                   ? SYMBOL_LINKAGE_EXTERNAL
	                   : SYMBOL_LINKAGE_INTERNAL;
	*has_specifier_static = (a->u.function.specifier == SPECIFIER_STATIC);
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_fn_call_collect(Arena *arena,
                     const struct ast *a,
                     struct string_view *fname,
                     long long int *n_args,
                     struct ctype **param_types)
{
	assert(a->node_type == NODE_EXPRESSION_FUNCTION_CALL);
	memcpy(fname, &a->u.call.identifier.name, sizeof(*fname));

	long long int count = 0;
	for (struct flat *z = a->u.call.args; z != NULL; z = z->cdr) {
		++count;
	}

	*n_args = count;
	*param_types = arena_alloc(arena, sizeof(**param_types) * count);

	count = 0;
	for (struct flat *z = a->u.call.args; z != NULL; z = z->cdr) {
		check(ctype_copy(arena,
		                 &z->car->expr_type,
		                 &(*param_types)[count++]));
	}
	return RESULT_OK;
}

static WARN_UNUSED bool
ast_contains(const struct flat *haystack, const struct ast *needle)
{
	for (; haystack != NULL; haystack = haystack->cdr) {
		if (needle == haystack->car) {
			return true;
		}
	}
	return false;
}

static WARN_UNUSED result_t
sema_fn_signature(struct ast *a, void *userdata)
{
	struct sema_symbol_state *state = userdata;
	struct string_view fname = {0};
	struct ctype return_type = {0};
	long long int n_args = 0;
	struct ctype *p_types = NULL;
	bool is_def = false;
	bool is_def_or_decl = false;
	enum symbol_linkage linkage = SYMBOL_LINKAGE_EXTERNAL;
	bool has_specifier_static = false;

	switch (a->node_type) {
	case NODE_PROGRAM:
		state->ast_program_globals = a->u.program.globals;
		return RESULT_OK;
	case NODE_FUNCTION:
		is_def_or_decl = true;
		check(sema_fn_decl_collect(state->arena,
		                           a,
		                           &fname,
		                           &return_type,
		                           &n_args,
		                           &p_types,
		                           &is_def,
		                           &linkage,
		                           &has_specifier_static));
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(sema_fn_call_collect(state->arena,
		                           a,
		                           &fname,
		                           &n_args,
		                           &p_types));
		break;
	default:
		return RESULT_OK;
	}

	if (is_def) {
		assert(state->ast_program_globals != NULL);
		bool allow_def = ast_contains(state->ast_program_globals, a);
		if (!allow_def) {
			return make_result(ERR_SEMA_FUNCTION_DEFINITION_NESTED,
			                   fname.data,
			                   fname.sz);
		}
	} else if (is_def_or_decl && has_specifier_static) {
		assert(state->ast_program_globals != NULL);
		bool allow_decl = ast_contains(state->ast_program_globals, a);
		if (!allow_decl) {
			return make_result(
				ERR_SEMA_FUNCTION_LINKAGE_BLOCK_SCOPE,
				fname.data,
				fname.sz);
		}
	}

	struct symbol **s = &state->function_symbols;
	struct symbol *dup = symbols_get_anywhere(*s, &fname);
	if (dup == NULL) {
		assert(is_def_or_decl);
		check(symbols_prepend(state->arena,
		                      s,
		                      &fname,
		                      is_def ? SYMBOL_FUNCTION_DEFINITION
		                             : SYMBOL_FUNCTION_DECLARATION,
		                      &return_type));
		check(sema_alloc_auxiliary(state->arena, &(**s).auxiliary));
		sema_get_auxiliary(*s)->n_args = n_args;
		sema_get_auxiliary(*s)->p_types = p_types;
		(**s).linkage.linkage = linkage;
	} else if (is_def && dup->stype == SYMBOL_FUNCTION_DEFINITION) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_DUPLICATE,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (is_def_or_decl && has_specifier_static &&
	           is_external(dup->linkage.linkage)) {
		return make_result(ERR_SEMA_FUNCTION_LINKAGE_CONFLICT,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (is_def_or_decl &&
	           !ctype_is_equal(&return_type, &dup->c89type)) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_CONFLICT,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (n_args != sema_get_auxiliary(dup)->n_args) {
		return make_result(
			is_def_or_decl
				? ERR_SEMA_FUNCTION_DEFINITION_CONFLICT
				: ERR_SEMA_FUNCTION_CALL_WRONG_NUMBER_OF_ARGS,
			dup->name.data,
			dup->name.sz);
	}

	if (dup == NULL) {
		return RESULT_OK;
	}

	bool p_types_match = true;
	for (long long int i = 0; i < n_args; ++i) {
		const struct ctype *to_check = NULL;
		if (is_def_or_decl) {
			/*
			 * Require exact parameter type match on redeclaration,
			 * definition of preceding declaration, etc.
			 */
			to_check = &p_types[i];
		} else {
			const struct ctype *lhs =
				&sema_get_auxiliary(dup)->p_types[i];
			const struct ctype *rhs = &p_types[i];
			/*
			 * On function call, try to widen or narrow argument
			 * expression type to declared parameter type.
			 */
			check(sema_pointer_cmp(lhs, rhs));
			to_check = get_common_ctype(lhs, rhs);
		}
		if (!ctype_is_equal(to_check,
		                    &sema_get_auxiliary(dup)->p_types[i])) {
			p_types_match = false;
			break;
		}
	}

	if (is_def_or_decl && !p_types_match) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_CONFLICT,
		                   dup->name.data,
		                   dup->name.sz);
	}

	if (!is_def_or_decl) {
		long long int i = 0;
		struct flat *actual = a->u.call.args;
		struct ctype *expected = sema_get_auxiliary(dup)->p_types;
		while (actual != NULL && i < sema_get_auxiliary(dup)->n_args) {
			if (!ctype_is_equal(&actual->car->expr_type,
			                    &expected[i])) {
				check(cast_if(state->arena,
				              &expected[i],
				              &actual->car));
			}
			++i;
			actual = actual->cdr;
		}
	}

	return RESULT_OK;
}

static WARN_UNUSED struct symbol *
symbols_get_scoped(struct symbol *head, /* maybe NULL */
                   const struct string_view *name,
                   enum symbol_declaration_scope dscope)
{
	while (true) {
		struct symbol *candidate = symbols_get_anywhere(head, name);
		if (candidate == NULL) {
			break;
		}
		if (sema_get_auxiliary(candidate)->dscope == dscope) {
			return candidate;
		}
		head = candidate->next;
	}
	return NULL;
}

static WARN_UNUSED result_t
sema_declare_file_scope(struct ast *a,
                        struct sema_symbol_state *state,
                        struct symbol **dup,
                        struct symbol_linkage_state *linkage_state)
{
	assert(a->node_type == NODE_DECLARATION);
	const struct string_view *varname = &a->u.declare.identifier.name;

	linkage_state->linkage = (a->u.declare.specifier != SPECIFIER_STATIC)
	                                 ? SYMBOL_LINKAGE_EXTERNAL
	                                 : SYMBOL_LINKAGE_INTERNAL;

	if (a->u.declare.init != NULL) {
		if (is_node_constant(a->u.declare.init)) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			check(map_numeric_type(state->arena,
			                       a->u.declare.init,
			                       &a->u.declare.var_type,
			                       &linkage_state->initializer));
			/*
			 * Remove init expression from AST. We will initialize
			 * this value via symbol table processing, not AST.
			 */
			a->u.declare.init = NULL; /* arena handles dealloc */
		} else {
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_INIT,
				varname->data,
				varname->sz);
		}
	} else if (a->u.declare.specifier == SPECIFIER_EXTERN) {
		linkage_state->initial = INITIAL_VALUE_NO_INITIALIZER;
	} else {
		linkage_state->initial = INITIAL_VALUE_TENTATIVE;
	}

	struct symbol *function_symbol_collision =
		symbols_get_anywhere(state->function_symbols, varname);

	if (function_symbol_collision != NULL) {
		assert(function_symbol_collision->stype != SYMBOL_VARIABLE);
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_MISMATCH,
			varname->data,
			varname->sz);
	}

	*dup = symbols_get_scoped(state->variable_symbols, varname, SCOPE_FILE);

	if (*dup == NULL) {
		/* no earlier declaration to cross-reference linkage */
	} else if (a->u.declare.specifier == SPECIFIER_EXTERN) {
		linkage_state->linkage = (**dup).linkage.linkage;
	} else if (linkage_state->linkage != (**dup).linkage.linkage) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_LINKAGE,
			varname->data,
			varname->sz);
	}

	if (*dup == NULL) {
		/* no earlier declaration to cross-reference initializer */
	} else if ((**dup).linkage.initial == INITIAL_VALUE_CONSTANT &&
	           linkage_state->initial == INITIAL_VALUE_CONSTANT) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_DUPLICATE,
			varname->data,
			varname->sz);
	} else {
		if ((**dup).linkage.initial == INITIAL_VALUE_CONSTANT) {
			linkage_state->initializer =
				(**dup).linkage.initializer;
		}
		// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
		linkage_state->initial =
			MAX(linkage_state->initial, (**dup).linkage.initial);
		// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare_block_scope(struct ast *a,
                         struct sema_symbol_state *state,
                         struct symbol **dup,
                         struct symbol_linkage_state *linkage_state)
{
	assert(a->node_type == NODE_DECLARATION);
	const struct string_view *varname = &a->u.declare.identifier.name;
	struct symbol *function_symbol_collision = NULL;

	switch (a->u.declare.specifier) {
	case SPECIFIER_EXTERN:
		if (a->u.declare.init != NULL) {
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_EXTERN_INIT,
				varname->data,
				varname->sz);
		}

		function_symbol_collision =
			symbols_get_anywhere(state->function_symbols, varname);
		if (function_symbol_collision != NULL) {
			assert(function_symbol_collision->stype !=
			       SYMBOL_VARIABLE);
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_EXTERN_MISMATCH,
				varname->data,
				varname->sz);
		}

		*dup = symbols_get_scoped(state->variable_symbols,
		                          varname,
		                          SCOPE_FILE);
		if (*dup != NULL) {
			/*
			 * In this case, extern causes this variable to take on
			 * the same linkage as the matching identifier that is
			 * already in scope. This may even be a variable with
			 * internal linkage via earlier use of keyword static!
			 */
			linkage_state->linkage = (**dup).linkage.linkage;
			linkage_state->initial = (**dup).linkage.initial;
			linkage_state->initializer =
				(**dup).linkage.initializer;
		} else {
			linkage_state->linkage = SYMBOL_LINKAGE_EXTERNAL;
			linkage_state->initial = INITIAL_VALUE_NO_INITIALIZER;
		}
		break;
	case SPECIFIER_STATIC:
		if (a->u.declare.init != NULL &&
		    !is_node_constant(a->u.declare.init)) {
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_STATIC_INIT,
				varname->data,
				varname->sz);
		}

		if (a->u.declare.init == NULL) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			check(constant_set_zero(state->arena,
			                        &a->u.declare.var_type,
			                        &linkage_state->initializer));
		} else if (is_node_constant(a->u.declare.init)) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			check(map_numeric_type(state->arena,
			                       a->u.declare.init,
			                       &a->u.declare.var_type,
			                       &linkage_state->initializer));
			/*
			 * Remove init expression from AST. We will initialize
			 * this value via symbol table processing, not AST.
			 */
			a->u.declare.init = NULL; /* arena handles dealloc */
		}

		linkage_state->linkage = SYMBOL_LINKAGE_INTERNAL;
		assert(linkage_state->initial == INITIAL_VALUE_CONSTANT);
		break;
	case SPECIFIER_NONE:
		/*
		 * Omit variables with no linkage from the symbol table. Future
		 * IR and codegen passes only care about variables bound for
		 * the data and BSS sections of the resulting binary.
		 */
		return RESULT_OK;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare_apply(struct ast *a,
                   struct sema_symbol_state *state,
                   enum symbol_declaration_scope dscope)
{
	assert(a->node_type == NODE_DECLARATION);
	struct symbol *dup = NULL;
	struct symbol_linkage_state linkage_state = {0};

	switch (dscope) {
	case SCOPE_BLOCK:
		check(sema_declare_block_scope(a, state, &dup, &linkage_state));
		break;
	case SCOPE_FILE:
		check(sema_declare_file_scope(a, state, &dup, &linkage_state));
		break;
	}

	if (dup == NULL) {
		check(symbols_prepend(state->arena,
		                      &state->variable_symbols,
		                      &a->u.declare.identifier.name,
		                      SYMBOL_VARIABLE,
		                      &a->u.declare.var_type));
		dup = state->variable_symbols;
		check(sema_alloc_auxiliary(state->arena, &dup->auxiliary));
		if (linkage_state.linkage == SYMBOL_LINKAGE_EXTERNAL) {
			sema_get_auxiliary(dup)->dscope = SCOPE_FILE;
		} else {
			sema_get_auxiliary(dup)->dscope = dscope;
		}
	} else if (!ctype_is_equal(&a->u.declare.var_type, &dup->c89type)) {
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_TYPE_CONFLICT,
		                   dup->name.data,
		                   dup->name.sz);
	}
	dup->unique = a->u.declare.identifier.unique; /* reuse unique ID */
	dup->linkage.linkage = linkage_state.linkage;
	dup->linkage.initial = linkage_state.initial;
	dup->linkage.initializer = linkage_state.initializer;
	a->u.declare.identifier.ltype = linkage_state.linkage;
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_propagate_linkage_from_declare_to_usage(struct ast *a,
                                             struct sema_symbol_state *state)
{
	assert(a->node_type == NODE_EXPRESSION_VARIABLE_USAGE);
	/*
	 * Lookup below causes overall O(n*m) runtime, where:
	 *
	 *   n = number of variable references spread across AST nodes
	 *   m = number of variables with linkage
	 */
	struct symbol *v =
		symbols_get_unique(state->variable_symbols, a->u.var.unique);
	if (v != NULL) {
		a->u.var.ltype = v->linkage.linkage;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_get_linkage_from_declarations(struct ast *a, void *userdata)
{
	if (a->node_type != NODE_DECLARATION &&
	    a->node_type != NODE_EXPRESSION_VARIABLE_USAGE) {
		return RESULT_OK;
	}

	struct sema_symbol_state *state = userdata;
	assert(state->ast_program_globals); /* from sema_fn_signature() */

	const bool file_scope = ast_contains(state->ast_program_globals, a);
	if (file_scope) {
		check(sema_declare_apply(a, state, SCOPE_FILE));
	} else if (a->node_type == NODE_DECLARATION) {
		check(sema_declare_apply(a, state, SCOPE_BLOCK));
	} else {
		check(sema_propagate_linkage_from_declare_to_usage(a, state));
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_mangle_internal_linkage_names(struct ast *a, void *userdata)
{
	struct sema_symbol_state *state = userdata;

	if (a->node_type == NODE_DECLARATION) {
		/*
		 * Lookup below causes O(n^2) runtime, where:
		 *
		 *   n = number of variables with linkage
		 */
		struct symbol *v =
			symbols_get_unique(state->variable_symbols,
		                           a->u.declare.identifier.unique);
		if (v != NULL && is_internal(v->linkage.linkage)) {
			/*
			 * For symbols with internal linkage, update the symbol
			 * table entry with a mangled name, unique within this
			 * translation unit.
			 */
			if (!is_mangled(v)) {
				check(mangle_name(state->arena, v));
			}
			/*
			 * Update this declaration in the AST to use the
			 * newly-mangled symbol name.
			 */
			a->u.declare.identifier.name = v->name;
		}
	} else if (a->node_type == NODE_EXPRESSION_VARIABLE_USAGE) {
		/*
		 * Lookup below causes overall O(n*m) runtime, where:
		 *
		 *   n = number of variable references spread across AST nodes
		 *   m = number of variables with linkage
		 */
		struct symbol *v = symbols_get_unique(state->variable_symbols,
		                                      a->u.var.unique);
		if (v != NULL && is_internal(v->linkage.linkage)) {
			/*
			 * Update this variable usage in the AST to use the
			 * newly-mangled symbol name.
			 */
			a->u.var.name = v->name;
		}
	}

	return RESULT_OK;
}

result_t
sema_typecheck(Arena *arena,
               struct ast *a,
               long long int *label_generator,
               struct symbol_table *s)
{
	struct sema_ops ops = {0};

	debug("Expanding compound assignment statements");
	ops.node_enter = sema_compound_assignment;
	check(sema_walk(a, &ops, arena));

	debug("Expanding array subscript expressions");
	ops.node_enter = sema_subscript;
	check(sema_walk(a, &ops, arena));

	debug("Expanding string literals and hoisting if necessary");
	ops.node_enter = sema_str_literal;
	{
		struct sema_str_literal_state str_state = {0};
		str_state.arena = arena;
		str_state.string_literal_symbols = s->string_literals;
		check(sema_walk(a, &ops, &str_state));
		s->string_literals = str_state.string_literal_symbols;
	}

	debug("Checking variable usage");
	ops.node_enter = sema_var_usage;
	check(sema_walk(a, &ops, NULL));

	debug("Checking label locations");
	ops.node_enter = sema_label_locations;
	check(sema_walk(a, &ops, NULL));

	debug("Checking function calls");
	ops.node_enter = sema_fn_call;
	check(sema_walk(a, &ops, NULL));

	debug("Propagating expression types");
	ops.node_enter = NULL;
	ops.node_exit = sema_expr_types;
	check(sema_walk(a, &ops, arena));
	ops.node_exit = NULL;

	debug("Checking for invalid lvalues");
	ops.node_enter = sema_lvalue;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid double usage");
	ops.node_enter = sema_double;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid pointer usage");
	ops.node_enter = sema_pointer;
	{
		struct sema_pointer_state pointer_state = {0};
		pointer_state.arena = arena;
		check(sema_walk(a, &ops, &pointer_state));
	}

	debug("Labeling loops, loop breaks, and continues");
	check(sema_label_loops(arena, a, label_generator));

	debug("Labeling goto statements and labels");
	check(sema_label_gotos(arena, a, label_generator));

	debug("Inserting cast expressions");
	ops.node_enter = sema_implicit_cast;
	{
		struct sema_implicit_cast_state cast_state = {0};
		cast_state.arena = arena;
		check(sema_walk(a, &ops, &cast_state));
	}

	struct sema_symbol_state state = {0};
	state.arena = arena;
	state.function_symbols = s->functions;
	state.variable_symbols = s->variables;

	debug("Checking function signatures");
	ops.node_enter = sema_fn_signature;
	check(sema_walk(a, &ops, &state));

	debug("Determining linkage from variable declarations");
	ops.node_enter = sema_get_linkage_from_declarations;
	check(sema_walk(a, &ops, &state));

	debug("Unique-ifying variables with internal linkage");
	ops.node_enter = sema_mangle_internal_linkage_names;
	check(sema_walk(a, &ops, &state));

	s->functions = state.function_symbols;
	s->variables = state.variable_symbols;
	return RESULT_OK;
}
