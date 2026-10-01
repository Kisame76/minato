// odrefresh's cache info for ART (com_android_art.h): the guest has none to read.
#include "com_android_art.h"

namespace com::android::art {

std::optional<CacheInfo> parse(const char*) {
    return std::nullopt;
}

}  // namespace com::android::art
