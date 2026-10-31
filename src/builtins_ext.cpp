#include "builtins_ext.hpp"
#include "pi.hpp"
#include "vm.hpp"
#include "value.hpp"
#include "dataio.hpp"
#include "fasthash.hpp"

#include <cmath>
#include <random>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <filesystem>
#include <cctype>
#include <iostream>


namespace csy {

static bool eq_values_local(const Value& a, const Value& b);

static double to_double(const Value& v) {
    if (v.type == Value::Float) return v.float_val;
    if (v.type == Value::Int) return (double)v.int_val;
    if (v.type == Value::Bool) return v.bool_val ? 1.0 : 0.0;
    return 0.0;
}

static long long to_int(const Value& v) {
    if (v.type == Value::Int) return v.int_val;
    if (v.type == Value::Float) {
        const double number = v.float_val;
        const long double wide = static_cast<long double>(number);
        if (!std::isfinite(number) ||
            wide < static_cast<long double>(std::numeric_limits<long long>::min()) ||
            wide >= -static_cast<long double>(std::numeric_limits<long long>::min()))
            throw std::runtime_error("float is outside the integer range");
        return static_cast<long long>(number);
    }
    if (v.type == Value::Bool) return v.bool_val ? 1 : 0;
    if (v.type == Value::Str) {
        try { return std::stoll(*v.str_val); } catch (...) { return 0; }
    }
    return 0;
}

static constexpr long long kMaxTensorElems = 1LL << 27;
static constexpr unsigned long long kMaxFileReadBytes = 1ULL << 30;

static void require_tensor_size(long long rows, long long cols, const char* what) {
    if (rows > 0 && cols > 0 && rows > kMaxTensorElems / cols) {
        throw std::runtime_error(std::string(what) + ": " + std::to_string(rows) + " x " +
            std::to_string(cols) + " elements is too large (limit " +
            std::to_string(kMaxTensorElems) + " elements)");
    }
}

static Value apply_elementwise(const Value& v, double (*fn)(double), const std::string& name, int depth = 0) {
    if (depth > 256) {
        throw std::runtime_error("tensor nesting is too deep (is a vector containing itself?)");
    }
    if (v.type == Value::Vec) {
        std::vector<Value> out;
        out.reserve(v.vec_val->size());
        for (const auto& el : *v.vec_val) out.push_back(apply_elementwise(el, fn, name, depth + 1));
        return Value::make_vec(std::move(out));
    }
    if (v.type == Value::F64Array) {
        const std::vector<double>& src = *v.f64_array_val;
        Value out = Value::make_f64_array(src.size());
        std::vector<double>& dst = *out.f64_array_val;
        for (size_t i = 0; i < src.size(); i++) dst[i] = fn(src[i]);
        return out;
    }
    if (v.type == Value::Int || v.type == Value::Float || v.type == Value::Bool) {
        return Value::make_float(fn(to_double(v)));
    }
    throw std::runtime_error("bc::" + name + " expects a number, a vector or an f64_array (got " + v.type_name() + ")");
}

static std::mt19937_64& rng() {
    static thread_local std::mt19937_64 gen{
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()
    };
    return gen;
}

static bool is_tensor_2d(const Value& v) {
    if (v.type != Value::Vec || v.vec_val->empty()) return false;
    if ((*v.vec_val)[0].type != Value::Vec) return false;
    size_t cols = (*v.vec_val)[0].vec_val->size();
    if (cols == 0) return false;
    for (const auto& row : *v.vec_val) {
        if (row.type != Value::Vec || row.vec_val->size() != cols) return false;
    }
    return true;
}

static Value call_callable(VM& vm, const Value& fn_val, std::vector<Value> call_args) {
    if (fn_val.type != Value::Func || !fn_val.func_val) {
        throw std::runtime_error("value is not callable");
    }
    if (fn_val.func_val->is_native) {
        return fn_val.func_val->native(vm, call_args);
    }
    return vm.call_function(fn_val.func_val->func_index, call_args, fn_val.func_val->upvalues);
}

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the f64 checkpoint format stores little-endian doubles directly");

static constexpr unsigned char kF64Magic[8] = {'C', 'S', 'Y', 'F', '6', '4', 0x00, 0x02};
static constexpr size_t kF64HeaderBytes = 16;
static constexpr size_t kF64TrailerBytes = 8;

static void save_f64_file(const std::string& path, const std::vector<double>& values) {
    if (values.size() > static_cast<size_t>(kMaxTensorElems)) {
        throw std::runtime_error("save_f64: " + std::to_string(values.size()) + " values is above the limit of " +
                                 std::to_string(kMaxTensorElems));
    }
    const uint64_t count = values.size();
    const size_t payload = values.size() * sizeof(double);
    dataio::AtomicWriter writer(path, "save_f64");
    unsigned char head[kF64HeaderBytes];
    std::memcpy(head, kF64Magic, 8);
    std::memcpy(head + 8, &count, 8);
    writer.write(reinterpret_cast<const char*>(head), kF64HeaderBytes);
    if (payload > 0) writer.write(reinterpret_cast<const char*>(values.data()), payload);
    const uint64_t sum = hash64_fast(values.data(), payload, count);
    unsigned char tail[kF64TrailerBytes];
    std::memcpy(tail, &sum, 8);
    writer.write(reinterpret_cast<const char*>(tail), kF64TrailerBytes);
    writer.commit();
}

static Value load_f64_text(std::ifstream& in, uintmax_t checkpoint_bytes) {
    constexpr unsigned long long max_values = 16ULL * 1024 * 1024;
    constexpr uintmax_t max_text_bytes = 128ULL * 1024 * 1024;
    if (checkpoint_bytes > max_text_bytes) {
        throw std::runtime_error("load_f64: text checkpoint exceeds the 128 MiB limit; save it again to convert it to the binary format");
    }
    std::string magic;
    int version = 0;
    unsigned long long count = 0;
    if (!(in >> magic >> version >> count) || magic != "CSYF64" || version != 1) {
        throw std::runtime_error("load_f64: not a CSy f64 checkpoint (unrecognized header)");
    }
    if (count > std::vector<double>().max_size() || count > max_values || count > checkpoint_bytes) {
        throw std::runtime_error("load_f64: checkpoint is too large");
    }
    Value result = Value::make_f64_array(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        double value = 0.0;
        if (!(in >> value)) throw std::runtime_error("load_f64: truncated or malformed text checkpoint data");
        (*result.f64_array_val)[i] = value;
    }
    std::string extra;
    if (in >> extra) throw std::runtime_error("load_f64: unexpected data after the checkpoint values");
    return result;
}

static Value load_f64_file(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        throw std::runtime_error("load_f64: '" + path + "' is a directory, not a file");
    }
    const uintmax_t bytes = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("load_f64: cannot open '" + path + "' (no such file or unreadable)");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("load_f64: cannot open '" + path + "'");

    unsigned char head[kF64HeaderBytes] = {};
    in.read(reinterpret_cast<char*>(head), kF64HeaderBytes);
    const bool is_binary = in.gcount() >= 8 && std::memcmp(head, kF64Magic, 8) == 0;
    if (!is_binary) {
        in.clear();
        in.seekg(0);
        return load_f64_text(in, bytes);
    }

    if (in.gcount() != static_cast<std::streamsize>(kF64HeaderBytes) || bytes < kF64HeaderBytes + kF64TrailerBytes) {
        throw std::runtime_error("load_f64: '" + path + "' is truncated");
    }
    uint64_t count = 0;
    std::memcpy(&count, head + 8, 8);
    if (count > static_cast<uint64_t>(kMaxTensorElems)) {
        throw std::runtime_error("load_f64: checkpoint declares " + std::to_string(count) +
                                 " values, above the limit of " + std::to_string(kMaxTensorElems));
    }
    const uintmax_t expected = kF64HeaderBytes + static_cast<uintmax_t>(count) * sizeof(double) + kF64TrailerBytes;
    if (bytes != expected) {
        throw std::runtime_error("load_f64: the size of '" + path + "' does not match its header, the file is truncated or corrupt");
    }
    Value result = Value::make_f64_array(static_cast<size_t>(count));
    std::vector<double>& data = *result.f64_array_val;
    const size_t payload = data.size() * sizeof(double);
    if (payload > 0) in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(payload));
    unsigned char tail[kF64TrailerBytes];
    in.read(reinterpret_cast<char*>(tail), kF64TrailerBytes);
    if (!in) throw std::runtime_error("load_f64: reading '" + path + "' failed");
    uint64_t want = 0;
    std::memcpy(&want, tail, 8);
    if (hash64_fast(data.data(), payload, count) != want) {
        throw std::runtime_error("load_f64: the checksum does not match, '" + path + "' is corrupt");
    }
    return result;
}

static double variance_of(const std::string& name, const std::vector<Value>& args) {
    if (args.empty() || args.size() > 2) {
        throw std::runtime_error("bc::" + name + " expects (values) or (values, ddof)");
    }
    long long ddof = 0;
    if (args.size() == 2) {
        if (args[1].type != Value::Int || args[1].int_val < 0) {
            throw std::runtime_error("bc::" + name + ": ddof must be a non-negative integer");
        }
        ddof = args[1].int_val;
    }
    const Value& v = args[0];
    const std::vector<double>* packed = nullptr;
    const std::vector<Value>* items = nullptr;
    if (v.type == Value::F64Array) packed = v.f64_array_val.get();
    else if (v.type == Value::Vec) items = v.vec_val.get();
    else throw std::runtime_error("bc::" + name + " expects a vector or an f64_array (got " + v.type_name() + ")");
    const size_t n = packed ? packed->size() : items->size();
    if (n == 0) throw std::runtime_error("bc::" + name + " of an empty collection");
    if (static_cast<unsigned long long>(n) <= static_cast<unsigned long long>(ddof)) {
        throw std::runtime_error("bc::" + name + " needs more than " + std::to_string(ddof) + " values");
    }
    auto at = [&](size_t i) -> double {
        if (packed) return (*packed)[i];
        const Value& e = (*items)[i];
        if (e.type != Value::Int && e.type != Value::Float && e.type != Value::Bool) {
            throw std::runtime_error("bc::" + name + " expects numbers (got " + e.type_name() + ")");
        }
        return to_double(e);
    };
    long double sum = 0.0L;
    for (size_t i = 0; i < n; i++) sum += at(i);
    const long double mean = sum / static_cast<long double>(n);
    long double squares = 0.0L;
    for (size_t i = 0; i < n; i++) {
        const long double d = static_cast<long double>(at(i)) - mean;
        squares += d * d;
    }
    return static_cast<double>(squares / static_cast<long double>(n - static_cast<size_t>(ddof)));
}

static Value deep_copy_value(const Value& v, int depth) {
    if (depth > 256) {
        throw std::runtime_error("bc::copy: nesting is too deep (is a collection containing itself?)");
    }
    switch (v.type) {
        case Value::Vec: {
            std::vector<Value> items;
            items.reserve(v.vec_val->size());
            for (const Value& e : *v.vec_val) items.push_back(deep_copy_value(e, depth + 1));
            return Value::make_vec(std::move(items));
        }
        case Value::F64Array: {
            const std::vector<double>& src = *v.f64_array_val;
            Value out = Value::make_f64_array(0);
            out.f64_array_val->assign(src.begin(), src.end());
            return out;
        }
        case Value::Dict: {
            std::unordered_map<std::string, Value> copy;
            copy.reserve(v.dict_val->size());
            for (const auto& kv : *v.dict_val) copy.emplace(kv.first, deep_copy_value(kv.second, depth + 1));
            return Value::make_dict(std::move(copy));
        }
        default:
            return v;
    }
}

void register_extended_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();

    auto add_math1 = [&](const std::string& name, double (*fn)(double)) {
        builtins[name] = [fn, name](VM&, std::vector<Value>& args) -> Value {
            if (args.empty()) throw std::runtime_error("bc::" + name + " expects a number, a vector or an f64_array");
            return apply_elementwise(args[0], fn, name);
        };
    };
    add_math1("sin",   std::sin);
    add_math1("cos",   std::cos);
    add_math1("tan",   std::tan);
    add_math1("asin",  std::asin);
    add_math1("acos",  std::acos);
    add_math1("atan",  std::atan);
    add_math1("sqrt",  std::sqrt);
    add_math1("exp",   std::exp);
    add_math1("log",   std::log);
    add_math1("log2",  std::log2);
    add_math1("log10", std::log10);
    add_math1("floor", std::floor);
    add_math1("ceil",  std::ceil);
    add_math1("round", std::round);
    add_math1("abs",   std::fabs);
    add_math1("sign",  [](double x){ return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); });
    add_math1("sigmoid", [](double x){ return 1.0 / (1.0 + std::exp(-x)); });
    add_math1("relu", [](double x){ return x > 0.0 ? x : 0.0; });
    add_math1("tanh", std::tanh);
    add_math1("sigmoid_deriv", [](double y){ return y * (1.0 - y); });
    add_math1("relu_deriv", [](double y){ return y > 0.0 ? 1.0 : 0.0; });
    add_math1("tanh_deriv", [](double y){ return 1.0 - y * y; });

    builtins["atan2"] = [](VM&, std::vector<Value>& args) -> Value {
        double y = args.size() > 0 ? to_double(args[0]) : 0.0;
        double x = args.size() > 1 ? to_double(args[1]) : 0.0;
        return Value::make_float(std::atan2(y, x));
    };

    builtins["pow"] = [](VM&, std::vector<Value>& args) -> Value {
        double base = args.size() > 0 ? to_double(args[0]) : 0.0;
        double exp = args.size() > 1 ? to_double(args[1]) : 0.0;
        return Value::make_float(std::pow(base, exp));
    };

    auto extreme = [](const char* name, bool want_max, std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error(std::string("bc::") + name + " expects numbers, a vector or an f64_array");
        auto better = [want_max](double x, double best) {
            if (std::isnan(best)) return !std::isnan(x);
            return want_max ? x > best : x <= best;
        };
        if (args.size() == 1 && args[0].type == Value::F64Array) {
            const std::vector<double>& d = *args[0].f64_array_val;
            if (d.empty()) throw std::runtime_error(std::string("bc::") + name + " of an empty f64_array");
            double best = d[0];
            for (size_t i = 1; i < d.size(); i++) if (better(d[i], best)) best = d[i];
            return Value::make_float(best);
        }
        const std::vector<Value>* items = &args;
        if (args.size() == 1 && args[0].type == Value::Vec) {
            items = args[0].vec_val.get();
            if (items->empty()) throw std::runtime_error(std::string("bc::") + name + " of an empty vector");
        }
        if (items->size() == 1) {
            const Value& only = (*items)[0];
            if (only.type != Value::Int && only.type != Value::Float && only.type != Value::Bool) {
                throw std::runtime_error(std::string("bc::") + name + " expects numbers (got " + only.type_name() + ")");
            }
            return only;
        }
        bool all_int = true;
        for (const Value& e : *items) {
            if (e.type != Value::Int && e.type != Value::Float && e.type != Value::Bool) {
                throw std::runtime_error(std::string("bc::") + name + " expects numbers (got " + e.type_name() + ")");
            }
            if (e.type != Value::Int) all_int = false;
        }
        if (all_int) {
            long long best = (*items)[0].int_val;
            for (const Value& e : *items) best = want_max ? std::max(best, e.int_val) : std::min(best, e.int_val);
            return Value::make_int(best);
        }
        double best = to_double((*items)[0]);
        for (const Value& e : *items) if (better(to_double(e), best)) best = to_double(e);
        return Value::make_float(best);
    };
    builtins["max"] = [extreme](VM&, std::vector<Value>& args) -> Value { return extreme("max", true, args); };
    builtins["min"] = [extreme](VM&, std::vector<Value>& args) -> Value { return extreme("min", false, args); };

    builtins["isnan"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 1) throw std::runtime_error("bc::isnan expects one number");
        const Value& v = args[0];
        if (v.type == Value::Float) return Value::make_bool(std::isnan(v.float_val));
        if (v.type == Value::Int || v.type == Value::Bool) return Value::make_bool(false);
        throw std::runtime_error(std::string("bc::isnan expects a number (got ") + v.type_name() + ")");
    };
    builtins["isinf"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 1) throw std::runtime_error("bc::isinf expects one number");
        const Value& v = args[0];
        if (v.type == Value::Float) return Value::make_bool(std::isinf(v.float_val));
        if (v.type == Value::Int || v.type == Value::Bool) return Value::make_bool(false);
        throw std::runtime_error(std::string("bc::isinf expects a number (got ") + v.type_name() + ")");
    };

    builtins["var"] = [](VM&, std::vector<Value>& args) -> Value {
        return Value::make_float(variance_of("var", args));
    };
    builtins["std"] = [](VM&, std::vector<Value>& args) -> Value {
        return Value::make_float(std::sqrt(variance_of("std", args)));
    };

    builtins["shuffle"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 1 || (args[0].type != Value::Vec && args[0].type != Value::F64Array)) {
            throw std::runtime_error(std::string("bc::shuffle expects a vector or an f64_array") +
                                     (args.empty() ? "" : std::string(" (got ") + args[0].type_name() + ")"));
        }
        auto fisher_yates = [](auto& data) {
            for (size_t i = data.size(); i > 1; i--) {
                std::uniform_int_distribution<size_t> pick(0, i - 1);
                std::swap(data[i - 1], data[pick(rng())]);
            }
        };
        if (args[0].type == Value::F64Array) {
            const std::vector<double>& src = *args[0].f64_array_val;
            Value out = Value::make_f64_array(0);
            out.f64_array_val->assign(src.begin(), src.end());
            fisher_yates(*out.f64_array_val);
            return out;
        }
        std::vector<Value> items = *args[0].vec_val;
        fisher_yates(items);
        return Value::make_vec(std::move(items));
    };

    builtins["slice"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args.size() > 3) {
            throw std::runtime_error("bc::slice expects (collection, start) or (collection, start, end)");
        }
        const Value& c = args[0];
        if (c.type == Value::Str) {
            throw std::runtime_error("bc::slice works on vectors and f64_arrays; use bc::substr for strings");
        }
        if (c.type != Value::Vec && c.type != Value::F64Array) {
            throw std::runtime_error(std::string("bc::slice expects a vector or an f64_array (got ") + c.type_name() + ")");
        }
        for (size_t k = 1; k < args.size(); k++) {
            if (args[k].type != Value::Int) throw std::runtime_error("bc::slice: start and end must be integers");
        }
        const long long n = c.type == Value::Vec ? static_cast<long long>(c.vec_val->size())
                                                 : static_cast<long long>(c.f64_array_val->size());
        const long long start_arg = args[1].int_val;
        const long long end_arg = args.size() == 3 ? args[2].int_val : n;
        long long first = start_arg;
        long long last = end_arg;
        if (first < 0) first += n;
        if (args.size() == 3 && last < 0) last += n;
        if (first < 0 || last < first || last > n) {
            throw std::runtime_error("bc::slice: range [" + std::to_string(start_arg) + ", " +
                                     std::to_string(end_arg) + ") is outside a collection of length " +
                                     std::to_string(n));
        }
        if (c.type == Value::F64Array) {
            Value out = Value::make_f64_array(0);
            out.f64_array_val->assign(c.f64_array_val->begin() + first, c.f64_array_val->begin() + last);
            return out;
        }
        std::vector<Value> part(c.vec_val->begin() + first, c.vec_val->begin() + last);
        return Value::make_vec(std::move(part));
    };

    builtins["copy"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 1) throw std::runtime_error("bc::copy expects one value");
        return deep_copy_value(args[0], 0);
    };

    builtins["clamp"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 3) return args.empty() ? Value::make_none() : args[0];
        double x = to_double(args[0]);
        double lo = to_double(args[1]);
        double hi = to_double(args[2]);
        if (std::isnan(x) || std::isnan(lo) || std::isnan(hi)) return Value::make_float(std::numeric_limits<double>::quiet_NaN());
        return Value::make_float(std::max(lo, std::min(x, hi)));
    };

    builtins["PI"] = [](VM&, std::vector<Value>&) -> Value {
        return Value::make_float(3.14159265358979323846);
    };
    builtins["pi_digits"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 1 || args[0].type != Value::Int || args[0].int_val <= 0) {
            throw std::runtime_error("bc::pi_digits expects one positive integer digit count");
        }
        return Value::make_str(calculate_pi_digits(static_cast<std::size_t>(args[0].int_val)));
    };
    builtins["E"] = [](VM&, std::vector<Value>&) -> Value {
        return Value::make_float(2.71828182845904523536);
    };

    builtins["random"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return Value::make_float(dist(rng()));
        }
        double lo = 0.0;
        double hi = to_double(args[0]);
        if (args.size() >= 2) { lo = to_double(args[0]); hi = to_double(args[1]); }
        if (!std::isfinite(lo) || !std::isfinite(hi))
            throw std::runtime_error("random: bounds must be finite numbers");
        if (hi < lo) std::swap(lo, hi);
        if (!std::isfinite(hi - lo))
            throw std::runtime_error("random: range is too wide");
        std::uniform_real_distribution<double> dist(lo, hi);
        return Value::make_float(dist(rng()));
    };
    builtins["randint"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_int(0);
        if (args.size() == 1) {
            long long lo = 0, hi = to_int(args[0]);
            if (hi < lo) std::swap(lo, hi);
            std::uniform_int_distribution<long long> dist(lo, hi);
            return Value::make_int(dist(rng()));
        }
        long long lo = to_int(args[0]);
        long long hi = to_int(args[1]);
        if (hi < lo) std::swap(lo, hi);
        std::uniform_int_distribution<long long> dist(lo, hi);
        return Value::make_int(dist(rng()));
    };
    builtins["seed"] = [](VM&, std::vector<Value>& args) -> Value {
        uint64_t s = args.empty() ? 0 : (uint64_t)to_int(args[0]);
        rng().seed(s);
        return Value::make_none();
    };

    builtins["zeros"] = [](VM&, std::vector<Value>& args) -> Value {
        long long rows = args.size() > 0 ? to_int(args[0]) : 0;
        long long cols = args.size() > 1 ? to_int(args[1]) : 0;
        if (rows <= 0 || cols <= 0) return Value::make_vec({});
        require_tensor_size(rows, cols, "zeros");
        std::vector<Value> result;
        result.reserve(rows);
        for (long long i = 0; i < rows; i++) {
            std::vector<Value> row;
            row.reserve(cols);
            for (long long j = 0; j < cols; j++) row.push_back(Value::make_float(0.0));
            result.push_back(Value::make_vec(std::move(row)));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["ones"] = [](VM&, std::vector<Value>& args) -> Value {
        long long rows = args.size() > 0 ? to_int(args[0]) : 0;
        long long cols = args.size() > 1 ? to_int(args[1]) : 0;
        if (rows <= 0 || cols <= 0) return Value::make_vec({});
        require_tensor_size(rows, cols, "ones");
        std::vector<Value> result;
        result.reserve(rows);
        for (long long i = 0; i < rows; i++) {
            std::vector<Value> row;
            row.reserve(cols);
            for (long long j = 0; j < cols; j++) row.push_back(Value::make_float(1.0));
            result.push_back(Value::make_vec(std::move(row)));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["rand_mat"] = [](VM&, std::vector<Value>& args) -> Value {
        long long rows = args.size() > 0 ? to_int(args[0]) : 0;
        long long cols = args.size() > 1 ? to_int(args[1]) : 0;
        double lo = args.size() > 2 ? to_double(args[2]) : 0.0;
        double hi = args.size() > 3 ? to_double(args[3]) : 1.0;
        if (rows <= 0 || cols <= 0) return Value::make_vec({});
        require_tensor_size(rows, cols, "rand_mat");
        if (!std::isfinite(lo) || !std::isfinite(hi)) throw std::runtime_error("rand_mat: bounds must be finite numbers");
        if (hi < lo) std::swap(lo, hi);
        std::uniform_real_distribution<double> dist(lo, hi);
        std::vector<Value> result;
        result.reserve(rows);
        for (long long i = 0; i < rows; i++) {
            std::vector<Value> row;
            row.reserve(cols);
            for (long long j = 0; j < cols; j++) row.push_back(Value::make_float(dist(rng())));
            result.push_back(Value::make_vec(std::move(row)));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["eye"] = [](VM&, std::vector<Value>& args) -> Value {
        long long n = args.empty() ? 0 : to_int(args[0]);
        if (n <= 0) return Value::make_vec({});
        require_tensor_size(n, n, "eye");
        std::vector<Value> result;
        result.reserve(n);
        for (long long i = 0; i < n; i++) {
            std::vector<Value> row;
            row.reserve(n);
            for (long long j = 0; j < n; j++) {
                row.push_back(Value::make_float(i == j ? 1.0 : 0.0));
            }
            result.push_back(Value::make_vec(std::move(row)));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["matmul"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::make_none();
        if (!is_tensor_2d(args[0]) || !is_tensor_2d(args[1])) {
            throw std::runtime_error("matmul requires two 2D tensors");
        }
        const auto& A = *args[0].vec_val;
        const auto& B = *args[1].vec_val;
        size_t m = A.size();
        size_t k = A[0].vec_val->size();
        size_t k2 = B.size();
        size_t n = B[0].vec_val->size();
        if (k != k2) {
            std::stringstream ss;
            ss << "matmul shape mismatch: (" << m << "x" << k << ") * ("
               << k2 << "x" << n << ")";
            throw std::runtime_error(ss.str());
        }
        std::vector<Value> result;
        result.reserve(m);
        for (size_t i = 0; i < m; i++) {
            std::vector<Value> row;
            row.reserve(n);
            for (size_t j = 0; j < n; j++) {
                double sum = 0.0;
                for (size_t p = 0; p < k; p++) {
                    sum += to_double((*A[i].vec_val)[p]) * to_double((*B[p].vec_val)[j]);
                }
                row.push_back(Value::make_float(sum));
            }
            result.push_back(Value::make_vec(std::move(row)));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["transpose"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || !is_tensor_2d(args[0])) {
            throw std::runtime_error("transpose requires a 2D tensor");
        }
        const auto& A = *args[0].vec_val;
        size_t rows = A.size();
        size_t cols = A[0].vec_val->size();
        std::vector<Value> result;
        result.reserve(cols);
        for (size_t j = 0; j < cols; j++) {
            std::vector<Value> row;
            row.reserve(rows);
            for (size_t i = 0; i < rows; i++) {
                row.push_back((*A[i].vec_val)[j]);
            }
            result.push_back(Value::make_vec(std::move(row)));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["shape"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_vec({});
        const auto& v = args[0];
        if (v.type == Value::F64Array) {
            std::vector<Value> dims;
            dims.push_back(Value::make_int((long long)v.f64_array_val->size()));
            return Value::make_vec(std::move(dims));
        }
        if (v.type != Value::Vec) return Value::make_vec({});
        std::vector<Value> result;
        result.push_back(Value::make_int((long long)v.vec_val->size()));
        if (!v.vec_val->empty() && (*v.vec_val)[0].type == Value::Vec) {
            result.push_back(Value::make_int((long long)(*v.vec_val)[0].vec_val->size()));
        }
        return Value::make_vec(std::move(result));
    };

    builtins["tprint"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) {
            std::cout << "[]\n";
            return Value::make_none();
        }
        std::stringstream ss;
        const auto& v = args[0];
        if (v.type != Value::Vec) {
            ss << v.to_display() << "\n";
            std::cout << ss.str();
            return Value::make_none();
        }
        if (v.vec_val->empty()) {
            ss << "[]\n";
            std::cout << ss.str();
            return Value::make_none();
        }
        if ((*v.vec_val)[0].type != Value::Vec) {
            ss << "[";
            for (size_t i = 0; i < v.vec_val->size(); i++) {
                if (i) ss << ", ";
                ss << (*v.vec_val)[i].to_display();
            }
            ss << "]\n";
        } else {
            ss << "[\n";
            for (const auto& row : *v.vec_val) {
                ss << "  [";
                for (size_t j = 0; j < row.vec_val->size(); j++) {
                    if (j) ss << ", ";
                    const Value& elem = (*row.vec_val)[j];
                    if (elem.type == Value::Float) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%8.4f", elem.float_val);
                        ss << buf;
                    } else {
                        ss << elem.to_display();
                    }
                }
                ss << "]\n";
            }
            ss << "]\n";
        }
        std::cout << ss.str();
        return Value::make_none();
    };

    builtins["dot"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::make_float(0.0);
        const bool a_ok = args[0].type == Value::Vec || args[0].type == Value::F64Array;
        const bool b_ok = args[1].type == Value::Vec || args[1].type == Value::F64Array;
        if (!a_ok || !b_ok) {
            throw std::runtime_error("dot requires two vectors or f64_arrays");
        }
        const size_t na = args[0].type == Value::Vec ? args[0].vec_val->size() : args[0].f64_array_val->size();
        const size_t nb = args[1].type == Value::Vec ? args[1].vec_val->size() : args[1].f64_array_val->size();
        if (na != nb) {
            throw std::runtime_error("dot: vectors must have the same length");
        }
        auto at = [](const Value& v, size_t i) {
            return v.type == Value::F64Array ? (*v.f64_array_val)[i] : to_double((*v.vec_val)[i]);
        };
        double sum = 0.0;
        for (size_t i = 0; i < na; i++) {
            sum += at(args[0], i) * at(args[1], i);
        }
        return Value::make_float(sum);
    };

    builtins["sum"] = [](VM&, std::vector<Value>& args) -> Value {
        if (!args.empty() && args[0].type == Value::F64Array) {
            double fs = 0.0;
            for (const double x : *args[0].f64_array_val) fs += x;
            return Value::make_float(fs);
        }
        if (args.empty() || args[0].type != Value::Vec) return Value::make_float(0.0);
        double s = 0.0;
        bool all_int = true;
        for (const auto& e : *args[0].vec_val) {
            if (e.type != Value::Int) all_int = false;
            s += to_double(e);
        }
        if (all_int) return Value::make_int((long long)s);
        return Value::make_float(s);
    };

    builtins["sum_cols"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || (args[0].type != Value::Vec && args[0].type != Value::F64Array)) {
            throw std::runtime_error(std::string("bc::sum_cols requires a 2D tensor (a nested vector)") +
                                     (args.empty() ? "" : std::string(" (got ") + args[0].type_name() + ")"));
        }
        if (args[0].type == Value::F64Array) {
            throw std::runtime_error("bc::sum_cols requires a 2D tensor (a nested vector); an f64_array is one-dimensional");
        }
        if (args[0].vec_val->empty()) return Value::make_vec({});
        const auto& v = *args[0].vec_val;
        if (v[0].type != Value::Vec) {
            throw std::runtime_error("sum_cols requires a 2D tensor");
        }
        size_t cols = v[0].vec_val->size();
        std::vector<double> acc(cols, 0.0);
        for (const auto& row : v) {
            if (row.type != Value::Vec || row.vec_val->size() != cols) {
                throw std::runtime_error("sum_cols: ragged tensor (rows of different widths)");
            }
            for (size_t j = 0; j < cols; j++) acc[j] += to_double((*row.vec_val)[j]);
        }
        std::vector<Value> out;
        out.reserve(cols);
        for (double x : acc) out.push_back(Value::make_float(x));
        return Value::make_vec(std::move(out));
    };

    builtins["mean"] = [](VM&, std::vector<Value>& args) -> Value {
        if (!args.empty() && args[0].type == Value::F64Array && !args[0].f64_array_val->empty()) {
            double fs = 0.0;
            for (const double x : *args[0].f64_array_val) fs += x;
            return Value::make_float(fs / (double)args[0].f64_array_val->size());
        }
        if (args.empty() || args[0].type != Value::Vec || args[0].vec_val->empty())
            return Value::make_float(0.0);
        double s = 0.0;
        for (const auto& e : *args[0].vec_val) s += to_double(e);
        return Value::make_float(s / (double)args[0].vec_val->size());
    };

    builtins["argmax"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || (args[0].type != Value::Vec && args[0].type != Value::F64Array)) {
            throw std::runtime_error(std::string("bc::argmax expects a vector or an f64_array") +
                                     (args.empty() ? "" : std::string(" (got ") + args[0].type_name() + ")"));
        }
        if (args[0].type == Value::F64Array) {
            const std::vector<double>& d = *args[0].f64_array_val;
            if (args.size() >= 2) {
                if (args[1].type != Value::Int || args[1].int_val <= 0 ||
                    d.size() % static_cast<size_t>(args[1].int_val) != 0) {
                    throw std::runtime_error("bc::argmax: cols must be a positive integer that divides the f64_array length");
                }
                const size_t cols = static_cast<size_t>(args[1].int_val);
                const size_t rows = d.size() / cols;
                std::vector<Value> picks;
                picks.reserve(rows);
                for (size_t r = 0; r < rows; r++) {
                    size_t best_c = 0;
                    for (size_t c = 1; c < cols; c++) {
                        if (d[r * cols + c] > d[r * cols + best_c]) best_c = c;
                    }
                    picks.push_back(Value::make_int(static_cast<long long>(best_c)));
                }
                return Value::make_vec(std::move(picks));
            }
            if (d.empty()) return Value::make_int(-1);
            size_t best_i = 0;
            for (size_t i = 1; i < d.size(); i++) if (d[i] > d[best_i]) best_i = i;
            return Value::make_int(static_cast<long long>(best_i));
        }
        if (args.size() >= 2) throw std::runtime_error("bc::argmax: cols applies only to an f64_array");
        if (args[0].vec_val->empty()) return Value::make_int(-1);
        const auto& v = *args[0].vec_val;
        size_t best = 0;
        double best_val = to_double(v[0]);
        for (size_t i = 1; i < v.size(); i++) {
            double x = to_double(v[i]);
            if (x > best_val) { best_val = x; best = i; }
        }
        return Value::make_int((long long)best);
    };

    builtins["softmax"] = [](VM&, std::vector<Value>& args) -> Value {
        auto softmax_1d = [](const std::vector<Value>& v) -> Value {
            if (v.empty()) return Value::make_vec({});
            double mx = to_double(v[0]);
            for (const auto& e : v) mx = std::max(mx, to_double(e));
            std::vector<double> exps(v.size());
            double sum = 0.0;
            for (size_t i = 0; i < v.size(); i++) {
                exps[i] = std::exp(to_double(v[i]) - mx);
                sum += exps[i];
            }
            std::vector<Value> out;
            out.reserve(v.size());
            for (double e : exps) out.push_back(Value::make_float(sum > 0.0 ? e / sum : 0.0));
            return Value::make_vec(std::move(out));
        };
        if (args.empty() || (args[0].type != Value::Vec && args[0].type != Value::F64Array)) {
            throw std::runtime_error(std::string("bc::softmax expects a vector, a nested vector or an f64_array") +
                                     (args.empty() ? "" : std::string(" (got ") + args[0].type_name() + ")"));
        }
        if (args[0].type == Value::F64Array) {
            const std::vector<double>& d = *args[0].f64_array_val;
            size_t cols = d.size();
            if (args.size() >= 2) {
                if (args[1].type != Value::Int || args[1].int_val <= 0 ||
                    d.size() % static_cast<size_t>(args[1].int_val) != 0) {
                    throw std::runtime_error("bc::softmax: cols must be a positive integer that divides the f64_array length");
                }
                cols = static_cast<size_t>(args[1].int_val);
            }
            Value out = Value::make_f64_array(d.size());
            std::vector<double>& o = *out.f64_array_val;
            if (d.empty()) return out;
            const size_t rows = d.size() / cols;
            for (size_t r = 0; r < rows; r++) {
                const size_t base = r * cols;
                double mx = d[base];
                for (size_t c = 1; c < cols; c++) mx = std::max(mx, d[base + c]);
                double sum = 0.0;
                for (size_t c = 0; c < cols; c++) {
                    o[base + c] = std::exp(d[base + c] - mx);
                    sum += o[base + c];
                }
                for (size_t c = 0; c < cols; c++) o[base + c] = sum > 0.0 ? o[base + c] / sum : 0.0;
            }
            return out;
        }
        if (args.size() >= 2) throw std::runtime_error("bc::softmax: cols applies only to an f64_array");
        const auto& v = *args[0].vec_val;
        if (!v.empty() && v[0].type == Value::Vec) {
            std::vector<Value> rows;
            rows.reserve(v.size());
            for (const auto& row : v) {
                if (row.type != Value::Vec) throw std::runtime_error("bc::softmax: every row of a nested vector must be a vector");
                rows.push_back(softmax_1d(*row.vec_val));
            }
            return Value::make_vec(std::move(rows));
        }
        return softmax_1d(v);
    };

    builtins["map"] = [&vm](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::make_vec({});
        if (args[0].type != Value::Func || args[1].type != Value::Vec) {
            throw std::runtime_error("map requires (function, vector)");
        }
        std::vector<Value> result;
        result.reserve(args[1].vec_val->size());
        for (const auto& e : *args[1].vec_val) {
            Value r = call_callable(vm, args[0], {e});
            result.push_back(r);
        }
        return Value::make_vec(std::move(result));
    };

    builtins["filter"] = [&vm](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::make_vec({});
        if (args[0].type != Value::Func || args[1].type != Value::Vec) {
            throw std::runtime_error("filter requires (function, vector)");
        }
        std::vector<Value> result;
        for (const auto& e : *args[1].vec_val) {
            Value r = call_callable(vm, args[0], {e});
            if (r.is_truthy()) result.push_back(e);
        }
        return Value::make_vec(std::move(result));
    };

    builtins["reduce"] = [&vm](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 3) return args.size() >= 2 ? args[1] : Value::make_none();
        if (args[0].type != Value::Func || args[1].type != Value::Vec) {
            throw std::runtime_error("reduce requires (function, vector, initial)");
        }
        Value acc = args[2];
        for (const auto& e : *args[1].vec_val) {
            acc = call_callable(vm, args[0], {acc, e});
        }
        return acc;
    };

    builtins["upper"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_str("");
        std::string s = *args[0].str_val;
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return Value::make_str(s);
    };
    builtins["lower"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_str("");
        std::string s = *args[0].str_val;
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return Value::make_str(s);
    };
    builtins["split"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_vec({});
        const std::string& s = *args[0].str_val;
        std::string sep = args.size() > 1 && args[1].type == Value::Str ? *args[1].str_val : " ";
        std::vector<Value> result;
        if (sep.empty()) {
            for (char c : s) result.push_back(Value::make_str(std::string(1, c)));
            return Value::make_vec(std::move(result));
        }
        size_t start = 0;
        size_t pos;
        while ((pos = s.find(sep, start)) != std::string::npos) {
            result.push_back(Value::make_str(s.substr(start, pos - start)));
            start = pos + sep.size();
        }
        result.push_back(Value::make_str(s.substr(start)));
        return Value::make_vec(std::move(result));
    };
    builtins["find"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args[0].type != Value::Str || args[1].type != Value::Str)
            return Value::make_int(-1);
        size_t pos = args[0].str_val->find(*args[1].str_val);
        if (pos == std::string::npos) return Value::make_int(-1);
        return Value::make_int((long long)pos);
    };
    builtins["substr"] = [](VM& vm, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_str("");
        const std::string& s = *args[0].str_val;
        long long start = args.size() > 1 ? to_int(args[1]) : 0;
        long long len = args.size() > 2 ? to_int(args[2]) : (long long)s.size();
        if (start < 0) start = 0;
        if (start > (long long)s.size()) start = (long long)s.size();
        if (len < 0) len = 0;
        if (start + len > (long long)s.size()) len = (long long)s.size() - start;
        if (len == 1) return vm.char_string(static_cast<unsigned char>(s[(size_t)start]));
        return Value::make_str(s.substr(start, len));
    };
    builtins["replace"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 3 || args[0].type != Value::Str) return Value::make_str("");
        const std::string& s = *args[0].str_val;
        const std::string from = args[1].type == Value::Str ? *args[1].str_val : args[1].to_display();
        const std::string to = args[2].type == Value::Str ? *args[2].str_val : args[2].to_display();
        if (from.empty()) return args[0];
        std::string out;
        out.reserve(s.size());
        size_t pos = 0, hit;
        while ((hit = s.find(from, pos)) != std::string::npos) {
            out.append(s, pos, hit - pos);
            out += to;
            pos = hit + from.size();
        }
        if (pos == 0) return args[0];
        out.append(s, pos, std::string::npos);
        return Value::make_str(std::move(out));
    };
    builtins["trim"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_str("");
        std::string s = *args[0].str_val;
        auto not_space = [](char c){ return !std::isspace(static_cast<unsigned char>(c)); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
        return Value::make_str(s);
    };
    builtins["repeat"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_str("");
        long long n = args.size() > 1 ? to_int(args[1]) : 0;
        if (n <= 0) return Value::make_str("");
        std::string s;
        const size_t unit = args[0].str_val->size();
        const auto count = static_cast<unsigned long long>(n);
        if (unit && count > s.max_size() / unit)
            throw std::runtime_error("repeated string exceeds maximum size");
        s.reserve(unit * static_cast<size_t>(count));
        for (long long i = 0; i < n; i++) s += *args[0].str_val;
        return Value::make_str(s);
    };

    builtins["typeof"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::make_str("none");
        return Value::make_str(args[0].type_name());
    };
    builtins["at"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args[0].type != Value::Vec) return Value::make_none();
        long long i = to_int(args[1]);
        if (i < 0 || i >= (long long)args[0].vec_val->size()) return Value::make_none();
        return (*args[0].vec_val)[i];
    };
    builtins["join"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || (args[0].type != Value::Vec && args[0].type != Value::F64Array)) {
            throw std::runtime_error(std::string("bc::join expects a vector or an f64_array") +
                                     (args.empty() ? "" : std::string(" (got ") + args[0].type_name() + ")"));
        }
        std::string sep = args.size() > 1 && args[1].type == Value::Str ? *args[1].str_val : " ";
        std::string out;
        if (args[0].type == Value::F64Array) {
            const std::vector<double>& d = *args[0].f64_array_val;
            for (size_t i = 0; i < d.size(); i++) {
                if (i) out += sep;
                out += Value::make_float(d[i]).to_display();
            }
            return Value::make_str(std::move(out));
        }
        const auto& items = *args[0].vec_val;
        for (size_t i = 0; i < items.size(); i++) {
            if (i) out += sep;
            if (items[i].type == Value::Str) out += *items[i].str_val;
            else out += items[i].to_display();
        }
        return Value::make_str(std::move(out));
    };
    builtins["contains"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::make_bool(false);
        if (args[0].type == Value::Str) {
            if (args[1].type != Value::Str) return Value::make_bool(false);
            return Value::make_bool(args[0].str_val->find(*args[1].str_val) != std::string::npos);
        }
        if (args[0].type == Value::Vec) {
            for (const auto& e : *args[0].vec_val) {
                if (eq_values_local(e, args[1])) return Value::make_bool(true);
            }
            return Value::make_bool(false);
        }
        if (args[0].type == Value::Dict) {
            if (args[1].type != Value::Str) return Value::make_bool(false);
            return Value::make_bool(args[0].dict_val->count(*args[1].str_val) > 0);
        }
        if (args[0].type == Value::F64Array) {
            if (args[1].type != Value::Int && args[1].type != Value::Float) return Value::make_bool(false);
            const double needle = to_double(args[1]);
            for (const double x : *args[0].f64_array_val) if (x == needle) return Value::make_bool(true);
            return Value::make_bool(false);
        }
        throw std::runtime_error(std::string("bc::contains expects a string, vector, dict or f64_array (got ") +
                                 args[0].type_name() + ")");
    };
    builtins["has"] = builtins["contains"];
    builtins["keys"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Dict) return Value::make_vec({});
        std::vector<Value> out;
        out.reserve(args[0].dict_val->size());
        for (const auto& kv : *args[0].dict_val) out.push_back(Value::make_str(kv.first));
        return Value::make_vec(std::move(out));
    };
    builtins["values"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Dict) return Value::make_vec({});
        std::vector<Value> out;
        out.reserve(args[0].dict_val->size());
        for (const auto& kv : *args[0].dict_val) out.push_back(kv.second);
        return Value::make_vec(std::move(out));
    };
    builtins["reverse"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty()) throw std::runtime_error("bc::reverse expects a vector, string or f64_array");
        if (args[0].type == Value::Vec) {
            std::vector<Value> v = *args[0].vec_val;
            std::reverse(v.begin(), v.end());
            return Value::make_vec(std::move(v));
        }
        if (args[0].type == Value::F64Array) {
            Value out = Value::make_f64_array(0);
            *out.f64_array_val = *args[0].f64_array_val;
            std::reverse(out.f64_array_val->begin(), out.f64_array_val->end());
            return out;
        }
        if (args[0].type == Value::Str) {
            std::string s = *args[0].str_val;
            std::reverse(s.begin(), s.end());
            return Value::make_str(s);
        }
        throw std::runtime_error(std::string("bc::reverse expects a vector, string or f64_array (got ") + args[0].type_name() + ")");
    };
    builtins["sort"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || (args[0].type != Value::Vec && args[0].type != Value::F64Array)) {
            throw std::runtime_error(std::string("bc::sort expects a vector or an f64_array") +
                                     (args.empty() ? "" : std::string(" (got ") + args[0].type_name() + ")"));
        }
        if (args[0].type == Value::F64Array) {
            Value out = Value::make_f64_array(0);
            *out.f64_array_val = *args[0].f64_array_val;
            std::stable_sort(out.f64_array_val->begin(), out.f64_array_val->end(), [](double x, double y) {
                if (std::isnan(x)) return false;
                if (std::isnan(y)) return true;
                return x < y;
            });
            return out;
        }
        const std::vector<Value>& src = *args[0].vec_val;
        bool all_int = true, all_num = true, all_str = true;
        for (const auto& v : src) {
            if (v.type != Value::Int) all_int = false;
            if (v.type != Value::Int && v.type != Value::Float) all_num = false;
            if (v.type != Value::Str) all_str = false;
        }
        if (src.empty()) return Value::make_vec({});
        if (all_int) {
            std::vector<long long> keys;
            keys.reserve(src.size());
            for (const auto& v : src) keys.push_back(v.int_val);
            std::sort(keys.begin(), keys.end());
            std::vector<Value> out;
            out.reserve(keys.size());
            for (long long k : keys) out.push_back(Value::make_int(k));
            return Value::make_vec(std::move(out));
        }
        if (all_num) {
            auto as_double = [](const Value& v) { return v.type == Value::Float ? v.float_val : (double)v.int_val; };
            std::vector<Value> out(src);
            std::stable_sort(out.begin(), out.end(), [&](const Value& a, const Value& b) {
                if (a.type == Value::Int && b.type == Value::Int) return a.int_val < b.int_val;
                const double x = as_double(a), y = as_double(b);
                if (std::isnan(x)) return false;
                if (std::isnan(y)) return true;
                return x < y;
            });
            return Value::make_vec(std::move(out));
        }
        if (all_str) {
            std::vector<Value> out(src);
            std::stable_sort(out.begin(), out.end(), [](const Value& a, const Value& b) {
                return *a.str_val < *b.str_val;
            });
            return Value::make_vec(std::move(out));
        }
        std::string found;
        for (const auto& v : src) {
            if (v.type != Value::Int && v.type != Value::Float && v.type != Value::Str) { found = v.type_name(); break; }
        }
        throw std::runtime_error(found.empty()
            ? "sort() cannot compare numbers with strings - sort numbers and strings separately"
            : "sort() only orders numbers or strings (found a " + found + ")");
    };

    builtins["dict_merge"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args[0].type != Value::Dict || args[1].type != Value::Dict) {
            throw std::runtime_error("dict_merge expects two dicts");
        }
        auto merged = *args[0].dict_val;
        for (const auto& kv : *args[1].dict_val) merged[kv.first] = kv.second;
        return Value::make_dict(std::move(merged));
    };

    builtins["raise"] = [](VM&, std::vector<Value>& args) -> Value {
        std::string msg = args.empty() ? "error" :
            (args[0].type == Value::Str ? *args[0].str_val : args[0].to_display());
        throw std::runtime_error(msg);
    };

    builtins["time"] = [](VM&, std::vector<Value>&) -> Value {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        return Value::make_float((double)ms / 1000.0);
    };
    builtins["time_ns"] = [](VM&, std::vector<Value>&) -> Value {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        return Value::make_int(static_cast<long long>(ns));
    };
    builtins["sleep"] = [](VM&, std::vector<Value>& args) -> Value {
        double secs = args.empty() ? 0.0 : to_double(args[0]);
        if (secs > 0 && std::isfinite(secs)) {
            if (secs > 86400.0) secs = 86400.0;
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(secs * 1000)));
        }
        return Value::make_none();
    };

    builtins["read_file"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_none();
        std::ifstream f(*args[0].str_val, std::ios::binary);
        if (!f) return Value::make_none();
        std::string content;
        std::error_code size_ec;
        const auto declared = std::filesystem::file_size(*args[0].str_val, size_ec);
        if (!size_ec && declared > 0) {
            if (declared > kMaxFileReadBytes) throw std::runtime_error("read_file: file is larger than the 1 GiB safety limit");
            content.resize(static_cast<size_t>(declared));
            f.read(&content[0], static_cast<std::streamsize>(declared));
            content.resize(static_cast<size_t>(f.gcount()));
            return Value::make_str(std::move(content));
        }
        char chunk[65536];
        while (f.read(chunk, sizeof(chunk)) || f.gcount() > 0) {
            content.append(chunk, static_cast<size_t>(f.gcount()));
            if (content.size() > kMaxFileReadBytes) throw std::runtime_error("read_file: input is larger than the 1 GiB safety limit");
        }
        return Value::make_str(std::move(content));
    };
    builtins["write_file"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args[0].type != Value::Str) return Value::make_bool(false);
        std::string content = args[1].type == Value::Str ? *args[1].str_val : args[1].to_display();
        const auto started = std::chrono::steady_clock::now();
        std::ofstream f(*args[0].str_val, std::ios::binary | std::ios::trunc);
        if (!f) return Value::make_bool(false);
        f << content;
        f.close();
        const bool ok = f.good();
        if (std::getenv("CSY_PROFILE")) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::fprintf(stderr, "[csy-profile] write_file_ns=%lld bytes=%zu ok=%d\n",
                         static_cast<long long>(elapsed), content.size(), ok ? 1 : 0);
        }
        return Value::make_bool(ok);
    };
    builtins["append_file"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() < 2 || args[0].type != Value::Str) return Value::make_bool(false);
        std::string content = args[1].type == Value::Str ? *args[1].str_val : args[1].to_display();
        std::ofstream f(*args[0].str_val, std::ios::binary | std::ios::app);
        if (!f) return Value::make_bool(false);
        f << content;
        return Value::make_bool(true);
    };
    builtins["file_exists"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.empty() || args[0].type != Value::Str) return Value::make_bool(false);
        std::ifstream f(*args[0].str_val);
        return Value::make_bool(f.good());
    };

    builtins["save_f64"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 2 || args[0].type != Value::Str ||
            args[1].type != Value::F64Array || !args[1].f64_array_val) {
            throw std::runtime_error("save_f64 expects (filename, f64_array)");
        }
        save_f64_file(*args[0].str_val, *args[1].f64_array_val);
        return Value::make_bool(true);
    };
    builtins["load_f64"] = [](VM&, std::vector<Value>& args) -> Value {
        if (args.size() != 1 || args[0].type != Value::Str) {
            throw std::runtime_error("load_f64 expects a filename");
        }
        return load_f64_file(*args[0].str_val);
    };

    builtins["exit"] = [](VM&, std::vector<Value>& args) -> Value {
        int code = args.empty() ? 0 : (int)to_int(args[0]);
        std::exit(code);
    };
    builtins["args"] = [](VM& vm, std::vector<Value>&) -> Value {
        std::vector<Value> values;
        const auto& args = vm.program_args();
        values.reserve(args.size());
        for (const auto& arg : args) values.push_back(Value::make_str(arg));
        return Value::make_vec(std::move(values));
    };

}

static bool eq_values_local(const Value& a, const Value& b) {
    if (a.type != b.type) {
        if ((a.type == Value::Int || a.type == Value::Float) &&
            (b.type == Value::Int || b.type == Value::Float)) {
            return to_double(a) == to_double(b);
        }
        return false;
    }
    switch (a.type) {
        case Value::None: return true;
        case Value::Bool: return a.bool_val == b.bool_val;
        case Value::Int:  return a.int_val == b.int_val;
        case Value::Float:return a.float_val == b.float_val;
        case Value::Str:  return *a.str_val == *b.str_val;
        case Value::F64Array: return a.f64_array_val.get() == b.f64_array_val.get();
        default: return false;
    }
}

}

