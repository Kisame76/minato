#include "vfs.h"

#include <sys/stat.h>

#include <mutex>
#include <vector>

namespace apkrt::vfs {
namespace {

struct Mount {
    std::string guest;  // absolute, normal
    std::string host;
};

/// The devices every Linux system has. /dev/random never blocks, like on Linux 5.6 and later;
/// bionic's arc4random checks access("/dev/urandom") early and otherwise lives on the 16 bytes of
/// AT_RANDOM alone.
std::vector<Mount> devices() {
    return {{"/dev/null", "/dev/null"},
            {"/dev/zero", "/dev/zero"},
            {"/dev/random", "/dev/urandom"},
            {"/dev/urandom", "/dev/urandom"}};
}

std::mutex mutex;
std::vector<Mount> mounts = devices();  // guarded by mutex
std::string cwd = "/";                  // guarded by mutex

/// Whether `path` is `prefix` or lies below it; both absolute and normal.
bool covers(std::string_view prefix, std::string_view path) {
    if (prefix == "/") return true;
    return path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '/');
}

/// `prefix` replaced by `replacement` at the start of `path`, which `prefix` covers.
std::string rebase(std::string_view path, std::string_view prefix, std::string_view replacement) {
    std::string_view rest = prefix == "/" ? path : path.substr(prefix.size());
    if (rest.starts_with('/') && replacement.ends_with('/')) rest.remove_prefix(1);
    std::string result(replacement);
    if (!rest.empty() && rest != "/") result += rest;
    return result;
}

}  // namespace

std::string normalize(std::string_view absolute_path) {
    std::vector<std::string_view> parts;
    std::size_t position = 0;
    while (position < absolute_path.size()) {
        const auto end = std::min(absolute_path.find('/', position), absolute_path.size());
        const auto part = absolute_path.substr(position, end - position);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        position = end + 1;
    }
    if (parts.empty()) return "/";
    std::string result;
    for (const auto part : parts) {
        result += '/';
        result += part;
    }
    return result;
}

std::string absolute(std::string_view path, std::string_view base) {
    if (path.starts_with('/')) return normalize(path);
    std::string joined(base);
    joined += '/';
    joined += path;
    return normalize(joined);
}

void mount(std::string_view guest, std::string_view host) {
    const std::lock_guard lock(mutex);
    const auto normal = normalize(guest);
    for (auto &existing : mounts) {
        if (existing.guest == normal) {
            existing.host = host;
            return;
        }
    }
    mounts.push_back({normal, std::string(host)});
}

void reset() {
    const std::lock_guard lock(mutex);
    mounts = devices();
    cwd = "/";
}

std::optional<std::string> host_path(std::string_view guest) {
    const std::lock_guard lock(mutex);
    const Mount *best = nullptr;
    for (const auto &candidate : mounts) {
        if (covers(candidate.guest, guest) && (best == nullptr || candidate.guest.size() > best->guest.size())) {
            best = &candidate;
        }
    }
    if (best == nullptr) return std::nullopt;
    return rebase(guest, best->guest, best->host);
}

std::optional<std::string> guest_path(std::string_view host) {
    const std::lock_guard lock(mutex);
    // The deepest guest path wins here as well: the mount whose host path is the longest match.
    const Mount *best = nullptr;
    for (const auto &candidate : mounts) {
        if (covers(candidate.host, host) && (best == nullptr || candidate.host.size() > best->host.size())) {
            best = &candidate;
        }
    }
    if (best == nullptr) return std::nullopt;
    return rebase(host, best->host, best->guest);
}

std::optional<std::string> mount_point(std::string_view guest) {
    const std::lock_guard lock(mutex);
    const Mount *best = nullptr;
    for (const auto &candidate : mounts) {
        if (!covers(candidate.guest, guest) || (best != nullptr && candidate.guest.size() <= best->guest.size())) {
            continue;
        }
        struct stat status;
        if (::stat(candidate.host.c_str(), &status) == 0 && !S_ISDIR(status.st_mode)) continue;  // a mounted file
        best = &candidate;
    }
    if (best == nullptr) return std::nullopt;
    return best->guest;
}

std::string working_directory() {
    const std::lock_guard lock(mutex);
    return cwd;
}

void set_working_directory(std::string_view guest) {
    const std::lock_guard lock(mutex);
    cwd = normalize(guest);
}

}  // namespace apkrt::vfs
