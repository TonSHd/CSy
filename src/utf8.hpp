#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace csy {

inline std::size_t utf8_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 1;
}

inline std::size_t utf8_step(const std::string& s, std::size_t i) {
    std::size_t n = utf8_length(static_cast<unsigned char>(s[i]));
    if (i + n > s.size()) return 1;
    for (std::size_t k = 1; k < n; k++)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 1;
    return n;
}

inline std::size_t utf8_count(const std::string& s) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size(); i += utf8_step(s, i)) count++;
    return count;
}

inline std::string utf8_encode(long long cp) {
    std::string out;
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
    return out;
}

inline long long utf8_decode(const std::string& s, std::size_t i, std::size_t n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (n == 1) return c;
    long long cp = n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return cp;
}

class StrObj : public std::string {
public:
    StrObj() = default;
    StrObj(std::string s) : std::string(std::move(s)) {}
    StrObj(const StrObj&) = default;
    StrObj(StrObj&&) = default;

    void changed() { chars_ = kUnknown; ascii_ = false; hint_char_ = 0; hint_byte_ = 0; }

    std::size_t char_count() const {
        if (chars_ == kUnknown) measure();
        return chars_;
    }

    std::size_t byte_offset(std::size_t i) const {
        if (chars_ == kUnknown) measure();
        if (ascii_) return i;
        std::size_t c = 0, b = 0;
        if (i >= hint_char_) { c = hint_char_; b = hint_byte_; }
        while (c < i && b < size()) { b += utf8_step(*this, b); c++; }
        hint_char_ = c;
        hint_byte_ = b;
        return b;
    }

    std::size_t char_bytes_at(std::size_t b) const { return b < size() ? utf8_step(*this, b) : 0; }

    std::size_t char_index_of_byte(std::size_t b) const {
        if (chars_ == kUnknown) measure();
        if (ascii_) return b;
        std::size_t c = 0, at = 0;
        while (at < b && at < size()) { at += utf8_step(*this, at); c++; }
        return c;
    }

private:
    static constexpr std::size_t kUnknown = static_cast<std::size_t>(-1);

    void measure() const {
        const std::size_t n = size();
        const char* p = data();
        std::size_t i = 0;
        bool ascii = true;
        for (; i + 8 <= n; i += 8) {
            std::uint64_t w;
            std::memcpy(&w, p + i, 8);
            if (w & 0x8080808080808080ULL) { ascii = false; break; }
        }
        if (ascii) {
            for (; i < n; i++) if (static_cast<unsigned char>(p[i]) & 0x80) { ascii = false; break; }
        }
        ascii_ = ascii;
        chars_ = ascii ? n : utf8_count(*this);
        hint_char_ = 0;
        hint_byte_ = 0;
    }

    mutable std::size_t chars_ = kUnknown;
    mutable bool ascii_ = false;
    mutable std::size_t hint_char_ = 0;
    mutable std::size_t hint_byte_ = 0;
};

}
