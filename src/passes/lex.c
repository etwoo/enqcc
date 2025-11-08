#include "passes/lex.h"

#include "passes.h"
#include "sys/array.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"
#include "sys/tmpfile.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>

static WARN_UNUSED result_t
lex_alloc(struct token **tok)
{
	*tok = malloc(sizeof(**tok));
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
	F('-', TOKEN_HYPHEN)

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

static WARN_UNUSED result_t
lex_one_token(struct string_view *pos, struct token **tok)
{
	check(lex_alloc(tok));
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
		if (cur->token_type == TOKEN_HYPHEN && /* peek ahead in case */
		    pos->sz > 1 &&                     /* of --, not just -  */
		    pos->data[1] == '-') {
			cur->token_type = TOKEN_HYPHEN_HYPHEN;
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
		if (0 == strncmp("return", cur->val.data, cur->val.sz)) {
			cur->token_type = TOKEN_KEYWORD_RETURN;
		} else if (0 == strncmp("void", cur->val.data, cur->val.sz)) {
			cur->token_type = TOKEN_KEYWORD_VOID;
		} else if (0 == strncmp("int", cur->val.data, cur->val.sz)) {
			cur->token_type = TOKEN_KEYWORD_INT;
		} else {
			cur->token_type = TOKEN_IDENTIFIER;
		}
		check(lex_peek_ok(pos, &cur->val));
	} else {
		return make_result(ERR_LEX_NO_MATCH, pos->data, pos->sz);
	}

	pos->data++;
	pos->sz--;

	return RESULT_OK;
}

result_t
lex_init(const char *src, struct token **tok)
{
	int fd = open(src, O_RDONLY);
	check_if(fd < 0, ERR_LEX_OPEN_SOURCE_FILE, errno, src);

	struct string_view code = {0}; // TODO: munmap in lex_free()
	check(tmpmap(fd, &code));

	while (code.sz > 0) {
		if (isspace(code.data[0])) {
			code.data++;
			code.sz--;
			continue;
		}
		check(lex_one_token(&code, tok));
		assert(*tok != NULL);
		tok = &(*tok)->next;
	}

	return RESULT_OK;
}

void
lex_free(struct token *tok)
{
	while (tok != NULL) {
		struct token *tmp = tok;
		tok = tok->next;
		free(tmp);
	}
}

void
lex_cleanup(struct token **tok)
{
	lex_free(*tok);
}

static void
lex_debug_one(const struct token *tok)
{
	switch (tok->token_type) {
	case TOKEN_IDENTIFIER:
		debug("IDENTIFIER %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_CONSTANT:
		debug("CONSTANT %.*s", (int)tok->val.sz, tok->val.data);
		break;
	case TOKEN_KEYWORD_RETURN:
		debug("KEYWORD return");
		break;
	case TOKEN_KEYWORD_VOID:
		debug("KEYWORD void");
		break;
	case TOKEN_KEYWORD_INT:
		debug("KEYWORD int");
		break;
	case TOKEN_PAREN_OPEN:
		debug("PAREN open");
		break;
	case TOKEN_PAREN_CLOSE:
		debug("PAREN close");
		break;
	case TOKEN_BRACE_OPEN:
		debug("BRACE open");
		break;
	case TOKEN_BRACE_CLOSE:
		debug("BRACE close");
		break;
	case TOKEN_SEMICOLON:
		debug("SEMICOLON");
		break;
	case TOKEN_TILDE:
		debug("TILDE");
		break;
	case TOKEN_HYPHEN_HYPHEN:
		debug("HYPHENHYPHEN");
		break;
	case TOKEN_HYPHEN:
		debug("HYPHEN");
		break;
	}
}

void
lex_debug_print(const struct token *tok)
{
	while (tok != NULL) {
		lex_debug_one(tok);
		tok = tok->next;
	}
}

#undef FOREACH_IMPORTANT_LEX_CHAR
