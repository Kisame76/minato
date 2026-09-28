// The guest's descriptor table (ADR-0027): Linux's numbers for the guest, each referring to an open
// file description that owns a host descriptor. Host descriptors of the app and of the runtime are
// never in it, so the guest can neither see nor close them. There is one table for the process,
// shared by every guest thread, as Linux's is with CLONE_FILES.
#pragma once

#include <dirent.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace apkrt::descriptors {

/// An open file description, Linux's struct file: what a guest number refers to. Its duplicates
/// share it, and with it the host descriptor's offset and status flags, and the guest's O_NONBLOCK.
class Description {
public:
    /// What it is: a host file, whose host descriptor the guest's calls use, or an object of the
    /// runtime's own (sys_events.cpp, ADR-0027; a vsync or input channel, channels.cpp, ADR-0036), whose
    /// host descriptor only shows when it is ready.
    enum class Kind { file, eventfd, epoll, channel };

    /// For a pipe or FIFO open in one direction: which end it is. The kqueue filter of the other
    /// direction fires only when the other end goes (sys_events.cpp).
    enum class PipeEnd { none, reading, writing };

    /// Takes `host`, which is closed with the description, when its last guest number closes.
    Description(int host, bool can_block, bool adopted, bool nonblocking, Kind kind = Kind::file,
                PipeEnd pipe_end = PipeEnd::none)
        : nonblocking_(nonblocking), host_(host), can_block_(can_block), adopted_(adopted), kind_(kind),
          pipe_end_(pipe_end) {}
    virtual ~Description();
    Description(const Description &) = delete;
    Description &operator=(const Description &) = delete;

    int host() const { return host_; }

    Kind kind() const { return kind_; }

    PipeEnd pipe_end() const { return pipe_end_; }

    /// Whether a read or write of it can wait (ADR-0027, decision 5): a pipe, a FIFO, a socket or a
    /// terminal. Its host descriptor is non-blocking, unless it is adopted; the guest's calls wait in
    /// the runtime (syscalls.h, wait_for_descriptors). An eventfd or an epoll waits in its own way.
    bool can_block() const { return can_block_; }

    /// Whether it is a duplicate of one of the starter's standard descriptors, whose host status flags
    /// it shares with the starter: the runtime never changes them.
    bool adopted() const { return adopted_; }

    /// The guest's O_NONBLOCK, which lives only here: F_GETFL, F_SETFL and FIONBIO read and change it.
    bool nonblocking() const { return nonblocking_.load(std::memory_order_relaxed); }
    void set_nonblocking(bool value) { nonblocking_.store(value, std::memory_order_relaxed); }

    // getdents64's stream on a directory (sys_files.cpp), opened at its first call. Its position
    // counts entries and belongs to the open file, as on Linux: duplicates share it.
    std::mutex stream_mutex;
    DIR *stream = nullptr;     // guarded by stream_mutex
    long stream_position = 0;  // guarded by stream_mutex

private:
    std::atomic<bool> nonblocking_;
    const int host_;
    const bool can_block_;
    const bool adopted_;
    const Kind kind_;
    const PipeEnd pipe_end_;
};

using Handle = std::shared_ptr<Description>;

/// The size of the table: the RLIMIT_NOFILE of Android apps, soft and hard (Android 15 in the emulator).
constexpr int limit = 32768;

/// The guest's RLIMIT_NOFILE: `soft` bounds every new number, and `hard` bounds `soft`. Both start at
/// `limit`; bionic's fdsan asks for them when a program closes a number above 127.
struct Limits {
    std::uint64_t soft;
    std::uint64_t hard;
};
Limits limits();

/// setrlimit's rules for RLIMIT_NOFILE: -EINVAL when `soft` exceeds `hard`, -EPERM when `hard` would
/// rise, as for a process without CAP_SYS_RESOURCE.
long set_limits(Limits requested);

/// The description behind `fd`, or null when `fd` is not open.
Handle get(int fd);

/// The size Linux's table would have after the numbers handed out so far: 64 at first, then 128 times
/// a power of two (alloc_fdtable in fs/file.c). select looks no further.
int capacity();

/// Takes `host`, a host descriptor the runtime has just opened for the guest, into the table at the
/// lowest free number and returns that number; `host` gets FD_CLOEXEC on the host, and O_NONBLOCK when
/// it can block. `nonblocking` is the guest's O_NONBLOCK. When the table is full, `host` is closed and
/// the result is -EMFILE.
long add(int host, bool close_on_exec, bool nonblocking = false);

/// Takes `description`, which the runtime has just made for the guest, into the table at the lowest
/// free number and returns that number; -EMFILE when the table is full, and it goes.
long add(Handle description, bool close_on_exec);

/// dup and F_DUPFD: `fd`'s description at the lowest free number from `minimum` on.
long duplicate(int fd, int minimum, bool close_on_exec);

/// dup3 without its check of the flags: `target` refers to `fd`'s description from now on, and what
/// it referred to before is closed.
long duplicate_to(int fd, int target, bool close_on_exec);

/// close.
long close(int fd);

/// FD_CLOEXEC of a number, 0 or 1, and setting it; -EBADF when the number is not open.
long close_on_exec(int fd);
long set_close_on_exec(int fd, bool value);

/// At a program's start: guest 0, 1 and 2 become duplicates of the starter's host 0, 1 and 2, adopted
/// with the O_NONBLOCK they have, and the host's soft limit of descriptors rises, as far as its hard
/// limit allows, to hold the table.
void adopt_standard_descriptors();

/// Empties the table, for tests.
void reset();

}  // namespace apkrt::descriptors
