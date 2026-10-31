#include "compress.hpp"
#include "fasthash.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace csy {
namespace compress {
namespace {

constexpr size_t kBlockMax = 128 * 1024;
constexpr unsigned kHufMaxBits = 11;
constexpr uint32_t kFrameMagic = 0xFD2FB528u;
constexpr uint32_t kSkippableMask = 0xFFFFFFF0u;
constexpr uint32_t kSkippableMagic = 0x184D2A50u;

[[noreturn]] void fail(const char* why) {
    throw std::runtime_error(std::string("zstd: ") + why);
}

const uint32_t kLlBase[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t kLlBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint32_t kMlBase[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t kMlBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const int16_t kLlDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int16_t kMlDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int16_t kOfDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

inline unsigned highbit(uint32_t v) {
    return 31u - static_cast<unsigned>(__builtin_clz(v));
}

class BackReader {
public:
    BackReader(const uint8_t* data, size_t len) : start_(data), small_(len < 8) {
        if (len == 0) fail("empty bitstream");
        const uint8_t last = data[len - 1];
        if (last == 0) fail("bitstream has no end marker");
        const unsigned pad = 8u - highbit(last);
        if (!small_) {
            ptr_ = data + len - 8;
            std::memcpy(&cont_, ptr_, 8);
            consumed_ = pad;
        } else {
            ptr_ = data;
            cont_ = 0;
            for (size_t i = 0; i < len; i++) cont_ |= static_cast<uint64_t>(data[i]) << (8 * i);
            consumed_ = pad + static_cast<unsigned>(8 - len) * 8u;
        }
    }

    inline void reload() {
        if (small_) return;
        const unsigned nbytes = consumed_ >> 3;
        if (nbytes == 0) return;
        const size_t room = static_cast<size_t>(ptr_ - start_);
        if (nbytes <= room) {
            ptr_ -= nbytes;
            consumed_ &= 7u;
        } else {
            ptr_ = start_;
            consumed_ -= static_cast<unsigned>(room) * 8u;
        }
        std::memcpy(&cont_, ptr_, 8);
    }

    inline uint32_t peek(unsigned nb) {
        if (consumed_ + nb > 64) reload();
        if (consumed_ >= 64) return 0;
        return static_cast<uint32_t>((cont_ << consumed_) >> (64 - nb));
    }

    inline void skip(unsigned nb) { consumed_ += nb; }

    inline uint32_t read(unsigned nb) {
        if (nb == 0) return 0;
        const uint32_t v = peek(nb);
        consumed_ += nb;
        return v;
    }

    int64_t remaining() const {
        return static_cast<int64_t>(ptr_ - start_) * 8 + 64 - static_cast<int64_t>(consumed_);
    }

    inline bool fast_ok() const { return static_cast<size_t>(ptr_ - start_) >= 8; }

    inline bool fast_ok_seq() const { return static_cast<size_t>(ptr_ - start_) >= 16; }

    inline uint32_t take_fast(unsigned nb) {
        const uint32_t v = static_cast<uint32_t>(((cont_ << consumed_) >> 1) >> (63u - nb));
        consumed_ += nb;
        return v;
    }

    inline void reload_fast() {
        ptr_ -= consumed_ >> 3;
        consumed_ &= 7u;
        std::memcpy(&cont_, ptr_, 8);
    }

    inline uint32_t peek_fast(unsigned nb) const {
        return static_cast<uint32_t>((cont_ << consumed_) >> (64 - nb));
    }

private:
    const uint8_t* start_;
    const uint8_t* ptr_ = nullptr;
    uint64_t cont_ = 0;
    unsigned consumed_ = 0;
    bool small_;
};

struct FseTable {
    uint8_t al = 0;
    uint8_t sym[512];
    uint8_t nb[512];
    uint16_t base[512];
};

void build_fse(FseTable& t, const int16_t* norm, unsigned nsyms, unsigned al) {
    const uint32_t size = 1u << al;
    const uint32_t mask = size - 1;
    uint32_t next[256];
    int64_t total = 0;
    int64_t high = static_cast<int64_t>(size) - 1;
    for (unsigned s = 0; s < nsyms; s++) {
        if (norm[s] == -1) {
            if (high < 0) fail("corrupt FSE table");
            t.sym[high--] = static_cast<uint8_t>(s);
            next[s] = 1;
            total += 1;
        } else {
            next[s] = static_cast<uint32_t>(norm[s]);
            total += norm[s];
        }
    }
    if (total != static_cast<int64_t>(size)) fail("corrupt FSE table");
    const uint32_t step = (size >> 1) + (size >> 3) + 3;
    uint32_t pos = 0;
    for (unsigned s = 0; s < nsyms; s++) {
        for (int16_t i = 0; i < norm[s]; i++) {
            t.sym[pos] = static_cast<uint8_t>(s);
            do {
                pos = (pos + step) & mask;
            } while (static_cast<int64_t>(pos) > high);
        }
    }
    if (pos != 0) fail("corrupt FSE table");
    for (uint32_t u = 0; u < size; u++) {
        const uint32_t ns = next[t.sym[u]]++;
        const unsigned nbits = al - highbit(ns);
        t.nb[u] = static_cast<uint8_t>(nbits);
        t.base[u] = static_cast<uint16_t>((ns << nbits) - size);
    }
    t.al = static_cast<uint8_t>(al);
}

size_t read_fse_header(const uint8_t* in, size_t n, unsigned max_al, unsigned max_sym, int16_t* norm,
                       unsigned& nsyms, unsigned& al_out) {
    size_t bitpos = 0;
    const size_t total_bits = n * 8;
    auto get = [&](unsigned k) -> uint32_t {
        if (bitpos + k > total_bits) fail("truncated FSE table");
        uint32_t v = 0;
        unsigned done = 0;
        while (done < k) {
            const size_t byte = bitpos >> 3;
            const unsigned shift = static_cast<unsigned>(bitpos & 7);
            const unsigned take = std::min(8 - shift, k - done);
            v |= ((static_cast<uint32_t>(in[byte]) >> shift) & ((1u << take) - 1)) << done;
            done += take;
            bitpos += take;
        }
        return v;
    };
    const unsigned al = get(4) + 5;
    if (al > max_al) fail("FSE accuracy log is too large");
    int32_t remaining = (1 << al) + 1;
    uint32_t threshold = 1u << al;
    unsigned nbits = al + 1;
    unsigned sym = 0;
    bool prev_zero = false;
    while (remaining > 1 && sym <= max_sym) {
        if (prev_zero) {
            unsigned n0 = sym;
            for (;;) {
                const uint32_t r = get(2);
                n0 += r;
                if (r != 3) break;
            }
            if (n0 > max_sym) fail("FSE table has too many symbols");
            while (sym < n0) norm[sym++] = 0;
        }
        const int32_t maxv = static_cast<int32_t>(2 * threshold - 1) - remaining;
        int32_t count;
        const uint32_t low = get(nbits - 1);
        if (static_cast<int32_t>(low) < maxv) {
            count = static_cast<int32_t>(low);
        } else {
            const uint32_t full = low | (get(1) << (nbits - 1));
            count = static_cast<int32_t>(full);
            if (count >= static_cast<int32_t>(threshold)) count -= maxv;
        }
        count--;
        remaining -= count < 0 ? -count : count;
        if (remaining < 1) fail("corrupt FSE table");
        norm[sym++] = static_cast<int16_t>(count);
        prev_zero = count == 0;
        while (remaining < static_cast<int32_t>(threshold)) {
            nbits--;
            threshold >>= 1;
        }
    }
    if (remaining != 1) fail("corrupt FSE table");
    nsyms = sym;
    al_out = al;
    return (bitpos + 7) / 8;
}

struct HufTable {
    unsigned max_bits = 0;
    bool valid = false;
    uint8_t sym[1u << kHufMaxBits];
    uint8_t nb[1u << kHufMaxBits];
};

size_t read_huf_table(const uint8_t* p, size_t n, HufTable& t) {
    if (n < 1) fail("truncated Huffman table");
    const unsigned hb = p[0];
    uint8_t w[257];
    unsigned nw = 0;
    size_t used = 0;
    if (hb >= 128) {
        nw = hb - 127;
        const size_t bytes = (nw + 1) / 2;
        if (1 + bytes > n) fail("truncated Huffman table");
        for (unsigned i = 0; i < nw; i++) {
            const uint8_t b = p[1 + i / 2];
            w[i] = (i % 2 == 0) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 15);
        }
        used = 1 + bytes;
    } else {
        if (hb == 0 || 1 + static_cast<size_t>(hb) > n) fail("truncated Huffman table");
        int16_t norm[256];
        unsigned nsyms = 0;
        unsigned al = 0;
        const size_t header = read_fse_header(p + 1, hb, 6, 255, norm, nsyms, al);
        if (header >= hb) fail("corrupt Huffman weights");
        FseTable ft;
        build_fse(ft, norm, nsyms, al);
        BackReader br(p + 1 + header, hb - header);
        uint32_t s1 = br.read(al);
        uint32_t s2 = br.read(al);
        for (;;) {
            if (nw + 2 > 255) fail("too many Huffman weights");
            w[nw++] = ft.sym[s1];
            s1 = ft.base[s1] + br.read(ft.nb[s1]);
            if (br.remaining() < 0) {
                w[nw++] = ft.sym[s2];
                break;
            }
            w[nw++] = ft.sym[s2];
            s2 = ft.base[s2] + br.read(ft.nb[s2]);
            if (br.remaining() < 0) {
                w[nw++] = ft.sym[s1];
                break;
            }
        }
        used = 1 + static_cast<size_t>(hb);
    }
    if (nw == 0 || nw > 255) fail("corrupt Huffman weights");
    uint32_t sum = 0;
    for (unsigned i = 0; i < nw; i++) {
        if (w[i] > kHufMaxBits) fail("corrupt Huffman weights");
        if (w[i] > 0) sum += 1u << (w[i] - 1);
    }
    if (sum == 0) fail("corrupt Huffman weights");
    const unsigned max_bits = highbit(sum) + 1;
    if (max_bits > kHufMaxBits) fail("corrupt Huffman weights");
    const uint32_t left = (1u << max_bits) - sum;
    if (left == 0 || (left & (left - 1)) != 0) fail("corrupt Huffman weights");
    w[nw] = static_cast<uint8_t>(highbit(left) + 1);
    const unsigned nsym = nw + 1;
    uint32_t start[kHufMaxBits + 2] = {0};
    uint32_t count[kHufMaxBits + 2] = {0};
    for (unsigned i = 0; i < nsym; i++) count[w[i]]++;
    uint32_t next = 0;
    for (unsigned wt = 1; wt <= max_bits; wt++) {
        start[wt] = next;
        next += count[wt] << (wt - 1);
    }
    for (unsigned i = 0; i < nsym; i++) {
        const unsigned wt = w[i];
        if (wt == 0) continue;
        const uint32_t len = 1u << (wt - 1);
        const uint8_t bits = static_cast<uint8_t>(max_bits + 1 - wt);
        for (uint32_t j = 0; j < len; j++) {
            t.sym[start[wt] + j] = static_cast<uint8_t>(i);
            t.nb[start[wt] + j] = bits;
        }
        start[wt] += len;
    }
    t.max_bits = max_bits;
    t.valid = true;
    return used;
}

void huf_decode_tail(const HufTable& t, BackReader& br, uint8_t* out, size_t count) {
    const unsigned mb = t.max_bits;
    for (size_t i = 0; i < count; i++) {
        const uint32_t idx = br.peek(mb);
        out[i] = t.sym[idx];
        br.skip(t.nb[idx]);
    }
    if (br.remaining() != 0) fail("corrupt Huffman stream");
}

void huf_decode_stream(const HufTable& t, const uint8_t* p, size_t n, uint8_t* out, size_t count) {
    BackReader br(p, n);
    huf_decode_tail(t, br, out, count);
}

void huf_decode_four(const HufTable& t, const uint8_t* const* sp, const size_t* sn, uint8_t* const* so,
                     const size_t* cnt) {
    BackReader r0(sp[0], sn[0]);
    BackReader r1(sp[1], sn[1]);
    BackReader r2(sp[2], sn[2]);
    BackReader r3(sp[3], sn[3]);
    const unsigned mb = t.max_bits;
    const uint8_t* const sym = t.sym;
    const uint8_t* const nb = t.nb;
    uint8_t* const o0 = so[0];
    uint8_t* const o1 = so[1];
    uint8_t* const o2 = so[2];
    uint8_t* const o3 = so[3];
    const size_t common = std::min(std::min(cnt[0], cnt[1]), std::min(cnt[2], cnt[3]));
    size_t i = 0;
    while (i + 4 <= common && r0.fast_ok() && r1.fast_ok() && r2.fast_ok() && r3.fast_ok()) {
        r0.reload_fast();
        r1.reload_fast();
        r2.reload_fast();
        r3.reload_fast();
        for (unsigned j = 0; j < 4; j++) {
            const uint32_t i0 = r0.peek_fast(mb);
            const uint32_t i1 = r1.peek_fast(mb);
            const uint32_t i2 = r2.peek_fast(mb);
            const uint32_t i3 = r3.peek_fast(mb);
            o0[i + j] = sym[i0];
            o1[i + j] = sym[i1];
            o2[i + j] = sym[i2];
            o3[i + j] = sym[i3];
            r0.skip(nb[i0]);
            r1.skip(nb[i1]);
            r2.skip(nb[i2]);
            r3.skip(nb[i3]);
        }
        i += 4;
    }
    huf_decode_tail(t, r0, o0 + i, cnt[0] - i);
    huf_decode_tail(t, r1, o1 + i, cnt[1] - i);
    huf_decode_tail(t, r2, o2 + i, cnt[2] - i);
    huf_decode_tail(t, r3, o3 + i, cnt[3] - i);
}

struct Predefined {
    FseTable ll, of, ml;
    Predefined() {
        build_fse(ll, kLlDefault, 36, 6);
        build_fse(of, kOfDefault, 29, 5);
        build_fse(ml, kMlDefault, 53, 6);
    }
};

const Predefined& predefined() {
    static const Predefined p;
    return p;
}

struct Context {
    HufTable huf;
    FseTable ll_store, of_store, ml_store;
    const FseTable* ll = nullptr;
    const FseTable* of = nullptr;
    const FseTable* ml = nullptr;
    uint32_t rep[3] = {1, 4, 8};
    std::vector<uint8_t> lit;

    Context() : lit(kBlockMax + 64) {}

    void reset_frame() {
        huf.valid = false;
        ll = of = ml = nullptr;
        rep[0] = 1;
        rep[1] = 4;
        rep[2] = 8;
    }
};

size_t parse_literals(Context& c, const uint8_t* p, size_t n, const uint8_t*& lit, size_t& lit_len) {
    if (n < 1) fail("truncated literals section");
    const unsigned b0 = p[0];
    const unsigned type = b0 & 3;
    const unsigned sf = (b0 >> 2) & 3;
    if (type <= 1) {
        size_t regen = 0;
        size_t hdr = 1;
        if (sf == 0 || sf == 2) {
            regen = b0 >> 3;
        } else if (sf == 1) {
            if (n < 2) fail("truncated literals section");
            regen = (b0 >> 4) | (static_cast<size_t>(p[1]) << 4);
            hdr = 2;
        } else {
            if (n < 3) fail("truncated literals section");
            regen = (b0 >> 4) | (static_cast<size_t>(p[1]) << 4) | (static_cast<size_t>(p[2]) << 12);
            hdr = 3;
        }
        if (regen > kBlockMax) fail("literals section is too large");
        if (type == 0) {
            if (hdr + regen > n) fail("truncated literals section");
            lit = p + hdr;
            lit_len = regen;
            return hdr + regen;
        }
        if (hdr + 1 > n) fail("truncated literals section");
        std::memset(c.lit.data(), p[hdr], regen);
        lit = c.lit.data();
        lit_len = regen;
        return hdr + 1;
    }
    size_t hdr = 3;
    unsigned streams = 1;
    unsigned bits = 10;
    if (sf == 1) {
        streams = 4;
    } else if (sf == 2) {
        hdr = 4;
        streams = 4;
        bits = 14;
    } else if (sf == 3) {
        hdr = 5;
        streams = 4;
        bits = 18;
    }
    if (n < hdr) fail("truncated literals section");
    uint64_t v = 0;
    for (size_t i = 0; i < hdr; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    const uint64_t field = (uint64_t(1) << bits) - 1;
    const size_t regen = static_cast<size_t>((v >> 4) & field);
    const size_t csize = static_cast<size_t>((v >> (4 + bits)) & field);
    if (regen > kBlockMax) fail("literals section is too large");
    if (hdr + csize > n) fail("truncated literals section");
    const uint8_t* sp = p + hdr;
    size_t sn = csize;
    if (type == 2) {
        const size_t used = read_huf_table(sp, sn, c.huf);
        sp += used;
        sn -= used;
    } else if (!c.huf.valid) {
        fail("literals reuse a Huffman table that was never defined");
    }
    if (regen == 0) {
        lit = c.lit.data();
        lit_len = 0;
        return hdr + csize;
    }
    if (streams == 1) {
        huf_decode_stream(c.huf, sp, sn, c.lit.data(), regen);
    } else {
        if (sn < 6) fail("truncated literals streams");
        const size_t s1 = sp[0] | (static_cast<size_t>(sp[1]) << 8);
        const size_t s2 = sp[2] | (static_cast<size_t>(sp[3]) << 8);
        const size_t s3 = sp[4] | (static_cast<size_t>(sp[5]) << 8);
        if (s1 + s2 + s3 + 6 >= sn) fail("corrupt literals stream sizes");
        const size_t s4 = sn - 6 - s1 - s2 - s3;
        const size_t seg = (regen + 3) / 4;
        if (seg * 3 > regen) fail("corrupt literals stream sizes");
        const size_t last = regen - seg * 3;
        const uint8_t* q = sp + 6;
        uint8_t* o = c.lit.data();
        const uint8_t* streams[4] = {q, q + s1, q + s1 + s2, q + s1 + s2 + s3};
        const size_t sizes[4] = {s1, s2, s3, s4};
        uint8_t* outs[4] = {o, o + seg, o + 2 * seg, o + 3 * seg};
        const size_t counts[4] = {seg, seg, seg, last};
        huf_decode_four(c.huf, streams, sizes, outs, counts);
    }
    lit = c.lit.data();
    lit_len = regen;
    return hdr + csize;
}

const FseTable* select_table(const FseTable* previous, FseTable& store, unsigned mode, const uint8_t*& p,
                             const uint8_t* end, const FseTable& predefined_table, unsigned max_al,
                             unsigned max_sym) {
    if (mode == 0) return &predefined_table;
    if (mode == 1) {
        if (p >= end) fail("truncated sequence table");
        const unsigned s = *p++;
        if (s > max_sym) fail("corrupt sequence table symbol");
        store.al = 0;
        store.sym[0] = static_cast<uint8_t>(s);
        store.nb[0] = 0;
        store.base[0] = 0;
        return &store;
    }
    if (mode == 3) {
        if (!previous) fail("sequence table repeat without a previous table");
        return previous;
    }
    int16_t norm[64];
    unsigned nsyms = 0;
    unsigned al = 0;
    const size_t used = read_fse_header(p, static_cast<size_t>(end - p), max_al, max_sym, norm, nsyms, al);
    build_fse(store, norm, nsyms, al);
    p += used;
    return &store;
}

size_t decode_block(Context& c, const uint8_t* in, size_t n, uint8_t* frame_begin, uint8_t* op, uint8_t* out_end,
                    uint64_t window) {
    const uint8_t* lit = nullptr;
    size_t lit_len = 0;
    const size_t lit_used = parse_literals(c, in, n, lit, lit_len);
    const uint8_t* p = in + lit_used;
    const uint8_t* const end = in + n;
    uint8_t* const block_begin = op;
    if (p >= end) fail("truncated sequences section");

    size_t nseq = 0;
    const unsigned b0 = *p++;
    if (b0 == 0) {
        if (p != end) fail("unexpected data after a block without sequences");
        if (lit_len > static_cast<size_t>(out_end - op)) fail("output is larger than expected");
        std::memcpy(op, lit, lit_len);
        return lit_len;
    }
    if (b0 < 128) {
        nseq = b0;
    } else if (b0 < 255) {
        if (p >= end) fail("truncated sequences section");
        nseq = ((b0 - 128) << 8) + *p++;
    } else {
        if (end - p < 2) fail("truncated sequences section");
        nseq = static_cast<size_t>(p[0]) + (static_cast<size_t>(p[1]) << 8) + 0x7F00;
        p += 2;
    }
    if (p >= end) fail("truncated sequences section");
    const unsigned modes = *p++;
    if (modes & 3) fail("reserved bits are set in the sequence modes");
    const Predefined& pre = predefined();
    c.ll = select_table(c.ll, c.ll_store, (modes >> 6) & 3, p, end, pre.ll, 9, 35);
    c.of = select_table(c.of, c.of_store, (modes >> 4) & 3, p, end, pre.of, 8, 31);
    c.ml = select_table(c.ml, c.ml_store, (modes >> 2) & 3, p, end, pre.ml, 9, 52);
    if (p >= end) fail("truncated sequences section");

    const FseTable& ll = *c.ll;
    const FseTable& of = *c.of;
    const FseTable& ml = *c.ml;
    BackReader br(p, static_cast<size_t>(end - p));
    uint32_t ls = br.read(ll.al);
    uint32_t os = br.read(of.al);
    uint32_t ms = br.read(ml.al);

    uint32_t rep0 = c.rep[0], rep1 = c.rep[1], rep2 = c.rep[2];
    const uint8_t* lp = lit;
    size_t lit_left = lit_len;
    const uint8_t* const lit_cap = (lit == c.lit.data()) ? c.lit.data() + c.lit.size() : end;
    const size_t frame_done = static_cast<size_t>(block_begin - frame_begin);

    for (size_t i = 0; i < nseq; i++) {
        const unsigned ofc = of.sym[os];
        const unsigned mlc = ml.sym[ms];
        const unsigned llc = ll.sym[ls];
        uint32_t off_value;
        size_t mlen;
        size_t llen;
        if (br.fast_ok_seq()) {
            br.reload_fast();
            off_value = (1u << ofc) + br.take_fast(ofc);
            mlen = kMlBase[mlc] + br.take_fast(kMlBits[mlc]);
            br.reload_fast();
            llen = kLlBase[llc] + br.take_fast(kLlBits[llc]);
            if (i + 1 < nseq) {
                ls = ll.base[ls] + br.take_fast(ll.nb[ls]);
                ms = ml.base[ms] + br.take_fast(ml.nb[ms]);
                os = of.base[os] + br.take_fast(of.nb[os]);
            }
        } else {
            off_value = (1u << ofc) + br.read(ofc);
            mlen = kMlBase[mlc] + br.read(kMlBits[mlc]);
            llen = kLlBase[llc] + br.read(kLlBits[llc]);
            if (i + 1 < nseq) {
                ls = ll.base[ls] + br.read(ll.nb[ls]);
                ms = ml.base[ms] + br.read(ml.nb[ms]);
                os = of.base[os] + br.read(of.nb[os]);
            }
        }

        uint32_t offset;
        if (off_value > 3) {
            offset = off_value - 3;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = offset;
        } else {
            unsigned idx = off_value;
            if (llen == 0) idx++;
            if (idx == 1) {
                offset = rep0;
            } else if (idx == 2) {
                offset = rep1;
                rep1 = rep0;
                rep0 = offset;
            } else if (idx == 3) {
                offset = rep2;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = offset;
            } else {
                offset = rep0 - 1;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = offset;
            }
        }
        if (offset == 0) fail("corrupt match offset");

        if (llen > lit_left) fail("literal length exceeds the literals section");
        if (llen > static_cast<size_t>(out_end - op)) fail("output is larger than expected");
        if (llen <= 16 && static_cast<size_t>(out_end - op) >= 16 && static_cast<size_t>(lit_cap - lp) >= 16) {
            std::memcpy(op, lp, 16);
        } else {
            std::memcpy(op, lp, llen);
        }
        op += llen;
        lp += llen;
        lit_left -= llen;

        const size_t produced = frame_done + static_cast<size_t>(op - block_begin);
        if (offset > produced || offset > window) fail("match offset is outside the window");
        if (mlen > static_cast<size_t>(out_end - op)) fail("output is larger than expected");
        uint8_t* dst = op;
        const uint8_t* src = op - offset;
        size_t remaining = mlen;
        if (offset >= 16 && static_cast<size_t>(out_end - op) >= mlen + 16) {
            uint8_t* const stop = op + mlen;
            do {
                std::memcpy(dst, src, 16);
                dst += 16;
                src += 16;
            } while (dst < stop);
        } else if (offset >= remaining) {
            std::memcpy(dst, src, remaining);
        } else if (offset >= 8) {
            while (remaining >= 8) {
                std::memcpy(dst, src, 8);
                dst += 8;
                src += 8;
                remaining -= 8;
            }
            while (remaining > 0) {
                *dst++ = *src++;
                remaining--;
            }
        } else if (offset == 1) {
            std::memset(dst, *src, remaining);
        } else {
            while (remaining > 0) {
                *dst++ = *src++;
                remaining--;
            }
        }
        op += mlen;
        if (static_cast<size_t>(op - block_begin) > kBlockMax) fail("block is larger than the format allows");
    }
    if (br.remaining() != 0) fail("corrupt sequences bitstream");
    c.rep[0] = rep0;
    c.rep[1] = rep1;
    c.rep[2] = rep2;
    if (lit_left > static_cast<size_t>(out_end - op)) fail("output is larger than expected");
    std::memcpy(op, lp, lit_left);
    op += lit_left;
    if (static_cast<size_t>(op - block_begin) > kBlockMax) fail("block is larger than the format allows");
    return static_cast<size_t>(op - block_begin);
}

}

void zstd_decompress(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size) {
    if (in_size == 0) {
        if (out_size == 0) return;
        fail("empty input");
    }
    Context ctx;
    size_t ip = 0;
    uint8_t* op = out;
    uint8_t* const out_end = out + out_size;
    while (ip < in_size) {
        if (in_size - ip < 4) fail("truncated frame header");
        uint32_t magic = 0;
        std::memcpy(&magic, in + ip, 4);
        ip += 4;
        if ((magic & kSkippableMask) == kSkippableMagic) {
            if (in_size - ip < 4) fail("truncated skippable frame");
            uint32_t skip = 0;
            std::memcpy(&skip, in + ip, 4);
            ip += 4;
            if (skip > in_size - ip) fail("truncated skippable frame");
            ip += skip;
            continue;
        }
        if (magic != kFrameMagic) fail("not a zstd frame");

        if (ip >= in_size) fail("truncated frame header");
        const unsigned desc = in[ip++];
        const unsigned fcs_flag = desc >> 6;
        const bool single = ((desc >> 5) & 1) != 0;
        if (desc & 0x08) fail("reserved frame header bit is set");
        const bool checksum = ((desc >> 2) & 1) != 0;
        const unsigned dict_flag = desc & 3;

        uint64_t window = 0;
        if (!single) {
            if (ip >= in_size) fail("truncated frame header");
            const unsigned wd = in[ip++];
            const unsigned wlog = 10 + (wd >> 3);
            if (wlog > 41) fail("window size is too large");
            const uint64_t base = uint64_t(1) << wlog;
            window = base + (base / 8) * (wd & 7);
        }
        static const unsigned dict_bytes[4] = {0, 1, 2, 4};
        const unsigned db = dict_bytes[dict_flag];
        if (in_size - ip < db) fail("truncated frame header");
        uint32_t dict_id = 0;
        for (unsigned i = 0; i < db; i++) dict_id |= static_cast<uint32_t>(in[ip + i]) << (8 * i);
        ip += db;
        if (dict_id != 0) fail("this frame needs a dictionary, which is not supported");

        unsigned fb = 0;
        if (fcs_flag == 0) fb = single ? 1 : 0;
        else if (fcs_flag == 1) fb = 2;
        else if (fcs_flag == 2) fb = 4;
        else fb = 8;
        if (in_size - ip < fb) fail("truncated frame header");
        bool has_fcs = fb > 0;
        uint64_t fcs = 0;
        for (unsigned i = 0; i < fb; i++) fcs |= static_cast<uint64_t>(in[ip + i]) << (8 * i);
        ip += fb;
        if (fcs_flag == 1) fcs += 256;
        if (single) window = fcs;
        if (has_fcs && fcs > static_cast<uint64_t>(out_end - op)) fail("frame is larger than the expected output");

        ctx.reset_frame();
        uint8_t* const frame_begin = op;
        for (;;) {
            if (in_size - ip < 3) fail("truncated block header");
            const uint32_t h = in[ip] | (static_cast<uint32_t>(in[ip + 1]) << 8) | (static_cast<uint32_t>(in[ip + 2]) << 16);
            ip += 3;
            const bool last = (h & 1) != 0;
            const unsigned type = (h >> 1) & 3;
            const size_t size = h >> 3;
            if (type == 3) fail("reserved block type");
            if (size > kBlockMax) fail("Block is larger than the format allows");
            if (type == 0) {
                if (size > in_size - ip) fail("truncated raw block");
                if (size > static_cast<size_t>(out_end - op)) fail("output is larger than expected");
                std::memcpy(op, in + ip, size);
                ip += size;
                op += size;
            } else if (type == 1) {
                if (ip >= in_size) fail("truncated run-length block");
                if (size > static_cast<size_t>(out_end - op)) fail("output is larger than expected");
                std::memset(op, in[ip], size);
                ip += 1;
                op += size;
            } else {
                if (size > in_size - ip) fail("truncated compressed block");
                op += decode_block(ctx, in + ip, size, frame_begin, op, out_end, window);
                ip += size;
            }
            if (last) break;
        }
        const uint64_t produced = static_cast<uint64_t>(op - frame_begin);
        if (has_fcs && produced != fcs) fail("frame size does not match its header");
        if (checksum) {
            if (in_size - ip < 4) fail("truncated content checksum");
            uint32_t want = 0;
            std::memcpy(&want, in + ip, 4);
            ip += 4;
            const uint32_t got = static_cast<uint32_t>(csy::hash64_fast(frame_begin, static_cast<size_t>(produced), 0));
            if (got != want) fail("content checksum mismatch");
        }
    }
    if (op != out_end) fail("decoded size do not match the expected size");
}

}
}
