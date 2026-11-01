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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <vector>

namespace apkrt::channels {
namespace {

namespace linux_errno = sys::linux_errno;
using descriptors::Description;

std::mutex lock;

/// A channel's description, whose pipe shows what it holds unread.
class Channel : public Description {
public:
    enum class Of { vsync, input, webview };

    /// `can_block`: the guest reads the channel itself, and the runtime's read waits on its non-blocking host end
    /// (a WebView channel); the other channels have only their private syscalls.
    Channel(int read_end, int write_end, Of of, bool can_block = false)
        : Description(read_end, can_block, false, false, Kind::channel), of_(of), write_end_(write_end) {}
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

/// A tick with its frame timelines. Timeline 0 presents at target_time + length, 2 periods after the frame time at
/// every rate, so that its deadline is never the frame time (ADR-0036): graphics/host sends a present that near
/// without a presentation time, and such a present reaches the iPhone's screen 2 refreshes after its drawable
/// (ADR-0041 decision 11). The others follow a period apart, each deadline a period before its present.
apkrt_vsync make_tick(std::int64_t number, std::int64_t time, std::int64_t target_time, std::int64_t length) {
    apkrt_vsync tick{};
    tick.time = time;
    tick.target_time = target_time;
    tick.period = length;
    const std::int64_t first = target_time + length;
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

// The kind sits at byte 12 of every record, and each record fits the buffer input_read fills.
static_assert(offsetof(apkrt_motion_event, kind) == 12 && offsetof(apkrt_key_event, kind) == 12 &&
              offsetof(apkrt_text_event, kind) == 12 && offsetof(apkrt_device_event, kind) == 12 &&
              offsetof(apkrt_axes_event, kind) == 12);
static_assert(sizeof(apkrt_key_event) <= sizeof(apkrt_motion_event) &&
              sizeof(apkrt_text_event) <= sizeof(apkrt_motion_event) &&
              sizeof(apkrt_device_event) <= sizeof(apkrt_motion_event) &&
              sizeof(apkrt_axes_event) <= sizeof(apkrt_motion_event));

class InputChannel;

/// The host's side of an input channel, from apkrt_input_add on. It stays after its queue closed, so that
/// its ID stays used.
/// What the host's mouse has done to a channel (apkrt_input_post_mouse): the buttons held, whether it hovers over the
/// window and its last position, so that the records Android makes of a report follow from the reports before it.
struct MouseState {
    std::int32_t buttons = 0;
    bool hovering = false;
    std::int32_t device = 0, source = 0, tool = 0;  // of the last report, and its time
    std::int64_t time = 0;
    float x = 0, y = 0;
    std::vector<apkrt_axis_value> hover_extra;  // what the last hover record had beyond its position: the relative axes
};

struct InputQueue {
    std::deque<apkrt_motion_event> events;
    InputChannel *channel = nullptr;  // the channel of the guest's descriptor, while that is open
    bool connected = false;           // a connect is made or under way
    bool ended = false;               // the descriptor was closed
    bool refusing = false;            // the rest of the last gesture the channel could not take
    std::int64_t refused = 0;         // that gesture's down time
    MouseState mouse;                 // what apkrt_input_post_mouse has made of the host's mouse so far
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
    return earlier.kind == APKRT_INPUT_MOTION && later.kind == APKRT_INPUT_MOTION && earlier.action == action_move &&
           later.action == action_move &&
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
    if (event.kind != APKRT_INPUT_MOTION) return queue.events.size() < most_events;
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


/// Keeps a record of another kind than motion in the buffer input_read fills, the rest zero.
template <typename Record>
apkrt_motion_event wrapped(const Record &record) {
    apkrt_motion_event event{};
    std::memcpy(&event, &record, sizeof record);
    return event;
}

/// AMOTION_EVENT_ACTION_* of an axes record (ADR-0068 decision 5).
constexpr std::int32_t action_hover_move = 7, action_scroll = 8, action_hover_enter = 9, action_hover_exit = 10,
                       action_button_press = 11, action_button_release = 12, most_tool_type = 5;

/// A full channel's answer to an axes MOVE (a stick or a pointer that moves): the newest MOVE of the same device,
/// source and action is erased and `next` goes to the back, so that the guest finds the newest report, the count stays
/// and event times keep their order. Under `lock`. Whether it did.
bool replaces_newest_axes(InputQueue &queue, const apkrt_motion_event &next_record) {
    apkrt_axes_event next;
    std::memcpy(&next, &next_record, sizeof next);
    if (next.action != action_move && next.action != action_hover_move) return false;
    for (auto at = queue.events.rbegin(); at != queue.events.rend(); ++at) {
        if (at->kind != APKRT_INPUT_AXES) continue;
        apkrt_axes_event old;
        std::memcpy(&old, &*at, sizeof old);
        if (old.device_id != next.device_id || old.source != next.source || old.action != next.action) continue;
        queue.events.erase(std::next(at).base());
        queue.events.push_back(next_record);
        return true;
    }
    return false;
}

/// The action of an axes record held in a motion event's buffer.
std::int32_t wrapped_action(const apkrt_motion_event &event) {
    return event.action;  // the action is the first word of every record
}

/// Queues `event`, a record of another kind, for the channel of `id`. 1 when it was taken.
int post_record(std::uint64_t id, apkrt_motion_event &event) {
    try {
        const std::lock_guard hold(lock);
        const auto found = inputs.find(id);
        if (found == inputs.end() || found->second.ended) return 0;
        InputQueue &queue = found->second;
        if (event.kind == APKRT_INPUT_AXES && queue.events.size() >= most_events) {
            if (replaces_newest_axes(queue, event)) return 1;
            // A press, a release or a scroll cannot merge: it takes the slack the ends of a touch gesture have. A
            // MOVE with no MOVE of its device waiting is refused.
            apkrt_axes_event axes;
            std::memcpy(&axes, &event, sizeof axes);
            if (axes.action == action_move || axes.action == action_hover_move) return 0;
            if (queue.events.size() >= most_events + most_ends) return 0;
            queue.events.push_back(event);
            if (queue.channel != nullptr) queue.channel->arrived();
            return 1;
        }
        if (!takes(queue, event)) return 0;
        queue.events.push_back(event);
        if (queue.channel != nullptr) queue.channel->arrived();
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}


/// AMOTION_EVENT_AXIS_* and BUTTON_* of a mouse's records (ADR-0068 decision 5, the mouse step).
constexpr std::int32_t axis_x = 0, axis_y = 1, axis_vscroll = 9, axis_hscroll = 10, axis_relative_x = 27,
                       axis_relative_y = 28;
constexpr std::int32_t button_primary_three = 0x7, button_back = 0x8, button_forward = 0x10, buttons_known = 0x1f;
constexpr std::int32_t keycode_back = 4, keycode_forward = 125, source_mouse = 0x00002002;

/// The records of one mouse report (apkrt_input_post_mouse), as Android 15's InputReader and dispatcher make them from
/// a `uinput` mouse (fixtures/expected/mouse-test.logcat).
class MouseReport {
public:
    MouseReport(const apkrt_axes_event &in, MouseState &state) : in_(in), state_(state) {
        state_.device = in.device_id;
        state_.source = in.source;
        state_.tool = in.tool_type;
        state_.time = in.event_time;
    }

    std::vector<apkrt_motion_event> records;

    void move() {
        place();
        std::vector<apkrt_axis_value> relative;
        for (std::uint32_t at = 0; at < in_.axis_count; ++at) {
            const apkrt_axis_value &axis = in_.axes[at];
            if (axis.axis == axis_relative_x || axis.axis == axis_relative_y) relative.push_back(axis);
        }
        if ((state_.buttons & button_primary_three) != 0) {
            add(action_move, state_.buttons, 0, relative);
        } else {
            hover(relative);
        }
    }

    void leave() {
        place();
        if (state_.hovering) add(action_hover_exit, state_.buttons, 0, state_.hover_extra);
        state_.hovering = false;
    }

    void scroll() {
        place();
        if ((state_.buttons & button_primary_three) == 0) hover({});
        std::vector<apkrt_axis_value> wheels;
        for (std::uint32_t at = 0; at < in_.axis_count; ++at) {
            const apkrt_axis_value &axis = in_.axes[at];
            if (axis.axis == axis_vscroll || axis.axis == axis_hscroll) wheels.push_back(axis);
        }
        add(action_scroll, state_.buttons, 0, wheels);
    }

    void press() {
        place();
        for (std::int32_t bit = 1; bit <= buttons_known; bit <<= 1) {
            if ((in_.button_state & bit) == 0 || (state_.buttons & bit) != 0) continue;
            const bool first = (state_.buttons & button_primary_three) == 0;
            state_.buttons |= bit;
            if (bit == button_back || bit == button_forward) {
                key(0, bit == button_back ? keycode_back : keycode_forward);
                hover({});
            } else if (first) {
                if (state_.hovering) add(action_hover_exit, state_.buttons, 0, state_.hover_extra);
                state_.hovering = false;
                add(action_down, state_.buttons, 0, {});
                add(action_button_press, state_.buttons, bit, {});
            } else {
                add(action_move, state_.buttons, 0, {});
                add(action_button_press, state_.buttons, bit, {});
            }
        }
    }

    void release() {
        place();
        for (std::int32_t bit = 1; bit <= buttons_known; bit <<= 1) {
            if ((in_.button_state & bit) != 0 || (state_.buttons & bit) == 0) continue;
            state_.buttons &= ~bit;
            if (bit == button_back || bit == button_forward) {
                hover({});
                key(1, bit == button_back ? keycode_back : keycode_forward);
            } else {
                add(action_button_release, state_.buttons, bit, {});
                if ((state_.buttons & button_primary_three) != 0) {
                    add(action_move, state_.buttons, 0, {});
                } else {
                    add(action_up, state_.buttons, 0, {});
                    state_.hovering = false;
                    hover({});
                }
            }
        }
    }

private:
    /// The position of the report, when it has one.
    void place() {
        for (std::uint32_t at = 0; at < in_.axis_count; ++at) {
            if (in_.axes[at].axis == axis_x) state_.x = in_.axes[at].value;
            if (in_.axes[at].axis == axis_y) state_.y = in_.axes[at].value;
        }
    }

    /// The first record over the window is a hover enter, the next ones hover moves. The exit that follows is made from
    /// the last of them, as Android 15's dispatcher makes it, with its relative axes.
    void hover(const std::vector<apkrt_axis_value> &extra) {
        state_.hover_extra = extra;
        add(state_.hovering ? action_hover_move : action_hover_enter, state_.buttons, 0, extra);
        state_.hovering = true;
    }

    void add(std::int32_t action, std::int32_t buttons, std::int32_t action_button,
             const std::vector<apkrt_axis_value> &extra) {
        apkrt_axes_event record{};
        record.action = action;
        record.reserved0[0] = static_cast<std::uint32_t>(action_button);
        record.kind = APKRT_INPUT_AXES;
        record.event_time = in_.event_time;
        record.device_id = in_.device_id;
        record.source = in_.source;
        record.tool_type = in_.tool_type;
        record.button_state = buttons;
        record.meta_state = in_.meta_state;
        record.axes[0] = {axis_x, state_.x};
        record.axes[1] = {axis_y, state_.y};
        record.axis_count = 2;
        for (const apkrt_axis_value &axis : extra) record.axes[record.axis_count++] = axis;
        records.push_back(wrapped(record));
    }

    void key(std::int32_t action, std::int32_t key_code) {
        apkrt_key_event record{};
        record.action = action;
        record.kind = APKRT_INPUT_KEY;
        record.down_time = in_.event_time;
        record.event_time = in_.event_time;
        record.key_code = key_code;
        record.meta_state = in_.meta_state;
        record.device_id = in_.device_id;
        record.source = in_.source;
        records.push_back(wrapped(record));
    }

    const apkrt_axes_event &in_;
    MouseState &state_;
};

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

// --- The WebView channel (ADR-0056 step a) -------------------------------------------------------------
// The host writes a stream of records to the guest over an AF_UNIX/SOCK_STREAM pair. The guest's read end is the
// description's host descriptor, which the guest reads with read(): non-blocking on the host and blocking for the
// guest, as the runtime's read waits for it as for a socket's, so that a signal ends the wait; the host's write end
// belongs to a per-channel writer thread that drains an unbounded queue with blocking writes, so a post never blocks
// its caller (ADR-0056 decision 3). Unlike the vsync and input channels this carries the records themselves, so it
// needs no doorbell pipe; readiness is the socket's own POLLIN.
namespace {

/// A WebView channel's description. Its host descriptor is the socket's guest-read end; it has no doorbell write
/// end (the base's stays -1), and its readiness is whatever the socket holds.
class WebViewChannel final : public Channel {
public:
    explicit WebViewChannel(int guest_read_end) : Channel(guest_read_end, -1, Of::webview, true) {}

    bool unread() const override {
        int pending = 0;
        return ::ioctl(host(), FIONREAD, &pending) == 0 && pending > 0;
    }
};

/// The host's side of a WebView channel: the socket's write end and the writer thread that drains its queue.
struct WebViewHost {
    std::mutex queue_lock;
    std::condition_variable wake;
    std::deque<std::vector<std::uint8_t>> queue;  // framed bytes waiting to go out, under queue_lock
    bool closing = false;                          // under queue_lock
    int write_end = -1;                            // the socket's host end; the writer thread closes it as it exits

    std::thread writer;

    ~WebViewHost() {
        if (writer.joinable()) {
            {
                const std::lock_guard hold(queue_lock);
                closing = true;
            }
            wake.notify_all();
            writer.join();  // the writer closed write_end on its way out
        }
        if (write_end >= 0) ::close(write_end);  // a channel whose writer thread never started
    }
};

std::mutex webview_lock;
std::map<std::uint64_t, std::unique_ptr<WebViewHost>> webviews;  // by token, under webview_lock

/// Writes all `length` bytes to `fd`, blocking, over EINTR and short writes. False once the guest closed its end
/// (EPIPE, which never raises SIGPIPE here) or the socket errors: nothing more can be delivered.
bool write_all(int fd, const std::uint8_t *data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const ssize_t moved = ::write(fd, data + sent, length - sent);
        if (moved < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<std::size_t>(moved);
    }
    return true;
}

void writer_loop(WebViewHost *host) {
    for (;;) {
        std::vector<std::uint8_t> buffer;
        {
            std::unique_lock<std::mutex> hold(host->queue_lock);
            host->wake.wait(hold, [host] { return !host->queue.empty() || host->closing; });
            if (host->queue.empty()) break;  // closing and drained
            buffer = std::move(host->queue.front());
            host->queue.pop_front();
        }
        if (!write_all(host->write_end, buffer.data(), buffer.size())) {
            const std::lock_guard hold(host->queue_lock);
            host->queue.clear();  // the guest's end is gone; drop the rest and wait for the close
        }
    }
    ::close(host->write_end);
    host->write_end = -1;
}

/// Tells the writer thread to drain what is queued and close the socket, then joins it.
void stop_writer(WebViewHost &host) {
    {
        const std::lock_guard hold(host.queue_lock);
        host.closing = true;
    }
    host.wake.notify_all();
    if (host.writer.joinable()) host.writer.join();
}

/// Frames `payload` (`length` bytes) as one record of `type`/`request_id`, or several split at
/// APKRT_WEBVIEW_RECORD_MAX with APKRT_WEBVIEW_MORE on all but the last. May throw std::bad_alloc.
std::vector<std::uint8_t> frame(std::uint32_t type, std::uint64_t request_id, const std::uint8_t *payload,
                                std::size_t length) {
    std::vector<std::uint8_t> out;
    std::size_t offset = 0;
    do {
        const std::size_t chunk = std::min<std::size_t>(length - offset, APKRT_WEBVIEW_RECORD_MAX);
        apkrt_webview_record_header header{};
        header.type = type;
        header.flags = offset + chunk < length ? APKRT_WEBVIEW_MORE : 0u;
        header.length = static_cast<std::uint32_t>(chunk);
        header.reserved = 0;
        header.request_id = request_id;
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(&header);
        out.insert(out.end(), bytes, bytes + sizeof header);
        if (chunk > 0) out.insert(out.end(), payload + offset, payload + offset + chunk);
        offset += chunk;
    } while (offset < length);
    return out;
}

/// Queues `bytes` for the token's writer thread, or -ENOENT when the token has no channel.
long enqueue(std::uint64_t token, std::vector<std::uint8_t> &&bytes) {
    const std::lock_guard hold(webview_lock);
    const auto found = webviews.find(token);
    if (found == webviews.end()) return -linux_errno::enoent;
    WebViewHost &host = *found->second;
    {
        const std::lock_guard queued(host.queue_lock);
        host.queue.push_back(std::move(bytes));
    }
    host.wake.notify_one();
    return 0;
}

}  // namespace

bool reads_records(descriptors::Description &channel) {
    return channel.kind() == Description::Kind::channel && static_cast<Channel &>(channel).of() == Channel::Of::webview;
}

long webview_make(std::uint64_t token) {
    if (token == 0) return -linux_errno::einval;
    int ends[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) return -sys::to_linux_errno(errno);
    // ends[0]: the guest's read end (the description's host descriptor, non-blocking on the host, the runtime's
    // read waiting for it). ends[1]: the host's write end, blocking, for the writer thread. Both close-on-exec; a
    // write never raises SIGPIPE once the guest's end is gone.
    for (const int end : ends) ::fcntl(end, F_SETFD, FD_CLOEXEC);
    ::fcntl(ends[0], F_SETFL, O_NONBLOCK);
    ::fcntl(ends[1], F_SETNOSIGPIPE, 1);

    std::shared_ptr<WebViewChannel> channel;  // owns ends[0] until the table takes it
    std::unique_ptr<WebViewHost> host;        // owns ends[1] and, once started, the writer thread
    try {
        channel = std::make_shared<WebViewChannel>(ends[0]);
        host = std::make_unique<WebViewHost>();
    } catch (const std::bad_alloc &) {
        if (!channel) ::close(ends[0]);  // else the channel owns and will close it
        ::close(ends[1]);
        return -linux_errno::enomem;
    }
    host->write_end = ends[1];
    try {
        host->writer = std::thread(writer_loop, host.get());  // waits on an empty queue until posts arrive
    } catch (const std::system_error &) {
        return -linux_errno::enomem;  // host closes ends[1], channel closes ends[0], as they go
    }

    const std::lock_guard hold(webview_lock);
    if (webviews.count(token) != 0) return -linux_errno::einval;  // one live channel per token
    // The table owns ends[0] now, or closed it on a full table.
    const long fd = descriptors::add(std::move(channel), true);
    if (fd < 0) return fd;
    try {
        webviews.emplace(token, std::move(host));
    } catch (const std::bad_alloc &) {
        // Undo the registration; the host (still here) stops its writer as it goes.
        descriptors::close(static_cast<int>(fd));
        return -linux_errno::enomem;
    }
    return fd;
}

long webview_post(std::uint64_t token, std::uint32_t type, std::uint64_t request_id, const void *payload,
                  std::size_t length) {
    if (payload == nullptr && length != 0) return -linux_errno::efault;
    std::vector<std::uint8_t> framed;
    try {
        framed = frame(type, request_id, static_cast<const std::uint8_t *>(payload), length);
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    return enqueue(token, std::move(framed));
}

long webview_post_raw(std::uint64_t token, const void *bytes, std::size_t length) {
    if (bytes == nullptr && length != 0) return -linux_errno::efault;
    std::vector<std::uint8_t> raw;
    try {
        const auto *start = static_cast<const std::uint8_t *>(bytes);
        raw.assign(start, start + length);
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    return enqueue(token, std::move(raw));
}

long webview_close(std::uint64_t token) {
    std::unique_ptr<WebViewHost> host;
    {
        const std::lock_guard hold(webview_lock);
        const auto found = webviews.find(token);
        if (found == webviews.end()) return -linux_errno::enoent;
        host = std::move(found->second);
        webviews.erase(found);
    }
    stop_writer(*host);  // drains what was queued, then closes the write end so the guest's reader meets EOF
    return 0;            // the guest fd and its read end stay until the guest closes them
}

}  // namespace apkrt::channels

using namespace apkrt::channels;

extern "C" int32_t apkrt_webview_post(uint64_t token, uint32_t type, uint64_t request_id, const void *bytes,
                                      uint32_t length) {
    return static_cast<int32_t>(webview_post(token, type, request_id, bytes, length));
}

extern "C" int32_t apkrt_webview_close(uint64_t token) { return static_cast<int32_t>(webview_close(token)); }

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
    return post_record(id, event);
}

extern "C" int apkrt_input_post_key(uint64_t id, const struct apkrt_key_event *key) {
    if (key == nullptr || (key->action != 0 && key->action != 1) || key->key_code < 0) return 0;
    apkrt_key_event record = *key;
    record.reserved0[0] = record.reserved0[1] = 0;
    record.reserved1 = 0;
    record.kind = APKRT_INPUT_KEY;
    apkrt_motion_event event = wrapped(record);
    return post_record(id, event);
}

extern "C" int apkrt_input_post_text(uint64_t id, const struct apkrt_text_event *text) {
    if (text == nullptr || text->op < 0 || text->op >= APKRT_TEXT_OPS || text->length > APKRT_TEXT_BYTES) return 0;
    apkrt_text_event record = *text;
    record.reserved = 0;
    record.kind = APKRT_INPUT_TEXT;
    std::memset(record.text + record.length, 0, sizeof record.text - record.length);
    apkrt_motion_event event = wrapped(record);
    return post_record(id, event);
}

/// Whether the NUL-terminated `name` (at most `size` bytes with its NUL) is UTF-8 that Java's JNI takes as it is: no
/// bad sequence, no overlong form, no surrogate, nothing above U+FFFF (the 4-byte forms JNI wants as surrogate pairs)
/// and no NUL inside.
static bool jni_safe_name(const char *name, std::size_t size) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(name);
    const void *end = std::memchr(name, 0, size);
    if (end == nullptr) return false;
    const std::size_t length = static_cast<std::size_t>(static_cast<const char *>(end) - name);
    for (std::size_t at = 0; at < length;) {
        const unsigned char lead = bytes[at];
        std::size_t more = 0;
        std::uint32_t code = lead;
        if (lead < 0x80) {
            ++at;
            continue;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            more = 1;
            code = lead & 0x1fu;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            more = 2;
            code = lead & 0x0fu;
        } else {
            return false;  // a continuation byte, an overlong 2-byte lead, or a 4-byte form
        }
        if (at + more >= length) return false;
        for (std::size_t i = 1; i <= more; ++i) {
            if ((bytes[at + i] & 0xc0u) != 0x80u) return false;
            code = (code << 6) | (bytes[at + i] & 0x3fu);
        }
        if ((more == 2 && code < 0x800) || (code >= 0xd800 && code <= 0xdfff)) return false;
        at += more + 1;
    }
    return true;
}

extern "C" int apkrt_input_post_device(uint64_t id, const struct apkrt_device_event *device) {
    if (device == nullptr || device->op < 0 || device->op >= APKRT_DEVICE_OPS || device->device_id < 1 ||
        !jni_safe_name(device->name, sizeof device->name)) {
        return 0;
    }
    apkrt_device_event record = *device;
    const std::size_t named = std::strlen(device->name);
    std::memset(record.name + named, 0, sizeof record.name - named);
    record.kind = APKRT_INPUT_DEVICE;
    apkrt_motion_event event = wrapped(record);
    if (device->op == APKRT_DEVICE_REMOVED) {
        // A mouse that goes while it hovers leaves the window first, as Android 15 does (mouse-test).
        try {
            const std::lock_guard hold(lock);
            const auto found = inputs.find(id);
            if (found != inputs.end() && !found->second.ended) {
                InputQueue &queue = found->second;
                MouseState &mouse = queue.mouse;
                if (mouse.hovering && mouse.device == device->device_id &&
                    queue.events.size() + 2 <= most_events + most_ends) {
                    apkrt_axes_event leave{};
                    leave.action = action_hover_exit;
                    leave.kind = APKRT_INPUT_AXES;
                    leave.event_time = mouse.time;
                    leave.device_id = mouse.device;
                    leave.source = mouse.source;
                    leave.tool_type = mouse.tool;
                    leave.button_state = mouse.buttons;
                    leave.axes[0] = {axis_x, mouse.x};
                    leave.axes[1] = {axis_y, mouse.y};
                    leave.axis_count = 2;
                    queue.events.push_back(wrapped(leave));
                    if (queue.channel != nullptr) queue.channel->arrived();
                }
                if (mouse.device == device->device_id) mouse = MouseState{};
            }
        } catch (const std::bad_alloc &) {
        }
    }
    return post_record(id, event);
}

namespace {

/// Whether an axes record is sound as far as its header and axes go: a device, a source, a tool type and axes that
/// are each named once, in range and finite.
bool axes_sound(const apkrt_axes_event *axes) {
    if (axes == nullptr || axes->device_id < 1 || axes->source == 0 || axes->tool_type < 0 ||
        axes->tool_type > most_tool_type || axes->axis_count > APKRT_AXES_MAX) {
        return false;
    }
    std::uint64_t named = 0;  // the axes seen, by number
    for (std::uint32_t at = 0; at < axes->axis_count; ++at) {
        const std::int32_t axis = axes->axes[at].axis;
        if (axis < 0 || axis >= APKRT_AXIS_LIMIT || !std::isfinite(axes->axes[at].value)) return false;
        const std::uint64_t bit = std::uint64_t{1} << axis;
        if ((named & bit) != 0) return false;
        named |= bit;
    }
    return true;
}

}  // namespace

extern "C" int apkrt_input_post_axes(uint64_t id, const struct apkrt_axes_event *axes) {
    if (!axes_sound(axes)) return 0;
    switch (axes->action) {
        case action_move:
        case action_hover_move:
        case action_scroll:
        case action_hover_enter:
        case action_hover_exit:
        case action_button_press:
        case action_button_release:
            break;
        default:
            return 0;
    }
    apkrt_axes_event record{};
    record.action = axes->action;
    record.kind = APKRT_INPUT_AXES;
    record.event_time = axes->event_time;
    record.device_id = axes->device_id;
    record.source = axes->source;
    record.tool_type = axes->tool_type;
    record.button_state = axes->button_state;
    record.meta_state = axes->meta_state;
    record.axis_count = axes->axis_count;
    std::memcpy(record.axes, axes->axes, axes->axis_count * sizeof record.axes[0]);
    apkrt_motion_event event = wrapped(record);
    return post_record(id, event);
}

extern "C" int apkrt_input_post_mouse(uint64_t id, const struct apkrt_axes_event *mouse) {
    if (!axes_sound(mouse) || (mouse->source & source_mouse) != source_mouse ||
        (mouse->button_state & ~buttons_known) != 0) {
        return 0;
    }
    switch (mouse->action) {
        case action_hover_move:
        case action_scroll:
        case action_hover_exit:
        case action_button_press:
        case action_button_release:
            break;
        default:
            return 0;
    }
    try {
        const std::lock_guard hold(lock);
        const auto found = inputs.find(id);
        if (found == inputs.end() || found->second.ended) return 0;
        InputQueue &queue = found->second;
        MouseState state = queue.mouse;
        MouseReport report(*mouse, state);
        switch (mouse->action) {
            case action_hover_move: report.move(); break;
            case action_scroll: report.scroll(); break;
            case action_hover_exit: report.leave(); break;
            case action_button_press: report.press(); break;
            default: report.release(); break;
        }
        const std::size_t count = report.records.size();
        const std::size_t size = queue.events.size();
        // A move is all a full channel merges: it replaces the newest one waiting. The rest takes the slack of the
        // touch ends, all of it or none.
        const bool only_move = mouse->action == action_hover_move && count == 1 &&
                               (wrapped_action(report.records[0]) == action_move ||
                                wrapped_action(report.records[0]) == action_hover_move);
        if (size + count <= most_events) {
            // room
        } else if (only_move) {
            if (!replaces_newest_axes(queue, report.records[0])) return 0;
            queue.mouse = state;
            return 1;
        } else if (size + count > most_events + most_ends) {
            return 0;
        }
        for (const apkrt_motion_event &record : report.records) {
            queue.events.push_back(record);
            if (queue.channel != nullptr) queue.channel->arrived();
        }
        queue.mouse = state;
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}
