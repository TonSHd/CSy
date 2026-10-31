#include "dataset.hpp"
#include "csv.hpp"
#include "dataio.hpp"
#include "json.hpp"
#include "parquet.hpp"
#include "value.hpp"
#include "vm.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace csy {

namespace {

constexpr unsigned long long kMaxElems = 1ULL << 27;
constexpr size_t kMaxDims = 8;
constexpr size_t kChunkBytes = 1u << 20;
const char kBinMagic[8] = {'C', 'S', 'Y', 'B', 'I', 'N', '0', '1'};

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

const std::string& need_path(const std::vector<Value>& a, const char* who, const char* shape) {
    if (a.empty() || a[0].type != Value::Str) fail(std::string(who) + " expects " + shape);
    return *a[0].str_val;
}

std::string extension_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

void put32(std::string& o, uint32_t v) {
    for (int k = 0; k < 4; k++) o.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}
void put64(std::string& o, uint64_t v) {
    for (int k = 0; k < 8; k++) o.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}
uint32_t get32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t get64(const unsigned char* p) {
    return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32);
}

struct DType {
    uint32_t code;
    const char* name;
    size_t size;
    bool is_int;
};

const DType kDTypes[] = {
    {1, "f64", 8, false},
    {2, "f32", 4, false},
    {3, "i64", 8, true},
    {4, "i32", 4, true},
    {5, "u8", 1, true},
};

const DType* dtype_by_name(const std::string& name) {
    for (const DType& t : kDTypes) {
        if (name == t.name) return &t;
    }
    return nullptr;
}

const DType* dtype_by_code(uint32_t code) {
    for (const DType& t : kDTypes) {
        if (code == t.code) return &t;
    }
    return nullptr;
}

const DType& require_dtype(const std::string& name, const char* who) {
    const DType* t = dtype_by_name(name);
    if (!t) fail(std::string(who) + ": unknown dtype '" + name + "' (use f64, f32, i64, i32 or u8)");
    return *t;
}

void infer_shape(const Value& v, std::vector<size_t>& shape) {
    const Value* cur = &v;
    for (;;) {
        if (cur->type == Value::F64Array) {
            if (shape.size() >= kMaxDims) fail("the data is nested more than 8 levels deep");
            shape.push_back(cur->f64_array_val->size());
            return;
        }
        if (cur->type != Value::Vec) return;
        if (shape.size() >= kMaxDims) fail("the data is nested more than 8 levels deep");
        shape.push_back(cur->vec_val->size());
        if (cur->vec_val->empty()) return;
        cur = &(*cur->vec_val)[0];
    }
}

unsigned long long shape_count(const std::vector<size_t>& shape) {
    unsigned long long n = 1;
    for (const size_t d : shape) {
        if (d != 0 && n > kMaxElems / d) fail("the data has more than " + std::to_string(kMaxElems) + " elements");
        n *= d;
    }
    return n;
}

template <class F>
void walk(const Value& v, size_t depth, const std::vector<size_t>& shape, F& f) {
    if (depth == shape.size()) {
        switch (v.type) {
            case Value::Int: f(static_cast<double>(v.int_val), true, v.int_val); return;
            case Value::Bool: f(v.bool_val ? 1.0 : 0.0, true, v.bool_val ? 1LL : 0LL); return;
            case Value::Float: f(v.float_val, false, 0LL); return;
            default:
                fail("the data holds a " + v.type_name() + " where a number is expected "
                     "(tensors must be rectangular and numeric)");
        }
    }
    if (v.type == Value::F64Array) {
        if (depth + 1 != shape.size() || v.f64_array_val->size() != shape[depth]) {
            fail("the data is not rectangular, an inner vector has a different length");
        }
        for (const double x : *v.f64_array_val) f(x, false, 0LL);
        return;
    }
    if (v.type != Value::Vec) {
        fail("the data is not rectangular, found " + v.type_name() + " where a vector is expected");
    }
    if (v.vec_val->size() != shape[depth]) fail("the data is not rectangular, an inner vector has a different length");
    for (const Value& e : *v.vec_val) walk(e, depth + 1, shape, f);
}

void encode_element(char* dst, const DType& t, double d, bool is_int, long long iv) {
    switch (t.code) {
        case 1: {
            uint64_t u;
            std::memcpy(&u, &d, 8);
            for (int k = 0; k < 8; k++) dst[k] = static_cast<char>((u >> (8 * k)) & 0xFF);
            return;
        }
        case 2: {
            const float f = static_cast<float>(d);
            if (std::isfinite(d) && !std::isfinite(f)) fail("a value is too large for f32");
            uint32_t u;
            std::memcpy(&u, &f, 4);
            for (int k = 0; k < 4; k++) dst[k] = static_cast<char>((u >> (8 * k)) & 0xFF);
            return;
        }
        default: {
            if (!is_int) fail(std::string("dtype ") + t.name + " needs integer values, but the data holds a float");
            if (t.code == 3) {
                const uint64_t u = static_cast<uint64_t>(iv);
                for (int k = 0; k < 8; k++) dst[k] = static_cast<char>((u >> (8 * k)) & 0xFF);
            } else if (t.code == 4) {
                if (iv < std::numeric_limits<int32_t>::min() || iv > std::numeric_limits<int32_t>::max()) {
                    fail("an integer is outside the i32 range");
                }
                const uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(iv));
                for (int k = 0; k < 4; k++) dst[k] = static_cast<char>((u >> (8 * k)) & 0xFF);
            } else {
                if (iv < 0 || iv > 255) fail("an integer is outside the u8 range (0 to 255)");
                dst[0] = static_cast<char>(static_cast<unsigned char>(iv));
            }
        }
    }
}

double decode_element(const unsigned char* p, const DType& t) {
    switch (t.code) {
        case 1: {
            const uint64_t u = get64(p);
            double d;
            std::memcpy(&d, &u, 8);
            return d;
        }
        case 2: {
            const uint32_t u = get32(p);
            float f;
            std::memcpy(&f, &u, 4);
            return static_cast<double>(f);
        }
        case 3: {
            const int64_t v = static_cast<int64_t>(get64(p));
            if (v > (1LL << 53) || v < -(1LL << 53)) {
                fail("an i64 value is too large to keep exactly in an f64_array");
            }
            return static_cast<double>(v);
        }
        case 4: return static_cast<double>(static_cast<int32_t>(get32(p)));
        default: return static_cast<double>(p[0]);
    }
}

uint64_t fnv1a(uint64_t h, const char* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

struct TensorPlan {
    std::vector<size_t> shape;
    unsigned long long count = 0;
    bool all_int = true;
};

TensorPlan plan_tensor(const Value& data, const char* who) {
    if (data.type != Value::F64Array && data.type != Value::Vec) {
        fail(std::string(who) + " expects an f64_array or a (nested) vector of numbers");
    }
    TensorPlan plan;
    infer_shape(data, plan.shape);
    plan.count = shape_count(plan.shape);
    auto scan = [&](double, bool is_int, long long) {
        if (!is_int) plan.all_int = false;
    };
    if (data.type == Value::F64Array) plan.all_int = false;
    else walk(data, 0, plan.shape, scan);
    return plan;
}

void stream_payload(dataio::AtomicWriter& out, const Value& data, const TensorPlan& plan, const DType& t,
                    uint64_t* checksum) {
    std::string chunk;
    chunk.reserve(kChunkBytes + 8);
    char buf[8];
    auto flush = [&]() {
        if (chunk.empty()) return;
        if (checksum) *checksum = fnv1a(*checksum, chunk.data(), chunk.size());
        out.write(chunk);
        chunk.clear();
    };
    auto emit = [&](double d, bool is_int, long long iv) {
        encode_element(buf, t, d, is_int, iv);
        chunk.append(buf, t.size);
        if (chunk.size() >= kChunkBytes) flush();
    };
    walk(data, 0, plan.shape, emit);
    flush();
}

void apply_tensor_args(std::vector<Value>& a, size_t from, const char* who, std::vector<long long>& shape,
                       bool& has_shape, const DType*& dtype) {
    for (size_t k = from; k < a.size(); k++) {
        if (a[k].type == Value::Vec && !has_shape) {
            has_shape = true;
            for (const Value& e : *a[k].vec_val) {
                if (e.type != Value::Int || e.int_val < 0) fail(std::string(who) + ": shape must be a vector of non-negative integers");
                shape.push_back(e.int_val);
            }
            if (shape.empty() || shape.size() > kMaxDims) fail(std::string(who) + ": shape needs between 1 and 8 dimensions");
        } else if (a[k].type == Value::Str && dtype == nullptr) {
            dtype = &require_dtype(*a[k].str_val, who);
        } else {
            fail(std::string(who) + ": unexpected argument, expected (path, data[, shape][, dtype])");
        }
    }
}

Value save_bin_impl(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_bin", "(path, data[, shape][, dtype])");
    if (a.size() < 2 || a.size() > 4) fail("save_bin expects (path, data[, shape][, dtype])");
    std::vector<long long> shape_arg;
    bool has_shape = false;
    const DType* dtype = nullptr;
    apply_tensor_args(a, 2, "save_bin", shape_arg, has_shape, dtype);
    TensorPlan plan = plan_tensor(a[1], "save_bin");
    if (!dtype) dtype = dtype_by_code(plan.all_int && a[1].type == Value::Vec && plan.count > 0 ? 3 : 1);
    std::vector<size_t> dims = plan.shape;
    if (has_shape) {
        unsigned long long n = 1;
        for (const long long d : shape_arg) {
            if (d != 0 && n > kMaxElems / static_cast<unsigned long long>(d)) fail("save_bin: the shape is too large");
            n *= static_cast<unsigned long long>(d);
        }
        if (n != plan.count) {
            fail("save_bin: the shape holds " + std::to_string(n) + " elements but the data has " + std::to_string(plan.count));
        }
        dims.assign(shape_arg.begin(), shape_arg.end());
    }
    std::string header(kBinMagic, 8);
    put32(header, dtype->code);
    put32(header, static_cast<uint32_t>(dims.size()));
    for (const size_t d : dims) put64(header, d);
    put64(header, plan.count * dtype->size);

    dataio::AtomicWriter out(path, "save_bin");
    out.write(header);
    uint64_t checksum = 1469598103934665603ULL;
    stream_payload(out, a[1], plan, *dtype, &checksum);
    std::string trailer;
    put64(trailer, checksum);
    out.write(trailer);
    out.commit();
    return Value::make_bool(true);
}

Value fn_load_bin(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_bin", "(path)");
    if (a.size() != 1) fail("load_bin expects (path)");
    const std::string file = dataio::read_all(path, "load_bin");
    const unsigned char* p = reinterpret_cast<const unsigned char*>(file.data());
    const size_t size = file.size();
    if (size < 8 || std::memcmp(p, kBinMagic, 8) != 0) {
        fail("load_bin: '" + path + "' is not a CSy .bin dataset (missing the CSYBIN01 header); "
             "for a file without a header use load_bin_raw(path, dtype)");
    }
    if (size < 16) fail("load_bin: the header is truncated");
    const DType* t = dtype_by_code(get32(p + 8));
    if (!t) fail("load_bin: the file uses an unknown dtype code");
    const uint32_t ndim = get32(p + 12);
    if (ndim == 0 || ndim > kMaxDims) fail("load_bin: the file has an invalid number of dimensions");
    const size_t header_len = 16 + static_cast<size_t>(ndim) * 8 + 8;
    if (size < header_len + 8) fail("load_bin: the header is truncated");
    std::vector<Value> dims;
    unsigned long long count = 1;
    for (uint32_t k = 0; k < ndim; k++) {
        const uint64_t d = get64(p + 16 + static_cast<size_t>(k) * 8);
        if (d > kMaxElems) fail("load_bin: a dimension is larger than the safety limit");
        if (d != 0 && count > kMaxElems / d) fail("load_bin: the data has more than " + std::to_string(kMaxElems) + " elements");
        count *= d;
        dims.push_back(Value::make_int(static_cast<long long>(d)));
    }
    const uint64_t payload = get64(p + 16 + static_cast<size_t>(ndim) * 8);
    if (payload != count * t->size) fail("load_bin: the payload size does not match the shape and dtype");
    if (size != header_len + payload + 8) fail("load_bin: the file size does not match its header, it is truncated or has extra data");
    const char* body = file.data() + header_len;
    const uint64_t want = get64(p + header_len + payload);
    if (fnv1a(1469598103934665603ULL, body, static_cast<size_t>(payload)) != want) {
        fail("load_bin: the checksum does not match, the file is corrupt");
    }
    Value arr = Value::make_f64_array(static_cast<size_t>(count));
    std::vector<double>& dst = *arr.f64_array_val;
    const unsigned char* src = reinterpret_cast<const unsigned char*>(body);
    for (size_t k = 0; k < static_cast<size_t>(count); k++) dst[k] = decode_element(src + k * t->size, *t);
    std::unordered_map<std::string, Value> result;
    result["dtype"] = Value::make_str(t->name);
    result["shape"] = Value::make_vec(std::move(dims));
    result["data"] = std::move(arr);
    return Value::make_dict(std::move(result));
}

Value save_bin_raw_impl(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_bin_raw", "(path, data, dtype)");
    if (a.size() != 3 || a[2].type != Value::Str) fail("save_bin_raw expects (path, data, dtype)");
    const DType& t = require_dtype(*a[2].str_val, "save_bin_raw");
    TensorPlan plan = plan_tensor(a[1], "save_bin_raw");
    dataio::AtomicWriter out(path, "save_bin_raw");
    stream_payload(out, a[1], plan, t, nullptr);
    out.commit();
    return Value::make_bool(true);
}

Value fn_load_bin_raw(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_bin_raw", "(path, dtype[, offset])");
    if (a.size() < 2 || a.size() > 3 || a[1].type != Value::Str) fail("load_bin_raw expects (path, dtype[, offset])");
    const DType& t = require_dtype(*a[1].str_val, "load_bin_raw");
    long long offset = 0;
    if (a.size() == 3) {
        if (a[2].type != Value::Int || a[2].int_val < 0) fail("load_bin_raw: offset must be a non-negative integer");
        offset = a[2].int_val;
    }
    const std::string file = dataio::read_all(path, "load_bin_raw");
    if (static_cast<unsigned long long>(offset) > file.size()) fail("load_bin_raw: the offset is past the end of the file");
    const size_t avail = file.size() - static_cast<size_t>(offset);
    if (avail % t.size != 0) {
        fail("load_bin_raw: the data after the offset (" + std::to_string(avail) + " bytes) is not a whole number of " +
             t.name + " values");
    }
    const size_t count = avail / t.size;
    if (count > kMaxElems) fail("load_bin_raw: the file has more than " + std::to_string(kMaxElems) + " elements");
    Value arr = Value::make_f64_array(count);
    std::vector<double>& dst = *arr.f64_array_val;
    const unsigned char* src = reinterpret_cast<const unsigned char*>(file.data()) + offset;
    for (size_t k = 0; k < count; k++) dst[k] = decode_element(src + k * t.size, t);
    return arr;
}

Value to_f64_impl(std::vector<Value>& a) {
    if (a.size() != 1) fail("to_f64 expects (vector or f64_array)");
    if (a[0].type == Value::F64Array) return a[0];
    if (a[0].type != Value::Vec) fail("to_f64 expects a (nested) vector of numbers or an f64_array");
    std::vector<size_t> shape;
    infer_shape(a[0], shape);
    const unsigned long long count = shape_count(shape);
    Value arr = Value::make_f64_array(0);
    std::vector<double>& dst = *arr.f64_array_val;
    dst.reserve(static_cast<size_t>(count));
    auto collect = [&](double d, bool, long long) { dst.push_back(d); };
    walk(a[0], 0, shape, collect);
    return arr;
}


template <class F>
Value with_prefix(const char* who, F&& body, std::vector<Value>& a) {
    try {
        return body(a);
    } catch (const std::runtime_error& e) {
        const std::string m = e.what();
        if (m.rfind(who, 0) == 0) throw;
        throw std::runtime_error(std::string(who) + ": " + m);
    }
}

Value fn_save_bin(std::vector<Value>& a) { return with_prefix("save_bin", save_bin_impl, a); }
Value fn_save_bin_raw(std::vector<Value>& a) { return with_prefix("save_bin_raw", save_bin_raw_impl, a); }
Value fn_to_f64(std::vector<Value>& a) { return with_prefix("to_f64", to_f64_impl, a); }

Value fn_json_parse(std::vector<Value>& a) {
    if (a.size() != 1 || a[0].type != Value::Str) fail("json_parse expects (text)");
    return json::parse(*a[0].str_val);
}

bool pretty_arg(std::vector<Value>& a, size_t index, const char* who) {
    if (a.size() <= index) return false;
    if (a.size() > index + 1 || a[index].type != Value::Bool) fail(std::string(who) + ": the pretty flag must be true or false");
    return a[index].bool_val;
}

Value fn_json_stringify(std::vector<Value>& a) {
    if (a.empty() || a.size() > 2) fail("json_stringify expects (value[, pretty])");
    return Value::make_str(json::stringify(a[0], pretty_arg(a, 1, "json_stringify")));
}

Value fn_load_json(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_json", "(path)");
    if (a.size() != 1) fail("load_json expects (path)");
    const std::string text = dataio::read_all(path, "load_json");
    res::require_headroom("load_json", static_cast<uint64_t>(text.size()) * 16);
    try {
        return json::parse(text);
    } catch (const std::runtime_error& e) {
        fail("load_json: '" + path + "': " + e.what());
    }
}

Value fn_save_json(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_json", "(path, value[, pretty])");
    if (a.size() < 2 || a.size() > 3) fail("save_json expects (path, value[, pretty])");
    std::string text = json::stringify(a[1], pretty_arg(a, 2, "save_json"));
    text.push_back('\n');
    dataio::AtomicWriter out(path, "save_json");
    out.write(text);
    out.commit();
    return Value::make_bool(true);
}

Value fn_load_jsonl(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_jsonl", "(path[, max_rows])");
    if (a.size() > 2) fail("load_jsonl expects (path[, max_rows])");
    long long max_rows = 0;
    if (a.size() == 2) {
        if (a[1].type == Value::None) max_rows = 0;
        else if (a[1].type == Value::Int && a[1].int_val >= 0) max_rows = a[1].int_val;
        else fail("load_jsonl: max_rows must be a non-negative integer");
    }
    const std::string text = dataio::read_all(path, "load_jsonl");
    res::require_headroom("load_jsonl", static_cast<uint64_t>(text.size()) * 16);
    std::vector<Value> rows;
    size_t pos = 0;
    size_t line_no = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3;
    }
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        line_no++;
        size_t line_end = end;
        if (line_end > pos && text[line_end - 1] == '\r') line_end--;
        size_t first = pos;
        while (first < line_end && (text[first] == ' ' || text[first] == '\t')) first++;
        if (first < line_end) {
            try {
                rows.push_back(json::parse(text.data() + pos, line_end - pos, line_no));
            } catch (const std::runtime_error& e) {
                fail("load_jsonl: '" + path + "': " + e.what());
            }
            if (max_rows > 0 && static_cast<long long>(rows.size()) >= max_rows) break;
        }
        pos = end + 1;
    }
    return Value::make_vec(std::move(rows));
}

Value fn_save_jsonl(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_jsonl", "(path, rows)");
    if (a.size() != 2 || a[1].type != Value::Vec) fail("save_jsonl expects (path, rows) where rows is a vector");
    dataio::AtomicWriter out(path, "save_jsonl");
    std::string chunk;
    chunk.reserve(kChunkBytes + 4096);
    size_t index = 0;
    for (const Value& row : *a[1].vec_val) {
        try {
            json::stringify_to(chunk, row, false);
        } catch (const std::runtime_error& e) {
            fail("save_jsonl: row " + std::to_string(index) + ": " + e.what());
        }
        chunk.push_back('\n');
        if (chunk.size() >= kChunkBytes) {
            out.write(chunk);
            chunk.clear();
        }
        index++;
    }
    out.write(chunk);
    out.commit();
    return Value::make_bool(true);
}

Value fn_load_parquet(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_parquet", "(path[, columns[, max_rows]])");
    if (a.size() > 3) fail("load_parquet expects (path[, columns[, max_rows]])");
    parquet::ReadOptions options;
    if (a.size() >= 2 && a[1].type != Value::None) {
        if (a[1].type != Value::Vec) fail("load_parquet: columns must be a vector of column names (or none for all)");
        options.has_columns = true;
        for (const Value& c : *a[1].vec_val) {
            if (c.type != Value::Str) fail("load_parquet: columns must be a vector of strings");
            options.columns.push_back(*c.str_val);
        }
    }
    if (a.size() == 3 && a[2].type != Value::None) {
        if (a[2].type != Value::Int || a[2].int_val < 0) fail("load_parquet: max_rows must be a non-negative integer");
        options.max_rows = a[2].int_val;
    }
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (!ec) res::require_headroom("load_parquet", static_cast<uint64_t>(bytes) * 8);
    return parquet::read(path, options);
}

Value fn_save_parquet(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_parquet", "(path, table)");
    if (a.size() != 2) fail("save_parquet expects (path, table)");
    parquet::write(path, a[1]);
    return Value::make_bool(true);
}

char csv_default_delimiter(const std::string& path) { return extension_of(path) == ".tsv" ? '\t' : ','; }

const Value* csv_options_value(const std::vector<Value>& a, size_t index, const char* who) {
    if (a.size() <= index || a[index].type == Value::None) return nullptr;
    if (a[index].type != Value::Dict) fail(std::string(who) + ": options must be a dict");
    return &a[index];
}

Value fn_load_csv(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_csv", "(path[, options])");
    if (a.size() > 2) fail("load_csv expects (path[, options])");
    const csv::Options options = csv::options_from(csv_options_value(a, 1, "load_csv"), "load_csv", csv_default_delimiter(path));
    return csv::read(path, options);
}

Value fn_save_csv(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_csv", "(path, table[, options])");
    if (a.size() < 2 || a.size() > 3) fail("save_csv expects (path, table[, options])");
    char delimiter = csv_default_delimiter(path);
    if (const Value* o = csv_options_value(a, 2, "save_csv")) {
        for (const auto& kv : *o->dict_val) {
            if (kv.first != "delimiter") fail("save_csv: unknown option '" + kv.first + "' (known options: delimiter)");
        }
        delimiter = csv::options_from(o, "save_csv", delimiter).delimiter;
    }
    csv::write(path, a[1], delimiter);
    return Value::make_bool(true);
}

Value fn_load_text(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_text", "(path)");
    if (a.size() != 1) fail("load_text expects (path)");
    const std::string text = dataio::read_all(path, "load_text");
    std::vector<Value> lines;
    size_t pos = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3;
    }
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        size_t line_end = end;
        if (line_end > pos && text[line_end - 1] == '\r') line_end--;
        lines.push_back(Value::make_str(text.substr(pos, line_end - pos)));
        pos = end + 1;
    }
    return Value::make_vec(std::move(lines));
}

Value fn_save_text(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_text", "(path, lines)");
    if (a.size() != 2) fail("save_text expects (path, lines)");
    dataio::AtomicWriter out(path, "save_text");
    if (a[1].type == Value::Str) {
        out.write(*a[1].str_val);
    } else if (a[1].type == Value::Vec) {
        std::string chunk;
        chunk.reserve(kChunkBytes + 4096);
        size_t index = 0;
        for (const Value& line : *a[1].vec_val) {
            const std::string text = line.type == Value::Str ? *line.str_val : line.to_display();
            if (text.find('\n') != std::string::npos) {
                fail("save_text: line " + std::to_string(index) + " contains a newline, so it would not read back as one line");
            }
            chunk += text;
            chunk.push_back('\n');
            if (chunk.size() >= kChunkBytes) {
                out.write(chunk);
                chunk.clear();
            }
            index++;
        }
        out.write(chunk);
    } else {
        fail("save_text expects lines as a vector (or a single string)");
    }
    out.commit();
    return Value::make_bool(true);
}

Value fn_load_dataset(std::vector<Value>& a) {
    const std::string& path = need_path(a, "load_dataset", "(path[, options])");
    const std::string ext = extension_of(path);
    if (ext == ".parquet" || ext == ".pq") return fn_load_parquet(a);
    if (ext == ".jsonl" || ext == ".ndjson") return fn_load_jsonl(a);
    if (ext == ".json") return fn_load_json(a);
    if (ext == ".bin") return fn_load_bin(a);
    if (ext == ".csv" || ext == ".tsv") return fn_load_csv(a);
    return fn_load_text(a);
}

Value fn_save_dataset(std::vector<Value>& a) {
    const std::string& path = need_path(a, "save_dataset", "(path, data[, options])");
    const std::string ext = extension_of(path);
    if (ext == ".parquet" || ext == ".pq") return fn_save_parquet(a);
    if (ext == ".jsonl" || ext == ".ndjson") return fn_save_jsonl(a);
    if (ext == ".json") return fn_save_json(a);
    if (ext == ".bin") return fn_save_bin(a);
    if (ext == ".csv" || ext == ".tsv") return fn_save_csv(a);
    return fn_save_text(a);
}

class LineReader {
public:
    LineReader(const std::string& path, const char* who) : path_(path), who_(who) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) fail(std::string(who) + ": '" + path + "' is a directory, not a file");
        in_.open(path, std::ios::binary);
        if (!in_) {
            fail(std::string(who) + ": cannot open '" + path + "' (" +
                 (std::filesystem::exists(path, ec) ? "permission denied or unreadable" : "no such file") + ")");
        }
    }

    bool next(std::string& line) {
        for (;;) {
            const size_t nl = buf_.find('\n', scan_);
            if (nl != std::string::npos) {
                take(line, nl);
                pos_ = nl + 1;
                scan_ = pos_;
                return true;
            }
            if (eof_) {
                if (pos_ >= buf_.size()) return false;
                take(line, buf_.size());
                pos_ = buf_.size();
                scan_ = pos_;
                return true;
            }
            buf_.erase(0, pos_);
            scan_ = buf_.size();
            pos_ = 0;
            char chunk[1 << 16];
            for (int k = 0; k < 16; k++) {
                in_.read(chunk, sizeof(chunk));
                const std::streamsize got = in_.gcount();
                if (got > 0) buf_.append(chunk, static_cast<size_t>(got));
                if (got < static_cast<std::streamsize>(sizeof(chunk))) {
                    eof_ = true;
                    break;
                }
            }
            if (buf_.size() > kMaxLineBytes) {
                fail(std::string(who_) + ": '" + path_ + "' line " + std::to_string(line_ + 1) + " is longer than 256 MiB");
            }
        }
    }

    size_t line_no() const { return line_; }

    void reset() {
        in_.clear();
        in_.seekg(0);
        buf_.clear();
        pos_ = scan_ = 0;
        eof_ = false;
        line_ = 0;
    }

private:
    static constexpr size_t kMaxLineBytes = 256u << 20;
    std::ifstream in_;
    std::string path_;
    const char* who_;
    std::string buf_;
    size_t pos_ = 0;
    size_t scan_ = 0;
    bool eof_ = false;
    size_t line_ = 0;

    void take(std::string& line, size_t end) {
        size_t begin = pos_;
        if (line_ == 0 && end - begin >= 3 && static_cast<unsigned char>(buf_[begin]) == 0xEF &&
            static_cast<unsigned char>(buf_[begin + 1]) == 0xBB && static_cast<unsigned char>(buf_[begin + 2]) == 0xBF) {
            begin += 3;
        }
        if (end > begin && buf_[end - 1] == '\r') end--;
        line.assign(buf_, begin, end - begin);
        line_++;
    }
};

class DatasetReader {
public:
    virtual ~DatasetReader() {}
    virtual Value next(long long n) = 0;
    virtual void reset() = 0;
    virtual Value info() = 0;
};

class DatasetWriter {
public:
    virtual ~DatasetWriter() {}
    virtual void append(const Value& batch) = 0;
    virtual void finish() = 0;
    virtual const std::string& path() const = 0;
};

Value make_info(std::initializer_list<std::pair<const char*, Value>> items) {
    std::unordered_map<std::string, Value> m;
    for (const auto& kv : items) m[kv.first] = kv.second;
    return Value::make_dict(std::move(m));
}

class JsonlReader : public DatasetReader {
public:
    explicit JsonlReader(const std::string& path) : path_(path), lines_(path, "open_dataset") {}

    Value next(long long n) override {
        std::vector<Value> rows;
        std::string line;
        while (static_cast<long long>(rows.size()) < n && lines_.next(line)) {
            size_t first = 0;
            while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) first++;
            if (first == line.size()) continue;
            try {
                rows.push_back(json::parse(line.data(), line.size(), lines_.line_no()));
            } catch (const std::runtime_error& e) {
                fail("next_batch: '" + path_ + "': " + e.what());
            }
        }
        if (rows.empty()) return Value();
        rows_read_ += static_cast<long long>(rows.size());
        return Value::make_vec(std::move(rows));
    }

    void reset() override {
        lines_.reset();
        rows_read_ = 0;
    }

    Value info() override {
        return make_info({{"format", Value::make_str("jsonl")}, {"path", Value::make_str(path_)},
                          {"rows_read", Value::make_int(rows_read_)}});
    }

private:
    std::string path_;
    LineReader lines_;
    long long rows_read_ = 0;
};

class TextReader : public DatasetReader {
public:
    explicit TextReader(const std::string& path) : path_(path), lines_(path, "open_dataset") {}

    Value next(long long n) override {
        std::vector<Value> rows;
        std::string line;
        while (static_cast<long long>(rows.size()) < n && lines_.next(line)) rows.push_back(Value::make_str(line));
        if (rows.empty()) return Value();
        rows_read_ += static_cast<long long>(rows.size());
        return Value::make_vec(std::move(rows));
    }

    void reset() override {
        lines_.reset();
        rows_read_ = 0;
    }

    Value info() override {
        return make_info({{"format", Value::make_str("text")}, {"path", Value::make_str(path_)},
                          {"rows_read", Value::make_int(rows_read_)}});
    }

private:
    std::string path_;
    LineReader lines_;
    long long rows_read_ = 0;
};

class ParquetStream : public DatasetReader {
public:
    ParquetStream(const std::string& path, const parquet::ReadOptions& options) : path_(path), reader_(path, options) {}

    Value next(long long n) override {
        const auto& names = reader_.names();
        struct Builder {
            bool is_f64 = false;
            std::vector<double> f;
            std::vector<Value> v;
        };
        std::vector<Builder> builders(names.size());
        bool typed = false;
        long long got = 0;
        while (got < n) {
            if (cur_pos_ >= cur_rows_) {
                if (next_group_ >= reader_.groups()) break;
                Value table = reader_.read_group(next_group_++);
                cur_rows_ = static_cast<size_t>(table.dict_val->find("rows")->second.int_val);
                cur_cols_ = table.dict_val->find("columns")->second;
                cur_pos_ = 0;
                if (cur_rows_ == 0) continue;
            }
            const size_t take = static_cast<size_t>(std::min<long long>(n - got, static_cast<long long>(cur_rows_ - cur_pos_)));
            for (size_t j = 0; j < names.size(); j++) {
                const Value& col = cur_cols_.dict_val->find(names[j])->second;
                Builder& b = builders[j];
                if (!typed) b.is_f64 = col.type == Value::F64Array;
                if (b.is_f64) {
                    b.f.insert(b.f.end(), col.f64_array_val->begin() + cur_pos_, col.f64_array_val->begin() + cur_pos_ + take);
                } else {
                    b.v.insert(b.v.end(), col.vec_val->begin() + cur_pos_, col.vec_val->begin() + cur_pos_ + take);
                }
            }
            typed = true;
            cur_pos_ += take;
            got += static_cast<long long>(take);
        }
        if (got == 0) return Value();
        rows_read_ += got;
        std::unordered_map<std::string, Value> columns;
        std::vector<Value> name_values;
        for (size_t j = 0; j < names.size(); j++) {
            name_values.push_back(Value::make_str(names[j]));
            if (builders[j].is_f64) {
                Value arr = Value::make_f64_array(0);
                arr.f64_array_val->swap(builders[j].f);
                columns[names[j]] = std::move(arr);
            } else {
                columns[names[j]] = Value::make_vec(std::move(builders[j].v));
            }
        }
        std::unordered_map<std::string, Value> table;
        table["names"] = Value::make_vec(std::move(name_values));
        table["rows"] = Value::make_int(got);
        table["columns"] = Value::make_dict(std::move(columns));
        return Value::make_dict(std::move(table));
    }

    void reset() override {
        next_group_ = 0;
        cur_pos_ = cur_rows_ = 0;
        cur_cols_ = Value();
        rows_read_ = 0;
    }

    Value info() override {
        std::vector<Value> names;
        for (const auto& n : reader_.names()) names.push_back(Value::make_str(n));
        return make_info({{"format", Value::make_str("parquet")}, {"path", Value::make_str(path_)},
                          {"rows", Value::make_int(reader_.rows())},
                          {"row_groups", Value::make_int(static_cast<long long>(reader_.groups()))},
                          {"names", Value::make_vec(std::move(names))}, {"rows_read", Value::make_int(rows_read_)}});
    }

private:
    std::string path_;
    parquet::Reader reader_;
    size_t next_group_ = 0;
    size_t cur_rows_ = 0;
    size_t cur_pos_ = 0;
    Value cur_cols_;
    long long rows_read_ = 0;
};

constexpr uint64_t kFnvBasis = 1469598103934665603ULL;

class BinStream : public DatasetReader {
public:
    BinStream(const std::string& path, const DType* raw_type, long long row_len, long long raw_offset) : path_(path) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) fail("open_dataset: '" + path + "' is a directory, not a file");
        in_.open(path, std::ios::binary);
        if (!in_) {
            fail("open_dataset: cannot open '" + path + "' (" +
                 (std::filesystem::exists(path, ec) ? "permission denied or unreadable" : "no such file") + ")");
        }
        const uint64_t fsize = std::filesystem::file_size(path, ec);
        if (ec) fail("open_dataset: cannot read the size of '" + path + "'");
        if (raw_type) {
            raw_ = true;
            type_ = raw_type;
            if (row_len < 1) fail("open_dataset: a raw .bin needs the option row_len (values per row)");
            if (static_cast<uint64_t>(raw_offset) > fsize) fail("open_dataset: the offset is past the end of the file");
            row_len_ = static_cast<uint64_t>(row_len);
            const uint64_t row_bytes = row_len_ * type_->size;
            const uint64_t avail = fsize - static_cast<uint64_t>(raw_offset);
            if (row_len_ > kMaxElems || avail % row_bytes != 0) {
                fail("open_dataset: the data after the offset (" + std::to_string(avail) + " bytes) is not a whole number of rows of " +
                     std::to_string(row_len) + " " + type_->name + " values");
            }
            rows_ = avail / row_bytes;
            dims_ = {rows_, row_len_};
            data_off_ = static_cast<uint64_t>(raw_offset);
        } else {
            unsigned char head[16];
            if (fsize < 16) fail("open_dataset: '" + path + "' is not a CSy .bin dataset (it is too small); for a file without a header use the options raw and row_len");
            read_exact(0, head, 16);
            if (std::memcmp(head, kBinMagic, 8) != 0) {
                fail("open_dataset: '" + path + "' is not a CSy .bin dataset (missing the CSYBIN01 header); "
                     "for a file without a header pass {\"raw\": dtype, \"row_len\": n}");
            }
            type_ = dtype_by_code(get32(head + 8));
            if (!type_) fail("open_dataset: the file uses an unknown dtype code");
            const uint32_t ndim = get32(head + 12);
            if (ndim == 0 || ndim > kMaxDims) fail("open_dataset: the file has an invalid number of dimensions");
            const uint64_t header_len = 16 + static_cast<uint64_t>(ndim) * 8 + 8;
            if (fsize < header_len + 8) fail("open_dataset: the header is truncated");
            std::string rest(static_cast<size_t>(ndim) * 8 + 8, '\0');
            read_exact(16, &rest[0], rest.size());
            const unsigned char* r = reinterpret_cast<const unsigned char*>(rest.data());
            uint64_t count = 1;
            for (uint32_t k = 0; k < ndim; k++) {
                const uint64_t d = get64(r + static_cast<size_t>(k) * 8);
                if (d != 0 && count > UINT64_MAX / d) fail("open_dataset: the shape is too large");
                count *= d;
                dims_.push_back(d);
            }
            const uint64_t payload = get64(r + static_cast<size_t>(ndim) * 8);
            if (count > UINT64_MAX / type_->size || payload != count * type_->size) fail("open_dataset: the payload size does not match the shape and dtype");
            if (fsize != header_len + payload + 8) fail("open_dataset: the file size does not match its header, it is truncated or has extra data");
            rows_ = dims_[0];
            row_len_ = 1;
            for (size_t k = 1; k < dims_.size(); k++) row_len_ *= dims_[k];
            data_off_ = header_len;
            trailer_off_ = header_len + payload;
        }
        if (row_len_ > kMaxElems) fail("open_dataset: a single row has more than " + std::to_string(kMaxElems) + " values");
    }

    Value next(long long n) override {
        if (pos_ >= rows_ || row_len_ == 0) return Value();
        const uint64_t cap = row_len_ ? std::max<uint64_t>(1, kMaxElems / row_len_) : 1;
        const uint64_t k = std::min<uint64_t>({static_cast<uint64_t>(n), rows_ - pos_, cap});
        const uint64_t count = k * row_len_;
        const uint64_t bytes = count * type_->size;
        std::string raw(static_cast<size_t>(bytes), '\0');
        read_exact(data_off_ + pos_ * row_len_ * type_->size, &raw[0], raw.size());
        if (!raw_) {
            if (pos_ != checked_rows_) checksum_valid_ = false;
            if (checksum_valid_) checksum_ = fnv1a(checksum_, raw.data(), raw.size());
        }
        Value arr = Value::make_f64_array(static_cast<size_t>(count));
        std::vector<double>& dst = *arr.f64_array_val;
        const unsigned char* src = reinterpret_cast<const unsigned char*>(raw.data());
        for (size_t i = 0; i < static_cast<size_t>(count); i++) dst[i] = decode_element(src + i * type_->size, *type_);
        pos_ += k;
        checked_rows_ = pos_;
        if (!raw_ && checksum_valid_ && pos_ == rows_) {
            unsigned char tail[8];
            read_exact(trailer_off_, tail, 8);
            if (get64(tail) != checksum_) fail("next_batch: the checksum of '" + path_ + "' does not match, the file is corrupt");
        }
        std::vector<Value> shape;
        shape.push_back(Value::make_int(static_cast<long long>(k)));
        for (size_t d = 1; d < dims_.size(); d++) shape.push_back(Value::make_int(static_cast<long long>(dims_[d])));
        return make_info({{"dtype", Value::make_str(type_->name)}, {"shape", Value::make_vec(std::move(shape))}, {"data", std::move(arr)}});
    }

    void reset() override {
        pos_ = 0;
        checked_rows_ = 0;
        checksum_ = kFnvBasis;
        checksum_valid_ = true;
    }

    Value info() override {
        std::vector<Value> shape;
        for (const uint64_t d : dims_) shape.push_back(Value::make_int(static_cast<long long>(d)));
        return make_info({{"format", Value::make_str("bin")}, {"path", Value::make_str(path_)},
                          {"dtype", Value::make_str(type_->name)}, {"shape", Value::make_vec(std::move(shape))},
                          {"rows", Value::make_int(static_cast<long long>(rows_))},
                          {"rows_read", Value::make_int(static_cast<long long>(pos_))}});
    }

private:
    std::string path_;
    std::ifstream in_;
    bool raw_ = false;
    const DType* type_ = nullptr;
    std::vector<uint64_t> dims_;
    uint64_t rows_ = 0;
    uint64_t row_len_ = 1;
    uint64_t data_off_ = 0;
    uint64_t trailer_off_ = 0;
    uint64_t pos_ = 0;
    uint64_t checked_rows_ = 0;
    uint64_t checksum_ = kFnvBasis;
    bool checksum_valid_ = true;

    void read_exact(uint64_t offset, void* dst, size_t n) {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset));
        in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        if (static_cast<size_t>(in_.gcount()) != n) fail("'" + path_ + "' could not be read, the file is truncated");
    }
};

class JsonlWriter : public DatasetWriter {
public:
    explicit JsonlWriter(const std::string& path) : path_(path), out_(path, "create_dataset") {}

    void append(const Value& batch) override {
        if (batch.type != Value::Vec) fail("write_batch: a .jsonl dataset takes a vector of rows");
        for (const Value& row : *batch.vec_val) {
            try {
                json::stringify_to(chunk_, row, false);
            } catch (const std::runtime_error& e) {
                fail("write_batch: row " + std::to_string(rows_) + ": " + e.what());
            }
            chunk_.push_back('\n');
            rows_++;
            if (chunk_.size() >= kChunkBytes) {
                out_.write(chunk_);
                chunk_.clear();
            }
        }
    }

    void finish() override {
        out_.write(chunk_);
        chunk_.clear();
        out_.commit();
    }

    const std::string& path() const override { return path_; }

private:
    std::string path_;
    dataio::AtomicWriter out_;
    std::string chunk_;
    size_t rows_ = 0;
};

class TextWriter : public DatasetWriter {
public:
    explicit TextWriter(const std::string& path) : path_(path), out_(path, "create_dataset") {}

    void append(const Value& batch) override {
        if (batch.type == Value::Str) {
            out_.write(*batch.str_val);
            return;
        }
        if (batch.type != Value::Vec) fail("write_batch: a text dataset takes a vector of lines");
        for (const Value& line : *batch.vec_val) {
            const std::string text = line.type == Value::Str ? *line.str_val : line.to_display();
            if (text.find('\n') != std::string::npos) {
                fail("write_batch: line " + std::to_string(lines_) + " contains a newline, so it would not read back as one line");
            }
            chunk_ += text;
            chunk_.push_back('\n');
            lines_++;
            if (chunk_.size() >= kChunkBytes) {
                out_.write(chunk_);
                chunk_.clear();
            }
        }
    }

    void finish() override {
        out_.write(chunk_);
        chunk_.clear();
        out_.commit();
    }

    const std::string& path() const override { return path_; }

private:
    std::string path_;
    dataio::AtomicWriter out_;
    std::string chunk_;
    size_t lines_ = 0;
};

class CsvStream : public DatasetReader {
public:
    CsvStream(const std::string& path, const csv::Options& options) : path_(path), reader_(path, options) {}

    Value next(long long n) override { return reader_.next(n); }
    void reset() override { reader_.reset(); }

    Value info() override {
        std::vector<Value> names;
        for (const std::string& name : reader_.names()) names.push_back(Value::make_str(name));
        return make_info({{"format", Value::make_str("csv")}, {"path", Value::make_str(path_)},
                          {"rows_read", Value::make_int(reader_.rows_read())}, {"names", Value::make_vec(std::move(names))}});
    }

private:
    std::string path_;
    csv::Reader reader_;
};

class CsvSink : public DatasetWriter {
public:
    CsvSink(const std::string& path, char delimiter) : path_(path), writer_(path, delimiter) {}
    void append(const Value& batch) override { writer_.append(batch); }
    void finish() override { writer_.finish(); }
    const std::string& path() const override { return path_; }

private:
    std::string path_;
    csv::Writer writer_;
};

class ParquetSink : public DatasetWriter {
public:
    explicit ParquetSink(const std::string& path) : path_(path), writer_(path, true) {}
    void append(const Value& batch) override { writer_.append(batch); }
    void finish() override { writer_.finish(); }
    const std::string& path() const override { return path_; }

private:
    std::string path_;
    parquet::Writer writer_;
};

class BinSink : public DatasetWriter {
public:
    BinSink(const std::string& path, const DType* forced) : path_(path), out_(path, "create_dataset"), type_(forced) {}

    void append(const Value& batch) override {
        TensorPlan plan;
        try {
            plan = plan_tensor(batch, "write_batch");
        } catch (const std::runtime_error& e) {
            const std::string m = e.what();
            if (m.rfind("write_batch", 0) == 0) throw;
            fail("write_batch: " + m);
        }
        if (plan.shape.empty()) fail("write_batch: a .bin dataset takes an f64_array or a vector of numbers");
        if (!started_) {
            if (!type_) type_ = dtype_by_code(plan.all_int && batch.type == Value::Vec && plan.count > 0 ? 3 : 1);
            ndim_ = plan.shape.size();
            inner_.assign(plan.shape.begin() + 1, plan.shape.end());
            std::string header(kBinMagic, 8);
            put32(header, type_->code);
            put32(header, static_cast<uint32_t>(ndim_));
            for (size_t k = 0; k < ndim_; k++) put64(header, 0);
            put64(header, 0);
            out_.write(header);
            started_ = true;
        } else {
            std::vector<size_t> inner(plan.shape.begin() + 1, plan.shape.end());
            if (plan.shape.size() != ndim_ || inner != inner_) {
                fail("write_batch: this batch has a different shape than the first batch (every dimension after the first must match)");
            }
        }
        try {
            if (plan.count > 0) stream_payload(out_, batch, plan, *type_, &checksum_);
        } catch (const std::runtime_error& e) {
            const std::string m = e.what();
            if (m.rfind("write_batch", 0) == 0) throw;
            fail("write_batch: " + m);
        }
        rows_ += plan.shape[0];
    }

    void finish() override {
        if (!started_) fail("close_dataset: nothing was written, so the shape is unknown");
        std::string trailer;
        put64(trailer, checksum_);
        out_.write(trailer);
        uint64_t row_len = 1;
        for (const size_t d : inner_) row_len *= d;
        std::string dims;
        put64(dims, rows_);
        for (const size_t d : inner_) put64(dims, d);
        out_.patch(16, dims);
        std::string payload;
        put64(payload, rows_ * row_len * type_->size);
        out_.patch(static_cast<std::streamoff>(16 + 8 * ndim_), payload);
        out_.commit();
    }

    const std::string& path() const override { return path_; }

private:
    std::string path_;
    dataio::AtomicWriter out_;
    const DType* type_;
    bool started_ = false;
    size_t ndim_ = 0;
    std::vector<size_t> inner_;
    uint64_t rows_ = 0;
    uint64_t checksum_ = kFnvBasis;
};

constexpr size_t kMaxOpenDatasets = 64;

struct Registry {
    std::mutex m;
    std::unordered_map<long long, std::unique_ptr<DatasetReader>> readers;
    std::unordered_map<long long, std::unique_ptr<DatasetWriter>> writers;
    long long next_id = 1;
};

Registry& registry() {
    static Registry r;
    static const bool hooked = [] {
        std::atexit([] {
            Registry& reg = registry();
            for (auto& kv : reg.writers) {
                std::fprintf(stderr, "warning: the dataset writer for '%s' was never closed, so nothing was saved; call bc::close_dataset(handle)\n",
                             kv.second->path().c_str());
            }
            reg.writers.clear();
            reg.readers.clear();
        });
        return true;
    }();
    (void)hooked;
    return r;
}

long long handle_arg(const std::vector<Value>& a, size_t index, const char* who) {
    if (a.size() <= index || a[index].type != Value::Int) fail(std::string(who) + " expects a dataset handle as its first argument");
    return a[index].int_val;
}

const std::unordered_map<std::string, Value>* options_arg(const std::vector<Value>& a, size_t index, const char* who,
                                                          std::initializer_list<const char*> allowed) {
    if (a.size() <= index || a[index].type == Value::None) return nullptr;
    if (a[index].type != Value::Dict) fail(std::string(who) + ": options must be a dict");
    for (const auto& kv : *a[index].dict_val) {
        bool known = false;
        for (const char* k : allowed) known = known || kv.first == k;
        if (!known) {
            std::string list;
            for (const char* k : allowed) list += (list.empty() ? "" : ", ") + std::string(k);
            fail(std::string(who) + ": unknown option '" + kv.first + "' (known options: " + list + ")");
        }
    }
    return &*a[index].dict_val;
}

const Value* option(const std::unordered_map<std::string, Value>* o, const char* key) {
    if (!o) return nullptr;
    auto it = o->find(key);
    return it == o->end() ? nullptr : &it->second;
}

Value fn_open_dataset(std::vector<Value>& a) {
    const std::string& path = need_path(a, "open_dataset", "(path[, options])");
    if (a.size() > 2) fail("open_dataset expects (path[, options])");
    const std::string ext = extension_of(path);
    std::unique_ptr<DatasetReader> reader;
    if (ext == ".parquet" || ext == ".pq") {
        auto o = options_arg(a, 1, "open_dataset", {"columns"});
        parquet::ReadOptions ro;
        if (const Value* c = option(o, "columns")) {
            if (c->type != Value::Vec) fail("open_dataset: columns must be a vector of column names");
            ro.has_columns = true;
            for (const Value& e : *c->vec_val) {
                if (e.type != Value::Str) fail("open_dataset: columns must be a vector of strings");
                ro.columns.push_back(*e.str_val);
            }
        }
        reader.reset(new ParquetStream(path, ro));
    } else if (ext == ".csv" || ext == ".tsv") {
        options_arg(a, 1, "open_dataset", {"delimiter", "header", "columns", "types"});
        reader.reset(new CsvStream(path, csv::options_from(csv_options_value(a, 1, "open_dataset"), "open_dataset", csv_default_delimiter(path))));
    } else if (ext == ".jsonl" || ext == ".ndjson") {
        options_arg(a, 1, "open_dataset", {});
        reader.reset(new JsonlReader(path));
    } else if (ext == ".json") {
        fail("open_dataset: a .json file is one value and cannot be streamed; use load_json, or store records as .jsonl");
    } else if (ext == ".bin") {
        auto o = options_arg(a, 1, "open_dataset", {"raw", "row_len", "offset"});
        const DType* raw_type = nullptr;
        long long row_len = 0, offset = 0;
        if (const Value* r = option(o, "raw")) {
            if (r->type != Value::Str) fail("open_dataset: raw must be a dtype name such as \"f32\"");
            raw_type = &require_dtype(*r->str_val, "open_dataset");
        }
        if (const Value* r = option(o, "row_len")) {
            if (r->type != Value::Int || r->int_val < 1) fail("open_dataset: row_len must be a positive integer");
            row_len = r->int_val;
        }
        if (const Value* r = option(o, "offset")) {
            if (r->type != Value::Int || r->int_val < 0) fail("open_dataset: offset must be a non-negative integer");
            offset = r->int_val;
        }
        if (!raw_type && (row_len || offset)) fail("open_dataset: row_len and offset only apply together with raw");
        reader.reset(new BinStream(path, raw_type, row_len, offset));
    } else {
        options_arg(a, 1, "open_dataset", {});
        reader.reset(new TextReader(path));
    }
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.m);
    if (reg.readers.size() + reg.writers.size() >= kMaxOpenDatasets) fail("open_dataset: too many datasets are open, close some with close_dataset");
    const long long id = reg.next_id++;
    reg.readers[id] = std::move(reader);
    return Value::make_int(id);
}

Value fn_next_batch(std::vector<Value>& a) {
    if (a.size() != 2 || a[1].type != Value::Int || a[1].int_val < 1) fail("next_batch expects (handle, rows) with rows at least 1");
    const long long id = handle_arg(a, 0, "next_batch");
    DatasetReader* r;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.m);
        auto it = reg.readers.find(id);
        if (it == reg.readers.end()) fail("next_batch: " + std::to_string(id) + " is not an open dataset reader");
        r = it->second.get();
    }
    return r->next(a[1].int_val);
}

Value fn_reset_dataset(std::vector<Value>& a) {
    if (a.size() != 1) fail("reset_dataset expects (handle)");
    const long long id = handle_arg(a, 0, "reset_dataset");
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.m);
    auto it = reg.readers.find(id);
    if (it == reg.readers.end()) fail("reset_dataset: " + std::to_string(id) + " is not an open dataset reader");
    it->second->reset();
    return Value::make_bool(true);
}

Value fn_dataset_info(std::vector<Value>& a) {
    if (a.size() != 1) fail("dataset_info expects (handle)");
    const long long id = handle_arg(a, 0, "dataset_info");
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.m);
    auto rit = reg.readers.find(id);
    if (rit != reg.readers.end()) return rit->second->info();
    auto wit = reg.writers.find(id);
    if (wit != reg.writers.end()) return make_info({{"format", Value::make_str("writer")}, {"path", Value::make_str(wit->second->path())}});
    fail("dataset_info: " + std::to_string(id) + " is not an open dataset");
}

Value fn_create_dataset(std::vector<Value>& a) {
    const std::string& path = need_path(a, "create_dataset", "(path[, options])");
    if (a.size() > 2) fail("create_dataset expects (path[, options])");
    const std::string ext = extension_of(path);
    std::unique_ptr<DatasetWriter> writer;
    if (ext == ".parquet" || ext == ".pq") {
        options_arg(a, 1, "create_dataset", {});
        writer.reset(new ParquetSink(path));
    } else if (ext == ".csv" || ext == ".tsv") {
        options_arg(a, 1, "create_dataset", {"delimiter"});
        writer.reset(new CsvSink(path, csv::options_from(csv_options_value(a, 1, "create_dataset"), "create_dataset", csv_default_delimiter(path)).delimiter));
    } else if (ext == ".jsonl" || ext == ".ndjson") {
        options_arg(a, 1, "create_dataset", {});
        writer.reset(new JsonlWriter(path));
    } else if (ext == ".json") {
        fail("create_dataset: a .json file is one value and cannot be written in batches; use save_json, or write .jsonl");
    } else if (ext == ".bin") {
        auto o = options_arg(a, 1, "create_dataset", {"dtype"});
        const DType* forced = nullptr;
        if (const Value* d = option(o, "dtype")) {
            if (d->type != Value::Str) fail("create_dataset: dtype must be a name such as \"f32\"");
            forced = &require_dtype(*d->str_val, "create_dataset");
        }
        writer.reset(new BinSink(path, forced));
    } else {
        options_arg(a, 1, "create_dataset", {});
        writer.reset(new TextWriter(path));
    }
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.m);
    if (reg.readers.size() + reg.writers.size() >= kMaxOpenDatasets) fail("create_dataset: too many datasets are open, close some with close_dataset");
    const long long id = reg.next_id++;
    reg.writers[id] = std::move(writer);
    return Value::make_int(id);
}

Value fn_write_batch(std::vector<Value>& a) {
    if (a.size() != 2) fail("write_batch expects (handle, batch)");
    const long long id = handle_arg(a, 0, "write_batch");
    DatasetWriter* w;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.m);
        auto it = reg.writers.find(id);
        if (it == reg.writers.end()) fail("write_batch: " + std::to_string(id) + " is not an open dataset writer");
        w = it->second.get();
    }
    w->append(a[1]);
    return Value::make_bool(true);
}

Value fn_close_dataset(std::vector<Value>& a) {
    if (a.size() != 1) fail("close_dataset expects (handle)");
    const long long id = handle_arg(a, 0, "close_dataset");
    std::unique_ptr<DatasetWriter> writer;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.m);
        auto rit = reg.readers.find(id);
        if (rit != reg.readers.end()) {
            reg.readers.erase(rit);
            return Value::make_bool(true);
        }
        auto wit = reg.writers.find(id);
        if (wit == reg.writers.end()) fail("close_dataset: " + std::to_string(id) + " is not an open dataset");
        writer = std::move(wit->second);
        reg.writers.erase(wit);
    }
    writer->finish();
    return Value::make_bool(true);
}

}

void register_dataset_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();
    auto add = [&](const char* name, Value (*fn)(std::vector<Value>&)) {
        builtins[name] = [fn](VM&, std::vector<Value>& args) -> Value { return fn(args); };
    };
    add("json_parse", fn_json_parse);
    add("json_stringify", fn_json_stringify);
    add("load_json", fn_load_json);
    add("save_json", fn_save_json);
    add("load_jsonl", fn_load_jsonl);
    add("save_jsonl", fn_save_jsonl);
    add("load_parquet", fn_load_parquet);
    add("save_parquet", fn_save_parquet);
    add("load_csv", fn_load_csv);
    add("save_csv", fn_save_csv);
    add("load_bin", fn_load_bin);
    add("save_bin", fn_save_bin);
    add("load_bin_raw", fn_load_bin_raw);
    add("save_bin_raw", fn_save_bin_raw);
    add("load_text", fn_load_text);
    add("save_text", fn_save_text);
    add("load_dataset", fn_load_dataset);
    add("save_dataset", fn_save_dataset);
    add("to_f64", fn_to_f64);
    add("open_dataset", fn_open_dataset);
    add("next_batch", fn_next_batch);
    add("reset_dataset", fn_reset_dataset);
    add("dataset_info", fn_dataset_info);
    add("create_dataset", fn_create_dataset);
    add("write_batch", fn_write_batch);
    add("close_dataset", fn_close_dataset);
}

}

