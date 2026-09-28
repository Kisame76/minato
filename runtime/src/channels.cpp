// The vsync and input channels (ADR-0036, decision 1). A channel is a description of the guest's table
// (Kind::channel) whose host descriptor is the read end of a pipe of its own. The pipe holds bytes exactly while
// the channel holds something unread, one for each arrival (at most most_held), so that a kqueue filter with
// EV_CLEAR (EPOLLET) sees each; the read that leaves nothing unread empties it. The records stay here. The
// host's posts and the guest's syscalls change them under `lock`, which none holds for longer than a copy, and
// the host never waits for the guest.
#include "channels.h"

#include "APKRuntime/apkrt.h"
#include "session.h"
#include "syscalls.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace apkrt::channels {
namespace {

namespace linux_errno = sys::linux_errno;
using descriptors::Description;

std::mutex lock;

/// A channel's description, whose pipe shows what it holds unread.
class Channel : public Description {
public:
    enum class Of { vsync, input };

    Channel(int read_end, int write_end, Of of)
        : Description(read_end, false, false, false, Kind::channel), of_(of), write_end_(write_end) {}
    ~Channel() override { ::close(write_end_); }
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    Of of() const { return of_; }

    /// Under `lock`.
    virtual bool unread() const = 0;

    /// Something arrived, under `lock`: one more byte. A non-blocking call on a Darwin pipe can still answer
    /// EINTR while it waits for the pipe's lock: it is made again.
    void arrived() {
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

    /// Nothing is unread any more, under `lock`: the pipe empties.
    void emptied() {
        char bytes[most_held];
        while (held_ > 0) {
            const ssize_t got = ::read(host(), bytes, static_cast<std::size_t>(std::min(held_, most_held)));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            held_ -= static_cast<int>(got);
        }
        held_ = 0;
    }

private:
    static constexpr int most_held = 64;

    const Of of_;
    const int write_end_;
    int held_ = 0;  // the bytes in the pipe, under `lock`
};

// --- The vsync channel ---------------------------------------------------------------------------------

class VsyncChannel;
std::vector<VsyncChannel *> vsyncs;  // the open ones, under `lock`
std::int64_t ticks = 0;              // posted so far, under `lock`
std::int64_t period = 0;             // the display's, once a tick came, under `lock`
bool period_known = false;           // whether one came

class VsyncChannel final : public Channel {
public:
    VsyncChannel(int read_end, int write_end) : Channel(read_end, write_end, Of::vsync) {}
    ~VsyncChannel() override {
        const std::lock_guard hold(lock);
        if (const auto found = std::find(vsyncs.begin(), vsyncs.end(), this); found != vsyncs.end()) vsyncs.erase(found);
    }

    bool unread() const override { return tick_unread || period_unread; }

    // Under `lock`.
    bool asked = false;          // for the next tick (vsync_request)
    bool tick_unread = false;
    bool period_unread = false;
    apkrt_vsync tick{};          // the last tick it got
};

/// The period before the first tick: the session's refresh rate, 0 without a session. Under `lock`.
std::int64_t current_period() {
    if (period_known) return period;
    const int rate = session::refresh_rate();
    return rate > 0 ? 1'000'000'000 / rate : 0;
}

/// Whether `length` differs from `base`, both positive, by more than 1 %; written so that it cannot overflow.
bool differs(std::int64_t length, std::int64_t base) {
    return std::llabs(length - base) > base / 100;
}

apkrt_vsync make_tick(std::int64_t number, std::int64_t time, std::int64_t target_time, std::int64_t length) {
    apkrt_vsync tick{};
    tick.time = time;
    tick.target_time = target_time;
    tick.period = length;
    for (std::int64_t k = 0; k < APKRT_FRAME_TIMELINES; ++k) {
        tick.timelines[k] = {number + k, target_time + (k + 1) * length, target_time + k * length};
    }
    return tick;
}

/// The vsync channel behind the guest's `fd`, or null.
std::shared_ptr<VsyncChannel> vsync_of(int fd) {
    auto description = descriptors::get(fd);
    if (!description || description->kind() != Description::Kind::channel ||
        static_cast<Channel &>(*description).of() != Channel::Of::vsync) {
        return nullptr;
    }
    return std::static_pointer_cast<VsyncChannel>(std::move(description));
}

}  // namespace

long vsync_connect() {
    int ends[2];
    if (::pipe(ends) != 0) return -sys::to_linux_errno(errno);
    for (const int end : ends) {
        ::fcntl(end, F_SETFD, FD_CLOEXEC);
        ::fcntl(end, F_SETFL, O_NONBLOCK);
    }
    ::fcntl(ends[1], F_SETNOSIGPIPE, 1);
    std::shared_ptr<VsyncChannel> channel;
    try {
        channel = std::make_shared<VsyncChannel>(ends[0], ends[1]);
        const std::lock_guard hold(lock);
        vsyncs.push_back(channel.get());
    } catch (const std::bad_alloc &) {
        if (!channel) {
            ::close(ends[0]);
            ::close(ends[1]);
        }
        return -linux_errno::enomem;
    }
    // A full table refuses it: then it goes, and its destructor takes it out of `vsyncs`.
    return descriptors::add(std::move(channel), true);
}

long vsync_request(int fd) {
    const auto channel = vsync_of(fd);
    if (!channel) return -linux_errno::ebadf;
    const std::lock_guard hold(lock);
    channel->asked = true;
    return 0;
}

long vsync_read(int fd, std::uint64_t address) {
    const auto channel = vsync_of(fd);
    if (!channel) return -linux_errno::ebadf;
    if (address == 0) return -linux_errno::efault;
    apkrt_vsync vsync;
    long result;
    {
        const std::lock_guard hold(lock);
        vsync = channel->tick;
        vsync.period = current_period();
        result = channel->tick_unread ? 1 : 0;
        channel->tick_unread = channel->period_unread = false;
        channel->emptied();
    }
    std::memcpy(reinterpret_cast<void *>(address), &vsync, sizeof vsync);
    return result;
}

bool readable(Description &channel) {
    const std::lock_guard hold(lock);
    return static_cast<Channel &>(channel).unread();
}

std::size_t vsync_count() {
    const std::lock_guard hold(lock);
    return vsyncs.size();
}

}  // namespace apkrt::channels

using namespace apkrt::channels;

extern "C" void apkrt_vsync_post(int64_t time, int64_t target_time) {
    const std::lock_guard hold(lock);
    const std::int64_t number = ++ticks;
    const std::int64_t base = current_period(), length = target_time - time;
    const bool changed = length > 0 && (base <= 0 || differs(length, base));
    period = changed ? length : base;
    period_known = true;
    for (VsyncChannel *channel : vsyncs) {
        bool arrived = changed;
        channel->period_unread = channel->period_unread || changed;
        if (channel->asked) {
            channel->asked = false;
            channel->tick = make_tick(number, time, target_time, period);
            channel->tick_unread = arrived = true;
        }
        if (arrived) channel->arrived();
    }
}

// The input channel comes with ADR-0036's step b); until then there is none.
extern "C" uint64_t apkrt_input_add(void) {
    return 0;
}

extern "C" int apkrt_input_post(uint64_t, int32_t, int64_t, uint32_t, const int32_t *, uint32_t, const int64_t *,
                                const struct apkrt_pointer_coords *) {
    return 0;
}
