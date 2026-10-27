#pragma once

#include "resource.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace csy {
namespace dataio {

constexpr unsigned long long kMaxFileBytes = 1ULL << 30;

inline std::string read_all(const std::string& path, const std::string& who) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        throw std::runtime_error(who + ": '" + path + "' is a directory, not a file");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(who + ": cannot open '" + path + "' (" +
                                 (std::filesystem::exists(path, ec) ? "permission denied or unreadable" : "no such file") + ")");
    }
    std::string data;
    const auto declared = std::filesystem::file_size(path, ec);
    if (!ec && declared > 0) {
        if (declared > kMaxFileBytes) {
            throw std::runtime_error(who + ": '" + path + "' is larger than the 1 GiB safety limit; "
                                     "open it with open_dataset and read it in batches instead");
        }
        data.resize(static_cast<size_t>(declared));
        in.read(&data[0], static_cast<std::streamsize>(declared));
        data.resize(static_cast<size_t>(in.gcount()));
        return data;
    }
    char chunk[65536];
    while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
        data.append(chunk, static_cast<size_t>(in.gcount()));
        if (data.size() > kMaxFileBytes) {
            throw std::runtime_error(who + ": input is larger than the 1 GiB safety limit");
        }
    }
    return data;
}

class AtomicWriter {
public:
    AtomicWriter(const std::string& path, const std::string& who) : target_(path), who_(who) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            throw std::runtime_error(who + ": '" + path + "' is a directory, not a file");
        }
        temp_ = path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
        res::register_temp_path(temp_);
        out_.open(temp_, std::ios::binary | std::ios::trunc);
        if (!out_) {
            res::unregister_temp_path(temp_);
            throw std::runtime_error(who + ": cannot write '" + path + "' (check that the folder exists and is writable)");
        }
    }

    ~AtomicWriter() {
        if (!done_) {
            if (out_.is_open()) out_.close();
            std::error_code ec;
            std::filesystem::remove(temp_, ec);
            res::unregister_temp_path(temp_);
        }
    }

    AtomicWriter(const AtomicWriter&) = delete;
    AtomicWriter& operator=(const AtomicWriter&) = delete;

    std::ofstream& stream() { return out_; }

    void write(const char* data, size_t size) {
        out_.write(data, static_cast<std::streamsize>(size));
        if (!out_) throw std::runtime_error(who_ + ": writing '" + target_ + "' failed (disk full?)");
    }
    void write(const std::string& s) { write(s.data(), s.size()); }

    void patch(std::streamoff position, const std::string& bytes) {
        const std::streamoff end = out_.tellp();
        out_.seekp(position);
        out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out_.seekp(end);
        if (!out_) throw std::runtime_error(who_ + ": writing '" + target_ + "' failed");
    }

    void commit() {
        out_.flush();
        if (!out_) throw std::runtime_error(who_ + ": writing '" + target_ + "' failed (disk full?)");
        out_.close();
        std::error_code ec;
        std::filesystem::rename(temp_, target_, ec);
        if (ec) {
            throw std::runtime_error(who_ + ": cannot replace '" + target_ + "' (" + ec.message() + ")");
        }
        done_ = true;
        res::unregister_temp_path(temp_);
    }

private:
    std::string target_;
    std::string who_;
    std::string temp_;
    std::ofstream out_;
    bool done_ = false;
};

}
}
