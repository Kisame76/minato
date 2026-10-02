// The vsync and input channels (ADR-0036, decision 1). A channel is a description of the guest's table
// (Kind::channel) whose host descriptor is the read end of a pipe of its own. The pipe holds bytes exactly while
// the channel holds something unread, one for each arrival (at most most_held), so that a kqueue filter with
// EV_CLEAR (EPOLLET) sees each; the read that leaves nothing unread empties it. The records stay here. The
// host's posts and the guest's syscalls change them under `lock`, which none holds for longer than a copy or a
// merge of MOVEs, and the host never waits for the guest.
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
#include <deque>
#include <map>
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
std::int64_t last_time = 0;          // the last tick's time, under `lock`
std::int64_t period = 0;             // the display's, once a tick came, under `lock`
bool period_known = false;           // whether one came

// ADR-0041 decision 11: the least time from a drawable with a presentation time to the screen on the iPhone, as the
// short sitting of 2026-10-02 measured it. No frame timeline offers a present sooner after its frame time.
constexpr std::int64_t least_delay = 33'200'000;

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

/// A tick with its frame timelines. Timeline 0 presents at the first refresh of the tick's grid,
/// target_time + m × length, at least the least delay after the frame time, and never before
/// target_time + length, so that its deadline is never the frame time (ADR-0036). The others follow a period
/// apart, each deadline a period before its present.
apkrt_vsync make_tick(std::int64_t number, std::int64_t time, std::int64_t target_time, std::int64_t length) {
    apkrt_vsync tick{};
    tick.time = time;
    tick.target_time = target_time;
    tick.period = length;
    std::int64_t first = target_time + length;
    if (const std::int64_t short_by = least_delay - (first - time); length > 0 && short_by > 0) {
        first += (short_by + length - 1) / length * length;
    }
    for (std::int64_t k = 0; k < APKRT_FRAME_TIMELINES; ++k) {
        tick.timelines[k] = {number + k, first + k * length, first + (k - 1) * length};
    }
    return tick;
}

/// The channel of the kind `of` behind the guest's `fd`, or null.
template <typename Kind>
std::shared_ptr<Kind> channel_of(int fd, Channel::Of of) {
    auto description = descriptors::get(fd);
    if (!description || description->kind() != Description::Kind::channel ||
        static_cast<Channel &>(*description).of() != of) {
        return nullptr;
    }
    return std::static_pointer_cast<Kind>(std::move(description));
}

/// A channel's pipe, both ends close-on-exec and non-blocking: 0, or the error as a negative Linux errno.
long open_pipe(int (&ends)[2]) {
    if (::pipe(ends) != 0) return -sys::to_linux_errno(errno);
    for (const int end : ends) {
        ::fcntl(end, F_SETFD, FD_CLOEXEC);
        ::fcntl(end, F_SETFL, O_NONBLOCK);
    }
    ::fcntl(ends[1], F_SETNOSIGPIPE, 1);
    return 0;
}

// --- The input channel ---------------------------------------------------------------------------------

constexpr std::size_t most_events = 256;
// Beyond a full channel: the ends of the open gesture, whose pointers can end one by one.
constexpr std::size_t most_ends = APKRT_MOTION_POINTERS + 1;
// AMOTION_EVENT_ACTION_*, and Android's MAX_POINTER_ID.
constexpr std::int32_t action_down = 0, action_up = 1, action_move = 2, action_cancel = 3, action_pointer_down = 5,
                       action_pointer_up = 6, index_shift = 8, most_id = 31;

class InputChannel;

/// The host's side of an input channel, from apkrt_input_add on. It stays after its queue closed, so that
/// its ID stays used.
struct InputQueue {
    std::deque<apkrt_motion_event> events;
    InputChannel *channel = nullptr;  // the channel of the guest's descriptor, while that is open
    bool connected = false;           // a connect is made or under way
    bool ended = false;               // the descriptor was closed
    bool refusing = false;            // the rest of the last gesture the channel could not take
    std::int64_t refused = 0;         // that gesture's down time
};

std::map<std::uint64_t, InputQueue> inputs;  // by ID, under `lock`; never erased
std::uint64_t last_input = 0;                // the last ID given, under `lock`

class InputChannel final : public Channel {
public:
    InputChannel(int read_end, int write_end, InputQueue &queue)
        : Channel(read_end, write_end, Of::input), queue_(queue) {}
    /// The channel ends, unless it is the one of a connect that failed. Its events are freed after the lock.
    ~InputChannel() override {
        std::deque<apkrt_motion_event> events;
        const std::lock_guard hold(lock);
        if (queue_.channel != this) return;
        queue_.channel = nullptr;
        queue_.ended = true;
        events.swap(queue_.events);
    }

    bool unread() const override { return !queue_.events.empty(); }

    InputQueue &queue() { return queue_; }

private:
    InputQueue &queue_;
};

/// Whether the host may post `action` with `pointers` pointers and `samples` samples, as Android's reader makes
/// the events: a MOVE, the only one with history; DOWN and UP of one pointer; CANCEL; POINTER_DOWN and
/// POINTER_UP of one of two or more, whose index they carry. Bits above the index make it 256 or more, a
/// negative action a negative index: neither passes.
bool sound_action(std::int32_t action, std::uint32_t pointers, std::uint32_t samples) {
    const std::int32_t index = action >> index_shift;
    switch (action & 0xff) {
        case action_move:
            return index == 0;
        case action_down:
        case action_up:
            return index == 0 && pointers == 1 && samples == 1;
        case action_cancel:
            return index == 0 && samples == 1;
        case action_pointer_down:
        case action_pointer_up:
            return static_cast<std::uint32_t>(index) < pointers && pointers >= 2 && samples == 1;
        default:
            return false;
    }
}

/// Fills `event` with a post's values, of more than APKRT_MOTION_SAMPLES samples the newest; false for an
/// event that is not sound.
bool make_event(apkrt_motion_event &event, std::int32_t action, std::int64_t down_time, std::uint32_t pointer_count,
                const std::int32_t *pointer_ids, std::uint32_t sample_count, const std::int64_t *sample_times,
                const apkrt_pointer_coords *coords) {
    if (pointer_ids == nullptr || sample_times == nullptr || coords == nullptr || pointer_count == 0 ||
        pointer_count > APKRT_MOTION_POINTERS || sample_count == 0 ||
        !sound_action(action, pointer_count, sample_count)) {
        return false;
    }
    for (std::uint32_t index = 0; index < pointer_count; ++index) {
        const std::int32_t id = pointer_ids[index];
        if (id < 0 || id > most_id || (index > 0 && id <= pointer_ids[index - 1])) return false;
    }
    event = {};
    event.action = action;
    event.pointer_count = pointer_count;
    event.down_time = down_time;
    const std::uint32_t first = sample_count > APKRT_MOTION_SAMPLES ? sample_count - APKRT_MOTION_SAMPLES : 0;
    event.sample_count = sample_count - first;
    std::copy_n(pointer_ids, pointer_count, event.pointer_ids);
    for (std::uint32_t sample = 0; sample < event.sample_count; ++sample) {
        event.sample_times[sample] = sample_times[first + sample];
        std::copy_n(coords + static_cast<std::size_t>(first + sample) * pointer_count, pointer_count,
                    event.coords[sample]);
    }
    return true;
}

/// Whether `later`, the event right after `earlier`, can take its samples: two MOVEs of the same pointers
/// and down time.
bool mergeable(const apkrt_motion_event &earlier, const apkrt_motion_event &later) {
    return earlier.action == action_move && later.action == action_move &&
           earlier.pointer_count == later.pointer_count && earlier.down_time == later.down_time &&
           std::equal(earlier.pointer_ids, earlier.pointer_ids + earlier.pointer_count, later.pointer_ids);
}

/// Gives `later` the samples of `earlier` before its own, of more than APKRT_MOTION_SAMPLES the newest.
void merge(const apkrt_motion_event &earlier, apkrt_motion_event &later) {
    apkrt_motion_event merged{};
    merged.action = later.action;
    merged.pointer_count = later.pointer_count;
    merged.down_time = later.down_time;
    std::copy_n(later.pointer_ids, APKRT_MOTION_POINTERS, merged.pointer_ids);
    const std::uint32_t total = earlier.sample_count + later.sample_count;
    merged.sample_count = std::min<std::uint32_t>(total, APKRT_MOTION_SAMPLES);
    for (std::uint32_t sample = 0; sample < merged.sample_count; ++sample) {
        const std::uint32_t from = total - merged.sample_count + sample;
        const apkrt_motion_event &source = from < earlier.sample_count ? earlier : later;
        const std::uint32_t at = from < earlier.sample_count ? from : from - earlier.sample_count;
        merged.sample_times[sample] = source.sample_times[at];
        std::copy_n(source.coords[at], APKRT_MOTION_POINTERS, merged.coords[sample]);
    }
    later = merged;
}

/// Makes room in a full channel for the MOVE `next`: of the pairs of neighbours that can merge, counting `next`
/// as the neighbour after the last event, the oldest merges into its later event. False when no pair can.
bool make_room(std::deque<apkrt_motion_event> &events, apkrt_motion_event &next) {
    for (std::size_t index = 0; index + 1 < events.size(); ++index) {
        if (mergeable(events[index], events[index + 1])) {
            merge(events[index], events[index + 1]);
            events.erase(events.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
    }
    if (events.empty() || !mergeable(events.back(), next)) return false;
    merge(events.back(), next);
    events.pop_back();
    return true;
}

/// The CANCEL that ends a gesture at its POINTER_DOWN `pointer_down`, which a full channel cannot take: the
/// pointers the guest has, where that event puts them.
apkrt_motion_event cancel_of(const apkrt_motion_event &pointer_down) {
    const auto added = static_cast<std::uint32_t>(pointer_down.action >> index_shift);
    apkrt_motion_event cancel{};
    cancel.action = action_cancel;
    cancel.pointer_count = pointer_down.pointer_count - 1;
    cancel.sample_count = 1;
    cancel.down_time = pointer_down.down_time;
    cancel.sample_times[0] = pointer_down.sample_times[0];
    for (std::uint32_t from = 0, to = 0; from < pointer_down.pointer_count; ++from) {
        if (from == added) continue;
        cancel.pointer_ids[to] = pointer_down.pointer_ids[from];
        cancel.coords[0][to++] = pointer_down.coords[0][from];
    }
    return cancel;
}

/// Whether `queue` takes `event`, and what it does to keep the guest's gestures whole, as Android's dispatcher
/// does: the rest of a gesture it could not take is refused, by its down time. A full channel merges a MOVE or
/// drops it; takes the ends of the open gesture beyond its 256 events, up to most_ends of them; refuses a DOWN
/// with the rest of its gesture; and ends the gesture at a POINTER_DOWN with a CANCEL of the pointers the guest
/// has, refusing its rest. Under `lock`.
bool takes(InputQueue &queue, apkrt_motion_event &event) {
    const std::int32_t kind = event.action & 0xff;
    if (queue.refusing && event.down_time == queue.refused) return false;
    if (queue.events.size() < most_events) return true;
    switch (kind) {
        case action_move:
            return make_room(queue.events, event);
        case action_up:
        case action_pointer_up:
        case action_cancel:
            return queue.events.size() < most_events + most_ends;
        default:
            queue.refusing = true;
            queue.refused = event.down_time;
            if (kind == action_pointer_down && queue.events.size() < most_events + most_ends) {
                queue.events.push_back(cancel_of(event));
                if (queue.channel != nullptr) queue.channel->arrived();
            }
            return false;
    }
}

}  // namespace

long vsync_connect() {
    int ends[2];
    if (const long error = open_pipe(ends); error != 0) return error;
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
    const auto channel = channel_of<VsyncChannel>(fd, Channel::Of::vsync);
    if (!channel) return -linux_errno::ebadf;
    const std::lock_guard hold(lock);
    channel->asked = true;
    return 0;
}

long vsync_read(int fd, std::uint64_t address) {
    const auto channel = channel_of<VsyncChannel>(fd, Channel::Of::vsync);
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

long input_connect(std::uint64_t id) {
    InputQueue *queue;
    {
        const std::lock_guard hold(lock);
        const auto found = inputs.find(id);
        if (found == inputs.end()) return -linux_errno::enoent;
        if (found->second.connected) return -linux_errno::ebusy;
        queue = &found->second;
        queue->connected = true;
    }
    // Until the descriptor is in the guest's table, a failure lets the queue connect again.
    const auto undo = [queue] {
        const std::lock_guard hold(lock);
        queue->channel = nullptr;
        queue->connected = false;
    };
    int ends[2];
    if (const long error = open_pipe(ends); error != 0) {
        undo();
        return error;
    }
    std::shared_ptr<InputChannel> channel;
    try {
        channel = std::make_shared<InputChannel>(ends[0], ends[1], *queue);
    } catch (const std::bad_alloc &) {
        ::close(ends[0]);
        ::close(ends[1]);
        undo();
        return -linux_errno::enomem;
    }
    {
        const std::lock_guard hold(lock);
        queue->channel = channel.get();
        if (!queue->events.empty()) channel->arrived();  // the events that waited for it
    }
    // A full table refuses it. `kept` holds the channel until undo has detached it, so that its destructor
    // leaves the queue alone.
    const auto kept = channel;
    const long fd = descriptors::add(std::move(channel), true);
    if (fd < 0) undo();
    return fd;
}

long input_read(int fd, std::uint64_t address) {
    const auto channel = channel_of<InputChannel>(fd, Channel::Of::input);
    if (!channel) return -linux_errno::ebadf;
    if (address == 0) return -linux_errno::efault;
    apkrt_motion_event event;
    {
        const std::lock_guard hold(lock);
        auto &events = channel->queue().events;
        if (events.empty()) return -linux_errno::eagain;
        event = events.front();
        events.pop_front();
        if (events.empty()) channel->emptied();
    }
    std::memcpy(reinterpret_cast<void *>(address), &event, sizeof event);
    return 0;
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
    last_time = time;
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

extern "C" int apkrt_vsync_grid(int64_t *time_ns, int64_t *period_ns) {
    const std::lock_guard hold(lock);
    if (ticks == 0) return -1;
    if (time_ns != nullptr) *time_ns = last_time;
    if (period_ns != nullptr) *period_ns = period;
    return 0;
}

extern "C" uint64_t apkrt_input_add(void) {
    try {
        const std::lock_guard hold(lock);
        inputs.try_emplace(last_input + 1);
        return ++last_input;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

extern "C" int apkrt_input_post(uint64_t id, int32_t action, int64_t down_time, uint32_t pointer_count,
                                const int32_t *pointer_ids, uint32_t sample_count, const int64_t *sample_times,
                                const struct apkrt_pointer_coords *coords) {
    apkrt_motion_event event;
    if (!make_event(event, action, down_time, pointer_count, pointer_ids, sample_count, sample_times, coords)) return 0;
    try {
        const std::lock_guard hold(lock);
        const auto found = inputs.find(id);
        if (found == inputs.end() || found->second.ended) return 0;
        InputQueue &queue = found->second;
        if (!takes(queue, event)) return 0;
        queue.events.push_back(event);
        if (queue.channel != nullptr) queue.channel->arrived();
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}
