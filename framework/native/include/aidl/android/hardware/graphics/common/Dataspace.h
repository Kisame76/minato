// Ours (the graphics breadth's G3a): the values of hardware/interfaces' AIDL Dataspace that libui's PublicFormat.cpp
// names, as the AIDL compiler would declare them, for building that file without the generated NDK backend. The
// values are Dataspace.aidl's at the pinned commit (build/sources/hardware_interfaces_graphics).
#pragma once

#include <stdint.h>

namespace aidl::android::hardware::graphics::common {

enum class Dataspace : int32_t {
    UNKNOWN = 0x0,
    ARBITRARY = 0x1,
    JFIF = 2 << 16 | 3 << 22 | 1 << 27,  // STANDARD_BT601_625 | TRANSFER_SMPTE_170M | RANGE_FULL
    DEPTH = 0x1000,
    DYNAMIC_DEPTH = 0x1002,
    HEIF = 0x1004,
    JPEG_R = 0x1005,
};

}  // namespace aidl::android::hardware::graphics::common
