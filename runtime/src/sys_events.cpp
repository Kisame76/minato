// eventfd, epoll, ppoll and pselect6 of the Linux syscall layer (ADR-0027, decisions 8 to 10), on the
// waits for descriptors of syscalls.h: every wait is a wait on host descriptors in the thread's kqueue,
// which a signal always ends.
#include "descriptors.h"
#include "syscalls.h"

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace apkrt::sys {
namespace {

using descriptors::Description;
using Kind = Description::Kind;
using PipeEnd = Description::PipeEnd;

// Linux's poll bits, which are epoll's as well (include/uapi/asm-generic/poll.h).
constexpr std::uint32_t pollin = 0x1, pollpri = 0x2, pollout = 0x4, pollerr = 0x8, pollhup = 0x10, pollnval = 0x20,
                        pollrdnorm = 0x40, pollrdband = 0x80, pollwrnorm = 0x100, pollwrband = 0x200;
constexpr std::uint32_t readable = pollin | pollrdnorm, writable = pollout | pollwrnorm;
/// What Linux reports for a file without a poll of its own: regular files, directories, /dev/null.
constexpr std::uint32_t always_ready = readable | writable;

bool valid(const timespec &time) {
    return time.tv_sec >= 0 && time.tv_nsec >= 0 && time.tv_nsec < 1'000'000'000;
}

// --- eventfd (decision 8) -----------------------------------------------------------------------------

/// An eventfd: the counter lives here. A host pipe shows when it can be read, holding bytes exactly
/// while the counter is not 0: every write adds one, and at `most_held` takes one out first, so that a
/// kqueue filter with EV_CLEAR (EPOLLET) sees every write and the write end always has room (EPOLLOUT).
/// A read that takes the counter to 0 empties the pipe.
class EventCounter final : public Description {
public:
    /// Takes both ends of the pipe, non-blocking.
    EventCounter(int read_end, int write_end, std::uint64_t initial, bool semaphore, bool nonblocking)
        : Description(read_end, false, false, nonblocking, Kind::eventfd), write_end_(write_end),
          semaphore_(semaphore) {
        static_cast<void>(add(initial));
    }
    ~EventCounter() override { ::close(write_end_); }

    int write_end() const { return write_end_; }

    /// What a read takes, as eventfd_read does: the counter, or 1 of it for a semaphore; false while
    /// it is 0.
    bool take(std::uint64_t &value) {
        const std::lock_guard lock(mutex_);
        if (counter_ == 0) return false;
        value = semaphore_ ? 1 : counter_;
        counter_ -= value;
        if (counter_ == 0) empty_the_pipe();
        return true;
    }

    /// Adds `value`, as eventfd_write does; false when the counter would reach 2^64 - 1.
    bool add(std::uint64_t value) {
        const std::lock_guard lock(mutex_);
        if (UINT64_MAX - counter_ <= value) return false;
        counter_ += value;
        if (counter_ != 0) hold_one_more();
        return true;
    }

    /// What poll reports, as eventfd_poll does: POLLIN while the counter is not 0, POLLOUT while a write
    /// of 1 fits.
    std::uint32_t poll_bits() {
        const std::lock_guard lock(mutex_);
        return (counter_ != 0 ? pollin : 0) | (counter_ < UINT64_MAX - 1 ? pollout : 0);
    }

private:
    static constexpr int most_held = 1024;

    // Both under mutex_. A non-blocking call on a Darwin pipe can still answer EINTR while it waits for
    // the pipe's lock (sys_files.cpp): it is made again.
    void hold_one_more() {
        char byte = 0;
        ssize_t moved;
        if (held_ >= most_held) {  // one out first: the byte that comes in is an arrival all the same
            do {
                moved = ::read(host(), &byte, 1);
            } while (moved < 0 && errno == EINTR);
            if (moved == 1) --held_;
        }
        do {
            moved = ::write(write_end_, &byte, 1);
        } while (moved < 0 && errno == EINTR);
        if (moved == 1) ++held_;
    }

    void empty_the_pipe() {
        char bytes[most_held];
        while (held_ > 0) {
            const ssize_t got = ::read(host(), bytes, static_cast<std::size_t>(held_));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            held_ -= static_cast<int>(got);
        }
        held_ = 0;
    }

    std::mutex mutex_;
    std::uint64_t counter_ = 0;  // guarded by mutex_
    int held_ = 0;               // the bytes in the pipe, guarded by mutex_
    const int write_end_;
    const bool semaphore_;
};

/// An eventfd's read of `vectors`: eight bytes, spread over the vectors as they come.
long read_count(EventCounter &counter, const iovec *vectors, int count) {
    std::size_t room = 0;
    for (int index = 0; index < count; ++index) room += vectors[index].iov_len;
    if (room < sizeof(std::uint64_t)) return -linux_errno::einval;
    std::uint64_t value = 0;
    const auto taken = [&] { return counter.take(value); };
    if (counter.nonblocking()) {
        if (!taken()) return -linux_errno::eagain;
    } else {
        Watched watched{counter.host(), EVFILT_READ};
        if (wait_for_descriptors(signals::current(), &watched, 1, taken, no_deadline) != WaitEnd::done) {
            return -linux_errno::eintr;
        }
    }
    const auto *bytes = reinterpret_cast<const char *>(&value);
    std::size_t copied = 0;
    for (int index = 0; copied < sizeof value; ++index) {
        const std::size_t part = std::min(vectors[index].iov_len, sizeof value - copied);
        std::memcpy(vectors[index].iov_base, bytes + copied, part);
        copied += part;
    }
    return sizeof value;
}

/// An eventfd's write of one buffer, as eventfd_write does. A write that would take the counter to
/// 2^64 - 1 answers EAGAIN even without O_NONBLOCK, where Linux waits: no program comes close.
long write_count(EventCounter &counter, const void *buffer, std::size_t size) {
    std::uint64_t value;
    if (size < sizeof value) return -linux_errno::einval;
    std::memcpy(&value, buffer, sizeof value);
    if (value == UINT64_MAX) return -linux_errno::einval;
    return counter.add(value) ? static_cast<long>(sizeof value) : -linux_errno::eagain;
}

// --- Readiness (decisions 9 and 10) -------------------------------------------------------------------

/// Whether Linux can poll it: an epoll_ctl on anything else answers EPERM, and poll and select report it
/// ready for reading and writing at once.
bool pollable(const Description &description) {
    return description.kind() != Kind::file || description.can_block();
}

/// A kqueue filter that shows when a description may be ready.
struct Filter {
    int host = -1;
    std::int16_t kind = 0;  // EVFILT_READ or EVFILT_WRITE
};

/// The filters, two at most, that show when `description`, which is pollable, may have one of `wanted`
/// (poll bits). Each fires only for something it reports among them, so that a level-triggered filter
/// cannot keep a wait awake for nothing.
int filters_for(const Description &description, std::uint32_t wanted, Filter (&filters)[2]) {
    int count = 0;
    const auto add = [&](int host, std::int16_t kind) { filters[count++] = {host, kind}; };
    const int host = description.host();
    switch (description.kind()) {
        case Kind::eventfd:
            if (wanted & pollin) add(host, EVFILT_READ);
            if (wanted & pollout) add(static_cast<const EventCounter &>(description).write_end(), EVFILT_WRITE);
            return count;
        case Kind::epoll:
            if (wanted & readable) add(host, EVFILT_READ);
            return count;
        case Kind::file: break;
    }
    // The filter of the other direction of a pipe's end fires only when the other end goes (measured on
    // macOS 27): the EPOLLHUP and EPOLLERR that Linux always reports, without the data or the room.
    switch (description.pipe_end()) {
        case PipeEnd::reading:
            if (wanted & readable) add(host, EVFILT_READ);
            else if (wanted & pollhup) add(host, EVFILT_WRITE);
            break;
        case PipeEnd::writing:
            if (wanted & writable) add(host, EVFILT_WRITE);
            else if (wanted & pollerr) add(host, EVFILT_READ);
            break;
        case PipeEnd::none:
            if (wanted & readable) add(host, EVFILT_READ);
            if (wanted & writable) add(host, EVFILT_WRITE);
            break;
    }
    return count;
}

/// The poll bits that a filter of `description` stands for when it fires, as Linux's poll of the file
/// tells them: `end` the kqueue's EV_EOF, `data` what there is to read (-1 unknown). A filter the kqueue
/// could not tell about (syscalls.h, Watched) counts as ready, but the other end as still there.
std::uint32_t bits_of(Description &description, std::int16_t filter, bool end, std::int64_t data) {
    const bool read_filter = filter == EVFILT_READ;
    switch (description.kind()) {
        // EPOLLOUT stays true (decision 8): the write end of the pipe always has room.
        case Kind::eventfd: return static_cast<EventCounter &>(description).poll_bits() | pollout;
        case Kind::epoll: return readable;
        case Kind::file: break;
    }
    const std::uint32_t hangup = end ? pollhup : 0, error = end ? pollerr : 0;
    switch (description.pipe_end()) {
        case PipeEnd::reading: return read_filter ? (data != 0 ? readable : 0) | hangup : hangup;
        case PipeEnd::writing: return read_filter ? error : writable | error;
        case PipeEnd::none: break;
    }
    return (read_filter ? readable : writable) | hangup;
}

// --- epoll (decision 9) --------------------------------------------------------------------------------

// epoll_ctl's operations and an interest's flags (include/uapi/linux/eventpoll.h).
constexpr int epoll_ctl_add = 1, epoll_ctl_del = 2, epoll_ctl_mod = 3;
constexpr std::uint32_t epollexclusive = 1u << 28, epollwakeup = 1u << 29, epolloneshot = 1u << 30,
                        epollet = 1u << 31;
/// What an interest with EPOLLEXCLUSIVE may ask for besides it.
constexpr std::uint32_t exclusive_bits = pollin | pollout | pollerr | pollhup | epollwakeup | epollet | epollexclusive;

/// struct epoll_event on arm64: 16 bytes, the data at offset 8 (only x86_64 packs it).
struct LinuxEpollEvent {
    std::uint32_t events;
    std::uint64_t data;
};
static_assert(sizeof(LinuxEpollEvent) == 16);

/// Serializes the additions of an epoll to an epoll, so that no two of them make a loop together.
std::mutex nesting;

/// An epoll instance: a host kqueue with filters for the files its interests watch. An interest belongs
/// to a number and its open file, as on Linux, and goes with the file's last number: its filters go with
/// the host descriptor, and the interest with the file, which the instance does not hold. Each interest
/// has filters of its own (EV_UDATA_SPECIFIC, with its id as udata), as each has its item on Linux: two
/// numbers of one file are two interests, in modes of their own.
class Epoll final : public Description {
public:
    explicit Epoll(int queue) : Description(queue, false, false, false, Kind::epoll) {}

    /// epoll_ctl's EPOLL_CTL_ADD, _MOD and _DEL of `target` under `number`, once the syscall has checked
    /// what Linux checks first.
    long control(int operation, int number, const descriptors::Handle &target, std::uint32_t events,
                 std::uint64_t data);

    /// Up to `maximum` events that are ready now, without waiting.
    int collect(LinuxEpollEvent *events, int maximum);

    /// The epolls among the files it watches.
    std::vector<descriptors::Handle> nested() {
        std::vector<descriptors::Handle> found;
        const std::lock_guard lock(mutex_);
        for (const auto &[id, interest] : interests_) {
            auto description = interest->description.lock();
            if (description && description->kind() == Kind::epoll) found.push_back(std::move(description));
        }
        return found;
    }

private:
    struct Interest {
        std::weak_ptr<Description> description;
        const Description *identity;  // compared only while `description` lives
        int number;
        std::uint32_t events;
        std::uint64_t data;
        bool armed = true;  // an EPOLLONESHOT interest that reported is not, until EPOLL_CTL_MOD
        Filter filters[2];
        int filter_count = 0;  // its filters in the kqueue now
    };

    long apply(std::uint64_t id, Interest &interest, Description &description);

    std::mutex mutex_;
    // Keyed by the id that their filters carry in udata: a filter that fires for an interest that is
    // gone finds nothing.
    std::unordered_map<std::uint64_t, std::unique_ptr<Interest>> interests_;  // guarded by mutex_
    std::uint64_t next_id_ = 1;                                                // guarded by mutex_
};

/// Makes the filters of `interest` anew, if it is armed, with EV_CLEAR for EPOLLET: the kqueue looks at
/// the file at once, as Linux looks at it when an interest comes or changes. One that is not armed has
/// none. Linux's errno of a filter the kqueue refuses.
long Epoll::apply(std::uint64_t id, Interest &interest, Description &description) {
    Filter filters[2];
    const int count = interest.armed ? filters_for(description, interest.events | pollerr | pollhup, filters) : 0;
    void *const udata = reinterpret_cast<void *>(static_cast<std::uintptr_t>(id));
    struct kevent changes[4];
    int changed = 0;
    for (int index = 0; index < interest.filter_count; ++index) {
        EV_SET(&changes[changed++], interest.filters[index].host, interest.filters[index].kind,
               EV_DELETE | EV_RECEIPT | EV_UDATA_SPECIFIC, 0, 0, udata);
    }
    const auto flags = static_cast<std::uint16_t>(EV_ADD | EV_RECEIPT | EV_UDATA_SPECIFIC |
                                                  ((interest.events & epollet) != 0 ? EV_CLEAR : 0));
    for (int index = 0; index < count; ++index) {
        EV_SET(&changes[changed++], filters[index].host, filters[index].kind, flags, 0, 0, udata);
    }
    interest.filter_count = 0;
    if (changed == 0) return 0;
    // With EV_RECEIPT every change is answered, and no event is taken from the kqueue.
    struct kevent receipts[4];
    const int got = ::kevent(host(), changes, changed, receipts, changed, nullptr);
    if (got < 0) return -to_linux_errno(errno);
    long error = 0;
    for (int index = 0; index < got; ++index) {
        const struct kevent &receipt = receipts[index];
        if ((receipt.flags & EV_DELETE) != 0) continue;  // a filter that went with its file already
        if (receipt.data == 0) {
            interest.filters[interest.filter_count++] = {static_cast<int>(receipt.ident), receipt.filter};
        } else if (error == 0) {
            // A kqueue refuses a kqueue too deep inside kqueues, which Linux refuses as ELOOP.
            error = description.kind() == Kind::epoll && receipt.data == EINVAL
                        ? -linux_errno::eloop
                        : -to_linux_errno(static_cast<int>(receipt.data));
        }
    }
    return error;
}

long Epoll::control(int operation, int number, const descriptors::Handle &target, std::uint32_t events,
                    std::uint64_t data) {
    std::unique_lock<std::mutex> nesting_lock;
    if (operation == epoll_ctl_add && target->kind() == Kind::epoll) {
        nesting_lock = std::unique_lock(nesting);
        // Linux's ep_loop_check: a loop, or more epolls below the new one than EP_MAX_NESTS (4) allows.
        std::set<const Description *> visited;
        const auto loops = [&](const auto &self, Epoll &from, int depth) -> bool {
            visited.insert(&from);
            for (const auto &inner : from.nested()) {
                if (visited.contains(inner.get())) continue;
                if (inner.get() == this || depth > 4) return true;
                if (self(self, static_cast<Epoll &>(*inner), depth + 1)) return true;
            }
            return false;
        };
        if (loops(loops, static_cast<Epoll &>(*target), 0)) return -linux_errno::eloop;
    }
    const std::lock_guard lock(mutex_);
    // Interests whose file is gone: their filters went with its host descriptor. A new file can have the
    // address of one of them, which is why an interest counts only while its file lives.
    std::erase_if(interests_, [](const auto &entry) { return entry.second->description.expired(); });
    const auto found = std::find_if(interests_.begin(), interests_.end(), [&](const auto &entry) {
        return entry.second->identity == target.get() && entry.second->number == number &&
               !entry.second->description.expired();
    });
    switch (operation) {
        case epoll_ctl_add: {
            if (found != interests_.end()) return -linux_errno::eexist;
            const std::uint64_t id = next_id_++;
            auto made = std::make_unique<Interest>();
            made->description = target;
            made->identity = target.get();
            made->number = number;
            made->events = events;
            made->data = data;
            Interest &interest = *interests_.emplace(id, std::move(made)).first->second;
            const long error = apply(id, interest, *target);
            if (error == 0) return 0;
            interest.armed = false;
            apply(id, interest, *target);  // takes out what came in
            interests_.erase(id);
            return error;
        }
        case epoll_ctl_mod: {
            if (found == interests_.end()) return -linux_errno::enoent;
            Interest &interest = *found->second;
            if ((interest.events & epollexclusive) != 0) return -linux_errno::einval;
            interest.events = events;
            interest.data = data;
            interest.armed = true;
            return apply(found->first, interest, *target);
        }
        case epoll_ctl_del: {
            if (found == interests_.end()) return -linux_errno::enoent;
            found->second->armed = false;
            apply(found->first, *found->second, *target);
            interests_.erase(found);
            return 0;
        }
        default:
            return -linux_errno::einval;
    }
}

int Epoll::collect(LinuxEpollEvent *events, int maximum) {
    const std::lock_guard lock(mutex_);
    // As many as there is room for, 1,024 at most: each interest's filters fire for it alone, so no
    // event of one fits in no more than its kevents.
    constexpr int batch = 64;
    struct kevent few[batch];
    std::vector<struct kevent> many;
    const int room = std::min(maximum, 1024);
    struct kevent *fired = few;
    if (room > batch) {
        many.resize(static_cast<std::size_t>(room));
        fired = many.data();
    }
    const timespec now{0, 0};
    const int got = ::kevent(host(), nullptr, 0, fired, room, &now);
    // The bits of each interest, from both of its filters when both fired.
    struct Ready {
        std::uint64_t id;
        descriptors::Handle description;
        std::uint32_t bits;
    };
    std::vector<Ready> ready;
    for (int index = 0; index < got; ++index) {
        const auto id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fired[index].udata));
        const auto found = interests_.find(id);
        if (found == interests_.end()) continue;
        auto description = found->second->description.lock();
        if (!description) continue;  // the file went meanwhile
        const auto bits =
            bits_of(*description, fired[index].filter, (fired[index].flags & EV_EOF) != 0, fired[index].data);
        const auto same = std::find_if(ready.begin(), ready.end(), [&](const Ready &entry) { return entry.id == id; });
        if (same != ready.end()) {
            same->bits |= bits;
        } else {
            ready.push_back({id, std::move(description), bits});
        }
    }
    int count = 0;
    for (auto &[id, description, bits] : ready) {
        Interest &interest = *interests_.at(id);
        const std::uint32_t reported = bits & (interest.events | pollerr | pollhup);
        if (reported == 0) continue;  // an interest that is not armed has no filters to fire
        events[count++] = {reported, interest.data};
        if ((interest.events & epolloneshot) != 0) {
            // Disarmed until EPOLL_CTL_MOD: its filters go.
            interest.armed = false;
            apply(id, interest, *description);
        }
    }
    return count;
}

/// epoll_pwait and epoll_pwait2 once their timeout is a deadline.
long wait_on_epoll(int epoll, std::uint64_t events_address, int maximum, std::int64_t deadline, std::uint64_t mask,
                   std::uint64_t mask_size) {
    if (mask != 0 && mask_size != sizeof(std::uint64_t)) return -linux_errno::einval;
    constexpr int most_events = INT_MAX / static_cast<int>(sizeof(LinuxEpollEvent));
    if (maximum <= 0 || maximum > most_events) return -linux_errno::einval;
    if (events_address == 0) return -linux_errno::efault;
    const auto description = descriptors::get(epoll);
    if (!description) return -linux_errno::ebadf;
    if (description->kind() != Kind::epoll) return -linux_errno::einval;
    auto &instance = static_cast<Epoll &>(*description);
    auto *const events = as_pointer<LinuxEpollEvent>(events_address);
    auto &state = signals::current();
    int got = 0;
    // The thread waits in its own kqueue, which watches the instance's: two threads that wait on the same
    // instance cannot take each other's wake-ups.
    Watched watched{instance.host(), EVFILT_READ};
    WaitMask wait_mask(state, mask);
    const auto end = wait_for_descriptors(
        state, &watched, 1, [&] { return (got = instance.collect(events, maximum)) > 0; }, deadline);
    wait_mask.end(end);
    if (end == WaitEnd::done) return got;
    return end == WaitEnd::timeout ? 0 : -linux_errno::eintr;
}

// --- ppoll and pselect6 (decision 10) ---------------------------------------------------------------

/// The descriptors of one ppoll or pselect6, with the filters that show when they are ready; a filter
/// that two of them share is watched once.
class Polled {
public:
    /// Takes `fd`, which wants one of `wanted` (poll bits), and returns its index; -1 when it is not open.
    int add(int fd, std::uint32_t wanted) {
        auto description = descriptors::get(fd);
        if (!description) return -1;
        Entry entry{std::move(description)};
        // An eventfd's POLLOUT is its counter's, which no filter has to wait for.
        if (entry.description->kind() == Kind::eventfd) wanted &= ~pollout;
        Filter filters[2];
        const int count = pollable(*entry.description) ? filters_for(*entry.description, wanted, filters) : 0;
        for (int index = 0; index < count; ++index) {
            const auto key = (static_cast<std::uint64_t>(filters[index].host) << 16) |
                             static_cast<std::uint16_t>(filters[index].kind);
            const auto [place, added] = by_filter_.emplace(key, static_cast<int>(watched_.size()));
            if (added) watched_.push_back({filters[index].host, filters[index].kind});
            entry.filters[index] = place->second;
        }
        entries_.push_back(std::move(entry));
        return static_cast<int>(entries_.size()) - 1;
    }

    /// What entry `index` reports now, as Linux's poll of the file tells it.
    std::uint32_t bits(int index) {
        const Entry &entry = entries_[static_cast<std::size_t>(index)];
        Description &description = *entry.description;
        std::uint32_t bits = 0;
        switch (description.kind()) {
            case Kind::eventfd: return static_cast<EventCounter &>(description).poll_bits();
            case Kind::epoll: break;  // its kqueue is readable while it has events, those that wait too
            case Kind::file:
                if (!description.can_block()) return always_ready;
                break;
        }
        for (const int filter : entry.filters) {
            if (filter < 0) continue;
            const Watched &watched = watched_[static_cast<std::size_t>(filter)];
            if (watched.ready) bits |= bits_of(description, watched.filter, watched.end, watched.available);
        }
        return bits;
    }

    Watched *watched() { return watched_.data(); }
    int watched_count() const { return static_cast<int>(watched_.size()); }

private:
    struct Entry {
        descriptors::Handle description;
        int filters[2] = {-1, -1};  // indexes into watched_
    };
    std::vector<Entry> entries_;
    std::vector<Watched> watched_;
    std::unordered_map<std::uint64_t, int> by_filter_;
};

/// Waits until `look()` finds something ready, `deadline` passes or a signal comes, with the thread's
/// mask replaced by the one at `mask` (if not 0) meanwhile. The first look asks the kqueue.
template <typename Look>
WaitEnd wait_for_polled(Polled &polled, Look look, std::int64_t deadline, std::uint64_t mask) {
    auto &state = signals::current();
    bool probed = false;
    const auto done = [&] {
        if (!probed && polled.watched_count() > 0) probe_descriptors(state, polled.watched(), polled.watched_count());
        probed = true;
        return look();
    };
    WaitMask wait_mask(state, mask);
    auto end = wait_for_descriptors(state, polled.watched(), polled.watched_count(), done, deadline);
    // Linux's do_poll and do_select look for signals before the time: with nothing ready, a signal the
    // thread takes, under the call's mask, ends even a wait whose time is up, or that had none.
    if (end == WaitEnd::timeout && (state.pending.load() & ~state.mask.load()) != 0) end = WaitEnd::signal;
    wait_mask.end(end);
    return end;
}

/// A timeout of ppoll or pselect6: its deadline, or no_deadline for none; nullopt when it is not valid.
std::optional<std::int64_t> deadline_of(std::uint64_t address) {
    if (address == 0) return no_deadline;
    const auto &timeout = *as_pointer<const timespec>(address);
    if (!valid(timeout)) return std::nullopt;
    return deadline_after(nanoseconds_of(timeout));
}

/// Writes the time left until `deadline` back to the timeout at `address`, as Linux's ppoll and
/// pselect6 do: 0 when it ran out, and nothing for a timeout of 0.
void write_time_left(std::uint64_t address, std::int64_t deadline) {
    if (address == 0) return;
    auto &timeout = *as_pointer<timespec>(address);
    if (timeout.tv_sec == 0 && timeout.tv_nsec == 0) return;
    const std::int64_t left = std::max<std::int64_t>(deadline - monotonic_now(), 0);
    timeout = {static_cast<time_t>(left / 1'000'000'000), static_cast<long>(left % 1'000'000'000)};
}

/// struct pollfd, the same on Linux and Darwin.
struct LinuxPollfd {
    std::int32_t fd;
    std::int16_t events;
    std::int16_t revents;
};

long poll_descriptors(std::uint64_t address, std::uint64_t count, std::int64_t deadline, std::uint64_t mask) {
    if (count > descriptors::limits().soft) return -linux_errno::einval;
    if (count > 0 && address == 0) return -linux_errno::efault;
    auto *const fds = as_pointer<LinuxPollfd>(address);
    Polled polled;
    std::vector<int> entry_of(count, -2);  // -2: a negative number, which Linux passes over; -1: not open
    std::vector<std::uint32_t> asked(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        if (fds[index].fd < 0) continue;
        asked[index] = static_cast<std::uint16_t>(fds[index].events) | pollerr | pollhup;
        entry_of[index] = polled.add(fds[index].fd, asked[index]);
    }
    std::vector<std::uint16_t> revents(count, 0);
    long ready = 0;
    const auto look = [&] {
        ready = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const int entry = entry_of[index];
            const std::uint32_t bits = entry == -1 ? pollnval : entry >= 0 ? polled.bits(entry) & asked[index] : 0;
            revents[index] = static_cast<std::uint16_t>(bits);
            if (bits != 0) ++ready;
        }
        return ready > 0;
    };
    const auto end = wait_for_polled(polled, look, deadline, mask);
    for (std::size_t index = 0; index < count; ++index) fds[index].revents = static_cast<std::int16_t>(revents[index]);
    if (end == WaitEnd::done) return ready;
    return end == WaitEnd::timeout ? 0 : -linux_errno::eintr;
}

// What makes a descriptor ready in each set of select (fs/select.c: POLLIN_SET, POLLOUT_SET, POLLEX_SET).
constexpr std::uint32_t select_reading = readable | pollrdband | pollhup | pollerr;
constexpr std::uint32_t select_writing = writable | pollwrband | pollerr;
constexpr std::uint32_t select_exceptional = pollpri;

long select_descriptors(int count, std::uint64_t read_set, std::uint64_t write_set, std::uint64_t except_set,
                        std::int64_t deadline, std::uint64_t mask) {
    if (count < 0) return -linux_errno::einval;
    // Linux looks no further than its table's size, and leaves the bits of higher numbers alone.
    count = std::min(count, descriptors::capacity());
    const auto words = static_cast<std::size_t>((count + 63) / 64);
    const std::uint64_t addresses[3] = {read_set, write_set, except_set};
    constexpr std::uint32_t conditions[3] = {select_reading, select_writing, select_exceptional};
    std::vector<std::uint64_t> sets[3];
    for (int set = 0; set < 3; ++set) {
        sets[set].assign(words, 0);
        if (addresses[set] != 0) std::memcpy(sets[set].data(), as_pointer<const void>(addresses[set]), words * 8);
    }
    struct Asked {
        int fd;
        int entry;
        std::uint32_t wanted[3];  // what counts in each set, or 0
    };
    std::vector<Asked> asked;
    Polled polled;
    for (int fd = 0; fd < count; ++fd) {
        const auto word = static_cast<std::size_t>(fd / 64);
        const std::uint64_t bit = std::uint64_t{1} << (fd % 64);
        Asked entry{fd, -1, {}};
        std::uint32_t wanted = 0;
        for (int set = 0; set < 3; ++set) {
            if ((sets[set][word] & bit) == 0) continue;
            entry.wanted[set] = conditions[set];
            wanted |= conditions[set];
        }
        if (wanted == 0) continue;
        entry.entry = polled.add(fd, wanted);
        if (entry.entry < 0) return -linux_errno::ebadf;
        asked.push_back(entry);
    }
    std::vector<std::uint64_t> results[3];
    long ready = 0;
    const auto look = [&] {
        ready = 0;
        for (auto &result : results) result.assign(words, 0);
        for (const auto &entry : asked) {
            const std::uint32_t bits = polled.bits(entry.entry);
            for (int set = 0; set < 3; ++set) {
                if ((bits & entry.wanted[set]) == 0) continue;
                results[set][static_cast<std::size_t>(entry.fd / 64)] |= std::uint64_t{1} << (entry.fd % 64);
                ++ready;
            }
        }
        return ready > 0;
    };
    const auto end = wait_for_polled(polled, look, deadline, mask);
    // After a signal, Linux leaves the sets as they were.
    if (end == WaitEnd::signal) return -linux_errno::eintr;
    for (int set = 0; set < 3; ++set) {
        if (addresses[set] != 0) std::memcpy(as_pointer<void>(addresses[set]), results[set].data(), words * 8);
    }
    return end == WaitEnd::done ? ready : 0;
}

}  // namespace

long sys_eventfd2(unsigned initial, int flags) {
    constexpr int efd_semaphore = 1, efd_nonblock = 04000, efd_cloexec = 02000000;
    if (flags & ~(efd_semaphore | efd_nonblock | efd_cloexec)) return -linux_errno::einval;
    int ends[2];
    if (::pipe(ends) != 0) return -to_linux_errno(errno);
    for (const int end : ends) {
        ::fcntl(end, F_SETFD, FD_CLOEXEC);
        ::fcntl(end, F_SETFL, O_NONBLOCK);
    }
    auto counter = std::make_shared<EventCounter>(ends[0], ends[1], initial, (flags & efd_semaphore) != 0,
                                                  (flags & efd_nonblock) != 0);
    return descriptors::add(std::move(counter), (flags & efd_cloexec) != 0);
}

long transfer_count(descriptors::Description &description, bool writing, const iovec *vectors, int count) {
    auto &counter = static_cast<EventCounter &>(description);
    if (!writing) return read_count(counter, vectors, count);
    // An eventfd has no write_iter: as for every such file, Linux makes each vector a write of its own and
    // stops at the first that fails or is short.
    long written = 0;
    for (int index = 0; index < count; ++index) {
        const long result = write_count(counter, vectors[index].iov_base, vectors[index].iov_len);
        if (result < 0) return written > 0 ? written : result;
        written += result;
        if (static_cast<std::size_t>(result) != vectors[index].iov_len) break;
    }
    return written;
}

long sys_epoll_create1(int flags) {
    constexpr int epoll_cloexec = 02000000;
    if (flags & ~epoll_cloexec) return -linux_errno::einval;
    const int queue = ::kqueue();
    if (queue < 0) return -to_linux_errno(errno);
    ::fcntl(queue, F_SETFD, FD_CLOEXEC);
    return descriptors::add(std::make_shared<Epoll>(queue), (flags & epoll_cloexec) != 0);
}

long sys_epoll_ctl(int epoll, int operation, int fd, std::uint64_t event) {
    // Linux copies the event first, for every operation but EPOLL_CTL_DEL, then looks at the descriptors
    // in this order.
    LinuxEpollEvent given{};
    if (operation != epoll_ctl_del) {
        if (event == 0) return -linux_errno::efault;
        given = *as_pointer<const LinuxEpollEvent>(event);
    }
    const auto instance = descriptors::get(epoll);
    if (!instance) return -linux_errno::ebadf;
    const auto target = descriptors::get(fd);
    if (!target) return -linux_errno::ebadf;
    if (!pollable(*target)) return -linux_errno::eperm;
    if (instance == target || instance->kind() != Kind::epoll) return -linux_errno::einval;
    // EPOLLEXCLUSIVE, where Linux takes it; it changes nothing here: every waiting thread wakes up.
    if (operation != epoll_ctl_del && (given.events & epollexclusive) != 0) {
        if (operation == epoll_ctl_mod) return -linux_errno::einval;
        if (operation == epoll_ctl_add &&
            (target->kind() == Kind::epoll || (given.events & ~exclusive_bits) != 0)) {
            return -linux_errno::einval;
        }
    }
    return static_cast<Epoll &>(*instance).control(operation, fd, target, given.events, given.data);
}

long sys_epoll_pwait(int epoll, std::uint64_t events, int maximum, int timeout, std::uint64_t mask,
                     std::uint64_t mask_size) {
    const std::int64_t deadline = timeout < 0 ? no_deadline : deadline_after(std::int64_t{timeout} * 1'000'000);
    return wait_on_epoll(epoll, events, maximum, deadline, mask, mask_size);
}

long sys_epoll_pwait2(int epoll, std::uint64_t events, int maximum, std::uint64_t timeout, std::uint64_t mask,
                      std::uint64_t mask_size) {
    const auto deadline = deadline_of(timeout);
    if (!deadline) return -linux_errno::einval;
    return wait_on_epoll(epoll, events, maximum, *deadline, mask, mask_size);
}

long sys_ppoll(std::uint64_t fds, std::uint64_t count, std::uint64_t timeout, std::uint64_t mask,
               std::uint64_t mask_size) {
    const auto deadline = deadline_of(timeout);
    if (!deadline) return -linux_errno::einval;
    if (mask != 0 && mask_size != sizeof(std::uint64_t)) return -linux_errno::einval;
    const long result = poll_descriptors(fds, count, *deadline, mask);
    write_time_left(timeout, *deadline);
    return result;
}

long sys_pselect6(int count, std::uint64_t read_set, std::uint64_t write_set, std::uint64_t except_set,
                  std::uint64_t timeout, std::uint64_t mask) {
    const auto deadline = deadline_of(timeout);
    if (!deadline) return -linux_errno::einval;
    std::uint64_t mask_address = 0;
    if (mask != 0) {
        const auto *pack = as_pointer<const std::uint64_t>(mask);  // the mask's address and its size
        if (pack[0] != 0 && pack[1] != sizeof(std::uint64_t)) return -linux_errno::einval;
        mask_address = pack[0];
    }
    const long result = select_descriptors(count, read_set, write_set, except_set, *deadline, mask_address);
    write_time_left(timeout, *deadline);
    return result;
}

}  // namespace apkrt::sys
