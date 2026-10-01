// frameworks/libs/modules-utils' SDK level checks for ART (ADR-0033 a): the guest system is Android 15 (API 35,
// "V"), so they answer as on it, also before the guest has a property area to read ro.build.version.sdk from.
#pragma once

namespace android::modules::sdklevel {

constexpr int kGuestApiLevel = 35;

constexpr bool IsAtLeastR() { return kGuestApiLevel >= 30; }
constexpr bool IsAtLeastS() { return kGuestApiLevel >= 31; }
constexpr bool IsAtLeastSv2() { return kGuestApiLevel >= 32; }
constexpr bool IsAtLeastT() { return kGuestApiLevel >= 33; }
constexpr bool IsAtLeastU() { return kGuestApiLevel >= 34; }
constexpr bool IsAtLeastV() { return kGuestApiLevel >= 35; }

}  // namespace android::modules::sdklevel
