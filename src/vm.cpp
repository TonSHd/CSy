#include "vm.hpp"
#include "builtins_ext.hpp"
#include "builtins_std.hpp"
#include "builtins_general.hpp"
#include "process.hpp"
#include "dataset.hpp"
#include "ai_ops.hpp"
#include "gpu_nvidia.hpp"
#include "tui.hpp"
#include "resource.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <functional>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <unistd.h>
#include <new>

namespace csy {

static std::string trim_ascii(const std::string& text) {
    size_t b = 0, e = text.size();
    while (b < e && (text[b] == ' ' || text[b] == '\t' || text[b] == '\n' || text[b] == '\r' || text[b] == '\f' || text[b] == '\v')) b++;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\n' || text[e - 1] == '\r' || text[e - 1] == '\f' || text[e - 1] == '\v')) e--;
    return text.substr(b, e - b);
}

static int parse_int_text(const std::string& text, long long& out) {
    const std::string t = trim_ascii(text);
    size_t i = 0;
    bool negative = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) { negative = t[i] == '-'; i++; }
    if (i >= t.size()) return 1;
    unsigned long long acc = 0;
    const unsigned long long limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    for (; i < t.size(); i++) {
        if (t[i] < '0' || t[i] > '9') return 1;
        const unsigned digit = static_cast<unsigned>(t[i] - '0');
        if (acc > (limit - digit) / 10) return 2;
        acc = acc * 10 + digit;
    }
    out = negative ? static_cast<long long>(0ULL - acc) : static_cast<long long>(acc);
    return 0;
}

static bool parse_float_text(const std::string& text, double& out) {
    const std::string t = trim_ascii(text);
    if (t.empty()) return false;
    std::string lowered;
    for (char c : t) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    size_t k = (lowered[0] == '+' || lowered[0] == '-') ? 1 : 0;
    const std::string rest = lowered.substr(k);
    if (rest == "nan" || rest == "inf" || rest == "infinity") {
        out = rest == "nan" ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
        if (lowered[0] == '-') out = -out;
        return true;
    }
    bool digit_seen = false;
    for (size_t i = k; i < lowered.size(); i++) {
        const char c = lowered[i];
        if (c >= '0' && c <= '9') digit_seen = true;
        else if (c != '.' && c != 'e' && c != '+' && c != '-') return false;
    }
    if (!digit_seen) return false;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

static double numeric_of(const Value& v);

static constexpr unsigned long long kMaxRuntimeStringBytes = 1ull << 30;
static constexpr unsigned long long kMaxRuntimeVecElems = 1ull << 27;

VM::VM(std::vector<std::string> program_args)
    : program_args_(std::move(program_args)) {
    constexpr size_t kStackBytesPerCall = 3072;
    constexpr size_t kNativeCallDepth = 20000;
    const size_t by_stack = res::current_stack_bytes() / kStackBytesPerCall;
    max_call_depth_ = static_cast<int>(by_stack < kNativeCallDepth ? by_stack : kNativeCallDepth);
    if (max_call_depth_ < 256) max_call_depth_ = 256;
    frames_.reserve(static_cast<size_t>(max_call_depth_) + 4);
    stdout_tty_ = ::isatty(STDOUT_FILENO) != 0;
    register_builtins();
}

void VM::register_builtins() {
    builtins_["out"] = [](VM& vm, std::vector<Value>& args) -> Value {
        for (const auto& a : args) std::cout << a.to_display();
        std::cout << "\n";
        return Value::make_none();
    };
    builtins_["range"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.size() != 1) {
            throw std::runtime_error("range expects one number: range(n) gives 0 to n - 1 (got " +
                std::to_string(args.size()) + " arguments)");
        }
        long long n = 0;
        if (args[0].type == Value::Int) n = args[0].int_val;
        else if (args[0].type == Value::Float) n = (long long)args[0].float_val;
        else throw std::runtime_error("range expects a number, got " + args[0].type_name());
        std::vector<Value> items;
        if (n < 0) n = 0;
        if (n > 50000000LL) {
            throw std::runtime_error("range(" + std::to_string(n) + ") would build a vector of " +
                std::to_string(n) + " elements; use 'for i in thru n:' (which does not allocate) "
                "or loop with a counter");
        }
        items.reserve(static_cast<size_t>(n));
        for (long long i = 0; i < n; i++) items.push_back(Value::make_int(i));
        return Value::make_vec(std::move(items));
    };
    builtins_["__range_bound"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_int(0);
        const Value& a = args[0];
        long long n = 0;
        if (a.type != Value::Int && a.type != Value::Float)
            throw std::runtime_error("'thru' needs a number of rounds, got " + a.type_name());
        if (a.type == Value::Int) n = a.int_val;
        else if (a.type == Value::Float) {
            const double d = a.float_val;
            if (!(d > 0.0)) n = 0;
            else if (d >= 9.2e18) n = std::numeric_limits<long long>::max();
            else n = static_cast<long long>(d);
        }
        return Value::make_int(n < 0 ? 0 : n);
    };
    builtins_["__iter_source"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_none();
        Value& a = args[0];
        switch (a.type) {
            case Value::Vec:
            case Value::F64Array:
            case Value::I64Array:
            case Value::Str:
                return a;
            case Value::Dict: {
                std::vector<Value> keys;
                keys.reserve(a.dict_val->size());
                for (const auto& kv : *a.dict_val) keys.push_back(Value::make_str(kv.first));
                return Value::make_vec(std::move(keys));
            }
            default:
                throw std::runtime_error("cannot loop over a " + a.type_name() +
                                         ": 'for x in ...' needs a vector, array, string or dict (use 'thru n' to count)");
        }
    };
    builtins_["f64_array"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args.size() > 2 || args[0].type != Value::Int ||
            args[0].int_val < 0) {
            throw std::runtime_error(
                "f64_array expects a non-negative integer length and optional numeric initial value");
        }
        double initial = 0.0;
        if (args.size() == 2) {
            if (args[1].type != Value::Int && args[1].type != Value::Float &&
                args[1].type != Value::Bool) {
                throw std::runtime_error("f64_array initial value must be numeric");
            }
            initial = numeric_of(args[1]);
        }
        const auto requested = static_cast<unsigned long long>(args[0].int_val);
        if (requested > std::vector<double>().max_size()) {
            throw std::runtime_error("f64_array length is too large");
        }
        const auto size = static_cast<std::size_t>(requested);
        return Value::make_f64_array(size, initial);
    };
    builtins_["i64_array"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args.size() > 2 || args[0].type != Value::Int || args[0].int_val < 0) {
            throw std::runtime_error("i64_array expects a non-negative integer length and optional integer initial value");
        }
        const auto requested = static_cast<unsigned long long>(args[0].int_val);
        if (requested > std::vector<long long>().max_size()) {
            throw std::runtime_error("i64_array length is too large");
        }
        long long initial = 0;
        if (args.size() == 2) {
            if (args[1].type != Value::Int) {
                throw std::runtime_error("i64_array initial value must be an integer");
            }
            initial = args[1].int_val;
        }
        return Value::make_i64_array(static_cast<std::size_t>(requested), initial);
    };
    builtins_["len"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_int(0);
        const auto& v = args[0];
        if (v.type == Value::Str) return Value::make_int((long long)v.str_val->char_count());
        if (v.type == Value::Vec) return Value::make_int((long long)v.vec_val->size());
        if (v.type == Value::F64Array) return Value::make_int((long long)v.f64_array_val->size());
        if (v.type == Value::I64Array) return Value::make_int((long long)v.i64_array_val->size());
        if (v.type == Value::Dict) return Value::make_int((long long)v.dict_val->size());
        return Value::make_int(0);
    };
    builtins_["push"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.size() != 2) throw std::runtime_error("bc::push expects (vector, value)");
        if (args[0].type == Value::F64Array) {
            throw std::runtime_error("bc::push cannot grow an f64_array (its size is fixed); create it with bc::f64_array(n) or use a vector");
        }
        if (args[0].type == Value::I64Array) {
            throw std::runtime_error("bc::push cannot grow an i64_array (its size is fixed); create it with bc::i64_array(n) or use a vector");
        }
        if (args[0].type != Value::Vec) {
            throw std::runtime_error("bc::push expects a vector as its first argument (got " + args[0].type_name() + ")");
        }
        args[0].vec_val->push_back(args[1]);
        return Value::make_none();
    };
    builtins_["input"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (!args.empty()) {
            std::cout << args[0].to_display();
            std::cout.flush();
        }
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) return Value::make_none();
        return Value::make_str(line);
    };
    builtins_["str"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_str("none");
        return Value::make_str(args[0].to_display());
    };
    builtins_["int"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_int(0);
        const auto& v = args[0];
        if (v.type == Value::Int) return v;
        if (v.type == Value::Float) {
            const double number = v.float_val;
            const long double wide = static_cast<long double>(number);
            if (!std::isfinite(number) ||
                wide < static_cast<long double>(std::numeric_limits<long long>::min()) ||
                wide >= -static_cast<long double>(std::numeric_limits<long long>::min()))
                throw std::runtime_error("float is outside the integer range");
            return Value::make_int(static_cast<long long>(number));
        }
        if (v.type == Value::Bool) return Value::make_int(v.bool_val ? 1 : 0);
        if (v.type == Value::Str) {
            long long parsed = 0;
            const int status = parse_int_text(*v.str_val, parsed);
            if (status == 1) throw std::runtime_error("bc::int cannot convert \"" + *v.str_val + "\" to an integer");
            if (status == 2) throw std::runtime_error("bc::int: \"" + *v.str_val + "\" is outside the integer range");
            return Value::make_int(parsed);
        }
        throw std::runtime_error("bc::int expects a number, a string or a bool (got " + v.type_name() + ")");
    };
    builtins_["float"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_float(0.0);
        const auto& v = args[0];
        if (v.type == Value::Float) return v;
        if (v.type == Value::Int) return Value::make_float((double)v.int_val);
        if (v.type == Value::Bool) return Value::make_float(v.bool_val ? 1.0 : 0.0);
        if (v.type == Value::Str) {
            double parsed = 0.0;
            if (!parse_float_text(*v.str_val, parsed))
                throw std::runtime_error("bc::float cannot convert \"" + *v.str_val + "\" to a number");
            if (std::isinf(parsed)) {
                std::string lowered = *v.str_val;
                for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (lowered.find("inf") == std::string::npos)
                    throw std::runtime_error("bc::float: \"" + *v.str_val + "\" is too big for a float");
            }
            return Value::make_float(parsed);
        }
        throw std::runtime_error("bc::float expects a number, a string or a bool (got " + v.type_name() + ")");
    };
    builtins_["bool"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_bool(false);
        return Value::make_bool(args[0].is_truthy());
    };
    builtin_names_ordered_ = {"out","range","len","push","input","str","int","float","bool","i64_array","__range_bound","__iter_source"};

    register_extended_builtins(*this);
    register_std_builtins(*this);
    register_general_builtins(*this);
    register_process_builtins(*this);
    register_dataset_builtins(*this);
    register_ai_builtins(*this);
    register_nvidia_builtins(*this);
    register_tui_builtins(*this);
}

const Value& VM::char_string(unsigned char c) {
    if (char_cache_.empty()) {
        char_cache_.reserve(256);
        for (int i = 0; i < 256; i++) char_cache_.push_back(Value::make_str(std::string(1, static_cast<char>(i))));
    }
    return char_cache_[c];
}

const Rc<Function>& VM::cached_user_func(int idx) {
    Rc<Function>& slot = user_func_cache_[idx];
    if (!slot) {
        slot = make_rc<Function>();
        slot->name = mod_->functions[idx].name;
        slot->func_index = idx;
    }
    return slot;
}

const Rc<Function>& VM::cached_builtin_func(int idx) {
    Rc<Function>& slot = builtin_func_cache_[idx];
    if (!slot) {
        const std::string& nm = mod_->builtin_names[idx];
        slot = make_rc<Function>();
        slot->name = nm;
        slot->is_native = true;
        slot->native = builtins_[nm];
    }
    return slot;
}

std::vector<Value> VM::acquire_locals(size_t n) {
    if (!locals_pool_.empty()) {
        std::vector<Value> v = std::move(locals_pool_.back());
        locals_pool_.pop_back();
        v.resize(n);
        return v;
    }
    return std::vector<Value>(n);
}

void VM::release_locals(std::vector<Value>&& v) {
    v.clear();
    locals_pool_.push_back(std::move(v));
}

Value* VM::alloc_locals(size_t n, size_t& chunk_out) {
    constexpr size_t kChunkValues = 1u << 14;
    if (locals_chunks_.empty()) {
        locals_chunks_.emplace_back();
        LocalsChunk& first = locals_chunks_.back();
        first.cap = kChunkValues;
        first.mem.reset(new Value[first.cap]());
    }
    if (locals_chunks_[locals_cur_].top + n > locals_chunks_[locals_cur_].cap) {
        const size_t next = locals_cur_ + 1;
        if (next >= locals_chunks_.size()) locals_chunks_.emplace_back();
        LocalsChunk& fresh = locals_chunks_[next];
        if (fresh.cap < n) {
            fresh.cap = n > kChunkValues ? n : kChunkValues;
            fresh.mem.reset(new Value[fresh.cap]());
            fresh.top = 0;
        }
        locals_cur_ = next;
    }
    LocalsChunk& c = locals_chunks_[locals_cur_];
    Value* p = c.mem.get() + c.top;
    c.top += n;
    chunk_out = locals_cur_;
    return p;
}

void VM::free_locals(Frame& f) {
    Value* p = f.locals;
    for (size_t i = 0; i < f.nlocals; i++) {
        Value& v = p[i];
        if (Value::heap_type(v.type)) v = Value();
        else { v.type = Value::None; v.raw_ = 0; }
    }
    LocalsChunk& c = locals_chunks_[f.chunk];
    c.top -= f.nlocals;
    locals_cur_ = f.chunk;
    if (c.top == 0 && locals_cur_ > 0) locals_cur_--;
}

void VM::pop_current_frame() {
    while (!try_handlers_.empty() && try_handlers_.back().frame_depth == frames_.size()) {
        try_handlers_.pop_back();
    }
    free_locals(frames_.back());
    frames_.pop_back();
}

Value VM::call_function(int func_index, std::vector<Value>& args, Rc<std::vector<Value>> upvalues) {
    if (!mod_ || func_index < 0 || func_index >= (int)mod_->functions.size()) {
        throw std::runtime_error("invalid function index in call_function");
    }
    IRFunction& target_fn = mod_->functions[func_index];
    Value native_result;
    if (try_call_native(target_fn, args, native_result)) {
        return native_result;
    }
    return exec_function(target_fn, std::move(args), std::move(upvalues));
}

Value VM::call_builtin(const std::string& name, std::vector<Value>& args) {
    auto it = builtins_.find(name);
    if (it == builtins_.end()) {
        throw std::runtime_error("unknown builtin '" + name + "'");
    }
    return it->second(*this, args);
}

static double numeric_of(const Value& v) {
    if (v.type == Value::Float) return v.float_val;
    if (v.type == Value::Int) return (double)v.int_val;
    if (v.type == Value::Bool) return v.bool_val ? 1.0 : 0.0;
    return 0.0;
}


template <typename Op>
static Value elementwise_op(const Value& a, const Value& b, const Op& op, int depth = 0) {
    if (depth > 256) {
        throw std::runtime_error("tensor nesting is too deep (is a vector containing itself?)");
    }
    bool av = a.type == Value::Vec, bv = b.type == Value::Vec;
    if (av && bv) {
        bool a_is_2d = !a.vec_val->empty() && (*a.vec_val)[0].type == Value::Vec;
        bool b_is_2d = !b.vec_val->empty() && (*b.vec_val)[0].type == Value::Vec;
        if (a_is_2d && !b_is_2d && (*a.vec_val)[0].vec_val->size() == b.vec_val->size()) {
            std::vector<Value> out;
            out.reserve(a.vec_val->size());
            for (const auto& row : *a.vec_val) out.push_back(elementwise_op(row, b, op, depth + 1));
            return Value::make_vec(std::move(out));
        }
        if (b_is_2d && !a_is_2d && (*b.vec_val)[0].vec_val->size() == a.vec_val->size()) {
            std::vector<Value> out;
            out.reserve(b.vec_val->size());
            for (const auto& row : *b.vec_val) out.push_back(elementwise_op(a, row, op, depth + 1));
            return Value::make_vec(std::move(out));
        }
        if (a.vec_val->size() == b.vec_val->size()) {
            std::vector<Value> out;
            out.reserve(a.vec_val->size());
            for (size_t i = 0; i < a.vec_val->size(); i++) {
                out.push_back(elementwise_op((*a.vec_val)[i], (*b.vec_val)[i], op, depth + 1));
            }
            return Value::make_vec(std::move(out));
        }
        throw std::runtime_error("shape mismatch: tensors of size " +
            std::to_string(a.vec_val->size()) + " and " + std::to_string(b.vec_val->size()) +
            " don't match and don't broadcast");
    }
    if (av) {
        double bs = numeric_of(b);
        std::vector<Value> out;
        out.reserve(a.vec_val->size());
        for (const auto& el : *a.vec_val) out.push_back(elementwise_op(el, Value::make_float(bs), op, depth + 1));
        return Value::make_vec(std::move(out));
    }
    if (bv) {
        double as = numeric_of(a);
        std::vector<Value> out;
        out.reserve(b.vec_val->size());
        for (const auto& el : *b.vec_val) out.push_back(elementwise_op(Value::make_float(as), el, op, depth + 1));
        return Value::make_vec(std::move(out));
    }
    return Value::make_float(op(numeric_of(a), numeric_of(b)));
}

template <typename Op>
static Value f64_array_op(const Value& a, const Value& b, const Op& op) {
    const bool a_array = a.type == Value::F64Array;
    const bool b_array = b.type == Value::F64Array;
    if (!a_array && !b_array) return Value::make_none();
    if (a_array && b_array && a.f64_array_val->size() != b.f64_array_val->size()) {
        throw std::runtime_error("shape mismatch: f64_arrays have different lengths");
    }
    const size_t size = a_array ? a.f64_array_val->size() : b.f64_array_val->size();
    Value out = Value::make_f64_array(size);
    for (size_t i = 0; i < size; i++) {
        const double av = a_array ? (*a.f64_array_val)[i] : numeric_of(a);
        const double bv = b_array ? (*b.f64_array_val)[i] : numeric_of(b);
        (*out.f64_array_val)[i] = op(av, bv);
    }
    return out;
}

static inline bool is_number(const Value& v) { return v.type == Value::Int || v.type == Value::Float; }

[[noreturn]] static void arithmetic_type_error(const char* action, const Value& a, const Value& b) {
    throw std::runtime_error(std::string("cannot ") + action + " " + a.type_name() + " and " + b.type_name() +
                             " (arithmetic needs two numbers)");
}

static Value add_values(const Value& a, const Value& b) {
    if (a.type == Value::Int && b.type == Value::Int) {
        if ((b.int_val > 0 && a.int_val > std::numeric_limits<long long>::max() - b.int_val) ||
            (b.int_val < 0 && a.int_val < std::numeric_limits<long long>::min() - b.int_val))
            throw std::runtime_error("integer overflow in addition");
        return Value::make_int(a.int_val + b.int_val);
    }
    if (a.type == Value::Str || b.type == Value::Str) {
        if (a.type == Value::Str && b.type == Value::Str) {
            std::string r;
            r.reserve(a.str_val->size() + b.str_val->size());
            r.append(*a.str_val);
            r.append(*b.str_val);
            return Value::make_str(std::move(r));
        }
        throw std::runtime_error(std::string("cannot add ") + a.type_name() + " and " + b.type_name() +
                                 " (+ joins two strings, wrap the other side in bc::str(...))");
    }
    if (a.type == Value::F64Array || b.type == Value::F64Array) {
        return f64_array_op(a, b, [](double x, double y){ return x + y; });
    }
    if (a.type == Value::Vec || b.type == Value::Vec) {
        return elementwise_op(a, b, [](double x, double y){ return x + y; });
    }
    if (a.type == Value::Float || b.type == Value::Float) {
        if (!is_number(a) || !is_number(b)) arithmetic_type_error("add", a, b);
        double av = (a.type == Value::Float) ? a.float_val : (double)a.int_val;
        double bv = (b.type == Value::Float) ? b.float_val : (double)b.int_val;
        return Value::make_float(av + bv);
    }
    if (a.type == Value::Bool && b.type == Value::Bool) {
        return Value::make_int((a.bool_val?1:0) + (b.bool_val?1:0));
    }
    arithmetic_type_error("add", a, b);
}

static Value sub_values(const Value& a, const Value& b) {
    if (a.type == Value::Int && b.type == Value::Int) {
        if ((b.int_val < 0 && a.int_val > std::numeric_limits<long long>::max() + b.int_val) ||
            (b.int_val > 0 && a.int_val < std::numeric_limits<long long>::min() + b.int_val))
            throw std::runtime_error("integer overflow in subtraction");
        return Value::make_int(a.int_val - b.int_val);
    }
    if (a.type == Value::F64Array || b.type == Value::F64Array) {
        return f64_array_op(a, b, [](double x, double y){ return x - y; });
    }
    if (a.type == Value::Vec || b.type == Value::Vec) {
        return elementwise_op(a, b, [](double x, double y){ return x - y; });
    }
    if (a.type == Value::Float || b.type == Value::Float) {
        if (!is_number(a) || !is_number(b)) arithmetic_type_error("subtract", a, b);
        double av = (a.type == Value::Float) ? a.float_val : (double)a.int_val;
        double bv = (b.type == Value::Float) ? b.float_val : (double)b.int_val;
        return Value::make_float(av - bv);
    }
    arithmetic_type_error("subtract", a, b);
}

static Value mul_values(const Value& a, const Value& b) {
    if (a.type == Value::Int && b.type == Value::Int) {
        const long long x = a.int_val, y = b.int_val;
        const long long hi = std::numeric_limits<long long>::max();
        const long long lo = std::numeric_limits<long long>::min();
        bool overflow = false;
        if (x > 0) overflow = y > 0 ? x > hi / y : y < lo / x;
        else if (x < 0) overflow = y > 0 ? x < lo / y : (y < 0 && x < hi / y);
        if (overflow) throw std::runtime_error("integer overflow in multiplication");
        return Value::make_int(x * y);
    }
    if (a.type == Value::Str && b.type == Value::Int) {
        std::string s;
        long long n = b.int_val;
        if (n > 0 && !a.str_val->empty() && static_cast<unsigned long long>(n) > kMaxRuntimeStringBytes / a.str_val->size())
            throw std::runtime_error("string repetition is too large (limit 1 GiB)");
        if (n > 0) s.reserve(a.str_val->size() * static_cast<size_t>(n));
        for (long long i = 0; i < n; i++) s += *a.str_val;
        return Value::make_str(std::move(s));
    }
    if (b.type == Value::Str && a.type == Value::Int) {
        std::string s;
        long long n = a.int_val;
        if (n > 0 && !b.str_val->empty() && static_cast<unsigned long long>(n) > kMaxRuntimeStringBytes / b.str_val->size())
            throw std::runtime_error("string repetition is too large (limit 1 GiB)");
        if (n > 0) s.reserve(b.str_val->size() * static_cast<size_t>(n));
        for (long long i = 0; i < n; i++) s += *b.str_val;
        return Value::make_str(std::move(s));
    }
    if (a.type == Value::F64Array || b.type == Value::F64Array) {
        return f64_array_op(a, b, [](double x, double y){ return x * y; });
    }
    if (a.type == Value::Vec && b.type == Value::Int) {
        std::vector<Value> out;
        long long n = b.int_val;
        if (n > 0) {
            if (!a.vec_val->empty() && static_cast<unsigned long long>(n) > kMaxRuntimeVecElems / a.vec_val->size())
                throw std::runtime_error("vector repetition is too large");
            out.reserve(a.vec_val->size() * (size_t)n);
            for (long long i = 0; i < n; i++) {
                for (const auto& v : *a.vec_val) out.push_back(v);
            }
        }
        return Value::make_vec(std::move(out));
    }
    if (b.type == Value::Vec && a.type == Value::Int) {
        std::vector<Value> out;
        long long n = a.int_val;
        if (n > 0) {
            if (!b.vec_val->empty() && static_cast<unsigned long long>(n) > kMaxRuntimeVecElems / b.vec_val->size())
                throw std::runtime_error("vector repetition is too large");
            out.reserve(b.vec_val->size() * (size_t)n);
            for (long long i = 0; i < n; i++) {
                for (const auto& v : *b.vec_val) out.push_back(v);
            }
        }
        return Value::make_vec(std::move(out));
    }
    if (a.type == Value::Vec || b.type == Value::Vec) {
        return elementwise_op(a, b, [](double x, double y){ return x * y; });
    }
    if (a.type == Value::Float || b.type == Value::Float) {
        if (!is_number(a) || !is_number(b)) arithmetic_type_error("multiply", a, b);
        double av = (a.type == Value::Float) ? a.float_val : (double)a.int_val;
        double bv = (b.type == Value::Float) ? b.float_val : (double)b.int_val;
        return Value::make_float(av * bv);
    }
    if (a.type == Value::Int && b.type == Value::Int) return Value::make_int(a.int_val * b.int_val);
    arithmetic_type_error("multiply", a, b);
}

static Value div_values(const Value& a, const Value& b) {
    if (a.type == Value::Int && b.type == Value::Int) {
        if (b.int_val == 0) throw std::runtime_error("division by zero");
        if (a.int_val == std::numeric_limits<long long>::min() && b.int_val == -1)
            throw std::runtime_error("integer overflow in division");
        return Value::make_int(a.int_val / b.int_val);
    }
    if (a.type == Value::F64Array || b.type == Value::F64Array) {
        return f64_array_op(a, b, [](double x, double y) -> double {
            if (y == 0.0) throw std::runtime_error("division by zero");
            return x / y;
        });
    }
    if (a.type == Value::Vec || b.type == Value::Vec) {
        return elementwise_op(a, b, [](double x, double y) -> double {
            if (y == 0.0) throw std::runtime_error("division by zero");
            return x / y;
        });
    }
    if (a.type == Value::Float || b.type == Value::Float) {
        if (!is_number(a) || !is_number(b)) arithmetic_type_error("divide", a, b);
        double av = (a.type == Value::Float) ? a.float_val : (double)a.int_val;
        double bv = (b.type == Value::Float) ? b.float_val : (double)b.int_val;
        if (bv == 0.0) throw std::runtime_error("division by zero");
        return Value::make_float(av / bv);
    }
    arithmetic_type_error("divide", a, b);
}

static Value mod_values(const Value& a, const Value& b) {
    if (a.type == Value::Int && b.type == Value::Int) {
        if (b.int_val == 0) throw std::runtime_error("modulo by zero");
        if (a.int_val == std::numeric_limits<long long>::min() && b.int_val == -1)
            return Value::make_int(0);
        return Value::make_int(a.int_val % b.int_val);
    }
    const bool a_num = a.type == Value::Int || a.type == Value::Float;
    const bool b_num = b.type == Value::Int || b.type == Value::Float;
    if (a_num && b_num) {
        const double x = a.type == Value::Float ? a.float_val : static_cast<double>(a.int_val);
        const double y = b.type == Value::Float ? b.float_val : static_cast<double>(b.int_val);
        if (y == 0.0) throw std::runtime_error("modulo by zero");
        return Value::make_float(std::fmod(x, y));
    }
    arithmetic_type_error("take the remainder of", a, b);
}

static bool eq_values(const Value& a, const Value& b) {
    return values_equal(a, b);
}

static bool lt_values(const Value& a, const Value& b) {
    if ((a.type == Value::Int || a.type == Value::Float) &&
        (b.type == Value::Int || b.type == Value::Float)) {
        return compare_mixed_numbers(a, b) == -1;
    }
    if (a.type == Value::Str && b.type == Value::Str) return *a.str_val < *b.str_val;
    return false;
}
bool VM::native_writes_arrays(const IRFunction& fn) {
    if (!mod_) return true;
    const size_t total = mod_->functions.size();
    if (array_write_cache_.size() != total) array_write_cache_.assign(total, -1);
    const size_t start = static_cast<size_t>(&fn - mod_->functions.data());
    if (start >= total) return true;
    if (array_write_cache_[start] >= 0) return array_write_cache_[start] != 0;
    std::vector<char> seen(total, 0);
    std::vector<size_t> pending{start};
    seen[start] = 1;
    bool writes = false;
    while (!pending.empty() && !writes) {
        const size_t at = pending.back();
        pending.pop_back();
        for (const Instr& ins : mod_->functions[at].code) {
            if (ins.op == Op::SET_INDEX) { writes = true; break; }
            if (ins.op == Op::PUSH_FUNC && ins.arg >= 0 && static_cast<size_t>(ins.arg) < total &&
                !seen[static_cast<size_t>(ins.arg)]) {
                seen[static_cast<size_t>(ins.arg)] = 1;
                pending.push_back(static_cast<size_t>(ins.arg));
            }
        }
    }
    array_write_cache_[start] = writes ? 1 : 0;
    return writes;
}

struct NativeFallbackContext {
    VM* vm = nullptr;
    std::string error;
};

static long long native_fallback_call(void* opaque, int function_index,
                                      const void* capture, int* error_code) {
    auto* context = static_cast<NativeFallbackContext*>(opaque);
    try {
        std::vector<Value> args{*static_cast<const Value*>(capture)};
        const Value result = context->vm->call_function(function_index, args);
        if (result.type != Value::Int)
            throw std::runtime_error("native interpreter fallback did not return an integer");
        return result.int_val;
    } catch (const std::exception& e) {
        context->error = e.what();
        if (*error_code == 0) *error_code = 11;
        return 0;
    }
}

bool VM::is_straight_line(const IRFunction& fn) {
    auto cached = straight_line_cache_.find(&fn);
    if (cached != straight_line_cache_.end()) return cached->second;
    bool straight = true;
    for (size_t i = 0; i < fn.code.size() && straight; i++) {
        const Instr& in = fn.code[i];
        switch (in.op) {
            case Op::JMP: case Op::JMPF: case Op::JMPR:
                if (in.arg >= 0 && static_cast<size_t>(in.arg) <= i) straight = false;
                break;
            case Op::CALL: case Op::CALL_NATIVE:
                straight = false;
                break;
            default:
                break;
        }
    }
    straight_line_cache_.emplace(&fn, straight);
    return straight;
}

bool VM::try_call_native(IRFunction& fn, const std::vector<Value>& args, Value& out) {
    if (fn.native_variants.empty()) return false;
    static const bool debug = std::getenv("CSY_JIT_DEBUG") != nullptr;
    if (args.size() > 16) return false;
    constexpr size_t kLargeVector = 128;
    for (const Value& a : args) {
        if (a.type == Value::Vec && a.vec_val && a.vec_val->size() >= kLargeVector && is_straight_line(fn)) {
            if (debug) fprintf(stderr, "[csy][jit-debug] '%s' has no loop or call and a large vector argument; using VM\n", fn.name.c_str());
            return false;
        }
    }
    char vector_kinds[16] = {0};
    bool any_vector = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].type != Value::Vec) continue;
        if (!args[i].vec_val || args[i].vec_val->empty()) {
            vector_kinds[i] = 'X';
            continue;
        }
        const Value::Type element_type = args[i].vec_val->front().type;
        if (element_type != Value::Int && element_type != Value::Float) {
            vector_kinds[i] = 'X';
            continue;
        }
        bool homogeneous = true;
        for (const Value& element : *args[i].vec_val)
            if (element.type != element_type) { homogeneous = false; break; }
        vector_kinds[i] = homogeneous ? (element_type == Value::Int ? 'J' : 'A') : 'X';
        if (homogeneous) any_vector = true;
    }
    const NativeVariant* variant = nullptr;
    std::vector<const Value*> global_values;
    for (const NativeVariant& v : fn.native_variants) {
        if (v.nparams != args.size() || v.global_indices.size() > 16 - args.size()) continue;
        uint32_t full_sig = 0;
        std::vector<const Value*> candidate_globals;
        bool usable = true;
        for (size_t i = 0; i < args.size(); i++) {
            uint32_t code = 0;
            if (v.opaque_mask & static_cast<uint16_t>(1u << i)) {
            } else {
                switch (args[i].type) {
                    case Value::Int: code = 1; break;
                    case Value::Float: code = 2; break;
                    case Value::F64Array:
                        if (!args[i].f64_array_val) usable = false;
                        else code = 3;
                        break;
                    case Value::I64Array:
                        if (!args[i].i64_array_val) usable = false;
                        else code = 0;
                        break;
                    case Value::Vec:
                        if (vector_kinds[i] == 'A') code = 3;
                        else if (vector_kinds[i] == 'J') code = 0;
                        else usable = false;
                        break;
                    default: usable = false; break;
                }
            }
            if (!usable) break;
            full_sig |= code << (2 * i);
        }
        if (!usable) continue;
        for (size_t i = 0; i < v.global_indices.size(); i++) {
            const int index = v.global_indices[i];
            if (index < 0 || static_cast<size_t>(index) >= mod_->global_names.size() ||
                static_cast<size_t>(index) >= globals_.size()) { usable = false; break; }
            const Value& value = globals_[static_cast<size_t>(index)];
            uint32_t code = value.type == Value::Int ? 1u : value.type == Value::Float ? 2u : 0u;
            if (!code) { usable = false; break; }
            const size_t shift = 2 * (args.size() + i);
            full_sig |= code << shift;
            candidate_globals.push_back(&value);
        }
        if (usable && v.sig == full_sig) {
            variant = &v;
            global_values = std::move(candidate_globals);
            break;
        }
    }
    if (!variant) {
        if (debug) fprintf(stderr, "[csy][jit-debug] '%s' has no native variant for this argument and module-global type combination; using VM\n", fn.name.c_str());
        return false;
    }
    for (size_t i = 0; i < args.size(); i++) {
        if ((variant->opaque_mask & static_cast<uint16_t>(1u << i)) ||
            args[i].type != Value::Vec || vector_kinds[i] == 'X') continue;
        for (size_t j = 0; j < i; j++)
            if (!(variant->opaque_mask & static_cast<uint16_t>(1u << j)) &&
                args[j].type == Value::Vec && args[i].vec_val == args[j].vec_val) return false;
    }
    NativeArg packed[16];
    std::vector<std::vector<double>> vector_floats;
    std::vector<std::vector<long long>> vector_ints;
    if (any_vector) {
        vector_floats.resize(args.size());
        vector_ints.resize(args.size());
    }
    for (size_t i = 0; i < args.size(); i++) {
        const Value& a = args[i];
        if (variant->opaque_mask & static_cast<uint16_t>(1u << i))
            packed[i] = {0.0, 0, nullptr, nullptr, 0, 0, &a, nullptr, nullptr};
        else if (a.type == Value::Int) packed[i] = {0.0, a.int_val, nullptr, nullptr, 0, 0};
        else if (a.type == Value::Float) packed[i] = {a.float_val, 0, nullptr, nullptr, 0, 0};
        else if (a.type == Value::F64Array)
            packed[i] = {0.0, 0, a.f64_array_val->data(), nullptr, static_cast<long long>(a.f64_array_val->size()), 1};
        else if (a.type == Value::I64Array)
            packed[i] = {0.0, 0, nullptr, a.i64_array_val->data(), static_cast<long long>(a.i64_array_val->size()), 2};
        else if (vector_kinds[i] == 'A') {
            auto& data = vector_floats[i];
            data.reserve(a.vec_val->size());
            for (const Value& element : *a.vec_val)
                data.push_back(element.type == Value::Int ? static_cast<double>(element.int_val) : element.float_val);
            packed[i] = {0.0, 0, data.data(), nullptr, static_cast<long long>(data.size()), 1};
        } else {
            auto& data = vector_ints[i];
            data.reserve(a.vec_val->size());
            for (const Value& element : *a.vec_val) data.push_back(element.int_val);
            packed[i] = {0.0, 0, nullptr, data.data(), static_cast<long long>(data.size()), 2};
        }
    }
    for (size_t i = 0; i < global_values.size(); i++) {
        const Value& a = *global_values[i];
        const size_t slot = args.size() + i;
        if (a.type == Value::Int) packed[slot] = {0.0, a.int_val, nullptr, nullptr, 0, 0};
        else packed[slot] = {a.float_val, 0, nullptr, nullptr, 0, 0};
    }
    NativeFallbackContext fallback_context{this, {}};
    for (size_t i = 0; i < args.size() + global_values.size(); i++) {
        packed[i].context = &fallback_context;
        packed[i].fallback = native_fallback_call;
    }
    const bool outlined_loop = fn.name.compare(0, 7, "__loop_") == 0;
    if (outlined_loop) {
        for (size_t i = 0; i < args.size(); i++) {
            if (!packed[i].is_array) continue;
            for (size_t j = i + 1; j < args.size(); j++) {
                if (!packed[j].is_array) continue;
                const uintptr_t a0 = reinterpret_cast<uintptr_t>(packed[i].is_array == 1
                    ? static_cast<void*>(packed[i].data) : static_cast<void*>(packed[i].idata));
                const uintptr_t b0 = reinterpret_cast<uintptr_t>(packed[j].is_array == 1
                    ? static_cast<void*>(packed[j].data) : static_cast<void*>(packed[j].idata));
                const size_t a_bytes = static_cast<size_t>(packed[i].length) *
                    (packed[i].is_array == 1 ? sizeof(double) : sizeof(long long));
                const size_t b_bytes = static_cast<size_t>(packed[j].length) *
                    (packed[j].is_array == 1 ? sizeof(double) : sizeof(long long));
                if (a0 < b0 + b_bytes && b0 < a0 + a_bytes) {
                    if (debug) fprintf(stderr, "[csy][jit-debug] '%s' has overlapping array arguments; using VM\n", fn.name.c_str());
                    return false;
                }
            }
        }
    }
    int error_code = 0, return_tag = 0;
    NativeRet ret;
    ret.i = 0;
    variant->entry(packed, &error_code, &return_tag, &ret);
    const int error_site = error_code >> 8;
    error_code &= 0xFF;
    if (error_code != 5 && native_writes_arrays(fn)) {
        for (size_t i = 0; i < args.size(); i++) {
            if (variant->opaque_mask & static_cast<uint16_t>(1u << i)) continue;
            if (vector_kinds[i] == 'A') {
                auto& items = *args[i].vec_val;
                for (size_t j = 0; j < items.size(); j++) items[j] = Value::make_float(vector_floats[i][j]);
            } else if (vector_kinds[i] == 'J') {
                auto& items = *args[i].vec_val;
                for (size_t j = 0; j < items.size(); j++) items[j] = Value::make_int(vector_ints[i][j]);
            }
        }
    }
    if (error_code == 5) {
        if (debug) fprintf(stderr, "[csy][jit-debug] '%s' int argument beyond 2^53 for a float parameter; using VM\n", fn.name.c_str());
        return false;
    }
    if (error_code == 11)
        throw std::runtime_error(fallback_context.error.empty()
            ? "interpreter fallback failed inside native code" : fallback_context.error);
    if (error_code != 0 && (outlined_loop || !native_writes_arrays(fn))) {
        if (debug) fprintf(stderr, "[csy][jit-debug] '%s' hit a native error (code %d); re-running in the VM\n", fn.name.c_str(), error_code);
        return false;
    }
    if (debug) fprintf(stderr, "[csy][jit-debug] '%s' called through native code\n", fn.name.c_str());
    if (error_code != 0) {
        const char* message = nullptr;
        switch (error_code) {
            case 1: message = "division by zero"; break;
            case 2:
                message = "maximum recursion depth exceeded (20000 nested calls) - this usually means a function "
                          "is calling itself without a base case that stops it";
                break;
            case 3: message = "f64_array index out of bounds"; break;
            case 13: message = "i64_array index out of bounds"; break;
            case 12: message = "modulo by zero"; break;
            case 4: message = "integer overflow in addition"; break;
            case 6: message = "integer overflow in subtraction"; break;
            case 7: message = "integer overflow in multiplication"; break;
            case 8: message = "integer overflow in negation"; break;
            case 9: message = "integer overflow in division"; break;
            case 10: message = "float is outside the integer range"; break;
            default: break;
        }
        if (message) {
            if (error_site > 0 && mod_) {
                const size_t index = static_cast<size_t>(variant->site_base) + static_cast<size_t>(error_site) - 1;
                if (index < mod_->native_sites.size()) {
                    const NativeSite& site = mod_->native_sites[index];
                    std::string where;
                    if (site.func == "<main>" || site.func.compare(0, 7, "__loop_") == 0) {
                        where = " (at top level";
                    } else {
                        const std::string name = display_function_name(site.func);
                        where = " (in function '" + name + "'";
                    }
                    if (site.line > 0) where += ", line " + std::to_string(site.line);
                    error_context_ = where + ")";
                    error_line_ = site.line;
                    error_col_ = site.col;
                    error_file_ = site.func.compare(0, 7, "__csy_u") == 0 ? site.file : std::string();
                }
            }
            throw std::runtime_error(message);
        }
    }
    if (variant->ret_bool) out = Value::make_bool(ret.i != 0);
    else if (!variant->ret_float) out = Value::make_int(ret.i);
    else if (return_tag == 1) out = Value::make_float(ret.f);
    else out = Value::make_int(static_cast<long long>(ret.f));
    return true;
}


static inline bool fast_arith(Op op, const Value& a, const Value& b, Value& out) {
    if (a.type == Value::Int && b.type == Value::Int) {
        const long long x = a.int_val, y = b.int_val;
        long long r = 0;
        switch (op) {
            case Op::ADD: if (__builtin_add_overflow(x, y, &r)) return false; break;
            case Op::SUB: if (__builtin_sub_overflow(x, y, &r)) return false; break;
            case Op::MUL: if (__builtin_mul_overflow(x, y, &r)) return false; break;
            case Op::DIV: if (y == 0 || y == -1) return false; r = x / y; break;
            case Op::MOD: if (y == 0 || y == -1) return false; r = x % y; break;
            default: return false;
        }
        out.type = Value::Int; out.int_val = r;
        return true;
    }
    if ((a.type == Value::Float || a.type == Value::Int) && (b.type == Value::Float || b.type == Value::Int) &&
        op != Op::MOD && (op == Op::ADD || op == Op::SUB || op == Op::MUL || op == Op::DIV)) {
        const double x = a.type == Value::Float ? a.float_val : (double)a.int_val;
        const double y = b.type == Value::Float ? b.float_val : (double)b.int_val;
        if (op == Op::DIV && y == 0.0) return false;
        double r = 0.0;
        switch (op) {
            case Op::ADD: r = x + y; break;
            case Op::SUB: r = x - y; break;
            case Op::MUL: r = x * y; break;
            default: r = x / y; break;
        }
        out.type = Value::Float; out.float_val = r;
        return true;
    }
    return false;
}

static inline bool fast_compare(Op op, const Value& a, const Value& b, bool& res) {
    const bool a_int = a.type == Value::Int, b_int = b.type == Value::Int;
    if (!((a_int || a.type == Value::Float) && (b_int || b.type == Value::Float))) return false;
    if (a_int && b_int) {
        const long long x = a.int_val, y = b.int_val;
        switch (op) {
            case Op::EQ: res = x == y; return true;
            case Op::NEQ: res = x != y; return true;
            case Op::LT: res = x < y; return true;
            case Op::GT: res = x > y; return true;
            case Op::LE: res = x <= y; return true;
            case Op::GE: res = x >= y; return true;
            default: return false;
        }
    }
    if (a_int != b_int) {
        const int c = compare_mixed_numbers(a, b);
        switch (op) {
            case Op::EQ: res = c == 0; return true;
            case Op::NEQ: res = c != 0; return true;
            case Op::LT: res = c == -1; return true;
            case Op::GT: res = c == 1; return true;
            case Op::LE: res = c == -1 || c == 0; return true;
            case Op::GE: res = c == 1 || c == 0; return true;
            default: return false;
        }
    }
    const double x = a.float_val;
    const double y = b.float_val;
    switch (op) {
        case Op::EQ: res = x == y; return true;
        case Op::NEQ: res = x != y; return true;
        case Op::LT: res = x < y; return true;
        case Op::GT: res = x > y; return true;
        case Op::LE: res = x <= y; return true;
        case Op::GE: res = x >= y; return true;
        default: return false;
    }
}

static inline bool fast_compare_value(Op op, const Value& a, const Value& b, Value& out) {
    bool res;
    if (!fast_compare(op, a, b, res)) return false;
    out.type = Value::Bool; out.bool_val = res;
    return true;
}

[[noreturn]] static void ordering_type_error(Op op, const Value& a, const Value& b) {
    const char* symbol = op == Op::LT ? "<" : op == Op::GT ? ">" : op == Op::LE ? "<=" : ">=";
    throw std::runtime_error("cannot compare " + a.type_name() + " with " + b.type_name() + " using '" + symbol +
                             "' (ordering needs two numbers or two strings)");
}

static inline void check_orderable(Op op, const Value& a, const Value& b) {
    if (op == Op::EQ || op == Op::NEQ) return;
    const bool numbers = (a.type == Value::Int || a.type == Value::Float) && (b.type == Value::Int || b.type == Value::Float);
    if (numbers || (a.type == Value::Str && b.type == Value::Str)) return;
    ordering_type_error(op, a, b);
}

static bool compare_slow(Op op, const Value& a, const Value& b) {
    check_orderable(op, a, b);
    switch (op) {
        case Op::EQ: return eq_values(a, b);
        case Op::NEQ: return !eq_values(a, b);
        case Op::LT: return lt_values(a, b);
        case Op::GT: return lt_values(b, a);
        case Op::LE: return lt_values(a, b) || eq_values(a, b);
        case Op::GE: return lt_values(b, a) || eq_values(a, b);
        default: return false;
    }
}

[[noreturn]] static void throw_arity_error(const IRFunction& fn, long long given) {
    size_t expected = fn.param_names.size();
    long long passed = given;
    const std::string shown = display_function_name(fn.name);
    if (shown.find("::") != std::string::npos && expected > 0 && passed > 0) {
        expected -= 1;
        passed -= 1;
    }
    throw std::runtime_error("'" + shown + "' takes " + std::to_string(expected) +
                             (expected == 1 ? " argument" : " arguments") + ", but was called with " + std::to_string(passed));
}

static std::vector<long long> quicken(IRModule& mod, size_t real_globals) {
    std::vector<long long> consts;
    std::unordered_map<long long, int> const_slot;
    for (IRFunction& fn : mod.functions) {
        const size_t n = fn.code.size();
        fn.qcode.assign(n, QInstr());
        for (size_t i = 0; i < n; i++) {
            const Instr& in = fn.code[i];
            QInstr& q = fn.qcode[i];
            q.op = in.op; q.arg = in.arg;
        }
        auto operand = [&](size_t k, uint8_t& kind, int& value, bool allow_imm) {
            if (k >= n) return false;
            const Instr& in = fn.code[k];
            if (in.op == Op::LOAD_LOCAL) { kind = 0; value = in.arg; return true; }
            if (in.op == Op::LOAD_GLOBAL) { kind = 1; value = in.arg; return true; }
            if (allow_imm && in.op == Op::PUSH_INT && in.arg >= 0 && (size_t)in.arg < mod.int_pool.size() &&
                real_globals + consts.size() < 0x3fffffff) {
                const long long v = mod.int_pool[(size_t)in.arg];
                auto it = const_slot.find(v);
                if (it == const_slot.end()) {
                    it = const_slot.emplace(v, (int)(real_globals + consts.size())).first;
                    consts.push_back(v);
                }
                kind = 1; value = it->second; return true;
            }
            return false;
        };
        std::vector<char> jump_target(n + 1, 0);
        for (size_t i = 0; i < n; i++) {
            const Op o = fn.code[i].op;
            if ((o == Op::JMP || o == Op::JMPF || o == Op::JMPR || o == Op::TRY_ENTER) &&
                fn.code[i].arg >= 0 && (size_t)fn.code[i].arg <= n)
                jump_target[(size_t)fn.code[i].arg] = 1;
        }
        auto in_is_const = [](const Instr& in) { return in.op == Op::PUSH_INT; };
        auto is_arith = [](Op o) { return o == Op::ADD || o == Op::SUB || o == Op::MUL || o == Op::DIV || o == Op::MOD; };
        auto is_cmp = [](Op o) { return o == Op::EQ || o == Op::NEQ || o == Op::LT || o == Op::GT || o == Op::LE || o == Op::GE; };
        for (size_t i = 0; i < n; i++) {
            uint8_t ak = 0, bk = 0; int av = 0, bv = 0;
            if (fn.code[i].op == Op::EQ || fn.code[i].op == Op::NEQ || fn.code[i].op == Op::LT ||
                fn.code[i].op == Op::GT || fn.code[i].op == Op::LE || fn.code[i].op == Op::GE) {
                if (i + 1 < n && fn.code[i + 1].op == Op::JMPF) {
                    QInstr& q = fn.qcode[i];
                    q.op = Op::Q_CMP_JMPF; q.sub = (uint8_t)fn.code[i].op; q.t = fn.code[i + 1].arg;
                }
                continue;
            }
            if (!operand(i, ak, av, true) || !operand(i + 1, bk, bv, true) || i + 2 >= n) continue;
            const Op third = fn.code[i + 2].op;
            QInstr& q = fn.qcode[i];
            if (third == Op::GET_INDEX) {
                if (in_is_const(fn.code[i])) continue;
                q.op = Op::Q_GETIDX; q.ak = ak; q.arg = av; q.bk = bk; q.b = bv;
                continue;
            }
            if (!is_arith(third) && !is_cmp(third)) continue;
            q.ak = ak; q.arg = av; q.bk = bk; q.b = bv; q.sub = (uint8_t)third;
            if (is_cmp(third) && i + 3 < n && fn.code[i + 3].op == Op::JMPF) {
                q.op = Op::Q_BIN_JMPF; q.t = fn.code[i + 3].arg;
            } else if (i + 3 < n && (fn.code[i + 3].op == Op::STORE_LOCAL || fn.code[i + 3].op == Op::STORE_GLOBAL)) {
                q.op = Op::Q_BIN_STORE; q.t = fn.code[i + 3].arg;
                if (fn.code[i + 3].op == Op::STORE_GLOBAL) q.sub |= 0x40;
            } else {
                q.op = Op::Q_BIN;
            }
        }
        for (size_t i = 0; i + 1 < n; i++) {
            if (fn.code[i].op == Op::PUSH_FUNC && fn.code[i + 1].op == Op::CALL && !jump_target[i + 1] &&
                fn.qcode[i].op == Op::PUSH_FUNC) {
                QInstr& q = fn.qcode[i];
                q.op = Op::Q_CALL_FUNC; q.arg = fn.code[i].arg; q.b = fn.code[i + 1].arg;
            }
        }
        for (size_t i = 0; i + 1 < n; i++) {
            if (fn.code[i].op == Op::LOAD_LOCAL && fn.code[i + 1].op == Op::RET && !jump_target[i + 1] &&
                fn.qcode[i].op == Op::LOAD_LOCAL) {
                fn.qcode[i].op = Op::Q_RET_LOCAL;
            }
        }
        for (size_t i = 0; i + 5 < n; i++) {
            const Instr* c = &fn.code[i];
            uint8_t k = 0;
            if (c[0].op == Op::LOAD_LOCAL) k = 0;
            else if (c[0].op == Op::LOAD_GLOBAL) k = 1;
            else continue;
            const Op store = k == 0 ? Op::STORE_LOCAL : Op::STORE_GLOBAL;
            if (c[1].op == c[0].op && c[2].op == Op::LT && c[3].op == Op::JMPF && c[4].op == c[0].op &&
                c[4].arg == c[0].arg && c[5].op == store) {
                QInstr& q = fn.qcode[i];
                q.op = Op::Q_FOR_HEAD; q.ak = k; q.arg = c[0].arg; q.b = c[1].arg; q.t = c[3].arg;
                continue;
            }
            if (c[1].op == Op::PUSH_INT && c[1].arg >= 0 && (size_t)c[1].arg < mod.int_pool.size() &&
                mod.int_pool[(size_t)c[1].arg] == 1 && c[2].op == Op::ADD && c[3].op == store &&
                c[3].arg == c[0].arg && c[4].op == Op::JMP) {
                QInstr& q = fn.qcode[i];
                q.op = Op::Q_INC_JMP; q.ak = k; q.arg = c[0].arg; q.t = c[4].arg;
            }
        }
    }
    return consts;
}

Value VM::exec_function(IRFunction& fn, std::vector<Value> args, Rc<std::vector<Value>> upvalues) {
    return exec_function_impl(fn, &args, 0, std::move(upvalues));
}

Value VM::exec_function_stack(IRFunction& fn, size_t nargs, Rc<std::vector<Value>> upvalues) {
    return exec_function_impl(fn, nullptr, nargs, std::move(upvalues));
}

Value VM::exec_function_impl(IRFunction& fn, std::vector<Value>* args, size_t stack_nargs,
                             Rc<std::vector<Value>> upvalues) {
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { depth++; }
        ~DepthGuard() { depth--; }
    } depth_guard(call_depth_);
    if (call_depth_ > max_call_depth_) {
        throw std::runtime_error(
            "maximum recursion depth exceeded (" + std::to_string(max_call_depth_) +
            " nested calls) - this usually means a function is calling itself "
            "without a base case that stops it");
    }

    frames_.emplace_back();
    Frame& f = frames_.back();
    f.fn = &fn;
    f.nlocals = fn.num_locals;
    f.locals = alloc_locals(f.nlocals, f.chunk);
    if (args) {
        for (size_t i = 0; i < args->size() && i < fn.param_names.size(); i++) {
            f.locals[i] = std::move((*args)[i]);
        }
        release_locals(std::move(*args));
    } else {
        const size_t params = fn.param_names.size();
        for (size_t i = stack_nargs; i-- > 0;) {
            if (i < params) f.locals[i] = std::move(stack_.back());
            stack_.pop_back();
        }
    }
    f.ip = 0;
    f.func_index = mod_ ? (int)(&fn - &mod_->functions[0]) : -1;
    f.upvalues = std::move(upvalues);

    const QInstr* const code = f.fn->qcode.data();
    const size_t code_size = f.fn->qcode.size();
    size_t ip = 0;
    Value* const locals = f.locals;
    Value* const globals = globals_.data();

    Value return_value = Value::make_none();
    for (;;) {
    try {
        while (ip < code_size) {
            const QInstr& ins = code[ip];
            ip++;

            switch (ins.op) {
                case Op::PUSH_NONE:
                    stack_.push_back(Value::make_none());
                    break;
                case Op::PUSH_BOOL:
                    stack_.push_back(Value::make_bool(ins.arg != 0));
                    break;
                case Op::PUSH_INT:
                    stack_.push_back(Value::make_int(mod_->int_pool[ins.arg]));
                    break;
                case Op::PUSH_FLOAT:
                    stack_.push_back(Value::make_float(mod_->float_pool[ins.arg]));
                    break;
                case Op::PUSH_STR: {
                    Value& cached = str_cache_[ins.arg];
                    if (cached.type != Value::Str) cached = Value::make_str(mod_->str_pool[ins.arg]);
                    stack_.push_back(cached);
                    break;
                }
                case Op::PUSH_SINK:
                    stack_.push_back(Value::make_sink(mod_->str_pool[ins.arg]));
                    break;
                case Op::PUSH_FUNC:
                    stack_.push_back(Value::make_func(cached_user_func(ins.arg)));
                    break;
                case Op::PUSH_BUILTIN:
                    stack_.push_back(Value::make_func(cached_builtin_func(ins.arg)));
                    break;
                case Op::LOAD_LOCAL:
                    stack_.push_back(f.locals[ins.arg]);
                    break;
                case Op::STORE_LOCAL:
                    f.locals[ins.arg] = std::move(stack_.back());
                    stack_.pop_back();
                    break;
                case Op::LOAD_LOCAL_BOXED: {
                    Value& box = f.locals[ins.arg];
                    if (box.type != Value::Vec || !box.vec_val || box.vec_val->empty()) {
                        throw std::runtime_error("invalid boxed local access");
                    }
                    stack_.push_back((*box.vec_val)[0]);
                    break;
                }
                case Op::STORE_LOCAL_BOXED: {
                    Value& box = f.locals[ins.arg];
                    if (box.type != Value::Vec || !box.vec_val || box.vec_val->empty()) {
                        throw std::runtime_error("invalid boxed local access");
                    }
                    (*box.vec_val)[0] = std::move(stack_.back());
                    stack_.pop_back();
                    break;
                }
                case Op::LOAD_GLOBAL:
                    stack_.push_back(globals_[ins.arg]);
                    break;
                case Op::STORE_GLOBAL:
                    globals_[ins.arg] = std::move(stack_.back());
                    stack_.pop_back();
                    break;
                case Op::LOAD_UPVALUE: {
                    if (!f.upvalues || ins.arg < 0 || ins.arg >= (int)f.upvalues->size()) {
                        throw std::runtime_error("invalid upvalue access");
                    }
                    const Value& box = (*f.upvalues)[ins.arg];
                    if (box.type != Value::Vec || !box.vec_val || box.vec_val->empty()) {
                        throw std::runtime_error("invalid upvalue access");
                    }
                    stack_.push_back((*box.vec_val)[0]);
                    break;
                }
                case Op::STORE_UPVALUE: {
                    if (!f.upvalues || ins.arg < 0 || ins.arg >= (int)f.upvalues->size()) {
                        throw std::runtime_error("invalid upvalue access");
                    }
                    Value& box = (*f.upvalues)[ins.arg];
                    if (box.type != Value::Vec || !box.vec_val || box.vec_val->empty()) {
                        throw std::runtime_error("invalid upvalue access");
                    }
                    (*box.vec_val)[0] = std::move(stack_.back());
                    stack_.pop_back();
                    break;
                }
                case Op::LOAD_UPVALUE_BOX: {
                    if (!f.upvalues || ins.arg < 0 || ins.arg >= (int)f.upvalues->size()) {
                        throw std::runtime_error("invalid upvalue access");
                    }
                    stack_.push_back((*f.upvalues)[ins.arg]);
                    break;
                }
                case Op::MAKE_CLOSURE: {
                    int target_idx = ins.arg;
                    IRFunction& target = mod_->functions[target_idx];
                    size_t n = target.upvalue_names.size();
                    auto captured = make_rc<std::vector<Value>>(std::vector<Value>(n));
                    for (size_t i = 0; i < n; i++) {
                        (*captured)[n - 1 - i] = std::move(stack_.back()); stack_.pop_back();
                    }
                    auto fv = make_rc<Function>();
                    fv->name = target.name;
                    fv->func_index = target_idx;
                    fv->upvalues = std::move(captured);
                    stack_.push_back(Value::make_func(std::move(fv)));
                    break;
                }
                case Op::PUSH_SELF: {
                    auto fv = make_rc<Function>();
                    fv->name = f.fn->name;
                    fv->func_index = f.func_index;
                    fv->upvalues = f.upvalues;
                    stack_.push_back(Value::make_func(std::move(fv)));
                    break;
                }
                case Op::TRY_ENTER: {
                    TryHandler h;
                    h.frame_depth = frames_.size();
                    h.stack_depth = stack_.size();
                    h.catch_ip = ins.arg;
                    try_handlers_.push_back(h);
                    break;
                }
                case Op::LEN: {
                    Value& v = stack_.back();
                    long long n = 0;
                    switch (v.type) {
                        case Value::Str: n = (long long)v.str_val->char_count(); break;
                        case Value::Vec: n = (long long)v.vec_val->size(); break;
                        case Value::F64Array: n = (long long)v.f64_array_val->size(); break;
                        case Value::I64Array: n = (long long)v.i64_array_val->size(); break;
                        case Value::Dict: n = (long long)v.dict_val->size(); break;
                        default: break;
                    }
                    v = Value::make_int(n);
                    break;
                }
                case Op::VEC_PUSH: {
                    Value x = std::move(stack_.back()); stack_.pop_back();
                    Value& v = stack_.back();
                    if (v.type == Value::F64Array) {
                        throw std::runtime_error("bc::push cannot grow an f64_array (its size is fixed); create it with bc::f64_array(n) or use a vector");
                    }
                    if (v.type != Value::Vec) {
                        throw std::runtime_error("bc::push expects a vector as its first argument (got " + v.type_name() + ")");
                    }
                    v.vec_val->push_back(std::move(x));
                    v = Value::make_none();
                    break;
                }
                case Op::TRY_EXIT: {
                    if (!try_handlers_.empty()) try_handlers_.pop_back();
                    break;
                }
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD: {
                    {
                        const size_t n = stack_.size();
                        Value& rb = stack_[n - 1];
                        Value& ra = stack_[n - 2];
                        if (ra.type == Value::Int && rb.type == Value::Int) {
                            const long long x = ra.int_val, y = rb.int_val;
                            long long r = 0;
                            bool ok = false;
                            switch (ins.op) {
                                case Op::ADD: ok = !__builtin_add_overflow(x, y, &r); break;
                                case Op::SUB: ok = !__builtin_sub_overflow(x, y, &r); break;
                                case Op::MUL: ok = !__builtin_mul_overflow(x, y, &r); break;
                                case Op::DIV: if (y != 0 && y != -1) { r = x / y; ok = true; } break;
                                case Op::MOD: if (y != 0 && y != -1) { r = x % y; ok = true; } break;
                                default: break;
                            }
                            if (ok) { ra.int_val = r; stack_.pop_back(); break; }
                        } else if (ins.op == Op::ADD && ra.type == Value::Str && rb.type == Value::Str &&
                                   ra.str_val.unique()) {
                            ra.str_val->append(*rb.str_val);
                            ra.str_val->changed();
                            stack_.pop_back();
                            break;
                        } else if (ins.op == Op::ADD && ra.type == Value::Str && rb.type == Value::Str &&
                                   ra.str_val.use_count() == 2 && rb.str_val != ra.str_val) {
                            const auto& next = code[ip];
                            Value* dest = nullptr;
                            if (next.op == Op::STORE_LOCAL) dest = &locals[next.arg];
                            else if (next.op == Op::STORE_GLOBAL) dest = &globals[next.arg];
                            if (dest && dest->type == Value::Str && dest->str_val == ra.str_val) {
                                Value rhs = std::move(stack_.back());
                                stack_.pop_back();
                                stack_.back() = Value::make_none();
                                dest->str_val->append(*rhs.str_val);
                                dest->str_val->changed();
                                stack_.back() = *dest;
                                break;
                            }
                            goto generic_arith;
                        } else if ((ra.type == Value::Float || ra.type == Value::Int) &&
                                   (rb.type == Value::Float || rb.type == Value::Int) &&
                                   ins.op != Op::MOD) {
                            const double x = ra.type == Value::Float ? ra.float_val : (double)ra.int_val;
                            const double y = rb.type == Value::Float ? rb.float_val : (double)rb.int_val;
                            if (ins.op != Op::DIV || y != 0.0) {
                                double r = 0.0;
                                switch (ins.op) {
                                    case Op::ADD: r = x + y; break;
                                    case Op::SUB: r = x - y; break;
                                    case Op::MUL: r = x * y; break;
                                    default: r = x / y; break;
                                }
                                ra.type = Value::Float;
                                ra.float_val = r;
                                stack_.pop_back();
                                break;
                            }
                        }
                    }
                    generic_arith:
                    Value b = std::move(stack_.back()); stack_.pop_back();
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    Value r;
                    switch (ins.op) {
                        case Op::ADD: r = add_values(a, b); break;
                        case Op::SUB: r = sub_values(a, b); break;
                        case Op::MUL: r = mul_values(a, b); break;
                        case Op::DIV: r = div_values(a, b); break;
                        case Op::MOD: r = mod_values(a, b); break;
                        default: break;
                    }
                    stack_.push_back(std::move(r));
                    break;
                }
                case Op::EQ: case Op::NEQ: case Op::LT: case Op::GT: case Op::LE: case Op::GE: {
                    {
                        const size_t n = stack_.size();
                        Value& rb = stack_[n - 1];
                        Value& ra = stack_[n - 2];
                        const bool a_int = ra.type == Value::Int, b_int = rb.type == Value::Int;
                        if ((a_int || ra.type == Value::Float) && (b_int || rb.type == Value::Float)) {
                            bool res = false;
                            if (a_int && b_int) {
                                const long long x = ra.int_val, y = rb.int_val;
                                switch (ins.op) {
                                    case Op::EQ: res = x == y; break;
                                    case Op::NEQ: res = x != y; break;
                                    case Op::LT: res = x < y; break;
                                    case Op::GT: res = x > y; break;
                                    case Op::LE: res = x <= y; break;
                                    default: res = x >= y; break;
                                }
                            } else if (a_int != b_int) {
                                const int c = compare_mixed_numbers(ra, rb);
                                switch (ins.op) {
                                    case Op::EQ: res = c == 0; break;
                                    case Op::NEQ: res = c != 0; break;
                                    case Op::LT: res = c == -1; break;
                                    case Op::GT: res = c == 1; break;
                                    case Op::LE: res = c == -1 || c == 0; break;
                                    default: res = c == 1 || c == 0; break;
                                }
                            } else {
                                const double x = ra.float_val;
                                const double y = rb.float_val;
                                switch (ins.op) {
                                    case Op::EQ: res = x == y; break;
                                    case Op::NEQ: res = x != y; break;
                                    case Op::LT: res = x < y; break;
                                    case Op::GT: res = x > y; break;
                                    case Op::LE: res = x <= y; break;
                                    default: res = x >= y; break;
                                }
                            }
                            ra.type = Value::Bool;
                            ra.bool_val = res;
                            stack_.pop_back();
                            break;
                        }
                    }
                    Value b = std::move(stack_.back()); stack_.pop_back();
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    bool r;
                    check_orderable(ins.op, a, b);
                    switch (ins.op) {
                        case Op::EQ: r = eq_values(a, b); break;
                        case Op::NEQ: r = !eq_values(a, b); break;
                        case Op::LT: r = lt_values(a, b); break;
                        case Op::GT: r = lt_values(b, a); break;
                        case Op::LE: r = lt_values(a, b) || eq_values(a, b); break;
                        case Op::GE: r = lt_values(b, a) || eq_values(a, b); break;
                        default: r = false; break;
                    }
                    stack_.push_back(Value::make_bool(r));
                    break;
                }
                case Op::AND: {
                    Value b = std::move(stack_.back()); stack_.pop_back();
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    stack_.push_back(Value::make_bool(a.is_truthy() && b.is_truthy()));
                    break;
                }
                case Op::OR: {
                    Value b = std::move(stack_.back()); stack_.pop_back();
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    stack_.push_back(Value::make_bool(a.is_truthy() || b.is_truthy()));
                    break;
                }
                case Op::NEG: {
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    if (a.type == Value::Int) {
                        if (a.int_val == std::numeric_limits<long long>::min())
                            throw std::runtime_error("integer overflow in negation");
                        stack_.push_back(Value::make_int(-a.int_val));
                    }
                    else if (a.type == Value::Float) stack_.push_back(Value::make_float(-a.float_val));
                    else throw std::runtime_error("cannot negate a " + a.type_name() + " (negation needs a number)");
                    break;
                }
                case Op::NOT: {
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    stack_.push_back(Value::make_bool(!a.is_truthy()));
                    break;
                }
                case Op::POP:
                    stack_.pop_back();
                    break;
                case Op::DUP:
                    stack_.push_back(stack_.back());
                    break;
                case Op::MAKE_VEC: {
                    int n = ins.arg;
                    std::vector<Value> items;
                    items.reserve(n);
                    for (int i = 0; i < n; i++) {
                        items.push_back(std::move(stack_.back()));
                        stack_.pop_back();
                    }
                    std::reverse(items.begin(), items.end());
                    stack_.push_back(Value::make_vec(std::move(items)));
                    break;
                }
                case Op::MAKE_DICT: {
                    int n = ins.arg;
                    DictMap m;
                    m.reserve(n);
                    std::vector<std::pair<Value, Value>> pairs;
                    pairs.reserve(n);
                    for (int i = 0; i < n; i++) {
                        Value val = std::move(stack_.back()); stack_.pop_back();
                        Value key = std::move(stack_.back()); stack_.pop_back();
                        pairs.push_back({key, val});
                    }
                    for (auto it = pairs.rbegin(); it != pairs.rend(); ++it) {
                        if (it->first.type != Value::Str) {
                            throw std::runtime_error("dict keys must be strings");
                        }
                        m[*it->first.str_val] = it->second;
                    }
                    stack_.push_back(Value::make_dict(std::move(m)));
                    break;
                }
                case Op::GET_INDEX: {
                    {
                        const size_t n = stack_.size();
                        Value& vi = stack_[n - 1];
                        Value& vo = stack_[n - 2];
                        if (vi.type == Value::Int) {
                            if (vo.type == Value::Vec && vo.vec_val) {
                                const auto& vec = *vo.vec_val;
                                long long i = vi.int_val;
                                const long long len = (long long)vec.size();
                                if (i < 0) i += len;
                                if (i >= 0 && i < len) {
                                    Value element = vec[(size_t)i];
                                    stack_.pop_back();
                                    stack_.back() = std::move(element);
                                    break;
                                }
                            } else if (vo.type == Value::F64Array && vo.f64_array_val) {
                                const auto& arr = *vo.f64_array_val;
                                long long i = vi.int_val;
                                const long long len = (long long)arr.size();
                                if (i < 0) i += len;
                                if (i >= 0 && i < len) {
                                    const double element = arr[(size_t)i];
                                    stack_.pop_back();
                                    stack_.back() = Value::make_float(element);
                                    break;
                                }
                            } else if (vo.type == Value::I64Array && vo.i64_array_val) {
                                const auto& arr = *vo.i64_array_val;
                                long long i = vi.int_val;
                                const long long len = (long long)arr.size();
                                if (i < 0) i += len;
                                if (i >= 0 && i < len) {
                                    const long long element = arr[(size_t)i];
                                    stack_.pop_back();
                                    stack_.back() = Value::make_int(element);
                                    break;
                                }
                            }
                        }
                    }
                    Value idx = std::move(stack_.back()); stack_.pop_back();
                    Value obj = std::move(stack_.back()); stack_.pop_back();
                    if (obj.type == Value::Dict) {
                        if (idx.type != Value::Str) {
                            throw std::runtime_error("dict keys must be strings");
                        }
                        auto it = obj.dict_val->find(*idx.str_val);
                        if (it == obj.dict_val->end()) {
                            throw std::runtime_error("dict has no key '" + *idx.str_val + "'");
                        }
                        stack_.push_back(it->second);
                        break;
                    }
                    if (obj.type == Value::Str) {
                        if (idx.type != Value::Int) {
                            throw std::runtime_error("string index must be an integer");
                        }
                        long long i = idx.int_val;
                        long long len = (long long)obj.str_val->char_count();
                        if (i < 0) i += len;
                        if (i < 0 || i >= len) {
                            throw std::runtime_error("string index out of bounds");
                        }
                        const StrObj& text = *obj.str_val;
                        if (text.size() == static_cast<std::size_t>(len)) {
                            stack_.push_back(char_string(static_cast<unsigned char>(text[static_cast<std::size_t>(i)])));
                        } else {
                            const std::size_t at = text.byte_offset(static_cast<std::size_t>(i));
                            const std::size_t width = text.char_bytes_at(at);
                            if (width == 1) stack_.push_back(char_string(static_cast<unsigned char>(text[at])));
                            else stack_.push_back(Value::make_str(text.substr(at, width)));
                        }
                        break;
                    }
                    if (obj.type == Value::F64Array) {
                        if (idx.type != Value::Int) {
                            throw std::runtime_error("f64_array index must be an integer");
                        }
                        long long i = idx.int_val;
                        const long long len = static_cast<long long>(obj.f64_array_val->size());
                        if (i < 0) i += len;
                        if (i < 0 || i >= len) {
                            throw std::runtime_error("f64_array index out of bounds");
                        }
                        stack_.push_back(Value::make_float((*obj.f64_array_val)[i]));
                        break;
                    }
                    if (obj.type == Value::I64Array) {
                        if (idx.type != Value::Int) {
                            throw std::runtime_error("i64_array index must be an integer");
                        }
                        long long i = idx.int_val;
                        const long long len = static_cast<long long>(obj.i64_array_val->size());
                        if (i < 0) i += len;
                        if (i < 0 || i >= len) {
                            throw std::runtime_error("i64_array index out of bounds");
                        }
                        stack_.push_back(Value::make_int((*obj.i64_array_val)[i]));
                        break;
                    }
                    if (obj.type != Value::Vec) {
                        throw std::runtime_error("cannot index into " + obj.type_name());
                    }
                    if (idx.type != Value::Int) {
                        throw std::runtime_error("vector index must be an integer");
                    }
                    long long i = idx.int_val;
                    long long len = (long long)obj.vec_val->size();
                    if (i < 0) i += len;
                    if (i < 0 || i >= len) {
                        std::stringstream ss;
                        ss << "vector index " << idx.int_val << " out of bounds (size "
                           << obj.vec_val->size() << ")";
                        throw std::runtime_error(ss.str());
                    }
                    stack_.push_back((*obj.vec_val)[i]);
                    break;
                }
                case Op::SET_INDEX: {
                    {
                        const size_t n = stack_.size();
                        Value& vv = stack_[n - 1];
                        Value& vi = stack_[n - 2];
                        Value& vo = stack_[n - 3];
                        if (vi.type == Value::Int) {
                            if (vo.type == Value::Vec && vo.vec_val) {
                                auto& vec = *vo.vec_val;
                                long long i = vi.int_val;
                                const long long len = (long long)vec.size();
                                if (i < 0) i += len;
                                if (i >= 0 && i < len) {
                                    vec[(size_t)i] = std::move(vv);
                                    stack_.pop_back(); stack_.pop_back(); stack_.pop_back();
                                    break;
                                }
                            } else if (vo.type == Value::F64Array && vo.f64_array_val &&
                                       (vv.type == Value::Float || vv.type == Value::Int)) {
                                auto& arr = *vo.f64_array_val;
                                long long i = vi.int_val;
                                const long long len = (long long)arr.size();
                                if (i < 0) i += len;
                                if (i >= 0 && i < len) {
                                    arr[(size_t)i] = vv.type == Value::Float ? vv.float_val : (double)vv.int_val;
                                    stack_.pop_back(); stack_.pop_back(); stack_.pop_back();
                                    break;
                                }
                            } else if (vo.type == Value::I64Array && vo.i64_array_val &&
                                       vv.type == Value::Int) {
                                auto& arr = *vo.i64_array_val;
                                long long i = vi.int_val;
                                const long long len = (long long)arr.size();
                                if (i < 0) i += len;
                                if (i >= 0 && i < len) {
                                    arr[(size_t)i] = vv.int_val;
                                    stack_.pop_back(); stack_.pop_back(); stack_.pop_back();
                                    break;
                                }
                            }
                        }
                    }
                    Value val = std::move(stack_.back()); stack_.pop_back();
                    Value idx = std::move(stack_.back()); stack_.pop_back();
                    Value obj = std::move(stack_.back()); stack_.pop_back();
                    if (obj.type == Value::Dict) {
                        if (idx.type != Value::Str) {
                            throw std::runtime_error("dict keys must be strings");
                        }
                        (*obj.dict_val)[*idx.str_val] = val;
                        break;
                    }
                    if (obj.type == Value::F64Array) {
                        if (idx.type != Value::Int) {
                            throw std::runtime_error("f64_array index must be an integer");
                        }
                        if (val.type != Value::Int && val.type != Value::Float && val.type != Value::Bool) {
                            throw std::runtime_error("f64_array elements must be numeric");
                        }
                        long long i = idx.int_val;
                        const long long len = static_cast<long long>(obj.f64_array_val->size());
                        if (i < 0) i += len;
                        if (i < 0 || i >= len) {
                            throw std::runtime_error("f64_array index out of bounds");
                        }
                        (*obj.f64_array_val)[i] = numeric_of(val);
                        break;
                    }
                    if (obj.type == Value::I64Array) {
                        if (idx.type != Value::Int) {
                            throw std::runtime_error("i64_array index must be an integer");
                        }
                        if (val.type != Value::Int) {
                            throw std::runtime_error("i64_array elements must be integers");
                        }
                        long long i = idx.int_val;
                        const long long len = static_cast<long long>(obj.i64_array_val->size());
                        if (i < 0) i += len;
                        if (i < 0 || i >= len) {
                            throw std::runtime_error("i64_array index out of bounds");
                        }
                        (*obj.i64_array_val)[i] = val.int_val;
                        break;
                    }
                    if (obj.type != Value::Vec) {
                        throw std::runtime_error("cannot index into " + obj.type_name());
                    }
                    if (idx.type != Value::Int) {
                        throw std::runtime_error("vector index must be an integer");
                    }
                    long long i = idx.int_val;
                    long long len = (long long)obj.vec_val->size();
                    if (i < 0) i += len;
                    if (i < 0 || i >= len) {
                        std::stringstream ss;
                        ss << "vector index " << idx.int_val << " out of bounds (size "
                           << obj.vec_val->size() << ")";
                        throw std::runtime_error(ss.str());
                    }
                    (*obj.vec_val)[i] = val;
                    break;
                }
                case Op::OUTPUT: {
                    Value val = std::move(stack_.back()); stack_.pop_back();
                    Value sink = std::move(stack_.back()); stack_.pop_back();
                    if (sink.type != Value::Sink) {
                        throw std::runtime_error("invalid output target");
                    }
                    if (sink.int_val == Value::SINK_OUT) {
                        std::cout << val.to_display();
                        if (ins.arg != 0) std::cout << "\n";
                        if (stdout_tty_) std::cout.flush();
                    } else {
                        throw std::runtime_error("unknown output sink");
                    }
                    stack_.push_back(sink);
                    break;
                }
                case Op::JMP:
                    ip = (size_t)ins.arg;
                    break;
                case Op::JMPF: {
                    const bool truthy = stack_.back().is_truthy();
                    stack_.pop_back();
                    if (!truthy) ip = (size_t)ins.arg;
                    break;
                }
                case Op::JMPR: {
                    if (stack_.back().is_truthy()) ip = (size_t)ins.arg;
                    else stack_.pop_back();
                    break;
                }
                case Op::Q_CALL_FUNC: {
                    IRFunction& target = mod_->functions[(size_t)ins.arg];
                    if (static_cast<size_t>(ins.b) != target.param_names.size()) throw_arity_error(target, ins.b);
                    if (target.native_variants.empty()) {
                        ip++;
                        Value ret = exec_function_stack(target, (size_t)ins.b, Rc<std::vector<Value>>());
                        stack_.push_back(std::move(ret));
                    } else {
                        stack_.push_back(Value::make_func(cached_user_func(ins.arg)));
                    }
                    break;
                }
                case Op::CALL: {
                    int nargs = ins.arg;
                    Value callee = std::move(stack_.back()); stack_.pop_back();
                    if (callee.type == Value::Func && callee.func_val && !callee.func_val->is_native &&
                        callee.func_val->func_index >= 0 &&
                        static_cast<size_t>(nargs) != mod_->functions[callee.func_val->func_index].param_names.size())
                        throw_arity_error(mod_->functions[callee.func_val->func_index], nargs);
                    if (callee.type == Value::Func && callee.func_val && !callee.func_val->is_native &&
                        mod_->functions[callee.func_val->func_index].native_variants.empty()) {
                        Value ret = exec_function_stack(mod_->functions[callee.func_val->func_index],
                                                        (size_t)nargs, callee.func_val->upvalues);
                        stack_.push_back(std::move(ret));
                    } else if (callee.type == Value::Func && callee.func_val) {
                        std::vector<Value> args = acquire_locals((size_t)nargs);
                        for (int i = nargs - 1; i >= 0; i--) {
                            args[i] = std::move(stack_.back()); stack_.pop_back();
                        }
                        if (callee.func_val->is_native) {
                            Value ret = callee.func_val->native(*this, args);
                            release_locals(std::move(args));
                            stack_.push_back(std::move(ret));
                        } else {
                            IRFunction& target_fn = mod_->functions[callee.func_val->func_index];
                            Value native_result;
                            if (try_call_native(target_fn, args, native_result)) {
                                release_locals(std::move(args));
                                stack_.push_back(native_result);
                            } else {
                                Value ret = exec_function(target_fn, std::move(args), callee.func_val->upvalues);
                                stack_.push_back(std::move(ret));
                            }
                        }
                    } else {
                        throw std::runtime_error("attempt to call a " + callee.type_name() +
                                                 " value as a function");
                    }
                    break;
                }
                case Op::CALL_NATIVE: {
                    int builtin_idx = ins.arg / 1000;
                    int nargs = ins.arg % 1000;
                    std::vector<Value> args = acquire_locals((size_t)nargs);
                    for (int i = nargs - 1; i >= 0; i--) {
                        args[i] = std::move(stack_.back()); stack_.pop_back();
                    }
                    const BuiltinFn* native = native_cache_[builtin_idx];
                    if (!native) {
                        const std::string& bname = mod_->builtin_names[builtin_idx];
                        auto found = builtins_.find(bname);
                        if (found == builtins_.end()) throw std::runtime_error("unknown builtin '" + bname + "'");
                        native = &found->second;
                        native_cache_[builtin_idx] = native;
                    }
                    Value ret = (*native)(*this, args);
                    release_locals(std::move(args));
                    stack_.push_back(std::move(ret));
                    break;
                }
                case Op::Q_RET_LOCAL: {
                    return_value = std::move(locals[ins.arg]);
                    pop_current_frame();
                    return return_value;
                }
                case Op::RET: {
                    return_value = std::move(stack_.back()); stack_.pop_back();
                    pop_current_frame();
                    return return_value;
                }

                case Op::Q_BIN: case Op::Q_BIN_STORE: case Op::Q_BIN_JMPF: {
                    const Value* const bases[2] = {locals, globals};
                    const Value* const pa = bases[ins.ak] + ins.arg;
                    const Value* const pb = bases[ins.bk] + ins.b;
                    const Op bop = (Op)(ins.sub & 0x3f);
                    if (ins.op == Op::Q_BIN_JMPF) {
                        bool res;
                        if (fast_compare(bop, *pa, *pb, res)) {
                            if (!res) ip = (size_t)ins.t; else ip += 3;
                            break;
                        }
                    } else {
                        Value result;
                        if (bop == Op::ADD && pa->type == Value::Str && pb->type == Value::Str) {
                            if (ins.op == Op::Q_BIN_STORE) {
                                Value& dest = (ins.sub & 0x40) ? globals[ins.t] : locals[ins.t];
                                if (&dest == pa && dest.str_val.unique()) {
                                    dest.str_val->append(*pb->str_val);
                                    dest.str_val->changed();
                                    ip += 3;
                                    break;
                                }
                            }
                            std::string r;
                            r.reserve(pa->str_val->size() + pb->str_val->size());
                            r.append(*pa->str_val);
                            r.append(*pb->str_val);
                            result = Value::make_str(std::move(r));
                            if (ins.op == Op::Q_BIN) { stack_.push_back(std::move(result)); ip += 2; }
                            else {
                                Value& dest = (ins.sub & 0x40) ? globals[ins.t] : locals[ins.t];
                                dest = std::move(result);
                                ip += 3;
                            }
                            break;
                        }
                        if (fast_arith(bop, *pa, *pb, result) || fast_compare_value(bop, *pa, *pb, result)) {
                            if (ins.op == Op::Q_BIN) {
                                stack_.push_back(std::move(result));
                                ip += 2;
                            } else {
                                Value& dest = (ins.sub & 0x40) ? globals[ins.t] : locals[ins.t];
                                dest = std::move(result);
                                ip += 3;
                            }
                            break;
                        }
                    }
                    stack_.push_back(*pa);
                    break;
                }
                case Op::Q_FOR_HEAD: {
                    Value* const base = ins.ak ? globals : locals;
                    const Value& idx = base[ins.arg];
                    const Value& bound = base[ins.b];
                    if (idx.type == Value::Int && bound.type == Value::Int) {
                        if (idx.int_val < bound.int_val) {
                            Value& var = base[code[ip + 4].arg];
                            const long long next = idx.int_val;
                            var = Value::make_int(next);
                            ip += 5;
                        } else {
                            ip = (size_t)ins.t;
                        }
                        break;
                    }
                    stack_.push_back(idx);
                    break;
                }
                case Op::Q_INC_JMP: {
                    Value& v = (ins.ak ? globals : locals)[ins.arg];
                    if (v.type == Value::Int && v.int_val != std::numeric_limits<long long>::max()) {
                        v.int_val++;
                        ip = (size_t)ins.t;
                        break;
                    }
                    stack_.push_back(v);
                    break;
                }
                case Op::Q_CMP_JMPF: {
                    const size_t n = stack_.size();
                    bool res;
                    if (fast_compare((Op)ins.sub, stack_[n - 2], stack_[n - 1], res)) {
                        stack_.pop_back(); stack_.pop_back();
                        if (!res) ip = (size_t)ins.t; else ip += 1;
                        break;
                    }
                    Value b = std::move(stack_.back()); stack_.pop_back();
                    Value a = std::move(stack_.back()); stack_.pop_back();
                    stack_.push_back(Value::make_bool(compare_slow((Op)ins.sub, a, b)));
                    break;
                }
                case Op::Q_GETIDX: {
                    const Value* const bases[2] = {locals, globals};
                    const Value* const po = bases[ins.ak] + ins.arg;
                    const Value& vi = bases[ins.bk][ins.b];
                    const bool have_index = vi.type == Value::Int;
                    long long i = vi.int_val;
                    if (have_index) {
                        if (po->type == Value::Vec && po->vec_val) {
                            const auto& vec = *po->vec_val;
                            const long long len = (long long)vec.size();
                            if (i < 0) i += len;
                            if (i >= 0 && i < len) { stack_.push_back(vec[(size_t)i]); ip += 2; break; }
                        } else if (po->type == Value::F64Array && po->f64_array_val) {
                            const auto& arr = *po->f64_array_val;
                            const long long len = (long long)arr.size();
                            if (i < 0) i += len;
                            if (i >= 0 && i < len) { stack_.push_back(Value::make_float(arr[(size_t)i])); ip += 2; break; }
                        }
                    }
                    stack_.push_back(*po);
                    break;
                }
                case Op::HALT:
                    return_value = stack_.empty() ? Value::make_none() : stack_.back();
                    pop_current_frame();
                    return return_value;
                default:
                    throw std::runtime_error("unknown opcode");
            }
        }
    } catch (const std::exception& e) {
        const bool out_of_memory = dynamic_cast<const std::bad_alloc*>(&e) != nullptr;
        while (!frames_.empty() && frames_.back().fn != &fn) pop_current_frame();
        if (frames_.empty()) throw;

        const bool handled_here = !try_handlers_.empty() && try_handlers_.back().frame_depth == frames_.size();
        const bool loop_fn = fn.name.compare(0, 7, "__loop_") == 0;
        if (handled_here) {
            error_context_.clear();
            error_trail_.clear();
            skip_trail_step_ = false;
        } else if (!error_context_.empty()) {
            if (skip_trail_step_) {
                skip_trail_step_ = false;
            } else if (error_trail_.size() < 12) {
                std::string step = "called from ";
                if (fn.name == "<main>" || loop_fn) step += "the top level";
                else step += "'" + display_function_name(fn.name) + "'";
                if (ip > 0 && ip - 1 < fn.code.size() && fn.code[ip - 1].line > 0)
                    step += " at line " + std::to_string(fn.code[ip - 1].line);
                error_trail_.push_back(step);
            }
            if (loop_fn) skip_trail_step_ = true;
        } else {
            if (loop_fn) skip_trail_step_ = true;
            std::string where;
            if (fn.name == "<main>" || fn.name.compare(0, 7, "__loop_") == 0) {
                where = " (at top level";
            } else {
                const std::string name = display_function_name(fn.name);
                where = " (in function '" + name + "'";
            }
            if (ip > 0 && ip - 1 < fn.code.size() && fn.code[ip - 1].line > 0) where += ", line " + std::to_string(fn.code[ip - 1].line);
            error_context_ = where + ")";
            error_line_ = 0;
            error_col_ = 0;
            error_file_.clear();
            if (ip > 0 && ip - 1 < fn.code.size()) {
                if (fn.name.compare(0, 7, "__csy_u") != 0) {
                    error_line_ = fn.code[ip - 1].line;
                    error_col_ = fn.code[ip - 1].col;
                } else {
                    size_t unit = 0;
                    size_t k = 7;
                    bool digits = false;
                    while (k < fn.name.size() && fn.name[k] >= '0' && fn.name[k] <= '9' && unit < 100000) {
                        unit = unit * 10 + static_cast<size_t>(fn.name[k] - '0');
                        digits = true;
                        k++;
                    }
                    if (digits && mod_ && unit < mod_->unit_files.size()) {
                        error_line_ = fn.code[ip - 1].line;
                        error_col_ = fn.code[ip - 1].col;
                        error_file_ = mod_->unit_files[unit];
                    }
                }
            }
        }
        if (handled_here) {
            TryHandler h = try_handlers_.back();
            try_handlers_.pop_back();
            stack_.resize(h.stack_depth);
            stack_.push_back(Value::make_str(out_of_memory ? "out of memory" : e.what()));
            ip = (size_t)h.catch_ip;
            continue;
        }

        pop_current_frame();
        throw;
    }
    break;
    }

    pop_current_frame();
    return return_value;
}

VMResult VM::run(IRModule& mod) {
    VMResult result;
    mod_ = &mod;
    const std::vector<long long> hidden_consts = quicken(mod, mod.global_names.size());
    globals_.assign(mod.global_names.size(), Value::make_none());
    for (long long c : hidden_consts) globals_.push_back(Value::make_int(c));
    user_func_cache_.assign(mod.functions.size(), Rc<Function>());
    builtin_func_cache_.assign(mod.builtin_names.size(), Rc<Function>());
    native_cache_.assign(mod.builtin_names.size(), nullptr);
    straight_line_cache_.clear();
    str_cache_.assign(mod.str_pool.size(), Value());
    try {
        IRFunction& main_fn = mod.functions[mod.main_func_index];
        Value ret = exec_function(main_fn, {});
        result.ok = true;
        if (ret.type == Value::Int) result.exit_code = (int)ret.int_val;
    } catch (const std::bad_alloc&) {
        result.ok = false;
        result.diags.error(mod.file, 0, 0,
            "runtime error: out of memory (the program asked for more memory than is available)");
    } catch (const std::exception& e) {
        result.ok = false;
        result.diags.error(error_file_.empty() ? mod.file : error_file_, error_line_ > 0 ? error_line_ : 0,
                           error_line_ > 0 ? error_col_ : 0, std::string("runtime error: ") + e.what() + error_context_);
        for (const auto& step : error_trail_) result.diags.trail(step);
        error_trail_.clear();
        skip_trail_step_ = false;
        error_context_.clear();
        error_line_ = 0;
        error_col_ = 0;
        error_file_.clear();
    }
    std::cout.flush();
    return result;
}

}
