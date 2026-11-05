// apk-run's headless host for --lifecycle (ADR-0043 c4, decision 4): the session's surfaces and requested windows
// (ADR-0041, gap 4), each a window of its own without a layer and answered with the events the iOS host posts, and
// the windows' buffers for drawing by the CPU, one per window, as apkrt.h's struct apkrt_window_buffers asks them to
// behave. The calls to the runtime's registry and session come in from outside, so that headless_host_test.cpp
// records them instead.
#pragma once

#include "APKRuntime/apkrt.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
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

/// What the host does with the requested windows (ADR-0041, gap 4), as Calls has it for the surfaces: in apk-run
/// apkrt_window_add of the request's buffer size, on graphics-run's layer for it or none, apkrt_window_set_gate with
/// 1, apkrt_window_set_size, apkrt_window_remove, apkrt_session_post, and graphics-run's change of the layer for each
/// place.
struct WindowCalls {
    std::function<std::uint64_t(const apkrt_window_request &)> add;
    std::function<void(std::uint64_t)> open;
    std::function<void(std::uint64_t, std::int32_t, std::int32_t)> size;
    std::function<void(std::uint64_t)> remove;
    std::function<void(std::uint32_t, std::uint64_t, std::uint64_t, std::uint64_t)> post;
    std::function<void(std::uint64_t, const apkrt_window_request &)> place;
};

/// The requested windows (apkrt.h's window handler), answered as Surfaces answers the surfaces: a make gets a window
/// and WINDOW_CREATED, then its gate opens unless the session is in the background; a place goes to the window's
/// layer, and one with another size then gets the window's new size and WINDOW_RESIZED; a remove takes the window
/// away without an event. At the background and the end every window goes with the main window, and WINDOW_DESTROYED
/// comes later for each one the guest has not removed, the sub-windows' first, each kind in the order made.
class Windows {
public:
    explicit Windows(WindowCalls calls) : calls_(std::move(calls)) {}

    void request(const apkrt_window_request &request) {
        const std::lock_guard<std::mutex> hold(lock_);
        const auto found = find(request.token);
        switch (request.op) {
            case APKRT_WINDOW_MAKE: {
                if (found != windows_.end()) return;  // the runtime refuses it first
                const std::uint64_t window = calls_.add(request);
                windows_.push_back({request.token, window, request.width, request.height,
                                    request.type >= 1000 && request.type <= 1999});
                calls_.post(APKRT_EVENT_WINDOW_CREATED, request.token, window, pair(request.width, request.height));
                if (!background_) calls_.open(window);
                return;
            }
            case APKRT_WINDOW_PLACE:
                if (found == windows_.end() || found->taken) return;
                calls_.place(found->window, request);
                if (found->width == request.width && found->height == request.height) return;
                found->width = request.width;
                found->height = request.height;
                calls_.size(found->window, request.width, request.height);
                calls_.post(APKRT_EVENT_WINDOW_RESIZED, request.token, found->window,
                            pair(request.width, request.height));
                return;
            case APKRT_WINDOW_REMOVE:
                if (found == windows_.end()) return;
                if (!found->taken) calls_.remove(found->window);
                windows_.erase(found);
                return;
            default:
                return;
        }
    }

    /// At the background and at the end, with the main window, before PAUSE: every window goes.
    void take_away() {
        const std::lock_guard<std::mutex> hold(lock_);
        for (auto &window : windows_) {
            if (window.taken) continue;
            calls_.remove(window.window);
            window.taken = true;
        }
    }

    /// After PAUSE and the focus, before NATIVE_WINDOW_DESTROYED and STOP: WINDOW_DESTROYED for each window take_away
    /// took that the guest has not removed meanwhile, the sub-windows' before the others', each in the order made.
    void post_destroyed() {
        const std::lock_guard<std::mutex> hold(lock_);
        for (const bool subs : {true, false}) {
            for (auto &window : windows_) {
                if (window.sub != subs || !window.taken || window.announced) continue;
                calls_.post(APKRT_EVENT_WINDOW_DESTROYED, window.token, window.window, 0);
                window.announced = true;
            }
        }
    }

    /// The session goes into the background (true) or comes back: at the return the gates of the windows made
    /// meanwhile open.
    void set_background(bool background) {
        const std::lock_guard<std::mutex> hold(lock_);
        background_ = background;
        if (background) return;
        for (const auto &window : windows_) {
            if (!window.taken) calls_.open(window.window);
        }
    }

private:
    struct Window {
        std::uint64_t token = 0, window = 0;
        std::int32_t width = 0, height = 0;
        bool sub = false;        // a sub-window, whose WINDOW_DESTROYED comes before its parent's
        bool taken = false;      // its window went at the background or the end
        bool announced = false;  // with WINDOW_DESTROYED
    };

    std::vector<Window>::iterator find(std::uint64_t token) {
        return std::find_if(windows_.begin(), windows_.end(),
                            [token](const Window &each) { return each.token == token; });
    }

    WindowCalls calls_;
    std::mutex lock_;
    std::vector<Window> windows_;  // in the order they were made
    bool background_ = false;
};

/// The windows' buffers: RGBA_8888, RGBX_8888 and RGB_565, each window's of its size with its width as its stride.
/// The dirty rectangle is the whole buffer for a new one or another geometry, else the guest's cut to the buffer, and
/// 0 0 0 0 when that leaves nothing, as graphics/host's buffers answer. A lock of a window whose buffer a lock holds
/// answers -EBUSY rather than take the buffer from under its writer; a removed window keeps a locked buffer until its
/// post and takes no lock after (-ENODEV).
/// Each window counts its posts, for the corpus harness's "frame" level: mark() notes the counts and each window's
/// last posted frame by its hash before a touch, and since_mark() tells per window what came after: its posts since,
/// whether one of their frames differed from the marked one, and whether the window is new or removed since.
class Buffers {
public:
    /// What a window did since the last mark: posts in all and since, a frame that differed from the one before the
    /// mark, made after the mark (with no frame before), removed after it.
    struct Since {
        std::uint64_t id = 0, posts = 0, since = 0;
        bool changed = false, made = false, removed = false;
    };

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
        if (const auto [count, made] = counts_.try_emplace(id); made) count->second.made = true;
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
        auto &count = counts_[id];
        ++count.posts;
        if (count.watching) {
            // Under the lock, as a mark is: the writer is done with these bytes, and no lock takes them meanwhile.
            const std::uint64_t hash = frame_hash(found->second.pixels);
            if (!count.hashed) {
                count.hash = hash;
                count.hashed = true;
            } else if (hash != count.hash) {
                count.changed = true;
                count.watching = false;
            }
        }
        if (removed_.count(id) != 0) buffers_.erase(found);
        return 0;
    }

    /// Before a touch: each window's posts so far and the hash of its last posted frame, or of the frame it posts next
    /// while a lock holds its buffer or it never posted; from then on each post hashes its frame until one differs.
    /// The windows removed before it are forgotten.
    void mark() {
        const std::lock_guard<std::mutex> hold(lock_);
        for (auto it = counts_.begin(); it != counts_.end();) {
            if (it->second.removed) {
                it = counts_.erase(it);
                continue;
            }
            auto &count = it->second;
            count.at_mark = count.posts;
            count.changed = count.made = false;
            count.watching = true;
            const auto found = buffers_.find(it->first);
            count.hashed = found != buffers_.end() && found->second.posted && !found->second.locked;
            if (count.hashed) count.hash = frame_hash(found->second.pixels);
            ++it;
        }
    }

    /// Every window there at the last mark or made since, by ID; the posts after this call hash no more.
    std::vector<Since> since_mark() {
        const std::lock_guard<std::mutex> hold(lock_);
        std::vector<Since> windows;
        for (auto &[id, count] : counts_) {
            windows.push_back({id, count.posts, count.posts - count.at_mark, count.changed, count.made,
                               count.removed});
            count.watching = false;
        }
        return windows;
    }

    /// Copies a rectangle of window `id`'s last posted buffer into `request`, scaled to the request's size, as
    /// RGBA_8888 by nearest neighbour (apk-run's headless host and its tests; graphics/host's window.m scales with
    /// vImage on the device, so a later pixel comparison must not expect equal edges). RGBX_8888's alpha is 255 and
    /// RGB_565 is widened, as window.m does. Only a window the host still has, with a posted buffer that no lock holds:
    /// -ENODATA otherwise (never posted, locked, removed, a format it never drew); -EINVAL for an empty rectangle or
    /// one outside the buffer. The runtime already refused an id that is not one of the guest's windows and checked the
    /// destination's format, sizes, stride and address.
    int readback(std::uint64_t id, const apkrt_window_readback &request) {
        const std::lock_guard<std::mutex> hold(lock_);
        const auto found = buffers_.find(id);
        if (found == buffers_.end() || !found->second.posted || found->second.locked) return -ENODATA;
        const Buffer &source = found->second;
        const int source_bytes = source.format == 1 || source.format == 2 ? 4 : source.format == 4 ? 2 : 0;
        if (source_bytes == 0) return -ENODATA;
        const std::int32_t left = request.left, top = request.top, right = request.right, bottom = request.bottom;
        if (right <= left || bottom <= top || left < 0 || top < 0 || right > source.width || bottom > source.height) {
            return -EINVAL;
        }
        // The rectangle is within [0, width/height] (checked above), so these are non-negative.
        const auto columns = static_cast<std::uint32_t>(right - left), rows = static_cast<std::uint32_t>(bottom - top);
        const auto source_stride = static_cast<std::size_t>(source.width);
        const auto source_pixel = static_cast<std::size_t>(source_bytes);
        auto *const out = static_cast<std::uint8_t *>(request.pixels);
        for (std::uint32_t y = 0; y < request.height; ++y) {
            const std::uint32_t sy = static_cast<std::uint32_t>(top) +
                                     static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * rows / request.height);
            for (std::uint32_t x = 0; x < request.width; ++x) {
                const std::uint32_t sx =
                        static_cast<std::uint32_t>(left) +
                        static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * columns / request.width);
                const std::uint8_t *const in =
                        source.pixels.data() + (static_cast<std::size_t>(sy) * source_stride + sx) * source_pixel;
                std::uint8_t *const pixel = out + static_cast<std::size_t>(y) * request.stride +
                                            static_cast<std::size_t>(x) * std::size_t{4};
                if (source_bytes == 4) {
                    pixel[0] = in[0];
                    pixel[1] = in[1];
                    pixel[2] = in[2];
                    pixel[3] = source.format == 2 ? 255 : in[3];  // RGBX_8888 is opaque
                } else {
                    const auto p = static_cast<std::uint16_t>(in[0] | (in[1] << 8));  // RGB_565, little-endian
                    pixel[0] = static_cast<std::uint8_t>(((p >> 11 & 0x1f) * 255 + 15) / 31);
                    pixel[1] = static_cast<std::uint8_t>(((p >> 5 & 0x3f) * 255 + 31) / 63);
                    pixel[2] = static_cast<std::uint8_t>(((p & 0x1f) * 255 + 15) / 31);
                    pixel[3] = 255;
                }
            }
        }
        return 0;
    }

    /// The window is gone: its buffer goes now, or at its post while a lock holds it.
    void removed(std::uint64_t id) {
        const std::lock_guard<std::mutex> hold(lock_);
        removed_.insert(id);
        if (const auto count = counts_.find(id); count != counts_.end()) count->second.removed = true;
        const auto found = buffers_.find(id);
        if (found != buffers_.end() && !found->second.locked) buffers_.erase(found);
    }

private:
    struct Buffer {
        std::vector<std::uint8_t> pixels;
        std::int32_t width = 0, height = 0, format = 0;
        bool locked = false, posted = false;
    };

    /// A window's posts and the frame before the last mark, kept past its removal until the next mark.
    struct Count {
        std::uint64_t posts = 0, at_mark = 0, hash = 0;
        bool hashed = false;    // `hash` is the frame before the mark's
        bool watching = false;  // posts hash their frames until one differs
        bool changed = false, made = false, removed = false;
    };

    /// FNV-1a over the frame's bytes, 8 at a time: the window's pixels and nothing else, as its stride is its width.
    static std::uint64_t frame_hash(const std::vector<std::uint8_t> &pixels) {
        constexpr std::uint64_t prime = 0x100000001b3;
        std::uint64_t hash = 0xcbf29ce484222325;
        std::size_t at = 0;
        for (; at + 8 <= pixels.size(); at += 8) {
            std::uint64_t word;
            std::memcpy(&word, pixels.data() + at, sizeof word);
            hash = (hash ^ word) * prime;
        }
        for (; at < pixels.size(); ++at) hash = (hash ^ pixels[at]) * prime;
        return hash;
    }

    std::mutex lock_;
    std::map<std::uint64_t, Buffer> buffers_;
    std::set<std::uint64_t> removed_;  // IDs are never reused
    std::map<std::uint64_t, Count> counts_;  // every window with a buffer since the last mark, by ID
};

}  // namespace headless
