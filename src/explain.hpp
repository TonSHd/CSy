#pragma once
#include "project.hpp"
#include <string>
#include <vector>

namespace csy {

int explain_command(int argc, char** argv);

std::vector<std::string> explain_warnings(const std::vector<SourceUnit>& units);

}
