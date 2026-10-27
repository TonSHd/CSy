#include "parquet.hpp"

#include "compress.hpp"
#include "dataio.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace csy {
namespace parquet {

namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("parquet: " + what); }

enum PType { P_BOOLEAN = 0, P_INT32 = 1, P_INT64 = 2, P_INT96 = 3, P_FLOAT = 4, P_DOUBLE = 5, P_BYTE_ARRAY = 6, P_FIXED = 7 };
enum Encoding { E_PLAIN = 0, E_PLAIN_DICT = 2, E_RLE = 3, E_BIT_PACKED = 4, E_DELTA_BINARY = 5,
                E_DELTA_LENGTH = 6, E_DELTA_BYTE = 7, E_RLE_DICT = 8, E_BYTE_STREAM_SPLIT = 9 };
enum Codec { C_UNCOMPRESSED = 0, C_SNAPPY = 1, C_GZIP = 2 };
enum PageType { PG_DATA = 0, PG_INDEX = 1, PG_DICT = 2, PG_DATA_V2 = 3 };
enum Repetition { R_REQUIRED = 0, R_OPTIONAL = 1, R_REPEATED = 2 };
constexpr int CT_UTF8 = 0;
constexpr int CT_UINT_32 = 13;

constexpr uint64_t kMaxFooterBytes = 64ULL << 20;
constexpr uint64_t kMaxPageBytes = 1ULL << 30;
constexpr uint64_t kMaxStringBytes = 1ULL << 30;
constexpr size_t kMaxColumns = 65536;
constexpr size_t kRowGroupRows = 1u << 20;

std::string codec_name(int c) {
    switch (c) {
        case 0: return "UNCOMPRESSED";
        case 1: return "SNAPPY";
        case 2: return "GZIP";
        case 3: return "LZO";
        case 4: return "BROTLI";
        case 5: return "LZ4";
        case 6: return "ZSTD";
        case 7: return "LZ4_RAW";
        default: return "codec " + std::to_string(c);
    }
}

std::string encoding_name(int e) {
    switch (e) {
        case 0: return "PLAIN";
        case 2: return "PLAIN_DICTIONARY";
        case 3: return "RLE";
        case 4: return "BIT_PACKED";
        case 5: return "DELTA_BINARY_PACKED";
        case 6: return "DELTA_LENGTH_BYTE_ARRAY";
        case 7: return "DELTA_BYTE_ARRAY";
        case 8: return "RLE_DICTIONARY";
        case 9: return "BYTE_STREAM_SPLIT";
        default: return "encoding " + std::to_string(e);
    }
}

std::string ptype_name(int t) {
    switch (t) {
        case 0: return "BOOLEAN";
        case 1: return "INT32";
        case 2: return "INT64";
        case 3: return "INT96";
        case 4: return "FLOAT";
        case 5: return "DOUBLE";
        case 6: return "BYTE_ARRAY";
        case 7: return "FIXED_LEN_BYTE_ARRAY";
        default: return "type " + std::to_string(t);
    }
}

inline uint32_t ld32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t ld64(const uint8_t* p) {
    return static_cast<uint64_t>(ld32(p)) | (static_cast<uint64_t>(ld32(p + 4)) << 32);
}
inline void st32(std::string& o, uint32_t v) {
    for (int k = 0; k < 4; k++) o.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}
inline void st64(std::string& o, uint64_t v) {
    for (int k = 0; k < 8; k++) o.push_back(static_cast<char>((v >> (8 * k)) & 0xFF));
}
inline double bits_to_double(uint64_t u) {
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}
inline uint64_t double_to_bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, 8);
    return u;
}

struct Buf {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t pos = 0;
    Buf() = default;
    Buf(const uint8_t* data, size_t size) : p(data), n(size) {}
    size_t left() const { return n - pos; }
    void need(size_t k) const {
        if (k > n - pos) fail("data ends unexpectedly, the file is truncated or corrupt");
    }
    uint8_t u8() {
        need(1);
        return p[pos++];
    }
    uint32_t u32() {
        need(4);
        const uint32_t v = ld32(p + pos);
        pos += 4;
        return v;
    }
    const uint8_t* take(size_t k) {
        need(k);
        const uint8_t* r = p + pos;
        pos += k;
        return r;
    }
    uint64_t varint() {
        uint64_t v = 0;
        int shift = 0;
        for (;;) {
            const uint8_t b = u8();
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
            shift += 7;
            if (shift > 63) fail("a number in the metadata is too long");
        }
    }
};

enum { T_STOP = 0, T_TRUE = 1, T_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5, T_I64 = 6,
       T_DOUBLE = 7, T_BINARY = 8, T_LIST = 9, T_SET = 10, T_MAP = 11, T_STRUCT = 12 };

struct TReader {
    Buf& b;
    int depth = 0;
    explicit TReader(Buf& buf) : b(buf) {}

    int64_t zz() {
        const uint64_t v = b.varint();
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }

    bool next(int& last, int& id, int& type) {
        const uint8_t h = b.u8();
        if (h == 0) return false;
        type = h & 15;
        const int delta = h >> 4;
        id = delta ? last + delta : static_cast<int>(zz());
        last = id;
        return true;
    }

    int64_t integer(int type) {
        if (type == T_BYTE) return static_cast<int8_t>(b.u8());
        if (type == T_I16 || type == T_I32 || type == T_I64) return zz();
        fail("the metadata has a field of an unexpected type");
    }

    bool boolean(int type) {
        if (type == T_TRUE) return true;
        if (type == T_FALSE) return false;
        fail("the metadata has a field of an unexpected type");
    }

    std::string binary() {
        const uint64_t len = b.varint();
        if (len > b.left()) fail("a string in the metadata is longer than the data");
        const uint8_t* s = b.take(static_cast<size_t>(len));
        return std::string(reinterpret_cast<const char*>(s), static_cast<size_t>(len));
    }

    void list(int& etype, size_t& count) {
        const uint8_t h = b.u8();
        etype = h & 15;
        uint64_t sz = h >> 4;
        if (sz == 15) sz = b.varint();
        if (sz > b.left()) fail("a list in the metadata is longer than the data");
        count = static_cast<size_t>(sz);
    }

    void skip(int type, bool in_list = false) {
        switch (type) {
            case T_TRUE:
            case T_FALSE:
                if (in_list) b.u8();
                return;
            case T_BYTE: b.u8(); return;
            case T_I16:
            case T_I32:
            case T_I64: b.varint(); return;
            case T_DOUBLE: b.take(8); return;
            case T_BINARY: {
                const uint64_t len = b.varint();
                if (len > b.left()) fail("a string in the metadata is longer than the data");
                b.take(static_cast<size_t>(len));
                return;
            }
            case T_LIST:
            case T_SET: {
                int et;
                size_t n;
                list(et, n);
                for (size_t i = 0; i < n; i++) skip(et, true);
                return;
            }
            case T_MAP: {
                const uint64_t n = b.varint();
                if (n == 0) return;
                if (n > b.left()) fail("a map in the metadata is longer than the data");
                const uint8_t kv = b.u8();
                for (uint64_t i = 0; i < n; i++) {
                    skip(kv >> 4, true);
                    skip(kv & 15, true);
                }
                return;
            }
            case T_STRUCT: {
                if (++depth > 32) fail("the metadata is nested too deeply");
                int last = 0, id, t;
                while (next(last, id, t)) skip(t);
                --depth;
                return;
            }
            default: fail("the metadata has a field of an unknown type");
        }
    }
};

struct SchemaEl {
    int type = -1;
    int type_length = 0;
    int rep = R_REQUIRED;
    std::string name;
    int children = 0;
    int converted = -1;
};

struct ColMeta {
    bool present = false;
    bool external = false;
    int type = -1;
    int codec = 0;
    int64_t num_values = 0;
    int64_t total_compressed = 0;
    int64_t data_off = -1;
    int64_t dict_off = -1;
    std::vector<std::string> path;
};

struct RowGroup {
    std::vector<ColMeta> cols;
    int64_t num_rows = 0;
};

struct FileMeta {
    std::vector<SchemaEl> schema;
    int64_t num_rows = 0;
    std::vector<RowGroup> groups;
};

void expect_struct_list(TReader& t, size_t& n) {
    int et;
    t.list(et, n);
    if (et != T_STRUCT) fail("the metadata has a list of an unexpected type");
}

SchemaEl parse_schema_element(TReader& t) {
    SchemaEl e;
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        switch (id) {
            case 1: e.type = static_cast<int>(t.integer(ty)); break;
            case 2: e.type_length = static_cast<int>(t.integer(ty)); break;
            case 3: e.rep = static_cast<int>(t.integer(ty)); break;
            case 4: e.name = t.binary(); break;
            case 5: e.children = static_cast<int>(t.integer(ty)); break;
            case 6: e.converted = static_cast<int>(t.integer(ty)); break;
            default: t.skip(ty);
        }
    }
    return e;
}

ColMeta parse_column_metadata(TReader& t) {
    ColMeta m;
    m.present = true;
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        switch (id) {
            case 1: m.type = static_cast<int>(t.integer(ty)); break;
            case 3: {
                size_t n;
                int et;
                t.list(et, n);
                for (size_t i = 0; i < n; i++) m.path.push_back(t.binary());
                break;
            }
            case 4: m.codec = static_cast<int>(t.integer(ty)); break;
            case 5: m.num_values = t.integer(ty); break;
            case 7: m.total_compressed = t.integer(ty); break;
            case 9: m.data_off = t.integer(ty); break;
            case 11: m.dict_off = t.integer(ty); break;
            default: t.skip(ty);
        }
    }
    return m;
}

ColMeta parse_column_chunk(TReader& t) {
    ColMeta m;
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        if (id == 1) {
            if (!t.binary().empty()) m.external = true;
        } else if (id == 3) {
            const bool ext = m.external;
            if (++t.depth > 32) fail("the metadata is nested too deeply");
            m = parse_column_metadata(t);
            --t.depth;
            m.external = ext;
        } else {
            t.skip(ty);
        }
    }
    return m;
}

RowGroup parse_row_group(TReader& t) {
    RowGroup g;
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        if (id == 1) {
            size_t n;
            expect_struct_list(t, n);
            for (size_t i = 0; i < n; i++) g.cols.push_back(parse_column_chunk(t));
        } else if (id == 3) {
            g.num_rows = t.integer(ty);
        } else {
            t.skip(ty);
        }
    }
    return g;
}

FileMeta parse_file_meta(TReader& t) {
    FileMeta fm;
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        if (id == 2) {
            size_t n;
            expect_struct_list(t, n);
            if (n > kMaxColumns * 2) fail("the schema has too many entries");
            for (size_t i = 0; i < n; i++) fm.schema.push_back(parse_schema_element(t));
        } else if (id == 3) {
            fm.num_rows = t.integer(ty);
        } else if (id == 4) {
            size_t n;
            expect_struct_list(t, n);
            for (size_t i = 0; i < n; i++) fm.groups.push_back(parse_row_group(t));
        } else {
            t.skip(ty);
        }
    }
    return fm;
}

struct Leaf {
    std::string name;
    std::vector<std::string> path;
    int ptype = -1;
    int converted = -1;
    int max_def = 0;
    int max_rep = 0;
    bool flat() const { return path.size() == 1 && max_rep == 0 && max_def <= 1; }
};

void walk_schema(const FileMeta& fm, size_t& idx, int children, std::vector<std::string>& path,
                 int def, int rep, int depth, std::vector<Leaf>& out) {
    if (depth > 64) fail("the schema is nested too deeply");
    for (int c = 0; c < children; c++) {
        if (idx >= fm.schema.size()) fail("the schema ends before all of its columns");
        const SchemaEl& e = fm.schema[idx++];
        int d = def, r = rep;
        if (e.rep == R_OPTIONAL) d++;
        else if (e.rep == R_REPEATED) { d++; r++; }
        path.push_back(e.name);
        if (e.children > 0) {
            walk_schema(fm, idx, e.children, path, d, r, depth + 1, out);
        } else if (e.type >= 0) {
            Leaf leaf;
            leaf.path = path;
            for (size_t k = 0; k < path.size(); k++) {
                if (k) leaf.name += ".";
                leaf.name += path[k];
            }
            leaf.ptype = e.type;
            leaf.converted = e.converted;
            leaf.max_def = d;
            leaf.max_rep = r;
            out.push_back(std::move(leaf));
        }
        path.pop_back();
    }
}

struct Decoded {
    std::vector<int64_t> i;
    std::vector<double> d;
    std::vector<std::string> s;
};

void decode_hybrid(Buf& b, int width, size_t count, std::vector<uint32_t>& out) {
    out.clear();
    if (count == 0) return;
    if (width < 0 || width > 32) fail("a bit width in the data is invalid");
    if (width == 0) {
        out.assign(count, 0);
        return;
    }
    out.reserve(count);
    const size_t byte_w = static_cast<size_t>((width + 7) / 8);
    const uint64_t mask = width == 32 ? 0xFFFFFFFFULL : ((1ULL << width) - 1);
    while (out.size() < count) {
        const uint64_t h = b.varint();
        if (h & 1) {
            const uint64_t groups = h >> 1;
            if (groups == 0) fail("the data has an empty bit-packed run");
            if (groups > b.left() / static_cast<size_t>(width)) fail("data ends unexpectedly, the file is truncated or corrupt");
            const size_t nbytes = static_cast<size_t>(groups) * static_cast<size_t>(width);
            const uint8_t* src = b.take(nbytes);
            const uint64_t nvals = groups * 8;
            uint64_t bitpos = 0;
            for (uint64_t k = 0; k < nvals && out.size() < count; k++) {
                const size_t byte = static_cast<size_t>(bitpos >> 3);
                const size_t avail = std::min<size_t>(8, nbytes - byte);
                uint64_t w = 0;
                for (size_t q = 0; q < avail; q++) w |= static_cast<uint64_t>(src[byte + q]) << (8 * q);
                out.push_back(static_cast<uint32_t>((w >> (bitpos & 7)) & mask));
                bitpos += static_cast<uint64_t>(width);
            }
        } else {
            const uint64_t run = h >> 1;
            if (run == 0) fail("the data has an empty run");
            const uint8_t* vp = b.take(byte_w);
            uint64_t val = 0;
            for (size_t k = 0; k < byte_w; k++) val |= static_cast<uint64_t>(vp[k]) << (8 * k);
            if (val > mask) fail("a run value does not fit its bit width");
            const size_t take = static_cast<size_t>(std::min<uint64_t>(run, count - out.size()));
            out.insert(out.end(), take, static_cast<uint32_t>(val));
        }
    }
}

int bit_width_of(int max_value) {
    int w = 0;
    while ((1 << w) <= max_value) w++;
    return w;
}

void decode_plain(Buf& b, const Leaf& leaf, size_t count, Decoded& out) {
    switch (leaf.ptype) {
        case P_BOOLEAN: {
            const size_t bytes = count / 8 + (count % 8 ? 1 : 0);
            const uint8_t* src = b.take(bytes);
            out.i.resize(count);
            for (size_t k = 0; k < count; k++) out.i[k] = (src[k >> 3] >> (k & 7)) & 1;
            return;
        }
        case P_INT32: {
            if (count > b.left() / 4) fail("data ends unexpectedly, the file is truncated or corrupt");
            const uint8_t* src = b.take(count * 4);
            out.i.resize(count);
            const bool unsigned32 = leaf.converted == CT_UINT_32;
            for (size_t k = 0; k < count; k++) {
                const uint32_t u = ld32(src + k * 4);
                out.i[k] = unsigned32 ? static_cast<int64_t>(u) : static_cast<int64_t>(static_cast<int32_t>(u));
            }
            return;
        }
        case P_INT64: {
            if (count > b.left() / 8) fail("data ends unexpectedly, the file is truncated or corrupt");
            const uint8_t* src = b.take(count * 8);
            out.i.resize(count);
            for (size_t k = 0; k < count; k++) out.i[k] = static_cast<int64_t>(ld64(src + k * 8));
            return;
        }
        case P_FLOAT: {
            if (count > b.left() / 4) fail("data ends unexpectedly, the file is truncated or corrupt");
            const uint8_t* src = b.take(count * 4);
            out.d.resize(count);
            for (size_t k = 0; k < count; k++) {
                const uint32_t u = ld32(src + k * 4);
                float f;
                std::memcpy(&f, &u, 4);
                out.d[k] = static_cast<double>(f);
            }
            return;
        }
        case P_DOUBLE: {
            if (count > b.left() / 8) fail("data ends unexpectedly, the file is truncated or corrupt");
            const uint8_t* src = b.take(count * 8);
            out.d.resize(count);
            for (size_t k = 0; k < count; k++) out.d[k] = bits_to_double(ld64(src + k * 8));
            return;
        }
        case P_BYTE_ARRAY: {
            if (count > b.left() / 4) fail("data ends unexpectedly, the file is truncated or corrupt");
            out.s.reserve(count);
            for (size_t k = 0; k < count; k++) {
                const uint32_t len = b.u32();
                const uint8_t* s = b.take(len);
                out.s.emplace_back(reinterpret_cast<const char*>(s), len);
            }
            return;
        }
        default:
            fail("columns of type " + ptype_name(leaf.ptype) + " are not supported");
    }
}

void decode_byte_stream_split(Buf& b, const Leaf& leaf, size_t count, Decoded& out) {
    size_t width;
    switch (leaf.ptype) {
        case P_FLOAT:
        case P_INT32: width = 4; break;
        case P_DOUBLE:
        case P_INT64: width = 8; break;
        default: fail("BYTE_STREAM_SPLIT is not supported for " + ptype_name(leaf.ptype) + " columns");
    }
    if (count > b.left() / width) fail("data ends unexpectedly, the file is truncated or corrupt");
    const uint8_t* src = b.take(count * width);
    std::string interleaved(count * width, '\0');
    for (size_t k = 0; k < width; k++) {
        for (size_t i = 0; i < count; i++) interleaved[i * width + k] = static_cast<char>(src[k * count + i]);
    }
    Buf plain(reinterpret_cast<const uint8_t*>(interleaved.data()), interleaved.size());
    decode_plain(plain, leaf, count, out);
}

struct Span {
    const uint8_t* p;
    size_t n;
};

Span inflate_block(int codec, const uint8_t* in, size_t in_n, size_t out_n, std::vector<uint8_t>& scratch) {
    if (out_n > kMaxPageBytes) fail("a page is larger than the 1 GiB safety limit");
    if (codec == C_UNCOMPRESSED) {
        if (in_n != out_n) fail("an uncompressed page has two different sizes in its header");
        return {in, in_n};
    }
    scratch.assign(out_n, 0);
    if (codec == C_SNAPPY) compress::snappy_decompress(in, in_n, scratch.data(), out_n);
    else if (codec == C_GZIP) compress::gzip_decompress(in, in_n, scratch.data(), out_n);
    else fail("compression codec " + codec_name(codec) + " is not supported (supported: UNCOMPRESSED, SNAPPY, GZIP)");
    return {scratch.data(), out_n};
}

struct PageHeader {
    int type = -1;
    int64_t uncompressed = 0;
    int64_t compressed = 0;
    int64_t num_values = 0;
    int64_t num_nulls = 0;
    int64_t num_rows = 0;
    int encoding = 0;
    int64_t def_len = 0;
    int64_t rep_len = 0;
    bool is_compressed = true;
    bool has_header = false;
};

void parse_sub_header(TReader& t, PageHeader& h, int which) {
    if (++t.depth > 32) fail("the metadata is nested too deeply");
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        if (which == 5) {
            if (id == 1) h.num_values = t.integer(ty);
            else if (id == 2) h.encoding = static_cast<int>(t.integer(ty));
            else t.skip(ty);
        } else if (which == 7) {
            if (id == 1) h.num_values = t.integer(ty);
            else if (id == 2) h.encoding = static_cast<int>(t.integer(ty));
            else t.skip(ty);
        } else {
            if (id == 1) h.num_values = t.integer(ty);
            else if (id == 2) h.num_nulls = t.integer(ty);
            else if (id == 3) h.num_rows = t.integer(ty);
            else if (id == 4) h.encoding = static_cast<int>(t.integer(ty));
            else if (id == 5) h.def_len = t.integer(ty);
            else if (id == 6) h.rep_len = t.integer(ty);
            else if (id == 7) h.is_compressed = t.boolean(ty);
            else t.skip(ty);
        }
    }
    --t.depth;
    h.has_header = true;
}

PageHeader parse_page_header(Buf& b) {
    TReader t(b);
    PageHeader h;
    int last = 0, id, ty;
    while (t.next(last, id, ty)) {
        switch (id) {
            case 1: h.type = static_cast<int>(t.integer(ty)); break;
            case 2: h.uncompressed = t.integer(ty); break;
            case 3: h.compressed = t.integer(ty); break;
            case 5: parse_sub_header(t, h, 5); break;
            case 7: parse_sub_header(t, h, 7); break;
            case 8: parse_sub_header(t, h, 8); break;
            default: t.skip(ty);
        }
    }
    return h;
}

struct ColumnOut {
    int ptype = 0;
    bool use_f64 = false;
    std::vector<double> f;
    std::vector<Value> v;
    size_t size() const { return use_f64 ? f.size() : v.size(); }
};

struct Context {
    uint64_t string_budget = kMaxStringBytes;
};

void append_values(ColumnOut& col, Decoded& d, const std::vector<uint32_t>& defs, size_t n, int max_def,
                   size_t non_null, Context& ctx) {
    size_t vi = 0;
    auto is_null = [&](size_t k) { return max_def > 0 && defs[k] < static_cast<uint32_t>(max_def); };
    switch (col.ptype) {
        case P_FLOAT:
        case P_DOUBLE:
            if (d.d.size() != non_null) fail("a page has fewer values than its header declares");
            col.f.reserve(col.f.size() + n);
            for (size_t k = 0; k < n; k++) col.f.push_back(is_null(k) ? std::nan("") : d.d[vi++]);
            return;
        case P_BOOLEAN:
            if (d.i.size() != non_null) fail("a page has fewer values than its header declares");
            col.v.reserve(col.v.size() + n);
            for (size_t k = 0; k < n; k++) col.v.push_back(is_null(k) ? Value() : Value::make_bool(d.i[vi++] != 0));
            return;
        case P_INT32:
        case P_INT64:
            if (d.i.size() != non_null) fail("a page has fewer values than its header declares");
            col.v.reserve(col.v.size() + n);
            for (size_t k = 0; k < n; k++) col.v.push_back(is_null(k) ? Value() : Value::make_int(d.i[vi++]));
            return;
        case P_BYTE_ARRAY:
            if (d.s.size() != non_null) fail("a page has fewer values than its header declares");
            col.v.reserve(col.v.size() + n);
            for (size_t k = 0; k < n; k++) {
                if (is_null(k)) {
                    col.v.push_back(Value());
                } else {
                    std::string& s = d.s[vi++];
                    if (s.size() > ctx.string_budget) fail("the decoded string data is larger than the 1 GiB safety limit");
                    ctx.string_budget -= s.size();
                    col.v.push_back(Value::make_str(std::move(s)));
                }
            }
            return;
        default:
            fail("columns of type " + ptype_name(col.ptype) + " are not supported");
    }
}

void gather_dictionary(const Decoded& dict, const std::vector<uint32_t>& idx, int ptype, Decoded& out, Context& ctx) {
    size_t dict_size = 0;
    switch (ptype) {
        case P_FLOAT:
        case P_DOUBLE: dict_size = dict.d.size(); break;
        case P_BYTE_ARRAY: dict_size = dict.s.size(); break;
        default: dict_size = dict.i.size();
    }
    for (const uint32_t k : idx) {
        if (k >= dict_size) fail("a dictionary index points outside the dictionary, the file is corrupt");
    }
    switch (ptype) {
        case P_FLOAT:
        case P_DOUBLE:
            out.d.reserve(idx.size());
            for (const uint32_t k : idx) out.d.push_back(dict.d[k]);
            break;
        case P_BYTE_ARRAY: {
            uint64_t total = 0;
            for (const uint32_t k : idx) {
                total += dict.s[k].size();
                if (total > ctx.string_budget) fail("the decoded string data is larger than the 1 GiB safety limit");
            }
            out.s.reserve(idx.size());
            for (const uint32_t k : idx) out.s.push_back(dict.s[k]);
            break;
        }
        default:
            out.i.reserve(idx.size());
            for (const uint32_t k : idx) out.i.push_back(dict.i[k]);
    }
}

void read_chunk(std::ifstream& in, uint64_t fsize, const ColMeta& m, const Leaf& leaf, ColumnOut& out, int64_t expect_rows, Context& ctx) {
    if (!m.present) fail("column '" + leaf.name + "' has no metadata");
    if (m.external) fail("column '" + leaf.name + "' is stored in another file, which is not supported");
    if (m.type != leaf.ptype) fail("column '" + leaf.name + "' has a type that disagrees with the schema");
    if (m.codec != C_UNCOMPRESSED && m.codec != C_SNAPPY && m.codec != C_GZIP) {
        fail("column '" + leaf.name + "' uses " + codec_name(m.codec) +
             " compression, which is not supported (supported: UNCOMPRESSED, SNAPPY, GZIP)");
    }
    if (m.num_values != expect_rows) fail("column '" + leaf.name + "' has " + std::to_string(m.num_values) +
                                          " values but its row group has " + std::to_string(expect_rows) + " rows");
    int64_t start = m.data_off;
    if (m.dict_off > 0 && m.dict_off < m.data_off) start = m.dict_off;
    if (start < 4 || m.total_compressed < 0 || static_cast<uint64_t>(start) > fsize ||
        static_cast<uint64_t>(m.total_compressed) > fsize - static_cast<uint64_t>(start)) {
        fail("column '" + leaf.name + "' points outside the file");
    }
    if (static_cast<uint64_t>(m.total_compressed) > kMaxPageBytes) {
        fail("column '" + leaf.name + "' has a row group larger than 1 GiB, which is not supported");
    }
    std::string raw(static_cast<size_t>(m.total_compressed), '\0');
    in.clear();
    in.seekg(static_cast<std::streamoff>(start));
    in.read(&raw[0], static_cast<std::streamsize>(raw.size()));
    if (static_cast<size_t>(in.gcount()) != raw.size()) fail("column '" + leaf.name + "' could not be read, the file is truncated");
    Buf chunk(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    Decoded dict;
    bool have_dict = false;
    std::vector<uint8_t> scratch;
    std::vector<uint32_t> defs, indices;
    int64_t seen = 0;
    while (seen < m.num_values) {
        if (chunk.left() == 0) fail("column '" + leaf.name + "' ends before all of its values were read");
        PageHeader h = parse_page_header(chunk);
        if (h.compressed < 0 || static_cast<uint64_t>(h.compressed) > chunk.left()) fail("a page is larger than its column");
        if (h.uncompressed < 0 || static_cast<uint64_t>(h.uncompressed) > kMaxPageBytes) fail("a page is larger than the 1 GiB safety limit");
        const uint8_t* body = chunk.take(static_cast<size_t>(h.compressed));
        const size_t comp = static_cast<size_t>(h.compressed);
        const size_t uncomp = static_cast<size_t>(h.uncompressed);

        if (h.type == PG_DICT) {
            if (!h.has_header || h.num_values < 0 || static_cast<uint64_t>(h.num_values) > kMaxCells) fail("a dictionary page header is invalid");
            if (h.encoding != E_PLAIN && h.encoding != E_PLAIN_DICT) {
                fail("dictionary pages in " + encoding_name(h.encoding) + " are not supported");
            }
            const Span s = inflate_block(m.codec, body, comp, uncomp, scratch);
            Buf b(s.p, s.n);
            dict = Decoded();
            decode_plain(b, leaf, static_cast<size_t>(h.num_values), dict);
            have_dict = true;
            continue;
        }
        if (h.type != PG_DATA && h.type != PG_DATA_V2) continue;
        if (!h.has_header) fail("a data page has no header");
        if (h.num_values < 0 || h.num_values > m.num_values - seen) fail("a page holds more values than its column");
        const size_t n = static_cast<size_t>(h.num_values);

        Span values{nullptr, 0};
        defs.clear();
        if (h.type == PG_DATA) {
            const Span s = inflate_block(m.codec, body, comp, uncomp, scratch);
            Buf b(s.p, s.n);
            if (leaf.max_def > 0) {
                const uint32_t len = b.u32();
                const uint8_t* level_bytes = b.take(len);
                Buf lv(level_bytes, len);
                decode_hybrid(lv, bit_width_of(leaf.max_def), n, defs);
            }
            values = {b.p + b.pos, b.left()};
        } else {
            if (h.rep_len != 0) fail("column '" + leaf.name + "' has repetition levels, nested columns are not supported");
            if (h.def_len < 0 || static_cast<uint64_t>(h.def_len) > comp || static_cast<uint64_t>(h.def_len) > uncomp) {
                fail("a data page has invalid level sizes");
            }
            const size_t dl = static_cast<size_t>(h.def_len);
            if (leaf.max_def > 0) {
                Buf lv(body, dl);
                decode_hybrid(lv, bit_width_of(leaf.max_def), n, defs);
            } else if (dl != 0) {
                fail("a data page has definition levels for a required column");
            }
            const bool compressed = h.is_compressed && m.codec != C_UNCOMPRESSED;
            values = inflate_block(compressed ? m.codec : C_UNCOMPRESSED, body + dl, comp - dl, uncomp - dl, scratch);
        }
        for (const uint32_t level : defs) {
            if (level > static_cast<uint32_t>(leaf.max_def)) fail("a definition level is out of range, the file is corrupt");
        }
        size_t non_null = n;
        if (leaf.max_def > 0) {
            non_null = 0;
            for (const uint32_t level : defs) non_null += (level == static_cast<uint32_t>(leaf.max_def)) ? 1 : 0;
        }

        Buf vb(values.p, values.n);
        Decoded d;
        if (h.encoding == E_PLAIN) {
            decode_plain(vb, leaf, non_null, d);
        } else if (h.encoding == E_PLAIN_DICT || h.encoding == E_RLE_DICT) {
            if (!have_dict) fail("column '" + leaf.name + "' uses a dictionary but has no dictionary page");
            const int width = vb.u8();
            decode_hybrid(vb, width, non_null, indices);
            gather_dictionary(dict, indices, leaf.ptype, d, ctx);
        } else if (h.encoding == E_RLE && leaf.ptype == P_BOOLEAN) {
            const uint32_t len = vb.u32();
            Buf rb(vb.take(len), len);
            decode_hybrid(rb, 1, non_null, indices);
            d.i.assign(indices.begin(), indices.end());
        } else if (h.encoding == E_BYTE_STREAM_SPLIT) {
            decode_byte_stream_split(vb, leaf, non_null, d);
        } else {
            fail("column '" + leaf.name + "' uses the " + encoding_name(h.encoding) + " encoding, which is not supported yet");
        }
        append_values(out, d, defs, n, leaf.max_def, non_null, ctx);
        seen += h.num_values;
    }
    if (static_cast<int64_t>(out.size()) < expect_rows) fail("column '" + leaf.name + "' has fewer values than its row group");
}

std::string join_names(const std::vector<Leaf>& leaves) {
    std::string s;
    size_t shown = 0;
    for (const auto& l : leaves) {
        if (shown == 20) { s += ", ..."; break; }
        if (shown) s += ", ";
        s += l.name;
        shown++;
    }
    return s;
}

}

namespace {

void read_at(std::ifstream& in, uint64_t offset, void* dst, size_t n, const std::string& path) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (static_cast<size_t>(in.gcount()) != n) fail("cannot read '" + path + "', the file is truncated or unreadable");
}

}

struct Reader::Impl {
    std::ifstream in;
    uint64_t fsize = 0;
    std::string path;
    FileMeta fm;
    std::vector<Leaf> leaves;
    std::vector<size_t> selected;
    std::vector<std::string> names;
    ReadOptions options;
    long long total_rows = 0;

    std::vector<ColumnOut> make_outs() const {
        std::vector<ColumnOut> outs(selected.size());
        for (size_t j = 0; j < selected.size(); j++) {
            outs[j].ptype = leaves[selected[j]].ptype;
            outs[j].use_f64 = outs[j].ptype == P_FLOAT || outs[j].ptype == P_DOUBLE;
            if (outs[j].ptype == P_INT96 || outs[j].ptype == P_FIXED) {
                fail("column '" + leaves[selected[j]].name + "' has type " + ptype_name(outs[j].ptype) + ", which is not supported");
            }
        }
        return outs;
    }

    void read_group_into(size_t g, std::vector<ColumnOut>& outs, Context& ctx) {
        const RowGroup& group = fm.groups[g];
        for (size_t j = 0; j < selected.size(); j++) {
            read_chunk(in, fsize, group.cols[selected[j]], leaves[selected[j]], outs[j], group.num_rows, ctx);
        }
    }

    Value table_from(std::vector<ColumnOut>& outs, long long rows) const {
        std::unordered_map<std::string, Value> columns;
        std::vector<Value> name_values;
        for (size_t j = 0; j < selected.size(); j++) {
            const std::string& name = names[j];
            name_values.push_back(Value::make_str(name));
            if (outs[j].use_f64) {
                Value arr = Value::make_f64_array(0);
                arr.f64_array_val->swap(outs[j].f);
                columns[name] = std::move(arr);
            } else {
                columns[name] = Value::make_vec(std::move(outs[j].v));
            }
        }
        std::unordered_map<std::string, Value> table;
        table["names"] = Value::make_vec(std::move(name_values));
        table["rows"] = Value::make_int(rows);
        table["columns"] = Value::make_dict(std::move(columns));
        return Value::make_dict(std::move(table));
    }
};

Reader::Reader(const std::string& path, const ReadOptions& options) : impl_(new Impl) {
    Impl& d = *impl_;
    d.path = path;
    d.options = options;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) fail("load_parquet: '" + path + "' is a directory, not a file");
    d.in.open(path, std::ios::binary);
    if (!d.in) {
        fail("load_parquet: cannot open '" + path + "' (" +
             (std::filesystem::exists(path, ec) ? "permission denied or unreadable" : "no such file") + ")");
    }
    d.fsize = std::filesystem::file_size(path, ec);
    if (ec) fail("cannot read the size of '" + path + "'");
    if (d.fsize < 12) fail("'" + path + "' is not a parquet file (it is too small)");
    uint8_t head[4];
    uint8_t tail[8];
    read_at(d.in, 0, head, 4, path);
    read_at(d.in, d.fsize - 8, tail, 8, path);
    if (std::memcmp(head, "PAR1", 4) != 0) {
        if (std::memcmp(head, "PARE", 4) == 0) fail("encrypted parquet files are not supported");
        fail("'" + path + "' is not a parquet file (missing the PAR1 header)");
    }
    if (std::memcmp(tail + 4, "PAR1", 4) != 0) {
        if (std::memcmp(tail + 4, "PARE", 4) == 0) fail("encrypted parquet files are not supported");
        fail("'" + path + "' is truncated or damaged (missing the PAR1 footer)");
    }
    const uint32_t footer_len = ld32(tail);
    if (footer_len > d.fsize - 12 || footer_len > kMaxFooterBytes) fail("the footer size is invalid, the file is corrupt");
    std::string footer(footer_len, '\0');
    read_at(d.in, d.fsize - 8 - footer_len, &footer[0], footer_len, path);
    Buf meta_buf(reinterpret_cast<const uint8_t*>(footer.data()), footer.size());
    TReader tr(meta_buf);
    d.fm = parse_file_meta(tr);

    if (d.fm.schema.empty()) fail("the file has no schema");
    {
        size_t idx = 1;
        std::vector<std::string> path_stack;
        walk_schema(d.fm, idx, d.fm.schema[0].children, path_stack, 0, 0, 0, d.leaves);
    }
    if (d.leaves.size() > kMaxColumns) fail("the file has more than 65536 columns");

    if (options.has_columns) {
        std::unordered_set<std::string> seen_names;
        for (const auto& want : options.columns) {
            if (!seen_names.insert(want).second) fail("column '" + want + "' was requested twice");
            size_t found = d.leaves.size();
            for (size_t k = 0; k < d.leaves.size(); k++) {
                if (d.leaves[k].name == want) { found = k; break; }
            }
            if (found == d.leaves.size()) fail("the file has no column '" + want + "' (columns: " + join_names(d.leaves) + ")");
            if (!d.leaves[found].flat()) fail("column '" + want + "' is nested or repeated, which is not supported");
            d.selected.push_back(found);
        }
    } else {
        for (size_t k = 0; k < d.leaves.size(); k++) {
            if (!d.leaves[k].flat()) {
                fail("column '" + d.leaves[k].name + "' is nested or repeated, which is not supported; "
                     "pass a list with the flat columns you want");
            }
            d.selected.push_back(k);
        }
    }
    for (const size_t k : d.selected) d.names.push_back(d.leaves[k].name);
    d.make_outs();
    for (const RowGroup& g : d.fm.groups) {
        if (g.num_rows < 0) fail("a row group has a negative row count");
        if (g.cols.size() != d.leaves.size()) fail("a row group has a different number of columns than the schema");
        d.total_rows += g.num_rows;
    }
}

Reader::~Reader() = default;

const std::vector<std::string>& Reader::names() const { return impl_->names; }
long long Reader::rows() const { return impl_->total_rows; }
size_t Reader::groups() const { return impl_->fm.groups.size(); }
long long Reader::group_rows(size_t g) const {
    if (g >= impl_->fm.groups.size()) fail("row group index is out of range");
    return impl_->fm.groups[g].num_rows;
}

Value Reader::read_group(size_t g) {
    Impl& d = *impl_;
    if (g >= d.fm.groups.size()) fail("row group index is out of range");
    const RowGroup& group = d.fm.groups[g];
    if (!d.selected.empty() && static_cast<uint64_t>(group.num_rows) > kMaxCells / d.selected.size()) {
        fail("a row group is larger than " + std::to_string(kMaxCells) + " cells; load fewer columns");
    }
    std::vector<ColumnOut> outs = d.make_outs();
    Context ctx;
    d.read_group_into(g, outs, ctx);
    return d.table_from(outs, group.num_rows);
}

Value Reader::read_all() {
    Impl& d = *impl_;
    if (d.options.max_rows <= 0 && !d.selected.empty() &&
        static_cast<uint64_t>(d.total_rows) > kMaxCells / d.selected.size()) {
        fail("the table is larger than " + std::to_string(kMaxCells) +
             " cells; load fewer columns, use max_rows, or read it in batches with open_dataset");
    }
    std::vector<ColumnOut> outs = d.make_outs();
    Context ctx;
    int64_t total_rows = 0;
    for (size_t g = 0; g < d.fm.groups.size(); g++) {
        const RowGroup& group = d.fm.groups[g];
        const uint64_t rows_after = static_cast<uint64_t>(total_rows) + static_cast<uint64_t>(group.num_rows);
        if (!d.selected.empty() && rows_after > kMaxCells / d.selected.size()) {
            fail("the table is larger than " + std::to_string(kMaxCells) +
                 " cells; load fewer columns, use max_rows, or read it in batches with open_dataset");
        }
        d.read_group_into(g, outs, ctx);
        total_rows += group.num_rows;
        if (d.options.max_rows > 0 && total_rows >= d.options.max_rows) break;
    }
    if (d.options.max_rows > 0 && total_rows > d.options.max_rows) {
        total_rows = d.options.max_rows;
        for (auto& c : outs) {
            if (c.use_f64) c.f.resize(static_cast<size_t>(total_rows));
            else c.v.resize(static_cast<size_t>(total_rows));
        }
    }
    return d.table_from(outs, total_rows);
}

Value read(const std::string& path, const ReadOptions& options) {
    Reader reader(path, options);
    return reader.read_all();
}


namespace {

class TWriter {
public:
    std::string out;

    void begin() { last_.push_back(0); }
    void end() {
        out.push_back(0);
        last_.pop_back();
    }
    void field(int id, int type) {
        const int delta = id - last_.back();
        if (delta > 0 && delta <= 15) {
            out.push_back(static_cast<char>((delta << 4) | type));
        } else {
            out.push_back(static_cast<char>(type));
            varint(zz(id));
        }
        last_.back() = id;
    }
    void i32(int id, int32_t v) {
        field(id, T_I32);
        varint(zz(v));
    }
    void i64(int id, int64_t v) {
        field(id, T_I64);
        varint(zz(v));
    }
    void str(int id, const std::string& s) {
        field(id, T_BINARY);
        varint(s.size());
        out += s;
    }
    void list(int id, int etype, size_t n) {
        field(id, T_LIST);
        if (n < 15) {
            out.push_back(static_cast<char>((n << 4) | etype));
        } else {
            out.push_back(static_cast<char>(0xF0 | etype));
            varint(n);
        }
    }
    void elem_i32(int32_t v) { varint(zz(v)); }
    void elem_str(const std::string& s) {
        varint(s.size());
        out += s;
    }
    void struct_field(int id) {
        field(id, T_STRUCT);
        begin();
    }

private:
    std::vector<int> last_;
    static uint64_t zz(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
    void varint(uint64_t v) {
        while (v >= 0x80) {
            out.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        out.push_back(static_cast<char>(v));
    }
};

struct ColSpec {
    std::string name;
    int ptype = P_DOUBLE;
    bool optional = false;
    bool is_string = false;
    const Value* col = nullptr;
    size_t rows = 0;
};

ColSpec classify(const std::string& name, const Value& col) {
    ColSpec s;
    s.name = name;
    s.col = &col;
    if (col.type == Value::F64Array) {
        s.ptype = P_DOUBLE;
        s.rows = col.f64_array_val->size();
        return s;
    }
    if (col.type != Value::Vec) {
        fail("column '" + name + "' must be an f64_array or a vector, not a " + col.type_name());
    }
    const auto& v = *col.vec_val;
    s.rows = v.size();
    size_t nbool = 0, nint = 0, nfloat = 0, nstr = 0, nnone = 0;
    for (const Value& e : v) {
        switch (e.type) {
            case Value::Bool: nbool++; break;
            case Value::Int: nint++; break;
            case Value::Float: nfloat++; break;
            case Value::Str: nstr++; break;
            case Value::None: nnone++; break;
            default:
                fail("column '" + name + "' holds a " + e.type_name() +
                     ", but parquet columns can only hold numbers, bools and strings");
        }
    }
    if (v.empty()) {
        s.ptype = P_DOUBLE;
        return s;
    }
    if (nnone == v.size()) fail("column '" + name + "' has only none values, so its type cannot be decided");
    const size_t kinds = (nbool > 0) + (nint + nfloat > 0) + (nstr > 0);
    if (kinds > 1) fail("column '" + name + "' mixes bools, numbers or strings, a column needs one kind of value");
    s.optional = nnone > 0;
    if (nbool) {
        s.ptype = P_BOOLEAN;
    } else if (nstr) {
        s.ptype = P_BYTE_ARRAY;
        s.is_string = true;
        for (const Value& e : v) {
            if (e.type == Value::Str && !json::valid_utf8(e.str_val->data(), e.str_val->size())) {
                s.is_string = false;
                break;
            }
        }
    } else if (nfloat) {
        s.ptype = P_DOUBLE;
        if (nint) {
            for (const Value& e : v) {
                if (e.type == Value::Int && (e.int_val > (1LL << 53) || e.int_val < -(1LL << 53))) {
                    fail("column '" + name + "' mixes floats with an integer too large to keep exactly as a double");
                }
            }
        }
    } else {
        s.ptype = P_INT64;
    }
    return s;
}

void put_varint(std::string& o, uint64_t v) {
    while (v >= 0x80) {
        o.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    o.push_back(static_cast<char>(v));
}

std::string build_page_body(const ColSpec& s, size_t r0, size_t r1) {
    std::string body;
    const Value& col = *s.col;
    const std::vector<Value>* vec = col.type == Value::Vec ? &*col.vec_val : nullptr;
    if (s.optional) {
        std::string levels;
        size_t k = r0;
        while (k < r1) {
            const bool defined = (*vec)[k].type != Value::None;
            size_t j = k + 1;
            while (j < r1 && ((*vec)[j].type != Value::None) == defined) j++;
            put_varint(levels, static_cast<uint64_t>(j - k) << 1);
            levels.push_back(defined ? 1 : 0);
            k = j;
        }
        st32(body, static_cast<uint32_t>(levels.size()));
        body += levels;
    }
    switch (s.ptype) {
        case P_BOOLEAN: {
            uint8_t cur = 0;
            int bit = 0;
            for (size_t k = r0; k < r1; k++) {
                const Value& e = (*vec)[k];
                if (e.type == Value::None) continue;
                if (e.bool_val) cur |= static_cast<uint8_t>(1u << bit);
                if (++bit == 8) {
                    body.push_back(static_cast<char>(cur));
                    cur = 0;
                    bit = 0;
                }
            }
            if (bit) body.push_back(static_cast<char>(cur));
            break;
        }
        case P_INT64:
            for (size_t k = r0; k < r1; k++) {
                const Value& e = (*vec)[k];
                if (e.type == Value::None) continue;
                st64(body, static_cast<uint64_t>(e.int_val));
            }
            break;
        case P_DOUBLE:
            if (vec == nullptr) {
                const auto& a = *col.f64_array_val;
                for (size_t k = r0; k < r1; k++) st64(body, double_to_bits(a[k]));
            } else {
                for (size_t k = r0; k < r1; k++) {
                    const Value& e = (*vec)[k];
                    if (e.type == Value::None) continue;
                    st64(body, double_to_bits(e.type == Value::Int ? static_cast<double>(e.int_val) : e.float_val));
                }
            }
            break;
        case P_BYTE_ARRAY:
            for (size_t k = r0; k < r1; k++) {
                const Value& e = (*vec)[k];
                if (e.type == Value::None) continue;
                if (e.str_val->size() > 0x7FFFFFFFu) fail("a string is too long for a parquet value");
                st32(body, static_cast<uint32_t>(e.str_val->size()));
                body += *e.str_val;
            }
            break;
    }
    return body;
}

struct ChunkInfo {
    int64_t offset = 0;
    int64_t size = 0;
    int64_t num_values = 0;
};

}


namespace {

void extract_table(const Value& table, std::vector<std::string>& names, const std::unordered_map<std::string, Value>*& cols) {
    if (table.type != Value::Dict) {
        fail("a table must be a dict of columns, or the dict returned by load_parquet");
    }
    cols = &*table.dict_val;
    auto cit = table.dict_val->find("columns");
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
            return;
        }
    }
    for (const auto& kv : *cols) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
}

void validate_names(const std::vector<std::string>& names, const std::unordered_map<std::string, Value>& cols) {
    if (names.empty()) fail("the table has no columns");
    if (names.size() > kMaxColumns) fail("the table has more than 65536 columns");
    std::unordered_set<std::string> seen;
    for (const std::string& name : names) {
        if (name.empty()) fail("a column name is empty");
        if (!json::valid_utf8(name.data(), name.size())) fail("a column name is not valid UTF-8");
        if (!seen.insert(name).second) fail("column '" + name + "' is listed twice");
        if (cols.find(name) == cols.end()) fail("table 'names' lists '" + name + "' but there is no such column");
    }
}

size_t column_rows(const std::string& name, const Value& col) {
    if (col.type == Value::F64Array) return col.f64_array_val->size();
    if (col.type == Value::Vec) return col.vec_val->size();
    fail("column '" + name + "' must be an f64_array or a vector, not a " + col.type_name());
}

std::string build_footer(const std::vector<ColSpec>& specs, const std::vector<std::vector<ChunkInfo>>& groups,
                         const std::vector<int64_t>& group_rows, uint64_t rows) {
    TWriter m;
    m.begin();
    m.i32(1, 1);
    m.list(2, T_STRUCT, specs.size() + 1);
    m.begin();
    m.str(4, "schema");
    m.i32(5, static_cast<int32_t>(specs.size()));
    m.end();
    for (const ColSpec& s : specs) {
        m.begin();
        m.i32(1, s.ptype);
        m.i32(3, s.optional ? R_OPTIONAL : R_REQUIRED);
        m.str(4, s.name);
        if (s.is_string) {
            m.i32(6, CT_UTF8);
            m.struct_field(10);
            m.struct_field(1);
            m.end();
            m.end();
        }
        m.end();
    }
    m.i64(3, static_cast<int64_t>(rows));
    m.list(4, T_STRUCT, groups.size());
    for (size_t g = 0; g < groups.size(); g++) {
        m.begin();
        m.list(1, T_STRUCT, specs.size());
        int64_t group_bytes = 0;
        for (size_t c = 0; c < specs.size(); c++) {
            const ChunkInfo& ci = groups[g][c];
            group_bytes += ci.size;
            m.begin();
            m.i64(2, ci.offset);
            m.struct_field(3);
            m.i32(1, specs[c].ptype);
            if (specs[c].optional) {
                m.list(2, T_I32, 2);
                m.elem_i32(E_PLAIN);
                m.elem_i32(E_RLE);
            } else {
                m.list(2, T_I32, 1);
                m.elem_i32(E_PLAIN);
            }
            m.list(3, T_BINARY, 1);
            m.elem_str(specs[c].name);
            m.i32(4, C_UNCOMPRESSED);
            m.i64(5, ci.num_values);
            m.i64(6, ci.size);
            m.i64(7, ci.size);
            m.i64(9, ci.offset);
            m.end();
            m.end();
        }
        m.i64(2, group_bytes);
        m.i64(3, group_rows[g]);
        m.end();
    }
    m.str(6, "csyc");
    m.end();
    return m.out;
}

constexpr size_t kFlushCells = 8u << 20;

}

struct Writer::Impl {
    Impl(const std::string& path, bool oneshot_mode) : out(path, "save_parquet"), oneshot(oneshot_mode) { put("PAR1"); }

    dataio::AtomicWriter out;
    bool oneshot;
    uint64_t offset = 0;
    bool started = false;
    std::vector<ColSpec> specs;
    std::vector<std::vector<ChunkInfo>> groups;
    std::vector<int64_t> group_rows;
    uint64_t total_rows = 0;
    std::vector<std::vector<Value>> pending;
    size_t pending_rows = 0;

    void put(const std::string& s) {
        out.write(s);
        offset += s.size();
    }

    void write_group(const std::vector<const Value*>& cols, size_t r0, size_t r1) {
        std::vector<ChunkInfo> chunks;
        for (size_t c = 0; c < specs.size(); c++) {
            ColSpec s = specs[c];
            s.col = cols[c];
            const std::string body = build_page_body(s, r0, r1);
            if (body.size() > 0x7FFFFFF0u) {
                fail("column '" + s.name + "' would need a page larger than 2 GiB, use smaller batches");
            }
            TWriter h;
            h.begin();
            h.i32(1, PG_DATA);
            h.i32(2, static_cast<int32_t>(body.size()));
            h.i32(3, static_cast<int32_t>(body.size()));
            h.struct_field(5);
            h.i32(1, static_cast<int32_t>(r1 - r0));
            h.i32(2, E_PLAIN);
            h.i32(3, E_RLE);
            h.i32(4, E_RLE);
            h.end();
            h.end();
            ChunkInfo ci;
            ci.offset = static_cast<int64_t>(offset);
            ci.num_values = static_cast<int64_t>(r1 - r0);
            put(h.out);
            put(body);
            ci.size = static_cast<int64_t>(h.out.size() + body.size());
            chunks.push_back(ci);
        }
        groups.push_back(std::move(chunks));
        group_rows.push_back(static_cast<int64_t>(r1 - r0));
    }

    void append_oneshot(const Value& table) {
        if (started) fail("save_parquet writes one table, use create_dataset to write in batches");
        std::vector<std::string> names;
        const std::unordered_map<std::string, Value>* cols = nullptr;
        extract_table(table, names, cols);
        validate_names(names, *cols);
        for (const std::string& name : names) specs.push_back(classify(name, cols->find(name)->second));
        const size_t rows = specs[0].rows;
        for (const ColSpec& s : specs) {
            if (s.rows != rows) {
                fail("column '" + s.name + "' has " + std::to_string(s.rows) + " rows but column '" + specs[0].name +
                     "' has " + std::to_string(rows));
            }
        }
        if (rows > kMaxCells / specs.size()) fail("the table is larger than " + std::to_string(kMaxCells) + " cells");
        started = true;
        std::vector<const Value*> ptrs;
        for (const ColSpec& s : specs) ptrs.push_back(s.col);
        for (size_t r0 = 0; r0 < rows; r0 += kRowGroupRows) write_group(ptrs, r0, std::min(rows, r0 + kRowGroupRows));
        total_rows = rows;
    }

    void push_column(const ColSpec& s, const Value& col, std::vector<Value>& dst) {
        auto mismatch = [&](const Value& e) {
            fail("column '" + s.name + "' holds a " + e.type_name() + " in this batch, but the first batch fixed its type as " +
                 ptype_name(s.ptype) + (s.ptype == P_INT64 ? " (if the column is really floats, start with a float value)" : ""));
        };
        if (col.type == Value::F64Array) {
            if (s.ptype != P_DOUBLE) fail("column '" + s.name + "' is an f64_array in this batch, but the first batch fixed its type as " + ptype_name(s.ptype));
            dst.reserve(dst.size() + col.f64_array_val->size());
            for (const double x : *col.f64_array_val) dst.push_back(Value::make_float(x));
            return;
        }
        dst.reserve(dst.size() + col.vec_val->size());
        for (const Value& e : *col.vec_val) {
            switch (e.type) {
                case Value::None: break;
                case Value::Bool: if (s.ptype != P_BOOLEAN) mismatch(e); break;
                case Value::Int:
                    if (s.ptype == P_DOUBLE) {
                        if (e.int_val > (1LL << 53) || e.int_val < -(1LL << 53)) {
                            fail("column '" + s.name + "' mixes floats with an integer too large to keep exactly as a double");
                        }
                    } else if (s.ptype != P_INT64) {
                        mismatch(e);
                    }
                    break;
                case Value::Float: if (s.ptype != P_DOUBLE) mismatch(e); break;
                case Value::Str:
                    if (s.ptype != P_BYTE_ARRAY) mismatch(e);
                    if (s.is_string && !json::valid_utf8(e.str_val->data(), e.str_val->size())) {
                        fail("column '" + s.name + "' has a string that is not valid UTF-8, but the first batch made it a text column");
                    }
                    break;
                default: mismatch(e);
            }
            dst.push_back(e);
        }
    }

    void flush_pending() {
        if (pending_rows == 0) return;
        std::vector<Value> colvals;
        colvals.reserve(specs.size());
        for (auto& p : pending) {
            colvals.push_back(Value::make_vec(std::move(p)));
            p.clear();
        }
        std::vector<const Value*> ptrs;
        for (const Value& v : colvals) ptrs.push_back(&v);
        for (size_t r0 = 0; r0 < pending_rows; r0 += kRowGroupRows) write_group(ptrs, r0, std::min(pending_rows, r0 + kRowGroupRows));
        total_rows += pending_rows;
        pending_rows = 0;
    }

    void append_stream(const Value& table) {
        std::vector<std::string> names;
        const std::unordered_map<std::string, Value>* cols = nullptr;
        extract_table(table, names, cols);
        validate_names(names, *cols);
        std::vector<ColSpec> fresh;
        const std::vector<ColSpec>* use = &specs;
        if (!started) {
            for (const std::string& name : names) {
                ColSpec s = classify(name, cols->find(name)->second);
                s.optional = true;
                s.col = nullptr;
                fresh.push_back(s);
            }
            use = &fresh;
        } else {
            if (names.size() != specs.size()) fail("this batch has " + std::to_string(names.size()) + " columns but the first batch had " + std::to_string(specs.size()));
            for (size_t c = 0; c < names.size(); c++) {
                if (names[c] != specs[c].name) fail("the columns of this batch differ from the first batch (expected '" + specs[c].name + "' at position " + std::to_string(c) + ")");
            }
        }
        const size_t rows = column_rows(names[0], cols->find(names[0])->second);
        for (const std::string& name : names) {
            if (column_rows(name, cols->find(name)->second) != rows) {
                fail("column '" + name + "' has a different number of rows than column '" + names[0] + "' in this batch");
            }
        }
        std::vector<std::vector<Value>> staged(use->size());
        for (size_t c = 0; c < use->size(); c++) push_column((*use)[c], cols->find((*use)[c].name)->second, staged[c]);
        if (!started) {
            specs = std::move(fresh);
            pending.resize(specs.size());
            started = true;
        }
        for (size_t c = 0; c < specs.size(); c++) {
            if (pending[c].empty()) pending[c] = std::move(staged[c]);
            else pending[c].insert(pending[c].end(), staged[c].begin(), staged[c].end());
        }
        pending_rows += rows;
        if (pending_rows >= kRowGroupRows || pending_rows * specs.size() >= kFlushCells) flush_pending();
    }

    void finish() {
        if (!started) fail("no table was written, so the schema is unknown");
        flush_pending();
        const std::string footer = build_footer(specs, groups, group_rows, total_rows);
        put(footer);
        std::string tail;
        st32(tail, static_cast<uint32_t>(footer.size()));
        tail += "PAR1";
        put(tail);
        out.commit();
    }
};

Writer::Writer(const std::string& path, bool streaming) : impl_(new Impl(path, !streaming)) {}
Writer::~Writer() = default;

void Writer::append(const Value& table) {
    if (impl_->oneshot) impl_->append_oneshot(table);
    else impl_->append_stream(table);
}

void Writer::finish() { impl_->finish(); }

void write(const std::string& path, const Value& table) {
    Writer w(path, false);
    w.append(table);
    w.finish();
}

}
}
