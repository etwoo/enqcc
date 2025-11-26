#ifndef COMPILER_PASSES_LEX_H
#define COMPILER_PASSES_LEX_H

#include "sys/string_view.h"

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

#define FOREACH_LEX_CHAR_REPEAT(F)                                             \
	F(TOKEN_HYPHEN, TOKEN_HYPHEN_HYPHEN)                                   \
	F(TOKEN_PLUS_SIGN, TOKEN_PLUS_SIGN_PLUS_SIGN)                          \
	F(TOKEN_AMPERSAND, TOKEN_AMPERSAND_AMPERSAND)                          \
	F(TOKEN_VERT_BAR, TOKEN_VERT_BAR_VERT_BAR)                             \
	F(TOKEN_EQUAL_SIGN, TOKEN_EQUAL_SIGN_EQUAL_SIGN)                       \
	F(TOKEN_LESS_THAN, TOKEN_LESS_THAN_LESS_THAN)                          \
	F(TOKEN_MORE_THAN, TOKEN_MORE_THAN_MORE_THAN)

#define FOREACH_LEX_COMPOUND_ASSIGNMENT(F)                                     \
	F(TOKEN_HYPHEN, TOKEN_HYPHEN_EQUAL_SIGN)                               \
	F(TOKEN_PLUS_SIGN, TOKEN_PLUS_SIGN_EQUAL_SIGN)                         \
	F(TOKEN_ASTERISK, TOKEN_ASTERISK_EQUAL_SIGN)                           \
	F(TOKEN_FORWARD_SLASH, TOKEN_FORWARD_SLASH_EQUAL_SIGN)                 \
	F(TOKEN_PERCENT_SIGN, TOKEN_PERCENT_SIGN_EQUAL_SIGN)                   \
	F(TOKEN_EXCLAMATION, TOKEN_EXCLAMATION_EQUAL_SIGN)                     \
	F(TOKEN_AMPERSAND, TOKEN_AMPERSAND_EQUAL_SIGN)                         \
	F(TOKEN_VERT_BAR, TOKEN_VERT_BAR_EQUAL_SIGN)                           \
	F(TOKEN_LESS_THAN, TOKEN_LESS_THAN_EQUAL_SIGN)                         \
	F(TOKEN_MORE_THAN, TOKEN_MORE_THAN_EQUAL_SIGN)                         \
	F(TOKEN_CARET, TOKEN_CARET_EQUAL_SIGN)

#define FOREACH_LEX_COMBINED(F)                                                \
	FOREACH_LEX_KEYWORD(F)                                                 \
	FOREACH_LEX_CHAR(F)                                                    \
	FOREACH_LEX_CHAR_REPEAT(F)                                             \
	FOREACH_LEX_COMPOUND_ASSIGNMENT(F)
#define TO_ENUM(x, enum_value) enum_value,

struct token {
	enum {
		TOKEN_IDENTIFIER,
		TOKEN_CONSTANT,
		TOKEN_LESS_THAN_LESS_THAN_EQUAL_SIGN,
		TOKEN_MORE_THAN_MORE_THAN_EQUAL_SIGN,
		FOREACH_LEX_COMBINED(TO_ENUM)
	} token_type;
	struct string_view val;
	struct token *next;
};

#undef FOREACH_LEX_COMBINED
#undef TO_ENUM

#endif
