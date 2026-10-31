#include "object_file.hpp"
#include "resource.hpp"
#include "hash128.hpp"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <streambuf>
#include <cerrno>
#include <deque>

namespace csy {
namespace {
constexpr char kMagic[8] = {'C','S','Y','C','O',1,'\r','\n'};
constexpr uint64_t kMaxItems = 1000000;
constexpr uint64_t kMaxStringBytes = 32 * 1024 * 1024;
constexpr uint64_t kMaxObjectBytes = 512 * 1024 * 1024;
constexpr uint64_t kMaxWriteBytes = 512 * 1024 * 1024;
constexpr uint8_t kVersionPlain = 1;
constexpr uint8_t kVersionNative = 2;
constexpr uint8_t kVersionNative3 = 3;
constexpr uint64_t kNativeTag = 0x5654414e5359434fULL;
constexpr uint64_t kMaxNativeBytes = 80ull * 1024 * 1024;

class BoundedFdBuf : public std::streambuf {
public:
    BoundedFdBuf(int fd, uint64_t maximum) : fd_(fd), maximum_(maximum) { buf_.reserve(kChunk); }
    uint64_t written() const { return written_ + buf_.size(); }
    bool failed() const { return failed_; }
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (failed_ || size < 0 || static_cast<uint64_t>(size) > maximum_ - written()) { failed_ = true; return 0; }
        if (buf_.size() + static_cast<size_t>(size) > kChunk) {
            if (!drain()) return 0;
            if (static_cast<size_t>(size) >= kChunk) return raw_write(data, size) ? size : 0;
        }
        buf_.insert(buf_.end(), data, data + size);
        return size;
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        const char c = traits_type::to_char_type(ch);
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }
    int sync() override { return drain() ? 0 : -1; }
private:
    static constexpr size_t kChunk = 64 * 1024;
    bool drain() {
        if (buf_.empty()) return !failed_;
        const bool ok = raw_write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
        buf_.clear();
        return ok;
    }
    bool raw_write(const char* data, std::streamsize size) {
        std::streamsize total = 0;
        while (total < size) {
            const ssize_t n = ::write(fd_, data + total, static_cast<size_t>(size - total));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { failed_ = true; return false; }
            total += n;
            written_ += static_cast<uint64_t>(n);
        }
        return true;
    }
    int fd_;
    uint64_t maximum_;
    uint64_t written_ = 0;
    bool failed_ = false;
    std::vector<char> buf_;
};

class MemBuf : public std::streambuf {
public:
    MemBuf(const char* data, size_t size) {
        char* p = const_cast<char*>(data);
        setg(p, p, p + size);
    }
protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
        char* target = nullptr;
        if (dir == std::ios_base::beg) target = eback() + off;
        else if (dir == std::ios_base::cur) target = gptr() + off;
        else target = egptr() + off;
        if (target < eback() || target > egptr()) return pos_type(off_type(-1));
        setg(eback(), target, egptr());
        return pos_type(target - eback());
    }
    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }
};

uint64_t remaining_bytes(std::istream& in) {
    const auto here = in.tellg();
    if (here < 0) return 0;
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    in.seekg(here);
    if (end < here || end < 0) return 0;
    return static_cast<uint64_t>(end - here);
}

template<class T> bool put(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return bool(out);
}
template<class T> bool get(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return bool(in);
}
bool put_string(std::ostream& out, const std::string& s) {
    if (s.size() > kMaxStringBytes) return false;
    uint64_t n = s.size(); return put(out, n) && (out.write(s.data(), (std::streamsize)n), bool(out));
}
bool get_string(std::istream& in, std::string& s) {
    uint64_t n = 0; if (!get(in, n) || n > kMaxStringBytes || n > remaining_bytes(in)) return false;
    s.resize((size_t)n); in.read(s.data(), (std::streamsize)n); return bool(in);
}
bool put_blob(std::ostream& out, const std::string& s) {
    if (s.size() > kMaxNativeBytes) return false;
    const Hash128 h = hash128(s);
    uint64_t n = s.size();
    return put(out, n) && put(out, h.a) && put(out, h.b) && (out.write(s.data(), (std::streamsize)n), bool(out));
}
bool get_blob(std::istream& in, std::string& s) {
    uint64_t n = 0; Hash128 h;
    if (!get(in, n) || !get(in, h.a) || !get(in, h.b) || n > kMaxNativeBytes || n > remaining_bytes(in)) return false;
    s.resize((size_t)n); in.read(s.data(), (std::streamsize)n);
    if (!in) return false;
    const Hash128 actual = hash128(s);
    return actual.a == h.a && actual.b == h.b;
}
template<class T, class F> bool put_vector(std::ostream& out, const std::vector<T>& v, F write_item) {
    if (v.size() > kMaxItems) return false;
    uint64_t n = v.size(); if (!put(out, n)) return false;
    for (const auto& x : v) if (!write_item(x)) return false;
    return true;
}
template<class T, class F> bool get_vector(std::istream& in, std::vector<T>& v,
                                           uint64_t minimum_item_bytes, F read_item) {
    uint64_t n = 0;
    if (!get(in, n) || n > kMaxItems ||
        (minimum_item_bytes != 0 && n > remaining_bytes(in) / minimum_item_bytes)) return false;
    v.resize((size_t)n); for (auto& x : v) if (!read_item(x)) return false;
    return true;
}

bool validate_module(const IRModule& module) {
    if (module.main_func_index < 0 ||
        static_cast<size_t>(module.main_func_index) >= module.functions.size()) return false;
    const auto contains = [](int value, size_t size) {
        return value >= 0 && static_cast<size_t>(value) < size;
    };
    for (const auto& fn : module.functions) {
        if (fn.num_locals < 0 || static_cast<uint64_t>(fn.num_locals) > kMaxItems ||
            fn.param_names.size() > static_cast<size_t>(fn.num_locals) ||
            fn.code.size() > kMaxItems || fn.upvalue_names.size() > kMaxItems) return false;
        for (const auto& ins : fn.code) {
            if (ins.line < 0 || ins.col < 0) return false;
            switch (ins.op) {
                case Op::PUSH_BOOL:
                    if (ins.arg != 0 && ins.arg != 1) return false;
                    break;
                case Op::PUSH_INT: if (!contains(ins.arg, module.int_pool.size())) return false; break;
                case Op::PUSH_FLOAT: if (!contains(ins.arg, module.float_pool.size())) return false; break;
                case Op::PUSH_STR: case Op::PUSH_SINK:
                    if (!contains(ins.arg, module.str_pool.size())) return false;
                    break;
                case Op::PUSH_FUNC: case Op::MAKE_CLOSURE:
                    if (!contains(ins.arg, module.functions.size())) return false;
                    break;
                case Op::PUSH_BUILTIN:
                    if (!contains(ins.arg, module.builtin_names.size())) return false;
                    break;
                case Op::LOAD_LOCAL: case Op::STORE_LOCAL:
                case Op::LOAD_LOCAL_BOXED: case Op::STORE_LOCAL_BOXED:
                    if (!contains(ins.arg, static_cast<size_t>(fn.num_locals))) return false;
                    break;
                case Op::LOAD_UPVALUE: case Op::STORE_UPVALUE: case Op::LOAD_UPVALUE_BOX:
                    if (!contains(ins.arg, fn.upvalue_names.size())) return false;
                    break;
                case Op::LOAD_GLOBAL: case Op::STORE_GLOBAL:
                    if (!contains(ins.arg, module.global_names.size())) return false;
                    break;
                case Op::JMP: case Op::JMPF: case Op::JMPR:
                    if (ins.arg < 0 || static_cast<size_t>(ins.arg) > fn.code.size()) return false;
                    break;
                case Op::TRY_ENTER:
                    if (ins.arg < 0 || static_cast<size_t>(ins.arg) >= fn.code.size()) return false;
                    break;
                case Op::CALL: case Op::MAKE_VEC:
                    if (ins.arg < 0 || static_cast<uint64_t>(ins.arg) > kMaxItems) return false;
                    break;
                case Op::MAKE_DICT:
                    if (ins.arg < 0 || static_cast<uint64_t>(ins.arg) > kMaxItems / 2) return false;
                    break;
                case Op::CALL_NATIVE: {
                    if (ins.arg < 0) return false;
                    const size_t builtin = static_cast<unsigned int>(ins.arg) / 1000;
                    if (builtin >= module.builtin_names.size()) return false;
                    break;
                }
                default: break;
            }
        }
        if (fn.code.empty()) continue;
        std::vector<int64_t> depths(fn.code.size() + 1, -1);
        std::deque<size_t> pending;
        depths[0] = 0;
        pending.push_back(0);
        auto edge = [&](size_t target, int64_t depth) {
            if (target > fn.code.size() || depth < 0 || depth > 1000000) return false;
            if (depths[target] == -1) {
                depths[target] = depth;
                pending.push_back(target);
                return true;
            }
            return depths[target] == depth;
        };
        while (!pending.empty()) {
            const size_t ip = pending.front();
            pending.pop_front();
            if (ip == fn.code.size()) continue;
            const Instr& ins = fn.code[ip];
            const int64_t depth = depths[ip];
            int64_t required = 0, delta = 0;
            bool terminal = false;
            switch (ins.op) {
                case Op::PUSH_NONE: case Op::PUSH_BOOL: case Op::PUSH_INT: case Op::PUSH_FLOAT:
                case Op::PUSH_STR: case Op::PUSH_FUNC: case Op::PUSH_BUILTIN: case Op::PUSH_SINK:
                case Op::PUSH_SELF: case Op::LOAD_LOCAL: case Op::LOAD_LOCAL_BOXED:
                case Op::LOAD_GLOBAL: case Op::LOAD_UPVALUE: case Op::LOAD_UPVALUE_BOX:
                    delta = 1; break;
                case Op::STORE_LOCAL: case Op::STORE_LOCAL_BOXED: case Op::STORE_GLOBAL:
                case Op::STORE_UPVALUE: case Op::POP:
                    required = 1; delta = -1; break;
                case Op::MAKE_CLOSURE:
                    required = static_cast<int64_t>(module.functions[ins.arg].upvalue_names.size());
                    delta = 1 - required; break;
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD:
                case Op::EQ: case Op::NEQ: case Op::LT: case Op::GT: case Op::LE: case Op::GE:
                case Op::AND: case Op::OR: case Op::GET_INDEX:
                    required = 2; delta = -1; break;
                case Op::NEG: case Op::NOT:
                    required = 1; break;
                case Op::DUP:
                    required = 1; delta = 1; break;
                case Op::MAKE_VEC:
                    required = ins.arg; delta = 1 - required; break;
                case Op::MAKE_DICT:
                    required = static_cast<int64_t>(ins.arg) * 2; delta = 1 - required; break;
                case Op::SET_INDEX:
                    required = 3; delta = -3; break;
                case Op::OUTPUT:
                    required = 2; delta = -1; break;
                case Op::JMP:
                    terminal = true; break;
                case Op::JMPF:
                    required = 1; delta = -1; break;
                case Op::JMPR:
                    required = 1;
                    if (!edge(static_cast<size_t>(ins.arg), depth) || !edge(ip + 1, depth - 1)) return false;
                    terminal = true;
                    break;
                case Op::CALL:
                    required = static_cast<int64_t>(ins.arg) + 1;
                    delta = -static_cast<int64_t>(ins.arg); break;
                case Op::CALL_NATIVE: {
                    const int64_t count = ins.arg % 1000;
                    required = count; delta = 1 - count; break;
                }
                case Op::RET:
                    required = 1; terminal = true; break;
                case Op::HALT:
                    terminal = true; break;
                case Op::LEN:
                    required = 1; delta = 0; break;
                case Op::VEC_PUSH:
                    required = 2; delta = -1; break;
                case Op::TRY_ENTER:
                    if (!edge(static_cast<size_t>(ins.arg), depth + 1)) return false;
                    break;
                case Op::TRY_EXIT:
                    break;
                case Op::Q_BIN: case Op::Q_BIN_STORE: case Op::Q_BIN_JMPF:
                case Op::Q_CMP_JMPF: case Op::Q_GETIDX:
                    return false;
            }
            if (depth < required || depth + delta < 0 || depth + delta > 1000000) return false;
            const int64_t after = depth + delta;
            if (terminal) {
                if (ins.op == Op::JMP && !edge(static_cast<size_t>(ins.arg), after)) return false;
                continue;
            }
            if (ins.op == Op::JMPF && !edge(static_cast<size_t>(ins.arg), after)) return false;
            if (!edge(ip + 1, after)) return false;
        }
    }
    return true;
}
}

bool write_object_file(const std::string& path, const IRModule& m, std::string& error) {
    std::string temp_path = path + ".tmp.XXXXXX";
    std::vector<char> temp_name(temp_path.begin(), temp_path.end());
    temp_name.push_back('\0');
    const int fd = ::mkstemp(temp_name.data());
    if (fd < 0) { error = "cannot create temporary object file for '" + path + "'"; return false; }
    temp_path = temp_name.data();
    res::register_temp_path(temp_path);
    if (::fchmod(fd, 0644) != 0) {
        res::unregister_temp_path(temp_path);
        ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        error = "cannot set permissions on temporary object file";
        return false;
    }
    BoundedFdBuf buffer(fd, kMaxWriteBytes);
    std::ostream out(&buffer);
    const bool has_native = !m.native_c_source.empty() || !m.native_library.empty() || !m.native_x64.empty();
    char magic[sizeof(kMagic)];
    std::memcpy(magic, kMagic, sizeof(kMagic));
    magic[5] = static_cast<char>(has_native ? kVersionNative3 : kVersionPlain);
    out.write(magic, sizeof(magic));
    bool ok = put_string(out, m.file) && put(out, m.main_func_index) &&
        put_vector(out, m.int_pool, [&](long long x){ return put(out, x); }) &&
        put_vector(out, m.float_pool, [&](double x){ return put(out, x); }) &&
        put_vector(out, m.str_pool, [&](const std::string& x){ return put_string(out, x); }) &&
        put_vector(out, m.global_names, [&](const std::string& x){ return put_string(out, x); }) &&
        put_vector(out, m.builtin_names, [&](const std::string& x){ return put_string(out, x); }) &&
        put_vector(out, m.functions, [&](const IRFunction& fn) {
            return put_string(out, fn.name) &&
                put_vector(out, fn.param_names, [&](const std::string& x){ return put_string(out, x); }) &&
                put(out, fn.num_locals) &&
                put_vector(out, fn.upvalue_names, [&](const std::string& x){ return put_string(out, x); }) &&
                put_vector(out, fn.code, [&](const Instr& ins) {
                    const uint8_t op = (uint8_t)ins.op;
                    return put(out, op) && put(out, ins.arg) && put(out, ins.line) && put(out, ins.col);
                });
        });
    if (ok && has_native) {
        ok = put(out, kNativeTag) && put_string(out, m.native_target) &&
             put_blob(out, m.native_c_source) && put_blob(out, m.native_library) &&
             put_blob(out, m.native_x64);
    }
    out.flush();
    const bool flushed = bool(out) && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!ok || !flushed || !closed || buffer.failed()) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        res::unregister_temp_path(temp_path);
        error = buffer.written() >= kMaxWriteBytes
            ? "compiled object exceeds the 512 MiB output limit"
            : "failed while writing object file '" + path + "'";
        return false;
    }
    if (::rename(temp_path.c_str(), path.c_str()) != 0) {
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        res::unregister_temp_path(temp_path);
        error = "cannot replace object file '" + path + "'";
        return false;
    }
    res::unregister_temp_path(temp_path);
    return true;
}

bool read_object_image(const std::string& image, const std::string& label, IRModule& m, std::string& error) {
    MemBuf membuf(image.data(), image.size());
    std::istream in(&membuf);
    char magic[sizeof(kMagic)]{}; in.read(magic, sizeof(magic));
    char expected[sizeof(kMagic)];
    std::memcpy(expected, kMagic, sizeof(kMagic));
    expected[5] = magic[5];
    const uint8_t version = static_cast<uint8_t>(magic[5]);
    if (!in || std::memcmp(magic, expected, sizeof(kMagic)) != 0 ||
        (version != kVersionPlain && version != kVersionNative && version != kVersionNative3)) {
        error = "'" + label + "' is not a supported CSy object file"; return false;
    }
    bool ok = get_string(in, m.file) && get(in, m.main_func_index) &&
        get_vector(in, m.int_pool, sizeof(long long), [&](long long& x){ return get(in, x); }) &&
        get_vector(in, m.float_pool, sizeof(double), [&](double& x){ return get(in, x); }) &&
        get_vector(in, m.str_pool, sizeof(uint64_t), [&](std::string& x){ return get_string(in, x); }) &&
        get_vector(in, m.global_names, sizeof(uint64_t), [&](std::string& x){ return get_string(in, x); }) &&
        get_vector(in, m.builtin_names, sizeof(uint64_t), [&](std::string& x){ return get_string(in, x); }) &&
        get_vector(in, m.functions, 36, [&](IRFunction& fn) {
            return get_string(in, fn.name) &&
                get_vector(in, fn.param_names, sizeof(uint64_t), [&](std::string& x){ return get_string(in, x); }) &&
                get(in, fn.num_locals) &&
                get_vector(in, fn.upvalue_names, sizeof(uint64_t), [&](std::string& x){ return get_string(in, x); }) &&
                get_vector(in, fn.code, 13, [&](Instr& ins) {
                    uint8_t op = 0;
                    if (!get(in, op) || op > (uint8_t)Op::VEC_PUSH) return false;
                    ins.op = (Op)op;
                    return get(in, ins.arg) && get(in, ins.line) && get(in, ins.col);
                });
        });
    if (ok && (version == kVersionNative || version == kVersionNative3)) {
        uint64_t tag = 0;
        ok = get(in, tag) && tag == kNativeTag && get_string(in, m.native_target) &&
             get_blob(in, m.native_c_source) && get_blob(in, m.native_library) &&
             (version == kVersionNative || get_blob(in, m.native_x64));
    }
    if (!ok || remaining_bytes(in) != 0 || !validate_module(m)) {
        error = "object file '" + label + "' is truncated or invalid"; return false;
    }
    for (size_t i = 0; i < m.int_pool.size(); ++i) m.int_index.emplace(m.int_pool[i], (int)i);
    for (size_t i = 0; i < m.float_pool.size(); ++i) m.float_index.emplace(m.float_pool[i], (int)i);
    for (size_t i = 0; i < m.str_pool.size(); ++i) m.str_index.emplace(m.str_pool[i], (int)i);
    for (size_t i = 0; i < m.global_names.size(); ++i) m.global_index.emplace(m.global_names[i], (int)i);
    for (size_t i = 0; i < m.builtin_names.size(); ++i) m.builtin_index.emplace(m.builtin_names[i], (int)i);
    return true;
}

bool read_object_file(const std::string& path, IRModule& m, std::string& error) {
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size > kMaxObjectBytes) {
        error = "object file exceeds the 512 MiB safety limit or cannot be sized";
        return false;
    }
    std::string image;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) { error = "cannot read object file '" + path + "'"; return false; }
        image.resize(static_cast<size_t>(file_size));
        file.read(image.data(), static_cast<std::streamsize>(file_size));
        if (static_cast<uint64_t>(file.gcount()) != file_size) {
            error = "object file '" + path + "' changed while it was being read"; return false;
        }
    }
    return read_object_image(image, path, m, error);
}

bool verify_module(const IRModule& module, std::string& why) {
    const auto too_big = [&](const std::string& what, uint64_t n) {
        why = what + " has " + std::to_string(n) + " entries (limit " +
              std::to_string(kMaxItems) + ") - split the program into smaller functions or files";
        return false;
    };
    if (module.functions.size() > kMaxItems) return too_big("the program's function table", module.functions.size());
    if (module.int_pool.size() > kMaxItems) return too_big("the integer-constant pool", module.int_pool.size());
    if (module.float_pool.size() > kMaxItems) return too_big("the float-constant pool", module.float_pool.size());
    if (module.str_pool.size() > kMaxItems) return too_big("the string-constant pool", module.str_pool.size());
    if (module.global_names.size() > kMaxItems) return too_big("the global-variable table", module.global_names.size());
    for (const auto& fn : module.functions) {
        if (fn.code.size() > kMaxItems) return too_big("function '" + fn.name + "' (compiled code)", fn.code.size());
        if (fn.num_locals < 0 || static_cast<uint64_t>(fn.num_locals) > kMaxItems)
            return too_big("function '" + fn.name + "' (local variables)", static_cast<uint64_t>(fn.num_locals < 0 ? 0 : fn.num_locals));
    }
    if (!validate_module(module)) {
        why = "generated bytecode failed structural verification (internal compiler error - please report this program)";
        return false;
    }
    return true;
}

}

