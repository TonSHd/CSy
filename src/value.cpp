#include "value.hpp"
#include "ir.hpp"
#include <sstream>
#include <cmath>

namespace csy {

bool Value::is_truthy() const {
    switch (type) {
        case None:   return false;
        case Bool:   return bool_val;
        case Int:    return int_val != 0;
        case Float:  return float_val != 0.0;
        case Str:    return str_val && !str_val->empty();
        case Vec:    return vec_val && !vec_val->empty();
        case F64Array: return f64_array_val && !f64_array_val->empty();
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
        case Dict:   return "dict";
        case Func:   return (func_val && func_val->is_native) ? "builtin" : "func";
        case Sink:   return "sink";
    }
    return "?";
}

static constexpr int kMaxDisplayDepth = 64;

static std::string display_impl(const Value& self, int depth);

std::string Value::to_display() const { return display_impl(*this, 0); }

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
        case V::Float:
            ss << self.float_val;
            return ss.str();
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
                    ss << (*self.f64_array_val)[i];
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
            return "<func " + (self.func_val ? self.func_val->name : "?") + ">";
        case V::Sink:   return (self.int_val == V::SINK_OUT) ? "<sink out>" : "<sink ?>";
    }
    return "?";
}

}

