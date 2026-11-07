#include "value.hpp"
#include "ir.hpp"
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <cmath>

namespace csy {

bool values_equal(const Value& a, const Value& b, int depth) {
    if (a.type != b.type) {
        if ((a.type == Value::Int || a.type == Value::Float) &&
            (b.type == Value::Int || b.type == Value::Float)) {
            return compare_mixed_numbers(a, b) == 0;
        }
        return false;
    }
    switch (a.type) {
        case Value::None: return true;
        case Value::Bool: return a.bool_val == b.bool_val;
        case Value::Int: return a.int_val == b.int_val;
        case Value::Float: return a.float_val == b.float_val;
        case Value::Str: return *a.str_val == *b.str_val;
        case Value::Vec: {
            if (a.vec_val.get() == b.vec_val.get()) return true;
            if (a.vec_val->size() != b.vec_val->size()) return false;
            if (depth > 256) throw std::runtime_error("comparison is nested too deeply (does a collection contain itself?)");
            for (size_t i = 0; i < a.vec_val->size(); i++)
                if (!values_equal((*a.vec_val)[i], (*b.vec_val)[i], depth + 1)) return false;
            return true;
        }
        case Value::F64Array: {
            if (a.f64_array_val.get() == b.f64_array_val.get()) return true;
            const auto& x = *a.f64_array_val;
            const auto& y = *b.f64_array_val;
            if (x.size() != y.size()) return false;
            for (size_t i = 0; i < x.size(); i++)
                if (!(x[i] == y[i])) return false;
            return true;
        }
        case Value::I64Array: {
            if (a.i64_array_val.get() == b.i64_array_val.get()) return true;
            return *a.i64_array_val == *b.i64_array_val;
        }
        case Value::Dict: {
            if (a.dict_val.get() == b.dict_val.get()) return true;
            if (a.dict_val->size() != b.dict_val->size()) return false;
            if (depth > 256) throw std::runtime_error("comparison is nested too deeply (does a collection contain itself?)");
            for (const auto& kv : *a.dict_val) {
                auto it = b.dict_val->find(kv.first);
                if (it == b.dict_val->end() || !values_equal(kv.second, it->second, depth + 1)) return false;
            }
            return true;
        }
        default: return false;
    }
}

bool Value::is_truthy() const {
    switch (type) {
        case None:   return false;
        case Bool:   return bool_val;
        case Int:    return int_val != 0;
        case Float:  return float_val != 0.0;
        case Str:    return str_val && !str_val->empty();
        case Vec:    return vec_val && !vec_val->empty();
        case F64Array: return f64_array_val && !f64_array_val->empty();
        case I64Array: return i64_array_val && !i64_array_val->empty();
        case Dict:   return dict_val && !dict_val->empty();
        case Func:   return true;
        case Sink:   return int_val == SINK_OUT;
    }
    return false;
}

std::string Value::type_name() const {
    switch (type) {
        case None:   return "none";
        case Bool:   return "bool";
        case Int:    return "int";
        case Float:  return "float";
        case Str:    return "str";
        case Vec:    return "vec";
        case F64Array: return "f64_array";
        case I64Array: return "i64_array";
        case Dict:   return "dict";
        case Func:   return (func_val && func_val->is_native) ? "builtin" : "func";
        case Sink:   return "sink";
    }
    return "?";
}

static constexpr int kMaxDisplayDepth = 64;

static std::string display_impl(const Value& self, int depth);

std::string Value::to_display() const { return display_impl(*this, 0); }

static std::string format_float(double value) {
    if (std::isnan(value)) return "nan";
    char buffer[128];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec != std::errc()) throw std::runtime_error("could not format float");
    return std::string(buffer, result.ptr);
}

static std::string display_impl(const Value& self, int depth) {
    using V = Value;
    std::stringstream ss;
    const V& v0 = self;
    if ((v0.type == V::Vec || v0.type == V::Dict) && depth > kMaxDisplayDepth) {
        return v0.type == V::Vec ? "[...]" : "{...}";
    }
    switch (self.type) {
        case V::None:   return "none";
        case V::Bool:   return self.bool_val ? "true" : "false";
        case V::Int:    return std::to_string(self.int_val);
        case V::Float:  return format_float(self.float_val);
        case V::Str:    return *self.str_val;
        case V::Vec: {
            ss << "[";
            if (self.vec_val) {
                for (size_t i = 0; i < self.vec_val->size(); i++) {
                    if (i) ss << ", ";
                    const auto& v = (*self.vec_val)[i];
                    if (v.type == V::Str) ss << "\"" << *v.str_val << "\"";
                    else ss << display_impl(v, depth + 1);
                }
            }
            ss << "]";
            return ss.str();
        }
        case V::F64Array: {
            ss << "[";
            if (self.f64_array_val) {
                for (size_t i = 0; i < self.f64_array_val->size(); i++) {
                    if (i) ss << ", ";
                    ss << format_float((*self.f64_array_val)[i]);
                }
            }
            ss << "]";
            return ss.str();
        }
        case V::I64Array: {
            ss << "[";
            if (self.i64_array_val) {
                for (size_t i = 0; i < self.i64_array_val->size(); i++) {
                    if (i) ss << ", ";
                    ss << (*self.i64_array_val)[i];
                }
            }
            ss << "]";
            return ss.str();
        }
        case V::Dict: {
            ss << "{";
            if (self.dict_val) {
                bool first = true;
                for (const auto& kv : *self.dict_val) {
                    if (!first) ss << ", ";
                    first = false;
                    ss << "\"" << kv.first << "\": ";
                    if (kv.second.type == V::Str) ss << "\"" << *kv.second.str_val << "\"";
                    else ss << display_impl(kv.second, depth + 1);
                }
            }
            ss << "}";
            return ss.str();
        }
        case V::Func:
            if (self.func_val && self.func_val->is_native) return "<builtin " + self.func_val->name + ">";
            return "<func " + (self.func_val ? display_function_name(self.func_val->name) : std::string("?")) + ">";
        case V::Sink:   return (self.int_val == V::SINK_OUT) ? "<sink out>" : "<sink ?>";
    }
    return "?";
}

}
