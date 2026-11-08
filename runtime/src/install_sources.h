// The runtime's record of whether the app may install packages (REQUEST_INSTALL_PACKAGES /
// canRequestPackageInstalls, an app that installs an APK). The host sets it before the start with
// apkrt_set_install_sources (denied by default), and properties::area() publishes it as the system property
// minato.install_sources ("0"/"1") so the framework reads the initial grant at start. A run-time grant comes to the
// framework through APKRT_EVENT_INSTALL_SOURCES_RESULT, not through re-reading the property (the guest's property area
// is a start snapshot). Header-only (inline), so program.cpp and properties.cpp share the one flag. One per process.
#pragma once

#include <atomic>

namespace apkrt::install_sources {

inline std::atomic<bool> granted_flag{false};  // Android's default: not allowed until the user grants the app op

inline bool granted() { return granted_flag.load(std::memory_order_relaxed); }
inline void set(bool value) { granted_flag.store(value, std::memory_order_relaxed); }
inline void reset() { granted_flag.store(false, std::memory_order_relaxed); }  // for tests

}  // namespace apkrt::install_sources
