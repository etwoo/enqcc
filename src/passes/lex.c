#include "passes/lex.h"

#include "passes.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
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

#define FOREACH_LEX_CHAR(F)                                                    \
	F('(', TOKEN_PAREN_OPEN)                                               \
	F(')', TOKEN_PAREN_CLOSE)                                              \
	F('{', TOKEN_BRACE_OPEN)                                               \
	F('}', TOKEN_BRACE_CLOSE)                                              \
	F(';', TOKEN_SEMICOLON)                                                \
	F('~', TOKEN_TILDE)                                                    \
	F('-', TOKEN_HYPHEN)                                                   \
	F('+', TOKEN_PLUS_SIGN)                                                \
	F('*', TOKEN_ASTERISK)                                                 \
	F('/', TOKEN_FORWARD_SLASH)                                            \
	F('%', TOKEN_PERCENT_SIGN)                                             \
	F('!', TOKEN_EXCLAMATION)                                              \
	F('&', TOKEN_AMPERSAND)                                                \
	F('|', TOKEN_VERT_BAR)                                                 \
	F('=', TOKEN_EQUAL_SIGN)                                               \
	F('<', TOKEN_LESS_THAN)                                                \
	F('>', TOKEN_MORE_THAN)                                                \
	F('?', TOKEN_QUESTION)                                                 \
	F(':', TOKEN_COLON)                                                    \
	F(',', TOKEN_COMMA)                                                    \
	F('^', TOKEN_CARET)

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

static WARN_UNUSED bool
lex_one_token_peek(struct string_view *pos, struct token *cur)
{
	assert(pos->sz > 1);
	bool matched = true;

	if (pos->data[0] == pos->data[1]) {
		switch (cur->token_type) {
		case TOKEN_HYPHEN:
			cur->token_type = TOKEN_HYPHEN_HYPHEN;
			break;
		case TOKEN_AMPERSAND:
			cur->token_type = TOKEN_AMPERSAND_AMPERSAND;
			break;
		case TOKEN_VERT_BAR:
			cur->token_type = TOKEN_VERT_BAR_VERT_BAR;
			break;
		case TOKEN_EQUAL_SIGN:
			cur->token_type = TOKEN_EQUAL_SIGN_EQUAL_SIGN;
			break;
		case TOKEN_LESS_THAN:
			cur->token_type = TOKEN_LESS_THAN_LESS_THAN;
			break;
		case TOKEN_MORE_THAN:
			cur->token_type = TOKEN_MORE_THAN_MORE_THAN;
			break;
		default:
			matched = false;
			break;
		}
	} else if (pos->data[1] == '=') {
		switch (cur->token_type) {
		case TOKEN_EXCLAMATION:
			cur->token_type = TOKEN_EXCLAMATION_EQUAL_SIGN;
			break;
		case TOKEN_AMPERSAND:
			cur->token_type = TOKEN_AMPERSAND_EQUAL_SIGN;
			break;
		case TOKEN_VERT_BAR:
			cur->token_type = TOKEN_VERT_BAR_EQUAL_SIGN;
			break;
		case TOKEN_LESS_THAN:
			cur->token_type = TOKEN_LESS_THAN_EQUAL_SIGN;
			break;
		case TOKEN_MORE_THAN:
			cur->token_type = TOKEN_MORE_THAN_EQUAL_SIGN;
			break;
		default:
			matched = false;
		}
	} else {
		matched = false;
	}

	return matched;
}

#define FOREACH_LEX_KEYWORD(F)                                                 \
	F("return", TOKEN_KEYWORD_RETURN)                                      \
	F("void", TOKEN_KEYWORD_VOID)                                          \
	F("int", TOKEN_KEYWORD_INT)                                            \
	F("if", TOKEN_KEYWORD_IF)                                              \
	F("else", TOKEN_KEYWORD_ELSE)                                          \
	F("do", TOKEN_KEYWORD_DO)                                              \
	F("while", TOKEN_KEYWORD_WHILE)                                        \
	F("for", TOKEN_KEYWORD_FOR)                                            \
	F("break", TOKEN_KEYWORD_BREAK)                                        \
	F("continue", TOKEN_KEYWORD_CONTINUE)                                  \
	F("static", TOKEN_KEYWORD_STATIC)                                      \
	F("extern", TOKEN_KEYWORD_EXTERN)

static WARN_UNUSED unsigned
lex_one_token_keyword_maybe(struct string_view *pos)
{
#define INIT_STRUCT(str, enum_value) {str, sizeof(str) - 1, enum_value},
	struct {
		const char *keyword;
		size_t keyword_strlen;
		unsigned value;
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
		const bool peek_match =
			(pos->sz > 1) && lex_one_token_peek(pos, cur);
		if (peek_match) {
			pos->data++;
			pos->sz--;
		}
	} else if (isdigit(c)) {
		cur->val.data = pos->data;
		do {
			pos->data++;
			pos->sz--;
		} while (isdigit(*pos->data));
		cur->val.sz = pos->data - cur->val.data;
		cur->token_type = TOKEN_CONSTANT;
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
		FOREACH_LEX_CHAR(TRY_DEBUG_PRINT_TOKEN)
		FOREACH_LEX_KEYWORD(TRY_DEBUG_PRINT_TOKEN)
	case TOKEN_IDENTIFIER:
		debug("IDENTIFIER %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_CONSTANT:
		debug("CONSTANT %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_HYPHEN_HYPHEN:
		debug("TOKEN_HYPHEN_HYPHEN");
		break;
	case TOKEN_EXCLAMATION_EQUAL_SIGN:
		debug("TOKEN_EXCLAMATION_EQUAL_SIGN");
		break;
	case TOKEN_AMPERSAND_AMPERSAND:
		debug("TOKEN_AMPERSAND_AMPERSAND");
		break;
	case TOKEN_AMPERSAND_EQUAL_SIGN:
		debug("TOKEN_AMPERSAND_EQUAL_SIGN");
		break;
	case TOKEN_VERT_BAR_VERT_BAR:
		debug("TOKEN_VERT_BAR_VERT_BAR");
		break;
	case TOKEN_VERT_BAR_EQUAL_SIGN:
		debug("TOKEN_VERT_BAR_EQUAL_SIGN");
		break;
	case TOKEN_EQUAL_SIGN_EQUAL_SIGN:
		debug("TOKEN_EQUAL_SIGN_EQUAL_SIGN");
		break;
	case TOKEN_LESS_THAN_LESS_THAN:
		debug("TOKEN_LESS_THAN_LESS_THAN");
		break;
	case TOKEN_LESS_THAN_EQUAL_SIGN:
		debug("TOKEN_LESS_THAN_EQUAL_SIGN");
		break;
	case TOKEN_MORE_THAN_MORE_THAN:
		debug("TOKEN_MORE_THAN_MORE_THAN");
		break;
	case TOKEN_MORE_THAN_EQUAL_SIGN:
		debug("TOKEN_MORE_THAN_EQUAL_SIGN");
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

#undef FOREACH_LEX_CHAR
#undef FOREACH_LEX_KEYWORD
