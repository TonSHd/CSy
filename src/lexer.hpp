// CSy Lexer - tokenizes CSy source code.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace csy {

enum class TokenType {
    INT, FLOAT, STRING, IDENT,

    KW_INC, // #inc
    KW_CONNECT, // #connect
    KW_EXPLAIN, // #explain
    KW_FUNC, // func
    KW_IF, // if
    KW_ELIF, // elif
    KW_ELSE, // else
    KW_WHILE, // while
    KW_FOR, // for
    KW_IN, // in
    KW_RT, // rt
    KW_USING, // using
    KW_AS, // as
    KW_NONE, // none
    KW_TRUE, // true
    KW_FALSE, // false
    KW_RANGE, // range
    KW_THRU, // thru
    KW_CLASS,  // class
    KW_STOP, // stop
    KW_SKIP, // skip
    KW_EXTENDS, // extends
    KW_TRY, // try
    KW_CATCH, // catch

    SEMI, // ;
    COLON, // :
    DOT, // . (legacy)
    DCOLON, // ::
    COMMA, // ,
    LBRACE, // {
    RBRACE, // }
    LPAREN, // (
    RPAREN, // )
    LBRACKET, // [
    RBRACKET, // ]
    EXT, // ::ext

    ASSIGN, // =
    PLUS, // +
    MINUS, // -
    STAR, // *
    SLASH, // /
    PERCENT, // %
    PLUS_EQ, // +=
    MINUS_EQ, // -=
    GT, // >
    LT, // <
    GE, // >=
    LE, // <=
    EQ, // ==
    NEQ, // !=
    AND, // &&
    OR, // ||
    NOT, // !

    END_OF_FILE,
    NEWLINE,
};

const std::string* intern_file(const std::string& path);

struct Token {
    TokenType type;
    std::string text;
    const std::string* file_ptr;
    int line;
    int col;

    Token() : type(TokenType::END_OF_FILE), file_ptr(nullptr), line(0), col(0) {}
    Token(TokenType t, std::string tx, const std::string* f, int l, int c)
        : type(t), text(std::move(tx)), file_ptr(f), line(l), col(c) {}

    const std::string& file() const;
    std::string loc() const;
    std::string type_name() const;
};

struct NodeTok {
    const std::string* file_ptr = nullptr;
    int line = 0;
    int col = 0;

    NodeTok() = default;
    NodeTok(const Token& t) : file_ptr(t.file_ptr), line(t.line), col(t.col) {}
    operator Token() const { return Token(TokenType::END_OF_FILE, std::string(), file_ptr, line, col); }

    const std::string& file() const;
    std::string loc() const;
};

struct LexResult {
    std::vector<Token> tokens;
    bool ok = true;
};

constexpr size_t kMaxTokensPerFile = 48u * 1000u * 1000u;

LexResult lex(const std::string& source, const std::string& file);

std::string token_type_name(TokenType t);

bool is_keyword(const std::string& word);

}
