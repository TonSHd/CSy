#pragma once

#include "project.hpp"
#include <string>
#include <vector>

namespace csy {

class VM;

bool approve_process_calls(std::vector<SourceUnit>& units,
                          const std::string& project_root,
                          std::string& error);
bool process_call_is_approved(const std::string& project_root,
                              const std::string& site_key,
                              const std::string& location,
                              bool prompt,
                              std::string& error);
void register_process_builtins(VM& vm);

}
