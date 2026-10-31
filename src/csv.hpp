#pragma once

#include "value.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>



namespace csy {
namespace csv {

struct Options {
    char delimiter = ',';
    bool header = true;
    bool has_columns = false;
    std::vector<std::string> columns;
    std::unordered_map<std::string, std::string> types;
    long long max_rows = 0;
};

Options options_from(const Value* dict, const char* who, char default_delimiter = ',');

Value read(const std::string& path, const Options& options);
void write(const std::string& path, const Value& table, char delimiter);

class Reader {
public:
    Reader(const std::string& path, const Options& options);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    Value next(long long rows);
    void reset();
    long long rows_read() const;
    const std::vector<std::string>& names() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Writer {
public:
    Writer(const std::string& path, char delimiter);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void append(const Value& table);
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
// Haiii :3
