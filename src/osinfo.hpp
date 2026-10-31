#pragma once
#include <string>
#include <vector>

namespace csy {
namespace osinfo {

struct Os {
    std::string kind;
    std::string id;
    std::string name;
    std::string family;
    std::string arch;
};

Os detect_os();
bool supported(const Os& os);

std::string find_in_path(const std::string& program);
std::string find_c_compiler(std::string& name);

struct PackagePlan {
    bool known = false;
    std::string manager;
    std::string packages;
    std::vector<std::vector<std::string>> steps;
    std::string why_not;
};
PackagePlan c_compiler_plan(const Os& os);

std::vector<std::string> privilege_prefix(bool& possible);
bool is_root();
std::string join_command(const std::vector<std::string>& argv);
int run_command(const std::vector<std::string>& argv);

void ensure_c_compiler(const Os& os, bool assume_yes, bool dry_run);

}
}

