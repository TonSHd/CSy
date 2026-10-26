#pragma once
#include "project.hpp"
#include <string>
#include <vector>

namespace csy {
bool namespace_project_symbols(std::vector<SourceUnit>& units, std::string& error,
                               std::vector<std::string>* warnings = nullptr);

std::vector<std::string> top_level_names(const Module& module, bool include_private = false);
}
