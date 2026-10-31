#pragma once

#include <cstddef>

namespace csy {
namespace compress {

void snappy_decompress(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size);

void gzip_decompress(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size);

void zstd_decompress(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size);

}
}

