// The audio output streams (ADR-0036, decisions 5 and 6). A stream lives in one of 64 static slots: its ring of
// interleaved float32 frames, its positions and its state. The guest reaches it only through the private syscalls of
// guest_interface.h and never maps the ring; the host's render block reads it with apkrt_audio_read, which takes no
// lock, allocates nothing and makes no syscall but the wakes of the guests that wait on the stream.
//
// Who changes what:
//   - the guest's syscalls, one at a time per stream (Slot::guest_lock), write the ring and `written`, and set the
//     state by the commands;
//   - the render block reads the ring and moves `read`, and changes the state only from STARTING to STARTED, at its
//     first read after a start, and from playing_out to STOPPED, once a stopped stream has played out;
//   - apkrt_audio_disconnected makes the stream DISCONNECTED, which holds.
// Every change of the state is a compare-and-swap, so none overwrites another side's. PAUSE and FLUSH wait out the
// reads under way, so that none takes frames once they return; the close waits out the guests and the reads inside
// before the host's close and before the ring goes.
//
// A guest that waits (audio_write for room, audio_wait for another sequence) sleeps on its own thread's signal word
// (ADR-0026, syscalls.h wait_until), which it first puts into one of its stream's places; whatever changes what it
// waits for raises the words there. A thread that finds no free place waits in rounds of a millisecond.
#include "audio.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "permissions.h"
#include "signals.h"
#include "syscalls.h"

#include <AudioToolbox/AudioUnitProperties.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/object.h>
#include <os/workgroup.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>

namespace apkrt::audio {
namespace {

namespace linux_errno = sys::linux_errno;

constexpr long emfile = 24;  // Linux: include/uapi/asm-generic/errno-base.h

constexpr int slot_count = 64;
constexpr int slot_bits = 6;  // a handle is the slot's generation above its index
constexpr int waiter_places = 16;
constexpr std::int64_t poll_round = 1'000'000;  // ns, for a waiter without a place
constexpr std::int32_t lowest_rate = 8'000, highest_rate = 1'600'000;
constexpr std::int32_t mode_none = 10, mode_low_latency = 12;  // AAudio's performance modes
constexpr double longest_burst = 65'536;                       // frames; more is no IO buffer
/// A stopped stream that still plays what it held when it stopped; the guest sees STOPPED.
constexpr std::int32_t playing_out = -APKRT_AUDIO_STOPPED;
/// after()'s answer for a command the state does not allow.
constexpr std::int32_t refused = 0;

struct Slot {
    // The open stream's handle; 0 while the slot is free or its stream closes. The render block and the guest's
    // syscalls count themselves in (readers, guests) before they compare it, so the close, which clears it first,
    // knows when none is left inside.
    std::atomic<std::uint64_t> handle{0};
    std::atomic<std::uint32_t> readers{0};
    std::atomic<std::uint32_t> guests{0};
    std::atomic<std::uint64_t> closing{0};  // the handle whose host close runs, for apkrt_audio_xruns

    // Under table_lock.
    bool used = false;
    std::uint32_t generation = 0;
    std::uint64_t opening = 0;        // the handle the host opens now
    bool disconnected_early = false;  // apkrt_audio_disconnected came for `opening`

    // Fixed from the open to the close.
    apkrt_audio_host host{};  // the one that opened the stream, which closes it (an input host has the same shape)
    bool input = false;       // an input stream (ADR-0064): the host writes the ring, the guest reads it
    std::atomic<bool> host_closed{false};  // the host's close has run (a revoked grant closes it before the guest does)
    std::int32_t rate = 0;
    std::int32_t channels = 0;
    std::int32_t burst = 0;
    std::int32_t capacity = 0;
    float *ring = nullptr;

    std::atomic<std::int32_t> state{0};
    std::atomic<std::uint32_t> sequence{0};
    std::atomic<std::int64_t> written{0};
    std::atomic<std::int64_t> read{0};
    std::atomic<std::int32_t> buffer_size{0};
    std::atomic<std::int64_t> stop_at{0};  // `written` at the STOP: playing out ends there
    std::atomic<std::int32_t> xruns{0};
    std::atomic<std::uint32_t> starts{0};  // the STARTs so far
    std::uint32_t played_in = 0;           // the render block's: the start in whose run it last took frames

    // The timestamp, which only the render block writes: a sequence lock, odd while it writes.
    std::atomic<std::uint32_t> stamp_sequence{0};
    std::atomic<std::int64_t> stamp_position{-1};
    std::atomic<std::int64_t> stamp_time{0};

    // The signal words of the guests that wait on the stream, and the raises of them under way.
    std::atomic<signals::ThreadSignals *> waiters[waiter_places];
    std::atomic<std::uint32_t> raising{0};

    std::mutex guest_lock;  // one guest syscall at a time writes the ring or changes the state
};

Slot slots[slot_count];
// The slots' use, generations and opens; immortal (apkrt_audio_disconnected during exit, network.cpp).
[[clang::no_destroy]] constinit std::mutex table_lock;

[[clang::no_destroy]] constinit std::mutex host_lock;  // constant-init + immortal (host callback, network.cpp)
apkrt_audio_host current_host{};
bool have_host = false;
apkrt_audio_host current_input_host{};  // an apkrt_audio_input_host, which has the same shape
bool have_input_host = false;

std::atomic<std::int64_t> output_latency{0};  // ns
// Constant-init + immortal (apkrt_audio_set_output, network.cpp).
[[clang::no_destroy]] constinit std::mutex output_lock;
os_workgroup_t output_workgroup = nullptr;  // +1, from the host's output unit

std::atomic<std::uint32_t> timebase_numer{1}, timebase_denom{1};  // mach_timebase_info, set at every open

Slot *slot_of(std::uint64_t handle) {
    if ((handle >> slot_bits) == 0) return nullptr;
    return &slots[handle & (slot_count - 1)];
}

std::int32_t reported(std::int32_t state) {
    return state == playing_out ? APKRT_AUDIO_STOPPED : state;
}

/// Raises the signal words of the guests that wait on `slot`.
void raise_waiters(Slot &slot) {
    slot.raising.fetch_add(1);
    for (auto &place : slot.waiters) {
        if (auto *waiter = place.load(); waiter != nullptr) signals::raise_signal_word(*waiter);
    }
    slot.raising.fetch_sub(1);
}

/// Something a guest may wait for changed: frames read or dropped, the state, the buffer size, the close.
void changed(Slot &slot) {
    slot.sequence.fetch_add(1);
    raise_waiters(slot);
}

/// Waits until no read of the render block is under way: one that saw the state before a change has ended.
void wait_out_reads(const Slot &slot) {
    while (slot.readers.load() != 0) sched_yield();
}

/// A guest syscall's hold on the stream of `handle`; none when it is not open.
class Entry {
public:
    explicit Entry(std::uint64_t handle) : slot_(slot_of(handle)) {
        if (slot_ == nullptr) return;
        slot_->guests.fetch_add(1);
        if (slot_->handle.load() == handle) return;
        slot_->guests.fetch_sub(1);
        slot_ = nullptr;
    }
    ~Entry() {
        if (slot_ != nullptr) slot_->guests.fetch_sub(1);
    }
    Entry(const Entry &) = delete;
    Entry &operator=(const Entry &) = delete;

    Slot *slot() const { return slot_; }

private:
    Slot *slot_;
};

void copy_in(Slot &slot, std::int64_t position, const float *frames, std::int64_t count) {
    const auto channels = static_cast<std::size_t>(slot.channels);
    const std::int64_t at = position % slot.capacity;
    const std::int64_t first = std::min<std::int64_t>(count, slot.capacity - at);
    std::memcpy(slot.ring + static_cast<std::size_t>(at) * channels, frames,
                static_cast<std::size_t>(first) * channels * sizeof(float));
    if (first < count) {
        std::memcpy(slot.ring, frames + static_cast<std::size_t>(first) * channels,
                    static_cast<std::size_t>(count - first) * channels * sizeof(float));
    }
}

void copy_out(const Slot &slot, std::int64_t position, float *frames, std::int64_t count) {
    const auto channels = static_cast<std::size_t>(slot.channels);
    const std::int64_t at = position % slot.capacity;
    const std::int64_t first = std::min<std::int64_t>(count, slot.capacity - at);
    std::memcpy(frames, slot.ring + static_cast<std::size_t>(at) * channels,
                static_cast<std::size_t>(first) * channels * sizeof(float));
    if (first < count) {
        std::memcpy(frames + static_cast<std::size_t>(first) * channels, slot.ring,
                    static_cast<std::size_t>(count - first) * channels * sizeof(float));
    }
}

/// The render block's timestamp: `position` is heard at `host_time` (mach ticks) plus the output's latency.
void stamp(Slot &slot, std::int64_t position, std::uint64_t host_time, std::int64_t latency) {
    const auto nanoseconds = host_time * timebase_numer.load(std::memory_order_relaxed) /
                             timebase_denom.load(std::memory_order_relaxed);
    const std::int64_t time = static_cast<std::int64_t>(nanoseconds) + latency;
    const std::uint32_t sequence = slot.stamp_sequence.load(std::memory_order_relaxed);
    slot.stamp_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.stamp_position.store(position, std::memory_order_relaxed);
    slot.stamp_time.store(time, std::memory_order_relaxed);
    slot.stamp_sequence.store(sequence + 2, std::memory_order_release);
}

void read_stamp(const Slot &slot, std::int64_t &position, std::int64_t &time) {
    while (true) {
        const std::uint32_t before = slot.stamp_sequence.load(std::memory_order_acquire);
        position = slot.stamp_position.load(std::memory_order_relaxed);
        time = slot.stamp_time.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((before & 1) == 0 && slot.stamp_sequence.load(std::memory_order_relaxed) == before) return;
        sched_yield();
    }
}

/// The render block's read of `slot`, whose handle it holds.
std::int64_t take_frames(Slot &slot, float *buffer, std::int64_t frames, std::uint64_t host_time) {
    std::int32_t state = slot.state.load();
    bool change = false;
    // The first read after a start makes the stream STARTED, also when it finds no frame.
    if (state == APKRT_AUDIO_STARTING && slot.state.compare_exchange_strong(state, APKRT_AUDIO_STARTED)) {
        state = APKRT_AUDIO_STARTED;
        change = true;
    }
    std::int64_t taken = 0;
    if (state == APKRT_AUDIO_STARTED || state == playing_out) {
        const std::int64_t read = slot.read.load();
        // A stopped stream plays out only the frames it held at the stop; later ones wait for the next start.
        const std::int64_t written = slot.written.load();
        const std::int64_t end = state == playing_out ? std::min(written, slot.stop_at.load()) : written;
        taken = std::max<std::int64_t>(0, std::min(end - read, frames));
        if (taken > 0) {
            copy_out(slot, read, buffer, taken);
            slot.read.store(read + taken);
            stamp(slot, read, host_time, output_latency.load());
            change = true;
        }
        if (state == APKRT_AUDIO_STARTED) {
            // An xrun: fewer frames than asked, once the stream has played since its start.
            const std::uint32_t start = slot.starts.load();
            if (taken < frames && slot.played_in == start) slot.xruns.fetch_add(1);
            if (taken > 0) slot.played_in = start;
        } else if (read + taken >= end) {
            std::int32_t stopping = playing_out;
            slot.state.compare_exchange_strong(stopping, APKRT_AUDIO_STOPPED);  // played out
        }
    }
    if (change) changed(slot);
    return taken;
}

/// The capture thread's write of `frames` frames to the input stream `slot`, whose handle it holds.
std::int64_t take_capture(Slot &slot, const float *buffer, std::int64_t frames, std::uint64_t host_time) {
    std::int32_t state = slot.state.load();
    bool change = false;
    // The first capture after a start makes the stream STARTED.
    if (state == APKRT_AUDIO_STARTING && slot.state.compare_exchange_strong(state, APKRT_AUDIO_STARTED)) {
        state = APKRT_AUDIO_STARTED;
        change = true;
    }
    if (state != APKRT_AUDIO_STARTED) {
        if (change) changed(slot);
        return 0;
    }
    const std::int64_t capacity = slot.capacity;
    std::int64_t skipped = 0;  // frames of the capture itself that cannot fit
    if (frames > capacity) {
        skipped = frames - capacity;
        buffer += static_cast<std::size_t>(skipped) * static_cast<std::size_t>(slot.channels);
        frames = capacity;
    }
    const std::int64_t written = slot.written.load();
    bool overrun = skipped > 0;
    std::int64_t read = slot.read.load();
    // Room first: a full ring gives up its oldest frames, by moving `read` on, which the guest's read notices.
    while (written + frames - read > capacity) {
        overrun = true;
        const std::int64_t oldest = written + frames - capacity;
        if (slot.read.compare_exchange_weak(read, oldest)) break;
    }
    if (overrun) slot.xruns.fetch_add(1);
    copy_in(slot, written, buffer, frames);
    slot.written.store(written + frames);
    stamp(slot, written, host_time, 0);
    changed(slot);
    return frames + skipped;
}

/// The state `command` gives from `state`, or `refused`.
std::int32_t after(int command, std::int32_t state, bool input) {
    const bool playing = state == APKRT_AUDIO_STARTING || state == APKRT_AUDIO_STARTED;
    switch (command) {
        case APKRT_AUDIO_START:
            return playing ? state : APKRT_AUDIO_STARTING;
        case APKRT_AUDIO_PAUSE:
            return APKRT_AUDIO_PAUSED;
        case APKRT_AUDIO_FLUSH:
            return playing ? refused : APKRT_AUDIO_FLUSHED;
        case APKRT_AUDIO_STOP:
            // An input stream stops at once; the frames it holds stay for the guest to read.
            if (input) return APKRT_AUDIO_STOPPED;
            // As AudioTrack's stop: what a started stream holds still plays.
            if (playing) return playing_out;
            return state == playing_out ? playing_out : APKRT_AUDIO_STOPPED;
        default:
            return refused;
    }
}

/// Whether `deadline` (the guest's CLOCK_MONOTONIC, ns) has passed; 0 and negative ones have.
bool passed(std::int64_t deadline) {
    return deadline <= 0 || deadline <= sys::monotonic_now();
}

enum class Woken { done, closed, timeout, signal };

/// Waits on the calling thread's signal word until `done()` holds, the stream of `handle` closes, `deadline`
/// passes, or a signal the thread takes is pending.
template <typename Done>
Woken wait_on(Slot &slot, std::uint64_t handle, Done done, std::int64_t deadline) {
    auto &self = signals::current();
    int place = -1;
    for (int index = 0; index < waiter_places && place < 0; ++index) {
        signals::ThreadSignals *vacant = nullptr;
        if (slot.waiters[index].compare_exchange_strong(vacant, &self)) place = index;
    }
    const auto closed = [&] { return slot.handle.load() != handle; };
    const auto over = [&] { return closed() || done(); };
    sys::WaitEnd end;
    if (place >= 0) {
        end = sys::wait_until(self, over, deadline);
        slot.waiters[place].store(nullptr);
        // A raise that found the word before the place was given back ends first: the thread's signal state may
        // go with the thread.
        while (slot.raising.load() != 0) sched_yield();
    } else {
        while (true) {
            const std::int64_t now = sys::monotonic_now();
            const bool last = deadline - now <= poll_round;
            end = sys::wait_until(self, over, last ? deadline : now + poll_round);
            if (end != sys::WaitEnd::timeout || last) break;
        }
    }
    if (closed()) return Woken::closed;
    switch (end) {
        case sys::WaitEnd::done:
            return Woken::done;
        case sys::WaitEnd::timeout:
            return Woken::timeout;
        case sys::WaitEnd::signal:
            return Woken::signal;
    }
    return Woken::done;
}

/// A stream thread's place in the output's workgroup; it leaves as the thread ends.
struct Joined {
    os_workgroup_t workgroup = nullptr;  // +1 while the thread is in it
    os_workgroup_join_token_s token{};

    Joined() = default;
    Joined(const Joined &) = delete;
    Joined &operator=(const Joined &) = delete;
    ~Joined() { leave(); }

    void leave() {
        if (workgroup == nullptr) return;
        os_workgroup_leave(workgroup, &token);
        os_release(workgroup);
        workgroup = nullptr;
    }
};
thread_local Joined joined;

/// Makes the calling thread a real-time one with a period of `seconds`, as the output's own IO thread is. A thread
/// the kernel refuses it goes on as it is.
void make_real_time(double seconds) {
    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    const double ticks = seconds * 1e9 * base.denom / base.numer;
    const auto period = static_cast<std::uint32_t>(std::clamp(ticks, 1.0, 1e9));
    thread_time_constraint_policy_data_t policy{period, period / 2, period, TRUE};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

}  // namespace

long open(std::uint64_t config_address) {
    auto *config = sys::as_pointer<apkrt_audio_config>(config_address);
    if (config == nullptr) return -linux_errno::efault;
    const apkrt_audio_config asked = *config;
    if ((asked.sample_rate != 0 && (asked.sample_rate < lowest_rate || asked.sample_rate > highest_rate)) ||
        asked.channels < 1 || asked.channels > APKRT_AUDIO_CHANNELS || asked.performance_mode < mode_none ||
        asked.performance_mode > mode_low_latency || asked.direction < 0 || asked.direction > 1) {
        return -linux_errno::einval;
    }
    const bool input = asked.direction == 1;
    if (input && permissions::outcome(APKRT_PERMISSION_MICROPHONE) != APKRT_PERMISSION_GRANTED) {
        return -linux_errno::eacces;  // not asked yet counts as not granted
    }
    apkrt_audio_host host{};
    {
        const std::lock_guard hold(host_lock);
        if (!(input ? have_input_host : have_host)) return -linux_errno::enodev;
        host = input ? current_input_host : current_host;
    }
    Slot *slot = nullptr;
    std::uint64_t handle = 0;
    {
        const std::lock_guard hold(table_lock);
        for (int index = 0; index < slot_count && slot == nullptr; ++index) {
            if (!slots[index].used) slot = &slots[index];
        }
        if (slot == nullptr) return -emfile;
        slot->used = true;
        if (++slot->generation == 0) slot->generation = 1;  // a handle is never below 64
        handle = (std::uint64_t{slot->generation} << slot_bits) | static_cast<std::uint64_t>(slot - slots);
        slot->opening = handle;
        slot->disconnected_early = false;
    }
    const auto give_up = [&](long error) {
        const std::lock_guard hold(table_lock);
        slot->opening = 0;
        slot->used = false;
        return error;
    };

    // The host opens with no lock held: it may block, and may call apkrt_audio_disconnected and
    // apkrt_audio_set_output.
    apkrt_audio_hardware hardware{};
    if (host.open(host.context, handle, asked.sample_rate, asked.channels, asked.performance_mode, &hardware) != 0) {
        return give_up(-linux_errno::eio);
    }
    const double hardware_rate = std::round(hardware.sample_rate);
    if (!(hardware_rate >= lowest_rate && hardware_rate <= highest_rate && hardware.io_buffer_duration > 0 &&
          hardware.io_buffer_duration <= longest_burst / lowest_rate)) {
        host.close(host.context, handle);
        return give_up(-linux_errno::eio);
    }
    const std::int32_t rate = asked.sample_rate != 0 ? asked.sample_rate : static_cast<std::int32_t>(hardware_rate);
    const double burst_frames = std::max(1.0, std::floor(hardware.io_buffer_duration * rate + 0.5));  // halves up
    if (burst_frames > longest_burst) {
        host.close(host.context, handle);
        return give_up(-linux_errno::eio);
    }
    const auto burst = static_cast<std::int32_t>(burst_frames);
    std::int32_t capacity = APKRT_AUDIO_BURSTS * burst;
    if (input) {
        // An input ring holds what accumulates between the guest's reads: APKRT_AUDIO_INPUT_BURSTS bursts, or the
        // frames the guest asked for in `buffer_size` (at most ten seconds), rounded up to a burst.
        const std::int64_t wanted = std::clamp<std::int64_t>(asked.buffer_size, 0, std::int64_t{10} * rate);
        const std::int64_t bursts = std::max<std::int64_t>(APKRT_AUDIO_INPUT_BURSTS, (wanted + burst - 1) / burst);
        capacity = static_cast<std::int32_t>(bursts * burst);
    }
    // As Android 15's streams start: a LOW_LATENCY one (MMAP) at two bursts, the others (AudioStreamTrack, whose
    // getBufferSize is the AudioTrack's frameCount at the open, AudioStreamTrack.cpp:211 and :500) at the capacity.
    const std::int32_t first_buffer_size =
        !input && asked.performance_mode == mode_low_latency ? APKRT_AUDIO_FIRST_BURSTS * burst : capacity;
    float *ring = new (std::nothrow) float[static_cast<std::size_t>(capacity) * static_cast<std::size_t>(asked.channels)];
    if (ring == nullptr) {
        host.close(host.context, handle);
        return give_up(-linux_errno::enomem);
    }

    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    timebase_numer.store(base.numer, std::memory_order_relaxed);
    timebase_denom.store(base.denom, std::memory_order_relaxed);
    slot->host = host;
    slot->input = input;
    slot->host_closed.store(false);
    slot->rate = rate;
    slot->channels = asked.channels;
    slot->burst = burst;
    slot->capacity = capacity;
    slot->ring = ring;
    slot->state.store(APKRT_AUDIO_OPEN);
    slot->written.store(0);
    slot->read.store(0);
    slot->buffer_size.store(first_buffer_size);
    slot->xruns.store(0);
    slot->starts.store(0);
    slot->played_in = 0;
    slot->stamp_sequence.store(0);
    slot->stamp_position.store(-1);
    slot->stamp_time.store(0);
    {
        const std::lock_guard hold(table_lock);
        slot->opening = 0;
        if (slot->disconnected_early) slot->state.store(APKRT_AUDIO_DISCONNECTED);
        slot->handle.store(handle);  // from here on the stream is there for the guest and the render block
    }
    config->sample_rate = rate;
    config->frames_per_burst = burst;
    config->capacity = capacity;
    config->buffer_size = first_buffer_size;
    config->hardware_rate = static_cast<std::int32_t>(hardware_rate);
    return static_cast<long>(handle);
}

long close(std::uint64_t handle) {
    const Entry entry(handle);
    Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    std::uint64_t open_handle = handle;
    if (!slot->handle.compare_exchange_strong(open_handle, 0)) return -linux_errno::ebadf;  // another close came first
    slot->closing.store(handle);
    changed(*slot);  // the waiting guests find the stream gone
    while (slot->guests.load() != 1 || slot->readers.load() != 0) sched_yield();
    if (!slot->host_closed.exchange(true)) slot->host.close(slot->host.context, handle);
    delete[] slot->ring;
    slot->ring = nullptr;
    const std::lock_guard hold(table_lock);
    slot->closing.store(0);
    slot->used = false;
    return 0;
}

long control(std::uint64_t handle, std::uint64_t command, std::int64_t argument) {
    const Entry entry(handle);
    Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    if (command < static_cast<std::uint64_t>(APKRT_AUDIO_START) ||
        command > static_cast<std::uint64_t>(APKRT_AUDIO_BUFFER_SIZE)) {
        return -linux_errno::einval;
    }
    const auto request = static_cast<int>(command);
    const std::lock_guard hold(slot->guest_lock);
    if (request == APKRT_AUDIO_BUFFER_SIZE) {
        if (slot->state.load() == APKRT_AUDIO_DISCONNECTED) return -linux_errno::epipe;
        const auto size = static_cast<std::int32_t>(std::clamp<std::int64_t>(argument, slot->burst, slot->capacity));
        if (slot->buffer_size.exchange(size) != size) changed(*slot);
        return size;
    }
    std::int32_t state = slot->state.load();
    while (true) {
        if (state == APKRT_AUDIO_DISCONNECTED) return -linux_errno::epipe;
        const std::int32_t next = after(request, state, slot->input);
        if (next == refused) return -linux_errno::eperm;
        if (request == APKRT_AUDIO_START && next != state) slot->starts.fetch_add(1);
        if (next == playing_out && state != playing_out) slot->stop_at.store(slot->written.load());  // under guest_lock
        if (next != state && !slot->state.compare_exchange_strong(state, next)) continue;  // the render block or a
                                                                                            // disconnect came first
        if (next == APKRT_AUDIO_PAUSED || next == APKRT_AUDIO_FLUSHED) wait_out_reads(*slot);
        bool dropped = false;
        if (next == APKRT_AUDIO_FLUSHED) {
            const std::int64_t written = slot->written.load();
            dropped = slot->read.exchange(written) != written;
        }
        if (reported(next) != reported(state) || dropped) changed(*slot);
        return 0;
    }
}

long write(std::uint64_t handle, std::uint64_t frames_address, std::int64_t frames, std::int64_t deadline) {
    const Entry entry(handle);
    Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    if (slot->input) return -linux_errno::eperm;
    if (frames < 0) return -linux_errno::einval;
    if (frames == 0) return 0;
    const auto *source = sys::as_pointer<const float>(frames_address);
    if (source == nullptr) return -linux_errno::efault;
    const auto channels = static_cast<std::size_t>(slot->channels);
    std::int64_t taken = 0;
    while (true) {
        {
            const std::lock_guard hold(slot->guest_lock);
            if (slot->state.load() == APKRT_AUDIO_DISCONNECTED) return taken > 0 ? taken : -linux_errno::epipe;
            const std::int64_t written = slot->written.load();
            const std::int64_t room = slot->buffer_size.load() - (written - slot->read.load());
            const std::int64_t count = std::min(room, frames - taken);
            if (count > 0) {
                copy_in(*slot, written, source + static_cast<std::size_t>(taken) * channels, count);
                slot->written.store(written + count);
                taken += count;
            }
        }
        if (taken == frames || passed(deadline)) return taken;
        const auto has_room = [&] {
            return slot->state.load() == APKRT_AUDIO_DISCONNECTED ||
                   slot->written.load() - slot->read.load() < slot->buffer_size.load();
        };
        switch (wait_on(*slot, handle, has_room, deadline)) {
            case Woken::done:
                break;
            case Woken::closed:
                return -linux_errno::ebadf;
            case Woken::timeout:
                return taken;
            case Woken::signal:
                return taken > 0 ? taken : -linux_errno::eintr;
        }
    }
}

long read(std::uint64_t handle, std::uint64_t buffer_address, std::int64_t frames, std::int64_t deadline) {
    const Entry entry(handle);
    Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    if (!slot->input) return -linux_errno::eperm;
    if (frames < 0) return -linux_errno::einval;
    if (frames == 0) return 0;
    auto *destination = sys::as_pointer<float>(buffer_address);
    if (destination == nullptr) return -linux_errno::efault;
    const auto channels = static_cast<std::size_t>(slot->channels);
    std::int64_t taken = 0;
    while (true) {
        {
            const std::lock_guard hold(slot->guest_lock);
            if (slot->state.load() == APKRT_AUDIO_DISCONNECTED) return taken > 0 ? taken : -linux_errno::epipe;
            const std::int64_t position = slot->read.load();
            const std::int64_t count = std::min(slot->written.load() - position, frames - taken);
            if (count > 0) {
                copy_out(*slot, position, destination + static_cast<std::size_t>(taken) * channels, count);
                // The host drops the oldest frames of a full ring by moving `read` on, and may have written over
                // what was copied: the claim fails then, and the copy is made again.
                std::int64_t expected = position;
                if (!slot->read.compare_exchange_strong(expected, position + count)) continue;
                taken += count;
                changed(*slot);
            }
        }
        if (taken == frames || passed(deadline)) return taken;
        const auto plays = [&] {
            const std::int32_t state = slot->state.load();
            return state == APKRT_AUDIO_STARTING || state == APKRT_AUDIO_STARTED;
        };
        if (!plays()) return taken;  // a stream that does not capture gives what it holds and waits for nothing
        const auto has_frames = [&] {
            return slot->state.load() == APKRT_AUDIO_DISCONNECTED || !plays() ||
                   slot->written.load() - slot->read.load() > 0;
        };
        switch (wait_on(*slot, handle, has_frames, deadline)) {
            case Woken::done:
                break;
            case Woken::closed:
                return -linux_errno::ebadf;
            case Woken::timeout:
                return taken;
            case Woken::signal:
                return taken > 0 ? taken : -linux_errno::eintr;
        }
    }
}

void revoke_input() {
    for (Slot &slot : slots) {
        const std::uint64_t handle = slot.handle.load();
        if (handle == 0 || !slot.input) continue;
        Entry entry(handle);  // the close cannot free the stream while the revoke is inside
        if (entry.slot() == nullptr) continue;
        if (slot.state.exchange(APKRT_AUDIO_DISCONNECTED) != APKRT_AUDIO_DISCONNECTED) changed(slot);
        wait_out_reads(slot);
        if (!slot.host_closed.exchange(true)) slot.host.close(slot.host.context, handle);
    }
}

long status(std::uint64_t handle, std::uint64_t address) {
    const Entry entry(handle);
    const Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    auto *out = sys::as_pointer<apkrt_audio_status>(address);
    if (out == nullptr) return -linux_errno::efault;
    apkrt_audio_status status{};
    // The sequence first: whatever changes after it changes the sequence too, so a wait for it ends at once.
    status.sequence = slot->sequence.load();
    status.state = reported(slot->state.load());
    status.frames_read = slot->read.load();  // before `written`, which never falls behind it
    status.frames_written = slot->written.load();
    status.buffer_size = slot->buffer_size.load();
    status.xruns = slot->xruns.load();
    read_stamp(*slot, status.position, status.time);
    *out = status;
    return 0;
}

long wait(std::uint64_t handle, std::uint32_t sequence, std::int64_t deadline) {
    const Entry entry(handle);
    Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    const auto another = [&] { return slot->sequence.load() != sequence; };
    if (another()) return 0;
    if (passed(deadline)) return -linux_errno::etimedout;
    switch (wait_on(*slot, handle, another, deadline)) {
        case Woken::done:
            return 0;
        case Woken::closed:
            return -linux_errno::ebadf;
        case Woken::timeout:
            return -linux_errno::etimedout;
        case Woken::signal:
            return -linux_errno::eintr;
    }
    return 0;
}

long join(std::uint64_t handle, std::uint64_t how) {
    const Entry entry(handle);
    const Slot *slot = entry.slot();
    if (slot == nullptr) return -linux_errno::ebadf;
    if (how > 1) return -linux_errno::einval;
    if (how == 0) {
        joined.leave();
        return 0;
    }
    os_workgroup_t group = nullptr;
    {
        const std::lock_guard hold(output_lock);
        group = output_workgroup;
        if (group != nullptr) os_retain(group);
    }
    if (group == nullptr) return -linux_errno::enoent;
    if (group == joined.workgroup) {
        os_release(group);
        return 0;
    }
    joined.leave();
    make_real_time(static_cast<double>(slot->burst) / slot->rate);
    if (const int error = os_workgroup_join(group, &joined.token); error != 0) {
        os_release(group);
        return -sys::to_linux_errno(error);
    }
    joined.workgroup = group;
    return 0;
}

}  // namespace apkrt::audio

using namespace apkrt::audio;

extern "C" {

int apkrt_set_audio_host(const apkrt_audio_host *host) {
    if (host != nullptr && (host->open == nullptr || host->close == nullptr)) return -1;
    const std::lock_guard hold(host_lock);
    have_host = host != nullptr;
    current_host = host != nullptr ? *host : apkrt_audio_host{};
    return 0;
}

int apkrt_set_audio_input_host(const apkrt_audio_input_host *host) {
    if (host != nullptr && (host->open == nullptr || host->close == nullptr)) return -1;
    const std::lock_guard hold(host_lock);
    have_input_host = host != nullptr;
    current_input_host = host != nullptr ? apkrt_audio_host{host->context, host->open, host->close}
                                         : apkrt_audio_host{};
    return 0;
}

int64_t apkrt_audio_input_write(uint64_t stream, const float *buffer, int64_t frames, uint64_t host_time) {
    if (buffer == nullptr || frames <= 0) return 0;
    Slot *slot = slot_of(stream);
    if (slot == nullptr) return 0;
    slot->readers.fetch_add(1);
    std::int64_t taken = 0;
    if (slot->handle.load() == stream && slot->input) taken = take_capture(*slot, buffer, frames, host_time);
    slot->readers.fetch_sub(1);
    return taken;
}

int64_t apkrt_audio_read(uint64_t stream, float *buffer, int64_t frames, uint64_t host_time) {
    if (buffer == nullptr || frames <= 0) return 0;
    Slot *slot = slot_of(stream);
    if (slot == nullptr) return 0;
    slot->readers.fetch_add(1);
    const std::int64_t taken = slot->handle.load() == stream ? take_frames(*slot, buffer, frames, host_time) : 0;
    slot->readers.fetch_sub(1);
    return taken;
}

void apkrt_audio_disconnected(uint64_t stream) {
    Slot *slot = slot_of(stream);
    if (slot == nullptr) return;
    const std::lock_guard hold(table_lock);
    if (slot->handle.load() == stream) {
        if (slot->state.exchange(APKRT_AUDIO_DISCONNECTED) != APKRT_AUDIO_DISCONNECTED) changed(*slot);
    } else if (slot->opening == stream) {
        slot->disconnected_early = true;
    }
}

void apkrt_audio_set_output(void *audio_unit, int64_t latency) {
    output_latency.store(latency);
    os_workgroup_t group = nullptr;
    if (audio_unit != nullptr) {
        // AudioToolbox is loaded wherever an AudioUnit exists: the runtime finds the getter there rather than
        // linking AudioToolbox, which the app does not.
        const auto get_property =
            reinterpret_cast<decltype(&AudioUnitGetProperty)>(dlsym(RTLD_DEFAULT, "AudioUnitGetProperty"));
        UInt32 size = sizeof group;
        if (get_property == nullptr ||
            get_property(static_cast<AudioUnit>(audio_unit), kAudioOutputUnitProperty_OSWorkgroup,
                         kAudioUnitScope_Global, 0, &group, &size) != noErr) {
            group = nullptr;
        }
    }
    const std::lock_guard hold(output_lock);
    if (output_workgroup != nullptr) os_release(output_workgroup);
    output_workgroup = group;
}

int32_t apkrt_audio_xruns(uint64_t stream) {
    Slot *slot = slot_of(stream);
    if (slot == nullptr) return 0;
    const auto ours = [&] { return slot->handle.load() == stream || slot->closing.load() == stream; };
    if (!ours()) return 0;
    const std::int32_t xruns = slot->xruns.load();
    return ours() ? xruns : 0;
}

}  // extern "C"
