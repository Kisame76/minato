#include "vfs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <utility>
#include <vector>

namespace apkrt::vfs {
namespace {

struct Mount {
    Mount(std::string guest_path, std::string host_path) : guest(std::move(guest_path)), host(std::move(host_path)) {}

    std::string guest;  // absolute, normal
    std::string host;
    int folder = -1;    // the host folder once opened (ADR-0052); AT_FDCWD for a mounted file, which needs none
};

/// Opens a mount's host folder once it is there. A mounted file is reached by its host path instead, without its
/// folder: iOS's sandbox lets an app open /dev/urandom but not read /dev (the iPhone's device suite, 2026-10-06).
/// A folder the app may not read stays closed, and nothing below it is there.
void open_folder(Mount &mount) {
    if (mount.folder != -1) return;
    struct stat status;
    if (::stat(mount.host.c_str(), &status) != 0) return;  // not there yet
    mount.folder = S_ISDIR(status.st_mode) ? ::open(mount.host.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) : AT_FDCWD;
}

void close_folder(Mount &mount) {
    if (mount.folder >= 0) ::close(mount.folder);
    mount.folder = -1;
}

/// The devices every Linux system has. /dev/random never blocks, like on Linux 5.6 and later;
/// bionic's arc4random checks access("/dev/urandom") early and otherwise lives on the 16 bytes of
/// AT_RANDOM alone.
std::vector<Mount> devices() {
    return {{"/dev/null", "/dev/null"},
            {"/dev/zero", "/dev/zero"},
            {"/dev/random", "/dev/urandom"},
            {"/dev/urandom", "/dev/urandom"}};
}

/// A link of Android's that the guest finds where Android 15 puts it in an app's mount namespace, though no mount
/// holds it; both paths absolute and normal.
struct Link {
    std::string guest, target;
};

/// /data/user/0 is /data/data (app data isolation, Zygote.cpp), and /sdcard is /storage/self/primary, which is
/// /storage/emulated/0.
const std::vector<Link> &links() {
    static const auto *table = new std::vector<Link>{{"/data/user/0", "/data/data"},
                                                     {"/sdcard", "/storage/self/primary"},
                                                     {"/storage/self/primary", "/storage/emulated/0"}};
    return *table;
}

/// The link at `guest`, a path in normal form, or null.
const Link *link_at(std::string_view guest) {
    for (const auto &link : links()) {
        if (link.guest == guest) return &link;
    }
    return nullptr;
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

std::string absolute(std::string_view path, std::string_view base, bool follow_last) {
    // Linux's path walk over the runtime's links: the components in turn, a link replaced by its target where the
    // walk meets it, so that ".." after a link goes up from its target. The base takes the same walk. The path walked
    // so far is one string, cut back at a "..": one pass over the text, however many components it has.
    std::string text;
    if (!path.starts_with('/')) {
        text = base;
        text += '/';
    }
    text += path;
    std::string walked;               // empty for the root
    std::vector<std::size_t> starts;  // where each component of `walked` starts, at its slash
    for (std::size_t position = 0; position < text.size();) {
        const auto end = std::min(text.find('/', position), text.size());
        const auto part = std::string_view(text).substr(position, end - position);
        position = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!starts.empty()) {
                walked.resize(starts.back());
                starts.pop_back();
            }
            continue;
        }
        starts.push_back(walked.size());
        walked += '/';
        walked += part;
        // The last component has nothing after it: a trailing "/" or "." makes a link one to follow (LOOKUP_FOLLOW).
        const bool last = end == text.size();
        // The table's links neither loop nor lead to one another but forward, so a few rounds end the chain.
        for (int round = 0; round < 8 && (!last || follow_last); ++round) {
            const Link *link = link_at(walked);
            if (link == nullptr) break;
            walked.clear();
            starts.clear();
            for (std::size_t at = 1; at < link->target.size();) {
                const auto stop = std::min(link->target.find('/', at), link->target.size());
                starts.push_back(walked.size());
                walked += '/';
                walked.append(link->target, at, stop - at);
                at = stop + 1;
            }
        }
    }
    return walked.empty() ? std::string("/") : walked;
}

std::optional<std::string> link_target(std::string_view guest) {
    if (const Link *link = link_at(guest)) return link->target;
    return std::nullopt;
}

void mount(std::string_view guest, std::string_view host) {
    const auto normal = absolute(guest, "/");  // as Linux's mount follows the links of its target
    const std::lock_guard lock(mutex);
    for (auto &existing : mounts) {
        if (existing.guest == normal) {
            close_folder(existing);
            existing.host = host;
            return;
        }
    }
    mounts.push_back({normal, std::string(host)});
}

void reset() {
    const std::lock_guard lock(mutex);
    for (auto &mount : mounts) close_folder(mount);
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

std::optional<Location> locate(std::string_view guest) {
    const std::lock_guard lock(mutex);
    Mount *best = nullptr;
    for (auto &candidate : mounts) {
        if (covers(candidate.guest, guest) && (best == nullptr || candidate.guest.size() > best->guest.size())) {
            best = &candidate;
        }
    }
    if (best == nullptr) return std::nullopt;
    open_folder(*best);
    std::string_view rest = best->guest == "/" ? guest : guest.substr(best->guest.size());
    while (rest.starts_with('/')) rest.remove_prefix(1);
    if (best->folder == AT_FDCWD) {  // a mounted file: its host path alone, with no guest text after it
        if (!rest.empty()) return Location{.below_a_file = true};
        return Location{AT_FDCWD, best->host};
    }
    if (best->folder < 0) return std::nullopt;
    return Location{best->folder, rest.empty() ? std::string(".") : std::string(rest)};
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
    auto resolved = absolute(guest, "/");  // getcwd names the folder after the links, as Linux's d_path does
    const std::lock_guard lock(mutex);
    cwd = std::move(resolved);
}

}  // namespace apkrt::vfs
