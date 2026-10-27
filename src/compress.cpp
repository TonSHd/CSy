#include "compress.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace csy {
namespace compress {

namespace {

[[noreturn]] void bad(const char* what) {
    throw std::runtime_error(std::string("decompress: ") + what);
}

}

void snappy_decompress(const unsigned char* in, size_t n, unsigned char* out, size_t out_size) {
    size_t pos = 0;
    uint64_t declared = 0;
    int shift = 0;
    for (;;) {
        if (pos >= n || shift > 35) bad("snappy length header is damaged");
        const unsigned char b = in[pos++];
        declared |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    if (declared != out_size) bad("snappy length does not match the page header");
    size_t o = 0;
    while (pos < n) {
        const unsigned tag = in[pos++];
        const unsigned kind = tag & 3;
        if (kind == 0) {
            size_t len = tag >> 2;
            if (len < 60) {
                len += 1;
            } else {
                const size_t extra = len - 59;
                if (n - pos < extra) bad("snappy literal is truncated");
                size_t v = 0;
                for (size_t k = 0; k < extra; k++) v |= static_cast<size_t>(in[pos + k]) << (8 * k);
                pos += extra;
                len = v + 1;
            }
            if (len > n - pos || len > out_size - o) bad("snappy literal is out of range");
            std::memcpy(out + o, in + pos, len);
            pos += len;
            o += len;
        } else {
            size_t len, off;
            if (kind == 1) {
                if (pos >= n) bad("snappy copy is truncated");
                len = 4 + ((tag >> 2) & 7);
                off = (static_cast<size_t>(tag >> 5) << 8) | in[pos++];
            } else if (kind == 2) {
                if (n - pos < 2) bad("snappy copy is truncated");
                len = 1 + (tag >> 2);
                off = static_cast<size_t>(in[pos]) | (static_cast<size_t>(in[pos + 1]) << 8);
                pos += 2;
            } else {
                if (n - pos < 4) bad("snappy copy is truncated");
                len = 1 + (tag >> 2);
                off = static_cast<size_t>(in[pos]) | (static_cast<size_t>(in[pos + 1]) << 8) |
                      (static_cast<size_t>(in[pos + 2]) << 16) | (static_cast<size_t>(in[pos + 3]) << 24);
                pos += 4;
            }
            if (off == 0 || off > o || len > out_size - o) bad("snappy copy is out of range");
            if (off >= len) {
                std::memcpy(out + o, out + o - off, len);
            } else {
                for (size_t k = 0; k < len; k++) out[o + k] = out[o + k - off];
            }
            o += len;
        }
    }
    if (o != out_size) bad("snappy data is shorter than declared");
}

namespace {

constexpr int kMaxBits = 15;
constexpr int kFixedLitCodes = 288;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[kFixedLitCodes];
};

class Inflater {
public:
    Inflater(const unsigned char* in, size_t n, unsigned char* out, size_t out_size)
        : in_(in), n_(n), out_(out), out_size_(out_size) {}

    size_t run() {
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (type == 0) stored();
            else if (type == 1) fixed();
            else if (type == 2) dynamic();
            else bad("deflate block type is invalid");
        } while (!last);
        return out_pos_;
    }

    size_t consumed() const { return in_pos_; }

private:
    const unsigned char* in_;
    size_t n_;
    size_t in_pos_ = 0;
    unsigned long bitbuf_ = 0;
    int bitcnt_ = 0;
    unsigned char* out_;
    size_t out_size_;
    size_t out_pos_ = 0;

    int bits(int need) {
        unsigned long val = bitbuf_;
        while (bitcnt_ < need) {
            if (in_pos_ >= n_) bad("deflate stream is truncated");
            val |= static_cast<unsigned long>(in_[in_pos_++]) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return static_cast<int>(val & ((1UL << need) - 1));
    }

    void stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (n_ - in_pos_ < 4) bad("deflate stored block is truncated");
        const unsigned len = in_[in_pos_] | (in_[in_pos_ + 1] << 8);
        const unsigned nlen = in_[in_pos_ + 2] | (in_[in_pos_ + 3] << 8);
        in_pos_ += 4;
        if (len != (~nlen & 0xFFFF)) bad("deflate stored block length is damaged");
        if (len > n_ - in_pos_) bad("deflate stored block is truncated");
        if (len > out_size_ - out_pos_) bad("deflate output is larger than declared");
        std::memcpy(out_ + out_pos_, in_ + in_pos_, len);
        in_pos_ += len;
        out_pos_ += len;
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; len++) {
            code |= bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        bad("deflate code is invalid");
    }

    static int construct(Huffman& h, const short* length, int n) {
        for (int len = 0; len <= kMaxBits; len++) h.count[len] = 0;
        for (int s = 0; s < n; s++) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; len++) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; s++) {
            if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<short>(s);
        }
        return left;
    }

    void codes(const Huffman& lencode, const Huffman& distcode) {
        static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = decode(lencode);
            if (symbol < 256) {
                if (out_pos_ >= out_size_) bad("deflate output is larger than declared");
                out_[out_pos_++] = static_cast<unsigned char>(symbol);
            } else if (symbol == 256) {
                return;
            } else {
                symbol -= 257;
                if (symbol >= 29) bad("deflate length code is invalid");
                const size_t len = static_cast<size_t>(lens[symbol] + bits(lext[symbol]));
                const int dsym = decode(distcode);
                if (dsym >= 30) bad("deflate distance code is invalid");
                const size_t dist = static_cast<size_t>(dists[dsym] + bits(dext[dsym]));
                if (dist > out_pos_) bad("deflate distance reaches before the start of the data");
                if (len > out_size_ - out_pos_) bad("deflate output is larger than declared");
                for (size_t k = 0; k < len; k++) out_[out_pos_ + k] = out_[out_pos_ + k - dist];
                out_pos_ += len;
            }
        }
    }

    struct FixedTables {
        Huffman lencode, distcode;
        FixedTables() {
            short lengths[kFixedLitCodes];
            int s = 0;
            for (; s < 144; s++) lengths[s] = 8;
            for (; s < 256; s++) lengths[s] = 9;
            for (; s < 280; s++) lengths[s] = 7;
            for (; s < kFixedLitCodes; s++) lengths[s] = 8;
            construct(lencode, lengths, kFixedLitCodes);
            for (s = 0; s < 30; s++) lengths[s] = 5;
            construct(distcode, lengths, 30);
        }
    };

    void fixed() {
        static const FixedTables tables;
        codes(tables.lencode, tables.distcode);
    }

    void dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        short lengths[320];
        const int nlen = bits(5) + 257;
        const int ndist = bits(5) + 1;
        const int ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) bad("deflate dynamic block header is invalid");
        int index = 0;
        for (; index < ncode; index++) lengths[order[index]] = static_cast<short>(bits(3));
        for (; index < 19; index++) lengths[order[index]] = 0;
        Huffman lencode, distcode;
        if (construct(lencode, lengths, 19) != 0) bad("deflate code length table is invalid");
        index = 0;
        while (index < nlen + ndist) {
            int symbol = decode(lencode);
            if (symbol < 16) {
                lengths[index++] = static_cast<short>(symbol);
            } else {
                int len = 0;
                int rep;
                if (symbol == 16) {
                    if (index == 0) bad("deflate repeat has nothing to repeat");
                    len = lengths[index - 1];
                    rep = 3 + bits(2);
                } else if (symbol == 17) {
                    rep = 3 + bits(3);
                } else {
                    rep = 11 + bits(7);
                }
                if (index + rep > nlen + ndist) bad("deflate code lengths overflow");
                while (rep--) lengths[index++] = static_cast<short>(len);
            }
        }
        if (lengths[256] == 0) bad("deflate block has no end code");
        int err = construct(lencode, lengths, nlen);
        if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) bad("deflate literal table is invalid");
        err = construct(distcode, lengths + nlen, ndist);
        if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) bad("deflate distance table is invalid");
        codes(lencode, distcode);
    }
};

struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};

uint32_t crc32(const unsigned char* data, size_t n) {
    static const CrcTable table;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table.t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}

void gzip_decompress(const unsigned char* in, size_t n, unsigned char* out, size_t out_size) {
    size_t pos = 0;
    bool gzip = false;
    if (n >= 18 && in[0] == 0x1F && in[1] == 0x8B) {
        if (in[2] != 8) bad("gzip uses an unknown compression method");
        const unsigned flags = in[3];
        pos = 10;
        if (flags & 4) {
            if (n - pos < 2) bad("gzip header is truncated");
            const size_t extra = in[pos] | (in[pos + 1] << 8);
            pos += 2;
            if (extra > n - pos) bad("gzip header is truncated");
            pos += extra;
        }
        for (const unsigned bit : {8u, 16u}) {
            if (flags & bit) {
                while (pos < n && in[pos] != 0) pos++;
                if (pos >= n) bad("gzip header is truncated");
                pos++;
            }
        }
        if (flags & 2) {
            if (n - pos < 2) bad("gzip header is truncated");
            pos += 2;
        }
        gzip = true;
    } else if (n >= 2 && (in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) {
        pos = 2;
        if (in[1] & 0x20) bad("zlib preset dictionaries are not supported");
    }
    if (pos > n) bad("gzip header is truncated");
    Inflater inflater(in + pos, n - pos, out, out_size);
    const size_t produced = inflater.run();
    if (produced != out_size) bad("inflated data is shorter than declared");
    if (gzip) {
        const size_t tail = pos + inflater.consumed();
        if (n - tail < 8) bad("gzip trailer is truncated");
        const uint32_t want_crc = static_cast<uint32_t>(in[tail]) | (static_cast<uint32_t>(in[tail + 1]) << 8) |
                                  (static_cast<uint32_t>(in[tail + 2]) << 16) | (static_cast<uint32_t>(in[tail + 3]) << 24);
        if (crc32(out, out_size) != want_crc) bad("gzip checksum does not match, the data is corrupt");
    }
}

}
}
