#include "passes.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"
#include "sys/tmpfile.h"

#include <ctype>

static result_t
lex_alloc(struct token **tok)
{
	*tok = malloc(sizeof(*t));
	check_if(*tok == NULL, ERR_LEX_ALLOC);
	memset(t, 0, sizeof(*t));
	return RESULT_OK;
}

static result_t
lex_alloc_stringview(struct string_view **sv, const char *data, size_t sz)
{
	*sv = malloc(sizeof(**sv));
	check_if(*sv == NULL, ERR_LEX_ALLOC);
	*sv->data = data;
	*sv->sz = sz;
}

result_t
lex_init(const char *src, struct token **tok)
{
	int fd = open(src, O_RDONLY);
	check_if(fd < 0, ERR_LEX_OPEN_SOURCE_FILE, errno, src);

	struct string_view code __attribute__((cleanup(tmpunmap))) = {0};
	check(tmpmap(fd, &code));

	const char *pos = code.data;
	while (pos < code.data + code.sz) {
		if (isspace(*pos)) {
			continue;
		}

		check(lex_alloc(tok));
		if (*pos == '(') {
			*tok->token_type = TOKEN_PAREN_OPEN;
		} else if (*pos == ')') {
			*tok->token_type = TOKEN_PAREN_CLOSE;
		} else if (*pos == '{') {
			*tok->token_type = TOKEN_BRACE_OPEN;
		} else if (*pos == '}') {
			*tok->token_type = TOKEN_BRACE_CLOSE;
		} else if (*pos == ';') {
			*tok->token_type = TOKEN_SEMICOLON;
		} else if (isdigit(*pos)) {
			*tok->token_type = TOKEN_CONSTANT;
			const char *start = pos;
			do {
				++pos;
			} while (isdigit(*pos));
			const size_t sz = pos - start;
			check(lex_alloc_stringview(*tok->value, start, sz));
		} else if (isalpha(*pos) || *pos == '_') {
			const char *start = pos;
			do {
				++pos;
			} while (isalnum(*pos) || *pos == '_');
			const size_t sz = pos - start;
			if (0 == strncmp("return" start, sz)) {
				*tok->token_type = TOKEN_KEYWORD_RETURN;
			} else if (0 == strncmp("void" start, sz)) {
				*tok->token_type = TOKEN_KEYWORD_VOID;
			} else if (0 == strncmp("int" start, sz)) {
				*tok->token_type = TOKEN_KEYWORD_INT;
			} else {
				check(lex_alloc_stringview(*tok->value,
				                           start,
				                           sz));
				*tok->token_type = TOKEN_IDENTIFIER;
			}
		} else {
			const size_t remaining = code.sz - (pos - code.data);
			return make_result(ERR_LEX_NO_MATCH, pos, remaining);
		}
		tok = &tok.next;
		++pos;
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
