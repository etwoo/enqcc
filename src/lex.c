#include "passes.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"
#include "sys/tmpfile.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h> /* for memset() */

static WARN_UNUSED result_t
lex_alloc(struct token **tok)
{
	*tok = malloc(sizeof(**tok));
	check_if(*tok == NULL, ERR_LEX_ALLOC);
	memset(*tok, 0, sizeof(**tok));
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_alloc_stringview(struct string_view **sv, const char *data, size_t sz)
{
	*sv = malloc(sizeof(**sv));
	check_if(*sv == NULL, ERR_LEX_ALLOC);
	(*sv)->data = data;
	(*sv)->sz = sz;
	return RESULT_OK;
}

static WARN_UNUSED result_t
lex_peek_ok(struct string_view *pos, const struct string_view *prefix)
{
	const char c = pos->data[0];
	const bool ok = (isspace(c) || c == '(' || c == ')' || c == '{' ||
	                 c == '}' || c == ';');
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
	const char c = pos->data[0];
	if (isspace(c)) {
		pos->data++;
		pos->sz--;
		return RESULT_OK;
	}

	check(lex_alloc(tok));
	assert(*tok != NULL);
	struct token *cur = *tok;

	if (c == '(') {
		cur->token_type = TOKEN_PAREN_OPEN;
	} else if (c == ')') {
		cur->token_type = TOKEN_PAREN_CLOSE;
	} else if (c == '{') {
		cur->token_type = TOKEN_BRACE_OPEN;
	} else if (c == '}') {
		cur->token_type = TOKEN_BRACE_CLOSE;
	} else if (c == ';') {
		cur->token_type = TOKEN_SEMICOLON;
	} else if (isdigit(c)) {
		cur->token_type = TOKEN_CONSTANT;
		const char *start = pos->data;
		do {
			pos->data++;
			pos->sz--;
		} while (isdigit(*pos->data));
		const size_t sz = pos->data - start;
		check(lex_alloc_stringview(&cur->value, start, sz));
		check(lex_peek_ok(pos, cur->value));
	} else if (isalpha(c) || c == '_') {
		const char *start = pos->data;
		do {
			pos->data++;
			pos->sz--;
		} while (isalnum(*pos->data) || *pos->data == '_');
		const size_t sz = pos->data - start;
		if (0 == strncmp("return", start, sz)) {
			cur->token_type = TOKEN_KEYWORD_RETURN;
		} else if (0 == strncmp("void", start, sz)) {
			cur->token_type = TOKEN_KEYWORD_VOID;
		} else if (0 == strncmp("int", start, sz)) {
			cur->token_type = TOKEN_KEYWORD_INT;
		} else {
			cur->token_type = TOKEN_IDENTIFIER;
			check(lex_alloc_stringview(&cur->value, start, sz));
			check(lex_peek_ok(pos, cur->value));
		}
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
		check(lex_one_token(&code, tok));
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

static void
debug_lex_one(struct token *tok)
{
	switch (tok->token_type) {
	case TOKEN_IDENTIFIER:
		assert(tok->value != NULL);
		debug("IDENTIFIER %.*s", (int)tok->value->sz, tok->value->data);
		break;
	case TOKEN_CONSTANT:
		assert(tok->value != NULL);
		debug("CONSTANT %.*s", (int)tok->value->sz, tok->value->data);
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
	}
}

void
debug_lex_output(struct token *tok)
{
	while (tok != NULL) {
		debug_lex_one(tok);
		tok = tok->next;
	}
}
