// Checks the guest's video decoders (ADR-0060 e2b3): video_decoder's operations through the syscall dispatcher, as
// libmediandk's AMediaCodec makes them, against a fake table of host decoders: the IDs the runtime keeps, the limits
// and checks that keep the host sound against hostile values, what reaches the host and what comes back, and a
// DESTROY that waits for a QUEUE still in the host; a surface decoder's tokens, and its frames drawn into the guest's
// hardware buffers (ADR-0060 e2c) against a fake table of those too. graphics/host's decoders on VideoToolbox have
// their own test, the guest's codec the fixture media-codec-test against Android 15.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "APKRuntime/media_codec.h"
#include "syscall_frame.h"
#include "syscalls.h"

#include "check.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <new>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef APKRT_RUNTIME_DIR
#error "APKRT_RUNTIME_DIR names runtime/, for the NR numbers' check"
#endif

// Allocations that fail on purpose, on the thread that asks for it: those of at least `fail_from` bytes, or every one
// while `fail_all`, so that the runtime's -ENOMEM paths run without memory running out.
thread_local std::size_t fail_from = 0;
thread_local bool fail_all = false;

void *operator new(std::size_t size) {
    if (fail_all || (fail_from != 0 && size >= fail_from)) throw std::bad_alloc();
    if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

constexpr std::int64_t enomem = 12;
constexpr std::int64_t eagain = 11, ebadf = 9, ebusy = 16, efault = 14, eio = 5, einval = 22, enodev = 19, enospc = 28,
                       enosys = 38;
constexpr std::uint32_t avc = APKRT_VIDEO_DECODER_AVC, hevc = APKRT_VIDEO_DECODER_HEVC;

static_assert(sizeof(apkrt_video_decoder) == 64);
static_assert(offsetof(apkrt_video_decoder, type) == 8 && offsetof(apkrt_video_decoder, width) == 16);
static_assert(offsetof(apkrt_video_decoder, data) == 24 && offsetof(apkrt_video_decoder, size) == 32);
static_assert(offsetof(apkrt_video_decoder, time_us) == 40 && offsetof(apkrt_video_decoder, stride) == 48);
static_assert(offsetof(apkrt_video_decoder, crop_left) == 56 && offsetof(apkrt_video_decoder, crop_bottom) == 62);

std::int64_t call(std::uint64_t op, std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = op;
    frame.x[1] = argument;
    frame.x[8] = APKRT_NR_VIDEO_DECODER;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

std::int64_t call(std::uint64_t op, apkrt_video_decoder &decoder) {
    return call(op, reinterpret_cast<std::uint64_t>(&decoder));
}

/// A fake table of host decoders: each keeps the units it was given and hands out the frames a test puts in its
/// queue, with a log of the calls, for any thread.
struct Fake {
    struct Decoder {
        apkrt_video_decoder_description description{};
        std::vector<unsigned char> codec_data;
        std::vector<std::vector<unsigned char>> units;
        std::vector<std::int64_t> times;
        std::vector<std::uint32_t> flags;
        std::vector<apkrt_video_frame> frames;  // peek gives the first
        std::vector<std::uint64_t> rendered, discarded;  // the tokens
        std::vector<apkrt_video_target> targets;         // each render's
        struct WindowRender {
            std::uint64_t token, window;
            std::int64_t time_ns;
            std::uint32_t flags;
        };
        std::vector<WindowRender> window_renders;  // each render_window's
        std::vector<std::vector<unsigned char>> frame_bytes;
        int flushes = 0;
        bool destroyed = false;
        int calls_running = 0;  // calls inside the fake, to catch a destroy among them
    };
    std::mutex mutex;
    std::map<std::uint64_t, Decoder> decoders;
    std::uint64_t next = 500;
    int creates = 0, destroys = 0, overlaps = 0;
    int refuse_create = 0, queue_answer = 0, peek_answer = 0, render_answer = 0, flush_answer = 0;  // when not 0
    int window_answer = 0;
    bool fail_after_create = false;  // the runtime's allocations fail once the host's decoder is made
    // A queue waits here while `hold_queue`, until released, so that a destroy can race it.
    bool hold_queue = false, queue_entered = false;
    std::condition_variable queue_changed;

    void reset() {
        const std::lock_guard hold(mutex);
        decoders.clear();
        creates = destroys = overlaps = refuse_create = queue_answer = peek_answer = render_answer = flush_answer = 0;
        fail_after_create = false;
        hold_queue = queue_entered = false;
    }

    /// A frame of `width` x `height` with the time, its NV12 bytes a pattern, rows of `stride` bytes, `slice` of them
    /// a plane (0: the height); `extra` bytes of the host's own after the frame's, which the guest must never get.
    void add_frame(std::uint64_t handle, std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                   std::int64_t time, std::uint32_t flags = 0, std::uint32_t slice = 0, std::size_t extra = 0) {
        const std::lock_guard hold(mutex);
        Decoder &decoder = decoders.at(handle);
        if (slice == 0) slice = height;
        const std::size_t size = flags != 0 ? 0 : std::size_t{stride} * (slice + (slice + 1) / 2);
        std::vector<unsigned char> bytes(size + extra);
        for (std::size_t at = 0; at < bytes.size(); at++) {
            bytes[at] = at < size ? static_cast<unsigned char>(at * 7 + static_cast<std::size_t>(time)) : 0x5a;
        }
        decoder.frame_bytes.push_back(std::move(bytes));
        apkrt_video_frame frame{};
        frame.size = size;
        frame.width = width, frame.height = height, frame.stride = stride, frame.slice_height = slice;
        frame.crop_right = static_cast<std::uint16_t>(width - 1);
        frame.crop_bottom = static_cast<std::uint16_t>(height - 1);
        frame.time_us = time, frame.flags = flags;
        decoder.frames.push_back(frame);
    }

    /// A surface decoder's frame of `width` x `height` with the time and its token, no bytes.
    void add_surface_frame(std::uint64_t handle, std::uint32_t width, std::uint32_t height, std::int64_t time,
                           std::uint64_t token) {
        const std::lock_guard hold(mutex);
        Decoder &decoder = decoders.at(handle);
        decoder.frame_bytes.emplace_back();
        apkrt_video_frame frame{};
        frame.width = width, frame.height = height;
        frame.crop_right = static_cast<std::uint16_t>(width - 1);
        frame.crop_bottom = static_cast<std::uint16_t>(height - 1);
        frame.time_us = time, frame.token = token;
        decoder.frames.push_back(frame);
    }
};

Fake fake;

int fake_create(void *context, const apkrt_video_decoder_description *description, std::uint64_t *handle) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.creates++;
    if (self.refuse_create != 0) return self.refuse_create;
    *handle = self.next++;
    Fake::Decoder &decoder = self.decoders[*handle];
    decoder.description = *description;
    const auto *bytes = static_cast<const unsigned char *>(description->codec_data);
    decoder.codec_data.assign(bytes, bytes + description->codec_data_size);
    decoder.description.codec_data = nullptr;
    if (self.fail_after_create) fail_all = true;
    return 0;
}

int fake_queue(void *context, std::uint64_t handle, const void *data, std::size_t size, std::int64_t time,
               std::uint32_t flags) {
    auto &self = *static_cast<Fake *>(context);
    std::unique_lock hold(self.mutex);
    Fake::Decoder &decoder = self.decoders.at(handle);
    if (decoder.destroyed) self.overlaps++;
    decoder.calls_running++;
    if (self.hold_queue) {
        self.queue_entered = true;
        self.queue_changed.notify_all();
        self.queue_changed.wait(hold, [&] { return !self.hold_queue; });
    }
    const auto *bytes = static_cast<const unsigned char *>(data);
    decoder.units.emplace_back(bytes, bytes + size);
    decoder.times.push_back(time);
    decoder.flags.push_back(flags);
    decoder.calls_running--;
    return self.queue_answer;
}

int fake_peek(void *context, std::uint64_t handle, apkrt_video_frame *frame) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    Fake::Decoder &decoder = self.decoders.at(handle);
    if (self.peek_answer != 0) return self.peek_answer;
    if (decoder.frames.empty()) return -static_cast<int>(eagain);
    *frame = decoder.frames.front();
    frame->data = frame->token != 0 || decoder.frame_bytes.front().empty() ? nullptr
                                                                          : decoder.frame_bytes.front().data();
    return 0;
}

void fake_pop(void *context, std::uint64_t handle) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    Fake::Decoder &decoder = self.decoders.at(handle);
    decoder.frames.erase(decoder.frames.begin());
    decoder.frame_bytes.erase(decoder.frame_bytes.begin());
}

int fake_flush(void *context, std::uint64_t handle) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    Fake::Decoder &decoder = self.decoders.at(handle);
    decoder.flushes++;
    if (self.flush_answer != 0) return self.flush_answer;
    decoder.frames.clear();
    decoder.frame_bytes.clear();
    return 0;
}

void fake_destroy(void *context, std::uint64_t handle) {
    fail_all = false;
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    Fake::Decoder &decoder = self.decoders.at(handle);
    if (decoder.calls_running != 0) self.overlaps++;
    decoder.destroyed = true;
    self.destroys++;
}

int fake_render(void *context, std::uint64_t handle, std::uint64_t token, const apkrt_video_target *target) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    Fake::Decoder &decoder = self.decoders.at(handle);
    decoder.rendered.push_back(token);
    decoder.targets.push_back(*target);
    return self.render_answer;
}

int fake_render_window(void *context, std::uint64_t handle, std::uint64_t token, std::uint64_t window,
                       std::int64_t time_ns, std::uint32_t flags) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.decoders.at(handle).window_renders.push_back({token, window, time_ns, flags});
    return self.window_answer;
}

void fake_discard(void *context, std::uint64_t handle, std::uint64_t token) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.decoders.at(handle).discarded.push_back(token);
}

const apkrt_video_decoders table{fake_create, fake_queue,  fake_peek,    fake_pop,           fake_flush,
                                 fake_destroy, fake_render, fake_discard, fake_render_window, &fake};

/// A fake table of hardware buffers for the targets: handles from 900, each buffer's frees counted.
struct FakeBuffers {
    std::mutex mutex;
    std::uint64_t next = 900;
    std::map<std::uint64_t, int> frees;
};

FakeBuffers buffers;

int buffers_supported(void *, const apkrt_hardware_buffer_description *) {
    return 1;
}

int buffers_allocate(void *context, const apkrt_hardware_buffer_description *description, std::uint64_t *handle,
                     std::uint32_t *stride) {
    auto &self = *static_cast<FakeBuffers *>(context);
    const std::lock_guard hold(self.mutex);
    *handle = self.next++;
    *stride = description->width;
    return 0;
}

int buffers_lock(void *, std::uint64_t, std::uint64_t, void **, std::uint32_t *, std::uint32_t *) {
    return -static_cast<int>(einval);
}

int buffers_unlock(void *, std::uint64_t) {
    return 0;
}

void buffers_free(void *context, std::uint64_t handle) {
    auto &self = *static_cast<FakeBuffers *>(context);
    const std::lock_guard hold(self.mutex);
    self.frees[handle]++;
}

const apkrt_hardware_buffers buffers_table{buffers_supported, buffers_allocate, buffers_lock, buffers_unlock,
                                           buffers_free, &buffers};

/// A hardware buffer of the guest's, YUV 420 (or `format`), through hardware_buffer's ALLOCATE: its ID.
std::uint64_t guest_buffer(std::uint32_t layers = 1, std::uint32_t format = 0x23) {
    CHECK(apkrt_set_hardware_buffers(&buffers_table) == 0);
    apkrt_hardware_buffer buffer{};
    buffer.width = 320, buffer.height = 180, buffer.layers = layers, buffer.format = format;
    buffer.usage = 0x300;  // GPU sampled and framebuffer
    apkrt_syscall_frame frame{};
    frame.x[0] = APKRT_HARDWARE_BUFFER_ALLOCATE;
    frame.x[1] = reinterpret_cast<std::uint64_t>(&buffer);
    frame.x[8] = APKRT_NR_HARDWARE_BUFFER;
    apkrt_syscall_dispatch(&frame);
    CHECK(frame.x[0] == 0 && buffer.id != 0);
    return buffer.id;
}

/// hardware_buffer's RELEASE of the guest's reference.
void release_guest_buffer(std::uint64_t id) {
    apkrt_syscall_frame frame{};
    frame.x[0] = APKRT_HARDWARE_BUFFER_RELEASE;
    frame.x[1] = id;
    frame.x[8] = APKRT_NR_HARDWARE_BUFFER;
    apkrt_syscall_dispatch(&frame);
    CHECK(frame.x[0] == 0);
}

void use_fake() {
    fake.reset();
    CHECK(apkrt_set_video_decoders(&table) == 0);
}

apkrt_video_decoder asked(std::uint32_t type = avc, std::uint32_t width = 320, std::uint32_t height = 180) {
    apkrt_video_decoder decoder{};
    decoder.type = type, decoder.width = width, decoder.height = height;
    return decoder;
}

/// A decoder made through CREATE: its ID, and the fake's handle (the newest).
std::uint64_t create(std::uint64_t *handle = nullptr, std::uint32_t type = avc, std::uint32_t flags = 0) {
    apkrt_video_decoder decoder = asked(type);
    decoder.flags = flags;
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == 0);
    if (handle != nullptr) {
        const std::lock_guard hold(fake.mutex);
        *handle = fake.decoders.rbegin()->first;
    }
    return decoder.id;
}

/// A page the guest cannot read or write.
std::uint64_t no_access_page() {
    static void *page = mmap(nullptr, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return reinterpret_cast<std::uint64_t>(page);
}

TEST(the_nr_number_is_the_only_one_of_its_value) {
    std::map<long, std::set<std::string>> numbers;
    const std::regex define(R"(#define\s+(APKRT_NR_\w+)\s+(\d+))");
    for (const char *name : {"include/APKRuntime/guest_interface.h", "src/loader_syscalls.h"}) {
        std::ifstream file(std::string(APKRT_RUNTIME_DIR) + "/" + name);
        CHECK(file.good());
        std::stringstream text;
        text << file.rdbuf();
        const std::string all = text.str();
        for (std::sregex_iterator it(all.begin(), all.end(), define), end; it != end; ++it) {
            numbers[std::stol((*it)[2].str())].insert((*it)[1].str());
        }
    }
    CHECK(numbers.size() > 10);
    CHECK(numbers.count(APKRT_NR_VIDEO_DECODER) == 1 && numbers[APKRT_NR_VIDEO_DECODER].size() == 1);
    for (const auto &[number, names] : numbers) CHECK(names.size() == 1);
}

TEST(without_host_decoders_nothing_is_made) {
    CHECK(apkrt_set_video_decoders(nullptr) == 0);
    apkrt_video_decoder decoder = asked();
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -enosys);
    CHECK(decoder.id == 0);
}

TEST(a_table_with_a_function_missing_is_refused) {
    for (int missing = 0; missing < 9; missing++) {
        apkrt_video_decoders partial = table;
        void **functions[] = {reinterpret_cast<void **>(&partial.create), reinterpret_cast<void **>(&partial.queue),
                              reinterpret_cast<void **>(&partial.peek), reinterpret_cast<void **>(&partial.pop),
                              reinterpret_cast<void **>(&partial.flush), reinterpret_cast<void **>(&partial.destroy),
                              reinterpret_cast<void **>(&partial.render),
                              reinterpret_cast<void **>(&partial.discard),
                              reinterpret_cast<void **>(&partial.render_window)};
        *functions[missing] = nullptr;
        CHECK(apkrt_set_video_decoders(&partial) == -1);
    }
}

TEST(create_gives_ids_never_0_nor_reused_and_the_host_gets_the_description) {
    use_fake();
    const unsigned char codec_data[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xce};
    apkrt_video_decoder decoder = asked(hevc, 1920, 1080);
    decoder.data = reinterpret_cast<std::uint64_t>(codec_data);
    decoder.size = sizeof codec_data;
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == 0);
    const std::uint64_t first = decoder.id;
    CHECK(first != 0);
    {
        const std::lock_guard hold(fake.mutex);
        const Fake::Decoder &made = fake.decoders.begin()->second;
        CHECK(made.description.type == hevc && made.description.width == 1920 && made.description.height == 1080);
        CHECK(made.codec_data == std::vector<unsigned char>(codec_data, codec_data + sizeof codec_data));
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, first) == 0);
    const std::uint64_t second = create();
    CHECK(second != 0 && second != first);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, first) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, second) == 0);
}

TEST(hostile_descriptions_never_reach_the_host) {
    use_fake();
    const std::uint32_t big = APKRT_VIDEO_DECODER_MAX_DIMENSION;
    for (const apkrt_video_decoder &bad : {asked(0), asked(3), asked(avc, 0, 180), asked(avc, 320, 0),
                                           asked(avc, big + 1, 180), asked(avc, 320, big + 1),
                                           asked(avc, 0xffffffffu, 0xffffffffu)}) {
        apkrt_video_decoder decoder = bad;
        CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -einval);
        CHECK(decoder.id == 0);
    }
    std::vector<unsigned char> codec_data(APKRT_VIDEO_DECODER_MAX_CODEC_DATA + 1);
    apkrt_video_decoder decoder = asked();
    decoder.data = reinterpret_cast<std::uint64_t>(codec_data.data());
    decoder.size = codec_data.size();
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -einval);
    decoder.size = UINT64_MAX;
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -einval);
    decoder.data = no_access_page();
    decoder.size = 16;
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -efault);
    decoder.data = 0;
    decoder.size = 16;
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -efault);
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, no_access_page()) == -efault);
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, 0) == -efault);
    CHECK(call(99, 0) == -einval);
    apkrt_video_decoder largest = asked(avc, big, big);
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, largest) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, largest.id) == 0);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.creates == 1);
}

TEST(a_process_has_at_most_its_decoders_at_once) {
    use_fake();
    std::vector<std::uint64_t> ids;
    for (std::uint32_t n = 0; n < APKRT_VIDEO_DECODER_MAX_DECODERS; n++) ids.push_back(create());
    apkrt_video_decoder one_more = asked();
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, one_more) == -ebusy);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, ids.back()) == 0);
    ids.pop_back();
    ids.push_back(create());
    for (const std::uint64_t id : ids) CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(the_hosts_refusal_comes_back_and_leaves_no_decoder) {
    use_fake();
    fake.refuse_create = -static_cast<int>(einval);
    apkrt_video_decoder decoder = asked();
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -einval);
    CHECK(decoder.id == 0);
    fake.refuse_create = 12345;
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -eio);
    fake.refuse_create = 0;
    for (std::uint32_t n = 0; n < APKRT_VIDEO_DECODER_MAX_DECODERS; n++) {
        const std::uint64_t id = create();
        CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
    }
}

TEST(queue_copies_the_guests_unit_to_the_host) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    unsigned char unit[] = {0, 0, 0, 1, 0x65, 1, 2, 3};
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(unit), decoder.size = sizeof unit;
    decoder.time_us = 33333;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == 0);
    unit[4] = 0;  // the host has its own copy
    decoder.data = 0, decoder.size = 0, decoder.flags = APKRT_VIDEO_DECODER_END_OF_STREAM, decoder.time_us = 0;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        const Fake::Decoder &made = fake.decoders.at(handle);
        CHECK(made.units.size() == 2 && made.units[0].size() == 8 && made.units[0][4] == 0x65 &&
              made.units[1].empty());
        CHECK(made.times[0] == 33333 && made.flags[0] == 0 && made.flags[1] == APKRT_VIDEO_DECODER_END_OF_STREAM);
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(hostile_units_never_reach_the_host) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    std::vector<unsigned char> huge(APKRT_VIDEO_DECODER_MAX_UNIT + 1);
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(huge.data()), decoder.size = huge.size();
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -einval);
    decoder.size = UINT64_MAX;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -einval);
    decoder.data = no_access_page(), decoder.size = 100;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -efault);
    decoder.data = no_access_page() + 16383, decoder.size = 2;  // runs past the guest's memory
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -efault);
    decoder.data = UINT64_MAX - 3, decoder.size = 16;  // wraps
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -efault);
    decoder.data = 0, decoder.size = 4;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -efault);
    decoder.data = reinterpret_cast<std::uint64_t>(huge.data()), decoder.size = 4;
    decoder.id = id + 1000;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -ebadf);
    decoder.id = 0;
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, no_access_page()) == -efault);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).units.empty());
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(dequeue_copies_the_oldest_frame_to_the_guest) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    std::vector<unsigned char> buffer(320 * 270);
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(buffer.data()), decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eagain);
    fake.add_frame(handle, 320, 180, 320, 66667);
    fake.add_frame(handle, 320, 180, 320, 100000);
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    CHECK(decoder.size == 320 * 270 && decoder.width == 320 && decoder.height == 180 && decoder.stride == 320);
    CHECK(decoder.slice_height == 180 && decoder.crop_right == 319 && decoder.crop_bottom == 179);
    CHECK(decoder.time_us == 66667 && decoder.flags == 0);
    bool same = true;
    for (std::size_t at = 0; at < buffer.size(); at++) {
        same = same && buffer[at] == static_cast<unsigned char>(at * 7 + 66667);
    }
    CHECK(same);
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.time_us == 100000);
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eagain);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(a_buffer_too_small_or_unwritable_keeps_the_frame) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    fake.add_frame(handle, 64, 48, 64, 7);
    std::vector<unsigned char> small(64 * 72 - 1), right(64 * 72);
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(small.data()), decoder.size = small.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -enospc);
    CHECK(decoder.size == 64 * 72 && decoder.stride == 64 && decoder.slice_height == 48);
    decoder.data = no_access_page(), decoder.size = 64 * 72;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -efault);
    decoder.data = 0;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -efault);
    decoder.data = reinterpret_cast<std::uint64_t>(right.data()), decoder.size = right.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.time_us == 7);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(the_end_of_the_stream_comes_with_size_0) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    fake.add_frame(handle, 64, 48, 64, 0, APKRT_VIDEO_DECODER_END_OF_STREAM);
    apkrt_video_decoder decoder{};
    decoder.id = id;  // no buffer is needed for it
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    CHECK(decoder.size == 0 && decoder.flags == APKRT_VIDEO_DECODER_END_OF_STREAM);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(a_larger_buffer_gets_the_frames_bytes_alone) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    fake.add_frame(handle, 64, 48, 64, 3, 0, 0, 4096);  // the host's memory goes on after the frame
    std::vector<unsigned char> buffer(64 * 72 + 4096, 0xa5);
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(buffer.data()), decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    CHECK(decoder.size == 64 * 72 && decoder.time_us == 3);
    bool kept = true;
    for (std::size_t at = 64 * 72; at < buffer.size(); at++) kept = kept && buffer[at] == 0xa5;
    CHECK(kept);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(rows_beyond_the_limits_never_reach_the_guest) {
    // Each limit alone, the frame's size the rows' own, so that no other check refuses it.
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    const std::uint32_t wide = APKRT_VIDEO_DECODER_MAX_DIMENSION * 4 + 1, tall = APKRT_VIDEO_DECODER_MAX_DIMENSION + 1;
    std::vector<unsigned char> buffer(std::size_t{wide} * 72 + std::size_t{64} * (tall + (tall + 1) / 2));
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(buffer.data()), decoder.size = buffer.size();
    fake.add_frame(handle, 64, 48, wide, 1);  // rows wider than the limit
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &made = fake.decoders.at(handle);
        made.frames.clear(), made.frame_bytes.clear();
    }
    fake.add_frame(handle, 64, 48, 64, 2, 0, tall);  // a plane of more rows than the limit
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(a_frame_the_host_describes_wrongly_never_reaches_the_guest) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    std::vector<unsigned char> buffer(1 << 20);
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(buffer.data()), decoder.size = buffer.size();
    // Rows narrower than the width, a size below the rows', a crop outside the frame, a size above the limits.
    fake.add_frame(handle, 64, 48, 32, 1);
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.stride = 64, frame.size = 64 * 72 - 1;
    }
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.size = 64 * 72, frame.crop_right = 64;
    }
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.crop_right = 63, frame.width = APKRT_VIDEO_DECODER_MAX_DIMENSION + 1;
    }
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.width = 64, frame.stride = 0xffffffffu, frame.slice_height = 0xffffffffu;
    }
    decoder.size = buffer.size();
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    fake.peek_answer = 777;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    fake.peek_answer = 0;
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(flush_and_destroy_go_to_the_host_by_id) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    fake.add_frame(handle, 64, 48, 64, 1);
    CHECK(call(APKRT_VIDEO_DECODER_FLUSH, id) == 0);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eagain);
    CHECK(call(APKRT_VIDEO_DECODER_FLUSH, id + 1) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_FLUSH, id) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == -ebadf);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.decoders.at(handle).flushes == 1 && fake.destroys == 1);
}

TEST(a_decoder_keeps_the_table_that_made_it) {
    use_fake();
    const std::uint64_t id = create();
    CHECK(apkrt_set_video_decoders(nullptr) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.destroys == 1);
}

TEST(memory_the_runtime_cannot_get_answers_enomem_and_leaves_no_decoder) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    std::vector<unsigned char> unit(9 << 20);  // a legal access unit of 9 MiB, whose copy cannot be had
    apkrt_video_decoder decoder{};
    decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(unit.data()), decoder.size = unit.size();
    fail_from = 8 << 20;
    const std::int64_t queued = call(APKRT_VIDEO_DECODER_QUEUE, decoder);
    fail_from = 0;
    CHECK(queued == -enomem);
    CHECK(call(APKRT_VIDEO_DECODER_QUEUE, decoder) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
    fake.fail_after_create = true;  // the decoder's entry cannot be had after the host made it
    apkrt_video_decoder made = asked();
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, made) == -enomem);
    CHECK(made.id == 0);
    fake.fail_after_create = false;
    CHECK(!fail_all);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.creates == 2 && fake.destroys == 2);  // the second one's too
}

TEST(destroy_waits_for_a_queue_still_in_the_host) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle);
    {
        const std::lock_guard hold(fake.mutex);
        fake.hold_queue = true;
    }
    unsigned char unit[16] = {};
    std::atomic<std::int64_t> queued{-1}, destroyed{-1};
    std::thread queuing([&] {
        apkrt_video_decoder decoder{};
        decoder.id = id, decoder.data = reinterpret_cast<std::uint64_t>(unit), decoder.size = sizeof unit;
        queued = call(APKRT_VIDEO_DECODER_QUEUE, decoder);
    });
    {
        std::unique_lock hold(fake.mutex);
        fake.queue_changed.wait(hold, [] { return fake.queue_entered; });
    }
    std::thread destroying([&] { destroyed = call(APKRT_VIDEO_DECODER_DESTROY, id); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.destroys == 0);  // still waiting for the queue
    }
    // A call that comes after the destroy began finds no decoder.
    apkrt_video_decoder late{};
    late.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, late) == -ebadf);
    {
        const std::lock_guard hold(fake.mutex);
        fake.hold_queue = false;
    }
    fake.queue_changed.notify_all();
    queuing.join();
    destroying.join();
    CHECK(queued == 0 && destroyed == 0);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.destroys == 1 && fake.overlaps == 0);
}

TEST(a_destroy_still_waiting_counts_against_the_limit) {
    // The registry's entry goes before the host's decoder does: until the host destroyed it, the process still has
    // the host's decoders at their limit.
    use_fake();
    std::vector<std::uint64_t> ids;
    for (std::uint32_t n = 0; n < APKRT_VIDEO_DECODER_MAX_DECODERS; n++) ids.push_back(create());
    {
        const std::lock_guard hold(fake.mutex);
        fake.hold_queue = true;
    }
    unsigned char unit[16] = {};
    std::atomic<std::int64_t> queued{-1}, destroyed{-1};
    std::thread queuing([&] {
        apkrt_video_decoder decoder{};
        decoder.id = ids.back(), decoder.data = reinterpret_cast<std::uint64_t>(unit), decoder.size = sizeof unit;
        queued = call(APKRT_VIDEO_DECODER_QUEUE, decoder);
    });
    {
        std::unique_lock hold(fake.mutex);
        fake.queue_changed.wait(hold, [] { return fake.queue_entered; });
    }
    std::thread destroying([&] { destroyed = call(APKRT_VIDEO_DECODER_DESTROY, ids.back()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the destroy takes the entry, then waits
    apkrt_video_decoder one_more = asked();
    CHECK(call(APKRT_VIDEO_DECODER_CREATE, one_more) == -ebusy);
    if (one_more.id != 0) ids.push_back(one_more.id);
    {
        const std::lock_guard hold(fake.mutex);
        fake.hold_queue = false;
    }
    fake.queue_changed.notify_all();
    queuing.join();
    destroying.join();
    CHECK(queued == 0 && destroyed == 0);
    ids.erase(ids.begin() + APKRT_VIDEO_DECODER_MAX_DECODERS - 1);
    ids.push_back(create());
    for (const std::uint64_t id : ids) CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.creates == fake.destroys);
}

constexpr std::uint32_t surface = APKRT_VIDEO_DECODER_SURFACE;

TEST(a_surface_decoder_hands_out_tokens_not_bytes) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).description.flags == surface);
    }
    fake.add_surface_frame(handle, 320, 180, 33333, 77);
    apkrt_video_decoder decoder{};
    decoder.id = id;  // no buffer: the frame stays the host's
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    CHECK(decoder.data == 77 && decoder.size == 0 && decoder.stride == 0 && decoder.slice_height == 0);
    CHECK(decoder.width == 320 && decoder.height == 180 && decoder.crop_right == 319 && decoder.crop_bottom == 179);
    CHECK(decoder.time_us == 33333 && decoder.flags == 0);
    fake.add_frame(handle, 64, 48, 64, 0, APKRT_VIDEO_DECODER_END_OF_STREAM);
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    CHECK(decoder.flags == APKRT_VIDEO_DECODER_END_OF_STREAM && decoder.size == 0 && decoder.data == 0);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(create_refuses_flags_it_does_not_have) {
    use_fake();
    for (const std::uint32_t flags : {1u, 4u, surface | 8u, 0x80000000u}) {
        apkrt_video_decoder decoder = asked();
        decoder.flags = flags;
        CHECK(call(APKRT_VIDEO_DECODER_CREATE, decoder) == -einval && decoder.id == 0);
    }
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.creates == 0);
}

TEST(render_draws_a_waiting_frame_into_the_guests_hardware_buffer) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    const std::uint64_t target = guest_buffer();
    fake.add_surface_frame(handle, 320, 180, 0, 77);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 77);
    decoder.size = target;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, decoder) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        const Fake::Decoder &made = fake.decoders.at(handle);
        CHECK(made.rendered == std::vector<std::uint64_t>{77} && made.targets.size() == 1);
        CHECK(made.targets[0].buffers == &buffers && made.targets[0].handle >= 900);
        CHECK(made.targets[0].description.width == 320 && made.targets[0].description.height == 180);
        CHECK(made.targets[0].description.format == 0x23 && made.targets[0].description.layers == 1);
    }
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, decoder) == -einval);  // the token has ended
    std::uint64_t buffer_handle = 0;
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).rendered.size() == 1);
        buffer_handle = fake.decoders.at(handle).targets[0].handle;
    }
    release_guest_buffer(target);  // the guest's last reference: the runtime's of the render is gone already
    {
        const std::lock_guard hold(buffers.mutex);
        CHECK(buffers.frees[buffer_handle] == 1);
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(hostile_renders_never_reach_the_host) {
    use_fake();
    std::uint64_t handle = 0, plain_handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    const std::uint64_t plain = create(&plain_handle);
    const std::uint64_t target = guest_buffer(), layered = guest_buffer(2);
    for (std::uint64_t token = 1; token <= 4; token++) fake.add_surface_frame(handle, 320, 180, 0, token);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    for (int n = 0; n < 4; n++) CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    apkrt_video_decoder bad{};
    bad.id = plain, bad.data = 1, bad.size = target;  // not a surface decoder
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval && call(APKRT_VIDEO_DECODER_DISCARD, bad) == -einval);
    bad.id = id, bad.data = 99;  // a token that does not wait
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval && call(APKRT_VIDEO_DECODER_DISCARD, bad) == -einval);
    bad.data = 0;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval);
    bad.id = id + 1000, bad.data = 1;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, no_access_page()) == -efault);
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, 0) == -efault);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).rendered.empty() && fake.decoders.at(handle).discarded.empty());
    }
    // A buffer that is not there, or of two layers: refused, and the token ends, its frame discarded.
    bad.id = id, bad.data = 1, bad.size = target + 1000;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval);
    bad.data = 2, bad.size = layered;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval);
    bad.data = 3, bad.size = 0;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval);
    bad.data = 1, bad.size = target;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -einval);  // ended
    fake.render_answer = 12345;
    bad.data = 4;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, bad) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).rendered == std::vector<std::uint64_t>{4});
        CHECK((fake.decoders.at(handle).discarded == std::vector<std::uint64_t>{1, 2, 3}));
    }
    // The runtime's references of the refused and failed renders are gone: the guest's releases free both buffers.
    std::size_t freed = 0;
    {
        const std::lock_guard hold(buffers.mutex);
        for (const auto &[buffer_handle, frees] : buffers.frees) freed += static_cast<std::size_t>(frees);
    }
    release_guest_buffer(target);
    release_guest_buffer(layered);
    {
        const std::lock_guard hold(buffers.mutex);
        std::size_t now = 0;
        for (const auto &[buffer_handle, frees] : buffers.frees) now += static_cast<std::size_t>(frees);
        CHECK(now == freed + 2);
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0 && call(APKRT_VIDEO_DECODER_DESTROY, plain) == 0);
}

TEST(render_window_draws_a_waiting_frame_into_a_host_window) {
    // ADR-0060 e2c3: the window's ID, the time in nanoseconds and the scaling reach the host; the token ends.
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    fake.add_surface_frame(handle, 320, 180, 0, 77);
    fake.add_surface_frame(handle, 320, 180, 33333, 78);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 77);
    decoder.size = 42, decoder.time_us = 1234, decoder.flags = APKRT_VIDEO_DECODER_CROP;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, decoder) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, decoder) == -einval);  // the token has ended
    decoder.size = 0, decoder.time_us = 0, decoder.flags = 0;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 78);
    decoder.size = 43, decoder.time_us = 0, decoder.flags = 0;  // at once
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, decoder) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        const auto &renders = fake.decoders.at(handle).window_renders;
        CHECK(renders.size() == 2);
        if (renders.size() == 2) {
            CHECK(renders[0].token == 77 && renders[0].window == 42 && renders[0].time_ns == 1234000 &&
                  renders[0].flags == APKRT_VIDEO_DECODER_CROP);
            CHECK(renders[1].token == 78 && renders[1].window == 43 && renders[1].time_ns == 0 &&
                  renders[1].flags == 0);
        }
        CHECK(fake.decoders.at(handle).rendered.empty() && fake.decoders.at(handle).discarded.empty());
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(hostile_window_renders_never_reach_the_host) {
    use_fake();
    std::uint64_t handle = 0, plain_handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    const std::uint64_t plain = create(&plain_handle);
    for (std::uint64_t token = 1; token <= 6; token++) fake.add_surface_frame(handle, 320, 180, 0, token);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    for (int n = 0; n < 6; n++) CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    apkrt_video_decoder bad{};
    bad.id = plain, bad.data = 1, bad.size = 42;  // not a surface decoder
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -einval);
    bad.id = id, bad.data = 99;  // a token that does not wait
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -einval);
    bad.id = id + 1000, bad.data = 1;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -ebadf);
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, no_access_page()) == -efault);
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, 0) == -efault);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).window_renders.empty() && fake.decoders.at(handle).discarded.empty());
    }
    // No window, flags it does not have, a time before 0 or past what nanoseconds hold: refused, and the token
    // ends, its frame discarded.
    bad.id = id, bad.data = 1, bad.size = 0;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -enodev);
    bad.data = 2, bad.size = 42, bad.flags = APKRT_VIDEO_DECODER_CROP | 8u;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -einval);
    bad.data = 3, bad.flags = 0, bad.time_us = -1;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -einval);
    bad.data = 4, bad.time_us = INT64_MAX / 1000 + 1;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -einval);
    bad.data = 1, bad.time_us = 0;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -einval);  // ended
    // The host's answers: a window it does not have, and one outside the errnos.
    fake.window_answer = -enodev;
    bad.data = 5;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -enodev);
    fake.window_answer = 12345;
    bad.data = 6, bad.time_us = INT64_MAX / 1000;  // the last time nanoseconds hold reaches the host
    CHECK(call(APKRT_VIDEO_DECODER_RENDER_WINDOW, bad) == -eio);
    fake.window_answer = 0;
    {
        const std::lock_guard hold(fake.mutex);
        const auto &made = fake.decoders.at(handle);
        CHECK(made.window_renders.size() == 2);
        if (made.window_renders.size() == 2) {
            CHECK(made.window_renders[0].token == 5 && made.window_renders[1].token == 6);
            CHECK(made.window_renders[1].time_ns == INT64_MAX / 1000 * 1000);
        }
        CHECK((made.discarded == std::vector<std::uint64_t>{1, 2, 3, 4}));
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0 && call(APKRT_VIDEO_DECODER_DESTROY, plain) == 0);
}

TEST(discard_ends_a_token_without_drawing) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    fake.add_surface_frame(handle, 320, 180, 0, 5);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 5);
    CHECK(call(APKRT_VIDEO_DECODER_DISCARD, decoder) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_DISCARD, decoder) == -einval);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.decoders.at(handle).discarded == std::vector<std::uint64_t>{5});
    CHECK(fake.decoders.at(handle).rendered.empty());
}

TEST(at_most_eight_frames_wait_for_their_render) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    for (std::uint64_t token = 1; token <= APKRT_VIDEO_DECODER_MAX_WAITING + 1; token++) {
        fake.add_surface_frame(handle, 320, 180, static_cast<std::int64_t>(token), token);
    }
    apkrt_video_decoder decoder{};
    decoder.id = id;
    for (std::uint32_t n = 0; n < APKRT_VIDEO_DECODER_MAX_WAITING; n++) {
        CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0);
    }
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eagain);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).frames.size() == 1);  // the ninth stays the host's
    }
    decoder.data = 3;
    CHECK(call(APKRT_VIDEO_DECODER_DISCARD, decoder) == 0);
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == APKRT_VIDEO_DECODER_MAX_WAITING + 1);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(a_surface_frame_the_host_describes_wrongly_never_reaches_the_guest) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    fake.add_surface_frame(handle, 320, 180, 0, 0);  // no token
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.token = 6, frame.size = 100;  // bytes after all
    }
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.size = 0, frame.crop_right = 320;  // a crop outside
    }
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        auto &frame = fake.decoders.at(handle).frames.front();
        frame.crop_right = 319, frame.width = APKRT_VIDEO_DECODER_MAX_DIMENSION + 1;
    }
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    {
        const std::lock_guard hold(fake.mutex);
        fake.decoders.at(handle).frames.front().width = 320;
    }
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 6);
    fake.add_surface_frame(handle, 320, 180, 0, 6);  // a token that waits already
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == -eio);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(a_flush_ends_every_token) {
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    const std::uint64_t target = guest_buffer();
    fake.add_surface_frame(handle, 320, 180, 0, 8);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 8);
    CHECK(call(APKRT_VIDEO_DECODER_FLUSH, id) == 0);
    decoder.size = target;
    CHECK(call(APKRT_VIDEO_DECODER_RENDER, decoder) == -einval);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).rendered.empty());
    }
    release_guest_buffer(target);
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(media_times_saturate) {
    // libmediandk's and the framework's frame times (media_codec.h): microseconds to nanoseconds, clamped where a
    // guest's time would overflow.
    CHECK(minato_media_time_ns(0) == 0 && minato_media_time_ns(33333) == 33333000);
    CHECK(minato_media_time_ns(-5) == -5000);
    CHECK(minato_media_time_ns(INT64_MAX / 1000) == INT64_MAX / 1000 * 1000);
    CHECK(minato_media_time_ns(INT64_MAX / 1000 + 1) == INT64_MAX);
    CHECK(minato_media_time_ns(INT64_MAX) == INT64_MAX);
    CHECK(minato_media_time_ns(INT64_MIN / 1000) == INT64_MIN / 1000 * 1000);
    CHECK(minato_media_time_ns(INT64_MIN / 1000 - 1) == INT64_MIN);
    CHECK(minato_media_time_ns(INT64_MIN) == INT64_MIN);
}

TEST(a_failed_flush_keeps_the_tokens) {
    // The guest keeps its output buffers when its flush fails, so their tokens still reach the host's discard.
    use_fake();
    std::uint64_t handle = 0;
    const std::uint64_t id = create(&handle, avc, surface);
    fake.add_surface_frame(handle, 320, 180, 0, 8);
    apkrt_video_decoder decoder{};
    decoder.id = id;
    CHECK(call(APKRT_VIDEO_DECODER_DEQUEUE, decoder) == 0 && decoder.data == 8);
    fake.flush_answer = -eio;
    CHECK(call(APKRT_VIDEO_DECODER_FLUSH, id) == -eio);
    fake.flush_answer = 0;
    CHECK(call(APKRT_VIDEO_DECODER_DISCARD, decoder) == 0);
    {
        const std::lock_guard hold(fake.mutex);
        CHECK(fake.decoders.at(handle).discarded == std::vector<std::uint64_t>{8});
    }
    CHECK(call(APKRT_VIDEO_DECODER_DESTROY, id) == 0);
}

TEST(many_threads_queue_dequeue_and_destroy_at_once) {
    // Eight makers create, use and destroy decoders while eight racers queue and dequeue on whatever ID a maker last
    // published, live or destroyed; no thread is made per round, so that ThreadSanitizer sees the races themselves.
    use_fake();
    constexpr int makers = 8, rounds = 200;
    std::atomic<std::uint64_t> published[makers] = {};
    std::atomic<bool> done{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int r = 0; r < makers; r++) {
        threads.emplace_back([&, r] {
            unsigned char unit[8] = {0, 0, 0, 1, 0x65};
            std::vector<unsigned char> buffer(64);
            while (!done) {
                apkrt_video_decoder mine{};
                mine.id = published[r].load(), mine.data = reinterpret_cast<std::uint64_t>(unit), mine.size = 8;
                const std::int64_t queued = call(APKRT_VIDEO_DECODER_QUEUE, mine);
                if (queued != 0 && queued != -ebadf) failures++;
                mine.data = reinterpret_cast<std::uint64_t>(buffer.data()), mine.size = buffer.size();
                const std::int64_t dequeued = call(APKRT_VIDEO_DECODER_DEQUEUE, mine);
                if (dequeued != -eagain && dequeued != -ebadf) failures++;
            }
        });
    }
    for (int m = 0; m < makers; m++) {
        threads.emplace_back([&, m] {
            for (int round = 0; round < rounds; round++) {
                apkrt_video_decoder made = asked();
                if (call(APKRT_VIDEO_DECODER_CREATE, made) != 0) continue;  // -EBUSY at the limit is fine
                published[m] = made.id;
                if (call(APKRT_VIDEO_DECODER_FLUSH, made.id) != 0) failures++;
                if (call(APKRT_VIDEO_DECODER_DESTROY, made.id) != 0) failures++;
            }
        });
    }
    for (int m = makers; m < 2 * makers; m++) threads[static_cast<std::size_t>(m)].join();
    done = true;
    for (int r = 0; r < makers; r++) threads[static_cast<std::size_t>(r)].join();
    CHECK(failures == 0);
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.overlaps == 0 && fake.destroys == fake.creates && fake.creates > 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // each test's line before a crash in the next
    return apk::testing::run_all();
}
