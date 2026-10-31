#pragma once

#include "rc.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include <new>

namespace csy {

struct Function;
struct VM;

using BuiltinFn = std::function<struct Value(VM&, std::vector<struct Value>&)>;

struct Value {
    enum Type {
        None, Bool, Int, Float, Str, Vec, F64Array, Dict, Func, Sink
    } type = None;

    static constexpr long long SINK_OUT = 0;

    union {
        bool bool_val;
        long long int_val;
        double float_val;
        Rc<std::string> str_val;
        Rc<std::vector<Value>> vec_val;
        Rc<std::vector<double>> f64_array_val;
        Rc<std::unordered_map<std::string, Value>> dict_val;
        Rc<Function> func_val;
        unsigned long long raw_;
    };

    static bool heap_type(Type t) { return t >= Str && t <= Func; }

    Value() noexcept : raw_(0) {}
    Value(const Value& other) : type(other.type), raw_(0) {
        switch (type) {
            case Str:      new (&str_val) Rc<std::string>(other.str_val); break;
            case Vec:      new (&vec_val) Rc<std::vector<Value>>(other.vec_val); break;
            case F64Array: new (&f64_array_val) Rc<std::vector<double>>(other.f64_array_val); break;
            case Dict:     new (&dict_val) Rc<std::unordered_map<std::string, Value>>(other.dict_val); break;
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
            case Str:      str_val.~Rc<std::string>(); break;
            case Vec:      vec_val.~Rc<std::vector<Value>>(); break;
            case F64Array: f64_array_val.~Rc<std::vector<double>>(); break;
            case Dict:     dict_val.~Rc<std::unordered_map<std::string, Value>>(); break;
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
        new (&v.str_val) Rc<std::string>(make_rc<std::string>(std::move(s)));
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
    static Value make_dict(std::unordered_map<std::string, Value> m) {
        Value v;
        new (&v.dict_val) Rc<std::unordered_map<std::string, Value>>(
            make_rc<std::unordered_map<std::string, Value>>(std::move(m)));
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

struct Function {
    std::string name;
    std::vector<std::string> param_names;
    int func_index = -1;
    BuiltinFn native;
    bool is_native = false;
    Rc<std::vector<Value>> upvalues;
};

}

