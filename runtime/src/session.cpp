#include "session.h"

#include "channels.h"
#include "descriptors.h"
#include "fake_path_monitor.h"
#include "guest_context.h"
#include "audio.h"
#include "memory_watermark.h"
#include "permissions.h"
#include "install_sources.h"
#include "stall_watchdog.h"
#include "syscalls.h"
#include "windows.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>  // os_proc_available_memory is iOS-only; the macOS host build (tests) leaves it out
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apkrt::session {
namespace {

namespace linux_errno = sys::linux_errno;

constexpr std::uint64_t keep_screen_on = 0x80;  // AWINDOW_FLAG_KEEP_SCREEN_ON
constexpr std::size_t text_limit = 1024;        // of a START_FAILED text, the NUL included

std::atomic_flag claimed = ATOMIC_FLAG_INIT;  // by the first apkrt_session_begin that is valid
apkrt_session described{};                    // written once, before `begun`
std::atomic<bool> begun{false};
// 1 from onPause/onStop until onResume; libc's minato_activity_paused reads it
std::atomic<int> activity_paused{0};
std::atomic<double> render_scale{1.0};  // ADR-0067 point 3: the session configuration the guest reads is scaled
                                        // by this (0.25 to 1, applied on read); 1 is no change

// The configuration the guest sees under the render scale: the size rounds (at least one pixel an axis) and the
// density scale follows, nothing else changes. memcpy copies the tail padding that apkrt_session_begin cleared.
apkrt_configuration scaled_configuration(const apkrt_configuration &full, double scale) {
    apkrt_configuration view;
    std::memcpy(&view, &full, sizeof view);
    if (scale < 1) {
        view.width = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(full.width * scale)));
        view.height = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(full.height * scale)));
        view.scale = full.scale * scale;
    }
    return view;
}

// The channel. Posts, answers and the end change it under `lock`, and `changed` wakes the host's waits.
std::mutex lock;
std::condition_variable changed;
int write_end = -1;        // the event pipe's, the runtime's; non-blocking
int read_end = -1;         // until activity-host connects: then its guest descriptor holds it
bool connected = false;
std::uint32_t posted = 0;    // the last serial number posted
std::uint32_t answered = 0;  // the last serial number answered
// The last answers with their times (CLOCK_UPTIME_RAW ns), oldest first once full, for apkrt_session_answer_time.
struct Answered {
    std::uint32_t serial;
    std::uint64_t time;
};
constexpr std::size_t answers_kept = 64;
std::array<Answered, answers_kept> answer_times{};
std::size_t answer_count = 0;  // answers so far; the ring holds the last answers_kept
bool broken = false;         // a post found the pipe full, or no reader: final
bool ended = false;          // the program has ended

// The window flags and format. A change and its report happen under one lock, so the reports come in the
// order of the changes, from whichever threads.
std::mutex flags_lock;
std::uint64_t window_flags = 0;
std::int32_t window_format = 4;  // PixelFormat.RGB_565, which NativeActivity.onCreate sets

/// PixelFormat.formatHasAlpha (Android 15): these have alpha, every other value has none. ViewRootImpl makes
/// a window's surface opaque when its format has none.
bool has_alpha(std::int32_t format) {
    switch (format) {
        case 8:     // A_8
        case 0xA:   // LA_88
        case 7:     // RGBA_4444
        case 6:     // RGBA_5551
        case 1:     // RGBA_8888
        case 0x16:  // RGBA_F16
        case 0x2B:  // RGBA_1010102
        case -3:    // TRANSLUCENT
        case -2:    // TRANSPARENT
            return true;
        default:
            return false;
    }
}

/// Calls one of the host's handlers with the runtime signal blocked: the handler is host code, whose calls must not
/// end with EINTR.
template <typename Call>
void as_host(Call call) {
    sigset_t runtime, before;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_BLOCK, &runtime, &before);
    call();
    ::pthread_sigmask(SIG_SETMASK, &before, nullptr);
}

/// Calls the host's request handler.
void report(int type, std::uint64_t argument, const char *text) {
    if (described.request == nullptr) return;
    as_host([&] { described.request(described.context, type, argument, text); });
}

// The surfaces' live tokens (ADR-0043 c4), each with its make's format. A request's check, its change of the tokens
// and the host's call happen under one lock, so the host gets the requests in the order the runtime took them.
static_assert(sizeof(apkrt_surface_request) == 48);
constexpr std::size_t surface_limit = 16;
constexpr std::int64_t buffer_side_limit = 8192;
constexpr std::int64_t frame_side_limit = std::int64_t{1} << 20;

struct LiveSurface {
    std::uint64_t token;
    std::int32_t format;
};

std::mutex surfaces_lock;
std::vector<LiveSurface> live_surfaces;

/// APKRT_REQUEST_SURFACE: the request at `address` checked, its buffer's size and opacity set, and handed to the
/// host's surface handler. 0, or the negative errno (guest_interface.h).
long surface(std::uint64_t address) {
    if (described.surface == nullptr) return -linux_errno::eopnotsupp;
    apkrt_surface_request request;
    if (address == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&request), address, sizeof request)) {
        return -linux_errno::efault;
    }
    if (request.op != APKRT_SURFACE_MAKE && request.op != APKRT_SURFACE_PLACE && request.op != APKRT_SURFACE_REMOVE) {
        return -linux_errno::einval;
    }
    if (request.token == 0) return -linux_errno::einval;
    apkrt_surface_request host{};
    host.op = request.op;
    host.token = request.token;
    if (request.op != APKRT_SURFACE_REMOVE) {
        const std::int64_t frame_width = std::int64_t{request.right} - request.left;
        const std::int64_t frame_height = std::int64_t{request.bottom} - request.top;
        if (frame_width <= 0 || frame_height <= 0 || frame_width > frame_side_limit ||
            frame_height > frame_side_limit) {
            return -linux_errno::einval;
        }
        if (request.z == 0 || request.z < -2 || request.z > 2) return -linux_errno::einval;
        if ((request.width == 0) != (request.height == 0)) return -linux_errno::einval;
        const std::int64_t width = request.width == 0 ? frame_width : request.width;
        const std::int64_t height = request.height == 0 ? frame_height : request.height;
        if (width < 1 || height < 1 || width > buffer_side_limit || height > buffer_side_limit) {
            return -linux_errno::einval;
        }
        host = request;
        host.width = static_cast<std::int32_t>(width);
        host.height = static_cast<std::int32_t>(height);
    }
    const std::lock_guard<std::mutex> hold(surfaces_lock);
    const auto found = std::find_if(live_surfaces.begin(), live_surfaces.end(),
                                    [&](const LiveSurface &live) { return live.token == request.token; });
    switch (request.op) {
        case APKRT_SURFACE_MAKE:
            if (found != live_surfaces.end()) return -linux_errno::eexist;
            if (live_surfaces.size() == surface_limit) return -linux_errno::enospc;
            live_surfaces.push_back({request.token, request.format});
            break;
        case APKRT_SURFACE_PLACE:
            if (found == live_surfaces.end()) return -linux_errno::enoent;
            host.format = found->format;
            break;
        default:  // remove
            if (found == live_surfaces.end()) return -linux_errno::enoent;
            live_surfaces.erase(found);
            break;
    }
    if (request.op != APKRT_SURFACE_REMOVE) host.opaque = has_alpha(host.format) ? 0 : 1;
    as_host([&] { described.surface(described.context, &host); });
    return 0;
}

// The windows' live tokens (ADR-0041), each with its make's type, parent, group and format, apart from the
// surfaces'. As for those, a request's check, its change of the tokens and the host's call happen under one lock.
static_assert(sizeof(apkrt_window_request) == 72);
static_assert(offsetof(apkrt_window_request, op) == 0 && offsetof(apkrt_window_request, type) == 4 &&
              offsetof(apkrt_window_request, token) == 8 && offsetof(apkrt_window_request, parent) == 16 &&
              offsetof(apkrt_window_request, left) == 24 && offsetof(apkrt_window_request, top) == 28 &&
              offsetof(apkrt_window_request, right) == 32 && offsetof(apkrt_window_request, bottom) == 36 &&
              offsetof(apkrt_window_request, width) == 40 && offsetof(apkrt_window_request, height) == 44 &&
              offsetof(apkrt_window_request, format) == 48 && offsetof(apkrt_window_request, opaque) == 52 &&
              offsetof(apkrt_window_request, group) == 56 && offsetof(apkrt_window_request, alpha) == 60 &&
              offsetof(apkrt_window_request, dim) == 64 && offsetof(apkrt_window_request, reserved) == 68);
constexpr std::size_t window_limit = 16;
constexpr std::int32_t type_toast = 2005;

struct LiveWindow {
    std::uint64_t token, parent;
    std::int32_t type, format;
    std::uint32_t group;
};

std::mutex windows_lock;
std::vector<LiveWindow> live_windows;

/// LayoutParams' sub-window types, FIRST_SUB_WINDOW to LAST_SUB_WINDOW.
bool sub_window(std::int32_t type) {
    return type >= 1000 && type <= 1999;
}

/// Whether a float's `bits` hold a finite value within 0 to 1, as alpha and dimAmount are.
bool unit_interval(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

// APKRT_REQUEST_PICK (ADR-0057 step a): the host parses the text of a request that the guest wrote, so the runtime
// checks every field before the host sees it.
constexpr std::size_t pick_text_limit = 1536;   // the NUL included
constexpr std::size_t pick_types_limit = 8;     // MIME types in a request
constexpr std::size_t pick_type_length = 100;   // of one MIME type
constexpr std::size_t pick_title_length = 200;  // bytes of the title
constexpr unsigned pick_max_limit = 1000;       // of the photo picker's maximum
constexpr std::size_t pick_live_limit = 4;      // requests that wait for their answer at once

/// Whether a code point may appear in a title: no control characters (C0, DEL, C1), no line or paragraph separators,
/// no zero-width or bidirectional formatting characters, no byte order mark, no interlinear annotation marks.
bool plain_code_point(std::uint32_t point) {
    if (point < 0x20 || (point >= 0x7f && point <= 0x9f)) return false;
    if (point == 0x34f || point == 0x61c) return false;  // combining grapheme joiner, Arabic letter mark
    if ((point >= 0x200b && point <= 0x200f) || (point >= 0x2028 && point <= 0x202e)) return false;
    if (point >= 0x2060 && point <= 0x206f) return false;
    // The stray tag characters, but not the flag tags U+E0020-E007F, which a subdivision flag (U+1F3F4 and then tags)
    // is made of, so those stay whole; a URL or file name drops them instead (visible_code_point).
    if (point >= 0xe0000 && point <= 0xe001f) return false;
    return point != 0xfeff && !(point >= 0xfff9 && point <= 0xfffb);
}

/// Whether `text` is well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF) whose every code
/// point `allowed` takes.
template <typename Allowed>
bool utf8_code_points(std::string_view text, Allowed allowed) {
    for (std::size_t at = 0; at < text.size();) {
        const auto byte = static_cast<unsigned char>(text[at]);
        std::size_t more = 0;
        std::uint32_t point = byte;
        if (byte < 0x80) {
            more = 0;
        } else if (byte >= 0xc2 && byte <= 0xdf) {
            more = 1;
            point = byte & 0x1f;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            more = 2;
            point = byte & 0x0f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            more = 3;
            point = byte & 0x07;
        } else {
            return false;
        }
        for (std::size_t i = 1; i <= more; ++i) {
            if (at + i >= text.size() || (static_cast<unsigned char>(text[at + i]) & 0xc0) != 0x80) return false;
            point = (point << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3f);
        }
        if (more > 0) {
            const auto second = static_cast<unsigned char>(text[at + 1]);
            if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second > 0x9f) ||
                (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second > 0x8f)) {
                return false;
            }
        }
        if (!allowed(point)) return false;
        at += more + 1;
    }
    return true;
}

/// Whether `text` is well-formed UTF-8 of plain code points.
bool plain_utf8(std::string_view text) { return utf8_code_points(text, plain_code_point); }

/// Whether `token` is a MIME type's type or subtype: RFC 6838's characters, or "*" alone.
bool mime_token(std::string_view token) {
    if (token.empty()) return false;
    if (token == "*") return true;
    for (const char c : token) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             (c != '\0' && std::strchr("!#$&^_.+-", c) != nullptr);
        if (!allowed) return false;
    }
    return true;
}

/// Whether `type` is a MIME type or a pattern such as image/* or */*: two tokens around one '/', at most
/// pick_type_length bytes.
bool mime_type(std::string_view type) {
    if (type.size() < 3 || type.size() > pick_type_length) return false;
    const auto slash = type.find('/');
    if (slash == std::string_view::npos) return false;
    return mime_token(type.substr(0, slash)) && mime_token(type.substr(slash + 1));
}

/// Whether `text` is "<kind>\t<flags>\t<max>\t<MIME types>\t<title>": a kind 1 to 5; flags 0 or 1, 1 only for the opens
/// and the photo picker (kinds 1, 2 and 5); a maximum without leading zeros, at most pick_max_limit, other than 0 only
/// for the photo picker; up to pick_types_limit MIME types separated by newlines, none is allowed, but for a folder
/// (kind 4); a title of at most pick_title_length bytes of plain UTF-8, only for a save (kind 3).
bool valid_pick(std::string_view text) {
    std::string_view field[5];
    std::size_t count = 0;
    for (std::size_t from = 0;;) {
        const auto tab = text.find('\t', from);
        if (count == 5) return false;
        field[count++] = text.substr(from, tab == std::string_view::npos ? std::string_view::npos : tab - from);
        if (tab == std::string_view::npos) break;
        from = tab + 1;
    }
    if (count != 5) return false;
    if (field[0].size() != 1 || field[0][0] < '1' || field[0][0] > '5') return false;
    const int kind = field[0][0] - '0';
    if (field[1].size() != 1 || (field[1][0] != '0' && field[1][0] != '1')) return false;
    if (field[1][0] == '1' && kind != 1 && kind != 2 && kind != 5) return false;
    if (field[2].empty() || field[2].size() > 4 || (field[2].size() > 1 && field[2][0] == '0')) return false;
    unsigned maximum = 0;
    for (const char c : field[2]) {
        if (c < '0' || c > '9') return false;
        maximum = maximum * 10 + static_cast<unsigned>(c - '0');
    }
    if (maximum > pick_max_limit || (maximum != 0 && kind != 5)) return false;
    if (!field[3].empty()) {
        if (kind == 4) return false;
        std::size_t types = 0;
        for (std::size_t from = 0;;) {
            const auto newline = field[3].find('\n', from);
            if (++types > pick_types_limit) return false;
            const auto type = field[3].substr(from, newline == std::string_view::npos ? std::string_view::npos
                                                                                        : newline - from);
            if (!mime_type(type)) return false;
            if (newline == std::string_view::npos) break;
            from = newline + 1;
        }
    }
    if (!field[4].empty() && kind != 3) return false;
    return field[4].size() <= pick_title_length && plain_utf8(field[4]);
}

// APKRT_REQUEST_EXPORT_MEDIA (ADR-0057 step e): "<kind>\t<path>", the path of a finished image or video below the app's
// data folder. The host opens it, so the runtime checks it as the framework checks a pick manifest's names.
constexpr std::size_t export_path_limit = 1024;      // bytes of the path
constexpr std::size_t export_components_limit = 16;  // names in it
constexpr std::size_t export_name_limit = 255;       // bytes of one name

/// Whether `text` is "<kind>\t<path>": a kind 1 or 2, then a path of 1 to export_components_limit names of 1 to
/// export_name_limit bytes of plain UTF-8 without '/' and not "." or "..", separated by single '/', in all at most
/// export_path_limit bytes.
bool valid_export(std::string_view text) {
    if (text.size() < 3 || (text[0] != '1' && text[0] != '2') || text[1] != '\t') return false;
    const std::string_view path = text.substr(2);
    if (path.size() > export_path_limit) return false;
    std::size_t names = 0;
    for (std::size_t from = 0;;) {
        const auto slash = path.find('/', from);
        const auto name = path.substr(from, slash == std::string_view::npos ? std::string_view::npos : slash - from);
        if (++names > export_components_limit || name.empty() || name.size() > export_name_limit || name == "." ||
            name == ".." || !plain_utf8(name)) {
            return false;
        }
        if (slash == std::string_view::npos) break;
        from = slash + 1;
    }
    return true;
}

/// The IDs of the requests of one kind that wait for their answer, at most N, kept without allocating: the requests are
/// taken inside the dispatch, which answers an errno and lets no exception out.
template <std::size_t N>
class WaitingIds {
public:
    bool contains(std::uint64_t id) const {
        for (std::size_t at = 0; at < count_; ++at) {
            if (ids_[at] == id) return true;
        }
        return false;
    }
    bool full() const { return count_ == N; }
    void add(std::uint64_t id) { ids_[count_++] = id; }
    /// Forgets `id`; whether it was there.
    bool remove(std::uint64_t id) {
        for (std::size_t at = 0; at < count_; ++at) {
            if (ids_[at] != id) continue;
            ids_[at] = ids_[--count_];
            return true;
        }
        return false;
    }

private:
    std::array<std::uint64_t, N> ids_{};
    std::size_t count_ = 0;
};

// The pick requests that wait for their answer (APKRT_EVENT_PICK_RESULT or APKRT_REQUEST_PICK_CANCEL). The check, the
// change of the table and the host's call happen under pick_handler_lock, so the host gets the requests one at a time,
// in the order the runtime took them. The table has its own lock, held only for the change itself, so that a post of
// the answer never waits for a handler: a handler may wait for another thread that posts the answer. The handler lock
// is recursive, so that a handler may also post the answer or cancel at once.
std::recursive_mutex pick_handler_lock;
std::mutex pick_lock;
WaitingIds<pick_live_limit> live_picks;

/// Forgets pick request `id`; whether it was waiting.
bool pick_done(std::uint64_t id) {
    const std::lock_guard<std::mutex> hold(pick_lock);
    return live_picks.remove(id);
}

// APKRT_REQUEST_OPEN (ADR-0062): as with a pick, the host parses the text of a request that the guest wrote, so the
// runtime checks every field before the host sees it.
constexpr std::size_t open_text_limit = 204801;      // the NUL included
constexpr std::size_t open_fields_limit = 24;        // the kind and a share's 3 + 2 * 10 fields
constexpr std::size_t open_url_length = 4096;        // decoded bytes of a URL
constexpr std::size_t open_subject_length = 1000;    // decoded bytes of a share's subject
constexpr std::size_t open_share_text_length = 65536;  // decoded bytes of a share's text
constexpr std::size_t open_name_length = 255;        // decoded bytes of a share file's name
constexpr unsigned open_files_limit = 10;            // files in a share
constexpr std::size_t open_live_limit = 4;           // requests that wait for their answer at once

/// The value of a hex digit, or -1.
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Whether `c` is one of RFC 3986's unreserved bytes.
bool unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

/// Whether a code point shows, for a URL or a file name the user sees: a plain one (plain_code_point) that is no
/// space separator (Unicode's Zs, but U+0020 when `space` allows it) and no other format character (Cf) either.
bool visible_code_point(std::uint32_t point, bool space) {
    if (!plain_code_point(point)) return false;
    if (point == 0x20) return space;
    if (point == 0xa0 || point == 0x1680 || (point >= 0x2000 && point <= 0x200a) || point == 0x202f ||
        point == 0x205f || point == 0x3000) {
        return false;  // Zs
    }
    if (point == 0xad || (point >= 0x600 && point <= 0x605) || point == 0x61c || point == 0x6dd || point == 0x70f ||
        (point >= 0x890 && point <= 0x891) || point == 0x8e2 || point == 0x180e || point == 0x110bd ||
        point == 0x110cd ||
        (point >= 0x13430 && point <= 0x1343f) || (point >= 0x1bca0 && point <= 0x1bca3) ||
        (point >= 0x1d173 && point <= 0x1d17a) || point == 0xe0001 || (point >= 0xe0020 && point <= 0xe007f)) {
        return false;  // the Cf that plain_code_point lets through
    }
    return true;
}

/// Decodes a percent-encoded field into `out`, which has room for the field's bytes: RFC 3986's unreserved bytes raw,
/// every other byte as % and two hex digits. The decoded length, or -1 when the field is not so encoded.
long percent_decode(std::string_view field, char *out) {
    std::size_t length = 0;
    for (std::size_t at = 0; at < field.size(); ++at) {
        const char c = field[at];
        if (unreserved(c)) {
            out[length++] = c;
            continue;
        }
        if (c != '%' || field.size() - at < 3) return -1;
        const int high = hex_digit(field[at + 1]), low = hex_digit(field[at + 2]);
        if (high < 0 || low < 0) return -1;
        const auto byte = static_cast<char>((high << 4) | low);
        if (unreserved(byte)) return -1;  // an unreserved byte is raw (hex digits may be of either case)
        out[length++] = byte;
        at += 2;
    }
    return static_cast<long>(length);
}

/// Whether a decoded URL is 1 to open_url_length bytes of UTF-8 of visible code points without spaces, whose scheme, up
/// to the first ':' and with ASCII's case ignored, is one of `schemes`.
bool open_url(std::string_view url, std::initializer_list<std::string_view> schemes) {
    const auto visible = [](std::uint32_t point) { return visible_code_point(point, false); };
    if (url.empty() || url.size() > open_url_length || !utf8_code_points(url, visible)) return false;
    const auto colon = url.find(':');
    if (colon == std::string_view::npos) return false;
    const auto scheme = url.substr(0, colon);
    return std::any_of(schemes.begin(), schemes.end(), [scheme](std::string_view allowed) {
        if (scheme.size() != allowed.size()) return false;
        for (std::size_t at = 0; at < scheme.size(); ++at) {
            char c = scheme[at];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != allowed[at]) return false;
        }
        return true;
    });
}

/// Whether a decoded share file name is 1 to open_name_length bytes of UTF-8 of visible code points or spaces, without
/// '/', and not "." or "..".
bool open_name(std::string_view name) {
    const auto visible = [](std::uint32_t point) { return visible_code_point(point, true); };
    return !name.empty() && name.size() <= open_name_length && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && utf8_code_points(name, visible);
}

/// Whether `text` is an open request of guest_interface.h's grammar: "<kind>\t<field>...", the fields percent-encoded
/// and each, decoded, well-formed UTF-8 without NUL and within its limit; a kind 1 to 4 with its number of fields: a
/// URL of the kind's scheme for kinds 1 to 3, a share's subject, text, count of files without leading zeros (0 to
/// open_files_limit), the files' names, each once, and their MIME types for kind 4. The fields are decoded into
/// `decoded`, which has room for the text's bytes.
bool valid_open(std::string_view text, char *decoded) {
    std::string_view encoded[open_fields_limit];
    std::size_t count = 0;
    for (std::size_t from = 0;;) {
        const auto tab = text.find('\t', from);
        if (count == open_fields_limit) return false;
        encoded[count++] = text.substr(from, tab == std::string_view::npos ? std::string_view::npos : tab - from);
        if (tab == std::string_view::npos) break;
        from = tab + 1;
    }
    if (encoded[0].size() != 1 || encoded[0][0] < '1' || encoded[0][0] > '4') return false;
    const int kind = encoded[0][0] - '0';
    std::string_view field[open_fields_limit];
    std::size_t used = 0;
    for (std::size_t at = 1; at < count; ++at) {
        const long length = percent_decode(encoded[at], decoded + used);
        if (length < 0) return false;
        field[at] = std::string_view(decoded + used, static_cast<std::size_t>(length));
        used += static_cast<std::size_t>(length);
        if (!utf8_code_points(field[at], [](std::uint32_t point) { return point != 0; })) return false;
    }
    switch (kind) {
        case 1:
            return count == 2 && open_url(field[1], {"http", "https", "tel", "maps"});
        case 2:
            return count == 2 && open_url(field[1], {"mailto"});
        case 3:
            return count == 2 && open_url(field[1], {"sms"});
        default:
            break;
    }
    if (count < 4 || field[1].size() > open_subject_length || field[2].size() > open_share_text_length) return false;
    const std::string_view files = field[3];
    if (files.empty() || files.size() > 2 || (files.size() > 1 && files[0] == '0')) return false;
    unsigned n = 0;
    for (const char c : files) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<unsigned>(c - '0');
    }
    if (n > open_files_limit || count != 4 + 2 * static_cast<std::size_t>(n)) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (!open_name(field[4 + i])) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (field[4 + j] == field[4 + i]) return false;
        }
        if (!mime_type(field[4 + n + i])) return false;
    }
    return true;
}

// The open requests that wait for their answer (APKRT_EVENT_OPEN_RESULT or APKRT_REQUEST_OPEN_CANCEL), as the picks':
// the check, the change of the table and the host's call under open_handler_lock, the table under its own lock.
std::recursive_mutex open_handler_lock;
std::mutex open_lock;
WaitingIds<open_live_limit> live_opens;

/// Forgets open request `id`; whether it was waiting.
bool open_done(std::uint64_t id) {
    const std::lock_guard<std::mutex> hold(open_lock);
    return live_opens.remove(id);
}

/// Copies the guest string at `address` into `out` through checked reads, so a bad guest pointer gives -EFAULT instead
/// of faulting the host; stops at the first NUL. Each read ends at a 4096-byte boundary, so it stays within one page
/// (pages are 4 or 16 KiB) and fails only where the string does: a string that ends before a page that cannot be read
/// is read whole, and each byte is read once. Returns the length before the NUL when one lies within `limit` bytes,
/// `limit` when none does (out then holds `limit` non-NUL bytes), or -EFAULT. The caller NUL-terminates and decides
/// whether no NUL within `limit` truncates (START_FAILED) or is refused (PICK, OPEN).
long copy_guest_text(char *out, std::uint64_t address, std::size_t limit) {
    constexpr std::uint64_t block = 4096;
    for (std::size_t done = 0; done < limit;) {
        const std::uint64_t at = address + done;
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(block - at % block, limit - done));
        if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(out + done), at, length)) {
            return -linux_errno::efault;
        }
        if (const void *nul = std::memchr(out + done, '\0', length)) {
            return static_cast<long>(static_cast<const char *>(nul) - out);
        }
        done += length;
    }
    return static_cast<long>(limit);
}

/// APKRT_REQUEST_PICK: the text at `address` copied once through a checked read, each byte read once, and handed to
/// the host's request handler with the ID. 0, or the negative errno: -EFAULT without text or when the text cannot be
/// read, -EINVAL for an ID of 0, a text of pick_text_limit bytes or more, or one that valid_pick refuses, -EEXIST for
/// an ID that waits for its answer, -EBUSY when pick_live_limit requests wait.
long pick(std::uint64_t address, std::uint64_t id) {
    if (address == 0) return -linux_errno::efault;
    if (id == 0) return -linux_errno::einval;
    char text[pick_text_limit];
    const long length = copy_guest_text(text, address, pick_text_limit);
    if (length < 0) return length;                                                  // -EFAULT: a byte unreadable
    if (length >= static_cast<long>(pick_text_limit)) return -linux_errno::einval;  // no NUL within the limit
    if (!valid_pick(std::string_view(text, static_cast<std::size_t>(length)))) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(pick_handler_lock);
    {
        const std::lock_guard<std::mutex> hold(pick_lock);
        if (live_picks.contains(id)) return -linux_errno::eexist;
        if (live_picks.full()) return -linux_errno::ebusy;
        live_picks.add(id);
    }
    report(APKRT_REQUEST_PICK, id, text);
    return 0;
}

// APKRT_REQUEST_NOTIFICATION (ADR-0074 decision 2): the app's post, cancel or cancel of all, whose fields the host
// puts on screen, so each is checked as a pick's title is.
constexpr std::size_t notification_text_limit = 8192;  // the NUL included
constexpr std::size_t notification_name_length = 64;   // bytes of a tag, a channel, a group
constexpr std::size_t notification_line_length = 256;  // bytes of the title and the sub text
constexpr std::size_t notification_body_length = 4096; // bytes of the text
constexpr std::size_t notification_intent_length = 2048;  // bytes of the content intent

/// Whether `field` is a decimal number from `low` to `high`: an optional '-', then digits without a leading zero.
bool decimal_in(std::string_view field, long long low, long long high) {
    const bool negative = !field.empty() && field[0] == '-';
    const std::string_view digits = negative ? field.substr(1) : field;
    if (digits.empty() || digits.size() > 19 || (digits.size() > 1 && digits[0] == '0')) return false;
    unsigned long long value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (negative) return value <= static_cast<unsigned long long>(-(low + 1)) + 1 && value != 0;
    return value <= static_cast<unsigned long long>(high);
}

/// Whether `field` is plain UTF-8 (plain_utf8) of at most `bytes` bytes, new lines allowed when `lines`.
bool notification_field(std::string_view field, std::size_t bytes, bool lines) {
    if (field.size() > bytes) return false;
    return utf8_code_points(field, [lines](std::uint32_t point) {
        return (lines && point == '\n') || plain_code_point(point);
    });
}

/// Whether `text` is one of APKRT_REQUEST_NOTIFICATION's forms (guest_interface.h).
bool valid_notification(std::string_view text) {
    std::vector<std::string_view> fields;
    for (std::size_t start = 0;;) {
        const std::size_t tab = text.find('\t', start);
        fields.push_back(text.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos || fields.size() > 12) break;
        start = tab + 1;
    }
    const auto id = [](std::string_view field) { return decimal_in(field, INT32_MIN, INT32_MAX); };
    if (fields[0] == "cancelAll") return fields.size() == 1;
    if (fields[0] == "cancel") {
        return fields.size() == 3 && id(fields[1]) && notification_field(fields[2], notification_name_length, false);
    }
    if (fields[0] != "post" || fields.size() != 12) return false;
    // The content intent, Intent.toUri of an activity of the app's own, or empty: printable ASCII
    // (ADR-0074 decision 4).
    const std::string_view intent = fields[10];
    if (intent.size() > notification_intent_length ||
        !std::all_of(intent.begin(), intent.end(), [](char c) { return c >= 0x21 && c <= 0x7e; })) {
        return false;
    }
    return id(fields[1]) && notification_field(fields[2], notification_name_length, false) &&
           notification_field(fields[3], notification_name_length, false) && fields[4].size() == 1 &&
           fields[4][0] >= '0' && fields[4][0] <= '5' && (fields[5] == "0" || fields[5] == "1") &&
           decimal_in(fields[6], 0, INT64_MAX) &&
           notification_field(fields[7], notification_name_length, false) &&
           notification_field(fields[8], notification_line_length, false) &&
           notification_field(fields[9], notification_line_length, false) &&
           notification_field(fields[11], notification_body_length, true);
}

/// APKRT_REQUEST_NOTIFICATION: with the app's notifications grant, the text at `address` copied once through a
/// checked read, checked whole and handed to the host's request handler. 0, or the negative errno: -EACCES without
/// the grant, -EFAULT without text or when it cannot be read, -EINVAL for a second argument other than 0, a text of
/// notification_text_limit bytes or more, or one that valid_notification refuses.
long notification(std::uint64_t address, std::uint64_t second) {
    if (apkrt::permissions::outcome(APKRT_PERMISSION_NOTIFICATIONS) != APKRT_PERMISSION_GRANTED) {
        return -linux_errno::eacces;
    }
    if (address == 0) return -linux_errno::efault;
    if (second != 0) return -linux_errno::einval;
    std::vector<char> text(notification_text_limit);
    const long length = copy_guest_text(text.data(), address, notification_text_limit);
    if (length < 0) return length;
    if (length >= static_cast<long>(notification_text_limit)) return -linux_errno::einval;
    if (!valid_notification(std::string_view(text.data(), static_cast<std::size_t>(length)))) {
        return -linux_errno::einval;
    }
    report(APKRT_REQUEST_NOTIFICATION, 0, text.data());
    return 0;
}

/// APKRT_REQUEST_EXPORT_MEDIA: the text at `address` copied once through a checked read and handed to the host's
/// request handler. 0, or the negative errno: -EFAULT without text or when it cannot be read, -EINVAL for a second
/// argument other than 0, a text of pick_text_limit bytes or more, or one that valid_export refuses.
long export_media(std::uint64_t address, std::uint64_t second) {
    if (address == 0) return -linux_errno::efault;
    if (second != 0) return -linux_errno::einval;
    char text[pick_text_limit];
    const long length = copy_guest_text(text, address, pick_text_limit);
    if (length < 0) return length;                                                  // -EFAULT: a byte unreadable
    if (length >= static_cast<long>(pick_text_limit)) return -linux_errno::einval;  // no NUL within the limit
    if (!valid_export(std::string_view(text, static_cast<std::size_t>(length)))) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(pick_handler_lock);
    report(APKRT_REQUEST_EXPORT_MEDIA, 0, text);
    return 0;
}

/// APKRT_REQUEST_PICK_CANCEL: the guest no longer waits for request `id`; the host gets it only when it still waits.
/// 0, or -EINVAL for an ID of 0, -ENOENT for one that does not wait.
long pick_cancel(std::uint64_t id) {
    if (id == 0) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(pick_handler_lock);
    if (!pick_done(id)) return -linux_errno::enoent;
    report(APKRT_REQUEST_PICK_CANCEL, id, nullptr);
    return 0;
}

/// APKRT_REQUEST_OPEN: the text at `address` copied once through a checked read, each byte read once, and handed to
/// the host's request handler with the ID. 0, or the negative errno: -EFAULT without text or when the text cannot be
/// read, -EINVAL for an ID of 0, a text of open_text_limit bytes or more, or one that valid_open refuses, -EEXIST for
/// an ID that waits for its answer, -EBUSY when open_live_limit requests wait, -ENOMEM without memory for the copy and
/// its decoded fields.
long open_request(std::uint64_t address, std::uint64_t id) {
    if (address == 0) return -linux_errno::efault;
    if (id == 0) return -linux_errno::einval;
    const std::unique_ptr<char[]> text(new (std::nothrow) char[open_text_limit]);
    const std::unique_ptr<char[]> decoded(text ? new (std::nothrow) char[open_text_limit] : nullptr);
    if (!text || !decoded) return -linux_errno::enomem;
    const long length = copy_guest_text(text.get(), address, open_text_limit);
    if (length < 0) return length;                                                  // -EFAULT: a byte unreadable
    if (length >= static_cast<long>(open_text_limit)) return -linux_errno::einval;  // no NUL within the limit
    if (!valid_open(std::string_view(text.get(), static_cast<std::size_t>(length)), decoded.get())) {
        return -linux_errno::einval;
    }
    const std::lock_guard<std::recursive_mutex> handler(open_handler_lock);
    {
        const std::lock_guard<std::mutex> hold(open_lock);
        if (live_opens.contains(id)) return -linux_errno::eexist;
        if (live_opens.full()) return -linux_errno::ebusy;
        live_opens.add(id);
    }
    report(APKRT_REQUEST_OPEN, id, text.get());
    return 0;
}

/// APKRT_REQUEST_OPEN_CANCEL: the guest no longer waits for request `id`; the host gets it only when it still waits.
/// 0, or -EINVAL for an ID of 0, -ENOENT for one that does not wait.
long open_request_cancel(std::uint64_t id) {
    if (id == 0) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(open_handler_lock);
    if (!open_done(id)) return -linux_errno::enoent;
    report(APKRT_REQUEST_OPEN_CANCEL, id, nullptr);
    return 0;
}

// APKRT_REQUEST_PERMISSION / _CANCEL (ADR-0063 location, ADR-0064 microphone): a tiny request in the PICK/OPEN
// family. The text is "<group>\t<detail>", both single decimals, so the runtime reads it like a pick and checks it
// whole before the host sees it; apkrt_permission_answer answers it and records the grant.
constexpr std::size_t permission_text_limit = 8;  // "<g>\t<d>" and the NUL, with room to spare
constexpr std::size_t permission_live_limit = 4;  // requests that wait for their answer at once
std::recursive_mutex permission_handler_lock;
std::mutex permission_lock;
WaitingIds<permission_live_limit> live_permissions;

/// Forgets permission request `id`; whether it was waiting.
bool permission_done(std::uint64_t id) {
    const std::lock_guard<std::mutex> hold(permission_lock);
    return live_permissions.remove(id);
}

/// Whether `text` is "<group>\t<detail>" with a known group and a detail the group allows: location (1) takes fine
/// (1) or coarse (2), microphone (2), camera (3) and notifications (4) take none (0).
bool valid_permission(std::string_view text) {
    if (text.size() != 3 || text[1] != '\t') return false;  // one group digit, a tab, one detail digit
    switch (text[0]) {
        case '1': return text[2] == '1' || text[2] == '2';  // location: fine or coarse
        case '2': return text[2] == '0';                    // microphone: none
        case '3': return text[2] == '0';                    // camera: none (ADR-0073)
        case '4': return text[2] == '0';                    // notifications: none (ADR-0074)
        default: return false;
    }
}

/// APKRT_REQUEST_PERMISSION: the text at `address` read through a checked copy, checked whole, and handed to the
/// host's request handler with the ID. 0, or -EFAULT without text or when it cannot be read, -EINVAL for an ID of 0,
/// a text of permission_text_limit bytes or more, or one that valid_permission refuses, -EEXIST for an ID that waits,
/// -EBUSY when permission_live_limit requests wait.
long permission(std::uint64_t address, std::uint64_t id) {
    if (address == 0) return -linux_errno::efault;
    if (id == 0) return -linux_errno::einval;
    char text[permission_text_limit];
    const long length = copy_guest_text(text, address, permission_text_limit);
    if (length < 0) return length;                                                        // -EFAULT: a byte unreadable
    if (length >= static_cast<long>(permission_text_limit)) return -linux_errno::einval;  // no NUL within the limit
    if (!valid_permission(std::string_view(text, static_cast<std::size_t>(length)))) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(permission_handler_lock);
    {
        const std::lock_guard<std::mutex> hold(permission_lock);
        if (live_permissions.contains(id)) return -linux_errno::eexist;
        if (live_permissions.full()) return -linux_errno::ebusy;
        live_permissions.add(id);
    }
    report(APKRT_REQUEST_PERMISSION, id, text);
    return 0;
}

/// APKRT_REQUEST_PERMISSION_CANCEL: the guest no longer waits for request `id`; the host gets it only when it still
/// waits. 0, or -EINVAL for an ID of 0, -ENOENT for one that does not wait.
long permission_cancel(std::uint64_t id) {
    if (id == 0) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(permission_handler_lock);
    if (!permission_done(id)) return -linux_errno::enoent;
    report(APKRT_REQUEST_PERMISSION_CANCEL, id, nullptr);
    return 0;
}

// An app installer's requests (APKRT_REQUEST_ASK_INSTALL_SOURCES, APKRT_REQUEST_CONFIRM_INSTALL): the host splits the
// text at its one newline, so the runtime checks the shape whole first, as a pick's. The longest text is a Linux path
// of PATH_MAX (4096 with its NUL) and a package name of 255 characters, with the newline and the NUL.
constexpr std::size_t install_text_limit = 4095 + 1 + 255 + 1;
constexpr std::size_t install_live_limit = 4;  // requests of one kind that wait for their answer at once

// The question's and the install's requests that wait for their answer, each kind apart, as the framework numbers
// them apart; the handler lock and the tables' own lock as a pick's.
std::recursive_mutex install_handler_lock;
std::mutex install_lock;
WaitingIds<install_live_limit> live_install_sources;
WaitingIds<install_live_limit> live_installs;
WaitingIds<install_live_limit> live_uninstalls;  // APKRT_REQUEST_UNINSTALL's, a text of the same shape

/// Forgets the question (`sources`) or install request `id`; whether it was waiting.
bool install_done(bool sources, std::uint64_t id) {
    const std::lock_guard<std::mutex> hold(install_lock);
    return (sources ? live_install_sources : live_installs).remove(id);
}

/// Whether `text` is "<first>\n<second>", both parts non-empty and no other newline.
bool valid_install(std::string_view text) {
    const std::size_t newline = text.find('\n');
    return newline != std::string_view::npos && newline != 0 && newline + 1 < text.size()
           && text.find('\n', newline + 1) == std::string_view::npos;
}

/// APKRT_REQUEST_ASK_INSTALL_SOURCES or APKRT_REQUEST_CONFIRM_INSTALL (`type`): the text at `address` read through a
/// checked copy, checked whole, and handed to the host's request handler with the ID; the host answers once with
/// APKRT_EVENT_INSTALL_SOURCES_RESULT or APKRT_EVENT_INSTALL_RESULT. 0, or -EFAULT without text or when it cannot be
/// read, -EINVAL for an ID of 0, a text of install_text_limit bytes or more, or one that valid_install refuses, -EEXIST
/// for an ID of its kind that waits, -EBUSY when install_live_limit requests of its kind wait.
long install_request(int type, std::uint64_t address, std::uint64_t id) {
    if (address == 0) return -linux_errno::efault;
    if (id == 0) return -linux_errno::einval;
    std::vector<char> text(install_text_limit);
    const long length = copy_guest_text(text.data(), address, install_text_limit);
    if (length < 0) return length;
    if (length >= static_cast<long>(install_text_limit)) return -linux_errno::einval;
    if (!valid_install(std::string_view(text.data(), static_cast<std::size_t>(length)))) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(install_handler_lock);
    {
        const std::lock_guard<std::mutex> hold(install_lock);
        auto &live = type == APKRT_REQUEST_ASK_INSTALL_SOURCES ? live_install_sources
                     : type == APKRT_REQUEST_UNINSTALL         ? live_uninstalls
                                                               : live_installs;
        if (live.contains(id)) return -linux_errno::eexist;
        if (live.full()) return -linux_errno::ebusy;
        live.add(id);
    }
    report(type, id, text.data());
    return 0;
}

// APKRT_REQUEST_IME (ADR-0068, part 1): the guest tells the host which view takes text and whether its keyboard is
// wanted. The host parses the text, so the runtime checks it whole first, as a pick's; no answer ends the request, the
// host's text records are the answers, and they carry the serial.
constexpr std::size_t ime_text_limit = 512;  // "start\t..." with a hint cut to fit, and the NUL
/// "extract\t..." holds up to 4096 characters of text, each up to four bytes, after a hint and six numbers.
constexpr std::size_t ime_extract_limit = 17 * 1024;
std::recursive_mutex ime_handler_lock;

/// Whether `text` holds only an optional '-' and decimal digits, between 1 and 11 of them.
bool ime_number(std::string_view text) {
    if (!text.empty() && text.front() == '-') text.remove_prefix(1);
    if (text.empty() || text.size() > 10) return false;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return false;
    }
    return true;
}

/// Whether `text` is "start" and eight numbers and the hint, each after a tab: inputType, imeOptions, capsMode,
/// selStart, selEnd, fieldId, actionId, actionLabel. The hint is the rest, which may hold tabs.
bool valid_ime_start(std::string_view text) {
    if (text.substr(0, 6) != "start\t") return false;
    text.remove_prefix(6);
    for (int field = 0; field < 8; ++field) {
        const std::size_t tab = text.find('\t');
        if (tab == std::string_view::npos || !ime_number(text.substr(0, tab))) return false;
        text.remove_prefix(tab + 1);
    }
    return true;
}

/// Whether `text` begins with `count` numbers, each followed by a tab, the first of which is `serial`; `text` is left
/// holding what follows the last tab.
bool ime_numbers(std::string_view &text, int count, std::uint64_t serial) {
    for (int field = 0; field < count; ++field) {
        const std::size_t tab = text.find('\t');
        if (tab == std::string_view::npos || !ime_number(text.substr(0, tab))) return false;
        if (field == 0 && std::string(text.substr(0, tab)) != std::to_string(serial)) return false;
        text.remove_prefix(tab + 1);
    }
    return true;
}

/// Whether `text` is "field", the connection's serial and four numbers, the field's rectangle in the window's pixels,
/// each after a tab, and nothing more.
bool valid_ime_field(std::string_view text, std::uint64_t serial) {
    if (text.substr(0, 6) != "field\t") return false;
    text.remove_prefix(6);
    if (!ime_numbers(text, 4, serial)) return false;
    return ime_number(text);  // the fifth number, the bottom, ends the text
}

/// Whether `text` is "extract", the connection's serial and six numbers (startOffset, selStart, selEnd, composingStart,
/// composingEnd, flags), a hint with no tab, and a tab and the text, which may hold tabs and new lines.
bool valid_ime_extract(std::string_view text, std::uint64_t serial) {
    if (text.substr(0, 8) != "extract\t") return false;
    text.remove_prefix(8);
    if (!ime_numbers(text, 7, serial)) return false;
    return text.find('\t') != std::string_view::npos;  // after the hint
}

/// APKRT_REQUEST_IME: the text at `address` read through a checked copy, checked whole, and handed to the host's
/// request handler with the serial. 0, or -EFAULT without text or when it cannot be read, -EINVAL for a text of
/// ime_text_limit bytes or more (ime_extract_limit for an extract), one that is not start, show, hide, stop, field or
/// extract in the forms above, or a serial of 0 for all but hide.
long ime(std::uint64_t address, std::uint64_t serial) {
    if (address == 0) return -linux_errno::efault;
    std::string text(ime_extract_limit, '\0');
    const long length = copy_guest_text(text.data(), address, ime_extract_limit);
    if (length < 0) return length;                                                    // -EFAULT: a byte unreadable
    if (length >= static_cast<long>(ime_extract_limit)) return -linux_errno::einval;  // no NUL within the limit
    const std::string_view view(text.data(), static_cast<std::size_t>(length));
    const bool extract = view.substr(0, 8) == "extract\t";
    if (!extract && length >= static_cast<long>(ime_text_limit)) return -linux_errno::einval;
    const bool hide = view == "hide";
    if (!hide && view != "show" && view != "stop" && !valid_ime_start(view) && !valid_ime_field(view, serial) &&
        !valid_ime_extract(view, serial)) {
        return -linux_errno::einval;
    }
    if (!hide && serial == 0) return -linux_errno::einval;
    const std::lock_guard<std::recursive_mutex> handler(ime_handler_lock);
    report(APKRT_REQUEST_IME, serial, text.c_str());
    return 0;
}

/// APKRT_REQUEST_WINDOW: the request at `address` checked, its buffer's size and opacity set, a place's type, parent,
/// group and format the make's, and handed to the host's window handler. 0, or the negative errno (guest_interface.h).
long window(std::uint64_t address) {
    if (described.window == nullptr) return -linux_errno::eopnotsupp;
    apkrt_window_request request;
    if (address == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&request), address, sizeof request)) {
        return -linux_errno::efault;
    }
    if (request.op != APKRT_WINDOW_MAKE && request.op != APKRT_WINDOW_PLACE && request.op != APKRT_WINDOW_REMOVE) {
        return -linux_errno::einval;
    }
    if (request.token == 0) return -linux_errno::einval;
    apkrt_window_request host{};
    host.op = request.op;
    host.token = request.token;
    if (request.op != APKRT_WINDOW_REMOVE) {
        // The surfaces' rules for the frame and the size.
        const std::int64_t frame_width = std::int64_t{request.right} - request.left;
        const std::int64_t frame_height = std::int64_t{request.bottom} - request.top;
        if (frame_width <= 0 || frame_height <= 0 || frame_width > frame_side_limit ||
            frame_height > frame_side_limit) {
            return -linux_errno::einval;
        }
        if ((request.width == 0) != (request.height == 0)) return -linux_errno::einval;
        const std::int64_t width = request.width == 0 ? frame_width : request.width;
        const std::int64_t height = request.height == 0 ? frame_height : request.height;
        if (width < 1 || height < 1 || width > buffer_side_limit || height > buffer_side_limit) {
            return -linux_errno::einval;
        }
        if (!unit_interval(request.alpha) || !unit_interval(request.dim) || request.reserved != 0) {
            return -linux_errno::einval;
        }
        host = request;
        host.width = static_cast<std::int32_t>(width);
        host.height = static_cast<std::int32_t>(height);
    }
    const bool sub = sub_window(request.type);
    if (request.op == APKRT_WINDOW_MAKE) {
        // An application window, a sub-window or a toast; a parent for a sub-window only, a group for an application
        // window only.
        if (!(request.type >= 1 && request.type <= 99) && !sub && request.type != type_toast) {
            return -linux_errno::einval;
        }
        if (!sub && request.parent != 0) return -linux_errno::einval;
        if ((sub || request.type == type_toast) && request.group != 0) return -linux_errno::einval;
    }
    const std::lock_guard<std::mutex> hold(windows_lock);
    const auto live = [](std::uint64_t token) {
        return std::find_if(live_windows.begin(), live_windows.end(),
                            [token](const LiveWindow &each) { return each.token == token; });
    };
    const auto found = live(request.token);
    switch (request.op) {
        case APKRT_WINDOW_MAKE:
            if (found != live_windows.end()) return -linux_errno::eexist;
            if (live_windows.size() == window_limit) return -linux_errno::enospc;
            // One level of sub-windows, as on Android 15: the parent is the session's window or a live window that is
            // no sub-window itself.
            if (sub && request.parent != 0) {
                const auto parent = live(request.parent);
                if (parent == live_windows.end()) return -linux_errno::enoent;
                if (sub_window(parent->type)) return -linux_errno::einval;
            }
            live_windows.push_back({request.token, request.parent, request.type, request.format, request.group});
            break;
        case APKRT_WINDOW_PLACE:
            if (found == live_windows.end()) return -linux_errno::enoent;
            host.type = found->type;
            host.parent = found->parent;
            host.group = found->group;
            host.format = found->format;
            break;
        default:  // remove
            if (found == live_windows.end()) return -linux_errno::enoent;
            if (std::any_of(live_windows.begin(), live_windows.end(),
                            [&](const LiveWindow &each) { return each.parent == request.token; })) {
                return -linux_errno::ebusy;  // its sub-windows go first
            }
            live_windows.erase(found);
            break;
    }
    if (request.op != APKRT_WINDOW_REMOVE) host.opaque = has_alpha(host.format) ? 0 : 1;
    as_host([&] { described.window(described.context, &host); });
    return 0;
}

// The WebView views' live tokens (ADR-0056 step a). A request's check and payload copy run first; then, under one
// lock as for surfaces and windows, the token set changes, the channel is made or closed, and the host's handler is
// called, so the host sees the requests in the order the runtime took them.
static_assert(sizeof(apkrt_webview_request) == 96);
constexpr std::size_t webview_limit = 16;  // live views of the session, as windows and surfaces have

std::mutex webviews_lock;
std::vector<std::uint64_t> live_webviews;  // the tokens with an open channel, under webviews_lock

/// APKRT_REQUEST_WEBVIEW: the request at `address` checked and handed to the host's WebView handler as an
/// apkrt_webview_call with host-safe fields (guest_interface.h). MAKE opens the record channel and returns its fd;
/// PLACE, CALL, ANSWER and REMOVE return 0 or a negative errno; REMOVE closes the channel. The host side is step c).
long webview(std::uint64_t address) {
    if (described.webview == nullptr) return -linux_errno::eopnotsupp;
    apkrt_webview_request request;
    if (address == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&request), address, sizeof request)) {
        return -linux_errno::efault;
    }
    if (request.op < APKRT_WEBVIEW_MAKE || request.op > APKRT_WEBVIEW_ANSWER) return -linux_errno::einval;
    if (request.token == 0) return -linux_errno::einval;

    apkrt_webview_call call{};
    call.op = request.op;
    call.token = request.token;
    call.fd = -1;

    // The fields each op carries, and the inline payload, checked and copied before the lock (a payload up to 1 MiB).
    std::vector<std::uint8_t> payload;  // lives until the handler returns
    switch (request.op) {
        case APKRT_WEBVIEW_MAKE:
        case APKRT_WEBVIEW_REMOVE:
            if (request.flags != 0) return -linux_errno::einval;
            break;
        case APKRT_WEBVIEW_PLACE: {
            if (request.flags != 0) return -linux_errno::einval;
            if (request.window == 0) return -linux_errno::einval;
            const std::int64_t frame_width = std::int64_t{request.right} - request.left;
            const std::int64_t frame_height = std::int64_t{request.bottom} - request.top;
            if (frame_width <= 0 || frame_height <= 0 || frame_width > frame_side_limit ||
                frame_height > frame_side_limit) {
                return -linux_errno::einval;
            }
            if (request.clip_right < request.clip_left || request.clip_bottom < request.clip_top) {
                return -linux_errno::einval;  // a negative extent; an empty clip (equal edges) is allowed
            }
            call.window = request.window;
            call.left = request.left;
            call.top = request.top;
            call.right = request.right;
            call.bottom = request.bottom;
            call.clip_left = request.clip_left;
            call.clip_top = request.clip_top;
            call.clip_right = request.clip_right;
            call.clip_bottom = request.clip_bottom;
            break;
        }
        case APKRT_WEBVIEW_CALL:
            if (request.flags != 0) return -linux_errno::einval;  // the fd mode is for ANSWER only
            call.request_id = request.request_id;
            break;
        case APKRT_WEBVIEW_ANSWER:
            if ((request.flags & ~static_cast<std::uint32_t>(APKRT_WEBVIEW_ANSWER_FD)) != 0) {
                return -linux_errno::einval;
            }
            call.request_id = request.request_id;
            if ((request.flags & APKRT_WEBVIEW_ANSWER_FD) != 0) {
                if (request.fd < 0 || request.length < -1) return -linux_errno::einval;
                call.flags = APKRT_WEBVIEW_ANSWER_FD;
                call.offset = request.offset;
                call.length = request.length;  // the guest fd is dup'd under the lock, once the token is known live
            }
            break;
    }
    const bool inline_body = (request.op == APKRT_WEBVIEW_CALL) ||
                             (request.op == APKRT_WEBVIEW_ANSWER && (request.flags & APKRT_WEBVIEW_ANSWER_FD) == 0);
    if (inline_body && request.payload_length != 0) {
        if (request.payload_length > APKRT_WEBVIEW_RECORD_MAX) return -linux_errno::einval;
        if (request.payload == 0) return -linux_errno::efault;
        try {
            payload.resize(request.payload_length);
        } catch (const std::bad_alloc &) {
            return -linux_errno::enomem;
        }
        if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(payload.data()), request.payload,
                                       request.payload_length)) {
            return -linux_errno::efault;
        }
        call.payload = payload.data();
        call.payload_length = request.payload_length;
    }

    const std::lock_guard<std::mutex> hold(webviews_lock);
    const auto found = std::find(live_webviews.begin(), live_webviews.end(), request.token);
    switch (request.op) {
        case APKRT_WEBVIEW_MAKE: {
            if (found != live_webviews.end()) return -linux_errno::eexist;
            if (live_webviews.size() == webview_limit) return -linux_errno::enospc;
            const long fd = channels::webview_make(request.token);
            if (fd < 0) return fd;
            try {
                live_webviews.push_back(request.token);
            } catch (const std::bad_alloc &) {
                channels::webview_close(request.token);  // undo the channel; the guest never learned of it
                return -linux_errno::enomem;
            }
            as_host([&] { described.webview(described.context, &call); });
            return fd;
        }
        case APKRT_WEBVIEW_REMOVE: {
            if (found == live_webviews.end()) return -linux_errno::enoent;
            live_webviews.erase(found);
            as_host([&] { described.webview(described.context, &call); });  // the host tears its WKWebView down
            channels::webview_close(request.token);  // then the channel closes and the guest's reader meets EOF
            return 0;
        }
        default: {  // PLACE, CALL, ANSWER
            if (found == live_webviews.end()) return -linux_errno::enoent;
            if (request.op == APKRT_WEBVIEW_ANSWER && (request.flags & APKRT_WEBVIEW_ANSWER_FD) != 0) {
                const auto description = descriptors::get(request.fd);
                if (!description) return -linux_errno::ebadf;
                const int duplicate = ::dup(description->host());  // the host owns it and closes it after streaming
                if (duplicate < 0) return -sys::to_linux_errno(errno);
                ::fcntl(duplicate, F_SETFD, FD_CLOEXEC);
                call.fd = duplicate;
            }
            as_host([&] { described.webview(described.context, &call); });
            return 0;
        }
    }
}

/// A time limit: none below 0 or for NaN, and at most 10^9 s (about 31 years), which fits every clock.
std::chrono::steady_clock::duration limit(double seconds) {
    if (!(seconds > 0)) return {};
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(std::min(seconds, 1e9)));
}

}  // namespace

EndHandler end_handler() {
    if (!begun.load(std::memory_order_acquire)) return {nullptr, nullptr};
    return {described.end, described.context};
}

void end() {
    const std::lock_guard<std::mutex> hold(lock);
    ended = true;
    changed.notify_all();
}

int refresh_rate() {
    return begun.load(std::memory_order_acquire) ? described.configuration.refresh_rate : 0;
}

long connect(std::uint64_t configuration) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    if (configuration == 0) return -linux_errno::efault;
    // Write the configuration to the guest before the lock and the state change, through a checked copy:
    // a bad pointer then returns -EFAULT with the session still unconnected and read_end intact, where a
    // raw store would fault the host (ADR-0020). A concurrent connect that loses the race writes the
    // (read-only) configuration too before its -EBUSY, which is harmless.
    const apkrt_configuration view = scaled_configuration(described.configuration,
                                                          render_scale.load(std::memory_order_relaxed));
    if (!sys::memory::copy_checked(configuration, reinterpret_cast<std::uint64_t>(&view), sizeof view))
        return -linux_errno::efault;
    int host;
    {
        const std::lock_guard<std::mutex> hold(lock);
        if (connected) return -linux_errno::ebusy;
        connected = true;
        host = std::exchange(read_end, -1);
    }
    return descriptors::add(host, true);
}

long configuration(std::uint64_t address) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    if (address == 0) return -linux_errno::efault;
    // Set at the begin, before `begun`, and never again: no lock. Written through a checked copy so a bad
    // guest pointer returns -EFAULT instead of faulting the host.
    const apkrt_configuration view = scaled_configuration(described.configuration,
                                                          render_scale.load(std::memory_order_relaxed));
    if (!sys::memory::copy_checked(address, reinterpret_cast<std::uint64_t>(&view), sizeof view))
        return -linux_errno::efault;
    return 0;
}

long answer(std::uint64_t serial) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    const std::lock_guard<std::mutex> hold(lock);
    if (serial == 0 || serial > posted) return -linux_errno::einval;
    if (serial > answered) {
        answered = static_cast<std::uint32_t>(serial);
        answer_times[answer_count++ % answers_kept] = {answered, clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
        changed.notify_all();
    }
    return 0;
}

long request(std::uint64_t type, std::uint64_t a0, std::uint64_t a1) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    switch (type) {
        case APKRT_REQUEST_FINISH:
            report(APKRT_REQUEST_FINISH, 0, nullptr);
            return 0;
        case APKRT_REQUEST_WINDOW_FLAGS: {
            // As Window.setFlags(add, add | remove): what is both added and removed stays added.
            const std::lock_guard<std::mutex> hold(flags_lock);
            const bool before = (window_flags & keep_screen_on) != 0;
            window_flags = (window_flags & ~a1) | a0;
            const bool on = (window_flags & keep_screen_on) != 0;
            if (on != before) report(APKRT_REQUEST_KEEP_SCREEN_ON, on ? 1 : 0, nullptr);
            return 0;
        }
        case APKRT_REQUEST_WINDOW_FORMAT: {
            // libandroid passes an int32_t: its low 32 bits.
            const auto format = static_cast<std::int32_t>(static_cast<std::uint32_t>(a0));
            const std::lock_guard<std::mutex> hold(flags_lock);
            const bool before = !has_alpha(window_format);
            window_format = format;
            const bool opaque = !has_alpha(format);
            if (opaque != before) report(APKRT_REQUEST_WINDOW_OPAQUE, opaque ? 1 : 0, nullptr);
            return 0;
        }
        case APKRT_REQUEST_START_FAILED: {
            if (a0 == 0) return -linux_errno::efault;
            char text[text_limit];
            const long length = copy_guest_text(text, a0, text_limit - 1);  // scans text_limit-1, as the old strnlen
            if (length < 0) return length;  // -EFAULT: a byte could not be read
            text[length] = '\0';            // a NUL was within reach, or the text is truncated to text_limit-1
            report(APKRT_REQUEST_START_FAILED, 0, text);
            return 0;
        }
        case APKRT_REQUEST_FRAME_RATE:
            windows::set_frame_rate(a0, static_cast<std::uint32_t>(a1),
                                    [](std::uint32_t bits) { report(APKRT_REQUEST_FRAME_RATE, bits, nullptr); });
            return 0;
        case APKRT_REQUEST_SURFACE:
            return surface(a0);
        case APKRT_REQUEST_WINDOW:
            return window(a0);
        case APKRT_REQUEST_PICK:
            return pick(a0, a1);
        case APKRT_REQUEST_PICK_CANCEL:
            return pick_cancel(a0);
        case APKRT_REQUEST_OPEN:
            return open_request(a0, a1);
        case APKRT_REQUEST_OPEN_CANCEL:
            return open_request_cancel(a0);
        case APKRT_REQUEST_EXPORT_MEDIA:
            return export_media(a0, a1);
        case APKRT_REQUEST_NOTIFICATION:
            return notification(a0, a1);
        case APKRT_REQUEST_PERMISSION:
            return permission(a0, a1);
        case APKRT_REQUEST_PERMISSION_CANCEL:
            return permission_cancel(a0);
        case APKRT_REQUEST_ASK_INSTALL_SOURCES:
        case APKRT_REQUEST_CONFIRM_INSTALL:
        case APKRT_REQUEST_UNINSTALL:
            return install_request(static_cast<int>(type), a0, a1);
        case APKRT_REQUEST_IME:
            return ime(a0, a1);
        case APKRT_REQUEST_WEBVIEW:
            return webview(a0);
        case APKRT_REQUEST_BACK_AT_ROOT:
            // Guest only, a0/a1 reserved (ADR-0041 decision 10): fire-and-forget, like FINISH. The runtime
            // forwards a plain notification (argument 0, no text); the host opens its overlay menu, no answer.
            report(APKRT_REQUEST_BACK_AT_ROOT, 0, nullptr);
            return 0;
        case APKRT_REQUEST_ORIENTATION:
            // Guest only: fire-and-forget; a0, the SCREEN_ORIENTATION value, is the notification's argument.
            report(APKRT_REQUEST_ORIENTATION, a0, nullptr);
            return 0;
        case APKRT_REQUEST_STARTING_WINDOW: {
            // Guest only (ADR-0076): a0 splash 0xFFRRGGBB, a1 icon bg (0 or 0xFFRRGGBB). A colour
            // that is not opaque (alpha != 0xFF) or does not fit in 32 bits is refused, so the host only ever gets
            // valid colours. A valid request is forwarded once, packed as (a1 << 32) | a0 (both 32-bit); no answer, a
            // second one is -EALREADY. Validate before claiming the once-slot, so a bad request can be retried.
            const auto opaque = [](std::uint64_t colour) { return (colour >> 32) == 0 && (colour >> 24) == 0xFF; };
            if (!opaque(a0) || (a1 != 0 && !opaque(a1))) return -linux_errno::einval;
            static std::atomic_flag asked = ATOMIC_FLAG_INIT;
            if (asked.test_and_set(std::memory_order_relaxed)) return -linux_errno::ealready;
            report(APKRT_REQUEST_STARTING_WINDOW, (a1 << 32) | (a0 & 0xFFFFFFFF), nullptr);
            return 0;
        }
        default:
            return -linux_errno::einval;
    }
}

int *activity_paused_address() { return reinterpret_cast<int *>(&activity_paused); }

}  // namespace apkrt::session

extern "C" int apkrt_session_set_render_scale(double scale) {
    // The guest sees the session configuration scaled by this (ADR-0067 point 3), as apkrt_window_set_render_scale
    // scales a window. A value outside [0.25, 1] -- NaN and the infinities too, since every comparison with them is
    // false -- is refused and leaves the scale unchanged. Applied on read, so it works before or after the begin.
    using namespace apkrt::session;
    if (!(scale >= 0.25 && scale <= 1.0)) return -1;
    render_scale.store(scale, std::memory_order_relaxed);
    return 0;
}

extern "C" int apkrt_session_begin(const struct apkrt_session *session) {
    using namespace apkrt::session;
    if (session == nullptr || session->end == nullptr) return -1;
    // The event pipe: the host writes without blocking and without SIGPIPE; both ends close on exec.
    int ends[2];
    if (::pipe(ends) != 0) return -1;
    for (const int end : ends) ::fcntl(end, F_SETFD, FD_CLOEXEC);
    ::fcntl(ends[1], F_SETFL, O_NONBLOCK);
    ::fcntl(ends[1], F_SETNOSIGPIPE, 1);
    if (claimed.test_and_set()) {
        ::close(ends[0]);
        ::close(ends[1]);
        return -1;
    }
    described = *session;
    // The connect and configuration requests write the whole apkrt_configuration to the guest; clear the tail
    // padding after its last field so no uninitialized host bytes reach the guest (the host need not zero them).
    constexpr std::size_t fields = offsetof(apkrt_configuration, country) + sizeof described.configuration.country;
    std::memset(reinterpret_cast<char *>(&described.configuration) + fields, 0, sizeof described.configuration - fields);
    read_end = ends[0];
    write_end = ends[1];
    // Game mode (MINATO_LOADER_LOG) logs the memory picture at 1 Hz and at each OS memory-pressure warning from here
    // on, even though the app does not call apkrt_report_memory_to; the lines survive a jetsam kill
    // (memory_watermark.h).
    if (::getenv("MINATO_LOADER_LOG") != nullptr) apkrt::memory::start_instrument();
    // The network probe: one counts-only line of what the sandbox lets the runtime read for LinkProperties.
    // Under MINATO_NET_PROBE, and under MINATO_LOADER_LOG too, so any instrumented run's kept guest log carries it.
    if (::getenv("MINATO_NET_PROBE") != nullptr || ::getenv("MINATO_LOADER_LOG") != nullptr) apkrt_network_probe();
    // The Mac session's fake path monitor: a fixed host network where there is no NWPathMonitor, so
    // net-link-test runs end to end on the Mac. Never set on the device. Set before the guest reads getifaddrs.
    if (::getenv("MINATO_FAKE_NETWORK") != nullptr) apkrt::network::start_fake_path_monitor();
    begun.store(true, std::memory_order_release);
    return 0;
}

extern "C" void apkrt_permission_answer(uint32_t request_id, int32_t group, int32_t outcome) {
    apkrt::permissions::set(group, outcome);
    if (group == APKRT_PERMISSION_MICROPHONE && outcome != APKRT_PERMISSION_GRANTED) apkrt::audio::revoke_input();
    apkrt_session_post(APKRT_EVENT_PERMISSION_RESULT, request_id, static_cast<uint64_t>(static_cast<uint32_t>(group)),
                       static_cast<uint64_t>(static_cast<uint32_t>(outcome)));
}

// The process's memory at a trim (APKRT_EVENT_TRIM_MEMORY), written to the session log so a memory warning leaves a
// trace beside the stall watchdog's lines (ADR-0035 item 3, the Ren'Py pressure). phys_footprint is what iOS counts
// against the app's limit (task_info TASK_VM_INFO); os_proc_available_memory is how much the app may still use before
// iOS kills it, iOS-only, so the macOS host build logs 0 for it. Tag "apkrt" like the stall watchdog, so it goes to
// the device's session log yet java_app_run's app-tag comparison passes over it. Only when MINATO_LOADER_LOG is set,
// as the loader's scudo line is (dl.cpp): the MiB figures are machine-dependent and the line would otherwise land on
// the guest's stderr, which the fixture harness (tools/run-fixture.cmake) compares byte for byte, and a window-trim
// test DOES post a trim. App's RuntimeSessionHost sets the env on the device, so the trace reaches the session log
// there beside the loader's and the watchdog's lines.
static void log_memory_footprint(unsigned level) {
    if (::getenv("MINATO_LOADER_LOG") == nullptr) return;
    constexpr std::uint64_t mib = std::uint64_t{1} << 20;
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const std::uint64_t footprint =
            ::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
                    ? static_cast<std::uint64_t>(info.phys_footprint) / mib
                    : 0;
#if TARGET_OS_IPHONE
    const std::uint64_t available = static_cast<std::uint64_t>(::os_proc_available_memory()) / mib;
#else
    const std::uint64_t available = 0;
#endif
    const std::string line =
            std::format("I/apkrt   : trim {}: footprint {} MiB, available {} MiB\n", level, footprint, available);
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
}

extern "C" uint32_t apkrt_session_post(uint32_t type, uint64_t a0, uint64_t a1, uint64_t a2) {
    using namespace apkrt::session;
    if (!begun.load(std::memory_order_acquire)) return 0;
    // Minato's stall watchdog: resume arms its no-first-frame timer, pause or stop disarms it (session log diagnostic).
    if (type == APKRT_EVENT_RESUME) {
        apkrt::stall::resumed(clock_gettime_nsec_np(CLOCK_MONOTONIC));
        activity_paused.store(0, std::memory_order_relaxed);
    } else if (type == APKRT_EVENT_PAUSE || type == APKRT_EVENT_STOP) {
        apkrt::stall::paused();
        activity_paused.store(1, std::memory_order_relaxed);
    }
    if (type == APKRT_EVENT_TRIM_MEMORY) {
        log_memory_footprint(static_cast<unsigned>(a0));
        // TRIM_MEMORY_UI_HIDDEN and above: the windows' buffers go first, as Android's hidden window gives its
        // surface's back before the app hears of the trim.
        if (a0 >= 20) apkrt::windows::trim_buffers();
    }
    // A memory warning (onLowMemory): the 1 Hz instrument's richer line now, so the session log shows the memory
    // picture at the warning even when no trim follows (seen on the iPhone: warnings but no trim lines).
    if (type == APKRT_EVENT_LOW_MEMORY) apkrt::memory::log_instrument(true);
    // The answer to a pick request frees its ID, also when the post fails: the request is over.
    if (type == APKRT_EVENT_PICK_RESULT) pick_done(a0);
    if (type == APKRT_EVENT_OPEN_RESULT) open_done(a0);
    if (type == APKRT_EVENT_PERMISSION_RESULT) permission_done(a0);
    // The answer to an app installer's question records the grant, as apkrt_set_install_sources does at the start.
    if (type == APKRT_EVENT_INSTALL_SOURCES_RESULT) {
        install_done(true, a0);
        apkrt::install_sources::set(a1 != 0);
    }
    if (type == APKRT_EVENT_INSTALL_RESULT) install_done(false, a0);
    if (type == APKRT_EVENT_UNINSTALL_RESULT) {
        const std::lock_guard<std::mutex> done(install_lock);
        live_uninstalls.remove(a0);
    }
    const std::lock_guard<std::mutex> hold(lock);
    if (broken || ended) return 0;
    const apkrt_event record{type, posted + 1, {a0, a1, a2}};
    // A record is far below PIPE_BUF: the write takes it whole or, in a full pipe, not at all.
    if (::write(write_end, &record, sizeof record) != static_cast<ssize_t>(sizeof record)) {
        broken = true;
        changed.notify_all();
        return 0;
    }
    return ++posted;
}

extern "C" int apkrt_session_answer_time(uint32_t serial, uint64_t *uptime_ns) {
    using namespace apkrt::session;
    const std::lock_guard<std::mutex> hold(lock);
    if (serial == 0 || serial > answered || uptime_ns == nullptr) return -1;
    // The earliest kept answer at or past `serial`: the answers rise, so the first one found from the oldest.
    const std::size_t kept = std::min(answer_count, answers_kept);
    for (std::size_t k = answer_count - kept; k < answer_count; ++k) {
        const Answered &one = answer_times[k % answers_kept];
        if (one.serial < serial) continue;
        // An answer before it that was pushed out may have covered it already.
        if (k == answer_count - kept && answer_count > answers_kept) return -1;
        *uptime_ns = one.time;
        return 0;
    }
    return -1;
}

extern "C" int apkrt_session_wait(uint32_t serial, double seconds) {
    using namespace apkrt::session;
    if (!begun.load(std::memory_order_acquire)) return -1;
    const auto deadline = std::chrono::steady_clock::now() + limit(seconds);
    std::unique_lock<std::mutex> hold(lock);
    // 0 is what a failed post returns; a serial not posted yet has no event to wait for.
    if (serial == 0 || serial > posted) return -1;
    while (true) {
        if (answered >= serial) return 1;
        if (broken || ended) return -1;
        if (changed.wait_until(hold, deadline) == std::cv_status::timeout) {
            if (answered >= serial) return 1;
            return broken || ended ? -1 : 0;
        }
    }
}
