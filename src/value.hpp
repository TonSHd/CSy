#pragma once

#include "rc.hpp"
#include "ordered_map.hpp"
#include "utf8.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <cmath>
#include <new>

namespace csy {

struct Function;
struct VM;
struct Value;
using DictMap = OrderedMap<Value>;

using BuiltinFn = std::function<struct Value(VM&, std::vector<struct Value>&)>;

struct Value {
    enum Type {
        None, Bool, Int, Float, Str, Vec, F64Array, I64Array, Dict, Func, Sink
    } type = None;

    static constexpr long long SINK_OUT = 0;

    union {
        bool bool_val;
        long long int_val;
        double float_val;
        Rc<StrObj> str_val;
        Rc<std::vector<Value>> vec_val;
        Rc<std::vector<double>> f64_array_val;
        Rc<std::vector<long long>> i64_array_val;
        Rc<DictMap> dict_val;
        Rc<Function> func_val;
        unsigned long long raw_;
    };

    static bool heap_type(Type t) { return t >= Str && t <= Func; }

    Value() noexcept : raw_(0) {}
    Value(const Value& other) : type(other.type), raw_(0) {
        switch (type) {
            case Str:      new (&str_val) Rc<StrObj>(other.str_val); break;
            case Vec:      new (&vec_val) Rc<std::vector<Value>>(other.vec_val); break;
            case F64Array: new (&f64_array_val) Rc<std::vector<double>>(other.f64_array_val); break;
            case I64Array: new (&i64_array_val) Rc<std::vector<long long>>(other.i64_array_val); break;
            case Dict:     new (&dict_val) Rc<DictMap>(other.dict_val); break;
            case Func:     new (&func_val) Rc<Function>(other.func_val); break;
            default:       raw_ = other.raw_; break;
        }
    }
    Value(Value&& other) noexcept : type(other.type), raw_(other.raw_) {
        if (heap_type(type)) other.raw_ = 0;
    }
    Value& operator=(const Value& other) {
        if (this != &other) {
            Value tmp(other);
            this->~Value();
            new (this) Value(std::move(tmp));
        }
        return *this;
    }
    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            this->~Value();
            new (this) Value(std::move(other));
        }
        return *this;
    }
    ~Value() {
        switch (type) {
            case Str:      str_val.~Rc<StrObj>(); break;
            case Vec:      vec_val.~Rc<std::vector<Value>>(); break;
            case F64Array: f64_array_val.~Rc<std::vector<double>>(); break;
            case I64Array: i64_array_val.~Rc<std::vector<long long>>(); break;
            case Dict:     dict_val.~Rc<DictMap>(); break;
            case Func:     func_val.~Rc<Function>(); break;
            default: break;
        }
    }

    static Value make_none() { return Value(); }
    static Value make_bool(bool b) { Value v; v.type = Bool; v.bool_val = b; return v; }
    static Value make_int(long long i) { Value v; v.type = Int; v.int_val = i; return v; }
    static Value make_float(double d) { Value v; v.type = Float; v.float_val = d; return v; }
    static Value make_str(std::string s) {
        Value v;
        new (&v.str_val) Rc<StrObj>(make_rc<StrObj>(std::move(s)));
        v.type = Str;
        return v;
    }
    static Value make_vec(std::vector<Value> items) {
        Value v;
        new (&v.vec_val) Rc<std::vector<Value>>(make_rc<std::vector<Value>>(std::move(items)));
        v.type = Vec;
        return v;
    }
    static Value make_f64_array(std::size_t size, double initial = 0.0) {
        Value v;
        new (&v.f64_array_val) Rc<std::vector<double>>(make_rc<std::vector<double>>(size, initial));
        v.type = F64Array;
        return v;
    }
    static Value make_i64_array(std::size_t size, long long initial = 0) {
        Value v;
        new (&v.i64_array_val) Rc<std::vector<long long>>(make_rc<std::vector<long long>>(size, initial));
        v.type = I64Array;
        return v;
    }
    static Value make_dict(DictMap m) {
        Value v;
        new (&v.dict_val) Rc<DictMap>(
            make_rc<DictMap>(std::move(m)));
        v.type = Dict;
        return v;
    }
    static Value make_func(Rc<Function> f) {
        Value v;
        new (&v.func_val) Rc<Function>(std::move(f));
        v.type = Func;
        return v;
    }
    static Value make_sink(const std::string& name) {
        Value v; v.type = Sink;
        v.int_val = (name == "out") ? SINK_OUT : -1;
        return v;
    }

    bool is_truthy() const;
    std::string to_display() const;
    std::string type_name() const;
};

inline std::string display_function_name(const std::string& raw) {
    std::string name;
    name.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 4, "_x24") == 0) { name.push_back('$'); i += 4; }
        else name.push_back(raw[i++]);
    }
    if (name.compare(0, 7, "__csy_u") == 0) {
        const std::size_t us = name.find('_', 7);
        if (us != std::string::npos && us + 1 < name.size()) name = name.substr(us + 1);
    }
    if (name.size() > 2 && name[0] == '$') {
        const std::size_t sep = name.find('$', 1);
        if (sep != std::string::npos && sep + 1 < name.size() && name.compare(0, 6, "$base$") != 0 &&
            name.compare(0, 5, "$csy_") != 0)
            name = name.substr(1, sep - 1) + "::" + name.substr(sep + 1);
    }
    return name;
}

inline int compare_int_double(long long i, double d) {
    if (d != d) return 2;
    if (i >= -9007199254740992LL && i <= 9007199254740992LL) {
        const double x = static_cast<double>(i);
        return x < d ? -1 : (x > d ? 1 : 0);
    }
    if (d >= 9223372036854775808.0) return -1;
    if (d < -9223372036854775808.0) return 1;
    const double fl = std::floor(d);
    const long long fi = static_cast<long long>(fl);
    if (i < fi) return -1;
    if (i > fi) return 1;
    return d > fl ? -1 : 0;
}

inline int compare_mixed_numbers(const Value& a, const Value& b) {
    if (a.type == Value::Int && b.type == Value::Float) return compare_int_double(a.int_val, b.float_val);
    if (a.type == Value::Float && b.type == Value::Int) {
        const int c = compare_int_double(b.int_val, a.float_val);
        return c == 2 ? 2 : -c;
    }
    const double x = a.type == Value::Float ? a.float_val : static_cast<double>(a.int_val);
    const double y = b.type == Value::Float ? b.float_val : static_cast<double>(b.int_val);
    return x < y ? -1 : (x > y ? 1 : (x == y ? 0 : 2));
}

bool values_equal(const Value& a, const Value& b, int depth = 0);

struct Function {
    std::string name;
    std::vector<std::string> param_names;
    int func_index = -1;
    BuiltinFn native;
    bool is_native = false;
    Rc<std::vector<Value>> upvalues;
};

}
