#include "process.hpp"
#include "hash128.hpp"
#include "vm.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace csy {
namespace {

void report_child_error(int fd, int code) {
    const ssize_t written = ::write(fd, &code, sizeof(code));
    (void)written;
}

struct TrustRecords {
    std::set<std::string> projects;
    std::set<std::pair<std::string, std::string>> sites;
};

std::string config_directory(bool create, std::string& error) {
    const char* configured = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base;
    if (configured && *configured) {
        base = configured;
        if (!base.is_absolute()) {
            error = "XDG_CONFIG_HOME must be an absolute path for local command approvals";
            return {};
        }
    } else if (home && *home) {
        base = std::filesystem::path(home) / ".config";
    } else {
        error = "cannot locate user configuration directory for local command approvals";
        return {};
    }
    const std::filesystem::path dir = base / "csyc";
    if (create) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            error = "cannot create local command approval directory '" + dir.string() + "': " + ec.message();
            return {};
        }
        if (::chmod(dir.c_str(), 0700) != 0) {
            error = "cannot protect local command approval directory '" + dir.string() + "'";
            return {};
        }
    }
    return dir.string();
}

bool read_fd(int fd, std::string& contents, std::string& error) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid()) {
        error = "local command approval file is not a regular file owned by this user";
        return false;
    }
    if (st.st_size < 0 || st.st_size > 1024 * 1024) {
        error = "local command approval file exceeds the 1 MiB limit";
        return false;
    }
    if ((st.st_mode & 077) != 0 && ::fchmod(fd, 0600) != 0) {
        error = "cannot restrict permissions on local command approval file";
        return false;
    }
    contents.resize(static_cast<size_t>(st.st_size));
    if (::lseek(fd, 0, SEEK_SET) < 0) {
        error = "cannot read local command approval file";
        return false;
    }
    size_t read_total = 0;
    while (read_total < contents.size()) {
        const ssize_t n = ::read(fd, contents.data() + read_total, contents.size() - read_total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = "local command approval file changed while being read";
            return false;
        }
        read_total += static_cast<size_t>(n);
    }
    return true;
}

bool parse_records(const std::string& contents, TrustRecords& records, std::string& error) {
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        char kind = 0;
        std::string project, site, extra;
        if (!(row >> kind >> project) || (row >> site && kind != 'S') || (row >> extra)) {
            error = "local command approval file has an invalid record";
            return false;
        }
        if (project.size() != 32 || !std::all_of(project.begin(), project.end(), [](unsigned char c) { return std::isxdigit(c); })) {
            error = "local command approval file has an invalid project key";
            return false;
        }
        if (kind == 'P' && site.empty()) records.projects.insert(project);
        else if (kind == 'S' && site.size() == 32 &&
                 std::all_of(site.begin(), site.end(), [](unsigned char c) { return std::isxdigit(c); }))
            records.sites.emplace(project, site);
        else {
            error = "local command approval file has an invalid record";
            return false;
        }
    }
    return true;
}

bool load_records(TrustRecords& records, std::string& error) {
    const std::string dir = config_directory(false, error);
    if (dir.empty()) return false;
    const std::string path = dir + "/command-trust";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        error = "cannot open local command approval file '" + path + "'";
        return false;
    }
    std::string contents;
    const bool read_ok = read_fd(fd, contents, error);
    ::close(fd);
    return read_ok && parse_records(contents, records, error);
}

bool write_records(const TrustRecords& records, std::string& error) {
    const std::string dir = config_directory(true, error);
    if (dir.empty()) return false;
    const std::string lock_path = dir + "/command-trust.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        error = "cannot open local command approval lock";
        return false;
    }
    if (::flock(lock_fd, LOCK_EX) != 0) {
        ::close(lock_fd);
        error = "cannot lock local command approval file";
        return false;
    }
    struct stat lock_stat {};
    if (::fstat(lock_fd, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) || lock_stat.st_uid != ::getuid()) {
        ::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
        error = "local command approval lock is not a regular file owned by this user";
        return false;
    }
    TrustRecords combined;
    const std::string target = dir + "/command-trust";
    const int current_fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (current_fd >= 0) {
        std::string current;
        const bool ok = read_fd(current_fd, current, error);
        ::close(current_fd);
        if (!ok || !parse_records(current, combined, error)) {
            ::flock(lock_fd, LOCK_UN);
            ::close(lock_fd);
            return false;
        }
    } else if (errno != ENOENT) {
        ::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
        error = "cannot open local command approval file";
        return false;
    }
    combined.projects.insert(records.projects.begin(), records.projects.end());
    combined.sites.insert(records.sites.begin(), records.sites.end());
    std::ostringstream data;
    for (const auto& project : combined.projects) data << "P " << project << "\n";
    for (const auto& site : combined.sites) data << "S " << site.first << " " << site.second << "\n";
    const std::string contents = data.str();
    std::string temporary = target + ".tmp.XXXXXX";
    std::vector<char> temp_path(temporary.begin(), temporary.end());
    temp_path.push_back('\0');
    const int fd = ::mkstemp(temp_path.data());
    temporary = temp_path.data();
    bool ok = fd >= 0;
    if (ok) {
        if (::fchmod(fd, 0600) != 0) ok = false;
        size_t written = 0;
        while (ok && written < contents.size()) {
            const ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            written += static_cast<size_t>(n);
        }
        if (ok && ::fsync(fd) != 0) ok = false;
        if (::close(fd) != 0) ok = false;
    }
    if (ok && ::rename(temporary.c_str(), target.c_str()) != 0) ok = false;
    if (!ok) {
        ::unlink(temporary.c_str());
        error = "cannot save local command approval";
    }
    ::flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
    return ok;
}

std::string canonical_directory(const std::string& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) return std::filesystem::path(path).lexically_normal().string();
    const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).string();
}

std::string project_key(const std::string& project_root) {
    return hash128_hex(canonical_directory(project_root));
}

bool prompt_for_approval(const std::string& project,
                         const std::string& site,
                         const std::string& location,
                         std::string& error) {
    std::cerr << "warning: " << location << ": this call can run an external command, including commands that modify or delete data or change system state\n"
              << "  1. I know what I'm doing.\n"
              << "  2. NO!\n"
              << "  3. I know what I'm doing. DON'T warn me again in THIS project directory\n"
              << "Choose 1, 2, or 3: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        error = "external command approval was not provided";
        return false;
    }
    if (answer != "1" && answer != "2" && answer != "3") {
        error = "external command approval was denied";
        return false;
    }
    TrustRecords records;
    if (!load_records(records, error)) return false;
    if (answer == "2") {
        error = "external command approval was denied";
        return false;
    }
    if (answer == "3") records.projects.insert(project);
    else records.sites.emplace(project, site);
    return write_records(records, error);
}

std::string source_file_fingerprint(const std::string& path, const std::string& source) {
    return hash128_hex(path + "\n" + hash128_hex(source));
}

bool process_member(const std::string& module, const std::string& member) {
    return (module == "process" && member == "run") ||
           (module == "shell" && (member == "run" || member == "interactive"));
}

std::string process_builtin(const std::string& module, const std::string& member) {
    if (module == "process") return "process_" + member;
    return "shell_" + member;
}

void annotate_expr(const ExprPtr& e,
                   const std::unordered_map<std::string, std::string>& modules,
                   const std::unordered_map<std::string, std::pair<std::string, std::string>>& using_modules,
                   const std::string& project_root,
                   const std::string& file_fingerprint,
                   const std::string& location,
                   std::vector<std::pair<ExprPtr, std::string>>& calls) {
    if (!e) return;
    if (e->kind == ExprKind::Call) {
        std::string module, member;
        if (e->kids().callee && e->kids().callee->kind == ExprKind::Member &&
            e->kids().callee->kids().object && e->kids().callee->kids().object->kind == ExprKind::Ident) {
            auto it = modules.find(e->kids().callee->kids().object->name);
            if (it != modules.end()) {
                module = it->second;
                member = e->kids().callee->name;
            }
        } else if (e->kids().callee && e->kids().callee->kind == ExprKind::Ident) {
            auto it = using_modules.find(e->kids().callee->name);
            if (it != using_modules.end()) {
                module = it->second.first;
                member = it->second.second;
            }
        }
        if (process_member(module, member)) {
            const std::string site_material =
                file_fingerprint + "\n" + std::to_string(e->tok.line) + "\n" +
                std::to_string(e->tok.col) + "\n" + process_builtin(module, member);
            e->aux_mut().process_project_root = project_root;
            e->aux_mut().process_site_key = hash128_hex(site_material);
            e->aux_mut().process_site_location = location + ":" + std::to_string(e->tok.line);
            calls.emplace_back(e, e->aux().process_site_location);
        }
    }
    annotate_expr(e->kids().left, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().right, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().operand, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().target, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().value, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().callee, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().object, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(e->kids().index, modules, using_modules, project_root, file_fingerprint, location, calls);
    for (const auto& arg : e->kids().args) annotate_expr(arg, modules, using_modules, project_root, file_fingerprint, location, calls);
    for (const auto& element : e->kids().elements) annotate_expr(element, modules, using_modules, project_root, file_fingerprint, location, calls);
    for (const auto& pair : e->kids().dict_pairs) {
        annotate_expr(pair.first, modules, using_modules, project_root, file_fingerprint, location, calls);
        annotate_expr(pair.second, modules, using_modules, project_root, file_fingerprint, location, calls);
    }
}

void annotate_stmt(const StmtPtr& s,
                   const std::unordered_map<std::string, std::string>& modules,
                   const std::unordered_map<std::string, std::pair<std::string, std::string>>& using_modules,
                   const std::string& project_root,
                   const std::string& file_fingerprint,
                   const std::string& location,
                   std::vector<std::pair<ExprPtr, std::string>>& calls) {
    if (!s) return;
    annotate_expr(s->expr, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(s->aux().using_expr, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(s->cond, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(s->aux().iterable, modules, using_modules, project_root, file_fingerprint, location, calls);
    annotate_expr(s->ret_value, modules, using_modules, project_root, file_fingerprint, location, calls);
    for (const auto& child : s->body) annotate_stmt(child, modules, using_modules, project_root, file_fingerprint, location, calls);
    for (const auto& child : s->aux().then_body) annotate_stmt(child, modules, using_modules, project_root, file_fingerprint, location, calls);
    for (const auto& branch : s->aux().elifs) {
        annotate_expr(branch.first, modules, using_modules, project_root, file_fingerprint, location, calls);
        for (const auto& child : branch.second) annotate_stmt(child, modules, using_modules, project_root, file_fingerprint, location, calls);
    }
    for (const auto& child : s->aux().else_body) annotate_stmt(child, modules, using_modules, project_root, file_fingerprint, location, calls);
}

using Args = std::vector<Value>;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string string_arg(const Value& value, const std::string& who) {
    if (value.type != Value::Str || !value.str_val) fail(who + " expects string arguments");
    const std::string& result = *value.str_val;
    if (result.find('\0') != std::string::npos) fail(who + " does not accept strings containing NUL");
    return result;
}

struct SiteInfo {
    std::string project_root;
    std::string key;
    std::string location;
};

SiteInfo remove_site_info(Args& args, const std::string& who) {
    if (args.size() < 3) fail(who + " is missing compiler approval metadata");
    SiteInfo site;
    site.project_root = string_arg(args[args.size() - 3], who);
    site.key = string_arg(args[args.size() - 2], who);
    site.location = string_arg(args.back(), who);
    args.resize(args.size() - 3);
    if (site.project_root.empty() || site.key.size() != 32)
        fail(who + " has invalid compiler approval metadata");
    return site;
}

void require_site_approval(const SiteInfo& site, const std::string& who) {
    std::string error;
    if (!process_call_is_approved(site.project_root, site.key, site.location, true, error))
        fail(error.empty() ? who + " was not approved" : error);
}

struct RunOptions {
    std::string cwd;
    std::map<std::string, std::string> environment;
    long long timeout_ms = 0;
    bool stream = false;
};

RunOptions parse_options(const Value* value, const std::string& who) {
    RunOptions options;
    if (!value) return options;
    if (value->type != Value::Dict || !value->dict_val) fail(who + " options must be a dictionary");
    for (const auto& item : *value->dict_val) {
        if (item.first == "cwd") {
            options.cwd = string_arg(item.second, who + " option 'cwd'");
        } else if (item.first == "timeout_ms") {
            if (item.second.type != Value::Int || item.second.int_val < 0 ||
                item.second.int_val > 604800000LL)
                fail(who + " option 'timeout_ms' must be an integer from 0 to 604800000");
            options.timeout_ms = item.second.int_val;
        } else if (item.first == "stream") {
            if (item.second.type != Value::Bool) fail(who + " option 'stream' must be a boolean");
            options.stream = item.second.bool_val;
        } else if (item.first == "env") {
            if (item.second.type != Value::Dict || !item.second.dict_val)
                fail(who + " option 'env' must be a dictionary of string keys and values");
            for (const auto& entry : *item.second.dict_val) {
                if (entry.first.empty() || entry.first.find('=') != std::string::npos ||
                    entry.first.find('\0') != std::string::npos)
                    fail(who + " option 'env' has an invalid variable name");
                options.environment[entry.first] = string_arg(entry.second, who + " option 'env'");
            }
        } else {
            fail(who + " has unknown option '" + item.first + "'");
        }
    }
    return options;
}

bool write_all(int fd, const char* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool make_pipe(int fds[2]) {
    return ::pipe2(fds, O_CLOEXEC) == 0;
}

struct RunResult {
    std::string out;
    std::string err;
    int status = 127;
    bool timed_out = false;
};

constexpr size_t kMaxCapturedOutput = 64ull * 1024 * 1024;

RunResult run_process(const std::string& program,
                     const std::vector<std::string>& arguments,
                     const RunOptions& options) {
    if (program.empty()) fail("process::run expects a non-empty program name");
    std::map<std::string, std::string> environment;
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const size_t equals = entry.find('=');
        if (equals != std::string::npos) environment[entry.substr(0, equals)] = entry.substr(equals + 1);
    }
    for (const auto& item : options.environment) environment[item.first] = item.second;

    std::vector<std::string> env_strings;
    env_strings.reserve(environment.size());
    for (const auto& item : environment) env_strings.push_back(item.first + "=" + item.second);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& item : env_strings) envp.push_back(item.data());
    envp.push_back(nullptr);

    std::vector<std::string> argv_strings;
    argv_strings.reserve(arguments.size() + 1);
    argv_strings.push_back(program);
    argv_strings.insert(argv_strings.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(argv_strings.size() + 1);
    for (auto& item : argv_strings) argv.push_back(item.data());
    argv.push_back(nullptr);

    std::vector<std::string> executable_paths;
    if (program.find('/') != std::string::npos) {
        executable_paths.push_back(program);
    } else {
        const auto path_it = environment.find("PATH");
        const std::string path = path_it == environment.end() ? "/bin:/usr/bin" : path_it->second;
        std::stringstream parts(path);
        for (std::string directory; std::getline(parts, directory, ':');) {
            if (directory.empty()) directory = ".";
            executable_paths.push_back(directory + "/" + program);
        }
    }

    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1}, exec_pipe[2] = {-1, -1};
    if (!make_pipe(out_pipe) || !make_pipe(err_pipe) || !make_pipe(exec_pipe)) {
        close_fd(out_pipe[0]); close_fd(out_pipe[1]);
        close_fd(err_pipe[0]); close_fd(err_pipe[1]);
        close_fd(exec_pipe[0]); close_fd(exec_pipe[1]);
        fail("process::run could not create output pipes: " + std::string(std::strerror(errno)));
    }
    const pid_t child = ::fork();
    if (child < 0) {
        const int error_code = errno;
        close_fd(out_pipe[0]); close_fd(out_pipe[1]);
        close_fd(err_pipe[0]); close_fd(err_pipe[1]);
        close_fd(exec_pipe[0]); close_fd(exec_pipe[1]);
        fail("process::run could not start '" + program + "': " + std::strerror(error_code));
    }
    if (child == 0) {
        ::setpgid(0, 0);
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::close(exec_pipe[0]);
        if (::dup2(out_pipe[1], STDOUT_FILENO) < 0 || ::dup2(err_pipe[1], STDERR_FILENO) < 0) {
            const int error_code = errno;
            report_child_error(exec_pipe[1], error_code);
            ::_exit(127);
        }
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0) {
            const int error_code = errno;
            report_child_error(exec_pipe[1], error_code);
            ::_exit(127);
        }
        int last_error = ENOENT;
        for (const auto& path : executable_paths) {
            ::execve(path.c_str(), argv.data(), envp.data());
            last_error = errno;
            if (last_error != ENOENT && last_error != ENOTDIR) break;
        }
        report_child_error(exec_pipe[1], last_error);
        ::_exit(127);
    }

    ::setpgid(child, child);
    close_fd(out_pipe[1]);
    close_fd(err_pipe[1]);
    close_fd(exec_pipe[1]);
    for (int fd : {out_pipe[0], err_pipe[0], exec_pipe[0]}) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::kill(-child, SIGKILL);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            close_fd(out_pipe[0]); close_fd(err_pipe[0]); close_fd(exec_pipe[0]);
            fail("process::run could not configure output pipes");
        }
    }

    RunResult result;
    size_t total_output = 0;
    bool out_open = true, err_open = true, exec_open = true, child_done = false;
    bool output_limit = false, termination_sent = false, kill_sent = false;
    int wait_status = 0, exec_error = 0;
    size_t exec_error_bytes = 0;
    const auto started = std::chrono::steady_clock::now();
    auto termination_time = started;
    auto drain = [&](int& fd, bool& open, std::string& target, int stream_fd) {
        std::array<char, 16384> buffer {};
        for (;;) {
            const ssize_t count = ::read(fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count <= 0) {
                if (count == 0) { close_fd(fd); open = false; }
                return;
            }
            const size_t size = static_cast<size_t>(count);
            total_output += size;
            if (total_output > kMaxCapturedOutput) {
                output_limit = true;
                ::kill(-child, SIGKILL);
                return;
            }
            target.append(buffer.data(), size);
            if (options.stream && !write_all(stream_fd, buffer.data(), size))
                fail("process::run could not stream child output");
        }
    };

    try {
    while (out_open || err_open || exec_open || !child_done) {
        const auto now = std::chrono::steady_clock::now();
        if (options.timeout_ms > 0 && !result.timed_out &&
            now - started >= std::chrono::milliseconds(options.timeout_ms)) {
            result.timed_out = true;
            termination_sent = true;
            termination_time = now;
            ::kill(-child, SIGTERM);
        }
        if (termination_sent && !kill_sent && (!child_done || out_open || err_open) &&
            now - termination_time >= std::chrono::milliseconds(200)) {
            ::kill(-child, SIGKILL);
            kill_sent = true;
        }

        pollfd descriptors[3] = {
            {out_open ? out_pipe[0] : -1, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0},
            {err_open ? err_pipe[0] : -1, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0},
            {exec_open ? exec_pipe[0] : -1, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0}
        };
        int wait_ms = 100;
        if (options.timeout_ms > 0 && !result.timed_out) {
            const auto left = std::chrono::milliseconds(options.timeout_ms) -
                              std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
            wait_ms = static_cast<int>(std::max<long long>(0, std::min<long long>(100, left.count())));
        }
        const int polled = ::poll(descriptors, 3, wait_ms);
        if (polled < 0 && errno != EINTR) {
            ::kill(-child, SIGKILL);
            while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {}
            close_fd(out_pipe[0]); close_fd(err_pipe[0]); close_fd(exec_pipe[0]);
            fail("process::run failed while waiting for child output");
        }
        if (out_open && descriptors[0].revents) drain(out_pipe[0], out_open, result.out, STDOUT_FILENO);
        if (err_open && descriptors[1].revents) drain(err_pipe[0], err_open, result.err, STDERR_FILENO);
        if (exec_open && descriptors[2].revents) {
            for (;;) {
                std::array<char, sizeof(int)> bytes {};
                const ssize_t count = ::read(exec_pipe[0], bytes.data(), bytes.size());
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                if (count == 0) { close_fd(exec_pipe[0]); exec_open = false; break; }
                if (count > 0) {
                    const size_t n = static_cast<size_t>(count);
                    std::memcpy(reinterpret_cast<char*>(&exec_error) + exec_error_bytes, bytes.data(), n);
                    exec_error_bytes += n;
                    if (exec_error_bytes >= sizeof(exec_error)) {
                        close_fd(exec_pipe[0]);
                        exec_open = false;
                        break;
                    }
                }
            }
        }
        if (!child_done) {
            const pid_t waited = ::waitpid(child, &wait_status, WNOHANG);
            if (waited == child) child_done = true;
            else if (waited < 0 && errno != EINTR) {
                close_fd(out_pipe[0]); close_fd(err_pipe[0]); close_fd(exec_pipe[0]);
                fail("process::run could not collect the child process");
            }
        }
    }
    } catch (...) {
        ::kill(-child, SIGKILL);
        while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {}
        close_fd(out_pipe[0]); close_fd(err_pipe[0]); close_fd(exec_pipe[0]);
        throw;
    }
    close_fd(out_pipe[0]); close_fd(err_pipe[0]); close_fd(exec_pipe[0]);
    if (!child_done) {
        while (::waitpid(child, &wait_status, 0) < 0) {
            if (errno == EINTR) continue;
            fail("process::run could not collect the child process");
        }
    }
    if (exec_error_bytes == sizeof(exec_error))
        fail("process::run could not start '" + program + "': " + std::strerror(exec_error));
    if (output_limit) fail("process::run output exceeded the 64 MiB capture limit");
    if (result.timed_out) result.status = 124;
    else if (WIFEXITED(wait_status)) result.status = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) result.status = 128 + WTERMSIG(wait_status);
    else result.status = 127;
    if (options.stream) {
        std::cout.flush();
        std::cerr.flush();
    }
    return result;
}

Value process_result(RunResult result) {
    DictMap values;
    values["stdout"] = Value::make_str(std::move(result.out));
    values["stderr"] = Value::make_str(std::move(result.err));
    values["status"] = Value::make_int(result.status);
    values["timed_out"] = Value::make_bool(result.timed_out);
    return Value::make_dict(std::move(values));
}

bool is_privilege_name(const std::string& value) {
    const size_t slash = value.find_last_of('/');
    std::string base = slash == std::string::npos ? value : value.substr(slash + 1);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return base == "sudo" || base == "sudoedit" || base == "doas" || base == "pkexec" || base == "run0" || base == "su";
}

bool script_mentions_privilege_plain(const std::string& script) {
    size_t i = 0;
    while (i < script.size()) {
        while (i < script.size() && !(std::isalnum(static_cast<unsigned char>(script[i])) ||
                                      script[i] == '_' || script[i] == '/' || script[i] == '-')) ++i;
        const size_t start = i;
        while (i < script.size() && (std::isalnum(static_cast<unsigned char>(script[i])) ||
                                     script[i] == '_' || script[i] == '/' || script[i] == '-')) ++i;
        if (start != i && is_privilege_name(script.substr(start, i - start))) return true;
    }
    return false;
}

bool script_mentions_privilege(const std::string& script) {
    if (script_mentions_privilege_plain(script)) return true;
    std::string joined;
    joined.reserve(script.size());
    for (char c : script) if (c != '\'' && c != '"' && c != '\\') joined.push_back(c);
    return joined.size() != script.size() && script_mentions_privilege_plain(joined);
}

bool ask_yes(const std::string& prompt) {
    std::cerr << prompt << " [y/N]: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    std::transform(answer.begin(), answer.end(), answer.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "y" || answer == "yes";
}

bool confirm_privilege() {
    if (!ask_yes("Are you sure? (sudo/doas can run with elevated privileges)")) return false;
    if (!ask_yes("Are you REALLY sure? (sudo/doas can change or damage the system)")) return false;
    std::cerr << "aight. fine\n";
    return true;
}

bool command_needs_privilege_confirmation(const std::string& program,
                                         const std::vector<std::string>& arguments) {
    if (is_privilege_name(program)) return true;
    static const char* const wrappers[] = {"env", "nice", "ionice", "timeout", "nohup", "xargs", "stdbuf", "time",
                                           "setsid", "chrt", "taskset", "command", "exec", "busybox", "strace",
                                           "ltrace", "flock", "watch", "unbuffer", "chroot", "nsenter", "systemd-run",
                                           "script", "parallel", "find"};
    static const char* const interpreters[] = {"bash", "sh", "dash", "zsh", "ksh", "fish", "ash", "python", "python3",
                                               "perl", "ruby", "node", "php", "lua", "awk", "gawk", "expect", "tclsh"};
    const std::string base = std::filesystem::path(program).filename().string();
    for (const char* wrapper : wrappers) {
        if (base == wrapper) {
            for (const auto& argument : arguments)
                if (is_privilege_name(argument) || script_mentions_privilege(argument)) return true;
        }
    }
    for (const char* interpreter : interpreters) {
        if (base == interpreter) {
            for (const auto& argument : arguments)
                if (script_mentions_privilege(argument)) return true;
        }
    }
    return false;
}

int wait_for_process(pid_t child, const std::string& command) {
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fail(command + " could not collect the child process");
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
}

std::string interactive_bash_rc() {
    return "if [ -r \"$HOME/.bashrc\" ]; then . \"$HOME/.bashrc\"; fi\n"
           "unalias sudo doas pkexec run0 su 2>/dev/null\n"
           "_csyc_confirm_privilege() {\n"
           "  printf 'Are you sure? (sudo/doas can run with elevated privileges) [y/N]: ' >&2\n"
           "  IFS= read -r _csyc_answer || return 1\n"
           "  case \"$_csyc_answer\" in y|Y|yes|YES) ;; *) return 1 ;; esac\n"
           "  printf 'Are you REALLY sure? (sudo/doas can change or damage the system) [y/N]: ' >&2\n"
           "  IFS= read -r _csyc_answer || return 1\n"
           "  case \"$_csyc_answer\" in y|Y|yes|YES) ;; *) return 1 ;; esac\n"
           "  printf 'aight. fine\\n' >&2\n"
           "}\n"
           "sudo() { _csyc_confirm_privilege || return 126; command sudo \"$@\"; }\n"
           "doas() { _csyc_confirm_privilege || return 126; command doas \"$@\"; }\n"
           "pkexec() { _csyc_confirm_privilege || return 126; command pkexec \"$@\"; }\n"
           "run0() { _csyc_confirm_privilege || return 126; command run0 \"$@\"; }\n"
           "su() { _csyc_confirm_privilege || return 126; command su \"$@\"; }\n";
}

int run_interactive_bash() {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO))
        fail("shell::interactive requires an interactive terminal");
    const pid_t original_group = ::tcgetpgrp(STDIN_FILENO);
    if (original_group < 0) fail("shell::interactive could not inspect the terminal process group");
    std::string pattern = "/tmp/csyc-bash-rc.XXXXXX";
    std::vector<char> temp_path(pattern.begin(), pattern.end());
    temp_path.push_back('\0');
    const int rc_fd = ::mkstemp(temp_path.data());
    if (rc_fd < 0) fail("shell::interactive could not create its temporary Bash configuration");
    const std::string rc_path = temp_path.data();
    const std::string rc = interactive_bash_rc();
    const bool rc_ok = ::fchmod(rc_fd, 0600) == 0 && write_all(rc_fd, rc.data(), rc.size()) && ::fsync(rc_fd) == 0;
    ::close(rc_fd);
    if (!rc_ok) {
        ::unlink(rc_path.c_str());
        fail("shell::interactive could not write its temporary Bash configuration");
    }
    const pid_t child = ::fork();
    if (child < 0) {
        const int error_code = errno;
        ::unlink(rc_path.c_str());
        fail("shell::interactive could not start Bash: " + std::string(std::strerror(error_code)));
    }
    if (child == 0) {
        ::setpgid(0, 0);
        char* argv[] = {
            const_cast<char*>("bash"), const_cast<char*>("--noprofile"),
            const_cast<char*>("--rcfile"), const_cast<char*>(rc_path.c_str()),
            const_cast<char*>("-i"), nullptr
        };
        ::execv("/bin/bash", argv);
        ::_exit(127);
    }
    ::setpgid(child, child);
    sigset_t blocked, previous;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGTTOU);
    if (::sigprocmask(SIG_BLOCK, &blocked, &previous) != 0) {
        ::kill(child, SIGKILL);
        wait_for_process(child, "shell::interactive");
        ::unlink(rc_path.c_str());
        fail("shell::interactive could not protect terminal handoff");
    }
    if (::tcsetpgrp(STDIN_FILENO, child) != 0) {
        const int error_code = errno;
        ::sigprocmask(SIG_SETMASK, &previous, nullptr);
        ::kill(child, SIGKILL);
        wait_for_process(child, "shell::interactive");
        ::unlink(rc_path.c_str());
        fail("shell::interactive could not hand the terminal to Bash: " + std::string(std::strerror(error_code)));
    }
    int status = 127;
    try {
        status = wait_for_process(child, "shell::interactive");
    } catch (...) {
        ::tcsetpgrp(STDIN_FILENO, original_group);
        ::sigprocmask(SIG_SETMASK, &previous, nullptr);
        ::unlink(rc_path.c_str());
        throw;
    }
    const bool terminal_restored = ::tcsetpgrp(STDIN_FILENO, original_group) == 0;
    ::sigprocmask(SIG_SETMASK, &previous, nullptr);
    ::unlink(rc_path.c_str());
    if (!terminal_restored) fail("shell::interactive could not restore the terminal process group");
    return status;
}

}

bool process_call_is_approved(const std::string& project_root,
                              const std::string& site_key,
                              const std::string& location,
                              bool prompt,
                              std::string& error) {
    const std::string project = project_key(project_root);
    TrustRecords records;
    if (!load_records(records, error)) return false;
    if (records.projects.count(project) || records.sites.count({project, site_key})) return true;
    if (!prompt) return false;
    return prompt_for_approval(project, site_key, location, error);
}

bool approve_process_calls(std::vector<SourceUnit>& units,
                          const std::string& project_root,
                          std::string& error) {
    const std::string canonical_root = canonical_directory(project_root);
    std::vector<std::pair<ExprPtr, std::string>> calls;
    for (auto& unit : units) {
        std::unordered_map<std::string, std::string> modules;
        std::unordered_map<std::string, std::pair<std::string, std::string>> using_modules;
        for (const auto& stmt : unit.parsed.module.body) {
            if (stmt->kind == StmtKind::Import) modules[stmt->aux().import_alias] = stmt->aux().import_module;
        }
        for (const auto& stmt : unit.parsed.module.body) {
            if (stmt->kind != StmtKind::Using || !stmt->aux().using_expr ||
                stmt->aux().using_expr->kind != ExprKind::Member || !stmt->aux().using_expr->kids().object ||
                stmt->aux().using_expr->kids().object->kind != ExprKind::Ident) continue;
            auto module = modules.find(stmt->aux().using_expr->kids().object->name);
            if (module != modules.end() && process_member(module->second, stmt->aux().using_expr->name))
                using_modules[stmt->aux().using_alias] = {module->second, stmt->aux().using_expr->name};
        }
        const std::string file = canonical_directory(std::filesystem::path(unit.path).parent_path().string()) +
                                 "/" + std::filesystem::path(unit.path).filename().string();
        const std::string fingerprint = source_file_fingerprint(file, unit.source);
        for (const auto& stmt : unit.parsed.module.body)
            annotate_stmt(stmt, modules, using_modules, canonical_root, fingerprint, file, calls);
    }
    const std::string project = project_key(canonical_root);
    TrustRecords records;
    if (!load_records(records, error)) return false;
    if (records.projects.count(project)) return true;
    for (const auto& call : calls) {
        if (records.sites.count({project, call.first->aux().process_site_key})) continue;
        std::cerr << "warning: " << call.second << ": external command call requires approval\n"
                  << "  1. I know what I'm doing.\n"
                  << "  2. NO!\n"
                  << "  3. I know what I'm doing. DON'T warn me again in THIS project directory\n"
                  << "Choose 1, 2, or 3: " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer)) {
            error = "external command approval was not provided";
            return false;
        }
        if (answer == "2") {
            error = "external command approval was denied at " + call.second;
            return false;
        }
        if (answer != "1" && answer != "3") {
            error = "external command approval was denied at " + call.second;
            return false;
        }
        if (answer == "3") {
            records.projects.insert(project);
            if (!write_records(records, error)) return false;
            break;
        }
        records.sites.emplace(project, call.first->aux().process_site_key);
        if (!write_records(records, error)) return false;
    }
    return true;
}

void register_process_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();
    builtins["process_run"] = [](VM&, Args& args) -> Value {
        const SiteInfo site = remove_site_info(args, "process::run");
        require_site_approval(site, "process::run");
        if (args.size() < 2 || args.size() > 3)
            fail("process::run expects (program, arguments, options?)");
        const std::string program = string_arg(args[0], "process::run");
        if (args[1].type != Value::Vec || !args[1].vec_val)
            fail("process::run arguments must be a vector of strings");
        std::vector<std::string> command_args;
        command_args.reserve(args[1].vec_val->size());
        for (const auto& item : *args[1].vec_val)
            command_args.push_back(string_arg(item, "process::run"));
        const RunOptions options = parse_options(args.size() == 3 ? &args[2] : nullptr, "process::run");
        if (command_needs_privilege_confirmation(program, command_args) && !confirm_privilege())
            fail("process::run was cancelled before elevated command execution");
        return process_result(run_process(program, command_args, options));
    };
    builtins["shell_run"] = [](VM&, Args& args) -> Value {
        const SiteInfo site = remove_site_info(args, "shell::run");
        require_site_approval(site, "shell::run");
        if (args.empty() || args.size() > 2)
            fail("shell::run expects (script, options?)");
        const std::string script = string_arg(args[0], "shell::run");
        const RunOptions options = parse_options(args.size() == 2 ? &args[1] : nullptr, "shell::run");
        if (script_mentions_privilege(script) && !confirm_privilege())
            fail("shell::run was cancelled before elevated command execution");
        return process_result(run_process("/bin/bash", {"--noprofile", "--norc", "-c", script}, options));
    };
    builtins["shell_interactive"] = [](VM&, Args& args) -> Value {
        const SiteInfo site = remove_site_info(args, "shell::interactive");
        require_site_approval(site, "shell::interactive");
        if (!args.empty()) fail("shell::interactive expects no arguments");
        return Value::make_int(run_interactive_bash());
    };
}

}
