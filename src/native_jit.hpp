// Written by ChatGPT, Claude
#pragma once

#include "ast.hpp"
#include "ir.hpp"
#include <string>
#include <vector>

namespace csy {

// What happened to each function, for `--explain-native` and the build summary.
struct NativeFunctionReport {
    std::string name;
    int line = 0;
    bool native = false;      // compiled natively (at least one variant)
    std::string detail;       // why it stayed interpreted, or which variants exist
};
struct NativeReport {
    std::vector<NativeFunctionReport> functions;
    size_t variants = 0;           // native variants emitted
    size_t interpreted_only = 0;   // methods / closures / nested functions (never candidates)
    size_t top_level_loops = 0;    // for/while loops left in module code (interpreted)
    std::vector<NativeFunctionReport> kept_loops;  // why individual top-level loops were not outlined
    bool library_ready = false;    // a compiled library is loaded (run) or embedded (build)
    std::string status;            // one-line global note ("no C compiler", "cached", ...)
};

enum class NativeMode {
    Run,     // compile if needed and bind the result into `ir` (the program runs in this process)
    Build,   // compile if needed, embed in `ir`, do not bind (a .cso is being written)
};

void outline_top_level_loops(Module& mod);

// Scans `mod` for eligible top-level functions, infers their parameter/
// return types, generates C source, compiles it, and - on success -
// fills in `native_variants` on the matching entries of `ir.functions`
// (matched by name). The generated C and, when a C compiler exists, the built
// shared library are also stored in `ir` so object files carry them.
// `embed_library` controls whether the library image is read back into `ir`
// (needed only when an object file will be written).
// Always safe to call: any failure at any stage (ineligible function,
// no C compiler found, compile error, dlopen failure) just leaves the
// function purely interpreted.
//
// Set CSY_NO_NATIVE_JIT=1 to skip this entirely and always run purely
// interpreted. Set CSY_JIT_DEBUG=1 for a running commentary on stderr.
NativeReport try_native_compile(const Module& mod, IRModule& ir,
                                NativeMode mode = NativeMode::Run, bool embed_library = false);

// For a loaded .cso: binds the native code carried in the object (embedded
// library first, then embedded C source through the cache/C compiler).
// Returns the number of native variants bound (0 = everything interpreted).
size_t attach_embedded_native(IRModule& ir, std::string* note = nullptr);

// Human-readable table for --explain-native.
std::string format_native_report(const NativeReport& report);
// One-line summary used at build time.
std::string summarize_native_report(const NativeReport& report);

// "linux-x86_64"-style identifier for the machine a library was built for.
std::string native_host_target();

}
