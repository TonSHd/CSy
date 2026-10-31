#pragma once

#include "value.hpp"
#include <string>

namespace csy {
namespace json {

constexpr int kMaxDepth = 256;
constexpr unsigned long long kMaxTextBytes = 1ULL << 30;

Value parse(const char* data, size_t size, size_t first_line = 1);
inline Value parse(const std::string& text) { return parse(text.data(), text.size()); }

void stringify_to(std::string& out, const Value& value, bool pretty);
std::string stringify(const Value& value, bool pretty = false);

bool valid_utf8(const char* data, size_t size);

}
}

