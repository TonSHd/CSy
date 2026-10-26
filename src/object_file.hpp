#pragma once
#include "ir.hpp"
#include <string>

namespace csy {
bool write_object_file(const std::string& path, const IRModule& module, std::string& error);
bool verify_module(const IRModule& module, std::string& why);
bool read_object_file(const std::string& path, IRModule& module, std::string& error);
bool read_object_image(const std::string& image, const std::string& label, IRModule& module, std::string& error);
}
