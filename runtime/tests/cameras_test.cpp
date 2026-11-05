// Checks the guest's cameras (ADR-0073): camera's operations through the syscall dispatcher against a fake table of
// host cameras: the IDs the runtime keeps, the facts it checks, the streams and controls it takes or refuses, what
// reaches the host and what comes back (a frame's tokens, its JPEG bytes, its time on the guest's CLOCK_BOOTTIME), the
// tokens that wait, and the limits that keep the host sound against hostile values.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscall_frame.h"
#include "syscalls.h"

#include "check.h"

#include <sys/mman.h>
#include <time.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr std::int64_t eacces = 13, eagain = 11, ebadf = 9, ebusy = 16, efault = 14, eio = 5, einval = 22, enospc = 28,
                       enosys = 38;

std::int64_t call(std::uint64_t op, apkrt_camera &camera) {
    apkrt_syscall_frame frame{};
    frame.x[0] = op;
    frame.x[1] = reinterpret_cast<std::uint64_t>(&camera);
    frame.x[8] = APKRT_NR_CAMERA;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

/// A facts list builder, as a host writes one.
struct FactsList {
    std::vector<unsigned char> bytes;
    template <typename T>
    FactsList &add(std::uint32_t key, std::uint32_t type, const std::vector<T> &values) {
        const std::uint32_t header[3] = {key, type, static_cast<std::uint32_t>(values.size())};
        const auto *raw = reinterpret_cast<const unsigned char *>(header);
        bytes.insert(bytes.end(), raw, raw + sizeof header);
        const auto *data = reinterpret_cast<const unsigned char *>(values.data());
        bytes.insert(bytes.end(), data, data + values.size() * sizeof(T));
        while (bytes.size() % 8 != 0) bytes.push_back(0);
        return *this;
    }
};

/// The model's back camera: YUV at 640x480 and 1280x720, JPEG at 4032x3024, zoom 1 to 10, 15-30 and 30-30 fps, AF
/// modes off, auto and continuous picture (0, 1, 4), AWB auto (1).
std::vector<unsigned char> back_facts() {
    FactsList list;
    list.add<std::int32_t>(APKRT_CAMERA_KEY_FACING, APKRT_CAMERA_TYPE_I32, {1})
            .add<std::int32_t>(APKRT_CAMERA_KEY_ORIENTATION, APKRT_CAMERA_TYPE_I32, {90})
            .add<float>(APKRT_CAMERA_KEY_ZOOM, APKRT_CAMERA_TYPE_F32, {1.0f, 10.0f})
            .add<std::int32_t>(APKRT_CAMERA_KEY_FPS_RANGES, APKRT_CAMERA_TYPE_I32, {15, 30, 30, 30})
            .add<std::int32_t>(APKRT_CAMERA_KEY_AF_MODES, APKRT_CAMERA_TYPE_I32, {0, 1, 4})
            .add<std::int32_t>(APKRT_CAMERA_KEY_AWB_MODES, APKRT_CAMERA_TYPE_I32, {1})
            .add<std::int64_t>(APKRT_CAMERA_KEY_STREAMS, APKRT_CAMERA_TYPE_I64,
                               {APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 33333333, APKRT_CAMERA_FORMAT_YUV_420_888,
                                1280, 720, 33333333, APKRT_CAMERA_FORMAT_JPEG, 4032, 3024, 33333333});
    return list.bytes;
}

/// A fake table of host cameras, with a log of the calls.
struct Fake {
    std::mutex mutex;
    std::vector<std::vector<unsigned char>> facts;  // per camera
    std::map<std::uint64_t, std::uint32_t> open;    // handle to camera
    std::uint64_t next = 700;
    std::vector<apkrt_camera_stream> streams;
    std::vector<apkrt_camera_controls> controls;
    std::vector<std::uint64_t> numbers;
    std::vector<std::uint32_t> targets, flags;
    std::vector<apkrt_camera_capture> captures;  // peek gives the first
    std::vector<unsigned char> jpeg;
    std::vector<std::uint64_t> discarded, rendered_windows;
    int closes = 0, flushes = 0, stops = 0, open_answer = 0;

    void reset() {
        const std::lock_guard hold(mutex);
        facts = {back_facts(), back_facts()};
        open.clear();
        streams.clear(), controls.clear(), numbers.clear(), targets.clear(), flags.clear(), captures.clear();
        discarded.clear(), rendered_windows.clear();
        closes = flushes = stops = open_answer = 0;
    }
};

Fake fake;

std::uint32_t fake_count(void *context) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    return static_cast<std::uint32_t>(self.facts.size());
}

int fake_facts(void *context, std::uint32_t camera, void *data, std::size_t capacity, std::size_t *size) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    const auto &bytes = self.facts.at(camera);
    *size = bytes.size();
    if (capacity < bytes.size()) return -enospc;
    std::memcpy(data, bytes.data(), bytes.size());
    return 0;
}

int fake_open(void *context, std::uint32_t camera, std::uint64_t *handle) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    if (self.open_answer != 0) return self.open_answer;
    *handle = self.next++;
    self.open[*handle] = camera;
    return 0;
}

int fake_configure(void *context, std::uint64_t, const apkrt_camera_stream *streams, std::uint32_t count) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.streams.assign(streams, streams + count);
    return 0;
}

int fake_request(void *context, std::uint64_t, const apkrt_camera_controls *controls, std::uint64_t number,
                 std::uint32_t targets, std::uint32_t flags) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.controls.push_back(*controls);
    self.numbers.push_back(number);
    self.targets.push_back(targets);
    self.flags.push_back(flags);
    return 0;
}

void fake_stop(void *context, std::uint64_t) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.stops++;
}

int fake_peek(void *context, std::uint64_t, apkrt_camera_capture *capture) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    if (self.captures.empty()) return -eagain;
    *capture = self.captures.front();
    return 0;
}

void fake_pop(void *context, std::uint64_t) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.captures.erase(self.captures.begin());
}

int fake_render(void *, std::uint64_t, std::uint64_t, const apkrt_video_target *) { return 0; }

void fake_discard(void *context, std::uint64_t, std::uint64_t token) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.discarded.push_back(token);
}

int fake_render_window(void *context, std::uint64_t, std::uint64_t token, std::uint64_t, std::int64_t,
                       std::uint32_t) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.rendered_windows.push_back(token);
    return 0;
}

int fake_flush(void *context, std::uint64_t) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.flushes++;
    self.captures.clear();
    return 0;
}

void fake_close(void *context, std::uint64_t handle) {
    auto &self = *static_cast<Fake *>(context);
    const std::lock_guard hold(self.mutex);
    self.closes++;
    self.open.erase(handle);
}

const apkrt_cameras table = {fake_count, fake_facts,   fake_open,    fake_configure,     fake_request,
                             fake_stop,  fake_peek,    fake_pop,     fake_render,        fake_discard,
                             fake_render_window,       fake_flush,   fake_close,         &fake};

/// A page the guest cannot read or write.
std::uint64_t no_access_page() {
    static void *page = mmap(nullptr, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return reinterpret_cast<std::uint64_t>(page);
}

void with_fake() {
    fake.reset();
    CHECK(apkrt_set_cameras(&table) == 0);
    apkrt_permission_answer(0, APKRT_PERMISSION_CAMERA, APKRT_PERMISSION_GRANTED);
}

std::uint64_t open_camera(std::uint32_t index) {
    apkrt_camera camera{};
    camera.camera = index;
    CHECK(call(APKRT_CAMERA_OPEN, camera) == 0);
    CHECK(camera.id != 0);
    return camera.id;
}

void close_camera(std::uint64_t id) {
    apkrt_camera camera{};
    camera.id = id;
    CHECK(call(APKRT_CAMERA_CLOSE, camera) == 0);
}

std::int64_t configure(std::uint64_t id, std::vector<apkrt_camera_stream> streams) {
    apkrt_camera camera{};
    camera.id = id;
    camera.count = static_cast<std::uint32_t>(streams.size());
    camera.data = reinterpret_cast<std::uint64_t>(streams.data());
    return call(APKRT_CAMERA_CONFIGURE, camera);
}

/// Controls the back camera's facts allow: continuous picture, auto white balance, 30 fps, zoom 1, quality 95.
apkrt_camera_controls good_controls() {
    apkrt_camera_controls controls{};
    controls.af_mode = 4, controls.awb_mode = 1, controls.ae_mode = 1, controls.jpeg_quality = 95;
    controls.zoom_ratio = 1.0f, controls.fps_min = 30, controls.fps_max = 30;
    return controls;
}

std::int64_t request(std::uint64_t id, const apkrt_camera_controls &controls, std::uint32_t targets,
                     std::uint32_t flags = 0, std::uint64_t number = 1) {
    apkrt_camera camera{};
    camera.id = id;
    camera.data = reinterpret_cast<std::uint64_t>(&controls);
    camera.targets = targets, camera.flags = flags, camera.number = number;
    return call(APKRT_CAMERA_REQUEST, camera);
}

TEST(without_host_cameras_there_is_none) {
    CHECK(apkrt_set_cameras(nullptr) == 0);
    apkrt_camera camera{};
    CHECK(call(APKRT_CAMERA_COUNT, camera) == -enosys);
    CHECK(call(APKRT_CAMERA_OPEN, camera) == -enosys);
    apkrt_cameras partial = table;
    partial.close = nullptr;
    CHECK(apkrt_set_cameras(&partial) == -1);
}

TEST(count_and_facts_come_from_the_host_checked) {
    with_fake();
    apkrt_camera camera{};
    CHECK(call(APKRT_CAMERA_COUNT, camera) == 0 && camera.count == 2);
    std::vector<unsigned char> buffer(4096);
    camera.camera = 0, camera.data = reinterpret_cast<std::uint64_t>(buffer.data()), camera.size = 8;
    CHECK(call(APKRT_CAMERA_FACTS, camera) == -enospc && camera.size == back_facts().size());
    camera.size = buffer.size();
    CHECK(call(APKRT_CAMERA_FACTS, camera) == 0 && camera.size == back_facts().size());
    CHECK(std::memcmp(buffer.data(), back_facts().data(), back_facts().size()) == 0);
    camera.camera = 2;
    CHECK(call(APKRT_CAMERA_FACTS, camera) == -einval);
    // A list whose entry runs past its end, or a known key of the wrong type, is the host's fault.
    fake.facts[1].resize(fake.facts[1].size() - 4);
    camera.camera = 1, camera.size = buffer.size();
    CHECK(call(APKRT_CAMERA_FACTS, camera) == -eio);
    fake.facts[1] = FactsList().add<float>(APKRT_CAMERA_KEY_FACING, APKRT_CAMERA_TYPE_F32, {1.0f}).bytes;
    CHECK(call(APKRT_CAMERA_FACTS, camera) == -eio);
    camera.camera = 1;
    CHECK(call(APKRT_CAMERA_OPEN, camera) == -eio);
    // An unknown key of a later host is skipped.
    fake.facts[1] = FactsList().add<std::int32_t>(999, APKRT_CAMERA_TYPE_I32, {1, 2, 3}).bytes;
    CHECK(call(APKRT_CAMERA_FACTS, camera) == 0);
    camera.data = no_access_page();
    CHECK(call(APKRT_CAMERA_FACTS, camera) == -efault);
}

TEST(a_camera_opens_once_and_at_most_two_at_once) {
    with_fake();
    const std::uint64_t first = open_camera(0);
    apkrt_camera again{};
    again.camera = 0;
    CHECK(call(APKRT_CAMERA_OPEN, again) == -ebusy);
    const std::uint64_t second = open_camera(1);
    CHECK(second != first);
    fake.facts.push_back(back_facts());
    again.camera = 2;
    CHECK(call(APKRT_CAMERA_OPEN, again) == -ebusy);  // APKRT_CAMERA_MAX_OPEN
    close_camera(first);
    CHECK(fake.closes == 1);
    apkrt_camera closed{};
    closed.id = first;
    CHECK(call(APKRT_CAMERA_CLOSE, closed) == -ebadf);
    CHECK(call(APKRT_CAMERA_FLUSH, closed) == -ebadf);
    const std::uint64_t reopened = open_camera(0);
    CHECK(reopened > second);  // IDs are never used again
    close_camera(second);
    close_camera(reopened);
    fake.open_answer = -eacces;  // iOS denies the camera
    again.camera = 0;
    CHECK(call(APKRT_CAMERA_OPEN, again) == -eacces);
    fake.open_answer = 0;
    close_camera(open_camera(0));  // the refused open left no claim
}

TEST(a_camera_opens_only_with_the_camera_grant) {
    with_fake();
    apkrt_permission_answer(0, APKRT_PERMISSION_CAMERA, APKRT_PERMISSION_DENIED);
    apkrt_camera camera{};
    CHECK(call(APKRT_CAMERA_OPEN, camera) == -eacces);
    CHECK(fake.open.empty());  // the host never asked
    CHECK(call(APKRT_CAMERA_COUNT, camera) == 0 && camera.count == 2);  // the list and facts are not gated
    apkrt_permission_answer(0, APKRT_PERMISSION_CAMERA, APKRT_PERMISSION_GRANTED);
    close_camera(open_camera(0));
}

TEST(only_listed_streams_are_configured) {
    with_fake();
    const std::uint64_t id = open_camera(0);
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_PRIVATE, 1280, 720, 0}, {APKRT_CAMERA_FORMAT_JPEG, 4032, 3024, 0}}) == 0);
    CHECK(fake.streams.size() == 2 && fake.streams[1].width == 4032);
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_YUV_420_888, 1920, 1080, 0}}) == -einval);  // not listed
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_JPEG, 640, 480, 0}}) == -einval);
    CHECK(configure(id, {{0x11, 640, 480, 0}}) == -einval);                               // NV21 is no stream here
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 1}}) == -einval);    // reserved
    CHECK(configure(id, std::vector<apkrt_camera_stream>(5, {APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 0})) ==
          -einval);
    apkrt_camera camera{};
    camera.id = id, camera.count = 1, camera.data = no_access_page();
    CHECK(call(APKRT_CAMERA_CONFIGURE, camera) == -efault);
    close_camera(id);
}

TEST(requests_take_the_controls_the_facts_allow) {
    with_fake();
    const std::uint64_t id = open_camera(0);
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 0}, {APKRT_CAMERA_FORMAT_JPEG, 4032, 3024, 0}}) ==
          0);
    apkrt_camera_controls controls = good_controls();
    CHECK(request(id, controls, 1, APKRT_CAMERA_REPEATING, 7) == 0);
    CHECK(fake.numbers.back() == 7 && fake.targets.back() == 1 && fake.flags.back() == APKRT_CAMERA_REPEATING);
    CHECK(fake.controls.back().af_mode == 4 && fake.controls.back().jpeg_quality == 95);
    CHECK(request(id, controls, 3) == 0);
    CHECK(request(id, controls, 4) == -einval);  // a stream not configured
    CHECK(request(id, controls, 0) == -einval);
    CHECK(request(id, controls, 1, 2) == -einval);
    const auto refused = [&](auto change) {
        apkrt_camera_controls changed = good_controls();
        change(changed);
        return request(id, changed, 1) == -einval;
    };
    CHECK(refused([](apkrt_camera_controls &c) { c.af_mode = 3; }));  // continuous video, not listed
    CHECK(refused([](apkrt_camera_controls &c) { c.awb_mode = 2; }));
    CHECK(refused([](apkrt_camera_controls &c) { c.zoom_ratio = 12.0f; }));
    CHECK(refused([](apkrt_camera_controls &c) { c.zoom_ratio = __builtin_nanf(""); }));
    CHECK(refused([](apkrt_camera_controls &c) { c.fps_min = 10; }));
    CHECK(refused([](apkrt_camera_controls &c) { c.jpeg_quality = 0; }));
    CHECK(refused([](apkrt_camera_controls &c) { c.jpeg_orientation = 45; }));
    CHECK(refused([](apkrt_camera_controls &c) { c.reserved[3] = 1; }));
    CHECK(refused([](apkrt_camera_controls &c) { c.af_region[4] = -1; }));
    apkrt_camera camera{};
    camera.id = id;
    CHECK(call(APKRT_CAMERA_STOP_REPEATING, camera) == 0 && fake.stops == 1);
    close_camera(id);
}

/// A capture of the configured YUV and JPEG streams with tokens and a JPEG of `jpeg` bytes.
apkrt_camera_capture capture(std::uint64_t number, std::uint64_t token, std::size_t jpeg, std::int64_t time) {
    apkrt_camera_capture capture{};
    capture.frame.number = number;
    capture.frame.frame = number - 1;
    capture.frame.timestamp_ns = time;
    capture.frame.streams = jpeg > 0 ? 3 : 1;
    capture.frame.tokens[0] = token;
    capture.frame.states = APKRT_CAMERA_STATE_FOCUSED;
    fake.jpeg.assign(jpeg, 0xd8);
    capture.jpeg = jpeg > 0 ? fake.jpeg.data() : nullptr;
    capture.jpeg_size = jpeg;
    return capture;
}

TEST(a_frame_gives_its_tokens_its_jpeg_and_its_boot_time) {
    with_fake();
    const std::uint64_t id = open_camera(0);
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 0}, {APKRT_CAMERA_FORMAT_JPEG, 4032, 3024, 0}}) ==
          0);
    const std::int64_t uptime = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    fake.captures.push_back(capture(1, 41, 1000, uptime));
    std::vector<unsigned char> jpeg(500);
    apkrt_camera_frame frame{};
    frame.jpeg = reinterpret_cast<std::uint64_t>(jpeg.data()), frame.jpeg_capacity = jpeg.size();
    apkrt_camera camera{};
    camera.id = id, camera.data = reinterpret_cast<std::uint64_t>(&frame);
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -enospc && frame.jpeg_size == 1000);
    CHECK(fake.captures.size() == 1);  // it stays for a larger buffer
    jpeg.resize(2000);
    frame.jpeg = reinterpret_cast<std::uint64_t>(jpeg.data()), frame.jpeg_capacity = jpeg.size();
    CHECK(call(APKRT_CAMERA_FRAME, camera) == 0);
    CHECK(frame.number == 1 && frame.tokens[0] == 41 && frame.tokens[1] == 0 && frame.jpeg_size == 1000);
    CHECK(jpeg[0] == 0xd8 && jpeg[999] == 0xd8 && jpeg[1000] == 0);
    CHECK(frame.states == APKRT_CAMERA_STATE_FOCUSED);
    const std::int64_t boot = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC)) -
                              static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) + uptime;
    CHECK(frame.timestamp_ns > boot - 1000000 && frame.timestamp_ns < boot + 1000000);
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eagain);
    // The token waits: drawn once into a window, after which it is no more.
    apkrt_camera draw{};
    draw.id = id, draw.data = 41, draw.size = 9;
    CHECK(call(APKRT_CAMERA_RENDER_WINDOW, draw) == 0 && fake.rendered_windows.back() == 41);
    CHECK(call(APKRT_CAMERA_RENDER_WINDOW, draw) == -einval);
    CHECK(call(APKRT_CAMERA_DISCARD, draw) == -einval);
    close_camera(id);
}

TEST(a_frame_that_does_not_hold_is_the_hosts_fault) {
    with_fake();
    const std::uint64_t id = open_camera(0);
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 0}}) == 0);
    apkrt_camera_frame frame{};
    apkrt_camera camera{};
    camera.id = id, camera.data = reinterpret_cast<std::uint64_t>(&frame);
    fake.captures.push_back(capture(1, 0, 0, 0));  // no token for the YUV stream
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eio);
    fake.captures[0].frame.tokens[0] = 5, fake.captures[0].frame.streams = 2;  // a stream not configured
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eio);
    fake.captures[0].frame.streams = 1, fake.captures[0].frame.faces = 5;
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eio);
    fake.captures[0].frame.faces = 0, fake.captures[0].jpeg_size = 10;  // a JPEG without a JPEG stream
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eio);
    fake.captures[0].jpeg_size = 0;
    CHECK(call(APKRT_CAMERA_FRAME, camera) == 0 && frame.tokens[0] == 5);
    fake.captures.push_back(capture(2, 5, 0, 0));  // a token that still waits
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eio);
    camera.data = no_access_page();
    fake.captures[0].frame.tokens[0] = 6;
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -efault);
    CHECK(fake.captures.size() == 1);  // it stays
    close_camera(id);
}

TEST(tokens_wait_up_to_the_limit_and_a_flush_ends_them) {
    with_fake();
    const std::uint64_t id = open_camera(0);
    CHECK(configure(id, {{APKRT_CAMERA_FORMAT_YUV_420_888, 640, 480, 0}}) == 0);
    apkrt_camera_frame frame{};
    apkrt_camera camera{};
    camera.id = id, camera.data = reinterpret_cast<std::uint64_t>(&frame);
    for (std::uint64_t n = 1; n <= APKRT_CAMERA_MAX_WAITING; n++) {
        fake.captures.push_back(capture(n, 100 + n, 0, 0));
        CHECK(call(APKRT_CAMERA_FRAME, camera) == 0);
    }
    fake.captures.push_back(capture(99, 300, 0, 0));
    CHECK(call(APKRT_CAMERA_FRAME, camera) == -eagain);
    apkrt_camera discard{};
    discard.id = id, discard.data = 101;
    CHECK(call(APKRT_CAMERA_DISCARD, discard) == 0 && fake.discarded.back() == 101);
    CHECK(call(APKRT_CAMERA_FRAME, camera) == 0 && frame.tokens[0] == 300);
    apkrt_camera flush{};
    flush.id = id;
    CHECK(call(APKRT_CAMERA_FLUSH, flush) == 0 && fake.flushes == 1);
    discard.data = 102;
    CHECK(call(APKRT_CAMERA_DISCARD, discard) == -einval);  // ended by the flush
    close_camera(id);
}

TEST(an_unknown_op_or_a_struct_the_guest_lacks_is_refused) {
    with_fake();
    apkrt_camera camera{};
    CHECK(call(99, camera) == -einval);
    apkrt_syscall_frame frame{};
    frame.x[0] = APKRT_CAMERA_COUNT;
    frame.x[1] = no_access_page();
    frame.x[8] = APKRT_NR_CAMERA;
    apkrt_syscall_dispatch(&frame);
    CHECK(static_cast<std::int64_t>(frame.x[0]) == -efault);
}

}  // namespace

int main() { return apk::testing::run_all(); }
