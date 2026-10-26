#pragma once

#include <string>
#include <vector>

namespace csy {

std::vector<std::string> suggest(const std::string& target,
                                 const std::vector<std::string>& candidates,
                                 int max_distance = 3);

std::string suggest_one(const std::string& target,
                        const std::vector<std::string>& candidates,
                        int max_distance = 3);

std::string did_you_mean(const std::string& target,
                         const std::vector<std::string>& candidates);

}
