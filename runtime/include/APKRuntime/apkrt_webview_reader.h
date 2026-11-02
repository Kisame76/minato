// The WebView channel's record reader (ADR-0056 step a). Header-only, so the guest's own code (App's WebView JNI,
// built with the NDK against bionic) includes it without linking the runtime: it uses only libc read() and the shared
// struct/constants of guest_interface.h, no apkrt symbols. The host frames records with apkrt_webview_post
// (apkrt.h); this joins the partial reads a SOCK_STREAM gives and the APKRT_WEBVIEW_MORE chain of a split payload into
// one whole record. The Mac test drives it over a real socketpair, so the one parser is tested once.
#pragma once

#include "APKRuntime/guest_interface.h"

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace apkrt::channels {

/// A whole logical record read from a WebView channel: its type, its request_id and the reassembled payload.
struct WebViewRecord {
    std::uint32_t type = 0;
    std::uint64_t request_id = 0;
    std::vector<std::uint8_t> payload;
};

/// Reads whole records from a WebView channel's fd (the guest reads its channel fd; the test a real socket end),
/// joining partial reads and APKRT_WEBVIEW_MORE chains. One reader per fd; next() blocks until a whole record or EOF.
class WebViewReader {
public:
    explicit WebViewReader(int fd) : fd_(fd) {}

    /// 1 and fills `out` with the next whole record; 0 at EOF (the host closed its end at a record boundary); a
    /// negative errno on an error -- -EINVAL for a header with an unknown type, a length over
    /// APKRT_WEBVIEW_RECORD_MAX, or an EOF part-way through a record, and -errno for a read() failure.
    long next(WebViewRecord &out) {
        out = WebViewRecord{};
        bool have_meta = false;  // the type and request_id, taken from the first record of a MORE chain
        for (;;) {
            apkrt_webview_record_header header;
            const long got = read_full(&header, sizeof header);
            if (got == 0) return have_meta ? -EINVAL : 0;  // EOF: a truncated chain, or a clean boundary
            if (got < 0) return got;
            if (header.length > APKRT_WEBVIEW_RECORD_MAX || !known_record_type(header.type)) return -EINVAL;
            if (!have_meta) {
                out.type = header.type;
                out.request_id = header.request_id;
                have_meta = true;
            }
            if (header.length > 0) {
                const std::size_t base = out.payload.size();
                out.payload.resize(base + header.length);
                const long body = read_full(out.payload.data() + base, header.length);
                if (body == 0) return -EINVAL;  // EOF inside a payload
                if (body < 0) return body;
            }
            if ((header.flags & APKRT_WEBVIEW_MORE) == 0) return 1;  // the last record of the chain
        }
    }

private:
    /// Whether `type` is a record type this ABI knows; an unknown one is refused when the reader meets it.
    static bool known_record_type(std::uint32_t type) {
        switch (type) {
            case APKRT_WEBVIEW_REC_CREATED:
            case APKRT_WEBVIEW_REC_PLACED:
            case APKRT_WEBVIEW_REC_DESTROYED:
            case APKRT_WEBVIEW_REC_NAVIGATION:
            case APKRT_WEBVIEW_REC_JS_RESULT:
            case APKRT_WEBVIEW_REC_JS_BRIDGE:
            case APKRT_WEBVIEW_REC_RESOURCE:
            case APKRT_WEBVIEW_REC_PERMISSION:
            case APKRT_WEBVIEW_REC_ERROR:
            case APKRT_WEBVIEW_REC_JS_DIALOG:
            case APKRT_WEBVIEW_REC_CONSOLE:
            case APKRT_WEBVIEW_REC_PROGRESS:
            case APKRT_WEBVIEW_REC_TITLE:
            case APKRT_WEBVIEW_REC_COOKIES:
                return true;
            default:
                return false;
        }
    }

    /// Reads exactly `length` bytes into `data`, over EINTR and short reads. Returns `length`; 0 at a clean EOF before
    /// any byte of the block; a negative errno on an error or an EOF part-way through the block.
    long read_full(void *data, std::size_t length) {
        auto *bytes = static_cast<std::uint8_t *>(data);
        std::size_t read = 0;
        while (read < length) {
            const ssize_t got = ::read(fd_, bytes + read, length - read);
            if (got < 0) {
                if (errno == EINTR) continue;
                return -errno;
            }
            if (got == 0) return read == 0 ? 0 : -EINVAL;  // a boundary EOF, or a truncated record
            read += static_cast<std::size_t>(got);
        }
        return static_cast<long>(length);
    }

    int fd_;
};

}  // namespace apkrt::channels
