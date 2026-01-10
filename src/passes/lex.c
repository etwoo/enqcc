#include "passes/lex.h"

#include "passes.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>

static WARN_UNUSED result_t
lex_alloc(Arena *arena, struct token **tok)
{
	*tok = arena_alloc(arena, sizeof(**tok));
	check_if(*tok == NULL, ERR_LEX_ALLOC);
	memset(*tok, 0, sizeof(**tok));
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_peek_ok(struct string_view *pos, const struct string_view *prefix)
{
#define GET_CHAR(candidate, enum_value) candidate,
	const char allowed[] = {FOREACH_LEX_CHAR(GET_CHAR)};
#undef GET_CHAR

	const char c = pos->data[0];
	bool ok = isspace(c);
	for (size_t i = 0; i < ARRAY_SIZE(allowed); ++i) {
		ok = (c == allowed[i]) || ok;
	}

	check_if(!ok,
	         ERR_LEX_IDENTIFIER_CONSTANT_KEYWORD_PEEK_ERROR,
	         c,
	         prefix->data,
	         prefix->sz);
	pos->data--; // allow caller's generic increment to handle last char
	pos->sz++;
	return RESULT_OK;
}

static WARN_UNUSED size_t
lex_readahead_one_or_two_chars(struct string_view *pos, struct token *cur)
{
	if (pos->sz <= 1) {
		return 0;
	}

	size_t readahead = 0;

#define TRY_READAHEAD(to_match, result)                                        \
	case to_match:                                                         \
		cur->token_type = result;                                      \
		readahead = 1;                                                 \
		break;

	if (pos->data[0] == pos->data[1]) {
		switch (cur->token_type) {
			FOREACH_LEX_CHAR_REPEAT(TRY_READAHEAD)
		default:
			break;
		}
		if (readahead == 1 && pos->sz > 2 && pos->data[2] == '=') {
			switch (cur->token_type) {
			case TOKEN_LESS_THAN_LESS_THAN:
				cur->token_type =
					TOKEN_LESS_THAN_LESS_THAN_EQUAL_SIGN;
				readahead = 2;
				break;
			case TOKEN_MORE_THAN_MORE_THAN:
				cur->token_type =
					TOKEN_MORE_THAN_MORE_THAN_EQUAL_SIGN;
				readahead = 2;
				break;
			default:
				break;
			}
		}
	} else if (pos->data[1] == '=') {
		switch (cur->token_type) {
			FOREACH_LEX_CHAR_EQUALS_SIGN(TRY_READAHEAD)
		default:
			break;
		}
	} else if (pos->data[1] == '>') {
		switch (cur->token_type) {
		case TOKEN_HYPHEN:
			cur->token_type = TOKEN_ARROW;
			readahead = 1;
			break;
		default:
			break;
		}
	}

	return readahead;

#undef TRY_READAHEAD
}

static WARN_UNUSED enum lex_tokentype
lex_one_token_keyword_maybe(struct string_view *pos)
{
#define INIT_STRUCT(str, enum_value) {str, sizeof(str) - 1, enum_value},
	struct {
		const char *keyword;
		size_t keyword_strlen;
		enum lex_tokentype value;
	} candidates[] = {FOREACH_LEX_KEYWORD(INIT_STRUCT)};
#undef INIT_STRUCT
	for (size_t i = 0; i < ARRAY_SIZE(candidates); ++i) {
		if (candidates[i].keyword_strlen == pos->sz &&
		    0 == strncmp(candidates[i].keyword, pos->data, pos->sz)) {
			return candidates[i].value;
		}
	}
	return TOKEN_IDENTIFIER;
}

const char DECIMAL_POINT = '.';
const char E_NOTATION_CHAR = 'E';

static WARN_UNUSED bool
isdot(char c)
{
	return c == DECIMAL_POINT;
}

static WARN_UNUSED bool
issign(char c)
{
	return c == '+' || c == '-';
}

struct lex_sep_state {
	const char sep;
	bool found;             /* found this separator during lex?       */
	size_t allow_sign_span; /* if found, allows +/- in following span */
	bool needs_digit;       /* if found, expects digit to follow      */
};

static WARN_UNUSED result_t
lex_one_constant_numeric_finalize(struct string_view *pos,
                                  struct token *cur,
                                  const struct lex_sep_state *sep_chars,
                                  size_t n_sep_chars)
{
	bool found_sep = false;
	bool malformed = false;
	for (size_t i = 0; i < n_sep_chars; ++i) {
		if (sep_chars[i].found) {
			found_sep = true;
			malformed = sep_chars[i].needs_digit || malformed;
		}
	}

	if (found_sep && malformed) {
		return make_result(ERR_LEX_FLOAT_EXPONENT_NO_DIGITS,
		                   cur->val.data,
		                   pos->data - cur->val.data);
	}

	const char allowed[2] = {'L', 'U'};
	for (size_t i = 0; !found_sep && i < ARRAY_SIZE(allowed); ++i) {
		if (toupper(*pos->data) == allowed[i]) {
			pos->data++;
			pos->sz--;
			const size_t other = ARRAY_SIZE(allowed) - (i + 1);
			if (toupper(*pos->data) == allowed[other]) {
				pos->data++;
				pos->sz--;
			}
			break;
		}
	}

	cur->val.sz = pos->data - cur->val.data;
	cur->token_type = TOKEN_CONSTANT;
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_one_constant_numeric(struct string_view *pos, struct token **tok)
{
	struct token *cur = *tok;
	struct lex_sep_state *sep_latest = NULL;

	struct lex_sep_state sep_chars[] = {
		{
			.sep = DECIMAL_POINT,
			.found = false,
			.allow_sign_span = 0,
			.needs_digit = false,
		},
		{
			.sep = E_NOTATION_CHAR,
			.found = false,
			.allow_sign_span = 2,
			.needs_digit = true,
		},
	};

	cur->val.data = pos->data;
	assert(isdigit(cur->val.data[0]) || isdot(cur->val.data[0]));

	while (pos->sz > 0) {
		const char c = *pos->data;

		bool newly_found_sep = false;
		for (size_t i = 0; i < ARRAY_SIZE(sep_chars); ++i) {
			if (sep_chars[i].sep == toupper(c)) {
				if (!sep_chars[i].found) {
					sep_chars[i].found = true;
					newly_found_sep = true;
					sep_latest = &sep_chars[i];
				}
				break;
			}
		}

		if (isdigit(c) ||          /* process decimal digit: [0-9]  */
		    newly_found_sep ||     /* process separator: '.' or 'E' */
		    (issign(c) &&          /* process +/- sign, if (!) ...  */
		     sep_latest != NULL && /* ... sign allowed in this span */
		     sep_latest->allow_sign_span > 0)) {
			pos->data++;
			pos->sz--;
			if (sep_latest != NULL) {
				if (sep_latest->allow_sign_span > 0) {
					sep_latest->allow_sign_span--;
				}
				if (isdigit(c)) {
					sep_latest->needs_digit = false;
				}
			}
			continue;
		}

		break;
	}

	check(lex_one_constant_numeric_finalize(pos,
	                                        cur,
	                                        sep_chars,
	                                        ARRAY_SIZE(sep_chars)));
	return RESULT_OK;
}

static WARN_UNUSED bool
is_quote_single(char c)
{
	return c == '\'';
}

static WARN_UNUSED bool
is_quote_double(char c)
{
	return c == '"';
}

static WARN_UNUSED bool
is_delimiter(enum lex_tokentype token_type, char c)
{
	return (token_type == TOKEN_CONSTANT_CHAR && is_quote_single(c)) ||
	       (token_type == TOKEN_CONSTANT_STR && is_quote_double(c));
}

static WARN_UNUSED bool
is_newline(char c)
{
	return c == '\n';
}

static WARN_UNUSED bool
is_backslash(char c)
{
	return c == '\\';
}

static WARN_UNUSED char
map_escape_char(char c)
{
	switch (c) {
	case '\'':
		c = '\'';
		break;
	case '"':
		c = '"';
		break;
	case '?':
		c = '?';
		break;
	case '\\':
		c = '\\';
		break;
	case 'a':
		c = '\a';
		break;
	case 'b':
		c = '\b';
		break;
	case 'f':
		c = '\f';
		break;
	case 'n':
		c = '\n';
		break;
	case 'r':
		c = '\r';
		break;
	case 't':
		c = '\t';
		break;
	case 'v':
		c = '\v';
		break;
	default:
		c = '\0'; /* use NUL to signal error to caller */
		break;
	}
	return c;
}

static WARN_UNUSED bool
is_valid_escape_char(char c)
{
	return (map_escape_char(c) != '\0');
}

static WARN_UNUSED result_t
map_span_to_strlike(Arena *arena,
                    enum lex_tokentype token_type,
                    const struct string_view *src,
                    struct string_view *dst)
{
	char *out = arena_alloc(arena, src->sz); /* source size -> capacity */
	dst->data = out;
	dst->sz = 0;

	bool escaped = false;
	bool between_neighbors = false;

	for (size_t i = 0; i < src->sz; ++i) {
		char c = src->data[i];

		if (between_neighbors) {
			if (is_delimiter(token_type, c)) {
				between_neighbors = false;
			} else {
				assert(isspace(c));
			}
			continue;
		}

		if (escaped) {
			escaped = false;
			c = map_escape_char(c);
		} else if (is_delimiter(token_type, c)) {
			between_neighbors = true;
			continue;
		} else if (is_backslash(c)) {
			escaped = true;
			continue;
		}

		out[dst->sz] = c;
		dst->sz++;
	}

	assert(!escaped);           /* no dangling escape char */
	assert(!between_neighbors); /* require final delimiter */
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_one_strlike_check_size(const struct token *tok,
                           enum lex_tokentype token_type)
{
	if (token_type == TOKEN_CONSTANT_CHAR) {
		switch (tok->val.sz) {
		case 0:
			return make_result(ERR_LEX_CHAR_INVALID_EMPTY);
		case 1:
			break;
		default:
			return make_result(ERR_LEX_CHAR_INVALID_MULTICHAR);
		}
	}
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_one_strlike(Arena *arena,
                struct string_view *pos,
                struct token **tok,
                enum lex_tokentype token_type)
{
	struct token *cur = *tok;
	cur->token_type = token_type;

	assert(pos->sz > 0);
	assert(is_delimiter(token_type, pos->data[0]));
	pos->data++;
	pos->sz--;

	cur->val.data = pos->data;
	cur->val.sz = 0;

	bool done = false;
	bool deepcopy = false;

	while (!done) {
		if (pos->sz == 0) {
			return make_result(ERR_LEX_CHAR_EXPECT_MORE);
		}

		if (is_delimiter(token_type, pos->data[0])) {
			done = true;
			for (size_t i = 1; i < pos->sz; ++i) {
				const char peek = pos->data[i];
				if (isspace(peek)) {
					/* continue scanning */
				} else if (is_delimiter(token_type, peek)) {
					/* merge current and next literal */
					deepcopy = true;
					done = false;
					i++; /* seek past delim */
					pos->data += i;
					pos->sz -= i;
					cur->val.sz += i;
					break;
				} else {
					/* next token should not merge */
					assert(done);
					break;
				}
			}
			continue;
		}

		if (is_newline(pos->data[0])) {
			return make_result(ERR_LEX_CHAR_INVALID_NEWLINE);
		}

		const bool escaped = is_backslash(pos->data[0]);
		if (escaped) {
			deepcopy = true;
			if (pos->sz == 0) {
				return make_result(ERR_LEX_CHAR_EXPECT_MORE);
			}
			pos->data++;
			pos->sz--;
			cur->val.sz++;
			if (!is_valid_escape_char(pos->data[0])) {
				return make_result(ERR_LEX_CHAR_ESCAPE_INVALID,
				                   cur->val.data,
				                   cur->val.sz + 1);
			}
		}

		pos->data++;
		pos->sz--;
		cur->val.sz++;
	}

	if (deepcopy) {
		struct string_view raw = cur->val;
		check(map_span_to_strlike(arena, token_type, &raw, &cur->val));
	}

	check(lex_one_strlike_check_size(cur, token_type));
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_one_token(Arena *arena, struct string_view *pos, struct token **tok)
{
	check(lex_alloc(arena, tok));
	assert(*tok != NULL);
	struct token *cur = *tok;
	const char c = pos->data[0];
	bool early_match = false;

#define TRY_EARLY_MATCH(candidate, enum_value)                                 \
	if (!early_match && c == (candidate)) {                                \
		cur->token_type = enum_value;                                  \
		early_match = true;                                            \
	}
	FOREACH_LEX_CHAR(TRY_EARLY_MATCH);

#undef TRY_EARLY_MATCH

	if (early_match) {
		const size_t ahead = lex_readahead_one_or_two_chars(pos, cur);
		pos->data += ahead;
		pos->sz -= ahead;
	} else if (is_quote_single(c)) {
		check(lex_one_strlike(arena, pos, &cur, TOKEN_CONSTANT_CHAR));
	} else if (is_quote_double(c)) {
		check(lex_one_strlike(arena, pos, &cur, TOKEN_CONSTANT_STR));
	} else if (isdigit(c) || isdot(c)) {
		check(lex_one_constant_numeric(pos, &cur));
		check(lex_peek_ok(pos, &cur->val));
	} else if (isalpha(c) || c == '_') {
		cur->val.data = pos->data;
		do {
			pos->data++;
			pos->sz--;
		} while (isalnum(*pos->data) || *pos->data == '_');
		cur->val.sz = pos->data - cur->val.data;
		cur->token_type = lex_one_token_keyword_maybe(&cur->val);
		check(lex_peek_ok(pos, &cur->val));
	} else {
		return make_result(ERR_LEX_NO_MATCH, pos->data, pos->sz);
	}

	pos->data++;
	pos->sz--;

	return RESULT_OK;
}

result_t
lex_init(Arena *arena, const char *src, struct token **tok)
{
	int fd = open(src, O_RDONLY);
	check_if(fd < 0, ERR_LEX_OPEN_SOURCE_FILE, errno, src);

	struct string_view code = {0}; // TODO: munmap in lex_free()

	struct stat st = {
		.st_size = 0,
	};
	check_if(fstat(fd, &st) < 0, ERR_LEX_OPEN_SOURCE_FILE, errno);
	code.sz = st.st_size;
	code.data = mmap(NULL, code.sz, PROT_READ, MAP_PRIVATE, fd, 0);
	check_if(code.data == MAP_FAILED, ERR_LEX_OPEN_SOURCE_FILE, errno);

	while (code.sz > 0) {
		if (isspace(code.data[0])) {
			code.data++;
			code.sz--;
			continue;
		}
		check(lex_one_token(arena, &code, tok));
		assert(*tok != NULL);
		tok = &(*tok)->next;
	}

	return RESULT_OK;
}

static void
lex_debug_one(const struct token *tok)
{
#define TRY_DEBUG_PRINT_TOKEN(candidate, enum_value)                           \
	case enum_value:                                                       \
		debug("%s", #enum_value);                                      \
		break;

	switch (tok->token_type) {
		FOREACH_LEX_KEYWORD(TRY_DEBUG_PRINT_TOKEN)
		FOREACH_LEX_CHAR(TRY_DEBUG_PRINT_TOKEN)
		FOREACH_LEX_CHAR_REPEAT(TRY_DEBUG_PRINT_TOKEN)
		FOREACH_LEX_CHAR_EQUALS_SIGN(TRY_DEBUG_PRINT_TOKEN)
	case TOKEN_IDENTIFIER:
		debug("IDENTIFIER %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_CONSTANT:
		debug("CONSTANT %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_CONSTANT_CHAR:
		debug("CONSTANT_CHAR %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_CONSTANT_STR:
		debug("CONSTANT_STR %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_LESS_THAN_LESS_THAN_EQUAL_SIGN:
		debug("TOKEN_LESS_THAN_LESS_THAN_EQUAL_SIGN");
		break;
	case TOKEN_MORE_THAN_MORE_THAN_EQUAL_SIGN:
		debug("TOKEN_MORE_THAN_MORE_THAN_EQUAL_SIGN");
		break;
	case TOKEN_ARROW:
		debug("TOKEN_ARROW");
		break;
	}

#undef TRY_DEBUG_PRINT_TOKEN
}

void
lex_debug_print(const struct token *tok)
{
	while (tok != NULL) {
		lex_debug_one(tok);
		tok = tok->next;
	}
}
