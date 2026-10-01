// apexd's APEX info list for ART (com_android_apex.h): the guest has none to read.
#include "com_android_apex.h"

namespace com::android::apex {

std::optional<ApexInfoList> readApexInfoList(const char*) {
    return std::nullopt;
}

}  // namespace com::android::apex
