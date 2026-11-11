// The Mac session's fake path monitor (ADR-0054 step d): see fake_path_monitor.cpp.
#pragma once

namespace apkrt::network {

/// Points netlink's host source at one fixed interface (wlan0, a private IPv4 and a global and link-local IPv6, a
/// default route per family, MTU 1500) and reports a satisfied Wi-Fi path, so the Mac -- which has no NWPathMonitor --
/// has a host network for net-link-test. Called at session start under MINATO_FAKE_NETWORK; never on the device.
void start_fake_path_monitor();

}  // namespace apkrt::network
