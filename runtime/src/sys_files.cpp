// File syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): Linux paths, flags and
// structures on Darwin's file descriptors. Every path goes through the path table (vfs.h); guest
// file descriptors are host file descriptors.
#include "syscalls.h"
#include "vfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace apkrt::sys {
namespace {

// Linux fcntl commands and status flags (include/uapi/asm-generic/fcntl.h, arm64 fcntl.h).
namespace linux_fcntl {
constexpr int dupfd = 0;
constexpr int getfd = 1;
constexpr int setfd = 2;
constexpr int getfl = 3;
constexpr int setfl = 4;
constexpr int dupfd_cloexec = 1030;
constexpr int append = 02000;
constexpr int nonblock = 04000;
constexpr int dsync = 010000;
constexpr int async = 020000;
constexpr int sync = 04000000 | dsync;
}  // namespace linux_fcntl

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

}  // namespace

long sys_fcntl(int fd, int command, std::uint64_t argument) {
    switch (command) {
        case linux_fcntl::dupfd: return result_or_error(::fcntl(fd, F_DUPFD, as_int(argument)));
        case linux_fcntl::dupfd_cloexec: return result_or_error(::fcntl(fd, F_DUPFD_CLOEXEC, as_int(argument)));
        case linux_fcntl::getfd: return result_or_error(::fcntl(fd, F_GETFD));  // FD_CLOEXEC is 1 on both
        case linux_fcntl::setfd: return result_or_error(::fcntl(fd, F_SETFD, as_int(argument) & FD_CLOEXEC));
        case linux_fcntl::getfl: {
            const int flags = ::fcntl(fd, F_GETFL);
            return flags < 0 ? -to_linux_errno(errno) : to_linux_status_flags(flags);
        }
        case linux_fcntl::setfl: {
            // Linux changes only O_APPEND, O_ASYNC, O_DIRECT, O_NOATIME and O_NONBLOCK here and ignores
            // the rest; Darwin has no O_DIRECT or O_NOATIME to set.
            const int requested = as_int(argument);
            const int current = ::fcntl(fd, F_GETFL);
            if (current < 0) return -to_linux_errno(errno);
            int flags = current & ~(O_APPEND | O_NONBLOCK | O_ASYNC);
            if (requested & linux_fcntl::append) flags |= O_APPEND;
            if (requested & linux_fcntl::nonblock) flags |= O_NONBLOCK;
            if (requested & linux_fcntl::async) flags |= O_ASYNC;
            return result_or_error(::fcntl(fd, F_SETFL, flags));
        }
        default: {
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

LinuxStat to_linux_stat(const struct stat &host) {
    LinuxStat result{};
    result.dev = to_linux_device(host.st_dev);
    result.ino = host.st_ino;
    result.mode = host.st_mode;
    result.nlink = host.st_nlink;
    result.uid = host.st_uid;
    result.gid = host.st_gid;
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

}  // namespace

long sys_fstat(int fd, std::uint64_t buffer) {
    struct stat host;
    if (::fstat(fd, &host) != 0) return -to_linux_errno(errno);
    *as_pointer<LinuxStat>(buffer) = to_linux_stat(host);
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

/// Where a guest path lies on the host: `path` relative to the host directory descriptor
/// `directory`, which is AT_FDCWD for an absolute host path; or an error.
struct HostPath {
    long error = 0;
    int directory = AT_FDCWD;
    std::string path;
};

/// Resolves a path argument of openat and its relatives. Absolute paths and paths relative to the
/// working directory go through the path table; a path relative to a directory descriptor stays
/// relative to that descriptor, which the guest can only have opened through the table.
HostPath resolve(int directory, std::uint64_t address) {
    if (address == 0) return {.error = -linux_errno::efault};
    const std::string_view guest(as_pointer<const char>(address));
    if (guest.empty()) return {.error = -linux_errno::enoent};
    if (!guest.starts_with('/') && directory != linux_at_fdcwd) return {.directory = directory, .path = std::string(guest)};
    const auto absolute = vfs::absolute(guest, guest.starts_with('/') ? "/" : vfs::working_directory());
    auto host = vfs::host_path(absolute);
    if (!host) return {.error = -linux_errno::enoent};
    // "file/" is not "file": Linux answers ENOTDIR for it, and so does Darwin with the slash kept.
    if (guest.ends_with('/') && !host->ends_with('/')) *host += '/';
    return {.path = std::move(*host)};
}

/// Darwin's *at() flag for AT_SYMLINK_NOFOLLOW.
int host_at_flags(int linux_flags) {
    return (linux_flags & linux_at_symlink_nofollow) ? AT_SYMLINK_NOFOLLOW : 0;
}

// --- Directory streams ---------------------------------------------------------------------------------
//
// getdents64 reads a directory through a DIR* of its own for each guest descriptor, opened on a
// duplicate at the first call. Positions (d_off, lseek) count entries from the start.

struct DirectoryStream {
    DIR *directory;
    long position;  // entries read so far
};

std::mutex streams_mutex;
std::map<int, DirectoryStream> streams;  // guarded by streams_mutex

/// Forgets the stream of a descriptor that is closed or replaced.
void drop_stream(int fd) {
    const std::lock_guard lock(streams_mutex);
    if (const auto found = streams.find(fd); found != streams.end()) {
        ::closedir(found->second.directory);
        streams.erase(found);
    }
}

/// struct linux_dirent64 (include/linux/dirent.h): 19 bytes of header, the name, 8-byte aligned.
constexpr std::size_t dirent_header = 19;

long get_directory_entries(int fd, std::uint64_t buffer, std::uint64_t size) {
    const std::lock_guard lock(streams_mutex);
    auto found = streams.find(fd);
    if (found == streams.end()) {
        struct stat status;
        if (::fstat(fd, &status) != 0) return -to_linux_errno(errno);
        if (!S_ISDIR(status.st_mode)) return -linux_errno::enotdir;
        const int copy = ::dup(fd);
        if (copy < 0) return -to_linux_errno(errno);
        DIR *directory = ::fdopendir(copy);
        if (directory == nullptr) {
            const int error = errno;
            ::close(copy);
            return -to_linux_errno(error);
        }
        ::rewinddir(directory);
        found = streams.emplace(fd, DirectoryStream{directory, 0}).first;
    }
    auto &stream = found->second;
    auto *const output = as_pointer<char>(buffer);
    std::uint64_t used = 0;
    while (true) {
        const long before = ::telldir(stream.directory);
        errno = 0;
        const dirent *entry = ::readdir(stream.directory);
        if (entry == nullptr) {
            if (errno != 0 && used == 0) return -to_linux_errno(errno);
            break;
        }
        const std::size_t name_length = std::strlen(entry->d_name);
        const std::size_t record = (dirent_header + name_length + 1 + 7) & ~std::size_t{7};
        if (used + record > size) {
            ::seekdir(stream.directory, before);  // it stays for the next call
            if (used == 0) return -linux_errno::einval;
            break;
        }
        char *const out = output + used;
        std::memset(out, 0, record);
        const std::uint64_t inode = entry->d_ino;
        const std::int64_t next = stream.position + 1;
        const auto length = static_cast<std::uint16_t>(record);
        std::memcpy(out, &inode, 8);
        std::memcpy(out + 8, &next, 8);
        std::memcpy(out + 16, &length, 2);
        out[18] = static_cast<char>(entry->d_type);  // DT_* values agree
        std::memcpy(out + dirent_header, entry->d_name, name_length);
        used += record;
        ++stream.position;
    }
    return static_cast<long>(used);
}

/// lseek on a descriptor getdents64 reads from: positions count entries.
std::optional<long> seek_stream(int fd, std::int64_t offset, int whence) {
    const std::lock_guard lock(streams_mutex);
    const auto found = streams.find(fd);
    if (found == streams.end()) return std::nullopt;
    auto &stream = found->second;
    std::int64_t target;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = stream.position + offset; break;
        default: return -linux_errno::einval;
    }
    if (target < 0) return -linux_errno::einval;
    ::rewinddir(stream.directory);
    stream.position = 0;
    while (stream.position < target && ::readdir(stream.directory) != nullptr) ++stream.position;
    return stream.position;
}

}  // namespace

long sys_openat(int directory, std::uint64_t path, int flags, int mode) {
    if (flags & linux_open::tmpfile) return -linux_errno::eopnotsupp;
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
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
    if (flags & linux_open::cloexec) host_flags |= O_CLOEXEC;
    if (flags & linux_open::sync) host_flags |= O_SYNC;
    else if (flags & linux_open::dsync) host_flags |= O_DSYNC;
    // O_LARGEFILE, O_NOATIME and O_DIRECT change nothing a program can observe here.
    const int fd = ::openat(host.directory, host.path.c_str(), host_flags, mode & 07777);
    if (fd < 0) return -to_linux_errno(errno);
    drop_stream(fd);  // a stale stream of an earlier descriptor with this number
    return fd;
}

long sys_close(int fd) {
    drop_stream(fd);
    return result_or_error(::close(fd));
}

long sys_faccessat(int directory, std::uint64_t path, int mode, int flags) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    int host_flags = host_at_flags(flags);
    if (flags & linux_at_eaccess) host_flags |= AT_EACCESS;
    return result_or_error(::faccessat(host.directory, host.path.c_str(), mode & 07, host_flags));
}

long sys_newfstatat(int directory, std::uint64_t path, std::uint64_t buffer, int flags) {
    // Since Linux 6.11 the path may be NULL along with AT_EMPTY_PATH.
    const bool empty = path == 0 || *as_pointer<const char>(path) == '\0';
    if (empty && (flags & linux_at_empty_path)) return sys_fstat(directory, buffer);
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    struct stat status;
    if (::fstatat(host.directory, host.path.c_str(), &status, host_at_flags(flags)) != 0) return -to_linux_errno(errno);
    *as_pointer<LinuxStat>(buffer) = to_linux_stat(status);
    return 0;
}

long sys_mkdirat(int directory, std::uint64_t path, int mode) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    return result_or_error(::mkdirat(host.directory, host.path.c_str(), static_cast<mode_t>(mode & 07777)));
}

long sys_unlinkat(int directory, std::uint64_t path, int flags) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    const bool remove_directory = (flags & linux_at_removedir) != 0;
    if (::unlinkat(host.directory, host.path.c_str(), remove_directory ? AT_REMOVEDIR : 0) == 0) return 0;
    const int error = errno;
    // unlink() of a directory: EISDIR on Linux, EPERM (as POSIX allows) on Darwin.
    struct stat status;
    if (!remove_directory && error == EPERM &&
        ::fstatat(host.directory, host.path.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(status.st_mode)) {
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
    const unsigned host_flags = (flags & rename_noreplace) ? RENAME_EXCL : (flags & rename_exchange) ? RENAME_SWAP : 0;
    return result_or_error(
        ::renameatx_np(from.directory, from.path.c_str(), to.directory, to.path.c_str(), host_flags));
}

long sys_linkat(int old_directory, std::uint64_t old_path, int new_directory, std::uint64_t new_path, int flags) {
    constexpr int linux_at_symlink_follow = 0x400;
    const auto from = resolve(old_directory, old_path);
    if (from.error != 0) return from.error;
    const auto to = resolve(new_directory, new_path);
    if (to.error != 0) return to.error;
    const int host_flags = (flags & linux_at_symlink_follow) ? AT_SYMLINK_FOLLOW : 0;
    return result_or_error(::linkat(from.directory, from.path.c_str(), to.directory, to.path.c_str(), host_flags));
}

long sys_symlinkat(std::uint64_t target, int directory, std::uint64_t path) {
    if (target == 0) return -linux_errno::efault;
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    // The link keeps the guest's text; readlink gives it back unchanged.
    return result_or_error(::symlinkat(as_pointer<const char>(target), host.directory, host.path.c_str()));
}

long sys_readlinkat(int directory, std::uint64_t path, std::uint64_t buffer, std::int64_t size) {
    if (size <= 0) return -linux_errno::einval;
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    return result_or_error(::readlinkat(host.directory, host.path.c_str(), as_pointer<char>(buffer),
                                        static_cast<std::size_t>(size)));
}

long sys_fchmodat(int directory, std::uint64_t path, int mode) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    return result_or_error(::fchmodat(host.directory, host.path.c_str(), static_cast<mode_t>(mode & 07777), 0));
}

long sys_fchownat(int directory, std::uint64_t path, std::uint32_t owner, std::uint32_t group, int flags) {
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    return result_or_error(::fchownat(host.directory, host.path.c_str(), owner, group, host_at_flags(flags)));
}

long sys_truncate(std::uint64_t path, std::int64_t length) {
    const auto host = resolve(linux_at_fdcwd, path);
    if (host.error != 0) return host.error;
    return result_or_error(::truncate(host.path.c_str(), length));
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
    if (path == 0) return result_or_error(::futimens(directory, pointer));  // Linux allows a NULL path
    const auto host = resolve(directory, path);
    if (host.error != 0) return host.error;
    return result_or_error(::utimensat(host.directory, host.path.c_str(), pointer, host_at_flags(flags)));
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
    if (path == 0) return -linux_errno::efault;
    const std::string_view text(as_pointer<const char>(path));
    if (text.empty()) return -linux_errno::enoent;
    const auto guest = vfs::absolute(text, vfs::working_directory());
    const auto host = vfs::host_path(guest);
    if (!host) return -linux_errno::enoent;
    return change_directory(guest, *host);
}

long sys_fchdir(int fd) {
    char host[MAXPATHLEN];
    if (::fcntl(fd, F_GETPATH, host) != 0) return -to_linux_errno(errno);
    const auto guest = vfs::guest_path(host);
    if (!guest) return -linux_errno::enoent;  // a directory the guest cannot name
    return change_directory(*guest, host);
}

long sys_lseek(int fd, std::int64_t offset, int whence) {
    constexpr int linux_seek_data = 3, linux_seek_hole = 4;  // the other way round on Darwin
    if (const auto result = seek_stream(fd, offset, whence)) return *result;
    int host_whence;
    switch (whence) {
        case SEEK_SET:
        case SEEK_CUR:
        case SEEK_END: host_whence = whence; break;
        case linux_seek_data: host_whence = SEEK_DATA; break;
        case linux_seek_hole: host_whence = SEEK_HOLE; break;
        default: return -linux_errno::einval;
    }
    return result_or_error(::lseek(fd, offset, host_whence));
}

long sys_getdents64(int fd, std::uint64_t buffer, std::uint64_t size) {
    return get_directory_entries(fd, buffer, size);
}

long sys_dup3(int fd, int target, int flags) {
    if (fd == target || (flags & ~linux_open::cloexec) != 0) return -linux_errno::einval;
    drop_stream(target);
    const int result = ::dup2(fd, target);
    if (result < 0) return -to_linux_errno(errno);
    if (flags & linux_open::cloexec) ::fcntl(result, F_SETFD, FD_CLOEXEC);
    return result;
}

long sys_pipe2(std::uint64_t fds, int flags) {
    constexpr int linux_o_direct = 0200000;  // packet mode; nothing Darwin offers
    if (flags & ~(linux_open::cloexec | linux_open::nonblock | linux_o_direct)) return -linux_errno::einval;
    int pair[2];
    if (::pipe(pair) != 0) return -to_linux_errno(errno);
    for (const int fd : pair) {
        drop_stream(fd);
        if (flags & linux_open::cloexec) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (flags & linux_open::nonblock) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    std::memcpy(as_pointer<int>(fds), pair, sizeof pair);
    return 0;
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
    switch (request) {
        case tcgets: {
            termios host;
            if (::tcgetattr(fd, &host) != 0) return errno == ENOTTY ? -linux_errno::enotty : -to_linux_errno(errno);
            *as_pointer<LinuxTermios>(argument) = to_linux_termios(host);
            return 0;
        }
        case tiocgwinsz:  // struct winsize: four unsigned shorts on both systems
            return result_or_error(::ioctl(fd, TIOCGWINSZ, as_pointer<winsize>(argument)));
        case fionread:
            return result_or_error(::ioctl(fd, FIONREAD, as_pointer<int>(argument)));
        case fionbio:
            return result_or_error(::ioctl(fd, FIONBIO, as_pointer<int>(argument)));
        case fioclex: return result_or_error(::fcntl(fd, F_SETFD, FD_CLOEXEC));
        case fionclex: return result_or_error(::fcntl(fd, F_SETFD, 0));
        default: {
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

LinuxStatfs to_linux_statfs(const struct statfs &host) {
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
    result.flags = st_valid | ((host.f_flags & MNT_RDONLY) ? st_rdonly : 0) | ((host.f_flags & MNT_NOSUID) ? st_nosuid : 0);
    return result;
}

}  // namespace

long sys_statfs(std::uint64_t path, std::uint64_t buffer) {
    const auto host = resolve(linux_at_fdcwd, path);
    if (host.error != 0) return host.error;
    struct statfs status;
    if (::statfs(host.path.c_str(), &status) != 0) return -to_linux_errno(errno);
    *as_pointer<LinuxStatfs>(buffer) = to_linux_statfs(status);
    return 0;
}

long sys_fstatfs(int fd, std::uint64_t buffer) {
    struct statfs status;
    if (::fstatfs(fd, &status) != 0) return -to_linux_errno(errno);
    *as_pointer<LinuxStatfs>(buffer) = to_linux_statfs(status);
    return 0;
}

}  // namespace apkrt::sys
