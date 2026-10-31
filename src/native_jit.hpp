#pragma once

#include "ast.hpp"
#include "ir.hpp"
#include <string>
#include <vector>

namespace csy {

struct NativeFunctionReport {
    std::string name;
    int line = 0;
    bool native = false;
    std::string detail;
};
struct NativeReport {
    std::vector<NativeFunctionReport> functions;
    size_t variants = 0;
    size_t interpreted_only = 0;
    size_t top_level_loops = 0;
    std::vector<NativeFunctionReport> kept_loops;
    bool library_ready = false;
    std::string status;
};

enum class NativeMode {
    Run,
    Build,
};

void outline_top_level_loops(Module& mod);

NativeReport try_native_compile(const Module& mod, IRModule& ir,
                                NativeMode mode = NativeMode::Run, bool embed_library = false);

size_t attach_embedded_native(IRModule& ir, std::string* note = nullptr);

std::string format_native_report(const NativeReport& report);
std::string summarize_native_report(const NativeReport& report);

std::string native_host_target();

}

