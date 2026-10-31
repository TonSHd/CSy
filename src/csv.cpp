#include "csv.hpp"
#include "dataio.hpp"
#include "json.hpp"
#include "parquet.hpp"
#include "resource.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace csy {
namespace csv {

namespace {

constexpr size_t kMaxFieldBytes = 16u << 20;
constexpr size_t kMaxColumns = 65536;
constexpr size_t kBufferBytes = 1u << 20;
constexpr size_t kChunkBytes = 1u << 20;
constexpr long long kSampleRows = 10000;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("csv: " + what); }

enum class Kind { Str, Int, F64, Bool };

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Str: return "str";
        case Kind::Int: return "int";
        case Kind::F64: return "f64";
        case Kind::Bool: return "bool";
    }
    return "str";
}

bool kind_from_name(const std::string& n, Kind& out) {
    if (n == "str" || n == "string") { out = Kind::Str; return true; }
    if (n == "int" || n == "i64") { out = Kind::Int; return true; }
    if (n == "f64" || n == "float" || n == "double") { out = Kind::F64; return true; }
    if (n == "bool") { out = Kind::Bool; return true; }
    return false;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_int_text(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (size_t k = i; k < s.size(); k++) if (!is_digit(s[k])) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

bool is_float_text(const std::string& s, double& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    const std::string rest = s.substr(i);
    if (rest == "nan" || rest == "NaN" || rest == "NAN") { out = std::numeric_limits<double>::quiet_NaN(); return true; }
    if (rest == "inf" || rest == "Inf" || rest == "INF" || rest == "infinity" || rest == "Infinity") {
        out = (s[0] == '-') ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    size_t k = i;
    size_t mantissa_digits = 0;
    while (k < s.size() && is_digit(s[k])) { k++; mantissa_digits++; }
    if (k < s.size() && s[k] == '.') {
        k++;
        while (k < s.size() && is_digit(s[k])) { k++; mantissa_digits++; }
    }
    if (mantissa_digits == 0) return false;
    if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
        k++;
        if (k < s.size() && (s[k] == '+' || s[k] == '-')) k++;
        size_t exp_digits = 0;
        while (k < s.size() && is_digit(s[k])) { k++; exp_digits++; }
        if (exp_digits == 0) return false;
    }
    if (k != s.size()) return false;
    out = std::strtod(s.c_str(), nullptr);
    return true;
}

bool is_bool_text(const std::string& s, bool& out) {
    if (s.size() == 4 || s.size() == 5) {
        std::string l = s;
        for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l == "true") { out = true; return true; }
        if (l == "false") { out = false; return true; }
    }
    return false;
}

class RecordReader {
public:
    RecordReader(const std::string& path, char delimiter) : path_(path), delimiter_(delimiter), buffer_(kBufferBytes) {
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) fail("'" + path + "' is a directory, not a file");
        in_.open(path, std::ios::binary);
        if (!in_) {
            fail("cannot open '" + path + "' (" +
                 (std::filesystem::exists(path, ec) ? "permission denied or unreadable" : "no such file") + ")");
        }
        start();
    }

    void reset() {
        in_.clear();
        in_.seekg(0);
        start();
    }

    long long line() const { return record_line_; }

    bool next(std::vector<std::string>& fields) {
        for (;;) {
            const int c = peek();
            if (c < 0) return false;
            if (c == '\n') { get(); physical_++; continue; }
            if (c == '\r') { get(); if (peek() == '\n') get(); physical_++; continue; }
            break;
        }
        fields.clear();
        record_line_ = physical_ + 1;
        std::string cur;
        bool in_quotes = false;
        bool quoted = false;
        for (;;) {
            const int c = get();
            if (c < 0) {
                if (in_quotes) fail("the quoted field that starts on line " + std::to_string(record_line_) + " is never closed");
                fields.push_back(std::move(cur));
                check_count(fields);
                return true;
            }
            if (in_quotes) {
                if (c == '"') {
                    if (peek() == '"') { get(); cur.push_back('"'); }
                    else in_quotes = false;
                } else {
                    if (c == '\n') physical_++;
                    cur.push_back(static_cast<char>(c));
                }
            } else if (c == '"' && cur.empty() && !quoted) {
                in_quotes = true;
                quoted = true;
            } else if (c == delimiter_) {
                fields.push_back(std::move(cur));
                cur.clear();
                quoted = false;
                check_count(fields);
            } else if (c == '\n') {
                physical_++;
                fields.push_back(std::move(cur));
                check_count(fields);
                return true;
            } else if (c == '\r') {
                if (peek() == '\n') get();
                physical_++;
                fields.push_back(std::move(cur));
                check_count(fields);
                return true;
            } else {
                cur.push_back(static_cast<char>(c));
            }
            if (cur.size() > kMaxFieldBytes) fail("a field on line " + std::to_string(record_line_) + " is longer than 16 MiB");
        }
    }

private:
    void start() {
        pos_ = 0;
        len_ = 0;
        physical_ = 0;
        record_line_ = 0;
        if (peek() == 0xEF) {
            if (len_ - pos_ >= 3 && static_cast<unsigned char>(buffer_[pos_ + 1]) == 0xBB &&
                static_cast<unsigned char>(buffer_[pos_ + 2]) == 0xBF) pos_ += 3;
        }
    }

    bool fill() {
        if (!in_) return false;
        in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        len_ = static_cast<size_t>(in_.gcount());
        pos_ = 0;
        return len_ > 0;
    }

    int peek() {
        if (pos_ >= len_ && !fill()) return -1;
        return static_cast<unsigned char>(buffer_[pos_]);
    }

    int get() {
        const int c = peek();
        if (c >= 0) pos_++;
        return c;
    }

    void check_count(const std::vector<std::string>& fields) const {
        if (fields.size() > kMaxColumns) fail("line " + std::to_string(record_line_) + " has more than 65536 columns");
    }

    std::string path_;
    char delimiter_;
    std::ifstream in_;
    std::vector<char> buffer_;
    size_t pos_ = 0, len_ = 0;
    long long physical_ = 0;
    long long record_line_ = 0;
};

struct Infer {
    bool any = false, all_int = true, all_float = true, all_bool = true;
};

void observe(Infer& f, const std::string& s) {
    if (s.empty()) return;
    f.any = true;
    long long i;
    double d;
    bool b;
    if (f.all_int && !is_int_text(s, i)) f.all_int = false;
    if (f.all_float && !is_float_text(s, d)) f.all_float = false;
    if (f.all_bool && !is_bool_text(s, b)) f.all_bool = false;
}

Kind decide(const Infer& f) {
    if (!f.any) return Kind::Str;
    if (f.all_bool) return Kind::Bool;
    if (f.all_int) return Kind::Int;
    if (f.all_float) return Kind::F64;
    return Kind::Str;
}

struct ColBuf {
    Kind kind = Kind::Str;
    std::vector<double> f;
    std::vector<Value> v;
};

class Source {
public:
    Source(const std::string& path, const Options& options) : opt_(options), rr_(path, options.delimiter) {
        begin();
        select_columns();
        apply_type_overrides();
    }

    void begin() {
        rr_.reset();
        has_pending_ = false;
        std::vector<std::string> first;
        if (!rr_.next(first)) fail("the file is empty");
        ncols_ = first.size();
        if (opt_.header) {
            file_names_ = first;
            for (size_t j = 0; j < file_names_.size(); j++) {
                if (file_names_[j].empty()) fail("column " + std::to_string(j + 1) + " has an empty name in the header");
                if (!json::valid_utf8(file_names_[j].data(), file_names_[j].size())) fail("a column name in the header is not valid UTF-8");
            }
            std::unordered_set<std::string> seen;
            for (const auto& n : file_names_) {
                if (!seen.insert(n).second) fail("column '" + n + "' appears twice in the header");
            }
        } else {
            file_names_.clear();
            for (size_t j = 0; j < ncols_; j++) file_names_.push_back("col" + std::to_string(j));
            pending_ = std::move(first);
            has_pending_ = true;
        }
    }

    bool row(std::vector<std::string>& fields) {
        if (has_pending_) {
            fields = std::move(pending_);
            has_pending_ = false;
            pending_line_ = rr_.line();
            line_ = pending_line_;
            return true;
        }
        if (!rr_.next(fields)) return false;
        line_ = rr_.line();
        if (fields.size() != ncols_) {
            fail("row on line " + std::to_string(line_) + " has " + std::to_string(fields.size()) + " field" +
                 (fields.size() == 1 ? "" : "s") + " but " + (opt_.header ? "the header has " : "the first row has ") +
                 std::to_string(ncols_));
        }
        return true;
    }

    void infer(long long limit) {
        std::vector<Infer> acc(sel_.size());
        std::vector<std::string> fields;
        long long n = 0;
        while ((limit <= 0 || n < limit) && row(fields)) {
            for (size_t j = 0; j < sel_.size(); j++) observe(acc[j], fields[sel_[j]]);
            n++;
        }
        kinds_.assign(sel_.size(), Kind::Str);
        for (size_t j = 0; j < sel_.size(); j++) kinds_[j] = forced_[j] ? forced_kind_[j] : decide(acc[j]);
        sampled_ = limit > 0;
        begin();
    }

    void convert(ColBuf& c, const std::string& s, size_t j) const {
        switch (c.kind) {
            case Kind::F64: {
                if (s.empty()) { c.f.push_back(std::numeric_limits<double>::quiet_NaN()); return; }
                double d;
                if (!is_float_text(s, d)) bad_cell(s, j);
                c.f.push_back(d);
                return;
            }
            case Kind::Int: {
                if (s.empty()) { c.v.push_back(Value()); return; }
                long long i;
                if (!is_int_text(s, i)) bad_cell(s, j);
                c.v.push_back(Value::make_int(i));
                return;
            }
            case Kind::Bool: {
                if (s.empty()) { c.v.push_back(Value()); return; }
                bool b;
                if (!is_bool_text(s, b)) bad_cell(s, j);
                c.v.push_back(Value::make_bool(b));
                return;
            }
            case Kind::Str:
                c.v.push_back(Value::make_str(s));
                return;
        }
    }

    [[noreturn]] void bad_cell(const std::string& s, size_t j) const {
        std::string shown = s.size() > 40 ? s.substr(0, 40) + "..." : s;
        std::string msg = "row on line " + std::to_string(line_) + ", column '" + names_[j] + "': '" + shown +
                          "' is not a valid " + kind_name(kinds_[j]);
        if (sampled_) msg += "; the column type was decided from the first " + std::to_string(kSampleRows) + " rows";
        msg += ". Choose it yourself with the option {\"types\": {\"" + names_[j] + "\": \"str\"}} (types: int, f64, bool, str)";
        fail(msg);
    }

    std::vector<ColBuf> new_buffers() const {
        std::vector<ColBuf> b(sel_.size());
        for (size_t j = 0; j < sel_.size(); j++) b[j].kind = kinds_[j];
        return b;
    }

    Value table_from(std::vector<ColBuf>& bufs, long long rows) const {
        std::unordered_map<std::string, Value> columns;
        std::vector<Value> name_values;
        for (size_t j = 0; j < sel_.size(); j++) {
            name_values.push_back(Value::make_str(names_[j]));
            if (bufs[j].kind == Kind::F64) {
                Value arr = Value::make_f64_array(0);
                arr.f64_array_val->swap(bufs[j].f);
                columns[names_[j]] = std::move(arr);
            } else {
                columns[names_[j]] = Value::make_vec(std::move(bufs[j].v));
            }
        }
        std::unordered_map<std::string, Value> table;
        table["names"] = Value::make_vec(std::move(name_values));
        table["rows"] = Value::make_int(rows);
        table["columns"] = Value::make_dict(std::move(columns));
        return Value::make_dict(std::move(table));
    }

    size_t selected() const { return sel_.size(); }
    size_t file_index(size_t j) const { return sel_[j]; }
    const std::vector<std::string>& names() const { return names_; }
    const std::vector<Kind>& kinds() const { return kinds_; }

private:
    void select_columns() {
        sel_.clear();
        names_.clear();
        if (opt_.has_columns) {
            for (const std::string& want : opt_.columns) {
                size_t at = file_names_.size();
                for (size_t j = 0; j < file_names_.size(); j++) if (file_names_[j] == want) { at = j; break; }
                if (at == file_names_.size()) {
                    std::string list;
                    for (size_t j = 0; j < file_names_.size() && j < 12; j++) list += (j ? ", " : "") + file_names_[j];
                    if (file_names_.size() > 12) list += ", ...";
                    fail("column '" + want + "' is not in the file (columns: " + list + ")");
                }
                sel_.push_back(at);
                names_.push_back(want);
            }
            if (sel_.empty()) fail("columns must name at least one column");
        } else {
            for (size_t j = 0; j < file_names_.size(); j++) { sel_.push_back(j); names_.push_back(file_names_[j]); }
        }
    }

    void apply_type_overrides() {
        forced_.assign(sel_.size(), false);
        forced_kind_.assign(sel_.size(), Kind::Str);
        for (const auto& kv : opt_.types) {
            size_t at = names_.size();
            for (size_t j = 0; j < names_.size(); j++) if (names_[j] == kv.first) { at = j; break; }
            if (at == names_.size()) fail("types names a column that is not being read: '" + kv.first + "'");
            Kind k;
            if (!kind_from_name(kv.second, k)) fail("type '" + kv.second + "' for column '" + kv.first + "' is not one of int, f64, bool, str");
            forced_[at] = true;
            forced_kind_[at] = k;
        }
    }

    Options opt_;
    RecordReader rr_;
    size_t ncols_ = 0;
    std::vector<std::string> file_names_, names_;
    std::vector<size_t> sel_;
    std::vector<Kind> kinds_;
    std::vector<bool> forced_;
    std::vector<Kind> forced_kind_;
    std::vector<std::string> pending_;
    bool has_pending_ = false;
    long long pending_line_ = 0;
    long long line_ = 0;
    bool sampled_ = false;
};

std::string format_double(double d) {
    char buf[40];
    for (int digits = 15; digits <= 17; digits++) {
        std::snprintf(buf, sizeof(buf), "%.*g", digits, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    return buf;
}

}

Options options_from(const Value* dict, const char* who, char default_delimiter) {
    Options o;
    auto bad = [who](const std::string& m) { throw std::runtime_error(std::string(who) + ": " + m); };
    o.delimiter = default_delimiter;
    if (!dict) return o;
    for (const auto& kv : *dict->dict_val) {
        const std::string& key = kv.first;
        const Value& v = kv.second;
        if (key == "delimiter") {
            if (v.type != Value::Str || v.str_val->size() != 1) bad("delimiter must be a one character string such as \",\" or \";\"");
            const char d = (*v.str_val)[0];
            if (d == '"' || d == '\n' || d == '\r') bad("delimiter cannot be a quote or a line break");
            o.delimiter = d;
        } else if (key == "header") {
            if (v.type != Value::Bool) bad("header must be true or false");
            o.header = v.bool_val;
        } else if (key == "columns") {
            if (v.type != Value::Vec) bad("columns must be a vector of column names");
            o.has_columns = true;
            for (const Value& e : *v.vec_val) {
                if (e.type != Value::Str) bad("columns must be a vector of strings");
                o.columns.push_back(*e.str_val);
            }
        } else if (key == "types") {
            if (v.type != Value::Dict) bad("types must be a dict from column name to \"int\", \"f64\", \"bool\" or \"str\"");
            for (const auto& t : *v.dict_val) {
                if (t.second.type != Value::Str) bad("each entry of types must be a string");
                o.types[t.first] = *t.second.str_val;
            }
        } else if (key == "max_rows") {
            if (v.type != Value::Int || v.int_val < 0) bad("max_rows must be a non-negative integer");
            o.max_rows = v.int_val;
        } else {
            bad("unknown option '" + key + "' (known options: delimiter, header, columns, types, max_rows)");
        }
    }
    return o;
}

Value read(const std::string& path, const Options& options) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (!ec) res::require_headroom("load_csv", static_cast<uint64_t>(bytes) * 6);
    Source src(path, options);
    src.infer(0);
    std::vector<ColBuf> bufs = src.new_buffers();
    std::vector<std::string> fields;
    long long rows = 0;
    while ((options.max_rows <= 0 || rows < options.max_rows) && src.row(fields)) {
        if (static_cast<unsigned long long>(rows + 1) * src.selected() > parquet::kMaxCells) {
            fail("the file has more cells than load_csv can hold in memory (134217728); read it in batches with open_dataset");
        }
        for (size_t j = 0; j < src.selected(); j++) src.convert(bufs[j], fields[src.file_index(j)], j);
        rows++;
    }
    return src.table_from(bufs, rows);
}

struct Reader::Impl {
    Impl(const std::string& p, const Options& o) : path(p), src(p, o) { src.infer(kSampleRows); }
    std::string path;
    Source src;
    long long rows_read = 0;
};

Reader::Reader(const std::string& path, const Options& options) : impl_(new Impl(path, options)) {}
Reader::~Reader() {}

Value Reader::next(long long rows) {
    Impl& d = *impl_;
    std::vector<ColBuf> bufs = d.src.new_buffers();
    std::vector<std::string> fields;
    long long got = 0;
    while (got < rows && d.src.row(fields)) {
        for (size_t j = 0; j < d.src.selected(); j++) d.src.convert(bufs[j], fields[d.src.file_index(j)], j);
        got++;
    }
    if (got == 0) return Value();
    d.rows_read += got;
    return d.src.table_from(bufs, got);
}

void Reader::reset() {
    impl_->src.begin();
    impl_->rows_read = 0;
}

long long Reader::rows_read() const { return impl_->rows_read; }
const std::vector<std::string>& Reader::names() const { return impl_->src.names(); }

namespace {

struct OutColumn {
    std::string name;
    const Value* col = nullptr;
    size_t rows = 0;
};

void extract_columns(const Value& table, std::vector<OutColumn>& out) {
    if (table.type != Value::Dict) fail("a table must be a dict of columns, or the dict returned by load_csv");
    const std::unordered_map<std::string, Value>* cols = &*table.dict_val;
    std::vector<std::string> names;
    auto cit = table.dict_val->find("columns");
    bool ordered = false;
    if (cit != table.dict_val->end() && cit->second.type == Value::Dict) {
        cols = &*cit->second.dict_val;
        auto nit = table.dict_val->find("names");
        if (nit != table.dict_val->end()) {
            if (nit->second.type != Value::Vec) fail("table 'names' must be a vector of strings");
            for (const Value& n : *nit->second.vec_val) {
                if (n.type != Value::Str) fail("table 'names' must be a vector of strings");
                names.push_back(*n.str_val);
            }
            if (names.size() != cols->size()) fail("table 'names' and 'columns' do not list the same columns");
            ordered = true;
        }
    }
    if (!ordered) {
        for (const auto& kv : *cols) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
    }
    if (names.empty()) fail("the table has no columns");
    if (names.size() > kMaxColumns) fail("the table has more than 65536 columns");
    std::unordered_set<std::string> seen;
    for (const std::string& name : names) {
        if (name.empty()) fail("a column name is empty");
        if (!seen.insert(name).second) fail("column '" + name + "' is listed twice");
        auto it = cols->find(name);
        if (it == cols->end()) fail("table 'names' lists '" + name + "' but there is no such column");
        OutColumn oc;
        oc.name = name;
        oc.col = &it->second;
        if (it->second.type == Value::F64Array) oc.rows = it->second.f64_array_val->size();
        else if (it->second.type == Value::Vec) oc.rows = it->second.vec_val->size();
        else fail("column '" + name + "' must be an f64_array or a vector");
        out.push_back(oc);
    }
    for (const OutColumn& oc : out) {
        if (oc.rows != out[0].rows) {
            fail("column '" + oc.name + "' has " + std::to_string(oc.rows) + " rows but column '" + out[0].name + "' has " + std::to_string(out[0].rows));
        }
    }
}

void put_field(std::string& out, const std::string& s, char delim, bool force_quote) {
    bool quote = force_quote;
    if (!quote) {
        for (char c : s) {
            if (c == delim || c == '"' || c == '\n' || c == '\r') { quote = true; break; }
        }
        if (!quote && !s.empty() && (s.front() == ' ' || s.back() == ' ')) quote = true;
    }
    if (!quote) { out += s; return; }
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
}

}

struct Writer::Impl {
    Impl(const std::string& p, char d) : path(p), delim(d), out(p, "create_dataset") {}
    std::string path;
    char delim;
    dataio::AtomicWriter out;
    bool started = false;
    std::vector<std::string> names;
    std::string chunk;
};

Writer::Writer(const std::string& path, char delimiter) : impl_(new Impl(path, delimiter)) {}
Writer::~Writer() {}

void Writer::append(const Value& table) {
    Impl& d = *impl_;
    std::vector<OutColumn> cols;
    extract_columns(table, cols);
    if (!d.started) {
        for (size_t j = 0; j < cols.size(); j++) {
            d.names.push_back(cols[j].name);
            put_field(d.chunk, cols[j].name, d.delim, cols.size() == 1 && cols[j].name.empty());
            if (j + 1 < cols.size()) d.chunk.push_back(d.delim);
        }
        d.chunk.push_back('\n');
        d.started = true;
    } else {
        bool same = cols.size() == d.names.size();
        for (size_t j = 0; same && j < cols.size(); j++) same = cols[j].name == d.names[j];
        if (!same) {
            std::string expect, got;
            for (size_t j = 0; j < d.names.size(); j++) expect += (j ? ", " : "") + d.names[j];
            for (size_t j = 0; j < cols.size(); j++) got += (j ? ", " : "") + cols[j].name;
            fail("the columns of this batch (" + got + ") differ from the first batch (" + expect + ")");
        }
    }
    const size_t rows = cols[0].rows;
    const bool single = cols.size() == 1;
    for (size_t r = 0; r < rows; r++) {
        for (size_t j = 0; j < cols.size(); j++) {
            std::string cell;
            bool force = false;
            const Value& col = *cols[j].col;
            if (col.type == Value::F64Array) {
                const double x = (*col.f64_array_val)[r];
                if (std::isnan(x)) cell.clear();
                else if (std::isinf(x)) cell = x < 0 ? "-inf" : "inf";
                else cell = format_double(x);
            } else {
                const Value& v = (*col.vec_val)[r];
                switch (v.type) {
                    case Value::Int: cell = std::to_string(v.int_val); break;
                    case Value::Float:
                        if (std::isnan(v.float_val)) cell.clear();
                        else if (std::isinf(v.float_val)) cell = v.float_val < 0 ? "-inf" : "inf";
                        else cell = format_double(v.float_val);
                        break;
                    case Value::Bool: cell = v.bool_val ? "true" : "false"; break;
                    case Value::Str: cell = *v.str_val; break;
                    case Value::None: break;
                    default:
                        fail("row " + std::to_string(r + 1) + " of column '" + cols[j].name + "' is not a number, bool, string or none");
                }
            }
            if (single && cell.empty()) force = true;
            put_field(d.chunk, cell, d.delim, force);
            if (j + 1 < cols.size()) d.chunk.push_back(d.delim);
        }
        d.chunk.push_back('\n');
        if (d.chunk.size() >= kChunkBytes) {
            d.out.write(d.chunk);
            d.chunk.clear();
        }
    }
}

void Writer::finish() {
    Impl& d = *impl_;
    if (!d.started) fail("no batch was written, so there is nothing to save");
    d.out.write(d.chunk);
    d.chunk.clear();
    d.out.commit();
}

void write(const std::string& path, const Value& table, char delimiter) {
    Writer w(path, delimiter);
    w.append(table);
    w.finish();
}

}
}

