#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace csy {
namespace json {

bool valid_utf8(const char* data, size_t size) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;
    while (i < size) {
        const unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t need;
        unsigned cp;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + need >= size) return false;
        for (size_t k = 1; k <= need; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        i += need + 1;
    }
    return true;
}

namespace {

class Parser {
public:
    Parser(const char* data, size_t size, size_t first_line) : begin_(data), p_(data), end_(data + size), first_line_(first_line) {}

    Value run() {
        if (end_ - p_ >= 3 && static_cast<unsigned char>(p_[0]) == 0xEF &&
            static_cast<unsigned char>(p_[1]) == 0xBB && static_cast<unsigned char>(p_[2]) == 0xBF) {
            p_ += 3;
        }
        skip_ws();
        if (p_ == end_) fail("empty input, expected a JSON value", p_);
        Value v = value(0);
        skip_ws();
        if (p_ != end_) fail("unexpected data after the JSON value", p_);
        return v;
    }

private:
    const char* begin_;
    const char* p_;
    const char* end_;
    size_t first_line_;

    [[noreturn]] void fail(const std::string& what, const char* at) const {
        size_t line = first_line_, col = 1;
        for (const char* q = begin_; q < at && q < end_; q++) {
            if (*q == '\n') { line++; col = 1; }
            else col++;
        }
        throw std::runtime_error("json: " + what + " (line " + std::to_string(line) +
                                 ", column " + std::to_string(col) + ")");
    }

    void skip_ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
    }

    bool match(const char* word) {
        const size_t n = std::strlen(word);
        if (static_cast<size_t>(end_ - p_) >= n && std::memcmp(p_, word, n) == 0) {
            p_ += n;
            return true;
        }
        return false;
    }

    Value value(int depth) {
        if (depth > kMaxDepth) fail("nesting is deeper than " + std::to_string(kMaxDepth) + " levels", p_);
        if (p_ >= end_) fail("unexpected end of input", p_);
        const char c = *p_;
        switch (c) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return Value::make_str(string());
            case 't':
                if (match("true")) return Value::make_bool(true);
                fail("invalid literal", p_);
            case 'f':
                if (match("false")) return Value::make_bool(false);
                fail("invalid literal", p_);
            case 'n':
                if (match("null")) return Value::make_none();
                fail("invalid literal", p_);
            case 'N':
                if (match("NaN")) return Value::make_float(std::nan(""));
                fail("invalid literal", p_);
            case 'I':
                if (match("Infinity")) return Value::make_float(INFINITY);
                fail("invalid literal", p_);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return number();
                fail(std::string("unexpected character '") + c + "'", p_);
        }
    }

    Value number() {
        const char* start = p_;
        if (*p_ == '-') {
            p_++;
            if (match("Infinity")) return Value::make_float(-INFINITY);
        }
        if (p_ >= end_) fail("incomplete number", start);
        if (*p_ == '0') {
            p_++;
            if (p_ < end_ && *p_ >= '0' && *p_ <= '9') fail("numbers cannot have leading zeros", start);
        } else if (*p_ >= '1' && *p_ <= '9') {
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        } else {
            fail("invalid number", start);
        }
        bool is_float = false;
        if (p_ < end_ && *p_ == '.') {
            is_float = true;
            p_++;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') fail("a digit is required after the decimal point", start);
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            is_float = true;
            p_++;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) p_++;
            if (p_ >= end_ || *p_ < '0' || *p_ > '9') fail("a digit is required in the exponent", start);
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') p_++;
        }
        if (!is_float) {
            long long iv = 0;
            const auto r = std::from_chars(start, p_, iv);
            if (r.ec == std::errc() && r.ptr == p_) return Value::make_int(iv);
        }
        double dv = 0.0;
        const auto r = std::from_chars(start, p_, dv);
        if (r.ec == std::errc::result_out_of_range) fail("number is out of range", start);
        if (r.ec != std::errc() || r.ptr != p_) fail("invalid number", start);
        return Value::make_float(dv);
    }

    unsigned hex4() {
        if (end_ - p_ < 4) fail("incomplete \\u escape", p_);
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            const char h = p_[i];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
            else fail("invalid \\u escape", p_);
        }
        p_ += 4;
        return v;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string string() {
        const char* open = p_;
        p_++;
        std::string out;
        for (;;) {
            const char* run = p_;
            while (p_ < end_ && *p_ != '"' && *p_ != '\\' && static_cast<unsigned char>(*p_) >= 0x20) p_++;
            if (p_ > run) {
                if (!valid_utf8(run, static_cast<size_t>(p_ - run))) fail("string is not valid UTF-8", run);
                out.append(run, static_cast<size_t>(p_ - run));
            }
            if (p_ >= end_) fail("unterminated string", open);
            const char c = *p_;
            if (c == '"') { p_++; return out; }
            if (c != '\\') fail("control character in string, it must be escaped", p_);
            p_++;
            if (p_ >= end_) fail("unterminated string", open);
            const char e = *p_++;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    const char* at = p_ - 2;
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                            p_ += 2;
                            const unsigned lo = hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid surrogate pair", at);
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            fail("lone surrogate in \\u escape", at);
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("lone surrogate in \\u escape", at);
                    }
                    put_utf8(out, cp);
                    break;
                }
                default:
                    fail(std::string("invalid escape '\\") + e + "'", p_ - 2);
            }
        }
    }

    Value array(int depth) {
        p_++;
        std::vector<Value> items;
        skip_ws();
        if (p_ < end_ && *p_ == ']') { p_++; return Value::make_vec(std::move(items)); }
        for (;;) {
            skip_ws();
            items.push_back(value(depth + 1));
            skip_ws();
            if (p_ >= end_) fail("unterminated array", p_);
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == ']') { p_++; break; }
            fail("expected ',' or ']' in array", p_);
        }
        return Value::make_vec(std::move(items));
    }

    Value object(int depth) {
        p_++;
        std::unordered_map<std::string, Value> members;
        skip_ws();
        if (p_ < end_ && *p_ == '}') { p_++; return Value::make_dict(std::move(members)); }
        for (;;) {
            skip_ws();
            if (p_ >= end_ || *p_ != '"') fail("object keys must be strings", p_);
            std::string key = string();
            skip_ws();
            if (p_ >= end_ || *p_ != ':') fail("expected ':' after object key", p_);
            p_++;
            skip_ws();
            Value v = value(depth + 1);
            members[std::move(key)] = std::move(v);
            skip_ws();
            if (p_ >= end_) fail("unterminated object", p_);
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == '}') { p_++; break; }
            fail("expected ',' or '}' in object", p_);
        }
        return Value::make_dict(std::move(members));
    }
};

void put_string(std::string& out, const std::string& s) {
    if (!valid_utf8(s.data(), s.size())) {
        throw std::runtime_error("json: a string value is not valid UTF-8 and cannot be written as JSON");
    }
    out.push_back('"');
    static const char* hex = "0123456789abcdef";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void put_double(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "null"; return; }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof(buf), d);
    const size_t n = static_cast<size_t>(r.ptr - buf);
    bool plain = true;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { plain = false; break; }
    }
    out.append(buf, n);
    if (plain) out += ".0";
}

void newline(std::string& out, int level) {
    out.push_back('\n');
    out.append(static_cast<size_t>(level) * 2, ' ');
}

void write(std::string& out, const Value& v, bool pretty, int level) {
    if (level > kMaxDepth) {
        throw std::runtime_error("json: nesting is deeper than " + std::to_string(kMaxDepth) +
                                 " levels (does a value contain itself?)");
    }
    if (out.size() > kMaxTextBytes) throw std::runtime_error("json: output is larger than the 1 GiB safety limit");
    switch (v.type) {
        case Value::None: out += "null"; return;
        case Value::Bool: out += v.bool_val ? "true" : "false"; return;
        case Value::Int: {
            char buf[24];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v.int_val);
            out.append(buf, static_cast<size_t>(r.ptr - buf));
            return;
        }
        case Value::Float: put_double(out, v.float_val); return;
        case Value::Str: put_string(out, *v.str_val); return;
        case Value::F64Array: {
            const auto& a = *v.f64_array_val;
            if (a.empty()) { out += "[]"; return; }
            out.push_back('[');
            for (size_t i = 0; i < a.size(); i++) {
                if (i) out += pretty ? ", " : ",";
                put_double(out, a[i]);
            }
            out.push_back(']');
            return;
        }
        case Value::Vec: {
            const auto& items = *v.vec_val;
            if (items.empty()) { out += "[]"; return; }
            out.push_back('[');
            for (size_t i = 0; i < items.size(); i++) {
                if (i) out.push_back(',');
                if (pretty) newline(out, level + 1);
                write(out, items[i], pretty, level + 1);
            }
            if (pretty) newline(out, level);
            out.push_back(']');
            return;
        }
        case Value::Dict: {
            const auto& m = *v.dict_val;
            if (m.empty()) { out += "{}"; return; }
            std::vector<const std::pair<const std::string, Value>*> entries;
            entries.reserve(m.size());
            for (const auto& kv : m) entries.push_back(&kv);
            std::sort(entries.begin(), entries.end(),
                      [](const auto* a, const auto* b) { return a->first < b->first; });
            out.push_back('{');
            bool first = true;
            for (const auto* kv : entries) {
                if (!first) out.push_back(',');
                first = false;
                if (pretty) newline(out, level + 1);
                put_string(out, kv->first);
                out += pretty ? ": " : ":";
                write(out, kv->second, pretty, level + 1);
            }
            if (pretty) newline(out, level);
            out.push_back('}');
            return;
        }
        default:
            throw std::runtime_error("json: a " + v.type_name() + " value cannot be written as JSON");
    }
}

}

Value parse(const char* data, size_t size, size_t first_line) {
    if (size > kMaxTextBytes) throw std::runtime_error("json: input is larger than the 1 GiB safety limit");
    Parser parser(data, size, first_line);
    return parser.run();
}

void stringify_to(std::string& out, const Value& value, bool pretty) {
    write(out, value, pretty, 0);
}

std::string stringify(const Value& value, bool pretty) {
    std::string out;
    write(out, value, pretty, 0);
    return out;
}

}
}

