// /sys/devices/system/cpu as Android 15 shows it on a big.LITTLE phone: the folder the guest lists and reads.
#pragma once

#include <string>

namespace apkrt::sys_cpu {

/// The host folder that holds /sys/devices/system/cpu for this process: cpu0..cpuN-1 (N is the device's CPU count),
/// possible, present, online, offline, kernel_max, and cpufreq/policyN, written once, read-only, and removed at exit.
/// The cores are numbered as Android numbers them: the efficiency cores first, the performance cores last; each
/// cluster has a cpufreq policy named for its first core, and each cpuN/cpufreq links to it. Empty if the folder
/// cannot be made.
const std::string &folder();

/// folder()'s work in `parent`: sweeps the folders that processes since gone left there, makes minato-sys-cpu-<pid>
/// and returns its path, or "" if anything fails (nothing is thrown, and nothing is left behind).
std::string make_in(const std::string &parent);

}  // namespace apkrt::sys_cpu
