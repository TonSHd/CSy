#pragma once

#include "ast.hpp"
#include "diagnostic.hpp"
#include <vector>
#include <string>

namespace csy {

struct ParseResult {
    Module module;
    DiagBag diags;
    bool ok = false;
};

ParseResult parse(const std::vector<Token>& tokens,
                  const std::string& file,
                  const std::string& source);

}
