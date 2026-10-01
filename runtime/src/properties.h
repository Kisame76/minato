// The system property area (ADR-0033 decision 8, ADR-0039 decisions 4 and 9): what init keeps at
// /dev/__properties__ on Android, which the runtime gives every program as one file of bionic's pre-split format.
#pragma once

#include <string>

namespace apkrt::properties {

/// The guest path of the area: bionic's PROP_DIRNAME.
inline constexpr const char *path = "/dev/__properties__";

/// The area's bytes, with every property the runtime sets: 128 KiB, as init's.
std::string area();

}  // namespace apkrt::properties
