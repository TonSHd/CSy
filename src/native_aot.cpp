#include "native_aot.hpp"
#include "resource.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <vector>

namespace csy {
namespace {

bool integer_expression(const ExprPtr& expr) {
    if (!expr) return false;
    if (expr->kind == ExprKind::Int) return true;
    if (expr->kind == ExprKind::Unary)
        return expr->op == TokenType::MINUS && integer_expression(expr->kids().operand);
    if (expr->kind == ExprKind::Binary)
        return (expr->op == TokenType::PLUS || expr->op == TokenType::MINUS ||
                expr->op == TokenType::STAR) &&
               integer_expression(expr->kids().left) && integer_expression(expr->kids().right);
    return false;
}

bool write_all(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += static_cast<size_t>(count);
    }
    return true;
}

struct TempPath {
    std::string path;
    int fd = -1;

    ~TempPath() {
        if (fd >= 0) ::close(fd);
        if (!path.empty()) {
            ::unlink(path.c_str());
            res::unregister_temp_path(path);
        }
    }

    bool create(const std::filesystem::path& directory, std::string& error) {
        std::string pattern = (directory / ".csy-native-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        fd = ::mkstemp(buffer.data());
        if (fd < 0) {
            error = "cannot create a temporary native build file: " + std::string(std::strerror(errno));
            return false;
        }
        path = buffer.data();
        res::register_temp_path(path);
        return true;
    }

    bool close_file() {
        if (fd < 0) return true;
        const int file = fd;
        fd = -1;
        return ::close(file) == 0;
    }
};

int invoke_compiler(const std::string& compiler, const std::string& source,
                    const std::string& output, std::string& error) {
    const pid_t child = ::fork();
    if (child < 0) {
        error = "cannot start the native C compiler: " + std::string(std::strerror(errno));
        return -1;
    }
    if (child == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        char* argv[] = {
            const_cast<char*>(compiler.c_str()),
            const_cast<char*>("-O2"),
            const_cast<char*>("-std=c11"),
            const_cast<char*>("-fno-math-errno"),
            const_cast<char*>("-ffp-contract=off"),
            const_cast<char*>("-x"),
            const_cast<char*>("c"),
            const_cast<char*>(source.c_str()),
            const_cast<char*>("-lm"),
            const_cast<char*>("-o"),
            const_cast<char*>(output.c_str()),
            nullptr
        };
        ::execvp(compiler.c_str(), argv);
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        error = "cannot collect the native C compiler process: " + std::string(std::strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 127;
}

}

bool write_native_aot(const Module& source, const IRModule& ir,
                      const NativeReport& report, const std::string& output,
                      std::string& error) {
    const Stmt* entry = nullptr;
    if (source.body.size() == 1 && source.body.front() &&
        source.body.front()->kind == StmtKind::FuncDecl)
        entry = source.body.front().get();
    if (!entry) {
        error = "native-only build supports a source containing exactly one function";
        return false;
    }
    if (entry->aux().func_name != "main" && entry->aux().func_name != "__csy_u0_main") {
        error = "native-only build requires the function to be named main";
        return false;
    }
    if (!entry->aux().params.empty()) {
        error = "native-only build requires main to have no parameters";
        return false;
    }
    if (entry->body.size() != 1 || !entry->body.front() ||
        entry->body.front()->kind != StmtKind::Return) {
        error = "native-only build requires main to contain exactly one return statement";
        return false;
    }
    if (!integer_expression(entry->body.front()->ret_value)) {
        error = "native-only build requires main to return an integer expression using literals and +, -, or *";
        return false;
    }
    if (report.functions.size() != 1 || !report.functions.front().native ||
        ir.native_c_source.empty()) {
        error = "native-only build supports only native-eligible integer main functions";
        return false;
    }
    if (output.empty()) {
        error = "native-only build requires an output path";
        return false;
    }

    const std::filesystem::path destination(output);
    const std::filesystem::path directory =
        destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create native executable directory '" + directory.string() + "': " + ec.message();
        return false;
    }

    TempPath source_file, executable;
    if (!source_file.create(directory, error) || !executable.create(directory, error)) return false;
    const std::string wrapper =
        "\n#include <stdio.h>\n"
        "int main(int argc, char **argv) {\n"
        "  (void)argv;\n"
        "  if (argc != 1) { fputs(\"error: native prototype does not accept program arguments\\n\", stderr); return 64; }\n"
        "  if (csy_entry_count != 1 || csy_entry_table[0].ret_float != 0) { fputs(\"error: native entry must return an integer\\n\", stderr); return 1; }\n"
        "  CsyNativeArg args[1] = {{0}};\n"
        "  CsyRet result = {0};\n"
        "  int err = 0, tag = 0;\n"
        "  csy_entry_table[0].entry(args, &err, &tag, &result);\n"
        "  if (err) {\n"
        "    err &= 255;\n"
        "    const char *message = \"native arithmetic error\";\n"
        "    if (err == 1) message = \"division by zero\";\n"
        "    else if (err == 2) message = \"maximum recursion depth exceeded (20000 nested calls)\";\n"
        "    else if (err == 3) message = \"f64_array index out of bounds\";\n"
        "    else if (err == 4) message = \"integer overflow in addition\";\n"
        "    else if (err == 6) message = \"integer overflow in subtraction\";\n"
        "    else if (err == 7) message = \"integer overflow in multiplication\";\n"
        "    else if (err == 8) message = \"integer overflow in negation\";\n"
        "    else if (err == 9) message = \"integer overflow in division\";\n"
        "    else if (err == 10) message = \"float is outside the integer range\";\n"
        "    else if (err == 12) message = \"modulo by zero\";\n"
        "    else if (err == 13) message = \"i64_array index out of bounds\";\n"
        "    fprintf(stderr, \"error: %s\\n\", message);\n"
        "    return 1;\n"
        "  }\n"
        "  return (int)((unsigned long long)result.i & 255u);\n"
        "}\n";
    if (!write_all(source_file.fd, ir.native_c_source + wrapper) ||
        ::fsync(source_file.fd) != 0 || !source_file.close_file()) {
        error = "cannot write the native C source";
        return false;
    }
    if (!executable.close_file()) {
        error = "cannot prepare the native executable output";
        return false;
    }

    const char* configured_compiler = std::getenv("CC");
    const std::string compiler = configured_compiler && *configured_compiler ? configured_compiler : "cc";
    const int status = invoke_compiler(compiler, source_file.path, executable.path, error);
    if (status != 0) {
        if (error.empty())
            error = "native C compiler '" + compiler + "' failed with exit status " + std::to_string(status);
        return false;
    }
    if (::chmod(executable.path.c_str(), 0755) != 0) {
        error = "cannot mark the native executable as runnable: " + std::string(std::strerror(errno));
        return false;
    }
    const int output_fd = ::open(executable.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (output_fd < 0 || ::fsync(output_fd) != 0) {
        const int saved_errno = errno;
        if (output_fd >= 0) ::close(output_fd);
        error = "cannot finalize the native executable: " + std::string(std::strerror(saved_errno));
        return false;
    }
    ::close(output_fd);
    struct stat output_stat{};
    if (::stat(output.c_str(), &output_stat) == 0 && !S_ISREG(output_stat.st_mode)) {
        error = "'" + output + "' exists and is not a regular file, so it will not be replaced";
        return false;
    }
    if (::rename(executable.path.c_str(), output.c_str()) != 0) {
        error = "cannot install native executable '" + output + "': " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

}
