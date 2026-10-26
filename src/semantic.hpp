// Written by Claude
#pragma once

#include "ast.hpp"
#include "diagnostic.hpp"

namespace csy {

struct SemanticResult {
    DiagBag diags;
    bool ok = false;
    // The set of imported module aliases (e.g. "bc").
    std::vector<std::string> import_aliases;
    // The set of using-aliases (e.g. "o" → maps to bc.out sink).
    std::vector<std::pair<std::string, std::string>> using_aliases;
};

SemanticResult analyze(const Module& mod, const std::string& source,
                        const std::vector<std::string>& builtin_names);

} // namespace csy
