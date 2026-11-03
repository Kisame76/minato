#include "sys_cpu.h"

#include <sys/sysctl.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <exception>
#include <string_view>
#include <system_error>
#include <vector>

namespace apkrt::sys_cpu {
namespace {

namespace fs = std::filesystem;

/// The cores of a big.LITTLE phone, as Android numbers them: the efficiency cores first, the performance cores last
/// (a Pixel's cpu0-3 are its little cores, the rest big). The host knows them by performance level: level 0 is the
/// performance cores, level 1 the efficiency cores (the A17 Pro has 2 and 4); a device with one level has no little
/// cores.
struct Layout {
    int total = 1, efficiency = 0;
};

int sysctl_cpus(const char *name) {
    int cpus = 0;
    std::size_t length = sizeof cpus;
    return ::sysctlbyname(name, &cpus, &length, nullptr, 0) == 0 && cpus > 0 ? cpus : 0;
}

Layout layout() {
    Layout cores;
    cores.total = std::max(1, sysctl_cpus("hw.ncpu"));
    const int performance = sysctl_cpus("hw.perflevel0.physicalcpu");
    const int efficiency = sysctl_cpus("hw.perflevel1.physicalcpu");
    if (performance > 0 && efficiency > 0 && performance + efficiency == cores.total) cores.efficiency = efficiency;
    return cores;
}

/// A CPU list as Linux writes it: "0-5" for six CPUs, "2-3" for two, "0" for one.
std::string list(int first, int last) {
    return first == last ? std::format("{}\n", first) : std::format("{}-{}\n", first, last);
}

/// iOS gives an app no clock, so the A17 Pro's are the fixed values of every device, a named approximation: only
/// their ratio matters for telling big from little. The capacity is Linux's scale (1024 for the biggest core, the
/// others by clock).
constexpr int performance_khz = 3'780'000, efficiency_khz = 2'110'000, minimum_khz = 600'000;

constexpr std::string_view prefix = "minato-sys-cpu-";

/// Writes `contents` read-only, as /sys; false if anything fails.
bool write(const fs::path &path, const std::string &contents) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    {
        std::ofstream file(path);
        file << contents;
        if (!file) return false;
    }
    return ::chmod(path.c_str(), 0444) == 0;
}

/// Removes a tree whose folders are read-only (0555).
void remove_tree(const fs::path &root) {
    std::error_code ignored;
    ::chmod(root.c_str(), 0755);
    for (const auto &entry : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ignored)) {
        if (entry.is_directory(ignored) && !entry.is_symlink(ignored)) ::chmod(entry.path().c_str(), 0755);
    }
    fs::remove_all(root, ignored);
}

/// The folders of processes that are gone: an exit handler does not run when iOS kills the app or a signal does.
void sweep(const fs::path &parent) {
    std::error_code error;
    std::vector<fs::path> stale;
    for (const auto &entry : fs::directory_iterator(parent, error)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(prefix)) continue;
        const long pid = std::strtol(name.c_str() + prefix.size(), nullptr, 10);
        if (pid > 0 && pid != ::getpid() && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            stale.push_back(entry.path());
        }
    }
    for (const auto &folder : stale) remove_tree(folder);
}

bool write_cluster(const fs::path &root, int first, int last, int khz) {
    const fs::path policy = root / "cpufreq" / std::format("policy{}", first);
    std::string members;
    for (int core = first; core <= last; ++core) members += std::format("{}{}", core == first ? "" : " ", core);
    return write(policy / "cpuinfo_max_freq", std::format("{}\n", khz)) &&
           write(policy / "scaling_max_freq", std::format("{}\n", khz)) &&
           write(policy / "cpuinfo_min_freq", std::format("{}\n", minimum_khz)) &&
           write(policy / "related_cpus", members + "\n") && write(policy / "affected_cpus", members + "\n");
}

/// Writes the whole tree; false if anything fails.
bool fill(const fs::path &root, const Layout &cores) {
    const std::string all = list(0, cores.total - 1);
    bool done = write(root / "possible", all) && write(root / "present", all) && write(root / "online", all) &&
                write(root / "offline", "\n") && write(root / "kernel_max", "31\n");
    if (done && cores.efficiency > 0) done = write_cluster(root, 0, cores.efficiency - 1, efficiency_khz);
    if (done) done = write_cluster(root, cores.efficiency, cores.total - 1, performance_khz);
    for (int core = 0; done && core < cores.total; ++core) {
        const bool little = core < cores.efficiency;
        const fs::path directory = root / std::format("cpu{}", core);
        std::error_code error;
        done = write(directory / "cpu_capacity",
                     std::format("{}\n", little ? 1024LL * efficiency_khz / performance_khz : 1024LL)) &&
               write(directory / "online", "1\n") &&
               write(directory / "topology" / "core_id", std::format("{}\n", core)) &&
               write(directory / "topology" / "physical_package_id", "0\n");
        if (!done) break;
        fs::create_symlink(std::format("../cpufreq/policy{}", little ? 0 : cores.efficiency), directory / "cpufreq",
                           error);
        done = !error;
    }
    return done;
}

/// Makes every folder of the tree read-only too (0555), so a guest cannot create files in its /sys.
void seal(const fs::path &root) {
    std::error_code ignored;
    std::vector<fs::path> folders;
    for (const auto &entry : fs::recursive_directory_iterator(root, ignored)) {
        if (entry.is_directory(ignored) && !entry.is_symlink(ignored)) folders.push_back(entry.path());
    }
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) ::chmod(at->c_str(), 0555);
    ::chmod(root.c_str(), 0555);
}

}  // namespace

std::string make_in(const std::string &parent) {
    try {
        std::error_code error;
        sweep(parent);
        const fs::path root = fs::path(parent) / std::format("{}{}", prefix, ::getpid());
        remove_tree(root);
        fs::create_directories(root, error);
        if (error || !fill(root, layout())) {
            remove_tree(root);
            return {};
        }
        seal(root);
        return root.string();
    } catch (const std::exception &) {
        return {};  // an allocation or a format failure: no folder, as when it cannot be written
    }
}

const std::string &folder() {
    static const std::string made = [] {
        char temporary[PATH_MAX] = {};
        if (::confstr(_CS_DARWIN_USER_TEMP_DIR, temporary, sizeof temporary) == 0) return std::string();
        const std::string root = make_in(temporary);
        if (!root.empty()) {
            static const std::string kept = root;  // for the exit handler
            std::atexit([] { remove_tree(kept); });
        }
        return root;
    }();
    return made;
}

}  // namespace apkrt::sys_cpu
