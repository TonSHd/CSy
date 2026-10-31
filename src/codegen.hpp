#pragma once

#include "ast.hpp"
#include "ir.hpp"
#include "diagnostic.hpp"

#include <vector>
#include <string>
#include <utility>


namespace csy {

struct CodegenResult {
    IRModule module;
    DiagBag diags;
    bool ok = false;
};

CodegenResult generate(const Module& ast,
                       const std::vector<std::pair<std::string,std::string>>& using_aliases,
                       const std::string& source,
                       const std::vector<std::string>& builtin_names);

}

