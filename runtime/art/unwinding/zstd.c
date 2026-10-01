// No zstd for libunwindstack (zstd.h): every decompression fails with zstd's generic error code, (size_t)-1, which
// is never the size libunwindstack expects.
#include "zstd.h"

size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t compressedSize) {
    (void)dst;
    (void)dstCapacity;
    (void)src;
    (void)compressedSize;
    return (size_t)-1;
}
