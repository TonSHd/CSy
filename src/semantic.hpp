#pragma once

#include "ast.hpp"
#include "diagnostic.hpp"

namespace csy {

struct SemanticResult {
    DiagBag diags;
    bool ok = false;
    std::vector<std::string> import_aliases;
    std::vector<std::pair<std::string, std::string>> using_aliases;
};

SemanticResult analyze(const Module& mod, const std::string& source,
                        const std::vector<std::string>& builtin_names);

}

