#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace csy {

struct Hash128 { uint64_t a = 0, b = 0; };

inline Hash128 hash128(const char* data, size_t size) {
    Hash128 h;
    h.a = 1469598103934665603ULL;
    h.b = 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < size; i++) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        h.a = (h.a ^ c) * 1099511628211ULL;
        h.b = (h.b ^ (c + 0x5bU)) * 0x100000001b3ULL;
        h.b ^= h.b >> 29;
    }
    return h;
}
inline Hash128 hash128(const std::string& s) { return hash128(s.data(), s.size()); }
inline std::string hash128_hex(const std::string& s) {
    const Hash128 h = hash128(s);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)h.a, (unsigned long long)h.b);
    return buf;
}

}
