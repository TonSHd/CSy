#include "pi.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef CSY_WITH_GMP
#include <gmpxx.h>
#include <mutex>
#endif

namespace csy {

#ifdef CSY_WITH_GMP
namespace {

struct Split {
    mpz_class p;
    mpz_class q;
    mpz_class t;
};

using Clock = std::chrono::steady_clock;

struct GmpMemoryStats {
    std::atomic<std::size_t> allocations{0};
    std::atomic<std::size_t> reallocations{0};
    std::atomic<std::size_t> frees{0};
    std::atomic<std::size_t> total_allocated{0};
    std::atomic<std::size_t> current_bytes{0};
    std::atomic<std::size_t> peak_bytes{0};
    std::atomic<long long> allocation_time_ns{0};
};

GmpMemoryStats gmp_memory_stats;
void* (*gmp_original_alloc)(std::size_t) = nullptr;
void* (*gmp_original_realloc)(void*, std::size_t, std::size_t) = nullptr;
void (*gmp_original_free)(void*, std::size_t) = nullptr;

void update_peak(std::size_t current) {
    std::size_t peak = gmp_memory_stats.peak_bytes.load(std::memory_order_relaxed);
    while (current > peak &&
           !gmp_memory_stats.peak_bytes.compare_exchange_weak(
               peak, current, std::memory_order_relaxed)) {}
}

void* tracked_gmp_alloc(std::size_t size) {
    const auto started = Clock::now();
    void* ptr = gmp_original_alloc(size);
    gmp_memory_stats.allocation_time_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count(),
        std::memory_order_relaxed);
    if (ptr) {
        gmp_memory_stats.allocations.fetch_add(1, std::memory_order_relaxed);
        gmp_memory_stats.total_allocated.fetch_add(size, std::memory_order_relaxed);
        const std::size_t current =
            gmp_memory_stats.current_bytes.fetch_add(size, std::memory_order_relaxed) + size;
        update_peak(current);
    }
    return ptr;
}

void* tracked_gmp_realloc(void* ptr, std::size_t old_size, std::size_t new_size) {
    const auto started = Clock::now();
    void* resized = gmp_original_realloc(ptr, old_size, new_size);
    gmp_memory_stats.allocation_time_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count(),
        std::memory_order_relaxed);
    if (resized) {
        gmp_memory_stats.reallocations.fetch_add(1, std::memory_order_relaxed);
        if (new_size > old_size) {
            const std::size_t growth = new_size - old_size;
            gmp_memory_stats.total_allocated.fetch_add(growth, std::memory_order_relaxed);
            const std::size_t current =
                gmp_memory_stats.current_bytes.fetch_add(growth, std::memory_order_relaxed) + growth;
            update_peak(current);
        } else {
            gmp_memory_stats.current_bytes.fetch_sub(old_size - new_size, std::memory_order_relaxed);
        }
    }
    return resized;
}

void tracked_gmp_free(void* ptr, std::size_t size) {
    if (ptr) {
        const auto started = Clock::now();
        gmp_memory_stats.frees.fetch_add(1, std::memory_order_relaxed);
        gmp_memory_stats.current_bytes.fetch_sub(size, std::memory_order_relaxed);
        gmp_original_free(ptr, size);
        gmp_memory_stats.allocation_time_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count(),
            std::memory_order_relaxed);
    }
}

void enable_gmp_memory_tracking() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        mp_get_memory_functions(&gmp_original_alloc, &gmp_original_realloc,
                                &gmp_original_free);
        mp_set_memory_functions(tracked_gmp_alloc, tracked_gmp_realloc,
                                tracked_gmp_free);
    });
}

double elapsed_ms(Clock::time_point started, Clock::time_point finished) {
    return std::chrono::duration<double, std::milli>(finished - started).count();
}

Split chudnovsky_split(unsigned long a, unsigned long b) {
    if (b - a == 1) {
        if (a == 0) return {1, 1, 13591409};

        mpz_class p = (6 * a - 5) * (2 * a - 1) * (6 * a - 1);
        mpz_class q = mpz_class(a) * a * a * mpz_class("10939058860032000");
        mpz_class t = p * (mpz_class(13591409) + mpz_class(545140134) * a);
        if (a & 1) t = -t;
        return {std::move(p), std::move(q), std::move(t)};
    }

    const unsigned long mid = a + (b - a) / 2;
    Split left = chudnovsky_split(a, mid);
    Split right = chudnovsky_split(mid, b);
    return {
        left.p * right.p,
        left.q * right.q,
        left.t * right.q + left.p * right.t
    };
}

}
#endif

std::string calculate_pi_digits(std::size_t digits) {
#ifndef CSY_WITH_GMP
    (void)digits;
    throw std::runtime_error(
        "bc::pi_digits requires GMP; rebuild with the GMP C++ development library installed");
#else
    if (digits == 0) {
        throw std::runtime_error("bc::pi_digits requires at least 1 decimal digit");
    }

    constexpr std::size_t guard_digits = 12;
    if (digits > std::numeric_limits<std::size_t>::max() - guard_digits) {
        throw std::runtime_error("bc::pi_digits digit count is too large");
    }
    const std::size_t work_digits = digits + guard_digits;
    if (work_digits > std::numeric_limits<unsigned long>::max() - 14) {
        throw std::runtime_error("bc::pi_digits digit count is too large for this platform");
    }

    const unsigned long terms =
        static_cast<unsigned long>(work_digits / 14 + 1);
    const unsigned long exponent = static_cast<unsigned long>(work_digits);
    const unsigned long scale_exponent = static_cast<unsigned long>(guard_digits);

    const bool profile = std::getenv("CSY_PROFILE") != nullptr;
    const bool profile_memory = std::getenv("CSY_PROFILE_ALLOC") != nullptr;
    if (profile_memory) enable_gmp_memory_tracking();
    const auto total_started = Clock::now();
    mpz_class scale;
    mpz_ui_pow_ui(scale.get_mpz_t(), 10, exponent);
    const auto scale_finished = Clock::now();
    Split sum = chudnovsky_split(0, terms);
    const auto split_finished = Clock::now();

    mpz_class radicand = 10005 * scale * scale;
    mpz_class root;
    mpz_sqrt(root.get_mpz_t(), radicand.get_mpz_t());

    mpz_class scaled_pi = 426880 * root * sum.q / sum.t;
    mpz_class guard_scale;
    mpz_ui_pow_ui(guard_scale.get_mpz_t(), 10, scale_exponent);
    scaled_pi /= guard_scale;
    const auto arithmetic_finished = Clock::now();

    std::string digits_string = scaled_pi.get_str();
    const auto conversion_finished = Clock::now();
    if (digits_string.size() < digits + 1) {
        digits_string.insert(0, digits + 1 - digits_string.size(), '0');
    }
    digits_string.insert(1, 1, '.');
    const auto total_finished = Clock::now();
    if (profile) {
        std::fprintf(stderr,
                     "[csy-profile] pi_scale_ms=%.3f pi_split_ms=%.3f "
                     "pi_arithmetic_ms=%.3f pi_decimal_conversion_ms=%.3f "
                     "pi_format_ms=%.3f pi_total_ms=%.3f\n",
                     elapsed_ms(total_started, scale_finished),
                     elapsed_ms(scale_finished, split_finished),
                     elapsed_ms(split_finished, arithmetic_finished),
                     elapsed_ms(arithmetic_finished, conversion_finished),
                     elapsed_ms(conversion_finished, total_finished),
                     elapsed_ms(total_started, total_finished));
    }
    if (profile_memory) {
        std::fprintf(stderr,
                     "[csy-profile] gmp_allocations=%zu gmp_reallocations=%zu "
                     "gmp_frees=%zu gmp_allocation_time_ns=%lld "
                     "gmp_total_allocated_bytes=%zu gmp_peak_live_bytes=%zu\n",
                     gmp_memory_stats.allocations.load(std::memory_order_relaxed),
                     gmp_memory_stats.reallocations.load(std::memory_order_relaxed),
                     gmp_memory_stats.frees.load(std::memory_order_relaxed),
                     gmp_memory_stats.allocation_time_ns.load(std::memory_order_relaxed),
                     gmp_memory_stats.total_allocated.load(std::memory_order_relaxed),
                     gmp_memory_stats.peak_bytes.load(std::memory_order_relaxed));
    }
    return digits_string;
#endif
}

}

