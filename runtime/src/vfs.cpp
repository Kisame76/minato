#include "vfs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
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
    int id = 0;         // 0 for a start-time or device mount; > 0 for a live mount, which unmount() removes
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
/// holds it, or the runtime's in place of a folder of Android 15's with another's files; both paths absolute and
/// normal.
struct Link {
    std::string guest, target;
};

/// /data/user/0 is /data/data (app data isolation, Zygote.cpp), and /sdcard is /storage/self/primary, which is
/// /storage/emulated/0. /system/etc/security/cacerts is a folder on Android 15 with the CA certificates of Conscrypt's
/// APEX folder, which TrustedCertificateStore reads from SDK 34 on: the guest has them once, in Conscrypt's folder,
/// and lstat and readlink show a link at the other name (ADR-0046 decision 4).
const std::vector<Link> &links() {
    static const auto *table =
        new std::vector<Link>{{"/data/user/0", "/data/data"},
                              {"/sdcard", "/storage/self/primary"},
                              {"/storage/self/primary", "/storage/emulated/0"},
                              {"/system/etc/security/cacerts", "/apex/com.android.conscrypt/cacerts"}};
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
int next_live_id = 1;                   // guarded by mutex; the next live mount's id, never reused
std::vector<int> retired_folders;       // guarded by mutex; unmounted live folders kept open until reset (tests),
                                        // else reclaimed at process exit (no reset caller in production)

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
    for (const int folder : retired_folders) ::close(folder);
    retired_folders.clear();
    mounts = devices();
    cwd = "/";
}

int mount_live(std::string_view guest, std::string_view host) {
    const auto normal = absolute(guest, "/");  // follow the runtime's links, as Linux's mount does for its target
    if (normal == "/") return -EINVAL;         // never over the root
    // Open the folder now, following no symlink at its last component (ADR-0057): the guest then reaches only
    // what truly lies below it, since locate() opens every path under this fd with RESOLVE_BENEATH.
    const std::string host_dir(host);  // named, so nothing frees (and may clobber errno) before the open's errno
    const int folder = ::open(host_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    // On a symlink the open fails without following it: ENOTDIR here on Darwin, because O_DIRECTORY rejects the
    // unfollowed link where O_NOFOLLOW alone would give ELOOP; also -ENOTDIR for a non-directory, -ENOENT when absent.
    if (folder < 0) return -errno;
    const std::lock_guard lock(mutex);
    bool under_a_mount = false;
    for (const auto &existing : mounts) {
        if (covers(normal, existing.guest)) {  // normal equals or lies above a mount: never over one
            ::close(folder);
            return -EEXIST;
        }
        if (covers(existing.guest, normal)) under_a_mount = true;  // normal lies strictly below this mount
    }
    if (!under_a_mount) {  // not under the app's storage (nor anything else mounted)
        ::close(folder);
        return -EACCES;
    }
    const int id = next_live_id++;
    mounts.push_back({normal, host_dir});
    mounts.back().folder = folder;
    mounts.back().id = id;
    return id;
}

int unmount(int id) {
    if (id <= 0) return -EINVAL;  // 0 is a start-time or device mount, never removed this way
    const std::lock_guard lock(mutex);
    for (auto it = mounts.begin(); it != mounts.end(); ++it) {
        if (it->id == id) {
            // Keep the folder open (retired, closed at reset): a guest thread may still hold it from a
            // locate() just before this unmount, and closing it now could let its fd number be reused.
            if (it->folder >= 0) retired_folders.push_back(it->folder);
            mounts.erase(it);
            return 0;
        }
    }
    return -EINVAL;
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
