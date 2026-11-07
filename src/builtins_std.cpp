#include "builtins_std.hpp"
#include "vm.hpp"
#include "value.hpp"
#include "utf8.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace csy {

namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void need_args(const std::vector<Value>& args, size_t lo, size_t hi, const char* usage) {
    if (args.size() < lo || args.size() > hi) fail(std::string("bc::") + usage);
}

const std::string& need_str(const std::vector<Value>& args, size_t i, const char* who) {
    if (i >= args.size() || args[i].type != Value::Str)
        fail(std::string("bc::") + who + " expects a string for argument " + std::to_string(i + 1) +
             (i < args.size() ? " (got " + args[i].type_name() + ")" : ""));
    return *args[i].str_val;
}

long long need_int(const std::vector<Value>& args, size_t i, const char* who) {
    if (i >= args.size() || args[i].type != Value::Int)
        fail(std::string("bc::") + who + " expects an integer for argument " + std::to_string(i + 1) +
             (i < args.size() ? " (got " + args[i].type_name() + ")" : ""));
    return args[i].int_val;
}

std::vector<Value>& need_vec(std::vector<Value>& args, size_t i, const char* who) {
    if (i >= args.size() || args[i].type != Value::Vec)
        fail(std::string("bc::") + who + " expects a vector for argument " + std::to_string(i + 1) +
             (i < args.size() ? " (got " + args[i].type_name() + ")" : ""));
    return *args[i].vec_val;
}

double number_of(const Value& v, const char* who) {
    if (v.type == Value::Int) return static_cast<double>(v.int_val);
    if (v.type == Value::Float) return v.float_val;
    if (v.type == Value::Bool) return v.bool_val ? 1.0 : 0.0;
    fail(std::string("bc::") + who + " expects a number (got " + v.type_name() + ")");
}

std::string pad(const std::string& s, long long width, const std::string& fill, bool left, const char* who) {
    if (fill.empty() || utf8_count(fill) != 1) fail(std::string("bc::") + who + " expects a one-character fill string");
    const long long have = static_cast<long long>(utf8_count(s));
    if (width <= have) return s;
    if (width > 100000000) fail(std::string("bc::") + who + ": width is too large");
    std::string padding;
    for (long long i = have; i < width; i++) padding += fill;
    return left ? padding + s : s + padding;
}

std::string format_value(const Value& v, const std::string& spec, size_t index) {
    char align = 0;
    size_t i = 0;
    if (i < spec.size() && (spec[i] == '<' || spec[i] == '>')) align = spec[i++];
    long long width = 0;
    bool has_width = false;
    while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
        width = width * 10 + (spec[i] - '0');
        has_width = true;
        i++;
        if (width > 100000000) fail("bc::format: width is too large");
    }
    long long precision = -1;
    if (i < spec.size() && spec[i] == '.') {
        i++;
        precision = 0;
        bool any = false;
        while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
            precision = precision * 10 + (spec[i] - '0');
            any = true;
            i++;
            if (precision > 400) fail("bc::format: precision is too large");
        }
        if (!any) fail("bc::format: expected digits after '.' in the placeholder");
    }
    if (i != spec.size()) fail("bc::format: unsupported placeholder '{:" + spec + "}'");
    std::string text;
    if (precision >= 0) {
        if (v.type != Value::Int && v.type != Value::Float)
            fail("bc::format: placeholder " + std::to_string(index) + " has a precision but the value is a " + v.type_name());
        char buf[1024];
        std::snprintf(buf, sizeof buf, "%.*f", static_cast<int>(precision), number_of(v, "format"));
        text = buf;
    } else {
        text = v.to_display();
    }
    if (has_width) text = pad(text, width, " ", align != '<' && (align == '>' || v.type == Value::Int || v.type == Value::Float), "format");
    return text;
}

Value make_pair_vec(const Value& a, const Value& b) {
    std::vector<Value> pair;
    pair.reserve(2);
    pair.push_back(a);
    pair.push_back(b);
    return Value::make_vec(std::move(pair));
}

bool scalar_key(const Value& v, std::string& key) {
    if (v.type == Value::Int) { key = "i" + std::to_string(v.int_val); return true; }
    if (v.type == Value::Str) { key = "s" + *v.str_val; return true; }
    if (v.type == Value::Bool) { key = v.bool_val ? "b1" : "b0"; return true; }
    return false;
}

}

void register_std_builtins(VM& vm) {
    auto& b = vm.builtins_table();

    b["starts_with"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 2, "starts_with expects (string, prefix)");
        const std::string& s = need_str(a, 0, "starts_with");
        const std::string& p = need_str(a, 1, "starts_with");
        return Value::make_bool(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
    };
    b["ends_with"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 2, "ends_with expects (string, suffix)");
        const std::string& s = need_str(a, 0, "ends_with");
        const std::string& p = need_str(a, 1, "ends_with");
        return Value::make_bool(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
    };
    b["chars"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "chars expects (string)");
        const std::string& s = need_str(a, 0, "chars");
        std::vector<Value> out;
        for (size_t i = 0; i < s.size();) {
            const size_t n = utf8_step(s, i);
            out.push_back(Value::make_str(s.substr(i, n)));
            i += n;
        }
        return Value::make_vec(std::move(out));
    };
    b["chr"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "chr expects (code point)");
        const long long cp = need_int(a, 0, "chr");
        if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            fail("bc::chr: " + std::to_string(cp) + " is not a valid Unicode code point");
        return Value::make_str(utf8_encode(cp));
    };
    b["ord"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "ord expects (string)");
        const std::string& s = need_str(a, 0, "ord");
        if (s.empty()) fail("bc::ord expects a non-empty string");
        return Value::make_int(utf8_decode(s, 0, utf8_step(s, 0)));
    };
    b["lines"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "lines expects (string)");
        const std::string& s = need_str(a, 0, "lines");
        std::vector<Value> out;
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find('\n', start);
            const bool last = end == std::string::npos;
            if (last) end = s.size();
            size_t stop = end;
            if (stop > start && s[stop - 1] == '\r') stop--;
            out.push_back(Value::make_str(s.substr(start, stop - start)));
            start = end + 1;
            if (last) break;
        }
        return Value::make_vec(std::move(out));
    };
    b["pad_left"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 3, "pad_left expects (string, width, fill)");
        return Value::make_str(pad(need_str(a, 0, "pad_left"), need_int(a, 1, "pad_left"),
                                   a.size() > 2 ? need_str(a, 2, "pad_left") : " ", true, "pad_left"));
    };
    b["pad_right"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 3, "pad_right expects (string, width, fill)");
        return Value::make_str(pad(need_str(a, 0, "pad_right"), need_int(a, 1, "pad_right"),
                                   a.size() > 2 ? need_str(a, 2, "pad_right") : " ", false, "pad_right"));
    };
    b["format"] = [](VM&, std::vector<Value>& a) -> Value {
        if (a.empty()) fail("bc::format expects (template, values...)");
        const std::string& t = need_str(a, 0, "format");
        std::string out;
        size_t next = 1;
        for (size_t i = 0; i < t.size(); i++) {
            const char c = t[i];
            if (c == '{') {
                if (i + 1 < t.size() && t[i + 1] == '{') { out.push_back('{'); i++; continue; }
                const size_t close = t.find('}', i + 1);
                if (close == std::string::npos) fail("bc::format: a '{' has no matching '}'");
                std::string spec = t.substr(i + 1, close - i - 1);
                if (!spec.empty()) {
                    if (spec[0] != ':') fail("bc::format: unsupported placeholder '{" + spec + "}'");
                    spec.erase(0, 1);
                }
                if (next >= a.size()) fail("bc::format: the template has more placeholders than values");
                out += format_value(a[next], spec, next);
                next++;
                i = close;
            } else if (c == '}') {
                if (i + 1 < t.size() && t[i + 1] == '}') { out.push_back('}'); i++; continue; }
                fail("bc::format: a '}' has no matching '{' (write '}}' for a literal brace)");
            } else {
                out.push_back(c);
            }
        }
        if (next != a.size()) fail("bc::format: " + std::to_string(a.size() - next) + " value(s) were not used by the template");
        return Value::make_str(std::move(out));
    };

    b["pop"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 2, "pop expects (vector) or (vector, index)");
        std::vector<Value>& v = need_vec(a, 0, "pop");
        if (v.empty()) fail("bc::pop cannot take from an empty vector");
        long long i = a.size() > 1 ? need_int(a, 1, "pop") : static_cast<long long>(v.size()) - 1;
        if (i < 0) i += static_cast<long long>(v.size());
        if (i < 0 || i >= static_cast<long long>(v.size()))
            fail("bc::pop: index " + std::to_string(a.size() > 1 ? a[1].int_val : i) + " is out of bounds (size " + std::to_string(v.size()) + ")");
        Value out = v[static_cast<size_t>(i)];
        v.erase(v.begin() + i);
        return out;
    };
    b["insert"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 3, 3, "insert expects (vector, index, value)");
        std::vector<Value>& v = need_vec(a, 0, "insert");
        long long i = need_int(a, 1, "insert");
        if (i < 0) i += static_cast<long long>(v.size());
        if (i < 0 || i > static_cast<long long>(v.size()))
            fail("bc::insert: index " + std::to_string(a[1].int_val) + " is out of bounds (size " + std::to_string(v.size()) + ")");
        v.insert(v.begin() + i, a[2]);
        return Value::make_none();
    };
    b["remove"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 2, "remove expects (vector, index) or (dict, key)");
        if (a[0].type == Value::Dict) {
            const std::string& key = need_str(a, 1, "remove");
            auto it = a[0].dict_val->find(key);
            if (it == a[0].dict_val->end()) fail("bc::remove: key \"" + key + "\" is not in the dict");
            Value out = it->second;
            a[0].dict_val->erase(key);
            return out;
        }
        std::vector<Value>& v = need_vec(a, 0, "remove");
        long long i = need_int(a, 1, "remove");
        const long long original = i;
        if (i < 0) i += static_cast<long long>(v.size());
        if (i < 0 || i >= static_cast<long long>(v.size()))
            fail("bc::remove: index " + std::to_string(original) + " is out of bounds (size " + std::to_string(v.size()) + ")");
        Value out = v[static_cast<size_t>(i)];
        v.erase(v.begin() + i);
        return out;
    };
    b["index_of"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 2, "index_of expects (vector, value)");
        std::vector<Value>& v = need_vec(a, 0, "index_of");
        for (size_t i = 0; i < v.size(); i++)
            if (values_equal(v[i], a[1])) return Value::make_int(static_cast<long long>(i));
        return Value::make_int(-1);
    };
    b["any"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "any expects (vector)");
        for (const auto& x : need_vec(a, 0, "any")) if (x.is_truthy()) return Value::make_bool(true);
        return Value::make_bool(false);
    };
    b["all"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "all expects (vector)");
        for (const auto& x : need_vec(a, 0, "all")) if (!x.is_truthy()) return Value::make_bool(false);
        return Value::make_bool(true);
    };
    b["zip"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 2, 2, "zip expects (vector, vector)");
        const std::vector<Value>& x = need_vec(a, 0, "zip");
        const std::vector<Value>& y = need_vec(a, 1, "zip");
        const size_t n = std::min(x.size(), y.size());
        std::vector<Value> out;
        out.reserve(n);
        for (size_t i = 0; i < n; i++) out.push_back(make_pair_vec(x[i], y[i]));
        return Value::make_vec(std::move(out));
    };
    b["unique"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "unique expects (vector)");
        const std::vector<Value>& v = need_vec(a, 0, "unique");
        std::vector<Value> out;
        std::unordered_set<std::string> seen;
        std::vector<size_t> others;
        for (size_t i = 0; i < v.size(); i++) {
            std::string key;
            if (scalar_key(v[i], key)) {
                if (seen.insert(key).second) out.push_back(v[i]);
            } else {
                bool dup = false;
                for (size_t j : others) if (values_equal(v[j], v[i])) { dup = true; break; }
                if (!dup) { others.push_back(i); out.push_back(v[i]); }
            }
        }
        return Value::make_vec(std::move(out));
    };
    b["items"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "items expects (dict)");
        if (a[0].type != Value::Dict) fail("bc::items expects a dict (got " + a[0].type_name() + ")");
        std::vector<Value> out;
        out.reserve(a[0].dict_val->size());
        for (const auto& kv : *a[0].dict_val) out.push_back(make_pair_vec(Value::make_str(kv.first), kv.second));
        return Value::make_vec(std::move(out));
    };
    b["sort_by"] = [](VM& vm, std::vector<Value>& a) -> Value {
        need_args(a, 2, 2, "sort_by expects (vector, key function)");
        const std::vector<Value>& v = need_vec(a, 0, "sort_by");
        if (a[1].type != Value::Func || !a[1].func_val) fail("bc::sort_by expects a function as its second argument");
        std::vector<Value> keys;
        keys.reserve(v.size());
        bool numeric = true, text = true;
        for (const auto& x : v) {
            std::vector<Value> call_args{x};
            Value k = a[1].func_val->is_native ? a[1].func_val->native(vm, call_args)
                                               : vm.call_function(a[1].func_val->func_index, call_args, a[1].func_val->upvalues);
            if (k.type != Value::Int && k.type != Value::Float) numeric = false;
            if (k.type != Value::Str) text = false;
            keys.push_back(std::move(k));
        }
        if (!v.empty() && !numeric && !text) fail("bc::sort_by needs the key function to return only numbers or only strings");
        std::vector<size_t> order(v.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](size_t i, size_t j) {
            if (text && !keys.empty()) return *keys[i].str_val < *keys[j].str_val;
            const Value& p = keys[i];
            const Value& q = keys[j];
            if (p.type == Value::Int && q.type == Value::Int) return p.int_val < q.int_val;
            const double x = number_of(p, "sort_by");
            const double y = number_of(q, "sort_by");
            const bool x_nan = std::isnan(x);
            const bool y_nan = std::isnan(y);
            if (x_nan || y_nan) return !x_nan && y_nan;
            return x < y;
        });
        std::vector<Value> out;
        out.reserve(v.size());
        for (size_t i : order) out.push_back(v[i]);
        return Value::make_vec(std::move(out));
    };

    b["read_lines"] = [](VM& vm, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "read_lines expects (path)");
        const std::string& path = need_str(a, 0, "read_lines");
        std::ifstream in(path, std::ios::binary);
        if (!in) return Value::make_none();
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<Value> args{Value::make_str(std::move(content))};
        return vm.builtins_table().at("lines")(vm, args);
    };
    b["list_dir"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "list_dir expects (path)");
        const std::string& path = need_str(a, 0, "list_dir");
        std::error_code ec;
        std::vector<std::string> names;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
            names.push_back(it->path().filename().string());
        if (ec) fail("bc::list_dir cannot read \"" + path + "\": " + ec.message());
        std::sort(names.begin(), names.end());
        std::vector<Value> out;
        out.reserve(names.size());
        for (auto& n : names) out.push_back(Value::make_str(std::move(n)));
        return Value::make_vec(std::move(out));
    };
    b["is_dir"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "is_dir expects (path)");
        std::error_code ec;
        return Value::make_bool(fs::is_directory(need_str(a, 0, "is_dir"), ec));
    };
    b["make_dir"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "make_dir expects (path)");
        std::error_code ec;
        fs::create_directories(need_str(a, 0, "make_dir"), ec);
        return Value::make_bool(!ec);
    };
    b["remove_file"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "remove_file expects (path)");
        std::error_code ec;
        const std::string& path = need_str(a, 0, "remove_file");
        if (fs::is_directory(path, ec)) fail("bc::remove_file refuses to remove a directory");
        return Value::make_bool(fs::remove(path, ec) && !ec);
    };
    b["getenv"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 1, 1, "getenv expects (name)");
        const char* value = std::getenv(need_str(a, 0, "getenv").c_str());
        return value ? Value::make_str(value) : Value::make_none();
    };
    b["read_stdin"] = [](VM&, std::vector<Value>& a) -> Value {
        need_args(a, 0, 0, "read_stdin expects no arguments");
        std::ostringstream all;
        all << std::cin.rdbuf();
        return Value::make_str(all.str());
    };
}

}
