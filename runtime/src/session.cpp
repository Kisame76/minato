#include "session.h"

#include "descriptors.h"
#include "guest_context.h"
#include "permissions.h"
#include "syscalls.h"
#include "windows.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
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

// The channel. Posts, answers and the end change it under `lock`, and `changed` wakes the host's waits.
std::mutex lock;
std::condition_variable changed;
int write_end = -1;        // the event pipe's, the runtime's; non-blocking
int read_end = -1;         // until activity-host connects: then its guest descriptor holds it
bool connected = false;
std::uint32_t posted = 0;    // the last serial number posted
std::uint32_t answered = 0;  // the last serial number answered
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

// The windows' live tokens (ADR-0041, gap 4), each with its make's type, parent, group and format, apart from the
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
    if ((point >= 0x200b && point <= 0x200f) || (point >= 0x2028 && point <= 0x202e)) return false;
    if (point >= 0x2060 && point <= 0x206f) return false;
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
/// (1) or coarse (2), microphone (2) takes none (0).
bool valid_permission(std::string_view text) {
    if (text.size() != 3 || text[1] != '\t') return false;  // one group digit, a tab, one detail digit
    switch (text[0]) {
        case '1': return text[2] == '1' || text[2] == '2';  // location: fine or coarse
        case '2': return text[2] == '0';                    // microphone: none
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
    if (!sys::memory::copy_checked(configuration, reinterpret_cast<std::uint64_t>(&described.configuration),
                                   sizeof described.configuration))
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
    if (!sys::memory::copy_checked(address, reinterpret_cast<std::uint64_t>(&described.configuration),
                                   sizeof described.configuration))
        return -linux_errno::efault;
    return 0;
}

long answer(std::uint64_t serial) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    const std::lock_guard<std::mutex> hold(lock);
    if (serial == 0 || serial > posted) return -linux_errno::einval;
    if (serial > answered) {
        answered = static_cast<std::uint32_t>(serial);
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
        case APKRT_REQUEST_PERMISSION:
            return permission(a0, a1);
        case APKRT_REQUEST_PERMISSION_CANCEL:
            return permission_cancel(a0);
        default:
            return -linux_errno::einval;
    }
}

}  // namespace apkrt::session

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
    begun.store(true, std::memory_order_release);
    return 0;
}

extern "C" void apkrt_permission_answer(uint32_t request_id, int32_t group, int32_t outcome) {
    apkrt::permissions::set(group, outcome);
    apkrt_session_post(APKRT_EVENT_PERMISSION_RESULT, request_id, static_cast<uint64_t>(static_cast<uint32_t>(group)),
                       static_cast<uint64_t>(static_cast<uint32_t>(outcome)));
}

extern "C" uint32_t apkrt_session_post(uint32_t type, uint64_t a0, uint64_t a1, uint64_t a2) {
    using namespace apkrt::session;
    if (!begun.load(std::memory_order_acquire)) return 0;
    // The answer to a pick request frees its ID, also when the post fails: the request is over.
    if (type == APKRT_EVENT_PICK_RESULT) pick_done(a0);
    if (type == APKRT_EVENT_OPEN_RESULT) open_done(a0);
    if (type == APKRT_EVENT_PERMISSION_RESULT) permission_done(a0);
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
