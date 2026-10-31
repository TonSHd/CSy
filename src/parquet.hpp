#pragma once

#include "value.hpp"
#include <memory>
#include <string>
#include <vector>

namespace csy {
namespace parquet {

constexpr unsigned long long kMaxCells = 1ULL << 27;

struct ReadOptions {
    bool has_columns = false;
    std::vector<std::string> columns;
    long long max_rows = 0;
};

class Reader {
public:
    Reader(const std::string& path, const ReadOptions& options);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    const std::vector<std::string>& names() const;
    long long rows() const;
    size_t groups() const;
    long long group_rows(size_t group) const;
    Value read_group(size_t group);
    Value read_all();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Writer {
public:
    explicit Writer(const std::string& path, bool streaming = true);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void append(const Value& table);
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Value read(const std::string& path, const ReadOptions& options);

void write(const std::string& path, const Value& table);

}
}

