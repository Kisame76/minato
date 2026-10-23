// The guest's descriptor table (ADR-0027, descriptors.h). Numbers index a vector of entries under one
// lock; a description's host descriptor is closed outside the lock, when its last handle goes.
#include "descriptors.h"
#include "watches.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <utility>
#include <vector>

namespace apkrt::descriptors {
namespace {

// Linux errno values (the syscall layer's are in syscalls.h, which this file does not need).
constexpr long eperm = 1;
constexpr long ebadf = 9;
constexpr long einval = 22;
constexpr long emfile = 24;

struct Entry {
    Handle description;
    bool close_on_exec = false;
};

std::mutex mutex;
std::vector<Entry> entries;       // guarded by mutex; grows with the highest number taken
int lowest_free = 0;              // guarded by mutex: no number below it is free
Limits current = {limit, limit};  // guarded by mutex

/// The soft limit: every number the guest gets lies below it.
int bound() {
    return static_cast<int>(current.soft);
}

bool is_open(int fd) {
    const auto index = static_cast<std::size_t>(fd);
    return fd >= 0 && index < entries.size() && entries[index].description;
}

/// The lowest free number from `minimum` on, or -1 when every one below the soft limit is taken.
int free_number(int minimum) {
    for (int fd = std::max(minimum, lowest_free); fd < bound(); ++fd) {
        if (!is_open(fd)) return fd;
    }
    return -1;
}

/// Makes `fd`, a number below the soft limit, refer to `description`, and returns the handle it held
/// before, for the caller to drop outside the lock.
Handle place(int fd, Handle description, bool close_on_exec) {
    const auto index = static_cast<std::size_t>(fd);
    if (index >= entries.size()) entries.resize(index + 1);
    auto previous = std::exchange(entries[index], Entry{std::move(description), close_on_exec}).description;
    if (fd == lowest_free) ++lowest_free;
    return previous;
}

struct Traits {
    bool can_block = false;
    Description::PipeEnd pipe_end = Description::PipeEnd::none;
};

/// Whether a read or write of `host` can wait (ADR-0027, decision 5): pipes, FIFOs, sockets and
/// terminals can; regular files, directories and devices such as /dev/null cannot. And which end a
/// pipe or FIFO open in one direction is.
Traits traits_of(int host) {
    struct stat status;
    if (::fstat(host, &status) != 0) return {};
    if (S_ISFIFO(status.st_mode)) {
        const int mode = ::fcntl(host, F_GETFL) & O_ACCMODE;
        using enum Description::PipeEnd;
        return {true, mode == O_RDONLY ? reading : mode == O_WRONLY ? writing : none};
    }
    if (S_ISSOCK(status.st_mode)) return {true};
    return {S_ISCHR(status.st_mode) && ::isatty(host) != 0};
}

/// Lets the host's soft limit of descriptors hold the table and what the app and the runtime open
/// themselves, as far as the hard limit allows.
void raise_host_limit() {
    rlimit host{};
    if (::getrlimit(RLIMIT_NOFILE, &host) != 0) return;
    constexpr rlim_t wanted = limit + 1024;
    if (host.rlim_cur >= wanted) return;
    rlimit raised = host;
    raised.rlim_cur = std::min(wanted, host.rlim_max);
    if (::setrlimit(RLIMIT_NOFILE, &raised) == 0) return;
    // Darwin refuses more than OPEN_MAX to a process whose hard limit is unlimited.
    raised.rlim_cur = std::min<rlim_t>(OPEN_MAX, host.rlim_max);
    static_cast<void>(::setrlimit(RLIMIT_NOFILE, &raised));
}

}  // namespace

Description::~Description() {
    if (stream != nullptr) ::closedir(stream);
    ::close(host_);
    if (const int watch = write_watch_.load(std::memory_order_relaxed)) watches::writer_closed(watch);
}

Handle get(int fd) {
    const std::lock_guard lock(mutex);
    return is_open(fd) ? entries[static_cast<std::size_t>(fd)].description : nullptr;
}

int capacity() {
    std::size_t size;
    {
        const std::lock_guard lock(mutex);
        size = entries.size();  // the highest number handed out, plus one: the vector never shrinks
    }
    if (size <= 64) return 64;
    return static_cast<int>(128 * std::bit_ceil((size - 1) / 128 + 1));
}

long add(int host, bool close_on_exec, bool nonblocking, int write_watch) {
    ::fcntl(host, F_SETFD, FD_CLOEXEC);
    // The guest's calls on a descriptor that can block wait in the runtime, never in the host call.
    const auto traits = traits_of(host);
    if (traits.can_block) ::fcntl(host, F_SETFL, ::fcntl(host, F_GETFL) | O_NONBLOCK);
    auto description = std::make_shared<Description>(host, traits.can_block, false, nonblocking,
                                                      Description::Kind::file, traits.pipe_end);
    description->set_write_watch(write_watch);  // before the number is published: no thread can close it first
    return add(std::move(description), close_on_exec);
}

long add(Handle description, bool close_on_exec) {
    Handle previous;
    const std::lock_guard lock(mutex);
    const int fd = free_number(0);
    if (fd < 0) return -emfile;  // the description closes its host descriptor on its way out, after the lock
    previous = place(fd, std::move(description), close_on_exec);
    return fd;
}

long duplicate(int fd, int minimum, bool close_on_exec) {
    Handle previous;
    const std::lock_guard lock(mutex);
    if (!is_open(fd)) return -ebadf;
    if (minimum < 0 || minimum >= bound()) return -einval;
    const int target = free_number(minimum);
    if (target < 0) return -emfile;
    previous = place(target, entries[static_cast<std::size_t>(fd)].description, close_on_exec);
    return target;
}

long duplicate_to(int fd, int target, bool close_on_exec) {
    Handle previous;  // dropped after the lock: closing a host descriptor can take a while
    const std::lock_guard lock(mutex);
    if (target < 0 || target >= bound() || !is_open(fd)) return -ebadf;
    previous = place(target, entries[static_cast<std::size_t>(fd)].description, close_on_exec);
    return target;
}

long close(int fd) {
    Handle previous;
    const std::lock_guard lock(mutex);
    if (!is_open(fd)) return -ebadf;
    previous = std::exchange(entries[static_cast<std::size_t>(fd)], Entry{}).description;
    lowest_free = std::min(lowest_free, fd);
    return 0;
}

long close_on_exec(int fd) {
    const std::lock_guard lock(mutex);
    if (!is_open(fd)) return -ebadf;
    return entries[static_cast<std::size_t>(fd)].close_on_exec ? 1 : 0;
}

long set_close_on_exec(int fd, bool value) {
    const std::lock_guard lock(mutex);
    if (!is_open(fd)) return -ebadf;
    entries[static_cast<std::size_t>(fd)].close_on_exec = value;
    return 0;
}

void adopt_standard_descriptors() {
    raise_host_limit();
    for (int standard = 0; standard < 3; ++standard) {
        // A duplicate of the starter's: the guest may close or redirect its own, never the starter's.
        const int copy = ::fcntl(standard, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) continue;  // closed at the start: closed for the guest as well
        // Its status flags are the starter's too: the guest's O_NONBLOCK starts as theirs and stays apart.
        const bool nonblocking = (::fcntl(copy, F_GETFL) & O_NONBLOCK) != 0;
        const auto traits = traits_of(copy);
        auto description = std::make_shared<Description>(copy, traits.can_block, true, nonblocking,
                                                         Description::Kind::file, traits.pipe_end);
        Handle previous;
        const std::lock_guard lock(mutex);
        previous = place(standard, std::move(description), false);
    }
}

Limits limits() {
    const std::lock_guard lock(mutex);
    return current;
}

long set_limits(Limits requested) {
    const std::lock_guard lock(mutex);
    if (requested.soft > requested.hard) return -einval;
    if (requested.hard > current.hard) return -eperm;
    current = requested;
    return 0;
}

void reset() {
    std::vector<Entry> previous;
    const std::lock_guard lock(mutex);
    previous.swap(entries);
    lowest_free = 0;
    current = {limit, limit};
}

}  // namespace apkrt::descriptors
