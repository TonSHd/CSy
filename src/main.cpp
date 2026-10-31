#include "lexer.hpp"
#include "ast.hpp"
#include "ast_printer.hpp"
#include "parser.hpp"
#include "diagnostic.hpp"
#include "semantic.hpp"
#include "codegen.hpp"
#include "ir.hpp"
#include "vm.hpp"
#include "ai_ops.hpp"
#include "native_jit.hpp"
#include "project.hpp"
#include "object_file.hpp"
#include "linker.hpp"
#include "explain.hpp"
#include "resource.hpp"
#include "ccm.hpp"
#include "osinfo.hpp"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cerrno>
#include <algorithm>


using namespace csy;

using ProfileClock = std::chrono::steady_clock;

static double profile_ms(ProfileClock::time_point started,
                         ProfileClock::time_point finished) {
    return std::chrono::duration<double, std::milli>(finished - started).count();
}

#ifndef CSYC_VERSION
#define CSYC_VERSION "1.1.0"
#endif

extern "C" const char csyc_version_marker[] __attribute__((used)) = "CSYC-VERSION-MARKER:" CSYC_VERSION;

static void print_usage() {
    std::cout <<
        "csyc - the CSy compiler (v1, bytecode VM)\n"
        "\n"
        "Usage:\n"
        "  csyc build [project.csp|file.csy]  Build without running it.\n"
        "  csyc build <file.csy> -o <out.cso>  Build one source file.\n"
        "  csyc build --verbose            Explain each build stage and show progress.\n"
        "  csyc build --memory-limit <MB>  Set the aggregate compiler memory budget (default 2048).\n"
        "  csyc build --threads <N|auto>   Cap compiler worker threads (default: auto = CPUs, at most 8,\n"
        "                                  reduced further if the memory budget cannot hold that many).\n"
        "  csyc init [path]                Create a default project (project.csp, src/main.csy) in path (default: here).\n"
        "  csyc install [--force] [--prefix DIR] [--yes] [--no-deps] [--dry-run]\n"
        "                                 Detect the OS, add the optional C compiler with its package manager, install as DIR/bin/csyc\n"
        "                                 (default /usr). --force replaces an existing csyc, --yes skips the question, --dry-run only shows.\n"
        "  csyc doctor [--prefix DIR]     Show the detected OS and what is missing before installing.\n"
        "  csyc explain [NAME] [--path F] Print the #explain text of a function or variable (no NAME: list them all).\n"
        "  csyc run <file.cso>             Build stale matching source when available, then run.\n"
        "  csyc <file.csy> [files...]      Compile and run a CSy project (legacy shorthand).\n"
        "  csyc <project.csp>              Build and run a configured project (legacy shorthand).\n"
        "  csyc <file.cso>                 Run a compiled CSy object (legacy shorthand).\n"
        "  csyc <project> --verbose        Explain each build stage and show progress.\n"
        "  csyc <file.csy> --deep         Deprecated: dump tokens, AST, and bytecode.\n"
        "  csyc <project> -o <out.cso>    Write a compiled object, then run it.\n"
        "  csyc <file.csy> -- <args...>   Pass arguments to bc::args().\n"
        "  csyc .                         Compile ./main.csy in the current directory.\n"
        "  csyc <file> --run-threads N    Threads for the built-in AI kernels at run time (default: auto).\n"
        "  csyc build <file> --exe -o <app>  Write a standalone executable (runtime + program; no csyc needed to run it).\n"
        "  csyc build <file> --no-native  Skip the native fast path (much faster builds; the object runs interpreted).\n"
        "  csyc <file> --explain-native   Report which functions run natively and why the rest do not.\n"
        "  csyc --help                    Show this message.\n"
        "  csyc --version                 Show the version.\n"
        "\n";
}

using res::kEstimatedBytesPerSourceByte;
constexpr uint64_t kEstimateBaseBytes = 48ull * 1024 * 1024;

class BuildGovernor {
public:
    explicit BuildGovernor(uint64_t limit_mb)
        : limit_bytes_(limit_mb > UINT64_MAX / (1024 * 1024) ? UINT64_MAX : limit_mb * 1024 * 1024) {
        res::set_limit_bytes(limit_bytes_);
    }
    BuildGovernor(const BuildGovernor&) = delete;
    BuildGovernor& operator=(const BuildGovernor&) = delete;

    void release_build_slot() { ledger_.release(); }

    bool acquire_build_slot(uint64_t estimated_bytes = kEstimateBaseBytes) {
        if (ledger_.held() || limit_bytes_ == 0) return true;
        reserved_ = std::min<uint64_t>(estimated_bytes, limit_bytes_);
        const auto result = ledger_.acquire(reserved_, limit_bytes_, 900.0,
            [](const std::string& msg) { std::cerr << "csyc: " << msg << "\n"; });
        switch (result) {
            case res::BuildLedger::Result::Ok:
                if (ledger_.peers() > 0 && std::getenv("CSY_PROFILE"))
                    std::cerr << "[csy-profile] sharing the memory budget with " << ledger_.peers()
                              << " other compiler process(es)\n";
                return true;
            case res::BuildLedger::Result::TooLarge:
                std::cerr << "error: this project needs more memory than the budget allows; raise --memory-limit\n";
                return false;
            case res::BuildLedger::Result::Timeout:
                std::cerr << "error: gave up waiting for the shared compiler memory budget (other csyc processes are using it)\n";
                return false;
            case res::BuildLedger::Result::Unavailable:
                if (std::getenv("CSY_PROFILE")) std::cerr << "[csy-profile] build ledger unavailable; continuing without cross-process accounting\n";
                return true;
        }
        return true;
    }

    bool check(const char* stage) {
        if (limit_bytes_ == 0) return true;
        if (!res::within_budget(stage)) {
            const uint64_t used = res::tracked_bytes();
            std::cerr << "error: compiler memory budget is at least 95% used during " << stage
                      << " (" << (used / (1024 * 1024)) << " MiB of " << (limit_bytes_ / (1024 * 1024))
                      << " MiB); stopping cleanly - raise --memory-limit or split the project\n";
            exceeded_ = true;
            return false;
        }
        if (ledger_.held()) {
            const uint64_t used = res::tracked_bytes();
            const uint64_t want = std::min<uint64_t>(limit_bytes_, used + used / 8 + (8ull << 20));
            if (want > reserved_) {
                reserved_ = want;
                if (!ledger_.grow(reserved_, limit_bytes_)) {
                    std::cerr << "error: the shared compiler memory budget (" << (limit_bytes_ / (1024 * 1024))
                              << " MiB) is exhausted by " << ledger_.peers()
                              << " other csyc process(es) while " << stage << "; stopping cleanly - retry later or raise --memory-limit\n";
                    exceeded_ = true;
                    return false;
                }
            }
        }
        return true;
    }
    bool exceeded() const { return exceeded_; }

private:
    uint64_t limit_bytes_;
    uint64_t reserved_ = 0;
    res::BuildLedger ledger_;
    bool exceeded_ = false;
};

static uint64_t estimate_build_bytes(const std::vector<std::string>& inputs) {
    uint64_t total = 0;
    auto add = [&](const std::string& path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) total += size;
    };
    ProjectInputs resolved;
    std::string ignored;
    if (resolve_project_inputs(inputs, resolved, ignored)) {
        add(resolved.entry);
        for (const auto& source : resolved.sources) add(source);
    } else {
        for (const auto& path : inputs) add(path);
    }
    return kEstimateBaseBytes + total * kEstimatedBytesPerSourceByte;
}

struct BuildProgress {
    bool verbose;
    bool tty;
    static constexpr int total = 6;
    explicit BuildProgress(bool v) : verbose(v), tty(::isatty(STDERR_FILENO) != 0) {}
    void pause() const { if (tty) std::cerr << "\n"; }
    void stage(int step, const std::string& message) const {
        if (tty) {
            const int width = 24, filled = width * step / total;
            std::cerr << "\r[" << std::string((size_t)filled, '=')
                      << std::string((size_t)(width - filled), ' ') << "] "
                      << (100 * step / total) << "%  " << message << std::flush;
            if (step == total) std::cerr << "\n";
        } else if (verbose) {
            std::cerr << "[build " << step << "/" << total << "] " << message << "\n";
        }
    }
};


namespace {
constexpr char kExeMagic[8] = {'C', 'S', 'Y', 'E', 'X', 'E', 1, '\n'};

bool find_embedded_object(std::string& image, std::uint64_t* runtime_size = nullptr) {
    std::ifstream self("/proc/self/exe", std::ios::binary);
    if (!self) return false;
    self.seekg(0, std::ios::end);
    const std::uint64_t total = static_cast<std::uint64_t>(self.tellg());
    if (total < 16) return false;
    char trailer[16];
    self.seekg(static_cast<std::streamoff>(total - 16));
    self.read(trailer, 16);
    if (!self || std::memcmp(trailer + 8, kExeMagic, 8) != 0) return false;
    std::uint64_t size = 0;
    std::memcpy(&size, trailer, 8);
    if (size == 0 || size > total - 16 || size > 512ull * 1024 * 1024) return false;
    image.resize(static_cast<std::size_t>(size));
    self.seekg(static_cast<std::streamoff>(total - 16 - size));
    self.read(image.data(), static_cast<std::streamsize>(size));
    if (!self) return false;
    if (runtime_size) *runtime_size = total - 16 - size;
    return true;
}

bool write_standalone_exe(const std::string& exe_path, const std::string& object_image, std::string& error) {
    std::string probe;
    std::uint64_t runtime_size = 0;
    std::ifstream self("/proc/self/exe", std::ios::binary);
    if (!self) { error = "cannot read the csyc runtime (/proc/self/exe)"; return false; }
    self.seekg(0, std::ios::end);
    std::uint64_t total = static_cast<std::uint64_t>(self.tellg());
    runtime_size = total;
    if (find_embedded_object(probe, &runtime_size)) {   }
    std::string runtime(static_cast<std::size_t>(runtime_size), '\0');
    self.seekg(0);
    self.read(runtime.data(), static_cast<std::streamsize>(runtime_size));
    if (!self) { error = "cannot read the csyc runtime"; return false; }
    const std::string tmp = exe_path + ".tmp" + std::to_string(::getpid());
    res::register_temp_path(tmp);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const std::uint64_t size = object_image.size();
        out.write(runtime.data(), static_cast<std::streamsize>(runtime.size()));
        out.write(object_image.data(), static_cast<std::streamsize>(object_image.size()));
        out.write(reinterpret_cast<const char*>(&size), 8);
        out.write(kExeMagic, 8);
        out.flush();
        if (!out) { error = "cannot write '" + exe_path + "'"; ::unlink(tmp.c_str()); res::unregister_temp_path(tmp); return false; }
    }
    if (::chmod(tmp.c_str(), 0755) != 0 || ::rename(tmp.c_str(), exe_path.c_str()) != 0) {
        error = "cannot finalize '" + exe_path + "'";
        ::unlink(tmp.c_str());
        res::unregister_temp_path(tmp);
        return false;
    }
    res::unregister_temp_path(tmp);
    return true;
}

int run_embedded_program(const std::string& image, int argc, char** argv) {
    IRModule object;
    std::string error;
    if (!read_object_image(image, "embedded program", object, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    attach_embedded_native(object);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);
    VM vm(std::move(args));
    res::set_tracking(false);
    VMResult result = vm.run(object);
    if (!result.ok) { std::cerr << result.diags.format(""); return 1; }
    return result.exit_code;
}
}

static int init_project(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc > 3) { std::cerr << "error: usage is 'csyc init [path]'\n"; return 1; }
    std::error_code ec;
    fs::path target = fs::absolute(argc == 3 ? fs::path(argv[2]) : fs::path("."), ec).lexically_normal();
    if (!target.has_filename() && target.has_parent_path()) target = target.parent_path();
    if (ec) { std::cerr << "error: cannot resolve the project path\n"; return 1; }
    if (fs::exists(target, ec) && !fs::is_directory(target, ec)) {
        std::cerr << "error: '" << target.string() << "' exists and is not a folder\n";
        return 1;
    }
    if (fs::exists(target / "project.csp", ec)) {
        std::cerr << "error: '" << target.string() << "' already is a project (project.csp exists)\n";
        return 1;
    }
    fs::create_directories(target / "src", ec);
    if (ec) { std::cerr << "error: cannot create '" << (target / "src").string() << "': " << ec.message() << "\n"; return 1; }

    std::string name = target.filename().string();
    if (name.empty() || name == "." || name == "..") name = target.parent_path().filename().string();
    bool safe = !name.empty();
    for (unsigned char c : name) if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ' ')) safe = false;
    if (!safe) name = "CSy";

    struct Item { fs::path path; std::string text; };
    const std::vector<Item> items = {
        {target / "project.csp", "entry = \"src/main.csy\"\n"},
        {target / "src" / "main.csy", "#inc basic as bc;\n\nbc::out>\"Hello from " + name + "\";\nrt 0;\n"},
        {target / ".gitignore", "*.cso\n"},
    };
    for (const auto& item : items) {
        if (fs::exists(item.path, ec)) {
            std::cout << "[csy] kept existing " << item.path.string() << "\n";
            continue;
        }
        std::ofstream out(item.path, std::ios::binary);
        out << item.text;
        out.close();
        if (!out) { std::cerr << "error: cannot write '" << item.path.string() << "'\n"; return 1; }
        std::cout << "[csy] created " << item.path.string() << "\n";
    }
    {   
        std::string detail;
        switch (ccm::copy_default_table(target.string(), detail)) {
            case ccm::TableCopy::Created:
                std::cout << "[csy] created " << detail << " (the Cooler Compiler's table; the Cooler Compiler stays off until 'csyc enable ccm')\n";
                break;
            case ccm::TableCopy::Kept: std::cout << "[csy] kept existing " << detail << "\n"; break;
            case ccm::TableCopy::NotFound:
                std::cerr << "note: no ccm_table.bin was found next to this csyc, so this project has none; the Cooler Compiler needs one in the project folder\n";
                break;
            case ccm::TableCopy::Failed: std::cerr << "note: could not write " << detail << "; the project works without it\n"; break;
        }
    }
    std::cout << "[csy] project ready in " << target.string() << "; build it with 'csyc build', run it with 'csyc run src/main.cso'\n";
    return 0;
}

static std::string installed_version_of(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::string();
    static const char key[] = "CSYC-VERSION-MARKER:";
    const size_t key_len = sizeof(key) - 1;
    std::string window;
    std::vector<char> chunk(1 << 20);
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        window.append(chunk.data(), static_cast<size_t>(got));
        for (size_t at = window.find(key); at != std::string::npos; at = window.find(key, at + 1)) {
            std::string v;
            for (size_t i = at + key_len; i < window.size() && v.size() < 32; ++i) {
                const unsigned char c = static_cast<unsigned char>(window[i]);
                if (std::isalnum(c) || c == '.' || c == '-' || c == '+' || c == '~') v.push_back(static_cast<char>(c)); else break;
            }
            if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) return v;
        }
        if (window.size() > key_len + 40) window.erase(0, window.size() - (key_len + 40));
    }
    return std::string();
}

static bool same_bytes(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), static_cast<size_t>(fa.gcount())) != 0) return false;
    }
    return true;
}

static bool folder_writable(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path p = dir;
    while (!p.empty() && !std::filesystem::exists(p, ec)) {
        const std::filesystem::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return ::access(p.c_str(), W_OK | X_OK) == 0;
}

static void print_detected_os(const osinfo::Os& os) {
    std::cout << "[csy] detected " << os.name;
    if (os.kind == "linux") std::cout << " (" << (os.family.empty() ? "distribution family not recognised" : os.family + " family") << ", " << os.arch << ")";
    std::cout << "\n";
}

static int refuse_unsupported_os(const osinfo::Os& os, const char* what) {
    std::cerr << "error: " << what << " supports Linux only for now (this is " << os.name
              << "); Windows and macOS support will come later. You can still build with 'make csyc' and copy the binary yourself.\n";
    return 1;
}

static int install_table(const std::filesystem::path& root, bool force, bool dry_run) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string src = csy::ccm::default_table_path();
    const fs::path dest = root / "share" / "csyc" / "ccm_table.bin";
    if (src.empty()) {
        std::cout << "note: no ccm_table.bin was found next to this csyc, so none was installed (projects made by 'csyc init' will not get one)\n";
        return 0;
    }
    if (fs::exists(dest, ec)) {
        if (fs::equivalent(src, dest, ec)) return 0;
        if (!force) {
            if (!same_bytes(src, dest)) std::cout << "[csy] kept existing " << dest.string() << " (use 'csyc install --force' to replace it)\n";
            return 0;
        }
    }
    const bool need_admin = !folder_writable(dest.parent_path());
    if (need_admin || dry_run) {
        bool possible = true;
        std::vector<std::string> cmd = osinfo::privilege_prefix(possible);
        for (const char* part : {"install", "-D", "-m", "0644"}) cmd.push_back(part);
        cmd.push_back(src);
        cmd.push_back(dest.string());
        if (dry_run) {
            std::cout << "[csy] would also install the Cooler Compiler's table as " << dest.string();
            if (need_admin) std::cout << " by running: " << osinfo::join_command(cmd);
            std::cout << "\n";
            return 0;
        }
        if (!possible) {
            std::cerr << "error: writing '" << dest.parent_path().string() << "' needs administrator rights and neither sudo nor doas was found; as root run: "
                      << osinfo::join_command(cmd) << "\n";
            return 1;
        }
        std::cout << "[csy] writing '" << dest.parent_path().string() << "' needs administrator rights; running: " << osinfo::join_command(cmd) << "\n";
        if (osinfo::run_command(cmd) != 0) { std::cerr << "error: the table install command failed\n"; return 1; }
    } else {
        fs::create_directories(dest.parent_path(), ec);
        if (ec) { std::cerr << "error: cannot create '" << dest.parent_path().string() << "': " << ec.message() << "\n"; return 1; }
        const fs::path temp = dest.parent_path() / (".ccm_table.new." + std::to_string(static_cast<long>(::getpid())));
        fs::copy_file(src, temp, fs::copy_options::overwrite_existing, ec);
        if (ec) { std::cerr << "error: cannot write '" << temp.string() << "': " << ec.message() << "\n"; return 1; }
        fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                        fs::perm_options::replace, ec);
        fs::rename(temp, dest, ec);
        if (ec) {
            std::error_code ignore;
            fs::remove(temp, ignore);
            std::cerr << "error: cannot put the table in place as '" << dest.string() << "': " << ec.message() << "\n";
            return 1;
        }
    }
    std::cout << "[csy] installed the Cooler Compiler's table as " << dest.string() << "\n";
    return 0;
}

static std::vector<std::filesystem::path> csyc_copies_on_path() {
    namespace fs = std::filesystem;
    std::vector<fs::path> found;
    const char* path_env = std::getenv("PATH");
    if (!path_env) return found;
    std::error_code ec;
    std::stringstream parts(path_env);
    for (std::string part; std::getline(parts, part, ':');) {
        if (part.empty()) continue;
        const fs::path candidate = fs::path(part) / "csyc";
        if (!fs::is_regular_file(candidate, ec) || ::access(candidate.c_str(), X_OK) != 0) continue;
        bool seen = false;
        for (const auto& f : found)
            if (fs::equivalent(f, candidate, ec)) { seen = true; break; }
        if (!seen) found.push_back(candidate);
    }
    return found;
}

static std::string removal_command(const std::filesystem::path& file) {
    bool possible = true;
    std::vector<std::string> cmd;
    if (!osinfo::is_root() && !folder_writable(file.parent_path())) cmd = osinfo::privilege_prefix(possible);
    cmd.push_back("rm");
    cmd.push_back(file.string());
    return osinfo::join_command(cmd);
}

static void warn_about_shadowing(const std::filesystem::path& target, bool installed) {
    namespace fs = std::filesystem;
    const std::vector<fs::path> copies = csyc_copies_on_path();
    if (copies.empty()) return;
    std::error_code ec;
    const bool target_exists = fs::exists(target, ec);
    for (size_t i = 0; i < copies.size(); ++i) {
        if (target_exists && fs::equivalent(copies[i], target, ec)) continue;
        std::string have = installed_version_of(copies[i]);
        if (have.empty()) have = "unknown version";
        const bool runs_first = (i == 0);
        if (runs_first) {
            if (installed)
                std::cout << "warning: typing 'csyc' still starts " << copies[i].string() << " (" << have
                          << "), not the copy just installed as " << target.string() << "\n";
            else
                std::cout << "warning: " << copies[i].string() << " (" << have
                          << ") is on your PATH and starts instead of " << target.string() << "\n";
        } else {
            std::cout << "note: another csyc is on your PATH after the first one: " << copies[i].string()
                      << " (" << have << "); it is not used\n";
        }
        std::cout << "      remove it with: " << removal_command(copies[i]) << "\n";
        if (runs_first) std::cout << "      then run 'hash -r' or open a new terminal\n";
    }
}

static int install_self(int argc, char** argv) {
    namespace fs = std::filesystem;
    bool force = false, assume_yes = false, no_deps = false, dry_run = false;
    std::string prefix = "/usr";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--force") force = true;
        else if (a == "--yes") assume_yes = true;
        else if (a == "--no-deps") no_deps = true;
        else if (a == "--dry-run") dry_run = true;
        else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
        else if (a.rfind("--prefix=", 0) == 0) prefix = a.substr(9);
        else { std::cerr << "error: usage is 'csyc install [--force] [--prefix DIR] [--yes] [--no-deps] [--dry-run]'\n"; return 1; }
    }
    if (prefix.empty()) { std::cerr << "error: --prefix needs a folder\n"; return 1; }
    const osinfo::Os os = osinfo::detect_os();
    if (!osinfo::supported(os)) return refuse_unsupported_os(os, "csyc install");
    print_detected_os(os);

    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) { std::cerr << "error: cannot find the running csyc (/proc/self/exe)\n"; return 1; }
    const fs::path dir = fs::absolute(fs::path(prefix), ec).lexically_normal() / "bin";
    const fs::path target = dir / "csyc";
    const fs::path root = dir.parent_path();

    if (fs::exists(target, ec)) {
        const bool this_is_it = fs::equivalent(self, target, ec);
        std::string have = installed_version_of(target);
        if (have.empty()) have = "unknown version";
        if (this_is_it) {
            std::cout << "[csy] csyc " << CSYC_VERSION << " is already installed as " << target.string() << " (this is that copy)\n";
            warn_about_shadowing(target, true);
            return 0;
        }
        if (!force) {
            if (same_bytes(self, target)) {
                std::cout << "[csy] csyc " << CSYC_VERSION << " is already installed in " << target.string()
                          << " and identical to this one; use 'csyc install --force' to reinstall anyway\n";
                const int same_rc = install_table(root, false, dry_run);
                warn_about_shadowing(target, true);
                return same_rc;
            }
            std::cerr << "error: csyc (" << have << ") is already installed as " << target.string()
                      << "; use 'csyc install --force' to replace it with this one (" << CSYC_VERSION << ")\n";
            return 1;
        }
        if (target == fs::path("/usr/bin/csyc") && fs::exists("/var/lib/dpkg/info/csyc.list", ec))
            std::cout << "note: " << target.string() << " belongs to the csyc package; dpkg will not know it was replaced\n";
    }

    if (!no_deps) osinfo::ensure_c_compiler(os, assume_yes, dry_run);

    const bool need_admin = !folder_writable(dir);
    if (need_admin || dry_run) {
        bool possible = true;
        std::vector<std::string> cmd = osinfo::privilege_prefix(possible);
        for (const char* part : {"install", "-D", "-m", "0755"}) cmd.push_back(part);
        cmd.push_back(self.string());
        cmd.push_back(target.string());
        if (dry_run) {
            std::cout << "[csy] would install csyc " << CSYC_VERSION << " as " << target.string();
            if (need_admin) std::cout << " by running: " << osinfo::join_command(cmd);
            std::cout << "\n";
            const int dry_rc = install_table(root, force, true);
            warn_about_shadowing(target, false);
            return dry_rc;
        }
        if (!possible) {
            std::cerr << "error: writing '" << dir.string() << "' needs administrator rights and neither sudo nor doas was found; as root run: "
                      << osinfo::join_command(cmd) << "\n";
            return 1;
        }
        std::cout << "[csy] writing '" << dir.string() << "' needs administrator rights; running: " << osinfo::join_command(cmd)
                  << " (you may be asked for your password)\n";
        const int rc = osinfo::run_command(cmd);
        if (rc != 0) { std::cerr << "error: the install command ended with status " << rc << "\n"; return 1; }
    } else {
        fs::create_directories(dir, ec);
        if (ec) { std::cerr << "error: cannot create '" << dir.string() << "': " << ec.message() << "\n"; return 1; }
        const fs::path temp = dir / (".csyc.new." + std::to_string(static_cast<long>(::getpid())));
        fs::copy_file(self, temp, fs::copy_options::overwrite_existing, ec);
        if (ec) { std::cerr << "error: cannot write '" << temp.string() << "': " << ec.message() << "\n"; return 1; }
        fs::permissions(temp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                                  fs::perms::others_exec, fs::perm_options::replace, ec);
        fs::rename(temp, target, ec);
        if (ec) {
            std::error_code ignore;
            fs::remove(temp, ignore);
            std::cerr << "error: cannot put csyc in place as '" << target.string() << "': " << ec.message() << "\n";
            return 1;
        }
    }
    std::cout << "[csy] installed csyc " << CSYC_VERSION << " as " << target.string() << "\n";
    const int table_rc = install_table(root, force, false);
    bool on_path = false;
    if (const char* path_env = std::getenv("PATH")) {
        std::stringstream parts(path_env);
        for (std::string part; std::getline(parts, part, ':');)
            if (!part.empty() && fs::path(part).lexically_normal() == dir) on_path = true;
    }
    if (!on_path) std::cout << "note: " << dir.string() << " is not in your PATH\n";
    warn_about_shadowing(target, true);
    return table_rc;
}

static int doctor_command(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::string prefix = "/usr";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
        else if (a.rfind("--prefix=", 0) == 0) prefix = a.substr(9);
        else { std::cerr << "error: usage is 'csyc doctor [--prefix DIR]'\n"; return 1; }
    }
    const osinfo::Os os = osinfo::detect_os();
    std::cout << "csyc " << CSYC_VERSION << " doctor\n";
    std::cout << "  OS:               " << os.name << "\n";
    if (os.kind == "linux")
        std::cout << "  Distribution:     " << (os.id.empty() ? "unknown" : os.id) << ", "
                  << (os.family.empty() ? "family not recognised" : os.family + " family") << "\n";
    std::cout << "  Architecture:     " << (os.arch.empty() ? "unknown" : os.arch) << "\n";
#ifdef __GLIBC__
    std::cout << "  C library:        glibc " << gnu_get_libc_version() << " (this build needs 2.38 or newer, and it is running)\n";
#else
    std::cout << "  C library:        not glibc\n";
#endif
    if (!osinfo::supported(os)) {
        std::cout << "  Supported:        no, csyc install supports Linux only for now; Windows and macOS will come later\n";
        return 1;
    }
    const osinfo::PackagePlan plan = osinfo::c_compiler_plan(os);
    std::cout << "  Package manager:  " << (plan.known ? plan.manager : "none (" + plan.why_not + ")") << "\n";
    bool possible = true;
    const std::vector<std::string> admin = osinfo::privilege_prefix(possible);
    std::cout << "  Admin rights:     " << (osinfo::is_root() ? "running as root" : (possible ? admin[0] + " is available (it asks for your password)" : "none (no sudo or doas)")) << "\n";
    std::string cc_name;
    const std::string cc = osinfo::find_c_compiler(cc_name);
    std::cout << "  C compiler:       ";
    if (!cc.empty()) std::cout << "found " << cc << "\n";
    else {
        std::cout << "missing (optional, enables the second native backend)";
        if (plan.known) {
            std::string shown;
            for (const auto& step : plan.steps) {
                std::vector<std::string> full = admin;
                full.insert(full.end(), step.begin(), step.end());
                shown += (shown.empty() ? "" : " && ") + osinfo::join_command(full);
            }
            std::cout << "\n                    csyc install can add it with: " << shown;
        }
        std::cout << "\n";
    }
    std::error_code ec;
    const fs::path dir = fs::absolute(fs::path(prefix), ec).lexically_normal() / "bin";
    const fs::path target = dir / "csyc";
    {
        const std::string table = ccm::default_table_path();
        std::cout << "  Cooler table:     " << (table.empty() ? std::string("not found (csyc init will not add one to new projects)") : table) << "\n";
    }
    std::cout << "  Install target:   " << target.string() << ", ";
    bool this_copy = false;
    if (!fs::exists(target, ec)) std::cout << "not installed";
    else {
        const std::string have = installed_version_of(target);
        std::cout << "installed (" << (have.empty() ? "unknown version" : have) << ")";
        this_copy = fs::equivalent("/proc/self/exe", target, ec);
        if (this_copy) std::cout << ", this is that copy";
    }
    std::cout << "\n  Folder writable:  " << (folder_writable(dir) ? "yes" : (possible ? "no, csyc install will use " + (admin.empty() ? std::string("administrator rights") : admin[0]) : "no, and no sudo or doas to get rights")) << "\n";
    {
        const std::vector<fs::path> copies = csyc_copies_on_path();
        if (copies.empty()) std::cout << "  csyc on PATH:     none\n";
        for (size_t i = 0; i < copies.size(); ++i) {
            std::string have = installed_version_of(copies[i]);
            if (have.empty()) have = "unknown version";
            std::cout << (i == 0 ? "  csyc on PATH:     " : "                    ") << copies[i].string() << " (" << have << ")"
                      << (i == 0 ? ", the one that runs" : ", never reached") << "\n";
        }
    }
    warn_about_shadowing(target, fs::exists(target, ec));
    if (this_copy) { std::cout << "  Result:           this csyc is the installed one\n"; return 0; }
    std::cout << "  Result:           " << (os.family.empty() ? "csyc can be installed; the distribution is not recognised, so the C compiler is left to you"
                                                               : "ready to install") << "\n";
    return 0;
}

static int csyc_main(int argc, char** argv) {
    {
        std::string embedded;
        if (find_embedded_object(embedded)) return run_embedded_program(embedded, argc, argv);
    }
    if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-V")) {
        std::cout << "csyc " CSYC_VERSION "\n";
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "init") return init_project(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "install") return install_self(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "doctor") return doctor_command(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "explain") return explain_command(argc, argv);
    {
        const int command_code = ccm::handle_command(argc, argv);
        if (command_code >= 0) return command_code;
    }
    ccm::begin(argc, argv);
    const auto main_started = ProfileClock::now();
    const bool profile = std::getenv("CSY_PROFILE") != nullptr;
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::vector<std::string> input_paths;
    std::string output_path;
    std::vector<std::string> program_args;
    bool deep = false;
    bool verbose = false;
    bool build_only = false;
    bool run_command = false;
    bool explain_native = false;
    bool no_native = false;
    bool make_exe = false;
    uint64_t memory_limit_mb = 2048;
    if (const char* env_limit = std::getenv("CSYC_MEMORY_LIMIT_MB")) {
        char* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(env_limit, &end, 10);
        if (env_limit[0] != '-' && errno == 0 && end != env_limit && *end == '\0') memory_limit_mb = parsed;
    }
    unsigned int thread_limit = 0;
    if (const char* env_threads = std::getenv("CSYC_THREADS")) {
        char* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(env_threads, &end, 10);
        if (env_threads[0] != '-' && errno == 0 && end != env_threads && *end == '\0' && parsed >= 1 && parsed <= 256)
            thread_limit = static_cast<unsigned int>(parsed);
    }
    int first_option = 1;
    if (argc > 1 && std::string(argv[1]) == "build") {
        build_only = true;
        first_option = 2;
    } else if (argc > 1 && std::string(argv[1]) == "run") {
        run_command = true;
        first_option = 2;
    }

    for (int i = first_option; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--") {
            for (++i; i < argc; i++) program_args.emplace_back(argv[i]);
            break;
        }
        if (a == "--help" || a == "-h") {
            print_usage();
            return 0;
        }
        if (a == "--deep") {
            deep = true;
            verbose = true;
            std::cerr << "warning: '--deep' is deprecated; use '--verbose' for the build log\n";
            continue;
        }
        if (a == "--verbose" || a == "-v") {
            verbose = true;
            continue;
        }
        if (a == "--run-threads") {
            if (i + 1 >= argc) { std::cerr << "error: --run-threads requires 'auto' or a number from 1 to 256\n"; return 1; }
            if (std::string(argv[i + 1]) == "auto") { ++i; set_ai_run_threads(0); continue; }
            char* end = nullptr;
            errno = 0;
            const unsigned long parsed = std::strtoul(argv[++i], &end, 10);
            if (argv[i][0] == '-' || errno != 0 || end == argv[i] || *end != '\0' || parsed == 0 || parsed > 256) {
                std::cerr << "error: --run-threads must be 'auto' or between 1 and 256\n";
                return 1;
            }
            set_ai_run_threads(static_cast<unsigned>(parsed));
            continue;
        }
        if (a == "--explain-native") {
            explain_native = true;
            continue;
        }
        if (a == "--exe") {
            make_exe = true;
            continue;
        }
        if (a == "--no-native") {
            no_native = true;
            continue;
        }
        if (a == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o requires an output filename\n";
                return 1;
            }
            output_path = argv[++i];
            continue;
        }
        if (a == "--memory-limit") {
            if (i + 1 >= argc) { std::cerr << "error: --memory-limit requires a number of MiB (0 disables the limit)\n"; return 1; }
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(argv[++i], &end, 10);
            if (argv[i][0] == '-' || errno != 0 || end == argv[i] || *end != '\0') { std::cerr << "error: --memory-limit expects a non-negative integer number of MiB\n"; return 1; }
            memory_limit_mb = parsed;
            continue;
        }
        if (a == "--threads" || a == "-j") {
            if (i + 1 >= argc) { std::cerr << "error: --threads requires a positive integer or 'auto'\n"; return 1; }
            if (std::string(argv[i + 1]) == "auto") { ++i; thread_limit = 0; continue; }
            char* end = nullptr;
            errno = 0;
            const unsigned long parsed = std::strtoul(argv[++i], &end, 10);
            if (argv[i][0] == '-' || errno != 0 || end == argv[i] || *end != '\0' || parsed == 0 || parsed > 256) {
                std::cerr << "error: --threads must be 'auto' or between 1 and 256\n";
                return 1;
            }
            thread_limit = static_cast<unsigned int>(parsed);
            continue;
        }
        if (a[0] == '-' && a.size() > 1) {
            std::cerr << "error: unknown flag '" << a << "'\n";
            return 1;
        }
        input_paths.push_back(a);
    }

    if (build_only && input_paths.empty()) input_paths.emplace_back("project.csp");
    if (!input_paths.empty()) ccm::set_subject(input_paths.front(), !build_only, false);

    if (!build_only && !run_command && input_paths.size() == 1 && input_paths[0] != "." &&
        std::filesystem::path(input_paths[0]).extension().empty()) {
        std::error_code exists_error;
        if (!std::filesystem::exists(input_paths[0], exists_error)) {
            std::cerr << "error: unknown command '" << input_paths[0]
                      << "' (a source file name needs an extension such as .csy)\n";
            return 1;
        }
    }

    const unsigned int worker_threads = res::resolve_threads(
        thread_limit != 0 ? thread_limit : std::min(res::hardware_threads(), 8u),
        64ull * 1024 * 1024);


    res::install_cleanup_handlers();
    BuildGovernor resource_guard(memory_limit_mb);
    res::set_tracking(true);
    if (run_command) {
        if (input_paths.size() != 1 || std::filesystem::path(input_paths.front()).extension() != ".cso") {
            std::cerr << "error: usage is 'csyc run <file.cso>'\n";
            return 1;
        }
        const std::filesystem::path artifact = input_paths.front();
        if (!resource_guard.acquire_build_slot()) return 1;
        if (!resource_guard.check("checking whether the object needs rebuilding")) return 1;
        std::string rebuild_input;
        std::error_code fs_error;
        const bool artifact_exists = std::filesystem::is_regular_file(artifact, fs_error) && !fs_error;
        bool rebuild = !artifact_exists;
        auto newer_than_artifact = [&](const std::filesystem::path& source) {
            std::error_code source_error, artifact_error;
            const auto source_time = std::filesystem::last_write_time(source, source_error);
            const auto artifact_time = std::filesystem::last_write_time(artifact, artifact_error);
            return !source_error && !artifact_error && source_time > artifact_time;
        };

        std::filesystem::path matching_source = artifact;
        matching_source.replace_extension(".csy");
        if (std::filesystem::is_regular_file(matching_source) &&
            (!artifact_exists || newer_than_artifact(matching_source))) {
            rebuild = true;
            rebuild_input = matching_source.string();
        }

        std::vector<std::filesystem::path> project_candidates;
        project_candidates.emplace_back(std::filesystem::current_path() / "project.csp");
        for (auto dir = std::filesystem::absolute(artifact).parent_path(); !dir.empty();) {
            project_candidates.push_back(dir / "project.csp");
            const auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        for (const auto& project_file : project_candidates) {
            if (!std::filesystem::is_regular_file(project_file)) continue;
            ProjectInputs candidate;
            std::string candidate_error;
            if (resolve_project_inputs({project_file.string()}, candidate, candidate_error)) {
                std::filesystem::path configured = candidate.output;
                if (configured.empty()) {
                    configured = candidate.entry;
                    configured.replace_extension(".cso");
                }
                const auto configured_abs = std::filesystem::weakly_canonical(configured);
                const auto artifact_abs = std::filesystem::weakly_canonical(artifact);
                if (configured_abs == artifact_abs) {
                    std::vector<SourceUnit> candidate_units;
                    if (load_program_sources(candidate, candidate_units, candidate_error,
                                             [&resource_guard]() { return resource_guard.check("checking project freshness"); })) {
                        if (!artifact_exists || newer_than_artifact(project_file)) {
                            rebuild = true;
                            rebuild_input = project_file.string();
                        }
                        for (const auto& unit : candidate_units) {
                            if (!artifact_exists || newer_than_artifact(unit.path)) {
                                rebuild = true;
                                rebuild_input = project_file.string();
                                break;
                            }
                        }
                    }
                }
            }
            if (rebuild_input == project_file.string()) break;
        }

        if (rebuild) {
            if (!rebuild_input.empty()) ccm::note_rebuilt();
            if (rebuild_input.empty()) {
                std::cerr << "error: cannot build '" << artifact.string()
                          << "': no matching .csy source or project.csp was found\n";
                return 1;
            }
            input_paths.assign(1, rebuild_input);
            output_path = artifact.string();
        } else {
            resource_guard.release_build_slot();
        }
        if (resource_guard.exceeded()) return 1;
    }

    if (input_paths.empty()) {
        std::cerr << "error: no input file\n";
        print_usage();
        return 1;
    }

    const bool object_input = input_paths.size() == 1 &&
        std::filesystem::path(input_paths[0]).extension() == ".cso";
    if (!object_input && !resource_guard.acquire_build_slot(estimate_build_bytes(input_paths))) return 1;
    if (!resource_guard.check(object_input ? "loading a compiled object" : "starting compilation")) return 1;

    if (input_paths.size() == 1 && std::filesystem::path(input_paths[0]).extension() == ".cso") {
        if (!output_path.empty()) { std::cerr << "error: -o is only used while compiling source files\n"; return 1; }
        IRModule object;
        std::string object_error;
        if (!read_object_file(input_paths[0], object, object_error)) {
            std::cerr << "error: " << object_error << "\n";
            return 1;
        }
        if (deep) std::cout << disassemble(object);
        if (!object.native_c_source.empty() || !object.native_library.empty() || !object.native_x64.empty()) {
            std::string native_note;
            attach_embedded_native(object, &native_note);
            if (explain_native) std::cerr << "[csy] " << native_note << "\n";
        } else if (explain_native) {
            std::cerr << "[csy] this object carries no native code (it was built without the native fast path)\n";
        }
        VM vm(std::move(program_args));
        res::set_tracking(false);
        resource_guard.release_build_slot();
        std::cerr << "[csy] running compiled object " << input_paths[0] << "...\n";
        ccm::set_subject(input_paths[0], true, true);
        ccm::user_code(true);
        VMResult result = vm.run(object);
        ccm::user_code(false);
        if (!result.ok) { std::cerr << result.diags.format(""); return 1; }
        return result.exit_code;
    }

    if (input_paths.size() == 1 && input_paths[0] == ".") input_paths[0] = "main.csy";
    ProjectInputs project_inputs;
    std::string project_error;
    if (!resolve_project_inputs(input_paths, project_inputs, project_error)) {
        std::cerr << "error: " << project_error << "\n";
        return 1;
    }
    ccm::set_subject(project_inputs.entry, !build_only, true);
    project_inputs.threads = worker_threads;
    project_inputs.keep_tokens = deep;
    if (build_only && output_path.empty()) {
        if (!project_inputs.output.empty()) {
            output_path = project_inputs.output;
        } else {
            std::filesystem::path default_output = project_inputs.entry;
            default_output.replace_extension(".cso");
            output_path = default_output.string();
        }
    }
    if (output_path.empty()) output_path = project_inputs.output;
    BuildProgress progress(verbose && !deep);
    if (verbose) std::cerr << "CSy is preparing a project build from '" << project_inputs.entry << "' using "
                           << worker_threads << " worker thread(s).\n";
    const bool profile_memory = std::getenv("CSY_PROFILE") != nullptr;
    auto mem_mark = [profile_memory](const char* stage) {
        if (!profile_memory) return;
        std::fprintf(stderr, "[csy-profile] mem stage=%s live_mib=%.1f peak_mib=%.1f\n", stage,
                     res::tracked_bytes() / (1024.0 * 1024.0), res::peak_tracked_bytes() / (1024.0 * 1024.0));
    };
    std::vector<SourceUnit> units;
    auto stage_started = ProfileClock::now();
    const auto source_read_started = stage_started;
    if (!load_program_sources(project_inputs, units, project_error,
                              [&resource_guard]() { return resource_guard.check("source loading"); })) {
        if (resource_guard.exceeded()) project_error = "compiler memory budget reached while processing project sources";
        std::cerr << "error: " << project_error << "\n";
        for (const auto& unit : units) {
            if (!unit.parsed.diags.items().empty()) std::cerr << unit.parsed.diags.format(unit.source);
            if (!unit.lexed.ok && !unit.lexed.tokens.empty()) {
                DiagBag d; const Token& e = unit.lexed.tokens[0]; d.error(unit.path, e.line, e.col, e.text);
                std::cerr << d.format(unit.source);
            }
        }
        return 1;
    }
    progress.stage(1, "Found and parsed " + std::to_string(units.size()) + " source file(s)");
    mem_mark("after_read_lex_parse");
    const auto source_read_finished = ProfileClock::now();
    const std::string input_path = units.back().path;
    const std::string& source = units.back().source;
    const auto lex_started = source_read_started;
    const auto lex_finished = source_read_finished;
    const auto parse_started = source_read_started;
    const auto parse_finished = source_read_finished;
    bool front_end_failed = false;
    for (const auto& unit : units) {
        if (!unit.parsed.diags.items().empty()) std::cerr << unit.parsed.diags.format(unit.source);
        if (!unit.parsed.ok) front_end_failed = true;
        if (deep) {
            std::cout << "[stage] " << unit.path << ": lexed " << unit.lexed.tokens.size() << " tokens\n";
            for (const auto& t : unit.lexed.tokens)
                std::cout << "    " << t.loc() << "  " << token_type_name(t.type) << "  '" << t.text << "'\n";
            std::cout << "[stage] AST: " << unit.path << "\n" << print_ast(unit.parsed.module) << "\n";
        }
    }
    if (front_end_failed) { std::cerr << "  project could not be parsed.\n"; return 1; }
    progress.stage(2, "All source files have valid syntax");
    if (!resource_guard.check("project linking")) return 1;
    Module linked_ast; linked_ast.file = input_path;
    std::unordered_map<std::string, std::string> symbols;
    std::unordered_map<std::string, const SourceUnit*> by_path;
    std::unordered_map<std::string, std::pair<std::string, std::string>> project_inc_aliases;
    for (const auto& unit : units) by_path[unit.path] = &unit;
    for (const auto& unit : units) {
        for (const auto& s : unit.parsed.module.body) {
            if (s->kind == StmtKind::Import) {
                auto prior = project_inc_aliases.emplace(s->import_alias, std::make_pair(s->import_module, s->tok.loc()));
                if (!prior.second && prior.first->second.first != s->import_module) {
                    std::cerr << "error: " << s->tok.loc() << ": '#inc " << s->import_module << " as " << s->import_alias
                              << "' conflicts with '#inc " << prior.first->second.first << " as " << s->import_alias << "' at "
                              << prior.first->second.second << " (an #inc alias must mean the same module everywhere in a project)\n";
                    return 1;
                }
            }
        }
    }
    for (const auto& unit : units) {
        for (const auto& s : unit.parsed.module.body) {
            if (s->kind != StmtKind::Connect || !s->connect_all) continue;
            const std::string dep = std::filesystem::weakly_canonical(
                std::filesystem::path(unit.path).parent_path() / s->connect_path).string();
            auto target = by_path.find(dep);
            if (target == by_path.end()) continue;
            s->connect_exports = top_level_names(target->second->parsed.module, false);
        }
    }
    for (const auto& unit : units) {
        std::unordered_map<std::string, std::pair<std::string, std::string>> file_connect_aliases;
        for (const auto& s : unit.parsed.module.body) {
            if (s->kind == StmtKind::Connect) {
                const std::string target = std::filesystem::weakly_canonical(
                    std::filesystem::path(unit.path).parent_path() / s->connect_path).string();
                auto inc = project_inc_aliases.find(s->connect_alias);
                if (inc != project_inc_aliases.end()) {
                    std::cerr << "error: " << s->tok.loc() << ": the #connect namespace '" << s->connect_alias
                              << "' is already the name of '#inc " << inc->second.first << " as " << s->connect_alias << "' (at "
                              << inc->second.second << "); choose a different name after 'as'\n";
                    return 1;
                }
                auto prior = file_connect_aliases.emplace(s->connect_alias, std::make_pair(target, s->tok.loc()));
                if (!prior.second && prior.first->second.first != target) {
                    std::cerr << "error: " << s->tok.loc() << ": the namespace '" << s->connect_alias << "' already names a different file in this file (at "
                              << prior.first->second.second << ")\n";
                    return 1;
                }
            }
            if (s->kind == StmtKind::FuncDecl) {
                auto inserted = symbols.emplace(s->func_name, unit.path);
                if (!inserted.second && inserted.first->second == unit.path) {
                    std::cerr << "error: function '" << s->func_name << "' is defined more than once in '"
                              << unit.path << "'\n";
                    return 1;
                }
            }
        }
        for (const auto& s : unit.parsed.module.body) if (s->kind == StmtKind::Connect) {
            std::filesystem::path dep = std::filesystem::weakly_canonical(
                std::filesystem::path(unit.path).parent_path() / s->connect_path);
            auto target = by_path.find(dep.string());
            if (target == by_path.end()) {
                std::cerr << "error: connected file was not loaded: '" << dep.string() << "'\n";
                return 1;
            }
            if (s->connect_all) continue;
            const std::vector<std::string> available_names = top_level_names(target->second->parsed.module, true);
            const std::unordered_set<std::string> available(available_names.begin(), available_names.end());
            std::unordered_set<std::string> unique;
            for (const auto& name : s->connect_exports) {
                if (!unique.insert(name).second) {
                    std::cerr << "error: " << s->tok.loc() << ": '#connect' lists '" << name << "' more than once\n";
                    return 1;
                }
                if (!available.count(name)) {
                    std::cerr << "error: " << s->tok.loc() << ": '" << name << "' is not declared at the top level of '"
                              << dep.string() << "' (the file named by #connect \"" << s->connect_path << "\")\n";
                    return 1;
                }
                if (!name.empty() && name[0] == '_') {
                    std::cerr << "error: " << s->tok.loc() << ": '" << name << "' is private in '" << dep.string()
                              << "' (a name that starts with '_' is not shared); rename it without the underscore to connect it\n";
                    return 1;
                }
            }
        }
        linked_ast.body.insert(linked_ast.body.end(), unit.parsed.module.body.begin(), unit.parsed.module.body.end());
    }

    stage_started = ProfileClock::now();
    const auto runtime_init_started = stage_started;
    VM vm(std::move(program_args));
    const auto runtime_init_finished = ProfileClock::now();
    std::vector<std::string> builtin_names;
    builtin_names.reserve(vm.builtins_table().size());
    for (const auto& kv : vm.builtins_table()) builtin_names.push_back(kv.first);

    ccm::phase_check();
    if (deep) std::cout << "[stage] semantic analysis...\n";
    stage_started = ProfileClock::now();
    const auto semantic_started = stage_started;
    std::vector<std::pair<std::string, std::string>> all_using_aliases;
    bool semantic_ok = true;
    std::vector<SemanticResult> semantic_results(units.size());
    res::parallel_for(units.size(), worker_threads, [&](size_t i) {
        semantic_results[i] = analyze(units[i].parsed.module, units[i].source, builtin_names);
        res::check("semantic analysis");
    });
    for (size_t i = 0; i < units.size(); ++i) {
        const SemanticResult& sem_r = semantic_results[i];
        if (!sem_r.diags.items().empty()) std::cerr << sem_r.diags.format(units[i].source);
        if (!sem_r.ok) semantic_ok = false;
        all_using_aliases.insert(all_using_aliases.end(), sem_r.using_aliases.begin(), sem_r.using_aliases.end());
    }
    const auto semantic_finished = ProfileClock::now();
    mem_mark("after_semantic");
    if (!semantic_ok) {
        std::cerr << "  compilation failed during semantic analysis.\n";
        return 1;
    }
    for (const auto& w : explain_warnings(units)) std::cerr << w << "\n";
    std::string link_error;
    std::vector<std::string> link_warnings;
    if (!namespace_project_symbols(units, link_error, &link_warnings)) {
        std::cerr << "error: " << link_error << "\n";
        return 1;
    }
    for (const auto& w : link_warnings) std::cerr << w << "\n";

    progress.stage(3, "Names, connections, and builtins are resolved");
    mem_mark("after_link");
    if (!resource_guard.check("code generation")) return 1;

    if (deep) std::cout << "[stage] code generation...\n";
    stage_started = ProfileClock::now();
    const auto codegen_started = stage_started;
    if (!no_native) outline_top_level_loops(linked_ast);
    CodegenResult cg_r = generate(linked_ast, all_using_aliases, source, builtin_names);
    const auto codegen_finished = ProfileClock::now();
    if (!cg_r.diags.items().empty()) {
        std::cerr << cg_r.diags.format(source);
    }
    if (!cg_r.ok) {
        std::cerr << "  compilation failed during code generation.\n";
        return 1;
    }
    progress.stage(4, "Generated one unified bytecode module");
    mem_mark("after_codegen");
    if (deep) {
        std::cout << "[stage] IR:\n";
        std::cout << disassemble(cg_r.module);
    }

    stage_started = ProfileClock::now();
    const auto jit_started = stage_started;
    progress.pause();
    if (no_native) ::setenv("CSY_NO_NATIVE_JIT", "1", 1);
    const NativeReport native_report = try_native_compile(
        linked_ast, cg_r.module, build_only ? NativeMode::Build : NativeMode::Run, !output_path.empty());
    const auto jit_finished = ProfileClock::now();
    progress.stage(5, build_only ? "Built native fast path for the object" : "Finished native fast-path analysis");
    if (explain_native) std::cerr << format_native_report(native_report);
    else if (build_only && !native_report.functions.empty()) std::cerr << "[csy] " << summarize_native_report(native_report) << "\n";
    if (!resource_guard.check("native fast-path analysis")) return 1;

    if (!output_path.empty()) {
        std::error_code ec;
        const auto parent = std::filesystem::path(output_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (ec) { std::cerr << "error: cannot create output directory: " << ec.message() << "\n"; return 1; }
        std::string object_error;
        if (!write_object_file(output_path, cg_r.module, object_error)) {
            std::cerr << "error: " << object_error << "\n";
            return 1;
        }
        if (deep) std::cout << "[stage] wrote compiled object " << output_path << "\n";
        if (make_exe) {
            std::ifstream obj_in(output_path, std::ios::binary);
            std::ostringstream obj_bytes;
            obj_bytes << obj_in.rdbuf();
            obj_in.close();
            std::string exe_error;
            if (!write_standalone_exe(output_path, obj_bytes.str(), exe_error)) {
                std::cerr << "error: " << exe_error << "\n";
                return 1;
            }
        }
    }
    progress.stage(6, output_path.empty() ? "No output artifact requested" : "Wrote runnable object '" + output_path + "'");
    mem_mark("after_object_write");
    resource_guard.release_build_slot();

    if (build_only) {
        std::cout << "[csy] build completed: " << output_path << "\n";
        if (std::getenv("CSY_PROFILE")) {
            std::fprintf(stderr, "[csy-profile] build_total_ms=%.3f build_peak_mib=%.1f threads=%u\n",
                         profile_ms(main_started, ProfileClock::now()),
                         res::peak_tracked_bytes() / (1024.0 * 1024.0), worker_threads);
        }
        return 0;
    }

    if (deep) std::cout << "[stage] running...\n";
    else      std::cerr << "[csy] running " << input_path << "...\n";

    stage_started = ProfileClock::now();
    const auto vm_started = stage_started;
    const uint64_t compile_peak_bytes = res::peak_tracked_bytes();
    res::set_tracking(false);
    ccm::user_code(true);
    VMResult vm_r = vm.run(cg_r.module);
    ccm::user_code(false);
    const auto vm_finished = ProfileClock::now();
    if (!vm_r.ok) {
        std::cerr << vm_r.diags.format(source);
        return 1;
    }
    if (verbose) std::cerr << "\n[run] Program finished successfully.\n";

    if (deep) std::cout << "[stage] exit code: " << vm_r.exit_code << "\n";
    if (profile) {
        const auto main_finished = ProfileClock::now();
        std::fprintf(stderr,
                     "[csy-profile] source_read_ms=%.3f lex_ms=%.3f parse_ms=%.3f "
                     "runtime_init_ms=%.3f semantic_ms=%.3f codegen_ms=%.3f "
                     "native_jit_ms=%.3f vm_run_ms=%.3f main_total_ms=%.3f compile_peak_mib=%.1f threads=%u\n",
                     profile_ms(source_read_started, source_read_finished),
                     profile_ms(lex_started, lex_finished),
                     profile_ms(parse_started, parse_finished),
                     profile_ms(runtime_init_started, runtime_init_finished),
                     profile_ms(semantic_started, semantic_finished),
                     profile_ms(codegen_started, codegen_finished),
                     profile_ms(jit_started, jit_finished),
                     profile_ms(vm_started, vm_finished),
                     profile_ms(main_started, main_finished),
                     compile_peak_bytes / (1024.0 * 1024.0), worker_threads);
    }
    return vm_r.exit_code;
}

static int guarded_main(int argc, char** argv) {
    try {
        return csyc_main(argc, argv);
    } catch (const res::BudgetExceeded& error) {
        res::set_tracking(false);
        res::remove_registered_temp_paths();
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    } catch (const std::bad_alloc&) {
        const bool budget = res::hard_limit_hit();
        res::set_tracking(false);
        res::remove_registered_temp_paths();
        if (budget) {
            std::cerr << "error: compiler memory budget (" << (res::limit_bytes() / (1024 * 1024))
                      << " MiB) exhausted; stopped cleanly - raise --memory-limit or split the project\n";
        } else {
            std::cerr << "error: the system ran out of memory; reduce the input size or close other programs\n";
        }
        return 1;
    } catch (const std::exception& error) {
        res::set_tracking(false);
        res::remove_registered_temp_paths();
        std::cerr << "error: compiler stopped safely: " << error.what() << "\n";
        return 1;
    }
}

int main(int argc, char** argv) {
    int code = 1;
    res::run_with_stack(512u * 1024u * 1024u, [&] {
        code = guarded_main(argc, argv);
        ccm::finish(code);
    });
    return code;
}

