#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <utility>
#include <string>

namespace csy {
namespace res {

constexpr uint64_t kEstimatedBytesPerSourceByte = 200;


class BudgetExceeded : public std::exception {
public:
    explicit BudgetExceeded(std::string what) : what_(std::move(what)) {}
    const char* what() const noexcept override { return what_.c_str(); }
private:
    std::string what_;
};

void set_tracking(bool enabled);
bool tracking();

void set_limit_bytes(uint64_t bytes);
uint64_t limit_bytes();

uint64_t tracked_bytes();
uint64_t peak_tracked_bytes();
uint64_t rss_bytes();

bool hard_limit_hit();

void check(const char* stage);

bool within_budget(const char* stage);

void require_headroom(const char* stage, uint64_t additional_bytes);

inline void poll(const char* stage) {
    static thread_local unsigned counter = 0;
    if ((++counter & 0x3ffu) == 0) check(stage);
}

void flush_thread();

class BuildLedger {
public:
    enum class Result { Ok, TooLarge, Timeout, Unavailable };
    BuildLedger() = default;
    ~BuildLedger();
    BuildLedger(const BuildLedger&) = delete;
    BuildLedger& operator=(const BuildLedger&) = delete;

    Result acquire(uint64_t bytes, uint64_t limit, double timeout_seconds,
                   const std::function<void(const std::string&)>& notice);
    bool grow(uint64_t bytes, uint64_t limit);
    void release();
    bool held() const { return slot_ >= 0; }
    int peers() const { return peers_; }

private:
    int fd_ = -1;
    int slot_ = -1;
    int peers_ = 0;
};

void install_cleanup_handlers();
void set_signal_hook(void (*hook)(int));
bool register_temp_path(const std::string& path);
void unregister_temp_path(const std::string& path);
void remove_registered_temp_paths();
void set_active_child(int pid);

unsigned hardware_threads();

unsigned resolve_threads(unsigned requested, uint64_t bytes_per_worker);

void parallel_for(size_t count, unsigned threads,
                  const std::function<void(size_t)>& fn);

void run_with_stack(size_t bytes, const std::function<void()>& fn);

}
}

