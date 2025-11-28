#include "passes.h"
#include "passes/parse.h"
#include "passes/symbol.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <sys/param.h> /* for MAX() */

struct sema_ops {
	result_t (*node_enter)(struct ast *a, void *userdata);
	result_t (*node_exit)(struct ast *a, void *userdata);
};

static WARN_UNUSED result_t
sema_walk(struct ast *a, const struct sema_ops *ops, void *u)
{
	if (ops->node_enter != NULL) {
		check(ops->node_enter(a, u));
	}

	switch (a->node_type) {
	case NODE_PROGRAM:
		check(sema_walk(a->u.program.globals, ops, u));
		break;
	case NODE_FUNCTION:
		if (a->u.function.block != NULL) {
			check(sema_walk(a->u.function.block, ops, u));
		}
		if (a->u.function.next != NULL) {
			check(sema_walk(a->u.function.next, ops, u));
		}
		break;
	case NODE_BLOCK:
		if (a->u.block.item != NULL) {
			check(sema_walk(a->u.block.item, ops, u));
			if (a->u.block.next != NULL) {
				check(sema_walk(a->u.block.next, ops, u));
			}
		}
		break;
	case NODE_DECLARATION:
		if (a->u.declare.init != NULL) {
			check(sema_walk(a->u.declare.init, ops, u));
		}
		if (a->u.declare.next != NULL) {
			check(sema_walk(a->u.declare.next, ops, u));
		}
		break;
	case NODE_IF_ELSE:
		check(sema_walk(a->u.if_.condition, ops, u));
		check(sema_walk(a->u.if_.then_clause, ops, u));
		if (a->u.if_.else_clause != NULL) {
			check(sema_walk(a->u.if_.else_clause, ops, u));
		}
		break;
	case NODE_LOOP:
		check(sema_walk(a->u.loop.precond, ops, u));
		check(sema_walk(a->u.loop.body, ops, u));
		check(sema_walk(a->u.loop.incr, ops, u));
		check(sema_walk(a->u.loop.postcond, ops, u));
		break;
	case NODE_BREAK:
	case NODE_CONTINUE:
	case NODE_GOTO:
	case NODE_LABEL:
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
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
		check(sema_walk(a->u.op_binary.lhs, ops, u));
		check(sema_walk(a->u.op_binary.rhs, ops, u));
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		check(sema_walk(a->u.op_binary.lhs, ops, u));
		check(sema_walk(a->u.op_ternary.condition, ops, u));
		check(sema_walk(a->u.op_ternary.then_expr, ops, u));
		if (a->u.op_ternary.else_expr != NULL) {
			check(sema_walk(a->u.op_ternary.else_expr, ops, u));
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		if (a->u.call.arguments != NULL) {
			check(sema_walk(a->u.call.arguments, ops, u));
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL_ARGUMENTS:
		check(sema_walk(a->u.call_args.expr, ops, u));
		if (a->u.call_args.next != NULL) {
			check(sema_walk(a->u.call_args.next, ops, u));
		}
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
	case NODE_EXPRESSION_NULL:
	case NODE_CONSTANT_INT:
		break;
	}

	if (ops->node_exit != NULL) {
		check(ops->node_exit(a, u));
	}
	return RESULT_OK;
}

struct sema_label_loops_state {
	long long int id;
	size_t loop_depth;
};

static WARN_UNUSED result_t
sema_enter_loop_id(struct ast *a, void *userdata)
{
	struct sema_label_loops_state *state = userdata;

	switch (a->node_type) {
	case NODE_LOOP:
		state->loop_depth++;
		a->u.loop.label_start = ++state->id;
		a->u.loop.label_continue = ++state->id; /* see NODE_CONTINUE */
		a->u.loop.label_end = ++state->id;      /* see NODE_BREAK */
		break;
	case NODE_BREAK:
		if (state->loop_depth == 0) {
			return make_result(ERR_SEMA_BREAK_OUTSIDE);
		}
		a->u.num = state->id; /* most recent label_end */
		break;
	case NODE_CONTINUE:
		info("loop_depth=%zu at continue", state->loop_depth);
		if (state->loop_depth == 0) {
			return make_result(ERR_SEMA_CONTINUE_OUTSIDE);
		}
		a->u.num = state->id - 1; /* most recent label_continue */
		break;
	default:
		break;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_exit_loop_id(struct ast *a, void *userdata)
{
	if (a->node_type != NODE_LOOP) {
		return RESULT_OK;
	}

	struct sema_label_loops_state *state = userdata;
	assert(state->loop_depth > 0);
	state->loop_depth--;
	return RESULT_OK;
}

result_t
sema_label_loops(struct ast *a, long long int *generator)
{
	debug("Labeling loops, loop breaks, and continues");
	struct sema_ops ops = {
		.node_enter = sema_enter_loop_id,
		.node_exit = sema_exit_loop_id,
	};
	struct sema_label_loops_state state = {
		.id = *generator,
		.loop_depth = 0,
	};
	check(sema_walk(a, &ops, &state));
	*generator = state.id;
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
	debug("Labeling goto statements and labels");
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
sema_lvalue(struct ast *a, void *userdata MAYBE_UNUSED)
{
	struct ast *to_check = NULL;
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
	default:
		return RESULT_OK;
	}

	while (to_check->node_type == NODE_EXPRESSION_PAREN_ENCLOSED) {
		to_check = to_check->u.op_unary.operand;
	}

	if (to_check->node_type != NODE_EXPRESSION_VARIABLE_USAGE) {
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

enum symbol_declaration_scope {
	SCOPE_BLOCK,
	SCOPE_FILE,
};

struct sema_symbol_auxiliary {
	long long int n_args;
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
	bool has_specifier_static = false;

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
		has_specifier_static =
			(a->u.function.specifier == SPECIFIER_STATIC);
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
	} else if (is_def_or_decl && has_specifier_static) {
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
	struct symbol *dup = symbols_get_anywhere(*s, fname);
	if (dup == NULL) {
		assert(is_def_or_decl);
		check(symbols_prepend(state->arena,
		                      s,
		                      fname,
		                      is_def ? SYMBOL_FUNCTION_DEFINITION
		                             : SYMBOL_FUNCTION_DECLARATION));
		check(sema_alloc_auxiliary(state->arena, &(**s).auxiliary));
		sema_get_auxiliary(*s)->n_args = n_args;
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
	} else if (n_args != sema_get_auxiliary(dup)->n_args) {
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

	linkage_state->linkage = (a->u.declare.specifier != SPECIFIER_STATIC)
	                                 ? SYMBOL_LINKAGE_EXTERNAL
	                                 : SYMBOL_LINKAGE_INTERNAL;

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
		symbols_get_anywhere(state->function_symbols, varname);

	if (function_symbol_collision != NULL) {
		assert(function_symbol_collision->stype != SYMBOL_VARIABLE);
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_FILESCOPE_MISMATCH,
			varname->data,
			varname->sz);
	}

	*dup = symbols_get_anywhere(state->variable_symbols, varname);

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
		                      SYMBOL_VARIABLE));
		dup = state->variable_symbols;
		check(sema_alloc_auxiliary(state->arena, &dup->auxiliary));
		sema_get_auxiliary(dup)->dscope = dscope;
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
sema_typecheck(Arena *arena, struct ast *a, struct symbol_table *s)
{
	struct sema_ops ops = {0};

	debug("Checking lvalues");
	ops.node_enter = sema_lvalue;
	check(sema_walk(a, &ops, NULL));

	debug("Checking variable usage");
	ops.node_enter = sema_var_usage;
	check(sema_walk(a, &ops, NULL));

	debug("Checking function calls");
	ops.node_enter = sema_fn_call;
	check(sema_walk(a, &ops, NULL));

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
