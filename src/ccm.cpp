#include "ccm.hpp"
#include "hash128.hpp"
#include "resource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>


namespace csy {
namespace ccm {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr size_t kMemoryCap = 500;
constexpr uint64_t kSourceBigBytes = 5ull << 20;
constexpr int64_t kBeenAWhileSeconds = 7 * 86400;
constexpr int kStreakMin = 3;
constexpr int kComebackMin = 2;
constexpr int kManySuccessesMin = 5;
constexpr int kRepeatedErrorMin = 3;

constexpr uintmax_t kMaxTableBytes = 16u << 20;
constexpr uintmax_t kMaxMemBytes = 4u << 20;
constexpr uintmax_t kMaxConfBytes = 1u << 20;
constexpr uintmax_t kMaxHashedBytes = 40u << 20;

bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

uint32_t crc32_of(const unsigned char* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t rd16(const unsigned char* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint64_t rd64(const unsigned char* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }
void wr16(std::string& o, uint16_t v) { o.push_back(char(v & 255)); o.push_back(char(v >> 8)); }
void wr32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(char((v >> (8 * i)) & 255)); }
void wr64(std::string& o, uint64_t v) { wr32(o, uint32_t(v)); wr32(o, uint32_t(v >> 32)); }

bool read_file_bounded(const fs::path& p, uintmax_t max_bytes, std::string& out, bool& too_big) {
    too_big = false;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return false;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    if (size > max_bytes) { too_big = true; return false; }
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(static_cast<size_t>(size), '\0');
    if (size != 0) in.read(&out[0], static_cast<std::streamsize>(size));
    return static_cast<uintmax_t>(in.gcount()) == size;
}

uint32_t id_hash(const std::string& id) {
    uint32_t h = 2166136261u;
    for (unsigned char c : id) h = (h ^ c) * 16777619u;
    return h == 0 ? 1u : h;
}

int64_t now_unix() { return static_cast<int64_t>(std::time(nullptr)); }

std::string fmt_duration_ms(double ms) {
    char b[48];
    if (ms < 10000.0) std::snprintf(b, sizeof b, "%lld ms", static_cast<long long>(ms + 0.5));
    else std::snprintf(b, sizeof b, "%.1f s", ms / 1000.0);
    return b;
}
std::string fmt_since(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    char b[48];
    auto plural = [&](long long n, const char* unit) {
        std::snprintf(b, sizeof b, "%lld %s%s", n, unit, n == 1 ? "" : "s");
    };
    if (seconds >= 86400) plural(seconds / 86400, "day");
    else if (seconds >= 3600) plural(seconds / 3600, "hour");
    else if (seconds >= 60) plural(seconds / 60, "minute");
    else std::snprintf(b, sizeof b, "under a minute");
    return b;
}
std::string fmt_size(uint64_t bytes) {
    char b[48];
    if (bytes < 1024) std::snprintf(b, sizeof b, "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < (1ull << 20)) std::snprintf(b, sizeof b, "%.1f KiB", bytes / 1024.0);
    else if (bytes < (1ull << 30)) std::snprintf(b, sizeof b, "%.1f MiB", bytes / 1048576.0);
    else std::snprintf(b, sizeof b, "%.1f GiB", bytes / 1073741824.0);
    return b;
}
std::string mib_text(uint64_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MiB"; }

std::string base_name(const std::string& p) {
    const std::string n = fs::path(p).filename().string();
    return n.empty() ? p : n;
}

struct Table {
    struct Sit { std::string id; uint32_t first = 0, count = 0; };
    std::vector<Sit> sits;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    std::string data;

    const Sit* find(const std::string& id) const {
        auto it = std::lower_bound(sits.begin(), sits.end(), id, [](const Sit& s, const std::string& k) { return s.id < k; });
        return (it != sits.end() && it->id == id) ? &*it : nullptr;
    }
    std::string line(const Sit& s, uint32_t k) const {
        const auto& l = lines[s.first + k];
        return data.substr(l.first, l.second);
    }
};

bool parse_table(const std::string& raw, Table& t) {
    constexpr size_t kHeader = 36;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
    if (raw.size() < kHeader || std::memcmp(p, "CCMT", 4) != 0) return false;
    if (rd16(p + 4) != 1) return false;
    const uint64_t nsit = rd32(p + 8), nlines = rd32(p + 12), ioff = rd32(p + 16), loff = rd32(p + 20), doff = rd32(p + 24),
                   dsize = rd32(p + 28);
    if (ioff != kHeader || loff != ioff + nsit * 16 || doff != loff + nlines * 8 || doff + dsize != raw.size()) return false;
    if (crc32_of(p + kHeader, raw.size() - kHeader) != rd32(p + 32)) return false;
    t.data.assign(raw, static_cast<size_t>(doff), static_cast<size_t>(dsize));
    t.lines.clear();
    t.sits.clear();
    for (uint64_t i = 0; i < nlines; ++i) {
        const uint64_t off = rd32(p + loff + 8 * i), len = rd32(p + loff + 8 * i + 4);
        if (off + len > dsize) return false;
        t.lines.emplace_back(static_cast<uint32_t>(off), static_cast<uint32_t>(len));
    }
    for (uint64_t i = 0; i < nsit; ++i) {
        const unsigned char* e = p + ioff + 16 * i;
        const uint64_t off = rd32(e), len = rd16(e + 4), first = rd32(e + 8), cnt = rd32(e + 12);
        if (off + len > dsize || first + cnt > nlines) return false;
        Table::Sit s;
        s.id.assign(t.data, static_cast<size_t>(off), static_cast<size_t>(len));
        s.first = static_cast<uint32_t>(first);
        s.count = static_cast<uint32_t>(cnt);
        if (!t.sits.empty() && !(t.sits.back().id < s.id)) return false;
        t.sits.push_back(std::move(s));
    }
    return true;
}

constexpr size_t kMemHeader = 32, kSlotCount = 128, kSlotSize = 32, kRecSize = 64;
enum RecType : uint8_t { kAttempt = 1, kEnableMarker = 2 };
enum RecFlag : uint8_t { kSuccess = 1, kRunMode = 2, kHasContent = 4 };

struct Rec {
    uint8_t type = 0, flags = 0;
    uint16_t errors = 0, warnings = 0;
    uint32_t sit = 0;
    int64_t time = 0;
    Hash128 path, content;
};
struct Slot { uint32_t sit = 0, last = 0; uint64_t used = 0; };

struct Mem {
    std::vector<Rec> recs;
    std::array<Slot, kSlotCount> slots{};
};

enum class MemStatus { Absent, Ok, Broken };

std::string serialize_mem(const Mem& m) {
    std::string body;
    for (const Slot& s : m.slots) {
        wr32(body, s.sit); wr32(body, s.last); wr64(body, s.used);
        body.append(kSlotSize - 16, '\0');
    }
    for (const Rec& r : m.recs) {
        const size_t start = body.size();
        body.push_back(char(r.type)); body.push_back(char(r.flags));
        wr16(body, r.errors); wr16(body, r.warnings); wr16(body, 0);
        wr32(body, r.sit); wr32(body, 0);
        wr64(body, static_cast<uint64_t>(r.time));
        wr64(body, r.path.a); wr64(body, r.path.b); wr64(body, r.content.a); wr64(body, r.content.b);
        body.append(kRecSize - (body.size() - start), '\0');
    }
    std::string out("CCMM", 4);
    wr16(out, 1); wr16(out, static_cast<uint16_t>(kSlotCount));
    wr32(out, static_cast<uint32_t>(m.recs.size()));
    wr32(out, crc32_of(reinterpret_cast<const unsigned char*>(body.data()), body.size()));
    out.append(kMemHeader - out.size(), '\0');
    return out + body;
}

bool parse_mem(const std::string& raw, Mem& m) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
    if (raw.size() < kMemHeader || std::memcmp(p, "CCMM", 4) != 0) return false;
    if (rd16(p + 4) != 1 || rd16(p + 6) != kSlotCount) return false;
    const uint64_t n = rd32(p + 8);
    if (n > kMemoryCap * 4 || kMemHeader + kSlotCount * kSlotSize + n * kRecSize != raw.size()) return false;
    if (crc32_of(p + kMemHeader, raw.size() - kMemHeader) != rd32(p + 12)) return false;
    const unsigned char* s = p + kMemHeader;
    for (size_t i = 0; i < kSlotCount; ++i, s += kSlotSize) {
        m.slots[i].sit = rd32(s); m.slots[i].last = rd32(s + 4); m.slots[i].used = rd64(s + 8);
    }
    m.recs.clear();
    for (uint64_t i = 0; i < n; ++i, s += kRecSize) {
        Rec r;
        r.type = s[0]; r.flags = s[1]; r.errors = rd16(s + 2); r.warnings = rd16(s + 4);
        r.sit = rd32(s + 8); r.time = static_cast<int64_t>(rd64(s + 16));
        r.path.a = rd64(s + 24); r.path.b = rd64(s + 32); r.content.a = rd64(s + 40); r.content.b = rd64(s + 48);
        if (r.type != kAttempt && r.type != kEnableMarker) return false;
        m.recs.push_back(r);
    }
    return true;
}

MemStatus load_mem(const fs::path& file, Mem& m) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return MemStatus::Absent;
    std::string raw;
    bool too_big = false;
    if (!read_file_bounded(file, kMaxMemBytes, raw, too_big)) return MemStatus::Broken;
    Mem tmp;
    if (!parse_mem(raw, tmp)) return MemStatus::Broken;
    m = std::move(tmp);
    return MemStatus::Ok;
}

struct MemLock {
    int fd = -1;
    explicit MemLock(const fs::path& lock_file) {
        fd = ::open(lock_file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0) return;
        for (int i = 0; i < 150; ++i) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return;
            ::usleep(20000);
        }
        ::close(fd);
        fd = -1;
    }
    bool ok() const { return fd >= 0; }
    ~MemLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
};

bool write_atomic(const fs::path& target, const std::string& bytes) {
    const fs::path tmp = target.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) { std::error_code ec; fs::remove(tmp, ec); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

void mark_used(Slot& s, uint32_t idx, uint32_t n) {
    if (n <= 64) {
        const uint64_t full = n == 64 ? ~0ull : ((1ull << n) - 1);
        if ((s.used & full) == full) s.used = 0;
        s.used |= (1ull << idx);
    }
    s.last = idx + 1;
}
Slot* find_slot(std::array<Slot, kSlotCount>& slots, uint32_t sit, bool create) {
    for (Slot& s : slots) if (s.sit == sit) return &s;
    if (!create) return nullptr;
    for (Slot& s : slots) if (s.sit == 0) { s.sit = sit; return &s; }
    return nullptr;
}

struct Pick { uint32_t sit; uint32_t idx; uint32_t n; };
struct Fields {
    std::map<std::string, std::string> v;
    Fields& set(const char* k, const std::string& val) { v[k] = val; return *this; }
};

enum class Phase { Syntax, Check };

struct State;
State* g_state = nullptr;

struct Tap;

struct State {
    bool on = false;
    fs::path root;
    Table table;
    Mem mem;
    MemStatus mem_status = MemStatus::Absent;
    std::vector<Pick> picks;
    std::mt19937_64 rng{std::random_device{}() ^ (uint64_t(std::time(nullptr)) << 20)};
    Clock::time_point t0 = Clock::now();
    Clock::time_point run_start{};
    bool run_started = false;
    bool running_user_code = false;

    bool has_subject = false, run_mode = false;
    std::string subject, subject_name;
    Hash128 path_hash, content_hash;
    bool has_content = false;

    Phase phase = Phase::Syntax;
    int errors = 0, warnings = 0;
    uint32_t first_error_sit = 0;
    std::string first_error_id;
    bool saw_run_line = false, saw_build_line = false;
    bool no_color = false;
    bool tty_out = false, tty_err = false;

    std::streambuf* orig_out = nullptr;
    std::streambuf* orig_err = nullptr;
    Tap* tap_out = nullptr;
    Tap* tap_err = nullptr;
    bool installed = false;
};

std::string pick_line(State& st, const std::string& id, const Fields& f) {
    const Table::Sit* sit = st.table.find(id);
    if (!sit || sit->count == 0) return std::string();
    const uint32_t n = sit->count;
    const uint32_t key = id_hash(id);
    Slot scratch;
    Slot* slot = find_slot(st.mem.slots, key, true);
    if (!slot) { scratch.sit = key; slot = &scratch; }
    std::vector<uint32_t> candidates;
    if (n <= 64) {
        const uint64_t full = n == 64 ? ~0ull : ((1ull << n) - 1);
        bool new_round = (slot->used & full) == full;
        const uint64_t used = new_round ? 0 : slot->used;
        for (uint32_t i = 0; i < n; ++i) {
            if (used & (1ull << i)) continue;
            if (new_round && n > 1 && slot->last == i + 1) continue;
            candidates.push_back(i);
        }
    } else {
        for (uint32_t i = 0; i < n; ++i) if (!(n > 1 && slot->last == i + 1)) candidates.push_back(i);
    }
    if (candidates.empty()) for (uint32_t i = 0; i < n; ++i) candidates.push_back(i);
    const uint32_t idx = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(st.rng)];
    mark_used(*slot, idx, n);
    st.picks.push_back({key, idx, n});

    const std::string tmpl = st.table.line(*sit, idx);
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const size_t close = tmpl.find('}', i);
            if (close != std::string::npos) {
                auto it = f.v.find(tmpl.substr(i + 1, close - i - 1));
                if (it != f.v.end()) { out += it->second; i = close + 1; continue; }
            }
        }
        out.push_back(tmpl[i++]);
    }
    return out;
}

std::string gray(const State& st, int fd, const std::string& line) {
    const bool tty = fd == 1 ? st.tty_out : st.tty_err;
    if (st.no_color || !tty) return line + "\n";
    return "\x1b[90m" + line + "\x1b[0m\n";
}

std::string say(State& st, const std::string& id, const Fields& f, int fd) {
    if (!st.on) return std::string();
    const bool tty = fd == 1 ? st.tty_out : st.tty_err;
    if (!tty) return std::string();
    const std::string line = pick_line(st, id, f);
    if (line.empty()) return std::string();
    return gray(st, fd, line);
}

void write_direct(State& st, int fd, const std::string& text) {
    if (text.empty()) return;
    std::streambuf* b = fd == 1 ? st.orig_out : st.orig_err;
    if (b) { b->sputn(text.data(), static_cast<std::streamsize>(text.size())); b->pubsync(); }
}

struct Hit { std::string id; Fields f; };

std::string between(const std::string& s, const char* a, const char* b) {
    const size_t i = s.find(a);
    if (i == std::string::npos) return std::string();
    const size_t from = i + std::strlen(a);
    const size_t j = b ? s.find(b, from) : std::string::npos;
    return s.substr(from, j == std::string::npos ? std::string::npos : j - from);
}
std::string quoted_after(const std::string& s, const char* marker) {
    const size_t i = s.find(marker);
    if (i == std::string::npos) return std::string();
    const size_t a = s.find('\'', i + std::strlen(marker));
    if (a == std::string::npos) return std::string();
    const size_t b = s.find('\'', a + 1);
    if (b == std::string::npos) return std::string();
    return s.substr(a + 1, b - a - 1);
}
std::string digits_after(const std::string& s, const char* marker) {
    const size_t i = s.find(marker);
    if (i == std::string::npos) return std::string();
    size_t k = i + std::strlen(marker);
    while (k < s.size() && !(s[k] >= '0' && s[k] <= '9')) ++k;
    std::string d;
    while (k < s.size() && s[k] >= '0' && s[k] <= '9') d.push_back(s[k++]);
    return d;
}
std::string file_size_text(const std::string& path) {
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    return ec ? std::string("unknown size") : fmt_size(sz);
}

bool classify_diag(const State& st, bool is_error, const std::string& file, const std::string& line, const std::string& col,
                   const std::string& msg, Hit& h) {
    h.f.set("file", base_name(file)).set("line", line).set("col", col).set("name", "");
    auto id = [&](const char* s) { h.id = s; return true; };

    if (starts_with(msg, "runtime error: ")) {
        const std::string r = msg.substr(15);
        const std::string at_line = digits_after(r, ", line ");
        if (!at_line.empty()) h.f.set("line", at_line);
        else if (line == "0") h.f.set("line", "unknown");
        const std::string fn = between(r, "(in function '", "'");
        if (contains(r, "division by zero") || contains(r, "modulo by zero")) return id("runtime.division.zero");
        if (contains(r, "integer overflow")) return id("runtime.integer.overflow");
        if (contains(r, "maximum recursion depth")) {
            std::string lim = digits_after(r, "exceeded (");
            h.f.set("name", fn.empty() ? "top level" : fn).set("limit", lim.empty() ? "the limit" : lim);
            return id("runtime.recursion.limit");
        }
        if (contains(r, "out of memory")) {
            h.f.set("size", fmt_size(res::peak_tracked_bytes()));
            return id("runtime.out.of.memory");
        }
        if (contains(r, "out of bounds") || contains(r, "out of range")) {
            std::string idx = digits_after(r, "vector index ");
            h.f.set("count", idx.empty() ? "unknown" : idx);
            return id("runtime.index.range");
        }
        if (contains(r, "unknown builtin")) { h.f.set("name", quoted_after(r, "unknown builtin")); return id("runtime.unknown.builtin"); }
        if (contains(r, "attempt to call") || contains(r, "is not callable")) {
            h.f.set("name", fn);
            return id("runtime.not.callable");
        }
        if (contains(r, "cannot index into") || contains(r, "dict keys must be strings") || contains(r, "index must be an integer"))
            return id("runtime.type");
        return id("runtime.other");
    }

    if (is_error) {
        if (contains(msg, "expected '&&' but got '&'") || contains(msg, "expected '||' but got '|'")) return id("syntax.single.and.or");
        if (contains(msg, "unterminated string literal")) return id("syntax.unterminated.string");
        if (contains(msg, "'elif' isn't CSy")) return id("syntax.elif");
        if (contains(msg, "has no member '")) { h.f.set("name", between(msg, "has no member '", "'")); return id("runtime.unknown.builtin"); }
        if (contains(msg, "unexpected '::ext'")) return id("syntax.stray.ext");
        if (contains(msg, "too many errors in this file")) return id("syntax.too.many.errors");
        if (contains(msg, "nesting depth exceeded")) { h.f.set("limit", digits_after(msg, "exceeded (")); return id("syntax.too.deep"); }
        if (contains(msg, "expression is too long or too deeply chained")) { h.f.set("limit", digits_after(msg, "limit ")); return id("syntax.too.deep"); }
        if (starts_with(msg, "integer literal '") || starts_with(msg, "number literal '")) return id("literal.overflow");
        if (starts_with(msg, "unknown name '")) { h.f.set("name", quoted_after(msg, "unknown name")); return id("name.undefined"); }
        if (contains(msg, "is already declared in this file") || contains(msg, "is declared more than once")) {
            h.f.set("name", quoted_after(msg, "namespace")); return id("name.duplicate");
        }
        if (contains(msg, "is not exposed by connection")) { h.f.set("name", quoted_after(msg, "")); return id("connect.unknown.export"); }
        if (starts_with(msg, "expected ")) {
            std::string what = msg.substr(9);
            const size_t but = what.find(" but got");
            if (but != std::string::npos) what.resize(but);
            h.f.set("name", what);
            return id("syntax.expected");
        }
        if (st.phase == Phase::Syntax) return id("syntax.other");
        return id("check.other");
    }

    if (starts_with(msg, "unknown module '")) { h.f.set("name", quoted_after(msg, "unknown module")); return id("module.unknown"); }
    if (contains(msg, "' takes ") && contains(msg, "but this call passes")) {
        h.f.set("name", quoted_after(msg, "")).set("count", digits_after(msg, "this call passes"));
        return id("call.wrong.arguments");
    }
    if (contains(msg, "hides the built-in function")) { h.f.set("name", quoted_after(msg, "variable")); return id("warn.hides.builtin"); }
    if (contains(msg, "condition is a constant") || contains(msg, "condition is always false")) return id("warn.constant.condition");
    if (starts_with(msg, "unreachable code")) return id("warn.unreachable");
    if (contains(msg, "assigns a variable to itself")) { h.f.set("name", quoted_after(msg, "").substr(0, quoted_after(msg, "").find(' '))); return id("warn.self.assign"); }
    if (starts_with(msg, "'stop' used outside")) { h.f.set("name", "stop"); return id("warn.stop.skip.outside.loop"); }
    if (starts_with(msg, "'skip' used outside")) { h.f.set("name", "skip"); return id("warn.stop.skip.outside.loop"); }
    if (starts_with(msg, "assigning to loop variable")) { h.f.set("name", quoted_after(msg, "loop variable")); return id("warn.loop.variable.assigned"); }
    if (contains(msg, "is deprecated")) { h.f.set("name", "matmul"); return id("warn.deprecated"); }
    if (starts_with(msg, "duplicate dictionary key")) { h.f.set("name", between(msg, "key \"", "\"")); return id("warn.duplicate.key"); }
    if (contains(msg, "has no member '")) { h.f.set("name", between(msg, "has no member '", "'")); return id("runtime.unknown.builtin"); }
    if (contains(msg, "is already defined as a ")) { h.f.set("name", quoted_after(msg, "")); return id("name.duplicate"); }
    return id("warn.other");
}

bool classify_plain(const State& st, bool is_error, const std::string& msg, Hit& h) {
    auto id = [&](const char* s) { h.id = s; return true; };
    h.f.set("file", st.subject_name);
    const std::string path = quoted_after(msg, "");
    if (!is_error) {
        if (contains(msg, "compiler memory") && contains(msg, "80%")) {
            h.f.set("percent", "80").set("limit", mib_text(res::limit_bytes()));
            return id("memory.warn.80");
        }
        if (contains(msg, "waiting for the shared compiler memory budget")) {
            h.f.set("time", fmt_duration_ms(std::chrono::duration<double, std::milli>(Clock::now() - st.t0).count()));
            return id("memory.waiting.shared");
        }
        return false;
    }
    if (contains(msg, "cannot read source file '") && contains(msg, "(named by #connect")) {
        h.f.set("name", between(msg, "(named by #connect \"", "\""));
        return id("connect.file.missing");
    }
    if (contains(msg, "cannot read source file '") || contains(msg, "' is not a regular file") ||
        starts_with(msg, "cannot determine source size")) {
        if (!path.empty() && st.has_subject && base_name(path) != st.subject_name) {
            h.f.set("name", path);
            return id("connect.file.missing");
        }
        return id("file.unreadable");
    }
    if (starts_with(msg, "source file exceeds the 32 MiB safety limit")) {
        h.f.set("file", base_name(path)).set("size", file_size_text(path)).set("limit", "32 MiB");
        return id("file.too.big");
    }
    if (starts_with(msg, "source changed or could not be read completely")) { h.f.set("file", base_name(path)); return id("file.changed.while.reading"); }
    if (starts_with(msg, "project file exceeds the 1 MiB")) {
        h.f.set("size", file_size_text(st.subject)).set("limit", "1 MiB");
        return id("project.file.too.big");
    }
    if (starts_with(msg, "cannot read project file")) { h.f.set("file", base_name(path)); return id("project.file.unreadable"); }
    if (starts_with(msg, "connected file was not loaded")) { h.f.set("name", path); return id("connect.file.missing"); }
    if (contains(msg, "is not declared at the top level of")) { h.f.set("name", quoted_after(msg, "")); return id("connect.unknown.export"); }
    if (contains(msg, "#connect expects a .csy source file")) { h.f.set("name", quoted_after(msg, "but got")); return id("connect.wrong.extension"); }
    if (contains(msg, "' is private in '")) { h.f.set("name", quoted_after(msg, "")); return id("connect.unknown.export"); }
    if (contains(msg, "connection nesting exceeds the 256-file")) { h.f.set("limit", "256 files"); return id("connect.too.deep"); }
    if (contains(msg, "memory budget is at least 95%") || contains(msg, "(abort threshold is 95%)")) {
        h.f.set("percent", "95").set("limit", mib_text(res::limit_bytes()));
        return id("memory.abort.95");
    }
    if (contains(msg, "the shared compiler memory budget (") || contains(msg, "needs more memory than the budget allows") ||
        contains(msg, "cannot fit")) {
        h.f.set("size", fmt_size(res::peak_tracked_bytes())).set("limit", mib_text(res::limit_bytes()));
        return id("memory.project.too.big");
    }
    if (contains(msg, "gave up waiting for the shared compiler memory budget")) {
        h.f.set("time", fmt_duration_ms(std::chrono::duration<double, std::milli>(Clock::now() - st.t0).count()));
        return id("memory.waiting.shared");
    }
    if (contains(msg, "the system ran out of memory")) { h.f.set("size", fmt_size(res::peak_tracked_bytes())); return id("memory.system.out"); }
    if (contains(msg, "is defined more than once in") || contains(msg, "' lists '")) { h.f.set("name", quoted_after(msg, "")); return id("name.duplicate"); }
    if (starts_with(msg, "unknown flag '") || starts_with(msg, "unknown command '")) { h.f.set("name", quoted_after(msg, "")); return id("cli.unknown.command"); }
    if (msg.size() > 1 && msg[0] == '-' && (contains(msg, " requires ") || contains(msg, " must be ") || contains(msg, " expects "))) {
        h.f.set("name", msg.substr(0, msg.find(' ')));
        return id("cli.bad.option.value");
    }
    if (starts_with(msg, "no input file") || starts_with(msg, "usage is '")) {
        h.f.set("name", starts_with(msg, "usage is 'csyc run") ? "run" : "csyc");
        return id("cli.missing.file");
    }
    return false;
}

struct Tap : public std::streambuf {
    State& st;
    std::streambuf* target;
    int fd;
    std::string partial;
    std::mutex mu;

    Tap(State& s, std::streambuf* t, int f) : st(s), target(t), fd(f) {}

    struct Diag { bool active = false; int stage = 0; bool is_error = false; std::string file, line, col, msg; size_t insert_at = 0; };
    Diag diag;
    int chunk_errors = 0;
    bool chunk_too_many = false;

    int sync() override { return target->pubsync(); }
    int overflow(int c) override {
        if (c == EOF) return 0;
        const char ch = static_cast<char>(c);
        return xsputn(&ch, 1) == 1 ? c : EOF;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::string out;
        try {
            std::lock_guard<std::mutex> g(mu);
            out = process(s, static_cast<size_t>(n));
        } catch (...) {
            out.assign(s, static_cast<size_t>(n));
            diag = Diag();
        }
        const std::streamsize w = target->sputn(out.data(), static_cast<std::streamsize>(out.size()));
        return w == static_cast<std::streamsize>(out.size()) ? n : 0;
    }

    void count_issue(bool is_error, const Hit* h) {
        if (is_error) {
            ++st.errors;
            ++chunk_errors;
            if (st.first_error_sit == 0 && h) { st.first_error_sit = id_hash(h->id); st.first_error_id = h->id; }
        } else {
            ++st.warnings;
        }
    }

    void finish_diag(std::string& out) {
        if (!diag.active) return;
        diag.active = false;
        if (diag.stage < 3) return;
        Hit h;
        if (!classify_diag(st, diag.is_error, diag.file, diag.line, diag.col, diag.msg, h)) return;
        count_issue(diag.is_error, &h);
        if (h.id == "syntax.too.many.errors") {
            chunk_too_many = true;
            h.f.set("count", std::to_string(chunk_errors));
        }
        const std::string said = say(st, h.id, h.f, fd);
        if (!said.empty() && diag.insert_at <= out.size()) out.insert(diag.insert_at, said);
    }

    void plain_line(const std::string& raw_line, std::string& out) {
        const bool progress = raw_line.find('\r') != std::string::npos;
        auto at = [&](const char* marker) -> size_t {
            if (starts_with(raw_line, marker)) return 0;
            return progress ? raw_line.find(marker) : std::string::npos;
        };
        Hit h;
        size_t k;
        if ((k = at("error: ")) != std::string::npos) {
            if (classify_plain(st, true, raw_line.substr(k + 7), h)) {
                count_issue(true, &h);
                out += say(st, h.id, h.f, fd);
            } else {
                count_issue(true, nullptr);
            }
            return;
        }
        if ((k = at("warning: ")) != std::string::npos) {
            if (classify_plain(st, false, raw_line.substr(k + 9), h)) {
                count_issue(false, &h);
                out += say(st, h.id, h.f, fd);
            } else {
                count_issue(false, nullptr);
            }
            return;
        }
        if (contains(raw_line, "waiting for the shared compiler memory budget") && !contains(raw_line, "gave up waiting")) {
            if (classify_plain(st, false, raw_line, h)) out += say(st, h.id, h.f, fd);
            return;
        }
        if ((k = at("[csy] build completed: ")) != std::string::npos) {
            st.saw_build_line = true;
            const std::string path = raw_line.substr(k + 23);
            Fields f;
            f.set("file", st.has_subject ? st.subject_name : base_name(path));
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - st.t0).count();
            f.set("time", fmt_duration_ms(ms)).set("size", file_size_text(path)).set("count", std::to_string(st.warnings));
            out += say(st, st.warnings > 0 ? "compile.success.warnings" : "compile.success", f, fd);
            return;
        }
        if ((k = at("[csy] native fast path: ")) != std::string::npos) {
            const std::string line = raw_line.substr(k);
            const std::string a = digits_after(line, "native fast path:");
            const size_t of_pos = line.find(" of ");
            const std::string of = of_pos == std::string::npos ? std::string() : digits_after(line.substr(of_pos), " of ");
            Fields f;
            f.set("file", st.subject_name).set("count", a).set("size", of);
            const long na = std::atol(a.c_str()), nt = std::atol(of.c_str());
            out += say(st, na == 0 ? "native.none" : (na == nt ? "native.active" : "native.partial"), f, fd);
            return;
        }
        if (at("[run] Program finished successfully.") != std::string::npos) {
            st.saw_run_line = true;
            Fields f;
            f.set("file", st.subject_name);
            const double ms = st.run_started ? std::chrono::duration<double, std::milli>(Clock::now() - st.run_start).count() : 0;
            f.set("time", fmt_duration_ms(ms));
            out += say(st, "run.success", f, fd);
            return;
        }
    }

    static bool parse_header(const std::string& l, std::string& file, std::string& line, std::string& col, std::string& kind) {
        if (l.size() < 12 || l[0] != ' ' || l[1] != ' ') return false;
        for (const char* k : {"error", "warning", "note"}) {
            const std::string tail = std::string("  ") + k;
            if (l.size() > tail.size() + 4 && l.compare(l.size() - tail.size(), tail.size(), tail) == 0) {
                const std::string head = l.substr(2, l.size() - 2 - tail.size());
                const size_t c2 = head.rfind(':');
                if (c2 == std::string::npos || c2 == 0) return false;
                const size_t c1 = head.rfind(':', c2 - 1);
                if (c1 == std::string::npos || c1 == 0) return false;
                const std::string ln = head.substr(c1 + 1, c2 - c1 - 1), cl = head.substr(c2 + 1);
                if (ln.empty() || cl.empty() || ln.find_first_not_of("0123456789") != std::string::npos ||
                    cl.find_first_not_of("0123456789") != std::string::npos)
                    return false;
                file = head.substr(0, c1); line = ln; col = cl; kind = k;
                return true;
            }
        }
        return false;
    }

    std::string process(const char* s, size_t n) {
        std::string out;
        out.reserve(n + 128);
        chunk_errors = 0;
        chunk_too_many = false;
        std::string chunk(s, n);
        size_t pos = 0;
        while (pos < chunk.size()) {
            const size_t nl = chunk.find('\n', pos);
            if (nl == std::string::npos) {
                out.append(chunk, pos, std::string::npos);
                partial.append(chunk, pos, std::string::npos);
                break;
            }
            const std::string line = partial + chunk.substr(pos, nl - pos);
            partial.clear();
            out.append(chunk, pos, nl - pos + 1);
            pos = nl + 1;

            std::string file, ln, col, kind;
            if (parse_header(line, file, ln, col, kind)) {
                finish_diag(out);
                if (kind != "note") {
                    diag = Diag();
                    diag.active = true; diag.stage = 1; diag.is_error = kind == "error";
                    diag.file = file; diag.line = ln; diag.col = col;
                    diag.insert_at = out.size();
                }
                continue;
            }
            if (diag.active) {
                if (starts_with(line, "  ... ") && contains(line, "diagnostic(s) not shown")) {
                    finish_diag(out);
                } else if (diag.stage == 1) {
                    if (line.size() >= 10 && line.compare(0, 4, "  --") == 0) diag.stage = 2;
                    else diag.active = false;
                    if (diag.active) diag.insert_at = out.size();
                    continue;
                } else if (diag.stage == 2) {
                    diag.msg = starts_with(line, "  ") ? line.substr(2) : line;
                    diag.stage = 3;
                    diag.insert_at = out.size();
                    continue;
                } else {
                    if (contains(line, " error(s), ") && contains(line, " warning(s)")) { finish_diag(out); continue; }
                    if (!line.empty()) diag.insert_at = out.size();
                    continue;
                }
            }
            if (starts_with(line, "  ... ") && contains(line, "diagnostic(s) not shown")) {
                if (!chunk_too_many) {
                    const std::string more = digits_after(line, "  ... ");
                    Fields f;
                    f.set("file", st.subject_name)
                        .set("count", std::to_string(chunk_errors + std::atoi(more.c_str())));
                    out += say(st, "syntax.too.many.errors", f, fd);
                }
                continue;
            }
            plain_line(line, out);
        }
        finish_diag(out);
        return out;
    }
};

struct InterruptPlan {
    char text[1536];
    uint16_t seg_end[9];
    int segs = 0;
    bool armed = false;
    struct timespec start;
};
InterruptPlan g_interrupt;

size_t put_uint(char* dst, unsigned long long v) {
    char tmp[24];
    size_t n = 0;
    do { tmp[n++] = char('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; ++i) dst[i] = tmp[n - 1 - i];
    return n;
}

void interrupt_hook(int sig) {
    if (sig != SIGINT || !g_interrupt.armed) return;
    struct timespec now;
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    const long long ms = (long long)(now.tv_sec - g_interrupt.start.tv_sec) * 1000 + (now.tv_nsec - g_interrupt.start.tv_nsec) / 1000000;
    char timebuf[40];
    size_t tn = 0;
    if (ms < 10000) { tn = put_uint(timebuf, ms < 0 ? 0 : (unsigned long long)ms); std::memcpy(timebuf + tn, " ms", 3); tn += 3; }
    else {
        tn = put_uint(timebuf, (unsigned long long)(ms / 1000));
        timebuf[tn++] = '.';
        timebuf[tn++] = char('0' + (ms % 1000) / 100);
        std::memcpy(timebuf + tn, " s", 2); tn += 2;
    }
    size_t from = 0;
    for (int i = 0; i < g_interrupt.segs; ++i) {
        const size_t to = g_interrupt.seg_end[i];
        if (to > from) { ssize_t r = ::write(STDERR_FILENO, g_interrupt.text + from, to - from); (void)r; }
        if (i + 1 < g_interrupt.segs) { ssize_t r = ::write(STDERR_FILENO, timebuf, tn); (void)r; }
        from = to;
    }
}

void arm_interrupt(State& st) {
    if (!st.on || !st.tty_err || !st.has_subject) return;
    const Table::Sit* sit = st.table.find("run.interrupted");
    if (!sit || sit->count == 0) return;
    Fields f;
    f.set("file", st.subject_name).set("time", "\x01");
    std::string line = pick_line(st, "run.interrupted", f);
    if (line.empty()) return;
    std::string full = "\n" + gray(st, 2, line);
    InterruptPlan& p = g_interrupt;
    size_t used = 0;
    p.segs = 0;
    size_t start = 0;
    while (true) {
        const size_t m = full.find('\x01', start);
        const std::string seg = full.substr(start, m == std::string::npos ? std::string::npos : m - start);
        if (p.segs >= 8 || used + seg.size() >= sizeof(p.text)) return;
        std::memcpy(p.text + used, seg.data(), seg.size());
        used += seg.size();
        p.seg_end[p.segs++] = static_cast<uint16_t>(used);
        if (m == std::string::npos) break;
        start = m + 1;
    }
    ::clock_gettime(CLOCK_MONOTONIC, &p.start);
    p.armed = true;
    res::set_signal_hook(&interrupt_hook);
}

fs::path cwd_root() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) return fs::path(".");
    for (fs::path d = dir;;) {
        if (fs::is_regular_file(d / "project.csp", ec)) return d;
        const fs::path parent = d.parent_path();
        if (parent == d || parent.empty()) break;
        d = parent;
    }
    return dir;
}

fs::path abs_dir_of(const std::string& arg) {
    std::error_code ec;
    fs::path p = fs::absolute(arg, ec);
    if (ec) return cwd_root();
    if (arg == ".") return p;
    return p.parent_path();
}

fs::path provisional_root(int argc, char** argv) {
    static const char* with_value[] = {"-o", "--threads", "--run-threads", "-j", "--memory-limit"};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") break;
        if (a.empty()) continue;
        if (a[0] == '-') {
            for (const char* w : with_value) if (a == w) { ++i; break; }
            continue;
        }
        if (a == "build" || a == "run") continue;
        const std::string ext = fs::path(a).extension().string();
        if (ext == ".csy" || ext == ".cso" || ext == ".csp" || a == ".") return abs_dir_of(a);
    }
    return cwd_root();
}

bool conf_value(const fs::path& conf, bool& present) {
    present = false;
    std::string raw;
    bool too_big = false;
    if (!read_file_bounded(conf, kMaxConfBytes, raw, too_big)) return false;
    const size_t k = raw.find("\"cooler_compiler\"");
    if (k == std::string::npos) return false;
    size_t i = k + 17;
    while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
    if (i >= raw.size() || raw[i] != ':') return false;
    ++i;
    while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
    present = true;
    return raw.compare(i, 4, "true") == 0;
}

bool conf_write(const fs::path& conf, bool value, std::string& why) {
    const std::string lit = value ? "true" : "false";
    std::string raw;
    bool too_big = false;
    std::error_code ec;
    if (!fs::exists(conf, ec)) return write_atomic(conf, "{\n  \"cooler_compiler\": " + lit + "\n}\n") || (why = "cannot write csyconf.json", false);
    if (!read_file_bounded(conf, kMaxConfBytes, raw, too_big)) { why = "cannot read csyconf.json"; return false; }
    const size_t first = raw.find_first_not_of(" \t\r\n");
    const size_t last = raw.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return write_atomic(conf, "{\n  \"cooler_compiler\": " + lit + "\n}\n");
    if (raw[first] != '{' || raw[last] != '}') { why = "csyconf.json is not a JSON object, nothing was changed"; return false; }
    const size_t k = raw.find("\"cooler_compiler\"");
    if (k != std::string::npos) {
        size_t i = k + 17;
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        if (i >= raw.size() || raw[i] != ':') { why = "csyconf.json has a damaged cooler_compiler entry, nothing was changed"; return false; }
        ++i;
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
        size_t j = i;
        if (raw.compare(i, 4, "true") == 0) j = i + 4;
        else if (raw.compare(i, 5, "false") == 0) j = i + 5;
        else { why = "csyconf.json has a cooler_compiler value that is not true or false, nothing was changed"; return false; }
        raw.replace(i, j - i, lit);
    } else {
        const bool empty_object = raw.find_first_not_of(" \t\r\n", first + 1) == last;
        if (empty_object) raw = "{\n  \"cooler_compiler\": " + lit + "\n}\n";
        else raw.insert(first + 1, "\n  \"cooler_compiler\": " + lit + ",");
    }
    if (!write_atomic(conf, raw)) { why = "cannot write csyconf.json"; return false; }
    return true;
}

bool load_table_for(State& st, bool& missing_or_broken) {
    missing_or_broken = false;
    std::string raw;
    bool too_big = false;
    Table t;
    if (!read_file_bounded(st.root / "ccm_table.bin", kMaxTableBytes, raw, too_big) || !parse_table(raw, t)) {
        missing_or_broken = true;
        return false;
    }
    st.table = std::move(t);
    return true;
}

void install(State& st) {
    if (!st.tap_out) {
        st.orig_out = std::cout.rdbuf();
        st.orig_err = std::cerr.rdbuf();
        st.tap_out = new Tap(st, st.orig_out, 1);
        st.tap_err = new Tap(st, st.orig_err, 2);
    }
    std::cout.rdbuf(st.tap_out);
    std::cerr.rdbuf(st.tap_err);
    st.installed = true;
}
void uninstall(State& st) {
    if (!st.installed) return;
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(st.orig_out);
    std::cerr.rdbuf(st.orig_err);
    st.installed = false;
}

Hash128 hash_of_path(const std::string& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return hash128(abs.string());
}

bool same(const Hash128& a, const Hash128& b) { return a.a == b.a && a.b == b.b; }

struct History {
    std::vector<const Rec*> attempts;
    const Rec* last_enable = nullptr;
};
History history_of(const Mem& m) {
    History h;
    for (const Rec& r : m.recs) {
        if (r.type == kAttempt) h.attempts.push_back(&r);
        else if (r.type == kEnableMarker) h.last_enable = &r;
    }
    return h;
}
bool ok_rec(const Rec& r) { return (r.flags & kSuccess) != 0; }

std::string sit_name(const State& st, uint32_t hash) {
    if (hash == 0) return "an error";
    for (const auto& s : st.table.sits) if (id_hash(s.id) == hash) return s.id;
    return "an error";
}

bool start_situation(State& st, std::string& id, Fields& f) {
    const History h = history_of(st.mem);
    const int64_t now = now_unix();
    f.set("file", st.subject_name);
    if (h.attempts.empty()) { id = "mem.first.ever"; return true; }
    int same_failed = 0;
    const Rec* last_same_failure = nullptr;
    bool known_path = false;
    for (const Rec* r : h.attempts) {
        if (same(r->path, st.path_hash)) known_path = true;
        if (st.has_content && (r->flags & kHasContent) && same(r->content, st.content_hash) && !ok_rec(*r)) {
            ++same_failed;
            last_same_failure = r;
        }
    }
    if (same_failed >= 1) {
        f.set("count", std::to_string(same_failed))
            .set("time", fmt_since(now - last_same_failure->time))
            .set("error", sit_name(st, last_same_failure->sit));
        id = same_failed >= 2 ? "mem.same.file.failed.many" : "mem.same.file.failed.once";
        return true;
    }
    const int64_t gap = now - h.attempts.back()->time;
    if (gap >= kBeenAWhileSeconds) { f.set("time", fmt_since(gap)); id = "mem.been.a.while"; return true; }
    if (!known_path) { id = "mem.new.file"; return true; }
    return false;
}

bool end_situation(State& st, bool success, std::string& id, Fields& f) {
    const History h = history_of(st.mem);
    const auto& A = h.attempts;
    f.set("file", st.subject_name);
    const std::string err_name = st.first_error_id.empty() ? "an error" : st.first_error_id;
    const Rec* last_on_path = nullptr;
    for (auto it = A.rbegin(); it != A.rend(); ++it) if (same((*it)->path, st.path_hash)) { last_on_path = *it; break; }
    int fail_run = 0, ok_run = 0;
    for (auto it = A.rbegin(); it != A.rend() && !ok_rec(**it); ++it) ++fail_run;
    for (auto it = A.rbegin(); it != A.rend() && ok_rec(**it); ++it) ++ok_run;
    const int64_t now = now_unix();

    if (success) {
        if (h.last_enable) {
            bool success_after = false;
            for (const Rec* r : A) if (r->time >= h.last_enable->time && ok_rec(*r)) success_after = true;
            if (!success_after) { id = "own.a.1"; return true; }
        }
        if (last_on_path && !ok_rec(*last_on_path) && last_on_path->sit != 0) {
            int repeats = 0;
            for (auto it = A.rbegin(); it != A.rend(); ++it) {
                if (!same((*it)->path, st.path_hash)) continue;
                if (ok_rec(**it) || (*it)->sit != last_on_path->sit) break;
                ++repeats;
            }
            if (repeats >= kRepeatedErrorMin) {
                f.set("error", sit_name(st, last_on_path->sit)).set("count", std::to_string(repeats));
                id = "own.b.1";
                return true;
            }
        }
        if (st.has_content) {
            int failed_before = 0;
            const Rec* newest = nullptr;
            for (const Rec* r : A)
                if ((r->flags & kHasContent) && same(r->content, st.content_hash)) { newest = r; if (!ok_rec(*r)) ++failed_before; }
            if (newest && !ok_rec(*newest)) {
                f.set("count", std::to_string(failed_before)).set("time", fmt_since(now - newest->time));
                id = "mem.same.file.now.succeeded";
                return true;
            }
        }
        if (fail_run >= kComebackMin) { f.set("count", std::to_string(fail_run)); id = "mem.comeback"; return true; }
        if (st.has_content) {
            int successes = 0;
            for (const Rec* r : A) if ((r->flags & kHasContent) && same(r->content, st.content_hash) && ok_rec(*r)) ++successes;
            if (successes >= kManySuccessesMin) { f.set("count", std::to_string(successes)); id = "mem.many.successes.same.file"; return true; }
        }
        if (ok_run + 1 >= kStreakMin) { f.set("count", std::to_string(ok_run + 1)); id = "mem.streak.success"; return true; }
        return false;
    }

    if (last_on_path && !ok_rec(*last_on_path)) {
        const bool contents_same = st.has_content && (last_on_path->flags & kHasContent) && same(last_on_path->content, st.content_hash);
        int on_path_failures = 1;
        int same_error_run = 1;
        bool still_same_error = true;
        for (auto it = A.rbegin(); it != A.rend(); ++it) {
            if (!same((*it)->path, st.path_hash)) continue;
            if (ok_rec(**it)) break;
            ++on_path_failures;
            if (still_same_error && st.first_error_sit != 0 && (*it)->sit == st.first_error_sit) ++same_error_run;
            else still_same_error = false;
        }
        if (contents_same) {
            f.set("count", std::to_string(on_path_failures)).set("time", fmt_since(now - last_on_path->time));
            id = "mem.unchanged.failed";
            return true;
        }
        f.set("error", err_name);
        if (st.first_error_sit != 0 && last_on_path->sit == st.first_error_sit) {
            f.set("count", std::to_string(same_error_run));
            id = "mem.same.error.again";
            return true;
        }
        if (st.first_error_sit != 0 && last_on_path->sit != 0) { id = "mem.different.error"; return true; }
        f.set("count", std::to_string(on_path_failures));
        id = "mem.changed.still.fails";
        return true;
    }
    if (fail_run + 1 >= kStreakMin) { f.set("count", std::to_string(fail_run + 1)); id = "mem.streak.fail"; return true; }
    return false;
}

void load_memory(State& st) {
    st.mem_status = load_mem(st.root / "ccm_mem.bin", st.mem);
}

}

namespace {

bool write_memory(State& st, const Rec* record) {
    const fs::path file = st.root / "ccm_mem.bin";
    MemLock lock(st.root / "ccm_mem.lock");
    if (!lock.ok()) return false;
    Mem m;
    if (load_mem(file, m) == MemStatus::Broken) return false;
    for (const Pick& p : st.picks) {
        if (Slot* sl = find_slot(m.slots, p.sit, true)) mark_used(*sl, p.idx, p.n);
    }
    if (record) m.recs.push_back(*record);
    if (m.recs.size() > kMemoryCap) m.recs.erase(m.recs.begin(), m.recs.begin() + (m.recs.size() - kMemoryCap));
    return write_atomic(file, serialize_mem(m));
}

const char* kWipeQuestion = "are you sure? (THIS WILL WIPE THE ENTIRE MEMORY OF COOLER COMPILER :< (y/n)";

State command_state() {
    State st;
    st.root = cwd_root();
    st.orig_out = std::cout.rdbuf();
    st.orig_err = std::cerr.rdbuf();
    st.tty_out = ::isatty(1) != 0;
    st.tty_err = ::isatty(2) != 0;
    const char* nc = std::getenv("NO_COLOR");
    const char* term = std::getenv("TERM");
    st.no_color = (nc && *nc) || (term && std::strcmp(term, "dumb") == 0);
    return st;
}

void speak_command(State& st, const char* id, const Fields& f) {
    if (st.table.sits.empty()) return;
    st.on = true;
    write_direct(st, 1, say(st, id, f, 1));
}

void table_notice(State& st) {
    if (st.tty_err)
        std::cerr << "note: the Cooler Compiler table '" << (st.root / "ccm_table.bin").string()
                  << "' is missing or damaged; it says nothing until that file is fixed\n";
}

void save_command_picks(State& st) {
    if (!st.picks.empty()) write_memory(st, nullptr);
}

}

int handle_command(int argc, char** argv) {
    if (argc < 2) return -1;
    const std::string a1 = argv[1];
    const bool toggle = (a1 == "enable" || a1 == "disable") && argc == 3 && std::string(argv[2]) == "ccm";
    bool wipe = false, yes = false;
    if (a1 == "wipe" && argc >= 3 && argc <= 4) {
        bool mem_flag = false, other = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--ccm-mem") mem_flag = true;
            else if (a == "--yes") yes = true;
            else other = true;
        }
        wipe = mem_flag && !other;
    }
    if (!toggle && !wipe) return -1;

    try {
        State st = command_state();
        const fs::path conf = st.root / "csyconf.json";
        bool present = false;
        const bool was_on = conf_value(conf, present);
        bool broken = false;
        if (!load_table_for(st, broken)) st.table = Table();
        load_memory(st);
        Fields none;

        if (toggle && a1 == "enable") {
            if (was_on) {
                std::cout << "[csy] the Cooler Compiler is already on (" << conf.string() << ")" << std::endl;
                speak_command(st, "ccm.already.enabled", none);
                save_command_picks(st);
                return 0;
            }
            std::string why;
            if (!conf_write(conf, true, why)) { std::cerr << "error: " << why << "\n"; return 1; }
            std::cout << "[csy] the Cooler Compiler is now on (" << conf.string() << ")" << std::endl;
            if (broken) table_notice(st);
            speak_command(st, "ccm.enabled", none);
            Rec marker;
            marker.type = kEnableMarker;
            marker.time = now_unix();
            if (st.mem_status != MemStatus::Broken) write_memory(st, &marker);
            return 0;
        }
        if (toggle) {
            if (!was_on) {
                std::cout << "[csy] the Cooler Compiler is already off (" << conf.string() << ")" << std::endl;
                speak_command(st, "ccm.already.disabled", none);
                save_command_picks(st);
                return 0;
            }
            std::cout << "[csy] switching the Cooler Compiler off (" << conf.string() << ")" << std::endl;
            speak_command(st, "ccm.disabled", none);
            std::string why;
            if (!conf_write(conf, false, why)) { std::cerr << "error: " << why << "\n"; return 1; }
            save_command_picks(st);
            return 0;
        }

        if (st.mem_status == MemStatus::Absent || (st.mem_status == MemStatus::Ok && st.mem.recs.empty())) {
            std::cout << "[csy] the Cooler Compiler memory is empty, nothing to wipe" << std::endl;
            speak_command(st, "ccm.memory.empty", none);
            save_command_picks(st);
            return 0;
        }
        if (!yes) {
            if (!::isatty(STDIN_FILENO)) {
                std::cerr << "error: wiping needs an answer and standard input is not a terminal; run it again with --yes\n";
                return 1;
            }
            std::cout << kWipeQuestion << " " << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            const size_t b = answer.find_first_not_of(" \t\r\n"), e = answer.find_last_not_of(" \t\r\n");
            const std::string t = b == std::string::npos ? std::string() : answer.substr(b, e - b + 1);
            if (!(t == "y" || t == "Y")) {
                std::cout << "[csy] nothing was wiped" << std::endl;
                speak_command(st, "ccm.cancelled", none);
                save_command_picks(st);
                return 0;
            }
        }
        MemLock lock(st.root / "ccm_mem.lock");
        std::error_code ec;
        fs::remove(st.root / "ccm_mem.bin", ec);
        if (ec) { std::cerr << "error: cannot remove ccm_mem.bin: " << ec.message() << "\n"; return 1; }
        std::cout << "[csy] the Cooler Compiler memory was wiped" << std::endl;
        st.picks.clear();
        speak_command(st, "ccm.wiped", none);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: the Cooler Compiler command stopped safely: " << e.what() << "\n";
        return 1;
    }
}

void begin(int argc, char** argv) {
    try {
        if (g_state) return;
        const fs::path root = provisional_root(argc, argv);
        bool present = false;
        if (!conf_value(root / "csyconf.json", present)) return;
        std::unique_ptr<State> st(new State());
        st->root = root;
        st->tty_out = ::isatty(1) != 0;
        st->tty_err = ::isatty(2) != 0;
        const char* nc = std::getenv("NO_COLOR");
        const char* term = std::getenv("TERM");
        st->no_color = (nc && *nc) || (term && std::strcmp(term, "dumb") == 0);
        st->orig_out = std::cout.rdbuf();
        st->orig_err = std::cerr.rdbuf();
        bool broken = false;
        if (!load_table_for(*st, broken)) {
            if (st->tty_err)
                std::cerr << "note: the Cooler Compiler is on but '" << (root / "ccm_table.bin").string()
                          << "' is missing or damaged; compiling without it\n";
            return;
        }
        load_memory(*st);
        if (st->mem_status == MemStatus::Broken && st->tty_err)
            std::cerr << "note: the Cooler Compiler memory '" << (root / "ccm_mem.bin").string()
                      << "' is damaged and is ignored; 'csyc wipe --ccm-mem' clears it\n";
        st->on = true;
        st->t0 = Clock::now();
        install(*st);
        g_state = st.release();
    } catch (...) {
        if (g_state) { uninstall(*g_state); g_state = nullptr; }
    }
}

void set_subject(const std::string& entry_path, bool run_mode, bool final_call) {
    if (!g_state) return;
    try {
        State& st = *g_state;
        st.subject = entry_path;
        st.subject_name = base_name(entry_path);
        st.run_mode = run_mode;
        st.has_subject = true;
        st.path_hash = hash_of_path(entry_path);
        if (!final_call) return;
        std::string raw;
        bool too_big = false;
        st.has_content = read_file_bounded(entry_path, kMaxHashedBytes, raw, too_big);
        if (st.has_content) st.content_hash = hash128(raw);
        const uint64_t bytes = raw.size();
        Fields f;
        f.set("file", st.subject_name);
        if (st.has_content && bytes >= kSourceBigBytes && !run_mode) {
            f.set("size", fmt_size(bytes));
            write_direct(st, 2, say(st, "source.big", f, 2));
        }
        if (st.mem_status != MemStatus::Broken) {
            std::string id;
            Fields mf;
            if (start_situation(st, id, mf)) write_direct(st, 2, say(st, id, mf, 2));
        }
        arm_interrupt(st);
    } catch (...) {}
}

void phase_check() {
    if (g_state) g_state->phase = Phase::Check;
}

void user_code(bool running) {
    if (!g_state) return;
    State& st = *g_state;
    try {
        if (running) {
            st.run_start = Clock::now();
            st.run_started = true;
            uninstall(st);
        } else {
            install(st);
        }
    } catch (...) {}
}

void note_rebuilt() {
    if (!g_state) return;
    try {
        State& st = *g_state;
        Fields f;
        f.set("file", st.subject_name);
        write_direct(st, 2, say(st, "run.rebuilt", f, 2));
    } catch (...) {}
}

void finish(int exit_code) {
    if (!g_state) return;
    State& st = *g_state;
    try {
        const bool success = exit_code == 0;
        uninstall(st);
        Fields f;
        f.set("file", st.subject_name);
        if (st.run_started) {
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - st.run_start).count();
            if (success && !st.saw_run_line) {
                f.set("time", fmt_duration_ms(ms));
                write_direct(st, 2, say(st, "run.success", f, 2));
            } else if (!success && st.errors == 0) {
                f.set("count", std::to_string(exit_code));
                write_direct(st, 2, say(st, "run.exit.nonzero", f, 2));
            }
        }
        if (st.has_subject) {
            const bool use_history = st.mem_status != MemStatus::Broken;
            if (use_history) {
                std::string id;
                Fields mf;
                if (end_situation(st, success, id, mf)) write_direct(st, 2, say(st, id, mf, 2));
                Rec r;
                r.type = kAttempt;
                r.flags = static_cast<uint8_t>((success ? kSuccess : 0) | (st.run_mode ? kRunMode : 0) | (st.has_content ? kHasContent : 0));
                r.errors = static_cast<uint16_t>(std::min(st.errors, 65535));
                r.warnings = static_cast<uint16_t>(std::min(st.warnings, 65535));
                r.sit = st.first_error_sit;
                r.time = now_unix();
                r.path = st.path_hash;
                if (st.has_content) r.content = st.content_hash;
                write_memory(st, &r);
            }
        }
    } catch (...) {}
    g_interrupt.armed = false;
    res::set_signal_hook(nullptr);
    g_state = nullptr;
}

std::string default_table_path() {
    std::vector<fs::path> dirs;
    std::error_code ec;
    if (const char* env = std::getenv("CSYC_DATA_DIR"); env && *env) {
        dirs.push_back(fs::path(env));
    } else {
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            dirs.push_back(self.parent_path().parent_path() / "share" / "csyc");
            dirs.push_back(self.parent_path());
        }
        dirs.push_back("/usr/local/share/csyc");
        dirs.push_back("/usr/share/csyc");
    }
    for (const auto& d : dirs) {
        const fs::path p = d / "ccm_table.bin";
        std::string raw;
        bool too_big = false;
        Table t;
        if (read_file_bounded(p, kMaxTableBytes, raw, too_big) && parse_table(raw, t)) return p.string();
    }
    return std::string();
}

TableCopy copy_default_table(const std::string& folder, std::string& detail) {
    const fs::path dest = fs::path(folder) / "ccm_table.bin";
    detail = dest.string();
    std::error_code ec;
    if (fs::exists(dest, ec)) return TableCopy::Kept;
    const std::string src = default_table_path();
    if (src.empty()) return TableCopy::NotFound;
    std::string raw;
    bool too_big = false;
    if (!read_file_bounded(src, kMaxTableBytes, raw, too_big) || !write_atomic(dest, raw)) return TableCopy::Failed;
    return TableCopy::Created;
}

}
}

