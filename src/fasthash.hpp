#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace csy {

inline uint64_t hash64_fast(const void* data, size_t size, uint64_t seed = 0) {
    constexpr uint64_t P1 = 11400714785074694791ULL;
    constexpr uint64_t P2 = 14029467366897019727ULL;
    constexpr uint64_t P3 = 1609587929392839161ULL;
    constexpr uint64_t P4 = 9650029242287828579ULL;
    constexpr uint64_t P5 = 2870177450012600261ULL;

    auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
    auto read64 = [](const unsigned char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; };
    auto read32 = [](const unsigned char* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    auto round = [&](uint64_t acc, uint64_t input) {
        acc += input * P2;
        acc = rotl(acc, 31);
        return acc * P1;
    };
    auto merge = [&](uint64_t acc, uint64_t val) {
        acc ^= round(0, val);
        return acc * P1 + P4;
    };

    const unsigned char* p = static_cast<const unsigned char*>(data);
    const unsigned char* const end = p + size;
    uint64_t h;

    if (size >= 32) {
        uint64_t v1 = seed + P1 + P2;
        uint64_t v2 = seed + P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - P1;
        const unsigned char* const limit = end - 32;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1);
        h = merge(h, v2);
        h = merge(h, v3);
        h = merge(h, v4);
    } else {
        h = seed + P5;
    }

    h += static_cast<uint64_t>(size);

    while (p + 8 <= end) {
        h ^= round(0, read64(p));
        h = rotl(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read32(p)) * P1;
        h = rotl(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * P5;
        h = rotl(h, 11) * P1;
        p++;
    }

    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

}
