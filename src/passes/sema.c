#include "lang/symbol.h"
#include "passes.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/sema/constant.h"
#include "passes/sema/expression.h"
#include "passes/sema/flow.h"
#include "passes/sema/implicit_cast.h"
#include "passes/sema/pointer.h"
#include "passes/sema/string.h"
#include "passes/sema/walk.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <stdint.h>    /* for SIZE_MAX */
#include <stdlib.h>    /* for strtoll() */
#include <sys/param.h> /* for MAX() */

static WARN_UNUSED bool
is_node_lvalue(const struct ast *a)
{
	while (a->node_type == NODE_EXPRESSION_PAREN_ENCLOSED) {
		a = a->u.op_unary.operand;
	}
	return a->node_type == NODE_EXPRESSION_VARIABLE_USAGE ||
	       (a->node_type == NODE_EXPRESSION_UNARY_DEREFERENCE &&
	        !ctype_is_void_ptr(&a->u.op_unary.operand->expr_type)) ||
	       (a->node_type == NODE_EXPRESSION_STRUCT_MEMBER &&
	        is_node_lvalue(a->u.member_access.lhs));
}

static WARN_UNUSED struct ast **
unpack_cast(struct ast **a)
{
	while ((**a).node_type == NODE_EXPRESSION_CAST) {
		/* unpack nodes inserted by sema_implicit_cast() */
		a = &(**a).u.cast.expr;
	}
	return a;
}

static WARN_UNUSED bool
is_node_constant(struct ast *a)
{
	{
		struct ast **tmp = unpack_cast(&a);
		a = *tmp;
	}

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
	/* XXX: copying u.op_binary does *not* deep-copy all descendants */

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
	new_node->u.op_binary = a->u.op_binary;

	a->node_type = NODE_EXPRESSION_UNARY_DEREFERENCE;
	a->u.op_unary.operand = new_node;

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_struct_pointer(struct ast *a, void *userdata)
{
	Arena *arena = userdata;

	if (a->node_type != NODE_EXPRESSION_STRUCT_POINTER) {
		return RESULT_OK;
	}

	struct ast *new_node = arena_alloc(arena, sizeof(*new_node));
	check_if(new_node == NULL, ERR_SEMA_ALLOC);
	new_node->node_type = NODE_EXPRESSION_UNARY_DEREFERENCE;
	new_node->u.op_unary.operand = a->u.member_access.lhs;

	a->node_type = NODE_EXPRESSION_STRUCT_MEMBER;
	a->u.member_access.lhs = new_node;

	return RESULT_OK;
}

static WARN_UNUSED result_t
sema_lvalue(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool allow_array = false;
	bool allow_struct = false;

	const struct ast *to_check = NULL;
	switch (a->node_type) {
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		to_check = a->u.op_binary.lhs;
		allow_array = false;
		allow_struct = true;
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		to_check = a->u.op_unary.operand;
		allow_array = false;
		allow_struct = false;
		break;
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
		to_check = a->u.op_unary.operand;
		allow_array = true;
		allow_struct = true;
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

	if (!allow_struct && ctype_is_struct(&to_check->expr_type)) {
		return make_result(
			ERR_SEMA_VARIABLE_DECLARATION_BAD_LVALUE_STRUCT);
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
		if (cursor->cdr->car->node_type == NODE_DECLARATION ||
		    cursor->cdr->car->node_type == NODE_STRUCT) {
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

static WARN_UNUSED bool
is_scalar(const struct ctype *c)
{
	/*
	 * Allow CTYPE_ARRAY_OF, despite it not truly being scalar, assuming
	 * other code will perform array-to-pointer decay.
	 */
	return !ctype_is_void(c) && !ctype_is_struct(c);
}

static WARN_UNUSED result_t
sema_non_scalar(struct ast *a, void *userdata MAYBE_UNUSED)
{
	bool scalar = true;

	switch (a->node_type) {
	case NODE_IF_ELSE:
		scalar = is_scalar(&a->u.if_.condition->expr_type);
		break;
	case NODE_LOOP:
		if (a->u.loop.precond->node_type != NODE_EXPRESSION_NULL &&
		    !is_scalar(&a->u.loop.precond->expr_type)) {
			scalar = false;
		}
		if (a->u.loop.postcond->node_type != NODE_EXPRESSION_NULL &&
		    !is_scalar(&a->u.loop.postcond->expr_type)) {
			scalar = false;
		}
		break;
	case NODE_SWITCH:
		scalar = is_scalar(&a->u.switch_.control->expr_type);
		break;
	case NODE_CASE:
		scalar = is_scalar(&a->u.case_.constant->expr_type);
		break;
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
		scalar = is_scalar(&a->u.op_unary.operand->expr_type);
		break;
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
		scalar = is_scalar(&a->u.op_binary.lhs->expr_type) &&
		         is_scalar(&a->u.op_binary.rhs->expr_type);
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		scalar = is_scalar(&a->u.op_ternary.condition->expr_type);
		if (is_scalar(&a->u.op_ternary.then_expr->expr_type) !=
		    is_scalar(&a->u.op_ternary.else_expr->expr_type)) {
			return make_result(ERR_SEMA_OPERAND_TERNARY_MISMATCH);
		}
		break;
	case NODE_EXPRESSION_CAST:
		if (ctype_is_aggregate(&a->u.cast.to_type)) {
			return make_result(
				ERR_SEMA_CAST_TO_ARRAY_OR_STRUCT_INVALID);
		}
		if (is_scalar(&a->u.cast.to_type)) {
			scalar = is_scalar(&a->u.cast.expr->expr_type);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
		/* assignment allows struct, in addition to scalar values */
		scalar = !ctype_is_void(&a->u.op_binary.lhs->expr_type) &&
		         !ctype_is_void(&a->u.op_binary.rhs->expr_type);
		break;
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		/* guaranteed by sema_lvalue(), is_node_lvalue() */
		assert(is_scalar(&a->u.op_unary.operand->expr_type));
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		/* sema_expr_types() already guarantees ctype_is_pointer() */
		assert(is_scalar(&a->u.op_unary.operand->expr_type));
		break;
	default:
		break;
	}

	if (!scalar) {
		return make_result(ERR_SEMA_OPERAND_SCALAR_REQUIRED);
	}
	return RESULT_OK;
}

struct sema_incomplete_state {
	struct type_table *types;
	struct ast *prev;
};

static WARN_UNUSED result_t
sema_incomplete(struct ast *a, void *userdata MAYBE_UNUSED)
{
	struct sema_incomplete_state *state = userdata;
	struct type_table *types = state->types;

	const bool parent_is_addr_of =
		(state->prev != NULL &&
	         state->prev->node_type == NODE_EXPRESSION_UNARY_ADDRESS_OF &&
	         state->prev->u.op_unary.operand == a);

	switch (a->node_type) {
	case NODE_EXPRESSION_UNARY_SIZE_OF:
		if (ctype_is_incomplete(&a->u.op_unary.operand->expr_type,
		                        types)) {
			return make_result(ERR_SEMA_OPERAND_SIZEOF_INCOMPLETE);
		}
		break;
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
		if (!parent_is_addr_of &&
		    ctype_is_pointer(&a->u.op_unary.operand->expr_type) &&
		    ctype_is_incomplete(
			    a->u.op_unary.operand->expr_type.referent,
			    types)) {
			return make_result(ERR_SEMA_OPERAND_DEREF_INCOMPLETE);
		}
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		if (!parent_is_addr_of &&
		    ctype_is_incomplete(&a->expr_type, types)) {
			return make_result(
				ERR_SEMA_VARIABLE_USAGE_TYPE_INCOMPLETE,
				a->u.var.name.data,
				a->u.var.name.sz);
		}
		break;
	default:
		break;
	}

	state->prev = a;
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

struct sema_symbol_state {
	Arena *arena;
	struct flat *ast_program_globals;
	struct symbol *function_symbols;
	struct symbol *variable_symbols;
	struct type_table *types;
};

enum symbol_declaration_scope {
	SCOPE_BLOCK,
	SCOPE_FILE,
};

struct sema_symbol_auxiliary {
	long long int n_args;
	struct ctype *p_types __attribute__((counted_by(n_args)));
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
sema_fn_signature_matches(
	struct symbol *dup,    /* declaration/definition to cross-reference */
	long long int n_args,  /* arg count of current declaration/def/call */
	struct ctype *p_types, /* arg types of current declaration/def/call */
	bool is_def_or_decl)   /* treat as fn declaration/def? or fn call?  */
{
	bool p_types_match = true;
	for (long long int i = 0; i < n_args; ++i) {
		const struct ctype *lhs = &sema_get_auxiliary(dup)->p_types[i];
		const struct ctype *rhs = &p_types[i];

		/*
		 * Require exact parameter type match on redeclaration,
		 * definition of preceding declaration, etc.
		 *
		 * Also require exact match between function call and function
		 * definition when passing struct by value.
		 *
		 * Otherwise, allow conversion between function call argument
		 * type and function definition parameter type.
		 */
		const bool try_convert = !is_def_or_decl &&
		                         !ctype_is_struct(lhs) &&
		                         !ctype_is_struct(rhs);

		if (try_convert) {
			/*
			 * On function call, try to widen or narrow argument
			 * expression type to declared parameter type.
			 */
			check(sema_pointer_cmp(lhs, rhs));
			continue;
		}

		if (!ctype_is_equal(lhs, rhs)) {
			p_types_match = false;
			break;
		}
	}

	if (!p_types_match) {
		return make_result(ERR_SEMA_FUNCTION_DEFINITION_CONFLICT,
		                   dup->name.data,
		                   dup->name.sz);
	}

	return RESULT_OK;
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

	check(sema_fn_signature_matches(dup, n_args, p_types, is_def_or_decl));

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
			                       state->types,
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
			                        state->types,
			                        &linkage_state->initializer));
		} else if (is_node_constant(a->u.declare.init)) {
			linkage_state->initial = INITIAL_VALUE_CONSTANT;
			check(map_numeric_type(state->arena,
			                       a->u.declare.init,
			                       &a->u.declare.var_type,
			                       state->types,
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

// TODO: split out helper functions into src/passes/sema/{walk,literal,...}.c
// this file is too confusing to navigate
// should be clearer once logic for lvalues/literals/pointers/etc are separate
result_t
sema_typecheck(Arena *arena,
               struct ast *a,
               long long int *label_generator,
               struct symbol_table *s,
               struct type_table *types)
{
	struct sema_ops ops = {0};

	debug("Expanding compound assignment statements");
	ops.node_enter = sema_compound_assignment;
	check(sema_walk(a, &ops, arena));

	debug("Expanding array subscript expressions");
	ops.node_enter = sema_subscript;
	check(sema_walk(a, &ops, arena));

	debug("Expanding struct pointer expressions");
	ops.node_enter = sema_struct_pointer;
	check(sema_walk(a, &ops, arena));

	debug("Expanding string literals and hoisting if necessary");
	check(sema_typecheck_strlit(arena, a, s, types));

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
	check(sema_typecheck_expr(arena, a, types));

	debug("Checking for invalid lvalues");
	ops.node_enter = sema_lvalue;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid usage of non-scalar expressions");
	ops.node_enter = sema_non_scalar;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid usage of incomplete types");
	ops.node_enter = sema_incomplete;
	{
		struct sema_incomplete_state incomplete_state = {0};
		incomplete_state.types = types;
		check(sema_walk(a, &ops, &incomplete_state));
	}

	debug("Checking for invalid double usage");
	ops.node_enter = sema_double;
	check(sema_walk(a, &ops, NULL));

	debug("Checking for invalid pointer usage");
	check(sema_typecheck_ptr(arena, a, types));

	debug("Inserting cast expressions");
	check(sema_typecheck_implicit_cast(arena, a, types));

	debug("Labeling loops, loop breaks, and continues");
	check(sema_label_loops(arena, a, label_generator));

	debug("Labeling goto statements and labels");
	check(sema_label_gotos(arena, a, label_generator));

	struct sema_symbol_state state = {0};
	state.arena = arena;
	state.function_symbols = s->functions;
	state.variable_symbols = s->variables;
	state.types = types;

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
