#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace csy {

enum class TokenType {
    INT, FLOAT, STRING, IDENT,

    KW_INC,
    KW_CONNECT,
    KW_EXPLAIN,
    KW_FUNC,
    KW_IF,
    KW_ELIF,
    KW_ELSE,
    KW_WHILE,
    KW_FOR,
    KW_IN,
    KW_RT,
    KW_USING,
    KW_AS,
    KW_NONE,
    KW_TRUE,
    KW_FALSE,
    KW_RANGE,
    KW_THRU,
    KW_CLASS,
    KW_STOP,
    KW_SKIP,
    KW_EXTENDS,
    KW_TRY,
    KW_CATCH,

    SEMI,
    COLON,
    DOT,
    DCOLON,
    COMMA,
    LBRACE,
    RBRACE,
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    EXT,

    ASSIGN,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    PLUS_EQ,
    MINUS_EQ,
    GT,
    LT,
    GE,
    LE,
    EQ,
    NEQ,
    AND,
    OR,
    NOT,

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

