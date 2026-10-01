// The part of zstd that libunwindstack calls for zstd-compressed debug sections (ADR-0033 a). The guest's
// libunwindstack has no zstd: every decompression fails, so such sections are not read.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t compressedSize);

#ifdef __cplusplus
}
#endif
