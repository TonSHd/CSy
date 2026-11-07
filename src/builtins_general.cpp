#include "builtins_general.hpp"
#include "vm.hpp"
#include <filesystem>
#include <stdexcept>

namespace csy {

namespace {

using Args = std::vector<Value>;
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

const std::string& path_arg(const Args& args, const char* name) {
    if (args.size() != 1 || args[0].type != Value::Str)
        fail(std::string("fs::") + name + " expects (path)");
    return *args[0].str_val;
}

const std::string& path_arg(const Args& args, size_t index, const char* name) {
    if (args.size() <= index || args[index].type != Value::Str)
        fail(std::string("path::") + name + " expects string arguments");
    return *args[index].str_val;
}

const std::string& path_single_arg(const Args& args, const char* name) {
    if (args.size() != 1 || args[0].type != Value::Str)
        fail(std::string("path::") + name + " expects (path)");
    return *args[0].str_val;
}

Value path_string(const std::filesystem::path& result) {
    return Value::make_str(result.generic_string());
}

}

void register_general_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();

    builtins["path_join"] = [](VM&, Args& args) -> Value {
        if (args.size() < 2) fail("path::join expects (path, path, ...)");
        fs::path result(path_arg(args, 0, "join"));
        for (size_t i = 1; i < args.size(); ++i) result /= path_arg(args, i, "join");
        return path_string(result);
    };
    builtins["path_basename"] = [](VM&, Args& args) -> Value {
        return path_string(fs::path(path_single_arg(args, "basename")).filename());
    };
    builtins["path_dirname"] = [](VM&, Args& args) -> Value {
        fs::path result = fs::path(path_single_arg(args, "dirname")).parent_path();
        if (result.empty()) result = ".";
        return path_string(result);
    };
    builtins["path_extension"] = [](VM&, Args& args) -> Value {
        return path_string(fs::path(path_single_arg(args, "extension")).extension());
    };
    builtins["path_normalize"] = [](VM&, Args& args) -> Value {
        return path_string(fs::path(path_single_arg(args, "normalize")).lexically_normal());
    };
    builtins["path_is_absolute"] = [](VM&, Args& args) -> Value {
        return Value::make_bool(fs::path(path_single_arg(args, "is_absolute")).is_absolute());
    };

    builtins["fs_exists"] = [](VM&, Args& args) -> Value {
        const auto& path = path_arg(args, "exists");
        std::error_code ec;
        const bool result = fs::exists(path, ec);
        if (ec) fail("fs::exists cannot inspect \"" + path + "\": " + ec.message());
        return Value::make_bool(result);
    };
    builtins["fs_is_file"] = [](VM&, Args& args) -> Value {
        const auto& path = path_arg(args, "is_file");
        std::error_code ec;
        const bool result = fs::is_regular_file(path, ec);
        if (ec) fail("fs::is_file cannot inspect \"" + path + "\": " + ec.message());
        return Value::make_bool(result);
    };

    builtins["fs_is_dir"] = [](VM&, Args& args) -> Value {
        const auto& path = path_arg(args, "is_dir");
        std::error_code ec;
        const bool result = fs::is_directory(path, ec);
        if (ec) fail("fs::is_dir cannot inspect \"" + path + "\": " + ec.message());
        return Value::make_bool(result);
    };
    builtins["fs_list_dir"] = builtins.at("list_dir");
    builtins["fs_create_dir"] = builtins.at("make_dir");
    builtins["fs_remove_file"] = builtins.at("remove_file");
    builtins["fs_read_file"] = builtins.at("read_file");
    builtins["fs_write_file"] = builtins.at("write_file");
    builtins["fs_append_file"] = builtins.at("append_file");
    builtins["fs_read_lines"] = builtins.at("read_lines");
}

}
