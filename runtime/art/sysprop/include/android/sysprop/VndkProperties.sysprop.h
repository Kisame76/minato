// The sysprop API of the VNDK properties that libnativeloader reads (ADR-0033 a). Android 15 dropped the VNDK and
// the guest has no vendor or product partition, so neither version is set.
#pragma once

#include <optional>
#include <string>

namespace android::sysprop::VndkProperties {

inline std::optional<std::string> vendor_vndk_version() { return std::nullopt; }
inline std::optional<std::string> product_vndk_version() { return std::nullopt; }

}  // namespace android::sysprop::VndkProperties
