#include "passes/sema/string.h"

#include "passes.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/sema/constant.h"
#include "passes/sema/walk.h"

#include <assert.h>
#include <stdint.h> /* for SIZE_MAX */
#include <string.h> /* for memcpy() */

static WARN_UNUSED result_t
sema_str_literal_expand(Arena *arena,
                        const struct string_view *lit,
                        const size_t capacity,
                        struct flat **dst)
{
	assert(dst != NULL);
	assert(*dst == NULL);

	for (size_t i = 0; i <= lit->sz; ++i) {
		struct flat *new_flat = NULL;
		check(flat_alloc(arena, &new_flat));
		check(parse_alloc(arena,
		                  &new_flat->car,
		                  NODE_EXPRESSION_INITIALIZER));
		new_flat->car->expr_type.t = CTYPE_INT;

		struct ast *new_init = NULL;
		check(parse_alloc(arena, &new_init, NODE_CONSTANT));
		new_init->expr_type.t = CTYPE_INT;

		if (i < lit->sz) {
			new_init->u.num = (int)lit->data[i];
		} else if (i < capacity) {
			new_init->u.num = 0; /* implicit NUL terminator */
		} else {
			break; /* lacks capacity; abandon new_flat/new_init */
		}

		new_flat->car->u.init.single = new_init;
		*dst = new_flat;

		dst = &(**dst).cdr;
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_str_literal_hoist(Arena *arena,
                       const struct ctype *var_type,
                       struct ast *init,
                       struct symbol **s,
                       struct type_table *types,
                       struct ast **new_node)
{
	struct ctype array_type = {0};
	check(ctype_copy(arena, var_type, &array_type));
	array_type.t = CTYPE_ARRAY_OF;
	assert(ctype_is_strlike_array(&array_type));

	/* translate u.init.multi into equivalent constant_initializer */
	assert(init->u.init.multi != NULL);
	struct constant_initializer initializer = {0};
	check(map_numeric_type(arena, init, &array_type, types, &initializer));

	array_type.sz = initializer.count;

	/* add constant_initializer to symbol table */
	struct string_view dummy_name = {0};
	check(symbols_prepend(arena,
	                      s,
	                      &dummy_name,
	                      SYMBOL_STRING_LITERAL,
	                      &array_type));
	if ((**s).unique == 0) {
		(**s).unique = 4096; /* avoid zero as string literal ID */
	}
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
	struct symbol **symbols;
	struct type_table *types;
};

static WARN_UNUSED result_t
visit_literal(struct ast **ast_handle,
              const struct ctype *declaration_type,
              void *userdata)
{
	struct sema_str_literal_state *state = userdata;
	Arena *arena = state->arena;
	struct symbol **sym = state->symbols;
	struct type_table *typ = state->types;

	bool expanded = false;

	struct ast *init = *ast_handle;
	assert(init->node_type == NODE_EXPRESSION_INITIALIZER);
	if (init->u.init.single != NULL &&
	    init->u.init.single->node_type == NODE_CONSTANT_STR) {
		if (!ctype_is_strlike_ptr(declaration_type) &&
		    !ctype_is_strlike_array(declaration_type)) {
			return make_result(ERR_SEMA_INIT_STR_LITERAL_INVALID);
		}
		const struct string_view deepcopy = init->u.init.single->u.str;
		init->u.init.single = NULL;

		assert(ctype_is_pointer(declaration_type));
		assert(declaration_type->referent != NULL);

		check(sema_str_literal_expand(arena,
		                              &deepcopy,
		                              ctype_is_array(declaration_type)
		                                      ? declaration_type->sz
		                                      : SIZE_MAX,
		                              &init->u.init.multi));
		expanded = true;
	}

	if (ctype_is_strlike_ptr(declaration_type) &&
	    init->u.init.multi != NULL) {
		if (!expanded) {
			return make_result(ERR_SEMA_INIT_SCALAR_WITH_COMPOUND);
		}
		struct ast *new_node = NULL;
		check(sema_str_literal_hoist(arena,
		                             declaration_type,
		                             init,
		                             sym,
		                             typ,
		                             &new_node));
		init->u.init.single = new_node;
		assert(init->u.init.multi == NULL);
	}

	if (init->u.init.multi == NULL) {
		return RESULT_OK;
	}

	if (!ctype_is_aggregate(declaration_type)) {
		return make_result(ERR_SEMA_INIT_SCALAR_WITH_COMPOUND);
	}

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_str_literal(struct ast *a, void *userdata)
{
	struct sema_str_literal_state *state = userdata;
	Arena *arena = state->arena;

	if (a->node_type == NODE_CONSTANT_STR) {
		struct ast *fake_init = NULL;
		check(parse_alloc(arena,
		                  &fake_init,
		                  NODE_EXPRESSION_INITIALIZER));
		check(sema_str_literal_expand(arena,
		                              &a->u.str,
		                              SIZE_MAX,
		                              &fake_init->u.init.multi));
		struct ast *new_node = NULL;
		check(sema_str_literal_hoist(arena,
		                             &a->expr_type,
		                             fake_init,
		                             state->symbols,
		                             state->types,
		                             &new_node));
		assert(new_node != NULL);
		memcpy(a, new_node, sizeof(*a));
	} else if (a->node_type == NODE_DECLARATION &&
	           a->u.declare.init != NULL) {
		check(sema_walk_initializer(&a->u.declare.init,
		                            &a->u.declare.var_type,
		                            state->types,
		                            visit_literal,
		                            state));
	}

	return RESULT_OK;
}

result_t
sema_typecheck_strlit(Arena *arena,
                      struct ast *a,
                      struct symbol_table *s,
                      struct type_table *types)
{
	struct sema_ops ops = {
		.node_enter = sema_str_literal,
	};
	struct sema_str_literal_state state = {
		.arena = arena,
		.symbols = &s->string_literals,
		.types = types,
	};
	check(sema_walk(a, &ops, &state));
	s->string_literals = *state.symbols;
	return RESULT_OK;
}
