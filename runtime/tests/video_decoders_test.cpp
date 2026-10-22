// Checks the guest's video decoders (ADR-0060 e2b3): video_decoder's operations through the syscall dispatcher, as
// libmediandk's AMediaCodec makes them, against a fake table of host decoders: the IDs the runtime keeps, the limits
// and checks that keep the host sound against hostile values, what reaches the host and what comes back, and a
// DESTROY that waits for a QUEUE still in the host. graphics/host's decoders on VideoToolbox have their own test, the
// guest's codec the fixture media-codec-test against Android 15.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
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
constexpr std::int64_t eagain = 11, ebadf = 9, ebusy = 16, efault = 14, eio = 5, einval = 22, enospc = 28,
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
        std::vector<std::vector<unsigned char>> frame_bytes;
        int flushes = 0;
        bool destroyed = false;
        int calls_running = 0;  // calls inside the fake, to catch a destroy among them
    };
    std::mutex mutex;
    std::map<std::uint64_t, Decoder> decoders;
    std::uint64_t next = 500;
    int creates = 0, destroys = 0, overlaps = 0;
    int refuse_create = 0, queue_answer = 0, peek_answer = 0;  // when not 0
    bool fail_after_create = false;  // the runtime's allocations fail once the host's decoder is made
    // A queue waits here while `hold_queue`, until released, so that a destroy can race it.
    bool hold_queue = false, queue_entered = false;
    std::condition_variable queue_changed;

    void reset() {
        const std::lock_guard hold(mutex);
        decoders.clear();
        creates = destroys = overlaps = refuse_create = queue_answer = peek_answer = 0;
        fail_after_create = false;
        hold_queue = queue_entered = false;
    }

    /// A frame of `width` x `height` with the time, its NV12 bytes a pattern, rows of `stride` bytes.
    void add_frame(std::uint64_t handle, std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                   std::int64_t time, std::uint32_t flags = 0) {
        const std::lock_guard hold(mutex);
        Decoder &decoder = decoders.at(handle);
        const std::uint32_t slice = height;
        std::vector<unsigned char> bytes(flags != 0 ? 0 : std::size_t{stride} * (slice + (slice + 1) / 2));
        for (std::size_t at = 0; at < bytes.size(); at++) {
            bytes[at] = static_cast<unsigned char>(at * 7 + static_cast<std::size_t>(time));
        }
        decoder.frame_bytes.push_back(std::move(bytes));
        apkrt_video_frame frame{};
        frame.size = decoder.frame_bytes.back().size();
        frame.width = width, frame.height = height, frame.stride = stride, frame.slice_height = slice;
        frame.crop_right = static_cast<std::uint16_t>(width - 1);
        frame.crop_bottom = static_cast<std::uint16_t>(height - 1);
        frame.time_us = time, frame.flags = flags;
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
    frame->data = decoder.frame_bytes.front().data();
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

const apkrt_video_decoders table{fake_create, fake_queue, fake_peek, fake_pop, fake_flush, fake_destroy, &fake};

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
std::uint64_t create(std::uint64_t *handle = nullptr, std::uint32_t type = avc) {
    apkrt_video_decoder decoder = asked(type);
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
    for (int missing = 0; missing < 6; missing++) {
        apkrt_video_decoders partial = table;
        void **functions[] = {reinterpret_cast<void **>(&partial.create), reinterpret_cast<void **>(&partial.queue),
                              reinterpret_cast<void **>(&partial.peek), reinterpret_cast<void **>(&partial.pop),
                              reinterpret_cast<void **>(&partial.flush), reinterpret_cast<void **>(&partial.destroy)};
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
    const std::lock_guard hold(fake.mutex);
    const Fake::Decoder &made = fake.decoders.at(handle);
    CHECK(made.units.size() == 2 && made.units[0].size() == 8 && made.units[0][4] == 0x65 && made.units[1].empty());
    CHECK(made.times[0] == 33333 && made.flags[0] == 0 && made.flags[1] == APKRT_VIDEO_DECODER_END_OF_STREAM);
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
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.decoders.at(handle).units.empty());
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
