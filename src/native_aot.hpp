#pragma once

#include "ast.hpp"
#include "ir.hpp"
#include "native_jit.hpp"
#include <string>

namespace csy {

bool write_native_aot(const Module& source, const IRModule& ir,
                      const NativeReport& report, const std::string& output,
                      std::string& error);

}
