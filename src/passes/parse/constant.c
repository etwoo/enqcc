#include "passes/parse/constant.h"

#include "passes/lex.h"
#include "passes/parse.h"
#include "passes/parse/alloc.h"
#include "passes/parse/token.h"
#include "sys/string_view.h"

#include <assert.h>
#include <ctype.h> /* for isupper() */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h> /* for strtoll() */
#include <string.h> /* for memchr() */

static WARN_UNUSED bool
is_constant_maybe_double(const struct string_view *val)
{
	assert(0 != isupper(E_NOTATION_CHAR));
	return NULL != memchr(val->data, DECIMAL_POINT, val->sz) ||
	       NULL != memchr(val->data, E_NOTATION_CHAR, val->sz) ||
	       NULL != memchr(val->data, tolower(E_NOTATION_CHAR), val->sz);
}

result_t
parse_constant(Arena *arena, const struct token **tok, struct ast **dst)
{
	check(parse_alloc(arena, dst, NODE_CONSTANT));

	if ((**tok).token_type == TOKEN_CONSTANT_CHAR) {
		assert((**tok).val.sz == 1);
		(**dst).expr_type.t = CTYPE_INT;
		(**dst).u.num = (int)(**tok).val.data[0];
		return RESULT_OK;
	}

	assert((**tok).token_type == TOKEN_CONSTANT);

	/*
	 * strtoull() does not update errno on success, so we must clear it
	 * explicitly if we want a predictable value.
	 */
	errno = 0;

	if (is_constant_maybe_double(&(**tok).val)) {
		const double tmp = strtod((**tok).val.data, NULL);
		if (errno != 0 && errno != ERANGE) {
			return make_result(ERR_PARSE_CONSTANT_STRTOD,
			                   errno,
			                   (**tok).val.data,
			                   (**tok).val.sz);
		}
		(**dst).expr_type.t = CTYPE_DOUBLE;
		(**dst).u.double_ = tmp;
		token_consume(tok);
		return RESULT_OK;
	}

	const long long unsigned tmp = strtoull((**tok).val.data, NULL, 0);
	if (errno != 0) {
		return make_result(ERR_PARSE_CONSTANT_STRTOULL,
		                   errno,
		                   (**tok).val.data,
		                   (**tok).val.sz);
	}

	bool suffix_long = false;
	bool suffix_unsigned = false;
	for (size_t i = 2; i > 0 && (**tok).val.sz >= i; --i) {
		switch (toupper((**tok).val.data[(**tok).val.sz - i])) {
		case 'L':
			suffix_long = true;
			break;
		case 'U':
			suffix_unsigned = true;
			break;
		default:
			break;
		}
	}

	bool too_large = false;
	if (suffix_unsigned && (tmp > UINT_MAX || suffix_long)) {
		(**dst).expr_type.t = CTYPE_UNSIGNED_LONG;
		too_large = (tmp > ULONG_MAX);
	} else if (suffix_unsigned) {
		(**dst).expr_type.t = CTYPE_UNSIGNED_INT;
		assert(tmp <= UINT_MAX);
	} else if (tmp > INT_MAX || suffix_long) {
		(**dst).expr_type.t = CTYPE_LONG;
		too_large = (tmp > LONG_MAX);
	} else {
		(**dst).expr_type.t = CTYPE_INT;
		assert(tmp <= INT_MAX);
	}
	(**dst).u.num = tmp;

	if (tmp == 0) {
		(**dst).expr_type.maybe_null_pointer_constant = true;
	}

	if (too_large) {
		return make_result(ERR_PARSE_CONSTANT_TOO_LARGE,
		                   (**tok).val.data,
		                   (**tok).val.sz);
	}

	token_consume(tok);
	return RESULT_OK;
}
