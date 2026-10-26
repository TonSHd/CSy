#include "lexer.hpp"
#include "resource.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace csy {

const std::string* intern_file(const std::string& path) {
    static std::mutex mutex;
    static std::unordered_set<std::string> table;
    std::lock_guard<std::mutex> lock(mutex);
    return &*table.insert(path).first;
}

const std::string& Token::file() const {
    static const std::string unknown = "<unknown>";
    return file_ptr ? *file_ptr : unknown;
}

std::string Token::loc() const {
    return file() + ":" + std::to_string(line) + ":" + std::to_string(col);
}

const std::string& NodeTok::file() const {
    static const std::string unknown = "<unknown>";
    return file_ptr ? *file_ptr : unknown;
}

std::string NodeTok::loc() const {
    return file() + ":" + std::to_string(line) + ":" + std::to_string(col);
}

std::string Token::type_name() const {
    return token_type_name(type);
}

std::string token_type_name(TokenType t) {
    switch (t) {
        case TokenType::INT:     return "INT";
        case TokenType::FLOAT:   return "FLOAT";
        case TokenType::STRING:  return "STRING";
        case TokenType::IDENT:   return "IDENT";
        case TokenType::KW_INC:  return "#inc";
        case TokenType::KW_CONNECT: return "#connect";
        case TokenType::KW_EXPLAIN: return "#explain";
        case TokenType::KW_FUNC: return "func";
        case TokenType::KW_IF:   return "if";
        case TokenType::KW_ELIF: return "orf";
        case TokenType::KW_ELSE: return "else";
        case TokenType::KW_WHILE:return "while";
        case TokenType::KW_FOR:  return "for";
        case TokenType::KW_IN:   return "in";
        case TokenType::KW_THRU: return "thru";
        case TokenType::KW_RT:   return "rt";
        case TokenType::KW_USING:return "using";
        case TokenType::KW_AS:   return "as";
        case TokenType::KW_NONE: return "none";
        case TokenType::KW_TRUE: return "true";
        case TokenType::KW_FALSE:return "false";
        case TokenType::SEMI:    return ";";
        case TokenType::COLON:   return ":";
        case TokenType::DOT:     return ".";
        case TokenType::DCOLON:  return "::";
        case TokenType::COMMA:   return ",";
        case TokenType::LPAREN:  return "(";
        case TokenType::RPAREN:  return ")";
        case TokenType::LBRACKET:return "[";
        case TokenType::RBRACKET:return "]";
        case TokenType::LBRACE:  return "{";
        case TokenType::RBRACE:  return "}";
        case TokenType::KW_CLASS:return "class";
        case TokenType::KW_STOP: return "stop";
        case TokenType::KW_SKIP: return "skip";
        case TokenType::KW_EXTENDS: return "extends";
        case TokenType::KW_TRY:  return "try";
        case TokenType::KW_CATCH:return "catch";
        case TokenType::EXT:     return "::ext";
        case TokenType::ASSIGN:  return "=";
        case TokenType::PLUS:    return "+";
        case TokenType::MINUS:   return "-";
        case TokenType::STAR:    return "*";
        case TokenType::SLASH:   return "/";
        case TokenType::PERCENT: return "%";
        case TokenType::PLUS_EQ: return "+=";
        case TokenType::MINUS_EQ:return "-=";
        case TokenType::GT:      return ">";
        case TokenType::LT:      return "<";
        case TokenType::GE:      return ">=";
        case TokenType::LE:      return "<=";
        case TokenType::EQ:      return "==";
        case TokenType::NEQ:     return "!=";
        case TokenType::AND:     return "&&";
        case TokenType::OR:      return "||";
        case TokenType::NOT:     return "!";
        case TokenType::END_OF_FILE: return "EOF";
        default: return "?";
    }
}

static const std::unordered_map<std::string, TokenType> kKeywords = {
    {"func", TokenType::KW_FUNC},
    {"if",   TokenType::KW_IF},
    {"orf",  TokenType::KW_ELIF},
    {"else", TokenType::KW_ELSE},
    {"while",TokenType::KW_WHILE},
    {"for",  TokenType::KW_FOR},
    {"in",   TokenType::KW_IN},
    {"thru", TokenType::KW_THRU},
    {"class",TokenType::KW_CLASS},
    {"stop", TokenType::KW_STOP},
    {"skip", TokenType::KW_SKIP},
    {"extends", TokenType::KW_EXTENDS},
    {"try",  TokenType::KW_TRY},
    {"catch",TokenType::KW_CATCH},
    {"rt",   TokenType::KW_RT},
    {"using",TokenType::KW_USING},
    {"as",   TokenType::KW_AS},
    {"none", TokenType::KW_NONE},
    {"true", TokenType::KW_TRUE},
    {"false",TokenType::KW_FALSE},
};

bool is_keyword(const std::string& word) { return kKeywords.count(word) != 0; }

class LexerImpl {
public:
    LexerImpl(const std::string& src, const std::string& file)
        : src_(src), file_(file), file_ptr_(intern_file(file)) {
        tokens_.reserve(std::min<size_t>(src.size() / 4 + 16, 4u * 1000u * 1000u));
    }

    LexResult run() {
        LexResult result;
        try {
            while (pos_ < src_.size()) {
                char c = src_[pos_];
                if (c == '\n') {
                    line_++;
                    col_ = 1;
                    pos_++;
                    continue;
                }
                if (c == ' ' || c == '\t' || c == '\r') {
                    pos_++;
                    col_++;
                    continue;
                }
                if (c == '#') {
                    if (src_.compare(pos_, 4, "#inc") == 0 && !ident_char_at(pos_ + 4)) {
                        emit(TokenType::KW_INC, "#inc");
                        pos_ += 4;
                        col_ += 4;
                        continue;
                    }
                    if (src_.compare(pos_, 8, "#connect") == 0 && !ident_char_at(pos_ + 8)) {
                        emit(TokenType::KW_CONNECT, "#connect");
                        pos_ += 8;
                        col_ += 8;
                        continue;
                    }
                    if (src_.compare(pos_, 8, "#explain") == 0 && !ident_char_at(pos_ + 8)) {
                        emit(TokenType::KW_EXPLAIN, "#explain");
                        pos_ += 8;
                        col_ += 8;
                        continue;
                    }
                    // otherwise it's a comment - skip to end of line
                    while (pos_ < src_.size() && src_[pos_] != '\n') {
                        pos_++;
                    }
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    lexNumber();
                    continue;
                }
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                    lexIdent();
                    continue;
                }
                if (c == '"') {
                    lexString();
                    continue;
                }
                if (c == ':' && pos_ + 4 <= src_.size() && src_.compare(pos_, 5, "::ext") == 0) {
                    emit(TokenType::EXT, "::ext");
                    pos_ += 5;
                    col_ += 5;
                    continue;
                }
                if (c == ':' && pos_ + 1 < src_.size() && src_[pos_ + 1] == ':') {
                    emit(TokenType::DCOLON, "::");
                    pos_ += 2;
                    col_ += 2;
                    continue;
                }
                if (c == ':') {
                    emit(TokenType::COLON, ":");
                    pos_++; col_++;
                    continue;
                }
                if (c == ';') { emit(TokenType::SEMI, ";"); pos_++; col_++; continue; }
                if (c == '.') { emit(TokenType::DOT, "."); pos_++; col_++; continue; }
                if (c == ',') { emit(TokenType::COMMA, ","); pos_++; col_++; continue; }
                if (c == '(') { emit(TokenType::LPAREN, "("); pos_++; col_++; continue; }
                if (c == ')') { emit(TokenType::RPAREN, ")"); pos_++; col_++; continue; }
                if (c == '[') { emit(TokenType::LBRACKET, "["); pos_++; col_++; continue; }
                if (c == ']') { emit(TokenType::RBRACKET, "]"); pos_++; col_++; continue; }
                if (c == '{') { emit(TokenType::LBRACE, "{"); pos_++; col_++; continue; }
                if (c == '}') { emit(TokenType::RBRACE, "}"); pos_++; col_++; continue; }
                if (c == '+') {
                    if (peek(1) == '=') { emit(TokenType::PLUS_EQ, "+="); pos_+=2; col_+=2; }
                    else { emit(TokenType::PLUS, "+"); pos_++; col_++; }
                    continue;
                }
                if (c == '-') {
                    if (peek(1) == '=') { emit(TokenType::MINUS_EQ, "-="); pos_+=2; col_+=2; }
                    else { emit(TokenType::MINUS, "-"); pos_++; col_++; }
                    continue;
                }
                if (c == '*') { emit(TokenType::STAR, "*"); pos_++; col_++; continue; }
                if (c == '/') { emit(TokenType::SLASH, "/"); pos_++; col_++; continue; }
                if (c == '%') { emit(TokenType::PERCENT, "%"); pos_++; col_++; continue; }
                if (c == '=') {
                    if (peek(1) == '=') { emit(TokenType::EQ, "=="); pos_+=2; col_+=2; }
                    else { emit(TokenType::ASSIGN, "="); pos_++; col_++; }
                    continue;
                }
                if (c == '!') {
                    if (peek(1) == '=') { emit(TokenType::NEQ, "!="); pos_+=2; col_+=2; }
                    else { emit(TokenType::NOT, "!"); pos_++; col_++; }
                    continue;
                }
                if (c == '>') {
                    if (peek(1) == '=') { emit(TokenType::GE, ">="); pos_+=2; col_+=2; }
                    else { emit(TokenType::GT, ">"); pos_++; col_++; }
                    continue;
                }
                if (c == '<') {
                    if (peek(1) == '=') { emit(TokenType::LE, "<="); pos_+=2; col_+=2; }
                    else { emit(TokenType::LT, "<"); pos_++; col_++; }
                    continue;
                }
                if (c == '&') {
                    if (peek(1) == '&') { emit(TokenType::AND, "&&"); pos_+=2; col_+=2; }
                    else throw std::runtime_error("expected '&&' but got '&'");
                    continue;
                }
                if (c == '|') {
                    if (peek(1) == '|') { emit(TokenType::OR, "||"); pos_+=2; col_+=2; }
                    else throw std::runtime_error("expected '||' but got '|'");
                    continue;
                }
                throw std::runtime_error(describe_byte(c));
            }
            emit(TokenType::END_OF_FILE, "<eof>");
            result.ok = true;
        } catch (const std::runtime_error& e) {
            result.ok = false;
            result.tokens.clear();
            Token err;
            err.type = TokenType::END_OF_FILE;
            err.text = e.what();
            err.file_ptr = file_ptr_;
            err.line = line_;
            err.col = col_;
            result.tokens.push_back(err);
        }
        return result;
    }

private:
    const std::string& src_;
    const std::string& file_;
    const std::string* file_ptr_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;

    bool ident_char_at(size_t p) const {
        if (p >= src_.size()) return false;
        const unsigned char ch = static_cast<unsigned char>(src_[p]);
        return std::isalnum(ch) || ch == '_';
    }

    static std::string describe_byte(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f) {
            if (c == '\x27') return "unexpected character \x27 - CSy strings use double quotes (\"...\")";
            return std::string("unexpected character '") + c + "'";
        }
        static const char* hex = "0123456789abcdef";
        std::string out = "unexpected byte 0x";
        out += hex[u >> 4];
        out += hex[u & 15];
        out += u >= 0x80 ? " (non-ASCII text is only allowed inside comments and strings)"
                         : " (control character)";
        return out;
    }

    void count_token() {
        if (tokens_.size() >= kMaxTokensPerFile) {
            throw std::runtime_error("file has more than " + std::to_string(kMaxTokensPerFile) +
                                     " tokens - split it into smaller source files");
        }
        if ((tokens_.size() & 0x3fff) == 0x3fff) res::check("lexing");
        if (tokens_.size() == tokens_.capacity()) {
            const size_t grown = tokens_.capacity() + tokens_.capacity() / 2 + 1024;
            res::require_headroom("lexing", static_cast<uint64_t>(grown) * sizeof(Token));
            tokens_.reserve(grown);
        }
    }

    char peek(int ahead) const {
        size_t p = pos_ + ahead;
        if (p >= src_.size()) return '\0';
        return src_[p];
    }

    void emit(TokenType t, std::string text) {
        count_token();
        Token tok(t, std::move(text), file_ptr_, line_, col_);
        tokens_.push_back(std::move(tok));
    }

    void lexNumber() {
        int start_line = line_;
        int start_col = col_;
        size_t start = pos_;
        bool is_float = false;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            pos_++; col_++;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            if (pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_+1]))) {
                is_float = true;
                pos_++; col_++;
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                    pos_++; col_++;
                }
                if (pos_ < src_.size() && src_[pos_] == '.' &&
                    pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_+1]))) {
                    throw std::runtime_error(
                        "malformed number literal - too many decimal points");
                }
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            size_t p = pos_ + 1;
            if (p < src_.size() && (src_[p] == '+' || src_[p] == '-')) p++;
            if (p < src_.size() && std::isdigit(static_cast<unsigned char>(src_[p]))) {
                is_float = true;
                int consumed = (int)(p - pos_);
                pos_ = p;
                col_ += consumed;
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                    pos_++; col_++;
                }
            }
        }
        std::string num = src_.substr(start, pos_ - start);
        count_token();
        Token tok(is_float ? TokenType::FLOAT : TokenType::INT,
                  std::move(num), file_ptr_, start_line, start_col);
        tokens_.push_back(std::move(tok));
    }

    void lexIdent() {
        int start_line = line_;
        int start_col = col_;
        size_t start = pos_;
        while (pos_ < src_.size() &&
               (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
            pos_++; col_++;
        }
        std::string ident = src_.substr(start, pos_ - start);
        count_token();
        auto it = kKeywords.find(ident);
        TokenType t = (it != kKeywords.end()) ? it->second : TokenType::IDENT;
        Token tok(t, std::move(ident), file_ptr_, start_line, start_col);
        tokens_.push_back(std::move(tok));
    }

    void lexString() {
        int start_line = line_;
        int start_col = col_;
        pos_++; col_++;
        std::string value;
        while (pos_ < src_.size() && src_[pos_] != '"') {
            char c = src_[pos_];
            if (c == '\\' && pos_ + 1 < src_.size()) {
                char n = src_[pos_+1];
                switch (n) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    case '\\': value.push_back('\\'); break;
                    case '"': value.push_back('"'); break;
                    case '0': value.push_back('\0'); break;
                    default: value.push_back('\\'); value.push_back(n); break;
                }
                pos_ += 2;
                if (n == '\n') { line_++; col_ = 1; } else { col_ += 2; }
                continue;
            }
            if (c == '\n') { line_++; col_ = 1; }
            else col_++;
            value.push_back(c);
            pos_++;
        }
        if (pos_ >= src_.size()) {
            throw std::runtime_error("unterminated string literal");
        }
        pos_++; col_++;
        count_token();
        Token tok(TokenType::STRING, std::move(value), file_ptr_, start_line, start_col);
        tokens_.push_back(std::move(tok));
    }

public:
    std::vector<Token> tokens_;
};

LexResult lex(const std::string& source, const std::string& file) {
    LexerImpl impl(source, file);
    LexResult r = impl.run();
    if (r.ok) {
        r.tokens = std::move(impl.tokens_);
    }
    return r;
}

}