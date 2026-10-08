// memfd_create's files (the compatibility plan's memfd step): what libcutils' ashmem makes once sys.use_memfd is on,
// for ASharedMemory, SharedMemory and CursorWindow's window. A memfd is a host file in the runtime's own folder
// (sockets::own_folder), named there until its last open file closes, so that an open of /proc/self/fd/<n> gets
// another open file of it with its own offset and flags, as Linux's does. Its Inode, Linux's shmem inode, holds the
// name, the seals and the mode that every open file of it shares. The seals are checked where Linux checks them:
// write and its relatives, ftruncate and fallocate (sys_files.cpp), mmap (sys_memory.cpp). Departure: the host's file
// system may write a memfd's dirty pages back to storage, where Linux keeps them in memory (tmpfs).
#pragma once

#include "descriptors.h"

#include <sys/types.h>
#include <sys/uio.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace apkrt::memfd {

// Linux's seals (include/uapi/linux/fcntl.h).
constexpr unsigned seal_seal = 0x1, seal_shrink = 0x2, seal_grow = 0x4, seal_write = 0x8, seal_future_write = 0x10,
                   seal_exec = 0x20;

/// What Linux keeps in a memfd's shmem inode. Its host file goes with it.
struct Inode {
    Inode(std::string guest_name, int host_folder, std::string file_name, unsigned initial_seals, mode_t file_mode)
        : name(std::move(guest_name)), folder(host_folder), host_name(std::move(file_name)), seals(initial_seals),
          mode(file_mode) {}
    ~Inode();
    Inode(const Inode &) = delete;
    Inode &operator=(const Inode &) = delete;

    const std::string name;       // the guest's, without "memfd:"
    const int folder;             // the runtime's own folder, open as long as the process
    const std::string host_name;  // the host file's name there
    std::mutex mutex;             // for what follows, and for a write's look at the seals and the write itself
    unsigned seals;
    mode_t mode;                  // 0777, or 0666 with MFD_NOEXEC_SEAL
    bool writes_denied = false;   // F_ADD_SEALS added F_SEAL_WRITE: no shared mapping of a writable open file comes
                                  // any more (mapping_deny_writable)
};

/// An open file of a memfd, Linux's struct file of it: memfd_create's, or a reopen's through /proc/self/fd.
class File final : public descriptors::Description {
public:
    File(int host, bool nonblocking, std::shared_ptr<Inode> shared, bool open_for_writing)
        : Description(host, false, false, nonblocking), inode(std::move(shared)), writable(open_for_writing) {}

    const std::shared_ptr<Inode> inode;
    const bool writable;  // FMODE_WRITE
};

/// The memfd's open file behind `description`, or null.
File *as_file(descriptors::Description &description);
const File *as_file(const descriptors::Description &description);

/// What /proc/self/fd's link and /proc/self/maps show for a memfd: "/memfd:<name> (deleted)".
std::string link_text(const File &file);

/// The memfd's mode bits, which fstat shows.
mode_t mode_of(const File &file);

/// memfd_create with Linux 6.6's flags and checks: the guest number, or a negative Linux errno.
long create(std::uint64_t name, unsigned flags);

/// An open of /proc/self/fd/<fd> with Linux's open `flags` when `fd` is a memfd: another open file of it, or a
/// negative Linux errno; nullopt for every other descriptor.
std::optional<long> reopen(int fd, int flags);

/// fcntl's F_GET_SEALS and F_ADD_SEALS, Linux's memfd_get_seals and memfd_add_seals, on any description.
long get_seals(descriptors::Description &description);
long add_seals(descriptors::Description &description, unsigned seals);

/// write and writev (`offset` -1: at the open file's offset), pwrite and pwritev of `vectors` under the seals:
/// F_SEAL_WRITE and F_SEAL_FUTURE_WRITE refuse it (EPERM), and F_SEAL_GROW keeps it inside the size as Linux's write
/// page by page does: up to the last page boundary below the size, or EPERM when that leaves nothing.
long write(File &file, const iovec *vectors, int count, std::int64_t offset);

/// ftruncate under F_SEAL_SHRINK and F_SEAL_GROW (EPERM).
long truncate(File &file, std::int64_t length);

/// fallocate under the seals, shmem_fallocate's checks, then `allocate`, with the inode's mutex held throughout: a
/// hole under F_SEAL_WRITE or F_SEAL_FUTURE_WRITE, and a range that ends past the size under F_SEAL_GROW, even with
/// FALLOC_FL_KEEP_SIZE, are EPERM.
long fallocate(File &file, bool hole, std::int64_t end, const std::function<long()> &allocate);

}  // namespace apkrt::memfd
