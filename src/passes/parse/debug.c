#include "passes/parse/debug.h"

#include "passes.h"
#include "passes/parse.h"
#include "sys/debug.h"

#include <assert.h>
#include <limits.h>

static void
parse_debug_print_ast_symbol(const char *description,
                             const struct ast_symbol *asym,
                             size_t indent)
{
	if (description != NULL) {
		debug("%*s%s", (int)indent, "", description);
	}
	debug("%*sIDENTIFIER %.*s",
	      (int)indent + 1,
	      "",
	      (int)asym->name.sz,
	      asym->name.data);
	debug("%*sIDENTIFIER.UNIQUE: %lld%s",
	      (int)indent + 1,
	      "",
	      asym->unique,
	      asym->unique == NOT_YET_UNIQUE ? " (not unique)" : "");

	const char *symbol_type_as_str = NULL;
	switch (asym->stype) {
	case SYMBOL_VARIABLE:
		symbol_type_as_str = "VARIABLE";
		break;
	case SYMBOL_FUNCTION_DECLARATION:
		symbol_type_as_str = "FUNCTION DECLARATION";
		break;
	case SYMBOL_FUNCTION_DEFINITION:
		symbol_type_as_str = "FUNCTION DEFINITION";
		break;
	case SYMBOL_STRING_LITERAL:
		symbol_type_as_str = "STRING LITERAL";
		break;
	}
	debug("%*sIDENTIFIER.TYPE: %s",
	      (int)indent + 1,
	      "",
	      symbol_type_as_str);

	const char *linkage_as_str = NULL;
	switch (asym->ltype) {
	case SYMBOL_LINKAGE_NONE:
		linkage_as_str = "NONE";
		break;
	case SYMBOL_LINKAGE_INTERNAL:
		linkage_as_str = "INTERNAL";
		break;
	case SYMBOL_LINKAGE_EXTERNAL:
		linkage_as_str = "EXTERNAL";
		break;
	}

	debug("%*sIDENTIFIER.LINKAGE: %s", (int)indent + 1, "", linkage_as_str);
}

static void
parse_debug_print_ast_spec(enum ast_specifier specifier, size_t indent)
{
	const char *spec_as_str = NULL;
	switch (specifier) {
	case SPECIFIER_NONE:
		break;
	case SPECIFIER_STATIC:
		spec_as_str = "STATIC";
		break;
	case SPECIFIER_EXTERN:
		spec_as_str = "EXTERN";
		break;
	}
	if (spec_as_str != NULL) {
		debug("%*sSPECIFIER: %s", (int)indent, "", spec_as_str);
	}
}

static void parse_debug_print_flat(const struct flat *a, size_t indent);

#define TO_STR(node_type, ...) #node_type,
static const char *const NODETYPE_NAMES[] = {FOREACH_AST_NODE(TO_STR)};
#undef TO_STR

void
parse_debug_print(const struct ast *a, size_t indent)
{
	char tmp[128] = {0};

	assert(indent <= INT_MAX);
	debug("%*s%s%s%s%s",
	      (int)indent,
	      "",
	      NODETYPE_NAMES[a->node_type],
	      a->node_type >= NODE_CONSTANT ? " [" : "",
	      a->node_type >= NODE_CONSTANT
	              ? ctype_to_str(&a->expr_type, tmp, sizeof(tmp))
	              : "",
	      a->node_type >= NODE_CONSTANT ? "]" : "");

	switch (a->node_type) {
	case NODE_PROGRAM:
		parse_debug_print_flat(a->u.program.globals, indent + 1);
		break;
	case NODE_FUNCTION:
		parse_debug_print_ast_symbol("NAME",
		                             &a->u.function.identifier,
		                             indent + 1);
		parse_debug_print_ast_spec(a->u.function.specifier, indent + 1);
		debug("%*sRETURNS: %s",
		      (int)(indent + 1),
		      "",
		      ctype_to_str(&a->u.function.return_type,
		                   tmp,
		                   sizeof(tmp)));
		FOREACH_FUNCTION_PARAMETER (cur, a->u.function.params) {
			parse_debug_print_ast_symbol("PARAMETER",
			                             &cur->symbol,
			                             indent + 1);
			debug("%*sPARAMETER.TYPE: %s",
			      (int)indent + 2,
			      "",
			      ctype_to_str(&cur->parameter_type,
			                   tmp,
			                   sizeof(tmp)));
		}
		debug("%*sBODY", (int)(indent + 1), "");
		if (a->u.function.block != NULL) {
			parse_debug_print(a->u.function.block, indent + 2);
		}
		break;
	case NODE_BLOCK:
		parse_debug_print_flat(a->u.block.statements, indent + 1);
		break;
	case NODE_DECLARATION:
		parse_debug_print_ast_symbol(NULL,
		                             &a->u.declare.identifier,
		                             indent);
		parse_debug_print_ast_spec(a->u.declare.specifier, indent + 1);
		debug("%*sVARIABLE.TYPE: %s",
		      (int)indent + 1,
		      "",
		      ctype_to_str(&a->u.declare.var_type, tmp, sizeof(tmp)));
		if (a->u.declare.init != NULL) {
			parse_debug_print(a->u.declare.init, indent + 1);
		}
		break;
	case NODE_IF_ELSE:
		debug("%*sCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.if_.condition, indent + 2);
		debug("%*sTHEN", (int)indent + 1, "");
		parse_debug_print_flat(a->u.if_.then_clause, indent + 2);
		if (a->u.if_.else_clause != NULL) {
			debug("%*sELSE", (int)indent + 1, "");
			parse_debug_print_flat(a->u.if_.else_clause,
			                       indent + 2);
		}
		break;
	case NODE_LOOP:
		debug("%*sLOOP ID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.loop.label_start,
		      a->u.loop.label_start == UNSET_LOOP_ID ? " (unset)" : "");
		debug("%*sPRECONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.loop.precond, indent + 2);
		debug("%*sBODY", (int)indent + 1, "");
		parse_debug_print_flat(a->u.loop.body, indent + 2);
		debug("%*sCONTINUE LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.loop.label_continue,
		      a->u.loop.label_continue == UNSET_LOOP_ID ? " (unset)"
		                                                : "");
		debug("%*sINCREMENTER", (int)indent + 1, "");
		parse_debug_print(a->u.loop.incr, indent + 2);
		debug("%*sPOSTCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.loop.postcond, indent + 2);
		debug("%*sEND LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.loop.label_end,
		      a->u.loop.label_end == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_BREAK:
		debug("%*sLOOP/SWITCH ID %lld%s",
		      (int)indent + 1,
		      "",
		      (long long)a->u.num,
		      a->u.num == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_CONTINUE:
		debug("%*sLOOP ID %lld%s",
		      (int)indent + 1,
		      "",
		      (long long)a->u.num,
		      a->u.num == UNSET_LOOP_ID ? " (unset)" : "");
		break;
	case NODE_GOTO:
		debug("%*sTARGET LABEL %.*s",
		      (int)indent + 1,
		      "",
		      (int)a->u.goto_.target_label.sz,
		      a->u.goto_.target_label.data);
		debug("%*sTARGET ID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.goto_.target_unique,
		      a->u.goto_.target_unique == UNSET_LABEL_ID ? " (unset)"
		                                                 : "");
		break;
	case NODE_LABEL:
		debug("%*sNAME %.*s",
		      (int)indent + 1,
		      "",
		      (int)a->u.label.name.sz,
		      a->u.label.name.data);
		debug("%*sID %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.label.unique,
		      a->u.label.unique == UNSET_LABEL_ID ? " (unset)" : "");
		break;
	case NODE_SWITCH:
		debug("%*sCONTROL", (int)indent + 1, "");
		parse_debug_print(a->u.switch_.control, indent + 2);
		debug("%*sBODY", (int)indent + 1, "");
		parse_debug_print_flat(a->u.switch_.body, indent + 2);
		debug("%*sSWITCH DEFAULT LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.switch_.label_default,
		      a->u.switch_.label_default == UNSET_SWITCH_ID ? " (unset)"
		                                                    : "");
		debug("%*sSWITCH END LABEL %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.switch_.label_end,
		      a->u.switch_.label_end == UNSET_SWITCH_ID ? " (unset)"
		                                                : "");
		debug("%*sSEMANTIC CASE INFORMATION", (int)indent + 1, "");
		parse_debug_print_flat(a->u.switch_.label_cases, indent + 2);
		break;
	case NODE_CASE:
		parse_debug_print(a->u.case_.constant, indent + 1);
		__attribute__((fallthrough));
	case NODE_CASE_DEFAULT:
		debug("%*sCASE LABEL: %lld%s",
		      (int)indent + 1,
		      "",
		      a->u.case_.unique,
		      a->u.case_.unique == UNSET_SWITCH_ID ? " (unset)" : "");
		break;
	case NODE_EXPRESSION_NULL:
		break;
	case NODE_EXPRESSION_INITIALIZER:
		if (a->u.init.single != NULL) {
			assert(a->u.init.multi == NULL);
			parse_debug_print(a->u.init.single, indent + 1);
		}
		if (a->u.init.multi != NULL) {
			assert(a->u.init.single == NULL);
			parse_debug_print_flat(a->u.init.multi, indent + 1);
		}
		break;
	case NODE_FUNCTION_RETURN_STATEMENT:
	case NODE_EXPRESSION_UNARY_NEGATE:
	case NODE_EXPRESSION_UNARY_NOT:
	case NODE_EXPRESSION_UNARY_COMPLEMENT:
	case NODE_EXPRESSION_UNARY_DEREFERENCE:
	case NODE_EXPRESSION_UNARY_ADDRESS_OF:
	case NODE_EXPRESSION_UNARY_SIZE_OF:
	case NODE_EXPRESSION_PAREN_ENCLOSED:
	case NODE_EXPRESSION_PREDECREMENT:
	case NODE_EXPRESSION_POSTDECREMENT:
	case NODE_EXPRESSION_PREINCREMENT:
	case NODE_EXPRESSION_POSTINCREMENT:
		parse_debug_print(a->u.op_unary.operand, indent + 1);
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
	case NODE_EXPRESSION_VARIABLE_ASSIGNMENT:
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
		parse_debug_print(a->u.op_binary.lhs, indent + 1);
		parse_debug_print(a->u.op_binary.rhs, indent + 1);
		break;
	case NODE_EXPRESSION_VARIABLE_USAGE:
		parse_debug_print_ast_symbol(NULL, &a->u.var, indent + 1);
		break;
	case NODE_EXPRESSION_TERNARY_CONDITIONAL:
		debug("%*sCONDITION", (int)indent + 1, "");
		parse_debug_print(a->u.op_ternary.condition, indent + 2);
		debug("%*sTHEN", (int)indent + 1, "");
		parse_debug_print(a->u.op_ternary.then_expr, indent + 2);
		if (a->u.op_ternary.else_expr != NULL) {
			debug("%*sELSE", (int)indent + 1, "");
			parse_debug_print(a->u.op_ternary.else_expr,
			                  indent + 2);
		}
		break;
	case NODE_EXPRESSION_FUNCTION_CALL:
		parse_debug_print_ast_symbol("FUNCTION",
		                             &a->u.call.identifier,
		                             indent + 1);
		debug("%*sARGUMENTS", (int)indent + 1, "");
		parse_debug_print_flat(a->u.call.args, indent + 2);
		break;
	case NODE_EXPRESSION_CAST:
		debug("%*sCAST.TO: %s",
		      (int)(indent + 1),
		      "",
		      ctype_to_str(&a->u.cast.to_type, tmp, sizeof(tmp)));
		parse_debug_print(a->u.cast.expr, indent + 1);
		break;
	case NODE_CONSTANT:
		if (ctype_is_floating_point(&a->expr_type)) {
			debug("%*sVALUE %f", (int)indent + 1, "", a->u.double_);
		} else {
			debug("%*sVALUE %lld",
			      (int)indent + 1,
			      "",
			      (long long)a->u.num);
		}
		break;
	case NODE_CONSTANT_STR:
		debug("%*sSTR %.*s",
		      (int)indent + 1,
		      "",
		      (int)a->u.str.sz,
		      a->u.str.data);
		break;
	}
}

static void
parse_debug_print_flat(const struct flat *a, size_t indent)
{
	const struct flat *cursor = a;
	for (; cursor != NULL; cursor = cursor->cdr) {
		assert(cursor->car != NULL);
		parse_debug_print(cursor->car, indent);
	}
}
