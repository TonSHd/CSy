#include "osinfo.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace csy {
namespace osinfo {
namespace {

std::string strip_quotes(std::string v) {
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
}

std::map<std::string, std::string> read_os_release() {
    std::map<std::string, std::string> kv;
    std::vector<std::string> files;
    if (const char* over = std::getenv("CSYC_OS_RELEASE")) files.push_back(over);
    else { files.push_back("/etc/os-release"); files.push_back("/usr/lib/os-release"); }
    for (const std::string& f : files) {
        std::ifstream in(f);
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
            kv[line.substr(0, eq)] = strip_quotes(line.substr(eq + 1));
        }
        break;
    }
    return kv;
}

std::string family_of(const std::string& id, const std::string& like) {
    static const struct { const char* family; const char* ids; } table[] = {
        {"debian", " debian ubuntu linuxmint pop raspbian kali elementary zorin neon devuan "},
        {"fedora", " fedora rhel centos rocky almalinux ol amzn nobara "},
        {"arch", " arch manjaro endeavouros artix garuda cachyos "},
        {"suse", " opensuse opensuse-leap opensuse-tumbleweed sles sled suse "},
        {"alpine", " alpine postmarketos "},
        {"void", " void "},
        {"gentoo", " gentoo "},
    };
    std::vector<std::string> tokens{id};
    std::stringstream ss(like);
    for (std::string t; ss >> t;) tokens.push_back(t);
    for (const std::string& t : tokens)
        for (const auto& row : table)
            if (std::string(row.ids).find(" " + t + " ") != std::string::npos) return row.family;
    return std::string();
}

}

Os detect_os() {
    Os os;
#if defined(__linux__)
    os.kind = "linux";
#elif defined(__APPLE__)
    os.kind = "macos";
#elif defined(_WIN32)
    os.kind = "windows";
#else
    os.kind = "unknown";
#endif
    if (const char* k = std::getenv("CSYC_OS_KIND")) os.kind = k;
    struct utsname u;
    if (::uname(&u) == 0) os.arch = u.machine;
    if (os.kind == "linux") {
        auto kv = read_os_release();
        os.id = kv.count("ID") ? kv["ID"] : "";
        os.name = kv.count("PRETTY_NAME") ? kv["PRETTY_NAME"] : (kv.count("NAME") ? kv["NAME"] : "Linux");
        os.family = family_of(os.id, kv.count("ID_LIKE") ? kv["ID_LIKE"] : "");
    } else if (os.kind == "macos") {
        os.name = "macOS";
    } else if (os.kind == "windows") {
        os.name = "Windows";
    } else {
        os.name = "an unknown system";
    }
    return os;
}

bool supported(const Os& os) { return os.kind == "linux"; }

std::string find_in_path(const std::string& program) {
    if (program.empty()) return std::string();
    if (program.find('/') != std::string::npos) return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
    const char* path = std::getenv("PATH");
    if (!path) return std::string();
    std::stringstream ss(path);
    for (std::string dir; std::getline(ss, dir, ':');) {
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + program;
        struct stat st;
        if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(full.c_str(), X_OK) == 0) return full;
    }
    return std::string();
}

std::string find_c_compiler(std::string& name) {
    if (const char* cc = std::getenv("CC")) {
        std::string first = cc;
        first = first.substr(0, first.find(' '));
        const std::string found = find_in_path(first);
        if (!found.empty()) { name = first; return found; }
    }
    for (const char* candidate : {"cc", "gcc", "clang"}) {
        const std::string found = find_in_path(candidate);
        if (!found.empty()) { name = candidate; return found; }
    }
    return std::string();
}

PackagePlan c_compiler_plan(const Os& os) {
    PackagePlan plan;
    plan.packages = "gcc";
    auto have = [&](const char* m) { return !find_in_path(m).empty(); };
    if (os.family == "debian") {
        plan.manager = "apt-get";
        plan.steps = {{"apt-get", "update"}, {"apt-get", "install", "-y", "gcc"}};
    } else if (os.family == "fedora") {
        plan.manager = have("dnf") ? "dnf" : (have("dnf5") ? "dnf5" : "yum");
        plan.steps = {{plan.manager, "install", "-y", "gcc"}};
    } else if (os.family == "arch") {
        plan.manager = "pacman";
        plan.steps = {{"pacman", "-S", "--needed", "--noconfirm", "gcc"}};
    } else if (os.family == "suse") {
        plan.manager = "zypper";
        plan.steps = {{"zypper", "--non-interactive", "install", "gcc"}};
    } else if (os.family == "alpine") {
        plan.manager = "apk";
        plan.packages = "gcc musl-dev";
        plan.steps = {{"apk", "add", "gcc", "musl-dev"}};
    } else if (os.family == "void") {
        plan.manager = "xbps-install";
        plan.steps = {{"xbps-install", "-y", "gcc"}};
    } else {
        plan.why_not = os.family == "gentoo" ? "Gentoo builds compilers from source, so csyc leaves that to you (emerge sys-devel/gcc)"
                                              : "this distribution is not recognised, so there is no package manager to choose";
        return plan;
    }
    if (!have(plan.manager.c_str())) { plan.why_not = "'" + plan.manager + "' was not found in PATH"; return plan; }
    plan.known = true;
    return plan;
}

bool is_root() { return ::geteuid() == 0; }

std::vector<std::string> privilege_prefix(bool& possible) {
    possible = true;
    if (is_root()) return {};
    if (!find_in_path("sudo").empty()) return {"sudo"};
    if (!find_in_path("doas").empty()) return {"doas"};
    possible = false;
    return {};
}

std::string join_command(const std::vector<std::string>& argv) {
    std::string out;
    for (const std::string& a : argv) {
        if (!out.empty()) out += ' ';
        const bool plain = !a.empty() && a.find_first_of(" \t'\"\\$`&|;<>()*?[]#~") == std::string::npos;
        out += plain ? a : "'" + a + "'";
    }
    return out;
}

int run_command(const std::vector<std::string>& argv) {
    if (argv.empty()) return 127;
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = ::fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 127;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
}

void ensure_c_compiler(const Os& os, bool assume_yes, bool dry_run) {
    std::string name;
    const std::string have = find_c_compiler(name);
    if (!have.empty()) {
        std::cout << "[csy] C compiler: found " << have << "\n";
        return;
    }
    const PackagePlan plan = c_compiler_plan(os);
    std::cout << "[csy] C compiler: not found. It is optional; it enables the second native backend (csyc works without it).\n";
    if (!plan.known) {
        std::cout << "note: " << plan.why_not << "; install a package that provides 'cc' yourself if you want it\n";
        return;
    }
    bool possible = true;
    const std::vector<std::string> prefix = privilege_prefix(possible);
    std::string shown;
    for (const auto& step : plan.steps) {
        std::vector<std::string> full = prefix;
        full.insert(full.end(), step.begin(), step.end());
        shown += (shown.empty() ? "" : " && ") + join_command(full);
    }
    if (!possible) {
        std::cout << "note: to install it you need administrator rights, and neither sudo nor doas was found; as root run: " << shown << "\n";
        return;
    }
    if (dry_run) {
        std::cout << "[csy] would run: " << shown << "\n";
        return;
    }
    if (!assume_yes) {
        if (!::isatty(STDIN_FILENO)) {
            std::cout << "note: not a terminal, so the C compiler was not installed; run 'csyc install --yes' to install it with: " << shown << "\n";
            return;
        }
        std::cout << "Install " << plan.packages << " with " << plan.manager << " now? It runs: " << shown
                  << (prefix.empty() ? "" : "\n(you will be asked for your password)") << "\n[y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        const size_t b = answer.find_first_not_of(" \t\r\n"), e = answer.find_last_not_of(" \t\r\n");
        const std::string t = b == std::string::npos ? std::string() : answer.substr(b, e - b + 1);
        if (!(t == "y" || t == "Y" || t == "yes" || t == "YES")) {
            std::cout << "[csy] skipped the C compiler\n";
            return;
        }
    }
    for (const auto& step : plan.steps) {
        std::vector<std::string> full = prefix;
        full.insert(full.end(), step.begin(), step.end());
        std::cout << "[csy] running: " << join_command(full) << "\n";
        const int rc = run_command(full);
        if (rc != 0) {
            std::cout << "note: '" << join_command(full) << "' ended with status " << rc << "; csyc works without a C compiler, so the install goes on\n";
            return;
        }
    }
    std::string again;
    std::cout << (find_c_compiler(again).empty() ? "note: the package was installed but no cc, gcc or clang is on PATH yet\n"
                                                  : "[csy] C compiler installed\n");
}

}
}

