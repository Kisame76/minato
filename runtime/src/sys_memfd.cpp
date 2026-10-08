// memfd_create, its seals and its reopen through /proc/self/fd (memfd.h), with Linux 6.6's answers (mm/memfd.c,
// mm/shmem.c) [verified: memfd-test on the emulator, 2026-10-08].
#include "memfd.h"

#include "sockets.h"
#include "syscalls.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <vector>

namespace apkrt::memfd {
namespace {

namespace linux_errno = sys::linux_errno;

// memfd_create's flags (include/uapi/linux/memfd.h).
constexpr unsigned mfd_cloexec = 0x1, mfd_allow_sealing = 0x2, mfd_hugetlb = 0x4, mfd_noexec_seal = 0x8,
                   mfd_exec = 0x10;
constexpr unsigned mfd_all = mfd_cloexec | mfd_allow_sealing | mfd_hugetlb | mfd_noexec_seal | mfd_exec;
constexpr unsigned mfd_huge_sizes = 0x3fu << 26;  // MFD_HUGE_MASK << MFD_HUGE_SHIFT, with MFD_HUGETLB only
constexpr unsigned all_seals = seal_seal | seal_shrink | seal_grow | seal_write | seal_future_write | seal_exec;
/// MFD_NAME_MAX_LEN: NAME_MAX less "memfd:".
constexpr std::size_t name_max = 255 - 6;

// Linux open flags (include/uapi/asm-generic/fcntl.h, arm64's fcntl.h).
namespace linux_open {
constexpr int access_mode = 03;  // O_RDONLY, O_WRONLY, O_RDWR agree with Darwin
constexpr int create = 0100;
constexpr int exclusive = 0200;
constexpr int truncate = 01000;
constexpr int append = 02000;
constexpr int nonblock = 04000;
constexpr int directory = 040000;
constexpr int cloexec = 02000000;
constexpr int path = 010000000;
}  // namespace linux_open

std::atomic<std::uint64_t> made{0};

/// Whether `description` is open for writing (FMODE_WRITE). An eventfd, an epoll or a channel is, as Linux's
/// anonymous files are.
bool open_for_writing(descriptors::Description &description) {
    if (const auto *file = as_file(description)) return file->writable;
    if (description.kind() != descriptors::Description::Kind::file) return true;
    const int flags = ::fcntl(description.host(), F_GETFL);
    return flags >= 0 && (flags & O_ACCMODE) != O_RDONLY;
}

/// The memfd's size, or -1 with errno.
std::int64_t size_of(const File &file) {
    struct stat status{};
    return ::fstat(file.host(), &status) == 0 ? status.st_size : -1;
}

}  // namespace

Inode::~Inode() {
    ::unlinkat(folder, host_name.c_str(), 0);
}

File *as_file(descriptors::Description &description) {
    return dynamic_cast<File *>(&description);
}

const File *as_file(const descriptors::Description &description) {
    return dynamic_cast<const File *>(&description);
}

std::string link_text(const File &file) {
    return "/memfd:" + file.inode->name + " (deleted)";
}

mode_t mode_of(const File &file) {
    const std::lock_guard lock(file.inode->mutex);
    return file.inode->mode;
}

long create(std::uint64_t name, unsigned flags) {
    // The flags first, then the name, then the file, in memfd_create's order.
    if (flags & ~(mfd_all | ((flags & mfd_hugetlb) ? mfd_huge_sizes : 0u))) return -linux_errno::einval;
    if ((flags & mfd_exec) && (flags & mfd_noexec_seal)) return -linux_errno::einval;
    if (name == 0) return -linux_errno::efault;
    const auto *start = sys::as_pointer<const char>(name);
    const std::size_t length = ::strnlen(start, name_max + 1);
    if (length > name_max) return -linux_errno::einval;
    // Android 15's kernel has no hugetlbfs (hugetlb_file_setup's ENOSYS).
    if (flags & mfd_hugetlb) return -linux_errno::enosys;
    const int folder = sockets::own_folder();
    if (folder < 0) return -linux_errno::enomem;
    std::string host_name;
    int host = -1;
    for (int attempt = 0; attempt < 4 && host < 0; ++attempt) {
        host_name = sockets::own_file_name("memfd-" + std::to_string(++made) + "-");
        host = ::openat(folder, host_name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    if (host < 0) return -sys::to_linux_errno(errno);
    // Sealing: F_SEAL_SEAL keeps every other seal away unless MFD_ALLOW_SEALING; MFD_NOEXEC_SEAL takes the execute
    // bits away and seals them, and allows sealing. Without MFD_EXEC or MFD_NOEXEC_SEAL a memfd is executable, as
    // vm.memfd_noexec's 0 has it on Android 15.
    unsigned seals = seal_seal;
    mode_t mode = 0777;
    if (flags & mfd_noexec_seal) {
        mode = 0666;
        seals = seal_exec;
    } else if (flags & mfd_allow_sealing) {
        seals = 0;
    }
    auto inode = std::make_shared<Inode>(std::string(start, length), folder, std::move(host_name), seals, mode);
    return descriptors::add(std::make_shared<File>(host, false, std::move(inode), true), (flags & mfd_cloexec) != 0);
}

std::optional<long> reopen(int fd, int flags) {
    const auto description = descriptors::get(fd);
    File *const file = description ? as_file(*description) : nullptr;
    if (file == nullptr) return std::nullopt;
    // The link names a file that is there, and no folder.
    if ((flags & linux_open::create) && (flags & linux_open::exclusive)) return -linux_errno::eexist;
    if (flags & linux_open::directory) return -linux_errno::enotdir;
    const int access = (flags & linux_open::path) ? O_RDONLY : (flags & linux_open::access_mode);
    int host_flags = access | O_NOFOLLOW | O_CLOEXEC;
    if (flags & linux_open::append) host_flags |= O_APPEND;
    Inode &inode = *file->inode;
    const std::lock_guard lock(inode.mutex);
    // O_TRUNC empties it as do_truncate does, which F_SEAL_SHRINK refuses for a memfd that holds something.
    if ((flags & linux_open::truncate) && access != O_RDONLY) {
        const std::int64_t size = size_of(*file);
        if (size > 0 && (inode.seals & seal_shrink)) return -linux_errno::eperm;
        host_flags |= O_TRUNC;
    }
    const int host = ::openat(inode.folder, inode.host_name.c_str(), host_flags);
    if (host < 0) return -sys::to_linux_errno(errno);
    auto reopened = std::make_shared<File>(host, (flags & linux_open::nonblock) != 0, file->inode, access != O_RDONLY);
    return descriptors::add(std::move(reopened), (flags & linux_open::cloexec) != 0);
}

long get_seals(descriptors::Description &description) {
    File *const file = as_file(description);
    if (file == nullptr) return -linux_errno::einval;
    const std::lock_guard lock(file->inode->mutex);
    return file->inode->seals;
}

long add_seals(descriptors::Description &description, unsigned seals) {
    // memfd_add_seals' order: a file open for writing, known seals, a memfd, no F_SEAL_SEAL, then F_SEAL_WRITE's wait
    // for no shared mapping of a writable open file.
    if (!open_for_writing(description)) return -linux_errno::eperm;
    if (seals & ~all_seals) return -linux_errno::einval;
    File *const file = as_file(description);
    if (file == nullptr) return -linux_errno::einval;
    Inode &inode = *file->inode;
    const auto add = [&](bool shared_writable_mapping) -> long {
        const std::lock_guard lock(inode.mutex);
        if (inode.seals & seal_seal) return -linux_errno::eperm;
        if ((seals & seal_write) && !(inode.seals & seal_write)) {
            if (shared_writable_mapping) return -linux_errno::ebusy;
            inode.writes_denied = true;
        }
        // F_SEAL_EXEC on a memfd with execute bits brings the seals of writes and of the size along (W^X).
        if ((seals & seal_exec) && (inode.mode & 0111)) seals |= seal_shrink | seal_grow | seal_write | seal_future_write;
        inode.seals |= seals;
        return 0;
    };
    if (seals & seal_write) return sys::memory::with_shared_mappings_of(inode, add);
    return add(false);
}

long write(File &file, const iovec *vectors, int count, std::int64_t offset) {
    if (!file.writable) return -linux_errno::ebadf;  // vfs_write, before the file's own checks
    std::uint64_t total = 0;
    for (int index = 0; index < count; ++index) total += vectors[index].iov_len;
    Inode &inode = *file.inode;
    const std::lock_guard lock(inode.mutex);
    if (total == 0) return 0;  // generic_write_checks: nothing to write is no write
    if (inode.seals & (seal_write | seal_future_write)) return -linux_errno::eperm;
    std::vector<iovec> clipped;
    if (inode.seals & seal_grow) {
        const std::int64_t size = size_of(file);
        if (size < 0) return -sys::to_linux_errno(errno);
        std::int64_t position = offset;
        if (position < 0) {
            const int status = ::fcntl(file.host(), F_GETFL);
            position = (status >= 0 && (status & O_APPEND)) ? size : ::lseek(file.host(), 0, SEEK_CUR);
            if (position < 0) return -sys::to_linux_errno(errno);
        }
        if (static_cast<std::uint64_t>(position) + total > static_cast<std::uint64_t>(size)) {
            // shmem_write_begin refuses the first page that would grow the file; the pages before it are written.
            const auto page = static_cast<std::int64_t>(::getpagesize());
            const std::int64_t boundary = size / page * page;
            if (boundary <= position) return -linux_errno::eperm;
            auto allowed = static_cast<std::uint64_t>(boundary - position);
            for (int index = 0; index < count && allowed > 0; ++index) {
                const std::size_t part = std::min<std::uint64_t>(vectors[index].iov_len, allowed);
                clipped.push_back({vectors[index].iov_base, part});
                allowed -= part;
            }
            vectors = clipped.data();
            count = static_cast<int>(clipped.size());
        }
    }
    const ssize_t written = offset < 0 ? ::writev(file.host(), vectors, count)
                                       : ::pwritev(file.host(), vectors, count, static_cast<off_t>(offset));
    return sys::result_or_error(written);
}

long truncate(File &file, std::int64_t length) {
    if (length < 0 || !file.writable) return -linux_errno::einval;  // do_sys_ftruncate
    Inode &inode = *file.inode;
    const std::lock_guard lock(inode.mutex);
    const std::int64_t size = size_of(file);
    if (size < 0) return -sys::to_linux_errno(errno);
    if ((length < size && (inode.seals & seal_shrink)) || (length > size && (inode.seals & seal_grow))) {
        return -linux_errno::eperm;  // shmem_setattr
    }
    return sys::result_or_error(::ftruncate(file.host(), static_cast<off_t>(length)));
}

long fallocate(File &file, bool hole, std::int64_t end, const std::function<long()> &allocate) {
    Inode &inode = *file.inode;
    const std::lock_guard lock(inode.mutex);
    if (hole && (inode.seals & (seal_write | seal_future_write))) return -linux_errno::eperm;
    if (inode.seals & seal_grow) {
        const std::int64_t size = size_of(file);
        if (size < 0) return -sys::to_linux_errno(errno);
        if (end > size) return -linux_errno::eperm;
    }
    return allocate();
}

}  // namespace apkrt::memfd
