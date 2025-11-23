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
	bool has_linkage = true;
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
		has_linkage = (a->u.function.specifier != SPECIFIER_STATIC);
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
		                             : SYMBOL_FUNCTION_DECLARATION,
		                      n_args));
		(**s).linkage.has_linkage = has_linkage;
	} else if (is_def && dup->stype == SYMBOL_FUNCTION_DEFINITION) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_DUPLICATE,
		                   dup->name.data,
		                   dup->name.sz);
	} else if (is_def_or_decl && dup->linkage.has_linkage && is_static) {
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
sema_declare_finalize(struct sema_symbol_state *state,
                      const struct string_view *varname,
                      struct symbol *dup,
                      bool has_linkage,
                      enum initializer_state initial,
                      long long int as_constant)
{
	if (dup == NULL) {
		check(symbols_prepend(state->arena,
		                      &state->variable_symbols,
		                      varname,
		                      SYMBOL_VARIABLE,
		                      0));
		dup = state->variable_symbols;
	}
	dup->linkage.has_linkage = has_linkage;
	dup->linkage.initial = initial;
	dup->linkage.as_constant = as_constant;
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare_file_scope(struct ast *a, struct sema_symbol_state *state)
{
	assert(a->node_type == NODE_DECLARATION);
	const struct string_view *varname = &a->u.declare.identifier.name;
	bool has_linkage = (a->u.declare.specifier != SPECIFIER_STATIC);
	enum initializer_state initial = INITIAL_VALUE_NO_INITIALIZER;
	long long int as_constant = 0;

	if (a->u.declare.init != NULL) {
		if (a->u.declare.init->node_type == NODE_CONSTANT_INT) {
			initial = INITIAL_VALUE_CONSTANT;
			as_constant = a->u.declare.init->u.num;
		} else {
			return make_result(
				ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_INIT,
				varname->data,
				varname->sz);
		}
	} else if (a->u.declare.specifier == SPECIFIER_EXTERN) {
		initial = INITIAL_VALUE_NO_INITIALIZER;
	} else {
		initial = INITIAL_VALUE_TENTATIVE;
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

	struct symbol *dup =
		symbols_get(state->variable_symbols, varname, false);

	if (dup == NULL) {
		/* no earlier declaration to cross-reference linkage */
	} else if (a->u.declare.specifier == SPECIFIER_EXTERN) {
		has_linkage = dup->linkage.has_linkage;
	} else if (has_linkage != dup->linkage.has_linkage) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_LINKAGE,
			varname->data,
			varname->sz);
	}

	if (dup == NULL) {
		/* no earlier declaration to cross-reference initializer */
	} else if (dup->linkage.initial == INITIAL_VALUE_CONSTANT &&
	           initial == INITIAL_VALUE_CONSTANT) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_DUPLICATE,
			varname->data,
			varname->sz);
	} else {
		if (dup->linkage.initial == INITIAL_VALUE_CONSTANT) {
			as_constant = dup->linkage.as_constant;
		}
		// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
		initial = MAX(initial, dup->linkage.initial);
	}

	check(sema_declare_finalize(state,
	                            varname,
	                            dup,
	                            has_linkage,
	                            initial,
	                            as_constant));
	if (has_linkage) {
		a->u.declare.identifier.has_linkage = true;
		a->u.declare.identifier.unique = UNIQUE_NOT_NECESSARY;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare_block_scope(struct ast *a, struct sema_symbol_state *state)
{
	assert(a->node_type == NODE_DECLARATION);
	const struct string_view *varname = &a->u.declare.identifier.name;
	bool has_linkage = false;
	unsigned initial = INITIAL_VALUE_NO_INITIALIZER;
	long long int as_constant = 0;
	struct symbol *function_symbol_collision = NULL;
	struct symbol *dup = NULL;

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

		dup = symbols_get(state->variable_symbols, varname, false);
		if (dup != NULL) {
			/*
			 * In this case, extern causes this variable to take on
			 * the same linkage as the matching identifier that is
			 * already in scope. This may even be a variable with
			 * internal linkage via earlier use of keyword static!
			 */
			has_linkage = dup->linkage.has_linkage;
			initial = dup->linkage.initial;
		} else {
			has_linkage = true;
			initial = INITIAL_VALUE_NO_INITIALIZER;
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
			initial = INITIAL_VALUE_CONSTANT;
			as_constant = 0;
		} else if (a->u.declare.init->node_type == NODE_CONSTANT_INT) {
			initial = INITIAL_VALUE_CONSTANT;
			as_constant = a->u.declare.init->u.num;
		}

		has_linkage = false;
		assert(initial == INITIAL_VALUE_CONSTANT);
		break;
	case SPECIFIER_NONE:
		/*
		 * Omit variables with no linkage from the symbol table. Future
		 * IR and codegen passes only care about variables bound for
		 * the data and BSS sections of the resulting binary.
		 */
		return RESULT_OK;
	}

	check(sema_declare_finalize(state,
	                            varname,
	                            NULL,
	                            has_linkage,
	                            initial,
	                            as_constant));
	if (has_linkage) {
		a->u.declare.identifier.has_linkage = true;
		a->u.declare.identifier.unique = UNIQUE_NOT_NECESSARY;
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_declare(struct ast *a, void *userdata)
{
	if (a->node_type != NODE_DECLARATION) {
		return RESULT_OK;
	}

	struct sema_symbol_state *state = userdata;
	assert(state->ast_program_globals); /* from sema_fn_signature() */

	const bool file_scope = ast_contains(state->ast_program_globals, a);
	if (file_scope) {
		check(sema_declare_file_scope(a, state));
	} else {
		check(sema_declare_block_scope(a, state));
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_linkage(struct ast *a, void *userdata)
{
	struct sema_symbol_state *state = userdata;

	struct ast_symbol *sym = NULL;
	switch (a->node_type) {
	case NODE_DECLARATION:
		sym = &a->u.declare.identifier;
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		sym = &a->u.op_binary.lhs->u.var;
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		sym = &a->u.var;
		break;
	default:
		return RESULT_OK;
	}

	/*
	 * Lookup below causes overall O(n*m) runtime, where:
	 *
	 *   n = number of variables with linkage
	 *   m = number of variable references spread across AST nodes
	 */
	for (struct symbol *cursor = state->variable_symbols; cursor != NULL;
	     cursor = cursor->next) {
		if (cursor->unique == sym->unique) {
			assert(cursor->stype == SYMBOL_VARIABLE);
			assert(cursor->name.sz == sym->name.sz);
			info("strncmp cursor=%.*s (%lld) sym=%.*s (%lld)",
			     (int)cursor->name.sz,
			     cursor->name.data,
			     cursor->unique,
			     (int)sym->name.sz,
			     sym->name.data,
			     sym->unique);
			// TODO: assert below fires on static_variables_in_expressions.c because the unique IDs created by sema.c do not match the original unique IDs generated by resolve_decl()!
			assert(0 == strncmp(cursor->name.data,
			                    sym->name.data,
			                    cursor->name.sz));
			/*
			 * Q: Why do we set has_linkage=true below, even if the
			 * match at cursor has cursor->has_linkage == false?
			 *
			 * A: Consumers in ir.c want to know if this symbol has
			 * any linkage, internal or external. This corresponds
			 * to presence in state->variable_symbols overall, not
			 * the matching node's has_linkage value in particular.
			 */
			sym->has_linkage = true;
			break;
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

	debug("Updating variable references"); /* based on sema_declare() */
	check(sema_walk(a, sema_linkage, &state));

	s->functions = state.function_symbols;
	s->variables = state.variable_symbols;
	return RESULT_OK;
}
