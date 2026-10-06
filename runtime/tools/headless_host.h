// apk-run's headless host for --lifecycle (ADR-0043 c4, decision 4): the session's surfaces, each a window of its own
// without a layer and answered with the events the iOS host posts, and the windows' buffers for drawing by the CPU,
// one per window, as apkrt.h's struct apkrt_window_buffers asks them to behave. The calls to the runtime's registry
// and session come in from outside, so that headless_host_test.cpp records them instead.
#pragma once

#include "APKRuntime/apkrt.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace headless {

/// Two 32-bit values in one event argument, `low` in the low half.
inline std::uint64_t pair(std::int32_t low, std::int32_t high) {
    return static_cast<std::uint32_t>(low) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32;
}

/// What the host does with the runtime's windows and session: in apk-run apkrt_window_add of the request's buffer
/// size, on graphics-run's layer for it or none, apkrt_window_set_gate with 1, apkrt_window_set_size,
/// apkrt_window_remove, apkrt_session_post, and graphics-run's move of the layer for each place.
struct Calls {
    std::function<std::uint64_t(const apkrt_surface_request &)> add;
    std::function<void(std::uint64_t)> open;
    std::function<void(std::uint64_t, std::int32_t, std::int32_t)> size;
    std::function<void(std::uint64_t)> remove;
    std::function<void(std::uint32_t, std::uint64_t, std::uint64_t, std::uint64_t)> post;
    std::function<void(std::uint64_t, const apkrt_surface_request &)> place;
};

/// The surfaces (apkrt.h's surface handler), from the guest's threads, and the lifecycle's steps, from its thread.
class Surfaces {
public:
    explicit Surfaces(Calls calls) : calls_(std::move(calls)) {}

    /// A request the runtime checked: a make gets a window and SURFACE_CREATED, then its gate opens unless the
    /// session is in the background; a place goes to the window's layer, and one with another size then gets the
    /// window's new size and SURFACE_RESIZED; a remove takes the window away without an event. A place or remove of
    /// a surface the host took away does nothing, but the remove forgets it.
    void request(const apkrt_surface_request &request) {
        const std::lock_guard<std::mutex> hold(lock_);
        const auto found = find(request.token);
        switch (request.op) {
            case APKRT_SURFACE_MAKE: {
                if (found != surfaces_.end()) return;  // the runtime refuses it first
                const std::uint64_t window = calls_.add(request);
                surfaces_.push_back({request.token, window, request.width, request.height});
                calls_.post(APKRT_EVENT_SURFACE_CREATED, request.token, window, pair(request.width, request.height));
                if (!background_) calls_.open(window);
                return;
            }
            case APKRT_SURFACE_PLACE:
                if (found == surfaces_.end() || found->taken) return;
                calls_.place(found->window, request);
                if (found->width == request.width && found->height == request.height) return;
                found->width = request.width;
                found->height = request.height;
                calls_.size(found->window, request.width, request.height);
                calls_.post(APKRT_EVENT_SURFACE_RESIZED, request.token, found->window,
                            pair(request.width, request.height));
                return;
            case APKRT_SURFACE_REMOVE:
                if (found == surfaces_.end()) return;
                if (!found->taken) calls_.remove(found->window);
                surfaces_.erase(found);
                return;
            default:
                return;
        }
    }

    /// At the background and at the end, with the main window, before PAUSE: every surface's window goes.
    void take_away() {
        const std::lock_guard<std::mutex> hold(lock_);
        for (auto &surface : surfaces_) {
            if (surface.taken) continue;
            calls_.remove(surface.window);
            surface.taken = true;
        }
    }

    /// After PAUSE and the focus, before NATIVE_WINDOW_DESTROYED and STOP, where Android 15 calls surfaceDestroyed
    /// from the stopped window: SURFACE_DESTROYED for each surface take_away took that the guest has not removed
    /// meanwhile, in the order they were made.
    void post_destroyed() {
        const std::lock_guard<std::mutex> hold(lock_);
        for (auto &surface : surfaces_) {
            if (!surface.taken || surface.announced) continue;
            calls_.post(APKRT_EVENT_SURFACE_DESTROYED, surface.token, surface.window, 0);
            surface.announced = true;
        }
    }

    /// The session goes into the background (true) or comes back: at the return the gates of the surfaces made
    /// meanwhile open.
    void set_background(bool background) {
        const std::lock_guard<std::mutex> hold(lock_);
        background_ = background;
        if (background) return;
        for (const auto &surface : surfaces_) {
            if (!surface.taken) calls_.open(surface.window);
        }
    }

private:
    struct Surface {
        std::uint64_t token = 0, window = 0;
        std::int32_t width = 0, height = 0;
        bool taken = false;      // its window went at the background or the end
        bool announced = false;  // with SURFACE_DESTROYED
    };

    std::vector<Surface>::iterator find(std::uint64_t token) {
        for (auto it = surfaces_.begin(); it != surfaces_.end(); ++it) {
            if (it->token == token) return it;
        }
        return surfaces_.end();
    }

    Calls calls_;
    std::mutex lock_;
    std::vector<Surface> surfaces_;  // in the order they were made
    bool background_ = false;
};

/// The windows' buffers: RGBA_8888, RGBX_8888 and RGB_565, each window's of its size with its width as its stride.
/// The dirty rectangle is the whole buffer for a new one or another geometry, else the guest's cut to the buffer, and
/// 0 0 0 0 when that leaves nothing, as graphics/host's buffers answer. A lock of a window whose buffer a lock holds
/// answers -EBUSY rather than take the buffer from under its writer; a removed window keeps a locked buffer until its
/// post and takes no lock after (-ENODEV).
class Buffers {
public:
    int lock(std::uint64_t id, apkrt_window_buffer &buffer) {
        const int bytes = buffer.format == 1 || buffer.format == 2 ? 4 : buffer.format == 4 ? 2 : 0;
        if (bytes == 0 || buffer.width <= 0 || buffer.height <= 0) return -EINVAL;
        const std::lock_guard<std::mutex> hold(lock_);
        if (removed_.count(id) != 0) return -ENODEV;
        auto &held = buffers_[id];
        if (held.locked) return -EBUSY;
        std::int32_t *const dirty = buffer.dirty;
        if (held.pixels.empty() || buffer.width != held.width || buffer.height != held.height ||
            buffer.format != held.format) {
            held.pixels.assign(static_cast<std::size_t>(buffer.width) * static_cast<std::size_t>(buffer.height) *
                                   static_cast<std::size_t>(bytes),
                               0);
            held.width = buffer.width;
            held.height = buffer.height;
            held.format = buffer.format;
            dirty[0] = dirty[1] = 0;
            dirty[2] = buffer.width;
            dirty[3] = buffer.height;
        } else {
            dirty[0] = std::max(dirty[0], 0);
            dirty[1] = std::max(dirty[1], 0);
            dirty[2] = std::min(dirty[2], buffer.width);
            dirty[3] = std::min(dirty[3], buffer.height);
            if (dirty[0] >= dirty[2] || dirty[1] >= dirty[3]) dirty[0] = dirty[1] = dirty[2] = dirty[3] = 0;
        }
        held.locked = true;
        buffer.stride = buffer.width;
        buffer.bits = held.pixels.data();
        return 0;
    }

    /// The post of a locked buffer: 0, with `first` for the window's first post; -EINVAL without a lock.
    int post(std::uint64_t id, bool &first) {
        const std::lock_guard<std::mutex> hold(lock_);
        const auto found = buffers_.find(id);
        if (found == buffers_.end() || !found->second.locked) return -EINVAL;
        first = !found->second.posted;
        found->second.posted = true;
        found->second.locked = false;
        if (removed_.count(id) != 0) buffers_.erase(found);
        return 0;
    }

    /// The window is gone: its buffer goes now, or at its post while a lock holds it.
    void removed(std::uint64_t id) {
        const std::lock_guard<std::mutex> hold(lock_);
        removed_.insert(id);
        const auto found = buffers_.find(id);
        if (found != buffers_.end() && !found->second.locked) buffers_.erase(found);
    }

private:
    struct Buffer {
        std::vector<std::uint8_t> pixels;
        std::int32_t width = 0, height = 0, format = 0;
        bool locked = false, posted = false;
    };

    std::mutex lock_;
    std::map<std::uint64_t, Buffer> buffers_;
    std::set<std::uint64_t> removed_;  // IDs are never reused
};

}  // namespace headless
