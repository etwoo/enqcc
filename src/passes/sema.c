#include "passes.h"
#include "passes/parse.h"
#include "passes/symbol.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

static WARN_UNUSED result_t
sema_walk(Arena *arena,
          struct ast *a,
          result_t (*f)(Arena *arena, struct ast *a, void *userdata),
          void *u)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
		check(f(arena, a, u));
		check(sema_walk(arena, a->u.program.globals, f, u));
		break;
	case NODE_FUNCTION:
		check(f(arena, a, u));
		if (a->u.function.block != NULL) {
			check(sema_walk(arena, a->u.function.block, f, u));
		}
		if (a->u.function.next != NULL) {
			check(sema_walk(arena, a->u.function.next, f, u));
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			check(sema_walk(arena, a->u.block.item, f, u));
			if (a->u.block.next != NULL) {
				check(sema_walk(arena, a->u.block.next, f, u));
			}
		}
		break;
	case NODE_IF_ELSE:
		check(sema_walk(arena, a->u.if_.then_clause, f, u));
		if (a->u.if_.else_clause != NULL) {
			check(sema_walk(arena, a->u.if_.else_clause, f, u));
		}
		break;
	case NODE_LOOP:
		check(f(arena, a, u));
		check(sema_walk(arena, a->u.loop.precond, f, u));
		check(sema_walk(arena, a->u.loop.body, f, u));
		check(sema_walk(arena, a->u.loop.postcond, f, u));
		break;
	case NODE_BREAK:
		check(f(arena, a, u));
		break;
	case NODE_CONTINUE:
		check(f(arena, a, u));
		break;
	case NODE_DECLARATION:
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_NULL:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_BINARY_ADD:
	case NODE_EXPRESSION_BINARY_SUBTRACT:
	case NODE_EXPRESSION_BINARY_MULTIPLY:
	case NODE_EXPRESSION_BINARY_DIVIDE:
	case NODE_EXPRESSION_BINARY_REMAINDER:
	case NODE_EXPRESSION_LOGICAL_AND:
	case NODE_EXPRESSION_LOGICAL_OR:
	case NODE_EXPRESSION_COMPARE_EQUAL:
	case NODE_EXPRESSION_COMPARE_NOT_EQUAL:
	case NODE_EXPRESSION_COMPARE_LESS_THAN:
	case NODE_EXPRESSION_COMPARE_LESS_THAN_EQ:
	case NODE_EXPRESSION_COMPARE_MORE_THAN:
	case NODE_EXPRESSION_COMPARE_MORE_THAN_EQ:
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
	case NODE_EXPRESSION_FUNCTION_CALL:
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
	case NODE_CONSTANT_INT:
		break;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_loop_id(Arena *arena MAYBE_UNUSED, struct ast *a, void *userdata)
{
	long long int *id = userdata;
	switch (a->node_type) {
	case NODE_LOOP:
		a->u.loop.label_start = ++*id;
		a->u.loop.label_continue = ++*id; /* see NODE_CONTINUE case */
		a->u.loop.label_end = ++*id;      /* see NODE_BREAK case */
		break;
	case NODE_BREAK:
		if (*id <= 0) {
			return make_result(ERR_SEMA_BREAK_OUTSIDE);
		}
		a->u.num = *id; /* most recent label_end */
		break;
	case NODE_CONTINUE:
		if (*id <= 0) {
			return make_result(ERR_SEMA_CONTINUE_OUTSIDE);
		}
		a->u.num = *id - 1; /* most recent label_continue */
		break;
	default:
		break;
	}

	return RESULT_OK;
}

result_t
sema_label_loops(struct ast *a, long long int *generator)
{
	debug("Labeling loops, loop breaks, and continues");
	*generator = 0;
	check(sema_walk(NULL, a, sema_loop_id, generator));
	return RESULT_OK;
}

struct sema_fn_signature_state {
	struct ast *ast_program_globals;
	struct symbol *symbols;
};

static WARN_UNUSED bool
ast_contains(const struct ast *haystack, const struct ast *needle)
{
	assert(haystack->node_type == NODE_FUNCTION);
	assert(needle->node_type == NODE_FUNCTION);

	while (haystack != NULL) {
		if (needle == haystack) {
			return true;
		}
		haystack = haystack->u.function.next;
	}

	return false;
}

static WARN_UNUSED result_t
sema_fn_signature(Arena *arena, struct ast *a, void *userdata)
{
	struct sema_fn_signature_state *state = userdata;

	switch (a->node_type) {
	case NODE_FUNCTION:
		break;
	case NODE_PROGRAM:
		state->ast_program_globals = a->u.program.globals;
		return RESULT_OK;
	default:
		return RESULT_OK;
	}

	const struct string_view *fname = &a->u.function.identifier.name;
	const bool is_def = (a->u.function.block != NULL);
	if (is_def) {
		assert(state->ast_program_globals != NULL);
		bool allow_def = ast_contains(state->ast_program_globals, a);
		if (!allow_def) {
			return make_result(ERR_SEMA_NESTED_FUNCTION_DEFINITION,
			                   fname->data,
			                   fname->sz);
		}
	}

	long long int n_args = 0;
	FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
		++n_args;
	}

	struct symbol **s = &state->symbols;
	struct symbol *dup = symbols_get(*s, fname, false);
	if (dup == NULL) {
		check(symbols_prepend(arena,
		                      s,
		                      fname,
		                      is_def ? SYMBOL_FUNCTION_DEFINITION
		                             : SYMBOL_FUNCTION_DECLARATION,
		                      LINKAGE_EXTERNAL,
		                      n_args));
	} else if (is_def && dup->stype == SYMBOL_FUNCTION_DEFINITION) {
		return make_result(ERR_SEMA_DUPLICATE_FUNCTION_DEFINITION,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (n_args != dup->n_args) {
		return make_result(ERR_SEMA_CONFLICTING_FUNCTION_DEFINITION,
		                   fname->data,
		                   fname->sz);
	}

	return RESULT_OK;
}

result_t
sema_typecheck(Arena *arena, struct ast *a)
{
	debug("Checking function signatures");
	struct sema_fn_signature_state state = {0};
	check(sema_walk(arena, a, sema_fn_signature, &state));
	return RESULT_OK;
}
