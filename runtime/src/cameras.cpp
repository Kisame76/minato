// The guest's cameras (cameras.h, ADR-0073), as the video decoders (video_decoders.cpp) are kept. The registry maps
// the runtime's IDs to open cameras under one mutex, which no call into the host holds; each camera has a mutex of its
// own, held around every call of its host's functions, so that they come one at a time and a CLOSE waits for the call
// still running on another thread. IDs count up from 1 and are never used again. An OPEN needs the guest's camera
// grant, which the host records (ADR-0073 decision 8); the count and the facts do not. A camera's facts are read from
// the host and checked at its OPEN (each entry within the list, its type and count as guest_interface.h has them), and
// kept, so that CONFIGURE takes only the streams they list and REQUEST only the controls they allow; a host whose facts
// do not hold answers -EIO, and is never opened. The guest's structs are read once; a frame's description is checked
// before the guest sees it (its tokens, streams and JPEG size), and a JPEG too large for the guest's buffer stays the
// host's for a later FRAME. The runtime keeps the tokens of the frames the guest took, at most
// APKRT_CAMERA_MAX_WAITING per camera, so that only a waiting token reaches the host's render, render_window or
// discard. Memory the runtime cannot get answers -ENOMEM: no exception leaves call(), whose caller is the syscall
// entry's assembly.
#include "cameras.h"

#include "permissions.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

#include <time.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <utility>
#include <vector>

namespace apkrt::cameras {
namespace {

namespace linux_errno = sys::linux_errno;

static_assert(sizeof(apkrt_camera) == 64 && sizeof(apkrt_camera_stream) == 16);
static_assert(sizeof(apkrt_camera_controls) == 128 && sizeof(apkrt_camera_frame) == 256);
static_assert(offsetof(apkrt_camera, data) == 16 && offsetof(apkrt_camera, number) == 32);
static_assert(offsetof(apkrt_camera, time_ns) == 48 && offsetof(apkrt_camera_frame, tokens) == 48);
static_assert(offsetof(apkrt_camera_frame, jpeg) == 80 && offsetof(apkrt_camera_frame, states) == 104);

/// An answer of the host's functions as an errno: 0, or -1 to -4095, anything else -EIO.
long host_answer(int answer) {
    return answer == 0 || (answer < 0 && answer >= -4095) ? answer : -linux_errno::eio;
}

/// The shift from the host's capture times (Darwin CLOCK_UPTIME_RAW, no sleep) to the guest's CLOCK_BOOTTIME (Darwin
/// CLOCK_MONOTONIC, through sleep), as the sensor hub shifts CoreMotion's.
std::int64_t boot_minus_uptime() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC)) -
           static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

/// What the runtime checks requests against, from a camera's facts.
struct Facts {
    std::vector<std::array<std::int64_t, 4>> streams;  // format, width, height, least frame duration
    float zoom_min = 1, zoom_max = 1;
    std::vector<std::pair<std::int32_t, std::int32_t>> fps;
    std::vector<std::int32_t> af_modes, awb_modes;
};

std::uint32_t value_bytes(std::uint32_t type) {
    return type == APKRT_CAMERA_TYPE_I64 ? 8 : type == APKRT_CAMERA_TYPE_I32 || type == APKRT_CAMERA_TYPE_F32 ? 4 : 0;
}

/// The facts' list checked and read: each entry's header and values within the list, a known key's type and count
/// as guest_interface.h has them (an unknown key is skipped); false for a list that does not hold.
bool parse_facts(const unsigned char *data, std::size_t size, Facts &facts) {
    std::size_t at = 0;
    while (at < size) {
        if (size - at < 12) return false;
        std::uint32_t header[3];
        std::memcpy(header, data + at, sizeof header);
        const std::uint32_t key = header[0], type = header[1], count = header[2], bytes = value_bytes(type);
        if (bytes == 0) return false;
        const std::uint64_t length = 12 + std::uint64_t{count} * bytes;
        const std::uint64_t padded = (length + 7) & ~std::uint64_t{7};
        if (padded > size - at) return false;
        const unsigned char *values = data + at + 12;
        const auto i32 = [&](std::uint32_t n) {
            std::int32_t value;
            std::memcpy(&value, values + 4 * std::size_t{n}, 4);
            return value;
        };
        const auto i64 = [&](std::uint32_t n) {
            std::int64_t value;
            std::memcpy(&value, values + 8 * std::size_t{n}, 8);
            return value;
        };
        const auto f32 = [&](std::uint32_t n) {
            float value;
            std::memcpy(&value, values + 4 * std::size_t{n}, 4);
            return value;
        };
        switch (key) {
            case APKRT_CAMERA_KEY_STREAMS:
                if (type != APKRT_CAMERA_TYPE_I64 || count % 4 != 0) return false;
                for (std::uint32_t n = 0; n < count; n += 4) {
                    facts.streams.push_back({i64(n), i64(n + 1), i64(n + 2), i64(n + 3)});
                }
                break;
            case APKRT_CAMERA_KEY_ZOOM:
                if (type != APKRT_CAMERA_TYPE_F32 || count != 2) return false;
                facts.zoom_min = f32(0), facts.zoom_max = f32(1);
                if (!std::isfinite(facts.zoom_min) || !std::isfinite(facts.zoom_max) || facts.zoom_min <= 0 ||
                    facts.zoom_min > facts.zoom_max) {
                    return false;
                }
                break;
            case APKRT_CAMERA_KEY_FPS_RANGES:
                if (type != APKRT_CAMERA_TYPE_I32 || count % 2 != 0) return false;
                for (std::uint32_t n = 0; n < count; n += 2) facts.fps.emplace_back(i32(n), i32(n + 1));
                break;
            case APKRT_CAMERA_KEY_AF_MODES:
            case APKRT_CAMERA_KEY_AWB_MODES:
                if (type != APKRT_CAMERA_TYPE_I32) return false;
                for (std::uint32_t n = 0; n < count; n++) {
                    (key == APKRT_CAMERA_KEY_AF_MODES ? facts.af_modes : facts.awb_modes).push_back(i32(n));
                }
                break;
            case APKRT_CAMERA_KEY_FACING:
            case APKRT_CAMERA_KEY_ORIENTATION:
            case APKRT_CAMERA_KEY_FLASH:
            case APKRT_CAMERA_KEY_FACES:
                if (type != APKRT_CAMERA_TYPE_I32 || count != 1) return false;
                break;
            case APKRT_CAMERA_KEY_PIXEL_ARRAY:
            case APKRT_CAMERA_KEY_ISO:
                if (type != APKRT_CAMERA_TYPE_I32 || count != 2) return false;
                break;
            case APKRT_CAMERA_KEY_ACTIVE_ARRAY:
            case APKRT_CAMERA_KEY_AE_COMPENSATION:
                if (type != APKRT_CAMERA_TYPE_I32 || count != 4) return false;
                break;
            case APKRT_CAMERA_KEY_EXPOSURE_NS:
                if (type != APKRT_CAMERA_TYPE_I64 || count != 2) return false;
                break;
            case APKRT_CAMERA_KEY_PHYSICAL_SIZE:
                if (type != APKRT_CAMERA_TYPE_F32 || count != 2) return false;
                break;
            case APKRT_CAMERA_KEY_FOCAL_LENGTHS:
            case APKRT_CAMERA_KEY_APERTURES:
            case APKRT_CAMERA_KEY_MIN_FOCUS_DISTANCE:
                if (type != APKRT_CAMERA_TYPE_F32 || count == 0) return false;
                break;
            default:
                break;  // a key of a later host
        }
        at += static_cast<std::size_t>(padded);
    }
    return true;
}

struct Camera {
    apkrt_cameras table{};
    std::uint64_t handle = 0;
    std::uint32_t index = 0;
    Facts facts;
    std::mutex mutex;  // around each call of the host's functions, and for the fields below
    bool gone = false;
    std::vector<apkrt_camera_stream> streams;  // CONFIGURE's
    std::set<std::uint64_t> waiting;           // the tokens the guest took and has not rendered or discarded
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Camera>> cameras;
    std::set<std::uint32_t> open;  // the indices open or opening, until their CLOSE has ended the host's camera
    std::uint64_t next_id = 1;
    apkrt_cameras table{};  // all NULL: none
};

Registry &registry() {
    static auto *one = new Registry;  // as long as the process
    return *one;
}

apkrt_cameras table_of() {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    return all.table;
}

std::shared_ptr<Camera> find(std::uint64_t id) {
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    const auto found = all.cameras.find(id);
    return found == all.cameras.end() ? nullptr : found->second;
}

bool take(std::uint64_t address, std::uint64_t size, void *bytes) {
    return size == 0 || (address != 0 && sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(bytes), address,
                                                                    size));
}

bool put(std::uint64_t address, std::size_t offset, const void *value, std::size_t size) {
    return sys::memory::copy_checked(address + offset, reinterpret_cast<std::uint64_t>(value), size);
}

/// The host's camera count, at most APKRT_CAMERA_MAX_CAMERAS; -ENOSYS without the host's cameras.
long camera_count(const apkrt_cameras &table) {
    if (table.count == nullptr) return -linux_errno::enosys;
    const std::uint32_t count = table.count(table.context);
    return count > APKRT_CAMERA_MAX_CAMERAS ? APKRT_CAMERA_MAX_CAMERAS : count;
}

/// Camera `index`'s facts from the host, checked into `facts` where it is not NULL: 0, or the answer.
long host_facts(const apkrt_cameras &table, std::uint32_t index, std::vector<unsigned char> &bytes, Facts *facts) {
    try {
        bytes.assign(APKRT_CAMERA_MAX_FACTS, 0);
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    std::size_t size = 0;
    const long answer = host_answer(table.facts(table.context, index, bytes.data(), bytes.size(), &size));
    if (answer != 0) return answer == -linux_errno::enospc ? -linux_errno::eio : answer;  // the limit is the host's
    if (size > bytes.size()) return -linux_errno::eio;
    bytes.resize(size);
    Facts parsed;
    try {
        if (!parse_facts(bytes.data(), bytes.size(), parsed)) return -linux_errno::eio;
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    if (facts != nullptr) *facts = std::move(parsed);
    return 0;
}

long count(std::uint64_t argument) {
    const long found = camera_count(table_of());
    if (found < 0) return found;
    const auto value = static_cast<std::uint32_t>(found);
    return put(argument, offsetof(apkrt_camera, count), &value, sizeof value) ? 0 : -linux_errno::efault;
}

long facts(std::uint64_t argument, const apkrt_camera &asked) {
    const apkrt_cameras table = table_of();
    const long found = camera_count(table);
    if (found < 0) return found;
    if (asked.camera >= static_cast<std::uint64_t>(found)) return -linux_errno::einval;
    std::vector<unsigned char> bytes;
    if (const long answer = host_facts(table, asked.camera, bytes, nullptr); answer != 0) return answer;
    const std::uint64_t size = bytes.size();
    if (!put(argument, offsetof(apkrt_camera, size), &size, sizeof size)) return -linux_errno::efault;
    if (asked.size < size) return -linux_errno::enospc;
    if (size > 0 && (asked.data == 0 || !sys::memory::copy_checked(asked.data,
                                                                  reinterpret_cast<std::uint64_t>(bytes.data()),
                                                                  size))) {
        return -linux_errno::efault;
    }
    return 0;
}

long close_camera(std::uint64_t id) {
    std::shared_ptr<Camera> camera;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        const auto found = all.cameras.find(id);
        if (found == all.cameras.end()) return -linux_errno::ebadf;
        camera = std::move(found->second);
        all.cameras.erase(found);
    }
    {
        const std::lock_guard hold(camera->mutex);  // after the call still running on it
        camera->gone = true;
        camera->waiting.clear();
        camera->table.close(camera->table.context, camera->handle);
    }
    auto &all = registry();
    const std::lock_guard hold(all.mutex);
    all.open.erase(camera->index);  // only now may the camera open again
    return 0;
}

long open(std::uint64_t argument, const apkrt_camera &asked) {
    const apkrt_cameras table = table_of();
    const long found = camera_count(table);
    if (found < 0) return found;
    if (asked.camera >= static_cast<std::uint64_t>(found)) return -linux_errno::einval;
    // ADR-0073 decision 8: the guest's camera grant, which the host records (apkrt_permission_answer), is the gate, as
    // the microphone's for audio input: iOS's authorisation is for all of Minato.
    if (permissions::outcome(APKRT_PERMISSION_CAMERA) != APKRT_PERMISSION_GRANTED) return -linux_errno::eacces;
    auto camera = std::shared_ptr<Camera>();
    try {
        camera = std::make_shared<Camera>();
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    std::vector<unsigned char> bytes;
    if (const long answer = host_facts(table, asked.camera, bytes, &camera->facts); answer != 0) return answer;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        if (all.open.count(asked.camera) != 0 || all.open.size() >= APKRT_CAMERA_MAX_OPEN) return -linux_errno::ebusy;
        try {
            all.open.insert(asked.camera);
        } catch (const std::bad_alloc &) {
            return -linux_errno::enomem;
        }
    }
    const auto unclaim = [&] {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        all.open.erase(asked.camera);
    };
    std::uint64_t handle = 0;
    const long opened = host_answer(table.open(table.context, asked.camera, &handle));
    if (opened != 0) {
        unclaim();
        return opened;
    }
    camera->table = table;
    camera->handle = handle;
    camera->index = asked.camera;
    std::uint64_t id = 0;
    {
        auto &all = registry();
        const std::lock_guard hold(all.mutex);
        try {
            all.cameras.emplace(all.next_id, camera);
            id = all.next_id++;
        } catch (const std::bad_alloc &) {
            id = 0;
        }
    }
    if (id == 0) {
        table.close(table.context, handle);
        unclaim();
        return -linux_errno::enomem;
    }
    if (!put(argument, offsetof(apkrt_camera, id), &id, sizeof id)) {
        close_camera(id);  // the guest cannot learn of it
        return -linux_errno::efault;
    }
    return 0;
}

bool listed(const Facts &facts, const apkrt_camera_stream &stream) {
    // A PRIVATE stream takes the sizes of the YUV streams the facts list, as camera2's StreamConfigurationMap does.
    const std::int64_t format = stream.format == APKRT_CAMERA_FORMAT_PRIVATE ? APKRT_CAMERA_FORMAT_YUV_420_888
                                                                            : stream.format;
    for (const auto &entry : facts.streams) {
        if (entry[0] == format && entry[1] == stream.width && entry[2] == stream.height) return true;
    }
    return false;
}

long configure(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    if (asked.count > APKRT_CAMERA_MAX_STREAMS || asked.reserved != 0) return -linux_errno::einval;
    apkrt_camera_stream streams[APKRT_CAMERA_MAX_STREAMS]{};
    if (!take(asked.data, std::uint64_t{asked.count} * sizeof *streams, streams)) return -linux_errno::efault;
    const std::lock_guard hold(camera->mutex);
    if (camera->gone) return -linux_errno::ebadf;
    for (std::uint32_t n = 0; n < asked.count; n++) {
        const apkrt_camera_stream &stream = streams[n];
        const bool format = stream.format == APKRT_CAMERA_FORMAT_PRIVATE ||
                            stream.format == APKRT_CAMERA_FORMAT_YUV_420_888 ||
                            stream.format == APKRT_CAMERA_FORMAT_JPEG;
        if (!format || stream.reserved != 0 || !listed(camera->facts, stream)) return -linux_errno::einval;
    }
    const long answer =
            host_answer(camera->table.configure(camera->table.context, camera->handle, streams, asked.count));
    if (answer != 0) return answer;
    try {
        camera->streams.assign(streams, streams + asked.count);
    } catch (const std::bad_alloc &) {
        camera->streams.clear();
        return -linux_errno::enomem;
    }
    return 0;
}

bool zero(const std::uint8_t *bytes, std::size_t size) {
    for (std::size_t n = 0; n < size; n++) {
        if (bytes[n] != 0) return false;
    }
    return true;
}

bool in(const std::vector<std::int32_t> &values, std::int32_t value) {
    for (const std::int32_t each : values) {
        if (each == value) return true;
    }
    return false;
}

/// Whether the facts allow the controls: modes they list, ranges they give, regions with weights not below 0.
bool allowed(const Facts &facts, const apkrt_camera_controls &controls) {
    if (!zero(controls.reserved0, sizeof controls.reserved0) || !zero(controls.reserved, sizeof controls.reserved)) {
        return false;
    }
    if (!in(facts.af_modes, controls.af_mode) || !in(facts.awb_modes, controls.awb_mode)) return false;
    if (controls.af_trigger > 2 || controls.ae_mode > 4 || controls.ae_lock > 1 || controls.ae_precapture > 2 ||
        controls.awb_lock > 1 || controls.flash_mode > 2 || controls.face_detect > 2) {
        return false;
    }
    if (controls.jpeg_quality < 1 || controls.jpeg_quality > 100) return false;
    if (controls.jpeg_orientation % 90 != 0 || controls.jpeg_orientation < 0 || controls.jpeg_orientation > 270) {
        return false;
    }
    if (!std::isfinite(controls.zoom_ratio) || controls.zoom_ratio < facts.zoom_min ||
        controls.zoom_ratio > facts.zoom_max) {
        return false;
    }
    bool range = false;
    for (const auto &[low, high] : facts.fps) range |= low == controls.fps_min && high == controls.fps_max;
    if (!range) return false;
    return controls.af_region[4] >= 0 && controls.ae_region[4] >= 0;
}

long request(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    if ((asked.flags & ~APKRT_CAMERA_REPEATING) != 0 || asked.reserved != 0) return -linux_errno::einval;
    apkrt_camera_controls controls{};
    if (!take(asked.data, sizeof controls, &controls)) return -linux_errno::efault;
    const std::lock_guard hold(camera->mutex);
    if (camera->gone) return -linux_errno::ebadf;
    const std::uint32_t configured = (1u << camera->streams.size()) - 1;
    if (asked.targets == 0 || (asked.targets & ~configured) != 0 || !allowed(camera->facts, controls)) {
        return -linux_errno::einval;
    }
    return host_answer(camera->table.request(camera->table.context, camera->handle, &controls, asked.number,
                                             asked.targets, asked.flags));
}

long stop_repeating(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(camera->mutex);
    if (camera->gone) return -linux_errno::ebadf;
    camera->table.stop_repeating(camera->table.context, camera->handle);
    return 0;
}

/// Whether the host's capture holds for the camera's streams: streams it has configured, a token (not 0, not one that
/// waits, none twice) for each PRIVATE or YUV stream it has and none for the others, a JPEG within the limit where it
/// has a JPEG stream and none otherwise, at most 4 faces.
bool capture_holds(const Camera &camera, const apkrt_camera_capture &capture) {
    const apkrt_camera_frame &frame = capture.frame;
    const std::uint32_t configured = (1u << camera.streams.size()) - 1;
    if ((frame.streams & ~configured) != 0 || frame.faces > 4) return false;
    bool jpeg = false;
    std::set<std::uint64_t> seen;
    for (std::size_t n = 0; n < 4; n++) {
        const bool has = n < camera.streams.size() && (frame.streams & (1u << n)) != 0;
        const bool by_token = has && camera.streams[n].format != APKRT_CAMERA_FORMAT_JPEG;
        jpeg |= has && camera.streams[n].format == APKRT_CAMERA_FORMAT_JPEG;
        const std::uint64_t token = frame.tokens[n];
        if (by_token != (token != 0)) return false;
        if (token != 0 && (camera.waiting.count(token) != 0 || !seen.insert(token).second)) return false;
    }
    if (jpeg ? capture.jpeg == nullptr || capture.jpeg_size == 0 || capture.jpeg_size > APKRT_CAMERA_MAX_JPEG
             : capture.jpeg_size != 0) {
        return false;
    }
    return true;
}

long frame(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    apkrt_camera_frame given{};
    if (!take(asked.data, sizeof given, &given)) return -linux_errno::efault;
    const std::lock_guard hold(camera->mutex);
    if (camera->gone) return -linux_errno::ebadf;
    if (camera->waiting.size() + camera->streams.size() > APKRT_CAMERA_MAX_WAITING) return -linux_errno::eagain;
    apkrt_camera_capture capture{};
    const long peeked = host_answer(camera->table.peek(camera->table.context, camera->handle, &capture));
    if (peeked != 0) return peeked;
    try {
        if (!capture_holds(*camera, capture)) return -linux_errno::eio;  // the host's frame stays its own
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    apkrt_camera_frame out = capture.frame;
    out.timestamp_ns += boot_minus_uptime();
    out.jpeg = given.jpeg, out.jpeg_capacity = given.jpeg_capacity, out.jpeg_size = capture.jpeg_size;
    std::memset(out.reserved, 0, sizeof out.reserved);
    if (capture.jpeg_size > given.jpeg_capacity) {
        return put(asked.data, offsetof(apkrt_camera_frame, jpeg_size), &out.jpeg_size, sizeof out.jpeg_size)
                       ? -linux_errno::enospc
                       : -linux_errno::efault;
    }
    if (capture.jpeg_size > 0 &&
        (given.jpeg == 0 || !sys::memory::copy_checked(given.jpeg, reinterpret_cast<std::uint64_t>(capture.jpeg),
                                                       capture.jpeg_size))) {
        return -linux_errno::efault;
    }
    std::vector<std::uint64_t> tokens;
    try {
        for (const std::uint64_t token : out.tokens) {
            if (token != 0) tokens.push_back(token);
        }
        camera->waiting.insert(tokens.begin(), tokens.end());  // before the frame leaves the host's queue
    } catch (const std::bad_alloc &) {
        for (const std::uint64_t token : tokens) camera->waiting.erase(token);
        return -linux_errno::enomem;
    }
    if (!put(asked.data, 0, &out, sizeof out)) {
        for (const std::uint64_t token : tokens) camera->waiting.erase(token);
        return -linux_errno::efault;
    }
    camera->table.pop(camera->table.context, camera->handle);
    return 0;
}

/// The camera of a RENDER, RENDER_WINDOW or DISCARD, its lock held, and the waiting token taken: 0, or the answer.
long take_token(const std::shared_ptr<Camera> &camera, std::uint64_t token) {
    if (camera == nullptr || camera->gone) return -linux_errno::ebadf;
    return camera->waiting.erase(token) == 0 ? -linux_errno::einval : 0;
}

long render(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(camera->mutex);
    if (const long taken = take_token(camera, asked.data); taken != 0) return taken;
    apkrt_video_target target{};
    if (asked.size == 0 || apkrt_hardware_buffer_acquire_handle(asked.size, &target.handle, &target.buffers,
                                                                 &target.description, &target.stride) != 0) {
        camera->table.discard(camera->table.context, camera->handle, asked.data);
        return -linux_errno::einval;
    }
    long answer = -linux_errno::einval;
    if (target.description.layers == 1) {
        answer = host_answer(camera->table.render(camera->table.context, camera->handle, asked.data, &target));
    } else {
        camera->table.discard(camera->table.context, camera->handle, asked.data);
    }
    apkrt_hardware_buffer_release_handle(asked.size);
    return answer;
}

/// RENDER_WINDOW: the frame drawn into a host window, which the host finds by its ID. A window of ID 0, flags other
/// than CROP or a time before 0 or past what nanoseconds hold end the token undrawn.
long render_window(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(camera->mutex);
    if (const long taken = take_token(camera, asked.data); taken != 0) return taken;
    if ((asked.flags & ~APKRT_VIDEO_DECODER_CROP) != 0 || asked.time_ns < 0) {
        camera->table.discard(camera->table.context, camera->handle, asked.data);
        return -linux_errno::einval;
    }
    if (asked.size == 0) {
        camera->table.discard(camera->table.context, camera->handle, asked.data);
        return -linux_errno::enodev;
    }
    return host_answer(camera->table.render_window(camera->table.context, camera->handle, asked.data, asked.size,
                                                   asked.time_ns, asked.flags));
}

long discard(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(camera->mutex);
    if (const long taken = take_token(camera, asked.data); taken != 0) return taken;
    camera->table.discard(camera->table.context, camera->handle, asked.data);
    return 0;
}

long flush(const apkrt_camera &asked) {
    const auto camera = find(asked.id);
    if (camera == nullptr) return -linux_errno::ebadf;
    const std::lock_guard hold(camera->mutex);
    if (camera->gone) return -linux_errno::ebadf;
    const long flushed = host_answer(camera->table.flush(camera->table.context, camera->handle));
    if (flushed == 0) camera->waiting.clear();  // a failed flush leaves the guest its tokens
    return flushed;
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) {
    apkrt_camera asked{};
    if (argument == 0 || !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&asked), argument, sizeof asked)) {
        return -linux_errno::efault;
    }
    switch (op) {
        case APKRT_CAMERA_COUNT: return count(argument);
        case APKRT_CAMERA_FACTS: return facts(argument, asked);
        case APKRT_CAMERA_OPEN: return open(argument, asked);
        case APKRT_CAMERA_CONFIGURE: return configure(asked);
        case APKRT_CAMERA_REQUEST: return request(asked);
        case APKRT_CAMERA_STOP_REPEATING: return stop_repeating(asked);
        case APKRT_CAMERA_FRAME: return frame(asked);
        case APKRT_CAMERA_RENDER: return render(asked);
        case APKRT_CAMERA_RENDER_WINDOW: return render_window(asked);
        case APKRT_CAMERA_DISCARD: return discard(asked);
        case APKRT_CAMERA_FLUSH: return flush(asked);
        case APKRT_CAMERA_CLOSE: return close_camera(asked.id);
        default: return -linux_errno::einval;
    }
}

}  // namespace apkrt::cameras

extern "C" int apkrt_set_cameras(const struct apkrt_cameras *cameras) {
    if (cameras != nullptr &&
        (cameras->count == nullptr || cameras->facts == nullptr || cameras->open == nullptr ||
         cameras->configure == nullptr || cameras->request == nullptr || cameras->stop_repeating == nullptr ||
         cameras->peek == nullptr || cameras->pop == nullptr || cameras->render == nullptr ||
         cameras->discard == nullptr || cameras->render_window == nullptr || cameras->flush == nullptr ||
         cameras->close == nullptr)) {
        return -1;
    }
    auto &all = apkrt::cameras::registry();
    const std::lock_guard hold(all.mutex);
    all.table = cameras != nullptr ? *cameras : apkrt_cameras{};
    return 0;
}
