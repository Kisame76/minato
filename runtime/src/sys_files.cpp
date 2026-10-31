// File syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): Linux paths, flags and
// structures on Darwin's file descriptors. Every path goes through the path table (vfs.h), every
// descriptor through the guest's descriptor table (descriptors.h, ADR-0027).
#include "descriptors.h"
#include "identity.h"
#include "memfd.h"
#include "proc.h"
#include "sockets.h"
#include "syscalls.h"
#include "vfs.h"
#include "watches.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <strings.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apkrt::sys {
namespace {

using descriptors::Description;

// Linux fcntl commands and status flags (include/uapi/asm-generic/fcntl.h, arm64 fcntl.h).
namespace linux_fcntl {
constexpr int dupfd = 0;
constexpr int getfd = 1;
constexpr int setfd = 2;
constexpr int getfl = 3;
constexpr int setfl = 4;
constexpr int getlk = 5;
constexpr int setlk = 6;
constexpr int setlkw = 7;
constexpr int dupfd_cloexec = 1030;
constexpr int add_seals = 1033;
constexpr int get_seals = 1034;
constexpr int append = 02000;
constexpr int nonblock = 04000;
constexpr int dsync = 010000;
constexpr int async = 020000;
constexpr int sync = 04000000 | dsync;
}  // namespace linux_fcntl

/// Runs `host_call` with the host descriptor behind the guest's `fd`, which stays open until it
/// returns even when another thread closes `fd` meanwhile, as on Linux; -EBADF when `fd` is not open.
template <typename HostCall>
long with_host(int fd, HostCall host_call) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    return host_call(description->host());
}

/// with_host for a call only files take: an eventfd or an epoll answers EINVAL, as Linux's anonymous
/// files have no such operation.
template <typename HostCall>
long with_file(int fd, HostCall host_call) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    if (description->kind() != Description::Kind::file) return -linux_errno::einval;
    return host_call(description->host());
}

/// File status flags of an open file, Darwin to Linux (for F_GETFL).
int to_linux_status_flags(int flags) {
    int result = flags & O_ACCMODE;  // O_RDONLY, O_WRONLY, O_RDWR agree
    if (flags & O_APPEND) result |= linux_fcntl::append;
    if (flags & O_NONBLOCK) result |= linux_fcntl::nonblock;
    if (flags & O_ASYNC) result |= linux_fcntl::async;
    if (flags & O_SYNC) result |= linux_fcntl::sync;
    else if (flags & O_DSYNC) result |= linux_fcntl::dsync;
    return result;
}

/// Linux's struct flock on arm64 (include/uapi/asm-generic/fcntl.h). Darwin's has its fields in another order and
/// numbers its lock types otherwise.
struct LinuxFlock {
    std::int16_t type;
    std::int16_t whence;
    std::int64_t start;
    std::int64_t length;
    std::int32_t pid;
};
static_assert(sizeof(LinuxFlock) == 32);

constexpr std::int16_t linux_rdlck = 0, linux_wrlck = 1, linux_unlck = 2;

/// F_GETLK, F_SETLK and F_SETLKW (`command`) on Darwin's record locks, POSIX's as Linux's are: a process's own never
/// conflict, and all of a file's go when the process closes any descriptor of it. Linux's checks first
/// (flock_to_posix_lock, fcntl_getlk). F_SETLKW tries again after a pause that a signal ends (EINTR, which SA_RESTART
/// restarts), as no host event tells when another process lets go; Linux's EDEADLK between waiting processes does not
/// come.
long record_lock(int fd, int command, std::uint64_t argument) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    if (argument == 0) return -linux_errno::efault;
    LinuxFlock guest;
    std::memcpy(&guest, as_pointer<const void>(argument), sizeof guest);
    struct flock host{};
    switch (guest.type) {
        case linux_rdlck: host.l_type = F_RDLCK; break;
        case linux_wrlck: host.l_type = F_WRLCK; break;
        case linux_unlck: host.l_type = F_UNLCK; break;
        default: return -linux_errno::einval;
    }
    if (guest.whence != SEEK_SET && guest.whence != SEEK_CUR && guest.whence != SEEK_END) return -linux_errno::einval;
    host.l_whence = guest.whence;  // the same numbers
    host.l_start = guest.start;
    host.l_len = guest.length;
    const int file = description->host();
    if (command == linux_fcntl::getlk) {
        if (guest.type == linux_unlck) return -linux_errno::einval;
        if (::fcntl(file, F_GETLK, &host) != 0) return -to_linux_errno(errno);
        // Nothing in the way: F_UNLCK, the rest as it was. Else the lock in the way, from the file's start.
        guest.type = host.l_type == F_RDLCK ? linux_rdlck : host.l_type == F_WRLCK ? linux_wrlck : linux_unlck;
        if (guest.type != linux_unlck) {
            guest.whence = SEEK_SET;
            guest.start = host.l_start;
            guest.length = host.l_len;
            guest.pid = host.l_pid;
        }
        std::memcpy(as_pointer<void>(argument), &guest, sizeof guest);
        return 0;
    }
    const auto conflict = [] { return errno == EAGAIN || errno == EACCES; };
    if (::fcntl(file, F_SETLK, &host) == 0) return 0;
    if (command == linux_fcntl::setlk || !conflict()) return -to_linux_errno(errno);
    auto &state = signals::current();
    std::int64_t pause = 1'000'000;  // 1 ms, then twice as long each time, up to 64 ms
    while (true) {
        if (wait_until(state, [] { return false; }, monotonic_now() + pause) == WaitEnd::signal) {
            return -linux_errno::eintr;
        }
        if (::fcntl(file, F_SETLK, &host) == 0) return 0;
        if (!conflict()) return -to_linux_errno(errno);
        pause = std::min<std::int64_t>(2 * pause, 64'000'000);
    }
}

}  // namespace

long sys_fcntl(int fd, int command, std::uint64_t argument) {
    switch (command) {
        case linux_fcntl::dupfd: return descriptors::duplicate(fd, as_int(argument), false);
        case linux_fcntl::dupfd_cloexec: return descriptors::duplicate(fd, as_int(argument), true);
        case linux_fcntl::getfd: return descriptors::close_on_exec(fd);  // FD_CLOEXEC is 1 on both
        case linux_fcntl::setfd: return descriptors::set_close_on_exec(fd, (as_int(argument) & FD_CLOEXEC) != 0);
        case linux_fcntl::getfl: {
            const auto description = descriptors::get(fd);
            if (!description) return -linux_errno::ebadf;
            int flags = ::fcntl(description->host(), F_GETFL);
            if (flags < 0) return -to_linux_errno(errno);
            // An eventfd or an epoll is open for reading and writing, as Linux's anonymous files are.
            if (description->kind() != Description::Kind::file) flags = (flags & ~O_ACCMODE) | O_RDWR;
            // O_NONBLOCK is the guest's own (descriptors.h), whatever the host descriptor has.
            return to_linux_status_flags(flags & ~O_NONBLOCK) | (description->nonblocking() ? linux_fcntl::nonblock : 0);
        }
        case linux_fcntl::setfl: {
            const auto description = descriptors::get(fd);
            if (!description) return -linux_errno::ebadf;
            // Linux changes only O_APPEND, O_ASYNC, O_DIRECT, O_NOATIME and O_NONBLOCK here and ignores
            // the rest; Darwin has no O_DIRECT or O_NOATIME to set. O_NONBLOCK is the guest's own, and a
            // flag that does not change leaves the host descriptor alone, which an adopted one shares
            // with the starter.
            const int requested = as_int(argument);
            const int host = description->host();
            const int current = ::fcntl(host, F_GETFL);
            if (current < 0) return -to_linux_errno(errno);
            int flags = current & ~(O_APPEND | O_ASYNC);
            if (requested & linux_fcntl::append) flags |= O_APPEND;
            if (requested & linux_fcntl::async) flags |= O_ASYNC;
            if (flags != current && ::fcntl(host, F_SETFL, flags) != 0) return -to_linux_errno(errno);
            description->set_nonblocking((requested & linux_fcntl::nonblock) != 0);
            return 0;
        }
        case linux_fcntl::getlk:
        case linux_fcntl::setlk:
        case linux_fcntl::setlkw: return record_lock(fd, command, argument);
        case linux_fcntl::add_seals:
        case linux_fcntl::get_seals: {
            const auto description = descriptors::get(fd);
            if (!description) return -linux_errno::ebadf;
            if (command == linux_fcntl::get_seals) return memfd::get_seals(*description);
            return memfd::add_seals(*description, static_cast<unsigned>(argument));
        }
        default: {
            if (!descriptors::get(fd)) return -linux_errno::ebadf;  // Linux's fcntl takes the file first
            char message[80];
            const int length =
                std::snprintf(message, sizeof message, "apkrt: fcntl command %d is not supported yet\n", command);
            if (length > 0) static_cast<void>(::write(STDERR_FILENO, message, static_cast<std::size_t>(length)));
            return -linux_errno::einval;
        }
    }
}

namespace {

/// struct stat as Linux fills it on arm64 (include/uapi/asm-generic/stat.h).
struct LinuxStat {
    std::uint64_t dev;
    std::uint64_t ino;
    std::uint32_t mode;  // file type and permission bits agree with Darwin's
    std::uint32_t nlink;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint64_t rdev;
    std::uint64_t pad1;
    std::int64_t size;
    std::int32_t blksize;
    std::int32_t pad2;
    std::int64_t blocks;
    std::int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    std::uint32_t unused4, unused5;
};
static_assert(sizeof(LinuxStat) == 128);

/// Device numbers: Darwin keeps the major in the top 8 bits, Linux spreads the minor around a
/// 12-bit major (new_encode_dev).
std::uint64_t to_linux_device(dev_t device) {
    const auto value = static_cast<std::uint32_t>(device);
    const std::uint64_t major_number = (value >> 24) & 0xff;
    const std::uint64_t minor_number = value & 0xff'ffff;
    return (minor_number & 0xff) | (major_number << 8) | ((minor_number & ~std::uint64_t{0xff}) << 12);
}

/// A file's owner and group, and Android's mode bits where the model has them (/data's own folders, ADR-0052), 0
/// where the host's count.
struct Owner {
    std::uint32_t uid = 0, gid = 0;
    mode_t mode = 0;
};

constexpr std::uint32_t system_uid = 1000;       // AID_SYSTEM
constexpr std::uint32_t ext_data_rw_gid = 1078;  // AID_EXT_DATA_RW

/// The owner Android 15 gives the file at the guest path `path` (ADR-0039 decision 3) [verified: the emulator,
/// 2026-10-01]: an app's code folder under /data/app belongs to system, the rest of /data to the app, external
/// storage to the app with the group ext_data_rw, /proc's files to the process, and everything else to root. The
/// folders match without regard to ASCII case: on a host that ignores case (the Mac's file system, so the
/// Simulator's), /data/APP/... is the same file as /data/app/....
Owner android_owner(std::string_view path) {
    const auto below = [path](std::string_view folder) {
        return path.size() >= folder.size() && ::strncasecmp(path.data(), folder.data(), folder.size()) == 0 &&
               (path.size() == folder.size() || path[folder.size()] == '/');
    };
    const auto is = [path](std::string_view folder) {
        return path.size() == folder.size() && ::strncasecmp(path.data(), folder.data(), folder.size()) == 0;
    };
    // /data's own folders and the folders between them and the app's own, with the owners and modes an app sees on
    // Android 15, so that the app makes, renames and removes no entry there (ADR-0052 with decision 3) [verified:
    // owner-rules-test's device lines on the emulator, 2026-10-05]. /data/user, /data/user/0 and /data/data are
    // root's with the app data isolation's 0751, and so are /data/user_de and /data/user_de/0, the parents of the
    // device protected data, zygote's tmpfs and the user's folder in it (com_android_internal_os_Zygote.cpp); files
    // below keep the host's modes.
    const auto depth_below = [path](std::size_t folder) { return std::count(path.begin() + folder, path.end(), '/'); };
    if (is("/data") || is("/data/app")) return {system_uid, system_uid, 0771};
    if (is("/data/user") || is("/data/user/0") || is("/data/data") || is("/data/user_de") || is("/data/user_de/0")) {
        return {0, 0, 0751};
    }
    if (below("/data/dalvik-cache")) {
        const auto depth = depth_below(18);
        return {0, 0, depth == 0 ? mode_t{0771} : depth == 1 ? mode_t{0711} : mode_t{0}};
    }
    if (below("/data/app")) {
        const auto depth = depth_below(9);  // a ~~ folder, and the app's code folder in it
        return {system_uid, system_uid, depth == 1 ? mode_t{0771} : depth == 2 ? mode_t{0775} : mode_t{0}};
    }
    // Where SystemUI and idmap2 leave Monet's overlays (ADR-0043 f): system's, the folder 0771 and its files 0644
    // [verified: ls -la on the emulator, 2026-10-07], so the app reads them and changes nothing there.
    if (is("/data/resource-cache")) return {system_uid, system_uid, 0771};
    if (below("/data/resource-cache")) return {system_uid, system_uid, 0644};
    if (below("/data") || below("/proc")) return {app_uid, app_uid};
    if (below("/storage") || below("/sdcard")) return {app_uid, ext_data_rw_gid};
    return {};
}

/// Whether the guest path `path` lies on one of the system's partitions, which Android 15 mounts read-only (ADR-0052's
/// later step): /system, /system_ext, /vendor, /product and /odm, and each APEX below /apex, whose own folder is a
/// tmpfs. Every change there answers EROFS before the permission checks, as Linux checks the mount first
/// (sb_permission, mnt_want_write) [verified: owner-rules-test's system and apex lines on the emulator, 2026-10-06].
/// The folders match without regard to ASCII case, as the owners' do.
bool read_only(std::string_view path) {
    const auto below = [path](std::string_view folder) {
        return path.size() >= folder.size() && ::strncasecmp(path.data(), folder.data(), folder.size()) == 0 &&
               (path.size() == folder.size() || path[folder.size()] == '/');
    };
    for (const std::string_view folder : {"/system", "/system_ext", "/vendor", "/product", "/odm"}) {
        if (below(folder)) return true;
    }
    return path.size() > 6 && ::strncasecmp(path.data(), "/apex/", 6) == 0;
}

/// The guest folder whose entry a guest path in normal form names. Linux's create, remove, rename and link go by the
/// mount of that folder (filename_create, filename_parentat), the other changes by the file's: so an entry of /apex
/// itself, a tmpfs, is not an APEX's.
std::string_view folder_of(std::string_view guest) {
    const auto slash = guest.rfind('/');
    return slash == std::string_view::npos || slash == 0 ? std::string_view("/") : guest.substr(0, slash);
}

/// A file's mode bits as stat reports them: Android's for /data's own folders, the host's else.
mode_t mode_of(mode_t host, const Owner &owner) {
    return owner.mode != 0 ? static_cast<mode_t>((host & S_IFMT) | owner.mode) : host;
}

/// The owner of the file a descriptor holds, by the path the runtime wrote it for or where the guest sees its
/// host file. A pipe, which has no path, belongs to the process, as on Linux; a host file outside the guest's
/// paths belongs to root.
Owner owner_of(const Description &description) {
    // A memfd is the app's, as Linux makes it with the caller's ids, with its own mode (memfd.h).
    if (const auto *file = memfd::as_file(description)) return {app_uid, app_uid, memfd::mode_of(*file)};
    if (!description.written_path.empty()) return android_owner(description.written_path);
    char host[MAXPATHLEN];
    if (::fcntl(description.host(), F_GETPATH, host) != 0) return {app_uid, app_uid};
    const auto guest = vfs::guest_path(host);
    return guest ? android_owner(*guest) : Owner{};
}

/// Whether a descriptor's file lies on the system's read-only partitions, by the path the runtime wrote it for or the
/// guest path its host file has; a pipe's and a file the guest cannot name do not.
bool read_only(const Description &description) {
    if (!description.written_path.empty()) return read_only(description.written_path);
    char host[MAXPATHLEN];
    if (::fcntl(description.host(), F_GETPATH, host) != 0) return false;
    const auto guest = vfs::guest_path(host);
    return guest && read_only(*guest);
}

LinuxStat to_linux_stat(const struct stat &host, Owner owner) {
    LinuxStat result{};
    result.dev = to_linux_device(host.st_dev);
    result.ino = host.st_ino;
    result.mode = mode_of(host.st_mode, owner);
    result.nlink = host.st_nlink;
    result.uid = owner.uid;
    result.gid = owner.gid;
    result.rdev = to_linux_device(host.st_rdev);
    result.size = host.st_size;
    result.blksize = host.st_blksize;
    result.blocks = host.st_blocks;
    result.atime = host.st_atimespec.tv_sec;
    result.atime_nsec = host.st_atimespec.tv_nsec;
    result.mtime = host.st_mtimespec.tv_sec;
    result.mtime_nsec = host.st_mtimespec.tv_nsec;
    result.ctime = host.st_ctimespec.tv_sec;
    result.ctime_nsec = host.st_ctimespec.tv_nsec;
    return result;
}

/// What Linux shows for an eventfd or an epoll: its anonymous inode, one for all of them, which has
/// no file type, belongs to root and has the size of a page for its blocks.
LinuxStat anonymous_inode() {
    LinuxStat result{};
    result.dev = 14;  // 0:14, where Android 15 has the anonymous inodes' file system
    result.ino = 1;
    result.mode = 0600;
    result.nlink = 1;
    result.blksize = 4096;
    return result;
}

}  // namespace

long sys_fstat(int fd, std::uint64_t buffer) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    if (description->kind() != Description::Kind::file) {
        *as_pointer<LinuxStat>(buffer) = anonymous_inode();
        return 0;
    }
    struct stat status;
    if (::fstat(description->host(), &status) != 0) return -to_linux_errno(errno);
    LinuxStat result = to_linux_stat(status, owner_of(*description));
    if (memfd::as_file(*description) != nullptr) result.nlink = 0;  // unlinked, as Linux's memfd is
    *as_pointer<LinuxStat>(buffer) = result;
    return 0;
}

namespace {

// *at() flags (include/uapi/linux/fcntl.h).
constexpr int linux_at_fdcwd = -100;
constexpr int linux_at_symlink_nofollow = 0x100;
constexpr int linux_at_removedir = 0x200;
constexpr int linux_at_eaccess = 0x200;  // for faccessat2, same bit as AT_REMOVEDIR
constexpr int linux_at_empty_path = 0x1000;

// Linux open flags (include/uapi/asm-generic/fcntl.h, arch/arm64/include/uapi/asm/fcntl.h).
namespace linux_open {
constexpr int access_mode = 03;  // O_RDONLY, O_WRONLY, O_RDWR agree with Darwin
constexpr int create = 0100;
constexpr int exclusive = 0200;
constexpr int no_ctty = 0400;
constexpr int truncate = 01000;
constexpr int append = 02000;
constexpr int nonblock = 04000;
constexpr int dsync = 010000;
constexpr int async = 020000;
constexpr int directory = 040000;
constexpr int no_follow = 0100000;
constexpr int cloexec = 02000000;
constexpr int sync = 04000000;
constexpr int path = 010000000;
constexpr int tmpfile = 020000000;
}  // namespace linux_open

/// Where a guest path lies on the host (ADR-0052): `path` below `directory`, the host folder of the path's mount,
/// which the mount table keeps open (vfs::locate), and the guest path in normal form; or an error. Every host call
/// starts at `directory` with Darwin's RESOLVE_BENEATH, so that no link and no ".." leads out of the mount; a mounted
/// file is `directory` AT_FDCWD and its host path from the table, which host calls take as it is (beneath).
struct HostPath {
    long error = 0;
    int directory = -1;
    std::string path;
    std::string guest;
    std::optional<std::string> link;  // the text of one of the guest's links (vfs::link_target) the path names
};

/// Resolves a path argument of openat and its relatives through the path table: an absolute path, one relative to
/// the working directory, or one relative to a directory descriptor, below the guest path of that descriptor's
/// folder, which the guest can only have opened through the table. The guest's links are followed on the way, and
/// the last one too unless `follow_last` is false: a path that then names a link has its text in `link` and no host
/// place.
HostPath resolve(int directory, std::uint64_t address, bool follow_last = true) {
    if (address == 0) return {.error = -linux_errno::efault};
    const auto *start = as_pointer<const char>(address);
    const std::string_view text(start, ::strnlen(start, vfs::path_max));
    if (text.size() == vfs::path_max) return {.error = -linux_errno::enametoolong};  // no NUL within PATH_MAX
    if (text.empty()) return {.error = -linux_errno::enoent};
    std::string base = "/";
    if (!text.starts_with('/')) {
        if (directory == linux_at_fdcwd) {
            base = vfs::working_directory();
        } else {
            const auto held = descriptors::get(directory);
            if (!held) return {.error = -linux_errno::ebadf};
            if (held->kind() != Description::Kind::file) return {.error = -linux_errno::enotdir};
            char folder[MAXPATHLEN];
            if (::fcntl(held->host(), F_GETPATH, folder) != 0) return {.error = -to_linux_errno(errno)};
            const auto guest = vfs::guest_path(folder);
            if (!guest) return {.error = -linux_errno::enoent};  // a folder the guest cannot name
            base = *guest;
        }
    }
    auto guest = vfs::absolute(text, base, follow_last);
    if (auto link = follow_last ? std::nullopt : vfs::link_target(guest)) {
        return {.guest = std::move(guest), .link = std::move(link)};
    }
    auto location = vfs::locate(guest);
    if (!location) return {.error = -linux_errno::enoent};
    if (location->below_a_file) return {.error = -linux_errno::enotdir};
    // "file/" is not "file": Linux answers ENOTDIR for it, and so does Darwin with the slash kept.
    if (text.ends_with('/') && location->path != "." && !location->path.ends_with('/')) location->path += '/';
    return {.directory = location->folder, .path = std::move(location->path), .guest = std::move(guest)};
}

/// The absolute guest path, in normal form, of a path argument that does not go through a directory
/// descriptor: how the files the runtime writes itself are found (proc.h). Nullopt for the others.
std::optional<std::string> guest_absolute(int directory, std::uint64_t address) {
    if (address == 0) return std::nullopt;
    const auto *start = as_pointer<const char>(address);
    const std::string_view guest(start, ::strnlen(start, vfs::path_max));
    if (guest.empty() || guest.size() == vfs::path_max || (!guest.starts_with('/') && directory != linux_at_fdcwd)) {
        return std::nullopt;  // resolve answers a path that is too long
    }
    return vfs::absolute(guest, guest.starts_with('/') ? "/" : vfs::working_directory());
}

/// Darwin's *at() flag for AT_SYMLINK_NOFOLLOW.
int host_at_flags(int linux_flags) {
    return (linux_flags & linux_at_symlink_nofollow) ? AT_SYMLINK_NOFOLLOW : 0;
}

/// Darwin's RESOLVE_BENEATH `flag` for a host call at `host`; none for a mounted file, whose host path is the mount
/// table's alone.
int beneath(const HostPath &host, int flag) {
    return host.directory == AT_FDCWD ? 0 : flag;
}

/// The owner stat reports for a path argument, by its guest path as written.
Owner owner_at(const HostPath &host) {
    return android_owner(host.guest);
}


/// The bits of the host's `mode`, or of Android's for /data's own folders, that Linux's generic_permission grants
/// the app uid and gid on a file of `owner`: the owner's when the app owns it, the group's when the app's gid is its
/// group, the others' else.
int granted(mode_t mode, Owner owner) {
    mode = mode_of(mode, owner);
    if (owner.uid == app_uid) return (mode >> 6) & 07;
    if (owner.gid == app_uid) return (mode >> 3) & 07;
    return mode & 07;
}

// --- Who may change a file or a folder's entries (ADR-0039 decision 3) -------------------------------------------
// The host's files and folders all belong to the app's process, so the host would let every change through. Before
// the host's call, the guest gets Linux's checks with the owners stat reports, in Linux's order after its lookups, and
// the answers Android 15 gives an app: its code under /data/app is system's and refuses every change, its own
// folders take them (owner-rules-test against the emulator, 2026-10-05). The checks look at what the host call will
// touch, found below the mount as the call finds it, not at the guest path as written (ADR-0052: a link inside one
// mount, the lead's reviewer).

/// The file or folder a path argument names, following a final symlink or not; nullopt when it is not there, or not
/// below its mount, which the host's call then answers.
std::optional<struct stat> entry_at(const HostPath &host, bool follow) {
    struct stat status;
    const int flags = (follow ? 0 : AT_SYMLINK_NOFOLLOW) | beneath(host, AT_RESOLVE_BENEATH);
    if (::fstatat(host.directory, host.path.c_str(), &status, flags) != 0) return std::nullopt;
    return status;
}

/// The owner and status of a host descriptor's file, by the guest path its host path names back: root's for a file
/// no guest path names.
std::pair<Owner, struct stat> owned(int descriptor) {
    std::pair<Owner, struct stat> result{};
    char real[MAXPATHLEN];
    if (::fstat(descriptor, &result.second) != 0 || ::fcntl(descriptor, F_GETPATH, real) != 0) return result;
    if (const auto guest = vfs::guest_path(real)) result.first = android_owner(*guest);
    return result;
}

/// What a host call at `host` touches: the file, or with `follow` false a final link itself, opened below its mount
/// without being read (O_EVTONLY), with its owner and status; nullopt when it is not there or not below its mount.
std::optional<std::pair<Owner, struct stat>> touched(const HostPath &host, bool follow) {
    const int flags = O_EVTONLY | O_CLOEXEC | beneath(host, O_RESOLVE_BENEATH) | (follow ? 0 : O_SYMLINK);
    const int descriptor = ::openat(host.directory, host.path.c_str(), flags);
    if (descriptor < 0) return std::nullopt;
    const auto result = owned(descriptor);
    ::close(descriptor);
    return result;
}

/// The folder of a path argument's last component, opened below its mount, and that component's name: where an
/// entry is made or removed, and the host calls that take no flags (mkdirat, symlinkat, readlinkat) start there, so
/// that their path leads nowhere else either. A mounted file opens none: AT_FDCWD and its host path.
class LastFolder {
public:
    explicit LastFolder(const HostPath &host) {
        if (host.directory == AT_FDCWD) {
            descriptor_ = AT_FDCWD;
            name_ = host.path;
            return;
        }
        std::string path = host.path;
        while (path.size() > 1 && path.ends_with('/')) path.pop_back();
        const auto slash = path.rfind('/');
        name_ = slash == std::string::npos ? path : path.substr(slash + 1);
        const std::string folder = slash == std::string::npos ? "." : path.substr(0, slash);
        descriptor_ = ::openat(host.directory, folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_RESOLVE_BENEATH);
        error_ = descriptor_ < 0 ? -to_linux_errno(errno) : 0;
    }
    ~LastFolder() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    LastFolder(const LastFolder &) = delete;
    LastFolder &operator=(const LastFolder &) = delete;

    int descriptor() const { return descriptor_; }
    const std::string &name() const { return name_; }
    long error() const { return error_; }

private:
    int descriptor_ = -1;
    std::string name_;
    long error_ = 0;
};

/// may_create's and may_delete's check (fs/namei.c): write and search permission on the entry's folder, EACCES
/// without, as Android 15 answers in the app's code folder (symlink, mkdir, create, rename and unlink there). A folder
/// that is not there is the host call's to answer.
long may_change_entries(const HostPath &host) {
    const LastFolder folder(host);
    if (folder.error() != 0) return 0;
    if (folder.descriptor() == AT_FDCWD) return -linux_errno::eacces;  // a mounted file's: /dev, the code folder
    const auto [owner, status] = owned(folder.descriptor());
    return (granted(status.st_mode, owner) & 03) == 03 ? 0 : -linux_errno::eacces;
}

/// may_open's check for writing (fs/namei.c): write permission on the file, EACCES without, as Android 15 answers
/// for the app's base.apk (opening for writing, O_TRUNC). A folder is the host's to refuse with EISDIR.
long may_write(const HostPath &host) {
    const auto object = touched(host, true);
    if (!object || S_ISDIR(object->second.st_mode)) return 0;
    return (granted(object->second.st_mode, object->first) & 02) != 0 ? 0 : -linux_errno::eacces;
}

/// setattr's check (chmod, chown, times): the app owns the file. For one it does not, Android 15 answers EACCES, as
/// its SELinux policy refuses the setattr before Linux's EPERM (chmod and chown of base.apk, chmod of the code folder).
long may_set_attributes(const Owner &owner) {
    return owner.uid == app_uid ? 0 : -linux_errno::eacces;
}

/// Whether two path arguments lie in different mounts, where Linux's rename and link answer EXDEV before their
/// permission checks: Android 15 mounts an app's data and external folders on their own (code to files: EXDEV).
bool in_different_mounts(const HostPath &host, const HostPath &other) {
    const auto mount = vfs::mount_point(host.guest), other_mount = vfs::mount_point(other.guest);
    return mount && other_mount && *mount != *other_mount;
}

/// Whether two path arguments' folders, which hold their names, lie in different mounts: rename's EXDEV, which
/// compares those (do_renameat2), not the mounts of what the names are.
bool folders_in_different_mounts(const HostPath &host, const HostPath &other) {
    const auto mount = vfs::mount_point(folder_of(host.guest));
    const auto other_mount = vfs::mount_point(folder_of(other.guest));
    return mount && other_mount && *mount != *other_mount;
}

/// Whether `guest` is a mount's own folder or file. The folder that holds its name is no folder of the app's on
/// Android 15 (/data/data, /data/user_de/0, /data/app/~~<a>, Android/data): rename, unlink and rmdir check its write
/// permission (may_delete) before a mount point's EBUSY, so the app gets EACCES.
bool is_mounted_itself(std::string_view guest) {
    const auto mount = vfs::mount_point(guest);
    return guest != "/" && mount && *mount == guest;
}

/// Whether `guest` is the data folder of an app, /data/data/<package> or /data/user_de/0/<package> (the canonical
/// spellings; /data/user/0 is a link to /data/data), whose listing hides the runtime's ".minato".
bool is_app_data_folder(std::string_view guest) {
    for (const std::string_view parent : {"/data/data/", "/data/user_de/0/"}) {
        if (guest.size() > parent.size() && guest.substr(0, parent.size()) == parent &&
            guest.find('/', parent.size()) == std::string_view::npos) {
            return true;
        }
    }
    return false;
}

// --- Directory streams ---------------------------------------------------------------------------------
//
// getdents64 reads a directory through a DIR* of its open file description (descriptors.h), opened
// on a host duplicate at the first call. The guest's duplicates share it, as they share the position
// on Linux. Positions (d_off, lseek) count entries from the start.

/// struct linux_dirent64 (include/linux/dirent.h): 19 bytes of header, the name, 8-byte aligned.
constexpr std::size_t dirent_header = 19;

/// The next entry of the stream, leaving out ".minato" where the description hides it; positions count what is listed.
const dirent *next_entry(descriptors::Description &description) {
    const dirent *entry;
    while ((entry = ::readdir(description.stream)) != nullptr && description.hides_runtime_folder &&
           std::strcmp(entry->d_name, ".minato") == 0) {
    }
    return entry;
}

long get_directory_entries(descriptors::Description &description, std::uint64_t buffer, std::uint64_t size) {
    const std::lock_guard lock(description.stream_mutex);
    if (description.stream == nullptr) {
        struct stat status;
        if (::fstat(description.host(), &status) != 0) return -to_linux_errno(errno);
        if (!S_ISDIR(status.st_mode)) return -linux_errno::enotdir;
        const int copy = ::fcntl(description.host(), F_DUPFD_CLOEXEC, 0);
        if (copy < 0) return -to_linux_errno(errno);
        DIR *directory = ::fdopendir(copy);
        if (directory == nullptr) {
            const int error = errno;
            ::close(copy);
            return -to_linux_errno(error);
        }
        ::rewinddir(directory);
        description.stream = directory;
        description.stream_position = 0;
    }
    DIR *const directory = description.stream;
    auto *const output = as_pointer<char>(buffer);
    std::uint64_t used = 0;
    while (true) {
        const long before = ::telldir(directory);
        errno = 0;
        const dirent *entry = next_entry(description);
        if (entry == nullptr) {
            if (errno != 0 && used == 0) return -to_linux_errno(errno);
            break;
        }
        const std::size_t name_length = std::strlen(entry->d_name);
        const std::size_t record = (dirent_header + name_length + 1 + 7) & ~std::size_t{7};
        if (used + record > size) {
            ::seekdir(directory, before);  // it stays for the next call
            if (used == 0) return -linux_errno::einval;
            break;
        }
        char *const out = output + used;
        std::memset(out, 0, record);
        const std::uint64_t inode = entry->d_ino;
        const std::int64_t next = description.stream_position + 1;
        const auto length = static_cast<std::uint16_t>(record);
        std::memcpy(out, &inode, 8);
        std::memcpy(out + 8, &next, 8);
        std::memcpy(out + 16, &length, 2);
        out[18] = static_cast<char>(entry->d_type);  // DT_* values agree
        std::memcpy(out + dirent_header, entry->d_name, name_length);
        used += record;
        ++description.stream_position;
    }
    return static_cast<long>(used);
}

/// lseek on a directory getdents64 reads from: positions count entries.
std::optional<long> seek_stream(descriptors::Description &description, std::int64_t offset, int whence) {
    const std::lock_guard lock(description.stream_mutex);
    if (description.stream == nullptr) return std::nullopt;
    std::int64_t target;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = description.stream_position + offset; break;
        default: return -linux_errno::einval;
    }
    if (target < 0) return -linux_errno::einval;
    ::rewinddir(description.stream);
    description.stream_position = 0;
    while (description.stream_position < target && next_entry(description) != nullptr) {
        ++description.stream_position;
    }
    return description.stream_position;
}

}  // namespace

long sys_openat(int directory, std::uint64_t path, int flags, int mode) {
    if (flags & linux_open::tmpfile) return -linux_errno::eopnotsupp;
    if (const auto absolute = guest_absolute(directory, path)) {
        // A memfd's link in /proc/self/fd opens another open file of it (memfd.h).
        if (const auto fd = proc::descriptor_entry(*absolute)) {
            if (const auto reopened = memfd::reopen(*fd, flags)) return *reopened;
        }
        if (const auto written = proc::open(*absolute, flags)) return *written;
    }
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    // A file that is there needs write permission to be written or truncated (O_PATH only names it), and on a read-only
    // partition answers EROFS first, a folder EISDIR first, the host's; one that is not there needs its folder to be
    // created in (may_o_create), EROFS first there too once the folder is there.
    if (const auto entry = entry_at(host, true)) {
        const bool writes = (flags & linux_open::access_mode) != O_RDONLY || (flags & linux_open::truncate) != 0;
        const bool exclusive = (flags & linux_open::create) != 0 && (flags & linux_open::exclusive) != 0;
        if (writes && !exclusive && !(flags & linux_open::path)) {
            if (read_only(host.guest) && S_ISREG(entry->st_mode)) return -linux_errno::erofs;
            if (const long refused = may_write(host)) return refused;
        }
    } else if ((flags & linux_open::create) != 0 && errno == ENOENT) {
        if (read_only(folder_of(host.guest)) && LastFolder(host).error() == 0) return -linux_errno::erofs;
        if (const long refused = may_change_entries(host)) return refused;
    }
    int host_flags = flags & linux_open::access_mode;
    if (flags & linux_open::path) host_flags = O_RDONLY;  // an O_PATH descriptor only names the file
    if (flags & linux_open::create) host_flags |= O_CREAT;
    if (flags & linux_open::exclusive) host_flags |= O_EXCL;
    if (flags & linux_open::no_ctty) host_flags |= O_NOCTTY;
    if (flags & linux_open::truncate) host_flags |= O_TRUNC;
    if (flags & linux_open::append) host_flags |= O_APPEND;
    if (flags & linux_open::nonblock) host_flags |= O_NONBLOCK;
    if (flags & linux_open::async) host_flags |= O_ASYNC;
    if (flags & linux_open::directory) host_flags |= O_DIRECTORY;
    if (flags & linux_open::no_follow) host_flags |= O_NOFOLLOW;
    if (flags & linux_open::sync) host_flags |= O_SYNC;
    else if (flags & linux_open::dsync) host_flags |= O_DSYNC;
    // O_LARGEFILE, O_NOATIME and O_DIRECT change nothing a program can observe here. O_CLOEXEC is the
    // guest number's (descriptors.h); on the host every descriptor of the guest has it.
    const int fd = ::openat(host.directory, host.path.c_str(),
                            host_flags | O_CLOEXEC | beneath(host, O_RESOLVE_BENEATH), mode & 07777);
    if (fd < 0) return -to_linux_errno(errno);
    // A file the host watches (apkrt_watch_writes) counts this open as a writer; its description tells when it goes.
    int writer = 0;
    struct stat identity {};
    if (((flags & linux_open::access_mode) != O_RDONLY || (flags & linux_open::truncate) != 0) &&
        !(flags & linux_open::path) && ::fstat(fd, &identity) == 0 && S_ISREG(identity.st_mode)) {
        writer = watches::writer_opened(identity);
    }
    const long number =
        descriptors::add(fd, (flags & linux_open::cloexec) != 0, (flags & linux_open::nonblock) != 0, writer);
    if (number >= 0 && is_app_data_folder(host.guest)) {
        if (const auto description = descriptors::get(static_cast<int>(number))) {
            description->hides_runtime_folder = true;
        }
    }
    return number;
}

long sys_close(int fd) {
    return descriptors::close(fd);
}

long sys_faccessat(int directory, std::uint64_t path, int mode, int flags) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    int host_flags = host_at_flags(flags) | beneath(host, AT_RESOLVE_BENEATH);
    if (flags & linux_at_eaccess) host_flags |= AT_EACCESS;
    if (::faccessat(host.directory, host.path.c_str(), mode & 07, host_flags) != 0) return -to_linux_errno(errno);
    if ((mode & 07) == 0) return 0;  // F_OK: the file is there
    // Reading, writing and executing as Linux grants them to the app uid and gid (ADR-0039 decision 3), by the
    // owner and group stat reports; AT_EACCESS answers the same, since the effective ids are the real ones. Writing a
    // file, folder or link on a read-only partition is EROFS first (sb_permission).
    struct stat status;
    if (::fstatat(host.directory, host.path.c_str(), &status,
                  host_at_flags(flags) | beneath(host, AT_RESOLVE_BENEATH)) != 0) {
        return -to_linux_errno(errno);
    }
    if ((mode & 02) != 0 && read_only(host.guest) &&
        (S_ISREG(status.st_mode) || S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))) {
        return -linux_errno::erofs;
    }
    return (mode & 07 & ~granted(status.st_mode, owner_at(host))) == 0 ? 0 : -linux_errno::eacces;
}

long sys_newfstatat(int directory, std::uint64_t path, std::uint64_t buffer, int flags) {
    // Since Linux 6.11 the path may be NULL along with AT_EMPTY_PATH.
    const bool empty = path == 0 || *as_pointer<const char>(path) == '\0';
    if (empty && (flags & linux_at_empty_path)) return sys_fstat(directory, buffer);
    const auto host = resolve(directory, path, (flags & linux_at_symlink_nofollow) == 0);
    if (host.error != 0) return host.error;
    if (host.link) {  // one of the guest's links, root's in an app's namespace on Android
        LinuxStat link{};
        link.mode = S_IFLNK | 0777;
        link.nlink = 1;
        link.size = static_cast<std::int64_t>(host.link->size());
        link.blksize = 4096;
        *as_pointer<LinuxStat>(buffer) = link;
        return 0;
    }
    struct stat status;
    if (::fstatat(host.directory, host.path.c_str(), &status,
                  host_at_flags(flags) | beneath(host, AT_RESOLVE_BENEATH)) != 0) {
        return -to_linux_errno(errno);
    }
    *as_pointer<LinuxStat>(buffer) = to_linux_stat(status, owner_at(host));
    return 0;
}

long sys_mkdirat(int directory, std::uint64_t path, int mode) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    const LastFolder folder(host);
    if (!entry_at(host, false)) {  // EEXIST first, the host's; then EROFS (filename_create)
        if (read_only(folder_of(host.guest)) && folder.error() == 0) return -linux_errno::erofs;
        if (const long refused = may_change_entries(host)) return refused;
    }
    if (folder.error() != 0) return folder.error();
    return result_or_error(::mkdirat(folder.descriptor(), folder.name().c_str(), static_cast<mode_t>(mode & 07777)));
}

long sys_unlinkat(int directory, std::uint64_t path, int flags) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    const bool remove_directory = (flags & linux_at_removedir) != 0;
    // On a read-only partition EROFS, once the folder is there: Linux checks the mount before the name (do_unlinkat).
    if (read_only(folder_of(host.guest)) && LastFolder(host).error() == 0) return -linux_errno::erofs;
    if (entry_at(host, false)) {  // ENOENT first, the host's
        if (is_mounted_itself(host.guest)) return -linux_errno::eacces;
        if (const long refused = may_change_entries(host)) return refused;
    }
    if (::unlinkat(host.directory, host.path.c_str(),
                   (remove_directory ? AT_REMOVEDIR : 0) | beneath(host, AT_RESOLVE_BENEATH)) == 0) {
        return 0;
    }
    const int error = errno;
    // unlink() of a directory: EISDIR on Linux, EPERM (as POSIX allows) on Darwin.
    struct stat status;
    if (!remove_directory && error == EPERM &&
        ::fstatat(host.directory, host.path.c_str(), &status,
                  AT_SYMLINK_NOFOLLOW | beneath(host, AT_RESOLVE_BENEATH)) == 0 &&
        S_ISDIR(status.st_mode)) {
        return -linux_errno::eisdir;
    }
    return -to_linux_errno(error);
}

long sys_renameat2(int old_directory, std::uint64_t old_path, int new_directory, std::uint64_t new_path,
                   unsigned flags) {
    constexpr unsigned rename_noreplace = 1, rename_exchange = 2;
    if (flags & ~(rename_noreplace | rename_exchange) || flags == (rename_noreplace | rename_exchange)) {
        return -linux_errno::einval;  // RENAME_WHITEOUT is for overlay file systems
    }
    const auto from = resolve(old_directory, old_path);
    if (from.error != 0) return from.error;
    const auto to = resolve(new_directory, new_path);
    if (to.error != 0) return to.error;
    if (folders_in_different_mounts(from, to)) return -linux_errno::exdev;
    // On a read-only partition EROFS, once both folders are there, before the names (do_renameat2).
    if (read_only(folder_of(from.guest)) && LastFolder(from).error() == 0 && LastFolder(to).error() == 0) {
        return -linux_errno::erofs;
    }
    if (entry_at(from, false)) {  // ENOENT first, the host's
        if (is_mounted_itself(from.guest)) return -linux_errno::eacces;  // a name onto one: may_change_entries
        if (const long refused = may_change_entries(from)) return refused;
        if (const long refused = may_change_entries(to)) return refused;
    }
    const unsigned host_flags = ((flags & rename_noreplace) ? RENAME_EXCL : (flags & rename_exchange) ? RENAME_SWAP : 0) |
                                RENAME_RESOLVE_BENEATH;
    return result_or_error(
        ::renameatx_np(from.directory, from.path.c_str(), to.directory, to.path.c_str(), host_flags));
}

/// Android 15 refuses an app's hard links with EACCES, its own files' too (data, cache and external storage on the
/// emulator), after Linux's lookups (do_linkat): the old path must be there, the new one not but its folder, and both
/// in one mount (EXDEV). So no link reaches the host.
long sys_linkat(int old_directory, std::uint64_t old_path, int new_directory, std::uint64_t new_path, int flags) {
    constexpr int linux_at_symlink_follow = 0x400;
    const auto from = resolve(old_directory, old_path);
    if (from.error != 0) return from.error;
    const auto to = resolve(new_directory, new_path);
    if (to.error != 0) return to.error;
    if (!entry_at(from, (flags & linux_at_symlink_follow) != 0)) return -to_linux_errno(errno);
    if (entry_at(to, false)) return -linux_errno::eexist;
    if (const LastFolder folder(to); folder.error() != 0) return folder.error();
    if (read_only(folder_of(to.guest))) return -linux_errno::erofs;  // filename_create's, before EXDEV
    if (in_different_mounts(from, to)) return -linux_errno::exdev;
    return -linux_errno::eacces;
}

long sys_symlinkat(std::uint64_t target, int directory, std::uint64_t path) {
    if (target == 0) return -linux_errno::efault;
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    const LastFolder folder(host);
    if (!entry_at(host, false)) {  // EEXIST first, the host's; then EROFS (filename_create)
        if (read_only(folder_of(host.guest)) && folder.error() == 0) return -linux_errno::erofs;
        if (const long refused = may_change_entries(host)) return refused;
    }
    // The link keeps the guest's text; readlink gives it back unchanged. Where it leads, the host follows only below
    // the mount it lies in (ADR-0052).
    if (folder.error() != 0) return folder.error();
    return result_or_error(::symlinkat(as_pointer<const char>(target), folder.descriptor(), folder.name().c_str()));
}

long sys_readlinkat(int directory, std::uint64_t path, std::uint64_t buffer, std::int64_t size) {
    if (size <= 0) return -linux_errno::einval;
    if (const auto absolute = guest_absolute(directory, path)) {
        if (const auto link = proc::read_link(*absolute, as_pointer<char>(buffer), static_cast<std::size_t>(size))) {
            return *link;
        }
    }
    const auto host = resolve(directory, path, false);
    if (host.error != 0) return host.error;
    if (host.link) {  // one of the guest's links: its text, cut to the buffer as readlink cuts it
        const auto length = std::min(host.link->size(), static_cast<std::size_t>(size));
        std::memcpy(as_pointer<char>(buffer), host.link->data(), length);
        return static_cast<long>(length);
    }
    const LastFolder folder(host);
    if (folder.error() != 0) return folder.error();
    return result_or_error(::readlinkat(folder.descriptor(), folder.name().c_str(), as_pointer<char>(buffer),
                                        static_cast<std::size_t>(size)));
}

long sys_fchmodat(int directory, std::uint64_t path, int mode) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    if (const auto object = touched(host, true)) {  // ENOENT first, the host's; then EROFS (chmod_common)
        if (read_only(host.guest)) return -linux_errno::erofs;
        if (const long refused = may_set_attributes(object->first)) return refused;
    }
    return result_or_error(::fchmodat(host.directory, host.path.c_str(), static_cast<mode_t>(mode & 07777),
                                      beneath(host, AT_RESOLVE_BENEATH)));
}

namespace {

/// chown's answer for the app, which has no capabilities (chown_ok and chgrp_ok in fs/attr.c): on a file of its own
/// it may set the owner the file has and the group it has or the app's, which changes nothing on the host, where the
/// app's process owns every file anyway; another owner or group is EPERM. Android 15 lets the app set its own ids on
/// its own file (files chown on the emulator).
long chown_answer(const Owner &file, std::uint32_t owner, std::uint32_t group) {
    if (const long refused = may_set_attributes(file)) return refused;
    constexpr std::uint32_t unchanged = ~std::uint32_t{0};
    if (owner != unchanged && owner != file.uid) return -linux_errno::eperm;
    if (group != unchanged && group != file.gid && group != app_uid) return -linux_errno::eperm;
    return 0;
}

}  // namespace

long sys_fchownat(int directory, std::uint64_t path, std::uint32_t owner, std::uint32_t group, int flags) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    const bool follow = (flags & linux_at_symlink_nofollow) == 0;
    const auto object = touched(host, follow);
    if (!object) return entry_at(host, follow) ? -linux_errno::eacces : -to_linux_errno(errno);
    if (read_only(host.guest)) return -linux_errno::erofs;  // do_fchownat's mnt_want_write
    return chown_answer(object->first, owner, group);
}

long sys_truncate(std::uint64_t path, std::int64_t length) {
    const auto host = resolve(linux_at_fdcwd, path);
    if (host.error != 0) return host.error;
    // vfs_truncate: a folder is EISDIR and another file than a regular one EINVAL, before EROFS.
    if (const auto entry = entry_at(host, true); entry && read_only(host.guest)) {
        return S_ISDIR(entry->st_mode) ? -linux_errno::eisdir
               : !S_ISREG(entry->st_mode) ? -linux_errno::einval
                                         : -linux_errno::erofs;
    }
    if (const long refused = may_write(host)) return refused;
    const int descriptor =
        ::openat(host.directory, host.path.c_str(), O_WRONLY | O_CLOEXEC | beneath(host, O_RESOLVE_BENEATH));
    if (descriptor < 0) return -to_linux_errno(errno);
    const long result = result_or_error(::ftruncate(descriptor, length));
    ::close(descriptor);
    return result;
}

long sys_utimensat(int directory, std::uint64_t path, std::uint64_t times, int flags) {
    // UTIME_NOW and UTIME_OMIT have other values on Linux.
    constexpr long linux_utime_now = (1L << 30) - 1, linux_utime_omit = (1L << 30) - 2;
    timespec host_times[2];
    const timespec *pointer = nullptr;
    if (times != 0) {
        std::memcpy(host_times, as_pointer<const timespec>(times), sizeof host_times);
        for (auto &time : host_times) {
            if (time.tv_nsec == linux_utime_now) time.tv_nsec = UTIME_NOW;
            else if (time.tv_nsec == linux_utime_omit) time.tv_nsec = UTIME_OMIT;
        }
        pointer = host_times;
    }
    // Linux's checks (may_setattr, setattr_prepare): times the guest gives need the file's owner; the current time, as
    // NULL or UTIME_NOW twice, the owner or write permission; UTIME_OMIT twice changes nothing and needs neither.
    const bool omitted = pointer != nullptr && host_times[0].tv_nsec == UTIME_OMIT && host_times[1].tv_nsec == UTIME_OMIT;
    const bool now = pointer == nullptr || (host_times[0].tv_nsec == UTIME_NOW && host_times[1].tv_nsec == UTIME_NOW);
    const auto refused = [&](const Owner &owner, const struct stat &status) {
        if (omitted || owner.uid == app_uid) return 0L;
        if (now && (granted(status.st_mode, owner) & 02) != 0) return 0L;
        return may_set_attributes(owner);
    };
    if (path == 0) {  // Linux allows a NULL path: the descriptor's own file
        const auto description = descriptors::get(directory);
        if (!description) return -linux_errno::ebadf;
        struct stat status;
        if (::fstat(description->host(), &status) == 0) {
            if (!omitted && read_only(*description)) return -linux_errno::erofs;  // vfs_utimes' mnt_want_write
            if (const long answer = refused(owner_of(*description), status)) return answer;
        }
        return result_or_error(::futimens(description->host(), pointer));
    }
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    if (const auto object = touched(host, (flags & linux_at_symlink_nofollow) == 0)) {  // ENOENT first, the host's
        if (!omitted && read_only(host.guest)) return -linux_errno::erofs;
        if (const long answer = refused(object->first, object->second)) return answer;
    }
    return result_or_error(::utimensat(host.directory, host.path.c_str(), pointer,
                                       host_at_flags(flags) | beneath(host, AT_RESOLVE_BENEATH)));
}

long sys_getcwd(std::uint64_t buffer, std::uint64_t size) {
    const auto cwd = vfs::working_directory();
    if (size < cwd.size() + 1) return -linux_errno::erange;
    std::memcpy(as_pointer<char>(buffer), cwd.c_str(), cwd.size() + 1);
    return static_cast<long>(cwd.size() + 1);
}

namespace {

/// Makes `guest`, an absolute guest path, the working directory if it is a directory on the host.
long change_directory(const std::string &guest, const std::string &host) {
    struct stat status;
    if (::stat(host.c_str(), &status) != 0) return -to_linux_errno(errno);
    if (!S_ISDIR(status.st_mode)) return -linux_errno::enotdir;
    vfs::set_working_directory(guest);
    return 0;
}

}  // namespace

long sys_chdir(std::uint64_t path) {
    const auto host = resolve(linux_at_fdcwd, path);
    if (host.error != 0) return host.error;
    struct stat status;
    if (::fstatat(host.directory, host.path.c_str(), &status, beneath(host, AT_RESOLVE_BENEATH)) != 0) {
        return -to_linux_errno(errno);
    }
    if (!S_ISDIR(status.st_mode)) return -linux_errno::enotdir;
    vfs::set_working_directory(host.guest);
    return 0;
}

long sys_fchdir(int fd) {
    return with_host(fd, [](int directory) {
        char host[MAXPATHLEN];
        if (::fcntl(directory, F_GETPATH, host) != 0) return -to_linux_errno(errno);
        const auto guest = vfs::guest_path(host);
        if (!guest) return -linux_errno::enoent;  // a directory the guest cannot name
        return change_directory(*guest, host);
    });
}

long sys_lseek(int fd, std::int64_t offset, int whence) {
    constexpr int linux_seek_data = 3, linux_seek_hole = 4;  // the other way round on Darwin
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    // An eventfd's and an epoll's lseek is Linux's noop_llseek: the position, which stays 0.
    if (description->kind() != Description::Kind::file) return 0;
    if (const auto result = seek_stream(*description, offset, whence)) return *result;
    int host_whence;
    switch (whence) {
        case SEEK_SET:
        case SEEK_CUR:
        case SEEK_END: host_whence = whence; break;
        case linux_seek_data: host_whence = SEEK_DATA; break;
        case linux_seek_hole: host_whence = SEEK_HOLE; break;
        default: return -linux_errno::einval;
    }
    return result_or_error(::lseek(description->host(), offset, host_whence));
}

long sys_getdents64(int fd, std::uint64_t buffer, std::uint64_t size) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    return get_directory_entries(*description, buffer, size);
}

long sys_dup(int fd) {
    return descriptors::duplicate(fd, 0, false);
}

long sys_dup3(int fd, int target, int flags) {
    if (fd == target || (flags & ~linux_open::cloexec) != 0) return -linux_errno::einval;
    return descriptors::duplicate_to(fd, target, (flags & linux_open::cloexec) != 0);
}

long sys_pipe2(std::uint64_t fds, int flags) {
    constexpr int linux_o_direct = 0200000;  // packet mode; nothing Darwin offers
    if (flags & ~(linux_open::cloexec | linux_open::nonblock | linux_o_direct)) return -linux_errno::einval;
    int pair[2];
    if (::pipe(pair) != 0) return -to_linux_errno(errno);
    const bool close_on_exec = (flags & linux_open::cloexec) != 0;
    const bool nonblocking = (flags & linux_open::nonblock) != 0;
    const long read_end = descriptors::add(pair[0], close_on_exec, nonblocking);
    if (read_end < 0) {
        ::close(pair[1]);
        return read_end;
    }
    const long write_end = descriptors::add(pair[1], close_on_exec, nonblocking);
    if (write_end < 0) {
        descriptors::close(static_cast<int>(read_end));  // Linux takes both numbers or none
        return write_end;
    }
    const int guest[2] = {static_cast<int>(read_end), static_cast<int>(write_end)};
    std::memcpy(as_pointer<int>(fds), guest, sizeof guest);
    return 0;
}

namespace {

// --- Reads and writes (ADR-0027, decision 7) -----------------------------------------------------------
//
// On a description that can block (descriptors.h) the host descriptor is non-blocking: a read or write
// tries the host call and, on EAGAIN, unless the guest's O_NONBLOCK is set, waits in the thread's kqueue
// (syscalls.h, wait_for_descriptors) and tries again, so a signal ends the wait at any moment. A write
// goes on until every byte is written; a signal after some bytes ends it with their count, as on Linux.
// An adopted description's host descriptor blocks, as the starter keeps it: the call waits until it is
// ready, then makes the blocking host call, which SIGURG may interrupt.

/// The buffers a read or write still has to fill or empty: the guest's list, or a copy of its rest
/// once a write has gone into it.
class Buffers {
public:
    Buffers(const iovec *vectors, int count) : vectors_(vectors), count_(count) {}
    Buffers(const Buffers &) = delete;  // a copy's vectors would be the original's rest
    Buffers &operator=(const Buffers &) = delete;

    const iovec *vectors() const { return vectors_; }
    int count() const { return count_; }
    bool empty() const { return count_ == 0; }

    /// The buffers of the first `limit` bytes.
    std::vector<iovec> within(std::size_t limit) const {
        std::vector<iovec> part;
        for (int index = 0; index < count_ && limit > 0; ++index) {
            const std::size_t size = std::min(vectors_[index].iov_len, limit);
            part.push_back({vectors_[index].iov_base, size});
            limit -= size;
        }
        return part;
    }

    /// Moves past `bytes` that went through.
    void advance(std::size_t bytes) {
        if (!copied_) {  // the guest's list stays as it is
            rest_.assign(vectors_, vectors_ + count_);
            copied_ = true;
        }
        auto first = rest_.begin();
        for (; first != rest_.end() && bytes >= first->iov_len; ++first) bytes -= first->iov_len;
        rest_.erase(rest_.begin(), first);
        if (!rest_.empty()) {
            rest_.front().iov_base = static_cast<char *>(rest_.front().iov_base) + bytes;
            rest_.front().iov_len -= bytes;
        }
        vectors_ = rest_.data();
        count_ = static_cast<int>(rest_.size());
    }

private:
    const iovec *vectors_;
    int count_;
    std::vector<iovec> rest_;
    bool copied_ = false;
};

/// A read (`writing` false) or a write of `buffers` on `description`, which can block, until `deadline` at the latest:
/// a socket's SO_RCVTIMEO or SO_SNDTIMEO, after which it answers EAGAIN, as Linux's sock_read_iter and sock_write_iter.
long transfer(descriptors::Description &description, bool writing, Buffers buffers, std::int64_t deadline) {
    constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();
    const int host = description.host();
    // The host call on the buffers still to go, of `limit` bytes of them at most.
    const auto host_call = [&](std::size_t limit) {
        const iovec *vectors = buffers.vectors();
        int count = buffers.count();
        // One buffer, as read and write give it: the plain calls, which cost Darwin 50 ns less each.
        if (count == 1) {
            const std::size_t size = std::min(vectors->iov_len, limit);
            return result_or_error(writing ? ::write(host, vectors->iov_base, size) : ::read(host, vectors->iov_base, size));
        }
        std::vector<iovec> part;
        if (limit != unlimited) {
            part = buffers.within(limit);
            vectors = part.data();
            count = static_cast<int>(part.size());
        }
        return result_or_error(writing ? ::writev(host, vectors, count) : ::readv(host, vectors, count));
    };
    // A write that got bytes out ends with their count, whatever came then; as Linux's pipe_write, a
    // reader that went away raises SIGPIPE even so. A socket's sendmsg raises it only when nothing went out.
    const bool socket = sockets::as_socket(description) != nullptr;
    const auto count_of_moved = [socket](long moved, long last) {
        if (last == -linux_errno::epipe && !socket) raise_sigpipe();
        return moved;
    };
    auto &state = signals::current();
    Watched watched{host, static_cast<std::int16_t>(writing ? EVFILT_WRITE : EVFILT_READ)};
    long moved = 0;  // what a write has written so far

    if (!description.adopted()) {
        long last = 0;
        const auto finished = [&] {
            while (true) {
                last = host_call(unlimited);
                // A non-blocking call can still wait for the lock of a Darwin pipe, which SIGURG ends with
                // EINTR: the call is simply made again, and a deliverable signal ends the wait after it.
                if (last == -linux_errno::eintr) continue;
                if (last == -linux_errno::eagain) return description.nonblocking();
                if (last <= 0 || !writing) return true;
                moved += last;
                buffers.advance(static_cast<std::size_t>(last));
                if (buffers.empty() || description.nonblocking()) return true;
            }
        };
        const auto end = wait_for_descriptors(state, &watched, 1, finished, deadline);
        if (moved > 0) return count_of_moved(moved, last);
        if (end == WaitEnd::timeout) return -linux_errno::eagain;
        return end == WaitEnd::done ? last : -linux_errno::eintr;
    }

    // Adopted: wait until the host descriptor is ready, then the blocking call, for a write with no more
    // bytes than the kqueue saw room for, so that the call does not wait in the host. Only the starter's
    // side can take the data or the room first, and a SIGURG that ends the call then leads to the same
    // again: what is ready first, else a deliverable signal ends it.
    const auto ready = [&] {
        probe_descriptors(state, &watched, 1);
        return watched.ready;
    };
    while (true) {
        const auto end =
            wait_for_descriptors(state, &watched, 1, ready, description.nonblocking() ? monotonic_now() : no_deadline);
        if (end != WaitEnd::done) {
            if (moved > 0) return moved;
            return end == WaitEnd::signal ? -linux_errno::eintr : -linux_errno::eagain;
        }
        const long result =
            host_call(writing && watched.available > 0 ? static_cast<std::size_t>(watched.available) : unlimited);
        if (result == -linux_errno::eintr || result == -linux_errno::eagain) continue;
        if (result <= 0 || !writing) return moved > 0 ? count_of_moved(moved, result) : result;
        moved += result;
        buffers.advance(static_cast<std::size_t>(result));
        if (buffers.empty() || description.nonblocking()) return moved;
    }
}

/// read, write, readv and writev on the guest's `fd`: `plain` makes the host call as it is on a
/// description that cannot block.
template <typename Plain>
long read_or_write(int fd, bool writing, const iovec *vectors, int count, Plain plain) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    // An epoll has no read or write, and a channel only its private syscalls (channels.h).
    if (description->kind() == Description::Kind::epoll || description->kind() == Description::Kind::channel) {
        return -linux_errno::einval;
    }
    if (count == 0) return 0;  // Linux moves nothing; Darwin answers EINVAL for no vectors
    if (writing) {
        if (auto *file = memfd::as_file(*description)) return memfd::write(*file, vectors, count, -1);
    }
    if (description->kind() == Description::Kind::eventfd) return transfer_count(*description, writing, vectors, count);
    if (!description->can_block()) return result_or_error(plain(description->host()));
    if (sockets::takes_messages(*description)) return sockets::transfer_message(*description, writing, vectors, count);
    return transfer(*description, writing, Buffers(vectors, count), sockets::deadline_of(*description, writing));
}

/// pwrite and pwritev of a memfd, through its seals (memfd.h); nullopt for every other descriptor.
std::optional<long> memfd_pwrite(int fd, const iovec *vectors, int count, std::int64_t offset) {
    const auto description = descriptors::get(fd);
    memfd::File *const file = description ? memfd::as_file(*description) : nullptr;
    if (file == nullptr) return std::nullopt;
    if (offset < 0) return -linux_errno::einval;
    return memfd::write(*file, vectors, count, offset);
}

}  // namespace

long sys_read(int fd, std::uint64_t buffer, std::uint64_t size) {
    const iovec one{as_pointer<void>(buffer), size};
    return read_or_write(fd, false, &one, 1, [&](int host) { return ::read(host, one.iov_base, size); });
}

long sys_write(int fd, std::uint64_t buffer, std::uint64_t size) {
    const iovec one{as_pointer<void>(buffer), size};
    return read_or_write(fd, true, &one, 1, [&](int host) { return ::write(host, one.iov_base, size); });
}

// struct iovec has the same layout on Linux and Darwin: a pointer and a size_t.

long sys_readv(int fd, std::uint64_t vectors, int count) {
    const auto *list = as_pointer<const iovec>(vectors);
    return read_or_write(fd, false, list, count, [&](int host) { return ::readv(host, list, count); });
}

long sys_writev(int fd, std::uint64_t vectors, int count) {
    const auto *list = as_pointer<const iovec>(vectors);
    return read_or_write(fd, true, list, count, [&](int host) { return ::writev(host, list, count); });
}

long sys_pread64(int fd, std::uint64_t buffer, std::uint64_t size, std::int64_t offset) {
    return with_host(fd, [&](int host) {
        return result_or_error(::pread(host, as_pointer<void>(buffer), size, offset));
    });
}

long sys_pwrite64(int fd, std::uint64_t buffer, std::uint64_t size, std::int64_t offset) {
    const iovec one{as_pointer<void>(buffer), size};
    if (const auto sealed = memfd_pwrite(fd, &one, 1, offset)) return *sealed;
    return with_host(fd, [&](int host) {
        return result_or_error(::pwrite(host, as_pointer<const void>(buffer), size, offset));
    });
}

long sys_preadv(int fd, std::uint64_t vectors, int count, std::int64_t offset) {
    return with_host(fd, [&](int host) {
        return result_or_error(::preadv(host, as_pointer<const iovec>(vectors), count, offset));
    });
}

long sys_pwritev(int fd, std::uint64_t vectors, int count, std::int64_t offset) {
    if (const auto sealed = memfd_pwrite(fd, as_pointer<const iovec>(vectors), count, offset)) return *sealed;
    return with_host(fd, [&](int host) {
        return result_or_error(::pwritev(host, as_pointer<const iovec>(vectors), count, offset));
    });
}

/// sendfile (fs/read_write.c's do_sendfile, Android 15's 6.6 kernel): `count` bytes of the regular file `in`, from
/// the offset at `offset` or from its own position, written to `out` as write(2) writes them, so a socket, a pipe or
/// a memfd waits, stops short and fails as it does. The errors come in the kernel's order: the offset's EFAULT, the
/// input's EBADF, ESPIPE and EINVAL for a negative offset, the output's EBADF, then EINVAL for an output open for
/// appending and for an input that is no regular file (no splice_read: a pipe, a socket). The offset given moves and
/// the input's position does not, else the position moves. A copy through the runtime: Darwin's sendfile takes only
/// a socket.
long sys_sendfile(int out, int in, std::uint64_t offset, std::uint64_t count) {
    std::int64_t position = 0;
    if (offset != 0 && !memory::copy_checked(reinterpret_cast<std::uint64_t>(&position), offset, sizeof position)) {
        return -linux_errno::efault;
    }
    const auto input = descriptors::get(in);
    if (!input || (::fcntl(input->host(), F_GETFL) & O_ACCMODE) == O_WRONLY) return -linux_errno::ebadf;
    struct stat status {};
    const bool regular = input->kind() == Description::Kind::file && ::fstat(input->host(), &status) == 0 &&
                         S_ISREG(status.st_mode);
    if (offset == 0) {
        const off_t current = regular ? ::lseek(input->host(), 0, SEEK_CUR) : 0;
        if (current < 0) return -to_linux_errno(errno);
        position = current;
    } else if (!regular && ::lseek(input->host(), 0, SEEK_CUR) < 0) {
        return -linux_errno::espipe;
    }
    if (position < 0) return -linux_errno::einval;
    count = std::min<std::uint64_t>(count, 0x7fff'c000);  // MAX_RW_COUNT with the 16 KiB pages
    const auto output = descriptors::get(out);
    if (!output) return -linux_errno::ebadf;
    const int out_flags = output->kind() == Description::Kind::file ? ::fcntl(output->host(), F_GETFL) : O_RDWR;
    if ((out_flags & O_ACCMODE) == O_RDONLY) return -linux_errno::ebadf;
    if ((out_flags & O_APPEND) != 0 || !regular) return -linux_errno::einval;

    std::vector<char> buffer(std::min<std::uint64_t>(count, 128 * 1024));
    std::uint64_t moved = 0;
    long failure = 0;
    while (moved < count) {
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(count - moved, buffer.size()));
        const ssize_t got = ::pread(input->host(), buffer.data(), wanted, position + static_cast<std::int64_t>(moved));
        if (got <= 0) {
            if (got < 0) failure = -to_linux_errno(errno);
            break;
        }
        const long written =
            sys_write(out, reinterpret_cast<std::uint64_t>(buffer.data()), static_cast<std::uint64_t>(got));
        if (written < 0) {
            failure = written;
            break;
        }
        moved += static_cast<std::uint64_t>(written);
        if (written < got) break;
    }
    const std::int64_t end = position + static_cast<std::int64_t>(moved);
    if (moved > 0 && offset == 0) ::lseek(input->host(), end, SEEK_SET);
    if (offset != 0) {
        const std::int64_t answer = moved > 0 ? end : position;
        if (!memory::copy_checked(offset, reinterpret_cast<std::uint64_t>(&answer), sizeof answer)) {
            return -linux_errno::efault;
        }
    }
    return moved > 0 ? static_cast<long>(moved) : failure;
}

long sys_fchmod(int fd, int mode) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    // SELinux refuses setattr on a memfd, labelled appdomain_tmpfs [verified: the emulator, 2026-10-08].
    if (memfd::as_file(*description) != nullptr) return -linux_errno::eacces;
    if (read_only(*description)) return -linux_errno::erofs;  // chmod_common's mnt_want_write
    if (const long refused = may_set_attributes(owner_of(*description))) return refused;
    return result_or_error(::fchmod(description->host(), static_cast<mode_t>(mode & 07777)));
}

long sys_fchown(int fd, std::uint32_t owner, std::uint32_t group) {
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    if (memfd::as_file(*description) != nullptr) return -linux_errno::eacces;  // as sys_fchmod's
    if (read_only(*description)) return -linux_errno::erofs;  // vfs_fchown's mnt_want_write_file
    return chown_answer(owner_of(*description), owner, group);
}

long sys_ftruncate(int fd, std::int64_t length) {
    if (const auto description = descriptors::get(fd)) {
        if (auto *file = memfd::as_file(*description)) return memfd::truncate(*file, length);
    }
    return with_file(fd, [&](int host) { return result_or_error(::ftruncate(host, length)); });
}

namespace {

// fallocate's modes (include/uapi/linux/falloc.h).
namespace linux_falloc {
constexpr int keep_size = 0x01;
constexpr int punch_hole = 0x02;
constexpr int collapse_range = 0x08;
constexpr int zero_range = 0x10;
constexpr int insert_range = 0x20;
constexpr int unshare_range = 0x40;
constexpr int known = keep_size | punch_hole | collapse_range | zero_range | insert_range | unshare_range;
}  // namespace linux_falloc

/// Zeros [offset, offset + length) of the host file `host` inside its size, as a hole reads: Darwin's F_PUNCHHOLE for
/// the file system's whole blocks, written zeros for the parts of blocks at either end and where it has no holes.
long punch_hole(int host, std::int64_t offset, std::int64_t length) {
    struct stat status{};
    if (::fstat(host, &status) != 0) return -to_linux_errno(errno);
    const std::int64_t end = std::min<std::int64_t>(offset + length, status.st_size);
    if (offset >= end) return 0;
    const auto zero = [host](std::int64_t from, std::int64_t to) -> long {
        static const char zeros[4096] = {};
        while (from < to) {
            const auto part = static_cast<std::size_t>(std::min<std::int64_t>(to - from, sizeof zeros));
            const ssize_t written = ::pwrite(host, zeros, part, from);
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) return -to_linux_errno(errno);
            from += written;
        }
        return 0;
    };
    const std::int64_t block = status.st_blksize > 0 ? status.st_blksize : 4096;
    const std::int64_t first = (offset + block - 1) / block * block, last = end / block * block;
    if (first >= last) return zero(offset, end);
    if (const long result = zero(offset, first)) return result;
    fpunchhole_t hole{};
    hole.fp_offset = first;
    hole.fp_length = last - first;
    if (::fcntl(host, F_PUNCHHOLE, &hole) != 0) {
        if (const long result = zero(first, last)) return result;
    }
    return zero(last, end);
}

/// An allocation of [offset, offset + length): Darwin's F_PREALLOCATE for the blocks past the file's, then the size
/// unless FALLOC_FL_KEEP_SIZE. A file system that cannot preallocate leaves the blocks to the first writes.
long allocate(int host, int mode, std::int64_t offset, std::int64_t length) {
    struct stat status{};
    if (::fstat(host, &status) != 0) return -to_linux_errno(errno);
    const std::int64_t end = offset + length;
    if (end <= status.st_size) return 0;
    fstore_t store{};
    store.fst_flags = F_ALLOCATEALL;
    store.fst_posmode = F_PEOFPOSMODE;
    store.fst_length = end - status.st_size;
    if (::fcntl(host, F_PREALLOCATE, &store) != 0 && errno == ENOSPC) return -linux_errno::enospc;
    if (!(mode & linux_falloc::keep_size) && ::ftruncate(host, end) != 0) return -to_linux_errno(errno);
    return 0;
}

}  // namespace

long sys_fallocate(int fd, int mode, std::int64_t offset, std::int64_t length) {
    // vfs_fallocate's order, once the number names a file.
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    if (offset < 0 || length <= 0) return -linux_errno::einval;
    if (mode & ~linux_falloc::known) return -linux_errno::eopnotsupp;
    const int hole_or_zeros = linux_falloc::punch_hole | linux_falloc::zero_range;
    if ((mode & hole_or_zeros) == hole_or_zeros) return -linux_errno::eopnotsupp;
    if ((mode & linux_falloc::punch_hole) && !(mode & linux_falloc::keep_size)) return -linux_errno::eopnotsupp;
    if ((mode & linux_falloc::collapse_range) && (mode & ~linux_falloc::collapse_range)) return -linux_errno::einval;
    if ((mode & linux_falloc::insert_range) && (mode & ~linux_falloc::insert_range)) return -linux_errno::einval;
    if ((mode & linux_falloc::unshare_range) && (mode & ~(linux_falloc::unshare_range | linux_falloc::keep_size))) {
        return -linux_errno::einval;
    }
    // An eventfd, an epoll or a channel is open for writing and no regular file.
    if (description->kind() != Description::Kind::file) return -linux_errno::enodev;
    const int host = description->host();
    const int status_flags = ::fcntl(host, F_GETFL);
    if (status_flags >= 0 && (status_flags & O_ACCMODE) == O_RDONLY) return -linux_errno::ebadf;
    struct stat status{};
    if (::fstat(host, &status) != 0) return -to_linux_errno(errno);
    if (S_ISFIFO(status.st_mode)) return -linux_errno::espipe;
    if (S_ISDIR(status.st_mode)) return -linux_errno::eisdir;
    if (!S_ISREG(status.st_mode)) return -linux_errno::enodev;
    if (offset > std::numeric_limits<std::int64_t>::max() - length) return -linux_errno::efbig;
    if (mode & ~(linux_falloc::keep_size | linux_falloc::punch_hole)) return -linux_errno::eopnotsupp;
    const auto run = [&]() -> long {
        return (mode & linux_falloc::punch_hole) ? punch_hole(host, offset, length) : allocate(host, mode, offset, length);
    };
    if (auto *file = memfd::as_file(*description)) {
        return memfd::fallocate(*file, (mode & linux_falloc::punch_hole) != 0, offset + length, run);
    }
    return run();
}

long sys_fsync(int fd) {
    return with_file(fd, [](int host) { return result_or_error(::fsync(host)); });
}

namespace {

/// struct termios as the Linux kernel fills it for TCGETS (include/uapi/asm-generic/termbits.h).
struct LinuxTermios {
    std::uint32_t iflag, oflag, cflag, lflag;
    std::uint8_t line;
    std::uint8_t cc[19];
};

/// The common termios bits, Darwin to Linux: enough for isatty() and for reading the line mode.
LinuxTermios to_linux_termios(const termios &host) {
    LinuxTermios result{};
    const auto map = [](tcflag_t flags, std::initializer_list<std::pair<tcflag_t, std::uint32_t>> bits) {
        std::uint32_t mapped = 0;
        for (const auto &[darwin, linux_bit] : bits) {
            if (flags & darwin) mapped |= linux_bit;
        }
        return mapped;
    };
    result.iflag = map(host.c_iflag, {{IGNBRK, 01}, {BRKINT, 02}, {IGNPAR, 04}, {PARMRK, 010}, {INPCK, 020},
                                      {ISTRIP, 040}, {INLCR, 0100}, {IGNCR, 0200}, {ICRNL, 0400}, {IXON, 02000},
                                      {IXANY, 04000}, {IXOFF, 010000}, {IMAXBEL, 020000}, {IUTF8, 040000}});
    result.oflag = map(host.c_oflag, {{OPOST, 01}, {ONLCR, 04}, {OCRNL, 010}, {ONOCR, 020}, {ONLRET, 040}});
    result.cflag = map(host.c_cflag, {{CSTOPB, 0100}, {CREAD, 0200}, {PARENB, 0400}, {PARODD, 01000},
                                      {HUPCL, 02000}, {CLOCAL, 04000}});
    switch (host.c_cflag & CSIZE) {
        case CS5: break;
        case CS6: result.cflag |= 020; break;
        case CS7: result.cflag |= 040; break;
        default: result.cflag |= 060; break;  // CS8
    }
    result.cflag |= 017;  // B38400: Linux reports a speed here, Darwin keeps it apart
    result.lflag = map(host.c_lflag, {{ISIG, 01}, {ICANON, 02}, {ECHO, 010}, {ECHOE, 020}, {ECHOK, 040},
                                      {ECHONL, 0100}, {NOFLSH, 0200}, {TOSTOP, 0400}, {ECHOCTL, 01000},
                                      {ECHOPRT, 02000}, {ECHOKE, 04000}, {IEXTEN, 0100000}});
    // c_cc indexes (Linux): VINTR 0, VQUIT 1, VERASE 2, VKILL 3, VEOF 4, VTIME 5, VMIN 6, VSTART 8,
    // VSTOP 9, VSUSP 10, VEOL 11, VREPRINT 12, VDISCARD 13, VWERASE 14, VLNEXT 15, VEOL2 16.
    const std::pair<int, int> characters[] = {{VINTR, 0},   {VQUIT, 1},     {VERASE, 2},    {VKILL, 3},
                                              {VEOF, 4},    {VTIME, 5},     {VMIN, 6},      {VSTART, 8},
                                              {VSTOP, 9},   {VSUSP, 10},    {VEOL, 11},     {VREPRINT, 12},
                                              {VDISCARD, 13}, {VWERASE, 14}, {VLNEXT, 15},  {VEOL2, 16}};
    for (const auto &[darwin, linux_index] : characters) result.cc[linux_index] = host.c_cc[darwin];
    return result;
}

}  // namespace

long sys_ioctl(int fd, std::uint32_t request, std::uint64_t argument) {
    // Requests from include/uapi/asm-generic/ioctls.h.
    constexpr std::uint32_t tcgets = 0x5401, tiocgwinsz = 0x5413, fionread = 0x541b, fionbio = 0x5421,
                            fionclex = 0x5450, fioclex = 0x5451;
    const auto description = descriptors::get(fd);
    if (!description) return -linux_errno::ebadf;
    const int host = description->host();
    if (auto *socket = sockets::as_socket(*description)) {
        bool handled = false;
        const long answer = sockets::socket_ioctl(*socket, request, argument, handled);
        if (handled) return answer;
    }
    switch (request) {
        case tcgets: {
            termios attributes;
            if (::tcgetattr(host, &attributes) != 0) {
                return errno == ENOTTY ? -linux_errno::enotty : -to_linux_errno(errno);
            }
            *as_pointer<LinuxTermios>(argument) = to_linux_termios(attributes);
            return 0;
        }
        case tiocgwinsz:  // struct winsize: four unsigned shorts on both systems
            return result_or_error(::ioctl(host, TIOCGWINSZ, as_pointer<winsize>(argument)));
        case fionread:
            // Linux answers it for regular files and for what implements it, not an eventfd or an epoll.
            if (description->kind() != Description::Kind::file) return -linux_errno::enotty;
            return result_or_error(::ioctl(host, FIONREAD, as_pointer<int>(argument)));
        case fionbio:  // the guest's O_NONBLOCK (descriptors.h)
            description->set_nonblocking(*as_pointer<const int>(argument) != 0);
            return 0;
        case fioclex: return descriptors::set_close_on_exec(fd, true);
        case fionclex: return descriptors::set_close_on_exec(fd, false);
        default: {
            // F2FS's (type 0xf5), which SQLite asks for atomic writes: no guest file lies on F2FS (Linux's ENOTTY).
            if (((request >> 8) & 0xff) == 0xf5) return -linux_errno::enotty;
            char message[80];
            std::snprintf(message, sizeof message, "apkrt: ioctl request %#x is not supported yet\n", request);
            report(message);
            return -linux_errno::enotty;
        }
    }
}

namespace {

/// struct statfs as Linux fills it on arm64 (include/uapi/asm-generic/statfs.h), all 64-bit words.
struct LinuxStatfs {
    std::int64_t type, block_size, blocks, free, available, files, free_files;
    std::int32_t fsid[2];
    std::int64_t name_length, fragment_size, flags, spare[4];
};
static_assert(sizeof(LinuxStatfs) == 120);

/// The host's statfs as Linux's, read-only (ST_RDONLY) also for the system's partitions, as Android 15 mounts them.
LinuxStatfs to_linux_statfs(const struct statfs &host, bool partition_read_only) {
    constexpr std::int64_t ext4_super_magic = 0xef53;  // what /data reports on many devices
    constexpr std::int64_t st_rdonly = 1, st_nosuid = 2, st_valid = 0x20;
    LinuxStatfs result{};
    result.type = ext4_super_magic;
    result.block_size = host.f_bsize;
    result.blocks = static_cast<std::int64_t>(host.f_blocks);
    result.free = static_cast<std::int64_t>(host.f_bfree);
    result.available = static_cast<std::int64_t>(host.f_bavail);
    result.files = static_cast<std::int64_t>(host.f_files);
    result.free_files = static_cast<std::int64_t>(host.f_ffree);
    result.fsid[0] = host.f_fsid.val[0];
    result.fsid[1] = host.f_fsid.val[1];
    result.name_length = 255;
    result.fragment_size = host.f_bsize;
    result.flags = st_valid | ((host.f_flags & MNT_RDONLY) != 0 || partition_read_only ? st_rdonly : 0) |
                   ((host.f_flags & MNT_NOSUID) ? st_nosuid : 0);
    return result;
}

}  // namespace

long sys_statfs(std::uint64_t path, std::uint64_t buffer) {
    const auto host = resolve(linux_at_fdcwd, path);
    if (host.error != 0) return host.error;
    const int descriptor =
        ::openat(host.directory, host.path.c_str(), O_EVTONLY | O_CLOEXEC | beneath(host, O_RESOLVE_BENEATH));
    if (descriptor < 0) return -to_linux_errno(errno);
    struct statfs status;
    const int answer = ::fstatfs(descriptor, &status);
    const int error = errno;
    ::close(descriptor);
    if (answer != 0) return -to_linux_errno(error);
    *as_pointer<LinuxStatfs>(buffer) = to_linux_statfs(status, read_only(host.guest));
    return 0;
}

long sys_fstatfs(int fd, std::uint64_t buffer) {
    const auto description = descriptors::get(fd);
    const bool partition_read_only = description && read_only(*description);
    return with_host(fd, [&](int host) {
        struct statfs status;
        if (::fstatfs(host, &status) != 0) return -to_linux_errno(errno);
        *as_pointer<LinuxStatfs>(buffer) = to_linux_statfs(status, partition_read_only);
        return 0L;
    });
}

}  // namespace apkrt::sys

// --- AF_UNIX's names in the file system (sockets.h) -------------------------------------------------------

namespace apkrt::sockets {

Place::~Place() {
    if (folder >= 0) ::close(folder);
}

Place &Place::operator=(Place &&other) noexcept {
    if (this != &other) {
        if (folder >= 0) ::close(folder);
        error = other.error;
        folder = other.folder;
        name = std::move(other.name);
        other.folder = -1;
    }
    return *this;
}

namespace {

/// The guest's links a path to a socket may pass at its end, as Linux's 40 (MAXSYMLINKS).
constexpr int most_links = 40;

Place place_at(std::string_view path, bool creating, int links) {
    using namespace sys;
    Place place;
    if (path.empty()) {
        place.error = -linux_errno::enoent;
        return place;
    }
    const std::string text(path);
    const auto host = resolve(linux_at_fdcwd, reinterpret_cast<std::uint64_t>(text.c_str()));
    if (host.error != 0) {
        place.error = host.error;
        return place;
    }
    const LastFolder folder(host);
    const auto keep = [&] {
        place.name = folder.name();
        if (folder.descriptor() == AT_FDCWD) {  // a mounted file: its host path from the table, as it is
            place.folder = AT_FDCWD;
            return;
        }
        place.folder = ::fcntl(folder.descriptor(), F_DUPFD_CLOEXEC, 0);
        if (place.folder < 0) place.error = -to_linux_errno(errno);
    };
    if (creating) {
        // unix_bind_bsd: filename_create's EEXIST, which bind answers EADDRINUSE, then EROFS, then may_create's EACCES.
        if (entry_at(host, false)) {
            place.error = -linux_errno::eaddrinuse;
        } else if (folder.error() != 0) {
            place.error = folder.error();
        } else if (read_only(folder_of(host.guest))) {
            place.error = -linux_errno::erofs;
        } else if (const long refused = may_change_entries(host)) {
            place.error = refused;
        } else {
            keep();
        }
        return place;
    }
    // unix_find_bsd: the path, write permission on what it names, and ECONNREFUSED when that is no socket.
    if (folder.error() != 0) {
        place.error = folder.error();
        return place;
    }
    struct stat status;
    const int nofollow = AT_SYMLINK_NOFOLLOW | (folder.descriptor() == AT_FDCWD ? 0 : AT_RESOLVE_BENEATH);
    if (::fstatat(folder.descriptor(), folder.name().c_str(), &status, nofollow) != 0) {
        place.error = -to_linux_errno(errno);
        return place;
    }
    if (S_ISLNK(status.st_mode)) {
        // The link's text is a guest path, relative to the link's folder (ADR-0052).
        if (links >= most_links) {
            place.error = -linux_errno::eloop;
            return place;
        }
        char target[MAXPATHLEN];
        const auto length = ::readlinkat(folder.descriptor(), folder.name().c_str(), target, sizeof target - 1);
        if (length < 0) {
            place.error = -to_linux_errno(errno);
            return place;
        }
        target[length] = '\0';
        const std::string_view guest = host.guest;
        const auto slash = guest.rfind('/');
        return place_at(vfs::absolute(target, slash == 0 ? "/" : std::string(guest.substr(0, slash))), false, links + 1);
    }
    if ((granted(status.st_mode, owner_at(host)) & 02) == 0) {
        place.error = -linux_errno::eacces;
    } else if (!S_ISSOCK(status.st_mode)) {
        place.error = -linux_errno::econnrefused;
    } else {
        keep();
        if (place_checked != nullptr && place.error == 0) place_checked(place.folder, place.name);
    }
    return place;
}

}  // namespace

Place place_of(std::string_view path, bool creating) {
    return place_at(path, creating, 0);
}

}  // namespace apkrt::sockets
