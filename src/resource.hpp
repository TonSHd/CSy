// Written by ChatGPT(codex)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <utility>
#include <string>

namespace csy {
namespace res {

// Rough upper bound of heap needed per byte of source text (tokens + AST +
// diagnostics + bytecode). Used to size per-worker memory and build reservations.
// Measured on generated projects: peak compile memory is ~185x the source size
// (an AST node costs ~400 bytes; a 16-byte statement becomes ~8 nodes). 200 keeps
// the estimate slightly pessimistic so concurrent builds are not over-admitted.
constexpr uint64_t kEstimatedBytesPerSourceByte = 200;

// ---------------------------------------------------------------------------
// Memory accounting
// ---------------------------------------------------------------------------
// The global operator new/delete (replaced in resource.cpp) count the live
// heap bytes while tracking is enabled. Counting is per-thread-batched, so
// the cost is a thread-local add and one well-predicted branch per
// allocation; the shared counter is exact to within ~64 KiB per thread.

// Thrown when the compiler's memory budget is (nearly) exhausted. The message
// is complete and user-facing.
// Deliberately NOT a std::runtime_error: lexer/parser/VM code catches
// runtime_error to report ordinary diagnostics, and a budget abort must unwind
// through all of them to the top level.
class BudgetExceeded : public std::exception {
public:
    explicit BudgetExceeded(std::string what) : what_(std::move(what)) {}
    const char* what() const noexcept override { return what_.c_str(); }
private:
    std::string what_;
};

void set_tracking(bool enabled);
bool tracking();

// 0 disables the budget. Hard limit == `bytes`; warn at 80%, abort at 95%.
void set_limit_bytes(uint64_t bytes);
uint64_t limit_bytes();

uint64_t tracked_bytes();       // live bytes (never negative)
uint64_t peak_tracked_bytes();  // high-water mark since set_tracking(true)
uint64_t rss_bytes();           // resident set size from /proc (0 if unknown)

// True once the allocator refused an allocation because of the hard limit.
bool hard_limit_hit();

// Cheap polling point for long loops. Prints one warning at >= 80% of the
// budget and throws BudgetExceeded at >= 95%. `stage` names where we are.
void check(const char* stage);

// Same as check() but never throws: returns false when the abort threshold
// has been reached (used by callers that prefer to unwind by return value).
bool within_budget(const char* stage);

// Call BEFORE a large allocation (a growing buffer, a big reserve): applies the
// 80% warning / 95% abort thresholds to the usage the allocation WOULD cause, so
// a single big step can never leap over both thresholds and hit the hard limit.
void require_headroom(const char* stage, uint64_t additional_bytes);

// Cheap poll for per-statement loops: only every 1024th call does real work.
inline void poll(const char* stage) {
    static thread_local unsigned counter = 0;
    if ((++counter & 0x3ffu) == 0) check(stage);
}

// Make the current thread's pending allocation delta visible to other
// threads. Worker threads call this before exiting.
void flush_thread();

// ---------------------------------------------------------------------------
// Concurrent build ledger
// ---------------------------------------------------------------------------
// Several csyc processes started by the same user share one memory budget.
// Each reserves an estimate of its needs in a small flock-protected table in
// a private directory; a process waits (politely, with notices) until its
// reservation fits next to the live reservations of the others.
class BuildLedger {
public:
    enum class Result { Ok, TooLarge, Timeout, Unavailable };
    BuildLedger() = default;
    ~BuildLedger();
    BuildLedger(const BuildLedger&) = delete;
    BuildLedger& operator=(const BuildLedger&) = delete;

    // Reserve `bytes` of the shared `limit` (limit==0: only record the slot).
    // `notice` is called (rarely) with a human-readable waiting message.
    Result acquire(uint64_t bytes, uint64_t limit, double timeout_seconds,
                   const std::function<void(const std::string&)>& notice);
    // Raise this process's reservation to `bytes`. Returns false if other live
    // reservations leave no room (the caller should stop cleanly).
    bool grow(uint64_t bytes, uint64_t limit);
    void release();
    bool held() const { return slot_ >= 0; }
    // Number of OTHER live compiler processes seen at the last acquire/grow.
    int peers() const { return peers_; }

private:
    int fd_ = -1;
    int slot_ = -1;
    int peers_ = 0;
};

// ---------------------------------------------------------------------------
// Temporary-file cleanup
// ---------------------------------------------------------------------------
// Paths registered here are unlinked at normal exit and - async-signal-safely -
// on SIGINT/SIGTERM/SIGHUP, so an interrupted build never leaves a half-written
// object or native-cache file behind.
void install_cleanup_handlers();
// Optional async-signal-safe function called first when SIGINT/SIGTERM/SIGHUP arrives (used to write one line).
void set_signal_hook(void (*hook)(int));
bool register_temp_path(const std::string& path);
void unregister_temp_path(const std::string& path);
void remove_registered_temp_paths();
// A child process (e.g. the C compiler used by the native JIT) that should be
// terminated if csyc itself is interrupted. Pass 0 to clear.
void set_active_child(int pid);

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
// CPUs this process may actually use (honours affinity / container cpusets).
unsigned hardware_threads();

// Resolve a --threads request. `requested == 0` means "auto". The result is
// always in [1, hardware_threads()*... capped by `max_allowed`] and, when a
// memory budget is active, is reduced so `workers * bytes_per_worker` leaves
// room inside it.
unsigned resolve_threads(unsigned requested, uint64_t bytes_per_worker);

// Run fn(0..count-1) on at most `threads` OS threads. The first exception
// thrown by any task is rethrown on the calling thread after all workers stop;
// remaining tasks are skipped once one has failed. threads<=1 runs inline.
void parallel_for(size_t count, unsigned threads,
                  const std::function<void(size_t)>& fn);

// Runs fn to completion on a new thread whose stack is at least `bytes` (address space
// is reserved, pages are only committed when touched). The compiler and the VM recurse
// on the C++ stack, and the default stack comes from `ulimit -s`, which can be tiny, so
// everything runs on a stack of known size instead. Falls back to smaller sizes and
// finally to running inline if the thread cannot be created. Returns after fn finishes.
void run_with_stack(size_t bytes, const std::function<void()>& fn);

}
}
