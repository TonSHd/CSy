#pragma once

#include "ir.hpp"
#include "value.hpp"
#include "diagnostic.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace csy {

struct VMResult {
    DiagBag diags;
    bool ok = false;
    int exit_code = 0;
};

class VM {
public:
    explicit VM(std::vector<std::string> program_args = {});

    VMResult run(IRModule& mod);

    std::vector<Value>& stack() { return stack_; }
    IRModule* module() { return mod_; }
    const std::vector<std::string>& program_args() const { return program_args_; }

    std::unordered_map<std::string, BuiltinFn>& builtins_table() { return builtins_; }
    const Value& char_string(unsigned char c);
    Value call_function(int func_index, std::vector<Value>& args,
                         Rc<std::vector<Value>> upvalues = Rc<std::vector<Value>>());

private:
    IRModule* mod_ = nullptr;
    std::vector<Value> stack_;
    std::vector<std::string> program_args_;
    std::vector<Value> globals_;
    std::unordered_map<std::string, BuiltinFn> builtins_;
    std::vector<std::string> builtin_names_ordered_;

    struct Frame {
        IRFunction* fn = nullptr;
        Value* locals = nullptr;
        size_t nlocals = 0;
        size_t chunk = 0;
        size_t ip = 0;
        int func_index = -1;
        Rc<std::vector<Value>> upvalues;
    };
    struct LocalsChunk {
        std::unique_ptr<Value[]> mem;
        size_t cap = 0;
        size_t top = 0;
    };
    std::vector<LocalsChunk> locals_chunks_;
    size_t locals_cur_ = 0;
    std::unordered_map<const IRFunction*, bool> straight_line_cache_;
    bool is_straight_line(const IRFunction& fn);
    Value* alloc_locals(size_t n, size_t& chunk_out);
    void free_locals(Frame& f);
    std::vector<std::string> error_trail_;
    bool skip_trail_step_ = false;
    std::vector<Frame> frames_;
    int call_depth_ = 0;
    int max_call_depth_ = 3000;

    struct TryHandler {
        size_t frame_depth;
        size_t stack_depth;
        int catch_ip;
    };
    std::vector<TryHandler> try_handlers_;
    std::vector<const BuiltinFn*> native_cache_;
    std::vector<Value> str_cache_;
    std::vector<Value> char_cache_;
    std::string error_context_;
    int error_line_ = 0;
    int error_col_ = 0;
    std::vector<signed char> array_write_cache_;
    bool native_writes_arrays(const IRFunction& fn);
    std::string error_file_;
    bool stdout_tty_ = false;
    std::vector<Rc<Function>> user_func_cache_;
    std::vector<Rc<Function>> builtin_func_cache_;
    const Rc<Function>& cached_user_func(int idx);
    const Rc<Function>& cached_builtin_func(int idx);
    std::vector<std::vector<Value>> locals_pool_;
    std::vector<Value> acquire_locals(size_t n);
    void release_locals(std::vector<Value>&& v);
    void pop_current_frame();
    Value exec_function(IRFunction& fn, std::vector<Value> args,
                         Rc<std::vector<Value>> upvalues = Rc<std::vector<Value>>());
    Value exec_function_stack(IRFunction& fn, size_t nargs, Rc<std::vector<Value>> upvalues);
    Value exec_function_impl(IRFunction& fn, std::vector<Value>* args, size_t stack_nargs,
                             Rc<std::vector<Value>> upvalues);
    Value call_builtin(const std::string& name, std::vector<Value>& args);
    bool try_call_native(IRFunction& fn, const std::vector<Value>& args, Value& out);

    void register_builtins();
};

}

