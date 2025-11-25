#include "passes.h"
#include "passes/parse.h"
#include "passes/symbol.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <sys/param.h> /* for MAX() */

static WARN_UNUSED result_t
sema_walk(struct ast *a, result_t (*f)(struct ast *a, void *userdata), void *u)
{
	switch (a->node_type) {
	case NODE_PROGRAM:
		check(f(a, u));
		check(sema_walk(a->u.program.globals, f, u));
		break;
	case NODE_FUNCTION:
		check(f(a, u));
		if (a->u.function.block != NULL) {
			check(sema_walk(a->u.function.block, f, u));
		}
		if (a->u.function.next != NULL) {
			check(sema_walk(a->u.function.next, f, u));
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			check(sema_walk(a->u.block.item, f, u));
			if (a->u.block.next != NULL) {
				check(sema_walk(a->u.block.next, f, u));
			}
		}
		break;
	case NODE_DECLARATION:
		check(f(a, u));
		if (a->u.declare.init != NULL) {
			check(sema_walk(a->u.declare.init, f, u));
		}
		if (a->u.declare.next != NULL) {
			check(sema_walk(a->u.declare.next, f, u));
		}
		break;
	case NODE_IF_ELSE:
		check(sema_walk(a->u.if_.condition, f, u));
		check(sema_walk(a->u.if_.then_clause, f, u));
		if (a->u.if_.else_clause != NULL) {
			check(sema_walk(a->u.if_.else_clause, f, u));
		}
		break;
	case NODE_LOOP:
		check(f(a, u));
		check(sema_walk(a->u.loop.precond, f, u));
		check(sema_walk(a->u.loop.body, f, u));
		check(sema_walk(a->u.loop.incr, f, u));
		check(sema_walk(a->u.loop.postcond, f, u));
		break;
	case NODE_BREAK:
		check(f(a, u));
		break;
	case NODE_CONTINUE:
		check(f(a, u));
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
		check(sema_walk(a->u.op_unary.operand, f, u));
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		check(f(a, u));
		__attribute__((fallthrough));
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
		check(sema_walk(a->u.op_binary.lhs, f, u));
		check(sema_walk(a->u.op_binary.rhs, f, u));
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		check(f(a, u));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(sema_walk(a->u.op_binary.lhs, f, u));
		check(sema_walk(a->u.op_ternary.condition, f, u));
		check(sema_walk(a->u.op_ternary.then_expr, f, u));
		if (a->u.op_ternary.else_expr != NULL) {
			check(sema_walk(a->u.op_ternary.else_expr, f, u));
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		check(f(a, u));
		if (a->u.call.arguments != NULL) {
			check(sema_walk(a->u.call.arguments, f, u));
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
		check(sema_walk(a->u.call_args.expr, f, u));
		if (a->u.call_args.next != NULL) {
			check(sema_walk(a->u.call_args.next, f, u));
		}
		break;
	case NODE_EXPRESSION_NULL:
	case NODE_CONSTANT_INT:
		break;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_loop_id(struct ast *a, void *userdata)
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
	check(sema_walk(a, sema_loop_id, generator));
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_lvalue(struct ast *a, void *userdata MAYBE_UNUSED)
{
	if (a->node_type == NODE_EXPRESSION_VARIABLE_ASSIGNMENT &&
	    a->u.op_binary.lhs->node_type != NODE_EXPRESSION_VARIABLE_USAGE) {
		/*
		 * See related assertions in src/passes/ir.c on u.op_binary.lhs
		 * and NODE_EXPRESSION_VARIABLE_USAGE.
		 */
		return make_result(ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE);
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

struct sema_symbol_state {
	Arena *arena;
	struct ast *ast_program_globals;
	struct symbol *function_symbols;
	struct symbol *variable_symbols;
};

static WARN_UNUSED result_t
sema_fn_param_names(struct ast_symbol *params)
{
	struct ast_symbol *dup = NULL;

	/* O(n^2) search over <params> for duplicates */
	FOREACH_FUNCTION_PARAMETER (i, params) {
		struct string_view *iname = &i->name;
		FOREACH_FUNCTION_PARAMETER (j, i + 1) {
			struct string_view *jname = &j->name;
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
		                   dup->name.data,
		                   dup->name.sz);
	}
	return RESULT_OK;
}

static WARN_UNUSED bool
ast_contains(const struct ast *haystack, const struct ast *needle)
{
	while (haystack != NULL) {
		if (needle == haystack) {
			return true;
		}
		switch (haystack->node_type) {
		case NODE_FUNCTION:
			haystack = haystack->u.function.next;
			break;
		case NODE_DECLARATION:
			haystack = haystack->u.declare.next;
			break;
		default:
			assert(0); /* logic error in caller */
			break;
		}
	}

	return false;
}

static WARN_UNUSED result_t
sema_fn_signature(struct ast *a, void *userdata)
{
	struct sema_symbol_state *state = userdata;
	const struct string_view *fname = NULL;
	long long int n_args = 0;
	bool is_def = false;
	bool is_def_or_decl = false;
	enum symbol_linkage linkage = SYMBOL_LINKAGE_EXTERNAL;
	bool is_static = false;

	switch (a->node_type) {
	case NODE_PROGRAM:
		state->ast_program_globals = a->u.program.globals;
		return RESULT_OK;
	case NODE_FUNCTION:
		fname = &a->u.function.identifier.name;
		FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
			++n_args;
		}
		check(sema_fn_param_names(a->u.function.params));
		is_def = (a->u.function.block != NULL);
		is_def_or_decl = true;
		linkage = (a->u.function.specifier != SPECIFIER_STATIC)
		                  ? SYMBOL_LINKAGE_EXTERNAL
		                  : SYMBOL_LINKAGE_INTERNAL;
		is_static = (a->u.function.specifier == SPECIFIER_STATIC);
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		fname = &a->u.call.identifier.name;
		for (struct ast *arguments = a->u.call.arguments;
		     arguments != NULL;
		     arguments = arguments->u.call_args.next) {
			++n_args;
		}
		break;
	default:
		return RESULT_OK;
	}

	if (is_def) {
		assert(state->ast_program_globals != NULL);
		bool allow_def = ast_contains(state->ast_program_globals, a);
		if (!allow_def) {
			return make_result(ERR_SEMA_FUNCTION_DEFINITION_NESTED,
			                   fname->data,
			                   fname->sz);
		}
	} else if (is_def_or_decl && is_static) {
		assert(state->ast_program_globals != NULL);
		bool allow_decl = ast_contains(state->ast_program_globals, a);
		if (!allow_decl) {
			return make_result(
				ERR_SEMA_FUNCTION_LINKAGE_BLOCK_SCOPE,
				fname->data,
				fname->sz);
		}
	}

	struct symbol **s = &state->function_symbols;
	struct symbol *dup = symbols_get(*s, fname, false);
	if (dup == NULL) {
		assert(is_def_or_decl);
		check(symbols_prepend(state->arena,
		                      s,
		                      fname,
		                      is_def ? SYMBOL_FUNCTION_DEFINITION
		                             : SYMBOL_FUNCTION_DECLARATION));
		(**s).n_args = n_args;
		(**s).linkage.linkage = linkage;
	} else if (is_def && dup->stype == SYMBOL_FUNCTION_DEFINITION) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_DUPLICATE,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (is_def_or_decl && is_external(dup->linkage.linkage) &&
	           is_static) {
		return make_result(ERR_SEMA_FUNCTION_LINKAGE_CONFLICT,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (n_args != dup->n_args) {
		return make_result(
			is_def_or_decl
				? ERR_SEMA_FUNCTION_DEFINITION_CONFLICT
				: ERR_SEMA_FUNCTION_CALL_WRONG_NUMBER_OF_ARGS,
			fname->data,
			fname->sz);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare_file_scope(struct ast *a,
                        struct sema_symbol_state *state,
                        struct symbol **dup,
                        struct symbol_linkage_state *linkage_state)
{
	assert(a->node_type == NODE_DECLARATION);
	const struct string_view *varname = &a->u.declare.identifier.name;

	if (a->u.declare.init != NULL) {
		if (a->u.declare.init->node_type == NODE_CONSTANT_INT) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			linkage_state->as_constant = a->u.declare.init->u.num;
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
		symbols_get(state->function_symbols, varname, false);

	if (function_symbol_collision != NULL) {
		assert(function_symbol_collision->stype != SYMBOL_VARIABLE);
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_MISMATCH,
			varname->data,
			varname->sz);
	}

	*dup = symbols_get(state->variable_symbols, varname, false);
	linkage_state->linkage = (a->u.declare.specifier != SPECIFIER_STATIC)
	                                 ? SYMBOL_LINKAGE_EXTERNAL
	                                 : SYMBOL_LINKAGE_INTERNAL;

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
			linkage_state->as_constant =
				(**dup).linkage.as_constant;
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
			symbols_get(state->function_symbols, varname, false);
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
			linkage_state->as_constant =
				(**dup).linkage.as_constant;
		} else {
			linkage_state->linkage = SYMBOL_LINKAGE_EXTERNAL;
			linkage_state->initial = INITIAL_VALUE_NO_INITIALIZER;
		}
		break;
	case SPECIFIER_STATIC:
		if (a->u.declare.init != NULL &&
		    a->u.declare.init->node_type != NODE_CONSTANT_INT) {
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_STATIC_INIT,
				varname->data,
				varname->sz);
		}

		if (a->u.declare.init == NULL) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			linkage_state->as_constant = 0;
		} else if (a->u.declare.init->node_type == NODE_CONSTANT_INT) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			linkage_state->as_constant = a->u.declare.init->u.num;
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
                   enum symbol_scope scope)
{
	assert(a->node_type == NODE_DECLARATION);
	struct symbol *dup = NULL;
	struct symbol_linkage_state linkage_state = {0};

	switch (scope) {
	case SCOPE_BLOCK:
		check(sema_declare_block_scope(a, state, &dup, &linkage_state));
		break;
	case SCOPE_FILE:
		check(sema_declare_file_scope(a, state, &dup, &linkage_state));
		break;
	case SCOPE_UNSPECIFIED:
		assert(0); /* logic error in caller */
		break;
	}

	if (dup == NULL) {
		check(symbols_prepend_scoped(state->arena,
		                             &state->variable_symbols,
		                             &a->u.declare.identifier.name,
		                             scope));
		dup = state->variable_symbols;
	}
	dup->unique = a->u.declare.identifier.unique; /* reuse unique ID */
	dup->linkage.linkage = linkage_state.linkage;
	dup->linkage.initial = linkage_state.initial;
	dup->linkage.as_constant = linkage_state.as_constant;
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
	struct symbol *v = state->variable_symbols;
	for (; v != NULL; v = v->next) {
		assert(v->stype == SYMBOL_VARIABLE);
		if (v->unique == a->u.var.unique) {
			a->u.var.ltype = v->linkage.linkage;
			break;
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare(struct ast *a, void *userdata)
{
	if (a->node_type != NODE_DECLARATION &&
	    a->node_type != NODE_EXPRESSION_VARIABLE_USAGE) {
		return RESULT_OK;
	}

	struct sema_symbol_state *state = userdata;
	assert(state->ast_program_globals); /* from sema_fn_signature() */

	const bool file_scope = ast_contains(state->ast_program_globals, a);
	if (file_scope) {
		assert(a->node_type == NODE_DECLARATION);
		check(sema_declare_apply(a, state, SCOPE_FILE));
	} else if (a->node_type == NODE_DECLARATION) {
		check(sema_declare_apply(a, state, SCOPE_BLOCK));
	} else {
		check(sema_propagate_linkage_from_declare_to_usage(a, state));
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_mangle(Arena *arena, struct ast_symbol *asym)
{
	char *mangled_str = arena_sprintf(arena,
	                                  "%.*s.%lld",
	                                  (int)asym->name.sz,
	                                  asym->name.data,
	                                  asym->unique);
	check_if(mangled_str == NULL, ERR_SEMA_ALLOC);
	asym->name.data = mangled_str;
	asym->name.sz = strlen(mangled_str);
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_internal_linkage(struct ast *a, void *userdata)
{
	struct sema_symbol_state *state = userdata;

	if (a->node_type == NODE_DECLARATION) {
		/*
		 * Lookup below causes O(n^2) runtime, where:
		 *
		 *   n = number of variables with linkage
		 */
		struct symbol *v = state->variable_symbols;
		for (; v != NULL; v = v->next) {
			assert(v->stype == SYMBOL_VARIABLE);
			if (v->unique == a->u.declare.identifier.unique) {
				if (is_external(v->linkage.linkage)) {
					break;
				}
				check(sema_mangle(state->arena,
				                  &a->u.declare.identifier));
				v->name = a->u.declare.identifier.name;
				break;
			}
		}
	} else if (a->node_type == NODE_EXPRESSION_VARIABLE_USAGE) {
		/*
		 * Lookup below causes overall O(n*m) runtime, where:
		 *
		 *   n = number of variable references spread across AST nodes
		 *   m = number of variables with linkage
		 */
		struct symbol *v = state->variable_symbols;
		for (; v != NULL; v = v->next) {
			assert(v->stype == SYMBOL_VARIABLE);
			if (v->unique == a->u.var.unique) {
				if (is_external(v->linkage.linkage)) {
					break;
				}
				/*
				 * For symbols with internal linkage, redirect
				 * any variable usage to a mangled name, unique
				 * within this translation unit.
				 */
				a->u.var.name = v->name;
				break;
			}
		}
	}

	return RESULT_OK;
}

result_t
sema_typecheck(Arena *arena, struct ast *a, struct symbol_table *s)
{
	debug("Checking lvalues");
	check(sema_walk(a, sema_lvalue, NULL));

	debug("Checking variable usage");
	check(sema_walk(a, sema_var_usage, NULL));

	debug("Checking function calls");
	check(sema_walk(a, sema_fn_call, NULL));

	struct sema_symbol_state state = {0};
	state.arena = arena;
	state.function_symbols = s->functions;
	state.variable_symbols = s->variables;

	debug("Checking function signatures");
	check(sema_walk(a, sema_fn_signature, &state));

	debug("Checking variable declarations");
	check(sema_walk(a, sema_declare, &state));

	debug("Unique-ifying variables with internal linkage");
	check(sema_walk(a, sema_internal_linkage, &state));

	s->functions = state.function_symbols;
	s->variables = state.variable_symbols;
	return RESULT_OK;
}
