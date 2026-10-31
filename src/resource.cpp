#include "resource.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <malloc.h>
#include <mutex>
#include <new>
#include <memory>
#include <thread>
#include <pthread.h>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace csy {
namespace res {
namespace {

std::atomic<bool> g_tracking{false};
std::atomic<int64_t> g_bytes{0};
std::atomic<int64_t> g_peak{0};
std::atomic<uint64_t> g_limit{0};
std::atomic<bool> g_limit_hit{false};
std::atomic<bool> g_warned{false};
thread_local int64_t t_delta = 0;
constexpr int64_t kFlushBytes = 64 * 1024;

inline void flush_delta() {
    const int64_t d = t_delta;
    t_delta = 0;
    const int64_t now = g_bytes.fetch_add(d, std::memory_order_relaxed) + d;
    if (d > 0) {
        int64_t peak = g_peak.load(std::memory_order_relaxed);
        while (now > peak &&
               !g_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
    }
}

inline void note_alloc(void* p) {
    if (!g_tracking.load(std::memory_order_relaxed)) return;
    t_delta += static_cast<int64_t>(::malloc_usable_size(p));
    if (t_delta >= kFlushBytes) flush_delta();
}

inline void note_free(void* p) {
    if (!p || !g_tracking.load(std::memory_order_relaxed)) return;
    t_delta -= static_cast<int64_t>(::malloc_usable_size(p));
    if (t_delta <= -kFlushBytes) flush_delta();
}

inline bool over_hard_limit(size_t n) {
    if (!g_tracking.load(std::memory_order_relaxed)) return false;
    const uint64_t limit = g_limit.load(std::memory_order_relaxed);
    if (limit == 0) return false;
    const int64_t live = g_bytes.load(std::memory_order_relaxed) + t_delta;
    return live > 0 && static_cast<uint64_t>(live) + n > limit;
}

void* alloc_or_null(size_t n) {
    if (over_hard_limit(n)) {
        if (std::getenv("CSY_MEM_TRACE")) {
            std::fprintf(stderr, "[mem-trace] allocation of %zu bytes refused (live %lld, limit %llu)\n", n,
                         (long long)(g_bytes.load() + t_delta), (unsigned long long)g_limit.load());
        }
        g_limit_hit.store(true, std::memory_order_relaxed);
        return nullptr;
    }
    void* p = std::malloc(n ? n : 1);
    if (p) note_alloc(p);
    return p;
}

void* alloc_aligned_or_null(size_t n, size_t align) {
    if (over_hard_limit(n)) {
        g_limit_hit.store(true, std::memory_order_relaxed);
        return nullptr;
    }
    if (align < sizeof(void*)) align = sizeof(void*);
    void* p = nullptr;
    if (::posix_memalign(&p, align, n ? n : 1) != 0) p = nullptr;
    if (p) note_alloc(p);
    return p;
}

}

void set_tracking(bool enabled) {
    if (enabled) {
        g_bytes.store(0, std::memory_order_relaxed);
        g_peak.store(0, std::memory_order_relaxed);
        t_delta = 0;
        g_limit_hit.store(false, std::memory_order_relaxed);
        g_warned.store(false, std::memory_order_relaxed);
    }
    g_tracking.store(enabled, std::memory_order_relaxed);
}
bool tracking() { return g_tracking.load(std::memory_order_relaxed); }
void set_limit_bytes(uint64_t bytes) { g_limit.store(bytes, std::memory_order_relaxed); }
uint64_t limit_bytes() { return g_limit.load(std::memory_order_relaxed); }
bool hard_limit_hit() { return g_limit_hit.load(std::memory_order_relaxed); }

uint64_t tracked_bytes() {
    const int64_t live = g_bytes.load(std::memory_order_relaxed) + t_delta;
    return live > 0 ? static_cast<uint64_t>(live) : 0;
}
uint64_t peak_tracked_bytes() {
    const int64_t peak = g_peak.load(std::memory_order_relaxed);
    return std::max<int64_t>(peak, static_cast<int64_t>(tracked_bytes())) > 0
        ? static_cast<uint64_t>(std::max<int64_t>(peak, static_cast<int64_t>(tracked_bytes()))) : 0;
}

uint64_t rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    const int n = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    if (n != 2) return 0;
    const long page = ::sysconf(_SC_PAGESIZE);
    return resident * static_cast<unsigned long long>(page > 0 ? page : 4096);
}

void flush_thread() {
    if (g_tracking.load(std::memory_order_relaxed)) flush_delta();
}

namespace {
std::string mib(uint64_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MiB"; }

enum class Level { Ok, Warn, Abort };
Level level_now(uint64_t& used, uint64_t& limit) {
    limit = g_limit.load(std::memory_order_relaxed);
    if (limit == 0 || !g_tracking.load(std::memory_order_relaxed)) return Level::Ok;
    used = tracked_bytes();
    if (used >= limit - limit / 20) return Level::Abort;
    if (used >= limit - limit / 5) return Level::Warn;
    return Level::Ok;
}
}

void check(const char* stage) {
    static const bool trace = std::getenv("CSY_MEM_TRACE") != nullptr;
    if (trace) std::cerr << "[mem-trace] " << stage << ": " << mib(tracked_bytes()) << "\n";
    uint64_t used = 0, limit = 0;
    switch (level_now(used, limit)) {
        case Level::Ok: return;
        case Level::Warn:
            if (!g_warned.exchange(true)) {
                std::cerr << "warning: compiler memory is at least 80% used during " << stage
                          << " (" << mib(used) << " of " << mib(limit) << " budget)\n";
            }
            return;
        case Level::Abort:
            throw BudgetExceeded("compiler memory budget is at least 95% used during " +
                                 std::string(stage) + " (" + mib(used) + " of " + mib(limit) +
                                 "); stopping cleanly - raise --memory-limit or split the project");
    }
}

void require_headroom(const char* stage, uint64_t additional_bytes) {
    uint64_t used = 0, limit = 0;
    limit = g_limit.load(std::memory_order_relaxed);
    if (limit == 0 || !g_tracking.load(std::memory_order_relaxed)) return;
    used = tracked_bytes();
    const uint64_t projected = used + additional_bytes;
    if (projected >= limit - limit / 20) {
        throw BudgetExceeded("the next step of " + std::string(stage) + " needs about " + mib(additional_bytes) +
                             " more, which would take compiler memory from " + mib(used) + " to " + mib(projected) +
                             " of the " + mib(limit) + " budget (abort threshold is 95%); stopping cleanly - raise --memory-limit or split the project");
    }
    if (projected >= limit - limit / 5 && !g_warned.exchange(true)) {
        std::cerr << "warning: compiler memory will reach at least 80% of its " << mib(limit)
                  << " budget during " << stage << " (" << mib(projected) << " projected)\n";
    }
}

bool within_budget(const char* stage) {
    uint64_t used = 0, limit = 0;
    switch (level_now(used, limit)) {
        case Level::Ok: return true;
        case Level::Warn:
            if (!g_warned.exchange(true)) {
                std::cerr << "warning: compiler memory is at least 80% used during " << stage
                          << " (" << mib(used) << " of " << mib(limit) << " budget)\n";
            }
            return true;
        case Level::Abort: return false;
    }
    return true;
}

namespace {

std::string private_dir() {
    const std::string dir = "/tmp/csyc-" + std::to_string(::geteuid());
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return {};
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) return {};
    if ((st.st_mode & 0077) != 0 && ::chmod(dir.c_str(), 0700) != 0) return {};
    return dir;
}

uint64_t proc_start_ticks(int pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    const char* p = std::strrchr(buf, ')');
    if (!p) return 0;
    ++p;
    int field = 3;
    while (*p && field < 22) {
        while (*p == ' ') ++p;
        while (*p && *p != ' ') ++p;
        ++field;
    }
    while (*p == ' ') ++p;
    return std::strtoull(p, nullptr, 10);
}

struct Slot {
    int32_t pid;
    int32_t pad;
    uint64_t start_ticks;
    uint64_t reserved;
};
constexpr int kSlots = 64;
struct Table { Slot slots[kSlots]; };

bool slot_alive(const Slot& s) {
    if (s.pid <= 0) return false;
    if (::kill(s.pid, 0) != 0 && errno != EPERM) return false;
    const uint64_t ticks = proc_start_ticks(s.pid);
    return ticks != 0 && ticks == s.start_ticks;
}

bool read_table(int fd, Table& t) {
    std::memset(&t, 0, sizeof(t));
    const ssize_t n = ::pread(fd, &t, sizeof(t), 0);
    return n == static_cast<ssize_t>(sizeof(t)) || n == 0;
}
bool write_table(int fd, const Table& t) {
    return ::pwrite(fd, &t, sizeof(t), 0) == static_cast<ssize_t>(sizeof(t));
}

struct FlockGuard {
    int fd;
    bool ok;
    explicit FlockGuard(int f) : fd(f), ok(false) {
        while (::flock(fd, LOCK_EX) != 0) { if (errno != EINTR) return; }
        ok = true;
    }
    ~FlockGuard() { if (ok) ::flock(fd, LOCK_UN); }
};

}

BuildLedger::~BuildLedger() { release(); }

BuildLedger::Result BuildLedger::acquire(uint64_t bytes, uint64_t limit, double timeout_seconds,
                                         const std::function<void(const std::string&)>& notice) {
    if (limit != 0 && bytes > limit) return Result::TooLarge;
    const std::string dir = private_dir();
    if (dir.empty()) return Result::Unavailable;
    const std::string path = dir + "/ledger.bin";
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) return Result::Unavailable;
    struct stat st{};
    if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) {
        ::close(fd_); fd_ = -1;
        return Result::Unavailable;
    }
    const auto started = std::chrono::steady_clock::now();
    auto last_notice = started - std::chrono::seconds(60);
    for (;;) {
        {
            FlockGuard lock(fd_);
            if (!lock.ok) { ::close(fd_); fd_ = -1; return Result::Unavailable; }
            Table t;
            if (!read_table(fd_, t)) { ::close(fd_); fd_ = -1; return Result::Unavailable; }
            uint64_t others = 0;
            int live = 0, free_slot = -1;
            for (int i = 0; i < kSlots; ++i) {
                if (t.slots[i].pid != 0 && !slot_alive(t.slots[i])) std::memset(&t.slots[i], 0, sizeof(Slot));
                if (t.slots[i].pid == 0) { if (free_slot < 0) free_slot = i; continue; }
                others += t.slots[i].reserved;
                ++live;
            }
            if (free_slot >= 0 && (limit == 0 || others + bytes <= limit)) {
                t.slots[free_slot].pid = static_cast<int32_t>(::getpid());
                t.slots[free_slot].start_ticks = proc_start_ticks(static_cast<int>(::getpid()));
                t.slots[free_slot].reserved = bytes;
                if (!write_table(fd_, t)) { ::close(fd_); fd_ = -1; return Result::Unavailable; }
                slot_ = free_slot;
                peers_ = live;
                return Result::Ok;
            }
            peers_ = live;
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - started).count() >= timeout_seconds) {
            ::close(fd_); fd_ = -1;
            return Result::Timeout;
        }
        if (notice && now - last_notice >= std::chrono::seconds(10)) {
            notice("waiting for the shared compiler memory budget (" + std::to_string(peers_) +
                   " other csyc process(es) are building)");
            last_notice = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

bool BuildLedger::grow(uint64_t bytes, uint64_t limit) {
    if (slot_ < 0 || fd_ < 0) return true;
    FlockGuard lock(fd_);
    if (!lock.ok) return true;
    Table t;
    if (!read_table(fd_, t)) return true;
    uint64_t others = 0;
    int live = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (i == slot_) continue;
        if (t.slots[i].pid != 0 && !slot_alive(t.slots[i])) std::memset(&t.slots[i], 0, sizeof(Slot));
        if (t.slots[i].pid == 0) continue;
        others += t.slots[i].reserved;
        ++live;
    }
    peers_ = live;
    if (limit != 0 && others + bytes > limit) return false;
    if (t.slots[slot_].pid == ::getpid()) {
        t.slots[slot_].reserved = bytes;
        write_table(fd_, t);
    }
    return true;
}

void BuildLedger::release() {
    if (fd_ >= 0) {
        if (slot_ >= 0) {
            FlockGuard lock(fd_);
            Table t;
            if (lock.ok && read_table(fd_, t) && t.slots[slot_].pid == ::getpid()) {
                std::memset(&t.slots[slot_], 0, sizeof(Slot));
                write_table(fd_, t);
            }
        }
        ::close(fd_);
    }
    fd_ = -1;
    slot_ = -1;
}

namespace {

constexpr int kTempSlots = 32;
constexpr size_t kTempPath = 1024;
struct TempSlot {
    std::atomic<int> state{0};
    char path[kTempPath];
};
TempSlot g_temp[kTempSlots];
std::atomic<bool> g_handlers_installed{false};
std::atomic<int> g_active_child{0};

void unlink_all_signal_safe() {
    for (int i = 0; i < kTempSlots; ++i) {
        if (g_temp[i].state.load(std::memory_order_acquire) == 1) ::unlink(g_temp[i].path);
    }
}

std::atomic<void (*)(int)> g_signal_hook{nullptr};

void on_fatal_signal(int sig) {
    if (auto hook = g_signal_hook.load(std::memory_order_relaxed)) hook(sig);
    const int child = g_active_child.load(std::memory_order_relaxed);
    if (child > 0) ::kill(child, SIGTERM);
    unlink_all_signal_safe();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}

bool register_temp_path(const std::string& path) {
    if (path.empty() || path.size() >= kTempPath) return false;
    for (int i = 0; i < kTempSlots; ++i) {
        int expected = 0;
        if (g_temp[i].state.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
            std::memcpy(g_temp[i].path, path.c_str(), path.size() + 1);
            g_temp[i].state.store(1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void unregister_temp_path(const std::string& path) {
    for (int i = 0; i < kTempSlots; ++i) {
        if (g_temp[i].state.load(std::memory_order_acquire) == 1 &&
            std::strcmp(g_temp[i].path, path.c_str()) == 0) {
            g_temp[i].state.store(0, std::memory_order_release);
            return;
        }
    }
}

void remove_registered_temp_paths() {
    for (int i = 0; i < kTempSlots; ++i) {
        if (g_temp[i].state.load(std::memory_order_acquire) == 1) {
            ::unlink(g_temp[i].path);
            g_temp[i].state.store(0, std::memory_order_release);
        }
    }
}

void set_signal_hook(void (*hook)(int)) { g_signal_hook.store(hook, std::memory_order_relaxed); }
void set_active_child(int pid) { g_active_child.store(pid, std::memory_order_relaxed); }

void install_cleanup_handlers() {
    if (g_handlers_installed.exchange(true)) return;
    std::atexit([] { remove_registered_temp_paths(); });
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) ::sigaction(sig, &sa, nullptr);
}

unsigned hardware_threads() {
    cpu_set_t set;
    CPU_ZERO(&set);
    unsigned n = 0;
    if (::sched_getaffinity(0, sizeof(set), &set) == 0) n = static_cast<unsigned>(CPU_COUNT(&set));
    if (n == 0) n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

unsigned resolve_threads(unsigned requested, uint64_t bytes_per_worker) {
    unsigned n = requested != 0 ? requested : hardware_threads();
    if (n > 256) n = 256;
    if (n < 1) n = 1;
    const uint64_t limit = limit_bytes();
    if (limit != 0 && bytes_per_worker != 0) {
        const uint64_t used = tracked_bytes();
        const uint64_t avail = limit > used ? limit - used : 0;
        const uint64_t fit = avail / bytes_per_worker;
        if (fit < n) n = fit < 1 ? 1u : static_cast<unsigned>(fit);
    }
    return n;
}

namespace {
struct StackThreadStart { std::function<void()> fn; };
void* stack_thread_trampoline(void* arg) {
    static_cast<StackThreadStart*>(arg)->fn();
    return nullptr;
}
bool spawn_stack_thread(pthread_t& handle, size_t bytes, StackThreadStart* start) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    bool ok = pthread_attr_setstacksize(&attr, bytes) == 0 &&
              pthread_create(&handle, &attr, stack_thread_trampoline, start) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}
constexpr size_t kWorkerStackBytes = 64u * 1024u * 1024u;
}

void run_with_stack(size_t bytes, const std::function<void()>& fn) {
    for (size_t size = bytes; size >= 8u * 1024u * 1024u; size /= 4) {
        StackThreadStart start{fn};
        pthread_t handle;
        if (spawn_stack_thread(handle, size, &start)) {
            pthread_join(handle, nullptr);
            return;
        }
    }
    fn();
}

void parallel_for(size_t count, unsigned threads, const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    if (threads > count) threads = static_cast<unsigned>(count);
    if (threads <= 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr first_error;
    std::mutex error_mutex;
    auto worker = [&] {
        for (;;) {
            if (failed.load(std::memory_order_relaxed)) break;
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) break;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!first_error) first_error = std::current_exception();
                failed.store(true, std::memory_order_relaxed);
            }
        }
        flush_thread();
    };
    std::vector<pthread_t> pool;
    std::vector<StackThreadStart> starts;
    pool.reserve(threads - 1);
    starts.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) {
        starts.push_back(StackThreadStart{worker});
        pthread_t handle;
        if (!spawn_stack_thread(handle, kWorkerStackBytes, &starts.back())) {
            starts.pop_back();
            break;
        }
        pool.push_back(handle);
    }
    worker();
    for (pthread_t th : pool) pthread_join(th, nullptr);
    if (first_error) std::rethrow_exception(first_error);
}

}
}

void* operator new(std::size_t n) {
    for (;;) {
        if (void* p = csy::res::alloc_or_null(n)) return p;
        if (csy::res::hard_limit_hit()) throw std::bad_alloc();
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return csy::res::alloc_or_null(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return csy::res::alloc_or_null(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = csy::res::alloc_aligned_or_null(n, static_cast<std::size_t>(a))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return csy::res::alloc_aligned_or_null(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return csy::res::alloc_aligned_or_null(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete[](void* p) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { csy::res::note_free(p); std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { csy::res::note_free(p); std::free(p); }

